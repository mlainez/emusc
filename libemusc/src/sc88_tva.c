/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_tva.h"

#include <limits.h>

#define SC88_LEVEL_TABLE 0x14f3eu
#define SC88_COARSE_GAIN_TABLE 0x1503eu
#define SC88_FINE_GAIN_TABLE 0x1523eu
#define SC88_ENVELOPE_RATE_TABLE 0x1543eu
#define SC88_RATE_SCALE_TABLE 0x1573eu
#define SC88_RELEASE_PEDAL_TABLE 0x78a02u
#define SC88_AMP_CURVE_1_TABLE 0x1553eu
#define SC88_AMP_CURVE_0_TABLE 0x1563eu

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
  uint16_t component_attenuation;

  if (!levels || !static_attenuation || !gain_q17 ||
      !sc88_tva_component_attenuation(rom, tone, component, zone,
                                      selector_key, velocity,
                                      &component_attenuation))
    return false;
  *static_attenuation = component_attenuation;
  return sc88_tva_gain_from_headroom_q17(
    rom, UINT16_MAX, levels, component_attenuation, gain_q17);
}

bool sc88_tva_gain_from_headroom_q17(const struct sc88_rom *rom,
                                     uint16_t headroom,
                                     const struct sc88_tva_levels *levels,
                                     uint16_t static_attenuation,
                                     uint32_t *gain_q17)
{
  uint8_t sources[4];
  uint16_t remaining = headroom;
  uint16_t reduction;
  uint16_t coarse;
  uint16_t fine;
  uint16_t gain_q15;
  unsigned i;

  if (!rom || !rom->bytes || !levels || !gain_q17)
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
      *gain_q17 = 0;
      return true;
    }
    remaining = (uint16_t)(remaining - reduction);
  }
  remaining = remaining <= static_attenuation
    ? 1 : (uint16_t)(remaining - static_attenuation);
  if (SC88_FINE_GAIN_TABLE + (uint32_t)(remaining & 0xff) * 2 + 2 >
        rom->size)
    return false;
  coarse = sc88_tva_be16(rom->bytes + SC88_COARSE_GAIN_TABLE +
                         (uint32_t)(remaining >> 8) * 2);
  fine = sc88_tva_be16(rom->bytes + SC88_FINE_GAIN_TABLE +
                       (uint32_t)(remaining & 0xff) * 2);
  gain_q15 = (uint16_t)(((uint32_t)coarse * fine) >> 17);
  *gain_q17 = (uint32_t)gain_q15 << 2;
  return true;
}

bool sc88_tva_release_prepare(const struct sc88_rom *rom,
                              const struct sc88_tone *tone,
                              const struct sc88_component *component,
                              uint8_t selector_key,
                              struct sc88_tva_release *release)
{
  uint32_t page;
  uint32_t key_curve;
  int key_value;
  int factor;
  int product_high;
  unsigned scale_index;
  uint16_t scale;
  uint16_t rate;
  uint32_t product;

  if (!rom || !rom->bytes || !tone || !tone->common || !component ||
      !component->bytes || !release || selector_key > 127)
    return false;
  page = (uint32_t)tone->common[0x21] << 16;
  key_curve = page | sc88_tva_be16(component->bytes + 0x8c);
  if (key_curve + selector_key >= rom->size ||
      SC88_RATE_SCALE_TABLE + 129u * 2 > rom->size ||
      SC88_ENVELOPE_RATE_TABLE + 128u * 2 > rom->size)
    return false;
  key_value = sc88_tva_s8(rom->bytes[key_curve + selector_key]);
  factor = sc88_tva_s8((uint8_t)(0u - component->bytes[0x8f]));
  product_high = sc88_tva_floor_div_pow2(key_value * factor, 8);
  scale_index = (unsigned)(product_high + 64);
  if (scale_index > 128)
    return false;
  scale = sc88_tva_be16(rom->bytes + SC88_RATE_SCALE_TABLE + scale_index * 2);
  rate = sc88_tva_be16(rom->bytes + SC88_ENVELOPE_RATE_TABLE +
                       (uint32_t)component->bytes[0x84] * 2);
  if (rate < 16)
    rate = UINT16_MAX;
  product = (uint32_t)rate * scale;
  release->current = UINT16_MAX;
  release->increment = product >= UINT32_C(0x01000000)
    ? UINT16_MAX : (uint16_t)(product >> 8);
  release->scale = UINT16_MAX;
  release->scale_enabled = false;
  release->active = false;
  return true;
}

