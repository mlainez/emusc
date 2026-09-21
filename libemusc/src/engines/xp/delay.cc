/* SPDX-License-Identifier: CC0-1.0 */
#include "delay.h"
#include "reverb.h"

#include <cstdlib>
#include <cstring>

namespace EmuSC { namespace Xp {

namespace {

constexpr uint32_t kDelayCentreTable = 0x15fb4u;
constexpr uint32_t kDelayRatioTable = 0x165cau;
constexpr uint32_t kDelayMacroTable = 0x158beu;
/* The delay memory counts in 1/32 ms in this path, and both the centre
   time and the side taps cap at 0x7d00 above the 0x8000 base: one second. */
constexpr double kDelayUnitsPerMs = 32.0;
constexpr unsigned kDelayMaxUnits = 0x7d00u;
constexpr double kDelayMaxMs = 1000.0;

constexpr unsigned kShift[4] = {0u, 1u, 2u, 4u};

uint16_t be16(const uint8_t *p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

double xp(uint16_t raw)
{
  int value = raw & 0x3fff;
  if (value & 0x2000)
    value -= 0x4000;
  return (double)value * (double)(1u << kShift[raw >> 14]) / 8192.0;
}

float tap(const struct sc88_delay *dl, double back)
{
  if (back < 1.0)
    back = 1.0;
  if (back > (double)(dl->len - 2u))
    back = (double)(dl->len - 2u);
  double read = (double)dl->pos - back;
  while (read < 0.0)
    read += (double)dl->len;
  size_t i0 = (size_t)read;
  double frac = read - (double)i0;
  size_t i1 = i0 + 1u >= dl->len ? 0u : i0 + 1u;
  return (float)((1.0 - frac) * dl->buf[i0] + frac * dl->buf[i1]);
}

}  // namespace

bool delay_init(struct sc88_delay *dl, double outputRate)
{
  if (!dl || outputRate < 8000.0 || outputRate > 192000.0)
    return false;
  std::memset(dl, 0, sizeof *dl);
  /* one second of delay plus a little for interpolation */
  dl->len = (size_t)(kDelayMaxMs * outputRate / 1000.0) + 8u;
  dl->buf = (float *)std::calloc(dl->len, sizeof *dl->buf);
  if (!dl->buf)
    return false;
  dl->output_rate = outputRate;
  dl->pre_in = 1.0f;
  dl->active = true;
  return true;
}

void delay_destroy(struct sc88_delay *dl)
{
  if (!dl)
    return;
  std::free(dl->buf);
  std::memset(dl, 0, sizeof *dl);
}

void delay_reset(struct sc88_delay *dl)
{
  if (!dl || !dl->buf)
    return;
  std::memset(dl->buf, 0, dl->len * sizeof *dl->buf);
  dl->pos = 0;
  dl->pre_state = 0.0f;
}

bool delay_macro(const struct sc88_rom *rom, uint8_t macro, uint8_t out[10])
{
  if (!rom || !rom->bytes || !out || macro > 9)
    return false;
  uint32_t base = kDelayMacroTable + (uint32_t)macro * 16u;
  if (base + 10u > rom->size)
    return false;
  for (unsigned i = 0; i < 10; ++i)
    out[i] = rom->bytes[base + i];
  return true;
}

bool delay_set_params(const struct sc88_rom *rom, struct sc88_delay *dl,
                       const uint8_t p[10])
{
  if (!rom || !rom->bytes || !dl || !p)
    return false;
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
  if (kDelayCentreTable + (uint32_t)v[1] * 2u + 2u > rom->size)
    return false;
  uint16_t centreWord = be16(rom->bytes + kDelayCentreTable +
                             (uint32_t)v[1] * 2u);
  if (centreWord < 0x8000u)
    return false;
  unsigned centreUnits = centreWord - 0x8000u;
  double scale = dl->output_rate / (kDelayUnitsPerMs * 1000.0);
  dl->centre_samples = (float)((double)centreUnits * scale);

  for (unsigned i = 0; i < 2; ++i) {
    uint8_t r = v[2 + i];
    unsigned units = 0;
    if (r >= 1 && r <= 0x78 &&
        kDelayRatioTable + (uint32_t)r * 2u + 2u <= rom->size) {
      uint32_t ratio = be16(rom->bytes + kDelayRatioTable + (uint32_t)r * 2u);
      units = (unsigned)((ratio * (uint32_t)centreUnits) / 256u);
      if (units > kDelayMaxUnits)
        units = kDelayMaxUnits;
    }
    if (i == 0)
      dl->left_samples = (float)((double)units * scale);
    else
      dl->right_samples = (float)((double)units * scale);
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

void delay_process(struct sc88_delay *dl, const float *send, float *stereo,
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
    float c = dl->centre_samples > 0.0 ? tap(dl, dl->centre_samples) : 0.0f;
    float l = dl->left_samples > 0.0 ? tap(dl, dl->left_samples) : 0.0f;
    float r = dl->right_samples > 0.0 ? tap(dl, dl->right_samples) : 0.0f;
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

// Compatibility shims for callers not yet ported to the EmuSC::Xp API.
extern "C" {

bool sc88_delay_init(struct sc88_delay *dl, double output_rate)
{
  return EmuSC::Xp::delay_init(dl, output_rate);
}

void sc88_delay_destroy(struct sc88_delay *dl)
{
  EmuSC::Xp::delay_destroy(dl);
}

void sc88_delay_reset(struct sc88_delay *dl)
{
  EmuSC::Xp::delay_reset(dl);
}

bool sc88_delay_macro(const struct sc88_rom *rom, uint8_t macro,
                      uint8_t out[10])
{
  return EmuSC::Xp::delay_macro(rom, macro, out);
}

bool sc88_delay_set_params(const struct sc88_rom *rom,
                           struct sc88_delay *dl, const uint8_t p[10])
{
  return EmuSC::Xp::delay_set_params(rom, dl, p);
}

void sc88_delay_process(struct sc88_delay *dl, const float *send,
                        float *stereo, float *to_reverb, size_t frames)
{
  EmuSC::Xp::delay_process(dl, send, stereo, to_reverb, frames);
}

}  // extern "C"
