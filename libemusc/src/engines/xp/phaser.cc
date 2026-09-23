/* SPDX-License-Identifier: CC0-1.0 */
#include "phaser.h"

#include "efx.h"

#include <cstring>

namespace EmuSC { namespace Xp {

namespace {

/* The profile's tables this effect reads (devices/jv1080.cc). */
const unsigned kLevelTable = 0u;     /* 0x03856C */
const unsigned kDepthTable = 1u;     /* 0x038732 */
const unsigned kResTable = 3u;       /* 0x038932 */
const unsigned kManualTable = 4u;    /* 0x038A32 */
const unsigned kRateTable = 5u;      /* 0x038B2E */
const unsigned kPanTable = 13u;      /* 0x0392A0, (L, R) */

const unsigned kManual = 0u, kRate = 1u, kDepth = 2u, kRes = 3u, kMix = 4u,
  kPan = 5u, kLevel = 6u;

/* The updater clamps Manual and Rate to 125; the rest take a 7-bit value. */
const uint8_t kMax[XP_PHASER_PARAMETERS] = {
  125u, 125u, 127u, 127u, 127u, 127u, 127u
};

/* CRAM unity, and the 9-bit level register's. */
const double kUnity = 8192.0;
const double kRegisterUnity = 512.0;

/* The Manual word's own scale: r = round(-512 a) over the whole table
   (phaser.h). */
const double kManualScale = 512.0;

/* MEASURED, NOT READ (phaser.h): the hardware's coefficient over -r/512.
   `phaser_char` static slots, Depth 0, each fitted over 150 Hz-12 kHz:
   Manual 32 0.9003, 40 0.9006, 45 0.8995 / 0.9075 / 0.8972 (Res 95, 127,
   0), 55 0.9027, 80 0.9083, 100 0.8945, 125 0.8997; Manual 65 (r = 0)
   reads a = +0.001. And the depth-127 sweep from Manual 32 parks at
   a = -0.900, which is this scale times the clamp. P-xxxx. */
const float kCoefficientScale = 0.900f;

/* The mono sum instructions 0..2 form, CRAM +0.5 on each input. */
const float kInputWeight = 0.5f;

/* A 24-bit phase accumulator stepped by the rate word once a sample, which
   is `word * 32000 / 2^24` Hz at the DSP's rate. */
const uint32_t kPhaseMask = 0xFFFFFFu;
const float kPhaseFull = 16777216.0f;

bool unit(const struct xp_rom *rom, unsigned table, unsigned index,
          unsigned column, float *out)
{
  uint16_t raw = 0;
  if (!efx_table_value(rom, table, index, column, &raw))
    return false;
  *out = (float)((double)raw / kUnity);
  return true;
}

inline float triangle(uint32_t phase)
{
  float u = (float)phase / kPhaseFull;
  float t = 2.0f * u - 1.0f;
  return 1.0f - (t < 0.0f ? -t : t);
}

}  // namespace

bool phaser_parameter_valid(unsigned index, uint8_t value)
{
  return index < XP_PHASER_PARAMETERS && value <= kMax[index];
}

/* HOW FAR THIS IS MEASURED (`phaser_char`, rig take 2026-09-23, white
 * noise through a flat voice, each slot against the slot with Mix 0; and
 * `efx_pnoise/efx04_phaser`). The model is the one in phaser.h with every
 * word from the ROM and only the 0.900 measured; the coefficient was the
 * one free number fitted per static slot, and it is what gave the 0.900.
 *
 *   Static, Manual 55 Res 79: hardware notches 1125, 3680, 7195, 12516 Hz
 *   and peaks 2086, 4617, 8297, 13570 Hz (+9.1, +8.8, +7.8, +4.9 dB); the
 *   model at the fitted coefficient 1133, 3672, 7170, 12479 and 2085, 4609,
 *   8222, 13824 (+9.3, +8.9, +7.9, +5.1). Over ten static slots - Manual
 *   32..125, Res 0, 79, 95, 127, Mix 125 and 127 - the model sits 0.22 to
 *   0.49 dB rms from the take on 1/12-octave bands, 0.80 at Res 127 and
 *   1.20 at Res 0, where notch depth dominates the error.
 *
 *   Swept, the coefficient tracked frame by frame and fitted with a clamped
 *   triangle: excursion over the depth word 0.7800/0.7754 (Depth 127,
 *   Manual 100), 0.7750/0.7754 (Depth 127, Manual 32, clamped), 0.6175/
 *   0.6150 (107), 0.3925/0.3922 (75), 0.1950/0.1892 (40); rate 0.2998 Hz
 *   against 0.2995 from the table (Rate 5), 0.2501 against 0.2499 (Rate
 *   4). A sine-shaped LFO fits four times worse than the triangle.
 *   P-xxxx. */
bool phaser_set(const struct xp_rom *rom, struct xp_phaser *ph,
                 const uint8_t p[XP_PHASER_PARAMETERS])
{
  if (!rom || !rom->bytes || !ph)
    return false;
  for (unsigned i = 0; i < XP_PHASER_PARAMETERS; ++i)
    if (!phaser_parameter_valid(i, p[i]))
      return false;
  struct xp_phaser built = *ph;
  uint16_t manual = 0, rate = 0, level = 0;
  if (!efx_table_value(rom, kManualTable, p[kManual], 0u, &manual) ||
      !efx_table_value(rom, kRateTable, p[kRate], 0u, &rate) ||
      !efx_table_value(rom, kLevelTable, p[kLevel], 0u, &level) ||
      !unit(rom, kDepthTable, p[kDepth], 0u, &built.depth) ||
      !unit(rom, kResTable, p[kRes], 0u, &built.feedback) ||
      !unit(rom, kLevelTable, p[kMix], 0u, &built.mix) ||
      !unit(rom, kPanTable, p[kPan], 0u, &built.pan_left) ||
      !unit(rom, kPanTable, p[kPan], 1u, &built.pan_right))
    return false;
  /* The word is ten bits, two's complement: 0x3F3 is -13, not 1011. */
  int r = manual & 0x3ffu;
  if (r & 0x200)
    r -= 0x400;
  built.manual = (float)(-(double)r / kManualScale);
  built.lfo_step = rate;
  built.level = (float)((double)(level >> 4) / kRegisterUnity);
  std::memcpy(built.param, p, sizeof built.param);
  built.ready = true;
  *ph = built;
  return true;
}

float phaser_coefficient(const struct xp_phaser *ph, float tri)
{
  float u = ph->manual - ph->depth * tri;
  if (u < -1.0f)
    u = -1.0f;
  else if (u > 1.0f)
    u = 1.0f;
  return kCoefficientScale * u;
}

void phaser_process(struct xp_phaser *ph, const float *inL, const float *inR,
                     float *outL, float *outR, size_t frames)
{
  float gl = ph->level * ph->pan_left;
  float gr = ph->level * ph->pan_right;
  for (size_t k = 0; k < frames; ++k) {
    float a = phaser_coefficient(ph, triangle(ph->lfo_phase));
    ph->lfo_phase = (ph->lfo_phase + ph->lfo_step) & kPhaseMask;
    float x = kInputWeight * (inL[k] + inR[k]);
    float v = x + ph->feedback * ph->feedback_state;
    for (unsigned s = 0; s < XP_PHASER_STAGES; ++s) {
      float y = a * v + ph->stage_x[s] - a * ph->stage_y[s];
      ph->stage_x[s] = v;
      ph->stage_y[s] = y;
      v = y;
    }
    ph->feedback_state = v;
    float w = x + ph->mix * v;
    outL[k] = gl * w;
    outR[k] = gr * w;
  }
}

}}  // namespace EmuSC::Xp
