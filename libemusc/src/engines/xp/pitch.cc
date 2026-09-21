/* SPDX-License-Identifier: CC0-1.0 */
#include "pitch.h"

#include "devices/sc88.h"

#include <cstring>

namespace EmuSC { namespace Xp {

namespace {

uint16_t be16(const uint8_t *p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

int16_t s16(uint16_t value)
{
  return value <= INT16_MAX ? (int16_t)value
    : (int16_t)(-1 - (int32_t)(UINT16_MAX - value));
}

int8_t s8(uint8_t value)
{
  return value <= INT8_MAX ? (int8_t)value
    : (int8_t)(-1 - (int16_t)(UINT8_MAX - value));
}

int32_t floor16(int32_t value)
{
  if (value >= 0)
    return value / 65536;
  return -(int32_t)(((uint32_t)(-value) + 65535u) >> 16);
}

int16_t scale_target(int16_t target, uint16_t depth)
{
  return s16((uint16_t)floor16((int32_t)target * depth));
}

bool rate_scale(const struct sc88_rom *rom, const struct sc88_tone *tone,
                 const struct sc88_component *component, uint8_t key,
                 uint16_t pointerAt, uint8_t factorAt, uint8_t velocity,
                 int velocityFactor, bool useVelocity, uint16_t *scale)
{
  if (!rom || !rom->bytes || !tone || !tone->common || !component ||
      !component->bytes || !scale || key > 127 || velocity > 127 ||
      kXpRateScaleTable + 258u > rom->size)
    return false;
  uint32_t curve = ((uint32_t)tone->common[0x21] << 16) |
    be16(component->bytes + pointerAt);
  if (curve + key >= rom->size)
    return false;
  int index = floor16((int32_t)s8(rom->bytes[curve + key]) *
                      s8((uint8_t)(0u - component->bytes[factorAt])) * 256) + 64;
  if (index < 0 || index > 128)
    return false;
  uint16_t keyScale = be16(rom->bytes + kXpRateScaleTable + (uint32_t)index * 2);
  uint16_t velocityScale = 0x0100;
  if (useVelocity) {
    int product = 2 * ((int)velocity - 64) * velocityFactor;
    index = (product >= 0 ? product / 256 :
             -(int)(((unsigned)(-product) + 255u) >> 8)) + 64;
    if (index < 0 || index > 128)
      return false;
    velocityScale = be16(rom->bytes + kXpRateScaleTable + (uint32_t)index * 2);
  }
  *scale = (uint16_t)(((uint32_t)keyScale * velocityScale) >> 8);
  return true;
}

void prepare_increment(uint16_t tableRate, uint16_t scale, uint16_t *phase,
                        uint16_t *increment)
{
  if (tableRate < 16)
    tableRate = UINT16_MAX;
  uint32_t product = (uint32_t)tableRate * scale;
  if (product >= UINT32_C(0x01000000)) {
    *phase = UINT16_MAX;
    *increment = UINT16_MAX;
  } else {
    *phase = 0;
    *increment = (uint16_t)(product >> 8);
  }
}

uint16_t velocity_depth(uint16_t input, uint8_t velocity, int factor)
{
  unsigned transformed = velocity;
  uint32_t magnitude;
  if (factor < 0) {
    magnitude = (uint32_t)(-factor);
    transformed = (unsigned)(uint8_t)(0u - transformed) & 0x7fu;
  } else {
    magnitude = (uint32_t)factor;
  }
  transformed = (unsigned)(uint8_t)(~transformed) & 0x7fu;
  uint16_t complement = (uint16_t)~(uint16_t)(magnitude * transformed);
  return (uint16_t)(((uint32_t)input * complement) >> 16);
}

}  // namespace

bool pitch_envelope_prepare(const struct sc88_rom *rom, const struct sc88_tone *tone,
                             const struct sc88_component *component,
                             uint8_t selectorKey, uint8_t velocity,
                             struct sc88_pitch_envelope *envelope)
{
  if (!rom || !rom->bytes || !tone || !component || !component->bytes ||
      !envelope || selectorKey > 127 || velocity > 127 ||
      kXpEnvelopeRateTable + 256u > rom->size)
    return false;
  uint16_t scale;
  if (!rate_scale(rom, tone, component, selectorKey, 0x30, 0x34,
                  velocity, s8(component->bytes[0x38]), true, &scale))
    return false;
  std::memset(envelope, 0, sizeof *envelope);
  envelope->depth = velocity_depth(be16(component->bytes + 0x1a), velocity,
                                   s16(be16(component->bytes + 0x36)));
  for (unsigned stage = 0; stage < 4; ++stage) {
    uint16_t rate = be16(rom->bytes + kXpEnvelopeRateTable +
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

bool pitch_envelope_advance(struct sc88_pitch_envelope *envelope,
                             unsigned elapsedPeriods)
{
  if (!envelope || !envelope->active || envelope->stage >= 4 ||
      elapsedPeriods == 0)
    return false;
  uint8_t catchup = (uint8_t)(elapsedPeriods - 1);
  uint16_t remaining = (uint16_t)(envelope->saved_count +
    (catchup <= 127 ? (int)catchup : (int)catchup - 256));
  uint16_t working = envelope->phase;
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

bool pitch_release_prepare(const struct sc88_rom *rom, const struct sc88_tone *tone,
                            const struct sc88_component *component,
                            uint8_t selectorKey, uint16_t envelopeDepth,
                            struct sc88_pitch_release *release)
{
  if (!release || !component || !component->bytes)
    return false;
  uint16_t scale;
  if (!rate_scale(rom, tone, component, selectorKey, 0x32, 0x35,
                  64, 0, false, &scale))
    return false;
  std::memset(release, 0, sizeof *release);
  release->scale = UINT16_MAX;
  uint16_t rate = be16(rom->bytes + kXpEnvelopeRateTable +
                       (uint32_t)component->bytes[0x2e] * 2);
  uint16_t phase;
  prepare_increment(rate, scale, &phase, &release->increment);
  release->phase = phase;
  release->destination = scale_target(
    s16(be16(component->bytes + 0x28)), envelopeDepth);
  /* `6559` and `58ce` write ONE word, `0x2a5a`: the destination at note on,
     and the distance left to it at note off. `delta` is that word and
     `destination` is kept beside it, so a note off that does not reach
     `58ce` - the `+0x15` tones, `sc88_engine_start_release` - ramps toward
     the destination exactly as the machine does. */
  release->delta = release->destination;
  return true;
}

bool pitch_release_activate(const struct sc88_rom *rom, uint8_t hold1,
                             bool continuousHold, bool keepScaleAtZero,
                             bool sostenutoRetained,
                             struct sc88_pitch_release *release)
{
  if (!rom || !rom->bytes || !release || hold1 > 127)
    return false;
  release->current = 0;
  release->scale_enabled = true;
  if (sostenutoRetained) {
    release->scale = 0;
  } else {
    release->scale = UINT16_MAX;
    unsigned effective = continuousHold ? hold1 : (hold1 >= 64 ? 127u : 0u);
    if (effective == 0) {
      if (!keepScaleAtZero)
        release->scale_enabled = false;
    } else {
      uint32_t offset = kXpReleasePedalTable + (127u - effective) * 2;
      if (offset + 2 > rom->size)
        return false;
      release->scale = be16(rom->bytes + offset);
    }
  }
  release->active = true;
  return true;
}

bool pitch_release_advance(struct sc88_pitch_release *release,
                            unsigned elapsedPeriods)
{
  if (!release || !release->active || elapsedPeriods == 0)
    return false;
  uint16_t step = release->scale_enabled
    ? (uint16_t)(((uint32_t)release->increment * release->scale) >> 16)
    : release->increment;
  uint8_t periods = (uint8_t)elapsedPeriods;
  uint32_t product = (uint32_t)step * periods;
  uint16_t next = (uint16_t)(release->phase + (uint16_t)product);
  if ((product >> 16) != 0 || next < release->phase) {
    release->current = release->delta;
    release->active = false;
  } else {
    release->phase = next;
    release->current = scale_target(release->delta, next);
  }
  return true;
}

int16_t pitch_envelope_sum(const struct sc88_pitch_envelope *envelope,
                            const struct sc88_pitch_release *release)
{
  if (!envelope || !release)
    return 0;
  return s16((uint16_t)((uint16_t)envelope->current +
                        (uint16_t)release->current));
}

uint32_t pitch_current_word(uint32_t base, int32_t offset, int16_t envelopeSum)
{
  /* `0x5e41`..`0x5e58` sign-extends the envelope sum into the register pair
     {r4,r5} and doubles it (`ad 18` `ac 1e` twice), then adds the base and
     offset pair with `add:g.w`/`addx.w` - one 32-bit sum that wraps rather
     than saturating.

     `0x5e59` `4c 00 03` `cmp:i.w #3,r4` and `0x5e5c` `23 06` `bls.b` test
     the HIGH WORD alone, UNSIGNED. Everything the branch does not take -
     which is every sum outside 0x00000..0x3ffff, including every negative
     one, whose high word is 0xffff - falls into `0x5e5e` `5c 00 03` and
     `0x5e61` `5d ff ff`: r4 = 3, r5 = 0xffff. The saturation is to the
     MAXIMUM at both ends. */
  uint32_t value = base + (uint32_t)offset +
    2u * (uint32_t)(int32_t)envelopeSum;
  if ((value >> 16) > 3u)
    value = 0x3ffffu;
  return value & ~UINT32_C(1);
}

uint32_t portamento_rate(const struct sc88_rom *rom, uint8_t time)
{
  if (!rom || !rom->bytes || time == 0 || time > 127)
    return 0;
  uint32_t at = kPortamentoRateTable + (uint32_t)time * 4u;
  if (at + 4u > rom->size)
    return 0;
  return ((uint32_t)be16(rom->bytes + at) << 16) | be16(rom->bytes + at + 2);
}

void portamento_advance(struct sc88_portamento *portamento,
                         unsigned elapsedPeriods)
{
  if (!portamento || !portamento->active)
    return;
  /* `0x5fdf`: a time byte of zero ends the glide where it stands rather
     than stepping by the table's 0xffffffff entry. */
  if (portamento->rate == 0) {
    portamento->current = portamento->target;
    portamento->active = false;
    return;
  }
  for (unsigned i = 0; i < elapsedPeriods; ++i) {
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

}}  // namespace EmuSC::Xp
