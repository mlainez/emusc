/* SPDX-License-Identifier: CC0-1.0 */
#include "engines/xp/oscillator.h"

#include <cassert>
#ifdef NDEBUG
#error "this test is assertion-driven; NDEBUG compiles it away"
#endif
#include <cmath>

using namespace EmuSC::Xp;

static int sample_value(float sample)
{
  return (int)lround(sample * 8388608.0f);
}

/* The sequences below fix the POSITION progression, which is what these
   cases are about.  The read is four-tap, so the value the oscillator
   emits at a position is that position mixed with its neighbours; the
   kernel is applied here rather than baked into the sequences, so a change
   of kernel touches this one function and the addressing cases stay
   readable.

   These cases all step a whole position at a time, where the cubic
   B-spline's weights are [1/6, 2/3, 1/6, 0] - so it is a SMOOTHER even
   here, where an interpolating kernel would hand the position through
   untouched, and the fourth tap's weight is fraction^3/6 = 0, so each
   sequence still needs only the one position beyond the samples it
   asserts.  The tap before the first position falls back to the first, as
   the oscillator does.  The two-away taps are exercised at a fractional
   phase at the end of main(), where they carry weight. */
static void expect(struct sc88_oscillator *oscillator,
                   const int *values, size_t count)
{
  size_t i;
  float sample;
  for (i = 0; i + 1 < count; ++i) {
    double want = (values[i ? i - 1 : 0] + 4.0 * values[i] +
                   values[i + 1]) / 6.0;
    assert(oscillator_next(oscillator, &sample));
    assert(fabs((double)sample * 8388608.0 - want) < 1e-4);
  }
}

/* HORN_B'S KEY-60 ZONE, THE SHORTEST LOOP ANYTHING AUDIBLE USES (emusc-match
   TASK-162).  The descriptor at SC88-CTL 0x3820c is a forward loop with
   A = 0x0fbc41, B = 0x0fbc60, C = 0x0fbcb5, root key 66 and a base pitch
   correction of -136 in the 16384-units-per-octave domain - 86 logical
   samples of loop, 2.7 ms at its own rate, so whatever the wrap does recurs
   370 times a second and is heard as a buzz rather than a pop.

   The ROM decides what the wrap does, at `sc88_oscillator_wrapped_phase`.
   These two cases are the arithmetic that decision rests on, on the real
   geometry, so it cannot quietly reopen:

   - at the root key the descriptor's own correction has to land the 86-sample
     loop on that root key's pitch, and it only does so if the traversal lasts
     `86 / step` output samples.  Clearing the phase makes it 87 and puts the
     zone ten cents flat of the F# it is cut for;
   - at key 60, the case the task names, the renderer's pitch word is 0x364a6
     and the traversal is 115.651 output samples.  Clearing the phase makes it
     exactly 116, which is a sustain 5.22 cents flat of the attack.

   The oscillator is stepped for real rather than having its phase inspected,
   because it is the sounding period that the ROM constrains. */
static double traversal(uint32_t pitch_word, enum sc88_fractional_wrap wrap,
                        const int32_t *pcm, size_t count,
                        const struct sc88_wave_registers *registers)
{
  struct sc88_oscillator oscillator;
  double previous;
  size_t i, wraps = 0, first = 0, last = 0;
  float sample;

  assert(oscillator_init(&oscillator, pcm, count, registers->start,
                         registers, SC88_WAVE_FORWARD_LOOP, pitch_word,
                         32000.0, wrap));
  assert(oscillator.cycle_count == 86);
  assert(oscillator.initial_count == 117);
  previous = oscillator.phase;
  for (i = 0; i < 32000 * 2; ++i) {
    assert(oscillator_next(&oscillator, &sample));
    if (!oscillator.initial && oscillator.phase < previous) {
      ++wraps;
      if (wraps == 1)
        first = i;
      last = i;
    }
    previous = oscillator.phase;
  }
  assert(wraps > 100);
  return (double)(last - first) / (double)(wraps - 1);
}

