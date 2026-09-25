/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * This file consists entirely of AI-generated code without direct human
 * authorship and is dedicated to the public domain under CC0 1.0.
 */


#ifndef __TVF_CENTS_RATIO_LAW_H__
#define __TVF_CENTS_RATIO_LAW_H__


#include "tvf_law.h"
#include "../../control_rom.h"
#include "../../device_profile.h"

#include <array>
#include <cstdint>


namespace EmuSC { namespace Gp {


// The filter chain that sums every modulation in CENTS, exponentiates the
// total and multiplies the tone's base coefficient by it, then hands the
// resulting coefficient and damping words straight to the filter
// (TvfLawKind::JVCentsRatio, PROVENANCE.md P-0390).
class CentsRatioTvfLaw : public TvfLaw
{
public:
  // ctrlAcc is the per-tone controller matrix's destination accumulators
  // (scdb D-79), owned by the caller and rebuilt every control period; null
  // where the tone has no matrix.
  CentsRatioTvfLaw(ControlRom::InstPartial &instPartial, uint8_t key,
                   uint8_t velocity, ControlRom::LookupTables &LUT,
                   const TvfJvLaw &law, const int *ctrlAcc,
                   const TvfLfoInputs &lfo);
  ~CentsRatioTvfLaw() override = default;

  // Whether LUT carries the tables this law reads. Without them the filter is
  // left disabled rather than run on zeros: a zero base coefficient is a filter
  // closed to silence, which is a far worse answer than no filter.
  static bool tables_loaded(const ControlRom::LookupTables &LUT);

  void update(const TvfLfoInputs &lfo, SVF &svf) override;
  void apply_sample_set(SVF &svf, std::array<float, 256> &dryBus) override;

  // The chip's cutoff coefficient at the end of the current tick, in
  // TvfJvLaw::cutoffUnity units; the CPU's last transmitted target word; the
  // damping handed to the filter; and the resonance after its slew.
  int cutoff_word(void) const { return _word; }
  int target_word(void) const { return _lastTarget; }
  float damping(void) const { return _q1; }
  int resonance(void) const { return _res; }

private:
  ControlRom::LookupTables &_LUT;
  ControlRom::InstPartial &_instPartial;
  const TvfJvLaw &_law;
  const int *_ctrlAcc;

  uint8_t _key;

  int _tickCount = 0;      // control periods since the last envelope tick
  int _decrement = 0;      // envelope accumulator step for the current segment
  int _envLevel = 0;       // envelope output, 0 .. 0x7f00
  int _envDepth = 0;       // TVF-ENV Depth, scaled and signed
  int _velAtten = 0;       // velocity attenuation of the envelope level
  int _keyFollow = 0;      // key follow offset in cents
  int _lfo1Depth = 0;      // LFO -> TVF depths, scaled and signed
  int _lfo2Depth = 0;
  int _cutoff = 0;         // the tone's base cutoff, 0..127
  int _resTarget = 0;      // the resonance the tone asks for
  int _res = 0;            // the resonance after the per-tick slew
  int _word = 0;           // the chip's cutoff coefficient at this tick's end,
  int _wordPrev = 0;       // and at its start, between which it moves
  int _chipTarget = 0;     // the high byte the CPU transmitted, where it stops
  int _rampStep = 0;       // this tick's signed movement before that stop
  int _lastTarget = -1;    // the CPU's last transmitted target, -1 before any
  float _q1 = 1.0f;        // damping, already in the filter's own units
  int _rampPos = 0;        // samples into the move between the two words

  void _next_phase(void);

  void _init_new_phase(enum Phase newPhase) override;
  void _iterate_phase(void) override;
};

}}  // namespace EmuSC::Gp

#endif  // __TVF_CENTS_RATIO_LAW_H__
