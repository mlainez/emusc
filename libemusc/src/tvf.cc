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
// The Sound Canvas is using a 2nd. order low or high pass filter for TVF.
// The "TVF Type" variable in partial definitions specifies the filter type or
// whether the TVF filter is disabled.

// If TVF filter is enabled, the "TVF Cutoff Frequency" in partial definition
// sets the base cutoff frequency, and envelope values are modifiers to this
// base frequency. The significance of the TVF Envelope on the cutoff frequency
// is controlled by the TVF Envelope Depth parameter.

// Cutoff Freq Key Follow scales filter freq with LUT index = "ROM / 10" keys.
// The Cutoff Freq Key Follow Direction has 4 modes:
//   0 => Adjust all keys with center on key 64
//   1 => Only adjust keys > C4
//   2 => Only adjust keys > C7
//   3 => Only adjust keys < C7

// To calculate the filter resonance there are two calculations needed:
//  * Read resonance value from partial def. in ROM and add 2x SysEx value
//  * Calculate the correct index and read the value from LUT.TVFResonanceFreq
// Whichever of the two values are lowest will be used for calculating the
// resonance from the LUT.Resonance lookup table.


#include "tvf.h"
#include "jv_velocity.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string.h>


namespace EmuSC {


TVF::TVF(ControlRom::InstPartial &instPartial, uint8_t key, uint8_t velocity,
         WaveGenerator *LFO1, WaveGenerator *LFO2,ControlRom::LookupTables &LUT,
         Settings *settings, int8_t partId)
  : Envelope(LUT),
    _sampleRate(settings->sample_rate()),
    _LFO1(LFO1),
    _LFO2(LFO2),
    _lfo1FadeComplete(false),
    _lfo2FadeComplete(false),
    _lfo1Depth(0),                 // _init_envelope() below scales _lfo1Depth
    _lfo2Depth(0),                 // by the LFO fade and _iterate_phase()
                                   // reads both, all before the first
                                   // _update_lfo_depth() call
    _LUT(LUT),
    _instPartial(instPartial),
    _resonance(0x40),
    _envLevel(0),
    _envLevelMode(0xff00),
    _prevEnvLevel(0),
    _coFreq{},
    _key(key),
    _settings(settings),
    _partId(partId),
    _jv(false),
    _jvLaw(nullptr),
    _jvTickCount(0),
    _jvDecrement(0),
    _jvEnvLevel(0),
    _jvEnvDepth(0),
    _jvVelAtten(0),
    _jvKeyFollow(0),
    _jvLfo1Depth(0),
    _jvLfo2Depth(0),
    _jvCutoff(0),
    _jvResTarget(0),
    _jvRes(0),
    _jvWord(0),
    _jvWordPrev(0),
    _jvQ1(1.0f),
    _jvRampPos(0)
{
  if (instPartial.TVFType == 0)
    _svf = new SVF(SVF::Mode::LowPass);
  else if (instPartial.TVFType == 1)
    _svf = new SVF(SVF::Mode::HighPass);
  else
    _svf = nullptr;

  if (_svf == nullptr)                       // TVF disabled
    return;

  if (_settings->device()->tvfLawKind == TvfLawKind::JVCentsRatio) {
    _jv = true;
    _jvLaw = &_settings->device()->tvfJv;
    _jv_init(velocity);
    return;
  }

  _velocity = _get_velocity_from_vcurve(velocity);

  // TODO: RENAME TO _cofKeyFollow? Any relation to timeKeyFollow?
  _keyFollow = _get_cof_key_follow(_instPartial.TVFCFKeyFlw - 0x40);
  _coFreqVSens = _read_cutoff_freq_vel_sens(_instPartial.TVFCOFVSens - 0x40);
  _envDepth = _LUT.TVFEnvDepth[_instPartial.TVFEnvDepth];

  _init_freq_and_res();
  _init_envelope();

  update();
}


TVF::~TVF()
{
  delete _svf;
}


// TODO: Add suport for Cutoff freq V-sens
// TVF consists of two values: Filter cutoff frequency and resonance.
// The cutoff frequency is controlled by the envelope generator and have a
// "mode" variable controlling how the frequency values are interpolated /
// smoothed across and inside control loops of 256 samples. Resonance is a more
// static variable controlled by instrument definition and SySEx messsages.
void TVF::apply_sample_set(std::array<float, 256> &dryBus)
{
  // Skip filter calculation if filter is disabled for this partial 
  if (_svf == nullptr)
    return;

  if (_jv) {
    _jv_apply_sample_set(dryBus);
    return;
  }

  _smooth_cutoff();

  for (int i = 0; i < 256; i++) {
    _svf->set_cutoff_freq(_coFreq[i]);
    dryBus[i] = _svf->process_sample(dryBus[i]);
  }
}


// The cutoff frequency moves inside the control period, it does not step at
// the period boundary.  Measured (PROVENANCE.md P-0134): a cutoff step of 50
// semitones, driven from the part parameter half-way through a held note,
// takes about 6.3 ms on the reference and under 1 ms on this engine, which
// applied the whole step at the first sample of the next period.
//
// The mode _iterate_phase() writes carries the speed of that move in its low
// byte, in the encoding slew_calc.h documents.  Two speeds land on the first
// sample and hold:
//   0x00  the level did not change this period (written as the mode 0xff00)
//   0xaf  "fast to target, land exactly and pin", which _iterate_phase()
//         writes for a segment of zero duration - including a note's first
//         control period.  The TVA's equivalent was measured at about 0.5 ms
//         (P-0062), and the arithmetic in slew_calc.h gives about 1 ms here;
//         both are shorter than the 8 ms period, so they are taken as
//         immediate.
// Every other speed is treated as a linear ramp across the period.  That is
// the same simplification TVA::_smooth() makes for the TVA envelope, and it
// keeps the settled cutoff exactly at the value _iterate_phase() computed,
// which is what the reference's steady state measures at; the reference's
// per-sample shape inside the period is finer than the measurement resolves.
void TVF::_smooth_cutoff(void)
{
  int speed = _envLevelMode & 0xff;

  if (speed == 0x00 || speed == 0xaf || _envLevel == _prevEnvLevel) {
    _coFreq.fill(_envLevel);
    return;
  }

  float level = _prevEnvLevel;
  float step = (_envLevel - _prevEnvLevel) / 256.0f;
  for (int i = 0; i < 256; i++) {
    level += step;
    _coFreq[i] = (int) level;
  }

  _coFreq[255] = _envLevel;                  // land exactly on the target
}


void TVF::note_off(uint8_t releaseVelocity)
{
  set_jv_release_velocity(releaseVelocity);
  set_phase(Envelope::Phase::Release);
}


// Run regularly for every 256 samples @32k sample rate => 125Hz
void TVF::update(void)
{
  if (_svf == nullptr)                       // TVF disabled
    return;

  // The JV's filter envelope steps on every SECOND control period, because its
  // firmware services this envelope on alternate wakes of an 8 ms task and this
  // engine's control period is that same 8 ms. Between ticks the coefficient
  // keeps moving: _jv_apply_sample_set() walks it across the whole tick.
  if (_jv) {
    if (++_jvTickCount >= _jvLaw->envTickPeriods) {
      _jvTickCount = 0;
      _jvRampPos = 0;
      _jv_iterate();
    }
    return;
  }

  // Update LFO depth parameters based on fade-in status
  if (!_lfo1FadeComplete)
    _update_lfo_depth(1);
  if (!_lfo2FadeComplete)
    _update_lfo_depth(2);

  _iterate_phase();

  // Update filter coefficients.  The cutoff is set per sample by
  // _smooth_cutoff() from apply_sample_set(); the resonance is a control-rate
  // parameter and moves one step per period (_iterate_phase()).
  _svf->set_resonance(_resonance);

  if (0)
    std::cout << std::hex << "0x" << _envLevel << "  -  " << _resonance
              << std::endl;
}


void TVF::_update_lfo_depth(int lfo)
{
  if (lfo == 1) {
    if (_LFO1->fade() != UINT16_MAX) {
      _lfo1Depth = (_LFO1->fade() *
		    _LUT.LFOTVFDepth[_instPartial.TVFLFO1Depth & 0x7f]) >> 16;
    } else {
      _lfo1Depth = _LUT.LFOTVFDepth[_instPartial.TVFLFO1Depth & 0x7f];
      _lfo1FadeComplete = true;
    }

  } else if (lfo == 2) {
    if (_LFO2->fade() != UINT16_MAX) {
      _lfo2Depth = (_LFO2->fade() *
		    _LUT.LFOTVFDepth[_instPartial.TVFLFO2Depth & 0x7f]) >> 16;
    } else {
      _lfo2Depth = _LUT.LFOTVFDepth[_instPartial.TVFLFO2Depth & 0x7f];
      _lfo2FadeComplete = true;
    }
  }
}


void TVF::_init_envelope(void)
{
  _phaseLevel[0] = 0x40;
  _phaseLevel[1] = _instPartial.TVFEnvL1;
  _phaseLevel[2] = _instPartial.TVFEnvL2;
  _phaseLevel[3] = _instPartial.TVFEnvL3;
  _phaseLevel[4] = _instPartial.TVFEnvL4;
  _phaseLevel[5] = _instPartial.TVFEnvL5;

  _phaseTime[0] = 0;                                     // Never used
  _phaseTime[1] = _instPartial.TVFEnvT1 & 0x7F;
  _phaseTime[2] = _instPartial.TVFEnvT2 & 0x7F;
  _phaseTime[3] = _instPartial.TVFEnvT3 & 0x7F;
  _phaseTime[4] = _instPartial.TVFEnvT4 & 0x7F;
  _phaseTime[5] = _instPartial.TVFEnvT5 & 0x7F;

  // Adjust time for Envelope Time Key Follow including Envelope Time Key Preset
  set_time_key_follow(Envelope::Type::TVF, 0, _key,
                      _instPartial.TVFETKeyF14 - 0x40, _instPartial.TVFETKeyFP14);
  set_time_key_follow(Envelope::Type::TVF, 1, _key,
                      _instPartial.TVFETKeyF5 - 0x40, _instPartial.TVFETKeyFP5);

  // Adjust time for Envelope Time Velocity Sensitivity
  set_time_velocity_sensitivity(Envelope::Type::TVF, 0,
                                _instPartial.TVFETVSens12 - 0x40, _velocity);
  set_time_velocity_sensitivity(Envelope::Type::TVF, 1,
                                _instPartial.TVFETVSens35 - 0x40, _velocity);

  // The JV's own time sense (scdb D-27), the same law as the TVA's on the
  // filter envelope's own three nibbles.
  set_jv_time_sense(_instPartial.TVFJVVelT1, _instPartial.TVFJVVelT4,
                    _instPartial.TVFJVTimeKF, _key, _velocity,
                    _instPartial.JVDelayKeyOff != 0);

  if (0) {
    std::cout << "\nNew TVF envelope [" << std::dec << (int) _key << "]\n"
	      << " Attack 1: L=0 -> L=" << _phaseLevel[1]
	      << " T=" << _phaseTime[1] << std::endl
	      << " Attack 2: L=" << _phaseLevel[1] << " -> L=" << _phaseLevel[2]
	      << " T=" << _phaseTime[2] << std::endl
	      << " Decay 1: L=" << _phaseLevel[2] << " -> L=" << _phaseLevel[3]
	      << " T=" << _phaseTime[3] << std::endl
	      << " Decay 2: L=" << _phaseLevel[3] << " -> L=" << _phaseLevel[4]
	      << " T=" << _phaseTime[4] << std::endl
              << " Sustain: -- L=" << _phaseLevel[4] << std::endl
              << " Release: --> L=" << _phaseLevel[5]
	      << " T=" << _phaseTime[5] << std::endl;
  }

  // Initialization run in Phase=Off and with manually update LFO1 depth
  _lfo1Depth = (_LFO1->fade() * _lfo1Depth) >> 16;
  _iterate_phase();

  _init_new_phase(Phase::Attack1);
}


int TVF::_get_velocity_from_vcurve(uint8_t velocity)
{
  // See the note in TVA::_get_velocity_from_vcurve: a single-curve bank means
  // the device does its own velocity shaping and the selector is not an index.
  const size_t curveCount = _LUT.VelocityCurves.size() / 128;
  const unsigned int curve = (curveCount > 1) ? _instPartial.TVFCOFVelCur : 0;

  unsigned int address = curve * 128 + velocity;
  if (address > _LUT.VelocityCurves.size()) {
    std::cerr << "libEmuSC internal error: Illegal velocity curve used"
              << std::endl;
    return 0;
  }

  return _LUT.VelocityCurves[address];
}


int TVF::_read_cutoff_freq_vel_sens(int cofvsROM)
{
  int v = 127 - _velocity;
  int res = 0x7fff;
  if (cofvsROM != 0)
    res -= ((v * _LUT.TVFCutoffVSens[std::abs(cofvsROM)]) & 0xffff);

  return res;
}


int TVF::_get_level_init(int level)
{
  int depth = (_envDepth * _coFreqVSens) * 2;
  int scale = _LUT.TVFEnvScale[std::clamp(std::abs(level - 0x40), 0, 63)];
  int tmp = (scale * ((depth & 0xffff0000) >> 16)) * 2;
  int res = ((tmp & 0x0000ff00) >> 8) + ((tmp & 0x00ff0000) >> 8);

  if (level >= 0x40)
    res += _keyFollow;
  else
    res = _keyFollow - res;

  return res;
}


void TVF::_init_freq_and_res(void)
{
  _L1Init = _get_level_init(_instPartial.TVFEnvL1);
  _L2Init = _get_level_init(_instPartial.TVFEnvL2);
  _L3Init = _get_level_init(_instPartial.TVFEnvL3);
  _L4Init = _get_level_init(_instPartial.TVFEnvL4);
  _L5Init = _get_level_init(_instPartial.TVFEnvL5);

  int envLevelMax = _keyFollow;
  envLevelMax = std::max(envLevelMax, _L1Init);
  envLevelMax = std::max(envLevelMax, _L2Init);
  envLevelMax = std::max(envLevelMax, _L3Init);
  envLevelMax = std::max(envLevelMax, _L4Init);
  envLevelMax = std::max(envLevelMax, _L5Init);

  // Whether a positive TVF Cutoff Frequency raises the partial's base cutoff is
  // per-device: see TvfCutoffLaw. The fit behind it is P-0133 - the mkII's
  // cutoff runs +2 -> +2.1, +8 -> +8.2, +16 -> +15.7, +32 -> +30.3, +40 -> +39.6
  // cutoff steps and then saturates, while the mk1 stays within 0.1 of its base
  // for every value from +2 to +63.
  const TvfCutoffLaw &law = _settings->device()->tvfCutoff;
  int tm3 = _settings->get_param(PatchParam::TVFCutoffFreq, _partId) - 0x40;
  bool cofOffsetRaises = law.offsetRaises;
  tm3 = std::clamp(tm3, -0x32, law.offsetMax);
  int bptm3 = (tm3 < 0 || cofOffsetRaises) ? _instPartial.TVFBaseFlt + tm3
                                           : _instPartial.TVFBaseFlt;
  // _iterate_phase() clamps the same sum to 0x7f before it uses it
  // (accCoFreq); this one did not, so a large positive offset ran the
  // resonance lookup off the end of the cutoff table while the cutoff itself
  // stopped at 0x7f.
  bptm3 = std::min(bptm3, 0x7f);

  int cofIndex = std::clamp(envLevelMax + (bptm3 << 8), 0, 0x7fff);
  cofIndex = std::min(cofIndex + 0xff, 0x7fff);

  // The cutoff this filter can reach is capped in _iterate_phase() at the
  // coefficient 0xe600, which is cutoff index 120.3; _cutoffCeiling is the
  // first table index above that cap.  Reading TVFResonanceFreq above it
  // asks what resonance a cutoff the filter never uses would allow, and the
  // table answers 0 there, so the damping index collapses to its floor of 8
  // and the filter rings where the reference does not.  Measured
  // (PROVENANCE.md P-0161) on five tones whose filter is static, at keys 24,
  // 36 and 60: with the part parameter driven past that point the
  // reference's damping index settles at 10, which is TVFResonanceFreq at
  // _cutoffCeiling exactly, not at 8.
  //
  // Only the offset is held back, never the partial's own base and envelope:
  // with no positive offset cofIndex is already cofIndexNoOffset, so this is
  // a no-op there, and the SC-55 generation - which ignores positive offsets
  // altogether (P-0133) - is untouched.
  int cofIndexNoOffset =
    std::clamp(envLevelMax + (_instPartial.TVFBaseFlt << 8), 0, 0x7fff);
  cofIndexNoOffset = std::min(cofIndexNoOffset + 0xff, 0x7fff);
  int cutoffIdx = std::min(cofIndex >> 8,
                           std::max(cofIndexNoOffset >> 8, _cutoffCeiling));

  int cof = _LUT.TVFCutoffFreq[cutoffIdx];
  int rfIndex = (2 * cof) + 0xff;
  rfIndex = rfIndex > 0xffff ? 0xff00 : rfIndex;
  _resIndexFreq = _LUT.TVFResonanceFreq[(rfIndex >> 8)];

  int tm4 = _settings->get_param(PatchParam::TVFResonance, _partId) - 0x40;
  tm4 = std::clamp(tm4, -0x32, 0x32);
  _resonance = std::min(_instPartial.TVFResonance - (tm4 * 2), _resIndexFreq);
  _resonance = std::max(_resonance, 0);
}


int TVF::_get_cof_key_follow(int cofkfROM)
{
  int kmIndex = _LUT.KeyMapperIndex[48 + _instPartial.TVFCFKeyFlwC] - _LUT.KeyMapperOffset;
  int km = static_cast<int>(_native_endian_uint16((uint8_t *) &_LUT.KeyMapper[kmIndex + _key * 2]));
  int cofkf = _LUT.TVFCutoffFreqKF[std::abs(cofkfROM)];
  int res = ((km - 0x4000) * cofkf) >> 8;

  if (0)
    std::cout << "km=0x" << std::hex << km
	      << " cofkfROM=" << std::dec << cofkfROM
	      << " mulxu.w=0x" << std::hex << ((km - 0x4000) * cofkf)
	      << " res=" << res << std::dec
	      << std::endl;

  if (cofkfROM < 0)
    return  -res;

  return res;
}


uint16_t TVF::_native_endian_uint16(uint8_t *ptr)
{
  if (_le_native())
    return (ptr[0] << 8 | ptr[1]);

  return (ptr[1] << 8 | ptr[0]);
}


void TVF::_iterate_phase(void)
{
  if (_phasePosition >= 0xffff) {
    if (_phase == Phase::Attack1) {
      _init_new_phase(Phase::Attack2);
    } else if (_phase == Phase::Attack2) {
      _init_new_phase(Phase::Decay1);
    } else if (_phase == Phase::Decay1) {
      _init_new_phase(Phase::Decay2);
    } else if (_phase == Phase::Decay2) {
      _init_new_phase(Phase::Sustain);
    } else if (_phase == Phase::Release) {
      _phase = Phase::Terminated;
      return;
    }
  }

  int segmentCurveIndex = 0;

  if (_phase == Phase::Init) {                    // Initialization run
    _ipLevelInit = _keyFollow & 0xffff;

  } else if (_phase == Phase::Sustain) {          // Sustain phase
    _phaseRemainder = 0;
    segmentCurveIndex = 8;

  } else if (_phaseDuration <= _instantTicks) {               // Very short phase duration
    _phasePosition = 0xffff;
    segmentCurveIndex = _phaseDuration;
    _ipLevelInit = _currentLevelInit & 0xffff;

  } else {                                        // Normal phase duration
    _phaseStepSize = (8 << 16) / _phaseDuration;
    segmentCurveIndex = 8;

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

    if (phaseStepDelta != 0) {
      if (phaseAccInc < 0xffff)
        phaseStepDelta -= 1;

      _phaseRemainder = (phaseStepDelta / _phaseStepSize) & 0xffff;
      _phasePosition = 0xffff;

      _ipLevelInit = _currentLevelInit & 0xffff;

    } else {
      _phasePosition = phaseAccInc;

      // Interpolation of Level Init values
      int prev = _prevLevelInit & 0xffff;
      int curr = _currentLevelInit & 0xffff;
      int phase = _phasePosition;

      if (prev == curr) {
        _ipLevelInit = prev;

      } else if ((prev ^ curr) & 0x8000) {        // Different signs
        int16_t absPrev = std::abs(prev);
        int16_t absCurr = std::abs(curr);
        int16_t mag = std::abs(absCurr - absPrev);
        if (mag < 0) mag = std::numeric_limits<int16_t>::max();

        int16_t scaled = (int16_t) ((uint32_t(uint16_t(mag)) * phase) >> 16);
        int16_t magResult = (absCurr >= absPrev) ? absPrev + scaled : absPrev - scaled;
        _ipLevelInit = (curr < 0) ? -magResult : magResult;

      } else {                                    // Same signs
        int16_t mag = std::abs(curr - prev);
        if (mag < 0) mag = std::numeric_limits<int16_t>::max();

        int16_t scaled = (int16_t) ((uint32_t(uint16_t(mag)) * phase) >> 16);
        _ipLevelInit = (curr >= prev) ? prev + scaled : prev - scaled;
      }
    }
  }

  // See _init_freq_and_res(): only the mkII generation lets the parameter
  // raise the cutoff above the partial's own base (PROVENANCE.md P-0133)
  const TvfCutoffLaw &law = _settings->device()->tvfCutoff;
  int tmCF = _settings->get_param(PatchParam::TVFCutoffFreq, _partId);
  bool cofOffsetRaises = law.offsetRaises;
  tmCF = std::clamp(tmCF, 0xe, law.paramMax);
  int accCoFreq = std::clamp(_instPartial.TVFBaseFlt + (tmCF - 0x40), 0,
                             cofOffsetRaises ? 0x7f
                                             : (int) _instPartial.TVFBaseFlt);

  uint16_t accDepth = _ipLevelInit + (accCoFreq << 8);
  accDepth = std::clamp((int) accDepth, 0, INT16_MAX);

  int accCutoffCtrl = _settings->get_acc_control_param(Settings::ControllerParam::TVFCutoff, _partId);

  accDepth = std::clamp(accDepth + std::abs(accCutoffCtrl), 0, (int) INT16_MAX);

  // This is how far the "TVF envelope" goes
  _envelopeOut = accDepth >> 8;

  // Add LFO modulations to cutoff frequency
  int lfoDepth = std::abs(_lfo1Depth +
                          _settings->get_acc_control_param(Settings::ControllerParam::LFO1TVFDepth, _partId));
  lfoDepth = std::min(lfoDepth, 0x1800);
  int lfoProd = (_LFO1->value() << 1) * lfoDepth;
  int lfoMod = (lfoProd + 0x8000) >> 16;
  accDepth = std::clamp(accDepth + lfoMod, 0, INT16_MAX);

  lfoDepth = std::abs(_lfo2Depth +
                      _settings->get_acc_control_param(Settings::ControllerParam::LFO2TVFDepth, _partId));
  lfoDepth = std::min(lfoDepth, 0x1800);
  lfoProd = int32_t(_LFO2->value() << 1) * lfoDepth;
  lfoMod = (lfoProd + 0x8000) >> 16;
  accDepth = std::clamp(accDepth + lfoMod, 0, INT16_MAX);

  int tmRes = _settings->get_param(PatchParam::TVFResonance, _partId) - 0x40;
  tmRes = std::clamp(_instPartial.TVFResonance - tmRes * 2,
                     0, _resIndexFreq);

  if ((tmRes & 0xff) != _resonance) {
    if ((tmRes & 0xff) < _resonance)
      _resonance -= 1;
    else
      _resonance += 1;

    int resFreqIndex = std::min(_envLevel + 0xff, 0xff00);
    int resFreq = _LUT.TVFResonanceFreq[resFreqIndex >> 8];
    _resonance = std::min(_resonance, resFreq);
  }

  int ipCoFreq;
  int coFreq1 = _LUT.TVFCutoffFreq[(accDepth >> 8)];

  if (accDepth == 0) {
    ipCoFreq = coFreq1;

  } else {
    int coFreq2 = _LUT.TVFCutoffFreq[(accDepth >> 8) + 1];
    ipCoFreq = coFreq1 + (((coFreq2 - coFreq1) * (accDepth & 0xff)) >> 8);
  }

  ipCoFreq *= 2;
  _resonance = std::max(_resonance, 8);

  int res = _LUT.TVFResonance[_resonance] << 8;
  if (res < ipCoFreq)
    ipCoFreq = res;

  ipCoFreq = std::min(ipCoFreq, 0xe600);

  _prevEnvLevel = _envLevel;
  _envLevel = ipCoFreq;

  int intEnvValue = _envLevel;
  if (_envLevel == _prevEnvLevel) {
    _envLevelMode = 0xff00;
    return;

  } else if (_envLevel > _prevEnvLevel) {
    intEnvValue &= 0xff00;
    if (intEnvValue == (_prevEnvLevel & 0xff00)) {
      intEnvValue += 0x100;
      intEnvValue = std::max(intEnvValue,
                             _LUT.TVFCutoffFreq[_resonance] << 8);
    }
  }

  intEnvValue = std::min(intEnvValue, 0xe600);

  if (segmentCurveIndex == 0) {
    _envLevelMode = (intEnvValue & 0xff00) | 0xaf;  // Env. segment acc. preload
    return;
  }

  int segmentStepIndex = _LUT.EnvSegmentCurve[segmentCurveIndex];
  int phaseAccumulator = _envLevel - _prevEnvLevel;
  if (phaseAccumulator < 0) phaseAccumulator = -phaseAccumulator;

  for (int i = 0; i < 8; i++) {
    uint16_t prev = phaseAccumulator;
    phaseAccumulator <<= 1;
    if(0)
      std::cout << "prev=" << prev << std::endl;
    if (prev & 0x8000)
      break;

    segmentStepIndex --;
  }

  if (segmentStepIndex < 0) {
    segmentStepIndex = 0;
    phaseAccumulator >>= 1;
  }

  int tvfLow = (phaseAccumulator >> 8) & 0xff;
  tvfLow = ((tvfLow >> 3) + 1) >> 1;
  tvfLow |= _LUT.EnvSegmentStep[segmentStepIndex];

  _envLevelMode = (intEnvValue & 0xff00) + tvfLow;
}


void TVF::_init_new_phase(enum Phase newPhase)
{
  // The JV's chain runs its own phases from _jv_next_phase(); the only phase
  // change that reaches it from outside is the note off.
  if (_jv) {
    if (newPhase != Phase::Release)
      return;

    // The release starts from wherever the envelope has got to and walks to the
    // release LEVEL, which for the filter envelope is a target like any other -
    // unlike the TVA's, which always releases to silence.
    _phaseStartValue = _jvEnvLevel >> 8;
    _phaseEndValue   = _phaseLevel[static_cast<int>(Phase::Release)];
    _phaseDuration   = _LUT.envelopeTime[
                         std::clamp(_phaseTime[static_cast<int>(Phase::Release)],
                                    0, 127)];
    _jvDecrement = (_phaseDuration > 0) ? std::clamp((1 << 20) / _phaseDuration,
                                                    1, 0xffff)
                                        : 0x10000;
    _phasePosition = 0;
    _phase = Phase::Release;
    return;
  }

  if (newPhase == Phase::Terminated) {
    std::cerr << "libEmuSC: Internal error, envelope in illegal state"
	      << std::endl;
    return;

  } else if (newPhase == Phase::Attack1) {
    _prevLevelInit = _ipLevelInit;                // Output from pre-run
    _currentLevelInit = _L1Init;

    _currentEnvTime = _phaseTime[static_cast<int>(newPhase)];

    _phaseStartValue = _phaseLevel[static_cast<int>(_phase)];
    _phaseEndValue = _phaseLevel[static_cast<int>(newPhase)];

  } else if (newPhase == Phase::Attack2) {
    _prevLevelInit = _L1Init;
    _currentLevelInit = _L2Init;

    _currentEnvTime = _phaseTime[static_cast<int>(newPhase)];

    _phaseStartValue = _phaseLevel[static_cast<int>(_phase)];
    _phaseEndValue = _phaseLevel[static_cast<int>(newPhase)];

  } else if (newPhase == Phase::Decay1) {
    _prevLevelInit = _L2Init;
    _currentLevelInit = _L3Init;

    _currentEnvTime = _phaseTime[static_cast<int>(newPhase)];

    _phaseStartValue = _phaseLevel[static_cast<int>(_phase)];
    _phaseEndValue = _phaseLevel[static_cast<int>(newPhase)];

  } else if (newPhase == Phase::Decay2) {
    _prevLevelInit = _L3Init;
    _currentLevelInit = _L4Init;

    _currentEnvTime = _phaseTime[static_cast<int>(newPhase)];

    _phaseStartValue = _phaseLevel[static_cast<int>(_phase)];
    _phaseEndValue = _phaseLevel[static_cast<int>(newPhase)];

  } else if (newPhase == Phase::Sustain) {
    _phase = newPhase;

    if (_envLevel == 0)
      _finished = true;

    return;

  } else if (newPhase == Phase::Release) {
    _prevLevelInit = _ipLevelInit;
    _currentLevelInit = _L5Init;
    _currentEnvTime = _phaseTime[static_cast<int>(newPhase)];

    _phaseStartValue = _envLevel >> 8;  //_envLevelMode; // (_envLevelMode >> 8);
    _phaseEndValue = _phaseLevel[static_cast<int>(newPhase)];
  }

  _phaseDuration = _phaseTime[static_cast<int>(newPhase)];

  // TODO: Add test for PD#4 on TVF envelopes
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
  _phaseDuration = _jv_time_sense(_phaseDuration, newPhase);
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
    std::cout << "New TVF envelope phase: -> "
              << std::dec << static_cast<int>(newPhase)
	      << " (" << _phaseName[static_cast<int>(newPhase)] << "): Level = "
              << _phaseStartValue << " -> " << _phaseEndValue
	      << " | Time = 0x" << std::hex << _phaseDuration << " => "
	      << std::dec << (_phaseDuration * 8) / 32000.0 << "s" << std::endl;
  }

