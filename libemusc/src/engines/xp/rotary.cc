/* SPDX-License-Identifier: CC0-1.0 */
#include "rotary.h"

#include "common/constants.h"
#include "efx.h"

#include <cmath>
#include <cstring>

namespace EmuSC { namespace Xp {

namespace {

/* The profile's tables this effect reads (devices/jv1080.cc). */
const unsigned kLevelTable = 0u;     /* 0x03856C */
const unsigned kSpeedTable = 6u;     /* 0x038C2E */
const unsigned kAccelTable = 30u;    /* 0x03EEE8 */

const unsigned kRotaryType = 7u;     /* 0-based; loads bank slot 13 */

const unsigned kHiSlow = 0u, kLowSlow = 1u, kHiFast = 2u, kLowFast = 3u,
  kSpeed = 4u, kHiAccl = 5u, kLowAccl = 6u, kHiLvl = 7u, kLowLvl = 8u,
  kLevel = 10u;

/* Rotor 0 is the horn (XP 0x3338, ramp 0x3928), rotor 1 the drum (0x333A,
   ramp 0x392A). */
const unsigned kHorn = 0u, kDrum = 1u;

/* The updater indexes the 126-entry speed table raw, and the 16-entry
   accel table raw; the rest take a 7-bit value. Separation (byte 9) is
   accepted and not modelled (rotary.h). */
const uint8_t kMax[XP_ROTARY_PARAMETERS] = {
  125u, 125u, 125u, 125u, 1u, 15u, 15u, 127u, 127u, 127u, 127u
};

/* Slot 13's image: the biquad and the one-pole the updater never writes. */
const unsigned kBiquad = 13u;        /* b0 b1 b2 a1 a2 */
const unsigned kLowpass = 26u;       /* b1 b0 a1 */

/* The mono sum instructions 1..2 form, CRAM +0.5 on each input. */
const float kInputWeight = 0.5f;

const double kUnity = 8192.0;
const double kRegisterUnity = 512.0;

/* Rotor Hz = register * 32000 / 2^20: the speed table's 2..328 is the
   manual's 0.05..10 Hz to within the table's own rounding (328 = 327.68).
   Measured on the drum: 0.0923 Hz fitted against 0.0916 from word 3, and a
   fast target of 7.02 Hz against 6.90 from word 226 (the fitted target sits
   1.8 % high; the table law is kept). P-xxxx. */
const double kNativeRate = 32000.0;
const double kTurnsPerWord = 1.0 / 1048576.0;   /* per sample */

/* THE FIXED TAPS, the four ERAM read addresses of slot 13 (FW-STRUCT), and
   which side each reaches (MEASURED: `efx_probe` noise burst, one tap per
   side, 123 and 328 left, 901 and 1188 right). */
const float kHornTap[2] = { 123.0f, 901.0f };
const float kDrumTap[2] = { 328.0f, 1188.0f };

/* MEASURED, NOT READ. Everything below was fitted to the corpus, and the
   fits are described where they are used (rotary_process). P-xxxx. */

/* The program's latency against a dry path, on every tap. */
const float kLatency = 3.53f;

/* Doppler: delay = depth ((1 + cos phi) / 2)^shape, per rotor. The horn's
   is the noise-burst fit's: a sinusoid (shape 1) leaves -7.9/-6.0 dB there
   against -10.0/-6.5 and needs an AM deeper than the tap's own gain. The
   drum's is the slow-sine fit's, where a sinusoid is within 0.4 dB of the
   best exponent (0.86) and 1.37 costs 2.9 dB; its depth sits among the
   other fits' 38.0 (burst) and 40.4 (fast sine). */
const float kDepth[2] = { 22.4f, 39.0f };
const float kShape[2] = { 1.37f, 1.0f };

/* Tap gains, relative to 0.5 (L + R) through the ROM filter and CRAM
   19/30 and before the 0x3334 level. Drum: the slow-sine fit, calibrated
   against `basic/single_note_dry` (hw = ours - 4.80 dB). Horn: the burst
   fit rescaled by the ratio the drum's fixed taps give between the two
   takes (0.312 left, 0.322 right) and by CRAM 30 over CRAM 19 at the
   factory 127/96, since the burst fit carried neither. The horn's sign is
   negative against the drum's. */
const float kHornDopplerGain = -0.780f;
const float kHornFixedGain = -1.064f;
const float kDrumDopplerGain = 1.301f;
const float kDrumFixedGain[2] = { 0.774f, 0.763f };

/* The horn's AM, as 1 + depth cos(phi + phase), phi the side's own rotor
   angle with the doppler longest at phi = 0. The drum shows none: a free
   AM on its doppler tap fits to depth 0.005 over 27 s of slow rotation. */
const float kHornDopplerAm = 0.717f;
const float kHornDopplerAmPhase = 2.326f;
const float kHornFixedAm = 0.461f;
const float kHornFixedAmPhase = 1.644f;

/* The speed register approaches its target exponentially, with time
   constant 0.861 s at accel word 10 (byte 6 = 9): the drum's spin-up and
   spin-down in `closing/efx_sweep_08_rotary`, fitted jointly. Scaled as
   1/word for every other word, which is the assumption rotary.h states. */
const double kRampPerWord = 1.0 / (0.861 * 10.0 * kNativeRate);

/* The rotors' angles when the effect loads, in turns of the angle the
   doppler above is written in. The drum's comes out the same in two
   independent takes fitted from their own load: 0.482 (`efx_probe` burst)
   and 0.474 (`efx_params` sine), so it is carried as a property of the
   load, not of a take; the horn's is the burst's alone. Half a turn is the
   doppler's shortest point: both rotors start near it. */
const double kStartTurns[2] = { 0.350, 0.478 };

const double kTwoPi = 6.283185307179586;
const uint32_t kLineMask = XP_ROTARY_LINE - 1u;

double xp(uint16_t raw)
{
  int value = raw & 0x3fff;
  if (value & 0x2000)
    value -= 0x4000;
  return (double)value * (double)(1u << kXpCoefficientShift[raw >> 14]) / 8192.0;
}

inline float tap(const float *line, uint32_t pos, float delay)
{
  float at = (float)pos - delay;
  float base = std::floor(at);
  float frac = at - base;
  uint32_t i = (uint32_t)(int32_t)base & kLineMask;
  return line[i] + frac * (line[(i + 1u) & kLineMask] - line[i]);
}

/* `0x0A0020CC(control, speed ? 127 : 0, 127)`: the sum of two signed
   bytes, clamped to 0..127. */
int clamp_sum(int control, uint8_t speed)
{
  int v = (int)(int8_t)control + (speed ? 127 : 0);
  if (v > 127)
    return 127;
  return v < 0 ? 0 : v;
}

void retarget(struct xp_rotary *rt)
{
  for (unsigned r = 0; r < 2u; ++r)
    rt->target[r] = (float)(rt->fast ? rt->fast_word[r] : rt->slow_word[r]);
}

}  // namespace

bool rotary_parameter_valid(unsigned index, uint8_t value)
{
  return index < XP_ROTARY_PARAMETERS && value <= kMax[index];
}

bool rotary_speed_fast(uint8_t speed, int control)
{
  return clamp_sum(control, speed) > 64;
}

bool rotary_control_fast(uint8_t speed, int control)
{
  return clamp_sum(control, speed) > 63;
}

double rotary_speed_hz(double word)
{
  return word * kNativeRate * kTurnsPerWord;
}

float rotary_doppler(unsigned rotor, float phi)
{
  if (rotor > 1u)
    return 0.0f;
  float c = 0.5f * (1.0f + std::cos(phi));
  if (c <= 0.0f)
    return 0.0f;
  return kDepth[rotor] * (kShape[rotor] == 1.0f ? c : std::pow(c, kShape[rotor]));
}

bool rotary_set(const struct xp_rom *rom, struct xp_rotary *rt,
                const uint8_t p[XP_ROTARY_PARAMETERS])
{
  if (!rom || !rom->bytes || !rt)
    return false;
  for (unsigned i = 0; i < XP_ROTARY_PARAMETERS; ++i)
    if (!rotary_parameter_valid(i, p[i]))
      return false;
  struct xp_efx_program prog;
  if (!efx_program_load(rom, kRotaryType, &prog))
    return false;
  uint16_t slow[2] = { 0, 0 }, fast[2] = { 0, 0 }, accel[2] = { 0, 0 };
  uint16_t hi = 0, lo = 0, level = 0;
  if (!efx_table_value(rom, kSpeedTable, p[kHiSlow], 0u, &slow[kHorn]) ||
      !efx_table_value(rom, kSpeedTable, p[kLowSlow], 0u, &slow[kDrum]) ||
      !efx_table_value(rom, kSpeedTable, p[kHiFast], 0u, &fast[kHorn]) ||
      !efx_table_value(rom, kSpeedTable, p[kLowFast], 0u, &fast[kDrum]) ||
      !efx_table_value(rom, kAccelTable, p[kHiAccl], 0u, &accel[kHorn]) ||
      !efx_table_value(rom, kAccelTable, p[kLowAccl], 0u, &accel[kDrum]) ||
      !efx_table_value(rom, kLevelTable, p[kHiLvl], 0u, &hi) ||
      !efx_table_value(rom, kLevelTable, p[kLowLvl], 0u, &lo) ||
      !efx_table_value(rom, kLevelTable, p[kLevel], 0u, &level))
    return false;

  /* Two delay lines make a copy large, so the fields are written in place,
     and only once every read above has succeeded: a failure leaves `*rt`
     untouched. A control offset set before the first build is kept. */
  bool fresh = !rt->ready;
  if (fresh) {
    int8_t control = rt->control;
    std::memset(rt, 0, sizeof *rt);
    rt->control = control;
  }
  rt->hp_b0 = (float)xp(prog.cram[kBiquad]);
  rt->hp_b1 = (float)xp(prog.cram[kBiquad + 1u]);
  rt->hp_b2 = (float)xp(prog.cram[kBiquad + 2u]);
  rt->hp_a1 = (float)xp(prog.cram[kBiquad + 3u]);
  rt->hp_a2 = (float)xp(prog.cram[kBiquad + 4u]);
  rt->lp_b1 = (float)xp(prog.cram[kLowpass]);
  rt->lp_b0 = (float)xp(prog.cram[kLowpass + 1u]);
  rt->lp_a1 = (float)xp(prog.cram[kLowpass + 2u]);
  /* CRAM 19/30 take the level word shifted right once, an arithmetic
     shift of a non-negative word. */
  rt->hi_gain = (float)((double)(hi >> 1) / kUnity);
  rt->lo_gain = (float)((double)(lo >> 1) / kUnity);
  rt->level = (float)((double)(level >> 4) / kRegisterUnity);
  for (unsigned r = 0; r < 2u; ++r) {
    rt->slow_word[r] = slow[r];
    rt->fast_word[r] = fast[r];
    rt->ramp[r] = (float)(accel[r] * kRampPerWord);
  }
  rt->fast = rotary_speed_fast(p[kSpeed], rt->control);
  retarget(rt);
  if (fresh)
    for (unsigned r = 0; r < 2u; ++r) {
      rt->speed[r] = rt->target[r];
      rt->phase[r] = kStartTurns[r];
    }
  std::memcpy(rt->param, p, sizeof rt->param);
  rt->ready = true;
  return true;
}

void rotary_control(struct xp_rotary *rt, int control)
{
  rt->control = (int8_t)control;
  if (!rt->ready)
    return;
  rt->fast = rotary_control_fast(rt->param[kSpeed], control);
  retarget(rt);
}

/* HOW FAR THIS IS MEASURED. Three corpus takes, each through this model
 * with the stimulus's own dry render (our engine, type unwired, is dry):
 *
 *   `efx_probe/efx08_rotary` noise burst, 1.43 s at factory speeds: a
 *   least-squares FIR per 60 ms window shows the four fixed taps at the ROM
 *   addresses + 3-4 samples and the three moving ones near zero; the whole
 *   model fitted to the burst leaves -10.0 dB (L) and -6.5 dB (R) of
 *   residual, which is the floor a free 250-tap FIR per 100 ms window
 *   reaches on the same data (-6.5 to -12.1 dB). With the horn's shape at
 *   1 the best of three seeds leaves -7.9/-6.0 dB, at 2 -9.0/-6.2.
 *
 *   `efx_params/efx08_rotary`, 262 Hz sine, eleven slow slots over 27 s:
 *   the drum's slow rotation 0.0923 Hz; its gains, depth and shape, -19.7
 *   dB residual on the complex envelopes.
 *
 *   `closing/efx_sweep_08_rotary` Speed 1 then back to 0: one time
 *   constant, 0.861 s, fits both the spin-up and the spin-down; -12.9 dB
 *   residual with no drum AM.
 *
 * AND WHAT IT REPRODUCES THAT WAS NOT FITTED, run from each take's own
 * load with the start angles and speed law above (hw = ours - 4.80 dB):
 * the twelve `efx_sweep` slots' levels within 0.9 dB over 25 s of drum
 * rotation; the characterisation row (`efx_probe`, sustained sine) at
 * -44.8 dB against -44.7, AM 4.5 dB against 6.5 at the same 0.20 Hz, L/R
 * +1.1 dB against +1.3, and 11.4 dB of the characterisation's sample-wise
 * L-R measure against 14.1. Short: the fast slot's modulation, 134 cents
 * and 5.0 dB against 171 and 8.5, and the slot after it, which inherits
 * the fast rate's phase error (the fitted target, 7.02 Hz, sits 1.8 % above
 * the table's). P-xxxx. */
void rotary_process(struct xp_rotary *rt, const float *inL, const float *inR,
                    float *outL, float *outR, size_t frames)
{
  for (size_t k = 0; k < frames; ++k) {
    float x = kInputWeight * (inL[k] + inR[k]);
    float h = rt->hp_b0 * x + rt->hp_b1 * rt->hp_x1 + rt->hp_b2 * rt->hp_x2 +
      rt->hp_a1 * rt->hp_y1 + rt->hp_a2 * rt->hp_y2;
    rt->hp_x2 = rt->hp_x1;
    rt->hp_x1 = x;
    rt->hp_y2 = rt->hp_y1;
    rt->hp_y1 = h;
    float l = rt->lp_b0 * x + rt->lp_b1 * rt->lp_x1 + rt->lp_a1 * rt->lp_y1;
    rt->lp_x1 = x;
    rt->lp_y1 = l;
    rt->line_hi[rt->pos] = rt->hi_gain * h;
    rt->line_lo[rt->pos] = rt->lo_gain * l;

    for (unsigned r = 0; r < 2u; ++r) {
      rt->speed[r] += (rt->target[r] - rt->speed[r]) * rt->ramp[r];
      rt->phase[r] += (double)rt->speed[r] * kTurnsPerWord;
      if (rt->phase[r] >= 1.0)
        rt->phase[r] -= 1.0;
    }

    float drumPhi = (float)(kTwoPi * rt->phase[kDrum]);
    float drum = kDrumDopplerGain *
      tap(rt->line_lo, rt->pos, kLatency + rotary_doppler(kDrum, drumPhi));
    float out[2];
    for (unsigned s = 0; s < 2u; ++s) {
      /* the horn's two sides half a turn apart */
      float phi = (float)(kTwoPi * (rt->phase[kHorn] + 0.5 * s));
      float dop = kHornDopplerGain *
        (1.0f + kHornDopplerAm * std::cos(phi + kHornDopplerAmPhase)) *
        tap(rt->line_hi, rt->pos, kLatency + rotary_doppler(kHorn, phi));
      float fix = kHornFixedGain *
        (1.0f + kHornFixedAm * std::cos(phi + kHornFixedAmPhase)) *
        tap(rt->line_hi, rt->pos, kLatency + kHornTap[s]);
      float low = kDrumFixedGain[s] *
        tap(rt->line_lo, rt->pos, kLatency + kDrumTap[s]);
      out[s] = rt->level * (dop + fix + drum + low);
    }
    outL[k] = out[0];
    outR[k] = out[1];
    rt->pos = (rt->pos + 1u) & kLineMask;
  }
}

}}  // namespace EmuSC::Xp
