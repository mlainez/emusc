// emuscd - headless Roland Sound Canvas daemon
// Listens for MIDI input and synthesizes audio to ALSA output

#include <iostream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <alsa/asoundlib.h>
#include "../libemusc/src/synth.h"
#include "../libemusc/src/control_rom.h"
#include "../libemusc/src/wave_rom.h"

namespace {

const char *SUPPORTED_DEVICES[] = { "sc55", "sc55mkii", "sc88", "jv880" };

// The sound map emuscd runs in. Named once so that the value handed to the
// Synth constructor and the value handed to its power-on reset cannot drift
// apart: reset(sm, ...) re-applies the map, so passing a different one there
// would silently switch the device into another mode. GS is the Synth default
// and the mode all four supported devices are addressed in; emuscd exposes no
// option to change it.
const EmuSC::Synth::SoundMap SOUND_MAP = EmuSC::Synth::SoundMap::GS;

bool device_supported(const std::string &dev) {
  for (const char *d : SUPPORTED_DEVICES)
    if (dev == d) return true;
  return false;
}

struct DeviceRoms {
  std::string control_rom, cpu_rom;
  std::vector<std::string> wave_roms;
};

// File layout matches emusc-render's --device preset (libemusc/tools/main.cc):
// SC-55/SC-55mkII split control data across an external rom2 (control) and an
// internal rom1 (CPU); SC-88 and JV-880 each hold everything in one image
// (SC-88: sc88_rom1.bin; JV-880: the 256 kB rom2.bin, per DeviceProfile in
// devices/jv880.cc, not the unused 32 kB rom1.bin).
DeviceRoms resolve_device_roms(const std::string &device, const std::string &rom_dir) {
  DeviceRoms r;
  if (device == "sc55" || device == "sc55mkii") {
    r.control_rom = rom_dir + "/" + device + "_rom2.bin";
    r.cpu_rom     = rom_dir + "/" + device + "_rom1.bin";
    int n = (device == "sc55") ? 3 : 2;
    for (int k = 1; k <= n; k++)
      r.wave_roms.push_back(rom_dir + "/" + device + "_waverom" + std::to_string(k) + ".bin");
  } else if (device == "sc88") {
    r.control_rom = rom_dir + "/sc88_rom1.bin";
    for (int k = 1; k <= 4; k++)
      r.wave_roms.push_back(rom_dir + "/sc88_waverom" + std::to_string(k) + ".bin");
  } else if (device == "jv880") {
    r.control_rom = rom_dir + "/jv880_rom2.bin";
    for (int k = 1; k <= 2; k++)
      r.wave_roms.push_back(rom_dir + "/jv880_waverom" + std::to_string(k) + ".bin");
  }
  return r;
}

inline int16_t to_i16(float x) {
  if (x > 1.0f) x = 1.0f;
  if (x < -1.0f) x = -1.0f;
  long v = std::lrintf(x * 32767.0f);
  if (v > 32767) v = 32767;
  if (v < -32767) v = -32767;
  return static_cast<int16_t>(v);
}

// Assembles raw MIDI bytes into complete channel messages, honouring running
// status so a stream of note-on/note-off pairs (which omit the repeated
// status byte) is parsed correctly instead of misreading data as a status.
// System real-time bytes (0xF8-0xFF) pass through without disturbing state,
// since they can be interleaved into another message.
//
// SysEx is assembled whole, 0xF0 through the terminating 0xF7, and handed over
// with both framing bytes: Synth::_apply_midi_sysex rejects any message whose
// first byte is not 0xF0 and whose last is not 0xF7. This is how a GS reset and
// every part parameter a song states up front reach the synth; on the Sound
// Canvas family that is most of how a song describes itself.
//
// A status byte other than 0xF7 arriving mid-message abandons the SysEx rather
// than terminating it, which is what the receiver is meant to do when a sender
// is interrupted, and keeps a truncated message from being acted on. Messages
// longer than the uint16_t length Synth::midi_input_sysex takes are dropped for
// the same reason: forwarding a prefix would apply a partial parameter block.
class MidiParser {
public:
  template <typename F, typename G>
  void feed(uint8_t byte, F &&on_message, G &&on_sysex) {
    if (byte >= 0xf8)
      return;

    if (_inSysex) {
      if (byte == 0xf7) {
        _inSysex = false;
        if (!_sysexOverflow) {
          _sysex.push_back(0xf7);
          on_sysex(_sysex.data(), static_cast<uint16_t>(_sysex.size()));
        }
        _sysex.clear();
        return;
      }
      if (byte & 0x80) {           // abandoned by the sender
        _inSysex = false;
        _sysex.clear();
        // fall through and parse this byte as the status it is
      } else {
        if (_sysex.size() >= 0xffff)
          _sysexOverflow = true;
        else
          _sysex.push_back(byte);
        return;
      }
    }

    if (byte == 0xf0) {
      // A System Common message cancels running status (MIDI 1.0), so the next
      // data byte cannot be taken for a continuation of whatever preceded it.
      _runningStatus = 0;
      _data.clear();
      _inSysex = true;
      _sysexOverflow = false;
      _sysex.clear();
      _sysex.push_back(0xf0);
      return;
    }
    if (byte == 0xf7)              // stray end-of-exclusive
      return;

    if (byte & 0x80) {
      if (byte >= 0xf1 && byte <= 0xf6) {
        _runningStatus = 0;
        return;
      }
      _runningStatus = byte;
      _data.clear();
      return;
    }
    if (!_runningStatus)
      return;
    _data.push_back(byte);
    if (_data.size() >= _data_bytes_needed(_runningStatus)) {
      uint8_t d1 = _data.size() > 0 ? _data[0] : 0;
      uint8_t d2 = _data.size() > 1 ? _data[1] : 0;
      on_message(_runningStatus, d1, d2);
      _data.clear();
    }
  }

private:
  static size_t _data_bytes_needed(uint8_t status) {
    switch (status & 0xf0) {
    case 0xc0: case 0xd0: return 1;   // program change, channel pressure
    default:              return 2;
    }
  }

