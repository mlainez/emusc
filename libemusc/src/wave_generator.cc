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

// Wave generator class used to generate low frequency oscialltors (LFOs).
// All models in the Sound Canvas line have 2 LFOs:
//  - LFO1 is definde per instrument and is shared with both partials
//  - LFO2 is defined per instrument partial
// This gives a maximum of 3 separate LFOs per note.

// Each LFO has 5 parameters in the instrument [partial] definition:
//   - Waveform and phase shift
//   - Rate
//   - Delay
//   - Fade (fade-in)
// LFO rate and delay parameters can be changed by clients or SysEx messages,
// but only rate can be changed after a "note on" event.

// The folllowing waveforms are supported by the SC-55 family:
//  0: Sine
//  1: Square
//  2: Sawtooth
//  3: Triangle
//  8: Sample & Hold (random sample)
//  9: Random (sample & glide)
// 10: Random (same as 9, but most likely intended to have longer step size)

// All waveforms can be 0, 90, 180 or 270 degrees phase shifted.
// Rate, delay and fade values are all defined by lookup tables in the CPU ROM.

// During LFO1 Fade parameter the LFO will also update TVA / TVF / Pitch depth.


#include "wave_generator.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include <stdint.h>


namespace EmuSC {


WaveGenerator::WaveGenerator(struct ControlRom::Instrument &instrument,
                             struct ControlRom::LookupTables &LUT,
                             Settings *settings, int partId)
  : _id(0),
    _LUT(LUT),
    _delay(0),
    _fade(0),
    _currentValue(0),
    _currentValueNorm(0),
    _random(0),
    _randomFirstRun(true),
    _settings(settings),
    _partId(partId)
{
  _waveform = (enum Waveform) (instrument.LFO1Waveform & 0x0f);
  _instRate = instrument.LFO1Rate;

  // Note: Vibrato Delay equals 2x in ROM values
  int delayInput = instrument.LFO1Delay +
    (settings->get_param(PatchParam::VibratoDelay, _partId) - 0x40) * 2;
  delayInput = std::clamp(delayInput, 0, 127);

  _delayIncLUT = LUT.LFODelayTime[delayInput];
  _fadeIncLUT = LUT.LFODelayTime[instrument.LFO1Fade];

  // Phase shift is done by moving the start off accumulated rate
  _accRate = ((instrument.LFO1Waveform & 0xf0) << 8);

  if (0)
    std::cout << "New LFO1: Waveform=" << (instrument.LFO1Waveform & 0x0f)
              << " Phase=" << (instrument.LFO1Waveform & 0xf0)
              << " Rate=" << _instRate
              << " Delay=" << (int) instrument.LFO1Delay << " -> "
              << _delayIncLUT << " -> " << 512.0 / _delayIncLUT << " s"
              << " Fade=" << (int) instrument.LFO1Fade << " -> " << _fadeIncLUT
              << " -> " << 512.0 / _fadeIncLUT << " s" << std::endl;

  update();
}


WaveGenerator::WaveGenerator(struct ControlRom::InstPartial &instPartial,
                             struct ControlRom::LookupTables &LUT,
                             Settings *settings, int partId)
  : _id(1),
    _LUT(LUT),
    _delay(0),
    _fade(0),
    _currentValue(0),
    _currentValueNorm(0),
    _random(0),
    _randomFirstRun(true),
    _settings(settings),
    _partId(partId)
{
  _waveform = (enum Waveform) (instPartial.LFO2Waveform & 0x0f);
  _instRate = instPartial.LFO2Rate;

  _delayIncLUT = LUT.LFODelayTime[instPartial.LFO2Delay];
  _fadeIncLUT = LUT.LFODelayTime[instPartial.LFO2Fade];

  // Phase shift is done by moving the start off accumulated rate
  _accRate = ((instPartial.LFO2Waveform & 0xf0) << 8);

  if (0)
    std::cout << "New LFO2: Waveform=" << (instPartial.LFO2Waveform & 0x0f)
              << " Phase=" << (instPartial.LFO2Waveform & 0xf0)
              << " Rate=" << _instRate
              << " Delay=" << (int) instPartial.LFO2Delay
              << " -> " << 512.0 / _delayIncLUT << "s"
              << " Fade=" << (int) instPartial.LFO2Fade
              << " -> " << 512.0 / _fadeIncLUT << "s" << std::endl;
}


WaveGenerator::~WaveGenerator()
{}


// This function is called at ~125Hz and has 256 samples between each run @32k
void WaveGenerator::update(void)
{
  if (_jv) {
    _jv_update();
    return;
  }

  // Check if we are in the delay phase (no LFO output)
  if (_delay < 0xffff) {
    _delay += _delayIncLUT;

    if (_delay < 0xffff)
      return;
  }

  // Check if we are in the fade-in phase (scaled LFO output)
  if (_fade < 0xffff) {
    _fade += _fadeIncLUT;

    if (_fade > 0xffff)
      _fade = 0xffff;
  }

  // To calculate the rate we need to use the LUT for converting ROM and
  // "Vibrato Rate" (only LFO1) values. Controller values for LFO1/2 rates are
  // pre-caclculated and just needs to be added. Max rate is 0x28f6.
  int index = _instRate;
  if (_id == 0)
    index += _settings->get_param(PatchParam::VibratoRate, _partId) - 0x40;

  int rate =  _LUT.LFORate[std::clamp(index, 0, 127)];
  if (_id == 0)
    rate += _settings->get_acc_control_param(Settings::ControllerParam::LFO1Rate, _partId);
  else
    rate += _settings->get_acc_control_param(Settings::ControllerParam::LFO2Rate, _partId);

  rate = std::clamp(rate, 0, 0x28f6);

  // FIXME: There is an unknwon multiplication that we are missing
  //        Seems to only be affected with multiple simultaneous notes
  //        -> ROM:3BE1                 mulxu.w @0xAC5A:16, r4

  int LFOValue;
  switch (_waveform) {
    case Waveform::Sine:       LFOValue = _generate_sine(rate);        break;
    case Waveform::Square:     LFOValue = _generate_square(rate);      break;
    case Waveform::Sawtooth:   LFOValue = _generate_sawtooth(rate);    break;
    case Waveform::Triangle:   LFOValue = _generate_triangle(rate);    break;
    case Waveform::SampleHold: LFOValue = _generate_sample_hold(rate); break;
    case Waveform::Random:     LFOValue = _generate_random(rate);      break;
    default:
      std::cerr << "libEmuSC: Internal error! Waveform generator called with "
                << "illegal waveform ID: " << (int) _waveform << std::endl;
      return;
  }

  _currentValue = LFOValue;

  if (LFOValue < 0x8000)
    _currentValueNorm = LFOValue * (_fade / 65535.0);
  else if (LFOValue > 0x8000)
    _currentValueNorm = (LFOValue - 0xffff) * (_fade / 65535.0);
  else
    _currentValueNorm = 0;
}


int WaveGenerator::_generate_sine(int rate)
{
  _accRate += rate;

  int v = _accRate - 0x8000;
  if (v & 0x8000)
    v = -v;

  int index = (v >> 8) & 0xff;
  int a = _LUT.LFOSine[index];
  int b = _LUT.LFOSine[index + 1];

  int diff = b - a;
  int tmp = (diff < 0) ? -(-diff & 0xff) * (v & 0xff) : diff * (v & 0xff);
  uint16_t result = ((a & 0xff) << 8) + tmp;
  result >>= 1;

  if (_accRate > 0x8000)
    result = -result;

  return result;
}


int WaveGenerator::_generate_square(int rate)
{
  _accRate += rate;

  return (_accRate < 0x8000) ? 0x7fff : 0x8001;
}


int WaveGenerator::_generate_sawtooth(int rate)
{
  _accRate += rate;

  return (_accRate - 0x8000) & 0xffff;
}


int WaveGenerator::_generate_triangle(int rate)
{
  _accRate += rate;

  int result;
  if (_accRate == 0x8000) {
    result = _accRate;

  } else if (_accRate < 0x8000u) {
    int32_t v = (int32_t)_accRate - 0x4000;
    if ((uint16_t) v < 0x4000u)
      v = - (int16_t)v;
    result = (uint16_t)(v + v) - 0x8000;

  } else {
    int16_t foldIn = -(int16_t) _accRate;
    int32_t v = (int32_t) foldIn - 0x4000;
    if ((uint16_t) v < 0x4000u)
      v = - (int16_t) v;

    uint16_t vv = (uint16_t) (v + v);
    result = (vv == 0) ? 0x7fff : 0x8000 - vv;
  }

  return result;
}


int WaveGenerator::_generate_sample_hold(int rate)
{
  uint32_t sum = _accRate + (uint32_t) (rate * 2);
  bool overflow = (sum >> 16) & 1;
  _accRate = (uint16_t) sum;

  if (overflow)
    _random = (uint16_t) (rand() & 0xFFFF);

  return _random;
}


int WaveGenerator::_generate_random(int rate)
{
  constexpr int step = 0x50;

  uint32_t sum = _accRate + (uint32_t) (rate * 2);
  bool overflow = (sum >> 16) & 1;
  _accRate = (uint16_t) sum;

  if (overflow || _randomFirstRun) {
    _randomFirstRun = false;
    _random = (uint16_t) (rand() & 0xFFFF);
  }

  int result;
  if (_currentValue == _random)
    result = _currentValue;
  else if (_currentValue < _random)
    result = (_currentValue + step < _random) ? _currentValue + step : _random;
  else
    result = (_currentValue - step > _random) ? _currentValue - step : _random;

  return result;
}


// ---------------------------------------------------------------------------
// The JV's LFO, from the firmware (scdb devices/jv880/07_synthesis/lfo.md,
// FW-EXACT except where marked; D-37). RTOS task 13 at ROM1 0x0EEE runs every
// 16 ms and, per voice and per LFO: adds LFO_RATE[rate] (ROM2 0x4C58) to a
// 16-bit phase, doubled for the two random forms; takes the waveform byte at
// phase >> 8 from one of four 256-byte tables (ROM2 0x4D60..), or a held /
// interpolated random byte; adds LFO_OFFSET[offset] (ROM2 0x4C52) as a byte;
// then runs the delay and fade coroutine, whose stages use the envelopes' time
// law (2^20 / LUT_5160[param] per tick, so a stage lasts its milliseconds) and
// whose fade ramps the sample by (accum >> 8). The result leaves two words per
// voice: the faded one (@0x9372/@0x93AA) the tone's own depths multiply, and
// the raw sample << 8 (@0x93E2/@0x941A) the controller matrix multiplies.
//
// Two things the ROM cannot give and are stated as deviations:
//  - Synchro OFF means the slot's phase is simply never reset, so a note takes
//    whatever phase the slot had. That history is not reproducible here; a
//    deterministic per-voice pseudo-random start phase stands in for it.
//  - The random forms draw from a sound-chip register (ROM1 0x17E1, undecoded);
//    an xorshift seeded per voice gives the character, not the sequence. Their
//    amplitude is taken as the waveform tables' +/-64.
// ---------------------------------------------------------------------------

WaveGenerator::WaveGenerator(struct ControlRom::InstPartial &ip,
                             struct ControlRom::LookupTables &LUT,
                             Settings *settings, int partId, int jvLfoIndex)
  : _id(jvLfoIndex ? 1 : 0),
    _waveform(Waveform::Sine),
    _LUT(LUT),
    _instRate(0),
    _rateChange(0),
    _delay(0xffff),
    _delayIncLUT(0),
    _fade(0xffff),                      // the Sound Canvas depth paths read
    _fadeIncLUT(0),                     // this; on a JV they multiply zeros
    _currentValue(0),
    _currentValueNorm(0),
    _accRate(0),
    _random(0),
    _randomFirstRun(true),
    _settings(settings),
    _partId(partId)
{
  static unsigned int seedCounter = 0;

  const int l = jvLfoIndex ? 1 : 0;
  _jv = true;
  _jvForm = ip.JVLfoForm[l] & 7;
  _jvOffsetIdx = ip.JVLfoOffset[l] & 7;
  _jvSync = ip.JVLfoSync[l];
  _jvFadeOut = ip.JVLfoFadeOut[l];
  _jvRate = ip.JVLfoRate[l] & 0x7f;
  _jvDelayKeyOff = ip.JVLfoDelayKeyOff[l];

  _jvRng = 0x9E3779B9u * ++seedCounter + 0x7F4A7C15u;
  // Synchro ON: the note-on request zeroes the phase at the next task-13 wake.
  _jvPhase = _jvSync ? 0 : (uint16_t) (_jvRng >> 8);

  // Delay and fade times in milliseconds from the shared time table; a byte of
  // 0 is table entry 0, and the stage completes on its first tick.
  const int dms = _LUT.envelopeTime[ip.JVLfoDelay[l] & 0x7f];
  const int fms = _LUT.envelopeTime[ip.JVLfoFade[l] & 0x7f];
  _jvDelayInc = dms > 0 ? std::max(1, (1 << 20) / dms) : 0x10000;
  _jvFadeInc  = fms > 0 ? std::max(1, (1 << 20) / fms) : 0x10000;
  _jvStage = 0;
  _jvStageAcc = 0;
}


void WaveGenerator::note_off(void)
{
  _jvKeyOff = true;
}


int WaveGenerator::_jv_draw(void)
{
  _jvRng ^= _jvRng << 13; _jvRng ^= _jvRng >> 17; _jvRng ^= _jvRng << 5;
  return (int) ((_jvRng >> 9) & 0x7f) - 64;
}


void WaveGenerator::_jv_update(void)
{
  if (++_jvTick & 1)                    // 16 ms task on an 8 ms control period
    return;

  int inc = _LUT.JVLfoRate[std::clamp(_jvRate, 0, 127)];
  if (_jvForm >= 4)
    inc <<= 1;

  int sample;
  if (_jvForm < 4) {
    _jvPhase = (uint16_t) (_jvPhase + inc);
    sample = _LUT.JVLfoWaves[_jvForm * 256 + (_jvPhase >> 8)];

  } else if (_jvForm == 4) {            // RND1: sample and hold on phase wrap
    const uint32_t sum = (uint32_t) _jvPhase + (uint32_t) inc;
    if (sum >> 16)
      _jvRnd = _jv_draw();
    _jvPhase = (uint16_t) sum;
    sample = _jvRnd;

  } else {                              // RND2: interpolate to the next draw
    const uint32_t sum = (uint32_t) _jvPhase + (uint32_t) inc;
    if (sum >> 16) {
      _jvRndPrev = (int8_t) (_jvRndPrev + _jvRndDelta);
      _jvRndDelta = (int8_t) (_jv_draw() - _jvRndPrev);
    }
    _jvPhase = (uint16_t) sum;
    sample = (int8_t) (_jvRndPrev + ((_jvRndDelta * (_jvPhase >> 8)) >> 8));
  }

  // The offset is a byte add and wraps as the firmware's does (ROM1 0x1309).
  sample = (int8_t) (sample + _LUT.JVLfoOffset[_jvOffsetIdx]);
  _jvRaw = sample << 8;

  // Delay, then fade, then steady. Fade IN: silent through the delay, ramping
  // up; fade OUT (manual): full through the delay, ramping down to nothing.
  // A stage whose time is 0 is left in the same tick it is entered.
  int word = 0;
  for (int guard = 0; guard < 3; guard++) {
    if (_jvStage == 0) {
      bool done;
      if (_jvDelayKeyOff) {
        done = _jvKeyOff;
      } else {
        _jvStageAcc += _jvDelayInc;
        done = _jvStageAcc >= 0x10000;
      }
      word = _jvFadeOut ? (sample << 8) : 0;
      if (!done)
        break;
      _jvStage = 1;
      _jvStageAcc = 0;
      if (_jvFadeInc < 0x10000)
        break;
      continue;
    }
    if (_jvStage == 1) {
      _jvStageAcc += _jvFadeInc;
      if (_jvStageAcc >= 0x10000) {
        _jvStage = 2;
        continue;
      }
      int level = (_jvStageAcc >> 8) & 0xff;          // ROM1 0x161E-0x1628
      if (_jvFadeOut)
        level = 255 - level;
      word = sample * level;
      break;
    }
    word = _jvFadeOut ? 0 : (sample << 8);
    break;
  }

  _currentValue = word;
  _currentValueNorm = word;
}

}
