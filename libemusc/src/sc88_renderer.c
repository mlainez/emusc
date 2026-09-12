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

/* Fractional remainder of the key transform, in pitch units.

   The transform at SC88-CTL 0x60c7..0x6123 holds `(midi_key - 60) * +0x14`
   as a 32-bit value shifted left twice (0x60fb..0x6101), so its high word is
   the integer key and its low word is the remainder in units of 1/65536
   semitone. 0x611e..0x6121 (`5c 05 55` `mov:i.w #0x555,r4`, `ab ac`
   `mulxu.w r3,r4`) multiply that remainder by 0x555 - 1365 pitch units, one
   semitone - and keep the high word of the product. The firmware stores it
   at RAM 0x197c (0x603f, 0x6072) and adds it into the pitch word at 0x6081
   (`f9 19 7c 23` plus `addx.w #0,r2`); nothing else reads 0x197c.

   r4 is cleared at 0x610d and the multiply is skipped on both clamp branches
   (0x6116 key <- 0, 0x611a key <- 0x7f), so a clamped key contributes no
   fraction. The multiply is unsigned, and the remainder is the distance
   above the floored key, so the term is 0..1364 and never negative. */
uint16_t sc88_renderer_key_fraction(const struct sc88_component *component,
                                    uint8_t midi_key)
{
  int32_t factor;
  int32_t product;
  int32_t key;
  uint32_t remainder;

  if (!component || !component->bytes || midi_key > 127)
    return 0;
  factor = sc88_renderer_s16(sc88_renderer_be16(component->bytes + 0x14));
  product = ((int32_t)midi_key - 60) * factor;
  key = 60 + sc88_renderer_floor_q14(product) +
    sc88_renderer_s8(component->bytes[0x16]);
  if (key < 0 || key > 127)
    return 0;
  remainder = ((uint32_t)product << 2) & 0xffffu;
  return (uint16_t)((0x555u * remainder) >> 16);
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
                                     uint16_t key_fraction,
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
    /* SC88-CTL 0x6081 adds RAM 0x197c into the low word of the pitch the
       key table just produced, before the 0x6124 offsets land on it. */
    (int32_t)key_fraction +
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

/* Centre of the 255-word bipolar pitch-control curve at 0x78304..0x78502,
   indexed -127..127 about this address (`02_rom/tables.md`). */
#define SC88_PITCH_CURVE_CENTRE 0x78402u

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
  renderer->tva_controls.part_attack = 64;
  renderer->tva_controls.secondary_attack = 64;
  renderer->tva_controls.part_decay = 64;
  renderer->tva_controls.secondary_decay = 64;
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

void sc88_renderer_set_only_component(struct sc88_renderer *renderer,
                                      unsigned which)
{
  if (renderer)
    renderer->only_component = which;
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
    levels, pan, &renderer->tvf_controls, &renderer->tva_controls, NULL);
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
  const struct sc88_tvf_controls *tvf_controls,
  const struct sc88_tva_controls *tva_controls,
  const struct sc88_lfo_controls *lfo_controls)
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
  /* `component_count` is the number of components this note actually
     sounds, which is not the tone's own count: a component whose velocity
     window excludes this note is not prepared and takes no slot. It is
     counted up as the loop below prepares them, and the prepared ones are
     packed from index zero, so a note that sounds only the tone's second
     component holds it in `components[0]`. */
  voice->only_component = renderer->only_component;
  voice->tone_offset = tone_offset;
  voice->key = key;
  voice->velocity = velocity;
  voice->provisional_gain = provisional_gain;
  /* melodic notes always receive Note Off; a kit may say otherwise */
  voice->ignore_note_off = false;
  voice->tvf_audio_transfer = renderer->tvf_audio_transfer;
  voice->tvf_audio_user = renderer->tvf_audio_user;

  for (i = 0; i < tone.component_count; ++i) {
    struct sc88_render_component *render_component;
    struct sc88_component component;
    struct sc88_zone_selection zone;
    struct sc88_wave_registers registers;
    enum sc88_wave_loop_type mode;
    const struct sc88_wave_bank *bank;
    uint32_t selector_key;
    uint16_t key_fraction;
    uint32_t pitch_word;
    uint32_t pcm_base;
    size_t capacity;
    int16_t tvf_key_modulation;

    /* Instrumentation: sound one component of a multi-component tone, so a
       defect in how the two are balanced can be separated from a defect in
       either one. The components are numbered from ONE, and zero - the
       default - sounds every component the tone asks for. Read as a
       zero-based index instead, `--only-component 0` renders the whole
       tone and looks like proof that the second component is silent. */
    if (voice->only_component > 0 && i + 1 != voice->only_component)
      continue;
    if (!sc88_rom_open_component(&renderer->rom, &tone, i, &component))
      goto fail;
    /* The component's velocity window, +6c..+6d inclusive. Outside it the
       component does not sound at all: `sc88_rom_component_sounds` carries
       the ROM evidence. Ignoring it does not merely add a layer that should
       be absent, it adds the LOUDEST one - the firmware's velocity index is
       `(velocity - low) * factor >> 8` on a wrapping byte, so one count
       below the window the subtraction wraps to 255 and the index saturates
       at the top of the curve. French Horns at velocity 100 sounded its
       101..127 component, six cutoff indices brighter than the one the note
       asks for, at full level. */
    if (!sc88_rom_component_sounds(&component, velocity))
      continue;
    render_component = voice->components + voice->component_count;
    selector_key = sc88_renderer_selector_key(&component, key);
    key_fraction = sc88_renderer_key_fraction(&component, key);
    if (!sc88_rom_select_zone(&renderer->rom, &component,
                              (uint8_t)selector_key, &zone) ||
        !sc88_wave_descriptor_loop_type(&zone.descriptor, &mode) ||
        // The descriptor's +16 word is NOT added to the start address. Adding it
        // skipped the first 4608 samples of every sample this tone plays -
        // 144 ms, which on a struck instrument is the whole strike: the
        // Xylophone's onset centroid read 1403 Hz against the reference's
        // 4603, and its timbre travel over the first 400 ms was 356 Hz against
        // 3548. Suppressed, they are 4863 Hz and 3816.
        //
        // `02_rom/wave_metadata.md` says the word is added "unless per-voice
        // state +187d bit 7 suppresses it", and that condition is NOT
        // recovered. Both branches are therefore a guess about when; this one
        // is the guess the measurements support - over seven demo songs the
        // onset excursion goes 0.54 to 0.72 and the static centroid to within
        // 45 Hz on both paths, and the 76-instrument set is unchanged at 65
        // within 6 dB, because the loop points are separate from the start
        // address and the sustain never moves.
        !sc88_wave_prepare_registers(&zone.descriptor, true, &registers) ||
        !sc88_renderer_static_pitch_word(&renderer->rom, &tone, &component,
                                         &zone.descriptor,
                                         (uint8_t)selector_key, key_fraction,
                                         &pitch_word) ||
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
                                   tva_controls,
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
    /* Neutral part and user modifiers: the part-level rate and delay
       offsets are a controller-matrix destination this does not model
       yet, so the tone's own rate stands. */
    if (!sc88_lfo_common_prepare(
          &renderer->rom, &tone,
          lfo_controls ? lfo_controls->rate : 64, 64,
          lfo_controls ? lfo_controls->delay : 64, 64,
          &render_component->lfo1))
      memset(&render_component->lfo1, 0, sizeof render_component->lfo1);
    if (!sc88_lfo_local_prepare(&renderer->rom, &component,
                                &render_component->lfo2))
      memset(&render_component->lfo2, 0, sizeof render_component->lfo2);
    /* `05_data_model/partial_schema.md` and `07_synthesis/pitch.md` agree,
       against the table in `lfo.md`: the tone-common oscillator's pitch
       depth is the **signed byte at `+17`**, and it is an index rather
       than a depth. Routine `0x6326..0x639e` clamps it to -127..127,
       doubles it as a byte offset and reads the 255-word curve about its
       centre at `0x78402`. That curve is strictly increasing, exactly
       antisymmetric, zero at its centre and saturating at +/-4032 - the
       same `(127 * 127) >> 2` the local field reaches, so its output
       carries the unit the manual's anchor was measured in.

       Read instead as a `+16` BE16, the field spans -9216..+6271, which
       is what `lfo.md` records as an unestablished unit, and applying the
       anchor to those raw numbers gives `Crystal` 721 cents of vibrato. */
    {
      int idx = (int)(int8_t)component.bytes[0x17];
      uint32_t at;
      if (idx < -127)
        idx = -127;
      else if (idx > 127)
        idx = 127;
      at = (uint32_t)(SC88_PITCH_CURVE_CENTRE + 2 * idx);
      render_component->lfo1_pitch_depth =
        at + 2 <= renderer->rom.size
          ? sc88_renderer_s16(sc88_renderer_be16(renderer->rom.bytes + at))
          : 0;
    }
    render_component->lfo2_pitch_depth =
      (int16_t)((uint16_t)((uint16_t)component.bytes[0x18] << 8) |
                component.bytes[0x19]);
    render_component->lfo1_tva_depth =
      (int16_t)((uint16_t)((uint16_t)component.bytes[0x74] << 8) |
                component.bytes[0x75]);
    render_component->lfo2_tva_depth =
      (int16_t)((uint16_t)((uint16_t)component.bytes[0x76] << 8) |
                component.bytes[0x77]);
    render_component->lfo1_tvf_depth =
      (int16_t)((uint16_t)((uint16_t)component.bytes[0x44] << 8) |
                component.bytes[0x45]);
    render_component->lfo2_tvf_depth =
      (int16_t)((uint16_t)((uint16_t)component.bytes[0x46] << 8) |
                component.bytes[0x47]);
    /* The part's vibrato depth is a centred modifier on the tone's own,
       in the same units (`M-021`). */
    if (lfo_controls)
      render_component->lfo2_pitch_depth = (int16_t)(
        render_component->lfo2_pitch_depth +
        ((int)lfo_controls->depth - 64) * 2);
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
    ++voice->component_count;
  }
  /* Every tone in the ROM covers every velocity: all 201 single-component
     melodic tones and every single-component rhythm tone carry the window
     0..127, and no two-component tone leaves a velocity uncovered. So zero
     here is not a ROM tone at all - it is `--only-component` naming a
     component the tone does not have. Refuse the note rather than allocate
     slots that sound nothing. */
  if (voice->component_count == 0)
    goto fail;
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
  const struct sc88_tvf_controls *tvf_controls,
  const struct sc88_tva_controls *tva_controls,
  const struct sc88_lfo_controls *lfo_controls)
{
  uint32_t tone_offset;
  if (!renderer ||
      !sc88_rom_select_melodic(&renderer->rom, variation, program,
                               &tone_offset))
    return false;
  return sc88_renderer_note_on_tone(renderer, voice, tone_offset, key,
                                    velocity, provisional_gain, levels, pan,
                                    tvf_controls, tva_controls,
                                    lfo_controls);
}