  uint8_t _runningStatus = 0;
  std::vector<uint8_t> _data;
  bool _inSysex = false;
  bool _sysexOverflow = false;
  std::vector<uint8_t> _sysex;
};

}  // namespace

class EmuscdDaemon {
  snd_rawmidi_t* midi_in = nullptr;
  snd_pcm_t* pcm_out = nullptr;
  unsigned int sample_rate = 44100;

  std::unique_ptr<EmuSC::ControlRom> ctrl_rom;
  std::unique_ptr<EmuSC::WaveRom> wave_rom;
  std::unique_ptr<EmuSC::Synth> synth;
  std::string device_name;

  MidiParser midi_parser;

  std::atomic<bool> running{true};
  std::atomic<bool> device_change_requested{false};
  std::mutex request_mutex;
  std::string requested_device;

public:
  EmuscdDaemon(const std::string& dev, const std::string& port_name) {
    init_alsa_midi(port_name);
    init_alsa_audio();
    if (!load_device(dev)) {
      std::cerr << "emuscd: failed to load initial device '" << dev << "'"
                << std::endl;
      exit(2);
    }
  }

  ~EmuscdDaemon() {
    running = false;
    if (midi_in) snd_rawmidi_close(midi_in);
    if (pcm_out) snd_pcm_close(pcm_out);
  }

  void init_alsa_midi(const std::string& port_name) {
    int err = snd_rawmidi_open(&midi_in, nullptr,
                               ("virtual:" + port_name).c_str(),
                               SND_RAWMIDI_NONBLOCK);
    if (err < 0) {
      std::cerr << "ALSA MIDI open error: " << snd_strerror(err) << std::endl;
      exit(1);
    }
    std::cout << "ALSA MIDI input opened: " << port_name << std::endl;
  }

  void init_alsa_audio() {
    snd_pcm_hw_params_t* hw_params;
    int err;

    err = snd_pcm_open(&pcm_out, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
      std::cerr << "PCM open error: " << snd_strerror(err) << std::endl;
      exit(1);
    }

    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(pcm_out, hw_params);
    snd_pcm_hw_params_set_access(pcm_out, hw_params,
                                 SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm_out, hw_params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm_out, hw_params, 2);

    unsigned int rate = 44100;
    snd_pcm_hw_params_set_rate_near(pcm_out, hw_params, &rate, 0);
    snd_pcm_hw_params(pcm_out, hw_params);
    sample_rate = rate;

    std::cout << "ALSA audio output initialized at " << rate << " Hz"
              << std::endl;
  }

