/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_tvf.h"

#include "sc88_tva.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#define SC88_TVF_BASE_TABLE 0x78702u
/* [FW-EXACT] TVF-Q current is the companion word << 2, i.e.
   resonance_index << 11, and one unit of damping is 131072 - that is,
   q = resonance_index / 64.

   The scaling is fixed by the chip's own limit table; see the note on
   SC88_TVF_LIMIT_TABLE. Read with q = index/64 that table is exactly
   f*f + f*q = 2 on all 128 entries; read with index/32 it spans
   2.000..3.458 and with index/128 1.270..2.000. The value libEmuSC's
   SC-55 path uses (svf.cc's set_resonance, q = resonance/64, which the
   SC-55's own two stability tables fix to a rounding unit in P-0130 and
   P-0131) is the value the SC-88's ROM asks for too. */
#define SC88_TVF_Q_UNITY 131072.0
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
   frequency, as the pitch word is a rate. The coefficient the chip
   derives from it is now recovered as well: see SC88_TVF_LIMIT_TABLE. */
#define SC88_TVF_OCTAVE_UNITS 16384.0
#define SC88_TVF_NYQUIST_WORD 0x40000
/* [FW-EXACT] The limit table is the filter's own topology, written down.
 *
 * 0x78802 holds one cutoff ceiling per resonance index. Read in the base
 * table's domain - f = 2*sin(pi*fc/32000), the Chamberlin coefficient the
 * base table already is - and with q = resonance_index/64, every one of
 * its 128 entries satisfies
 *
 *     f*f + f*q = 2
 *
 * to a rounding unit: the residual against the exact law is -0.53 +- 0.29
 * table units and lies in [-0.99, 0], which is the signature of a floor(),
 * not of a fit. In octaves that is 0.33 cent of cutoff.
 *
 * That expression is not a general fact about two-pole filters. It is the
 * trace of the state matrix of the forward-Euler state-variable filter
 *
 *     lp[n] = lp[n-1] + f*bp[n-1]
 *     hp[n] = x[n] - lp[n] - q*bp[n-1]
 *     bp[n] = bp[n-1] + f*hp[n]
 *
 * whose trace is 2 - f*f - f*q and whose determinant is 1 - f*q. Setting
 * the trace to zero puts the pole pair at exactly +-90 degrees: the chip
 * refuses to let the filter's realised resonance climb past fs/4, and the
 * ceiling it writes for each resonance is the cutoff at which that
 * happens. A trapezoidal (bilinear) realisation is stable for every
 * positive coefficient and has no such ceiling to tabulate; its own
 * coefficient g = tan(pi*fc/fs) puts g*g + g*q at 0.906..1.000 over the
 * same table, and reading the word as a frequency instead of a sine puts
 * it at 3.211..3.414. The topology is forward-Euler.
 *
 * This is the SC-55's filter. libEmuSC's svf.cc is the same three lines,
 * and the SC-55's control ROM carries the same law twice: TVFResonance[]
 * is 128*(sqrt(q*q+4) - q) and TVFResonanceFreq[] is 32*(4-f*f)/f, the
 * two sides of that state matrix's stability edge, with q = r/64 and
 * f = x/128 (P-0130, P-0131). The SC-88 tabulates the same topology at a
 * tighter bound.
 *
 * Two things follow that the audio path could not have been told any
 * other way. The coefficient is the ROM's own word and is not warped
 * again; and a tone whose computed cutoff is clamped to this ceiling is
 * a tone the chip has been asked to leave alone - at q = 1 the law gives
 * f = 1 exactly, where the difference equations above collapse to
 * y[n] = x[n-1], a bare sample of delay. "Wide open" is realised as
 * transparent, which is what asking for a cutoff above the ceiling
 * should mean. */
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

