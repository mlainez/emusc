/* SPDX-License-Identifier: CC0-1.0 */
#include "tva.h"

#include "devices/sc88.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace EmuSC { namespace Xp {

namespace {

uint16_t be16(const uint8_t *p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

int16_t s16(uint16_t value)
{
  return value <= INT16_MAX
    ? (int16_t)value
    : (int16_t)(-1 - (int32_t)(UINT16_MAX - value));
}

int8_t s8(uint8_t value)
{
  return value <= INT8_MAX
    ? (int8_t)value
    : (int8_t)(-1 - (int16_t)(UINT8_MAX - value));
}

int32_t floor_div_pow2(int32_t value, unsigned shift)
{
  if (value >= 0)
    return value / (INT32_C(1) << shift);
  return -(int32_t)(((uint32_t)(-value) +
    ((UINT32_C(1) << shift) - 1)) >> shift);
}

bool level_word(const struct sc88_rom *rom, uint8_t index, uint16_t *word)
{
  uint32_t offset = kLevelTable + (uint32_t)index * 2;
  if (!rom || !rom->bytes || !word || offset + 2 > rom->size)
    return false;
  *word = be16(rom->bytes + offset);
  return true;
}

bool component_attenuation(const struct sc88_rom *rom, const struct sc88_tone *tone,
                            const struct sc88_component *component,
                            const struct sc88_zone_selection *zone,
                            uint8_t selectorKey, uint8_t velocity,
                            uint16_t *attenuation)
{
  if (!rom || !rom->bytes || !tone || !tone->common || !component ||
      !component->bytes || !zone || !attenuation || selectorKey > 127 ||
      velocity > 127)
    return false;
  const uint8_t *bytes = component->bytes;
  uint32_t page = (uint32_t)tone->common[0x21] << 16;
  uint32_t keyCurve = page | be16(bytes + 0x68);
  uint32_t velocityCurve = page | be16(bytes + 0x64);
  if (keyCurve + selectorKey >= rom->size)
    return false;

  int32_t first = floor_div_pow2(
    s8(rom->bytes[keyCurve + selectorKey]) * s16(be16(bytes + 0x6a)), 8);
  int32_t second = floor_div_pow2(first * INT32_C(0x2437), 16);
  int16_t keyAdjustment = s16((uint16_t)((uint16_t)second << 2));

  uint8_t velocityIndex = (uint8_t)(((uint32_t)(uint8_t)(
    velocity - bytes[0x6c]) * be16(bytes + 0x70)) >> 8);
  /* Clamp to the curve's own 128 entries. The curves at `0x2d150` are laid
     end to end - that one is an identity ramp 0..127 and a different,
     convex curve begins at index 128 - so an index past the end reads a
     *different curve*, not more of this one. French Horns scales velocity
     by 325/256, which sends velocity 127 to index 161: unclamped that
     lands 33 bytes into the next curve and the patch reads 104, 116, 102
     for velocities 18, 64, 127, louder at half velocity than at full.
     Rendered, its level did not move at all across the range. Clamped it
     reads 104, 116, 125 (`M-017`). */
  if (velocityIndex > 127)
    velocityIndex = 127;
  if (velocityCurve + velocityIndex >= rom->size)
    return false;
  uint8_t velocityResult = (uint8_t)(
    ((uint32_t)rom->bytes[velocityCurve + velocityIndex] *
      be16(bytes + 0x72) >> 8) + bytes[0x6e]);
  uint16_t velocityWord;
  if (!level_word(rom, velocityResult & 0x7f, &velocityWord))
    return false;

  uint32_t sum = (uint32_t)be16(tone->common + 0x0c) + zone->static_attenuation;
  if (sum > UINT16_MAX) {
    *attenuation = UINT16_MAX;
    return true;
  }
  uint16_t adjustedComponent = be16(bytes + 0x66);
  if (keyAdjustment < 0) {
    uint32_t expanded = (uint32_t)adjustedComponent +
      (uint16_t)(0u - (uint16_t)keyAdjustment);
    adjustedComponent = expanded > UINT16_MAX
      ? UINT16_MAX : (uint16_t)expanded;
  } else {
    adjustedComponent = adjustedComponent < (uint16_t)keyAdjustment
      ? 0 : (uint16_t)(adjustedComponent - (uint16_t)keyAdjustment);
  }
  sum += adjustedComponent;
  if (sum > UINT16_MAX) {
    *attenuation = UINT16_MAX;
    return true;
  }
  sum += velocityWord;
  *attenuation = sum > UINT16_MAX ? UINT16_MAX : (uint16_t)sum;
  return true;
}

/* A stage's stored word is an attenuation, so what the gain tables convert is
 * the headroom **remaining** after it - exactly what the static path does
 * with `tva_gain_from_headroom_q17`. Converting the stored word itself
 * inverts every envelope in the ROM: it made a piano swell from silence over
 * fourteen seconds and left every sustaining patch at -87 dB. */
bool envelope_target_q17(const struct sc88_rom *rom, uint16_t attenuation,
                          uint32_t *gainQ17)
{
  uint16_t level = (uint16_t)(UINT16_MAX - attenuation);
  if (!rom || !rom->bytes || !gainQ17 ||
      kFineGainTable + (uint32_t)(level & 0xff) * 2 + 2 > rom->size)
    return false;
  uint16_t coarse = be16(rom->bytes + kCoarseGainTable + (uint32_t)(level >> 8) * 2);
  uint16_t fine = be16(rom->bytes + kFineGainTable + (uint32_t)(level & 0xff) * 2);
  uint16_t gainQ16 = (uint16_t)(((uint32_t)coarse * fine) >> 16);
  *gainQ17 = (uint32_t)gainQ16 << 1;
  return true;
}

bool key_rate_scale(const struct sc88_rom *rom, const struct sc88_tone *tone,
                     const struct sc88_component *component,
                     uint8_t selectorKey, uint16_t pointerAt,
                     uint8_t factorAt, uint16_t *scale)
{
  if (!rom || !rom->bytes || !tone || !tone->common || !component ||
      !component->bytes || !scale)
    return false;
  uint32_t curve = ((uint32_t)tone->common[0x21] << 16) |
    be16(component->bytes + pointerAt);
  if (curve + selectorKey >= rom->size)
    return false;
  int keyValue = s8(rom->bytes[curve + selectorKey]);
  int factor = s8((uint8_t)(0u - component->bytes[factorAt]));
  int index = floor_div_pow2(keyValue * factor, 8) + 64;
  if (index < 0 || index > 128)
    return false;
  *scale = be16(rom->bytes + kXpRateScaleTable + (uint32_t)index * 2);
  return true;
}

bool velocity_rate_scale(const struct sc88_rom *rom, uint8_t velocity,
                          int factor, uint16_t *scale)
{
  if (!rom || !rom->bytes || !scale || velocity > 127 ||
      factor < -128 || factor > 127)
    return false;
  int index = floor_div_pow2((2 * ((int)velocity - 64)) * factor, 8) + 64;
  if (index < 0 || index > 128)
    return false;
  *scale = be16(rom->bytes + kXpRateScaleTable + (uint32_t)index * 2);
  return true;
}

uint16_t curve_pack(uint16_t curveEntry, uint16_t scale)
{
  uint32_t product = (uint32_t)(curveEntry & 0x0fff) * scale;
  uint16_t exponent = (uint16_t)(curveEntry & 0xf000);
  exponent = (uint16_t)(exponent << 8) | (uint16_t)(exponent >> 8);
  exponent = (uint16_t)(exponent << 2);
  uint8_t exponentByte = (uint8_t)exponent;
  uint16_t mantissa;
  if ((product >> 16) != 0) {
    if (exponentByte != 0) {
      for (;;) {
        product >>= 2;
        exponentByte = (uint8_t)(exponentByte - 0x40);
        exponent = (uint16_t)((exponent & 0xff00) | exponentByte);
        if (exponentByte == 0) {
          product >>= 1;
          break;
        }
        if ((product >> 16) == 0)
          break;
      }
    }
    mantissa = exponentByte == 0 && (product >> 16) >= 16
      ? 0x0fff : (uint16_t)(product >> 8);
  } else {
    while (exponentByte != 0xc0 && (product >> 16) == 0 &&
           (uint16_t)product < 0x2000) {
      product <<= 2;
      if (exponentByte == 0)
        product <<= 1;
      exponentByte = (uint8_t)(exponentByte + 0x40);
      exponent = (uint16_t)((exponent & 0xff00) | exponentByte);
    }
    mantissa = (uint16_t)(product >> 8);
  }
  exponent >>= 2;
  exponent = (uint16_t)(exponent << 8) | (uint16_t)(exponent >> 8);
  return (uint16_t)(exponent | mantissa);
}

/* The component's own rate index shifted by the part's modifier for that
   stage pair: `clamp(index + 2 * (part + secondary - 128), 0, 127)`. */
uint8_t adjusted_rate_index(uint8_t index, const struct sc88_tva_controls *controls,
                            unsigned stage)
{
  if (!controls)
    return index;
  int part;
  int secondary;
  if (stage < 2) {
    part = controls->part_attack;
    secondary = controls->secondary_attack;
  } else {
    part = controls->part_decay;
    secondary = controls->secondary_decay;
  }
  int adjusted = (int)index + 2 * (part + secondary - 128);
  if (adjusted < 0)
    adjusted = 0;
  else if (adjusted > 127)
    adjusted = 127;
  return (uint8_t)adjusted;
}

/* Where the chip's amplitude register stands `periods` into the stage.
   `76b3..7705` converts the stage's target attenuation through the coarse
   and fine gain tables and stores the GAIN at `3d5a`; `71a9` hands that to
   the chip with the stage's interpolation word beside it. So the approach
   is in the gain domain, and its shape is the word's, not the counter's.

   This replaces a ramp that was linear in the attenuation word and
   therefore exactly as long as the dwell (`M-016`, `M-149`), together with
   the separate gain-domain case that a stage rising out of digital silence
   needed: an exponential stage covers most of its gap early whichever
   direction it moves, so neither special case survives. */
uint32_t stage_point(const struct sc88_tva_envelope *envelope, unsigned stage,
                      double periods)
{
  double progress = tva_curve_progress(envelope->curves + stage, periods);
  double start = (double)envelope->start_q17;
  double value = start +
    progress * ((double)envelope->targets_q17[stage] - start);
  if (value <= 0.0)
    return 0;
  return (uint32_t)(value + 0.5);
}

}  // namespace

void tva_curve_decode(uint16_t word, struct sc88_tva_curve *curve)
{
  /* The exponent's shifts, `0, 3, 5, 7`, read out of `78af..78bf`: stepping
     the exponent byte down by `0x40` shifts the product right by two, and
     the last step from `e = 1` to `e = 0` shifts by one more. */
  static constexpr uint16_t shift[4] = {0, 3, 5, 7};
  if (!curve)
    return;
  curve->linear = (word & 0x4000) != 0;
  curve->rate = (double)(word & 0x0fff) /
    (double)(1u << shift[(word >> 12) & 3]) / 64.0;
}

double tva_curve_progress(const struct sc88_tva_curve *curve, double periods)
{
  if (!curve || periods <= 0.0 || curve->rate <= 0.0)
    return 0.0;
  double q = curve->rate * periods;
  if (curve->linear)
    return q >= 1.0 ? 1.0 : q;
  /* An exponential stage is 10.95 time constants long, so `q` runs to
     about 11 and the gap left when the counter carries is 2e-5. */
  return q >= 40.0 ? 1.0 : 1.0 - std::exp(-q);
}

bool tva_static_gain_q17(const struct sc88_rom *rom, const struct sc88_tone *tone,
                          const struct sc88_component *component,
                          const struct sc88_zone_selection *zone,
                          uint8_t selectorKey, uint8_t velocity,
                          const struct sc88_tva_levels *levels,
                          uint8_t drumLevel, uint16_t *staticAttenuation,
                          uint32_t *gainQ17)
{
  uint16_t componentAttenuation;
  if (!levels || !staticAttenuation || !gainQ17 ||
      !component_attenuation(rom, tone, component, zone, selectorKey,
                              velocity, &componentAttenuation))
    return false;
  *staticAttenuation = componentAttenuation;
  return tva_gain_from_headroom_q17(rom, UINT16_MAX, levels, drumLevel,
                                     componentAttenuation, gainQ17);
}

/* `compose_voice_amplitude` subtracts five level words from the headroom
 * before the component's own static attenuation at `72b2`: master,
 * secondary, part, expression, and - on a rhythm note whose gate `7295`
 * finds set - the kit's per-note level read at `72a3` and subtracted at
 * `72ab` through the same table at `0x14f3e` as the other four. The fifth
 * is therefore in this loop and not a multiply on the way in: the table is
 * a log-domain attenuation, and applying the level as a linear ratio
 * delivered almost exactly half the attenuation in dB. */
bool tva_gain_from_headroom_q17(const struct sc88_rom *rom, uint16_t headroom,
                                 const struct sc88_tva_levels *levels,
                                 uint8_t drumLevel, uint16_t staticAttenuation,
                                 uint32_t *gainQ17)
{
  if (!rom || !rom->bytes || !levels || !gainQ17)
    return false;
  uint8_t sources[5];
  unsigned sourceCount = 4;
  sources[0] = levels->master;
  sources[1] = levels->secondary;
  sources[2] = levels->part;
  sources[3] = levels->expression;
  if (drumLevel <= 127) {
    sources[4] = drumLevel;
    sourceCount = 5;
  } else if (drumLevel != SC88_TVA_NO_DRUM_LEVEL) {
    return false;
  }
  uint16_t remaining = headroom;
  for (unsigned i = 0; i < sourceCount; ++i) {
    uint16_t reduction;
    if (sources[i] > 127 || !level_word(rom, sources[i], &reduction))
      return false;
    if (remaining <= reduction) {
      *gainQ17 = 0;
      return true;
    }
    remaining = (uint16_t)(remaining - reduction);
  }
  remaining = remaining <= staticAttenuation
    ? 1 : (uint16_t)(remaining - staticAttenuation);
  if (kFineGainTable + (uint32_t)(remaining & 0xff) * 2 + 2 > rom->size)
    return false;
  uint16_t coarse = be16(rom->bytes + kCoarseGainTable + (uint32_t)(remaining >> 8) * 2);
  uint16_t fine = be16(rom->bytes + kFineGainTable + (uint32_t)(remaining & 0xff) * 2);
  uint16_t gainQ15 = (uint16_t)(((uint32_t)coarse * fine) >> 17);
  *gainQ17 = (uint32_t)gainQ15 << 2;
  return true;
}

bool tva_release_prepare(const struct sc88_rom *rom, const struct sc88_tone *tone,
                          const struct sc88_component *component,
                          uint8_t selectorKey, struct sc88_tva_release *release)
{
  if (!rom || !rom->bytes || !tone || !tone->common || !component ||
      !component->bytes || !release || selectorKey > 127)
    return false;
  uint32_t page = (uint32_t)tone->common[0x21] << 16;
  uint32_t keyCurve = page | be16(component->bytes + 0x8c);
  if (keyCurve + selectorKey >= rom->size ||
      kXpRateScaleTable + 129u * 2 > rom->size ||
      kXpEnvelopeRateTable + 128u * 2 > rom->size)
    return false;
  int keyValue = s8(rom->bytes[keyCurve + selectorKey]);
  int factor = s8((uint8_t)(0u - component->bytes[0x8f]));
  int productHigh = floor_div_pow2(keyValue * factor, 8);
  unsigned scaleIndex = (unsigned)(productHigh + 64);
  if (scaleIndex > 128)
    return false;
  uint16_t scale = be16(rom->bytes + kXpRateScaleTable + scaleIndex * 2);
  uint16_t rate = be16(rom->bytes + kXpEnvelopeRateTable +
                       (uint32_t)component->bytes[0x84] * 2);
  if (rate < 16)
    rate = UINT16_MAX;
  uint32_t product = (uint32_t)rate * scale;
  release->current = UINT16_MAX;
  release->increment = product >= UINT32_C(0x01000000)
    ? UINT16_MAX : (uint16_t)(product >> 8);
  release->scale = UINT16_MAX;
  release->scale_enabled = false;
  release->active = false;
  return true;
}

bool tva_release_set_pedal(const struct sc88_rom *rom, uint8_t hold1,
                            bool continuousHold, bool keepScaleAtZero,
                            bool sostenutoRetained,
                            struct sc88_tva_release *release)
{
  if (!rom || !rom->bytes || !release || hold1 > 127)
    return false;
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

bool tva_release_advance(struct sc88_tva_release *release, unsigned elapsedPeriods)
{
  if (!release || !release->active || elapsedPeriods == 0)
    return false;
  uint16_t step = release->scale_enabled
    ? (uint16_t)(((uint32_t)release->increment * release->scale) >> 16)
    : release->increment;
  uint8_t periods = (uint8_t)elapsedPeriods;
  uint32_t product = (uint32_t)step * periods;
  if ((product >> 16) != 0 || release->current <= (uint16_t)product) {
    release->current = 0;
    release->active = false;
  } else {
    release->current = (uint16_t)(release->current - (uint16_t)product);
  }
  return true;
}

bool tva_envelope_prepare(const struct sc88_rom *rom, const struct sc88_tone *tone,
                           const struct sc88_component *component,
                           uint8_t selectorKey, uint8_t velocity,
                           const struct sc88_tva_controls *controls,
                           struct sc88_tva_envelope *envelope)
{
  uint16_t keyScale;
  if (!rom || !rom->bytes || !tone || !component || !component->bytes ||
      !envelope || selectorKey > 127 || velocity > 127 ||
      kXpRateScaleTable + 129u * 2 > rom->size ||
      kXpEnvelopeRateTable + 128u * 2 > rom->size ||
      !key_rate_scale(rom, tone, component, selectorKey, 0x8a, 0x8e, &keyScale))
    return false;
  for (unsigned stage = 0; stage < 4; ++stage) {
    uint8_t rateIndex = adjusted_rate_index(
      component->bytes[0x80 + stage], controls, stage);
    int factor = s8(component->bytes[stage < 2 ? 0x90 : 0x91]);
    envelope->target_attenuations[stage] =
      be16(component->bytes + 0x78 + stage * 2);
    uint16_t velocityScale;
    if (!envelope_target_q17(rom, envelope->target_attenuations[stage],
                              envelope->targets_q17 + stage) ||
        !velocity_rate_scale(rom, velocity, factor, &velocityScale))
      return false;
    uint16_t finalScale = (uint16_t)(((uint32_t)keyScale * velocityScale) >> 8);
    uint32_t curveTable = component->bytes[0x85 + stage] == 0
      ? kAmpCurve0Table : kAmpCurve1Table;
    if (curveTable + (uint32_t)rateIndex * 2 + 2 > rom->size)
      return false;
    uint16_t curveEntry = be16(rom->bytes + curveTable + (uint32_t)rateIndex * 2);
    if (component->bytes[0x85 + stage] != 0)
      curveEntry |= 0x4000;
    envelope->curve_words[stage] = curve_pack(curveEntry, finalScale);
    tva_curve_decode(envelope->curve_words[stage], envelope->curves + stage);
    uint16_t rate = be16(rom->bytes + kXpEnvelopeRateTable + (uint32_t)rateIndex * 2);
    if (std::getenv("SC88_TRACE_TVA"))
      std::fprintf(stderr, "  stage %u: rate_index %3u rate %5u key_scale %5u "
              "vel_scale %5u final_scale %5u\n",
              stage, rateIndex, rate, keyScale, velocityScale, finalScale);
    if (rate < 16)
      rate = UINT16_MAX;
    uint32_t product = (uint32_t)rate * finalScale;
    if (product >= UINT32_C(0x01000000)) {
      envelope->initial_phases[stage] = UINT16_MAX;
      envelope->increments[stage] = UINT16_MAX;
    } else {
      envelope->initial_phases[stage] = 0;
      envelope->increments[stage] = (uint16_t)(product >> 8);
    }
  }
  /* "If the adjusted stage-0 rate is nonpositive, preparation advances
     directly to stage 1" - adjusted, so a part modifier can move it. */
  envelope->stage =
    adjusted_rate_index(component->bytes[0x80], controls, 0) == 0 ? 1 : 0;
  envelope->saved_count = 0;
  envelope->stage_periods = 0.0;
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

uint32_t tva_envelope_linear_q17(const struct sc88_rom *rom,
  const struct sc88_tva_envelope *envelope, double periodFraction)
{
  if (!envelope)
    return 0;
  if (!envelope->active || envelope->stage >= 4)
    return envelope->current_q17;
  if (periodFraction < 0.0)
    periodFraction = 0.0;
  else if (periodFraction > 1.0)
    periodFraction = 1.0;
  (void)rom;
  return stage_point(envelope, envelope->stage,
                      envelope->stage_periods + periodFraction);
}

bool tva_envelope_advance(const struct sc88_rom *rom,
                           struct sc88_tva_envelope *envelope,
                           unsigned elapsedPeriods)
{
  if (!envelope || !envelope->active || envelope->stage >= 4 ||
      elapsedPeriods == 0)
    return false;
  (void)rom;
  uint8_t catchup = (uint8_t)(elapsedPeriods - 1);
  uint16_t remaining = (uint16_t)(envelope->saved_count +
    (catchup <= 127 ? (int)catchup : (int)catchup - 256));
  uint16_t working = envelope->phase;
  uint16_t increment = envelope->increments[envelope->stage];
  double steps = 0.0;
  for (;;) {
    uint16_t next = (uint16_t)(working + increment);
    if (next < working) {
      envelope->saved_count = (uint8_t)remaining;
      envelope->current_q17 = envelope->targets_q17[envelope->stage];
      envelope->start_q17 = envelope->current_q17;
      envelope->start_attenuation =
        envelope->target_attenuations[envelope->stage];
      ++envelope->stage;
      envelope->stage_periods = 0.0;
      envelope->phase = envelope->stage < 4
        ? envelope->initial_phases[envelope->stage] : 0;
      if (envelope->stage == 4)
        envelope->active = false;
      return true;
    }
    working = next;
    steps += 1.0;
    --remaining;
    if (remaining == UINT16_MAX)
      break;
  }
  envelope->phase = working;
  envelope->saved_count = 0;
  envelope->stage_periods += steps;
  envelope->current_q17 = stage_point(envelope, envelope->stage,
                                       envelope->stage_periods);
  return true;
}

void tva_envelope_freeze(const struct sc88_rom *rom,
                          struct sc88_tva_envelope *envelope,
                          double periodFraction)
{
  if (!envelope)
    return;
  envelope->current_q17 = tva_envelope_linear_q17(rom, envelope, periodFraction);
  envelope->active = false;
}

}}  // namespace EmuSC::Xp
