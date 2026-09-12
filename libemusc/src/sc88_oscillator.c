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

/* A ping-pong turn in a DIFFERENTIAL format is not a time reversal.

   The wave ROM stores deltas, and the decoder is an accumulator.  Walking
   the address back down the stream while still ADDING what it reads gives,
   from the turn at address_c,

       y[m] = x[c] + sum(d[c] .. d[c-m+1]) = 2*x[c] - x[c-m]

   - the loop's own waveform, backwards AND reflected about the value it
   turned at.  That is continuous in value and in SLOPE at the turn, where a
   plain time reversal puts a corner and a forward wrap puts a phase jump.

   The ROM says this is the shape the format is cut for: over all 1733
   looping descriptors the deltas from address_b to address_c sum to exactly
   zero, so x[c] == x[b-1] without exception.  That single fact makes the
   reflected pass land exactly on x[b-1] when the address reaches b-1, and
   the cycle closes with no step and no drift - which a reflection about any
   other value would not do.

   Cycle of 2*span positions, span = c - b + 1:
     index < span        address c-1 down to b-1, value reflected
     index >= span       address b up to c, value as decoded

   The descending pass's last position is b-1, where the reflection returns
   x[c] by the invariant above, so it is answered directly rather than read:
   b-1 can sit before the first decoded frame when a zone loops from its own
   start.  */
static uint32_t sc88_oscillator_cycle_address(
  const struct sc88_oscillator *oscillator, size_t index)
{
  const size_t span = (size_t)(oscillator->end - oscillator->loop) + 1;
  index %= oscillator->cycle_count;
  if (oscillator->mode == SC88_WAVE_FORWARD_LOOP)
    return oscillator->loop + (uint32_t)index;
  if (index < span)
    return oscillator->end - 1 - (uint32_t)index;
  return oscillator->loop + (uint32_t)(index - span);
}

/* True while the cycle is on its reflected descending pass. */
static bool sc88_oscillator_cycle_reflected(
  const struct sc88_oscillator *oscillator, size_t index)
{
  const size_t span = (size_t)(oscillator->end - oscillator->loop) + 1;
  if (oscillator->mode != SC88_WAVE_PING_PONG_LOOP || !oscillator->cycle_count)
    return false;
  return (index % oscillator->cycle_count) < span;
}

static bool sc88_oscillator_reflected(
  const struct sc88_oscillator *oscillator, size_t index)
{
  if (oscillator->initial) {
    if (index < oscillator->initial_count || !oscillator->cycle_count)
      return false;
    return sc88_oscillator_cycle_reflected(
      oscillator, index - oscillator->initial_count);
  }
  return sc88_oscillator_cycle_reflected(oscillator, index);
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

/* The decoded value the oscillator is standing on, with the reflected
   descending pass of a ping-pong applied.  The one position the reflection
   cannot read is address_b - 1, which can precede the first decoded frame;
   the ROM's zero-sum invariant answers it as x[c] exactly. */
static bool sc88_oscillator_value(const struct sc88_oscillator *oscillator,
                                  size_t index, double *out)
{
  uint32_t address = sc88_oscillator_address(oscillator, index);
  double anchor;

  if (sc88_oscillator_reflected(oscillator, index)) {
    anchor = (double)oscillator->pcm24[oscillator->end -
                                       oscillator->pcm_base];
    if (sc88_oscillator_contains(oscillator, address)) {
      *out = 2.0 * anchor -
        (double)oscillator->pcm24[address - oscillator->pcm_base];
      return true;
    }
    /* address_b - 1 only: it can precede the first decoded frame when a
       zone loops from its own start, and the ROM's zero-sum invariant
       answers it as x[c] exactly. */
    if (address + 1 == oscillator->loop) {
      *out = anchor;
      return true;
    }
    return false;
  }
  if (!sc88_oscillator_contains(oscillator, address))
    return false;
  *out = (double)oscillator->pcm24[address - oscillator->pcm_base];
  return true;
}

bool sc88_oscillator_next(struct sc88_oscillator *oscillator, float *sample)
{
  size_t index;
  double fraction;
  double value0;
  double value1;

  if (!oscillator || !sample || oscillator->ended)
    return false;
  index = (size_t)floor(oscillator->phase);
  fraction = oscillator->phase - (double)index;
  if (!sc88_oscillator_value(oscillator, index, &value0) ||
      !sc88_oscillator_value(oscillator, index + 1, &value1))
    return false;
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
