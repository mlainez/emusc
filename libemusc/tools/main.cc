// emusc-render: headless, single-threaded, deterministic SMF -> WAV renderer
// built on libEmuSC (LGPL-2.1+, https://github.com/skjelten/emusc).
//
// The MIDI file is parsed up front (smf.cc), every event is given an exact
// sample position at the requested output rate, and the events are handed to
// EmuSC::Synth::midi_input() / midi_input_sysex() with the offset, in output
// frames, from the frame about to be pulled - a lead of EVENT_LEAD_FRAMES, so
// the synth learns of an event before it generates the control period the
// event falls in and can start a voice on the exact sample. Floats are
// quantised to 16-bit only at the very end, when written.

#include "smf.h"
#include "wav.h"
#include "version.h"

#include "synth.h"          // libEmuSC public API (emusc/libemusc/src)
#include "control_rom.h"
#include "wave_rom.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

const char *USAGE = R"(usage: emusc-render [options] <input.mid> <output.wav>

Renders a Standard MIDI File through libEmuSC to a 16-bit stereo WAV.

ROM selection (either --device with --rom-dir, or the three explicit options):
  --device DEVICE        Device preset (sc55, sc55mkii, sc88, jv880)
  --rom-dir DIR          Directory holding device ROM files, named:
                           sc55:     sc55_rom1.bin sc55_rom2.bin
                                     sc55_waverom{1,2,3}.bin
                           sc55mkii: sc55mkii_rom1.bin sc55mkii_rom2.bin
                                     sc55mkii_waverom{1,2}.bin
                           sc88:     sc88_rom1.bin sc88_waverom{1,2,3,4}.bin
                           jv880:    jv880_rom2.bin jv880_waverom{1,2}.bin
  --control-rom FILE     External program EPROM
  --cpu-rom FILE         Internal CPU EPROM (32 kB; SC-55/SC-55mkII only)
  --wave-rom FILE        PCM/wave ROM; repeat in bank order (each a multiple of 1 MB)

Rendering:
  --rate HZ              Output sample rate (required; e.g. 66207, 64000, 44100)
  --reset gm|gs          Initial sound-map / reset state (default: gs)
  --tail SECONDS         Silence rendered after the last MIDI event (default: 2)
  --seed N               Seed for libEmuSC's use of std::rand() (default: 1)
  --float                Write IEEE float32 samples instead of 16-bit PCM.
                         For measurement: 16 bits floors a decaying tail at
                         about -101 dBFS, which is above where a reverb's
                         top octave has to be fitted.
  --verbose              Let libEmuSC's own stdout diagnostics through
  --version              Print tool and libEmuSC version and exit
  --help                 This text

Exit status: 0 success, 1 usage error, 2 ROM load failure, 3 MIDI/IO error.
)";

struct Options {
  std::string in, out;
  std::string device, rom_dir, control_rom, cpu_rom;
  std::vector<std::string> wave_roms;
  uint32_t rate = 0;
  std::string reset = "gs";
  double tail = 2.0;
  unsigned seed = 1;
  bool as_float = false;
  bool verbose = false;
};

[[noreturn]] void die(int code, const std::string &msg) {
  std::cerr << "emusc-render: " << msg << std::endl;
  std::exit(code);
}

std::string default_rom_dir(const std::string &romset) {
  const char *home = std::getenv("SC55_ORACLE_HOME");
  std::string base;
  if (home && *home) base = home;
  else {
    const char *h = std::getenv("HOME");
    base = std::string(h ? h : "") + "/.local/share/sc55-oracle";
  }
  return base + "/roms/" + romset;
}

