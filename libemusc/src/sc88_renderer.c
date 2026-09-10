/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_renderer.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static uint16_t sc88_renderer_be16(const uint8_t *p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static int16_t sc88_renderer_s16(uint16_t value)
{
  return value <= INT16_MAX
    ? (int16_t)value
    : (int16_t)(-1 - (int32_t)(UINT16_MAX - value));
}

static int8_t sc88_renderer_s8(uint8_t value)
{
  return value <= INT8_MAX
    ? (int8_t)value
    : (int8_t)(-1 - (int16_t)(UINT8_MAX - value));
}

static int32_t sc88_renderer_floor_q14(int32_t value)
{
  if (value >= 0)
    return value / 16384;
  return -(int32_t)(((uint32_t)(-value) + 16383u) / 16384u);
}

uint8_t sc88_renderer_selector_key(const struct sc88_component *component,
                                   uint8_t midi_key)
{
  int32_t factor;
  int32_t key;

  if (!component || !component->bytes || midi_key > 127)
    return 0;
  factor = sc88_renderer_s16(sc88_renderer_be16(component->bytes + 0x14));
  key = 60 + sc88_renderer_floor_q14(((int32_t)midi_key - 60) * factor) +
    sc88_renderer_s8(component->bytes[0x16]);
  if (key < 0)
    return 0;
  if (key > 127)
    return 127;
  return (uint8_t)key;
}

static int32_t sc88_renderer_relative_pitch(int difference)
{
  int32_t value = (int32_t)((difference * 16384) / 12);
  return value > 32767 ? 32767 : value;
}

bool sc88_renderer_static_pitch_word(const struct sc88_rom *rom,
                                     const struct sc88_tone *tone,
                                     const struct sc88_component *component,
                                     const struct sc88_wave_descriptor *desc,
                                     uint8_t selector_key,
                                     uint32_t *pitch_word)
{
  uint32_t table_offset;
  int32_t pitch;

  if (!rom || !rom->bytes || !tone || !tone->common || !component ||
      !component->bytes || !desc || !pitch_word || selector_key > 127)
    return false;
  table_offset = ((uint32_t)tone->common[0x21] << 16) |
    sc88_renderer_be16(tone->common + 0x10);
  if (table_offset + (uint32_t)selector_key * 2 + 2 > rom->size)
    return false;
  pitch = 0x38000 +
    sc88_renderer_relative_pitch((int)selector_key - desc->root_key) +
    sc88_wave_pitch_correction(desc, false) +
    sc88_renderer_s16(sc88_renderer_be16(
      rom->bytes + table_offset + (uint32_t)selector_key * 2)) +
    sc88_renderer_s16(sc88_renderer_be16(component->bytes + 0x10));
  if (pitch < 0)
    pitch = 0;
  if (pitch > 0x3ffff)
    pitch = 0x3ffff;
  *pitch_word = (uint32_t)pitch;
  return true;
}

static int sc88_renderer_bank_index(uint8_t selector)
{
  switch (selector) {
  case 0x00: return 0;
  case 0x01: return 1;
  case 0x10: return 2;
  case 0x11: return 3;
  case 0x20: return 4;
  case 0x21: return 5;
  case 0x30: return 6;
  case 0x31: return 7;
  default: return -1;
  }
}

bool sc88_renderer_init(struct sc88_renderer *renderer,
                        const uint8_t *control_rom, size_t control_rom_size,
                        const struct sc88_wave_bank *banks, size_t bank_count,
                        double output_rate, enum sc88_fractional_wrap wrap)
{
  bool occupied[SC88_WAVE_BANK_COUNT] = {false};
  size_t i;

  if (!renderer || !banks || bank_count != SC88_WAVE_BANK_COUNT ||
      output_rate <= 0.0 || wrap < SC88_WRAP_FULL_CARRY ||
      wrap > SC88_WRAP_FRACTION_ONLY)
    return false;
  memset(renderer, 0, sizeof *renderer);
  if (!sc88_rom_init(&renderer->rom, control_rom, control_rom_size))
    return false;
  for (i = 0; i < bank_count; ++i) {
    int index = sc88_renderer_bank_index(banks[i].selector);
    if (index < 0 || occupied[index] || !banks[i].bytes ||
        banks[i].size != SC88_WAVE_BANK_SIZE)
      return false;
    occupied[index] = true;
    renderer->banks[index] = banks[i];
  }
  renderer->output_rate = output_rate;
  renderer->wrap = wrap;
  renderer->levels.master = 127;
  renderer->levels.secondary = 127;
  renderer->levels.part = 127;
  renderer->levels.expression = 127;
  renderer->pan.master = 64;
  renderer->pan.part = 64;
  renderer->tvf_controls.part_cutoff = 64;
  renderer->tvf_controls.secondary_cutoff = 64;
  renderer->tvf_controls.part_resonance = 64;
  renderer->tvf_controls.secondary_resonance = 64;
  return true;
}

void sc88_renderer_set_pan(struct sc88_renderer *renderer,
                           const struct sc88_pan_controls *pan)
{
  if (!renderer || !pan || pan->master < 1 || pan->master > 127 ||
      pan->part > 127)
    return;
  renderer->pan = *pan;
}

void sc88_renderer_set_levels(struct sc88_renderer *renderer,
                              const struct sc88_tva_levels *levels)
{
  if (!renderer || !levels || levels->master > 127 ||
      levels->secondary > 127 || levels->part > 127 ||
      levels->expression > 127)
    return;
  renderer->levels = *levels;
}

void sc88_renderer_set_tvf_audio_transfer(
  struct sc88_renderer *renderer, sc88_tvf_audio_transfer_fn transfer,
  void *user)
{
  if (!renderer)
    return;
  renderer->tvf_audio_transfer = transfer;
  renderer->tvf_audio_user = user;
}

void sc88_renderer_set_tvf_controls(
  struct sc88_renderer *renderer, const struct sc88_tvf_controls *controls)
{
  if (!renderer || !controls || controls->part_cutoff > 127 ||
      controls->secondary_cutoff > 127 || controls->part_resonance > 127 ||
      controls->secondary_resonance > 127)
    return;
  renderer->tvf_controls = *controls;
}

static const struct sc88_wave_bank *sc88_renderer_find_bank(
  const struct sc88_renderer *renderer, uint8_t selector)
{
  int index = sc88_renderer_bank_index(selector);
  return index < 0 ? NULL : renderer->banks + index;
}

void sc88_renderer_voice_destroy(struct sc88_render_voice *voice)
{
  unsigned i;
  if (!voice)
    return;
  for (i = 0; i < SC88_MAX_TONE_COMPONENTS; ++i)
    free(voice->components[i].pcm24);
  memset(voice, 0, sizeof *voice);
}

bool sc88_renderer_note_on(const struct sc88_renderer *renderer,
                           struct sc88_render_voice *voice,
                           uint8_t variation, uint8_t program,
                           uint8_t key, uint8_t velocity,
                           float provisional_gain)
{
  if (!renderer)
    return false;
  return sc88_renderer_note_on_with_levels(
    renderer, voice, variation, program, key, velocity, provisional_gain,
    &renderer->levels);
}

bool sc88_renderer_note_on_with_levels(
  const struct sc88_renderer *renderer, struct sc88_render_voice *voice,
  uint8_t variation, uint8_t program, uint8_t key, uint8_t velocity,
  float provisional_gain, const struct sc88_tva_levels *levels)
{
  if (!renderer)
    return false;
  return sc88_renderer_note_on_with_controls(
    renderer, voice, variation, program, key, velocity, provisional_gain,
    levels, &renderer->pan);
}

bool sc88_renderer_note_on_with_controls(
  const struct sc88_renderer *renderer, struct sc88_render_voice *voice,
  uint8_t variation, uint8_t program, uint8_t key, uint8_t velocity,
  float provisional_gain, const struct sc88_tva_levels *levels,
  const struct sc88_pan_controls *pan)
{
  if (!renderer)
    return false;
  return sc88_renderer_note_on_with_part_controls(
    renderer, voice, variation, program, key, velocity, provisional_gain,
    levels, pan, &renderer->tvf_controls);
}

/* The body both entry points share. A melodic note selects its tone through
 * the variation map and plays it at the MIDI key; a rhythm note's tone and
 * key both come from its kit record, so the two differ only in what they
 * hand in here. */
static bool sc88_renderer_note_on_tone(
  const struct sc88_renderer *renderer, struct sc88_render_voice *voice,
  uint32_t tone_offset, uint8_t key, uint8_t velocity,
  float provisional_gain, const struct sc88_tva_levels *levels,
  const struct sc88_pan_controls *pan,
  const struct sc88_tvf_controls *tvf_controls)
{
  struct sc88_tone tone;
  unsigned i;

  if (!renderer || !voice || !levels || !pan || !tvf_controls ||
      levels->master > 127 ||
      levels->secondary > 127 || levels->part > 127 ||
      levels->expression > 127 || key > 127 || velocity > 127 ||
      provisional_gain < 0.0f ||
      !sc88_rom_open_tone(&renderer->rom, tone_offset, &tone))
    return false;
  memset(voice, 0, sizeof *voice);
  voice->component_count = tone.component_count;
  voice->tone_offset = tone_offset;
  voice->key = key;
  voice->velocity = velocity;
  voice->provisional_gain = provisional_gain;
  voice->tvf_audio_transfer = renderer->tvf_audio_transfer;
  voice->tvf_audio_user = renderer->tvf_audio_user;

  for (i = 0; i < tone.component_count; ++i) {
    struct sc88_render_component *render_component = voice->components + i;
    struct sc88_component component;
    struct sc88_zone_selection zone;
    struct sc88_wave_registers registers;
    enum sc88_wave_loop_type mode;
    const struct sc88_wave_bank *bank;
    uint32_t selector_key;
    uint32_t pitch_word;
    uint32_t pcm_base;
    size_t capacity;
    int16_t tvf_key_modulation;

    if (!sc88_rom_open_component(&renderer->rom, &tone, i, &component))
      goto fail;
    selector_key = sc88_renderer_selector_key(&component, key);
    if (!sc88_rom_select_zone(&renderer->rom, &component,
                              (uint8_t)selector_key, &zone) ||
        !sc88_wave_descriptor_loop_type(&zone.descriptor, &mode) ||
        !sc88_wave_prepare_registers(&zone.descriptor, false, &registers) ||
        !sc88_renderer_static_pitch_word(&renderer->rom, &tone, &component,
                                         &zone.descriptor,
                                         (uint8_t)selector_key, &pitch_word) ||
        !sc88_tva_static_gain_q17(&renderer->rom, &tone, &component, &zone,
                                  (uint8_t)selector_key, velocity,
                                  levels,
                                  &render_component->static_attenuation,
                                  &render_component->static_gain_q17) ||
        !sc88_pan_static_q15(&renderer->rom, &tone, &component,
                             (uint8_t)selector_key, pan,
                             &render_component->pan_position,
                             &render_component->left_gain_q15,
                             &render_component->right_gain_q15) ||
        !sc88_pan_component_offset(&renderer->rom, &tone, &component,
                                   (uint8_t)selector_key,
                                   &render_component->pan_component_offset) ||
        !sc88_tva_release_prepare(&renderer->rom, &tone, &component,
                                  (uint8_t)selector_key,
                                  &render_component->release) ||
        !sc88_tva_envelope_prepare(&renderer->rom, &tone, &component,
                                   (uint8_t)selector_key, velocity,
                                   &render_component->envelope) ||
        !sc88_pitch_envelope_prepare(
          &renderer->rom, &tone, &component, (uint8_t)selector_key,
          velocity, &render_component->pitch_envelope) ||
        !sc88_pitch_release_prepare(
          &renderer->rom, &tone, &component, (uint8_t)selector_key,
          render_component->pitch_envelope.depth,
          &render_component->pitch_release) ||
        !sc88_tvf_key_modulation(&renderer->rom, &tone, &component,
                                 (uint8_t)selector_key,
                                 &tvf_key_modulation) ||
        !sc88_tvf_envelope_prepare(&renderer->rom, &tone, &component,
                                    (uint8_t)selector_key, velocity, false,
                                    &render_component->tvf_envelope) ||
        !sc88_tvf_release_prepare(
          &renderer->rom, &tone, &component, (uint8_t)selector_key,
          render_component->tvf_envelope.depth,
          &render_component->tvf_release) ||
        !sc88_tvf_prepare_registers(
                                    &renderer->rom, &component,
                                    tvf_key_modulation,
                                    tvf_controls,
                                    &render_component->tvf) ||
        !sc88_tvf_update_frequency(
          &renderer->rom, render_component->tvf_envelope.current,
          &render_component->tvf))
      goto fail;
    sc88_tvf_latch_frequency(&render_component->tvf);
    sc88_tvf_audio_reset(&render_component->tvf_audio);
    render_component->tvf_key_modulation = tvf_key_modulation;
    render_component->rom_component_offset = component.offset;
    render_component->reverb_send = 127;
    render_component->chorus_send = 127;
    render_component->pan_target_position = render_component->pan_position;
    render_component->static_pitch_word = pitch_word;
    pitch_word = sc88_pitch_current_word(
      pitch_word, 0, render_component->pitch_envelope.current);
    render_component->keep_release_scale_at_zero = tone.common[0x14] != 0;
    render_component->continuous_hold_release = tone.common[0x15] != 0;
    bank = sc88_renderer_find_bank(renderer, zone.descriptor.bank_select);
    if (!bank)
      goto fail;
    pcm_base = zone.descriptor.address_a & ~UINT32_C(0x0f);
    capacity = (size_t)(zone.descriptor.address_c - pcm_base) + 1;
    if (capacity > SIZE_MAX / sizeof *render_component->pcm24)
      goto fail;
    render_component->pcm24 = (int32_t *)malloc(
      capacity * sizeof *render_component->pcm24);
    if (!render_component->pcm24 ||
        !sc88_fce_decode_storage(bank->bytes, bank->size, &zone.descriptor,
                                 render_component->pcm24, capacity, &pcm_base,
                                 &render_component->pcm_count) ||
        !sc88_oscillator_init(&render_component->oscillator,
                              render_component->pcm24,
                              render_component->pcm_count, pcm_base,
                              &registers, mode, pitch_word,
                              renderer->output_rate, renderer->wrap))
      goto fail;
    render_component->active = true;
  }
  return true;

fail:
  sc88_renderer_voice_destroy(voice);
  return false;
}

bool sc88_renderer_note_on_with_part_controls(
  const struct sc88_renderer *renderer, struct sc88_render_voice *voice,
  uint8_t variation, uint8_t program, uint8_t key, uint8_t velocity,
  float provisional_gain, const struct sc88_tva_levels *levels,
  const struct sc88_pan_controls *pan,
  const struct sc88_tvf_controls *tvf_controls)
{
  uint32_t tone_offset;
  if (!renderer ||
      !sc88_rom_select_melodic(&renderer->rom, variation, program,
                               &tone_offset))
    return false;
  return sc88_renderer_note_on_tone(renderer, voice, tone_offset, key,
                                    velocity, provisional_gain, levels, pan,
                                    tvf_controls);
}

bool sc88_renderer_note_on_drum(
  const struct sc88_renderer *renderer, struct sc88_render_voice *voice,
  uint8_t map, uint8_t program, uint8_t key, uint8_t velocity,
  float provisional_gain, const struct sc88_tva_levels *levels,
  const struct sc88_pan_controls *pan,
  const struct sc88_tvf_controls *tvf_controls,
  struct sc88_drum_note *note)
{
  struct sc88_drum_note slot;
  struct sc88_tva_levels drum_levels;
  struct sc88_pan_controls drum_pan;
  uint32_t kit;
  unsigned i;
  if (!renderer || !levels || !pan ||
      !sc88_rom_select_drum(&renderer->rom, map, program, &kit) ||
      !sc88_rom_open_drum_note(&renderer->rom, kit, key, &slot))
    return false;
  /* The kit record carries this key's own level and pan, and the key the
     tone is actually played at - a kick is not the sample transposed to the
     key that triggered it.
     The level is a **per-note** property, so it does not belong in any of
     the four part-level sources, all of which are part or global controls;
     hijacking the secondary level for it both under-drove the kit and threw
     away whatever that control was doing. Where in the amplitude chain the
     firmware applies it is not yet traced, so it is applied to the
     provisional gain, which this codebase already labels provisional. */
  drum_levels = *levels;
  drum_pan = *pan;
  if (slot.level <= 127)
    provisional_gain *= (float)slot.level / 127.0f;
  if (slot.pan >= 1 && slot.pan <= 127)
    drum_pan.part = slot.pan;
  if (note)
    *note = slot;
  if (!sc88_renderer_note_on_tone(renderer, voice, slot.tone_offset,
                                  slot.play_note <= 127 ? slot.play_note : key,
                                  velocity, provisional_gain, &drum_levels,
                                  &drum_pan, tvf_controls))
    return false;
  for (i = 0; i < voice->component_count; ++i) {
    voice->components[i].reverb_send = slot.reverb_send;
    voice->components[i].chorus_send = slot.chorus_send;
  }
  return true;
}

bool sc88_renderer_voice_active(const struct sc88_render_voice *voice)
{
  unsigned i;
  if (!voice)
    return false;
  for (i = 0; i < voice->component_count; ++i)
    if (voice->components[i].active &&
        !voice->components[i].oscillator.ended)
      return true;
  return false;
}

size_t sc88_renderer_render(struct sc88_render_voice *voice,
                            float *stereo, size_t frames)
{
  size_t frame;
  if (!voice || !stereo)
    return 0;
  for (frame = 0; frame < frames; ++frame) {
    float left = 0.0f;
    float right = 0.0f;
    unsigned i;
    bool active = false;
    for (i = 0; i < voice->component_count; ++i) {
      struct sc88_render_component *component = voice->components + i;
      float sample;
      if (component->active &&
          sc88_oscillator_next(&component->oscillator, &sample)) {
        if (voice->tvf_audio_transfer)
          sample = voice->tvf_audio_transfer(
            voice->tvf_audio_user, &component->tvf_audio, &component->tvf,
            1.0, sample);
        float gained = sample * (component->static_gain_q17 / 131072.0f);
        left += gained * (component->left_gain_q15 / 32768.0f);
        right += gained * (component->right_gain_q15 / 32768.0f);
        active = true;
        if (component->oscillator.ended)
          component->active = false;
      } else {
        component->active = false;
      }
    }
    if (!active)
      break;
    stereo[frame * 2] = left * voice->provisional_gain;
    stereo[frame * 2 + 1] = right * voice->provisional_gain;
  }
  return frame;
}
