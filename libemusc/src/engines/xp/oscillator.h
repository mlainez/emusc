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
   XP_WRAP_FULL_CARRY, and the evidence is at
   `wrapped_phase`.  The other two are kept only so that
   arithmetic stays testable; neither is a candidate. */
enum xp_fractional_wrap {
  XP_WRAP_FULL_CARRY,
  XP_WRAP_FULL_RESET,
  XP_WRAP_FRACTION_ONLY
};

struct xp_oscillator {
  const int32_t *pcm24;
  size_t pcm_count;
  uint32_t pcm_base;
  uint32_t start;
  uint32_t loop;
  uint32_t end;
  size_t initial_count;
  size_t cycle_count;
  /* The phase accumulator, in two representations of one quantity: a
     position in samples into the run the oscillator is on. `phase_fixed`
     is that number in fixed point, which yields the sample index and the
     fraction without a double-to-integer conversion.
     EMUSC_LEGACY_DSP_FAST advances that one and leaves `phase` standing;
     every other build advances `phase`. Read either through
     oscillator_phase() and write either through oscillator_set_phase(),
     which act on whichever one the build in hand advances.

     Both are declared in every build: this struct crosses the library
     boundary, and one whose size depends on a build option would link
     quietly and behave differently. */
  double phase;
  uint64_t phase_fixed;
  /* Samples of wave per output sample, and the same rate in fixed point.
     Unlike the phase, `step` is an input rather than state and stays
     valid in both tiers; `step_fixed` is derived from it. Write it
     through oscillator_set_step(), which keeps the two together. */
  double step;
  uint64_t step_fixed;
  enum xp_wave_loop_type mode;
  enum xp_fractional_wrap wrap;
  bool initial;
  bool ended;
};

#ifdef __cplusplus
}

namespace EmuSC { namespace Xp {

// Four-point (cubic B-spline) wave-sample interpolator for the XP-family
// chip's phase accumulator, as measured on the SC-88 (see oscillator.cc
// for the interpolation-kernel measurement, cross-checked on the JV-1080's
// dumped wave ROM since it carries the identical part). The plain C
// `xp_oscillator` struct above is shared, unrenamed, with sibling
// engines/xp/*.c modules and the xp_dump_samples.c tool, which both
// read its fields directly.

double pitch_word_rate(uint32_t pitchWord, double outputRate);

/* The phase accumulator and the playback rate, in whichever representation
 * the build in hand advances. Every reader and writer outside
 * oscillator.cc goes through these rather than the fields, so that which
 * representation is live stays a detail of this module. */
double oscillator_phase(const struct xp_oscillator *oscillator);
void oscillator_set_phase(struct xp_oscillator *oscillator, double phase);
void oscillator_set_step(struct xp_oscillator *oscillator, double step);
bool oscillator_init(const struct XpDeviceProfile *profile,
                      struct xp_oscillator *oscillator,
                      const int32_t *pcm24, size_t pcmCount,
                      uint32_t pcmBase,
                      const struct xp_wave_registers *registers,
                      enum xp_wave_loop_type mode,
                      uint32_t pitchWord, double outputRate,
                      enum xp_fractional_wrap wrap);
bool oscillator_next(struct xp_oscillator *oscillator, float *sample);

}}  // namespace EmuSC::Xp
#endif

#endif
