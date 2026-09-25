/* SPDX-License-Identifier: CC0-1.0 */
#include "delay.h"
#include "reverb.h"

#include "common/constants.h"
#include "devices/sc88.h"

#include <cstdlib>
#include <cstring>

namespace EmuSC { namespace Xp {

namespace {

uint16_t be16(const uint8_t *p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* The XP coefficient law: a sign-extended 14-bit mantissa, thirteen bits
   fractional, scaled by the two-bit exponent bits 15:14 carry
   (kXpCoefficientShift, common/constants.h).

   CREDITED, AND SECONDARY. The formula is
   github.com/giulioz/roland-dsps's, admissible as a lead under the
   owner's ruling of 2026-08-30 (emusc-match TAINT-REGISTER T-008). Its
   origin is that source's prose, not a ROM read here, so it is neither
   firmware-exact nor confirmed. TASK-343 is the measurement that can
   confirm or refute it independently of the source: a chorus-feedback
   decay sweep on the live JV-1080 rig, which is that device's own
   suggested test. The SC-88 has no confirming oracle, so for this device
   the law is PERMANENTLY UNVERIFIED - labelled wherever it is used, and
   never promotable past a lead whatever TASK-343 returns. */
double xp(uint16_t raw)
{
  int value = raw & 0x3fff;
  if (value & 0x2000)
    value -= 0x4000;
  return (double)value * (double)(1u << kXpCoefficientShift[raw >> 14]) / 8192.0;
}

void set_tap(const struct xp_delay *dl, float samples,
             struct xp_delay_tap *t)
{
  t->on = samples > 0.0;
  if (!t->on)
    return;
  double back = samples;
  if (back < 1.0)
    back = 1.0;
  if (back > (double)(dl->len - 2u))
    back = (double)(dl->len - 2u);
  size_t whole = (size_t)back;
  double fraction = back - (double)whole;
  if (fraction > 0.0) {
    t->offset = whole + 1u;
    t->frac = 1.0 - fraction;
  } else {
    t->offset = whole;
    t->frac = 0.0;
  }
}

float tap(const struct xp_delay *dl, const struct xp_delay_tap *t)
{
  size_t i0 = dl->pos >= t->offset ? dl->pos - t->offset
                                   : dl->pos + dl->len - t->offset;
  size_t i1 = i0 + 1u >= dl->len ? 0u : i0 + 1u;
  double frac = t->frac;
  return (float)((1.0 - frac) * dl->buf[i0] + frac * dl->buf[i1]);
}

}  // namespace

bool delay_init(struct xp_delay *dl, double outputRate,
                 const struct XpDeviceProfile *profile)
{
  if (!dl || outputRate < 8000.0 || outputRate > 192000.0)
    return false;
  if (!profile)
    profile = &SC88_PROFILE;
  std::memset(dl, 0, sizeof *dl);
  /* one second of delay plus a little for interpolation */
  dl->len = (size_t)(profile->delayMaxMs * outputRate / 1000.0) + 8u;
  dl->buf = (float *)std::calloc(dl->len, sizeof *dl->buf);
  if (!dl->buf)
    return false;
  dl->output_rate = outputRate;
  dl->pre_in = 1.0f;
  dl->active = true;
  return true;
}

void delay_destroy(struct xp_delay *dl)
{
  if (!dl)
    return;
  std::free(dl->buf);
  std::memset(dl, 0, sizeof *dl);
}

void delay_reset(struct xp_delay *dl)
{
  if (!dl || !dl->buf)
    return;
  std::memset(dl->buf, 0, dl->len * sizeof *dl->buf);
  dl->pos = 0;
  dl->pre_state = 0.0f;
}

bool delay_macro(const struct xp_rom *rom, uint8_t macro, uint8_t out[10])
{
  if (!rom || !rom->bytes || !out || macro > 9)
    return false;
  uint32_t base = xp_profile(rom)->delayMacroTable + (uint32_t)macro * 16u;
  if (base + 10u > rom->size)
    return false;
  for (unsigned i = 0; i < 10; ++i)
    out[i] = rom->bytes[base + i];
  return true;
}

bool delay_set_params(const struct xp_rom *rom, struct xp_delay *dl,
                       const uint8_t p[10])
{
  if (!rom || !rom->bytes || !dl || !p)
    return false;
  const struct XpDeviceProfile *profile = xp_profile(rom);
  uint8_t v[10];
  for (unsigned i = 0; i < 10; ++i)
    v[i] = p[i] > 127 ? 127 : p[i];

  float fb, in;
  if (reverb_pre_lpf(v[0] > 7 ? 7 : v[0], &fb, &in)) {
    dl->pre_fb = fb;
    dl->pre_in = in;
  }
  /* The centre time is a table read, and index zero is outside the public
     range and must not be treated as a time. */
  if (v[1] < 1 || v[1] > 0x73)
    return false;
  if (profile->delayCentreTable + (uint32_t)v[1] * 2u + 2u > rom->size)
    return false;
  uint16_t centreWord = be16(rom->bytes + profile->delayCentreTable +
                             (uint32_t)v[1] * 2u);
  if (centreWord < 0x8000u)
    return false;
  unsigned centreUnits = centreWord - 0x8000u;
  double scale = dl->output_rate / (profile->delayUnitsPerMs * 1000.0);
  dl->centre_samples = (float)((double)centreUnits * scale);
  set_tap(dl, dl->centre_samples, &dl->centre_tap);

  for (unsigned i = 0; i < 2; ++i) {
    uint8_t r = v[2 + i];
    unsigned units = 0;
    if (r >= 1 && r <= 0x78 &&
        profile->delayRatioTable + (uint32_t)r * 2u + 2u <= rom->size) {
      uint32_t ratio = be16(rom->bytes + profile->delayRatioTable +
                             (uint32_t)r * 2u);
      units = (unsigned)((ratio * (uint32_t)centreUnits) / 256u);
      if (units > profile->delayMaxUnits)
        units = profile->delayMaxUnits;
    }
    if (i == 0) {
      dl->left_samples = (float)((double)units * scale);
      set_tap(dl, dl->left_samples, &dl->left_tap);
    } else {
      dl->right_samples = (float)((double)units * scale);
      set_tap(dl, dl->right_samples, &dl->right_tap);
    }
  }

  /* `64*p` read as an XP coefficient is p/128 */
  dl->centre_level = (float)xp((uint16_t)(64u * v[4]));
  dl->left_level = (float)xp((uint16_t)(64u * v[5]));
  dl->right_level = (float)xp((uint16_t)(64u * v[6]));
  /* the reverb's own level law */
  dl->overall = (float)(4u * (unsigned)v[7]) / 512.0f;
  /* bipolar about 64, and never unity */
  dl->feedback = (float)xp(
    (uint16_t)((124u * (unsigned)v[8] + 0x2100u) & 0x3fffu));
  dl->reverb_send = v[9];
  return true;
}

void delay_process(struct xp_delay *dl, const float *send, float *stereo,
                    float *toReverb, size_t frames)
{
  if (!dl || !dl->active || !dl->buf || !send || !stereo)
    return;
  /* the delay's own send into the reverb, `2*p` against 256 */
  float sendGain = (float)(2u * (unsigned)dl->reverb_send) / 256.0f;
  for (size_t k = 0; k < frames; ++k) {
    float x = send[k];
    dl->pre_state = dl->pre_in * x + dl->pre_fb * dl->pre_state;
    x = dl->pre_state;
    float c = dl->centre_tap.on ? tap(dl, &dl->centre_tap) : 0.0f;
    float l = dl->left_tap.on ? tap(dl, &dl->left_tap) : 0.0f;
    float r = dl->right_tap.on ? tap(dl, &dl->right_tap) : 0.0f;
    /* Which tap closes the loop is not recovered; the centre time is the
       delay's own period, so it is the one used. */
    dl->buf[dl->pos] = x + dl->feedback * c;
    if (++dl->pos >= dl->len)
      dl->pos = 0;
    float outL = dl->overall * (dl->centre_level * c + dl->left_level * l);
    float outR = dl->overall * (dl->centre_level * c + dl->right_level * r);
    stereo[k * 2] += outL;
    stereo[k * 2 + 1] += outR;
    if (toReverb)
      toReverb[k] += sendGain * 0.5f * (outL + outR);
  }
}

}}  // namespace EmuSC::Xp