bool sc88_tva_release_set_pedal(const struct sc88_rom *rom,
                                uint8_t hold1, bool continuous_hold,
                                bool keep_scale_at_zero,
                                bool sostenuto_retained,
                                struct sc88_tva_release *release)
{
  unsigned effective;
  uint32_t offset;
  if (!rom || !rom->bytes || !release || hold1 > 127)
    return false;
  release->scale_enabled = true;
  if (sostenuto_retained) {
    release->scale = 0;
  } else {
    release->scale = UINT16_MAX;
    effective = continuous_hold ? hold1 : (hold1 >= 64 ? 127u : 0u);
    if (effective == 0) {
      if (!keep_scale_at_zero)
        release->scale_enabled = false;
    } else {
      offset = SC88_RELEASE_PEDAL_TABLE + (127u - effective) * 2;
      if (offset + 2 > rom->size)
        return false;
      release->scale = sc88_tva_be16(rom->bytes + offset);
    }
  }
  release->active = true;
  return true;
}

bool sc88_tva_release_advance(struct sc88_tva_release *release,
                              unsigned elapsed_periods)
{
  uint16_t step;
  uint8_t periods;
  uint32_t product;
  if (!release || !release->active || elapsed_periods == 0)
    return false;
  step = release->scale_enabled
    ? (uint16_t)(((uint32_t)release->increment * release->scale) >> 16)
    : release->increment;
  periods = (uint8_t)elapsed_periods;
  product = (uint32_t)step * periods;
  if ((product >> 16) != 0 || release->current <= (uint16_t)product) {
    release->current = 0;
    release->active = false;
  } else {
    release->current = (uint16_t)(release->current - (uint16_t)product);
  }
  return true;
}

/* A stage's stored word is an attenuation, so what the gain tables convert is
 * the headroom **remaining** after it - exactly what the static path does
 * with `sc88_tva_gain_from_headroom_q17`. Converting the stored word itself
 * inverts every envelope in the ROM: it made a piano swell from silence over
 * fourteen seconds and left every sustaining patch at -87 dB. */
static bool sc88_tva_envelope_target_q17(const struct sc88_rom *rom,
                                         uint16_t attenuation,
                                         uint32_t *gain_q17)
{
  uint16_t level = (uint16_t)(UINT16_MAX - attenuation);
  uint16_t coarse;
  uint16_t fine;
  uint16_t gain_q16;
  if (!rom || !rom->bytes || !gain_q17 ||
      SC88_FINE_GAIN_TABLE + (uint32_t)(level & 0xff) * 2 + 2 > rom->size)
    return false;
  coarse = sc88_tva_be16(rom->bytes + SC88_COARSE_GAIN_TABLE +
                         (uint32_t)(level >> 8) * 2);
  fine = sc88_tva_be16(rom->bytes + SC88_FINE_GAIN_TABLE +
                       (uint32_t)(level & 0xff) * 2);
  gain_q16 = (uint16_t)(((uint32_t)coarse * fine) >> 16);
  *gain_q17 = (uint32_t)gain_q16 << 1;
  return true;
}

static bool sc88_tva_key_rate_scale(const struct sc88_rom *rom,
                                    const struct sc88_tone *tone,
                                    const struct sc88_component *component,
                                    uint8_t selector_key, uint16_t pointer_at,
                                    uint8_t factor_at, uint16_t *scale)
{
  uint32_t curve;
  int key_value;
  int factor;
  int index;
  if (!rom || !rom->bytes || !tone || !tone->common || !component ||
      !component->bytes || !scale)
    return false;
  curve = ((uint32_t)tone->common[0x21] << 16) |
    sc88_tva_be16(component->bytes + pointer_at);
  if (curve + selector_key >= rom->size)
    return false;
  key_value = sc88_tva_s8(rom->bytes[curve + selector_key]);
  factor = sc88_tva_s8((uint8_t)(0u - component->bytes[factor_at]));
  index = sc88_tva_floor_div_pow2(key_value * factor, 8) + 64;
  if (index < 0 || index > 128)
    return false;
  *scale = sc88_tva_be16(rom->bytes + SC88_RATE_SCALE_TABLE +
                         (uint32_t)index * 2);
  return true;
}

static bool sc88_tva_velocity_rate_scale(const struct sc88_rom *rom,
                                         uint8_t velocity, int factor,
                                         uint16_t *scale)
{
  int index;
  if (!rom || !rom->bytes || !scale || velocity > 127 ||
      factor < -128 || factor > 127)
    return false;
  index = sc88_tva_floor_div_pow2(
    (2 * ((int)velocity - 64)) * factor, 8) + 64;
  if (index < 0 || index > 128)
    return false;
  *scale = sc88_tva_be16(rom->bytes + SC88_RATE_SCALE_TABLE +
                         (uint32_t)index * 2);
  return true;
}

