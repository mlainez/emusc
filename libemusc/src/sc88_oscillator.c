/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_oscillator.h"

#include <math.h>

double sc88_pitch_word_rate(uint32_t pitch_word, double output_rate)
{
  if (output_rate <= 0.0)
    return 0.0;
  return (SC88_WAVE_SAMPLE_RATE / output_rate) *
    pow(2.0, ((double)pitch_word - 0x38000) / 0x4000);
}

static uint32_t sc88_oscillator_cycle_address(
  const struct sc88_oscillator *oscillator, size_t index)
{
  const size_t span = (size_t)(oscillator->end - oscillator->loop) + 1;
  index %= oscillator->cycle_count;
  if (oscillator->mode == SC88_WAVE_FORWARD_LOOP)
    return oscillator->loop + (uint32_t)index;
  if (index < span)
    return oscillator->end - (uint32_t)index;
  return oscillator->loop + (uint32_t)(index - span);
}

static uint32_t sc88_oscillator_address(
  const struct sc88_oscillator *oscillator, size_t index)
{
  if (oscillator->initial) {
    if (index < oscillator->initial_count) {
      if (oscillator->mode == SC88_WAVE_REVERSE_ONE_SHOT)
        return oscillator->start - (uint32_t)index;
      return oscillator->start + (uint32_t)index;
    }
    if (oscillator->cycle_count)
      return sc88_oscillator_cycle_address(
        oscillator, index - oscillator->initial_count);
    return oscillator->mode == SC88_WAVE_REVERSE_ONE_SHOT
      ? oscillator->end + 1 : oscillator->end;
  }
  return sc88_oscillator_cycle_address(oscillator, index);
}

static bool sc88_oscillator_contains(const struct sc88_oscillator *oscillator,
                                     uint32_t address)
{
  return address >= oscillator->pcm_base &&
    (size_t)(address - oscillator->pcm_base) < oscillator->pcm_count;
}

bool sc88_oscillator_init(struct sc88_oscillator *oscillator,
                          const int32_t *pcm24, size_t pcm_count,
                          uint32_t pcm_base,
                          const struct sc88_wave_registers *registers,
                          enum sc88_wave_loop_type mode,
                          uint32_t pitch_word, double output_rate,
                          enum sc88_fractional_wrap wrap)
{
  uint32_t last;
  size_t span;

  if (!oscillator || !pcm24 || !pcm_count || !registers ||
      output_rate <= 0.0 || pitch_word > 0x3ffff ||
      wrap < SC88_WRAP_FULL_CARRY || wrap > SC88_WRAP_FRACTION_ONLY ||
      registers->start >= SC88_WAVE_BANK_SIZE ||
      registers->loop >= SC88_WAVE_BANK_SIZE ||
      registers->end >= SC88_WAVE_BANK_SIZE)
    return false;

  oscillator->pcm24 = pcm24;
  oscillator->pcm_count = pcm_count;
  oscillator->pcm_base = pcm_base;
  oscillator->start = registers->start;
  oscillator->loop = registers->loop;
  oscillator->end = registers->end;
  oscillator->mode = mode;
  oscillator->wrap = wrap;
  oscillator->phase = 0.0;
  oscillator->step = sc88_pitch_word_rate(pitch_word, output_rate);
  oscillator->initial = true;
  oscillator->ended = false;
  oscillator->cycle_count = 0;

  if (mode == SC88_WAVE_REVERSE_ONE_SHOT) {
    last = registers->end + 1;
    if (last > registers->start)
      return false;
    oscillator->initial_count =
      (size_t)(registers->start - last) + 1;
  } else {
    if (registers->start > registers->end)
      return false;
    oscillator->initial_count =
      (size_t)(registers->end - registers->start) + 1;
    if (mode == SC88_WAVE_FORWARD_LOOP ||
        mode == SC88_WAVE_PING_PONG_LOOP) {
      if (registers->loop > registers->end)
        return false;
      span = (size_t)(registers->end - registers->loop) + 1;
      oscillator->cycle_count = mode == SC88_WAVE_FORWARD_LOOP
        ? span : span * 2;
    }
  }

  last = mode == SC88_WAVE_REVERSE_ONE_SHOT
    ? registers->end + 1 : registers->end;
  return sc88_oscillator_contains(oscillator, registers->start) &&
    sc88_oscillator_contains(oscillator, last) &&
    (oscillator->cycle_count == 0 ||
     sc88_oscillator_contains(oscillator, registers->loop));
}

static double sc88_oscillator_wrapped_phase(
  const struct sc88_oscillator *oscillator, double overflow)
{
  switch (oscillator->wrap) {
  case SC88_WRAP_FULL_CARRY:
    return fmod(overflow, (double)oscillator->cycle_count);
  case SC88_WRAP_FULL_RESET:
    return 0.0;
  case SC88_WRAP_FRACTION_ONLY:
    return overflow - floor(overflow);
  }
  return 0.0;
}

bool sc88_oscillator_next(struct sc88_oscillator *oscillator, float *sample)
{
  size_t index;
  uint32_t address0;
  uint32_t address1;
  double fraction;
  double value0;
  double value1;

  if (!oscillator || !sample || oscillator->ended)
    return false;
  index = (size_t)floor(oscillator->phase);
  fraction = oscillator->phase - (double)index;
  address0 = sc88_oscillator_address(oscillator, index);
  address1 = sc88_oscillator_address(oscillator, index + 1);
  if (!sc88_oscillator_contains(oscillator, address0) ||
      !sc88_oscillator_contains(oscillator, address1))
    return false;
  value0 = oscillator->pcm24[address0 - oscillator->pcm_base];
  value1 = oscillator->pcm24[address1 - oscillator->pcm_base];
  *sample = (float)((value0 + fraction * (value1 - value0)) /
                    8388608.0);

  oscillator->phase += oscillator->step;
  if (oscillator->initial &&
      oscillator->phase >= (double)oscillator->initial_count) {
    double overflow = oscillator->phase - oscillator->initial_count;
    if (!oscillator->cycle_count) {
      oscillator->ended = true;
    } else {
      oscillator->initial = false;
      oscillator->phase = sc88_oscillator_wrapped_phase(oscillator, overflow);
    }
  } else if (!oscillator->initial &&
             oscillator->phase >= (double)oscillator->cycle_count) {
    oscillator->phase = sc88_oscillator_wrapped_phase(
      oscillator, oscillator->phase - oscillator->cycle_count);
  }
  return true;
}
