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
#include <cstdlib>


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
    _delayFeedback(-1),
    _jvRecords(settings->device()->reverb.network ==
               ReverbNetworkKind::JVRecordProgram)
{
  _rBuffer.fill(0.0f);

  _jvTapBase = 0;
  _jvTimeScale = 0;
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
  // c4's high byte, the input gain, is a SIGNED coefficient: the JV-880's
  // records carry 0xE9 / 0xE1 (-23, -31) in firmware 1.0.1 where 1.0.0 had
  // +23 / +31, and the reference's wet flips polarity against the dry with
  // it (scdb devices/jv880/notes/track_reverb_program_2026-09-05.md). The
  // Sound Canvas blocks above hold 0x08 / 0x10, for which signed and unsigned
  // agree, so this changes nothing there.
  float x = _preLpfState * sByte(_activeCharRegs.c4, true);

  const float dLo   = uByte(_activeCharRegs.c4, false);
  const float d4Lo  = uByte(_activeCharRegs.c5, false);
  const bool  dEn   = (_activeCharRegs.c4 & 0x30) != 0;
  const bool  d4En  = (_activeCharRegs.c5 & 0x30) != 0;
  const float aTank = sByte(_activeCharRegs.c6, true);
  // c6's low byte is signed too: the JV's STAGE2 and both HALLs carry 0xE5 /
  // 0xE1, i.e. an allpass pair (+g, -g) with the signs the other way round
  // from the Sound Canvas's 0xE020 = (-32, +32). Read unsigned it would be
  // 3.5 and the tank could not be stable; the Sound Canvas value 0x20 reads
  // the same either way.
  const float bTank = sByte(_activeCharRegs.c6, false);

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

  const float fade = _fade_step();

  output[0] = wetL * _outGain * fade;
  output[1] = wetR * _outGain * fade;
}


// One step of the character-change fade-out. Extracted unchanged from
// process_sample; the arithmetic and its order are the same, which the Sound
// Canvas corpus check verifies.
float Reverb::_fade_step(void)
{
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
  return fade;
}


