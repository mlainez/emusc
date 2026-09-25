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

// TVF - Time Variant Filter
// A 2nd. order low or high pass filter per partial. The "TVF Type" variable in
// partial definitions specifies the filter type or whether the TVF filter is
// disabled. How the cutoff and resonance are derived from the partial, the
// part parameters and the LFOs is the device's filter law (tvf_law.h).


#include "tvf.h"


namespace EmuSC { namespace Gp {


TVF::TVF(ControlRom::InstPartial &instPartial, uint8_t key, uint8_t velocity,
         WaveGenerator *LFO1, WaveGenerator *LFO2,ControlRom::LookupTables &LUT,
         Settings *settings, int8_t partId, const int *jvCtrlAcc)
  : _LFO1(LFO1),
    _LFO2(LFO2),
    _controls(settings, partId),
    _svf(nullptr),
    _law(nullptr)
{
  if (instPartial.TVFType == 0)
    _svf = new SVF(SVF::Mode::LowPass);
  else if (instPartial.TVFType == 1)
    _svf = new SVF(SVF::Mode::HighPass);
  else
    return;                                  // TVF disabled

  _law = TvfLaw::create(*settings->device(), instPartial, key, velocity, LUT,
                        _controls, jvCtrlAcc, _lfo_inputs(), *_svf);
  if (_law == nullptr) {
    delete _svf;
    _svf = nullptr;
  }
}


TVF::~TVF()
{
  delete _law;
  delete _svf;
}


TvfLfoInputs TVF::_lfo_inputs(void)
{
  return { { _LFO1->value(), _LFO1->fade(), _LFO1->jv_raw() },
           { _LFO2->value(), _LFO2->fade(), _LFO2->jv_raw() } };
}


void TVF::apply_sample_set(std::array<float, 256> &dryBus)
{
  // Skip filter calculation if filter is disabled for this partial
  if (_law == nullptr)
    return;

  _law->apply_sample_set(*_svf, dryBus);
}


void TVF::note_off(uint8_t releaseVelocity)
{
  if (_law)
    _law->note_off(releaseVelocity);
}


// Run regularly for every 256 samples @32k sample rate => 125Hz
void TVF::update(void)
{
  if (_law == nullptr)                       // TVF disabled
    return;

  _law->update(_lfo_inputs(), *_svf);
}

}}  // namespace EmuSC::Gp
