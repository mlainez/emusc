/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_tvf.h"

#include <limits.h>
#include <string.h>

#define SC88_TVF_BASE_TABLE 0x78702u
#define SC88_TVF_LIMIT_TABLE 0x78802u
#define SC88_ENVELOPE_RATE_TABLE 0x1543eu
#define SC88_RATE_SCALE_TABLE 0x1573eu

static uint16_t sc88_tvf_be16(const uint8_t *p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static int8_t sc88_tvf_s8(uint8_t value)
{
  return value <= INT8_MAX
    ? (int8_t)value
    : (int8_t)(-1 - (int16_t)(UINT8_MAX - value));
}

static int16_t sc88_tvf_s16(uint16_t value)
{
  return value <= INT16_MAX
    ? (int16_t)value
    : (int16_t)(-1 - (int32_t)(UINT16_MAX - value));
}

static int32_t sc88_tvf_floor_div_pow2(int32_t value, unsigned shift)
{
  if (value >= 0)
    return value / (INT32_C(1) << shift);
  return -(int32_t)(((uint32_t)(-value) +
    ((UINT32_C(1) << shift) - 1)) >> shift);
}

static int sc88_tvf_clamp_index(int value)
{
  if (value < 0)
    return 0;
  if (value > 127)
    return 127;
  return value;
}

bool sc88_tvf_prepare_registers(const struct sc88_rom *rom,
                                const struct sc88_component *component,
                                int16_t accumulated_modulation,
                                const struct sc88_tvf_controls *controls,
                                struct sc88_tvf_registers *registers)
{
  int mode;
  int cutoff_index;
  int resonance_index;
  int resonance_floor;
  int32_t combined;
  uint16_t limit;

  if (!rom || !rom->bytes || rom->size < SC88_TVF_LIMIT_TABLE + 256u ||
      !component || !component->bytes || !controls || !registers ||
      controls->part_cutoff > 127 || controls->secondary_cutoff > 127 ||
      controls->part_resonance > 127 ||
      controls->secondary_resonance > 127)
    return false;

  memset(registers, 0, sizeof *registers);
  registers->frequency_interpolation = 0x4100;
  registers->resonance_interpolation = 0x095f;
  mode = sc88_tvf_s8(component->bytes[0x3e]);
  if (mode < 0) {
    registers->resonance_current = 0x20000;
    registers->resonance_target = 0x80000;
    registers->filter_select = 0x0800;
    registers->fixed_tuple = true;
    return true;
  }

  cutoff_index = sc88_tvf_clamp_index(
    component->bytes[0x3c] + controls->part_cutoff +
    controls->secondary_cutoff - 128);
  resonance_index = sc88_tvf_clamp_index(
    component->bytes[0x3d] - 2 * (controls->part_resonance +
                                  controls->secondary_resonance - 128));
  resonance_floor = component->bytes[0x3d] < 4
    ? component->bytes[0x3d] : 4;
  if (resonance_index < resonance_floor)
    resonance_index = resonance_floor;

  combined = sc88_tvf_be16(rom->bytes + SC88_TVF_BASE_TABLE +
                           (unsigned)cutoff_index * 2u);
  combined += accumulated_modulation;
  if (combined < 0)
    combined = 0;
  else if (combined > UINT16_MAX)
    combined = UINT16_MAX;
  combined >>= 1;
  limit = (uint16_t)(sc88_tvf_be16(
    rom->bytes + SC88_TVF_LIMIT_TABLE + (unsigned)resonance_index * 2u) >> 1);
  if (combined > limit)
    combined = limit;

  registers->cutoff_index = (uint8_t)cutoff_index;
  registers->resonance_index = (uint8_t)resonance_index;
  registers->combined = (uint16_t)combined;
  registers->frequency_current = (uint32_t)combined << 3;
  registers->frequency_target = registers->frequency_current;
  registers->resonance_current = (uint32_t)resonance_index << 11;
  registers->resonance_target = (uint32_t)resonance_index << 13;
  registers->filter_select = (uint16_t)((unsigned)mode << 8);
  return true;
}

static bool sc88_tvf_key_rate_scale(const struct sc88_rom *rom,
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
      !component->bytes || !scale || selector_key > 127)
    return false;
  curve = ((uint32_t)tone->common[0x21] << 16) |
    sc88_tvf_be16(component->bytes + pointer_at);
  if (curve + selector_key >= rom->size ||
      SC88_RATE_SCALE_TABLE + 129u * 2 > rom->size)
    return false;
  key_value = sc88_tvf_s8(rom->bytes[curve + selector_key]);
  factor = sc88_tvf_s8((uint8_t)(0u - component->bytes[factor_at]));
  index = sc88_tvf_floor_div_pow2(key_value * factor, 8) + 64;
  if (index < 0 || index > 128)
    return false;
  *scale = sc88_tvf_be16(rom->bytes + SC88_RATE_SCALE_TABLE +
                         (uint32_t)index * 2);
  return true;
}

