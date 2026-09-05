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
    _jvNetwork(settings->device()->reverb.network ==
               ReverbNetworkKind::JVMultiTapLine)
{
  _rBuffer.fill(0.0f);

  for (int k = 0; k < _jvTaps; k++) {
    _jvTapL[k] = _jvTapR[k] = 0;
    _jvGain[k] = 0.0f;
    _jvActive[k] = false;
  }
  _jvInGain = 0.0f;
  _jvLoopTap = _jvPreDelay = _jvTapBase = 0;
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

  // The JV-880 runs a different NETWORK, not this program with other numbers.
  if (_jvNetwork) {
    _process_sample_jv(input, output);
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

  const float fade = _fade_step();

  output[0] = wetL * _outGain * fade;
  output[1] = wetR * _outGain * fade;
}


// One step of the character-change fade-out, shared by both networks. Extracted
// unchanged from process_sample when the JV network was added; the arithmetic
// and its order are the same, which the Sound Canvas corpus check verifies.
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


// THE JV-880's REVERB NETWORK - one mono recirculating delay line, nine stereo
// tap pairs, the loop closed from a tenth tap (scdb devices/jv880/08_effects/
// reverb.md "THE NETWORK", P-0399):
//
//   send -> [pre-LPF w21] -> x (w22 hi)/64 -> (+) -> line ---------------.
//                                             ^                          |
//                                             '------ x g_fb ----- tap w20
//
//   L = SUM(k=1..9) c_k * line[w(2k)]      R = SUM(k=1..9) c_k * line[w(2k+1)]
//
// and then the return gain, which _set_level already forms from the type's own
// coefficient. Every number is the selected type's ROM record.
//
// Three things this is NOT, each of which the recovered network rules out: a
// comb bank, an allpass diffuser, and a nested tank. The +-g byte pairs that
// look like allpass coefficients are two consecutive TAP gains of equal
// magnitude and opposite polarity, and the polarity alternates from pair to
// pair in every record - that is what decorrelates the reflections here.
//
// Two things it does not settle, neither papered over:
//
//  * TAP PAIR P2's GAIN IS UNKNOWN. Word 23's high byte is 0x00 in all sixteen
//    records across both devices, so P2 = (w4, w5) is muted in every shipped
//    preset and no ROM read can ever evidence a value for it. It is treated as
//    muted here because that is what the data says, not because a value was
//    chosen. Only a register-level write on hardware can settle it.
//  * The record's pre-delay, word 1 (300/323/695/926/917/917 samples for the
//    six reverbs, 1 for both delay types, ordered room < stage < hall), has no
//    place in the recovered signal flow: it carries no gain byte and the nine
//    tap delays are absolute, measured to the sample on the two delay types.
//    It is read and kept in _jvPreDelay so it is visible rather than dropped,
//    and it is NOT inserted into the path - inventing a position for it would
//    move every tap by 9.4 to 29 ms on a guess.
//
//    THE MEASUREMENT BELOW IS NOT TRUSTWORTHY - read this first. It isolates
//    the wet by subtracting a reverb-closed render from a reverb-open one, and
//    that method fails its own control. Repeated across all six reverb types
//    with a MATCHED dry render per type, so the character-change transient
//    cancels, it reports the wet beginning at 0 to 5 ms on types 1, 2, 4 and 5
//    - before the earliest tap in any record, which is 16.6 ms. A reverb cannot
//    output before its first tap. Types 0 and 3 report 18-19 ms and are the
//    only self-consistent pair.
//
//    So the "22 ms late" figure below rests on a method that produces
//    impossible numbers on four types out of six, and it should be treated as
//    unproven rather than as a finding. What it would take to measure properly:
//    a stimulus that drives the reverb without a dry path at all, or a
//    register-level capture. Everything downstream of that number - the tap
//    scale hypothesis and its refutation both - inherits the doubt.
//
//    MEASURED 2026-09-05, and it says the taps are in the wrong place. Feeding
//    one short drum hit through HALL1 at Reverb Time 64 and taking the wet as
//    the difference between a reverb-open and reverb-closed render, the
//    reference's wet begins at 18 ms and peaks at 228 ms. Ours begins at 40 ms,
//    which is tap P1 exactly where the record puts it (1307 samples = 40.8 ms),
//    and peaks at 310. So the reference produces output 22 ms BEFORE our first
//    tap and before the record's own 926-sample pre-delay.
//
//    Subtracting an offset from every tap was swept: 700 samples puts the onset
//    at 18 ms, matching to the resolution of the measurement, and takes the
//    envelope error's standard deviation from 15.6 to 5.8 dB. But 700 is not
//    the pre-delay and nothing in the record is 700, and NO offset reconciles
//    the onset with the build-up - the peak lands at 124 to 140 ms against the
//    reference's 228 whatever value is used. A constant offset is therefore the
//    wrong shape of answer, and none is applied.
//
//    THE STRONGEST LEAD, and why it is not applied. If the record's tap words
//    are BYTE addresses into a 16-bit effect memory - which is what that memory
//    is, and what the delay line's own 1/32768 truncation already assumes -
//    then every delay is half what this code uses. Halving them puts the onset
//    at 20 ms against the reference's 18, where reading them as samples gives
//    40, and takes the envelope error's standard deviation from 15.6 to 8.1 dB.
//    The loop tap halves with them: 14334/2 = 7167 samples = 224 ms, and the
//    reference's build-up peaks at 228.
//
//    It cannot be adopted alone. Halving the loop halves the time of a pass, so
//    the same per-pass gain doubles the decay RATE: the tail then dies to
//    silence, envelope mean -48 dB against the reference and a worst step of
//    -160. Every g measured from a decay in this file was derived assuming the
//    unhalved length, so the loop-gain law has to be re-derived in the same
//    move - g would go roughly as its own square root. That is a coherent piece
//    of work and not a one-line change, and shipping half of it is worse than
//    shipping neither.
//
//    AND WHY EVERY GAIN FIT IN THAT DIRECTION FAILED. In this topology the
//    feedback is the wet SUM, so the path already carries the nine tap gains:
//    their absolute sum over 64 is 3.19 to 3.81 across the six reverbs. The
//    per-pass ratio a decay measures is therefore gLoop TIMES that factor, not
//    gLoop. The firmware's own value lands the product near unity - 0.57, 1.12,
//    0.86, 1.04, 1.07, 1.33 at Time 80 - which is what a reverb on the edge of
//    sustaining looks like and is why it works at all. Every gain fitted here
//    from a measured decay was the RATIO, so feeding it back in overshot by
//    about 3.7x and the loop ran away: with byte-addressed taps and a gain
//    refitted to 0.39 dB rms on the sweep, the tail pinned at full scale and
//    measured +105 dB against the reference.
//
//    So the tap scale, the loop gain and the tap-sum feedback are ONE system
//    and have to be solved together. Fitting any of them alone will keep
//    producing numbers that measure beautifully on the axis they were fitted
//    on and destroy the others.
//
//    AND IT IS REFUTED by a measurement scdb had already made. Its
//    08_effects/reverb.md tested a w20/2 sub-period directly against the
//    reference: the first and second half of one w20 window are ANTI-correlated
//    at r = -0.69 to -0.92 at every Time, while adjacent w20 windows correlate
//    at r = +0.82 to +0.87. That is how w20 was confirmed as the loop period in
//    the first place, and byte-addressed taps require exactly the w20/2 period
//    that test rules out. The same page settles the other half: +0x36 is word
//    27's HIGH byte, values 74/142/94/116/116/154, and the firmware's mulxu.b
//    at ROM2 0x71CD multiplies by it "confirmed to the instruction" - so
//    reading the LOW byte as the Time scale contradicts a disassembly.
//
//    Both were built and measured before that page was read, and both measured
//    BETTER - envelope error +3.91 -> +0.30 dB, the kit's effects tail 4.81 ->
//    3.27. Neither is kept. A structure that measures better and contradicts a
//    direct correlation measurement and a disassembled instruction is a warning
//    that the metric is incomplete, not a discovery; this session has already
//    produced three of those. Read 08_effects/reverb.md before touching this
//    network again - it also records that in-loop damping is real but "not
//    driven by any field in the record", which makes the byte used above for
//    the damping pole a candidate rather than a finding.
//
//    THAT JOINT SEARCH WAS RUN, and it is a negative result worth having. Three
//    axes: tap scale (as read, or halved), feedback (the wet sum, or the single
//    w20 tap), and a multiplier on the loop gain. Scored on the drum tail with
//    tail_fine.py, envelope in 5 ms steps and 1/24-octave bands together:
//
//      as read + wet sum      envelope +3.91 dB, 161/360 steps out; 45 bins >6 dB
//      halved  + wet sum      no gain is stable - x1.0 dies at -48 dB, x1.3
//                             overshoots to +15, x2.0 to +53. The transition
//                             from dead to runaway skips the target entirely,
//                             which is a loop sitting on its stability boundary
//      halved  + w20 tap x1.8 envelope +1.34 dB, 113/360 steps out - the BEST
//                             envelope measured - but 99 bins >6 dB, twice as
//                             many as the shipped structure
//
//    The two feedback topologies trade against each other: the wet sum smears
//    the comb and gets the narrowband right, the single tap gets the timing and
//    the envelope right. No point in the space is good at both, so the
//    STRUCTURE is wrong, not the constants, and no further parameter sweep will
//    find it. What would: the firmware's own tap-setup code, or a register-level
//    trace. Until then the shipped structure is kept because it is the one that
//    is right about frequency, and being wrong about time is the lesser of the
//    two audible errors - the owner can still hear it.
//
//    What this does establish: the residual is a TIMING error, not a spectral
//    one. It is why the narrowband comb is misplaced by up to 27 dB at 209 Hz
//    while the octave-band mean reads 2.5 dB, and why no damping or gain
//    constant has ever fixed what the owner keeps hearing.
// FIDELITY OF THIS NETWORK -- read before trusting it, and before extending it.
//
// The firmware is the ground truth here, and by that standard this network is
// NOT firmware-exact. It is firmware-derived in its data and its structure, and
// measured or missing in three places. Stated plainly so nobody mistakes a
// measurement for a fact (evidence classes and their precedence: scdb Agent.md,
// "Evidence precedence").
//
// FIRMWARE-DERIVED, and safe to rely on:
//   - Every number in the eight type records: pre-delay, the nine stereo tap
//     delays, their signed Q6 gains, the feedback tap, the pre-LPF pair and the
//     input gain. Read from the device's own ROM through the 0x4800 pointer
//     walk and verified cell by cell against the recovered record map.
//   - That the taps are absolute delays in samples, even word LEFT and odd
//     RIGHT, and that the effect line runs at exactly 32000 Hz.
//   - The return level law, the delay tap law and the Delay-Feedback gating,
//     each traced to the instruction that computes it.
//
// MEASURED THROUGH THE REFERENCE EMULATOR, not stated by the firmware:
//   - The recirculating multi-tap topology itself. The firmware uploads tap
//     values; it does not say what the chip does with them. The structure comes
//     from measured echo times and levels plus a cross-device register split.
//   - g_fb = (Delay Feedback - 2) / 128, i.e. F010 is Q8. Residual RMS 0.046 dB
//     over nine feedback values on both delay types, and the remaining factor
//     of two settled by stability rather than by fitting. The firmware writes
//     the register; what the chip makes of it had to be measured.
//
// MISSING OR UNKNOWN -- the reasons this is not a finished model:
//   - IN-LOOP HF DAMPING IS ABSENT. The reference loses roughly 5 to 9 dB per
//     pass above 3 kHz that this network does not, so the tail is about 3 dB
//     bright in a full mix and up to 20 dB bright in the last second, where the
//     mix IS the tail. The pre-LPF is refuted as the source three ways: the loss
//     accumulates per pass, it is roughly type-independent across an 8x range of
//     pre-LPF pole, and swapping the pair's halves kills the delay types
//     outright. The responsible field is NOT IDENTIFIED. Candidates are the
//     record's unassigned bytes. A one-pole at a ~ 0.7 matches the residual and
//     is deliberately NOT implemented: it is outside this chip family's own
//     damping poles (0.125-0.48) and a filter fitted to a residual is not
//     evidence about a device.
//   - Tap pair P2's gain is UNKNOWN and the pair is muted. Word 23's high byte
//     is zero in all sixteen records across two devices, so ROM data can never
//     evidence it.
//   - The pre-delay in word 1 is read and NOT inserted; the nine tap delays are
//     absolute and measured to the sample without it.
//   - The input gain's unity reconciles the six reverbs but leaves the two delay
//     types 11 to 14 dB quiet, constant across level. One of the two readings is
//     wrong and it is not known which.
//
// So: keep the ROM data, distrust the tail's spectrum, and do not tune any
// constant here to close a residual. When the damping field is found, it belongs
// in the loop and the residual above should collapse.

void Reverb::_process_sample_jv(float input, float output[2])
{
  // The pre-LPF, y[n] = (lo*x[n] + hi*y[n-1]) / 64. The pair sums to exactly 64
  // in all eight records, so its DC gain is exactly 1 and it only shapes.
  _preLpfState = _preLpfA * _preLpfState + _preLpfB * input;

  // The line feeds back its OWN WET OUTPUT, not a single point w20 samples
  // back. Using w20 as a feedback tap - which this did, and whose supposed
  // confirmation was measured against our own render rather than the reference
  // (scdb docs/corrections_2026-09-04.md 27) - makes the whole nine-tap pattern
  // recirculate at w20 and be heard as a discrete repeat. On HALL1, w20 is
  // 12919 samples = 403.7 ms at this line's 32 kHz, and the owner heard it as
  // a stutter in the piano's reverb at the end of demo song 4.
  //
  // Measured on the reference with a single 60 ms stab: envelope
  // self-similarity at the w20 lag is +0.04 on HALL1 and +0.05 on PAN-DLY,
  // i.e. nothing, and the structure that IS there sits at 46 and 48 ms -
  // HALL1's second tap is 1483 samples = 46.3 ms. Ours had +0.52 at w20.
  //
  // Feeding the wet output back instead reproduces that: +0.06 at w20, its
  // strongest structure at 44 ms, and a decay slope of -30.9 dB/s against the
  // reference's -29.4 where the old model gave -26.9. It also tracks Reverb
  // Time better on an axis that was not tuned for it - the slope error across
  // times 30/60/90/127 falls from +18.0/+9.9/+2.7/-2.6 to +2.6/+0.6/+2.0/+5.8
  // dB/s, a mean of 8.3 down to 2.75.
  //
  // This is a BETTER-MEASURING MODEL, not a trace. w20's real role is unknown:
  // it sits just past the last tap in every one of the eight records (12172 vs
  // 12919 on HALL1, 7115 vs 7440 on ROOM1), which is what a line LENGTH looks
  // like, and nothing here establishes that.
  float wetL = 0.0f, wetR = 0.0f;
  for (int k = 0; k < _jvTaps; k++) {
    if (!_jvActive[k])                  // a zero gain byte is a MUTED pair
      continue;
    wetL += _jvGain[k] * _rBuffer[(_jvTapL[k] + _sweepIndex) & rBufferMask];
    wetR += _jvGain[k] * _rBuffer[(_jvTapR[k] + _sweepIndex) & rBufferMask];
  }
  // How much of that wet sum returns, and a warning about how it was nearly
  // got wrong. Fitting it against the reference's T60 - Schroeder integration
  // over a 6 type x 6 Time sweep, 24 stable cells - says 0.80: mean error
  // 22.0 % -> 11.2 %, worst 71.8 % -> 29.0 %, and the systematic bias +18.0 %
  // -> +0.7 %. It wins on every one of those statistics and it is still wrong.
  //
  // T20 measures the SLOPE between -5 and -25 dB and says nothing about where
  // a tail STOPS. Compared point by point against the reference instead, 0.80
  // reaches digital silence a second early - -96 dB against the reference at
  // 2.5 s - and the mean error over the whole decay goes 3.3 dB at 1.0 to
  // 10.9 dB at 0.80. The two interact: less feedback reaches the line's one-LSB
  // floor sooner, so a gain fitted on slope alone truncates the end.
  const float fb = 0.5f * (wetL + wetR);
  // The delay line is the chip's, and the chip's is FIXED POINT. Writing a
  // float here means the recirculating signal halves forever and never reaches
  // zero; the hardware's underflows to silence once it falls below one LSB,
  // and that is not a detail - it is the whole shape of the end of a tail.
  //
  // Measured on a drum hit, ours against the reference: identical to within
  // 1-2 dB for the first second, then the reference falls off a cliff to
  // EXACT digital silence by about 3 s while ours decayed on, 12 to 17 dB hot
  // at 1.8-2.5 s. The owner heard it as a wash over every percussion hit. It
  // reads like a loop-gain error and is not one: measured directly over a
  // 6 type x 6 Time sweep, our gain at the character the demos use sits within
  // 0.4 dB of the reference's.
  //
  // rBufferQuantum is one LSB of the effect memory's word. Zero leaves the
  // line in float, which is what every Sound Canvas profile wants until the
  // same measurement is made on one.
  {
    const float q = _settings->device()->reverb.rBufferQuantum;
    // HF DAMPING IN THE LOOP, from two record words that were never read.
    //
    // Without it the tail stays bright and the error is monotonic in
    // frequency: measured against the reference on a drum hit at 1.2-1.8 s,
    // +0.2 dB at 60-250 Hz rising to +29.8 dB at 6-12 kHz. The bass was always
    // right; every decibel of the excess was treble. That is a damping
    // failure, and no loop-gain constant can fix it - fitting one is what
    // nearly shipped a tail that stopped a second early.
    //
    // The coefficients are words 28 and 29 of the type's own record. They were
    // outside the 28 words the loader read, which is why the JV's loop had no
    // damping at all. They are coefficients and not addresses: their low
    // halves are zero and their high halves are 28/29 on HALL1-2 and 31 twice
    // on ROOM1 - the shape of this chip's damping pair on the Sound Canvas
    // side, where the same two registers are c7 and c8.
    //
    // Two cascaded one-poles, each with unity DC gain, is a FIT and not a
    // trace: the words are the device's, the topology is inferred from what
    // measures. Against the reference it takes the mean band error at
    // 1.2-1.8 s from 14.0 to 4.0 dB on one drum and 14.3 to 6.5 on another,
    // and it leaves the bass alone. The alternative reading, y = w28*y + w29*x
    // in the Sound Canvas's own form, fixes the midrange better but its
    // coefficients sum to 0.89 and it loses 1.1 dB a pass broadband, pulling
    // 60-250 Hz down 7.5 to 9.4 dB where the reference has it exact.
    //
    // What is still open: 600 Hz to 3 kHz remains +10 to +15 dB on the second
    // drum. The damping is right in kind and not yet right in detail.
    // Two cascaded one-poles on the RETURN, and the same first pole again on
    // what is written to the line. Damping the write as well as the return
    // costs no new constant and measures better - mean band error at
    // 1.2-1.8 s 5.2 -> 4.2 dB - which is weak evidence that the filter sits on
    // the line's write port rather than in the return alone. Filtering ONLY
    // the write port measures 4.7, so it is not simply that either.
    // HF DAMPING IN THE LOOP: one one-pole on the return, and its coefficient
    // is the LOW byte of record word 27 - 217 on ROOM1/ROOM3, 199 on
    // everything else - over 256.
    //
    // That byte was the last one in a reverb record that nothing else claimed.
    // Words 0-26 are the pre-delay, the nine tap pairs, the loop tap, the
    // pre-LPF pair, the input gain and the nine tap gains; word 27's HIGH byte
    // is the Time scale at +0x36 and word 28's is the return coefficient at
    // +0x38. The loader read 28 words, so words 28-29 were never loaded at all
    // and this byte was simply never looked at.
    //
    // Without any damping the tail error is monotonic in frequency - measured
    // against the reference on a drum hit at 1.2-1.8 s, +0.2 dB at 60-250 Hz
    // rising to +29.8 dB at 6-12 kHz. The bass was always right; all of it was
    // treble, which is why no loop-gain constant ever helped.
    //
    // Two wrong turns are worth recording. Word 28's high byte gives a pole of
    // 0.44 and measures 4.2 dB mean band error, which looked like a find - but
    // that byte is the RETURN coefficient, and reaching 4.2 needed three
    // cascaded poles to make up the missing damping. Word 27's low byte gives
    // 0.777, needs ONE pole, and measures 2.5 dB. The three-pole stack was
    // compensating for the wrong coefficient.
    //
    // Per-band, ours minus the reference at 1.2-1.8 s: -1.5 at 60-250 Hz,
    // +0.3 at 250-600, +3.9 at 600-1500, -2.4 at 1.5-3k, 0.0 at 3-6k, +2.0 at
    // 6-12k. The DELAY records are 0x38 bytes and end at word 27, so their low
    // byte is the same 199 by coincidence of layout; only characters 0-5 run
    // this network at all.
    _jvLoopLpf = _jvDampPole * _jvLoopLpf + (1.0f - _jvDampPole) * fb;
    const float fbd = _jvLoopLpf;
    // The pre-LPF stays on the fresh INPUT. Putting the record's own pair on
    // the whole write instead kills the line outright - measured at -200 dB,
    // i.e. silence - so that structure is refuted, not merely worse.
    float v = _preLpfState * _jvInGain + _gLoop * fbd;
    if (q > 0.0f)
      v = q * truncf(v / q);
    _rBuffer[_sweepIndex] = v;
  }

  _sweepIndex = (_sweepIndex - 1) & rBufferMask;

  const float fade = _fade_step();

  output[0] = wetL * _outGain * fade;
  output[1] = wetR * _outGain * fade;
}


// Load one type's whole network out of its ROM record. The word -> field map is
// scdb's, and the coefficient byte order is what the two DELAY records prove:
// the only nonzero tap byte in them is the LAST one, on the pair the delay arm
// drives, and word 22's high byte is the input gain because PAN-DLY's wet is
// exactly 2.000x DELAY's with +16 against +8 there and everything else equal.
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

  _jvPreDelay  = (uint16_t) rec[1];
  _jvLoopTap   = (uint16_t) rec[20];
  _jvTapBase   = (uint16_t) (rec[13] + 1);   // word +0x1A plus one
  _jvInGain    = sByte((uint16_t) rec[22], true);
  // +0x36, the Time scale the firmware multiplies Reverb Time by at ROM2
  // 0x71CD - confirmed to the instruction, values 74/142/94/116/116/154 across
  // the six reverbs (scdb 08_effects/reverb.md). Reading the LOW byte instead
  // needs no fitted multiplier and measures better; it was tried and is NOT
  // kept, because it contradicts a disassembled mulxu.b. See the note below.
  _jvTimeScale = rec[27] >> 8;               // record[+0x36]

  // The pre-LPF pair, high byte the pole and low byte the input gain. Which is
  // which comes from the SC-55 mk1, which drives the same chip register from its
  // GS Reverb Pre-LPF parameter: "off" is 0x003F there, so the LOW byte is the
  // input. The JV has no such parameter and bakes a pair per type.
  // Words 28 and 29 are byte coefficients, not addresses: their low halves are
  // zero and their high halves are 28/29 on HALL1-2 and 31 on ROOM1, which is
  // the shape of this chip's damping pair on the Sound Canvas side (c7/c8).
  // The damping coefficient is word 29's HIGH byte - 31, 29, 31, 29, 29, 29 on
  // ROOM1..PLATE - and NOT word 28's, which is byte +0x38, the reverb RETURN
  // coefficient this profile already reads as JVReverbReturnCoeff (31, 29, 31,
  // 28, 28, 28, 0, 61). The two differ only on HALL1-2 and PLATE, which is
  // exactly why using the wrong one measured almost as well and looked right.
  // w29 is the only byte of a reverb record that nothing else claims.
  //
  // The DELAY records are 0x38 bytes and do not own words 28-29 at all; theirs
  // read out of the record that follows, so only characters 0-5 use this.
  _jvDampPole = (rec[27] & 0xff) / 256.0f;   // record[+0x37]

  _preLpfA = (rec[21] >> 8)   / 64.0f;
  _preLpfB = (rec[21] & 0xff) / 64.0f;

  // The nine tap-pair gains, in stream order from word 22's LOW byte: w22 lo,
  // then each of words 23-26 high byte before low. Shifted and reversed byte
  // orders were both tried by scdb and both are refuted by a delay record.
  static constexpr struct { int word; bool hi; } gainByte[_jvTaps] = {
    { 22, false }, { 23, true }, { 23, false }, { 24, true }, { 24, false },
    { 25, true  }, { 25, false }, { 26, true }, { 26, false }
  };

  for (int k = 0; k < _jvTaps; k++) {
    _jvTapL[k] = (uint16_t) rec[2 * (k + 1)];
    _jvTapR[k] = (uint16_t) rec[2 * (k + 1) + 1];
    const int raw = gainByte[k].hi ? (rec[gainByte[k].word] >> 8)
                                   : (rec[gainByte[k].word] & 0xff);
    // k == 1 is P2, whose byte is 0x00 in every record on both devices. It is
    // muted for the same reason every other zero byte is, and its true role is
    // UNKNOWN rather than zero - see the note on _process_sample_jv.
    _jvActive[k] = (raw != 0);
    _jvGain[k]   = sByte((uint16_t) rec[gainByte[k].word], gainByte[k].hi);
  }
}


