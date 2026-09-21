/* SPDX-License-Identifier: CC0-1.0 */
#include "lfo.h"

#include "common/constants.h"
#include "devices/sc88.h"

namespace EmuSC { namespace Xp {

namespace {

uint16_t be16(const uint8_t *p)
{
  return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

/* A two's-complement word, without the implementation-defined result of an
 * out-of-range unsigned-to-signed conversion. */
int16_t s16(uint16_t value)
{
  return value <= INT16_MAX
    ? (int16_t)value
    : (int16_t)(-1 - (int32_t)(UINT16_MAX - value));
}

int32_t floor_shift(int32_t value, unsigned bits)
{
  return value < 0 ? -(int32_t)(((uint32_t)(-value) +
                                 ((UINT32_C(1) << bits) - 1u)) >> bits)
                   : (int32_t)((uint32_t)value >> bits);
}

bool prepare_ramp(struct sc88_lfo *lfo, uint16_t delay, uint16_t fade)
{
  if (!lfo_ramp_initialize(delay, fade, &lfo->ramp))
    return false;
  lfo->output = 0;
  lfo->random_target = 0;
  return true;
}

}  // namespace

bool lfo_rate_control(int16_t routed, int16_t *out)
{
  if (!out)
    return false;
  int32_t clipped = routed;
  if (clipped < -4000)
    clipped = -4000;
  else if (clipped > 4000)
    clipped = 4000;
  *out = (int16_t)floor_shift(clipped * INT32_C(2) * INT32_C(0xa7c7), 16);
  return true;
}

bool lfo_common_rate_index(unsigned toneRate, unsigned partRate,
                            unsigned userRate, uint8_t *out)
{
  if (!out || toneRate > 127 || partRate > 127 || userRate > 127)
    return false;
  int index = (int)toneRate + 2 * ((int)partRate + (int)userRate - 0x80);
  if (index < 0)
    index = 0;
  else if (index > 127)
    index = 127;
  *out = (uint8_t)index;
  return true;
}

bool lfo_common_delay_index(int toneDelay, unsigned partDelay,
                             unsigned userDelay, int16_t *out)
{
  if (!out || toneDelay < -128 || toneDelay > 127 || partDelay > 127 ||
      userDelay > 127)
    return false;
  if (toneDelay < 0) {
    *out = -1;
    return true;
  }
  int index = toneDelay + 2 * ((int)partDelay + (int)userDelay - 0x80);
  if (index < 0)
    index = 0;
  else if (index > 127)
    index = 127;
  *out = (int16_t)index;
  return true;
}

bool lfo_effective_increment(uint16_t base, int16_t control, uint16_t *out)
{
  if (!out)
    return false;
  uint16_t sum = (uint16_t)(base + (uint16_t)control);
  if (s16(sum) <= 0)
    sum = 0;
  else if (sum > kMaxIncrement)
    sum = kMaxIncrement;
  *out = sum;
  return true;
}

int16_t lfo_square(uint16_t phase)
{
  return (phase & UINT16_C(0x8000)) ? INT16_MIN : INT16_MAX;
}

int16_t lfo_triangle(uint16_t phase)
{
  uint16_t doubled = (uint16_t)(phase << 1);
  if ((phase ^ doubled) & UINT16_C(0x8000))
    doubled = (uint16_t)~doubled;
  return s16(doubled);
}

int16_t lfo_rectified_triangle(uint16_t phase)
{
  uint16_t value = (uint16_t)lfo_triangle(phase);
  if (value & UINT16_C(0x8000))
    value = (uint16_t)(value - UINT16_C(0x7fff));
  return s16(value);
}

int16_t lfo_slew_random(int16_t current, int16_t target)
{
  int32_t candidate;
  if (target >= current) {
    candidate = (int32_t)current + kSlewStep;
    return candidate > target ? target : (int16_t)candidate;
  }
  candidate = (int32_t)current - kSlewStep;
  return candidate < target ? target : (int16_t)candidate;
}

uint16_t lfo_random_target(uint16_t seed, uint16_t phase)
{
  uint16_t sum = (uint16_t)(seed + phase);
  return (uint16_t)((uint16_t)(sum << 8) | (uint16_t)(sum >> 8));
}

bool lfo_phase_advance(uint16_t increment, uint8_t catchupCount,
                        uint16_t *phase, uint16_t *seed, uint16_t *target)
{
  if (!phase || !seed || !target || increment == 0 ||
      increment > kMaxIncrement)
    return false;
  unsigned steps = (unsigned)catchupCount + 1u;
  for (unsigned index = 0; index < steps; ++index) {
    uint16_t previous = *phase;
    uint16_t next = (uint16_t)(previous + increment);
    /* the random word turns over on the H8's signed overflow, not on carry */
    if ((uint16_t)(~(previous ^ increment) & (previous ^ next)) &
        UINT16_C(0x8000)) {
      *seed = lfo_random_target(*seed, next);
      *target = *seed;
    }
    *phase = next;
  }
  return true;
}

bool lfo_table_sample(const struct sc88_rom *rom, uint32_t table,
                       uint16_t phase, uint16_t increment, int16_t *out)
{
  if (!rom || !rom->bytes || !out ||
      rom->size < table + kTablePoints * 2u)
    return false;
  unsigned index = phase >> 9;
  uint16_t first = be16(rom->bytes + table + index * 2u);
  if (increment >= kInterpolateBelow) {
    *out = s16(first);
    return true;
  }
  /* The firmware's own multiplier keeps the seven-bit table index in its low
   * byte; an idealized fractional phase is a different number. */
  uint16_t multiplier = (uint16_t)((uint16_t)((phase >> 1) << 8) |
                                   (uint16_t)((phase >> 1) >> 8));
  uint16_t second = be16(rom->bytes + table + (index + 1u) * 2u);
  int16_t delta = s16((uint16_t)(second - first));
  int32_t scaled = (int32_t)delta * multiplier / INT32_C(65536);
  *out = s16((uint16_t)(first + (uint16_t)scaled));
  return true;
}

bool lfo_waveform(const struct sc88_rom *rom, uint8_t selector,
                   uint16_t phase, uint16_t increment, int16_t previous,
                   int16_t target, int16_t *out)
{
  if (!out || (selector & 1u) || selector > 0x1e)
    return false;
  switch (selector) {
  case 0x00:
    return lfo_table_sample(rom, kSineTable, phase, increment, out);
  case 0x02:
    *out = lfo_square(phase);
    return true;
  case 0x06:
    *out = lfo_triangle(phase);
    return true;
  case 0x08:
    *out = lfo_rectified_triangle(phase);
    return true;
  case 0x0a:
    *out = target;
    return true;
  case 0x0c:
    *out = lfo_slew_random(previous, target);
    return true;
  case 0x10:
    return lfo_table_sample(rom, kTable10, phase, increment, out);
  case 0x12:
    return lfo_table_sample(rom, kTable12, phase, increment, out);
  case 0x14:
    return lfo_table_sample(rom, kTable14, phase, increment, out);
  case 0x16:
    return lfo_table_sample(rom, kTable16, phase, increment, out);
  default:
    /* 0x04, 0x0e and 0x18..0x1e all return the phase word itself */
    *out = s16(phase);
    return true;
  }
}

bool lfo_ramp_initialize(uint16_t delayIncrement, uint16_t fadeIncrement,
                          struct sc88_lfo_ramp *ramp)
{
  if (!ramp)
    return false;
  ramp->delay_phase = delayIncrement;
  ramp->delay_increment = delayIncrement;
  ramp->fade_increment = fadeIncrement;
  /* ffff is the sentinel that starts the fade already running */
  ramp->fade = delayIncrement == UINT16_MAX ? fadeIncrement : 0;
  return true;
}

bool lfo_ramp_activate_immediate(struct sc88_lfo_ramp *ramp)
{
  if (!ramp)
    return false;
  if (ramp->delay_increment == 0)
    ramp->fade = 1;
  return true;
}

bool lfo_ramp_advance(struct sc88_lfo_ramp *ramp, uint8_t catchupCount)
{
  if (!ramp)
    return false;
  unsigned steps = (unsigned)catchupCount + 1u;
  unsigned index = 0;
  if (ramp->fade == 0) {
    for (; index < steps; ++index) {
      uint16_t previous = ramp->delay_phase;
      ramp->delay_phase = (uint16_t)(previous + ramp->delay_increment);
      if (ramp->delay_phase < previous)
        break;                          /* the carrying step begins the fade */
    }
    if (index == steps)
      return true;
  }
  for (; index < steps; ++index) {
    uint16_t previous = ramp->fade;
    ramp->fade = (uint16_t)(previous + ramp->fade_increment);
    if (ramp->fade < previous) {
      ramp->fade = UINT16_MAX;
      break;
    }
  }
  return true;
}

bool lfo_common_prepare(const struct sc88_rom *rom, const struct sc88_tone *tone,
                         unsigned partRate, unsigned userRate,
                         unsigned partDelay, unsigned userDelay,
                         struct sc88_lfo *lfo)
{
  if (!rom || !rom->bytes || !tone || !tone->common || !lfo ||
      rom->size < kDelayTable + 256u)
    return false;
  uint8_t rateIndex;
  int16_t delayIndex;
  if (!lfo_common_rate_index(tone->common[0x1a], partRate, userRate,
                              &rateIndex) ||
      !lfo_common_delay_index((int)(int8_t)tone->common[0x1b],
                               partDelay, userDelay, &delayIndex))
    return false;
  lfo->selector = tone->common[0x17];
  lfo->share_request = tone->common[0x18];
  /* the initial phase is a high byte; the low byte is cleared */
  lfo->phase = (uint16_t)((uint16_t)tone->common[0x19] << 8);
  lfo->base_increment = be16(rom->bytes + kRateTable + (unsigned)rateIndex * 2u);
  /* a negative delay byte bypasses the table rather than indexing it */
  uint16_t delay = delayIndex < 0 ? 0 : be16(
    rom->bytes + kDelayTable + (unsigned)delayIndex * 2u);
  return prepare_ramp(lfo, delay, be16(tone->common + 0x1c));
}

bool lfo_local_prepare(const struct sc88_rom *rom,
                        const struct sc88_component *component,
                        struct sc88_lfo *lfo)
{
  if (!rom || !rom->bytes || !component || !component->bytes || !lfo)
    return false;
  lfo->selector = component->bytes[0x07];
  lfo->share_request = component->bytes[0x08];
  lfo->phase = (uint16_t)((uint16_t)component->bytes[0x09] << 8);
  /* the local path has no rate table: its increment is the field itself */
  lfo->base_increment = be16(component->bytes + 0x0a);
  return prepare_ramp(lfo, be16(component->bytes + 0x0c),
                      be16(component->bytes + 0x0e));
}

bool lfo_advance(const struct sc88_rom *rom, struct sc88_lfo *lfo,
                  int16_t rateControl, uint8_t catchupCount, uint16_t *seed)
{
  if (!lfo || !seed)
    return false;
  uint16_t increment;
  if (!lfo_effective_increment(lfo->base_increment, rateControl, &increment))
    return false;
  if (!lfo_ramp_advance(&lfo->ramp, catchupCount))
    return false;
  if (increment == 0)
    return true;                        /* stalled: phase and output stand */
  uint16_t target = (uint16_t)lfo->random_target;
  if (!lfo_phase_advance(increment, catchupCount, &lfo->phase, seed, &target))
    return false;
  lfo->random_target = s16(target);
  /* exactly one waveform evaluation per service, however late it ran */
  return lfo_waveform(rom, lfo->selector, lfo->phase, increment, lfo->output,
                      lfo->random_target, &lfo->output);
}

double lfo_frequency(uint16_t increment)
{
  return (double)increment * kXpControlPeriodHz / 65536.0;
}

}}  // namespace EmuSC::Xp
