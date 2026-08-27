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


#include "tva.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string.h>


namespace EmuSC {


TVA::TVA(ControlRom &ctrlRom, uint8_t key, uint8_t velocity, int sampleIndex,
         WaveGenerator *LFO1, WaveGenerator *LFO2, Settings *settings,
         int8_t partId, uint16_t instrumentIndex, int partialId)
  : Envelope(ctrlRom.lookupTables),
    _initRunComplete(false),
    _dynLevel(0),
    _prevDynLevel(0),
    _envLevel(0),
    _LFO1(LFO1),
    _LFO2(LFO2),
    _lfo1FadeComplete(false),
    _lfo2FadeComplete(false),
    _lfo1Depth(0),                 // Both LFOs start their fade-in at 0, and
    _lfo2Depth(0),                 // _update_lfo_depth() only runs from the
                                   // second control block onwards, so the
                                   // first block must find a depth of 0 here
    _LUT(ctrlRom.lookupTables),
    _instPartial(ctrlRom.instrument(instrumentIndex).partials[partialId]),
    _key(key),
    _drumSet(settings->get_param(PatchParam::UseForRhythm, partId)),
    _panpotBase(ctrlRom.instrument(instrumentIndex).panKeyFlw ?
                ctrlRom.lookupTables.TVAPanKeyFollow[key] :
                (int) ctrlRom.instrument(instrumentIndex)
                        .partials[partialId].panpot),
    _panpot(-1),
    _panpotLocked(false),
    _settings(settings),
    _partId(partId),
    _dynLevelEC(0), _envLevelEC(0),
    _slewDynGain{}, _slewEnvGain{}
{
  int cVelocity = _get_velocity_from_vcurve(velocity);
  _init_envelope(ctrlRom, sampleIndex, instrumentIndex, cVelocity);

  // Calculate random pan if part pan or drum pan value is 0 (RND)
  // A note is locked to RND if it is started with that setting
  if (settings->get_param(PatchParam::PartPanpot, _partId) == 0 ||
      (_drumSet &&
       settings->get_param(DrumParam::Panpot, _drumSet - 1, _key) == 0)) {
    _panpot = std::rand() % 128;
    _panpotL = _LUT.TVAPanpot[_panpot];
    _panpotR = _LUT.TVAPanpot[0x80 - _panpot];
    _panpotLocked = true;
  }
}


// TVA consists of two values: dynamic volume corrections and envelope level.
// Each of these levels have their own "mode" variable controlling how they are
// interpolated / smoothed across and inside control loops of 256 samples.
void TVA::apply_sample_set(std::array<std::array<float, 256>, 2> &dryBus)
{
  auto norm = [](float v) { return v / 32768.0f; };
  _smooth(_envLevelMode, norm(_prevEnvLevel), norm(_envLevel), _slewEnvGain);

  float panL = _panpotL / 127.0f;
  float panR = _panpotR / 127.0f;

  for (int i = 0; i < 256; i++) {
    float sample = dryBus[0][i] * _slewDynGain[i] * _slewEnvGain[i];
    dryBus[0][i] = sample * panR;
    dryBus[1][i] = sample * panL;
  }
}


void TVA::note_off()
{
  set_phase(Envelope::Phase::Release);
}


// Run regularly for every 256 samples @32k sample rate => 125Hz
// TVA produces two values:
//  - TVA envelope level (ADSR)
//  - TVA dynamic level (expression, LFOs, controllers, volume levels)
void TVA::update(bool reset)
{
  // Run a specialized initialization update on first iteration
  if (!_initRunComplete) {
    _init_update();
    _initRunComplete = true;
    return;
  }

  // Update LFO depth parameters based on fade-in status
  if (!_lfo1FadeComplete)
    _update_lfo_depth(1);
  if (!_lfo2FadeComplete)
    _update_lfo_depth(2);

  // Update external envelope variable for e.g. bar display (clamp to 0xfe)
  _envelopeOut = std::min(_envLevel >> 8, 0xfe);

  // Iterate 8 ticks for the TVA envelope
  _iterate_phase();

  // Slew control
  _slew_function_dynvol(_dynLevelMode);

  // TODO: Move over to proper slew function for envelope output
  //  _slew_function_envelope(_envLevelMode);


  _update_dynamic_level();

  // TODO: Apply fade to dynamic volume if we are in portamento mode
  _dynLevel = (_dynLevel * 0xffff) >> 16;

  // Update dynamic volume mode
  if (std::abs(_prevDynLevel - _dynLevel) <= 0x10)
    _dynLevelMode = 0xff00;
  else
    _dynLevelMode = (_dynLevel & 0xff00) | 0xb4;

  _update_panpot_level(reset);

  if (0)
    std::cout << "TVA dv=0x" << std::hex << _dynLevel
              << " (mode=0x" << _dynLevelMode
              << ")  env=0x" << _envLevel
              << " (mode=0x" << _envLevelMode << ")" << std::endl;
}


// Initial update for initializing dynamic volume and envelope runs
void TVA::_init_update(void)
{
  // Initialize dynamic volume level & mode (0xba -> instant jump)
  _update_dynamic_level();
  _dynLevelMode = (_dynLevel & 0xff00) | 0xba;

  _update_panpot_level(true);

  // Initialize envelope track
  _init_new_phase(Phase::Attack1);

  // Make an initial envelope run and ensure proper slew mode
  _iterate_phase();
  if ((_envLevelMode & 0xff) == 0xaf)
    _envLevelMode = (_envLevelMode & 0xff00) | 0xba;

  _slew_function_dynvol(_dynLevelMode);
//  _slew_function_envelope(_envLevelMode);

  if(0)
    std::cout << "TVA init dv=0x" << std::hex << _dynLevel
              << " (mode=0x" << _dynLevelMode
              << ")  env=0x" << _envLevel
              << " (mode=0x" << _envLevelMode << ")" << std::endl;
}


void TVA::_update_lfo_depth(int lfo)
{
  // TVA LFO depth does not use a LUT, but is Control ROM value << 8
  if (lfo == 1) {
    if (_LFO1->fade() != UINT16_MAX) {
      _lfo1Depth = (_LFO1->fade() *
		    ((_instPartial.TVALFO1Depth & 0x7f) << 8)) >> 16;
    } else {
      _lfo1Depth = (_instPartial.TVALFO1Depth & 0x7f) << 8;
      _lfo1FadeComplete = true;
    }

    // MSB is used to flag invertion
    if (_instPartial.TVALFO1Depth >= 0x80)
      _lfo1Depth *= -1;

  } else if (lfo == 2) {
    if (_LFO2->fade() != UINT16_MAX) {
      _lfo2Depth = (_LFO2->fade() *
		    ((_instPartial.TVALFO2Depth & 0x7f)<< 8)) >> 16;
    } else {
      _lfo2Depth = (_instPartial.TVALFO2Depth & 0x7f) << 8;
      _lfo2FadeComplete = true;
    }

    if (_instPartial.TVALFO2Depth >= 0x80)
      _lfo2Depth *= -1;
  }
}


void TVA::_update_dynamic_level()
{
  _prevDynLevel = _dynLevel;

  // 1: Read expression, Part level and System level
  _dynLevel = _settings->get_param(PatchParam::Expression, _partId) *
    _settings->get_param(PatchParam::PartLevel, _partId) *
    _settings->get_param(SystemParam::Volume);
  _dynLevel = (((4 * _dynLevel) >> 8) & 0xffff);

  // 2: Add corrections, different between normal instruments and drums
  if (_drumSet) {
    _dynLevel *= _settings->get_param(DrumParam::Level, _drumSet - 1, _key);
    _dynLevel = ((_dynLevel * 2) >> 8) & 0xffff;
    _dynLevel = ((_dynLevel * 0x830e * 2) >> 16);
  } else {
    _dynLevel = ((_dynLevel * 0x8208 * 2) >> 16);
  }

  // If volume level is 0 at this point we want to force no sound
  if (_dynLevel == 0)
    return;

  // 3: Add accumulated controller values for Amplitude (they are already
  //    adjusted for direct addition)
  _dynLevel += _settings->get_acc_control_param(Settings::ControllerParam::Amplitude, _partId);
  if (_dynLevel < 0) _dynLevel = 0;

  // 4: Add LFO1 and LFO2, including accumulated controller value for TVA Depth
  int lfoDepth = std::abs(_lfo1Depth +
    _settings->get_acc_control_param(Settings::ControllerParam::LFO1TVADepth,
                                     _partId));
  lfoDepth = std::min(lfoDepth, 0x7f00);

  int mod = _LFO1->value() > 0 ? (_LFO1->value() * lfoDepth + 0x7fff) >> 15 :
                                 (_LFO1->value() * lfoDepth) >> 15;
  _dynLevel = std::max(_dynLevel + mod, 0);

  lfoDepth = std::abs(_lfo2Depth) +
    _settings->get_acc_control_param(Settings::ControllerParam::LFO2TVADepth,
                                     _partId);
  lfoDepth = std::min(lfoDepth, 0x7f00);

  mod = _LFO2->value() > 0 ? (_LFO2->value() * lfoDepth + 0x7fff) >> 15 :
                             (_LFO2->value() * lfoDepth) >> 15;
  _dynLevel = std::max(_dynLevel + mod, 0);

  // 5. Add corrections
  _dynLevel = ((_dynLevel * _dynLevel) >> 16) * 0x208;
  if ((_dynLevel >> 16) >= 0xff)
    _dynLevel = 0xffff;
  else
    _dynLevel = (_dynLevel >> 8) & 0xffff;
}


void TVA::_update_panpot_level(bool reset)
{
  if (_panpotLocked)               // Do not update panpot if in random mode
    return;

  // Nine SC-55mkII instruments carry a non-zero panKeyFlw and take their pan
  // position from the control ROM's key follow curve at the note's key
  // instead of from the partial's own panpot; every other instrument, and
  // every SC-55 one, uses the partial's panpot as before. Both are the same
  // kind of number and enter the sum in the same place, so the part and
  // system pan offsets and the clamp below are unchanged (PROVENANCE.md
  // P-0124).
  int newPanpot = _panpotBase +
    _settings->get_param(PatchParam::PartPanpot, _partId) +
    _settings->get_param(SystemParam::Pan) - 0x80;

  if (_drumSet)
    newPanpot +=
      _settings->get_param(DrumParam::Panpot, _drumSet - 1, _key) - 0x40;

  newPanpot = std::clamp(newPanpot, 0, 0x7f);

  if (newPanpot == _panpot)
    return;

  if (reset == true) {
    _panpot = newPanpot;
  } else {
    if (newPanpot > _panpot && _panpot < 0x7f)
      _panpot ++;
    else if(newPanpot < _panpot && _panpot > 0)
      _panpot --;
  }

  _panpotL = _LUT.TVAPanpot[_panpot];
  _panpotR = _LUT.TVAPanpot[0x80 - _panpot];
}


void TVA::_iterate_phase(void)
{
  _prevEnvLevel = _envLevel;

  if (_phase == Phase::Terminated) {
    std::cerr << "libEmuSC: Internal error, envelope used in Terminated phase"
	      << std::endl;
    return;

  } else if (_phase == Phase::Sustain) {
    _phaseRemainder = 0;
    _envLevelMode = 0xff00;
    _envLevel = _phaseEndValue << 8;
//    if (_envLevel == 0) _init_new_phase(Phase::Terminated); // Verify behavior before enabling this
    return;
  }

  if (_phasePosition >= 0xffff) {
    if (_phase == Phase::Attack1) {
      _init_new_phase(Phase::Attack2);
    } else if (_phase == Phase::Attack2) {
      _init_new_phase(Phase::Decay1);
    } else if (_phase == Phase::Decay1) {
      _init_new_phase(Phase::Decay2);
    } else if (_phase == Phase::Decay2) {
      if (_phaseValueInit[static_cast<int>(Phase::Decay2)] == 0)
        _init_new_phase(Phase::Release);
      else
        _init_new_phase(Phase::Sustain);
    } else if (_phase == Phase::Release) {
      _finished = true;
      return;
    }

  } else if (_phase == Phase::Release && _envLevel == 0) {
    _finished = true;
    return;
  }

  int segmentIndex;
  int prevIntEnvValue = _envLevel;
  int tvaHigh, tvaLow;
  int phaseAccumulator;

  if (_phaseDuration == 0) {
    _envLevel = (_phaseEndValue << 8);
    _envLevelMode = (_phaseEndValue << 8) + 0xaf;
    _phasePosition = 0xffff;
    return;

  } else if (_phaseDuration <= 8) {
    _envLevel = (_phaseEndValue << 8);
    phaseAccumulator = (_phaseEndValue << 8) - prevIntEnvValue;
    tvaHigh = (_phaseEndValue << 8);
    _phasePosition = 0xffff;
    segmentIndex = _LUT.EnvSegmentCurve[_phaseDuration];

  } else {  // _phaseDuration > 8
    _phaseStepSize = (8 << 16) / _phaseDuration;

    segmentIndex = 7;
    int loadScale = 1;    // TODO: Move to central location (Settings?) for
                          //       coordination between notes
    int step = loadScale + _phaseRemainder;
    _phaseRemainder = 0;

    int mul = _phaseStepSize * step;
    int phaseStepDelta = (mul >> 16);
    int phaseAccInc = (mul & 0xffff) + _phasePosition;

    if (phaseAccInc > 0xffff) {
      phaseAccInc &= 0xffff;
      phaseStepDelta += 1;
    }

    if (phaseStepDelta == 0) {
      _phasePosition = phaseAccInc;

    } else {
      int tmp = phaseAccInc - 0xffff;
      if (tmp < 0)
        phaseStepDelta -= 1;

      _phaseRemainder = (phaseStepDelta / _phaseStepSize) & 0xffff;
      _phasePosition = 0xffff;
    }

    if (_phasePosition >= 0xffff) {
      tvaHigh = _phaseEndValue << 8;
      phaseAccumulator = tvaHigh - prevIntEnvValue;
      _envLevel = tvaHigh; // TODO: Rounding error has been observed

    } else {
      int delta = _phaseEndValue - _phaseStartValue;

      // Exponential curve calculation
      if (_phaseShape[static_cast<int>(_phase)] == 1 &&
          _phasePosition != 0xffff) {

        int index = (-_phasePosition >> 8) & 0xff;
        int v0 = _LUT.TVAEnvExpChange[index];
        int v1 = _LUT.TVAEnvExpChange[index + 1];
        uint16_t lutDelta = ((v1 - v0) * (~_phasePosition & 0xFF)) >> 8;

        uint32_t scaled;
        if (delta > 0) {                                  // Positive change
          uint16_t expValue = ~(v0 + lutDelta);
          scaled = delta * expValue;
          tvaHigh = (scaled >> 8) + (_phaseStartValue << 8);

        } else {                                          // Negateive change
          uint16_t expValue = (v0 + lutDelta);
          scaled = (uint16_t) -delta * expValue;
          tvaHigh = (scaled >> 8) + (_phaseEndValue << 8);
        }

        phaseAccumulator = _envLevel = tvaHigh;

      // Linear curve calculation
      } else {
        phaseAccumulator =
          ((_phaseStartValue << 8) + ((delta * _phasePosition) >> 8));

        if (_phasePosition >= 0xffff) {
          phaseAccumulator += 1;
        }

        int phaseIncrement = phaseAccumulator - _envLevel;
        _envLevel += phaseIncrement;
        tvaHigh = _envLevel;
      }
    }
  }

  phaseAccumulator -= prevIntEnvValue;

  if (phaseAccumulator == 0) {             // EnvLevel == PrevEnvLevel
    _envLevelMode = 0xff00;
    return;

  } else if (phaseAccumulator > 0) {
    if ((prevIntEnvValue & 0xff00) == (_envLevel & 0xff00)) {
      if ((tvaHigh & 0xff00) < 0xff00)
        tvaHigh += 0x100;
    }
  } else {
    phaseAccumulator = -phaseAccumulator;
  }

  for (int i = 0; i < 8; i++) {
    uint16_t prevPhaseAccumulator = phaseAccumulator;
    phaseAccumulator <<= 1;
    if (prevPhaseAccumulator & 0x8000)
      break;

    segmentIndex --;
  }

  if (segmentIndex < 0) {
    segmentIndex = 0;
    phaseAccumulator >>= 1;
  }

  tvaLow = (phaseAccumulator >> 8) & 0xff;
  tvaLow = ((tvaLow >> 3) + 1) >> 1;
  tvaLow |= _LUT.EnvSegmentStep[segmentIndex];

  if (tvaLow == 0)
    _envLevelMode = 0xff00;
  else
    _envLevelMode = (tvaHigh & 0xff00) + tvaLow;
}


int TVA::_get_bias_level(int km, int biasPoint)
{
  int kfDiv = _LUT.EnvTimeKeyFollowSens[std::abs(_instPartial.TVABiasLevel - 0x40)];
  int biasLevelIndex = ((std::abs(km - 128) * kfDiv) * 2) >> 8;

  return _LUT.TVABiasLevel[biasLevelIndex];
}


int TVA::_get_velocity_from_vcurve(uint8_t velocity)
{
  unsigned int address = _instPartial.TVALvlVelCur * 128 + velocity;
  if (address > _LUT.VelocityCurves.size()) {
    std::cerr << "libEmuSC internal error: Illegal velocity curve used"
              << std::endl;
    return 0;
  }

  return _LUT.VelocityCurves[address];
}


// How much of the velocity's level attenuation a partial actually gets is set
// by its TVA Level Velocity Sensitivity: the velocity is moved towards the
// maximum by that fraction of the distance, so 0 keeps the full attenuation
// and 127 removes it. Most capital tones are at 0 -- which is why this went
// unnoticed -- but the pianos and basses use 5-30 and the whole MT-32 set
// (variation 127) uses up to 100.
// Measured against the reference: P-0054 (levels of variation bank 127 and of
// the capital tones over key and velocity), P-0055 (the fit of this formula).
int TVA::_get_level_velocity(int cVelocity)
{
  int vSens = std::clamp((int) _instPartial.TVALvlVSens, 0, 127);

  return 127 - (((127 - cVelocity) * (127 - vSens)) / 127);
}


void TVA::_init_envelope(ControlRom &ctrlRom, int sampleIndex,
                         int instrumentIndex, uint8_t cVelocity)
{
  // First step is to calculate correct initial phase levels
  int levelIndex = std::max(0xff - _LUT.TVALevelIndex[_instPartial.volume], 1);
  int kmIndex = _LUT.KeyMapperIndex[0 + _instPartial.TVABiasPoint] -
                _LUT.KeyMapperOffset;
  int km = _LUT.KeyMapper[kmIndex + _key];
  int biasLevel = _get_bias_level(km, _instPartial.TVABiasPoint);
  if (_instPartial.TVABiasLevel >= 0x40) {
    if (km < 0x80)
      levelIndex = std::max(1, levelIndex - biasLevel);
    else
      levelIndex = std::min(levelIndex + biasLevel, 0xff);
  } else {
    if (km < 0x80)
      levelIndex = std::min(levelIndex + biasLevel, 0xff);
    else
      levelIndex = std::max(1, levelIndex - biasLevel);
  }

  levelIndex = std::max(levelIndex -
                        _LUT.TVALevelIndex[_get_level_velocity(cVelocity)], 1);
  levelIndex = std::max(levelIndex - _LUT.TVALevelIndex[ctrlRom.sample(sampleIndex).volume], 1);
  levelIndex = std::max(levelIndex - _LUT.TVALevelIndex[ctrlRom.instrument(instrumentIndex).volume], 1);

  int envL1Index = levelIndex - _LUT.TVALevelIndex[_instPartial.TVAEnvL1];
  int envL2Index = levelIndex - _LUT.TVALevelIndex[_instPartial.TVAEnvL2];
  int envL3Index = levelIndex - _LUT.TVALevelIndex[_instPartial.TVAEnvL3];
  int envL4Index = levelIndex - _LUT.TVALevelIndex[_instPartial.TVAEnvL4];

  _phaseValueInit[0] = 0;
  _phaseValueInit[1] = _LUT.TVALevel[std::max(0, envL1Index)];
  _phaseValueInit[2] = _LUT.TVALevel[std::max(0, envL2Index)];
  _phaseValueInit[3] = _LUT.TVALevel[std::max(0, envL3Index)];
  _phaseValueInit[4] = _LUT.TVALevel[std::max(0, envL4Index)];
  _phaseValueInit[5] = 0;

  _phaseDurationInit[0] = 0;                                     // Never used
  _phaseDurationInit[1] = _instPartial.TVAEnvT1 & 0x7F;
  _phaseDurationInit[2] = _instPartial.TVAEnvT2 & 0x7F;
  _phaseDurationInit[3] = _instPartial.TVAEnvT3 & 0x7F;
  _phaseDurationInit[4] = _instPartial.TVAEnvT4 & 0x7F;
  _phaseDurationInit[5] = _instPartial.TVAEnvT5 & 0x7F;

  _phaseShape[0] = 0;                                            // Never used
  _phaseShape[1] = (_instPartial.TVAEnvT1 & 0x80) ? 0 : 1;
  _phaseShape[2] = (_instPartial.TVAEnvT2 & 0x80) ? 0 : 1;
  _phaseShape[3] = (_instPartial.TVAEnvT3 & 0x80) ? 0 : 1;
  _phaseShape[4] = (_instPartial.TVAEnvT4 & 0x80) ? 0 : 1;
  _phaseShape[5] = (_instPartial.TVAEnvT5 & 0x80) ? 0 : 1;

  // Adjust time for Envelope Time Key Follow including Envelope Time Key Preset
  set_time_key_follow(Envelope::Type::TVA, 0, _key,
                      _instPartial.TVAETKeyF14 - 0x40, _instPartial.TVAETKeyFP14);
  set_time_key_follow(Envelope::Type::TVA, 1, _key,
                      _instPartial.TVAETKeyF5 - 0x40, _instPartial.TVAETKeyFP5);

  // Adjust time for Envelope Time Velocity Sensitivity
  set_time_velocity_sensitivity(Envelope::Type::TVA, 0,
                                _instPartial.TVAETVSens12 - 0x40, cVelocity);
  set_time_velocity_sensitivity(Envelope::Type::TVA, 1,
                                _instPartial.TVAETVSens35 - 0x40, cVelocity);
}


void TVA::_init_new_phase(enum Phase newPhase)
{
  if (newPhase == Phase::Terminated) {
    // TVA dynamic and envelope levels are supposed to be 0 => kill now
    _dynLevel = _envLevel = 0;
    _finished = true;
    return;

  } else if (newPhase == Phase::Sustain) {
    _phaseRemainder = 0;
    _phase = newPhase;

    if (_envLevel == 0)
      _finished = true;

    return;

  } else if (newPhase == Phase::Release) {
    _phaseStartValue = _envLevel >> 8;                 // Use current value
    _phaseEndValue = _phaseValueInit[static_cast<int>(newPhase)];

  } else {
    _phaseStartValue = _phaseValueInit[static_cast<int>(_phase)];
    _phaseEndValue = _phaseValueInit[static_cast<int>(newPhase)];
  }

  _phaseDuration = _phaseDurationInit[static_cast<int>(newPhase)];

  if (newPhase == Phase::Attack1 || newPhase == Phase::Attack2) {
    _phaseDuration +=
      (_settings->get_param(PatchParam::TVFAEnvAttack, _partId) - 0x40) * 2;

  } else if (newPhase == Phase::Decay1 || newPhase == Phase::Decay2) {
    _phaseDuration +=
      (_settings->get_param(PatchParam::TVFAEnvDecay, _partId) - 0x40) * 2;

  } else if (newPhase == Phase::Release) {
    _phaseDuration +=
      (_settings->get_param(PatchParam::TVFAEnvRelease, _partId) - 0x40) * 2;
  }

  _phaseDuration = _LUT.envelopeTime[std::clamp(_phaseDuration, 0, 127)];
  _phasePosition = 0;
  _phaseRemainder = 0;

  // Correct phase duration for Time Key Follow
  if (newPhase != Phase::Release)
    _phaseDuration = (_phaseDuration * _timeKeyFlwT1T4) >> 8;
  else
    _phaseDuration = (_phaseDuration * _timeKeyFlwT5) >> 8;

  // Correct phase duration for Time Velocity Sensitivity
  if (newPhase == Phase::Attack1 || newPhase == Phase::Attack2)
    _phaseDuration = (_phaseDuration * _timeVelSensT1T2) >> 8;
  else
    _phaseDuration = (_phaseDuration * _timeVelSensT3T5) >> 8;

  if (0) {
    std::cout << " => DURATION=0x" << std::hex << _phaseDuration << std::endl;
    std::cout << "New TVA envelope phase: -> "
              << std::dec << static_cast<int>(newPhase)
	      << " (" << _phaseName[static_cast<int>(newPhase)] << "): Level = "
              << _phaseStartValue << " -> " << _phaseEndValue
	      << " | Time = 0x" << std::hex << _phaseDuration << " => "
	      << std::dec << (_phaseDuration * 8) / 32000.0 << "s" << std::endl;
  }

  _phase = newPhase;
}


void TVA::_slew_function_dynvol(uint16_t mode)
{
  if ((mode & 0xff) == 0x00) {
    SlewCalc::set_level_direct(_dynLevelEC, (uint16_t) (_dynLevel >> 1));
    float g = _dynLevelEC / 16384.0f;
    _slewDynGain.fill(g);
    return;
  }

  for (int i = 0; i < 256; i++)
    _slewDynGain[i] = _tvDyn.tick(mode, _dynLevelEC, 0) / 16384.0f;

  _dynLevel = _dynLevelEC << 1;
}


void TVA::_slew_function_envelope(uint16_t mode)
{
  if ((mode & 0xff) == 0x00) {
    SlewCalc::set_level_direct(_envLevelEC, (uint16_t) (_envLevel >> 1));
    float g = _envLevelEC / 16384.0f;
    _slewEnvGain.fill(g);
    return;
  }

  for (int i = 0; i < 256; i++)
    _slewEnvGain[i] = _tvEnv.tick(mode, _envLevelEC, 1) / 16384.0f;

  _envLevel = _envLevelEC << 1;
}


// Simplified linear smoothing used inside control block
// The SC-55 is observed to do integer math and report back deviation from
// intended target value to avoid drift. We use float and clamp to target
// (for now).
void TVA::_smooth(int mode, float start, float target,
                  std::array<float, 256> &gain)
{
  // The mode's low byte is the slew speed, read here as the slew functions
  // above read it. Two speeds reach the target within the block's first
  // sample and hold it for the rest of the block:
  //   0x00 => no slew (the envelope writes this mode as 0xff00)
  //   0xba => instant jump (slew_calc.h), which _init_update() sets for a
  //           note's first control block
  // Interpolating either of them from the previous level spreads a note's
  // initial level over its first 256 samples, so the first 8 ms of every note
  // comes out too quiet and percussion shorter than one control block comes
  // out ~14 dB down. Measured: P-0053 (the interpolation) and P-0062 (the
  // reference's first-block level), verified in P-0063.
  int speed = mode & 0xff;
  if (speed == 0x00 || speed == 0xba) {
    for (int i = 0; i < 256; i++)
      gain[i] = target;
    return;
  }

  // All other mode values are ignored => linear ramp
  float step = (target - start) / 256.0f;
  float level = start;
  for (int i = 0; i < 256; i++) {
    level += step;
    gain[i] = level;
  }

  // Clamp to target at last value to avoid any drift
  gain[255] = target;
}


}  // namespace EmuSC