Options parse_args(int argc, char **argv) {
  Options o;
  std::vector<std::string> positional;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto need = [&](const char *name) -> std::string {
      if (i + 1 >= argc) die(1, std::string(name) + " requires an argument");
      return argv[++i];
    };
    if      (a == "--device")      o.device = need("--device");
    else if (a == "--rom-dir")     o.rom_dir = need("--rom-dir");
    else if (a == "--control-rom") o.control_rom = need("--control-rom");
    else if (a == "--cpu-rom")     o.cpu_rom = need("--cpu-rom");
    else if (a == "--wave-rom")    o.wave_roms.push_back(need("--wave-rom"));
    else if (a == "--rate")        o.rate = static_cast<uint32_t>(std::stoul(need("--rate")));
    else if (a == "--reset")       o.reset = need("--reset");
    else if (a == "--tail")        o.tail = std::stod(need("--tail"));
    else if (a == "--seed")        o.seed = static_cast<unsigned>(std::stoul(need("--seed")));
    else if (a == "--float")       o.as_float = true;
    else if (a == "--verbose")     o.verbose = true;
    else if (a == "--version") {
      // The commit is read at CMake configure time and can be stale - it has
      // twice reported the wrong thing on this project. The source hash is
      // taken at build time from the files actually compiled in and is the
      // identity to quote in a measurement.
      std::cout << "emusc-render " << EMUSC_RENDER_VERSION << "\n"
                << "libEmuSC source-sha256 " << EMUSC_RENDER_LIBEMUSC_SOURCE_SHA
                << " (" << EMUSC_RENDER_LIBEMUSC_SOURCE_FILES << " files)\n"
                << "libEmuSC commit " << EMUSC_RENDER_LIBEMUSC_COMMIT
                << " (" << EMUSC_RENDER_LIBEMUSC_REF
                << ", read at configure time - may be stale)\n"
                << "libEmuSC version string " << EmuSC::Synth::version() << "\n"
                << "libEmuSC source " << EMUSC_RENDER_LIBEMUSC_SOURCE_DIR << std::endl;
      std::exit(0);
    }
    else if (a == "--help" || a == "-h") { std::cout << USAGE; std::exit(0); }
    else if (!a.empty() && a[0] == '-') die(1, "unknown option " + a + "\n" + USAGE);
    else positional.push_back(a);
  }
  if (positional.size() != 2) die(1, std::string("expected <input.mid> <output.wav>\n") + USAGE);
  o.in = positional[0];
  o.out = positional[1];

  if (o.rate == 0) die(1, "--rate is required");
  if (o.reset != "gm" && o.reset != "gs") die(1, "--reset must be gm or gs");
  if (o.tail < 0) die(1, "--tail must be >= 0");

  if (!o.device.empty()) {
    if (o.device != "sc55" && o.device != "sc55mkii" && o.device != "sc88" && o.device != "jv880")
      die(1, "--device must be sc55, sc55mkii, sc88, or jv880");
    std::string dir = o.rom_dir.empty() ? default_rom_dir(o.device) : o.rom_dir;
    if (o.control_rom.empty()) {
      if (o.device == "sc55" || o.device == "sc55mkii")
        o.control_rom = dir + "/" + o.device + "_rom2.bin";
      else if (o.device == "sc88")
        o.control_rom = dir + "/" + o.device + "_rom1.bin";
      else if (o.device == "jv880")
        // JV-880's control ROM is its 256 kB table image (DeviceProfile::romSize
        // in devices/jv880.cc); the 32 kB rom1 image is not read for this device.
        o.control_rom = dir + "/" + o.device + "_rom2.bin";
    }
    if (o.cpu_rom.empty() && (o.device == "sc55" || o.device == "sc55mkii")) {
      o.cpu_rom = dir + "/" + o.device + "_rom1.bin";
    }
    if (o.wave_roms.empty()) {
      // Chip counts: SC-55 3, SC-55mkII 2 (waverom bank layout), SC-88 4
      // (SC88_WAVE_CHIP_COUNT in sc88_device.h), JV-880 2 (DeviceProfile
      // waveRomBanks in devices/jv880.cc).
      int n = (o.device == "sc55") ? 3 : (o.device == "sc55mkii") ? 2 :
              (o.device == "sc88") ? 4 : 2;
      for (int k = 1; k <= n; k++)
        o.wave_roms.push_back(dir + "/" + o.device + "_waverom" + std::to_string(k) + ".bin");
    }
  }
  if (o.control_rom.empty() || o.wave_roms.empty())
    die(1, "ROMs not specified: give --device with --rom-dir, or --control-rom and --wave-rom");
  return o;
}

// libEmuSC reports progress on std::cout. Keep stdout clean unless asked.
struct NullBuf : std::streambuf { int overflow(int c) override { return c; } };

inline int16_t to_i16(float x) {
  // Clamp then round to nearest (lrintf honours the default FE_TONEAREST).
  if (x > 1.0f) x = 1.0f;
  if (x < -1.0f) x = -1.0f;
  long v = std::lrintf(x * 32767.0f);
  if (v > 32767) v = 32767;
  if (v < -32767) v = -32767;
  return static_cast<int16_t>(v);
}

}  // namespace

