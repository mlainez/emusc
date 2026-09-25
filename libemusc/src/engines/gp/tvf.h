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


#ifndef __TVF_H__
#define __TVF_H__


#include "svf.h"
#include "tvf_law.h"
#include "../../control_rom.h"
#include "../../settings.h"
#include "wave_generator.h"

#include <array>
#include <cstdint>


namespace EmuSC { namespace Gp {


// The partial's filter: the state-variable filter core, and the law that
// drives its coefficients, chosen once from the device profile's TvfLawKind.
class TVF
{
public:
  // jvCtrlAcc is the per-tone controller matrix's twelve destination
  // accumulators (scdb D-79), owned by Partial and rebuilt every control
  // period. Null on every device without a matrix, and the whole matrix is
  // then inert. It is a CONSTRUCTOR argument and not a setter because the
  // note's first filter coefficient is computed here, with the controller
  // already in it.
  TVF(ControlRom::InstPartial &instPartial, uint8_t key, uint8_t velocity,
      WaveGenerator *LFO1, WaveGenerator *LFO2, ControlRom::LookupTables &LUT,
      Settings *settings, int8_t partId, const int *jvCtrlAcc = nullptr);
  ~TVF();

  void apply_sample_set(std::array<float, 256> &dryBus);
  void update(void);

  void note_off(uint8_t releaseVelocity = 64);

  // The filter envelope's current level, for the part callback. 0 while the
  // filter is disabled.
  int get_envelope_value(void)
  { return _law ? _law->get_envelope_value() : 0; }

private:
  // A part's live parameters, read from Settings for the law.
  class PartControls : public TvfPartControls
  {
  public:
    PartControls(Settings *settings, int8_t partId)
      : _settings(settings), _partId(partId) {}

    uint8_t patch_param(enum PatchParam pp) override
    { return _settings->get_param(pp, _partId); }
    int controller(enum Settings::ControllerParam cp) override
    { return _settings->get_acc_control_param(cp, _partId); }

  private:
    Settings *_settings;
    int8_t _partId;
  };

  WaveGenerator *_LFO1;
  WaveGenerator *_LFO2;

  PartControls _controls;

  // Both null when the partial's filter is disabled; otherwise both set.
  SVF *_svf;
  TvfLaw *_law;

  TvfLfoInputs _lfo_inputs(void);

  TVF();
};

}}  // namespace EmuSC::Gp

#endif  // __TVF_H__
