/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * This file consists entirely of AI-generated code without direct human
 * authorship and is dedicated to the public domain under CC0 1.0.
 *
 *  Roland JV-880 identification.
 *
 *  The JV-880's synthesis is implemented by the GP engine, whose own device
 *  profile is engines/gp/devices/jv880.cc. This file carries only what is
 *  needed to recognise the ROM before any engine is selected.
 *
 *  Unlike the Sound Canvas family, the JV control ROM carries no GS banner:
 *  there is no fixed byte string to match anywhere in it. It is identified
 *  by its own table structure instead - this device's identify is the one
 *  in this file that does not build on devices/common/rom_signature.h.
 */
#include <cctype>

#include "../control_rom.h"
#include "../device_profile.h"

namespace EmuSC
{

extern const DeviceProfile JV880_PROFILE;

static bool jv880_identify(const std::vector<uint8_t> &rom,
                            const DeviceProfile *profile,
                            std::string &model, std::string &version,
                            std::string &date)
{
  const WaveformTableLayout &wave = profile->records->waveform;

  auto namelike = [&rom, &wave](uint32_t offset) -> bool {
    if ((size_t) offset + wave.nameLength > rom.size())
      return false;
    int alnum = 0;
    for (int i = 0; i < wave.nameLength; i++) {
      uint8_t ch = rom[offset + i];
      if (ch < 0x20 || ch > 0x7e)
        return false;
      if (isalnum(ch))
        alnum++;
    }
    return alnum >= 3;
  };

  // A run, not a single record: isolated printable triples occur by chance.
  // No separate ROM-size gate is needed: namelike()'s own bounds check
  // already rejects a ROM too small to hold this table, and checking size
  // here would only ever reorder which of several *different* devices gets
  // tried first - moot with one device on this path.
  int run = 0;
  for (int k = 0; k < 8; k++)
    run += namelike(wave.offset + k * wave.stride) ? 1 : 0;

  if (run < 8)
    return false;

  model = profile->name;
  version = "?";
  date = "?";
  return true;
}

extern const ControlRom::DeviceEntry JV880_DEVICE = {
  ControlRom::sm_JV880, ControlRom::SynthGen::JV880, &JV880_PROFILE,
  jv880_identify
};

}
