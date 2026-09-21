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


#include "svf.h"


namespace EmuSC {

SVF::SVF(Mode mode)
  : _mode(mode),
    _f(0.0f),
    _q(2.0f),
    _lp(0.0f),
    _bp(0.0f)
{}


void SVF::set_cutoff_freq(int coFreq)
{
  _f = coFreq / 32768.0f;
}


void SVF::set_resonance(int resonance)
{
  _q = resonance / 64.0f;
}


void SVF::clear()
{
  _lp = 0.0f;
  _bp = 0.0f;
}

}
