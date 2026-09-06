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
 *  The fallback profile for a Sound Canvas ROM that is recognised but whose
 *  layout is not mapped.
 */
#include "../device_profile.h"

namespace EmuSC
{

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

  { 0, 0, 0, 0, 0, 0, 8, 1 },

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
    ReverbDelayTapLaw::LinearPerTime,

    // The Sound Canvas program and its own Time -> loop-gain line, unchanged.
    ReverbNetworkKind::SoundCanvasProgram,
    ReverbFeedbackLaw::SoundCanvasTimeLine
  },

  { true,  0x3f, 0x7f },

  { false, 0x100000 }
};

}
