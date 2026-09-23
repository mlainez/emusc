/* SPDX-License-Identifier: CC0-1.0 */
#include "stereo_eq.h"

#include "common/constants.h"
#include "efx.h"

#include <cstring>

namespace EmuSC { namespace Xp {

namespace {

/* The profile's tables this effect reads (devices/jv1080.cc). */
const unsigned kLevelTable = 0u;       /* 0x03856C */
const unsigned kLowShelf400 = 21u;     /* 0x039802, LowFreq != 0 */
const unsigned kHighShelf4k = 22u;     /* 0x0398BC, HiFreq 0 */
const unsigned kLowShelf200 = 23u;     /* 0x039748, LowFreq 0 */
const unsigned kHighShelf8k = 24u;     /* 0x039976, HiFreq != 0 */
const unsigned kPeakFrequency = 25u;   /* 0x039A30, 17 words */
const unsigned kPeakBoost = 26u;       /* 0x039A52, 85 x (t1, t3, t4) */
const unsigned kPeakBoostGain = 27u;   /* 0x039C50, 1275 x t6 */
const unsigned kPeakCut = 28u;         /* 0x03A646, 1275 x (w0..w3) */

/* The peaking writer's index arithmetic, `0x0A0023F8` (FW-EXACT): Q has
   five values, a boost or a cut fifteen, so one frequency spans 75 rows. */
const unsigned kQs = 5u;
const unsigned kRowsPerFrequency = 75u;
const uint8_t kFlatGain = 15u;

/* Stored byte maxima, `M-091`, which are also the tables' own extents. */
const uint8_t kMax[XP_STEREO_EQ_PARAMETERS] = {
  1u, 30u, 1u, 30u, 16u, 4u, 30u, 16u, 4u, 30u, 127u
};

const double kRegisterUnity = 512.0;

/* The XP coefficient law, as drive.cc states it and with the same
   standing (`U-R5-01`). */
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

bool shelf(const struct xp_rom *rom, unsigned table, uint8_t gain,
           struct xp_stereo_eq_shelf *s)
{
  double c[3];
  for (unsigned k = 0; k < 3u; ++k)
    if (!word(rom, table, gain, k, &c[k]))
      return false;
  s->b0 = (float)c[0];
  s->b1 = (float)c[1];
  s->a1 = (float)c[2];
  return true;
}

/* The seven words t0..t6 exactly as `0x0A0023F8` writes them. */
bool peak_words(const struct xp_rom *rom, uint8_t freq, uint8_t q,
                uint8_t gain, double t[7])
{
  for (unsigned k = 0; k < 7u; ++k)
    t[k] = 0.0;
  if (!word(rom, kPeakFrequency, freq, 0, &t[2]))
    return false;
  unsigned base = kRowsPerFrequency * freq + q;
  if (gain > kFlatGain) {
    if (!word(rom, kPeakBoost, kQs * freq + q, 0, &t[1]) ||
        !word(rom, kPeakBoost, kQs * freq + q, 1, &t[3]) ||
        !word(rom, kPeakBoost, kQs * freq + q, 2, &t[4]) ||
        !word(rom, kPeakBoostGain, base + kQs * (gain - kFlatGain - 1u), 0,
              &t[6]))
      return false;
    t[5] = 1.0;
  } else if (gain < kFlatGain) {
    unsigned row = base + kQs * gain;
    if (!word(rom, kPeakCut, row, 0, &t[0]) ||
        !word(rom, kPeakCut, row, 1, &t[3]) ||
        !word(rom, kPeakCut, row, 2, &t[4]) ||
        !word(rom, kPeakCut, row, 3, &t[6]))
      return false;
    t[1] = t[0];
    t[5] = 1.0;
  }
  return true;
}

/* THE PEAKING BAND.
 *
 * THE LOOP. Read off the tables' arithmetic, not an opcode. With f = t2,
 * q = -t3 and f1 = -t4 the loop above has the denominator
 *
 *   D = 1 + (f q + f f1 - 2) z^-1 + (1 - f q) z^-2
 *
 * t1 is 1.5925 q on every row to the words' rounding (half that at Q 0.5,
 * where t6 is doubled to keep the product), and t6 = 0.314 (V - 1),
 * V = 10^(dB/20). The tables were computed for the output
 * y = x + t6 t1 (B + B'): under it all 1275 boost rows peak at their
 * nominal frequency (within 0.25 %) and gain (within 0.007 dB), and all
 * 1275 cut rows are the exact inverse of the mirrored boost. Swapping the
 * two integrators, the other arrangement the words allow, moves those
 * peaks by hundreds of Hz above 3 kHz. That is what fixes D.
 *
 * THE OUTPUT IS THE HARDWARE'S CHOICE, NOT THE TABLES'. The machine
 * returns y = x + t6 t1 B', which peaks at +10.42 dB for a '+15 dB'
 * setting at 1 kHz and +1.06 dB for '+2', and drifts off the nominal
 * frequency near 8 kHz:
 *
 *   `closing/efx_sweep_01` P1 Q stepped 0..4 at the factory setting
 *   (P1 1 kHz, +2 dB), 261.6 Hz sine, level against Q 0:
 *     machine        0  -0.196  -0.263  -0.282  -0.288 dB
 *     y = x + G B'   0  -0.195  -0.262  -0.282  -0.287
 *     y = x + G B    0  -0.187  -0.247  -0.262  -0.266
 *     G(B + B')/2    0  -0.191  -0.255  -0.272  -0.277
 *     G(B + B')      0  -0.388  -0.521  -0.557  -0.567
 *
 *   `efx_probe` noise burst, STEREO-EQ over SPECTRUM (which calls the same
 *   writer for eight bands at Q 9), so the voice and the output chain
 *   cancel: 1/12-octave bands 60 Hz .. 13 kHz, one level offset removed,
 *   mean |error| 0.09 dB and at most 0.58 dB for B'; 0.41 (B), 0.25
 *   (G(B + B')/2) and 0.54 dB (G(B + B')). P-xxxx.
 *
 *   `efx_transfer/efx01` top step against `basic/single_note_dry`, same
 *   voice: +2.05 dB on the machine, +2.08 predicted, so the program adds
 *   no gain of its own beyond the bands and Level.
 *
 * THE CUT is the only setting that writes t0, and the machine adds it on
 * the CURRENT band state: y = x + t6 (t1 B' + t0 B), which with t0 = t1
 * is the tables' own algebra and gives the nominal dip. A capture made
 * for this (JV-1080 rig, 2026-09-23, `White Noise` through STEREO-EQ with
 * shelves and P2 flat, 1/6-octave bands 100 Hz .. 12 kHz against a flat
 * slot; canary and a re-take of `closing/efx_sweep_01` agreeing with the
 * corpus to 0.01 dB), P-xxxx:
 *
 *   P1 1 kHz Q 1  +15   machine +10.37 dB   this reading +10.38
 *                 -15            -14.88                  -14.94
 *                  -7             -6.92                   -6.95
 *                  -1             -0.97                   -0.98
 *   P1 4 kHz Q 4  -15            -14.13                  -14.08
 *
 * with mean |error| over the bands 0.01 to 0.04 dB on every slot. Leaving
 * t0 out would have made the -15 dB cut -4.6 dB.
 *
 * Nothing above was adjusted: every coefficient is a ROM word, and every
 * reading was put through once.
 *
 * AT GAIN 15 every word but t2 is zero, so the band's output is its input
 * exactly; the loop is skipped, since with q = f1 = 0 it would integrate
 * its input without bound while contributing nothing. */
void set_band(struct xp_stereo_eq_band *b, const double t[7], bool clear)
{
  b->f = (float)t[2];
  b->q = (float)-t[3];
  b->f1 = (float)-t[4];
  b->tap_delayed = (float)(t[6] * t[1]);
  b->tap_current = (float)(t[6] * t[0]);
  b->flat = t[1] == 0.0 && t[0] == 0.0;
  if (clear || b->flat)
    for (unsigned c = 0; c < 2u; ++c)
      b->low[c] = b->band[c] = 0.0f;
}

inline float run_shelf(struct xp_stereo_eq_shelf *s, unsigned c, float x)
{
  float y = s->b0 * x + s->b1 * s->x1[c] + s->a1 * s->y1[c];
  s->x1[c] = x;
  s->y1[c] = y;
  return y;
}

inline float run_band(struct xp_stereo_eq_band *b, unsigned c, float x)
{
  if (b->flat)
    return x;
  float prev = b->band[c];
  float low = b->low[c] + b->f1 * prev;
  float high = x - low - b->q * prev;
  float band = prev + b->f * high;
  b->low[c] = low;
  b->band[c] = band;
  return x + b->tap_delayed * prev + b->tap_current * band;
}

}  // namespace

bool stereo_eq_parameter_valid(unsigned index, uint8_t value)
{
  return index < XP_STEREO_EQ_PARAMETERS && value <= kMax[index];
}

void stereo_eq_clear(struct xp_stereo_eq *eq)
{
  struct xp_stereo_eq_shelf *s[2] = { &eq->low_shelf, &eq->high_shelf };
  for (unsigned c = 0; c < 2u; ++c) {
    for (unsigned k = 0; k < 2u; ++k)
      s[k]->x1[c] = s[k]->y1[c] = 0.0f;
    for (unsigned k = 0; k < 2u; ++k)
      eq->peak[k].low[c] = eq->peak[k].band[c] = 0.0f;
  }
}

bool stereo_eq_set(const struct xp_rom *rom, struct xp_stereo_eq *eq,
                    const uint8_t p[XP_STEREO_EQ_PARAMETERS])
{
  if (!rom || !rom->bytes || !eq)
    return false;
  for (unsigned i = 0; i < XP_STEREO_EQ_PARAMETERS; ++i)
    if (!stereo_eq_parameter_valid(i, p[i]))
      return false;
  struct xp_stereo_eq built = *eq;
  if (!shelf(rom, p[0] ? kLowShelf400 : kLowShelf200, p[1], &built.low_shelf) ||
      !shelf(rom, p[2] ? kHighShelf8k : kHighShelf4k, p[3], &built.high_shelf))
    return false;
  for (unsigned k = 0; k < 2u; ++k) {
    const uint8_t *band = p + 4u + 3u * k;
    double t[7];
    if (!peak_words(rom, band[0], band[1], band[2], t))
      return false;
    bool moved = !eq->ready || std::memcmp(band, eq->param + 4u + 3u * k, 3u);
    set_band(&built.peak[k], t, moved);
  }
  uint16_t raw = 0;
  if (!efx_table_value(rom, kLevelTable, p[10], 0, &raw))
    return false;
  built.level = (float)((double)(raw >> 4) / kRegisterUnity);
  std::memcpy(built.param, p, sizeof built.param);
  built.ready = true;
  *eq = built;
  return true;
}

void stereo_eq_process(struct xp_stereo_eq *eq, const float *inL,
                        const float *inR, float *outL, float *outR,
                        size_t frames)
{
  const float *in[2] = { inL, inR };
  float *out[2] = { outL, outR };
  for (unsigned c = 0; c < 2u; ++c)
    for (size_t k = 0; k < frames; ++k) {
      float w = run_shelf(&eq->low_shelf, c, in[c][k]);
      w = run_shelf(&eq->high_shelf, c, w);
      w = run_band(&eq->peak[0], c, w);
      w = run_band(&eq->peak[1], c, w);
      out[c][k] = eq->level * w;
    }
}

}}  // namespace EmuSC::Xp