  _phase = newPhase;
}



// ---------------------------------------------------------------------------
// The JV family's filter chain (PROVENANCE.md P-0390)
//
// Every step below is the firmware's, in the firmware's units. Read the tone's
// fields, then once per envelope tick:
//
//   env    = envelope level << 8, less its velocity attenuation
//   x      = (env * envDepth) >> 16, in CENTS, plus key follow and both LFOs
//   E      = 256 * 2^(x/1200), from a coarse table and a fine one
//   word   = (E * BASE[cutoff]) >> 8, saturating
//   word   = min(word, LIMIT[resonance]), damp = DAMP[resonance]
//
// and hand F1 = word/0x8000 and Q1 = damp/0x4000 to the state-variable filter.
// x being in cents is what makes the whole modulation stack a frequency RATIO
// and BASE[cutoff] the coefficient it multiplies; the Sound Canvas chain above
// instead builds a cutoff INDEX, which is why the two cannot share code.
// ---------------------------------------------------------------------------

void TVF::_jv_init(uint8_t velocity)
{
  // If the device's own filter tables did not load, the filter is left DISABLED
  // rather than run on zeros: a zero base coefficient is a filter closed to
  // silence, which is a far worse answer than no filter. Entry 127 of the base
  // table is 0xffff in every ROM that has one, so it doubles as the check.
  if (_LUT.JVTvfBase[127] == 0) {
    delete _svf;
    _svf = nullptr;
    return;
  }

  // TVF-ENV Depth, signed. The scale is the firmware's and it is not arbitrary:
  // depth +63 at envelope level 127 comes out at exactly 9600 cents, eight
  // octaves, which is where the exponential table saturates.
  {
    const int d = (int8_t) _instPartial.TVFEnvDepth;
    const int m = ((std::abs(2 * d) << 8) * _jvLaw->envDepthScale) >> 16;
    _jvEnvDepth = (d < 0) ? -m : m;
  }

  // ROM1 0x489d: the shared velocity helper with this tone's TVF curve and
  // TVF-ENV velocity sensitivity (jv_velocity.h).
  _jvVelAtten = jv_velocity_attenuation(_LUT.JVVelCurves,
                                        _instPartial.TVFCOFVelCur,
                                        _instPartial.TVFEnvVelSens, velocity);

  // Key follow, in cents per semitone from note 60. The table IS the manual's
  // published percentage list: +100 % is the value 100, i.e. 1:1 tracking.
  _jvKeyFollow = ((int) _key - 60) *
                 _LUT.JVTvfCutoffKF[_instPartial.TVFCOFKeyFlwIdx & 0x0f];

  _jvLfo1Depth = (int8_t) _instPartial.TVFLFO1Depth * _jvLaw->lfoDepthScale;
  _jvLfo2Depth = (int8_t) _instPartial.TVFLFO2Depth * _jvLaw->lfoDepthScale;

  _jvCutoff = std::clamp((int) _instPartial.TVFBaseFlt, 0, 127);
  _jvResTarget = std::clamp((int) _instPartial.TVFResonance, 0, 127);

  // No glide at note on. The per-voice resonance slew state is RAM whose value
  // when a note starts is not established, so it starts AT the target: the
  // alternative, starting from zero, would invent an eight-tick sweep on every
  // note. A resonance change during the note still slews.
  _jvRes = _jvResTarget;

  // The envelope: three time/level segments and a release, all levels 0..127.
  // Level 0 is where a note on starts, which is the firmware's own segment 0
  // start value.
  _phaseLevel[0] = 0;
  _phaseLevel[1] = _instPartial.TVFEnvL1;
  _phaseLevel[2] = _instPartial.TVFEnvL2;
  _phaseLevel[3] = _instPartial.TVFEnvL3;
  _phaseLevel[4] = _instPartial.TVFEnvL3;    // not the JV's; never entered
  _phaseLevel[5] = _instPartial.TVFEnvL5;

  _phaseTime[0] = 0;
  _phaseTime[1] = _instPartial.TVFEnvT1 & 0x7f;
  _phaseTime[2] = _instPartial.TVFEnvT2 & 0x7f;
  _phaseTime[3] = _instPartial.TVFEnvT3 & 0x7f;
  _phaseTime[4] = 0;
  _phaseTime[5] = _instPartial.TVFEnvT5 & 0x7f;

  _phase = Phase::Init;
  _jv_next_phase();                          // -> Attack1
  _jv_iterate();                             // the coefficient the note starts on
  _jvWordPrev = _jvWord;                     // and so nothing to ramp from
}


