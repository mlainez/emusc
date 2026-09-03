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


// A tone whose pan is RANDOM draws its position once, at note start, exactly as
// a part or drum pan does. 189 of the JV's 768 factory tones ask for it - patch
// 21 `SAW Lead` tone 3 among them, which is three of the demo's twelve voices on
// channel 1 - so it is not a corner case.
//
// Until this existed the raw sentinel reached _update_panpot_level() and was
// clamped by it: 0xff clamped to 0x7f, putting every RANDOM tone HARD RIGHT.
// Before the sentinel was introduced the same tones were nudged to hard LEFT.
// Both are wrong in the same way - a fixed position where the device has none.
//
// std::rand() is deliberate rather than lazy: the engine never seeds it, so the
// sequence is the C library's default and renders stay reproducible, which is
// what keeps the corpus byte-identity check meaningful.
static int jv_tone_pan(uint8_t stored)
{
  return stored == ControlRom::PANPOT_RANDOM ? (std::rand() % 128) : (int) stored;
}


TVA::TVA(ControlRom &ctrlRom, uint8_t key, uint8_t velocity, int sampleIndex,
         WaveGenerator *LFO1, WaveGenerator *LFO2, Settings *settings,
         int8_t partId, uint16_t instrumentIndex, int partialId)
  : Envelope(ctrlRom.lookupTables),
    _initRunComplete(false),
    _firstBlock(true),
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
                jv_tone_pan(ctrlRom.instrument(instrumentIndex)
                              .partials[partialId].panpot)),
    _panpot(-1),
    _panpotLocked(false),
    _settings(settings),
    _partId(partId),
    _dynLevelEC(0), _envLevelEC(0),
    _slewDynGain{}, _slewEnvGain{}
{
  // The velocity the LEVEL chain sees is rescaled by the partial's velocity
  // range low bound: (v - low) * 127 / (127 - low), so the bound maps to 0
  // and 127 stays 127. Measured on the SC-55mkII: E.Piano 2v's second
  // partial fades in above velocity 75 on exactly this rescaled velocity,
  // while Funk Gt.2's second partial enters BRIGHT at velocity 116, so the
  // TVF and the envelope-time paths keep the raw velocity (PROVENANCE.md,
  // anomalies lane pending B).
  int lvlVelocity = velocity;
  if (_instPartial.velRangeLow > 0 && _instPartial.velRangeLow < 127) {
    lvlVelocity = ((velocity - _instPartial.velRangeLow) * 127) /
                  (127 - _instPartial.velRangeLow);
    lvlVelocity = std::clamp(lvlVelocity, 0, 127);
  }
  // The velocity sensitivity the second partial receives is the SUM of the
  // two partials' sensitivity bytes. Measured on the SC-55mkII (PROVENANCE.md,
  // anomalies lane pending C): decomposing two-partial MT-32 tones per
  // partial, partial 1's velocity attenuation follows s0+s1 (Fantasy, s 40/50:
  // within 0.15 dB of the s=90 curve at every velocity 32..127), while
  // partial 0 follows its own byte alone. This is also P-0055's residual: the
  // fit it declined to apply is the law.
  _lvlVSensEff = _instPartial.TVALvlVSens;
  if (partialId == 1 &&
      (ctrlRom.instrument(instrumentIndex).partialsUsed & 0x01)) {
    int sum = (int) _instPartial.TVALvlVSens +
              (int) ctrlRom.instrument(instrumentIndex).partials[0].TVALvlVSens;
    _lvlVSensEff = (uint8_t) std::min(sum, 127);
  }

  int cVelocity = _get_velocity_from_vcurve(velocity);
  int cVelocityLvl = _get_velocity_from_vcurve((uint8_t) lvlVelocity);
  _init_envelope(ctrlRom, sampleIndex, instrumentIndex,
                 (uint8_t) cVelocityLvl, cVelocity);

  // Calculate random pan if part pan or drum pan value is 0 (RND)
  // A note is locked to RND if it is started with that setting
  const int drumPan = _drumSet
    ? (int) settings->get_param(DrumParam::Panpot, _drumSet - 1, _key) : -1;
  const bool drumRandom =
    drumPan == (int) ControlRom::PANPOT_RANDOM ||
    (drumPan == 0 && !_LUT.hasJVPanLaw);      // 0 is random only where 0 cannot
                                              // be a real position: not the JV
  if (settings->get_param(PatchParam::PartPanpot, _partId) == 0 || drumRandom) {
    _panpot = std::rand() % 128;
    _set_panpot_gains();
    _panpotLocked = true;
  }
}


