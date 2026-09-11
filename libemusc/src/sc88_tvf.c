/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_tvf.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#define SC88_TVF_BASE_TABLE 0x78702u
/* The registers are ROM-exact; this normalisation is not. TVF-Q current
   is the companion word << 2, i.e. resonance_index << 11, so this unity
   makes the neutral index 64 a damping of 2.0 - a critically damped
   section with no peak - and index 3 (Reso Panner) a Q near 10. */
#define SC88_TVF_Q_UNITY 65536.0
/* The TVF-F register is a log-frequency word in the XP pitch register's
   own domain. `07_synthesis/pitch.md` has the pitch word at 16384 units
   per octave, 18 bits, unity playback at 0x38000; routine 67a8 forms
   TVF-F as the same 18-bit high/low pair in the same scratch tuple with
   the same interpolation word 0x4100, and the base table at 0x78702
   steps by exactly 16384/12 per index once expanded - one semitone per
   index. The ROM fixes the slope. It does not say which frequency any
   register value means, so the anchor is inferred: the word one past the
   18-bit range, 0x40000, is read as the chip's Nyquist, so the base
   table's saturating top (0xffff, register 0x3fff8) names the highest
   frequency the filter has and nothing in either table lands above it.
   Unity (0x38000) is then fs/8, the limit table at 0x78802 spans
   11.3 kHz (resonance index 0) to 5.9 kHz (index 127), and the corners
   fitted on seven hardware notes sit within 0.44 octave rms of the
   computed word (`11_validation/measurements.md` M-136). The word is a
   frequency, as the pitch word is a rate; the coefficient the chip
   derives from it is not recovered, so the audio path warps it itself. */
#define SC88_TVF_OCTAVE_UNITS 16384.0
#define SC88_TVF_NYQUIST_WORD 0x40000
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

