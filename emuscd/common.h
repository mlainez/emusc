// Pieces of the Roland Sound Canvas daemon shared between its ALSA backend
// (main.cc, Linux) and its WinMM backend (main_winmidi.cc, Windows): ROM
// filename resolution, which touches neither platform's audio/MIDI API.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>

#include "../libemusc/src/synth.h"

namespace emuscd {

inline const char *SUPPORTED_DEVICES[] = { "sc55", "sc55mkii", "sc88", "jv880" };

// The sound map emuscd runs in. Named once so that the value handed to the
// Synth constructor and the value handed to its power-on reset cannot drift
// apart: reset(sm, ...) re-applies the map, so passing a different one there
// would silently switch the device into another mode. GS is the Synth default
// and the mode all four supported devices are addressed in; emuscd exposes no
// option to change it.
inline const EmuSC::Synth::SoundMap SOUND_MAP = EmuSC::Synth::SoundMap::GS;

inline bool device_supported(const std::string &dev) {
  for (const char *d : SUPPORTED_DEVICES)
    if (dev == d) return true;
  return false;
}

struct DeviceRoms {
  std::string control_rom, cpu_rom;
  std::vector<std::string> wave_roms;
};

// One naming convention for all four devices, shared with emusc-render's
// --device preset (libemusc/tools/main.cc): <device>_control.bin is always
// the control/program ROM, <device>_cpu.bin is the internal CPU ROM that
// only SC-55 and SC-55mkII have. See README.md for exact file hashes.
inline DeviceRoms resolve_device_roms(const std::string &device, const std::string &rom_dir) {
  DeviceRoms r;
  r.control_rom = rom_dir + "/" + device + "_control.bin";
  if (device == "sc55" || device == "sc55mkii")
    r.cpu_rom = rom_dir + "/" + device + "_cpu.bin";

  int n = (device == "sc55") ? 3 : (device == "sc88") ? 4 : 2;  // sc55mkii, jv880
  for (int k = 1; k <= n; k++)
    r.wave_roms.push_back(rom_dir + "/" + device + "_waverom" + std::to_string(k) + ".bin");
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

}  // namespace emuscd
