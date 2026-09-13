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

/* THE THREE-POINT READ, and the response is measured rather than chosen.

   The wave is read with a kernel, and a kernel is one fixed filter in the
   WAVE's own time base.  A tone transposed by `ratio` puts output
   frequency f at the normalised wave frequency

       theta = f / (32000 * ratio)      cycles per wave sample

   so two tones at different transpositions reach the same theta from
   different output frequencies, and a real kernel has to give them the
   same answer.  That over-determination is what separates a measurement
   from a curve fitted to a residual.

   63 archive single notes against our render, every band the reference
   carries above its own noise floor, 1101 points.  At FIXED theta the
   excess does not depend on output frequency at all - the slope is +0.15,
   -0.02 and -0.95 dB per octave in three theta bands, every |t| <= 0.3,
   over f from 726 Hz to 13.5 kHz.  At FIXED output frequency it depends
   strongly on the transposition: -2.76 dB per octave of ratio from 5 to
   9.5 kHz and -5.36 above.  Fitting both at once over every band above
   1 kHz gives +0.90 +- 0.21 dB per octave of frequency and -1.25 +- 0.43
   per octave of ratio; an output-stage response requires the second to be
   zero and it is 2.9 sigma away, while a kernel in the wave's time base
   requires it to be the negative of the first, which it is.

   The reference's own response then follows from ours, because ours is
   known exactly: the two-point linear read is the unit triangle, whose
   power response is 40*log10|sinc(theta)|.  Recovered on the r < 0.90
   group alone and expressed against theta, it PREDICTS the held-out
   0.90 <= r < 1.25 group to a mean 0.81 dB over nine shared bins, eight of
   nine 68 % intervals overlapping, at output frequencies an octave apart
   - and the same points binned by output frequency instead miss by
   2.56 dB with none of ten overlapping.

       theta         0.12   0.16   0.20   0.24   0.28   0.32   0.43   0.50
       reference dB  -0.74  -1.02  -1.64  -2.45  -3.78  -5.12  -9.74 -11.08
       our linear    -0.43  -0.75  -1.18  -1.71  -2.35  -3.10  -5.79  -7.53

   Written as sinc**n the recovered curve is n = 3.25 [2.99, 3.42] at 68 %,
   resampled over tones.  Two-point linear is n = 2.  The same recovery run
   on renders made with n = 3 and with n = 4 returns 3.24 and 3.34, so the
   curve does not move with the kernel that produced the residual, which a
   fit to that residual would.

   The kernel below is the three-tap quadratic B-spline, which is exactly
   n = 3 and the shortest kernel inside the interval.  It is a CHOICE among
   the kernels that realise the measured response, not itself a recovered
   fact: the control ROM holds no interpolation coefficients (no
   phase-indexed unity-summing table exists in it under any of four numeric
   readings, against a search that recovers three of three and four of four
   planted ones), and the 28-register XP voice upload has no coefficient
   bank and no interpolation-mode field.  The interpolation is XP silicon
   and the response is what this project can have.  Identical-XP measurement
   M-087 preferred linear over four-point Hermite and windowed sinc, but
   every candidate it tried was SHARPER than linear; the SC-88's is softer.

   Whole board, 63 single notes against the archive, wet at CC91 24:
   median MAD 1.2 -> 0.9 dB, within 3 dB 57 -> 59 of 63, past 6 dB 2
   unchanged, tilted bright 8 -> 3, no drift outlier either way.  Dry, the
   same 63 read median MAD 1.5 -> 1.0 and tilted bright 11 -> 6, but within
   3 dB 53 -> 51: the tones it moves the wrong way are the struck ones
   whose defect is their decay, not their brightness.  Per-band median
   residual, ours minus the archive, 3.1 to 13.5 kHz:

       linear sinc^2   0.2  0.3  0.4  0.7  1.1  1.9  2.8  4.8 dB
       this   sinc^3   0.1  0.0 -0.0  0.1  0.1  0.7  0.8  2.0
       cubic  sinc^4  -0.2 -0.2 -0.5 -0.9 -0.8 -1.0 -1.5 -1.1

   and the correlation of the excess against log2(ratio), which is what
   named the wave's time base in the first place, collapses with it: -0.29
   to -0.13 at 11.0 kHz and -0.32 to -0.05 at 13.5.  So the answer is
   bracketed and not merely improved.  The remaining
   +2.0 dB at 13.5 kHz is the n = 3.25 the curve actually wants, and above
   theta 0.6 - deep in the imaging region, eight points - the reference
   falls off faster still than any single exponent.  [MEASURED].  */

/* The position before this one.  Inside a cycle, position zero's
   predecessor is the cycle's LAST position and not something before the
   note: a three-tap read that folded back there would put a step into
   every loop turn, at the loop's own rate.  Before the note's first
   position there is genuinely nothing, and the caller folds back. */
static bool sc88_oscillator_previous(const struct sc88_oscillator *oscillator,
                                     size_t index, size_t *out)
{
  if (index > 0) {
    *out = index - 1;
    return true;
  }
  if (oscillator->initial || !oscillator->cycle_count)
    return false;
  *out = oscillator->cycle_count - 1;
  return true;
}

/* An outer tap of the three-point read.  It is optional: when it falls
   before the first position, or on an address the zone does not contain,
   the centre tap stands in.  The two inner taps stay mandatory so the
   one-shot termination test is exactly the one the two-point read used. */
static double sc88_oscillator_outer(const struct sc88_oscillator *oscillator,
                                    size_t index, bool back, double centre)
{
  size_t at;
  double value;

  if (back) {
    if (!sc88_oscillator_previous(oscillator, index, &at))
      return centre;
  } else {
    at = index + 1;
  }
  return sc88_oscillator_value(oscillator, at, &value) ? value : centre;
}

bool sc88_oscillator_next(struct sc88_oscillator *oscillator, float *sample)
{
  size_t index;
  double fraction;
  double value0;
  double value1;
  double left;
  double centre;
  double right;
  double offset;

  if (!oscillator || !sample || oscillator->ended)
    return false;
  index = (size_t)floor(oscillator->phase);
  fraction = oscillator->phase - (double)index;
  if (!sc88_oscillator_value(oscillator, index, &value0) ||
      !sc88_oscillator_value(oscillator, index + 1, &value1))
    return false;
  /* Three-point read, centred on whichever of the two inner positions the
     phase is nearer, so `offset` is the signed distance to the centre and
     the weights below are symmetric in it. */
  if (fraction < 0.5) {
    offset = fraction;
    centre = value0;
    right = value1;
    left = sc88_oscillator_outer(oscillator, index, true, centre);
  } else {
    offset = fraction - 1.0;
    centre = value1;
    left = value0;
    right = sc88_oscillator_outer(oscillator, index + 1, false, centre);
  }
  *sample = (float)((0.5 * (0.5 - offset) * (0.5 - offset) * left +
                     (0.75 - offset * offset) * centre +
                     0.5 * (0.5 + offset) * (0.5 + offset) * right) /
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
