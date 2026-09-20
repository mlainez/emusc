/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_eq.h"

#include <string.h>

/* The four 25-record blocks, six bytes each, indexed by gain - 0x34. */
#define SC88_EQ_LOW_200 0x1609cu
#define SC88_EQ_LOW_400 0x16132u
#define SC88_EQ_HIGH_3K 0x161c8u
#define SC88_EQ_HIGH_6K 0x1625eu
#define SC88_EQ_GAIN_MIN 0x34u
#define SC88_EQ_GAIN_MAX 0x4cu

/* The XP coefficient law: fourteen signed bits with thirteen fractional,
   scaled by the two-bit exponent the top bits carry. At the centre gain
   the low 200 Hz record is `5000 2162 1e9e`, which decodes to exactly
   1, -0.956787 and +0.956787 - the identity eq.md describes. */
static const unsigned sc88_eq_shift[4] = {0u, 1u, 2u, 4u};

static float sc88_eq_coefficient(uint16_t raw)
{
  int value = raw & 0x3fff;
  if (value & 0x2000)
    value -= 0x4000;
  return (float)((double)value *
    (double)(1u << sc88_eq_shift[raw >> 14]) / 8192.0);
}

static bool sc88_eq_read_band(const struct sc88_rom *rom, uint32_t block,
                             uint8_t gain, struct sc88_eq_band *band)
{
  uint32_t base;
  unsigned i;
  float c[3];
  if (!rom || !rom->bytes || !band || gain < SC88_EQ_GAIN_MIN ||
      gain > SC88_EQ_GAIN_MAX)
    return false;
  base = block + 6u * (uint32_t)(gain - SC88_EQ_GAIN_MIN);
  if (base + 6u > rom->size)
    return false;
  for (i = 0; i < 3; ++i) {
    uint16_t raw = (uint16_t)(((uint16_t)rom->bytes[base + i * 2] << 8) |
                              rom->bytes[base + i * 2 + 1]);
    c[i] = sc88_eq_coefficient(raw);
  }
  /* A shelf has a nonzero input coefficient and a stable pole. A ROM that
     does not carry these records reads as zeros, which would otherwise be
     accepted and silence the output. */
  if (c[0] == 0.0f || c[2] <= -1.0f || c[2] >= 1.0f)
    return false;
  band->c0 = c[0];
  band->c1 = c[1];
  band->c2 = c[2];
  return true;
}

bool sc88_eq_set_params(const struct sc88_rom *rom, struct sc88_eq *eq,
                        uint8_t low_frequency, uint8_t low_gain,
                        uint8_t high_frequency, uint8_t high_gain)
{
  struct sc88_eq_band low, high;
  if (!eq || low_frequency > 1 || high_frequency > 1)
    return false;
  if (!sc88_eq_read_band(rom, low_frequency ? SC88_EQ_LOW_400
                                            : SC88_EQ_LOW_200,
                         low_gain, &low) ||
      !sc88_eq_read_band(rom, high_frequency ? SC88_EQ_HIGH_6K
                                             : SC88_EQ_HIGH_3K,
                         high_gain, &high))
    return false;
  eq->low.c0 = low.c0;
  eq->low.c1 = low.c1;
  eq->low.c2 = low.c2;
  eq->high.c0 = high.c0;
  eq->high.c1 = high.c1;
  eq->high.c2 = high.c2;
  return true;
}

void sc88_eq_init(struct sc88_eq *eq)
{
  if (!eq)
    return;
  memset(eq, 0, sizeof *eq);
  eq->low.c0 = 1.0f;
  eq->high.c0 = 1.0f;
  eq->enabled = true;
}

void sc88_eq_reset(struct sc88_eq *eq)
{
  if (!eq)
    return;
  memset(eq->low.x1, 0, sizeof eq->low.x1);
  memset(eq->low.y1, 0, sizeof eq->low.y1);
  memset(eq->high.x1, 0, sizeof eq->high.x1);
  memset(eq->high.y1, 0, sizeof eq->high.y1);
}

static float sc88_eq_step(struct sc88_eq_band *band, unsigned ch, float x)
{
  float y = band->c0 * x + band->c1 * band->x1[ch] +
    band->c2 * band->y1[ch];
  band->x1[ch] = x;
  band->y1[ch] = y;
  return y;
}

void sc88_eq_process(struct sc88_eq *eq, float *stereo, size_t frames)
{
  size_t k;
  unsigned ch;
  if (!eq || !eq->enabled || !stereo)
    return;
  for (k = 0; k < frames; ++k)
    for (ch = 0; ch < 2; ++ch) {
      float x = stereo[k * 2 + ch];
      stereo[k * 2 + ch] = sc88_eq_step(&eq->high, ch,
                                        sc88_eq_step(&eq->low, ch, x));
    }
}