// THE JV-880's REVERB IS THIS PROGRAM WITH THE JV's OWN NUMBERS.
//
// The reverb above is the TC6116AF's own program, and it is the same program on
// the JV-880. The JV's firmware streams each reverb type's 30-word ROM record
// into exactly the registers the Sound Canvas character blocks in reverb.h
// fill: words 0-11 to slot 0x1C F010-F036 (p28[0..11]), words 12-20 to slot
// 0x1D F010-F030 (p29[0..8]), words 22-26 to slot 0x1E F018/F01A/F01C/F01E/
// F030 (c4..c8) and word 21 to F012, the pre-LPF pair (scdb devices/jv880/
// 08_effects/dsp_program.md, the streamed copy at ROM2 0x703C-0x70DF). Three
// of its six records are byte-identical in words 0-20 to blocks in reverb.h -
// ROOM2 = _crPlate, HALL2 = _crRoom2, STAGE2 = _crHall2 - and its coefficient
// words have the same shape: c5 with an empty high byte, c6 an allpass pair,
// c7 and c8 a (pole, signed input) damping pair each.
//
// Until 2026-09-05 this file ran a different network for the JV, "one line,
// nine stereo tap pairs, the loop closed from a tenth" (scdb 08_effects/
// reverb.md "THE NETWORK", since retracted). That reading measured the two
// DELAY types correctly - they are the one case where this program collapses
// to a single tap - and every reverb type wrong in kind. Measured against the
// reference with the SAME firmware revision on both sides (scdb notes/
// track_reverb_program_2026-09-05.md, all on the drum stimuli in
// ~/jv880-listening/taps and revprog):
//
//  * the first wet arrival, predicted from pointer differences alone and
//    different per type AND channel - ROOM2 4.7 / 4.3 ms, STAGE1 4.7 / 6.7,
//    STAGE2 17.4 / 56.3, HALL1 1.2 / 14.4, HALL2 0.6 / 0.6 - lands within
//    1-2 ms of the reference on every one. The old network could put nothing
//    before its first tap at 16.6 ms, which is why its onset measurements
//    "failed their control": the control was right;
//  * that arrival's sign and level: HALL2 wet/dry -0.0258 on the reference,
//    -0.0271 here; STAGE2 +0.0106 / +0.0112; HALL1 +0.0063 / +0.0065 (ROM2
//    1.0.1; the mirror image on 1.0.0, both engines);
//  * the loop MODE positions of the tail: all six types correlate with the
//    reference at zero frequency shift (r = 0.68-0.81). The old network:
//    r = 0.05 at any shift;
//  * a wood-block tail, tail_fine.py: envelope steps over 6 dB 160/314 -> 0,
//    1/24-octave bins over 6 dB 27 -> 2, worst bin +19.1 -> -9.6 dB.
//
// The evidence classes: the records and the registers they land in are the
// firmware's (FW-EXACT, scdb dsp_program.md); that the chip runs this program
// on them is MEASURED on the reference emulator, above; the program itself is
// the Sound Canvas's, whose provenance is in this file's header.
//
// So this function only LOADS. Nothing here is a constant of the JV, and the
// two byte readings it relies on (c4 high and c6 low signed) are in
// process_sample with their reasons.
void Reverb::_set_jv_character(int character)
{
  constexpr int nw = ControlRom::LookupTables::JVReverbRecordWords;

  std::fill(_rBuffer.begin(), _rBuffer.end(), 0.0f);
  _dampA = _dampB = 0.0f;
  _preLpfState = 0.0f;

  if (character < 0 || character > 7 ||
      (size_t) (nw * (character + 1)) > _LUT.JVReverbRecord.size())
    return;

  const int *rec = _LUT.JVReverbRecord.data() + nw * character;
  if (rec[20] == 0)                     // no record read: leave the line silent
    return;

  for (int i = 0; i < 12; i++)
    _activeCharRegs.p28[i] = (uint16_t) rec[i];
  for (int i = 0; i < 9; i++)
    _activeCharRegs.p29[i] = (uint16_t) rec[12 + i];
  _activeCharRegs.c4 = (uint16_t) rec[22];
  _activeCharRegs.c5 = (uint16_t) rec[23];
  _activeCharRegs.c6 = (uint16_t) rec[24];
  _activeCharRegs.c7 = (uint16_t) rec[25];
  _activeCharRegs.c8 = (uint16_t) rec[26];

  // Word 21 is the pre-LPF pair, (pole, input) / 64 summing to 64 in every
  // record, so its DC gain is exactly 1 (scdb reverb.md "The pre-LPF"). Which
  // byte is which comes from the SC-55 mk1, which drives the same register from
  // its GS Reverb Pre-LPF parameter: "off" is 0x003F there, so the low byte is
  // the input. The JV has no such parameter and bakes a pair per type.
  _preLpfA = (rec[21] >> 8)   / 64.0f;
  _preLpfB = (rec[21] & 0xff) / 64.0f;

  // The two bytes the parameter path reads out of the record: +0x36, the Time
  // scale the firmware multiplies Reverb Time by (ROM2 0x71CD, mulxu.b), and
  // +0x1A + 1, the base the delay arm adds its computed taps to.
  _jvTimeScale = rec[27] >> 8;
  _jvTapBase   = (uint16_t) (rec[13] + 1);
}


