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
#include "control_rom.h"
#include "device_profile.h"
#include "envelope.h"
#include "settings.h"
#include "wave_generator.h"

#include <array>
#include <cstdint>


namespace EmuSC {


class TVF : public Envelope
{
public:
  TVF(ControlRom::InstPartial &instPartial, uint8_t key, uint8_t velocity,
      WaveGenerator *LFO1, WaveGenerator *LFO2, ControlRom::LookupTables &LUT,
      Settings *settings, int8_t partId);
  ~TVF();

  void apply(float *sample);
  void apply_sample_set(std::array<float, 256> &dryBus);
  void update(void);

  void note_off();

private:
  // A segment this short or shorter snaps instantly. The Sound Canvas's value;
  // the JV's filter chain does not use it - it takes its segment durations from
  // the device's own millisecond table and a duration of 0 there IS the skip.
  int _instantTicks = 8;


  uint32_t _sampleRate;

  WaveGenerator *_LFO1;
  WaveGenerator *_LFO2;

  bool _lfo1FadeComplete;
  bool _lfo2FadeComplete;
  int _lfo1Depth;
  int _lfo2Depth;

  ControlRom::LookupTables &_LUT;
  ControlRom::InstPartial &_instPartial;

  int _L1Init;
  int _L2Init;
  int _L3Init;
  int _L4Init;
  int _L5Init;

  int _ipLevelInit;

  int _currentEnvTime;
  int _currentLevelInit;
  int _prevLevelInit;

  int _coFreqIndex;

  int _resIndexFreq;
  int _resIndexUsed;

  // First cutoff-table index whose coefficient exceeds the 0xe600 cap that
  // _iterate_phase() applies, i.e. the first index the filter cannot reach.
  static const int _cutoffCeiling = 121;

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

  SVF *_svf;

  Settings *_settings;
  int8_t _partId;

  // ---- The JV family's chain (PROVENANCE.md P-0390) ----------------------
  //
  // A different arithmetic, not the same one with different constants: this one
  // accumulates every modulation in CENTS, exponentiates the total and
  // multiplies the tone's base coefficient by it, then hands the two 16-bit
  // words the firmware computes straight to the filter. TvfLawKind picks between
  // the two chains the way LevelLawKind picks between the two level laws.
  bool _jv;
  const TvfJvLaw *_jvLaw;

  int _jvTickCount;        // control periods since the last envelope tick
  int _jvDecrement;        // envelope accumulator step for the current segment
  int _jvEnvLevel;         // envelope output, 0 .. 0x7f00
  int _jvEnvDepth;         // TVF-ENV Depth, scaled and signed
  int _jvVelAtten;         // velocity attenuation of the envelope level
  int _jvKeyFollow;        // key follow offset in cents
  int _jvLfo1Depth;        // LFO -> TVF depths, scaled and signed
  int _jvLfo2Depth;
  int _jvCutoff;           // the tone's base cutoff, 0..127
  int _jvResTarget;        // the resonance the tone asks for
  int _jvRes;              // the resonance after the per-tick slew
  int _jvWord;             // cutoff coefficient word, and the one before it,
  int _jvWordPrev;         // between which the coefficient moves
  float _jvQ1;             // damping, already in the filter's own units
  int _jvRampPos;          // samples into the move between the two words

  void _jv_init(uint8_t velocity);
  void _jv_iterate(void);
  void _jv_next_phase(void);
  void _jv_apply_sample_set(std::array<float, 256> &dryBus);
  int  _jv_velocity_attenuation(int curve, int sens, int velocity);

  TVF();

  int _get_velocity_from_vcurve(uint8_t velocity);

  void _init_envelope(void);
  void _init_freq_and_res(void);

  void _update_lfo_depth(int lfo);

  int _get_cof_key_follow(int cofkfROM);
  int _get_level_init(int level);

  int _read_cutoff_freq_vel_sens(int cofvsROM);

  inline bool _le_native(void) { uint16_t n = 1; return (*(uint8_t *) & n); }
  uint16_t _native_endian_uint16(uint8_t *ptr);

  void _init_new_phase(enum Phase newPhase);
  void _iterate_phase(void);

  void _smooth_cutoff(void);
};

}

#endif  // __TVF_H__
