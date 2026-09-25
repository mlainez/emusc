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
#include "audio_out.h"
#include "version.h"
#include "mxcsr_ftz.h"
#include "rom_paths.h"
#include "gain.h"

#include "synth.h"          // libEmuSC public API (emusc/libemusc/src)
#include "control_rom.h"
#include "wave_rom.h"

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#define EMUSC_DUP _dup
#define EMUSC_DUP2 _dup2
#define EMUSC_CLOSE _close
#define EMUSC_OPEN _open
static const int kNullFlags = _O_WRONLY;
static const char *const kNullDevice = "NUL";
#else
#include <unistd.h>
#include <fcntl.h>
#define EMUSC_DUP dup
#define EMUSC_DUP2 dup2
#define EMUSC_CLOSE close
#define EMUSC_OPEN open
static const int kNullFlags = O_WRONLY;
static const char *const kNullDevice = "/dev/null";
#endif

namespace {

const char *USAGE = R"(usage: emusc-render [options] <input.mid> [<output.wav>]
       emusc-render [options] --midi <input.mid> [--out <output.wav>]

Renders a Standard MIDI File through libEmuSC to a 16-bit stereo WAV.
The input file is the only thing that must be given; every option below has
a default.

Input/output (either positional paths, or these two flags - for scripts that
drive emusc-render and another renderer with the same options):
  --midi FILE            Input Standard MIDI File (required)
  --out FILE             Output WAV (default: the input's file name with its
                         extension replaced by .wav, or .wav appended if it
                         has none, written to the current directory - so
                         dir/song.mid renders to ./song.wav). With --play and
                         no --out, nothing is written: audio only. Give both
                         to render a file and listen at the same time.
  --play                 Straight to the sound card as it renders - ALSA on
                         Linux, WinMM on Windows - instead of relying on an
                         OS or user MIDI player. Always 16-bit regardless of
                         --bits/--float; paced by the audio device itself,
                         so this run takes as long as the song does.
  --block N              --play only: audio frames per device write, same
                         meaning as emuscd's/emusc-winmidi's --block
                         (default: 2048)
  --latency MS           --play only: requested output buffer depth, same
                         meaning as emuscd's/emusc-winmidi's --latency.
                         Raise this (and --block) if playback breaks up on
                         slow hardware (default: 200)
  --max-voices N         caps simultaneous voices below the loaded device's
                         real polyphony (24 SC-55, 28 SC-55mkII/JV-880, 64
                         SC-88/JV-1080), trading polyphony for headroom on
                         too slow to sustain the worst case (default: the
                         device's own ceiling - this can only lower it)

ROM selection (--device and --rom-dir, both defaulted, or the explicit
--control-rom/--wave-rom/--cpu-rom files, which bypass both):
  --device DEVICE        Device preset (sc55, sc55mkii, sc88, jv880, jv1080).
                         Default: the one device whose <device>_control.bin
                         is in the ROM directory; an error listing what was
                         found if there is none or more than one.
  --rom-dir DIR          Directory holding device ROM files, named
                         <device>_control.bin, <device>_cpu.bin (SC-55 and
                         SC-55mkII only) and <device>_waverom1.bin upward.
                         Default, the same in emuscd and emusc-winmidi:
                         $EMUSCD_ROM_DIR if set, else ./roms if it exists,
                         else /usr/share/emuscd/roms (./roms on Windows).
                         File names per device:
                           sc55:     sc55_control.bin sc55_cpu.bin
                                     sc55_waverom{1,2,3}.bin
                           sc55mkii: sc55mkii_control.bin sc55mkii_cpu.bin
                                     sc55mkii_waverom{1,2}.bin
                           sc88:     sc88_control.bin sc88_waverom{1,2,3,4}.bin
                           jv1080:   jv1080_control.bin
                                     jv1080_waverom{1,2,3,4}.bin
                           jv880:    jv880_control.bin jv880_waverom{1,2}.bin
                         See README.md for exact ROM file hashes.
  --control-rom FILE     External program EPROM
  --cpu-rom FILE         Internal CPU EPROM (32 kB; SC-55/SC-55mkII only)
  --wave-rom FILE        PCM/wave ROM; repeat in bank order (each a multiple of 1 MB)

Rendering:
  --rate HZ              Output sample rate (e.g. 44100, 48000). Defaults to
                         the device's own rate where that is established -
                         32000 for sc88 and jv1080. For sc55, sc55mkii and
                         jv880, whose own rates are not established, it
                         falls back to 32000, a commonly published figure
                         for them that this project has not verified, and
                         says so on stderr.
  --reset gm|gs|none     Initial sound-map / reset state (default: gs).
                         none skips the power-on reset call entirely.
                         The JV-1080 has no GS mode: gs and gm both put it
                         in GM mode, and none leaves it in its own power-on
                         patch mode (one patch on channel 1).
  --tail SECONDS         Silence rendered after the last MIDI event (default: 2)
  --seed N               Seed for libEmuSC's random source (default: 1)
  --bits 16|32           16-bit PCM or IEEE float32 samples (default: 16).
                         32 floors a decaying tail at about -101 dBFS lower
                         than 16 bits does, which matters when the top octave
                         of a reverb tail is being measured.
  --float                Same as --bits 32.
  --gain-db DB           Output gain in decibels, applied as a linear
                         multiplier (10^(DB/20)) to the final samples, right
                         before they are quantized to 16-bit (or clamped to
                         +-1.0 for --float output). Default: 0, which is no
                         change at all - a render without this flag is
                         byte-identical to one with --gain-db 0. A listening-
                         convenience knob only: it runs after everything
                         libEmuSC itself computes, never feeds back into the
                         synth, and does not change any hardware-fidelity
                         measurement. Full-scale samples this produces are
                         reported the same way as full-scale samples from the
                         engine itself (see the exit-time summary).
  --verbose              Let libEmuSC's own stdout diagnostics through
  --version              Print tool and libEmuSC version and exit
  --help                 This text

Exit status: 0 success, 1 usage error, 2 ROM load failure, 3 MIDI/IO error,
             4 audio output error (--play).
)";

struct Options {
  std::string in, out;
  std::string device, rom_dir, control_rom, cpu_rom;
  std::vector<std::string> wave_roms;
  uint32_t rate = 0;
  std::string reset = "gs";
  double tail = 2.0;
  unsigned seed = 1;
  double gain_db = 0.0;
  bool as_float = false;
  bool verbose = false;
  bool play = false;
  unsigned block = 2048;
  unsigned latency = 200;
  bool max_voices_set = false;
  unsigned max_voices = 0;
};

[[noreturn]] void die(int code, const std::string &msg) {
  std::fprintf(stderr, "emusc-render: %s\n", msg.c_str());
  std::exit(code);
}

// The --out default: the input's last path component, with its extension
// replaced by .wav, in the current directory. Only the directory is dropped,
// so dir/song.mid and song.mid both give song.wav. The extension is whatever
// follows the last '.' of that component, unless the '.' is its first
// character (".mid" is a name, not an extension): "song" gives song.wav,
// "song.v2.mid" gives song.v2.wav, ".mid" gives .mid.wav. An input already
// named *.wav (any case) has no default, since the result could be the input
// itself; the empty string is returned and the caller asks for --out.
std::string default_out_path(const std::string &in) {
  size_t slash = in.find_last_of('/');
#ifdef _WIN32
  size_t bslash = in.find_last_of("\\:");
  if (bslash != std::string::npos && (slash == std::string::npos || bslash > slash))
    slash = bslash;
#endif
  std::string name = (slash == std::string::npos) ? in : in.substr(slash + 1);
  size_t dot = name.find_last_of('.');
  if (dot != std::string::npos && dot > 0) {
    std::string ext = name.substr(dot);
    for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".wav") return "";
    name.resize(dot);
  }
  return name + ".wav";
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
    if      (a == "--midi")        o.in = need("--midi");
    else if (a == "--out")         o.out = need("--out");
    else if (a == "--device")      o.device = need("--device");
    else if (a == "--rom-dir")     o.rom_dir = need("--rom-dir");
    else if (a == "--control-rom") o.control_rom = need("--control-rom");
    else if (a == "--cpu-rom")     o.cpu_rom = need("--cpu-rom");
    else if (a == "--wave-rom")    o.wave_roms.push_back(need("--wave-rom"));
    else if (a == "--rate")        o.rate = static_cast<uint32_t>(std::stoul(need("--rate")));
    else if (a == "--reset")       o.reset = need("--reset");
    else if (a == "--tail")        o.tail = std::stod(need("--tail"));
    else if (a == "--seed")        o.seed = static_cast<unsigned>(std::stoul(need("--seed")));
    else if (a == "--gain-db")     o.gain_db = std::stod(need("--gain-db"));
    else if (a == "--float")       o.as_float = true;
    else if (a == "--bits") {
      std::string bits = need("--bits");
      if      (bits == "16") o.as_float = false;
      else if (bits == "32") o.as_float = true;
      else die(1, "--bits must be 16 or 32");
    }
    else if (a == "--verbose")     o.verbose = true;
    else if (a == "--play")        o.play = true;
    else if (a == "--block")       o.block = static_cast<unsigned>(std::stoul(need("--block")));
    else if (a == "--latency")     o.latency = static_cast<unsigned>(std::stoul(need("--latency")));
    else if (a == "--max-voices") {
      o.max_voices_set = true;
      o.max_voices = static_cast<unsigned>(std::stoul(need("--max-voices")));
    }
    else if (a == "--version") {
      // The commit is read at CMake configure time and can be stale - it has
      // twice reported the wrong thing on this project. The source hash is
      // taken at build time from the files actually compiled in and is the
      // identity to quote in a measurement.
      std::printf("emusc-render %s\n"
                  "libEmuSC source-sha256 %s (%s files)\n"
                  "libEmuSC commit %s (%s, read at configure time - may be stale)\n"
                  "libEmuSC version string %s\n"
                  "libEmuSC source %s\n",
                  EMUSC_RENDER_VERSION, EMUSC_RENDER_LIBEMUSC_SOURCE_SHA,
                  EMUSC_RENDER_LIBEMUSC_SOURCE_FILES, EMUSC_RENDER_LIBEMUSC_COMMIT,
                  EMUSC_RENDER_LIBEMUSC_REF, EmuSC::Synth::version().c_str(),
                  EMUSC_RENDER_LIBEMUSC_SOURCE_DIR);
      std::exit(0);
    }
    else if (a == "--help" || a == "-h") { std::printf("%s", USAGE); std::exit(0); }
    else if (!a.empty() && a[0] == '-') die(1, "unknown option " + a + "\n" + USAGE);
    else positional.push_back(a);
  }
  if (!positional.empty()) {
    if (!o.in.empty() || !o.out.empty() || positional.size() > 2)
      die(1, std::string("give either <input.mid> [<output.wav>], or --midi [--out], not a mix\n") + USAGE);
    o.in = positional[0];
    if (positional.size() == 2) o.out = positional[1];
  }
  if (o.in.empty())
    die(1, std::string("no input: give <input.mid> or --midi\n") + USAGE);
  // Without --play the render has to go somewhere, so the output file is
  // defaulted; with --play and no output named, the run is audio-only and no
  // file is written.
  if (o.out.empty() && !o.play) {
    o.out = default_out_path(o.in);
    if (o.out.empty())
      die(1, "the input is named .wav, so the default output could overwrite it; give --out");
    std::fprintf(stderr, "emusc-render: --out defaulted to %s\n", o.out.c_str());
  }

  if (o.reset != "gm" && o.reset != "gs" && o.reset != "none")
    die(1, "--reset must be gm, gs or none");
  if (o.tail < 0) die(1, "--tail must be >= 0");
  if (o.block < 1) die(1, "--block must be >= 1");
  if (o.max_voices_set && o.max_voices < 1)
    die(1, "--max-voices must be >= 1");

  // Explicit --control-rom and --wave-rom together bypass the ROM directory
  // and device defaults entirely. Otherwise the directory is defaulted, and
  // the device is taken from it when exactly one device's control ROM is
  // there: this only saves typing a name the directory already implies, so
  // anything else is reported rather than resolved by picking one.
  bool explicit_roms = !o.control_rom.empty() && !o.wave_roms.empty();
  if (o.device.empty() && !explicit_roms) {
    std::string dir = o.rom_dir.empty() ? emusc_tools::default_rom_dir() : o.rom_dir;
    std::vector<std::string> found = emusc_tools::devices_in_rom_dir(dir);
    if (found.size() == 1) {
      o.device = found[0];
      std::fprintf(stderr, "emusc-render: --device defaulted to %s, the only "
                   "device with a control ROM in %s\n", o.device.c_str(), dir.c_str());
    } else if (found.empty()) {
      die(1, "no device given and no <device>_control.bin found in ROM directory " +
             dir + "; give --device with --rom-dir, or --control-rom and --wave-rom");
    } else {
      std::string list;
      for (auto &d : found) list += (list.empty() ? "" : ", ") + d;
      die(1, "no device given and ROM directory " + dir + " holds control ROMs for " +
             std::to_string(found.size()) + " devices (" + list +
             "); choose one with --device");
    }
  }

  if (!o.device.empty()) {
    bool known = false;
    for (const char *d : emusc_tools::SUPPORTED_DEVICES)
      if (o.device == d) known = true;
    if (!known)
      die(1, "--device must be sc55, sc55mkii, sc88, jv880, or jv1080");
    // One naming convention for all four devices: <device>_control.bin is
    // always the control/program ROM, <device>_cpu.bin is the internal CPU
    // ROM that only SC-55 and SC-55mkII have. SC-88 has no separate CPU ROM,
    // and JV-880's second physical ROM chip (DeviceProfile::romSize in
    // engines/gp/devices/jv880.cc) is what "control" means for it; JV-880 has no
    // <device>_cpu.bin because its other chip is never read at all.
    // The same default emuscd and emusc-winmidi use, so all three tools
    // share one ROM-location convention.
    std::string dir = o.rom_dir.empty() ? emusc_tools::default_rom_dir() : o.rom_dir;
    if (o.control_rom.empty())
      o.control_rom = dir + "/" + o.device + "_control.bin";
    if (o.cpu_rom.empty() && (o.device == "sc55" || o.device == "sc55mkii"))
      o.cpu_rom = dir + "/" + o.device + "_cpu.bin";
    if (o.wave_roms.empty()) {
      // Chip counts: SC-55 3, SC-55mkII 2 (waverom bank layout), the two
      // XP-family devices 4 (XP_WAVE_CHIP_COUNT in
      // engines/xp/devices/profile.h), JV-880 2 (DeviceProfile
      // waveRomBanks in engines/gp/devices/jv880.cc).
      int n = (o.device == "sc55") ? 3 : (o.device == "sc55mkii") ? 2 :
              (o.device == "sc88" || o.device == "jv1080") ? 4 : 2;
      for (int k = 1; k <= n; k++)
        o.wave_roms.push_back(dir + "/" + o.device + "_waverom" + std::to_string(k) + ".bin");
    }
  }
  if (o.control_rom.empty() || o.wave_roms.empty())
    die(1, "ROMs not specified: give --device with --rom-dir, or --control-rom and --wave-rom");
  return o;
}

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
  set_flush_denormals_to_zero();

  Options o = parse_args(argc, argv);

  // libEmuSC reports progress on stdout. Keep it clean unless --verbose.
  // Redirecting the file descriptor itself, rather than an iostream buffer,
  // catches libEmuSC's stdio-based prints the same as it would iostream's.
  int saved_stdout_fd = -1;
  if (!o.verbose) {
    std::fflush(stdout);
    saved_stdout_fd = EMUSC_DUP(fileno(stdout));
    int null_fd = EMUSC_OPEN(kNullDevice, kNullFlags);
    if (null_fd >= 0) {
      EMUSC_DUP2(null_fd, fileno(stdout));
      EMUSC_CLOSE(null_fd);
    }
  }
  auto restore_cout = [&]() {
    if (saved_stdout_fd < 0) return;
    std::fflush(stdout);
    EMUSC_DUP2(saved_stdout_fd, fileno(stdout));
    EMUSC_CLOSE(saved_stdout_fd);
    saved_stdout_fd = -1;
  };

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
              (o.device == "jv880" && gen == EmuSC::ControlRom::SynthGen::JV880) ||
              (o.device == "jv1080" && gen == EmuSC::ControlRom::SynthGen::JV1080);
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

  // The default output rate is the device's own, where the device's rate is
  // established: the SC-88's DAC images mirror 32.000 kHz (scdb sc88
  // `M-166`, and its 24.576 MHz crystal divides to it by 768), and the
  // JV-1080 carries the same sound-generator part on the same crystal. The
  // SC-55, SC-55mkII and JV-880 run three different engine clocks and none
  // of their rates is settled (scdb `01_hardware/hardware.md` for each).
  // For those the default is a convenience fallback only: 32000 Hz is the
  // figure commonly published for them, which may describe a nominal output
  // rather than the engine's own rate and has not been verified here, so
  // every run that uses it says so.
  if (o.rate == 0) {
    auto gen = ctrl->generation();
    o.rate = 32000;
    if (gen != EmuSC::ControlRom::SynthGen::SC88 &&
        gen != EmuSC::ControlRom::SynthGen::JV1080)
      std::fprintf(stderr, "emusc-render: note: --rate defaulted to 32000 Hz - "
                   "a commonly published spec for the %s, not independently "
                   "verified by this project; pass --rate explicitly if you "
                   "need a different one\n", ctrl->model().c_str());
  }

  std::fprintf(stderr, "emusc-render: control ROM %s v%s (%s), wave ROM v%s (%s)\n",
               ctrl->model().c_str(), ctrl->version().c_str(), ctrl->date().c_str(),
               wave->version().c_str(), wave->date().c_str());

  // ---- Synth ----------------------------------------------------------------
  EmuSC::Synth::SoundMap map = (o.reset == "gm") ? EmuSC::Synth::SoundMap::GS_GM
                                                 : EmuSC::Synth::SoundMap::GS;
  EmuSC::Synth synth(*ctrl, *wave, map);
  // Synth's constructor seeds libEmuSC's random source from the wall clock;
  // libEmuSC draws from it for random pan, random pitch and the sample-and-hold
  // LFO. Re-seed with a fixed value so the render is reproducible.
  EmuSC::Synth::seed_random(o.seed);
  if (o.max_voices_set) synth.set_max_voices(o.max_voices);
  synth.set_audio_format(o.rate, 2);   // also instantiates the 16 parts

  // Power-on reset, before the first event is queued. --reset selects the sound
  // MAP, which the constructor above already applied; it does not put the parts
  // on their default instrument, and only reset(map, resetParts=true) does.
  //
  // On a Sound Canvas that is redundant here: Settings::reset() runs exactly
  // the four _initialize_*/_apply_device_performance calls the Settings
  // constructor runs, so the synth already holds bank 0 program 0 on every part
  // and Drum1 on part 10, and Part::reset() only clears notes and counters that
  // are already clear on a part built moments ago. It is here because the tool
  // should start the device the way the device starts itself, so that a file
  // which plays a note before its first program change is rendered from a
  // stated default rather than an implied one.
  //
  // It is not redundant on a device whose profile names a reset Performance
  // (DeviceProfile's PerformanceLayout::resetSelector): there this call is what
  // takes the machine out of its layered boot Performance, and the render is
  // not the same without it.
  //
  // --reset none skips this call, for comparing against a renderer that
  // never injects a reset of its own and leaves the map exactly as the
  // constructor above set it (GS).
  if (o.reset != "none")
    synth.reset(map, true);

  // What the reset left the device in, where the flag's name would say
  // something else.
  std::string resetLabel = o.reset;
  if (ctrl->generation() == EmuSC::ControlRom::SynthGen::JV1080)
    resetLabel += o.reset == "none" ? " (patch mode)" : " (GM mode)";

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

  std::string timeBase = midi.smpte ? "SMPTE time base"
                                     : std::to_string(midi.ppqn) + " ppqn";
  std::fprintf(stderr,
               "emusc-render: %s: format %d, %d tracks, %s, %zu tempo change(s), "
               "%zu channel events, %zu SysEx, %zu meta\n",
               o.in.c_str(), midi.format, midi.ntracks, timeBase.c_str(),
               midi.n_tempo_changes, n_channel, n_sysex, n_meta);
  std::fprintf(stderr,
               "emusc-render: last event at frame %llu (%g s), end-of-track at "
               "%g s, rendering %llu frames at %u Hz (%g s), reset=%s, seed=%u\n",
               (unsigned long long) last_event_frame,
               (double) last_event_frame / o.rate,
               (double) midi.end_of_track_num / midi.time_den,
               (unsigned long long) total_frames, o.rate,
               (double) total_frames / o.rate, resetLabel.c_str(), o.seed);

  // ---- Render ---------------------------------------------------------------
  try {
    std::unique_ptr<WavWriter> wav;
    if (!o.out.empty()) wav.reset(new WavWriter(o.out, o.rate, 2, o.as_float));
    // Opened after the WAV file, so a bad --out path is reported before the
    // audio device claims the sound card for a run that was going to fail
    // anyway.
    std::unique_ptr<AudioOut> audio_out;
    if (o.play) audio_out.reset(new AudioOut(o.rate, o.block, o.latency));

    std::vector<int16_t> buf;
    std::vector<float> fbuf;
    // --play always needs 16-bit frames to hand the audio device, even when
    // --bits 32/--float is also given for the WAV file - kept separate from
    // `buf` so the two flush independently and float mode still works.
    std::vector<int16_t> play_buf;
    buf.reserve(2 * 4096);
    fbuf.reserve(2 * 4096);
    play_buf.reserve(2 * (size_t) o.block);
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
        std::fprintf(stderr, "emusc-render: %d of %zu tracks play into MIDI port B\n",
                     onB, trackPort.size());
    }

    // --play real-time headroom: printed periodically so a render that
    // cannot keep up shows exactly when it falls behind, rather than just
    // sounding increasingly delayed with no way to tell why. "window" is
    // since the last report, "overall" is since playback started - a
    // steadily healthy window ratio with a falling overall ratio means an
    // early one-time stall (e.g. ROM load) rather than a sustained deficit.
    const auto play_start = std::chrono::steady_clock::now();
    auto window_start = play_start;
    uint64_t window_start_frame = 0;

    // Samples at full scale, counted here on what the tool writes rather
    // than taken from the library's own report: anything that rounds to
    // the 16-bit ceiling, the level a clip leaves in the file.
    const float kFullScale = 32766.5f / 32767.0f;
    uint64_t full_scale = 0, first_full_scale = 0, last_full_scale = 0;

    // --gain-db 0 (the default) converts to exactly 1.0f, and apply_gain()
    // then never touches a sample, so leaving the flag unset renders
    // byte-identically to a build without this option at all.
    const float gain_lin = emusc_tools::gain_db_to_linear(o.gain_db);

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
            std::fprintf(stderr, "emusc-render: SysEx longer than 65535 bytes skipped\n");
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
      emusc_tools::apply_gain(l, r, gain_lin);
      // Float output has no quantization step of its own to clamp it, so a
      // requested gain that pushes a sample past +-1.0 is clamped here
      // explicitly; skipped along with apply_gain() itself when no gain was
      // requested, so a default render's float samples are never touched.
      if (o.gain_db != 0.0 && o.as_float) {
        if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
        if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;
      }
      for (float v : {l, r})
        if (std::fabs(v) >= kFullScale) {
          if (!full_scale++) first_full_scale = fr;
          last_full_scale = fr;
        }
      if (wav) {
        if (o.as_float) {
          fbuf.push_back(l);
          fbuf.push_back(r);
          if (fbuf.size() >= 2 * 4096) { wav->write(fbuf.data(), fbuf.size() / 2); fbuf.clear(); }
        } else {
          buf.push_back(to_i16(l));
          buf.push_back(to_i16(r));
          if (buf.size() >= 2 * 4096) { wav->write(buf.data(), buf.size() / 2); buf.clear(); }
        }
      }
      if (audio_out) {
        play_buf.push_back(to_i16(l));
        play_buf.push_back(to_i16(r));
        if (play_buf.size() >= 2 * (size_t) o.block) {
          audio_out->write(play_buf.data(), play_buf.size() / 2);
          play_buf.clear();

          const auto now = std::chrono::steady_clock::now();
          if (now - window_start >= std::chrono::seconds(2)) {
            const double window_wall =
              std::chrono::duration<double>(now - window_start).count();
            const double window_audio =
              (double)(fr + 1 - window_start_frame) / o.rate;
            const double overall_wall =
              std::chrono::duration<double>(now - play_start).count();
            const double overall_audio = (double)(fr + 1) / o.rate;
            std::fprintf(stderr,
                         "emusc-render: --play: window %.0f%% real-time, "
                         "overall %.0f%% real-time\n",
                         100.0 * window_audio / window_wall,
                         100.0 * overall_audio / overall_wall);
            window_start = now;
            window_start_frame = fr + 1;
          }
        }
      }
    }
    if (wav) {
      if (!buf.empty()) wav->write(buf.data(), buf.size() / 2);
      if (!fbuf.empty()) wav->write(fbuf.data(), fbuf.size() / 2);
      wav->close();
      std::fprintf(stderr,
                   "emusc-render: wrote %llu frames to %s; libEmuSC reported "
                   "%u clipped samples\n",
                   (unsigned long long) wav->frames(), o.out.c_str(),
                   synth.get_num_clipped_samples(false));
    }
    if (full_scale)
      std::fprintf(stderr,
                   "emusc-render: %llu samples at full scale, frames "
                   "%llu-%llu (%.3f-%.3f s)\n",
                   (unsigned long long) full_scale,
                   (unsigned long long) first_full_scale,
                   (unsigned long long) last_full_scale,
                   (double) first_full_scale / o.rate,
                   (double) last_full_scale / o.rate);
    if (audio_out) {
      if (!play_buf.empty()) audio_out->write(play_buf.data(), play_buf.size() / 2);
      if (!wav)
        std::fprintf(stderr,
                     "emusc-render: played %llu frames; libEmuSC reported "
                     "%u clipped samples\n",
                     (unsigned long long) total_frames,
                     synth.get_num_clipped_samples(false));
    }
  } catch (const std::exception &e) {
    restore_cout();
    die(3, e.what());
  }

  restore_cout();
  return 0;
}
