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
#include "../rom.h"

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
   between the measured distances rather than evaluating a formula.

   The two points past 48 are this lane's own measurement on the device, a
   tone-pan sweep of one dry part with the part's pan centred, and they
   exist because a straight line from 48 to 64 was 17 dB too wide at 56.
   The sweep also re-measures the four points above, independently and in
   the other direction: 2.21, 4.50, 9.23 and 15.50 dB against `M-015`'s
   2.20, 4.50, 9.20 and 15.60, after the 0.46 dB the interface contributes
   at centre was removed. The 60 point is the same rig reading a PART pan
   of 60 rather than a tone pan, which is a table point only under
   `M-002`/`M-048`'s finding that the two fields index one table.

   The last entry is NOT a measured difference. At a distance of 63 the
   quiet channel sits on the recorder's own noise floor (-95.9 dBFS, the
   same figure as the silence between notes), so all that is established is
   that the separation is at least 45 dB and the corner is effectively
   hard; 65 dB is a stand-in for "hard" and the curve's true shape over the
   last few steps is not recovered. */
const struct { uint8_t distance; double difference_db; } kPanTable[] = {
  {  0u,  0.00 }, {  8u,  2.20 }, { 16u,  4.50 }, { 32u,  9.20 },
  { 48u, 15.60 }, { 56u, 23.22 }, { 60u, 30.57 }, { 64u, 65.00 },
};

double interpolate_points(const double *xs, const double *ys, unsigned count,
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
  return interpolate_points(xs, ys, 10u, (double)value);
}

/* The RIGHT channel's level minus the LEFT channel's, in dB, for an offset
   that is positive to the right. */
