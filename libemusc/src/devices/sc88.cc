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


// What the engine reads for a Sound Canvas generation with no profile of its
// own - the SC-88 here, and any ROM that is recognised but not mapped. The ROM
// layout members stay null on purpose: a reader that reaches for one should
// fail loudly rather than read a wrong offset. Only the synthesis constants are
// filled, and they are exactly the values reverb.cc, tva.cc and wave_rom.cc
// used to hold in their own else branches, so nothing changed when they moved.
const DeviceProfile SOUND_CANVAS_DEFAULT_PROFILE = {
  "Sound Canvas (unmapped generation)",

  0, 2,
  24,
  18.0f,

  nullptr,
  nullptr,

  nullptr, 0,

  { 0, 0, 0, 0, 0, 0, 0, 0, 8, 1 },

  LevelLawKind::SoundCanvasLogIndex,

  {
    1.829f, -11.9f, 187,

    // Return level, pre-LPF pair and delay taps. Shared across the family
    // because the reverb DSP is: see ReverbLaw. levelDivisor is not the
    // firmware's return law and ReverbLaw says why it still stands here.
    64.0f,
    ReverbReturnLaw::LevelOverDivisor,
    8, 0x3f, 4,
    0x16, 112,
    ReverbDelayTapLaw::LinearPerTime
  },

  { true,  0x3f, 0x7f },

  { false, 0x100000 }
};

}
