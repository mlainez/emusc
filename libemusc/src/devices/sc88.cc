/*
 *  This file is part of libEmuSC, a Sound Canvas emulator library
 *
 *  Roland SC-88 identification only.
 *
 *  The SC-88's ROM layout is not known, so it has no profile: the ROM is
 *  recognised well enough to give the user a clear message instead of a wrong
 *  sound. When the layout is worked out, a profile joins this signature here.
 */

#include "../device_profile.h"

namespace EmuSC
{

const RomSignature SC88_SIGNATURE = {
  "SC-88", 0x7fc0, 24, "GS-64 VER=3.00  SC-88   ", 24,
  RomVersionStyle::Unknown, 0
};

}
