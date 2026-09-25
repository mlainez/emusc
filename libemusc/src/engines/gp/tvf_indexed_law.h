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


#ifndef __TVF_INDEXED_LAW_H__
#define __TVF_INDEXED_LAW_H__


#include "tvf_law.h"
#include "../../control_rom.h"
#include "../../device_profile.h"

#include <array>
#include <cstdint>


namespace EmuSC { namespace Gp {


// The filter chain that builds a cutoff-table INDEX from the envelope, key
// follow, part parameters and LFOs, and reads the coefficient from the cutoff
// and resonance tables (TvfLawKind::SoundCanvasIndex).
class IndexedTvfLaw : public TvfLaw
{
public:
  IndexedTvfLaw(ControlRom::InstPartial &instPartial, uint8_t key,
                uint8_t velocity, ControlRom::LookupTables &LUT,
                const TvfCutoffLaw &cutoffLaw, TvfPartControls &controls,
                const TvfLfoInputs &lfo, SVF &svf);
  ~IndexedTvfLaw() override = default;

  void update(const TvfLfoInputs &lfo, SVF &svf) override;
  void apply_sample_set(SVF &svf, std::array<float, 256> &dryBus) override;

  // The coefficient this control period ends on, in SVF::set_cutoff_freq()'s
  // units; the mode word whose low byte is the speed of the move to it; and
  // the resonance index handed to SVF::set_resonance().
  int cutoff_level(void) const { return _envLevel; }
  int cutoff_mode(void) const { return _envLevelMode; }
  int resonance(void) const { return _resonance; }

private:
  ControlRom::LookupTables &_LUT;
  ControlRom::InstPartial &_instPartial;
  const TvfCutoffLaw &_cutoffLaw;
  TvfPartControls &_controls;

  // A segment this short or shorter snaps instantly.
  int _instantTicks = 8;

  bool _lfo1FadeComplete;
  bool _lfo2FadeComplete;
  int _lfo1Depth;
  int _lfo2Depth;

  int _L1Init;
  int _L2Init;
  int _L3Init;
  int _L4Init;
  int _L5Init;

  int _ipLevelInit;

  int _currentEnvTime;
  int _currentLevelInit;
  int _prevLevelInit;

  int _resIndexFreq;

  // First cutoff-table index whose coefficient exceeds the 0xe600 cap that
  // _iterate_phase() applies, i.e. the first index the filter cannot reach.
  // constexpr, not const: std::max takes its arguments by reference, which
  // odr-uses this and so needs a definition. Without one the library links only
  // as a shared object, where an undefined symbol is tolerated, and every
  // static link of libEmuSC fails to resolve it.
  static constexpr int _cutoffCeiling = 121;

  int _resonance;

  int _envDepth;

  int _envLevel;
  int _envLevelMode;
  int _prevEnvLevel;

  std::array<int, 256> _coFreq;    // Cutoff frequency for each sample

  uint8_t _key;
  int _velocity;

  int _coFreqVSens;

  int _keyFollow;

  int _get_velocity_from_vcurve(uint8_t velocity);

  void _init_envelope(void);
  void _init_freq_and_res(void);

  void _update_lfo_depth(int lfo);

  int _get_cof_key_follow(int cofkfROM);
  int _get_level_init(int level);

  int _read_cutoff_freq_vel_sens(int cofvsROM);

  inline bool _le_native(void) { uint16_t n = 1; return (*(uint8_t *) & n); }
  uint16_t _native_endian_uint16(uint8_t *ptr);

  void _init_new_phase(enum Phase newPhase) override;
  void _iterate_phase(void) override;

  void _smooth_cutoff(void);
};

}}  // namespace EmuSC::Gp

#endif  // __TVF_INDEXED_LAW_H__
