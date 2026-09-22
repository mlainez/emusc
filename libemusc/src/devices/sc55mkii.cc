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
 *  Roland SC-55mkII identification.
 *
 *  The SC-55mkII's synthesis is implemented by the GP engine, whose own
 *  device profile is engines/gp/devices/sc55mkii.cc. This file carries only
 *  what is needed to recognise the ROM before any engine is selected.
 */
#include "../control_rom.h"
#include "../device_profile.h"
#include "common/rom_signature.h"

namespace EmuSC
{

// A GS banner, with the version and a BCD date held elsewhere.
const RomSignature SC55MKII_SIGNATURE = {
  "SC-55mkII", 0x3d148, 32, "GS-28 VER=2.00  SC              ", 32,
  RomVersionStyle::SeparateBcd, 0xfff0
};

// The SCB-55 is the same machine on a card, and carries no version we can read.
const RomSignature SCB55_SIGNATURE = {
  "SCB-55 (SC-55mkII)", 0x3d148, 32, "GS-28 VER=2.00  LCGS-3 module   ", 32,
  RomVersionStyle::Unknown, 0
};

static bool sc55mkii_identify(const std::vector<uint8_t> &rom,
                               const DeviceProfile *profile,
                               std::string &model, std::string &version,
                               std::string &date)
{
  (void) profile;
  if (try_rom_signature(rom, SC55MKII_SIGNATURE, version, date)) {
    model = SC55MKII_SIGNATURE.modelName;
    return true;
  }
  if (try_rom_signature(rom, SCB55_SIGNATURE, version, date)) {
    model = SCB55_SIGNATURE.modelName;
    return true;
  }
  return false;
}

extern const DeviceProfile SC55MKII_PROFILE;

extern const ControlRom::DeviceEntry SC55MKII_DEVICE = {
  ControlRom::sm_SC55mkII, ControlRom::SynthGen::SC55mk2, &SC55MKII_PROFILE,
  sc55mkii_identify
};

}
