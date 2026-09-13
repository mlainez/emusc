/* SPDX-License-Identifier: CC0-1.0 */
/* See sc88_output.h for what this stage is and why it is not part of any
   ROM-derived law. */
#include "sc88_output.h"

#include <math.h>
#include <string.h>

/* The response list is EMPTY, and that is the finding.

   A high shelf stood here: 6692.7 Hz, +8.986 dB, Q 0.910, the
   least-squares inverse of the whole-set median band deficit. It was
   standing in for high frequency the engine lost in its own TVF, whose
   trapezoidal integrators are a bilinear transform and left a double
   zero at Nyquist, so the two poles the ROM asks for rolled off like
   three. With the TVF realised in the topology the chip's limit table
   names (sc88_tvf.c, SC88_TVF_LIMIT_TABLE) there is nothing left for the
   shelf to stand in for.

   Measured, 63 single notes against the archive recordings, median band
   error of ours minus the hardware, level-matched per tone:

     trapezoidal, no shelf   median MAD 2.94 dB   median tilt -1.56 dB/oct
     trapezoidal + shelf           1.81                  -0.36
     forward Euler + shelf         2.73                  +1.23
     forward Euler, no shelf       1.77                  +0.25

   The structure change on its own does what the fitted shelf did and a
   little more, and the two together over-correct. Per band, with no
   shelf, the median error is now within 0.7 dB of flat from 111 Hz to
   5.9 kHz and runs +1.3 dB at 7.2 kHz, +2.6 at 8.9 and +4.1 at 11.0.

   Nothing is fitted to that remainder, on purpose. Split by the tone's
   own cutoff it is not one response: tones cut off below 3.2 kHz are
   flat to 0.7 dB across the whole band, while tones cut off above
   3.2 kHz carry all of it. A fixed output section cannot be that shape,
   and fitting one to the median would darken the tones that are already
   right - which is exactly what the shelf it replaces did in the other
   direction, brightening Bagpipe from 0.44 to 1.59 dB MAD and Oboe from
   0.61 to 1.53. The remainder is the open question in TASK-193, not an
   output response.

   The stage stays, with the DC blocker in it, because it is the one
   labelled place for this class of thing. C has no empty array, so one
   zeroed entry stands in the list and the count is zero. */
const struct sc88_output_section SC88_OUTPUT_RESPONSE[1] = {
  { SC88_OUTPUT_PEAKING, 0.0f, 0.0f, 0.0f },
};
const unsigned SC88_OUTPUT_RESPONSE_SECTIONS = 0;

/* THE CONVERTER'S HOLD, and it is measured rather than assumed.

   The SC-88's parts list gives a PCM69AU-1/T2 and no oversampling filter
   in front of it, so the DAC should be a plain zero-order hold at the
   chip's 32 kHz - a sin(x)/x droop of -0.48 dB at 5.9 kHz rising to
   -2.72 at 13.5 and -3.92 at Nyquist. Our engine runs the whole voice
   path at the host rate and emits samples, not steps, so it carried none
   of it.

   The measurement that establishes the hold, and it does not use the
   engine at all. A digital filter clocked at 32 kHz has
   |H(32000 - f)| = |H(f)|, so everything digital in the machine attenuates
   a DAC image exactly as much as the baseband component that produced it
   and cancels in their ratio. The hold and the analog board do not. The
   archive recordings carry the images: correlating each recording's
   spectrum above 16.3 kHz against its own baseband mirrored about a
   candidate centre, swept from 29 to 35 kHz, peaks at 32.000 kHz and
   nowhere else - 0.92 on Bagpipe, 0.91 on Harpsichord, 0.84 on Seashore,
   0.81 on Accordion, 0.79 on Applause, and near zero or negative at
   every other centre tried.

   Their ratio, ten recordings, image at 32000-f over baseband at f, f
   from 11.0 to 15.8 kHz:

     f base kHz      11.0  12.0  13.0  14.0  15.0  15.8
     measured dB     -7.3  -5.7  -4.3  -2.9  -1.4  -0.1
     sin(x)/x dB     -5.6  -4.4  -3.3  -2.2  -1.1  -0.3
     residual        -1.7  -1.3  -1.0  -0.7  -0.3  +0.2

   So the hold is there, at 32 kHz, to within a dB and a half over a ten
   kilohertz span - and the residual is the whole analog path, device plus
   whatever recorded these, which is down only 1.7 dB from 11 kHz to
   21 kHz. That is the reason the response list above this one is still
   empty: an output filter steep enough to matter in the audio band would
   have crushed these images, and it did not.

   Whole board, 63 single notes against the archive recordings, wet at
   CC91 24: median MAD 1.5 -> 1.2 dB, 57 of 63 within 3 dB unchanged, 2
   past 6 dB unchanged, tilted bright 9 -> 7.

   [MEASURED]. It is the converter, not the chip, so it is here and not in
   the engine. Delete it the day libEmuSC's SC-88 path emits at 32 kHz and
   reconstructs properly, because then it is already in the signal. */