/* The word is the log of sin(pi * f / fs), not the log of f.

   Both machines hold the same cutoff table and the SC-55 holds it in the
   clear. The SC-55 mk1 CPU ROM at 0x7612 is 32768 * sin(pi * f / 32000)
   for f = 440 * 2^((index - 64)/12): fitted over its 116 pre-saturation
   entries the residual is 0.435 LSB rms and the anchor lands on 440.0 Hz
   at index 64 - one semitone per index, saturating where the sine folds.

   The SC-88's base table at 0x78702 is that same quantity in the XP pitch
   register's log domain: word = 0x40000 + 16384 * log2(sin(pi * f /
   32000)) reproduces indices 0..126 to an sd of 5.7 word units, which is
   0.00035 octave. Reading the word as a log FREQUENCY leaves sd 2072 word
   units and up to 0.67 octave - 363 times worse - because it has no
   account of why the table's steps shrink from 1368 to 0 over its last
   twelve entries. That compression is the sine approaching one; the
   frequency underneath it is a clean note table.

   Measured on the archive recordings against our own --no-filter render,
   so that our filter is not in the measurement: the hardware's own corner
   is 0.67x the computed cutoff under the old reading and 1.03x under this
   one, and the hardware's fitted order goes from 2.5 poles to 1.9
   (filter_slope.py --cutoff, 9 instruments with r2 >= 0.5).

   0x40000 still means the top of the range and still means fs/2; what
   runs exponentially between is sin(pi * f / fs). Below about 4 kHz the
   two readings differ by exactly pi/2 - 0.651 octave - converging at the
   top. */
