/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_oscillator.h"

#include <assert.h>
#ifdef NDEBUG
#error "this test is assertion-driven; NDEBUG compiles it away"
#endif
#include <math.h>

static int sample_value(float sample)
{
  return (int)lround(sample * 8388608.0f);
}

/* The sequences below fix the POSITION progression, which is what these
   cases are about.  The read is three-tap, so the value the oscillator
   emits at a position is that position mixed with its two neighbours; the
   kernel is applied here rather than baked into the sequences, so a change
   of kernel touches this one function and the addressing cases stay
   readable.  Each sequence carries one position beyond the samples it
   asserts, because the last sample needs a neighbour after it, and the tap
   before the first position falls back to the first, as the oscillator
   does. */
static void expect(struct sc88_oscillator *oscillator,
                   const int *values, size_t count)
{
  size_t i;
  float sample;
  for (i = 0; i + 1 < count; ++i) {
    double want = 0.125 * values[i ? i - 1 : 0] + 0.75 * values[i] +
      0.125 * values[i + 1];
    assert(sc88_oscillator_next(oscillator, &sample));
    assert(fabs((double)sample * 8388608.0 - want) < 1e-4);
  }
}

int main(void)
{
  const int32_t pcm[] = {8, 9, 10, 11, 12};
  /* The same five addresses, cut the way the wave ROM cuts a loop: the
     deltas from address_b to address_c sum to zero, so x[b-1] == x[c].
     Every looping descriptor in the SC-88 ROM satisfies this. */
  const int32_t closed[] = {8, 12, 10, 11, 12};
  const struct sc88_wave_registers registers = {0, 8, 10, 12, 0, 0x18};
  struct sc88_oscillator oscillator;
  static const int forward[] = {8, 9, 10, 11, 12, 10, 11, 12};
  /* Ping-pong descends by REFLECTION about x[c], not by replaying the
     samples backwards: the decoder is an accumulator and running the
     address back down the delta stream while still adding gives
     2*x[c] - x[a].  Addresses c-1, c-2, b-1, then b, b+1, c. */
  static const int ping_pong[] =
    {8, 9, 10, 11, 12, 13, 14, 15, 10, 11, 12, 13};
  /* On a loop cut the way the ROM cuts them, that lands the descending
     pass exactly on x[b-1] and the cycle closes with no step: the turn
     reads 12 and the next sample is x[b], as it was the first time. */
  static const int closed_ping_pong[] =
    {8, 12, 10, 11, 12, 13, 14, 12, 10, 11, 12, 13, 14, 12, 10, 11};
  /* A one-shot holds its last position, so the neighbour after the
     final sample is that position again. */
  static const int one_shot[] = {8, 9, 10, 11, 12, 12};
  float sample;

  assert(fabs(sc88_pitch_word_rate(0x38000, 32000.0) - 1.0) < 1e-12);
  assert(fabs(sc88_pitch_word_rate(0x3c000, 32000.0) - 2.0) < 1e-12);
  assert(sc88_oscillator_init(&oscillator, pcm, 5, 8, &registers,
                              SC88_WAVE_FORWARD_LOOP, 0x38000, 32000.0,
                              SC88_WRAP_FULL_CARRY));
  expect(&oscillator, forward, sizeof forward / sizeof forward[0]);

  assert(sc88_oscillator_init(&oscillator, pcm, 5, 8, &registers,
                              SC88_WAVE_PING_PONG_LOOP, 0x38000, 32000.0,
                              SC88_WRAP_FULL_CARRY));
  expect(&oscillator, ping_pong, sizeof ping_pong / sizeof ping_pong[0]);

  assert(sc88_oscillator_init(&oscillator, closed, 5, 8, &registers,
                              SC88_WAVE_PING_PONG_LOOP, 0x38000, 32000.0,
                              SC88_WRAP_FULL_CARRY));
  expect(&oscillator, closed_ping_pong,
         sizeof closed_ping_pong / sizeof closed_ping_pong[0]);

  assert(sc88_oscillator_init(&oscillator, pcm, 5, 8, &registers,
                              SC88_WAVE_FORWARD_ONE_SHOT, 0x38000, 32000.0,
                              SC88_WRAP_FULL_CARRY));
  expect(&oscillator, one_shot, sizeof one_shot / sizeof one_shot[0]);
  assert(!sc88_oscillator_next(&oscillator, &sample));

  assert(sc88_oscillator_init(&oscillator, pcm, 5, 8, &registers,
                              SC88_WAVE_FORWARD_LOOP, 0x34000, 32000.0,
                              SC88_WRAP_FULL_CARRY));
  /* Half-step: the first read sits on position 0 with the tap before it
     folded back, and the second sits exactly between 0 and 1, where the
     three-tap weights are (1/2, 1/2, 0). */
  assert(sc88_oscillator_next(&oscillator, &sample));
  assert(fabs((double)sample * 8388608.0 -
              (0.125 * 8 + 0.75 * 8 + 0.125 * 9)) < 1e-4);
  assert(sc88_oscillator_next(&oscillator, &sample));
  assert(fabs((double)sample * 8388608.0 - (0.5 * 8 + 0.5 * 9)) < 1e-4);
  return 0;
}
