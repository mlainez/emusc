/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_tva.h"

#include <limits.h>

#define SC88_LEVEL_TABLE 0x14f3eu
#define SC88_COARSE_GAIN_TABLE 0x1503eu
#define SC88_FINE_GAIN_TABLE 0x1523eu

static uint16_t sc88_tva_be16(const uint8_t *p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static int16_t sc88_tva_s16(uint16_t value)
{
  return value <= INT16_MAX
    ? (int16_t)value
    : (int16_t)(-1 - (int32_t)(UINT16_MAX - value));
}

static int8_t sc88_tva_s8(uint8_t value)
{
  return value <= INT8_MAX
    ? (int8_t)value
    : (int8_t)(-1 - (int16_t)(UINT8_MAX - value));
}

static int32_t sc88_tva_floor_div_pow2(int32_t value, unsigned shift)
{
  if (value >= 0)
    return value / (INT32_C(1) << shift);
  return -(int32_t)(((uint32_t)(-value) +
    ((UINT32_C(1) << shift) - 1)) >> shift);
}

static bool sc88_tva_level_word(const struct sc88_rom *rom, uint8_t index,
                                uint16_t *word)
{
  uint32_t offset = SC88_LEVEL_TABLE + (uint32_t)index * 2;
  if (!rom || !rom->bytes || !word || offset + 2 > rom->size)
    return false;
  *word = sc88_tva_be16(rom->bytes + offset);
  return true;
}

static bool sc88_tva_component_attenuation(
  const struct sc88_rom *rom, const struct sc88_tone *tone,
  const struct sc88_component *component,
  const struct sc88_zone_selection *zone, uint8_t selector_key,
  uint8_t velocity, uint16_t *attenuation)
{
  const uint8_t *bytes;
  uint32_t page;
  uint32_t key_curve;
  uint32_t velocity_curve;
  uint8_t velocity_index;
  uint8_t velocity_result;
  int32_t first;
  int32_t second;
  int16_t key_adjustment;
  uint16_t adjusted_component;
  uint16_t velocity_word;
  uint32_t sum;

  if (!rom || !rom->bytes || !tone || !tone->common || !component ||
      !component->bytes || !zone || !attenuation || selector_key > 127 ||
      velocity > 127)
    return false;
  bytes = component->bytes;
  page = (uint32_t)tone->common[0x21] << 16;
  key_curve = page | sc88_tva_be16(bytes + 0x68);
  velocity_curve = page | sc88_tva_be16(bytes + 0x64);
  if (key_curve + selector_key >= rom->size)
    return false;

  first = sc88_tva_floor_div_pow2(
    sc88_tva_s8(rom->bytes[key_curve + selector_key]) *
      sc88_tva_s16(sc88_tva_be16(bytes + 0x6a)), 8);
  second = sc88_tva_floor_div_pow2(first * INT32_C(0x2437), 16);
  key_adjustment = sc88_tva_s16((uint16_t)((uint16_t)second << 2));

  velocity_index = (uint8_t)(((uint32_t)(uint8_t)(
    velocity - bytes[0x6c]) * sc88_tva_be16(bytes + 0x70)) >> 8);
  /* Do not clamp to the nominal 128-byte curve. The firmware retains a byte
   * after low-end underflow, and held components exercise the adjacent ROM. */
  if (velocity_curve + velocity_index >= rom->size)
    return false;
  velocity_result = (uint8_t)(
    ((uint32_t)rom->bytes[velocity_curve + velocity_index] *
      sc88_tva_be16(bytes + 0x72) >> 8) + bytes[0x6e]);
  if (!sc88_tva_level_word(rom, velocity_result & 0x7f, &velocity_word))
    return false;

  sum = (uint32_t)sc88_tva_be16(tone->common + 0x0c) +
    zone->static_attenuation;
  if (sum > UINT16_MAX) {
    *attenuation = UINT16_MAX;
    return true;
  }
  adjusted_component = sc88_tva_be16(bytes + 0x66);
  if (key_adjustment < 0) {
    uint32_t expanded = (uint32_t)adjusted_component +
      (uint16_t)(0u - (uint16_t)key_adjustment);
    adjusted_component = expanded > UINT16_MAX
      ? UINT16_MAX : (uint16_t)expanded;
  } else {
    adjusted_component = adjusted_component < (uint16_t)key_adjustment
      ? 0 : (uint16_t)(adjusted_component - (uint16_t)key_adjustment);
  }
  sum += adjusted_component;
  if (sum > UINT16_MAX) {
    *attenuation = UINT16_MAX;
    return true;
  }
  sum += velocity_word;
  *attenuation = sum > UINT16_MAX ? UINT16_MAX : (uint16_t)sum;
  return true;
}

bool sc88_tva_static_gain_q17(const struct sc88_rom *rom,
                              const struct sc88_tone *tone,
                              const struct sc88_component *component,
                              const struct sc88_zone_selection *zone,
                              uint8_t selector_key, uint8_t velocity,
                              const struct sc88_tva_levels *levels,
                              uint16_t *static_attenuation,
                              uint32_t *gain_q17)
{
  uint8_t sources[4];
  uint16_t remaining = UINT16_MAX;
  uint16_t component_attenuation;
  uint16_t reduction;
  uint16_t coarse;
  uint16_t fine;
  uint16_t gain_q15;
  unsigned i;

  if (!levels || !static_attenuation || !gain_q17 ||
      !sc88_tva_component_attenuation(rom, tone, component, zone,
                                      selector_key, velocity,
                                      &component_attenuation))
    return false;
  sources[0] = levels->master;
  sources[1] = levels->secondary;
  sources[2] = levels->part;
  sources[3] = levels->expression;
  for (i = 0; i < 4; ++i) {
    if (sources[i] > 127 ||
        !sc88_tva_level_word(rom, sources[i], &reduction))
      return false;
    if (remaining <= reduction) {
      *static_attenuation = component_attenuation;
      *gain_q17 = 0;
      return true;
    }
    remaining = (uint16_t)(remaining - reduction);
  }
  remaining = remaining <= component_attenuation
    ? 1 : (uint16_t)(remaining - component_attenuation);
  if (SC88_FINE_GAIN_TABLE + (uint32_t)(remaining & 0xff) * 2 + 2 >
        rom->size)
    return false;
  coarse = sc88_tva_be16(rom->bytes + SC88_COARSE_GAIN_TABLE +
                         (uint32_t)(remaining >> 8) * 2);
  fine = sc88_tva_be16(rom->bytes + SC88_FINE_GAIN_TABLE +
                       (uint32_t)(remaining & 0xff) * 2);
  gain_q15 = (uint16_t)(((uint32_t)coarse * fine) >> 17);
  *static_attenuation = component_attenuation;
  *gain_q17 = (uint32_t)gain_q15 << 2;
  return true;
}