// Enter the next segment of the JV's filter envelope. Its envelope has one
// segment fewer than this engine's, so Decay1 goes straight to Sustain rather
// than through Decay2.
void TVF::_jv_next_phase(void)
{
  Phase next;
  switch (_phase) {
  case Phase::Init:    next = Phase::Attack1; break;
  case Phase::Attack1: next = Phase::Attack2; break;
  case Phase::Attack2: next = Phase::Decay1;  break;
  case Phase::Decay1:  next = Phase::Sustain; break;
  case Phase::Release: next = Phase::Terminated; break;
  default:             next = Phase::Sustain; break;
  }

  if (next == Phase::Sustain || next == Phase::Terminated) {
    // Sustain holds L3 and the terminated release holds L4, both of which are
    // already this segment's target.
    _phase = next;
    _phasePosition = 0;
    _jvDecrement = 0;
    return;
  }

  _phaseStartValue = _phaseLevel[static_cast<int>(_phase)];
  _phaseEndValue   = _phaseLevel[static_cast<int>(next)];
  _phaseDuration   = _LUT.envelopeTime[
                       std::clamp(_phaseTime[static_cast<int>(next)], 0, 127)];

  // The firmware's own rate arithmetic: a 16-bit accumulator stepped by
  // 2^20 / duration_ms once per 16 ms tick, so a segment lasts its duration in
  // milliseconds. A duration of 0 - which is what a time byte of 0 gives - is
  // the skip: the accumulator runs out on the first tick and the segment is
  // left again in the same tick, at its end level.
  _jvDecrement = (_phaseDuration > 0) ? std::clamp((1 << 20) / _phaseDuration,
                                                  1, 0xffff)
                                      : 0x10000;
  _phasePosition = 0;
  _phase = next;
}


