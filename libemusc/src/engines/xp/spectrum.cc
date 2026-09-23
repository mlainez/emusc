/* SPDX-License-Identifier: CC0-1.0 */
#include "spectrum.h"

#include "efx.h"

#include <cstring>

namespace EmuSC { namespace Xp {

namespace {

/* The profile's tables this effect reads (devices/jv1080.cc). */
const unsigned kLevelTable = 0u;       /* 0x03856C */
const unsigned kPanTable = 13u;        /* 0x0392A0, (L, R) */
const unsigned kBandFrequency = 29u;   /* 0x03CEF6, eight index bytes */

const unsigned kWidth = 8u;
const unsigned kPan = 9u;
const unsigned kLevel = 10u;

/* Stored byte maxima (`M-091`), which are also the peaking tables' own
   extents for the bands and Width. */
const uint8_t kMax[XP_SPECTRUM_PARAMETERS] = {
  30u, 30u, 30u, 30u, 30u, 30u, 30u, 30u, 4u, 127u, 127u
};

const double kRegisterUnity = 512.0;

/* The mono sum instructions 0..2 form, CRAM +0.5 on each input. */
const float kInputWeight = 0.5f;

bool reg9(const struct xp_rom *rom, unsigned table, unsigned index,
          unsigned column, float *out)
{
  uint16_t raw = 0;
  if (!efx_table_value(rom, table, index, column, &raw))
    return false;
  *out = (float)((double)(raw >> 4) / kRegisterUnity);
  return true;
}

/* The frequency index of band `i`: byte i of 0x03CEF6, two to a word. */
bool band_frequency(const struct xp_rom *rom, unsigned i, uint8_t *out)
{
  uint16_t raw = 0;
  if (!efx_table_value(rom, kBandFrequency, 0u, i / 2u, &raw))
    return false;
  *out = (uint8_t)(i % 2u ? raw & 0xffu : raw >> 8);
  return true;
}

}  // namespace

bool spectrum_parameter_valid(unsigned index, uint8_t value)
{
  return index < XP_SPECTRUM_PARAMETERS && value <= kMax[index];
}

/* HOW FAR THIS IS MEASURED. Nothing below was adjusted: every word is a
 * ROM word through STEREO-EQ's band, which was itself measured on STEREO-EQ,
 * and every reading was put through once.
 *
 *   `closing/efx_sweep_05` (M-096), `Sine` at key 60 (261.6 Hz), factory
 *   bytes 19 20 19 18 21 20 21 20 4 64 114, fundamental against the first
 *   slot:
 *
 *     Band1  0  8  15  22  30   machine 0  +7.569 +12.834 +15.698 +21.158
 *                               this    0  +7.581 +12.849 +15.713 +21.159
 *     Width  0 .. 4             machine +18.768 +16.732 +15.566 +14.972 +14.248
 *                               this    +18.778 +16.746 +15.581 +14.987 +14.263
 *
 *   within 0.015 dB on every slot, cut rows (Band1 0 and 8) included, which
 *   is the band on its own and not a cross-check through STEREO-EQ. P-xxxx.
 *
 *   The same band was earlier read as the ratio of the two types' takes on
 *   one noise burst (stereo_eq.cc); this is the direct reading. */
bool spectrum_set(const struct xp_rom *rom, struct xp_spectrum *sp,
                   const uint8_t p[XP_SPECTRUM_PARAMETERS])
{
  if (!rom || !rom->bytes || !sp)
    return false;
  for (unsigned i = 0; i < XP_SPECTRUM_PARAMETERS; ++i)
    if (!spectrum_parameter_valid(i, p[i]))
      return false;
  struct xp_spectrum built = *sp;
  bool width_moved = !sp->ready || p[kWidth] != sp->param[kWidth];
  for (unsigned i = 0; i < XP_SPECTRUM_BANDS; ++i) {
    uint8_t freq = 0;
    if (!band_frequency(rom, i, &freq))
      return false;
    bool moved = width_moved || p[i] != sp->param[i];
    if (!stereo_eq_band_set(rom, &built.band[i], freq, p[kWidth], p[i],
                            moved))
      return false;
  }
  if (!reg9(rom, kPanTable, p[kPan], 0u, &built.pan_left) ||
      !reg9(rom, kPanTable, p[kPan], 1u, &built.pan_right) ||
      !reg9(rom, kLevelTable, p[kLevel], 0u, &built.level))
    return false;
  std::memcpy(built.param, p, sizeof built.param);
  built.ready = true;
  *sp = built;
  return true;
}

void spectrum_process(struct xp_spectrum *sp, const float *inL,
                       const float *inR, float *outL, float *outR,
                       size_t frames)
{
  float gl = sp->level * sp->pan_left;
  float gr = sp->level * sp->pan_right;
  for (size_t k = 0; k < frames; ++k) {
    float w = kInputWeight * (inL[k] + inR[k]);
    for (unsigned i = 0; i < XP_SPECTRUM_BANDS; ++i)
      w = stereo_eq_band_run(&sp->band[i], 0u, w);
    outL[k] = gl * w;
    outR[k] = gr * w;
  }
}

}}  // namespace EmuSC::Xp
