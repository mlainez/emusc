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


#include "reverb.h"
#include "control_rom.h"

#include <algorithm>
#include <cmath>


namespace EmuSC {


Reverb::Reverb(Settings *settings, const struct ControlRom::LookupTables &LUT)
  : _settings(settings),
    _LUT(LUT),
    _sweepIndex(0),
    _preLpfState(0.0f),
    _preLpfA(0.0f),
    _preLpfB(1.0f),
    _dampA(0.0f),
    _dampB(0.0f),
    _gLoop(0.0f),
    _outGain(0.0f),
    _character(-1),
    _pendingCharacter(-1),
    _fadeRemaining(0),
    _silenceRemaining(0),
    _preLPF(-1),
    _reverbTime(-1),
    _delayFeedback(-1)
{
  _rBuffer.fill(0.0f);
}


void Reverb::update(void)
{
  // A change of character starts a fade-out; the switch itself happens when
  // the fade finishes. Measured on the SC-55mkII by writing a new character
  // under a sounding tail: the wet falls from 0 to -27 dB over about 165 ms
  // and then sits at the measurement floor, and the render stops depending on
  // WHICH new character was chosen until 470 ms after the write - two
  // different new characters are sample-identical until then. A linear
  // amplitude ramp over 165 ms predicts -3.9, -11.3 and silence at 60, 120
  // and 180 ms where the machine gives -4.4, -10.9 and -26.7.
  //
  // Writing the character it already has does nothing audible, so the fade is
  // only armed on an actual change.
  int character = _settings->get_param(PatchParam::ReverbCharacter);
  if (character != _character && character != _pendingCharacter) {
    if (_character < 0) {                 // first time: no tail to fade
      _set_character(character);
    } else {
      _pendingCharacter = character;
      _fadeRemaining = _fadeOutSamples;
    }
    _reverbTime = -1;
    _delayFeedback = -1;
  }

  int preLPF = _settings->get_param(PatchParam::ReverbPreLPF);
  if (preLPF != _preLPF)
    _set_pre_lpf(preLPF);

  int reverbTime = _settings->get_param(PatchParam::ReverbTime);
  if (reverbTime != _reverbTime)
    _set_reverb_time(reverbTime);

  int delayFeedback = _settings->get_param(PatchParam::ReverbDelayFeedback);
  if (delayFeedback != _delayFeedback)
    _set_delay_feedback(delayFeedback);

  _set_level(_settings->get_param(PatchParam::ReverbLevel));
}


// Reverb algorithm based on information from the Nuked-SC55 project by nukeykt
void Reverb::process_sample(float input, float output[2])
{
  if (_character < 0 || _character > 7) {
    output[0] = output[1] = 0;
    return;
  }

  // The silent window between the old character and the new one. Nothing is
  // processed, so the new character's buffer is still empty when it starts and
  // fills from nothing - which is what makes the new reverb appear gradually
  // rather than at full level.
  if (_silenceRemaining > 0) {
    if (--_silenceRemaining == 0) {
      int c = _pendingCharacter;
      _pendingCharacter = -1;
      _set_character(c);               // selects the registers and clears state
      _reverbTime = -1;
      _delayFeedback = -1;
      update();                        // reverb time and level for the new one
    }
    output[0] = output[1] = 0;
    return;
  }

  auto read = [&](uint16_t base) -> float {
    return _rBuffer[(base + _sweepIndex) & rBufferMask];
  };

  auto write = [&](uint16_t base, float v) {
    _rBuffer[(base + _sweepIndex) & rBufferMask] = v;
  };

  _preLpfState = _preLpfA * _preLpfState + _preLpfB * input;
  float x = _preLpfState * uByte(_activeCharRegs.c4, true);

  const float dLo   = uByte(_activeCharRegs.c4, false);
  const float d4Lo  = uByte(_activeCharRegs.c5, false);
  const bool  dEn   = (_activeCharRegs.c4 & 0x30) != 0;
  const bool  d4En  = (_activeCharRegs.c5 & 0x30) != 0;
  const float aTank = sByte(_activeCharRegs.c6, true);
  const float bTank = uByte(_activeCharRegs.c6, false);

  float D1 = read(_activeCharRegs.p28[1]);
  float n1 = dEn ? (x - 0.5f * D1) : x;
  float o1 = dLo * n1 + D1;

  float D2 = read(_activeCharRegs.p28[2]);
  float n2 = dEn ? (o1 - 0.5f * D2) : o1;
  float o2 = dLo * n2 + D2;

  float D3 = read(_activeCharRegs.p28[3]);
  float n3 = dEn ? (o2 - 0.5f * D3) : o2;
  float o3 = dLo * n3 + D3;

  float dA1 = read(_activeCharRegs.p28[5]);

  float D4 = read(_activeCharRegs.p28[4]);
  float n4 = d4En ? (o3 - 0.5f * D4) : o3;
  float o4 = d4Lo * n4 + D4;

  float dB1 = read(_activeCharRegs.p29[1]);
  float fbA = read(_activeCharRegs.p29[0]);
  write(_activeCharRegs.p28[0], n1);
  float fbB = read(_activeCharRegs.p29[8]);
  write(_activeCharRegs.p28[1], n2);
  write(_activeCharRegs.p28[2], n3);
  write(_activeCharRegs.p28[3], n4);

  _dampA = uByte(_activeCharRegs.c7, true) * _dampA +
           sByte(_activeCharRegs.c7, false) * fbA;
  _dampB = uByte(_activeCharRegs.c8, true) * _dampB +
           sByte(_activeCharRegs.c8, false) * fbB;

  float inA = o4 + _gLoop * _dampA;
  float vA1 = inA + aTank * dA1;
  float mA1 = dA1 + bTank * vA1;

  float dA2  = read(_activeCharRegs.p28[9]);
  float dB2  = read(_activeCharRegs.p29[5]);
  float inA2 = read(_activeCharRegs.p28[8]);
  write(_activeCharRegs.p28[4], vA1);
  float inB2 = read(_activeCharRegs.p29[4]);
  write(_activeCharRegs.p28[5], mA1);

  float inB = o4 + _gLoop * _dampB;
  float vB1 = inB + aTank * dB1;
  float mB1 = dB1 + bTank * vB1;
  write(_activeCharRegs.p29[0], vB1);

  float wetL = read(_activeCharRegs.p28[6]) + read(_activeCharRegs.p28[10]) +
               read(_activeCharRegs.p29[2]) + read(_activeCharRegs.p29[6]);
  float wetR = read(_activeCharRegs.p28[7]) + read(_activeCharRegs.p28[11]) +
               read(_activeCharRegs.p29[3]) + read(_activeCharRegs.p29[7]);

  float vA2 = inA2 + aTank * dA2;
  float mA2 = dA2 + bTank * vA2;
  float vB2 = inB2 + aTank * dB2;
  float mB2 = dB2 + bTank * vB2;
  write(_activeCharRegs.p29[1], mB1);
  write(_activeCharRegs.p28[8], vA2);
  write(_activeCharRegs.p28[9], mA2);
  write(_activeCharRegs.p29[4], vB2);
  write(_activeCharRegs.p29[5], mB2);

  _sweepIndex = (_sweepIndex - 1) & rBufferMask;

  float fade = 1.0f;
  if (_fadeRemaining > 0) {
    fade = (float) _fadeRemaining / (float) _fadeOutSamples;
    if (--_fadeRemaining == 0) {
      _silenceRemaining = _silenceSamples;
      std::fill(_rBuffer.begin(), _rBuffer.end(), 0.0f);
      _dampA = _dampB = 0.0f;
      _preLpfState = 0.0f;
    }
  }

  output[0] = wetL * _outGain * fade;
  output[1] = wetR * _outGain * fade;
}


void Reverb::_set_character(int character)
{
  _character = character;

  // TODO: Firmware fades out before starting with the new character.
  //       We simply reset the ring buffer and switch character instantly, but
  //       must add the following:
  //        - Fade-out (measured to ~165 ms)
  //        - Silent reset time (measured to ~280 ms)

  // Room1-3, Hall1-2, Plate
  if (character >= 0 && character < 6) {
    _activeCharRegs = *_charRegs[character];

    std::fill(_rBuffer.begin(), _rBuffer.end(), 0.0f);
    _dampA = _dampB = 0.0f;
    _preLpfState = 0.0f;

  // Delay, Panning Delay
  } else if (character == 6 || character == 7) {
    _activeCharRegs = _crDelayBase;
    std::fill(_rBuffer.begin(), _rBuffer.end(), 0.0f);
    _dampA = _dampB = 0.0f;
    _preLpfState = 0.0f;
  }
}


void Reverb::_set_reverb_time(int reverbTime)
{
  _reverbTime = reverbTime;

  const ReverbLaw &law = _settings->device()->reverb;

  int rt = std::clamp(reverbTime, 0, 127);
  if (_character >= 0 && _character <= 5) {
    // Reverb time is a rounded straight line into the loop gain, saturating.
    // What this used to index was a table, but the table is such a line, and
    // taking it literally made our T60 10.5 to 11.8 % LONG across a 4.7x range
    // of reverb time on the mkII (P-0296, P-0300, P-0303). The line was then
    // inverted from our own measured T60(gLoop) curve against each machine's
    // T60, which is why it is per-device data: see ReverbLaw.
    int lut = std::min((int) lroundf(law.timeSlope * rt + law.timeOffset),
                       law.timeCap);
    _gLoop = std::max(lut, 0) / 2 / 64.0f;

  } else if (_character == 6 || _character == 7) {
    // The delay-line taps. On the Sound Canvas this is a straight line in the
    // parameter, measured on the machine, with the panning type's left tap at
    // half the spacing.
    //
    // The JV-880 forms it from the selected type's own record instead
    // (ROM2 0x47255-0x4727B, P-0397). The two scale words ARE the per-tap
    // spacing, so PAN-DLY's 2:1 ratio comes out of the data rather than from a
    // halving here: 0x3CF0/0x3CF0 for DELAY and 0x1E7D/0x3CF0 for PAN-DLY.
    //
    //     E = 2 * time + (time >= 64)
    //     M = (E << 8) | (E >= 128 ? 0xFF : 0)      exts.b then swap.b
    //     tap = base + ((M * scale) >> 16)
    //
    // M's low byte is the firmware's sign extension of E's bit 7, and it is
    // worth about 61 samples from time 64 up - not a rounding term. The
    // reference's echo steps by exactly that at exactly that point, which is
    // how the term was found: measured against the oracle at nine Delay Times
    // on both delay types, every point lands on this arithmetic to the sample.
    //
    // `base` is this program's own register layout, not a delay time - see
    // ReverbLaw. Ours is 0x16 where the device's records carry 10, because the
    // JV's delay program has ten write pointers and reverb.cc's has twenty-two.
    // The residue is a constant 10 samples, 0.31 ms, at every Time; it belongs
    // to the delay PROGRAM (which also scales two registers with Time that this
    // one does not) and is recorded rather than cancelled with a fitted offset.
    uint16_t tapL, tapR;
    if (law.delayTapLaw == ReverbDelayTapLaw::JVRecordScale &&
        2 * _character + 1 < (int) _LUT.JVReverbTapScale.size()) {
      const int E = 2 * rt + (rt >= 64 ? 1 : 0);
      const int M = (E << 8) | (E >= 128 ? 0xff : 0);
      const int sA = _LUT.JVReverbTapScale[2 * _character];
      const int sB = _LUT.JVReverbTapScale[2 * _character + 1];
      tapL = (uint16_t)(law.delayTapBase + ((M * sA) >> 16));
      tapR = (uint16_t)(law.delayTapBase + ((M * sB) >> 16));
    } else {
      tapR = (uint16_t)(law.delayTapBase + law.delayTapPerTime * rt);
      tapL = (_character == 7)
        ? (uint16_t)(law.delayTapBase + law.delayTapPerTime / 2 * rt) : tapR;
    }
    _activeCharRegs.p28[6] = tapL;  _activeCharRegs.p28[10] = tapL;   // wet L
    _activeCharRegs.p29[2] = tapL;  _activeCharRegs.p29[6]  = tapL;
    _activeCharRegs.p28[7] = tapR;  _activeCharRegs.p28[11] = tapR;   // wet R
    _activeCharRegs.p29[3] = tapR;  _activeCharRegs.p29[7]  = tapR;
    _activeCharRegs.p29[8] = tapR;                 // Feedback tap (damp B)
  }
}


void Reverb::_set_pre_lpf(int preLPF)
{
  _preLPF = preLPF;

  const ReverbLaw &law = _settings->device()->reverb;

  // PreLPF runs 0-7 but the device caps it; the pair is complementary.
  int k = std::clamp(preLPF, 0, law.preLpfMaxLevel);

  _preLpfA = (law.preLpfStep * k) / 64.0f;
  _preLpfB = (law.preLpfPairSum - law.preLpfStep * k) / 64.0f;
}


void Reverb::_set_delay_feedback(int delayFeedback)
{
  _delayFeedback = delayFeedback;

  if (_character == 6 || _character == 7)
    _gLoop = fbToTarget(delayFeedback) / 128.0f;
}


// The wet RETURN gain, from the Reverb Level parameter.
//
// The Sound Canvas path is level / levelDivisor, unchanged. The JV-880's own
// firmware (ROM2 0x71EC-0x7227, P-0395) instead scales the level by a
// coefficient belonging to the reverb TYPE and lands the result in the same
// 0..63 field, 64 = unity:
//
//     expand(v) = 2 * v + (v >= 64)          0..127 -> 0..255
//     target    = (expand(Level) * rec[+0x38]) >> 8      types 0-5
//     target    = Level >> 1                             types 6-7
//
// so at Level 127 ROOM1 returns 30/64 and the two delay types return 63/64 -
// the delays are about 6.4 dB louder than the reverbs at the same setting, and
// the level byte taken as a gain is 12.5 dB hot on the reverbs.
//
// Note the expansion is 2v below 64 and 2v + 1 from 64 up, not 2v + 1
// throughout: the firmware adds the carry of a byte-wide shift.
//
// NOT IMPLEMENTED, and named rather than approximated: the firmware ramps the
// register to this target one LSB at a time with a delay between steps, so a
// Level change GLIDES over some tens of milliseconds. Nothing in this engine
// steps a gain per control period, the ramp does not fall out of anything here,
// and every measurement in P-0395 is of a level set before the first note.
void Reverb::_set_level(int level)
{
  const ReverbLaw &law = _settings->device()->reverb;
  const int lvl = std::clamp(level, 0, 127);

  if (law.returnLaw == ReverbReturnLaw::JVTypeCoefficient &&
      _character >= 0 && _character < (int) _LUT.JVReverbReturnCoeff.size()) {
    const int expand = 2 * lvl + (lvl >= 64 ? 1 : 0);
    const int target = (_character <= 5)
      ? ((expand * _LUT.JVReverbReturnCoeff[_character]) >> 8)
      : (lvl >> 1);
    _outGain = target / law.levelDivisor;
    return;
  }

  _outGain = lvl / law.levelDivisor;
}


}  // namespace EmuSC