static bool sc88_tvf_velocity_rate_scale(const struct sc88_rom *rom,
                                         uint8_t velocity, int factor,
                                         uint16_t *scale)
{
  int index;
  if (!rom || !rom->bytes || !scale || velocity > 127 ||
      factor < -128 || factor > 127 ||
      SC88_RATE_SCALE_TABLE + 129u * 2 > rom->size)
    return false;
  index = sc88_tvf_floor_div_pow2(
    (2 * ((int)velocity - 64)) * factor, 8) + 64;
  if (index < 0 || index > 128)
    return false;
  *scale = sc88_tvf_be16(rom->bytes + SC88_RATE_SCALE_TABLE +
                         (uint32_t)index * 2);
  return true;
}

static bool sc88_tvf_envelope_depth(const struct sc88_rom *rom,
                                    const struct sc88_tone *tone,
                                    const struct sc88_component *component,
                                    uint8_t velocity, bool soft_pedal,
                                    uint16_t *depth)
{
  uint16_t input;
  uint32_t curve;
  unsigned curve_value;
  int factor;
  uint32_t magnitude;
  uint16_t complement;

  if (!rom || !rom->bytes || !tone || !tone->common || !component ||
      !component->bytes || !depth || velocity > 127)
    return false;
  input = sc88_tvf_be16(component->bytes + 0x48);
  if (input == 0) {
    *depth = 0;
    return true;
  }
  if (soft_pedal)
    velocity = (uint8_t)(((uint32_t)velocity * UINT16_C(0xb76f)) >> 16);
  curve = ((uint32_t)tone->common[0x21] << 16) |
    sc88_tvf_be16(component->bytes + 0x3a);
  if (curve + velocity >= rom->size)
    return false;
  curve_value = rom->bytes[curve + velocity];
  factor = sc88_tvf_s16(sc88_tvf_be16(component->bytes + 0x60));
  if (factor < 0) {
    magnitude = (uint32_t)(-factor);
    curve_value = (unsigned)(uint8_t)(0u - curve_value) & 0x7fu;
  } else {
    magnitude = (uint32_t)factor;
  }
  curve_value = (unsigned)(uint8_t)(~curve_value) & 0x7fu;
  complement = (uint16_t)~(uint16_t)(magnitude * curve_value);
  *depth = (uint16_t)(((uint32_t)input * complement) >> 16);
  return true;
}

static void sc88_tvf_prepare_increment(uint16_t table_rate, uint16_t scale,
                                       uint16_t *phase,
                                       uint16_t *increment)
{
  uint32_t product;
  if (table_rate < 16)
    table_rate = UINT16_MAX;
  product = (uint32_t)table_rate * scale;
  if (product >= UINT32_C(0x01000000)) {
    *phase = UINT16_MAX;
    *increment = UINT16_MAX;
  } else {
    *phase = 0;
    *increment = (uint16_t)(product >> 8);
  }
}

static int16_t sc88_tvf_scale_target(int16_t target, uint16_t depth)
{
  return sc88_tvf_s16((uint16_t)sc88_tvf_floor_div_pow2(
    (int32_t)target * depth, 16));
}

bool sc88_tvf_envelope_prepare(const struct sc88_rom *rom,
                               const struct sc88_tone *tone,
                               const struct sc88_component *component,
                               uint8_t selector_key, uint8_t velocity,
                               bool soft_pedal,
                               struct sc88_tvf_envelope *envelope)
{
  uint16_t key_scale;
  unsigned stage;