double pan_difference_db(int offset)
{
  const unsigned count = (unsigned)(sizeof kPanTable / sizeof kPanTable[0]);
  double xs[sizeof kPanTable / sizeof kPanTable[0]];
  double ys[sizeof kPanTable / sizeof kPanTable[0]];
  for (unsigned i = 0; i < count; ++i) {
    xs[i] = kPanTable[i].distance;
    ys[i] = kPanTable[i].difference_db;
  }
  double magnitude = interpolate_points(xs, ys, count,
                                         (double)(offset < 0 ? -offset
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

/* MEASURED (`M-081`): CC7 indexes the same square law, with a floor - the
   values 0, 1 and 2 all read -81.7 dB, which is what the law gives for
   1.15. Worst deviation 0.41 dB. */
double cc7_gain(unsigned value)
{
  double v = (double)(value > 127u ? 127u : value);
  if (v < 1.15)
    v = 1.15;
  v /= 127.0;
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

/* MEASURED ON THE DEVICE, fourteen points across the field, with the wave's
   own onset taken out of the way.
 *
 *   The stimulus is the internal `Sine` wave - a looped sine, so it has no
 *   attack of its own and the output IS the envelope - played at key 84
 *   (1046.5 Hz) with the filter switched off, every LFO depth zeroed, the
 *   two later envelope levels at the top so the segment ends on a plateau,
 *   and the release at zero so notes cannot run into each other. The
 *   envelope is read off by quadrature demodulation at the carrier, which
 *   gives amplitude at about 1 ms resolution.
 *
 *   These are the full travels, in seconds, from silence to the level the
 *   segment is heading for. They are NOT the 0-to-90 % rise `M-011`
 *   reports, which is 0.74 of this; converting M-011's three points that
 *   way, its 1210 ms at value 64 against this sweep's 1183 ms agrees to
 *   2 %, its 2560 ms at 80 against 2290 ms to 11 %, and its 35 ms at 8
 *   against 29 ms to 17 % - an independent corroboration on a different
 *   wave and rig, which is why the two disagreeing at the BOTTOM of the
 *   field matters: extrapolating one exponential through M-011's points
 *   put value 0 at 21 ms where the machine takes about 2, and that is a
 *   pick transient smeared into a whisper on every tone that asks for the
 *   fastest attack. Six of the first factory song's tones do.
 *
 *   NOT ONE EXPONENTIAL, and now it is clear why nobody could fit one: the
 *   doubling interval widens steadily up the field - about 10.7 value steps
 *   between 8 and 64, 12.0 between 16 and 64, 13.4 between 32 and 64 and
 *   14.1 between 48 and 64. So this interpolates the measured points in log
 *   time, and above the top of the table continues at that last measured
 *   slope, which is an extrapolation and says so.
 *
 *   The first entry is at the measurement's own floor: 2 ms is one
 *   demodulator window, so all that is established at value 0 is "at or
 *   below 2 ms", i.e. instant for any purpose. */
const struct { uint8_t value; double seconds; } kAmpEnvAttackTable[] = {
  {  0u, 0.00204 }, {  1u, 0.00254 }, {  2u, 0.00817 }, {  3u, 0.01001 },
  {  4u, 0.01831 }, {  6u, 0.03137 }, {  8u, 0.03946 }, { 12u, 0.06668 },
  { 16u, 0.09821 }, { 24u, 0.18481 }, { 32u, 0.30751 }, { 48u, 0.73174 },
  { 64u, 1.60289 }, { 80u, 3.09262 },
};

double amp_env_attack_seconds(unsigned value)
{
  const unsigned count =
    (unsigned)(sizeof kAmpEnvAttackTable / sizeof kAmpEnvAttackTable[0]);
  double v = (double)value;
  if (v <= kAmpEnvAttackTable[0].value)
    return kAmpEnvAttackTable[0].seconds;
  for (unsigned i = 1; i < count; ++i) {
    if (v <= kAmpEnvAttackTable[i].value) {
      double x0 = kAmpEnvAttackTable[i - 1].value;
      double x1 = kAmpEnvAttackTable[i].value;
      double y0 = std::log(kAmpEnvAttackTable[i - 1].seconds);
      double y1 = std::log(kAmpEnvAttackTable[i].seconds);
      return std::exp(y0 + (v - x0) * (y1 - y0) / (x1 - x0));
    }
  }
  /* Past the last measured point, continue at the slope of the last
     measured interval. Extrapolation, not measurement. */
  double x0 = kAmpEnvAttackTable[count - 2].value;
  double x1 = kAmpEnvAttackTable[count - 1].value;
  double y0 = std::log(kAmpEnvAttackTable[count - 2].seconds);
  double y1 = std::log(kAmpEnvAttackTable[count - 1].seconds);
  return std::exp(y1 + (v - x1) * (y1 - y0) / (x1 - x0));
}

/* MEASURED ON THE DEVICE, from the same sweep: the SHAPE the attack segment
   traverses, as the fraction of its travel reached at each fraction of its
   duration. It is not a straight amplitude ramp - it is front-loaded, half
   the travel being done in the first 31 % of the time.
 *
 *   Averaged over the six segment durations from value 12 to value 64,
 *   which span 24x in time, the fifteen points below have a worst standard
 *   deviation of 0.016 and are under 0.005 over most of the range - so the
 *   shape is scale-invariant, which is itself the finding. It is close to
 *   sin(pi t / 2T) but not equal to it: that curve misses by up to 0.019,
 *   consistently high past the middle, so the measured points are kept
 *   rather than the formula they resemble.
 *
 *   Measured only at full travel, from silence to the first level, which is
 *   what the stimulus set up; whether a shorter travel uses the same curve
 *   is not established. */
const double kAmpEnvAttackShape[15] = {
  0.116, 0.218, 0.308, 0.394, 0.476, 0.556, 0.630, 0.697,
  0.760, 0.816, 0.866, 0.905, 0.938, 0.966, 0.983,
};

/* One sample of the element, `offset` positions from the read head, with a
   loop read as the cycle it is: a position past the loop's end comes back
   round to its start, and the position one before its start is its end -
   but only once the head is inside the loop, since before that the element
   is still playing straight through and the sample behind really is the one
   behind it in memory. Where there is no such sample - a one-shot's two
   ends - the caller's inner tap stands in, which repeats a sample the note
   really has rather than inventing one and leaves the four weights summing
   to one, so a boundary cannot put a gain step or a DC offset into the
   output. The same choice `oscillator.cc` makes for the sibling engine. */
/* One position of a ping-pong loop's cycle, as a value.

   A ping-pong turn in a DIFFERENTIAL format is not a time reversal. The
   decoder accumulates deltas, so running the address back down the stream
   while still adding what it reads gives

       y[m] = x[c] + d[c] + ... + d[c-m+1] = 2*x[c] - x[c-m]

   - the loop backwards AND reflected about the value it turned at, which is
   continuous in value and in SLOPE. A plain time reversal leaves a corner
   and a forward wrap leaves a phase jump.

   The cycle is `2*span` long, `span = loop_last - loop_first + 1`:

     index < span     address loop_last-1 down to loop_first-1, REFLECTED
     index >= span    address loop_first up to loop_last, as decoded

   This ROM satisfies the invariant the turn needs. The reflected pass lands
   on x[loop_first-1] as its address reaches loop_first-1, and the cycle
   closes with no step and no drift only if x[loop_last] == x[loop_first-1].
   Decoded through this engine's own FCE decoder that holds with INTEGER
   EQUALITY on 204 OF 204 of this ROM's loop-type-1 elements - every one -
   against 1029 of 1031 for loop type 0. So loop_first-1 is answered with
   x[loop_last] directly rather than read: it can sit before the element's
   first decoded frame when a zone loops from its own start.

   THE SIBLING'S TELL-TALE IS ABSENT HERE, which is why the ROM alone could
   not settle it. On the SC-88 a forward read leaves a visible step and a
   phase jump at the join - correlation across it +0.197 against +0.968 for
   its forward loops. On this device both ends of the loop sit at EXACTLY
   ZERO: on the loop-type-1 elements examined sample by sample, x[loop_last]
   and x[loop_first-1] are both 0, and the step a FORWARD wrap would leave
   measures 0.23, 0.58 and 0.48 times the loop's own median sample-to-sample
   delta. A forward read is continuous in VALUE here, so the step says
   nothing either way.

   MEASURED ON THIS DEVICE'S OWN HARDWARE RECORDINGS (`TASK-346` AC#2), and
   the answer is the reflection. Rendering two preset rhythm kits both ways
   and comparing each against the machine's recording of the same key, in the
   key's own 520 ms slot: on the ELEVEN keys where the two readings differ at
   all, the reflected read matches the machine's sustained spectrum better on
   TEN, the one exception being a key whose correlation is 0.19 either way.
   The two tonal ones settle it outright. On the Long Whistle the machine's
   strongest partial is 2047.0 Hz: the reflected read puts it at 2038.9,
   -6.8 cents away, while the forward read's strongest is 2269.6 - it
   promotes to dominant what is only the machine's SECOND partial at 2277.
   On the Short Whistle the forward read adds partials at 3692 Hz that the
   machine does not have at all, where the reflected read's partial set is
   the machine's, 9418-9422 against 9416-9419. Both reproduce identically
   from two different kits, the same element through two banks. */
double cycle_sample(const struct XpJv1080Voice *voice, long long index)
{
  if (voice->loop_last >= voice->pcm_count ||
      voice->loop_last < voice->loop_first)
    return 0.0;
  long long first = (long long)voice->loop_first;
  long long last = (long long)voice->loop_last;
  long long span = last - first + 1;
  long long cycle = 2 * span;
  index %= cycle;
  if (index < 0)
    index += cycle;
  double turn = (double)voice->pcm[voice->loop_last];
  if (index >= span) {
    long long at = first + (index - span);
    if (at < 0 || (size_t)at >= voice->pcm_count)
      return turn;
    return (double)voice->pcm[(size_t)at];
  }
  long long at = last - 1 - index;
  if (at < first || at < 0 || (size_t)at >= voice->pcm_count)
    return turn;                 /* the invariant's own answer at b-1 */
  return 2.0 * turn - (double)voice->pcm[(size_t)at];
}

double wave_tap(const struct XpJv1080Voice *voice, size_t index, int offset,
                 double fallback)
{
  long long at = (long long)index + offset;
  /* A ping-pong element has not wrapped when the head runs past the loop's
     end - it has TURNED, so the sample after loop_last is the cycle's first
     reflected one. Behind the head it is still plain memory, because this
     branch is only reached before the first turn. */
  if (voice->ping_pong && voice->loop_last >= voice->loop_first &&
      at > (long long)voice->loop_last)
    return cycle_sample(voice, at - (long long)voice->loop_last - 1);
  if (!voice->ping_pong && voice->looping &&
      voice->loop_last >= voice->loop_first) {
    long long first = (long long)voice->loop_first;
    long long last = (long long)voice->loop_last;
    long long length = last - first + 1;
    if (at > last)
      at = first + (at - first) % length;
    else if (at < first && (long long)index >= first)
      at = last + 1 - (first - at);
  }
  if (at < 0 || (size_t)at >= voice->pcm_count)
    return fallback;
  return (double)voice->pcm[(size_t)at];
}

double amp_env_attack_shape(double done)
{
  if (done <= 0.0)
    return 0.0;
  if (done >= 1.0)
    return 1.0;
  double u = done * 16.0;                /* the table is on sixteenths */
  unsigned i = (unsigned)u;
  double f = u - (double)i;
  double a = i == 0u ? 0.0 : kAmpEnvAttackShape[i - 1u];
  double b = i >= 15u ? 1.0 : kAmpEnvAttackShape[i];
  return a + (b - a) * f;
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
double tvf_cutoff_hz(double cutoff)
{
  return 341.0 * std::pow(2.0, (cutoff - 64.0) / 10.0);
}

/* MEASURED (`M-082`): the seven F-ENV velocity curves, ten points each, as
   functions of velocity onto the fraction of the envelope's travel - and
   the travel is in CUTOFF-PARAMETER units, not in hertz, which is what
   curve 0 coming out linear in the normalised OCTAVE fraction says. The
   take was built for this reading: depth +30 from cutoff 24 keeps the whole
   sweep below the 7891 Hz ceiling that clipped five of the seven in
   `M-078`.

   Between the ten velocities this interpolates linearly, which is
   interpolation and not a recovered law. THE BOTTOM OF EACH CURVE IS SOFT:
   the take's velocity-1 note read 41 Hz where cutoff 24 predicts about
   23 Hz, so the rig's own low-frequency roll-off (`M-012`: below 33 Hz it
   cannot place a corner) is what the zeros at velocities 1 and 8 rest on,
   and the real curve may leave zero earlier than this table does. */
const uint8_t kFilterEnvCurveVelocity[10] = {
  1u, 8u, 16u, 32u, 48u, 64u, 80u, 96u, 112u, 127u,
};

const double kFilterEnvCurve[7][10] = {
  { 0.0, 0.0,   0.024, 0.164, 0.301, 0.443, 0.579, 0.721, 0.864, 1.0 },
  { 0.0, 0.0,   0.0,   0.005, 0.046, 0.144, 0.277, 0.452, 0.700, 1.0 },
  { 0.0, 0.0,   0.0,   0.0,   0.0,   0.0,   0.005, 0.155, 0.452, 1.0 },
  { 0.0, 0.098, 0.252, 0.459, 0.608, 0.718, 0.801, 0.875, 0.943, 1.0 },
  { 0.0, 0.397, 0.541, 0.694, 0.781, 0.849, 0.900, 0.937, 0.968, 1.0 },
  { 0.0, 0.0,   0.0,   0.0,   0.046, 0.452, 0.844, 0.941, 0.980, 1.0 },
  { 0.0, 0.183, 0.277, 0.366, 0.410, 0.441, 0.471, 0.511, 0.605, 1.0 },
};

/* MEASURED ON THE DEVICE - a depth sweep with the filter's corner read
   directly, which is what the three indirect anchors below could never
   agree on.

   The stimulus isolates the scale from everything it used to be tangled
   with: the internal `White Noise` wave at its own root key, so the source
   spectrum is flat and known; LPF with resonance 100, so the corner shows
   as a peak; base cutoff 40; the F-ENV holding at its top with level 1 at
   127 and time 1 at 0; and - the point of it - THE VELOCITY SENSITIVITY AT
   ZERO, so the depth's own scale is observed rather than its product with
   the sensitivity's. The corner is read as the peak of the note's spectrum
   divided by the same wave's spectrum with the filter switched off, and
   converted through `M-012`'s `fc = 341 * 2^((cutoff-64)/10)`.

   Depths +6, +12, +18 and +24 put the corner at 181.6, 521.5, 1502.9 and
   4347.7 Hz, i.e. cutoff 54.91, 70.13, 85.40 and 100.72. The consecutive
   differences are 2.537, 2.545 and 2.553 cutoff units per depth unit and a
   least-squares line through the four fits with residuals of +-0.025
   cutoff units, so the relation is linear and the slope is

       2.545 cutoff units per unit of depth.

   The line's intercept comes out at 39.62 against the 40 the cutoff was
   set to, and the depth-0 note reads 39.29 - so the method carries about
   half a cutoff unit of its own bias, which the SLOPE does not care about.
   A fifth point at depth +30 is deliberately excluded: its corner lands at
   12 kHz where the machine's own 32 kHz output and its reconstruction
   filter flatten the peak, and it reads 2.32 for that reason alone.

   THE THREE OLD ANCHORS AND WHY THEY DISAGREED, kept because the
   disagreement is the lesson: `M-082`'s whole travel gave 2.27, its
   velocity-127 endpoint alone 2.58, and `M-078`'s two unclipped points
   2.16 and 2.17. All three are products of this scale with the velocity
   sensitivity's, read off takes whose sensitivity was at 74 or +50, and
   `M-082`'s own velocity-1 note sits on the interface's roll-off (41 Hz
   where cutoff 24 predicts 21, against a rig `M-012` says cannot place a
   corner below about 33 Hz), which tilts the travel it normalises on. The
   measured 2.545 sits at the top of that spread, nearest the endpoint
   anchor - the one of the three that does not depend on the bad bottom
   point. The code used 2.27, so this opens the filter about a third of an
   octave further at a depth of 30 than it did.

   Measured with the SENSITIVITY AT ZERO, and that case is now confirmed
   rather than assumed: at depth +30 with sensitivity 0 the corner sits at
   8003.9, 8001.0 and 8003.9 Hz for velocities 1, 64 and 127. Sensitivity
   zero means full depth at every velocity, which is what the exponent form
   below was built to satisfy. What is still NOT measured is the
   sensitivity's own shape between 0 and its limits. */
inline constexpr double kFilterEnvDepthScale = 2.545;

/* The record's velocity curve, interpolated between `M-082`'s ten points.
   A record type with no curve field is rendered on curve 0; which curve
   such a record uses is not established. */
double filter_env_curve_fraction(unsigned curve, unsigned velocity)
{
  if (curve > 6u)
    curve = 0u;
  double xs[10], ys[10];
  for (unsigned i = 0; i < 10u; ++i) {
    xs[i] = kFilterEnvCurveVelocity[i];
    ys[i] = kFilterEnvCurve[curve][i];
  }
  return interpolate_points(xs, ys, 10u, (double)velocity);
}

/* How far velocity is allowed to move the envelope, from the record's
   velocity sensitivity field (decoded -50..+75).

   MEASURED, IN PART, AND THE FORM IS THIS MODEL'S OWN. Two things are
   measured and both are reproduced exactly here: at sensitivity 0 velocity
   does not move the envelope at all - the corpus states it as the
   `fenv_vel_sens_000` stimulus's own hypothesis, "sensitivity 0 must be
   flat" - and at the top of the field the curve spans the whole travel
   (`M-082` at 74). A THIRD fact rules out the obvious interpolation
   between those two: `M-078` at sensitivity +50 reads its velocity-1 note
   at 72 Hz, which is the unmodulated cutoff 40 and not the third of full
   depth that a linear blend predicts (that would put it near 1.4 kHz).

   So the sensitivity is applied as an exponent on the curve rather than as
   a blend with it, which is 1 at sensitivity 0 for every velocity, is the
   bare curve at the top of the field, and vanishes at velocity 1 for every
   positive setting - the three measured facts, in that order. The negative
   half inverts the curve over its own half-range. THE EXPONENT IS THIS
   MODEL'S CHOICE: it is the simplest form consistent with all three, not a
   recovered law, and the `fenv_vel_sens_{000,m50,p75}` takes have never
   been read. */
double filter_env_velocity_scale(int sensitivity, double fraction)
{
  if (!sensitivity)
    return 1.0;
  if (fraction < 0.0)
    fraction = 0.0;
  if (fraction > 1.0)
    fraction = 1.0;
  if (sensitivity > 0)
    return std::pow(fraction, (double)sensitivity / 75.0);
  return std::pow(1.0 - fraction, (double)(-sensitivity) / 50.0);
}

/* MEASURED (`M-040`): the filter envelope reads the amplitude envelope's
   time table, scaled. Read as a 20 dB fall on both sides - which needed an
   amplitude envelope outliving the filter, and `gaps/fenv_t4_hold` was
   written for it - F-ENV time 4 falls in 25, 95, 250, 540 and 1125 ms
   against the A-ENV's 25, 50, 130, 310 and 685, a ratio running 1.90,
   1.92, 1.74 and 1.64 over values 16 to 64.

   1.75 is `M-040`'s own figure for that scale. WHICH OF THE TWO IT IS -
   a per-envelope scale on one table, or a second table of the same shape -
   IS NOT SEPARABLE from seven points, and M-040 says so; the single
   constant also flattens a ratio that measures 1.64 to 1.92 across the
   field.

   The value returned is a FULL 0-to-127 traverse, so a segment that has
   less far to go takes proportionally less - which is linear in the cutoff
   parameter and therefore a constant rate in octaves per second, the form
   `M-069` read the F-ENV's own glide as. */
inline constexpr double kFilterEnvTimeScale = 1.75;

double filter_env_full_traverse_seconds(unsigned value)
{
  return kFilterEnvTimeScale * amp_env_fall_seconds_per_20db(value);
}

/* MEASURED (`M-069`): time key follow is one law on all three envelopes -
   a factor of two per octave of key, pivoting exactly on key 60, with the
   15-entry enum running -1 to +1. `M-066` measured on the A-ENV that it
   does NOT scale the attack; whether the filter envelope's own attack is
   likewise exempt was not measured, and this follows the amplitude
   envelope's rule. */
double time_key_follow_scale(unsigned enumValue, unsigned key)
{
  double kf = ((double)(enumValue > 14u ? 14u : enumValue) - 7.0) / 7.0;
  return std::pow(2.0, -kf * ((double)key - 60.0) / 12.0);
}

/* MEASURED (`M-070`): velocity-time sensitivity is the same shape 27x
   weaker, pivoting on velocity 64 - `t = t_64 * 2^(-vs*0.39*(vel-64)/63)`
   with vs from -1 to +1 - and the pitch and filter envelopes measure the
   same 1.30x span from velocity 1 to 127 to within 0.4 %. */
double velocity_time_scale(unsigned enumValue, unsigned velocity)
{
  double vs = ((double)(enumValue > 14u ? 14u : enumValue) - 7.0) / 7.0;
  return std::pow(2.0, -vs * 0.39 * ((double)velocity - 64.0) / 63.0);
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
   unused. The realisation is this model's own: the chip's is silicon.

   This writes the coefficients and leaves the delay line alone, because
   the filter envelope re-solves it while the note is sounding and
   restarting the section every millisecond would put a step in the output
   at every control block. */
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
}

/* Where the filter envelope currently puts the cutoff parameter, clamped
   to the field's own 0..127 range. The machine's further ceiling - a
   resonant peak that stops climbing around 7891 Hz whatever the cutoff
   asks for (`M-077`, `M-078`) - is a property of the wave rather than of
   this law and is NOT modelled here. */
double filter_env_cutoff(const struct XpJv1080Voice *voice)
{
  double cutoff = voice->cutoff_base + voice->cutoff_offset * voice->fenv_value;
  if (cutoff < 0.0)
    return 0.0;
  return cutoff > 127.0 ? 127.0 : cutoff;
}

/* Enter a segment, from wherever the envelope currently stands. The time
   field names a FULL 0-to-127 traverse, so a segment that has less far to
   go takes proportionally less of it. */
void filter_env_enter(struct XpJv1080Voice *voice, unsigned segment)
{
  voice->fenv_segment = segment;
  voice->fenv_start = voice->fenv_value;
  voice->fenv_total = voice->fenv_time[segment] *
    std::fabs(voice->fenv_level[segment] - voice->fenv_start);
  voice->fenv_remaining = voice->fenv_total;
}

/* Step the envelope on by one control block. Segments 0 to 2 run from
   note-on; segment 3 is the release, entered by jv1080_voice_release.
   Several segments may finish inside one block - every time field reaches
   down to a few milliseconds - so this consumes the block rather than
   assuming one segment survives it. */
void filter_env_advance(struct XpJv1080Voice *voice, double seconds)
{
  while (voice->fenv_segment < 4u && seconds > 0.0) {
    if (voice->fenv_remaining > seconds) {
      voice->fenv_remaining -= seconds;
      double done = voice->fenv_total > 0.0
        ? 1.0 - voice->fenv_remaining / voice->fenv_total : 1.0;
      double target = voice->fenv_level[voice->fenv_segment];
      voice->fenv_value =
        voice->fenv_start + (target - voice->fenv_start) * done;
      return;
    }
    seconds -= voice->fenv_remaining;
    voice->fenv_value = voice->fenv_level[voice->fenv_segment];
    if (voice->releasing || voice->fenv_segment >= 2u) {
      voice->fenv_segment = 4u;    /* holding, or released to level 4 */
      return;
    }
    filter_env_enter(voice, voice->fenv_segment + 1u);
  }
}

int tone_field(const struct xp_rom *rom, const uint8_t *tone, unsigned index)
{
  (void)rom;
  return tone[index];
}

/* The key every key-scaling field pivots on (`M-066`, `M-069`, `M-077`). */
const double kKeyFollowPivot = 60.0;

/* The record's pitch key follow as a fraction of one semitone per key:
   the device's own value string, parsed. 1 where the record has no such
   field or the device no list. */
double pitch_key_follow(const struct xp_rom *rom,
                        const struct XpVoiceFieldMap *fields,
                        const uint8_t *record)
{
  const struct XpDeviceProfile *profile = xp_profile(rom);
  if (fields->pitchKeyFollow == XP_VOICE_FIELD_NONE ||
      !profile->pitchKeyFollowTable || !profile->pitchKeyFollowWidth)
    return 1.0;
  unsigned index = record[fields->pitchKeyFollow];
  uint32_t at = profile->pitchKeyFollowTable +
    (uint32_t)index * profile->pitchKeyFollowWidth;
  if (index >= profile->pitchKeyFollowCount ||
      at + profile->pitchKeyFollowWidth > rom->size)
    return 1.0;
  int sign = 1, value = 0;
  for (unsigned c = 0; c < profile->pitchKeyFollowWidth; ++c) {
    char ch = (char)rom->bytes[at + c];
    if (ch == '-')
      sign = -1;
    else if (ch >= '0' && ch <= '9')
      value = value * 10 + (ch - '0');
  }
  return sign * value / 100.0;
}

/* A field this record type has, or `absent` where it does not have one. */
unsigned field_or(const struct XpVoiceFieldMap *fields, uint16_t which,
                   const uint8_t *record, unsigned absent)
{
  return which == XP_VOICE_FIELD_NONE ? absent : record[which];
}

/* Which key selects the zone, and which key the wave is played at. They are
   the same on a record that transposes and differ on one that names its own
   source key. */
unsigned playback_key(const struct XpVoiceFieldMap *fields,
                       const uint8_t *record, unsigned key)
{
  return field_or(fields, fields->sourceKey, record, key);
}

/* The five links from a record's three wave fields to a wave-element
   record, shared by the span query and the note-on below so the two cannot
   drift. */
bool resolve_element(const struct xp_rom *rom,
                      const struct XpVoiceFieldMap *fields,
                      const uint8_t *record, unsigned key,
                      struct xp_wave_element *element)
{
  unsigned source = 0;
  uint8_t msBank = 0;
  uint16_t msRow = 0;
  struct xp_wave_zone zone;
  return wave_source_select(rom, (unsigned)record[fields->waveGroup],
                             (unsigned)record[fields->waveGroupId],
                             &source) &&
    wave_number_resolve(rom, source, (unsigned)record[fields->waveNumber],
                         &msBank, &msRow) &&
    multisample_select(rom, msBank, msRow,
                        playback_key(fields, record, key), &zone) &&
    wave_element_open(rom, zone.directory, zone.element, element);
}

/* The record's own gates. One that is off, or whose key or velocity range
   excludes this note, does not sound - which is not an error: a patch's
   four tones routinely split the keyboard between them. A record type with
   no range fields gates on its switch alone. */
bool record_sounds(const struct XpVoiceFieldMap *fields,
                    const uint8_t *record, unsigned key, unsigned velocity)
{
  if (!record[fields->enable])
    return false;
  if (key < field_or(fields, fields->keyRangeLow, record, 0) ||
      key > field_or(fields, fields->keyRangeHigh, record, 127))
    return false;
  return velocity >= field_or(fields, fields->velocityRangeLow, record, 1) &&
    velocity <= field_or(fields, fields->velocityRangeHigh, record, 127);
}

}  // namespace

double jv1080_filter_env_curve(unsigned curve, unsigned velocity)
{
  return filter_env_curve_fraction(curve, velocity);
}

double jv1080_filter_env_offset(const struct XpVoiceFieldMap *fields,
                                 const uint8_t *record, unsigned velocity)
{
  if (!fields || !record || fields->filterEnvDepth == XP_VOICE_FIELD_NONE)
    return 0.0;
  int depth = (int8_t)record[fields->filterEnvDepth];
  if (!depth)
    return 0.0;
  /* A record type with no velocity-curve field of its own is rendered on
     curve 0; which curve such a record uses is not established. */
  unsigned curve = field_or(fields, fields->filterEnvVelCurve, record, 0u);
  int sensitivity =
    (int8_t)(uint8_t)field_or(fields, fields->filterEnvVelSens, record, 0u);
  double fraction = filter_env_curve_fraction(curve, velocity);
  return kFilterEnvDepthScale * (double)depth *
    filter_env_velocity_scale(sensitivity, fraction);
}

bool jv1080_voice_span(const struct xp_rom *rom,
                        const struct XpVoiceFieldMap *fields,
                        const uint8_t *tone, unsigned key, unsigned velocity,
                        size_t *samples)
{
  struct xp_wave_element element;
  if (!rom || !fields || !tone || !samples || key > 127u || velocity == 0u ||
      velocity > 127u || !record_sounds(fields, tone, key, velocity) ||
      !resolve_element(rom, fields, tone, key, &element))
    return false;
  *samples = (size_t)(element.bank_end - (element.bank_start & ~0x0fu)) + 1u;
  return true;
}

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

bool jv1080_voice_start(const struct xp_rom *rom,
                         const struct XpVoiceFieldMap *fields,
                         const uint8_t *tone,
                         const struct XpJv1080PartControls *controls,
                         unsigned key, unsigned velocity,
                         const uint8_t *const banks[XP_WAVE_BANK_COUNT],
                         const size_t bankSizes[XP_WAVE_BANK_COUNT],
                         int32_t *pcm, size_t capacity,
                         double outputRate, struct XpJv1080Voice *voice)
{
  const struct XpDeviceProfile *profile = xp_profile(rom);
  if (!rom || !fields || !tone || !controls || !banks || !bankSizes ||
      !pcm || !voice ||
      key > 127u || velocity > 127u || velocity == 0u || outputRate <= 0.0)
    return false;
  std::memset(voice, 0, sizeof *voice);

  struct xp_wave_element element;
  if (!record_sounds(fields, tone, key, velocity) ||
      !resolve_element(rom, fields, tone, key, &element))
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
  voice->ping_pong = element.mode == XP_WAVE_PING_PONG_LOOP;
  voice->in_cycle = false;
  voice->loop_first = (size_t)(element.bank_loop - base);
  voice->loop_last = (size_t)(element.bank_end - base);
  /* MEASURED (`M-090`): a reversed element plays its last N samples
     backwards - the reversed array's first N, not the first N reversed - so
     the read head starts at the far end and walks down. A reversed element
     does not loop. */
  voice->position = voice->reverse
    ? (double)(voice->pcm_count - 1u)
    : (double)(element.bank_start - base);
  if (voice->reverse) {
    voice->looping = false;
    voice->ping_pong = false;
  }

  /* MEASURED (`M-014`), all exact: coarse tune is `value - 48` semitones to
     within 0.3 cents over the whole range and fine tune is `value - 50`
     cents to within 0.1. The bias is already applied by the descriptor, so
     the decoded bytes are signed.

     The element record's own fine-tune field (+0x0E) is NOT applied: its
     units are open (`U-R3-03`), and a guess at them would be a tuning error
     on every note rather than on none. */
  int coarse = (int8_t)(uint8_t)field_or(fields, fields->coarseTune, tone, 0);
  int fine = (int8_t)(uint8_t)field_or(fields, fields->fineTune, tone, 0);
  /* The part's key shift moves the pitch rather than the note number, so
     the zone the key chose is left alone. */
  const unsigned soundedKey = playback_key(fields, tone, key);
  /* MEASURED on the device: the patch's octave shift is a third, independent
     transposition term, exactly twelve semitones per unit, and it adds to
     both coarse tunes rather than replacing either. Leaving it out is an
     octave error on every patch that uses it - nine of the fourteen
     sounding parts of the first factory song do. */
  /* Key follow scales the key's distance from the pivot. The part's key
     shift below stays outside it: whether the machine scales a shift by
     key follow is not measured. */
  const double trackedKey =
    kKeyFollowPivot + pitch_key_follow(rom, fields, tone) *
                        ((double)soundedKey - kKeyFollowPivot);
  double keyHz = 440.0 * std::pow(2.0, (trackedKey - 69.0) / 12.0) *
    std::pow(2.0, (double)coarse / 12.0) *
    std::pow(2.0, (double)controls->key_shift / 12.0) *
    std::pow(2.0, (double)controls->patch_octave) *
    std::pow(2.0, (double)controls->fine_tune / 1200.0) *
    std::pow(2.0, (double)fine / 1200.0);
  double rootHz = 440.0 * std::pow(2.0,
                                    ((double)element.root_key - 69.0) / 12.0);
  /* MEASURED, from the ROM against itself: the wave-element record's
     `+0x0E` field is a SIGNED PITCH OFFSET of the recorded sample from its
     own root key - neutral at 1024, one semitone per 1024 units - and
     without it every instrument is detuned by its own amount.

     `wave_metadata.md` carries this field as "fine tune, typical
     0x03xx-0x06xx" with its units open (`U-R3-03`). They are recovered
     here by measuring the ROM's own sample data: for each element, the
     pitch of its loop region read at the wave ROM's 32 kHz against the
     frequency its root key names. Over the 26 elements the first factory
     song plays, 23 fit

         cents = -(fine - 1024) * 100 / 1024

     with a residual rms of 3.2 cents across a measured spread of 81 cents
     peak to peak. The three that do not are the three whose pitch the
     measurement cannot pin - `Orch. Hit`, which is a stab with no pitch,
     `Rot.Org Fst`, whose second harmonic is louder than its fundamental,
     and `EG Harm`, a guitar harmonic.

     This is what "the instruments are out of tune" was: not a transpose,
     which is why the whole mix still aligned with the hardware take to
     0 cents, but every element sitting at its own offset - from -23 to
     +48 cents in this song alone - so the parts disagree with each other. */
  rootHz *= std::pow(2.0, -((double)element.fine_tune - 1024.0) / 1024.0 / 12.0);
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
    square_law_gain(tone[fields->level]) *
    square_law_gain(controls->patch_level) *
    square_law_gain(controls->part_level) *
    cc7_gain(controls->volume) *
    velocityGain *
    wave_gain(field_or(fields, fields->waveGain, tone, 1u)) *
    (profile->voiceMixScale > 0.0 ? profile->voiceMixScale : 1.0);

  /* Pan: the tone's and the patch's index one table and sum as offsets from
     centre (`M-002`, `M-048`). */
  int panOffset = (int)tone[fields->pan] - 64 +
    ((int)controls->patch_pan - 64) + ((int)controls->part_pan - 64);
  if (panOffset < -64)
    panOffset = -64;
  if (panOffset > 63)
    panOffset = 63;
  /* The difference is the right channel's level minus the left's, so the
     ratio multiplies the RIGHT gain. Applying it to the left instead put
     every pan on the wrong side, which is audible on any part the song
     places off centre and was confirmed on the device: a tone pan of 127
     with the part centred measures the right channel 56 dB above the left
     on hardware, where this rendered it 62 dB BELOW. */
  double difference = pan_difference_db(panOffset);
  double ratio = std::pow(10.0, difference / 20.0);
  double left = std::sqrt(1.0 / (1.0 + ratio * ratio));
  voice->gain_left = left;
  voice->gain_right = ratio * left;

  /* The envelope's three level fields plus its implicit final zero. */
  for (unsigned i = 0; i < 3u; ++i)
    voice->level[i] =
      std::pow(10.0,
                amp_env_level_db(tone[fields->ampLevel1 + i]) / 20.0);
  voice->level[3] = 0.0;
  voice->time[0] = amp_env_attack_seconds(tone[fields->ampTime1]);
  for (unsigned i = 1; i < 4u; ++i)
    voice->time[i] =
      amp_env_fall_seconds_per_20db(tone[fields->ampTime1 + i]);
  voice->segment = 0u;
  voice->envelope = 0.0;
  voice->segment_start = 0.0;
  voice->segment_remaining = voice->time[0];
  voice->sample_period = 1.0 / outputRate;
  voice->segment_total = voice->segment_remaining;

  voice->filter_type = (int)tone[fields->filterType];
  voice->output_rate = outputRate;
  voice->cutoff_base = (double)tone[fields->cutoff];
  voice->resonance_q = tvf_q(tone[fields->resonance]);

  /* The filter envelope. It moves the cutoff PARAMETER, so its whole
     sweep is worked out in cutoff units here and the corner law is applied
     to the sum once per control block. */
  voice->cutoff_offset = jv1080_filter_env_offset(fields, tone, velocity);
  if (voice->cutoff_offset != 0.0) {
    unsigned timeKf = field_or(fields, fields->filterEnvTimeKeyFollow, tone, 7u);
    unsigned velT1 = field_or(fields, fields->filterEnvVelTime1, tone, 7u);
    unsigned velT4 = field_or(fields, fields->filterEnvVelTime4, tone, 7u);
    for (unsigned i = 0; i < 4u; ++i) {
      voice->fenv_level[i] =
        (double)tone[fields->filterEnvLevel1 + i] / 127.0;
      voice->fenv_time[i] =
        filter_env_full_traverse_seconds(tone[fields->filterEnvTime1 + i]);
      /* `M-066` measured on the amplitude envelope that time key follow
         does not scale the attack; the filter envelope's own attack was
         not measured separately and follows that rule here. */
      if (i)
        voice->fenv_time[i] *= time_key_follow_scale(timeKf, soundedKey);
    }
    voice->fenv_time[0] *= velocity_time_scale(velT1, velocity);
    voice->fenv_time[3] *= velocity_time_scale(velT4, velocity);
    /* The envelope starts closed - at the record's own cutoff - and its
       first segment is the move to level 1. */
    voice->fenv_value = 0.0;
    filter_env_enter(voice, 0u);
    /* `M-074`: a modulator re-evaluated once per block of a millisecond or
       less is indistinguishable from the machine on the one observable
       that can see it. */
    voice->control_period = (size_t)(outputRate / 1000.0);
    if (!voice->control_period)
      voice->control_period = 1u;
    voice->control_countdown = 0u;
  }

  if (voice->filter_type)
    set_biquad(voice, voice->filter_type,
                tvf_cutoff_hz(filter_env_cutoff(voice)),
                voice->resonance_q, outputRate);

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
  /* The filter envelope releases on the same note-off, from wherever it
     had reached, to its own level 4. */
  if (voice->cutoff_offset != 0.0)
    filter_env_enter(voice, 3u);
}

bool jv1080_voice_render(struct XpJv1080Voice *voice, float *l, float *r,
                          size_t frames)
{
  if (!voice || !voice->active || !voice->pcm || !l || !r)
    return false;

  const bool sweeping = voice->filter_type && voice->cutoff_offset != 0.0;

  for (size_t n = 0; n < frames; ++n) {
    /* The filter envelope, once per control block rather than per sample
       (`M-074`). A voice whose envelope cannot move the corner - no filter
       or no depth - never enters here and its section is solved once at
       note-on, as it was before this envelope existed. */
    if (sweeping) {
      if (!voice->control_countdown) {
        filter_env_advance(voice, (double)voice->control_period *
                                    voice->sample_period);
        set_biquad(voice, voice->filter_type,
                    tvf_cutoff_hz(filter_env_cutoff(voice)),
                    voice->resonance_q, voice->output_rate);
        voice->control_countdown = voice->control_period;
      }
      --voice->control_countdown;
    }

    /* THE INTERPOLATOR IS A FOUR-POINT CUBIC B-SPLINE, the kernel this
       chip was measured to use - the same one `oscillator.cc` gives the
       sibling engine, since it is the same Roland part.

       This read was two-point linear, on `M-087`. THAT MEASUREMENT IS
       WITHDRAWN ON THE KERNEL by `M-107` and `M-108`, and the reason is
       instructive rather than a correction of arithmetic: M-087 ranked
       four candidates by time-domain correlation, all four of them
       INTERPOLATING kernels and therefore the identity at fraction 0, so
       none of them could represent a chip that smooths when the fraction
       is zero and the ranking could not detect one. Linear won by being
       the least sharp of four candidates all sharper than the chip. Read
       as a RESPONSE instead, over 42 dB of span, a cubic B-spline fits to
       0.63 and 0.59 dB rms on the two waves against two-point linear's
       5.48 and 4.23; and a dedicated unity-ladder capture measures the
       fraction-0 weights directly as `[w, 1-2w, w]` with
       w = 0.1614 +- 0.0088, against 1/6 for the cubic (0.6 sigma) and 0
       for every interpolating kernel (18.3 sigma). The chip is NOT
       transparent at its own root key: it is 8.4 dB down at theta 0.39
       where a linear read would be flat.

       What that costs in the mix is the top octave. Reading linearly left
       the interpolation's images unsmoothed, and against a hardware take
       of the first factory song this engine carried 15.2 % of its energy
       between 8 and 16 kHz where the machine carries 1.3 %. */
    double frac;
    double v0, v1, vBack, vFwd;
    if (voice->in_cycle) {
      /* Past the first turn, `position` is a position in the ping-pong
         cycle, not an index into `pcm`. Every one of the four taps goes
         through the same mapping, so the turn needs no special case: the
         interpolator reads across it exactly as it reads anywhere else. */
      long long c0 = (long long)voice->position;
      frac = voice->position - (double)c0;
      v0 = cycle_sample(voice, c0);
      v1 = cycle_sample(voice, c0 + 1);
      vBack = cycle_sample(voice, c0 - 1);
      vFwd = cycle_sample(voice, c0 + 2);
    } else {
    size_t i0 = (size_t)voice->position;
    /* The loop's last sample is a valid read head position - its partner
       for the interpolation is the loop's first sample - so only a head
       genuinely past it has run off the end. THE LOOP INCLUDES THAT LAST
       SAMPLE: reading straight on past it instead costs the loop a sample,
       a pitch error of one part in the loop's length, which is nothing on
       a long loop and 73.6 measured cents on the internal `Sine` wave's
       24-sample top zone. */
    bool atLoopEnd = voice->looping && i0 == voice->loop_last;
    if (!atLoopEnd && i0 + 1u >= voice->pcm_count) {
      if (!voice->looping) {
        voice->active = false;   /* the element is played out */
        break;
      }
      i0 = voice->loop_first;
      voice->position = (double)i0;
    }
    frac = voice->position - (double)i0;
    /* The uniform cubic B-spline basis, with `rest` = 1 - fraction:
         index - 1   rest^3 / 6
         index       2/3 - fraction^2 + fraction^3 / 2
         index + 1   2/3 - rest^2     + rest^3 / 2
         index + 2   fraction^3 / 6
       which is [1/6, 2/3, 1/6, 0] at fraction 0 - a smoother, not an
       identity - and sums to one at every fraction. */
    v0 = (double)voice->pcm[i0];
    v1 = wave_tap(voice, i0, 1, v0);
    vBack = wave_tap(voice, i0, -1, v0);
    vFwd = wave_tap(voice, i0, 2, v1);
    }
    double rest = 1.0 - frac;
    double sample = (rest * rest * rest / 6.0 * vBack +
                      (2.0 / 3.0 - frac * frac * (1.0 - frac * 0.5)) * v0 +
                      (2.0 / 3.0 - rest * rest * (1.0 - rest * 0.5)) * v1 +
                      frac * frac * frac / 6.0 * vFwd) / 8388608.0;

    /* The envelope, one segment at a time. The attack follows the measured
       front-loaded shape over its measured duration; the three falls are
       straight in dB, which is what a time-per-20-dB describes. Neither is
       the chip's segment stepper - that is internal (`M-011`'s own
       caveat). */
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
          voice->envelope = target * amp_env_attack_shape(done);
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
      if (voice->in_cycle) {
        /* Wrapped by the cycle, not by the loop's length: a full cycle is
           the reflected descending pass and the forward ascending one, so
           it is twice the loop. */
        double cycle =
          2.0 * (double)(voice->loop_last - voice->loop_first + 1u);
        while (voice->position >= cycle)
          voice->position -= cycle;
      } else if (voice->position >= (double)voice->loop_last + 1.0) {
        if (voice->ping_pong) {
          /* The head has read loop_last and turned. Cycle position 0 is the
             first reflected sample, at loop_last-1. */
          voice->in_cycle = true;
          voice->position -= (double)voice->loop_last + 1.0;
        } else if (voice->looping)
          voice->position -=
            (double)(voice->loop_last - voice->loop_first + 1u);
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
