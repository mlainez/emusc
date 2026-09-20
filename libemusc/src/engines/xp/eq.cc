/* SPDX-License-Identifier: CC0-1.0 */
#include "eq.h"

#include <cstring>

namespace EmuSC { namespace Xp {

namespace {

/* The four 25-record blocks, six bytes each, indexed by gain - 0x34. */
constexpr uint32_t kEqLow200 = 0x1609cu;
constexpr uint32_t kEqLow400 = 0x16132u;
constexpr uint32_t kEqHigh3k = 0x161c8u;
constexpr uint32_t kEqHigh6k = 0x1625eu;
constexpr uint8_t kEqGainMin = 0x34u;
constexpr uint8_t kEqGainMax = 0x4cu;

/* The XP coefficient law: fourteen signed bits with thirteen fractional,
   scaled by the two-bit exponent the top bits carry. At the centre gain
   the low 200 Hz record is `5000 2162 1e9e`, which decodes to exactly
   1, -0.956787 and +0.956787 - the identity eq.md describes. */
constexpr unsigned kShift[4] = {0u, 1u, 2u, 4u};

float coefficient(uint16_t raw)
{
  int value = raw & 0x3fff;
  if (value & 0x2000)
    value -= 0x4000;
  return (float)((double)value * (double)(1u << kShift[raw >> 14]) / 8192.0);
}

bool readBand(const struct sc88_rom *rom, uint32_t block, uint8_t gain,
              struct sc88_eq_band *band)
{
  if (!rom || !rom->bytes || !band || gain < kEqGainMin || gain > kEqGainMax)
    return false;
  uint32_t base = block + 6u * (uint32_t)(gain - kEqGainMin);
  if (base + 6u > rom->size)
    return false;
  float c[3];
  for (unsigned i = 0; i < 3; ++i) {
    uint16_t raw = (uint16_t)(((uint16_t)rom->bytes[base + i * 2] << 8) |
                              rom->bytes[base + i * 2 + 1]);
    c[i] = coefficient(raw);
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

float step(struct sc88_eq_band *band, unsigned ch, float x)
{
  float y = band->c0 * x + band->c1 * band->x1[ch] + band->c2 * band->y1[ch];
  band->x1[ch] = x;
  band->y1[ch] = y;
  return y;
}

}  // namespace

bool eq_set_params(const struct sc88_rom *rom, struct sc88_eq *eq,
                    uint8_t lowFrequency, uint8_t lowGain,
                    uint8_t highFrequency, uint8_t highGain)
{
  if (!eq || lowFrequency > 1 || highFrequency > 1)
    return false;
  struct sc88_eq_band low, high;
  if (!readBand(rom, lowFrequency ? kEqLow400 : kEqLow200, lowGain, &low) ||
      !readBand(rom, highFrequency ? kEqHigh6k : kEqHigh3k, highGain, &high))
    return false;
  eq->low.c0 = low.c0;
  eq->low.c1 = low.c1;
  eq->low.c2 = low.c2;
  eq->high.c0 = high.c0;
  eq->high.c1 = high.c1;
  eq->high.c2 = high.c2;
  return true;
}

void eq_init(struct sc88_eq *eq)
{
  if (!eq)
    return;
  std::memset(eq, 0, sizeof *eq);
  eq->low.c0 = 1.0f;
  eq->high.c0 = 1.0f;
  eq->enabled = true;
}

void eq_reset(struct sc88_eq *eq)
{
  if (!eq)
    return;
  std::memset(eq->low.x1, 0, sizeof eq->low.x1);
  std::memset(eq->low.y1, 0, sizeof eq->low.y1);
  std::memset(eq->high.x1, 0, sizeof eq->high.x1);
  std::memset(eq->high.y1, 0, sizeof eq->high.y1);
}

void eq_process(struct sc88_eq *eq, float *stereo, size_t frames)
{
  if (!eq || !eq->enabled || !stereo)
    return;
  for (size_t k = 0; k < frames; ++k)
    for (unsigned ch = 0; ch < 2; ++ch) {
      float x = stereo[k * 2 + ch];
      stereo[k * 2 + ch] = step(&eq->high, ch, step(&eq->low, ch, x));
    }
}

}}  // namespace EmuSC::Xp

// Compatibility shims for callers not yet ported to the EmuSC::Xp API.
extern "C" {

void sc88_eq_init(struct sc88_eq *eq)
{
  EmuSC::Xp::eq_init(eq);
}

bool sc88_eq_set_params(const struct sc88_rom *rom, struct sc88_eq *eq,
                        uint8_t low_frequency, uint8_t low_gain,
                        uint8_t high_frequency, uint8_t high_gain)
{
  return EmuSC::Xp::eq_set_params(rom, eq, low_frequency, low_gain,
                                   high_frequency, high_gain);
}

void sc88_eq_reset(struct sc88_eq *eq)
{
  EmuSC::Xp::eq_reset(eq);
}

void sc88_eq_process(struct sc88_eq *eq, float *stereo, size_t frames)
{
  EmuSC::Xp::eq_process(eq, stereo, frames);
}

}  // extern "C"