double sc88_tvf_word_to_hz(uint32_t word)
{
  double sine = exp2(((double)word - SC88_TVF_NYQUIST_WORD) /
                     SC88_TVF_OCTAVE_UNITS);

  if (sine >= 1.0)
    return 0.5 * SC88_TVF_NATIVE_RATE;
  return (SC88_TVF_NATIVE_RATE / 3.14159265358979323846) * asin(sine);
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

/* [FW-EXACT] The registers approach their targets; they are not latched
 * onto them, and the word beside the target says how.
 *
 * One encoding serves every register the chip approaches. `67a8` writes
 * the TVF-F target at scratch `1a7c+24/+26`, the current value at
 * `+2c/+2e`, and the interpolation pair at `+28/+2a`: `683b` puts zero in
 * the high word and `683f: ea 2a 07 41 00` puts `#0x4100` in the low one.
 * The update path at `6918`/`691c` and the fixed tuple at `67dc`/`67e0`
 * write the same constant. It is the word the pitch register gets at
 * `+1c/+1e` as well, and `sc88_tva_curve_decode` is where the packing is
 * read - `0x4100` is bit 14 set, exponent 0, mantissa 0x100, so the
 * LINEAR family at value 256 and `q = 4 * periods`.
 *
 * Linear covers `min(1, q)` of the gap, so TVF-F arrives a quarter of the
 * way into a control period - 2.0 ms. That is a de-click on a register the
 * CPU rewrites every period, the same office `0x2a7` holds for the static
 * amplitude at 0.75 ms, not a glide with a time constant of its own. */
static double sc88_tvf_frequency_progress(
  const struct sc88_tvf_registers *registers, double periods)
{
  struct sc88_tva_curve curve;
  sc88_tva_curve_decode(registers->frequency_interpolation, &curve);
  return sc88_tva_curve_progress(&curve, periods);
}

void sc88_tvf_advance_registers(struct sc88_tvf_registers *registers,
                                unsigned periods)
{
  double value;
  if (!registers || periods == 0)
    return;
  value = (double)registers->frequency_current +
    sc88_tvf_frequency_progress(registers, (double)periods) *
    ((double)registers->frequency_target - registers->frequency_current);
  registers->frequency_current = value <= 0.0 ? 0u : (uint32_t)(value + 0.5);
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
  double high;
  double band;
  double low;

  if (!state || !registers)
    return input;
  /* A negative mode byte installs the fixed tuple: zero cutoff, type word
     0x0800 - type code 2, the high-pass. A high-pass at zero cutoff
     passes everything, so the tuple is a bypass by its own terms; see the
     type-code note in the filter below. */
  if (registers->fixed_tuple)
    return input;
  if (period_fraction < 0.0)
    period_fraction = 0.0;
  else if (period_fraction > 1.0)
    period_fraction = 1.0;
  /* Where the register stands this far into the period, by its own
     interpolation word rather than by a ramp spread over the whole of it. */
  word = (double)registers->frequency_current +
    sc88_tvf_frequency_progress(registers, period_fraction) *
    ((double)registers->frequency_target - registers->frequency_current);
  {
    /* The coefficient the ROM's word already is. At the chip's own rate
       this is 2 * exp2((word - 0x40000)/16384) exactly - the sine is
       taken out of the word by sc88_tvf_word_to_hz and put straight back
       - and at any other host rate it is the same analog corner retuned
       to that rate, which is the only part of this that is a choice. */
    double rate = user ? *(const double *)user : SC88_TVF_NATIVE_RATE;
    double cutoff = sc88_tvf_word_to_hz((uint32_t)(word + 0.5));
    double nyquist = rate * 0.5;
    if (cutoff > nyquist * 0.99)
      cutoff = nyquist * 0.99;
    g = 2.0 * sin(3.14159265358979323846 * cutoff / rate);
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
  /* The two poles are realised with forward-Euler integrators, the
     topology the chip's limit table names (see SC88_TVF_LIMIT_TABLE) and
     the one libEmuSC's SC-55 path already runs in svf.cc. The trapezoidal
     form this replaced is a bilinear transform: it leaves a double zero
     at Nyquist, so the two poles the ROM asks for rolled off like three
     near the top of the band - 0.5 dB darker than its own analog
     prototype at twice the corner for a tone cut off at 1.6 kHz, 3.0 dB
     at 3.7 kHz and 7.5 dB at 5.3 kHz. Forward Euler has no zero but the
     one sample of delay, and its error runs the other way. */
  {
    unsigned section;
    double signal = input;
    for (section = 0; section < SC88_TVF_SECTIONS; ++section) {
      double d = section == 0 ? damping : 2.0;
      double f = g;
      double sb = section == 0 ? state->integrator_band
                               : state->section_band[section];
      double sl = section == 0 ? state->integrator_low
                               : state->section_low[section];
      /* The chip's own limit table keeps f*f + f*d at or below 2, which is
         well inside this bound; the bound is here because the damping floor
         above and a host sample rate other than the chip's are not the ROM's
         doing and must not be able to put a pole outside the unit circle. */
      double bound = 0.99 * (sqrt(d * d + 4.0) - d);
      if (f > bound)
        f = bound;
      low = sl + f * sb;
      high = signal - low - d * sb;
      band = sb + f * high;
      if (section == 0) {
        state->integrator_band = (float)band;
        state->integrator_low = (float)low;
      } else {
        state->section_band[section] = (float)band;
        state->section_low[section] = (float)low;
      }
      /* XP bits 10..11 carry a type code - 0 on 1193 components, 1 on 12,
         2 on 38 - and the name binding proposed from JV-1080
         documentation is LPF/BPF/HPF for 0/1/2. Code 2 takes the
         high-pass output, which the SC-88's own archive recordings show
         directly.

         Breath Noise is the case that cannot be read two ways: its one
         component is code 2 with a static 2.7 kHz cutoff and no envelope,
         and it is broadband, so the filter's shape is the tone. Against
         the archive recording at C4, level-matched over 17 log bands, the
         high-pass output lands within 1.1 dB from 256 Hz to 7.2 kHz. The
         same render with the filter removed is 36 dB too loud at 256 Hz
         and 22 dB at 590 Hz: the hardware has less low end than the raw
         sample, which neither a low-pass nor a bypass can produce, and
         the low-pass output is 20 dB short at 8.9 kHz on top of that.
         Seashore, also code 2, moves from 20.0 to 1.6 dB median band
         error at the same key. In the 63-note set the Fiddle goes from
         19.19 to 1.75 dB and Halo Pad from 8.34 to 2.24.

         The earlier reading - every code takes the low-pass output -
         rested on M-100, which put the Fiddle's cutoff at 8.1 kHz, where
         a high-pass would indeed have removed its fundamental. The
         firmware-exact cutoff law puts that same component at 439 Hz at
         C5, where a two-pole high-pass leaves the 523 Hz fundamental at
         +1.0 dB and everything above it flat. M-100's measurement stands;
         what it refutes does not survive the law it was read under.

         This also settles the negative-mode tuple, which installs type
         code 2 with a cutoff word of zero: a high-pass at zero passes
         everything, so `fixed_tuple` returning the input untouched is the
         type code's own behaviour rather than an assumption about it.

         Code 1 keeps the low-pass output. No component in any archive
         recording carries it - the only one in the GM bank is Slap Bass
         2, which is not in the single-note set - so band-pass is
         unverified here and is not adopted on the strength of the
         binding alone. */
      signal = ((registers->filter_select >> 10) & 3u) == 2u ? high : low;
    }
    return (float)signal;
  }
}
