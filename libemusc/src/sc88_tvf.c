/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_tvf.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#define SC88_TVF_BASE_TABLE 0x78702u
#define SC88_TVF_LIMIT_TABLE 0x78802u
/* The sound chip's own sample rate, which the cutoff word is a
   fraction of. */
#define SC88_TVF_NATIVE_RATE 32000.0
#define SC88_ENVELOPE_RATE_TABLE 0x1543eu
#define SC88_RATE_SCALE_TABLE 0x1573eu
#define SC88_RELEASE_PEDAL_TABLE 0x78a02u

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
                                int16_t pre_base_modulation,
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
  combined += pre_base_modulation;
  if (combined < 0)
    combined = 0;
  else if (combined > UINT16_MAX)
    combined = UINT16_MAX;
  combined >>= 1;
  registers->base_value = (uint16_t)combined;
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

bool sc88_tvf_key_modulation(const struct sc88_rom *rom,
                             const struct sc88_tone *tone,
                             const struct sc88_component *component,
                             uint8_t selector_key, int16_t *modulation)
{
  uint32_t table;
  int16_t key_value;
  int16_t factor;
  int32_t high;
  if (!rom || !rom->bytes || !tone || !tone->common || !component ||
      !component->bytes || !modulation || selector_key > 127)
    return false;
  table = ((uint32_t)tone->common[0x21] << 16) |
    sc88_tvf_be16(component->bytes + 0x40);
  if (table + (uint32_t)selector_key * 2 + 2 > rom->size)
    return false;
  key_value = sc88_tvf_s16(sc88_tvf_be16(
    rom->bytes + table + (uint32_t)selector_key * 2));
  factor = sc88_tvf_s16(sc88_tvf_be16(component->bytes + 0x42));
  high = sc88_tvf_floor_div_pow2((int32_t)key_value * factor, 16);
  *modulation = sc88_tvf_s16((uint16_t)((uint16_t)high << 1));
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

bool sc88_tvf_release_prepare(const struct sc88_rom *rom,
                              const struct sc88_tone *tone,
                              const struct sc88_component *component,
                              uint8_t selector_key, uint16_t envelope_depth,
                              struct sc88_tvf_release *release)
{
  uint16_t key_scale;
  uint16_t table_rate;
  uint16_t initial_phase;
  if (!rom || !rom->bytes || !tone || !component || !component->bytes ||
      !release || selector_key > 127 ||
      SC88_ENVELOPE_RATE_TABLE + 128u * 2 > rom->size)
    return false;
  memset(release, 0, sizeof *release);
  release->scale = UINT16_MAX;
  if (envelope_depth == 0)
    return true;
  if (!sc88_tvf_key_rate_scale(rom, tone, component, selector_key,
                                0x5c, 0x5f, &key_scale))
    return false;
  table_rate = sc88_tvf_be16(rom->bytes + SC88_ENVELOPE_RATE_TABLE +
                             (uint32_t)component->bytes[0x58] * 2);
  sc88_tvf_prepare_increment(table_rate, key_scale, &initial_phase,
                             &release->increment);
  release->phase = initial_phase;
  release->target = sc88_tvf_scale_target(
    sc88_tvf_s16(sc88_tvf_be16(component->bytes + 0x52)), envelope_depth);
  return true;
}

bool sc88_tvf_release_set_pedal(const struct sc88_rom *rom,
                                uint8_t hold1, bool continuous_hold,
                                bool keep_scale_at_zero,
                                bool sostenuto_retained,
                                struct sc88_tvf_release *release)
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
      release->scale = sc88_tvf_be16(rom->bytes + offset);
    }
  }
  release->active = true;
  return true;
}

bool sc88_tvf_release_advance(struct sc88_tvf_release *release,
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
    release->current = release->target;
    release->active = false;
  } else {
    release->phase = next;
    release->current = sc88_tvf_scale_target(release->target, next);
  }
  return true;
}