void TVF::_jv_iterate(void)
{
  // Step the envelope. The decrement happens before the level is taken, and a
  // segment that runs out is left at its end level rather than interpolated -
  // both as the firmware does it. The guard bounds the walk through segments
  // whose duration is zero.
  if (_phase != Phase::Sustain && _phase != Phase::Terminated) {
    _phasePosition += _jvDecrement;

    for (int guard = 0; guard < 8 && _phasePosition >= 0xffff; guard++) {
      _jv_next_phase();
      if (_phase == Phase::Sustain || _phase == Phase::Terminated)
        break;
      if (_jvDecrement >= 0x10000)           // a zero-length segment: skip it
        _phasePosition = 0xffff;
    }
  }

  if (_phase == Phase::Sustain || _phase == Phase::Terminated) {
    _jvEnvLevel = _phaseEndValue << 8;
  } else {
    const int from = _phaseStartValue << 8;
    const int to   = _phaseEndValue << 8;
    _jvEnvLevel = from + (int) (((int64_t) (to - from) * _phasePosition) >> 16);
  }
  _envelopeOut = _jvEnvLevel >> 8;

  int env = _jvEnvLevel;
  env -= (int) (((int64_t) env * _jvVelAtten) >> 16);

  // Cents from here on: envelope depth, key follow and the two LFOs all land in
  // one signed 16-bit accumulator.
  int x = (int) (((int64_t) env * _jvEnvDepth) >> 16);
  x += _jvKeyFollow;
  x += ((int) _LFO1->value() * _jvLfo1Depth) >> 16;
  x += ((int) _LFO2->value() * _jvLfo2Depth) >> 16;
  x = (int16_t) x;

  const int coarse = _LUT.JVTvfExpCoarse[(x >> 8) & 0xff];
  const int E = coarse + ((coarse * _LUT.JVTvfExpFine[x & 0xff]) >> 16);

  // The product SATURATES, and it has to be computed wide enough to see that.
  // E reaches 65536 when the envelope drives the cutoff a full octave up
  // (x = 9600 cents) and JVTvfBase[127] is 0xffff, so E * base reaches about
  // 4.29e9 - past INT_MAX. Computed in `int` it wrapped NEGATIVE, and then
  // `word > 0xffff` could not fire on a negative value: the resonance-0 cap
  // below kept it, and a negative F1 (measured -0.3125) puts the
  // state-variable filter's poles outside the unit circle. The state then grew
  // exponentially to inf and to NaN, which the render clamped to the negative
  // rail - 427496 non-finite samples on one demo channel, sounding as a 6.68 s
  // full-scale blast. scdb D-51.
  int word = (int) std::min<int64_t>(
      ((int64_t) E * (int64_t) _LUT.JVTvfBase[_jvCutoff]) >> 8, 0xffff);

  // Resonance moves at most one slew step per tick, and decides both the cutoff
  // ceiling and the damping. Resonance 0 is not a table row but a rule of its
  // own, whose boundary agrees with the tables exactly.
  if (_jvRes != _jvResTarget)
    _jvRes += std::clamp(_jvResTarget - _jvRes,
                         -_jvLaw->resSlewPerTick, _jvLaw->resSlewPerTick);

  int damp;
  if (_jvRes == 0) {
    word = std::min(word, _jvLaw->zeroResLimit);
    damp = std::max(_jvLaw->zeroResDampBase - (word >> 3),
                    _jvLaw->zeroResDampFloor);
  } else {
    const bool hard = _instPartial.TVFResoMode != 0;
    word = std::min(word, hard ? _LUT.JVTvfLimitHard[_jvRes]
                               : _LUT.JVTvfLimitSoft[_jvRes]);
    damp = hard ? _LUT.JVTvfDampHard[_jvRes] : _LUT.JVTvfDampSoft[_jvRes];
  }

  _jvWordPrev = _jvWord;
  _jvWord = word;
  _jvQ1 = (float) damp / (float) _jvLaw->dampUnity;

  static const bool dbg = getenv("EMUSC_DEBUG_TVF") != nullptr;
  if (dbg)
    std::cerr << "JV TVF: env=" << std::dec << (_jvEnvLevel >> 8)
              << " x=" << x << " word=0x" << std::hex << word
              << " damp=0x" << damp << std::dec << " res=" << _jvRes
              << " F1=" << ((float) word / 32768.0f)
              << " Q1=" << _jvQ1
              << std::endl;
}


// The coefficient moves across the tick rather than stepping at its boundary.
// On the hardware the CPU writes the target's high byte together with a slew
// rate and the chip walks the coefficient there itself; the reconstruction of
// that rate byte is not established, so the move is taken as linear across the
// tick, landing exactly on the target. That is the same simplification
// _smooth_cutoff() and TVA::_smooth() already make, and for the same reason:
// the settled value is what the reference measures at, and the shape inside one
// tick is finer than the measurement resolves.
void TVF::_jv_apply_sample_set(std::array<float, 256> &dryBus)
{
  const float unity = (float) _jvLaw->cutoffUnity;
  const float from = _jvWordPrev / unity;
  const float to   = _jvWord / unity;
  const float span = (float) (256 * _jvLaw->envTickPeriods);

  for (int i = 0; i < 256; i++) {
    float t = (_jvRampPos + i + 1) / span;
    if (t > 1.0f)
      t = 1.0f;
    _svf->set_coefficients(from + (to - from) * t, _jvQ1);
    dryBus[i] = _svf->process_sample(dryBus[i]);
  }

  _jvRampPos += 256;
}

}