static void short_loop_wrap(void)
{
  /* Addresses are the descriptor's; the values are not, because what these
     cases fix is the POSITION progression and not what is read at it. */
  static const uint32_t start = 0x0fbc41, loop = 0x0fbc60, end = 0x0fbcb5;
  const struct sc88_wave_registers registers = {0, start, loop, end, 0, 0x18};
  int32_t pcm[117];
  size_t i;
  double period;
  /* The root key's own rate: the pitch word is the unity anchor plus the
     descriptor's +4 correction, and nothing else. */
  const uint32_t root_word = 0x38000u - 136u;
  /* What the renderer computes for program 60, variation 1, key 60. */
  const uint32_t key60_word = 0x364a6u;
  const double fsharp4 = 369.99442271163446;

  assert(end - loop + 1 == 86);
  assert(end - start + 1 == 117);
  for (i = 0; i < sizeof pcm / sizeof pcm[0]; ++i)
    pcm[i] = (int32_t)(i * 1000);

  /* The +4 correction tunes this loop to its own root key, and only if the
     traversal is 86/step output samples. */
  period = traversal(root_word, SC88_WRAP_FULL_CARRY, pcm,
                     sizeof pcm / sizeof pcm[0], &registers);
  assert(fabs(period - 86.0 / pow(2.0, -136.0 / 16384.0)) < 1e-3);
  assert(fabs(1200.0 * log2((32000.0 / period) / fsharp4)) < 0.25);

  /* Cleared, the traversal has to end on an output sample, and the zone is
     ten cents flat of the key it is cut for. */
  period = traversal(root_word, SC88_WRAP_FULL_RESET, pcm,
                     sizeof pcm / sizeof pcm[0], &registers);
  assert(fabs(period - 87.0) < 1e-9);
  assert(1200.0 * log2((32000.0 / period) / fsharp4) < -10.0);

  /* Key 60, the case the task names. */
  period = traversal(key60_word, SC88_WRAP_FULL_CARRY, pcm,
                     sizeof pcm / sizeof pcm[0], &registers);
  assert(fabs(period - 115.65092) < 1e-3);
  period = traversal(key60_word, SC88_WRAP_FULL_RESET, pcm,
                     sizeof pcm / sizeof pcm[0], &registers);
  assert(fabs(period - 116.0) < 1e-9);

  /* Dropping only the integer part of the overshoot is the same as carrying
     it whenever the rate is at or below unity, which every key in this zone
     is; it separates from carrying only above. */
  assert(fabs(traversal(key60_word, SC88_WRAP_FRACTION_ONLY, pcm,
                        sizeof pcm / sizeof pcm[0], &registers) -
              115.65092) < 1e-3);
}