static double sc88_output_i0(double x)
{
  double sum = 1.0, term = 1.0, xh = 0.5 * x;
  int k;
  for (k = 1; k <= 25; ++k) {
    term *= xh / k;
    sum += term * term;
  }
  return sum;
}

/* The zero-phase filter whose magnitude is |sin(pi f / 32000) /
   (pi f / 32000)| over the whole output band, by frequency sampling, then
   a Kaiser window so a 31-tap truncation does not ripple. */
static void sc88_output_design_hold(struct sc88_output *out, double rate)
{
  const double pi = 3.14159265358979323846;
  const int half = SC88_OUTPUT_HOLD_TAPS / 2;
  const int steps = 4096;
  const double beta = 7.0;
  const double i0beta = sc88_output_i0(beta);
  double h[SC88_OUTPUT_HOLD_TAPS];
  double sum = 0.0;
  int k, j;

  for (k = -half; k <= half; ++k) {
    double acc = 0.0;
    for (j = 0; j <= steps; ++j) {
      double f = 0.5 * rate * (double)j / (double)steps;
      double x = pi * f / SC88_OUTPUT_DAC_RATE;
      double mag = (x > 1e-12) ? fabs(sin(x) / x) : 1.0;
      double w = (j == 0 || j == steps) ? 0.5 : 1.0;
      acc += w * mag * cos(2.0 * pi * f * (double)k / rate);
    }
    acc *= (0.5 * rate / (double)steps) * 2.0 / rate;
    {
      double wn = (double)k / (double)half;
      double win = sc88_output_i0(beta * sqrt(fmax(0.0, 1.0 - wn * wn))) /
        i0beta;
      h[k + half] = acc * win;
    }
    sum += h[k + half];
  }
  for (k = 0; k < SC88_OUTPUT_HOLD_TAPS; ++k)
    out->hold[k] = (float)(h[k] / sum);
  out->hold_taps = SC88_OUTPUT_HOLD_TAPS;
  out->hold_pos = 0;
}

/* Audio EQ Cookbook forms, normalised by a0. */
static void sc88_output_design(struct sc88_output_biquad *bq,
                               const struct sc88_output_section *s,
                               double rate)
{
  double a = pow(10.0, s->gain_db / 40.0);
  double w0 = 2.0 * 3.14159265358979323846 * s->frequency / rate;
  double cw, sw, alpha;
  double b0, b1, b2, a0, a1, a2;

  memset(bq, 0, sizeof *bq);
  if (!(w0 > 0.0) || w0 >= 3.14159265358979323846) {
    bq->b0 = 1.0f;
    return;
  }
  cw = cos(w0);
  sw = sin(w0);
  alpha = sw / (2.0 * (s->q > 0.0f ? s->q : 0.7071));

  switch (s->type) {
  case SC88_OUTPUT_HIGH_SHELF: {
    double sq = 2.0 * sqrt(a) * alpha;
    b0 = a * ((a + 1.0) + (a - 1.0) * cw + sq);
    b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cw);
    b2 = a * ((a + 1.0) + (a - 1.0) * cw - sq);
    a0 = (a + 1.0) - (a - 1.0) * cw + sq;
    a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cw);
    a2 = (a + 1.0) - (a - 1.0) * cw - sq;
    break;
  }
  case SC88_OUTPUT_LOW_SHELF: {
    double sq = 2.0 * sqrt(a) * alpha;
    b0 = a * ((a + 1.0) - (a - 1.0) * cw + sq);
    b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cw);
    b2 = a * ((a + 1.0) - (a - 1.0) * cw - sq);
    a0 = (a + 1.0) + (a - 1.0) * cw + sq;
    a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cw);
    a2 = (a + 1.0) + (a - 1.0) * cw - sq;
    break;
  }
  default:
    b0 = 1.0 + alpha * a;
    b1 = -2.0 * cw;
    b2 = 1.0 - alpha * a;
    a0 = 1.0 + alpha / a;
    a1 = -2.0 * cw;
    a2 = 1.0 - alpha / a;
    break;
  }
  bq->b0 = (float)(b0 / a0);
  bq->b1 = (float)(b1 / a0);
  bq->b2 = (float)(b2 / a0);
  bq->a1 = (float)(a1 / a0);
  bq->a2 = (float)(a2 / a0);
}

