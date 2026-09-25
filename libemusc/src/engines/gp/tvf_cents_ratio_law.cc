/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * This file consists entirely of AI-generated code without direct human
 * authorship and is dedicated to the public domain under CC0 1.0.
 */

#include "tvf_cents_ratio_law.h"
#include "velocity_curve.h"
#include "ctrl_matrix.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>


namespace EmuSC { namespace Gp {


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
// and BASE[cutoff] the coefficient it multiplies; the Sound Canvas chain
// (IndexedTvfLaw) instead builds a cutoff INDEX, which is why the two cannot
// share code.
// ---------------------------------------------------------------------------

// Entry 127 of the base table is 0xffff in every ROM that has one, so a zero
// there means the device's own filter tables did not load.
bool CentsRatioTvfLaw::tables_loaded(const ControlRom::LookupTables &LUT)
{
  return LUT.JVTvfBase[127] != 0;
}


CentsRatioTvfLaw::CentsRatioTvfLaw(ControlRom::InstPartial &instPartial,
                                   uint8_t key, uint8_t velocity,
                                   ControlRom::LookupTables &LUT,
                                   const TvfJvLaw &law, const int *ctrlAcc,
                                   const TvfLfoInputs &lfo)
  : TvfLaw(LUT),
    _LUT(LUT),
    _instPartial(instPartial),
    _law(law),
    _ctrlAcc(ctrlAcc),
    _key(key)
{
  _lfo = lfo;

  // TVF-ENV Depth, signed. The scale is the firmware's and it is not arbitrary:
  // depth +63 at envelope level 127 comes out at exactly 9600 cents, eight
  // octaves, which is where the exponential table saturates.
  {
    const int d = (int8_t) _instPartial.TVFEnvDepth;
    const int m = ((std::abs(2 * d) << 8) * _law.envDepthScale) >> 16;
    _envDepth = (d < 0) ? -m : m;
  }

  // ROM1 0x489d: the shared velocity helper with this tone's TVF curve and
  // TVF-ENV velocity sensitivity (velocity_curve.h).
  _velAtten = jv_velocity_attenuation(_LUT.JVVelCurves,
                                      _instPartial.TVFCOFVelCur,
                                      _instPartial.TVFEnvVelSens, velocity);

  // Key follow, in cents per semitone from note 60. The table IS the manual's
  // published percentage list: +100 % is the value 100, i.e. 1:1 tracking.
  _keyFollow = ((int) _key - 60) *
               _LUT.JVTvfCutoffKF[_instPartial.TVFCOFKeyFlwIdx & 0x0f];

  _lfo1Depth = (int8_t) _instPartial.TVFLFO1Depth * _law.lfoDepthScale;
  _lfo2Depth = (int8_t) _instPartial.TVFLFO2Depth * _law.lfoDepthScale;

  _cutoff = std::clamp((int) _instPartial.TVFBaseFlt, 0, 127);
  _resTarget = std::clamp((int) _instPartial.TVFResonance, 0, 127);

  // No glide at note on. The per-voice resonance slew state is RAM whose value
  // when a note starts is not established, so it starts AT the target: the
  // alternative, starting from zero, would invent an eight-tick sweep on every
  // note. A resonance change during the note still slews.
  _res = _resTarget;
  if (_ctrlAcc && _ctrlAcc[(int) JvCtrlDest::Resonance])
    _res = std::clamp(_res + (_ctrlAcc[(int) JvCtrlDest::Resonance] >> 8),
                      0, 127);

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
  _next_phase();                             // -> Attack1
  _iterate_phase();                          // the coefficient the note starts on
  _wordPrev = _word;                         // and so nothing to ramp from
  _rampStep = 0;
}


// The JV's filter envelope steps on every SECOND control period, because its
// firmware services this envelope on alternate wakes of an 8 ms task and this
// engine's control period is that same 8 ms. Between ticks the coefficient
// keeps moving: apply_sample_set() walks it across the whole tick.
void CentsRatioTvfLaw::update(const TvfLfoInputs &lfo, SVF &)
{
  _lfo = lfo;

  if (++_tickCount >= _law.envTickPeriods) {
    _tickCount = 0;
    _rampPos = 0;
    _iterate_phase();
  }
}


// The chain runs its own phases from _next_phase(); the only phase change that
// reaches it from outside is the note off.
void CentsRatioTvfLaw::_init_new_phase(enum Phase newPhase)
{
  if (newPhase != Phase::Release)
    return;

  // The release starts from wherever the envelope has got to and walks to the
  // release LEVEL, which for the filter envelope is a target like any other -
  // unlike the TVA's, which always releases to silence.
  _phaseStartValue = _envLevel >> 8;
  _phaseEndValue   = _phaseLevel[static_cast<int>(Phase::Release)];
  _phaseDuration   = _LUT.envelopeTime[
                       std::clamp(_phaseTime[static_cast<int>(Phase::Release)],
                                  0, 127)];
  _decrement = (_phaseDuration > 0) ? std::clamp((1 << 20) / _phaseDuration,
                                                1, 0xffff)
                                    : 0x10000;
  _phasePosition = 0;
  _phase = Phase::Release;
}


// Enter the next segment of the JV's filter envelope. Its envelope has one
// segment fewer than this engine's, so Decay1 goes straight to Sustain rather
// than through Decay2.
void CentsRatioTvfLaw::_next_phase(void)
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
    _decrement = 0;
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
  _decrement = (_phaseDuration > 0) ? std::clamp((1 << 20) / _phaseDuration,
                                                1, 0xffff)
                                    : 0x10000;
  _phasePosition = 0;
  _phase = next;
}


void CentsRatioTvfLaw::_iterate_phase(void)
{
  // Step the envelope. The decrement happens before the level is taken, and a
  // segment that runs out is left at its end level rather than interpolated -
  // both as the firmware does it. The guard bounds the walk through segments
  // whose duration is zero.
  if (_phase != Phase::Sustain && _phase != Phase::Terminated) {
    _phasePosition += _decrement;

    for (int guard = 0; guard < 8 && _phasePosition >= 0xffff; guard++) {
      _next_phase();
      if (_phase == Phase::Sustain || _phase == Phase::Terminated)
        break;
      if (_decrement >= 0x10000)           // a zero-length segment: skip it
        _phasePosition = 0xffff;
    }
  }

  if (_phase == Phase::Sustain || _phase == Phase::Terminated) {
    _envLevel = _phaseEndValue << 8;
  } else {
    const int from = _phaseStartValue << 8;
    const int to   = _phaseEndValue << 8;
    _envLevel = from + (int) (((int64_t) (to - from) * _phasePosition) >> 16);
  }
  _envelopeOut = _envLevel >> 8;

  int env = _envLevel;
  env -= (int) (((int64_t) env * _velAtten) >> 16);

  // Cents from here on: envelope depth, key follow and the two LFOs all land in
  // one signed 16-bit accumulator.
  int x = (int) (((int64_t) env * _envDepth) >> 16);
  x += _keyFollow;
  x += ((int) _lfo.lfo1.value * _lfo1Depth) >> 16;
  x += ((int) _lfo.lfo2.value * _lfo2Depth) >> 16;

  // The controller matrix's TVF LFO1 and TVF LFO2 accumulators, against the
  // RAW LFO words, into the same cents accumulator the tone's own depths reach
  // (ROM1 0x434D / 0x4375). scdb D-79.
  if (_ctrlAcc) {
    x += jv_mul_hi(_ctrlAcc[(int) JvCtrlDest::TvfLfo1], _lfo.lfo1.raw);
    x += jv_mul_hi(_ctrlAcc[(int) JvCtrlDest::TvfLfo2], _lfo.lfo2.raw);
  }
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
  // The cutoff index is a WORD, not a byte: ROM1 0x43AD-0x43D2 shifts the
  // tone's 0-127 cutoff up eight bits, adds the controller matrix's CUTOFF
  // accumulator to it and indexes the base table with the high byte alone. A
  // sum that goes negative bottoms the base at 0x100 and one that overflows a
  // signed word tops it at 0xffff, both bypassing the table. scdb D-79.
  int cutoffBase;
  if (_ctrlAcc && _ctrlAcc[(int) JvCtrlDest::Cutoff]) {
    const int cw = (_cutoff << 8) + _ctrlAcc[(int) JvCtrlDest::Cutoff];
    cutoffBase = (cw > 0x7fff) ? 0xffff
               : (cw < 0)      ? 0x100
                               : _LUT.JVTvfBase[std::min(cw >> 8, 127)];
  } else {
    cutoffBase = _LUT.JVTvfBase[_cutoff];
  }

  int word = (int) std::min<int64_t>(
      ((int64_t) E * (int64_t) cutoffBase) >> 8, 0xffff);

  // Resonance moves at most one slew step per tick, and decides both the cutoff
  // ceiling and the damping. Resonance 0 is not a table row but a rule of its
  // own, whose boundary agrees with the tables exactly.
  // The controller matrix's RESONANCE accumulator moves the target by its HIGH
  // byte, and the sum is clamped to 0-127 before the slew (ROM1 0x43E4-0x43FF).
  int resTarget = _resTarget;
  if (_ctrlAcc && _ctrlAcc[(int) JvCtrlDest::Resonance])
    resTarget = std::clamp(resTarget +
                           (_ctrlAcc[(int) JvCtrlDest::Resonance] >> 8),
                           0, 127);

  if (_res != resTarget)
    _res += std::clamp(resTarget - _res,
                       -_law.resSlewPerTick, _law.resSlewPerTick);

  int damp;
  if (_res == 0) {
    word = std::min(word, _law.zeroResLimit);
    damp = std::max(_law.zeroResDampBase - (word >> 3),
                    _law.zeroResDampFloor);
  } else {
    const bool hard = _instPartial.TVFResoMode != 0;
    word = std::min(word, hard ? _LUT.JVTvfLimitHard[_res]
                               : _LUT.JVTvfLimitSoft[_res]);
    damp = hard ? _LUT.JVTvfDampHard[_res] : _LUT.JVTvfDampSoft[_res];
  }

  // The damping is a plain high byte: ROM1 0x224E stores `damp >> 8` and the
  // writer at 0x2A6B hands that one byte to F01C. Resonance 40 and 41 differ in
  // DAMP_SOFT (0x297f / 0x290d) but share the high byte, and the reference
  // renders them byte-identically.
  _q1 = (float) (damp & 0xff00) / (float) _law.dampUnity;

  // The cutoff word does NOT reach the chip as a value. The write is
  // differential (D-15): ROM1 0x224C reads the chip's current coefficient back
  // through F03A / F036, then sends `(target & 0xFF00) | compand(current -
  // target)` - the target's HIGH BYTE and a companded slew rate. So the chip is
  // told the distance to the full-precision target and is told to stop at the
  // high byte. The low byte is spent in the ramp rather than discarded, and
  // three behaviours follow, each of them measured on the reference:
  //
  //   - A word that never moves settles on its high byte, because a note starts
  //     the chip at zero and the first write walks it UP into the stop. Word
  //     0x02ff settles where 0x0200 does - the reference separates that pair by
  //     37.9 dB below the render, this engine by 38.0, where the unquantised
  //     word separated them by 4.1 - and 0x00ff / 0x00c0 / 0x0080 settle at
  //     zero, a filter closed to digital silence.
  //   - A FALLING word is tracked at the full 16 bits: the commanded distance
  //     puts the coefficient on the target, which is always above the high byte
  //     the chip stops at, so the stop never bites. It then KEEPS that value
  //     when the word stops falling, because the CPU only writes when its
  //     target changes (ROM1 0x2256 returns 0 and the leaf writer skips it).
  //   - A RISING word climbs in 8-bit steps: the stop is in the way.
  //
  // The commanded distance is taken as one tick's worth of movement, which is
  // the scale that makes the read-back-and-resend loop a tracker rather than a
  // lag; the compand's exact reconstruction weight is not established (scdb
  // tvf.md open item 6) and would quantise it to about 4 bits of mantissa.
  const int targetHi = word & 0xff00;

  _wordPrev = _word;
  _rampStep = 0;

  // ROM1 0x2256: when the target has not changed the routine returns 0 and the
  // leaf writer skips it, so NOTHING is sent and the coefficient stays where
  // the last ramp left it. That is why a word that stops moving keeps its low
  // byte, while a word that never moved - the note starts the chip at zero and
  // walks up - is stopped at its high byte and keeps none of it.
  if (word != _lastTarget) {
    const bool rising  = word > _lastTarget;
    const bool towards = rising ? (word > _word) : (word < _word);
    _lastTarget = word;

    // ROM1 0x220F: when the coefficient sits on the far side of the new target
    // but inside its high byte, the CPU sends 0xff00 - a rate of zero - and
    // leaves it alone rather than steering it.
    if (towards || (_word & 0xff00) != targetHi) {
      _chipTarget = targetHi;
      _rampStep = word - _word;            // ROM1 0x21DC compands this
      const int end = _word + _rampStep;
      if (_rampStep > 0 && _word <= targetHi)
        _word = std::min(end, targetHi);
      else if (_rampStep < 0 && _word >= targetHi)
        _word = std::max(end, targetHi);
      else
        _word = end;
    }
  }

  static const bool dbg = getenv("EMUSC_DEBUG_TVF") != nullptr;
  if (dbg)
    std::fprintf(stderr,
                 "JV TVF: env=%d x=%d word=0x%x chip=0x%x damp=0x%x res=%d "
                 "F1=%g Q1=%g\n",
                 _envLevel >> 8, x, word, _word, damp, _res,
                 (float) _word / 32768.0f, _q1);
}


// The coefficient moves across the tick rather than stepping at its boundary:
// the chip walks it at the rate the CPU sent and stops when it reaches the
// transmitted high byte, which can happen part way through the tick. Taking
// the walk as linear over the tick is the same simplification
// IndexedTvfLaw::_smooth_cutoff() and TVA::_smooth() already make - the shape
// inside 16 ms is finer than the measurement resolves - but the stop is not a
// simplification and is applied where it falls.
void CentsRatioTvfLaw::apply_sample_set(SVF &svf, std::array<float, 256> &dryBus)
{
  const float unity = (float) _law.cutoffUnity;
  const float from  = _wordPrev / unity;
  const float step  = _rampStep / unity;
  const float stop  = _chipTarget / unity;
  const float span  = (float) (256 * _law.envTickPeriods);

  for (int i = 0; i < 256; i++) {
    float t = (_rampPos + i + 1) / span;
    if (t > 1.0f)
      t = 1.0f;
    float f1 = from + step * t;
    if (step > 0.0f)
      f1 = std::min(f1, stop);
    else if (step < 0.0f)
      f1 = std::max(f1, stop);
    svf.set_coefficients(f1, _q1);
    dryBus[i] = svf.process_sample(dryBus[i]);
  }

  _rampPos += 256;
}

}}  // namespace EmuSC::Gp
