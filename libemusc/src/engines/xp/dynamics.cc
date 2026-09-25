/* SPDX-License-Identifier: CC0-1.0 */
#include "dynamics.h"

#include "common/constants.h"
#include "devices/profile.h"
#include "efx.h"

#include <cmath>
#include <cstring>

namespace EmuSC { namespace Xp {

namespace {

/* The profile's tables this effect reads (devices/jv1080.cc). */
const unsigned kLevelTable = 0u;      /* 0x03856C */
const unsigned kPanTable = 13u;       /* 0x0392A0, (L, R) */
const unsigned kGainTable = 17u;      /* 0x03EBB8 */
const unsigned kLowShelfTable = 21u;  /* 0x039802, 31 x 3 */
const unsigned kHighShelfTable = 22u; /* 0x0398BC, 31 x 3 */

/* CRAM 76 for PostGain 0..3, as the updaters write it: x1, x2, x4, x8.
   The machine steps 6.02, 6.03 and 6.03 dB (LIMITER, `closing/
   efx_sweep_10`) and 6.01, 6.03, 6.03 (COMPRESSOR, `_09`). */
const uint16_t kPostGain[4] = { 0x5000u, 0x9000u, 0xC800u, 0xD000u };

/* The owner's manual's ratios, p.87. Only the last is measured (below). */
const float kRatio[4] = { 1.5f, 2.0f, 4.0f, 100.0f };

/* Stored ranges, as the machine accepts them (`M-092`). */
const uint8_t kCompressorMax[XP_COMPRESSOR_PARAMETERS] =
  { 127u, 127u, 127u, 3u, 30u, 30u, 127u };
const uint8_t kLimiterMax[XP_LIMITER_PARAMETERS] =
  { 127u, 127u, 3u, 127u, 3u, 30u, 30u, 127u };

/* The 9-bit level registers take a table word shifted right four. */
const double kRegisterUnity = 512.0;

/* @provenance class=MEASURED devices=JV-1080 ref=closing/efx_sweep_10_limiter
   THE THRESHOLD (`closing/efx_sweep_10_limiter`).

   The ROM `Sine` at key 60, flat voice, tone level 127, into the LIMITER
   at Ratio 3, PostGain 2, Threshold stepped 0 / 32 / 64 / 95 / 127, reads
   -60.02, -45.12, -30.03, -22.64 and -22.64 dB. The two top steps are the
   unlimited level, the same as `efx_transfer/efx10`'s top step in the same
   session. The three limited steps are 14.90 and 15.09 dB apart where the
   Threshold table moves 15.12 dB and a 100:1 law predicts 14.97, and each
   one alone puts the knee at a pre-gain of 0.2086, 0.2056 and 0.2070 of
   the note's own envelope: 0.2071 is taken, a spread of 0.12 dB.

   The COMPRESSOR knees at the same place. Its Attack byte is the same
   pre-gain, and its static curve (below) sits on the LIMITER's 100:1 line
   through that knee to within 0.2 dB up to 15 dB of reduction.

   In this engine's units the reference note's steady envelope, taken by
   the detector below, is 0.042154 (its peak 0.04330), so the threshold is
   0.2071 x 0.042154. This rests on the engine's dry level standing to the
   machine's as it does: the offset is -4.75 dB on every take read here
   (`efx_transfer/efx01`, `05`, `06`, `closing/efx_sweep_01`,
   `basic/single_note_dry`). Whether the detector reads peak or average is
   not separable on a sine: this threshold is for the peak-type detector
   below, and a signal with a different crest factor is where the choice
   shows. */
const float kThreshold = 0.2071f * 0.042154f;

/* THE PATH GAINS, MEASURED (`P-xxxx`). With the pan table's centre
   (377/512) and the Level table's word taken out, the LIMITER's unlimited
   return reads +0.04 dB against its input and the COMPRESSOR's
   below-threshold return +6.10 dB against its pre-gained input. They are
   taken as x1 and x2. */
const float kLimiterPathGain = 1.0f;
const float kCompressorPathGain = 2.0f;

/* THE COMPRESSOR'S STATIC CURVE, MEASURED AT SUSTAIN 127 (`P-xxxx`,
   `efx_transfer/efx09_compressor` and `closing/efx_sweep_09`). Gain
   reduction in dB against the envelope's dB over the threshold. Every
   point is a steady level of one note, the two takes together giving
   Attack 0, 32, 64, 95, 100 and 127 at full input and input -28.94,
   -16.90, -9.86, -4.86 dB at Attack 100. Sweep Attack 64 and transfer
   step 2 land 0.1 dB apart in pre-gained level and read the same -46.88
   dB, which is what says Attack is a pure pre-gain.

   Near the knee this is the LIMITER's 100:1 line; above 15 dB it opens to
   about 9:1, which the LIMITER at the same pre-gained level does not do
   (Threshold 0 reads 37.38 dB of reduction against 37.29 for 100:1). What
   in the program opens it - most likely Sustain's CRAM 29, which is the
   one detector word the two types set differently - is not established.

   The first point read 8.085 and is held to the 100:1 line's 7.97, the
   difference being inside the reading's own 0.1 dB and the excess making
   the output fall with its input. The knee is taken hard, from the
   LIMITER: -3.93 dB under it the machine already reads 0.20 dB. Past the
   last point the last slope is continued, which no capture checks. The
   curve reproduces the factory setting's 23.52 dB of compression over
   `efx_transfer`'s 28.94 dB input range as 23.70. */
struct Knot { float over, reduction; };
const Knot kCompressorCurve[] = {
  { 0.0f, 0.0f }, { 8.055f, 7.97f }, { 15.15f, 14.76f }, { 20.15f, 19.36f },
  { 22.64f, 21.59f }, { 25.01f, 23.71f }, { 37.76f, 34.64f },
};
const unsigned kCompressorKnots =
  sizeof kCompressorCurve / sizeof *kCompressorCurve;

/* THE DETECTOR - ITS STRUCTURE IS MINE.

   What it has to reproduce (`P-xxxx`, the same takes): every limited note
   opens loud and settles, with no harmonic rise at any reduction. In dB
   over the settled level at 2 / 4 / 6 / 8 / 10 / 15 / 20 / 30 ms:

     LIMITER  Threshold 0 and 32   +3.9 +2.8 +2.3 +1.7 +1.3 +0.7 +0.4 +0.2
              Threshold 64         +2.8 +2.1 +1.8 +1.3 +1.1 +0.6 +0.4 +0.1
     COMPRESSOR, full input        +7.3 +3.9 +2.8 +1.8 +1.2 +0.5 +0.2 +0.0

   The LIMITER's transient does not grow with the reduction (37 and 22 dB
   read the same), so the envelope approaches its level in the linear
   domain ahead of the gain law, not in the gain. Most of the reduction is
   in place inside 2 ms and the rest arrives over tens of ms, so the
   envelope here is a blend: an instant peak follower `P`, and `S`, a
   one-pole smoothing of `P`, as `(1 - b) P + b S`. `b` and `S`'s time
   constant are chosen per type to reproduce the rows above: b = 1/3 and
   10 ms (LIMITER, 0.18 dB rms over its three rows), b = 2/3 and 5.5 ms
   (COMPRESSOR, 0.20 dB rms). Neither pair is a ROM value, and both hold
   only at the factory time settings the takes use (Sustain 127,
   Release 0).

   THE RELEASE IS NOT RECOVERED. The takes bound it from both sides: every
   note's transient repeats exactly after a 400 ms silence at up to 37 dB
   of reduction, which a release much slower than 90 ms would not allow,
   and the harmonics stay flat at every reduction, which a release faster
   than about 30 ms would not allow (the follower's droop between peaks
   would modulate the gain at twice the note's frequency). 50 ms is taken
   inside those bounds. */
const float kLimiterBlend = 1.0f / 3.0f;
const float kLimiterAttackSeconds = 0.010f;
const float kCompressorBlend = 2.0f / 3.0f;
const float kCompressorAttackSeconds = 0.0055f;
const float kReleaseSeconds = 0.050f;

double xp(uint16_t raw)
{
  int value = raw & 0x3fff;
  if (value & 0x2000)
    value -= 0x4000;
  return (double)value * (double)(1u << kXpCoefficientShift[raw >> 14]) / 8192.0;
}

bool word(const struct xp_rom *rom, unsigned table, unsigned index,
          unsigned column, double *out)
{
  uint16_t raw = 0;
  if (!efx_table_value(rom, table, index, column, &raw))
    return false;
  *out = xp(raw);
  return true;
}

bool reg9(const struct xp_rom *rom, unsigned table, unsigned index,
          unsigned column, float *out)
{
  uint16_t raw = 0;
  if (!efx_table_value(rom, table, index, column, &raw))
    return false;
  *out = (float)((double)(raw >> 4) / kRegisterUnity);
  return true;
}

void set_shelf(struct xp_dynamics_shelf *s, const double c[3])
{
  s->b0 = (float)c[0];
  s->b1 = (float)c[1];
  s->a1 = (float)c[2];
}

inline float shelf(struct xp_dynamics_shelf *s, float x)
{
  float y = s->b0 * x + s->b1 * s->x1 + s->a1 * s->y1;
  s->x1 = x;
  s->y1 = y;
  return y;
}

float rate(float seconds)
{
  return (float)(1.0 - std::exp(-1.0 / ((double)seconds *
                                        kXpNativeRate)));
}

float compressor_reduction_db(float over)
{
  if (over <= 0.0f)
    return 0.0f;
  for (unsigned i = 1; i < kCompressorKnots; ++i)
    if (over <= kCompressorCurve[i].over) {
      const Knot &a = kCompressorCurve[i - 1];
      const Knot &b = kCompressorCurve[i];
      return a.reduction + (b.reduction - a.reduction) *
        (over - a.over) / (b.over - a.over);
    }
  const Knot &a = kCompressorCurve[kCompressorKnots - 2];
  const Knot &b = kCompressorCurve[kCompressorKnots - 1];
  return b.reduction + (b.reduction - a.reduction) *
    (over - b.over) / (b.over - a.over);
}

}  // namespace

float dynamics_threshold(void)
{
  return kThreshold;
}

bool dynamics_parameter_valid(const struct xp_rom *rom, bool limiter,
                               unsigned index, uint8_t value)
{
  unsigned n = limiter ? XP_LIMITER_PARAMETERS : XP_COMPRESSOR_PARAMETERS;
  if (index >= n)
    return false;
  const uint8_t *max = limiter ? kLimiterMax : kCompressorMax;
  if (value > max[index])
    return false;
  /* The bytes a table serves are also held to that table's length. */
  unsigned table = 0;
  bool tabled = true;
  if (index == 0u)
    table = kGainTable;
  else if (index == (limiter ? 3u : 2u))
    table = kPanTable;
  else if (index == (limiter ? 5u : 4u))
    table = kLowShelfTable;
  else if (index == (limiter ? 6u : 5u))
    table = kHighShelfTable;
  else if (index == n - 1u)
    table = kLevelTable;
  else
    tabled = false;
  if (!tabled)
    return true;
  unsigned count = 0;
  return efx_table_shape(rom, table, &count, NULL) && value < count;
}

float dynamics_static_gain(const struct xp_dynamics *dy, float envelope)
{
  if (!(envelope > kThreshold))
    return 1.0f;
  if (dy->limiter)
    return std::pow(kThreshold / envelope, dy->exponent);
  float over = 20.0f * std::log10(envelope / kThreshold);
  return std::pow(10.0f, -compressor_reduction_db(over) / 20.0f);
}

bool dynamics_set(const struct xp_rom *rom, struct xp_dynamics *out,
                   bool limiter, const uint8_t *p)
{
  if (!rom || !rom->bytes || !out || !p)
    return false;
  unsigned n = limiter ? XP_LIMITER_PARAMETERS : XP_COMPRESSOR_PARAMETERS;
  for (unsigned i = 0; i < n; ++i)
    if (!dynamics_parameter_valid(rom, limiter, i, p[i]))
      return false;
  const unsigned pan = limiter ? 3u : 2u;
  const unsigned post = limiter ? 4u : 3u;
  const unsigned low = limiter ? 5u : 4u;
  const unsigned high = limiter ? 6u : 5u;
  const unsigned level = n - 1u;

  struct xp_dynamics built;
  std::memset(&built, 0, sizeof built);
  struct xp_dynamics *dy = &built;
  double gain = 0.0;
  double lo[3], hi[3];
  if (!word(rom, kGainTable, limiter ? 127u - p[0] : p[0], 0, &gain))
    return false;
  for (unsigned k = 0; k < 3u; ++k)
    if (!word(rom, kLowShelfTable, p[low], k, &lo[k]) ||
        !word(rom, kHighShelfTable, p[high], k, &hi[k]))
      return false;
  if (!reg9(rom, kPanTable, p[pan], 0, &dy->pan_left) ||
      !reg9(rom, kPanTable, p[pan], 1, &dy->pan_right) ||
      !reg9(rom, kLevelTable, p[level], 0, &dy->level))
    return false;

  dy->limiter = limiter;
  dy->pre_gain = (float)gain;
  dy->exponent = limiter ? 1.0f - 1.0f / kRatio[p[2]] : 0.0f;
  dy->path_gain = limiter ? kLimiterPathGain : kCompressorPathGain;
  dy->post_gain = (float)xp(kPostGain[p[post]]);
  dy->attack_blend = limiter ? kLimiterBlend : kCompressorBlend;
  dy->attack_rate = rate(limiter ? kLimiterAttackSeconds
                                 : kCompressorAttackSeconds);
  dy->release_rate = rate(kReleaseSeconds);
  set_shelf(&dy->low_shelf, lo);
  set_shelf(&dy->high_shelf, hi);
  if (out->ready && out->limiter == limiter) {
    dy->peak = out->peak;
    dy->smooth = out->smooth;
    dy->low_shelf.x1 = out->low_shelf.x1;
    dy->low_shelf.y1 = out->low_shelf.y1;
    dy->high_shelf.x1 = out->high_shelf.x1;
    dy->high_shelf.y1 = out->high_shelf.y1;
  }
  std::memcpy(dy->param, p, n);
  dy->ready = true;
  *out = built;
  return true;
}

void dynamics_process(struct xp_dynamics *dy, const float *inL,
                       const float *inR, float *outL, float *outR,
                       size_t frames)
{
  const float blend = dy->attack_blend;
  const float out_gain = dy->post_gain * dy->level;
  for (size_t k = 0; k < frames; ++k) {
    /* Mono in, one pan pair out (manual p.87, and CRAM 1/2 = +0.5 each). */
    const float x = 0.5f * (inL[k] + inR[k]);
    const float u = dy->pre_gain * x;
    const float a = std::fabs(u);
    if (a > dy->peak)
      dy->peak = a;
    else
      dy->peak -= dy->release_rate * dy->peak;
    dy->smooth += dy->attack_rate * (dy->peak - dy->smooth);
    const float envelope = (1.0f - blend) * dy->peak + blend * dy->smooth;
    const float g = dynamics_static_gain(dy, envelope);
    float w = g * dy->path_gain * (dy->limiter ? x : u);
    w = shelf(&dy->low_shelf, w);
    w = shelf(&dy->high_shelf, w);
    w *= out_gain;
    outL[k] = dy->pan_left * w;
    outR[k] = dy->pan_right * w;
  }
}

}}  // namespace EmuSC::Xp