  if (!rom || !rom->bytes || !tone || !component || !component->bytes ||
      !envelope || selector_key > 127 || velocity > 127 ||
      SC88_ENVELOPE_RATE_TABLE + 128u * 2 > rom->size ||
      !sc88_tvf_envelope_depth(rom, tone, component, velocity, soft_pedal,
                               &envelope->depth))
    return false;
  {
    uint16_t depth = envelope->depth;
    memset(envelope, 0, sizeof *envelope);
    envelope->depth = depth;
  }
  envelope->stage = 4;
  if (envelope->depth == 0)
    return true;
  if (!sc88_tvf_key_rate_scale(rom, tone, component, selector_key,
                                0x5a, 0x5e, &key_scale))
    return false;
  for (stage = 0; stage < 4; ++stage) {
    uint16_t velocity_scale;
    uint16_t final_scale;
    uint16_t table_rate;
    int factor = sc88_tvf_s8(component->bytes[stage < 2 ? 0x62 : 0x63]);
    if (!sc88_tvf_velocity_rate_scale(rom, velocity, factor,
                                      &velocity_scale))
      return false;
    final_scale = (uint16_t)(((uint32_t)key_scale * velocity_scale) >> 8);
    table_rate = sc88_tvf_be16(rom->bytes + SC88_ENVELOPE_RATE_TABLE +
      (uint32_t)component->bytes[0x54 + stage] * 2);
    sc88_tvf_prepare_increment(table_rate, final_scale,
                               envelope->initial_phases + stage,
                               envelope->increments + stage);
    envelope->targets[stage] = sc88_tvf_scale_target(
      sc88_tvf_s16(sc88_tvf_be16(component->bytes + 0x4a + stage * 2)),
      envelope->depth);
  }
  envelope->stage = component->bytes[0x54] == 0 ? 1 : 0;
  envelope->base = envelope->stage == 1 ? envelope->targets[0] : 0;
  envelope->current = envelope->base;
  envelope->delta = sc88_tvf_s16((uint16_t)(
    (uint16_t)envelope->targets[envelope->stage] -
    (uint16_t)envelope->base));
  envelope->phase = envelope->initial_phases[envelope->stage];
  envelope->active = true;
  return true;
}

bool sc88_tvf_envelope_advance(struct sc88_tvf_envelope *envelope,
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
      ++envelope->stage;
      if (envelope->stage == 4) {
        envelope->current = sc88_tvf_s16((uint16_t)(
          (uint16_t)envelope->base + (uint16_t)envelope->delta));
        envelope->phase = 0;
        envelope->active = false;
      } else {
        envelope->base = sc88_tvf_s16((uint16_t)(
          (uint16_t)envelope->base + (uint16_t)envelope->delta));
        envelope->current = envelope->base;
        envelope->delta = sc88_tvf_s16((uint16_t)(
          (uint16_t)envelope->targets[envelope->stage] -
          (uint16_t)envelope->base));
        envelope->phase = envelope->initial_phases[envelope->stage];
      }
      return true;
    }
    working = next;
    --remaining;
    if (remaining == UINT16_MAX)
      break;
  }
  envelope->phase = working;
  envelope->saved_count = 0;
  envelope->current = sc88_tvf_s16((uint16_t)(
    (uint16_t)envelope->base + (uint16_t)sc88_tvf_floor_div_pow2(
      (int32_t)envelope->delta * working, 16)));
  return true;
}

bool sc88_tvf_update_frequency(const struct sc88_rom *rom,
                               int16_t accumulated_modulation,
                               struct sc88_tvf_registers *registers)
{
  int32_t combined;
  uint16_t limit;
  if (!rom || !rom->bytes || !registers ||
      rom->size < SC88_TVF_LIMIT_TABLE + 256u)
    return false;
  if (registers->fixed_tuple)
    return true;
  combined = sc88_tvf_be16(rom->bytes + SC88_TVF_BASE_TABLE +
                           (uint32_t)registers->cutoff_index * 2);
  combined += accumulated_modulation;
  if (combined < 0)
    combined = 0;
  else if (combined > UINT16_MAX)
    combined = UINT16_MAX;
  combined >>= 1;
  limit = (uint16_t)(sc88_tvf_be16(
    rom->bytes + SC88_TVF_LIMIT_TABLE +
    (uint32_t)registers->resonance_index * 2) >> 1);
  if (combined > limit)
    combined = limit;
  registers->combined = (uint16_t)combined;
  registers->frequency_target = (uint32_t)combined << 3;
  return true;
}