void Reverb::_set_character(int character)
{
  _character = character;

  if (_jvNetwork) {
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
    _jvLoopLpf = 0.0f;

  // Delay, Panning Delay
  } else if (character == 6 || character == 7) {
    _activeCharRegs = _crDelayBase;
    std::fill(_rBuffer.begin(), _rBuffer.end(), 0.0f);
    _dampA = _dampB = 0.0f;
    _preLpfState = 0.0f;
    _jvLoopLpf = 0.0f;
  }
}


void Reverb::_set_reverb_time(int reverbTime)
{
  _reverbTime = reverbTime;

  const ReverbLaw &law = _settings->device()->reverb;

  int rt = std::clamp(reverbTime, 0, 127);

  // The JV-880's own network takes its loop gain and its delay taps from the
  // selected type's record, so neither the Sound Canvas line nor this program's
  // register layout is involved (P-0399).
  if (_jvNetwork) {
    // The firmware's expansion of a 0-127 parameter to 0-255: 2v below 64 and
    // 2v + 1 from 64 up, the carry of a byte-wide shift rather than rounding.
    const int ex = 2 * rt + (rt >= 64 ? 1 : 0);

    if (_character >= 0 && _character <= 5) {
      _gLoop = _jv_loop_gain(ex);

    } else if (_character == 6 || _character == 7) {
      // The delay arm, ROM2 0x7283/0x72A0: two tap addresses A and B out of the
      // record's own scale words, written into the FIVE tap pairs the arm
      // drives, with the loop tap at B. Only P9's gain is nonzero (+64), so
      // only P9 is heard - which is why the firmware can write the same pair of
      // addresses into four places without consequence.
      //
      //   A -> w6, w10, w14, w18        i.e. the LEFT tap of P3, P5, P7, P9
      //   B -> w7, w11, w15, w19, w20   the RIGHT tap of those, and the loop
      //   B, B+1 -> w16, w17            P8, which the JV parks here and mutes
      const int E = 2 * rt + (rt >= 64 ? 1 : 0);
      const int M = (E << 8) | (E >= 128 ? 0xff : 0);
      const int sA = _LUT.JVReverbTapScale[2 * _character];
      const int sB = _LUT.JVReverbTapScale[2 * _character + 1];
      const uint16_t A = (uint16_t) (_jvTapBase + ((M * sA) >> 16));
      const uint16_t B = (uint16_t) (_jvTapBase + ((M * sB) >> 16));

      _jvTapL[2] = _jvTapL[4] = _jvTapL[6] = _jvTapL[8] = A;
      _jvTapR[2] = _jvTapR[4] = _jvTapR[6] = _jvTapR[8] = B;
      _jvTapL[7] = B;
      _jvTapR[7] = (uint16_t) (B + 1);
      _jvLoopTap = B;
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


// Reverb Time -> the recirculation gain on the JV's loop tap, for the six
// reverb types. Two candidate laws, and they contradict each other; which one
// runs is a field of the device profile so both can be measured.
//
// JVFirmwareRegister is the firmware's arithmetic, confirmed instruction by
// instruction at ROM2 0x71C3-0x71DC: an unsigned mulxu.b of the expanded Time
// byte by record[+0x36], written to slot 0x1E F010's high byte, and the >>8 is
// not even a shift - it falls out of overwriting the product's low byte with
// the constant 0xB0. The byte over 256 is then the loop gain.
//
// JVLoopLengthFit is scdb's empirical replacement, from a 28-cell measurement
// of the per-pass loss on the reference:
//
//     20*log10(g) = 20*log10(w20) + 40*log10(expand(Time)) - 184.6
//
// The register law is falsified by CONTRADICTION rather than by residual size -
// ROM1 at Time 127 and STAGE2 at Time 81 compute the identical byte 73 and
// measure 2.7 dB apart, so no function of that byte alone can fit both - and
// the fit's held-out error is 0.30 to 3.15 dB over the six types. But it is a
// `FIT`: it has no mechanism, and it drops a byte the firmware demonstrably
// multiplies into the register. Tagged as such in ReverbFeedbackLaw, and the
// measured comparison is in the journal rather than assumed here.
float Reverb::_jv_loop_gain(int expandedTime) const
{
  const ReverbLaw &law = _settings->device()->reverb;

  if (law.feedbackLaw == ReverbFeedbackLaw::JVLoopLengthFit) {
    if (expandedTime <= 0 || _jvLoopTap == 0)
      return 0.0f;
    const float dB = 20.0f * log10f((float) _jvLoopTap) +
                     40.0f * log10f((float) expandedTime) - 184.6f;
    return std::min(powf(10.0f, dB / 20.0f), 0.99f);
  }

  return (float) ((expandedTime * _jvTimeScale) >> 8) / 256.0f;
}


void Reverb::_set_pre_lpf(int preLPF)
{
  _preLPF = preLPF;

  // The JV-880 has NO Pre-LPF parameter - the manual's reverb page lists Type,
  // Level, Time and Feedback and nothing else - and its network's pre-LPF is a
  // fixed pair per type, baked into the record and loaded with it. Letting a
  // GS parameter this device does not have reach the filter would overwrite
  // that pair with a Sound Canvas value on the first update().
  if (_jvNetwork)
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

  // The JV writes Delay Feedback into the loop tap's gain register on slot 0x1E
  // F010 (ROM2 0x72C5-0x72D1) - the same register the reverb arm drives from
  // Reverb Time, which is what identifies that register as the recirculation
  // gain and why the manual says Feedback works only on the two delay types.
  //
  // MEASURED, not assumed, and it is the tightest law in this device. Because
  // the delay arm reaches F010 from Delay Feedback ALONE - no Reverb Time, no
  // record[+0x36] - it is the only place where that register can be varied on
  // its own, so it is the only clean test of what the chip does with it. A
  // 150 ms note through a single-tap line at Delay Time 81 puts one echo every
  // 312.5 ms, each g times the last, so the ratio of successive echo PEAKS is
  // the per-pass gain with no fitting at all. Nine feedback values on both
  // delay types, on the reference:
  //
  //     fb        16     32     48     64     80     96    112    127
  //     g       .1098  .2336  .3627  .4875  .6117  .7345  .8590  .9840
  //     g*128   14.05  29.90  46.42  62.40  78.29  94.02 109.95 125.95
  //
  // so g = (fb - 2) / 128, residual RMS 0.046 dB and max 0.08 dB over a 19 dB
  // span - and the reading g = fb/256 that the topology assumed is out by a
  // clean factor of two, 5.67 dB, at every point.
  //
  // WHICH factor of two this is cannot be told apart from audio: either the
  // register field is Q7 (128 = 1.0), or it is Q8 as everything else assumed
  // and the byte written is 2*(Feedback - 2) rather than Feedback. The reverb
  // arm settles it in favour of the second: ROOM2's own byte reaches 141 at
  // Reverb Time 127, and 141/128 > 1 would make ROOM2 self-oscillate, which it
  // demonstrably does not. So F010 is Q8 on both arms and the delay arm's byte
  // is doubled somewhere between the DT1 and the write - handed to scdb, since
  // the disassembly there has (feedback << 8) | 0xB0 with no doubling in it.
  if (_jvNetwork) {
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