double sc88_tvf_word_to_hz(uint32_t word)
{
  return 0.5 * SC88_TVF_NATIVE_RATE *
    exp2(((double)word - SC88_TVF_NYQUIST_WORD) / SC88_TVF_OCTAVE_UNITS);
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

  /* Base and limit share one domain: both halved. Routine 6ccd shifts
   * the saturated table-plus-modulation sum right one (6d21) and the
   * limit is read from 78802 and shifted right one (68b4, 6984) before
   * the unsigned compare. Halved on both sides the base spans
   * 12561..32767 against a limit of 29811..31744, so the clamp bites
   * only for the top dozen indices. */
  combined = sc88_tvf_be16(rom->bytes + SC88_TVF_BASE_TABLE +
                           (unsigned)cutoff_index * 2u);
  combined += pre_base_modulation;
  if (combined < 0)
    combined = 0;
  else if (combined > UINT16_MAX)
    combined = UINT16_MAX;
  registers->base_unshifted = (uint16_t)combined;
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
  /* The envelope and release outputs join the word after the halving.
     Routine 6ccd saturates the table entry plus the key, controller and
     LFO terms, shifts the sum right one and stores it; 68a1, 68e3 and
     6971 then add the envelope output (and, in the update path, the
     release ramp) to that stored word with a plain 16-bit add, and the
     sum is compared unsigned against the halved limit. One unit of
     envelope is therefore 1/2048 octave in the register - twice the
     weight of one unit of table or key term. The add does not saturate;
     over the held bank the halved word never falls below 12561 and no
     envelope target reaches -9101 from there, so it never wraps. */
  combined = (uint16_t)((uint16_t)registers->base_value +
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

/* The registers approach their targets; they are not latched onto them.
 *
 * `07_synthesis/tvf.md` records the TVF-F interpolation word `0x4100`.
 * Read as the fraction of the remaining gap closed per period,
 * `0x4100/65536` is 0.254 - a time constant near 27 ms, the window the
 * hardware's onset centroid moves in. */
static uint32_t sc88_tvf_approach(uint32_t current, uint32_t target,
                                  uint16_t interpolation,
                                  unsigned periods)
{
  unsigned i;
  for (i = 0; i < periods && current != target; ++i) {
    int64_t gap = (int64_t)target - current;
    int64_t step = gap * interpolation / 65536;
    if (step == 0)
      step = gap > 0 ? 1 : -1;
    current = (uint32_t)((int64_t)current + step);
  }
  return current;
}

void sc88_tvf_advance_registers(struct sc88_tvf_registers *registers,
                                unsigned periods)
{
  if (!registers || periods == 0)
    return;
  if (periods > 64)
    periods = 64;
  registers->frequency_current = sc88_tvf_approach(
    registers->frequency_current, registers->frequency_target,
    registers->frequency_interpolation, periods);
  /* TVF-Q is not approached: its target is the same value as its current
     with two more fraction bits (see the damping note in the audio path). */
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
  double g;
  double damping;
  double denominator;
  double high;
  double band;
  double low;

  if (!state || !registers)
    return input;
  /* A negative mode byte installs the fixed tuple: zero cutoff, type word
     0x0800. `07_synthesis/tvf.md` reads it as bypass-like, and a filter
     with no cutoff has nothing to do, so the signal passes untouched. */
  if (registers->fixed_tuple)
    return input;
  if (period_fraction < 0.0)
    period_fraction = 0.0;
  else if (period_fraction > 1.0)
    period_fraction = 1.0;
  word = registers->frequency_current + period_fraction *
    ((double)registers->frequency_target - registers->frequency_current);
  {
    double rate = user ? *(const double *)user : SC88_TVF_NATIVE_RATE;
    double cutoff = sc88_tvf_word_to_hz((uint32_t)(word + 0.5));
    double nyquist = rate * 0.5;
    if (cutoff > nyquist * 0.99)
      cutoff = nyquist * 0.99;
    g = tan(3.14159265358979323846 * cutoff / rate);
  }
  /* Damping from the register the ROM supplies. `07_synthesis/tvf.md`:
   * the companion table at 0x78902 is exactly index * 512, expanded left
   * 2 into TVF-Q current and left 4 into TVF-Q target, and the target is
   * shifted right two bits before use - the target carries two extra
   * fraction bits, so current and target name the same value and Q does
   * not move after note-on. Approaching current toward target as if they
   * shared units ramped Q fourfold over 220 ms and opened every note with
   * a +6 dB peak at the cutoff, which set the drum path 200 Hz too bright
   * (`M-105`). The fixed-tuple path's TVF-Q current 0x20000 is what index
   * 64 produces, so 64 is the neutral resonance; the index runs opposite
   * to the player's setting, so a low index is a high Q. */
  damping = (double)registers->resonance_current / SC88_TVF_Q_UNITY;
  /* The register runs to index 127, which asks for 3.97. The topology is
     stable for any positive damping, so the only bound needed is one that
     keeps the denominator away from zero; refusing the overdamped end
     flattened 13 components onto the neutral value. */
  if (damping < 0.05)
    damping = 0.05;
  else if (damping > 4.0)
    damping = 4.0;
  {
    unsigned section;
    double signal = input;
    for (section = 0; section < SC88_TVF_SECTIONS; ++section) {
      double d = section == 0 ? damping : 2.0;
      double sb = section == 0 ? state->integrator_band
                               : state->section_band[section];
      double sl = section == 0 ? state->integrator_low
                               : state->section_low[section];
      denominator = 1.0 + d * g + g * g;
      high = (signal - (d + g) * sb - sl) / denominator;
      band = g * high + sb;
      low = g * band + sl;
      sb = 2.0 * band - sb;
      sl = 2.0 * low - sl;
      if (section == 0) {
        state->integrator_band = (float)sb;
        state->integrator_low = (float)sl;
      } else {
        state->section_band[section] = (float)sb;
        state->section_low[section] = (float)sl;
      }
      /* Every type code takes the low-pass output. XP bits 10..11 carry a
         type code - 0 for 1193 components, 1 for 12, 2 for 38 - and the
         only name binding on record, LPF/BPF/HPF for 0/1/2, was proposed
         from JV-1080 documentation without SC-88 audio. The SC-88 audio
         refutes it for code 2: the Fiddle, code 2 with a cutoff word of
         8.1 kHz, is recorded on the hardware with its fundamental intact
         and a low-pass roll-off above 8 kHz, while the high-pass output
         here removed everything below the cutoff and left a hiss 100 dB
         above the hardware's balance (`11_validation/measurements.md`
         M-100). Code 1 has no hardware note to test against. */
      signal = low;
    }
    return (float)signal;
  }
}