// TVA consists of two values: dynamic volume corrections and envelope level.
// Each of these levels have their own "mode" variable controlling how they are
// interpolated / smoothed across and inside control loops of 256 samples.
void TVA::apply_sample_set(std::array<std::array<float, 256>, 2> &dryBus,
			   std::array<float, 256> &sendBuf)
{
  auto norm = [](float v) { return v / 32768.0f; };

  // The envelope register's word, which is not always the envelope's own value.
  // On the JV the running envelope value is converted through the register's
  // OWN curve at every emit (ROM1 0x4578-0x459a), so the walk above is in the
  // pre-curve domain and the conversion belongs here, at the point the value
  // becomes a gain. On the Sound Canvas the walk already IS the gain.
  const int envNow  = _env_register_value(_envLevel);
  const int envPrev = _env_register_value(_prevEnvLevel);

  _smooth(_envLevelMode, norm(envPrev), norm(envNow), _slewEnvGain,
          _firstBlock);
  _firstBlock = false;

  float panL = _panpotL / 127.0f;
  float panR = _panpotR / 127.0f;

  // The amplified sample before the panner is the signal the effect sends are
  // taken from, so it is handed back separately. Measured on the SC-55mkII
  // (PROVENANCE.md P-0182): sweeping a part's pan across CC10 = 0..127 with a
  // fixed reverb send leaves the reverb's level unchanged to 0.00 dB, and the
  // chorus likewise, while the dry signal pans normally. Taking the send from
  // the panned signal instead makes it vary with pan position by 1.5 dB and
  // lose 4.6 dB at centre, since the pan table's centre gain is 75/127.
  // The dry attenuator sits on the direct output register only. The sends are
  // taken from the amplified sample before it, which is why sendBuf is written
  // from `sample` and not from the attenuated dry pair (P-0382 findings 2/8).
  const float dry = _dryGain;

  for (int i = 0; i < 256; i++) {
    float sample = dryBus[0][i] * _slewDynGain[i] * _slewEnvGain[i];
    sendBuf[i] = sample;
    dryBus[0][i] = sample * dry * panR;
    dryBus[1][i] = sample * dry * panL;
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

  // The JV's level is TWO chip registers, not one. ROM1 0x38a4-0x38b8 writes
  // the level law's own byte to F016 and the TVA envelope's byte to F018, each
  // as (byte << 8) | slew-mode - which is exactly the pair this engine already
  // models, a dynamic register and an envelope register each contributing
  // value/32768. So the static byte belongs HERE and the envelope's in
  // _phaseValueInit. It had been multiplied into the envelope target instead,
  // collapsing the two into one and losing the second register's gain: at
  // envelope level 127 the F018 byte is 255, worth 255/128 = 1.9922, and that
  // is +5.987 dB. Predicted before rendering and then measured on a single
  // melodic voice on channel 4 and on four drums, where the deficit was a
  // level-independent -5.2 to -6.0 dB, constant over velocity 20-127
  // (PROVENANCE.md P-0398).
  //
  // Nothing below this point runs on that device: expression, system volume,
  // the drum-set level and the two 0x8208/0x830e/0x208 corrections are the
  // Sound Canvas's own arithmetic for its dynamic register, and the JV's
  // firmware does not compute any of it - CC7 reaches the level law as Part
  // Level instead. CC11 and master volume are therefore a NAMED GAP on this
  // device rather than an approximation.
  if (_settings->device()->levelLawKind == LevelLawKind::JVCurveProduct) {
    _dynLevel = _staticLevel8 << 8;
    return;
  }

  // 1: Read expression, Part level and System level.
  //
  // A device whose own level law already folds the part level in says so in its
  // profile, and it is held at full scale here. Applying it in both places
  // attenuates twice: measured against the reference on a part-level sweep, the
  // JV's curve ran -36.5 dB at CC7 47 where the machine runs -18.5 and the law
  // predicts -19.6 (P-0386).
  const int partLevel = _partLevelInDynamics
    ? _settings->get_param(PatchParam::PartLevel, _partId) : 0x7f;

  _dynLevel = _settings->get_param(PatchParam::Expression, _partId) *
    partLevel *
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
  //
  // The depth is SIGNED: the top bit of the control ROM's depth byte inverts
  // the modulation, and _update_lfo_depth() above negates the depth for it.
  // Taking the absolute value here threw that sign away three lines after it
  // was computed, so an inverted tone was modulated in phase instead of anti-
  // phase. Measured on Flute, whose byte 72 is 0x87 (depth 7, inverted) and
  // which also carries a pitch LFO depth, so the phase between its amplitude
  // and pitch modulation is a property of the tone: the reference puts them
  // 174.7 degrees apart on the mkII and 175.4 on the SC-55, this engine put
  // them 10.8 degrees apart on both. Tones without the flag agreed to within
  // 17 degrees, so it was the flag and not a general phase error
  // (PROVENANCE.md P-0152). The flag is set on 29 of 469 mkII
  // instrument-partials and 28 of 417 on the SC-55.
  //
  // The clamp becomes symmetric for the same reason. How a *controller* TVA
  // depth combines with an inverted ROM depth is not measured - the tones
  // above were rendered with no modulation wheel - so the two terms are still
  // summed before clamping, as they were, and LFO2 now sums in the same order
  // as LFO1 rather than taking the absolute value of only its own term.
  int lfoDepth = _lfo1Depth +
    _settings->get_acc_control_param(Settings::ControllerParam::LFO1TVADepth,
                                     _partId);
  lfoDepth = std::clamp(lfoDepth, -0x7f00, 0x7f00);

  int mod = _LFO1->value() > 0 ? (_LFO1->value() * lfoDepth + 0x7fff) >> 15 :
                                 (_LFO1->value() * lfoDepth) >> 15;
  _dynLevel = std::max(_dynLevel + mod, 0);

  lfoDepth = _lfo2Depth +
    _settings->get_acc_control_param(Settings::ControllerParam::LFO2TVADepth,
                                     _partId);
  lfoDepth = std::clamp(lfoDepth, -0x7f00, 0x7f00);

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

  _set_panpot_gains();
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

  } else if (_phaseDuration <= _instantTicks) {
    _envLevel = (_phaseEndValue << 8);
    // phaseAccumulator is the level here, not yet the change: the common
    // "phaseAccumulator -= prevIntEnvValue" below turns it into one, as it
    // does for the two branches further down.  Subtracting the previous level
    // here as well doubled the magnitude that picks the slew speed, so a
    // segment that ends inside one control period was given a speed for twice
    // the distance.  Measured: PROVENANCE.md P-0156.
    phaseAccumulator = (_phaseEndValue << 8);
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
      phaseAccumulator = tvaHigh;             // see the note above
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


// The value the TVA's envelope CHIP REGISTER receives, given the envelope's own
// running value.
//
// On the Sound Canvas these are the same number: the envelope walks in the gain
// domain and its value is written as it stands. On the JV they are not, and the
// difference is not small. ROM1 0x4578-0x459a takes the RUNNING envelope value,
// splits it as h = value >> 8 and frac = value & 0xff, and reads a (curve,
// slope) pair at ROM2 0x6060/0x6160:
//
//     out = CURVE[h] + ((SLOPE[h] * frac) >> 16)
//
// storing the HIGH BYTE of that as the F018 write (ROM1 0x3da6). So the
// envelope's segments are straight lines in the PRE-curve domain and the gain
// they produce is that line pushed through an exponential.
//
// This port used to convert the segment ENDPOINTS once, at note-on, and then
// interpolate between the converted values - a straight line in gain. The
// endpoints were therefore exact and everything between them was wrong, by up
// to 12.86 dB: for a decay from level 127 to 0, at the midpoint in TIME the
// firmware's gain is CURVE[64] >> 8 = 29 of 255 and this port's was 128 of 255.
// Measured on the reference at 12.39 dB on the one tone of SAW Lead that decays
// (P-0400). It read as a LAYERING error because a patch's first tone sustains
// and its later tones decay, and because single-note validation measured a 30 ms
// window at the attack, where the endpoints are exact by construction.
//
// The slope term is carried even though it is a sub-LSB trim rather than an
// interpolation - the shift is 16 where a linear interpolation would need 8, so
// it contributes at most 6 of 65535 - because the byte taken is the high byte
// and a trim of 6 can tip it at a boundary.
int TVA::_env_register_value(int envValue) const
{
  if (!_envThroughCurve)
    return envValue;

  const int h    = std::clamp(envValue >> 8, 0, 127);
  const int frac = envValue & 0xff;
  const int out  = _LUT.JVLevelEnv[h] +
                   ((_LUT.JVLevelEnvSlope[h] * frac) >> 16);

  return std::clamp(out >> 8, 0, 255) << 8;
}


int TVA::_get_bias_level(int km, int biasPoint)
{
  int kfDiv = _LUT.EnvTimeKeyFollowSens[std::abs(_instPartial.TVABiasLevel - 0x40)];
  int biasLevelIndex = ((std::abs(km - 128) * kfDiv) * 2) >> 8;

  return _LUT.TVABiasLevel[biasLevelIndex];
}


int TVA::_get_velocity_from_vcurve(uint8_t velocity)
{
  // How many curves the bank actually holds decides whether the tone's curve
  // selector means anything here. A device that carries a single identity
  // curve is telling us velocity is not pre-shaped -- it applies its own curve
  // further down, inside the level law -- so the selector must not index off
  // the end of a table that has nowhere to go. Reading the count from the
  // table keeps this decision in the device's data, not in the engine.
  const size_t curveCount = _LUT.VelocityCurves.size() / 128;
  const unsigned int curve = (curveCount > 1) ? _instPartial.TVALvlVelCur : 0;

  unsigned int address = curve * 128 + velocity;
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
  int vSens = std::clamp((int) _lvlVSensEff, 0, 127);

  return 127 - (((127 - cVelocity) * (127 - vSens)) / 127);
}


void TVA::_init_envelope(ControlRom &ctrlRom, int sampleIndex,
                         int instrumentIndex, uint8_t cVelocityLvl,
                         uint8_t cVelocity)
{
  if (ctrlRom.profile()) {
    _instantTicks = ctrlRom.profile()->level.envelopeInstantTicks;
    _partLevelInDynamics = ctrlRom.profile()->level.partLevelInDynamics != 0;
  }

  // Dry Level, 7 bits widened to 8 the way the firmware widens a gain byte.
  // 0x7f gives exactly 1.0f, so a device without a dry attenuator is bit-exact.
  {
    const int d = _instPartial.dryLevel & 0x7f;
    _dryGain = (2 * d + (d >= 64 ? 1 : 0)) / 255.0f;
  }

  // The JV family computes level multiplicatively in the linear domain and
  // converts once through its own curve, where the Sound Canvas accumulates
  // attenuations in a log index domain and subtracts them (P-0381):
  //
  //   final = T[(toneLevel x sampleLevel8 x velScale) >> 8]
  //         x T[(partLevel x patchLevel) >> 7]
  //
  // and the envelope is a separate multiplier, because on the hardware the static
  // level and the envelope are two chip registers the tone generator multiplies.
  // Sharing the Sound Canvas chain here is why not one JV instrument was inside
  // 0.5 dB of the machine on level: it subtracts a velocity curve as though it
  // were a level law.
  if (_settings->device()->levelLawKind == LevelLawKind::JVCurveProduct) {
    const auto &T  = _LUT.JVLevel;
    const LevelLaw &L = _settings->device()->level;

    // Sample level is byte 0 of the sample record, 7 bits widened to 8 the way
    // the firmware widens it.
    const int sv    = ctrlRom.sample(sampleIndex).volume & 0x7f;
    const int smpl8 = 2 * sv + (sv >= 64 ? 1 : 0);

    int index = (_instPartial.volume & 0x7f) * smpl8;

    // Velocity, through the tone's own curve out of the bank, applied
    // multiplicatively. A sensitivity of 0 means no velocity effect at all.
    const int sens = (int8_t) _instPartial.TVALvlVSens;
    if (sens != 0) {
      const int banked = (int) (_LUT.JVVelCurves.size() / 128);
      const int curve = std::min((int) _instPartial.TVALvlVelCur,
                                 banked - 1) * 128;
      const int v     = cVelocity & 0x7f;
      int w = 0;
      if (sens > 0) {
        const int idx = L.velocityPivot - ((sens * (L.velocityPivot - v)) >> L.velocityShift);
        w = (idx < 0) ? 0xffff : (_LUT.JVVelCurves[curve + idx] << 8);
      } else {
        const int idx = (-sens * v) >> L.velocityShift;
        w = (idx > 127) ? 0 : ((255 - _LUT.JVVelCurves[curve + idx]) << 8);
      }
      index -= (int) (((int64_t) index * w) >> 16);
    }

    const int gain = T[std::clamp(index >> L.toneIndexShift, 0, 127)];

    // The level index, formed the way the firmware forms @0x9a1e and then reads
    // it back (scdb D-28, FW-EXACT throughout).
    //
    // A PATCH part multiplies the Performance part level by the patch's own
    // level byte and shifts down 7 (ROM1 0x4641-0x4648). The RHYTHM part - part
    // 8 of a Performance, manual p.2-14 - does NOT: it stores the part level
    // raw (ROM1 0x4c2a), because a Rhythm Set has no patch level and the per-key
    // level is applied separately. Worth 0.23 dB at part level 127, since the
    // patch-part product would give 126 where the device keeps 127.
    const int part  = _settings->get_param(PatchParam::PartLevel, _partId) & 0x7f;
    const int patch = ctrlRom.instrument(instrumentIndex).volume & 0x7f;
    int composed = _drumSet ? part : ((part * patch) >> L.dynamicsShift);

    // Then CC7 Volume, on a device that keeps it apart from the part level.
    // ROM1 0x44c8-0x44d4: the byte is widened 0..127 -> 0..255 by the firmware's
    // usual gain-byte expansion and multiplied by the raw controller value,
    // shifted down by volumeIndexShift. The multiply is what the port was
    // missing, and letting CC7 overwrite the part level instead is what made
    // the demo's channel 1 +2.7 dB hot.
    //
    // Note this is a /127.5 scale, not /127: at CC7 = 127 the index still lands
    // one or two steps under `composed`, so full volume is not unity here. The
    // measurements say the same, and the ROM is why.
    if (L.volumeIndexShift) {
      const int vol = _settings->get_param(PatchParam::PartVolume, _partId) & 0x7f;
      composed = ((2 * composed + (composed >= 64 ? 1 : 0)) * vol)
                 >> L.volumeIndexShift;
    }

    const int dyn = T[std::clamp(composed, 0, 127)];

    // The static level byte, exactly as the firmware stores it: the high byte
    // of high16(T[a] * T[b]), written to @0x8dc2 at ROM1 0x3d4c and from there
    // to the chip's F016 register at ROM1 0x38a4. It is the DYNAMIC register on
    // this engine, so _update_dynamic_level() reads it; it must not be
    // multiplied into the envelope target as well.
    _staticLevel8 =
      std::clamp((int) (((int64_t) gain * dyn) >> L.staticShift), 0, 255);

    // The envelope walks in the device's PRE-CURVE domain, so the segment
    // levels go in RAW. The envelope register's byte is not a linear fraction
    // of the static level: ROM1 0x4578-0x459a converts the running envelope
    // value through a SECOND (curve, slope) pair at ROM2 0x6060/0x6160 and
    // stores the high byte of the result (ROM1 0x3da6), so level 127 gives 255
    // and level 64 gives 29 - 29/128 rather than 64/127, which is 7 dB apart.
    // The two chip registers then multiply, and 255/128 is where the missing
    // 5.99 dB was (P-0398).
    //
    // The conversion is done at every emit, in _env_register_value(), because
    // that is where the firmware does it. Converting the ENDPOINTS here and
    // interpolating between them - which is what this branch used to do, and
    // what D-21 recorded as a named gap "separable from the level" - leaves the
    // endpoints exact and everything between them out by up to 12.86 dB
    // (P-0400).
    _envThroughCurve = true;
    _phaseValueInit[0] = 0;
    _phaseValueInit[1] = _instPartial.TVAEnvL1 & 0x7f;
    _phaseValueInit[2] = _instPartial.TVAEnvL2 & 0x7f;
    _phaseValueInit[3] = _instPartial.TVAEnvL3 & 0x7f;
    _phaseValueInit[4] = _instPartial.TVAEnvL4 & 0x7f;
    _phaseValueInit[5] = 0;

    _phaseDurationInit[0] = 0;
    _phaseDurationInit[1] = _instPartial.TVAEnvT1 & 0x7F;
    _phaseDurationInit[2] = _instPartial.TVAEnvT2 & 0x7F;
    _phaseDurationInit[3] = _instPartial.TVAEnvT3 & 0x7F;
    _phaseDurationInit[4] = _instPartial.TVAEnvT4 & 0x7F;
    _phaseDurationInit[5] = _instPartial.TVAEnvT5 & 0x7F;

    // LINEAR, every segment. This branch used to return without writing
    // _phaseShape at all, so _iterate_phase() selected each segment's shape
    // from UNINITIALISED memory: whether a segment ramped linearly or
    // exponentially depended on what the heap happened to hold, which made the
    // render depend on the allocator. It is measurable - adding three bytes of
    // padding to InstPartial, with no behavioural change whatever, alters the
    // JV render from the first note off onwards - and it is the reason a JV
    // byte-identity check could not be relied on.
    //
    // Zero, not a guess: the JV's envelope stepper subtracts a fixed decrement
    // from a 16-bit counter and interpolates level = target + (remaining *
    // (start - target) >> 16). remaining falls linearly in time, so the level
    // is a linear ramp between the two segment levels. There is no
    // remaining-distance-times-rate term anywhere in that path, which is what
    // an exponential approach would need (P-0391).
    for (int i = 0; i < 6; i++)
      _phaseShape[i] = 0;
    return;
  }

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
                        _LUT.TVALevelIndex[_get_level_velocity(cVelocityLvl)], 1);
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
  // On a rhythm part the envelope time key follow sees the drum set's play
  // key (DrumParam::PlayKeyNumber), not the note number, exactly as the
  // pitch chain already does. Measured on the SC-55mkII STANDARD set, where
  // Crash 1 (note 49), Splash (55) and Crash 2 (57) are one instrument
  // record (time key follow byte 59) at play keys 60, 69 and 61: each note's
  // single-strike decay slope matched the reference only under the play key
  // (candidate/reference slope ratios 0.898/0.889/0.957 before, 1.001/0.979/
  // 1.006 after), the toms' latent error of up to 17 dB on mk1 closed with
  // it, and the two cymbals with key follow 0x40 (Ride 1, China) were exact
  // either way (PROVENANCE.md, tone-records lane).
  int tkfKey = _key;
  if (_drumSet)
    tkfKey = _settings->get_param(DrumParam::PlayKeyNumber, _drumSet - 1, _key);
  set_time_key_follow(Envelope::Type::TVA, 0, tkfKey,
                      _instPartial.TVAETKeyF14 - 0x40, _instPartial.TVAETKeyFP14);
  set_time_key_follow(Envelope::Type::TVA, 1, tkfKey,
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


// The envelope level moves inside the control block at the rate the mode's
// speed byte encodes, and holds once it is there.  The hardware does this in
// integer arithmetic with a dither that gives the accumulator a fractional
// average slope; the dither is smaller than one level unit (1/32768 of full
// gain), so this uses float and lands exactly on the target instead.
void TVA::_smooth(int mode, float start, float target,
                  std::array<float, 256> &gain, bool firstBlock)
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
  if (speed == 0x00 || speed == 0xba || start == target) {
    for (int i = 0; i < 256; i++)
      gain[i] = target;
    return;
  }

  // A note's first control block is the exception: it covers the whole period
  // whatever rate the speed byte carries.  Measured twice - P-0062 found the
  // 256-sample ramp the best of eight models on all sixteen of its first
  // blocks that were in a ramp mode, and PROVENANCE.md P-0155 measures a
  // first block whose encoded rate would cover the period twice over and
  // finds the reference still taking the whole period.  Why the first block
  // differs is not explained here.
  float step;
  if (firstBlock) {
    step = std::fabs(target - start) / 256.0f;
  } else {
    // Every other speed the envelope writes is one of slew_calc.h's linear
    // modes, and its two nibbles give the per-sample increment: an unsigned
    // mantissa (with an implied leading bit unless the high nibble is zero)
    // shifted by the high nibble.  This is the same arithmetic
    // _slew_function_envelope() runs through SlewCalc::tick(), lifted out
    // here so the envelope's own level stays the target.
    //
    // Measured on the SC-55mkII (PROVENANCE.md P-0154): a note held on Sine
    // Wave (bank 8, PC 81), whose TVA envelope is flat at maximum for the
    // whole hold, released with the release-time part parameter set so the
    // fall takes one control period.  The fall is linear in amplitude, and
    // its length is set by the speed byte, not by the control period: 90 % to
    // 10 % of the sustain level in 0.77 ms for speed 0xaf and 3.05 ms for
    // speed 0x90, where a ramp across the whole period takes 6.40 ms.  Where
    // the ramp reaches the target early the level then holds for the rest of
    // the period, which the same measurement shows directly as a plateau.
    int hi = (speed >> 4) & 0x0f;
    bool w1 = (hi == 0);
    bool w2 = w1 || (speed & 0x10);
    bool w3 = !(speed & 0x80) ||
              (!(speed & 0x40) && (!w2 || !(speed & 0x20)));

    if (w3) {                                // linear mode
      int shift = (10 - ((hi & 14) | (w2 ? 1 : 0))) & 15;
      int inc = (((speed & 15) << 9) | (w1 ? 0 : 0x2000)) >> shift;
      step = inc / (16.0f * 16384.0f);       // level units -> gain units
    } else {
      // An exponential mode other than the instant jump above. No tone in
      // either control ROM was observed to reach one, so there is nothing
      // measured to reproduce; cover the block as before.
      step = 0.0f;
    }

    // The ramp never outlasts the control period: where the encoded rate is
    // slower than that, the reference still completes the move inside the
    // period (measured on the same tone with the attack-time part parameter,
    // speed 0x70: 8.0 ms, against the 16 ms its encoded rate alone gives).
    step = std::max(step, std::fabs(target - start) / 256.0f);
  }

  float level = start;
  if (target > start) {
    for (int i = 0; i < 256; i++) {
      level = std::min(level + step, target);
      gain[i] = level;
    }
  } else {
    for (int i = 0; i < 256; i++) {
      level = std::max(level - step, target);
      gain[i] = level;
    }
  }

  // Land exactly on the target, so the block boundary carries no drift
  gain[255] = target;
}



// The pan law. A device whose profile names a packed pan table has independent
// L and R channels, because its centre is asymmetric and no mirrored table can
// express it; every other device keeps the Sound Canvas's single 129-byte ramp
// read as T[p] against T[0x80 - p], unchanged.
void TVA::_set_panpot_gains(void)
{
  if (_LUT.hasJVPanLaw) {
    int p = _panpot < 0 ? 0 : (_panpot > 127 ? 127 : _panpot);
    // NOTE THE SWAP, and that it is not a bug here. `_panpotL` does not carry
    // the left channel: the Sound Canvas path below assigns it TVAPanpot[_panpot]
    // from a table that RISES 0..127, so pan 0 gives _panpotL = 0 and the sound
    // arrives on the right. The field names are inverted with respect to the
    // output, which the symmetric sin table it used to be filled with could never
    // reveal. The JV's table is asymmetric and its hard-left entry is explicit
    // (0x7f00 at index 0), so it exposes the misnomer immediately: assigned by
    // name, Closed HAT 1 came out +30 dB right where the reference puts it 27 dB
    // LEFT. Reading the high byte into _panpotR restores the reference's side.
    _panpotR = _LUT.JVPanLawL[p];
    _panpotL = _LUT.JVPanLawR[p];
  } else {
    _panpotL = _LUT.TVAPanpot[_panpot];
    _panpotR = _LUT.TVAPanpot[0x80 - _panpot];
  }
}

}  // namespace EmuSC
