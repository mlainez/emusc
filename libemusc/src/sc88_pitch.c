/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_pitch.h"

#include <limits.h>
#include <string.h>

#define SC88_ENVELOPE_RATE_TABLE 0x1543eu
#define SC88_RATE_SCALE_TABLE 0x1573eu
#define SC88_RELEASE_PEDAL_TABLE 0x78a02u
#define SC88_PORTAMENTO_RATE_TABLE 0x78502u

static uint16_t be16(const uint8_t *p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static int16_t s16(uint16_t value)
{
  return value <= INT16_MAX ? (int16_t)value
    : (int16_t)(-1 - (int32_t)(UINT16_MAX - value));
}

static int8_t s8(uint8_t value)
{
  return value <= INT8_MAX ? (int8_t)value
    : (int8_t)(-1 - (int16_t)(UINT8_MAX - value));
}

static int32_t floor16(int32_t value)
{
  if (value >= 0)
    return value / 65536;
  return -(int32_t)(((uint32_t)(-value) + 65535u) >> 16);
}

static int16_t scale_target(int16_t target, uint16_t depth)
{
  return s16((uint16_t)floor16((int32_t)target * depth));
}

static bool rate_scale(const struct sc88_rom *rom,
                       const struct sc88_tone *tone,
                       const struct sc88_component *component,
                       uint8_t key, uint16_t pointer_at, uint8_t factor_at,
                       uint8_t velocity, int velocity_factor,
                       bool use_velocity, uint16_t *scale)
{
  uint32_t curve;
  int index;
  uint16_t key_scale;
  uint16_t velocity_scale = 0x0100;
  if (!rom || !rom->bytes || !tone || !tone->common || !component ||
      !component->bytes || !scale || key > 127 || velocity > 127 ||
      SC88_RATE_SCALE_TABLE + 258u > rom->size)
    return false;
  curve = ((uint32_t)tone->common[0x21] << 16) |
    be16(component->bytes + pointer_at);
  if (curve + key >= rom->size)
    return false;
  index = floor16((int32_t)s8(rom->bytes[curve + key]) *
                  s8((uint8_t)(0u - component->bytes[factor_at])) * 256) + 64;
  if (index < 0 || index > 128)
    return false;
  key_scale = be16(rom->bytes + SC88_RATE_SCALE_TABLE + (uint32_t)index * 2);
  if (use_velocity) {
    int product = 2 * ((int)velocity - 64) * velocity_factor;
    index = (product >= 0 ? product / 256 :
             -(int)(((unsigned)(-product) + 255u) >> 8)) + 64;
    if (index < 0 || index > 128)
      return false;
    velocity_scale = be16(rom->bytes + SC88_RATE_SCALE_TABLE +
                          (uint32_t)index * 2);
  }
  *scale = (uint16_t)(((uint32_t)key_scale * velocity_scale) >> 8);
  return true;
}

static void prepare_increment(uint16_t table_rate, uint16_t scale,
                              uint16_t *phase, uint16_t *increment)
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

static uint16_t velocity_depth(uint16_t input, uint8_t velocity, int factor)
{
  unsigned transformed = velocity;
  uint32_t magnitude;
  uint16_t complement;
  if (factor < 0) {
    magnitude = (uint32_t)(-factor);
    transformed = (unsigned)(uint8_t)(0u - transformed) & 0x7fu;
  } else {
    magnitude = (uint32_t)factor;
  }
  transformed = (unsigned)(uint8_t)(~transformed) & 0x7fu;
  complement = (uint16_t)~(uint16_t)(magnitude * transformed);
  return (uint16_t)(((uint32_t)input * complement) >> 16);
}

bool sc88_pitch_envelope_prepare(const struct sc88_rom *rom,
                                 const struct sc88_tone *tone,
                                 const struct sc88_component *component,
                                 uint8_t selector_key, uint8_t velocity,
                                 struct sc88_pitch_envelope *envelope)
{
  unsigned stage;
  uint16_t scale;
  if (!rom || !rom->bytes || !tone || !component || !component->bytes ||
      !envelope || selector_key > 127 || velocity > 127 ||
      SC88_ENVELOPE_RATE_TABLE + 256u > rom->size ||
      !rate_scale(rom, tone, component, selector_key, 0x30, 0x34,
                  velocity, s8(component->bytes[0x38]), true, &scale))
    return false;
  memset(envelope, 0, sizeof *envelope);
  envelope->depth = velocity_depth(be16(component->bytes + 0x1a), velocity,
                                   s16(be16(component->bytes + 0x36)));
  for (stage = 0; stage < 4; ++stage) {
    uint16_t rate = be16(rom->bytes + SC88_ENVELOPE_RATE_TABLE +
                         (uint32_t)component->bytes[0x2a + stage] * 2);
    envelope->targets[stage] = scale_target(
      s16(be16(component->bytes + 0x20 + stage * 2)), envelope->depth);
    prepare_increment(rate, scale, envelope->initial_phases + stage,
                      envelope->increments + stage);
  }
  envelope->stage = component->bytes[0x2a] == 0 ? 1 : 0;
  envelope->base = scale_target(
    s16(be16(component->bytes + 0x1e + envelope->stage * 2)),
    envelope->depth);
  envelope->current = envelope->base;
  envelope->delta = s16((uint16_t)((uint16_t)envelope->targets[envelope->stage] -
                                    (uint16_t)envelope->base));
  envelope->phase = envelope->initial_phases[envelope->stage];
  envelope->active = true;
  return true;
}

bool sc88_pitch_envelope_advance(struct sc88_pitch_envelope *envelope,
                                 unsigned elapsed_periods)
{
  uint8_t catchup;
  uint16_t remaining;
  uint16_t working;
  if (!envelope || !envelope->active || envelope->stage >= 4 ||
      elapsed_periods == 0)
    return false;
  catchup = (uint8_t)(elapsed_periods - 1);
  remaining = (uint16_t)(envelope->saved_count +
    (catchup <= 127 ? (int)catchup : (int)catchup - 256));
  working = envelope->phase;
  for (;;) {
    uint16_t next = (uint16_t)(working + envelope->increments[envelope->stage]);
    if (next < working) {
      envelope->saved_count = (uint8_t)remaining;
      envelope->base = s16((uint16_t)((uint16_t)envelope->base +
                                      (uint16_t)envelope->delta));
      envelope->current = envelope->base;
      if (++envelope->stage == 4) {
        envelope->phase = 0;
        envelope->active = false;
      } else {
        envelope->delta = s16((uint16_t)(
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
  envelope->current = s16((uint16_t)((uint16_t)envelope->base +
    (uint16_t)floor16((int32_t)envelope->delta * working)));
  return true;
}

bool sc88_pitch_release_prepare(const struct sc88_rom *rom,
                                const struct sc88_tone *tone,
                                const struct sc88_component *component,
                                uint8_t selector_key, uint16_t envelope_depth,
                                struct sc88_pitch_release *release)
{
  uint16_t scale;
  uint16_t phase;
  uint16_t rate;
  if (!release || !component || !component->bytes ||
      !rate_scale(rom, tone, component, selector_key, 0x32, 0x35,
                  64, 0, false, &scale))
    return false;
  memset(release, 0, sizeof *release);
  release->scale = UINT16_MAX;
  rate = be16(rom->bytes + SC88_ENVELOPE_RATE_TABLE +
              (uint32_t)component->bytes[0x2e] * 2);
  prepare_increment(rate, scale, &phase, &release->increment);
  release->phase = phase;
  release->destination = scale_target(
    s16(be16(component->bytes + 0x28)), envelope_depth);
  return true;
}

bool sc88_pitch_release_activate(const struct sc88_rom *rom,
                                 uint8_t hold1, bool continuous_hold,
                                 bool keep_scale_at_zero,
                                 bool sostenuto_retained,
                                 int16_t envelope_current,
                                 struct sc88_pitch_release *release)
{
  unsigned effective;
  uint32_t offset;
  if (!rom || !rom->bytes || !release || hold1 > 127)
    return false;
  release->delta = s16((uint16_t)((uint16_t)release->destination -
                                  (uint16_t)envelope_current));
  release->current = 0;
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
      release->scale = be16(rom->bytes + offset);
    }
  }
  release->active = true;
  return true;
}

bool sc88_pitch_release_advance(struct sc88_pitch_release *release,
                                unsigned elapsed_periods)
{
  uint16_t step;
  uint8_t periods;
  uint32_t product;
  uint16_t next;
  if (!release || !release->active || elapsed_periods == 0)
    return false;
  step = release->scale_enabled
    ? (uint16_t)(((uint32_t)release->increment * release->scale) >> 16)
    : release->increment;
  periods = (uint8_t)elapsed_periods;
  product = (uint32_t)step * periods;
  next = (uint16_t)(release->phase + (uint16_t)product);
  if ((product >> 16) != 0 || next < release->phase) {
    release->current = release->delta;
    release->active = false;
  } else {
    release->phase = next;
    release->current = scale_target(release->delta, next);
  }
  return true;
}

int16_t sc88_pitch_envelope_sum(const struct sc88_pitch_envelope *envelope,
                                const struct sc88_pitch_release *release)
{
  if (!envelope || !release)
    return 0;
  return s16((uint16_t)((uint16_t)envelope->current +
                        (uint16_t)release->current));
}

uint32_t sc88_pitch_current_word(uint32_t base, int32_t offset,
                                 int16_t envelope_sum)
{
  int64_t value = (int64_t)base + offset + 2 * (int64_t)envelope_sum;
  if (value < 0)
    return 0;
  if (value > 0x3ffff)
    value = 0x3ffff;
  return (uint32_t)value & ~UINT32_C(1);
}

uint32_t sc88_portamento_rate(const struct sc88_rom *rom, uint8_t time)
{
  uint32_t at;
  if (!rom || !rom->bytes || time == 0 || time > 127)
    return 0;
  at = SC88_PORTAMENTO_RATE_TABLE + (uint32_t)time * 4u;
  if (at + 4u > rom->size)
    return 0;
  return ((uint32_t)be16(rom->bytes + at) << 16) | be16(rom->bytes + at + 2);
}

void sc88_portamento_advance(struct sc88_portamento *portamento,
                             unsigned elapsed_periods)
{
  unsigned i;
  if (!portamento || !portamento->active)
    return;
  /* `0x5fdf`: a time byte of zero ends the glide where it stands rather
     than stepping by the table's 0xffffffff entry. */
  if (portamento->rate == 0) {
    portamento->current = portamento->target;
    portamento->active = false;
    return;
  }
  for (i = 0; i < elapsed_periods; ++i) {
    if (portamento->ascending) {
      portamento->current += portamento->rate;
      if (portamento->current >= portamento->target)
        break;
    } else {
      portamento->current -= portamento->rate;
      if (portamento->current <= portamento->target)
        break;
    }
  }
  if (portamento->ascending ? portamento->current >= portamento->target
                            : portamento->current <= portamento->target) {
    portamento->current = portamento->target;
    portamento->active = false;
  }
}