bool sc88_tvf_update_frequency(const struct sc88_rom *rom,
                               int16_t post_base_modulation,
                               struct sc88_tvf_registers *registers)
{
  uint16_t combined;
  uint16_t limit;
  if (!rom || !rom->bytes || !registers ||
      rom->size < SC88_TVF_LIMIT_TABLE + 256u)
    return false;
  if (registers->fixed_tuple)
    return true;
  combined = (uint16_t)(registers->base_value +
                        (uint16_t)post_base_modulation);
  limit = (uint16_t)(sc88_tvf_be16(
    rom->bytes + SC88_TVF_LIMIT_TABLE +
    (uint32_t)registers->resonance_index * 2) >> 1);
  if (combined > limit)
    combined = limit;
  registers->combined = combined;
  registers->frequency_target = (uint32_t)combined << 3;
  return true;
}

void sc88_tvf_latch_frequency(struct sc88_tvf_registers *registers)
{
  if (registers)
    registers->frequency_current = registers->frequency_target;
}

void sc88_tvf_audio_reset(struct sc88_tvf_audio_state *state)
{
  if (state)
    memset(state, 0, sizeof *state);
}

float sc88_tvf_audio_process_provisional(
  void *user, struct sc88_tvf_audio_state *state,
  const struct sc88_tvf_registers *registers,
  double period_fraction, float input)
{
  double word;
  double f1;
  double sine;
  double g;
  double damping;
  double denominator;
  double high;
  double band;
  double low;
  unsigned mode;

  if (!state || !registers)
    return input;
  if (period_fraction < 0.0)
    period_fraction = 0.0;
  else if (period_fraction > 1.0)
    period_fraction = 1.0;
  word = registers->frequency_current + period_fraction *
    ((double)registers->frequency_target - registers->frequency_current);
  f1 = word / 262144.0;
  if (f1 < 0.0)
    f1 = 0.0;
  else if (f1 > 0.999)
    f1 = 0.999;

  /* The word is `sin(pi * fc / 32000)`, not twice it.
   *
   * Reading it as Chamberlin's `F1 = 2*sin(pi*fc/fs)` halves every cutoff,
   * and because the firmware's own ceiling is `filter_limit >> 1 << 3`,
   * whose largest entry is `0xf800`, that put the **highest cutoff the
   * device could ask for at 5.3 kHz**. Everything came out dull: a snare
   * measured 1.0 % of its energy above 8 kHz where its own ROM sample has
   * 30.4 %, which is why it sounded like a tick rather than a snare. Taken
   * as `sin`, the same ceiling lands at 13.4 kHz and the snare matches its
   * sample to within 50 Hz of centroid (`M-014`).
   *
   * The fraction is of the sound chip's 32 kHz, so the cutoff is a real
   * frequency and the coefficient is recomputed for the output rate -
   * otherwise rendering at 48 kHz moves every cutoff up by half again. */
  sine = f1;
  {
    double rate = user ? *(const double *)user : SC88_TVF_NATIVE_RATE;
    double cutoff = asin(sine) * SC88_TVF_NATIVE_RATE / 3.14159265358979323846;
    double nyquist = rate * 0.5;
    if (cutoff > nyquist * 0.99)
      cutoff = nyquist * 0.99;
    g = tan(3.14159265358979323846 * cutoff / rate);
  }
  damping = 2.0 - 1.9 * (registers->resonance_index / 127.0);
  denominator = 1.0 + damping * g + g * g;
  high = (input - (damping + g) * state->integrator_band -
          state->integrator_low) / denominator;
  band = g * high + state->integrator_band;
  low = g * band + state->integrator_low;
  state->integrator_band = (float)(2.0 * band - state->integrator_band);
  state->integrator_low = (float)(2.0 * low - state->integrator_low);

  mode = (registers->filter_select >> 10) & 3u;
  if (mode == 0)
    return (float)low;
  if (mode == 1)
    return (float)band;
  if (mode == 2)
    return (float)high;
  return input;
}
