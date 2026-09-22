/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * This file consists entirely of AI-generated code without direct human
 * authorship and is dedicated to the public domain under CC0 1.0.
 *
 *  Roland JV-1080 identification.
 *
 *  The JV-1080's synthesis is implemented by the XP engine, which reads the
 *  control ROM itself and owns every table it needs. This file therefore
 *  carries the banner and nothing else: a DeviceProfile here would restate
 *  what that engine already reads, and could contradict it. The engine's own
 *  parameters for this device are engines/xp/devices/jv1080.cc.
 *
 *  Unlike the JV-880, whose control ROM carries no banner at all and which
 *  is identified by a structural probe, this device's external program ROM
 *  holds a fixed firmware banner at a fixed offset - so it builds on the
 *  shared signature mechanism rather than on a bespoke one.
 */
#include <string>

#include "../control_rom.h"
#include "../device_profile.h"
#include "common/rom_signature.h"

namespace EmuSC
{

// PRG offset 0x04A1CE, read from the owner's own dump: forty bytes reading
// "NU-10B Version 1.02     1994/09/12 00:37". The five spaces between the
// version and the date are in the ROM - a single-space transcription of this
// banner matches nothing in the image - so the match runs over the whole
// forty bytes rather than over a prefix that might recur. Version and date
// are picked out of the banner below, since where they sit inside it is
// this device's own layout rather than the shared mechanism's.
//
// The banner is the only fixed string in the image that names the firmware;
// none of the Sound Canvas family's signatures match anywhere here.
const RomSignature JV1080_SIGNATURE = {
  "JV-1080", 0x04a1ce, 40, "NU-10B Version 1.02     1994/09/12 00:37", 40,
  RomVersionStyle::Unknown, 0
};

static const int JV1080_VERSION_AT = 15;
static const int JV1080_VERSION_LENGTH = 4;
static const int JV1080_DATE_AT = 24;
static const int JV1080_DATE_LENGTH = 10;

static bool jv1080_identify(const std::vector<uint8_t> &rom,
                             const DeviceProfile *profile,
                             std::string &model, std::string &version,
                             std::string &date)
{
  (void) profile;   // no GP profile for this device - see the file comment
  if (!try_rom_signature(rom, JV1080_SIGNATURE, version, date))
    return false;

  const char *banner = (const char *) &rom[JV1080_SIGNATURE.offset];
  version.assign(banner + JV1080_VERSION_AT, JV1080_VERSION_LENGTH);
  date.assign(banner + JV1080_DATE_AT, JV1080_DATE_LENGTH);
  model = JV1080_SIGNATURE.modelName;
  return true;
}

extern const ControlRom::DeviceEntry JV1080_DEVICE = {
  ControlRom::sm_JV1080, ControlRom::SynthGen::JV1080, nullptr, jv1080_identify
};

}