int main(int argc, char **argv) {
  Options o = parse_args(argc, argv);

  NullBuf nullbuf;
  std::streambuf *saved_cout = std::cout.rdbuf();
  if (!o.verbose) std::cout.rdbuf(&nullbuf);
  auto restore_cout = [&]() { std::cout.rdbuf(saved_cout); };

  // ---- MIDI file ------------------------------------------------------------
  smf::File midi;
  try {
    midi = smf::load(o.in);
  } catch (const std::exception &e) {
    restore_cout();
    die(3, e.what());
  }

  // ---- ROMs -----------------------------------------------------------------
  // libEmuSC signals ROM problems by throwing std::string.
  std::unique_ptr<EmuSC::ControlRom> ctrl;
  std::unique_ptr<EmuSC::WaveRom> wave;
  try {
    ctrl.reset(new EmuSC::ControlRom(o.control_rom, o.cpu_rom));
  } catch (const std::string &e) {
    restore_cout();
    die(2, "control ROM load failed: " + e + "\n  control ROM: " + o.control_rom +
           "\n  CPU ROM:     " + o.cpu_rom);
  } catch (const std::exception &e) {
    restore_cout();
    die(2, std::string("control ROM load failed: ") + e.what());
  }

  if (!o.device.empty()) {
    // Guard against a directory holding the wrong device's ROMs.
    auto gen = ctrl->generation();
    bool ok = (o.device == "sc55" && gen == EmuSC::ControlRom::SynthGen::SC55) ||
              (o.device == "sc55mkii" && gen == EmuSC::ControlRom::SynthGen::SC55mk2) ||
              (o.device == "sc88" && gen == EmuSC::ControlRom::SynthGen::SC88) ||
              (o.device == "jv880" && gen == EmuSC::ControlRom::SynthGen::JV880);
    if (!ok) {
      restore_cout();
      die(2, "control ROM identifies as " + ctrl->model() + " v" + ctrl->version() +
             ", which is not what --device " + o.device + " expects");
    }
  }

  try {
    wave.reset(new EmuSC::WaveRom(o.wave_roms, *ctrl));
  } catch (const std::string &e) {
    restore_cout();
    std::string files;
    for (auto &w : o.wave_roms) files += "\n  wave ROM:    " + w;
    die(2, "wave ROM load failed: " + e + files);
  } catch (const std::exception &e) {
    restore_cout();
    die(2, std::string("wave ROM load failed: ") + e.what());
  }

  std::cerr << "emusc-render: control ROM " << ctrl->model() << " v" << ctrl->version()
            << " (" << ctrl->date() << "), wave ROM v" << wave->version()
            << " (" << wave->date() << ")" << std::endl;

  // ---- Synth ----------------------------------------------------------------
  EmuSC::Synth::SoundMap map = (o.reset == "gm") ? EmuSC::Synth::SoundMap::GS_GM
                                                 : EmuSC::Synth::SoundMap::GS;
  EmuSC::Synth synth(*ctrl, *wave, map);
  // Synth's constructor seeds std::rand() from the wall clock; libEmuSC then
  // draws from std::rand() for random pan, random pitch and the sample-and-hold
  // LFO. Re-seed with a fixed value so the render is reproducible.
  std::srand(o.seed);
  synth.set_audio_format(o.rate, 2);   // also instantiates the 16 parts

  // Power-on reset, before the first event is queued. --reset selects the sound
  // MAP, which the constructor above already applied; it does not put the parts
  // on their default instrument, and only reset(map, resetParts=true) does.
  //
  // On this engine that is redundant here: Settings::reset() runs exactly the
  // four _initialize_*/_apply_device_performance calls the Settings constructor
  // runs, so the synth already holds bank 0 program 0 on every part and Drum1
  // on part 10, and Part::reset() only clears notes and counters that are
  // already clear on a part built moments ago. Renders are unchanged by it. It
  // is here because the tool should start the device the way the device starts
  // itself, so that a file which plays a note before its first program change
  // is rendered from a stated default rather than an implied one.
  synth.reset(map, true);

  // ---- Schedule -------------------------------------------------------------
  struct Sched { uint64_t frame; const smf::Event *ev; };
  std::vector<Sched> sched;
  sched.reserve(midi.events.size());
  uint64_t last_event_frame = 0;
  size_t n_channel = 0, n_sysex = 0, n_meta = 0;
  for (const smf::Event &e : midi.events) {
    if (e.kind == smf::Kind::Meta) { n_meta++; continue; }
    uint64_t fr = smf::to_sample(e.time_num, midi.time_den, o.rate);
    sched.push_back({fr, &e});
    if (fr > last_event_frame) last_event_frame = fr;
    if (e.kind == smf::Kind::Channel) n_channel++; else n_sysex++;
  }
  // Events are already sorted by time in smf::parse; rounding to frames is
  // monotonic, so sched is sorted too.

  uint64_t tail_frames = static_cast<uint64_t>(std::llround(o.tail * o.rate));
  uint64_t total_frames = (sched.empty() ? 0 : last_event_frame + 1) + tail_frames;

  std::cerr << "emusc-render: " << o.in << ": format " << midi.format << ", "
            << midi.ntracks << " tracks, "
            << (midi.smpte ? "SMPTE time base" : std::to_string(midi.ppqn) + " ppqn")
            << ", " << midi.n_tempo_changes << " tempo change(s), "
            << n_channel << " channel events, " << n_sysex << " SysEx, " << n_meta << " meta"
            << std::endl;
  std::cerr << "emusc-render: last event at frame " << last_event_frame << " ("
            << (double)last_event_frame / o.rate << " s), end-of-track at "
            << (double)midi.end_of_track_num / midi.time_den << " s, rendering "
            << total_frames << " frames at " << o.rate << " Hz ("
            << (double)total_frames / o.rate << " s), reset=" << o.reset
            << ", seed=" << o.seed << std::endl;

  // ---- Render ---------------------------------------------------------------
  try {
    WavWriter wav(o.out, o.rate, 2, o.as_float);
    std::vector<int16_t> buf;
    std::vector<float> fbuf;
    buf.reserve(2 * 4096);
    fbuf.reserve(2 * 4096);
    // Enough lead for one control period (256 samples at 32 kHz, at most
    // 1024 output frames up to 128 kHz) plus the synth's own note-on delay,
    // so every event is queued before the period it belongs to is generated.
    const uint64_t EVENT_LEAD_FRAMES = 2048;

    // Which MIDI port each track plays into. The standard statement is the
    // FF 21 port meta event; Roland's own SC-88 demo files carry none and say
    // it only in the track name, "PartA 1ch." through "PartB 16ch.". Dropping
    // it puts a 32-part song's two halves on the same sixteen channels, where
    // whichever program change lands last wins for both tracks' notes.
    std::vector<uint8_t> trackPort(midi.ntracks ? midi.ntracks : 1, 0);
    for (const smf::Event &e : midi.events) {
      if (e.kind != smf::Kind::Meta || e.bytes.empty()) continue;
      if (e.track < 0 || (size_t) e.track >= trackPort.size()) continue;
      if (e.bytes[0] == 0x21 && e.bytes.size() > 1) {
        trackPort[e.track] = e.bytes[1] < 2 ? e.bytes[1] : 0;
      } else if (e.bytes[0] == 0x03) {
        const std::string nm(e.bytes.begin() + 1, e.bytes.end());
        if (nm.find("PartB") != std::string::npos ||
            nm.find("Partb") != std::string::npos)
          trackPort[e.track] = 1;
      }
    }
    if (synth.midi_ports() > 1) {
      int onB = 0;
      for (uint8_t p : trackPort) if (p) onB++;
      if (onB)
        std::cerr << "emusc-render: " << onB << " of " << trackPort.size()
                  << " tracks play into MIDI port B" << std::endl;
    }

    size_t next = 0;
    for (uint64_t fr = 0; fr < total_frames; fr++) {
      while (next < sched.size() && sched[next].frame <= fr + EVENT_LEAD_FRAMES) {
        const smf::Event &e = *sched[next].ev;
        const uint32_t off = static_cast<uint32_t>(sched[next].frame - fr);
        if (e.kind == smf::Kind::Channel) {
          const uint8_t port =
            (e.track >= 0 && (size_t) e.track < trackPort.size())
              ? trackPort[e.track] : 0;
          synth.midi_input(e.bytes[0], e.bytes[1], e.bytes.size() > 2 ? e.bytes[2] : 0,
                           off, port);
        } else if (e.kind == smf::Kind::SysEx) {
          if (e.bytes.size() > 0xffff)
            std::cerr << "emusc-render: SysEx longer than 65535 bytes skipped" << std::endl;
          else
            synth.midi_input_sysex(const_cast<uint8_t *>(e.bytes.data()),
                                   static_cast<uint16_t>(e.bytes.size()), off,
                                   (e.track >= 0 &&
                                    (size_t) e.track < trackPort.size())
                                     ? trackPort[e.track] : 0);
        }
        next++;
      }
      float l = 0.0f, r = 0.0f;
      synth.get_next_frame(l, r);
      if (o.as_float) {
        fbuf.push_back(l);
        fbuf.push_back(r);
        if (fbuf.size() >= 2 * 4096) { wav.write(fbuf.data(), fbuf.size() / 2); fbuf.clear(); }
      } else {
        buf.push_back(to_i16(l));
        buf.push_back(to_i16(r));
        if (buf.size() >= 2 * 4096) { wav.write(buf.data(), buf.size() / 2); buf.clear(); }
      }
    }
    if (!buf.empty()) wav.write(buf.data(), buf.size() / 2);
    if (!fbuf.empty()) wav.write(fbuf.data(), fbuf.size() / 2);
    wav.close();
    std::cerr << "emusc-render: wrote " << wav.frames() << " frames to " << o.out
              << "; libEmuSC reported " << synth.get_num_clipped_samples(false)
              << " clipped samples" << std::endl;
  } catch (const std::exception &e) {
    restore_cout();
    die(3, e.what());
  }

  restore_cout();
  return 0;
}
