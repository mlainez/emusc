/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_delay.h"
#include "reverb.h"

#include <stdlib.h>
#include <string.h>

#define SC88_DELAY_CENTRE_TABLE 0x15fb4u
#define SC88_DELAY_RATIO_TABLE 0x165cau
#define SC88_DELAY_MACRO_TABLE 0x158beu
/* The delay memory counts in 1/32 ms in this path, and both the centre
   time and the side taps cap at 0x7d00 above the 0x8000 base: one second. */
#define SC88_DELAY_UNITS_PER_MS 32.0
#define SC88_DELAY_MAX_UNITS 0x7d00u
#define SC88_DELAY_MAX_MS 1000.0

static const unsigned sc88_delay_shift[4] = {0u, 1u, 2u, 4u};

static uint16_t sc88_delay_be16(const uint8_t *p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static double sc88_delay_xp(uint16_t raw)
{
  int value = raw & 0x3fff;
  if (value & 0x2000)
    value -= 0x4000;
  return (double)value * (double)(1u << sc88_delay_shift[raw >> 14]) / 8192.0;
}

bool sc88_delay_init(struct sc88_delay *dl, double output_rate)
{
  if (!dl || output_rate < 8000.0 || output_rate > 192000.0)
    return false;
  memset(dl, 0, sizeof *dl);
  /* one second of delay plus a little for interpolation */
  dl->len = (size_t)(SC88_DELAY_MAX_MS * output_rate / 1000.0) + 8u;
  dl->buf = (float *)calloc(dl->len, sizeof *dl->buf);
  if (!dl->buf)
    return false;
  dl->output_rate = output_rate;
  dl->pre_in = 1.0f;
  dl->active = true;
  return true;
}

void sc88_delay_destroy(struct sc88_delay *dl)
{
  if (!dl)
    return;
  free(dl->buf);
  memset(dl, 0, sizeof *dl);
}

void sc88_delay_reset(struct sc88_delay *dl)
{
  if (!dl || !dl->buf)
    return;
  memset(dl->buf, 0, dl->len * sizeof *dl->buf);
  dl->pos = 0;
  dl->pre_state = 0.0f;
}

bool sc88_delay_macro(const struct sc88_rom *rom, uint8_t macro,
                      uint8_t out[10])
{
  uint32_t base;
  unsigned i;
  if (!rom || !rom->bytes || !out || macro > 9)
    return false;
  base = SC88_DELAY_MACRO_TABLE + (uint32_t)macro * 16u;
  if (base + 10u > rom->size)
    return false;
  for (i = 0; i < 10; ++i)
    out[i] = rom->bytes[base + i];
  return true;
}

bool sc88_delay_set_params(const struct sc88_rom *rom,
                           struct sc88_delay *dl, const uint8_t p[10])
{
  float fb, in;
  uint16_t centre_word;
  unsigned centre_units;
  double scale;
  unsigned i;
  uint8_t v[10];
  if (!rom || !rom->bytes || !dl || !p)
    return false;
  for (i = 0; i < 10; ++i)
    v[i] = p[i] > 127 ? 127 : p[i];

  if (sc88_reverb_pre_lpf(v[0] > 7 ? 7 : v[0], &fb, &in)) {
    dl->pre_fb = fb;
    dl->pre_in = in;
  }
  /* The centre time is a table read, and index zero is outside the public
     range and must not be treated as a time. */
  if (v[1] < 1 || v[1] > 0x73)
    return false;
  if (SC88_DELAY_CENTRE_TABLE + (uint32_t)v[1] * 2u + 2u > rom->size)
    return false;
  centre_word = sc88_delay_be16(rom->bytes + SC88_DELAY_CENTRE_TABLE +
                                (uint32_t)v[1] * 2u);
  if (centre_word < 0x8000u)
    return false;
  centre_units = centre_word - 0x8000u;
  scale = dl->output_rate / (SC88_DELAY_UNITS_PER_MS * 1000.0);
  dl->centre_samples = (double)centre_units * scale;

  for (i = 0; i < 2; ++i) {
    uint8_t r = v[2 + i];
    unsigned units = 0;
    if (r >= 1 && r <= 0x78 &&
        SC88_DELAY_RATIO_TABLE + (uint32_t)r * 2u + 2u <= rom->size) {
      uint32_t ratio = sc88_delay_be16(rom->bytes + SC88_DELAY_RATIO_TABLE +
                                       (uint32_t)r * 2u);
      units = (unsigned)((ratio * (uint32_t)centre_units) / 256u);
      if (units > SC88_DELAY_MAX_UNITS)
        units = SC88_DELAY_MAX_UNITS;
    }
    if (i == 0)
      dl->left_samples = (double)units * scale;
    else
      dl->right_samples = (double)units * scale;
  }

  /* `64*p` read as an XP coefficient is p/128 */
  dl->centre_level = (float)sc88_delay_xp((uint16_t)(64u * v[4]));
  dl->left_level = (float)sc88_delay_xp((uint16_t)(64u * v[5]));
  dl->right_level = (float)sc88_delay_xp((uint16_t)(64u * v[6]));
  /* the reverb's own level law */
  dl->overall = (float)(4u * (unsigned)v[7]) / 512.0f;
  /* bipolar about 64, and never unity */
  dl->feedback = (float)sc88_delay_xp(
    (uint16_t)((124u * (unsigned)v[8] + 0x2100u) & 0x3fffu));
  dl->reverb_send = v[9];
  return true;
}

static float sc88_delay_tap(const struct sc88_delay *dl, double back)
{
  double read;
  size_t i0, i1;
  double frac;
  if (back < 1.0)
    back = 1.0;
  if (back > (double)(dl->len - 2u))
    back = (double)(dl->len - 2u);
  read = (double)dl->pos - back;
  while (read < 0.0)
    read += (double)dl->len;
  i0 = (size_t)read;
  frac = read - (double)i0;
  i1 = i0 + 1u >= dl->len ? 0u : i0 + 1u;
  return (float)((1.0 - frac) * dl->buf[i0] + frac * dl->buf[i1]);
}

void sc88_delay_process(struct sc88_delay *dl, const float *send,
                        float *stereo, float *to_reverb, size_t frames)
{
  size_t k;
  float send_gain;
  if (!dl || !dl->active || !dl->buf || !send || !stereo)
    return;
  /* the delay's own send into the reverb, `2*p` against 256 */
  send_gain = (float)(2u * (unsigned)dl->reverb_send) / 256.0f;
  for (k = 0; k < frames; ++k) {
    float x = send[k];
    float c, l, r, out_l, out_r;
    dl->pre_state = dl->pre_in * x + dl->pre_fb * dl->pre_state;
    x = dl->pre_state;
    c = dl->centre_samples > 0.0 ? sc88_delay_tap(dl, dl->centre_samples)
                                 : 0.0f;
    l = dl->left_samples > 0.0 ? sc88_delay_tap(dl, dl->left_samples) : 0.0f;
    r = dl->right_samples > 0.0 ? sc88_delay_tap(dl, dl->right_samples)
                                : 0.0f;
    /* Which tap closes the loop is not recovered; the centre time is the
       delay's own period, so it is the one used. */
    dl->buf[dl->pos] = x + dl->feedback * c;
    if (++dl->pos >= dl->len)
      dl->pos = 0;
    out_l = dl->overall * (dl->centre_level * c + dl->left_level * l);
    out_r = dl->overall * (dl->centre_level * c + dl->right_level * r);
    stereo[k * 2] += out_l;
    stereo[k * 2 + 1] += out_r;
    if (to_reverb)
      to_reverb[k] += send_gain * 0.5f * (out_l + out_r);
  }
}
