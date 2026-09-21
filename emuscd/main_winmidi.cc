// emusc-winmidi - Windows real-time Roland Sound Canvas MIDI device
//
// The WinMM equivalent of emuscd (main.cc, ALSA/Linux). It does not create a
// MIDI port of its own - unlike ALSA's "virtual:" ports, WinMM's midiIn API
// only opens an EXISTING one - so a virtual MIDI cable (loopMIDI on Windows
// 7+, Maple Virtual MIDI Cable on 95/98/ME) is what a sequencer or game
// connects to; this process then opens that port by device index.

#include <windows.h>
#include <mmsystem.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common.h"
#include "../libemusc/src/synth.h"
#include "../libemusc/src/control_rom.h"
#include "../libemusc/src/wave_rom.h"

using namespace emuscd;

namespace {

const char *USAGE =
"emusc-winmidi - Roland Sound Canvas MIDI device for Windows\n"
"Usage: emusc-winmidi [options]\n"
"Options:\n"
"  --device NAME       Device to emulate (default: sc88)\n"
"                       Supported: sc55, sc55mkii, sc88, jv880\n"
"  --midi-in N          MIDI input device index (default: 0)\n"
"  --wave-out N         Wave output device index (default: system default)\n"
"  --list-midi-in       List MIDI input devices and exit\n"
"  --list-wave-out      List wave output devices and exit\n"
"  --rom-dir DIR        Directory holding device ROM files (default:\n"
"                        %EMUSCD_ROM_DIR%, or .\\roms if unset)\n"
"  --rate HZ            Audio sample rate (default: 48000)\n"
"  --block N            Audio frames per wave buffer (default: 256)\n"
"  --latency MS         Requested output buffer size (default: 20)\n"
"  --help               Show this help\n"
"\n"
"ROM files are named <device>_control.bin, <device>_cpu.bin\n"
"(SC-55/SC-55mkII only) and <device>_waverom<N>.bin. See README.md\n"
"for exact ROM hashes.\n"
"\n"
"emusc-winmidi does not create a MIDI port itself. Route MIDI into it with\n"
"a virtual MIDI cable (loopMIDI, or Maple Virtual MIDI Cable on Windows\n"
"95/98/ME), then pick that port with --midi-in.\n";

// Bytes 0-2 of MIM_DATA's packed dwParam1 are the status byte and up to two
// data bytes, already driver-normalised. Unlike ALSA's raw rawmidi byte
// stream, WinMM has resolved running status and framed the message before
// this ever arrives, so there's no need for emuscd's own byte-level
// MidiParser here - that class exists for the ALSA side's raw stream.
struct MidiEvt { uint8_t status, d1, d2; };

inline MidiEvt unpack_midi_message(DWORD_PTR dwParam1) {
  DWORD packed = static_cast<DWORD>(dwParam1);
  return { static_cast<uint8_t>(packed & 0xff),
           static_cast<uint8_t>((packed >> 8) & 0xff),
           static_cast<uint8_t>((packed >> 16) & 0xff) };
}

void list_midi_in_devices() {
  UINT n = midiInGetNumDevs();
  if (n == 0) { std::cout << "(no MIDI input devices found)" << std::endl; return; }
  for (UINT i = 0; i < n; i++) {
    MIDIINCAPSA caps;
    if (midiInGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
      std::cout << i << ": " << caps.szPname << std::endl;
  }
}

void list_wave_out_devices() {
  UINT n = waveOutGetNumDevs();
  if (n == 0) { std::cout << "(no wave output devices found)" << std::endl; return; }
  for (UINT i = 0; i < n; i++) {
    WAVEOUTCAPSA caps;
    if (waveOutGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
      std::cout << i << ": " << caps.szPname << std::endl;
  }
}

}  // namespace

class WinMidiDaemon {
public:
  WinMidiDaemon(const std::string &dev, const std::string &romDir,
                int midiInId, int waveOutId, unsigned rate, unsigned block,
                unsigned latencyMs)
      : _sampleRate(rate), _blockFrames(block), _romDir(romDir) {
    InitializeCriticalSection(&_midiLock);
    InitializeCriticalSection(&_sysexLock);
    if (!load_device(dev)) {
      std::cerr << "emusc-winmidi: failed to load initial device '" << dev
                << "'" << std::endl;
      std::exit(2);
    }
    open_midi_in(midiInId);
    open_wave_out(waveOutId, latencyMs);
  }

  ~WinMidiDaemon() {
    if (_hMidiIn) { midiInStop(_hMidiIn); midiInReset(_hMidiIn); midiInClose(_hMidiIn); }
    if (_hWaveOut) { waveOutReset(_hWaveOut); waveOutClose(_hWaveOut); }
    DeleteCriticalSection(&_midiLock);
    DeleteCriticalSection(&_sysexLock);
  }

  // Mirrors emuscd's own device_supported()/rom-resolution/reset sequence
  // (main.cc, EmuscdDaemon::load_device) so the two behave identically aside
  // from the audio/MIDI backend underneath.
  bool load_device(const std::string &dev) {
    if (!device_supported(dev)) {
      std::cerr << "emusc-winmidi: unsupported device '" << dev << "' (supported: "
                << "sc55, sc55mkii, sc88, jv880)" << std::endl;
      return false;
    }

    DeviceRoms roms = resolve_device_roms(dev, _romDir);

    std::unique_ptr<EmuSC::ControlRom> new_ctrl;
    std::unique_ptr<EmuSC::WaveRom> new_wave;
    try {
      new_ctrl.reset(new EmuSC::ControlRom(roms.control_rom, roms.cpu_rom));
      new_wave.reset(new EmuSC::WaveRom(roms.wave_roms, *new_ctrl));
    } catch (const std::string &e) {
      std::cerr << "emusc-winmidi: failed to load '" << dev << "' ROMs: " << e
                << std::endl;
      return false;
    } catch (const std::exception &e) {
      std::cerr << "emusc-winmidi: failed to load '" << dev << "' ROMs: "
                << e.what() << std::endl;
      return false;
    }

    std::unique_ptr<EmuSC::Synth> new_synth(
      new EmuSC::Synth(*new_ctrl, *new_wave, SOUND_MAP));
    new_synth->set_audio_format(_sampleRate, 2);
    new_synth->reset(SOUND_MAP, true);

    // Only the main loop (run()) ever touches _synth after construction, on
    // both the fill/pump and the device-switch path below, so there's no
    // concurrent access to guard here - the MIDI callback only ever writes
    // to _midiQueue/_sysexPending under their own critical sections.
    _synth.reset();
    _ctrlRom = std::move(new_ctrl);
    _waveRom = std::move(new_wave);
    _synth = std::move(new_synth);

    std::cerr << "emusc-winmidi: loaded device " << dev << " ("
              << _ctrlRom->model() << " v" << _ctrlRom->version() << ")"
              << std::endl;
    return true;
  }

  void open_midi_in(int id) {
    UINT n = midiInGetNumDevs();
    if (n == 0) {
      std::cerr << "emusc-winmidi: no MIDI input devices available - "
                   "install a virtual MIDI cable (loopMIDI/Maple) first"
                << std::endl;
      std::exit(1);
    }
    if (id < 0 || static_cast<UINT>(id) >= n) {
      std::cerr << "emusc-winmidi: --midi-in " << id << " out of range (0.."
                << (n - 1) << "); see --list-midi-in" << std::endl;
      std::exit(1);
    }
    MIDIINCAPSA caps{};
    midiInGetDevCapsA(id, &caps, sizeof(caps));

    MMRESULT r = midiInOpen(&_hMidiIn, static_cast<UINT>(id),
                             reinterpret_cast<DWORD_PTR>(&midi_in_proc),
                             reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION);
    if (r != MMSYSERR_NOERROR) {
      std::cerr << "emusc-winmidi: midiInOpen failed (error " << r << ")" << std::endl;
      std::exit(1);
    }
    prepare_sysex_buffer();
    midiInStart(_hMidiIn);
    std::cerr << "emusc-winmidi: MIDI input '" << caps.szPname
              << "' (device " << id << ")" << std::endl;
  }

  void open_wave_out(int id, unsigned latencyMs) {
    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = _sampleRate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = static_cast<WORD>(wfx.nChannels * wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    UINT_PTR devId = (id < 0) ? WAVE_MAPPER : static_cast<UINT_PTR>(id);
    // CALLBACK_NULL: buffer completion is polled (WHDR_DONE) from the main
    // loop instead. WinMM's own documented list of functions safe to call
    // from inside a wave/MIDI callback does not include waveOutWrite or
    // midiInAddBuffer, so nothing here re-arms a buffer from a callback.
    MMRESULT r = waveOutOpen(&_hWaveOut, devId, &wfx, 0, 0, CALLBACK_NULL);
    if (r != MMSYSERR_NOERROR) {
      std::cerr << "emusc-winmidi: waveOutOpen failed (error " << r << ")" << std::endl;
      std::exit(1);
    }

    unsigned numBuffers = (latencyMs * _sampleRate) / 1000 / _blockFrames;
    if (numBuffers < 2) numBuffers = 2;
    _buffers.resize(numBuffers);
    for (auto &b : _buffers) {
      b.data.assign(static_cast<size_t>(_blockFrames) * 2, 0);
      std::memset(&b.hdr, 0, sizeof(WAVEHDR));
      b.hdr.lpData = reinterpret_cast<LPSTR>(b.data.data());
      b.hdr.dwBufferLength = static_cast<DWORD>(b.data.size() * sizeof(int16_t));
      waveOutPrepareHeader(_hWaveOut, &b.hdr, sizeof(WAVEHDR));
      fill_buffer(b);
      waveOutWrite(_hWaveOut, &b.hdr, sizeof(WAVEHDR));
    }
    std::cerr << "emusc-winmidi: audio output at " << _sampleRate << " Hz, "
              << numBuffers << " x " << _blockFrames << "-frame buffers"
              << std::endl;
  }

  void run() {
    std::thread stdin_thread([this]() { read_stdin_commands(); });

    std::cerr << "emusc-winmidi running. Type a device name to switch, "
                 "or 'quit' to exit." << std::endl;
    _running = true;
    while (_running) {
      if (_deviceChangeRequested.exchange(false)) {
        std::string dev;
        {
          std::lock_guard<std::mutex> lock(_requestMutex);
          dev = _requestedDevice;
        }
        load_device(dev);
      }

      drain_midi_queue();
      drain_sysex();
      pump_wave_buffers();
      Sleep(1);
    }

    stdin_thread.join();
  }

private:
  struct WaveBuffer { WAVEHDR hdr; std::vector<int16_t> data; };

  void fill_buffer(WaveBuffer &b) {
    for (unsigned i = 0; i < _blockFrames; i++) {
      float l = 0.0f, r = 0.0f;
      if (_synth) _synth->get_next_frame(l, r);
      b.data[i * 2]     = to_i16(l);
      b.data[i * 2 + 1] = to_i16(r);
    }
  }

  void pump_wave_buffers() {
    for (auto &b : _buffers) {
      if (b.hdr.dwFlags & WHDR_DONE) {
        fill_buffer(b);
        waveOutWrite(_hWaveOut, &b.hdr, sizeof(WAVEHDR));
      }
    }
  }

  void drain_midi_queue() {
    std::vector<MidiEvt> local;
    EnterCriticalSection(&_midiLock);
    local.swap(_midiQueue);
    LeaveCriticalSection(&_midiLock);
    if (_synth)
      for (const MidiEvt &e : local) _synth->midi_input(e.status, e.d1, e.d2);
  }

  void prepare_sysex_buffer() {
    _sysexHdr = {};
    _sysexBuf.assign(SYSEX_BUFFER_SIZE, 0);
    _sysexHdr.lpData = reinterpret_cast<LPSTR>(_sysexBuf.data());
    _sysexHdr.dwBufferLength = SYSEX_BUFFER_SIZE;
    midiInPrepareHeader(_hMidiIn, &_sysexHdr, sizeof(MIDIHDR));
    midiInAddBuffer(_hMidiIn, &_sysexHdr, sizeof(MIDIHDR));
  }

  // A SysEx longer than SYSEX_BUFFER_SIZE arrives as MIM_LONGDATA with
  // dwBytesRecorded == dwBufferLength and MIDIERR_STILLPLAYING-free but
  // incomplete; multi-buffer chaining for that case isn't implemented, so
  // such a message is forwarded truncated rather than dropped outright -
  // acceptable for the GS/GM resets and patch dumps this targets, which are
  // well under 4 kB.
  void drain_sysex() {
    bool pending;
    EnterCriticalSection(&_sysexLock);
    pending = _sysexPending;
    _sysexPending = false;
    LeaveCriticalSection(&_sysexLock);
    if (!pending) return;

    if (_synth && _sysexHdr.dwBytesRecorded > 0)
      _synth->midi_input_sysex(reinterpret_cast<uint8_t *>(_sysexHdr.lpData),
                                static_cast<uint16_t>(_sysexHdr.dwBytesRecorded));

    midiInUnprepareHeader(_hMidiIn, &_sysexHdr, sizeof(MIDIHDR));
    midiInPrepareHeader(_hMidiIn, &_sysexHdr, sizeof(MIDIHDR));
    midiInAddBuffer(_hMidiIn, &_sysexHdr, sizeof(MIDIHDR));
  }

  void read_stdin_commands() {
    std::string line;
    while (_running) {
      if (std::getline(std::cin, line)) {
        if (line == "quit" || line == "exit") {
          _running = false;
        } else if (!line.empty()) {
          std::lock_guard<std::mutex> lock(_requestMutex);
          _requestedDevice = line;
          _deviceChangeRequested = true;
        }
      } else {
        break;
      }
    }
  }

  static void CALLBACK midi_in_proc(HMIDIIN, UINT wMsg, DWORD_PTR dwInstance,
                                     DWORD_PTR dwParam1, DWORD_PTR) {
    auto *self = reinterpret_cast<WinMidiDaemon *>(dwInstance);
    if (wMsg == MIM_DATA) {
      MidiEvt e = unpack_midi_message(dwParam1);
      EnterCriticalSection(&self->_midiLock);
      self->_midiQueue.push_back(e);
      LeaveCriticalSection(&self->_midiLock);
    } else if (wMsg == MIM_LONGDATA) {
      EnterCriticalSection(&self->_sysexLock);
      self->_sysexPending = true;
      LeaveCriticalSection(&self->_sysexLock);
    }
  }

  static const DWORD SYSEX_BUFFER_SIZE = 4096;

  unsigned _sampleRate, _blockFrames;
  std::string _romDir;
  std::atomic<bool> _running{false};

  std::unique_ptr<EmuSC::ControlRom> _ctrlRom;
  std::unique_ptr<EmuSC::WaveRom> _waveRom;
  std::unique_ptr<EmuSC::Synth> _synth;

  HMIDIIN _hMidiIn = nullptr;
  HWAVEOUT _hWaveOut = nullptr;
  std::vector<WaveBuffer> _buffers;

  CRITICAL_SECTION _midiLock;
  std::vector<MidiEvt> _midiQueue;

  CRITICAL_SECTION _sysexLock;
  bool _sysexPending = false;
  MIDIHDR _sysexHdr{};
  std::vector<uint8_t> _sysexBuf;

  std::atomic<bool> _deviceChangeRequested{false};
  std::mutex _requestMutex;
  std::string _requestedDevice;
};

int main(int argc, char **argv) {
  std::string device = "sc88";
  std::string romDir;
  int midiInId = 0;
  int waveOutId = -1;
  unsigned rate = 48000;
  unsigned block = 256;
  unsigned latency = 20;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto need = [&](const char *name) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "emusc-winmidi: " << name << " requires an argument" << std::endl;
        std::exit(1);
      }
      return argv[++i];
    };
    if      (a == "--device")        device = need("--device");
    else if (a == "--rom-dir")       romDir = need("--rom-dir");
    else if (a == "--midi-in")       midiInId = std::stoi(need("--midi-in"));
    else if (a == "--wave-out")      waveOutId = std::stoi(need("--wave-out"));
    else if (a == "--rate")          rate = static_cast<unsigned>(std::stoul(need("--rate")));
    else if (a == "--block")         block = static_cast<unsigned>(std::stoul(need("--block")));
    else if (a == "--latency")       latency = static_cast<unsigned>(std::stoul(need("--latency")));
    else if (a == "--list-midi-in")  { list_midi_in_devices(); return 0; }
    else if (a == "--list-wave-out") { list_wave_out_devices(); return 0; }
    else if (a == "--help" || a == "-h") { std::cout << USAGE; return 0; }
    else {
      std::cerr << "emusc-winmidi: unknown option '" << a << "'" << std::endl;
      return 1;
    }
  }

  if (!device_supported(device)) {
    std::cerr << "emusc-winmidi: unsupported device '" << device << "' (supported: "
              << "sc55, sc55mkii, sc88, jv880)" << std::endl;
    return 1;
  }
  if (block < 1) {
    std::cerr << "emusc-winmidi: --block must be >= 1" << std::endl;
    return 1;
  }

  if (romDir.empty()) {
    const char *envDir = std::getenv("EMUSCD_ROM_DIR");
    romDir = (envDir && *envDir) ? envDir : "roms";
  }

  WinMidiDaemon daemon(device, romDir, midiInId, waveOutId, rate, block, latency);
  daemon.run();
  return 0;
}