void sc88_output_init(struct sc88_output *out, double rate)
{
  unsigned i;
  if (!out)
    return;
  memset(out, 0, sizeof *out);
  if (!(rate > 0.0))
    rate = 32000.0;
  out->sections = SC88_OUTPUT_RESPONSE_SECTIONS;
  if (out->sections > SC88_OUTPUT_MAX_SECTIONS)
    out->sections = SC88_OUTPUT_MAX_SECTIONS;
  for (i = 0; i < out->sections; ++i)
    sc88_output_design(&out->section[i], &SC88_OUTPUT_RESPONSE[i], rate);
  sc88_output_design_hold(out, rate);
  /* A 10 Hz single-pole blocker: 0.03 dB at 111 Hz and unity everywhere
     the audit measures, so it is not what removes the high frequency
     above - but it is not cited to any ROM either, and libEmuSC's SC-55
     path has no output blocker at all, so it is declared here with the
     rest of the unverified output behaviour rather than inline. */
  out->dc_pole = (float)(1.0 - 2.0 * 3.14159265358979323846 * 10.0 / rate);
  out->enabled = true;
}

void sc88_output_reset(struct sc88_output *out)
{
  unsigned i;
  if (!out)
    return;
  for (i = 0; i < SC88_OUTPUT_MAX_SECTIONS; ++i) {
    memset(out->section[i].x1, 0, sizeof out->section[i].x1);
    memset(out->section[i].x2, 0, sizeof out->section[i].x2);
    memset(out->section[i].y1, 0, sizeof out->section[i].y1);
    memset(out->section[i].y2, 0, sizeof out->section[i].y2);
  }
  out->dc_x[0] = out->dc_x[1] = 0.0f;
  out->dc_y[0] = out->dc_y[1] = 0.0f;
  memset(out->hold_z, 0, sizeof out->hold_z);
  out->hold_pos = 0;
}

void sc88_output_process(struct sc88_output *out, float *stereo,
                         size_t frames)
{
  size_t k;
  unsigned ch, i;
  if (!out || !out->enabled || !stereo)
    return;
  for (k = 0; k < frames; ++k)
    for (ch = 0; ch < 2; ++ch) {
      float x = stereo[k * 2 + ch];
      float y;
      for (i = 0; i < out->sections; ++i) {
        struct sc88_output_biquad *b = &out->section[i];
        y = b->b0 * x + b->b1 * b->x1[ch] + b->b2 * b->x2[ch] -
          b->a1 * b->y1[ch] - b->a2 * b->y2[ch];
        b->x2[ch] = b->x1[ch];
        b->x1[ch] = x;
        b->y2[ch] = b->y1[ch];
        b->y1[ch] = y;
        x = y;
      }
      y = x - out->dc_x[ch] + out->dc_pole * out->dc_y[ch];
      out->dc_x[ch] = x;
      out->dc_y[ch] = y;
      stereo[k * 2 + ch] = y;
    }
  for (k = 0; k < frames; ++k) {
    unsigned pos = out->hold_pos;
    for (ch = 0; ch < 2; ++ch) {
      float acc = 0.0f;
      unsigned t;
      out->hold_z[ch][pos] = stereo[k * 2 + ch];
      for (t = 0; t < out->hold_taps; ++t) {
        unsigned idx = (pos + out->hold_taps - t) % out->hold_taps;
        acc += out->hold[t] * out->hold_z[ch][idx];
      }
      stereo[k * 2 + ch] = acc;
    }
    out->hold_pos = (pos + 1) % out->hold_taps;
  }
}
