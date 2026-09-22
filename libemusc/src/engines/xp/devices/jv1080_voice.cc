/* SPDX-License-Identifier: CC0-1.0 */
/*
 *  Roland JV-1080 behavioural voice model for the XP engine (engines/xp/).
 *
 *  Separate from jv1080.cc, which is pure profile data that the engine's
 *  identification links in unconditionally: a test that only needs this
 *  device's profile to exist should not have to link a voice model and the
 *  packed-record reader behind it.
 */
#include "jv1080.h"

/* ======================================================================
 *  The behavioural voice model.
 *
 *  Every law below carries the measurement it comes from. None of it is
 *  firmware-exact and none of it may be cited as such: this device's
 *  synthesis engine is in the SH7034's undumped 64 KB internal mask ROM,
 *  so what exists is the transfer function measured at the machine's
 *  output, not the code that produces it. Where a law is measured at too
 *  few points to pin its shape, that is written down next to it rather
 *  than smoothed over.
 * ====================================================================== */

#include "../common/constants.h"
#include "../packed_rom.h"

#include <cmath>
#include <cstring>

namespace EmuSC { namespace Xp {

namespace {

/* MEASURED (`M-018`): the A-ENV's level field is its own table, not the
   level fields' square law - the two differ by 10.08 dB at worst. These are
   the eleven points, in dB relative to value 127, read with all four times
   at zero so the note is a rectangle at one level and the envelope is
   standing still. Value 0 is the noise floor.

   Its closed form is not established; the eleven points ARE the table, and
   between them this interpolates in dB, which is interpolation and not a
   recovered law. The slope converges to 0.391 dB per step at the top and
   steepens to 0.96 dB per step at value 8, so it is neither the square law
   nor a constant-dB table. */
const struct { uint8_t value; double db; } kAmpEnvLevelTable[] = {
  {   8u, -56.65 }, {  16u, -48.98 }, {  24u, -43.60 }, {  32u, -39.24 },
  {  48u, -31.71 }, {  64u, -24.93 }, {  80u, -18.47 }, {  96u, -12.13 },
  { 112u,  -5.86 }, { 127u,   0.00 },
};

/* MEASURED (`M-015`, `M-023`): pan is a constant-power law, antisymmetric
   about value 64, and the tone and part fields index one table. These are
   the measured channel differences in dB at each distance from centre,
   after the 0.97 dB that belongs to the capture interface was cancelled by
   recording the sweep twice with L and R interchanged.

   It is a TABLE and not a trigonometric or linear law: the calibrated curve
   misses cos/sin by 2.05 dB and a linear pan by 2.07. So this interpolates
   between the measured distances rather than evaluating a formula, and the
   table itself is five points wide - its shape between them is not
   recovered. */
const struct { uint8_t distance; double difference_db; } kPanTable[] = {
  {  0u,  0.00 }, {  8u,  2.20 }, { 16u,  4.50 }, { 32u,  9.20 },
  { 48u, 15.60 }, { 64u, 65.00 },
};

double interpolate_db(const double *xs, const double *ys, unsigned count,
                       double x)
{
  if (x <= xs[0])
    return ys[0];
  for (unsigned i = 1; i < count; ++i) {
    if (x <= xs[i]) {
      double t = (x - xs[i - 1]) / (xs[i] - xs[i - 1]);
      return ys[i - 1] + t * (ys[i] - ys[i - 1]);
    }
  }
  return ys[count - 1];
}

double amp_env_level_db(unsigned value)
{
  if (!value)
    return -120.0;               /* the field's own floor is the noise floor */
  double xs[10], ys[10];
  for (unsigned i = 0; i < 10; ++i) {
    xs[i] = kAmpEnvLevelTable[i].value;
    ys[i] = kAmpEnvLevelTable[i].db;
  }
  return interpolate_db(xs, ys, 10u, (double)value);
}

double pan_difference_db(int offset)
{
  double xs[6], ys[6];
  for (unsigned i = 0; i < 6; ++i) {
    xs[i] = kPanTable[i].distance;
    ys[i] = kPanTable[i].difference_db;
  }
  double magnitude = interpolate_db(xs, ys, 6u, (double)(offset < 0 ? -offset
                                                                    : offset));
  return offset < 0 ? -magnitude : magnitude;
}

/* MEASURED (`M-009`, `M-019`, `M-048`): one square-law table, indexed by
   five different fields - tone level, patch level, part level, tone output
   level and CC7 - to a worst deviation of 0.41 dB and often under 0.15.
   `gain = (value/127)^2`. */
double square_law_gain(unsigned value)
{
  double v = (double)(value > 127u ? 127u : value) / 127.0;
  return v * v;
}

/* MEASURED (`M-011`): the A-ENV's times 2, 3 and 4 index one exponential
   table - the same table to within 5 % - and a 20 dB fall takes 50 ms at
   value 16, 675 ms at 64 and 4.3 s at 104, doubling every 13.2 value steps.
   So the field is a RATE in dB per second, and a segment's duration is how
   far it has to travel at that rate.

   The doubling interval is a fit over 24 points with a worst ratio error of
   1.42x, concentrated below value 24 where a 5 ms measurement grid and the
   note's own decay dominate. The exact table is not recovered. */
double amp_env_fall_seconds_per_20db(unsigned value)
{
  return 0.675 * std::pow(2.0, ((double)value - 64.0) / 13.2);
}

/* MEASURED, AT THREE POINTS, AND THEY DO NOT FIT ONE EXPONENTIAL
   (`M-011`). Time 1's rise, as the time to first reach 90 % of the note's
   peak, runs 35 ms at value 8, 1210 ms at 64 and 2560 ms at 80. Fitted
   between the first two that is a doubling every 10.96 steps; fitted between
   the last two it is 14.8. This uses 1210 ms at 64 doubling every 11.0,
   which reproduces value 8 to 2 % and is 30 % high at value 80.

   THE EXACT TABLE IS NOT RECOVERED, and the value here is not fitted to
   make anything match: three measured points cannot separate a table from a
   formula, and M-011 says so itself - a 0-to-90 % rise and a 20 dB fall
   measure different fractions of a segment, so the fall table above cannot
   be borrowed for this one either. What is justified is the ORDER: tens of
   milliseconds at the bottom of the field, seconds at the top. */
double amp_env_attack_seconds(unsigned value)
{
  return 1.210 * std::pow(2.0, ((double)value - 64.0) / 11.0);
}

/* MEASURED (`M-035`): wave gain is exactly the display enum,
   -6 / 0 / +6 / +12 dB, within 0.06 dB. */
double wave_gain(unsigned raw)
{
  static const double db[4] = { -6.0, 0.0, 6.0, 12.0 };
  return std::pow(10.0, db[raw & 3u] / 20.0);
}

/* MEASURED (`M-012`, `M-076`): `fc = 341 Hz * 2^((cutoff - 64)/10)`, two
   poles at -12.2 dB per octave. Ten measured points from 33 Hz at cutoff 32
   to 5939 Hz at 104, worst error 1.12x, and white noise through the same law
   predicts 0.301 dB of rms per cutoff step against a measured 0.308.

   Below cutoff about 20 the machine emits digital silence - -99.9 dBFS
   against a -99.96 floor - and so does extrapolating the law, because a
   12 Hz corner attenuates everything audible by over 100 dB. The two cannot
   be told apart, so the law is simply extrapolated there. */
double tvf_cutoff_hz(unsigned cutoff)
{
  return 341.0 * std::pow(2.0, ((double)cutoff - 64.0) / 10.0);
}

/* MEASURED (`M-021`): resonance does not saturate. It is roughly 0.28 dB
   per step to value 96 and then climbs steeply - +46.7 dB of peak at cutoff
   64 and +67.4 dB at cutoff 112 - so how steep the top is depends on the
   cutoff.

   THE CUTOFF DEPENDENCE ABOVE VALUE 96 IS NOT MODELLED. This reads the
   0.28 dB per step below 96 and then the measured cutoff-64 endpoint,
   46.7 dB at 127, straight between them. The peak gain of a two-pole
   section is its Q for a Q well above unity, which is how the dB reaches
   the coefficients below; the chip's own coefficient form is unknown
   (`L-05`) and is not what this reproduces. */
double tvf_q(unsigned resonance)
{
  double peakDb = resonance <= 96u
    ? 0.28 * (double)resonance
    : 26.88 + (46.70 - 26.88) * ((double)resonance - 96.0) / (127.0 - 96.0);
  double q = std::pow(10.0, peakDb / 20.0);
  return q < 0.70710678 ? 0.70710678 : q;
}

/* A two-pole section per filter type. MEASURED (`M-017`): all four types
   are active and each has the response its name says - the type register
   reading zero says the type is carried elsewhere, not that the types are
   unused. The realisation is this model's own: the chip's is silicon. */
void set_biquad(struct XpJv1080Voice *voice, int type, double fc,
                 double q, double rate)
{
  double nyquist = rate * 0.5;
  if (fc > nyquist * 0.99)
    fc = nyquist * 0.99;
  if (fc < 1.0)
    fc = 1.0;
  double w = 2.0 * 3.14159265358979323846 * fc / rate;
  double cs = std::cos(w);
  double sn = std::sin(w);
  double alpha = sn / (2.0 * q);
  double a0 = 1.0 + alpha;

  switch (type) {
  case 2:                        /* BPF */
    voice->b0 = alpha / a0;
    voice->b1 = 0.0;
    voice->b2 = -alpha / a0;
    break;
  case 3:                        /* HPF */
    voice->b0 = ((1.0 + cs) / 2.0) / a0;
    voice->b1 = -(1.0 + cs) / a0;
    voice->b2 = ((1.0 + cs) / 2.0) / a0;
    break;
  case 4:                        /* PKG */
    voice->b0 = (1.0 + alpha * q) / a0;
    voice->b1 = (-2.0 * cs) / a0;
    voice->b2 = (1.0 - alpha * q) / a0;
    break;
  default:                       /* LPF */
    voice->b0 = ((1.0 - cs) / 2.0) / a0;
    voice->b1 = (1.0 - cs) / a0;
    voice->b2 = ((1.0 - cs) / 2.0) / a0;
    break;
  }
  voice->a1 = (-2.0 * cs) / a0;
  voice->a2 = (1.0 - alpha) / a0;
  voice->x1 = voice->x2 = voice->y1 = voice->y2 = 0.0;
}

int tone_field(const struct xp_rom *rom, const uint8_t *tone, unsigned index)
{
  (void)rom;
  return tone[index];
}

}  // namespace

bool jv1080_patch_tone(const struct xp_rom *rom,
                        const struct xp_packed_record *patch, unsigned index,
                        uint8_t *tone, unsigned *patchLevel,
                        unsigned *patchPan)
{
  const struct XpDeviceProfile *profile = xp_profile(rom);
  if (!patch || !tone || !patchLevel || !patchPan ||
      index >= patch->part_count)
    return false;
  for (unsigned f = 0; f < XP_JV1080_TONE_FIELDS; ++f) {
    int value = 0;
    if (!packed_part_field(rom, patch, index, f, &value))
      return false;
    /* The decoder stores each field in a signed byte, so the group's six
       eight-bit fields come back negative and every consumer re-widens
       them. Keeping the unwrapped byte is the same value. */
    tone[f] = (uint8_t)(value & 0xff);
  }
  int level = 0;
  int pan = 0;
  if (!packed_common_field(rom, patch, profile->patchFieldLevel, &level) ||
      !packed_common_field(rom, patch, profile->patchFieldPan, &pan))
    return false;
  *patchLevel = (unsigned)(level < 0 ? 0 : level);
  *patchPan = (unsigned)(pan < 0 ? 0 : pan);
  return true;
}

bool jv1080_voice_start(const struct xp_rom *rom, const uint8_t *tone,
                         unsigned patchLevel, unsigned patchPan,
                         unsigned key, unsigned velocity,
                         const uint8_t *const banks[XP_WAVE_BANK_COUNT],
                         const size_t bankSizes[XP_WAVE_BANK_COUNT],
                         int32_t *pcm, size_t capacity,
                         double outputRate, struct XpJv1080Voice *voice)
{
  const struct XpDeviceProfile *profile = xp_profile(rom);
  if (!rom || !tone || !banks || !bankSizes || !pcm || !voice ||
      key > 127u || velocity > 127u || velocity == 0u || outputRate <= 0.0)
    return false;
  std::memset(voice, 0, sizeof *voice);

  /* The tone switch and the two zone gates: a tone that is off, or whose
     key or velocity range excludes this note, does not sound. Not an
     error - a patch's four tones routinely split the keyboard. */
  if (!tone_field(rom, tone, profile->toneFieldToneSwitch))
    return false;
  unsigned keyLow = tone[profile->toneFieldKeyRangeLow];
  unsigned keyHigh = tone[profile->toneFieldKeyRangeHigh];
  if (key < keyLow || key > keyHigh)
    return false;
  unsigned velLow = tone[0x0cu];
  unsigned velHigh = tone[0x0du];
  if (velocity < velLow || velocity > velHigh)
    return false;

  /* The wave chain: three tone fields to a multisample row, the row's
     splits to a zone, the zone's reference to an element record. */
  unsigned source = 0;
  uint8_t msBank = 0;
  uint16_t msRow = 0;
  struct xp_wave_zone zone;
  struct xp_wave_element element;
  if (!wave_source_select(rom, (unsigned)tone[profile->toneFieldWaveGroup],
                           (unsigned)tone[profile->toneFieldWaveGroupId],
                           &source) ||
      !wave_number_resolve(rom, source,
                            (unsigned)tone[profile->toneFieldWaveNumber],
                            &msBank, &msRow) ||
      !multisample_select(rom, msBank, msRow, key, &zone) ||
      !wave_element_open(rom, zone.directory, zone.element, &element))
    return false;
  if (element.bank >= XP_WAVE_BANK_COUNT || !banks[element.bank])
    return false;

  /* Decode the element. The whole span is decoded at note-on rather than
     streamed: the accumulator is differential, so a sample's value depends
     on every delta before it in the block, and a probe that holds the span
     is simpler than one that reseeds. */
  size_t needed = (size_t)(element.bank_end - (element.bank_start & ~0x0fu)) + 1u;
  if (needed > capacity)
    return false;
  struct xp_fce_decoder decoder;
  if (!fce_decoder_reset(profile, &decoder, element.bank_start))
    return false;
  uint32_t base = element.bank_start & ~UINT32_C(0x0f);
  for (size_t i = 0; i < needed; ++i)
    if (!fce_decoder_read(&decoder, banks[element.bank],
                          bankSizes[element.bank], pcm + i))
      return false;

  voice->pcm = pcm;
  voice->pcm_count = needed;
  voice->reverse = element.reverse;
  voice->looping = element.mode == XP_WAVE_FORWARD_LOOP ||
    element.mode == XP_WAVE_PING_PONG_LOOP;
  voice->loop_first = (size_t)(element.bank_loop - base);
  voice->loop_last = (size_t)(element.bank_end - base);
  /* MEASURED (`M-090`): a reversed element plays its last N samples
     backwards - the reversed array's first N, not the first N reversed - so
     the read head starts at the far end and walks down. A reversed element
     does not loop. */
  voice->position = voice->reverse
    ? (double)(voice->pcm_count - 1u)
    : (double)(element.bank_start - base);
  if (voice->reverse)
    voice->looping = false;

  /* MEASURED (`M-014`), all exact: coarse tune is `value - 48` semitones to
     within 0.3 cents over the whole range and fine tune is `value - 50`
     cents to within 0.1. The bias is already applied by the descriptor, so
     the decoded bytes are signed.

     The element record's own fine-tune field (+0x0E) is NOT applied: its
     units are open (`U-R3-03`), and a guess at them would be a tuning error
     on every note rather than on none. */
  int coarse = (int8_t)tone[profile->toneFieldCoarseTune];
  int fine = (int8_t)tone[profile->toneFieldFineTune];
  double keyHz = 440.0 * std::pow(2.0, ((double)key - 69.0) / 12.0) *
    std::pow(2.0, (double)coarse / 12.0) *
    std::pow(2.0, (double)fine / 1200.0);
  double rootHz = 440.0 * std::pow(2.0,
                                    ((double)element.root_key - 69.0) / 12.0);
  if (rootHz <= 0.0)
    return false;
  voice->increment = (keyHz / rootHz) * (kXpNativeRate / outputRate);

  /* Amplitude. MEASURED (`M-029`): curve 0 fits `40*log10(v/127)` - the
     same square law the level fields use - to a worst 1.35 dB, and curve 0
     is what the bench selects. The other six are seven distinct measured
     laws and an implementation needs all seven; only ten points per curve
     exist in `M-029` and they are not transcribed into this project's data
     yet, so a tone selecting one of them is rendered on curve 0 and is
     WRONG BY UP TO 36 dB at velocity 64 (curve 2 reads -48.4 dB there
     against curve 0's -11.8). */
  double velocityGain = square_law_gain(velocity);
  voice->static_gain =
    square_law_gain(tone[profile->toneFieldLevel]) *
    square_law_gain(patchLevel) *
    velocityGain *
    wave_gain(tone[0x05u]);

  /* Pan: the tone's and the patch's index one table and sum as offsets from
     centre (`M-002`, `M-048`). */
  int panOffset = (int)tone[profile->toneFieldPan] - 64 +
    ((int)patchPan - 64);
  if (panOffset < -64)
    panOffset = -64;
  if (panOffset > 63)
    panOffset = 63;
  double difference = pan_difference_db(panOffset);
  double ratio = std::pow(10.0, difference / 20.0);
  double right = std::sqrt(1.0 / (1.0 + ratio * ratio));
  voice->gain_right = right;
  voice->gain_left = ratio * right;

  /* The envelope's three level fields plus its implicit final zero. */
  for (unsigned i = 0; i < 3u; ++i)
    voice->level[i] =
      std::pow(10.0,
                amp_env_level_db(tone[profile->toneFieldAEnvLevel1 + i]) / 20.0);
  voice->level[3] = 0.0;
  voice->time[0] = amp_env_attack_seconds(tone[profile->toneFieldAEnvTime1]);
  for (unsigned i = 1; i < 4u; ++i)
    voice->time[i] =
      amp_env_fall_seconds_per_20db(tone[profile->toneFieldAEnvTime1 + i]);
  voice->segment = 0u;
  voice->envelope = 0.0;
  voice->segment_start = 0.0;
  voice->segment_remaining = voice->time[0];
  voice->sample_period = 1.0 / outputRate;
  voice->segment_total = voice->segment_remaining;

  voice->filter_type = tone[profile->toneFieldFilterType];
  if (voice->filter_type)
    set_biquad(voice, voice->filter_type,
                tvf_cutoff_hz(tone[profile->toneFieldCutoff]),
                tvf_q(tone[profile->toneFieldResonance]), outputRate);

  voice->active = true;
  return true;
}

/* Time 4 names a rate, so the release's duration is how far the envelope
   has to travel at it. Sixty dB is taken as silent: the level table's own
   floor is the machine's noise floor and no field can ask for less. */
void jv1080_voice_release(struct XpJv1080Voice *voice)
{
  if (!voice || !voice->active || voice->releasing)
    return;
  voice->releasing = true;
  voice->segment = 3u;
  voice->segment_start = voice->envelope;
  voice->segment_total = (60.0 / 20.0) * voice->time[3];
  voice->segment_remaining = voice->segment_total;
}

bool jv1080_voice_render(struct XpJv1080Voice *voice, float *l, float *r,
                          size_t frames)
{
  if (!voice || !voice->active || !voice->pcm || !l || !r)
    return false;

  for (size_t n = 0; n < frames; ++n) {
    /* THE INTERPOLATOR IS TWO-POINT LINEAR, AND A BETTER ONE IS WRONG
       (`M-087`). Two single-element looping waves swept across their phase
       increments - one with 84 % of its energy above 8 kHz - give linear on
       29 of 29 and 26 of 29 slots, with Hermite and sinc progressively
       better as interpolators and progressively WORSE as matches, which can
       only happen if the recording carries linear interpolation's own
       error. */
    size_t i0 = (size_t)voice->position;
    if (i0 + 1u >= voice->pcm_count) {
      if (!voice->looping)
        break;
      i0 = voice->loop_first;
      voice->position = (double)i0;
    }
    double frac = voice->position - (double)i0;
    double a = (double)voice->pcm[i0];
    double b = (double)voice->pcm[i0 + 1u];
    double sample = (a + (b - a) * frac) / 8388608.0;   /* 24-bit full scale */

    /* The envelope, one segment at a time. The attack is a straight
       amplitude ramp, which is what a 0-to-90 % rise time describes; the
       three falls are straight in dB, which is what a time-per-20-dB
       describes. Neither is the chip's segment stepper - that is internal
       (`M-011`'s own caveat). */
    if (voice->segment < 4u) {
      double target = voice->level[voice->segment];
      if (voice->segment_remaining <= 0.0) {
        voice->envelope = target;
        if (voice->releasing) {
          voice->active = false;
          break;
        }
        if (voice->segment < 2u) {
          ++voice->segment;
          voice->segment_start = voice->envelope;
          /* Times 2, 3 and 4 name a RATE - seconds per 20 dB - so a
             segment's own duration is how far it has to travel at it. */
          double from = voice->segment_start > 1e-9 ? voice->segment_start
                                                     : 1e-9;
          double to = voice->level[voice->segment] > 1e-9
            ? voice->level[voice->segment] : 1e-9;
          double span = 20.0 * std::fabs(std::log10(to / from));
          voice->segment_total =
            (span / 20.0) * voice->time[voice->segment];
          voice->segment_remaining = voice->segment_total;
        } else {
          voice->segment = 4u;   /* holding the sustain level */
        }
      } else {
        double done = voice->segment_total > 0.0
          ? 1.0 - voice->segment_remaining / voice->segment_total : 1.0;
        if (!voice->segment) {
          voice->envelope = target * done;
        } else {
          double from = voice->segment_start > 1e-9 ? voice->segment_start
                                                     : 1e-9;
          double to = target > 1e-9 ? target : 1e-9;
          voice->envelope = from * std::pow(to / from, done);
        }
        voice->segment_remaining -= voice->sample_period;
      }
    }

    double value = sample * voice->envelope * voice->static_gain;

    if (voice->filter_type) {
      double out = voice->b0 * value + voice->b1 * voice->x1 +
        voice->b2 * voice->x2 - voice->a1 * voice->y1 - voice->a2 * voice->y2;
      voice->x2 = voice->x1;
      voice->x1 = value;
      voice->y2 = voice->y1;
      voice->y1 = out;
      value = out;
    }

    l[n] += (float)(value * voice->gain_left);
    r[n] += (float)(value * voice->gain_right);

    if (voice->reverse) {
      voice->position -= voice->increment;
      if (voice->position < 1.0) {
        voice->active = false;
        break;
      }
    } else {
      voice->position += voice->increment;
      if (voice->position >= (double)voice->loop_last) {
        if (voice->looping)
          voice->position -= (double)(voice->loop_last - voice->loop_first);
        else if (voice->position + 1.0 >= (double)voice->pcm_count) {
          voice->active = false;
          break;
        }
      }
    }
  }
  return voice->active;
}

}}  // namespace EmuSC::Xp