static uint16_t sc88_tva_curve_pack(uint16_t curve_entry, uint16_t scale)
{
  uint32_t product = (uint32_t)(curve_entry & 0x0fff) * scale;
  uint16_t exponent = (uint16_t)(curve_entry & 0xf000);
  uint8_t exponent_byte;
  uint16_t mantissa;

  exponent = (uint16_t)(exponent << 8) | (uint16_t)(exponent >> 8);
  exponent = (uint16_t)(exponent << 2);
  exponent_byte = (uint8_t)exponent;
  if ((product >> 16) != 0) {
    if (exponent_byte != 0) {
      for (;;) {
        product >>= 2;
        exponent_byte = (uint8_t)(exponent_byte - 0x40);
        exponent = (uint16_t)((exponent & 0xff00) | exponent_byte);
        if (exponent_byte == 0) {
          product >>= 1;
          break;
        }
        if ((product >> 16) == 0)
          break;
      }
    }
    mantissa = exponent_byte == 0 && (product >> 16) >= 16
      ? 0x0fff : (uint16_t)(product >> 8);
  } else {
    while (exponent_byte != 0xc0 && (product >> 16) == 0 &&
           (uint16_t)product < 0x2000) {
      product <<= 2;
      if (exponent_byte == 0)
        product <<= 1;
      exponent_byte = (uint8_t)(exponent_byte + 0x40);
      exponent = (uint16_t)((exponent & 0xff00) | exponent_byte);
    }
    mantissa = (uint16_t)(product >> 8);
  }
  exponent >>= 2;
  exponent = (uint16_t)(exponent << 8) | (uint16_t)(exponent >> 8);
  return (uint16_t)(exponent | mantissa);
}

bool sc88_tva_envelope_prepare(const struct sc88_rom *rom,
                               const struct sc88_tone *tone,
                               const struct sc88_component *component,
                               uint8_t selector_key, uint8_t velocity,
                               struct sc88_tva_envelope *envelope)
{
  uint16_t key_scale;
  unsigned stage;
  if (!rom || !rom->bytes || !tone || !component || !component->bytes ||
      !envelope || selector_key > 127 || velocity > 127 ||
      SC88_RATE_SCALE_TABLE + 129u * 2 > rom->size ||
      SC88_ENVELOPE_RATE_TABLE + 128u * 2 > rom->size ||
      !sc88_tva_key_rate_scale(rom, tone, component, selector_key,
                               0x8a, 0x8e, &key_scale))
    return false;
  for (stage = 0; stage < 4; ++stage) {
    uint16_t velocity_scale;
    uint16_t final_scale;
    uint16_t rate;
    uint16_t curve_entry;
    uint32_t product;
    uint32_t curve_table;
    int factor = sc88_tva_s8(component->bytes[stage < 2 ? 0x90 : 0x91]);
    envelope->target_attenuations[stage] =
      sc88_tva_be16(component->bytes + 0x78 + stage * 2);
    if (!sc88_tva_envelope_target_q17(
          rom, envelope->target_attenuations[stage],
          envelope->targets_q17 + stage) ||
        !sc88_tva_velocity_rate_scale(rom, velocity, factor,
                                      &velocity_scale))
      return false;
    final_scale = (uint16_t)(((uint32_t)key_scale * velocity_scale) >> 8);
    curve_table = component->bytes[0x85 + stage] == 0
      ? SC88_AMP_CURVE_0_TABLE : SC88_AMP_CURVE_1_TABLE;
    if (curve_table + (uint32_t)component->bytes[0x80 + stage] * 2 + 2 >
        rom->size)
      return false;
    curve_entry = sc88_tva_be16(rom->bytes + curve_table +
      (uint32_t)component->bytes[0x80 + stage] * 2);
    if (component->bytes[0x85 + stage] != 0)
      curve_entry |= 0x4000;
    envelope->curve_words[stage] = sc88_tva_curve_pack(
      curve_entry, final_scale);
    rate = sc88_tva_be16(rom->bytes + SC88_ENVELOPE_RATE_TABLE +
                         (uint32_t)component->bytes[0x80 + stage] * 2);
    if (rate < 16)
      rate = UINT16_MAX;
    product = (uint32_t)rate * final_scale;
    if (product >= UINT32_C(0x01000000)) {
      envelope->initial_phases[stage] = UINT16_MAX;
      envelope->increments[stage] = UINT16_MAX;
    } else {
      envelope->initial_phases[stage] = 0;
      envelope->increments[stage] = (uint16_t)(product >> 8);
    }
  }
  envelope->stage = component->bytes[0x80] == 0 ? 1 : 0;
  envelope->saved_count = 0;
  envelope->phase = envelope->initial_phases[envelope->stage];
  /* A stage ramps from wherever the one before it ended. When the first
     stage is skipped because it has no rate, its target is still where the
     envelope begins - for a piano that is full level, so the note starts
     instantly and stage 1 decays to the sustain. Starting from silence
     instead makes an attack that ramps its attenuation up from -87 dB,
     which is inaudible for most of the stage: a 62 ms note came out
     silent altogether. */
  if (envelope->stage == 0) {
    envelope->start_attenuation = UINT16_MAX;
    envelope->start_q17 = 0;
  } else {
    envelope->start_attenuation = envelope->target_attenuations[0];
    envelope->start_q17 = envelope->targets_q17[0];
  }
  envelope->current_q17 = envelope->start_q17;
  envelope->active = true;
  return true;
}

