/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_XP_OSCILLATOR_H
#define EMUSC_XP_OSCILLATOR_H

#include "wave.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the phase accumulator does at a loop wrap.  The ROM says
   SC88_WRAP_FULL_CARRY, and the evidence is at
   `sc88_oscillator_wrapped_phase`.  The other two are kept only so that
   arithmetic stays testable; neither is a candidate. */
enum sc88_fractional_wrap {
  SC88_WRAP_FULL_CARRY,
  SC88_WRAP_FULL_RESET,
  SC88_WRAP_FRACTION_ONLY
};

struct sc88_oscillator {
  const int32_t *pcm24;
  size_t pcm_count;
  uint32_t pcm_base;
  uint32_t start;
  uint32_t loop;
  uint32_t end;
  size_t initial_count;
  size_t cycle_count;
  double phase;
  double step;
  enum sc88_wave_loop_type mode;
  enum sc88_fractional_wrap wrap;
  bool initial;
  bool ended;
};

/* Compatibility surface for callers not yet ported to the EmuSC::Xp API
 * below (sibling engines/xp/*.c modules, sc88_dump_samples.c, and
 * sc88_oscillator_test.c, all of which read this struct's fields
 * directly). Each forwards to the real implementation in namespace
 * EmuSC::Xp. */
double sc88_pitch_word_rate(uint32_t pitch_word, double output_rate);
bool sc88_oscillator_init(struct sc88_oscillator *oscillator,
                          const int32_t *pcm24, size_t pcm_count,
                          uint32_t pcm_base,
                          const struct sc88_wave_registers *registers,
                          enum sc88_wave_loop_type mode,
                          uint32_t pitch_word, double output_rate,
                          enum sc88_fractional_wrap wrap);
bool sc88_oscillator_next(struct sc88_oscillator *oscillator, float *sample);

#ifdef __cplusplus
}

namespace EmuSC { namespace Xp {

// Four-point (cubic B-spline) wave-sample interpolator for the XP-family
// chip's phase accumulator, as measured on the SC-88 (see oscillator.cc
// for the interpolation-kernel measurement, cross-checked on the JV-1080's
// dumped wave ROM since it carries the identical part). The plain C
// `sc88_oscillator` struct above is shared, unrenamed, with sibling
// engines/xp/*.c modules and the sc88_dump_samples.c tool, which both
// read its fields directly.

double pitch_word_rate(uint32_t pitchWord, double outputRate);
bool oscillator_init(struct sc88_oscillator *oscillator,
                      const int32_t *pcm24, size_t pcmCount,
                      uint32_t pcmBase,
                      const struct sc88_wave_registers *registers,
                      enum sc88_wave_loop_type mode,
                      uint32_t pitchWord, double outputRate,
                      enum sc88_fractional_wrap wrap);
bool oscillator_next(struct sc88_oscillator *oscillator, float *sample);

}}  // namespace EmuSC::Xp
#endif

#endif
