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
 *  The identification mechanism shared by every device with a fixed banner
 *  in its ROM: a byte string at a known offset, optionally followed by a
 *  version and date in one of two encodings. A device with no such banner
 *  (the JV family) does not use this at all - see its own identify function.
 */
#ifndef EMUSC_DEVICES_COMMON_ROM_SIGNATURE_H
#define EMUSC_DEVICES_COMMON_ROM_SIGNATURE_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace EmuSC
{

enum class RomVersionStyle
{
  Unknown,        // the ROM does not carry one we can read
  Inline,         // version and date sit inside the signature block itself
  SeparateBcd     // version elsewhere, followed by a BCD year/month/day
};

struct RomSignature
{
  const char     *modelName;
  uint32_t        offset;
  int             readLength;
  const char     *match;
  int             matchLength;
  RomVersionStyle versionStyle;
  uint32_t        versionOffset;        // SeparateBcd only
};

// Checks the signature against the ROM and, on a match, fills version/date
// per its own versionStyle. Knows nothing about which device it's checking -
// every byte position it reads comes from the signature itself.
inline bool try_rom_signature(const std::vector<uint8_t> &rom,
                               const RomSignature &sig,
                               std::string &version, std::string &date)
{
  if ((uint64_t) sig.offset + sig.readLength > rom.size())
    return false;

  const char *data = (const char *) &rom[sig.offset];
  if (sig.readLength < sig.matchLength ||
      std::strncmp(data, sig.match, sig.matchLength) != 0)
    return false;

  switch (sig.versionStyle) {
  case RomVersionStyle::Inline:
    version.assign(&data[3], 4);
    date.assign(&data[24], 5);
    break;

  case RomVersionStyle::SeparateBcd: {
    if ((uint64_t) sig.versionOffset + 10 > rom.size())
      return false;
    const uint8_t *v = &rom[sig.versionOffset];
    version.assign((const char *) v, 4);
    char dateBuf[16];
    std::snprintf(dateBuf, sizeof(dateBuf), "19%x-%x-%x",
                  (unsigned) v[7], (unsigned) v[8], (unsigned) v[9]);
    date.assign(dateBuf);
    break;
  }

  case RomVersionStyle::Unknown:
    version.assign("?");
    date.assign("?");
    break;
  }

  return true;
}

}

#endif  // EMUSC_DEVICES_COMMON_ROM_SIGNATURE_H
