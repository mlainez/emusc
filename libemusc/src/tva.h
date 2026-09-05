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


#ifndef __TVA_H__
#define __TVA_H__


#include "control_rom.h"
#include "envelope.h"
#include "settings.h"
#include "slew_calc.h"
#include "wave_generator.h"

#include <array>
#include <cstdint>


namespace EmuSC {


class TVA : public Envelope
{
public:
  TVA(ControlRom &ctrlRom, uint8_t key, uint8_t velocity, int sampleIndex,
      WaveGenerator *LFO1, WaveGenerator *LFO2, Settings *settings,
      int8_t partId, uint16_t instrumentIndex, int partialId);

  void update(bool reset = false);
  void apply(double *sample);
  // dryBus receives the partial's panned stereo output. sendBuf receives the
  // same block as it stands *before* the panner, which is where the reverb and
  // chorus sends are taken from (PROVENANCE.md P-0182).
  void apply_sample_set(std::array<std::array<float, 256>, 2> &dryBus,
			std::array<float, 256> &sendBuf);

  void note_off(uint8_t releaseVelocity = 64);

private:
  // From the device profile: a segment this short or shorter snaps instantly.
  int _instantTicks = 8;


  bool _initRunComplete;
  bool _firstBlock;                          // First control block of the note

  int _dynLevel;
  int _dynLevelMode;
  int _prevDynLevel;

  // The JV's static level byte: the level law's own result, which on that
  // device IS the dynamic register (chip F016, ROM1 0x38a4). Unused by the
  // Sound Canvas path, whose dynamic register is computed from expression,
  // part level and system volume instead.
  int _staticLevel8 = 0;

  // The tone side of the level law, fixed for the life of the voice, and the
  // patch's own level byte. The part side is recomposed every control period -
  // see _compose_static_level().
  int _toneGain = 0;
  int _patchLevel = 0x7f;

  // True on a device whose envelope register carries its OWN curve, so the
  // envelope walks in the PRE-curve domain and is converted at every emit.
  bool _envThroughCurve = false;

  int _envLevel;
  int _envLevelMode;
  int _prevEnvLevel;

  WaveGenerator *_LFO1;
  WaveGenerator *_LFO2;

  bool _lfo1FadeComplete;
  bool _lfo2FadeComplete;
  int _lfo1Depth;
  int _lfo2Depth;

  ControlRom::LookupTables &_LUT;
  ControlRom::InstPartial &_instPartial;

  uint8_t _key;
  uint8_t _velocity;              // raw note-on velocity (JV time sense)
  int _drumSet;

  int _panpotBase;
  int _panpot;
  int _panpotL;
  int _panpotR;
  bool _panpotLocked;

  Settings *_settings;
  int8_t _partId;

  SlewCalc _tvDyn, _tvEnv;
  uint16_t _dynLevelEC, _envLevelEC;         // External chip's mirror of levels
  std::array<float, 256> _slewDynGain, _slewEnvGain;

  TVA();

  void _init_update(void);
  void _set_panpot_gains(void);

  void _compose_static_level(void);
  void _init_envelope(ControlRom &ctrlRom, int sampleIndex, int instrumentIndex,
                      uint8_t cVelocityLvl,
                      uint8_t cVelocity);

  void _update_dynamic_level(void);
  void _update_panpot_level(bool reset);
  void _update_lfo_depth(int lfo);

  int _get_bias_level(int km, int biasPoint);
  int _env_register_value(int envValue) const;
  float _dryGain = 1.0f;   // JV Dry Level; unity on devices without one
  bool _partLevelInDynamics = true;  // false when the level law owns it

  int _get_velocity_from_vcurve(uint8_t velocity);
  int _get_level_velocity(int cVelocity);
  uint8_t _lvlVSensEff;   // partial 0: its own byte 66; partial 1: the SUM of
                          // both partials' byte 66 (measured; see tva.cc)

  void _init_new_phase(enum Phase newPhase);
  void _iterate_phase(void);

  static void _smooth(int mode, float start, float target,
                      std::array<float, 256> &gain, bool firstBlock);
  void _slew_function_dynvol(uint16_t mode);
  void _slew_function_envelope(uint16_t mode);
  };

}

#endif  // __TVA_H__