void Reverb::_set_character(int character)
{
  _character = character;

  if (_jvRecords) {
    _set_jv_character(character);
    return;
  }

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

  // The JV-880 takes its registers from the selected type's record, so its
  // Time parameter reaches this program through two firmware paths of its own
  // (ROM2 0x71C3-0x72D1, scdb dsp_program.md), neither of which is the Sound
  // Canvas line below.
  if (_jvRecords) {
    // The firmware's expansion of a 0-127 parameter to 0-255: 2v below 64 and
    // 2v + 1 from 64 up, the carry of a byte-wide shift rather than rounding.
    const int ex = 2 * rt + (rt >= 64 ? 1 : 0);

    if (_character >= 0 && _character <= 5) {
      // Slot 0x1E F010's high byte is (expand(Time) * record[+0x36]) >> 8,
      // traced to the instruction (mulxu.b at ROM2 0x71CD). It is the same
      // register the Sound Canvas drives from ITS time byte and the delay arm
      // drives from Delay Feedback, and this program reads that register as
      // byte / 128 in both other cases (lut / 2 / 64 below; the delay arm's
      // (Feedback - 2) / 128, measured on the reference to 0.05 dB rms). So
      // byte / 128 here too, with no constant of its own.
      //
      // Checked, not fitted: over 6 types x 7 Times the tail's broadband level
      // after a note's release tracks the reference within a few dB in most
      // cells; ROOM1 and HALL1 decay somewhat slower than the reference at
      // long Times and HALL2 somewhat faster (scdb notes/
      // track_reverb_program_2026-09-05.md has the table). That residual is
      // reported there and NOT absorbed into a multiplier here.
      _gLoop = (float) ((ex * _jvTimeScale) >> 8) / 128.0f;

    } else if (_character == 6 || _character == 7) {
      // The delay arm, ROM2 0x7283/0x72A0: two tap addresses A and B out of the
      // record's own scale words, written into the program's pointer registers
      // exactly where the firmware puts them:
      //
      //   A -> w6, w10, w14, w18        p28[6], p28[10], p29[2], p29[6]
      //   B -> w7, w11, w15, w19, w20   p28[7], p28[11], p29[3], p29[7], p29[8]
      //   B, B+1 -> w16, w17            p29[4], p29[5]
      //
      // In this program w6/w10/w14/w18 are the four LEFT output taps and
      // w7/w11/w15/w19 the four RIGHT ones, w20 is tank B's recirculation read
      // and w16/w17 are its second allpass, which the delay records disable
      // (c6 = 0). So the delay types come out of the same program as a single
      // echo at A left and B right, recirculating at B - which is what the
      // reference measures: DELAY at 258 ms both sides, PAN-DLY at 134 / 258 ms
      // at Time 64, levels within 0.5 dB.
      //
      //     E = 2 * time + (time >= 64)
      //     M = (E << 8) | (E >= 128 ? 0xFF : 0)      exts.b then swap.b
      //     tap = base + ((M * scale) >> 16)
      //
      // M's low byte is the firmware's sign extension of E's bit 7, worth about
      // 61 samples from Time 64 up; the reference's echo steps by exactly that
      // there (scdb reverb.md "The effect sample rate is exactly 32 000 Hz").
      const int M = (ex << 8) | (ex >= 128 ? 0xff : 0);
      const int sA = _LUT.JVReverbTapScale[2 * _character];
      const int sB = _LUT.JVReverbTapScale[2 * _character + 1];
      const uint16_t A = (uint16_t) (_jvTapBase + ((M * sA) >> 16));
      const uint16_t B = (uint16_t) (_jvTapBase + ((M * sB) >> 16));

      _activeCharRegs.p28[6] = _activeCharRegs.p28[10] = A;
      _activeCharRegs.p29[2] = _activeCharRegs.p29[6]  = A;
      _activeCharRegs.p28[7] = _activeCharRegs.p28[11] = B;
      _activeCharRegs.p29[3] = _activeCharRegs.p29[7]  = B;
      _activeCharRegs.p29[8] = B;
      _activeCharRegs.p29[4] = B;
      _activeCharRegs.p29[5] = (uint16_t) (B + 1);
    }
    return;
  }

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

  // The JV-880 has NO Pre-LPF parameter - the manual's reverb page lists Type,
  // Level, Time and Feedback and nothing else - and its network's pre-LPF is a
  // fixed pair per type, baked into the record and loaded with it. Letting a
  // GS parameter this device does not have reach the filter would overwrite
  // that pair with a Sound Canvas value on the first update().
  if (_jvRecords)
    return;

  const ReverbLaw &law = _settings->device()->reverb;

  // PreLPF runs 0-7 but the device caps it; the pair is complementary.
  int k = std::clamp(preLPF, 0, law.preLpfMaxLevel);

  _preLpfA = (law.preLpfStep * k) / 64.0f;
  _preLpfB = (law.preLpfPairSum - law.preLpfStep * k) / 64.0f;
}


void Reverb::_set_delay_feedback(int delayFeedback)
{
  _delayFeedback = delayFeedback;

  if (_character != 6 && _character != 7)
    return;

  // The JV writes Delay Feedback into slot 0x1E F010 (ROM2 0x72C5-0x72D1), the
  // register the reverb arm drives from Reverb Time: on the two delay types
  // that register is tank B's recirculation gain on a single echo, which is
  // why the manual says Feedback works only on them.
  //
  // MEASURED on the reference, and it is the tightest law in this device: a
  // 150 ms note through DELAY at Time 81 puts one echo every 312.5 ms, each g
  // times the last, so the ratio of successive echo peaks is the per-pass gain
  // with no fitting at all. Nine feedback values on both delay types:
  //
  //     fb        16     32     48     64     80     96    112    127
  //     g       .1098  .2336  .3627  .4875  .6117  .7345  .8590  .9840
  //     g*128   14.05  29.90  46.42  62.40  78.29  94.02 109.95 125.95
  //
  // so g = (fb - 2) / 128, residual RMS 0.046 dB and max 0.08 dB over a 19 dB
  // span. That is this program's own reading of the register - byte over 128,
  // as the Sound Canvas path below and the reverb arm above - with a 2-count
  // offset the firmware's (feedback << 8) | 0xB0 does not show and which is
  // kept as measured.
  if (_jvRecords) {
    _gLoop = std::max(std::clamp(delayFeedback, 0, 127) - 2, 0) / 128.0f;
    return;
  }

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