static uint32_t sc88_tva_linear_between(uint32_t start, uint32_t target,
                                        double fraction)
{
  double value;
  if (fraction <= 0.0)
    return start;
  if (fraction >= 1.0)
    return target;
  value = start + fraction * ((double)target - start);
  return (uint32_t)(value < 0.0 ? 0.0 : value + 0.5);
}

/* One point along a stage: the attenuation ramps linearly and the level
   tables turn it into a gain, so the amplitude falls exponentially. */
static uint32_t sc88_tva_attenuation_between(
  const struct sc88_rom *rom, uint16_t start, uint16_t target,
  double fraction, uint32_t fallback)
{
  double value;
  uint32_t gain_q17;
  if (fraction <= 0.0)
    fraction = 0.0;
  else if (fraction >= 1.0)
    fraction = 1.0;
  value = (double)start + fraction * ((double)target - (double)start);
  if (value < 0.0)
    value = 0.0;
  else if (value > (double)UINT16_MAX)
    value = (double)UINT16_MAX;
  if (!sc88_tva_envelope_target_q17(rom, (uint16_t)(value + 0.5), &gain_q17))
    return fallback;
  return gain_q17;
}

uint32_t sc88_tva_envelope_linear_q17(const struct sc88_rom *rom,
  const struct sc88_tva_envelope *envelope, double period_fraction)
{
  double phase;
  if (!envelope)
    return 0;
  if (!envelope->active || envelope->stage >= 4)
    return envelope->current_q17;
  if (period_fraction < 0.0)
    period_fraction = 0.0;
  else if (period_fraction > 1.0)
    period_fraction = 1.0;
  phase = envelope->phase +
    period_fraction * envelope->increments[envelope->stage];
  if (phase > 65535.0)
    phase = 65535.0;
  return sc88_tva_attenuation_between(
    rom, envelope->start_attenuation,
    envelope->target_attenuations[envelope->stage], phase / 65536.0,
    envelope->current_q17);
}

bool sc88_tva_envelope_advance(const struct sc88_rom *rom,
                               struct sc88_tva_envelope *envelope,
                               unsigned elapsed_periods)
{
  uint8_t catchup;
  uint16_t remaining;
  uint16_t working;
  uint16_t increment;
  if (!envelope || !envelope->active || envelope->stage >= 4 ||
      elapsed_periods == 0)
    return false;
  catchup = (uint8_t)(elapsed_periods - 1);
  remaining = (uint16_t)(envelope->saved_count +
    (catchup <= 127 ? (int)catchup : (int)catchup - 256));
  working = envelope->phase;
  increment = envelope->increments[envelope->stage];
  for (;;) {
    uint16_t next = (uint16_t)(working + increment);
    if (next < working) {
      envelope->saved_count = (uint8_t)remaining;
      envelope->current_q17 = envelope->targets_q17[envelope->stage];
      envelope->start_q17 = envelope->current_q17;
      envelope->start_attenuation =
        envelope->target_attenuations[envelope->stage];
      ++envelope->stage;
      envelope->phase = envelope->stage < 4
        ? envelope->initial_phases[envelope->stage] : 0;
      if (envelope->stage == 4)
        envelope->active = false;
      return true;
    }
    working = next;
    --remaining;
    if (remaining == UINT16_MAX)
      break;
  }
  envelope->phase = working;
  envelope->saved_count = 0;
  envelope->current_q17 = sc88_tva_attenuation_between(
    rom, envelope->start_attenuation,
    envelope->target_attenuations[envelope->stage], working / 65536.0,
    envelope->current_q17);
  return true;
}

void sc88_tva_envelope_freeze(const struct sc88_rom *rom,
                              struct sc88_tva_envelope *envelope,
                              double period_fraction)
{
  if (!envelope)
    return;
  envelope->current_q17 = sc88_tva_envelope_linear_q17(
    rom, envelope, period_fraction);
  envelope->active = false;
}
