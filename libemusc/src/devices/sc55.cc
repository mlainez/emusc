/*
 *  This file is part of libEmuSC, a Sound Canvas emulator library
 *  Copyright (C) 2022-2026  Håkon Skjelten
 *
 *  libEmuSC is free software: you can redistribute it and/or modify it
 *  under the terms of the GNU Lesser General Public License as published
 *  by the Free Software Foundation, either version 2.1 of the License, or
 *  (at your option) any later version.
 *
 *  libEmuSC is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with libEmuSC. If not, see <http://www.gnu.org/licenses/>.
 *
 *  Roland SC-55 identification.
 *
 *  The SC-55's synthesis is implemented by the GP engine, whose own device
 *  profile is engines/gp/devices/sc55.cc. This file carries only what is
 *  needed to recognise the ROM before any engine is selected.
 */
#include "../control_rom.h"
#include "../device_profile.h"
#include "common/rom_signature.h"

namespace EmuSC
{

// "Ver" then a version and a date, in ASCII, in the same block.
const RomSignature SC55_SIGNATURE = {
  "SC-55", 0xf380, 29, "Ver", 3, RomVersionStyle::Inline, 0
};

// The SCC-1 is an SC-55 on an ISA card: same banks, same tables, same limits.
const RomSignature SCC1_SIGNATURE = {
  "SCC-1", 0x3d155, 29, "VER", 3, RomVersionStyle::Inline, 0
};

static bool sc55_identify(const std::vector<uint8_t> &rom,
                           const DeviceProfile *profile,
                           std::string &model, std::string &version,
                           std::string &date)
{
  (void) profile;
  if (try_rom_signature(rom, SC55_SIGNATURE, version, date)) {
    model = SC55_SIGNATURE.modelName;
    return true;
  }
  if (try_rom_signature(rom, SCC1_SIGNATURE, version, date)) {
    model = SCC1_SIGNATURE.modelName;
    return true;
  }
  return false;
}

extern const DeviceProfile SC55_PROFILE;

extern const ControlRom::DeviceEntry SC55_DEVICE = {
  ControlRom::sm_SC55, ControlRom::SynthGen::SC55, &SC55_PROFILE, sc55_identify
};

}
