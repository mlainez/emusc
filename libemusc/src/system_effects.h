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
 *  You should have received a copy of the GNU General Public License
 *  along with libEmuSC. If not, see <http://www.gnu.org/licenses/>.
 */


#ifndef __SYSTEM_EFFECTS_H__
#define __SYSTEM_EFFECTS_H__


#include "chorus.h"
#include "reverb.h"
#include "settings.h"

#include <stdint.h>


namespace EmuSC {


class SystemEffects
{
public:
  // The control ROM comes in because the chorus reads its type records from the
  // device's own ROM (PROVENANCE.md P-0394); nothing else here needs it.
  SystemEffects(Settings *settings, ControlRom &ctrlRom);
  ~SystemEffects();

  // Both effects take a single input sample per frame, and the parts feed
  // them their pre-pan signal (PROVENANCE.md P-0182), so the two input buses
  // are mono. The outputs are stereo.
  int apply(std::array<float, 256> &chorusBus,
	    std::array<float, 256> &reverbBus,
	    std::array<std::array<float, 256>, 2> &chorusOut,
	    std::array<std::array<float, 256>, 2> &reverbOut);
  void update(void);

private:
  Settings *_settings;

  Chorus *_chorus;
  Reverb *_reverb;

  SystemEffects();
};

}  // namespace EmuSC

#endif  // __SYSTEM_EFFECTS_H__