int main()
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
  size_t i;

  assert(fabs(pitch_word_rate(0x38000, 32000.0) - 1.0) < 1e-12);
  assert(fabs(pitch_word_rate(0x3c000, 32000.0) - 2.0) < 1e-12);
  assert(oscillator_init(&oscillator, pcm, 5, 8, &registers,
                         SC88_WAVE_FORWARD_LOOP, 0x38000, 32000.0,
                         SC88_WRAP_FULL_CARRY));
  expect(&oscillator, forward, sizeof forward / sizeof forward[0]);

  assert(oscillator_init(&oscillator, pcm, 5, 8, &registers,
                         SC88_WAVE_PING_PONG_LOOP, 0x38000, 32000.0,
                         SC88_WRAP_FULL_CARRY));
  expect(&oscillator, ping_pong, sizeof ping_pong / sizeof ping_pong[0]);

  assert(oscillator_init(&oscillator, closed, 5, 8, &registers,
                         SC88_WAVE_PING_PONG_LOOP, 0x38000, 32000.0,
                         SC88_WRAP_FULL_CARRY));
  expect(&oscillator, closed_ping_pong,
         sizeof closed_ping_pong / sizeof closed_ping_pong[0]);

  assert(oscillator_init(&oscillator, pcm, 5, 8, &registers,
                         SC88_WAVE_FORWARD_ONE_SHOT, 0x38000, 32000.0,
                         SC88_WRAP_FULL_CARRY));
  expect(&oscillator, one_shot, sizeof one_shot / sizeof one_shot[0]);
  assert(!oscillator_next(&oscillator, &sample));

  assert(oscillator_init(&oscillator, pcm, 5, 8, &registers,
                         SC88_WAVE_FORWARD_LOOP, 0x34000, 32000.0,
                         SC88_WRAP_FULL_CARRY));
  /* Half-step.  The first read sits on position 0 with the tap before it
     folded back, weights [1/6, 2/3, 1/6, 0] over 8, 8, 9 and a zero.  The
     second sits exactly between 0 and 1, where all four weights are
     nonzero - [1/48, 23/48, 23/48, 1/48] over the folded 8, then 8, 9 and
     position 2's 10.  A kernel that read only three taps, or one that
     passed position 0 through at fraction 0, fails both. */
  assert(oscillator_next(&oscillator, &sample));
  assert(fabs((double)sample * 8388608.0 - (8 + 4 * 8 + 9) / 6.0) < 1e-4);
  assert(oscillator_next(&oscillator, &sample));
  assert(fabs((double)sample * 8388608.0 -
              (8 + 23 * 8 + 23 * 9 + 10) / 48.0) < 1e-4);

  /* THE TWO-AWAY FORWARD TAP, which the three-tap read never had to
     answer.  Half-step through a one-shot: at phase 3.5 the tap at
     index + 2 is position 5, past the note's last position, and the
     one-shot holds there exactly as the mandatory tap at index + 1
     already did.  At 4.5, the last read before the note ends, both
     forward taps are that held last position. */
  assert(oscillator_init(&oscillator, pcm, 5, 8, &registers,
                         SC88_WAVE_FORWARD_ONE_SHOT, 0x34000, 32000.0,
                         SC88_WRAP_FULL_CARRY));
  for (i = 0; i < 8; ++i)
    assert(oscillator_next(&oscillator, &sample));
  assert(fabs((double)sample * 8388608.0 -
              (10 + 23 * 11 + 23 * 12 + 12) / 48.0) < 1e-4);
  for (i = 0; i < 2; ++i)
    assert(oscillator_next(&oscillator, &sample));
  assert(fabs((double)sample * 8388608.0 -
              (11 + 23 * 12 + 23 * 12 + 12) / 48.0) < 1e-4);
  assert(!oscillator_next(&oscillator, &sample));

  /* THE TWO-AWAY BACKWARD TAP AT THE LOOP SEAM.  Every pitch word that is
     a whole number of octaves lands on fraction 0 exactly where the cycle
     restarts, so standing at a FRACTIONAL phase on cycle position zero -
     the one place the backward tap is not simply index - 1 - means putting
     the oscillator there.  Position zero is address b (value 10) and its
     predecessor is the cycle's LAST position, address c (value 12), not a
     fold back onto position zero: at fraction 1/4 the weights are
     [27, 235, 121, 1]/384, so folding would read 10 there and land on
     3963/384 instead of 4017/384 - a step of 0.14 into every loop turn. */
  assert(oscillator_init(&oscillator, pcm, 5, 8, &registers,
                         SC88_WAVE_FORWARD_LOOP, 0x38000, 32000.0,
                         SC88_WRAP_FULL_CARRY));
  oscillator.initial = false;
  oscillator.phase = 0.25;
  assert(oscillator_next(&oscillator, &sample));
  assert(fabs((double)sample * 8388608.0 -
              (27 * 12 + 235 * 10 + 121 * 11 + 12) / 384.0) < 1e-4);

  short_loop_wrap();
  return 0;
}
