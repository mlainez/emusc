/* SPDX-License-Identifier: CC0-1.0 */
#include "enhancer.h"

#include "common/constants.h"
#include "efx.h"

#include <cstring>

namespace EmuSC { namespace Xp {

namespace {

/* The profile's tables this effect reads (devices/jv1080.cc). */
const unsigned kLevelTable = 0u;       /* 0x03856C, Mix and Level */
const unsigned kSensTable = 2u;        /* 0x038832 */
const unsigned kLowShelf400 = 21u;     /* 0x039802 */
const unsigned kHighShelf4k = 22u;     /* 0x0398BC */

const unsigned kEnhancerType = 5u;     /* 0-based; loads bank slot 2 */

/* Slot 2's image: the words the updater never writes. */
const unsigned kHighWords[2] = { 22u, 51u };
const unsigned kLowWords[2] = { 33u, 62u };
const unsigned kGainA = 28u;
const unsigned kGainB = 30u;
const unsigned kTrim = 32u;

const uint8_t kMax[XP_ENHANCER_PARAMETERS] = { 127u, 127u, 30u, 30u, 127u };

const double kRegisterUnity = 512.0;

/* drive.cc's clamp, for the reason enhancer.h gives. */
const float kSaturation = 1.0f;

/* The XP coefficient law, as drive.cc states it and with the same
   standing (`U-R5-01`). */
double xp(uint16_t raw)
{
  int value = raw & 0x3fff;
  if (value & 0x2000)
    value -= 0x4000;
  return (double)value * (double)(1u << kXpCoefficientShift[raw >> 14]) / 8192.0;
}

bool reg9(const struct xp_rom *rom, unsigned table, unsigned index,
          float *out)
{
  uint16_t raw = 0;
  if (!efx_table_value(rom, table, index, 0u, &raw))
    return false;
  *out = (float)((double)(raw >> 4) / kRegisterUnity);
  return true;
}

/* A shelf block's triple, (b0, b1, a1) as the table stores it. */
bool shelf(const struct xp_rom *rom, unsigned table, uint8_t gain,
           struct xp_enhancer_fo *s)
{
  uint16_t raw[3];
  for (unsigned k = 0; k < 3u; ++k)
    if (!efx_table_value(rom, table, gain, k, &raw[k]))
      return false;
  s->b0 = (float)xp(raw[0]);
  s->b1 = (float)xp(raw[1]);
  s->a1 = (float)xp(raw[2]);
  return true;
}

/* An image section at CRAM i..i+2, in program order (b1, b0, a1). */
void image_section(const struct xp_efx_program *prog, unsigned i,
                   struct xp_enhancer_fo *s)
{
  s->b1 = (float)xp(prog->cram[i]);
  s->b0 = (float)xp(prog->cram[i + 1u]);
  s->a1 = (float)xp(prog->cram[i + 2u]);
}

inline float fo(struct xp_enhancer_fo *s, float x)
{
  float y = s->b0 * x + s->b1 * s->x1 + s->a1 * s->y1;
  s->x1 = x;
  s->y1 = y;
  return y;
}

inline float clamp(float u)
{
  if (u > kSaturation)
    return kSaturation;
  if (u < -kSaturation)
    return -kSaturation;
  return u;
}

}  // namespace

bool enhancer_parameter_valid(unsigned index, uint8_t value)
{
  return index < XP_ENHANCER_PARAMETERS && value <= kMax[index];
}

/* HOW FAR THIS IS MEASURED.
 *
 * `efx_pnoise/efx06` (M-096 family), `White Noise` through the factory
 * setting (Sens 88, Mix 97, LowGain 20, HiGain 23, Level 111) and then with
 * one byte moved, each slot against the factory one in 1/3-octave bands:
 *
 *   Sens 88 -> 0, left   566 Hz  1.4k  2.3k  3.6k  5.7k  7.2k  11.4k
 *     machine            -0.6   -2.6  -4.8  -7.2  -8.7  -8.9   -6.1 dB
 *     this chain         -0.6   -2.7  -4.8  -7.1  -8.7  -8.8   -6.0
 *   right                -0.7   -3.0  -5.3  -7.7  -9.3  -9.4   -6.7
 *                        -0.7   -3.0  -5.3  -7.7  -9.3  -9.4   -6.5
 *
 * and Mix 97 -> 0 the same to 0.2 dB, in every band from 50 Hz to 12.7 kHz.
 * The right channel's extra half dB is the crossed corners. Nothing was
 * adjusted; every coefficient is a ROM word. The other reading of the
 * section words - the highpass negated, so the enhancement subtracts - puts
 * the Sens 0 slot at +1.4 dB where the machine reads -8.9: that is what
 * settles the sign. P-xxxx.
 *
 * THE M-096 SWEEP GAP, STATED. The Sens sweep, `closing/efx_sweep_06`,
 * reads "nothing above the noise" (`insert_effect_behaviour.md`): its
 * probe is a 261.6 Hz sine, which the 7 kHz highpass removes before Sens
 * applies, so that sweep says nothing about this effect and is not claimed
 * as confirming it. What confirms it is the noise reading above, at two
 * settings of each of Sens and Mix, not a curve across either.
 *
 * AND WHAT IS NOT MEASURED: the clamp. Rendered here, that noise take
 * drives the clamp to 0.64 at its peak, 3.9 dB short of it, and the
 * corpus's other ENHANCER takes are sines the highpass removes; so where
 * it bends on loud, bright material rests on drive.cc's measurement of the
 * same gain staging in the drive program, not on this type's. */
bool enhancer_set(const struct xp_rom *rom, struct xp_enhancer *en,
                   const uint8_t p[XP_ENHANCER_PARAMETERS])
{
  if (!rom || !rom->bytes || !en)
    return false;
  for (unsigned i = 0; i < XP_ENHANCER_PARAMETERS; ++i)
    if (!enhancer_parameter_valid(i, p[i]))
      return false;
  struct xp_efx_program prog;
  if (!efx_program_load(rom, kEnhancerType, &prog))
    return false;
  struct xp_enhancer built = *en;
  float sens = 0.0f;
  if (!reg9(rom, kSensTable, p[0], &sens) ||
      !reg9(rom, kLevelTable, p[1], &built.mix) ||
      !reg9(rom, kLevelTable, p[4], &built.level))
    return false;
  for (unsigned c = 0; c < 2u; ++c) {
    struct xp_enhancer_channel *ch = &built.ch[c];
    image_section(&prog, kHighWords[c], &ch->high);
    image_section(&prog, kLowWords[c], &ch->low);
    if (!shelf(rom, kLowShelf400, p[2], &ch->low_shelf) ||
        !shelf(rom, kHighShelf4k, p[3], &ch->high_shelf))
      return false;
  }
  built.drive = (float)(sens * xp(prog.cram[kGainA]) * xp(prog.cram[kGainB]));
  built.trim = (float)xp(prog.cram[kTrim]);
  std::memcpy(built.param, p, sizeof built.param);
  built.ready = true;
  *en = built;
  return true;
}

void enhancer_process(struct xp_enhancer *en, const float *inL,
                       const float *inR, float *outL, float *outR,
                       size_t frames)
{
  const float *in[2] = { inL, inR };
  float *out[2] = { outL, outR };
  for (unsigned c = 0; c < 2u; ++c) {
    struct xp_enhancer_channel *ch = &en->ch[c];
    for (size_t k = 0; k < frames; ++k) {
      float x = in[c][k];
      float e = clamp(fo(&ch->high, x) * en->drive) * en->trim;
      e = fo(&ch->low, e);
      float y = x + en->mix * e;
      y = fo(&ch->low_shelf, y);
      y = fo(&ch->high_shelf, y);
      out[c][k] = en->level * y;
    }
  }
}

}}  // namespace EmuSC::Xp
