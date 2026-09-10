/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_lfo.h"

/* SC88-CTL v1.01 offsets. The two increment tables sit immediately after the
 * 16-entry callback dispatch table at 0x29ba. */
#define SC88_LFO_RATE_TABLE 0x29dau
#define SC88_LFO_DELAY_TABLE 0x2adau
#define SC88_LFO_SINE_TABLE 0x1492cu
#define SC88_LFO_TABLE_10 0x14524u
#define SC88_LFO_TABLE_12 0x14626u
#define SC88_LFO_TABLE_14 0x14728u
#define SC88_LFO_TABLE_16 0x1482au
#define SC88_LFO_TABLE_POINTS 129u
#define SC88_LFO_MAX_INCREMENT UINT16_C(0x28f6)
#define SC88_LFO_INTERPOLATE_BELOW UINT16_C(0x0200)
#define SC88_LFO_SLEW_STEP INT32_C(0x1c2)

/* The control task's own period, 8.0008 ms, gives the phase unit its size. */
#define SC88_LFO_SERVICE_HZ 124.987501249875

static uint16_t sc88_lfo_be16(const uint8_t *p)
{
  return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

/* A two's-complement word, without the implementation-defined result of an
 * out-of-range unsigned-to-signed conversion. */
static int16_t sc88_lfo_s16(uint16_t value)
{
  return value <= INT16_MAX
    ? (int16_t)value
    : (int16_t)(-1 - (int32_t)(UINT16_MAX - value));
}

static int32_t sc88_lfo_floor_shift(int32_t value, unsigned bits)
{
  return value < 0 ? -(int32_t)(((uint32_t)(-value) +
                                 ((UINT32_C(1) << bits) - 1u)) >> bits)
                   : (int32_t)((uint32_t)value >> bits);
}

bool sc88_lfo_rate_control(int16_t routed, int16_t *out)
{
  int32_t clipped = routed;
  if (!out)
    return false;
  if (clipped < -4000)
    clipped = -4000;
  else if (clipped > 4000)
    clipped = 4000;
  *out = (int16_t)sc88_lfo_floor_shift(
    clipped * INT32_C(2) * INT32_C(0xa7c7), 16);
  return true;
}

bool sc88_lfo_common_rate_index(unsigned tone_rate, unsigned part_rate,
                                unsigned user_rate, uint8_t *out)
{
  int index;
  if (!out || tone_rate > 127 || part_rate > 127 || user_rate > 127)
    return false;
  index = (int)tone_rate + 2 * ((int)part_rate + (int)user_rate - 0x80);
  if (index < 0)
    index = 0;
  else if (index > 127)
    index = 127;
  *out = (uint8_t)index;
  return true;
}

bool sc88_lfo_common_delay_index(int tone_delay, unsigned part_delay,
                                 unsigned user_delay, int16_t *out)
{
  int index;
  if (!out || tone_delay < -128 || tone_delay > 127 || part_delay > 127 ||
      user_delay > 127)
    return false;
  if (tone_delay < 0) {
    *out = -1;
    return true;
  }
  index = tone_delay + 2 * ((int)part_delay + (int)user_delay - 0x80);
  if (index < 0)
    index = 0;
  else if (index > 127)
    index = 127;
  *out = (int16_t)index;
  return true;
}

bool sc88_lfo_effective_increment(uint16_t base, int16_t control,
                                  uint16_t *out)
{
  uint16_t sum;
  if (!out)
    return false;
  sum = (uint16_t)(base + (uint16_t)control);
  if (sc88_lfo_s16(sum) <= 0)
    sum = 0;
  else if (sum > SC88_LFO_MAX_INCREMENT)
    sum = SC88_LFO_MAX_INCREMENT;
  *out = sum;
  return true;
}

int16_t sc88_lfo_square(uint16_t phase)
{
  return (phase & UINT16_C(0x8000)) ? INT16_MIN : INT16_MAX;
}

int16_t sc88_lfo_triangle(uint16_t phase)
{
  uint16_t doubled = (uint16_t)(phase << 1);
  if ((phase ^ doubled) & UINT16_C(0x8000))
    doubled = (uint16_t)~doubled;
  return sc88_lfo_s16(doubled);
}

int16_t sc88_lfo_rectified_triangle(uint16_t phase)
{
  uint16_t value = (uint16_t)sc88_lfo_triangle(phase);
  if (value & UINT16_C(0x8000))
    value = (uint16_t)(value - UINT16_C(0x7fff));
  return sc88_lfo_s16(value);
}

int16_t sc88_lfo_slew_random(int16_t current, int16_t target)
{
  int32_t candidate;
  if (target >= current) {
    candidate = (int32_t)current + SC88_LFO_SLEW_STEP;
    return candidate > target ? target : (int16_t)candidate;
  }
  candidate = (int32_t)current - SC88_LFO_SLEW_STEP;
  return candidate < target ? target : (int16_t)candidate;
}

uint16_t sc88_lfo_random_target(uint16_t seed, uint16_t phase)
{
  uint16_t sum = (uint16_t)(seed + phase);
  return (uint16_t)((uint16_t)(sum << 8) | (uint16_t)(sum >> 8));
}

bool sc88_lfo_phase_advance(uint16_t increment, uint8_t catchup_count,
                            uint16_t *phase, uint16_t *seed,
                            uint16_t *target)
{
  unsigned steps, index;
  if (!phase || !seed || !target || increment == 0 ||
      increment > SC88_LFO_MAX_INCREMENT)
    return false;
  steps = (unsigned)catchup_count + 1u;
  for (index = 0; index < steps; ++index) {
    uint16_t previous = *phase;
    uint16_t next = (uint16_t)(previous + increment);
    /* the random word turns over on the H8's signed overflow, not on carry */
    if ((uint16_t)(~(previous ^ increment) & (previous ^ next)) &
        UINT16_C(0x8000)) {
      *seed = sc88_lfo_random_target(*seed, next);
      *target = *seed;
    }
    *phase = next;
  }
  return true;
}

bool sc88_lfo_table_sample(const struct sc88_rom *rom, uint32_t table,
                           uint16_t phase, uint16_t increment, int16_t *out)
{
  unsigned index;
  uint16_t multiplier, first, second;
  int16_t delta;
  int32_t scaled;
  if (!rom || !rom->bytes || !out ||
      rom->size < table + SC88_LFO_TABLE_POINTS * 2u)
    return false;
  index = phase >> 9;
  first = sc88_lfo_be16(rom->bytes + table + index * 2u);
  if (increment >= SC88_LFO_INTERPOLATE_BELOW) {
    *out = sc88_lfo_s16(first);
    return true;
  }
  /* The firmware's own multiplier keeps the seven-bit table index in its low
   * byte; an idealized fractional phase is a different number. */
  multiplier = (uint16_t)((uint16_t)((phase >> 1) << 8) |
                          (uint16_t)((phase >> 1) >> 8));
  second = sc88_lfo_be16(rom->bytes + table + (index + 1u) * 2u);
  delta = sc88_lfo_s16((uint16_t)(second - first));
  scaled = (int32_t)delta * multiplier / INT32_C(65536);
  *out = sc88_lfo_s16((uint16_t)(first + (uint16_t)scaled));
  return true;
}

bool sc88_lfo_waveform(const struct sc88_rom *rom, uint8_t selector,
                       uint16_t phase, uint16_t increment, int16_t previous,
                       int16_t target, int16_t *out)
{
  if (!out || (selector & 1u) || selector > 0x1e)
    return false;
  switch (selector) {
  case 0x00:
    return sc88_lfo_table_sample(rom, SC88_LFO_SINE_TABLE, phase, increment,
                                 out);
  case 0x02:
    *out = sc88_lfo_square(phase);
    return true;
  case 0x06:
    *out = sc88_lfo_triangle(phase);
    return true;
  case 0x08:
    *out = sc88_lfo_rectified_triangle(phase);
    return true;
  case 0x0a:
    *out = target;
    return true;
  case 0x0c:
    *out = sc88_lfo_slew_random(previous, target);
    return true;
  case 0x10:
    return sc88_lfo_table_sample(rom, SC88_LFO_TABLE_10, phase, increment,
                                 out);
  case 0x12:
    return sc88_lfo_table_sample(rom, SC88_LFO_TABLE_12, phase, increment,
                                 out);
  case 0x14:
    return sc88_lfo_table_sample(rom, SC88_LFO_TABLE_14, phase, increment,
                                 out);
  case 0x16:
    return sc88_lfo_table_sample(rom, SC88_LFO_TABLE_16, phase, increment,
                                 out);
  default:
    /* 0x04, 0x0e and 0x18..0x1e all return the phase word itself */
    *out = sc88_lfo_s16(phase);
    return true;
  }
}

bool sc88_lfo_ramp_initialize(uint16_t delay_increment,
                              uint16_t fade_increment,
                              struct sc88_lfo_ramp *ramp)
{
  if (!ramp)
    return false;
  ramp->delay_phase = delay_increment;
  ramp->delay_increment = delay_increment;
  ramp->fade_increment = fade_increment;
  /* ffff is the sentinel that starts the fade already running */
  ramp->fade = delay_increment == UINT16_MAX ? fade_increment : 0;
  return true;
}

bool sc88_lfo_ramp_activate_immediate(struct sc88_lfo_ramp *ramp)
{
  if (!ramp)
    return false;
  if (ramp->delay_increment == 0)
    ramp->fade = 1;
  return true;
}

bool sc88_lfo_ramp_advance(struct sc88_lfo_ramp *ramp, uint8_t catchup_count)
{
  unsigned steps, index;
  if (!ramp)
    return false;
  steps = (unsigned)catchup_count + 1u;
  index = 0;
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

static bool sc88_lfo_prepare_ramp(struct sc88_lfo *lfo, uint16_t delay,
                                  uint16_t fade)
{
  if (!sc88_lfo_ramp_initialize(delay, fade, &lfo->ramp))
    return false;
  lfo->output = 0;
  lfo->random_target = 0;
  return true;
}

bool sc88_lfo_common_prepare(const struct sc88_rom *rom,
                             const struct sc88_tone *tone,
                             unsigned part_rate, unsigned user_rate,
                             unsigned part_delay, unsigned user_delay,
                             struct sc88_lfo *lfo)
{
  uint8_t rate_index;
  int16_t delay_index;
  uint16_t delay;
  if (!rom || !rom->bytes || !tone || !tone->common || !lfo ||
      rom->size < SC88_LFO_DELAY_TABLE + 256u)
    return false;
  if (!sc88_lfo_common_rate_index(tone->common[0x1a], part_rate, user_rate,
                                  &rate_index) ||
      !sc88_lfo_common_delay_index((int)(int8_t)tone->common[0x1b],
                                   part_delay, user_delay, &delay_index))
    return false;
  lfo->selector = tone->common[0x17];
  lfo->share_request = tone->common[0x18];
  /* the initial phase is a high byte; the low byte is cleared */
  lfo->phase = (uint16_t)((uint16_t)tone->common[0x19] << 8);
  lfo->base_increment = sc88_lfo_be16(
    rom->bytes + SC88_LFO_RATE_TABLE + (unsigned)rate_index * 2u);
  /* a negative delay byte bypasses the table rather than indexing it */
  delay = delay_index < 0 ? 0 : sc88_lfo_be16(
    rom->bytes + SC88_LFO_DELAY_TABLE + (unsigned)delay_index * 2u);
  return sc88_lfo_prepare_ramp(lfo, delay,
                               sc88_lfo_be16(tone->common + 0x1c));
}

bool sc88_lfo_local_prepare(const struct sc88_rom *rom,
                            const struct sc88_component *component,
                            struct sc88_lfo *lfo)
{
  if (!rom || !rom->bytes || !component || !component->bytes || !lfo)
    return false;
  lfo->selector = component->bytes[0x07];
  lfo->share_request = component->bytes[0x08];
  lfo->phase = (uint16_t)((uint16_t)component->bytes[0x09] << 8);
  /* the local path has no rate table: its increment is the field itself */
  lfo->base_increment = sc88_lfo_be16(component->bytes + 0x0a);
  return sc88_lfo_prepare_ramp(lfo, sc88_lfo_be16(component->bytes + 0x0c),
                               sc88_lfo_be16(component->bytes + 0x0e));
}

bool sc88_lfo_advance(const struct sc88_rom *rom, struct sc88_lfo *lfo,
                      int16_t rate_control, uint8_t catchup_count,
                      uint16_t *seed)
{
  uint16_t increment, target;
  if (!lfo || !seed)
    return false;
  if (!sc88_lfo_effective_increment(lfo->base_increment, rate_control,
                                    &increment))
    return false;
  if (!sc88_lfo_ramp_advance(&lfo->ramp, catchup_count))
    return false;
  if (increment == 0)
    return true;                        /* stalled: phase and output stand */
  target = (uint16_t)lfo->random_target;
  if (!sc88_lfo_phase_advance(increment, catchup_count, &lfo->phase, seed,
                              &target))
    return false;
  lfo->random_target = sc88_lfo_s16(target);
  /* exactly one waveform evaluation per service, however late it ran */
  return sc88_lfo_waveform(rom, lfo->selector, lfo->phase, increment,
                           lfo->output, lfo->random_target, &lfo->output);
}

double sc88_lfo_frequency(uint16_t increment)
{
  return (double)increment * SC88_LFO_SERVICE_HZ / 65536.0;
}
