/* SPDX-License-Identifier: CC0-1.0 */
/* See sc88_output.h for what this stage is and why it is not part of any
   ROM-derived law. */
#include "sc88_output.h"

#include <math.h>
#include <string.h>

/* [MEASURED] - not [FW-EXACT]. One high shelf, standing in for high
   frequency the engine loses somewhere between the wave and the output
   and which no ROM table accounts for.

   What it is fitted to. Over the 61-instrument single-note audit against
   the archive recordings, with the TVF cutoff law and the damping
   normalisation both firmware-correct, our render's median band error
   against the hardware is flat to within 0.9 dB from 111 Hz to 4.8 kHz
   and then falls away: -2.63 dB at 5.9 kHz, -5.48 at 7.2 kHz, -8.73 at
   8.9 kHz. This shelf is the least-squares inverse of that deficit over
   479 Hz to 8.9 kHz, and reproduces it to an rms of 0.30 dB.

   Why the fit stops at 8.9 kHz. Above it our own signal is already 15 to
   23 dB down, so a boost has almost nothing left to act on; and the
   archive's own noise floor is close enough there that the deficit
   cannot be read cleanly. The shelf therefore under-corrects above
   9 kHz on purpose rather than amplifying what is nearly silence.

   What it is standing in for, and why it is a stand-in rather than a
   model. The deficit is not a fixed output response: measured at matched
   fractions of each tone's own cutoff it is the same curve for every
   tone, which means it belongs to the filter, not to the connector. Two
   terms are visible in it. The first is ours and is understood exactly:
   sc88_tvf.c realises its two-pole with trapezoidal integrators, whose
   bilinear mapping leaves a double zero at Nyquist, so our two-pole
   rolls off like three at the top of the band - worth 0.5 dB at 2x the
   corner for a tone cut off at 1.6 kHz and 7.5 dB for one cut off at
   5.3 kHz. The second is unresolved: even where that warping is
   negligible the hardware's rolloff above its corner is gentler than a
   two-pole at the computed cutoff, which is either a corner higher than
   the ROM law states or a dry signal of ours that is already too dark.
   Neither is settled here, so neither is written into a law. When either
   is, this section shrinks or goes. */
const struct sc88_output_section SC88_OUTPUT_RESPONSE[] = {
  { SC88_OUTPUT_HIGH_SHELF, 6692.7f, +8.986f, 0.910f },
};
const unsigned SC88_OUTPUT_RESPONSE_SECTIONS =
  (unsigned)(sizeof SC88_OUTPUT_RESPONSE / sizeof SC88_OUTPUT_RESPONSE[0]);

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
}
