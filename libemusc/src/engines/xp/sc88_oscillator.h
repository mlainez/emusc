/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_SC88_OSCILLATOR_H
#define EMUSC_SC88_OSCILLATOR_H

#include "sc88_wave.h"

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
#endif

#endif