  bool load_device(const std::string& dev) {
    if (!device_supported(dev)) {
      std::cerr << "emuscd: unsupported device '" << dev << "' (supported: "
                << "sc55, sc55mkii, sc88, jv880)" << std::endl;
      return false;
    }

    std::string rom_dir = std::getenv("EMUSCD_ROM_DIR")
                             ? std::getenv("EMUSCD_ROM_DIR")
                             : "/usr/share/emuscd/roms";
    DeviceRoms roms = resolve_device_roms(dev, rom_dir);

    std::unique_ptr<EmuSC::ControlRom> new_ctrl;
    std::unique_ptr<EmuSC::WaveRom> new_wave;
    try {
      new_ctrl.reset(new EmuSC::ControlRom(roms.control_rom, roms.cpu_rom));
      new_wave.reset(new EmuSC::WaveRom(roms.wave_roms, *new_ctrl));
    } catch (const std::string &e) {
      std::cerr << "emuscd: failed to load '" << dev << "' ROMs: " << e
                << std::endl;
      return false;
    } catch (const std::exception &e) {
      std::cerr << "emuscd: failed to load '" << dev << "' ROMs: "
                << e.what() << std::endl;
      return false;
    }

    std::unique_ptr<EmuSC::Synth> new_synth(
      new EmuSC::Synth(*new_ctrl, *new_wave, SOUND_MAP));
    new_synth->set_audio_format(sample_rate, 2);

    // Power-on reset, which is what the hardware does when it is switched on
    // and what a GS reset in the stream asks for later. It must follow
    // set_audio_format(), which is where the 16 parts are instantiated: called
    // before that, the resetParts loop has nothing to walk.
    //
    // On this engine the reset is redundant at startup - Settings::reset()
    // runs exactly the four _initialize_*/_apply_device_performance calls the
    // Settings constructor runs, so a freshly built Synth already holds the
    // same defaults, bank 0 program 0 on every part and Drum1 on part 10. It
    // is here because a real device resets at power-on regardless, and because
    // the same call has to be correct when Synth::_apply_midi_sysex reaches it
    // from a GS reset mid-song, where the parts are no longer fresh.
    new_synth->reset(SOUND_MAP, true);

    // Destroy the old Synth before its ControlRom/WaveRom, which it holds by
    // reference, are replaced out from under it.
    synth.reset();
    ctrl_rom = std::move(new_ctrl);
    wave_rom = std::move(new_wave);
    synth = std::move(new_synth);
    device_name = dev;

    std::cout << "Loaded device: " << dev << " (" << ctrl_rom->model()
              << " v" << ctrl_rom->version() << ")" << std::endl;
    return true;
  }

  void run() {
    std::thread stdin_thread([this]() { read_stdin_commands(); });

    unsigned char midi_buf[64];
    std::cout << "emuscd daemon running (device: " << device_name << "). "
              << "Type a device name to switch, or 'quit' to exit."
              << std::endl;

    while (running) {
      if (device_change_requested.exchange(false)) {
        std::string dev;
        {
          std::lock_guard<std::mutex> lock(request_mutex);
          dev = requested_device;
        }
        load_device(dev);
      }

      int count = snd_rawmidi_read(midi_in, midi_buf, sizeof(midi_buf));
      if (count > 0 && synth) {
        for (int i = 0; i < count; i++) {
          midi_parser.feed(midi_buf[i],
            [this](uint8_t status, uint8_t d1, uint8_t d2) {
              synth->midi_input(status, d1, d2);
            },
            [this](uint8_t *data, uint16_t length) {
              synth->midi_input_sysex(data, length);
            });
        }
      }

      if (synth && pcm_out) {
        float left = 0.0f, right = 0.0f;
        synth->get_next_frame(left, right);
        int16_t buf[2] = { to_i16(left), to_i16(right) };

        snd_pcm_sframes_t written = snd_pcm_writei(pcm_out, buf, 1);
        if (written < 0) {
          if (snd_pcm_recover(pcm_out, (int) written, 1) < 0) {
            std::cerr << "emuscd: PCM write error: "
                      << snd_strerror((int) written) << std::endl;
          }
        }
      } else {
        std::this_thread::yield();
      }
    }

    stdin_thread.join();
  }

private:
  void read_stdin_commands() {
    std::string line;
    while (running) {
      if (std::getline(std::cin, line)) {
        if (line == "quit" || line == "exit") {
          running = false;
        } else if (!line.empty()) {
          std::lock_guard<std::mutex> lock(request_mutex);
          requested_device = line;
          device_change_requested = true;
        }
      } else {
        // stdin closed (e.g. running under a service manager): stop polling
        // it, but keep serving MIDI/audio.
        break;
      }
    }
  }
};

int main(int argc, char* argv[]) {
  std::string device = "sc88";
  std::string port_name = "emuscd";

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--device" && i + 1 < argc) {
      device = argv[++i];
    } else if (arg == "--port-name" && i + 1 < argc) {
      port_name = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::cout
          << "emuscd - Roland Sound Canvas daemon\n"
          << "Usage: emuscd [options]\n"
          << "Options:\n"
          << "  --device NAME       Device to emulate (default: sc88)\n"
          << "                       Supported: sc55, sc55mkii, sc88, jv880\n"
          << "  --port-name NAME    ALSA MIDI port name (default: emuscd)\n"
          << "  --help              Show this help\n"
          << "\n"
          << "ROM files are read from $EMUSCD_ROM_DIR (default:\n"
          << "/usr/share/emuscd/roms), named <device>_rom1.bin,\n"
          << "<device>_rom2.bin and <device>_waverom<N>.bin. See README.md.\n";
      return 0;
    } else {
      std::cerr << "emuscd: unknown option '" << arg << "'" << std::endl;
      return 1;
    }
  }

  if (!device_supported(device)) {
    std::cerr << "emuscd: unsupported device '" << device << "' (supported: "
              << "sc55, sc55mkii, sc88, jv880)" << std::endl;
    return 1;
  }

  try {
    EmuscdDaemon daemon(device, port_name);
    daemon.run();
  } catch (const std::exception& e) {
    std::cerr << "Fatal error: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