bool sc88_renderer_note_on_drum(
  const struct sc88_renderer *renderer, struct sc88_render_voice *voice,
  uint8_t map, uint8_t program, uint8_t key, uint8_t velocity,
  float provisional_gain, const struct sc88_tva_levels *levels,
  const struct sc88_pan_controls *pan,
  const struct sc88_tvf_controls *tvf_controls,
  const struct sc88_tva_controls *tva_controls,
  const struct sc88_lfo_controls *lfo_controls,
  const struct sc88_drum_overlay *overlay,
  struct sc88_drum_note *note)
{
  struct sc88_drum_note slot;
  struct sc88_tva_levels drum_levels;
  struct sc88_pan_controls drum_pan;
  uint32_t kit;
  unsigned i;
  if (!renderer || !levels || !pan ||
      !sc88_rom_select_drum(&renderer->rom, map, program, &kit) ||
      !sc88_rom_open_drum_note_overlaid(&renderer->rom, kit, key, overlay,
                                       map, &slot))
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
                                  &drum_pan, tvf_controls, tva_controls,
                                  lfo_controls))
    return false;
  for (i = 0; i < voice->component_count; ++i) {
    voice->components[i].reverb_send = slot.reverb_send;
    voice->components[i].chorus_send = slot.chorus_send;
  }
  voice->ignore_note_off = (slot.flags & 0x01u) == 0u;
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
