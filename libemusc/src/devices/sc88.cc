/*
 *  This file is part of libEmuSC, a Sound Canvas emulator library
 *
 *  Roland SC-88 identification.
 *
 *  The SC-88's synthesis is implemented by the XP engine, which reads the
 *  control ROM itself and owns every table it needs. This file therefore
 *  carries the signature and nothing else: a DeviceProfile here would restate
 *  what that engine already reads, and could contradict it.
 */

#include "../control_rom.h"
#include "../device_profile.h"
#include "common/rom_signature.h"

namespace EmuSC
{

// The banner sits where the whole Sound Canvas family keeps it. Read from the
// owner's own dump: 0x7fc0 holds "GS-64 VER=3.00  SC-88", and the firmware
// revision "SC-88 Ver101" sits separately at 0x2ff80. Identification is
// unambiguous - none of the SC-55 family's signatures match anywhere in this
// image, including the SC-55's loose three-byte "Ver", which lands on erased
// 0xff bytes here.
const RomSignature SC88_SIGNATURE = {
  "SC-88", 0x7fc0, 21, "GS-64 VER=3.00  SC-88", 21,
  RomVersionStyle::Unknown, 0
};

static bool sc88_identify(const std::vector<uint8_t> &rom,
                           const DeviceProfile *profile,
                           std::string &model, std::string &version,
                           std::string &date)
{
  (void) profile;   // no GP profile for this device - see the file comment
  if (!try_rom_signature(rom, SC88_SIGNATURE, version, date))
    return false;
  model = SC88_SIGNATURE.modelName;
  return true;
}

extern const ControlRom::DeviceEntry SC88_DEVICE = {
  ControlRom::sm_SC88, ControlRom::SynthGen::SC88, nullptr, sc88_identify
};

}
