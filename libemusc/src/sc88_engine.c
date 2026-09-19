/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_engine.h"
#define SC88_COMMON_PITCH_SCALE 0.0625

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define SC88_CONTROL_TIMER_HZ 1250000.0
#define SC88_CONTROL_PERIOD_CLOCKS 10001.0

static void sc88_engine_queue_note(struct sc88_engine *engine, uint8_t note)
{
  engine->note_next_free[note] = SC88_ENGINE_NONE;
  if (engine->free_note_tail == SC88_ENGINE_NONE)
    engine->free_note_head = note;
  else
    engine->note_next_free[engine->free_note_tail] = note;
  engine->free_note_tail = note;
}

static uint8_t sc88_engine_pop_note(struct sc88_engine *engine)
{
  uint8_t note = engine->free_note_head;
  if (note == SC88_ENGINE_NONE)
    return note;
  engine->free_note_head = engine->note_next_free[note];
  if (engine->free_note_head == SC88_ENGINE_NONE)
    engine->free_note_tail = SC88_ENGINE_NONE;
  engine->note_next_free[note] = SC88_ENGINE_NONE;
  return note;
}

static void sc88_engine_queue_slot(struct sc88_engine *engine, uint8_t slot)
{
  engine->slots[slot].next_free = SC88_ENGINE_NONE;
  if (engine->free_slot_tail == SC88_ENGINE_NONE)
    engine->free_slot_head = slot;
  else
    engine->slots[engine->free_slot_tail].next_free = slot;
  engine->free_slot_tail = slot;
  ++engine->free_slot_count;
}

static uint8_t sc88_engine_pop_slot(struct sc88_engine *engine)
{
  uint8_t slot = engine->free_slot_head;
  if (slot == SC88_ENGINE_NONE)
    return slot;
  engine->free_slot_head = engine->slots[slot].next_free;
  if (engine->free_slot_head == SC88_ENGINE_NONE)
    engine->free_slot_tail = SC88_ENGINE_NONE;
  engine->slots[slot].next_free = SC88_ENGINE_NONE;
  --engine->free_slot_count;
  return slot;
}

bool sc88_engine_init(struct sc88_engine *engine,
                      const struct sc88_renderer *renderer)
{
  unsigned i;
  if (!engine || !renderer || renderer->output_rate <= 0.0)
    return false;
  memset(engine, 0, sizeof *engine);
  engine->renderer = renderer;
  engine->free_note_head = 0;
  engine->free_note_tail = SC88_ENGINE_NOTE_COUNT - 1;
  engine->free_slot_head = 0;
  engine->free_slot_tail = SC88_ENGINE_SLOT_COUNT - 1;
  engine->free_slot_count = SC88_ENGINE_SLOT_COUNT;
  engine->lfo_seed = 0x1234u;
  engine->next_serial = 1;
  for (i = 0; i < SC88_ENGINE_NOTE_COUNT; ++i)
    engine->note_next_free[i] = i + 1 < SC88_ENGINE_NOTE_COUNT
      ? (uint8_t)(i + 1) : SC88_ENGINE_NONE;
  for (i = 0; i < SC88_ENGINE_SLOT_COUNT; ++i) {
    engine->slots[i].note = SC88_ENGINE_NONE;
    engine->slots[i].next_free = i + 1 < SC88_ENGINE_SLOT_COUNT
      ? (uint8_t)(i + 1) : SC88_ENGINE_NONE;
  }
  engine->pan_seed = 0x4d55u;
  for (i = 0; i < SC88_ENGINE_PART_COUNT; ++i) {
    engine->parts[i].levels.master = 127;
    engine->parts[i].levels.secondary = 127;
    engine->parts[i].levels.part = 127;
    engine->parts[i].levels.expression = 127;
    engine->parts[i].pan.master = 64;
    engine->parts[i].pan.part = 64;
    engine->parts[i].pan.random_position = 64;
    engine->parts[i].delay_send = 0;
    engine->parts[i].lfo1_pitch_depth = 0;
    engine->parts[i].lfo_controls.rate = 64;
    engine->parts[i].lfo_controls.delay = 64;
    engine->parts[i].lfo_controls.depth = 64;
    engine->parts[i].tva_controls.part_attack = 64;
    engine->parts[i].tva_controls.secondary_attack = 64;
    engine->parts[i].tva_controls.part_decay = 64;
    engine->parts[i].tva_controls.secondary_decay = 64;
    engine->parts[i].tvf_controls.part_cutoff = 64;
    engine->parts[i].tvf_controls.secondary_cutoff = 64;
    engine->parts[i].tvf_controls.part_resonance = 64;
    engine->parts[i].tvf_controls.secondary_resonance = 64;
  }
  return true;
}

void sc88_engine_set_part_pan(struct sc88_engine *engine, uint8_t part,
                              const struct sc88_pan_controls *pan)
{
  unsigned i;
  if (!engine || !pan || part >= SC88_ENGINE_PART_COUNT ||
      pan->master < 1 || pan->master > 127 || pan->part > 127)
    return;
  engine->parts[part].pan = *pan;
  if (pan->part == 0)
    return;
  for (i = 0; i < SC88_ENGINE_SLOT_COUNT; ++i) {
    struct sc88_engine_slot *slot = engine->slots + i;
    int target;
    if (!slot->allocated || slot->note >= SC88_ENGINE_NOTE_COUNT ||
        engine->notes[slot->note].part != part)
      continue;
    target = pan->part + ((int)pan->master - 64) +
      slot->component.pan_component_offset;
    if (target < 1)
      target = 1;
    else if (target > 127)
      target = 127;
    slot->component.pan_target_position = (uint8_t)target;
  }
}

/* The oscillator's pitch contribution, in pitch-word units.
 *
 * Exact: the waveform, the rate (`increment * 124.987501249875 / 65536` Hz)
 * and the delay/fade ramp, whose fade multiplies the depth. Exact too is
 * the depth word the controller matrix forms, `(depth * value) >> 2`
 * summed over its sources.
 *
 * Calibrated, not decoded: what that word is worth in cents. The manual
 * publishes one figure - the initial modulation-to-LFO1-pitch depth of
 * `0x0a` is 47 cents - and at full modulation that depth gives the word
 * `(10 * 127) >> 2 = 317`, so 317 stands for 47 cents. The scaling of
 * these words into XP pitch state is listed as still being decoded in
 * `07_synthesis/lfo.md`, so this is the anchor available (`M-019`).
 */
static int32_t sc88_engine_lfo_pitch_offset(
  const struct sc88_engine *engine, const struct sc88_engine_slot *slot,
  uint8_t part)
{
  double cents = 0.0;
  uint16_t matrix = engine->parts[part].lfo1_pitch_depth;
  int16_t local = slot->component.lfo2_pitch_depth;
  int16_t common = slot->component.lfo1_pitch_depth;
  if (matrix)
    cents += 47.0 / 317.0 * (double)matrix *
      ((double)slot->component.lfo1.ramp.fade / 65535.0) *
      ((double)slot->component.lfo1.output / 32767.0);
  /* The tone-common oscillator's vibrato is READ but not applied.
     
     Its depth now reaches the voice correctly resolved - component byte
     `+17` through the curve at `0x78304`, as `05_data_model` and
     `07_synthesis/pitch.md` specify - and every property of that curve
     verifies against the ROM. What is not established is the unit of the
     curve's OUTPUT. Sharing the local field's +/-4032 bound does not
     prove it shares the local field's unit, and applying the manual's
     anchor to it measures worse at every magnitude tried: the attack
     excursion over the seven hardware recordings is 0.67 with this term
     absent, 0.67 at a quarter of the anchor, and 0.37 at the anchor
     itself. A term that only ever subtracts is not merely mis-scaled.
     
     The likeliest reason is one this code does not model: `lfo.md`
     records the tone-common oscillator as SHARED between voices, and
     detached when its owner is released, so "an implementation cannot
     give every voice an independent phase unconditionally". Ours does.
     Independent phases make an ensemble's vibrato incoherent, which
     smears exactly what this metric measures. */
  /* Instrumentation only: apply it at a settable fraction of the local
     field's anchor, so the unit can be calibrated against a direct
     observable instead of a whole-song proxy. Off unless asked. */
  if (common) {
    const char *scale = getenv("SC88_COMMON_VIBRATO");
    if (scale) {
      double k = atof(scale);
      if (k != 0.0)
        cents += k * 47.0 / 317.0 * (double)common *
          ((double)slot->component.lfo1.ramp.fade / 65535.0) *
          ((double)slot->component.lfo1.output / 32767.0);
    }
  }
  /* The local oscillator's vibrato. Its field shares the matrix's units -
     both saturate at exactly 4032, which is `(127 * 127) >> 2` - so the
     same 47-cents-per-317 anchor applies (`M-020`). */
  if (local)
    cents += 47.0 / 317.0 * (double)local *
      ((double)slot->component.lfo2.ramp.fade / 65535.0) *
      ((double)slot->component.lfo2.output / 32767.0);
  if (cents == 0.0)
    return 0;
  /* 0x4000 pitch-word units to the octave */
  return (int32_t)(cents * 16384.0 / 1200.0);
}

/* One oscillator's contribution, as its depth scaled by the waveform and
   the fade. Both oscillators sum, which is what `07_synthesis/lfo.md`
   describes: two waveforms, each multiplied by its own depth term and its
   own fade. */
static double sc88_engine_lfo_term(const struct sc88_lfo *lfo, int16_t depth)
{
  if (!depth)
    return 0.0;
  return (double)depth * ((double)lfo->ramp.fade / 65535.0) *
    ((double)lfo->output / 32767.0);
}

/* The amplitude modulation, as a gain. The depths are attenuation-word
   units and the level tables run at about -5.26 dB per 0x1000, so one unit
   is 0.001284 dB. */
static float sc88_engine_lfo_amplitude(const struct sc88_engine_slot *slot)
{
  double attenuation =
    sc88_engine_lfo_term(&slot->component.lfo1,
                         slot->component.lfo1_tva_depth) +
    sc88_engine_lfo_term(&slot->component.lfo2,
                         slot->component.lfo2_tva_depth);
  if (attenuation == 0.0)
    return 1.0f;
  return (float)pow(10.0, -attenuation * 0.001284 / 20.0);
}

/* The filter modulation, in cutoff-word units. */
static int32_t sc88_engine_lfo_filter(const struct sc88_engine_slot *slot)
{
  double term =
    sc88_engine_lfo_term(&slot->component.lfo1,
                         slot->component.lfo1_tvf_depth) +
    sc88_engine_lfo_term(&slot->component.lfo2,
                         slot->component.lfo2_tvf_depth);
  if (term > 32767.0)
    term = 32767.0;
  else if (term < -32768.0)
    term = -32768.0;
  return (int32_t)term;
}

static int sc88_engine_shared_lfo_slot(struct sc88_engine *engine,
                                       uint32_t tone, uint32_t comp,
                                       uint8_t which);
static void sc88_engine_shared_lfo_join(struct sc88_engine *engine,
                                        uint32_t tone, uint32_t comp,
                                        uint8_t which, struct sc88_lfo *lfo);

static void sc88_engine_update_slot_pitch(struct sc88_engine *engine,
                                          struct sc88_engine_slot *slot)
{
  const struct sc88_engine_note *note;
  uint32_t word;
  if (!engine || !slot || !slot->allocated ||
      slot->note >= SC88_ENGINE_NOTE_COUNT)
    return;
  note = engine->notes + slot->note;
  word = sc88_pitch_current_word(
    slot->component.static_pitch_word,
    engine->parts[note->part].pitch_offset +
      sc88_engine_lfo_pitch_offset(engine, slot, note->part),
    sc88_pitch_envelope_sum(&slot->component.pitch_envelope,
                            &slot->component.pitch_release));
  slot->component.oscillator.step = sc88_pitch_word_rate(
    word, engine->renderer->output_rate);
}

void sc88_engine_set_part_rhythm(struct sc88_engine *engine, uint8_t part,
                                 uint8_t map)
{
  if (!engine || part >= SC88_ENGINE_PART_COUNT || map > 2)
    return;
  engine->parts[part].rhythm_map = map;
}

void sc88_engine_set_part_lfo1_pitch_depth(struct sc88_engine *engine,
                                           uint8_t part, uint16_t depth)
{
  if (!engine || part >= SC88_ENGINE_PART_COUNT)
    return;
  engine->parts[part].lfo1_pitch_depth = depth;
}

void sc88_engine_set_part_reverb_send(struct sc88_engine *engine,
                                      uint8_t part, uint8_t send)
{
  if (!engine || part >= SC88_ENGINE_PART_COUNT || send > 127)
    return;
  engine->parts[part].reverb_send = send;
}

bool sc88_engine_set_drum_parameter(struct sc88_engine *engine,
                                    uint8_t map, uint8_t field,
                                    uint8_t note, uint8_t value)
{
  if (!engine || map < 1 || map > 2 || field < 1 ||
      field > SC88_DRUM_FIELDS || note > 127)
    return false;
  engine->drum_overlay.value[map - 1u][field - 1u][note] = value;
  engine->drum_overlay.present[map - 1u][field - 1u][note] = 1u;
  return true;
}

void sc88_engine_clear_drum_overlay(struct sc88_engine *engine, uint8_t map)
{
  if (!engine || map < 1 || map > 2)
    return;
  memset(engine->drum_overlay.present[map - 1u], 0,
         sizeof engine->drum_overlay.present[map - 1u]);
}

void sc88_engine_set_part_delay_send(struct sc88_engine *engine,
                                     uint8_t part, uint8_t send)
{
  if (!engine || part >= SC88_ENGINE_PART_COUNT || send > 127)
    return;
  engine->parts[part].delay_send = send;
}

void sc88_engine_set_part_chorus_send(struct sc88_engine *engine,
                                      uint8_t part, uint8_t send)
{
  if (!engine || part >= SC88_ENGINE_PART_COUNT || send > 127)
    return;
  engine->parts[part].chorus_send = send;
}

void sc88_engine_set_part_pitch_offset(struct sc88_engine *engine,
                                       uint8_t part, int32_t pitch_offset)
{
  unsigned i;
  if (!engine || !engine->renderer || part >= SC88_ENGINE_PART_COUNT)
    return;
  engine->parts[part].pitch_offset = pitch_offset;
  for (i = 0; i < SC88_ENGINE_SLOT_COUNT; ++i) {
    struct sc88_engine_slot *slot = engine->slots + i;
    if (slot->allocated && slot->note < SC88_ENGINE_NOTE_COUNT &&
        engine->notes[slot->note].part == part) {
      sc88_engine_update_slot_pitch(engine, slot);
    }
  }
}

void sc88_engine_set_part_lfo_controls(
  struct sc88_engine *engine, uint8_t part,
  const struct sc88_lfo_controls *controls)
{
  if (!engine || part >= SC88_ENGINE_PART_COUNT || !controls)
    return;
  engine->parts[part].lfo_controls = *controls;
}

void sc88_engine_set_part_tva_controls(
  struct sc88_engine *engine, uint8_t part,
  const struct sc88_tva_controls *controls)
{
  if (!engine || part >= SC88_ENGINE_PART_COUNT || !controls)
    return;
  engine->parts[part].tva_controls = *controls;
}

void sc88_engine_set_part_tvf_controls(
  struct sc88_engine *engine, uint8_t part,
  const struct sc88_tvf_controls *controls)
{
  if (!engine || !controls || part >= SC88_ENGINE_PART_COUNT ||
      controls->part_cutoff > 127 || controls->secondary_cutoff > 127 ||
      controls->part_resonance > 127 ||
      controls->secondary_resonance > 127)
    return;
  engine->parts[part].tvf_controls = *controls;
  engine->parts[part].tvf_dirty = true;
}

static void sc88_engine_free_note_if_empty(struct sc88_engine *engine,
                                           uint8_t note_index)
{
  struct sc88_engine_note *note = engine->notes + note_index;
  if (!note->allocated || note->slot_count)
    return;
  memset(note, 0, sizeof *note);
  sc88_engine_queue_note(engine, note_index);
}

static void sc88_engine_free_slot(struct sc88_engine *engine,
                                  uint8_t slot_index, bool prepend)
{
  struct sc88_engine_slot *slot = engine->slots + slot_index;
  uint8_t note_index;
  unsigned i;
  if (!slot->allocated)
    return;
  note_index = slot->note;
  if (note_index < SC88_ENGINE_NOTE_COUNT) {
    struct sc88_engine_note *note = engine->notes + note_index;
    for (i = 0; i < SC88_MAX_TONE_COMPONENTS; ++i) {
      if (note->slots[i] == slot_index) {
        note->slots[i] = SC88_ENGINE_NONE;
        --note->slot_count;
        break;
      }
    }
  }
  free(slot->component.pcm24);
  memset(slot, 0, sizeof *slot);
  slot->note = SC88_ENGINE_NONE;
  if (prepend) {
    slot->next_free = engine->free_slot_head;
    engine->free_slot_head = slot_index;
    if (engine->free_slot_tail == SC88_ENGINE_NONE)
      engine->free_slot_tail = slot_index;
    ++engine->free_slot_count;
  } else {
    sc88_engine_queue_slot(engine, slot_index);
  }
  if (note_index < SC88_ENGINE_NOTE_COUNT)
    sc88_engine_free_note_if_empty(engine, note_index);
}

static void sc88_engine_start_release(struct sc88_engine *engine,
                                      uint8_t note_index)
{
  struct sc88_engine_note *note = engine->notes + note_index;
  unsigned i;
  if (!note->allocated || note->key_down || note->hold_retained ||
      note->sostenuto_retained)
    return;
  for (i = 0; i < SC88_MAX_TONE_COMPONENTS; ++i) {
    uint8_t slot_index = note->slots[i];
    struct sc88_render_component *component;
    if (slot_index == SC88_ENGINE_NONE)
      continue;
    component = &engine->slots[slot_index].component;
    if (!component->release.active) {
      sc88_tva_envelope_freeze(
        &engine->renderer->rom, &component->envelope,
        engine->scheduler_clocks / SC88_CONTROL_PERIOD_CLOCKS);
      (void)sc88_tva_release_set_pedal(
        &engine->renderer->rom, engine->parts[note->part].hold_value,
        component->continuous_hold_release,
        component->keep_release_scale_at_zero, false,
        &component->release);
      (void)sc88_tvf_release_set_pedal(
        &engine->renderer->rom, engine->parts[note->part].hold_value,
        component->continuous_hold_release,
        component->keep_release_scale_at_zero, false,
        &component->tvf_release);
      (void)sc88_pitch_release_activate(
        &engine->renderer->rom, engine->parts[note->part].hold_value,
        component->continuous_hold_release,
        component->keep_release_scale_at_zero, false,
        component->pitch_envelope.current, &component->pitch_release);
      component->pitch_envelope.stage = 4;
      component->pitch_envelope.active = false;
    }
  }
}

void sc88_engine_destroy(struct sc88_engine *engine)
{
  unsigned i;
  if (!engine)
    return;
  for (i = 0; i < SC88_ENGINE_SLOT_COUNT; ++i)
    if (engine->slots[i].allocated)
      free(engine->slots[i].component.pcm24);
  for (i = 0; i < SC88_ENGINE_STOPPING_COUNT; ++i)
    if (engine->stopping[i].active)
      free(engine->stopping[i].component.pcm24);
  memset(engine, 0, sizeof *engine);
}

void sc88_engine_set_control_service(struct sc88_engine *engine,
                                     sc88_control_service_fn service,
                                     void *user)
{
  if (!engine)
    return;
  engine->control_service = service;
  engine->control_user = user;
}

void sc88_engine_set_part_levels(struct sc88_engine *engine, uint8_t part,
                                 const struct sc88_tva_levels *levels)
{
  unsigned i;
  if (!engine || !levels || part >= SC88_ENGINE_PART_COUNT ||
      levels->master > 127 || levels->secondary > 127 ||
      levels->part > 127 || levels->expression > 127)
    return;
  engine->parts[part].levels = *levels;
  for (i = 0; i < SC88_ENGINE_NOTE_COUNT; ++i) {
    struct sc88_engine_note *note = engine->notes + i;
    unsigned component_index;
    if (!note->allocated || note->part != part)
      continue;
    note->levels = *levels;
    for (component_index = 0;
         component_index < SC88_MAX_TONE_COMPONENTS; ++component_index) {
      uint8_t slot_index = note->slots[component_index];
      struct sc88_render_component *component;
      if (slot_index == SC88_ENGINE_NONE)
        continue;
      component = &engine->slots[slot_index].component;
      (void)sc88_tva_gain_from_headroom_q17(
        &engine->renderer->rom, component->release.current, levels,
        component->drum_level, component->static_attenuation,
        &component->static_gain_q17);
    }
  }
}

static uint8_t sc88_engine_oldest_slot(const struct sc88_engine *engine,
                                       bool released_only)
{
  uint8_t candidate = SC88_ENGINE_NONE;
  uint64_t serial = UINT64_MAX;
  unsigned i;
  for (i = 0; i < SC88_ENGINE_SLOT_COUNT; ++i) {
    const struct sc88_engine_slot *slot = engine->slots + i;
    const struct sc88_engine_note *note;
    if (!slot->allocated || slot->note >= SC88_ENGINE_NOTE_COUNT)
      continue;
    note = engine->notes + slot->note;
    if (released_only && note->key_down)
      continue;
    if (slot->serial < serial) {
      candidate = (uint8_t)i;
      serial = slot->serial;
    }
  }
  return candidate;
}

static void sc88_engine_stop_voice(struct sc88_engine *engine,
                                   uint8_t slot_index);

static void sc88_engine_reclaim_slots(struct sc88_engine *engine,
                                      unsigned count)
{
  while (count--) {
    uint8_t slot = sc88_engine_oldest_slot(engine, true);
    if (slot == SC88_ENGINE_NONE)
      slot = sc88_engine_oldest_slot(engine, false);
    if (slot == SC88_ENGINE_NONE)
      return;
    /* A stolen voice is still sounding when the CPU takes its slot back
       for a different note - the same circumstance same-note recycle
       hands to the stop ramp below, and the same traced routine (0x4c60)
       serves both (P-0358). free_slot's memset would end it here, the
       hard cut TASK-189 AC#1 is about; run it down instead. */
    sc88_engine_stop_voice(engine, slot);
    sc88_engine_free_slot(engine, slot, false);
  }
}

static bool sc88_engine_note_matches(const struct sc88_engine_note *note,
                                     uint8_t part, uint8_t key,
                                     uint32_t tone_offset, uint8_t context)
{
  return note->allocated && note->part == part && note->key == key &&
    note->tone_offset == tone_offset && note->context == context;
}

/* Hand a still-sounding voice to the chip's stop ramp, so that taking its
   slot back does not end the waveform where it stands.

   The register is picked up where it stands this instant and given the
   target zero; everything else the CPU composed is frozen with it, so the
   first ramped sample continues the last serviced one. See
   `sc88_engine_stopping` for what is traced here and what is not: the slot
   accounting is, the stop's effect on the sound is not. */
static void sc88_engine_stop_voice(struct sc88_engine *engine,
                                   uint8_t slot_index)
{
  struct sc88_engine_slot *slot = engine->slots + slot_index;
  double fraction = engine->scheduler_clocks / SC88_CONTROL_PERIOD_CLOCKS;
  uint32_t current;
  unsigned i;
  if (!slot->allocated || !slot->component.active ||
      slot->note >= SC88_ENGINE_NOTE_COUNT)
    return;
  current = sc88_render_static_gain_q17(&slot->component, fraction);
  if (!current)
    return;
  for (i = 0; i < SC88_ENGINE_STOPPING_COUNT; ++i) {
    struct sc88_engine_stopping *stop = engine->stopping + i;
    if (stop->active)
      continue;
    stop->gain =
      (sc88_tva_envelope_linear_q17(&engine->renderer->rom,
                                    &slot->component.envelope, fraction) /
       131072.0f) *
      sc88_engine_lfo_amplitude(slot) *
      engine->notes[slot->note].provisional_gain;
    stop->part = engine->notes[slot->note].part;
    stop->periods = 0.0;
    stop->component = slot->component;
    stop->component.static_gain_current_q17 = current;
    stop->component.static_gain_q17 = 0;
    /* The decoded sample moves with the voice; the slot must not free the
       buffer the ramp is still reading. */
    slot->component.pcm24 = NULL;
    stop->active = true;
    return;
  }
  /* Nowhere to put it: the voice ends where it stands. */
}

static void sc88_engine_recycle_note(struct sc88_engine *engine,
                                     uint8_t note_index)
{
  uint8_t slots[SC88_MAX_TONE_COMPONENTS];
  unsigned i;
  memcpy(slots, engine->notes[note_index].slots, sizeof slots);
  for (i = 0; i < SC88_MAX_TONE_COMPONENTS; ++i) {
    uint8_t slot = slots[SC88_MAX_TONE_COMPONENTS - 1 - i];
    if (slot == SC88_ENGINE_NONE)
      continue;
    /* The slot goes back on the free list this instant and at its head, as
       `0x2333` does; the sound of the voice it held runs down separately. */
    sc88_engine_stop_voice(engine, slot);
    sc88_engine_free_slot(engine, slot, true);
  }
}

static void sc88_engine_apply_same_note_mode(
  struct sc88_engine *engine, const struct sc88_render_voice *voice,
  uint8_t part, uint8_t key, uint8_t context, enum sc88_same_note_mode mode)
{
  uint8_t oldest = SC88_ENGINE_NONE;
  uint64_t oldest_serial = UINT64_MAX;
  unsigned matches = 0;
  unsigned i;
  if (mode == SC88_SAME_NOTE_FULL_MULTI)
    return;
  for (i = 0; i < SC88_ENGINE_NOTE_COUNT; ++i) {
    const struct sc88_engine_note *note = engine->notes + i;
    if (!sc88_engine_note_matches(note, part, key, voice->tone_offset,
                                  context))
      continue;
    ++matches;
    if (note->serial < oldest_serial) {
      oldest = (uint8_t)i;
      oldest_serial = note->serial;
    }
  }
  if (oldest != SC88_ENGINE_NONE &&
      (mode == SC88_SAME_NOTE_SINGLE || matches >= 2))
    sc88_engine_recycle_note(engine, oldest);
}

/* The position a part-pan of zero asks for: seven bits with zero rejected,
   which is what the firmware takes from the XP readback. The chip's own
   distribution and seeding are not in the CPU path, so the sequence here is
   a stand-in; only the range and the per-voice freshness are recovered
   (`09_mixer/mixer_output.md`). */
static uint8_t sc88_engine_pan_draw(struct sc88_engine *engine)
{
  unsigned tries;
  for (tries = 0; tries < 8u; ++tries) {
    uint16_t s = engine->pan_seed;
    s ^= (uint16_t)(s << 7);
    s ^= (uint16_t)(s >> 9);
    s ^= (uint16_t)(s << 8);
    engine->pan_seed = s;
    if ((uint8_t)(s & 0x7fu))
      return (uint8_t)(s & 0x7fu);
  }
  return 64u;
}

bool sc88_engine_note_on(struct sc88_engine *engine, uint8_t part,
                         uint8_t variation, uint8_t program,
                         uint8_t key, uint8_t velocity, uint8_t context,
                         enum sc88_same_note_mode mode,
                         float provisional_gain)
{
  struct sc88_render_voice voice = {0};
  struct sc88_engine_note *note;
  uint8_t note_index;
  unsigned i;

  if (engine && part < SC88_ENGINE_PART_COUNT &&
      engine->parts[part].pan.part == 0)
    engine->parts[part].pan.random_position = sc88_engine_pan_draw(engine);
  if (!engine || !engine->renderer || part >= SC88_ENGINE_PART_COUNT ||
      mode > SC88_SAME_NOTE_FULL_MULTI || velocity == 0 ||
      !(engine->parts[part].rhythm_map
          ? sc88_renderer_note_on_drum(
              engine->renderer, &voice, engine->parts[part].rhythm_map,
              program, key, velocity,
              &engine->parts[part].levels, &engine->parts[part].pan,
              &engine->parts[part].tvf_controls,
              &engine->parts[part].tva_controls,
              &engine->parts[part].lfo_controls,
              &engine->drum_overlay, NULL)
          : sc88_renderer_note_on_with_part_controls(
              engine->renderer, &voice, variation, program, key, velocity,
              &engine->parts[part].levels,
              &engine->parts[part].pan,
              &engine->parts[part].tvf_controls,
              &engine->parts[part].tva_controls,
              &engine->parts[part].lfo_controls)))
    return false;
  sc88_engine_apply_same_note_mode(engine, &voice, part, key, context, mode);
  if (engine->free_slot_count < voice.component_count)
    sc88_engine_reclaim_slots(engine,
      voice.component_count - engine->free_slot_count);
  note_index = sc88_engine_pop_note(engine);
  if (note_index == SC88_ENGINE_NONE ||
      engine->free_slot_count < voice.component_count)
    goto fail;
  note = engine->notes + note_index;
  memset(note, 0, sizeof *note);
  note->slots[0] = SC88_ENGINE_NONE;
  note->slots[1] = SC88_ENGINE_NONE;
  note->allocated = true;
  note->key_down = true;
  note->part = part;
  note->key = key;
  note->velocity = velocity;
  note->context = context;
  note->tone_offset = voice.tone_offset;
  note->serial = engine->next_serial++;
  /* The engine's own output trim, and the only place it lives: the renderer
     takes no gain and the voice carries no gain field, so there is no second
     copy that could silently disagree with this one. */
  note->provisional_gain = provisional_gain;
  note->ignore_note_off = voice.ignore_note_off;
  note->levels = engine->parts[part].levels;
  for (i = 0; i < voice.component_count; ++i) {
    uint8_t slot_index = sc88_engine_pop_slot(engine);
    struct sc88_engine_slot *slot = engine->slots + slot_index;
    slot->allocated = true;
    slot->note = note_index;
    slot->serial = engine->next_serial++;
    slot->component = voice.components[i];
    /* A nonzero share byte means this voice joins its tone's oscillator
       rather than starting one of its own, so overlapping notes of the
       same tone modulate in phase (`07_synthesis/lfo.md`). */
    if (slot->component.lfo1.share_request)
      sc88_engine_shared_lfo_join(engine, voice.tone_offset, 0, 1,
                                  &slot->component.lfo1);
    if (slot->component.lfo2.share_request)
      sc88_engine_shared_lfo_join(engine, voice.tone_offset,
                                  slot->component.rom_component_offset, 2,
                                  &slot->component.lfo2);
    sc88_engine_update_slot_pitch(engine, slot);
    voice.components[i].pcm24 = NULL;
    voice.components[i].active = false;
    note->slots[i] = slot_index;
    ++note->slot_count;
  }
  return true;

fail:
  sc88_renderer_voice_destroy(&voice);
  return false;
}

bool sc88_engine_note_off(struct sc88_engine *engine, uint8_t part,
                          uint8_t key)
{
  uint8_t candidate = SC88_ENGINE_NONE;
  uint64_t serial = UINT64_MAX;
  unsigned i;
  if (!engine || part >= SC88_ENGINE_PART_COUNT || key > 127)
    return false;
  for (i = 0; i < SC88_ENGINE_NOTE_COUNT; ++i) {
    const struct sc88_engine_note *note = engine->notes + i;
    if (note->allocated && note->key_down && note->part == part &&
        note->key == key && note->serial < serial) {
      candidate = (uint8_t)i;
      serial = note->serial;
    }
  }
  /* A note the kit exempts is left sounding: the key is no longer down but
     nothing is released, so the sample and its envelope run to their end. */
  if (candidate != SC88_ENGINE_NONE &&
      engine->notes[candidate].ignore_note_off) {
    engine->notes[candidate].key_down = false;
    return true;
  }
  if (candidate == SC88_ENGINE_NONE)
    return false;
  engine->notes[candidate].key_down = false;
  engine->notes[candidate].hold_retained = engine->parts[part].hold;
  engine->notes[candidate].sostenuto_retained =
    engine->parts[part].sostenuto &&
    (engine->parts[part].sostenuto_keys[key >> 3] &
     (uint8_t)(1u << (key & 7))) != 0;
  sc88_engine_start_release(engine, candidate);
  return true;
}

void sc88_engine_hold(struct sc88_engine *engine, uint8_t part, bool enabled)
{
  sc88_engine_hold_value(engine, part, enabled ? 127 : 0);
}

void sc88_engine_hold_value(struct sc88_engine *engine, uint8_t part,
                            uint8_t value)
{
  unsigned i;
  bool enabled;
  if (!engine || part >= SC88_ENGINE_PART_COUNT || value > 127)
    return;
  enabled = value >= 64;
  engine->parts[part].hold = enabled;
  engine->parts[part].hold_value = value;
  if (enabled)
    return;
  for (i = 0; i < SC88_ENGINE_NOTE_COUNT; ++i) {
    if (engine->notes[i].allocated && engine->notes[i].part == part) {
      engine->notes[i].hold_retained = false;
      sc88_engine_start_release(engine, (uint8_t)i);
    }
  }
}

void sc88_engine_sostenuto(struct sc88_engine *engine, uint8_t part,
                           bool enabled)
{
  unsigned i;
  if (!engine || part >= SC88_ENGINE_PART_COUNT)
    return;
  engine->parts[part].sostenuto = enabled;
  memset(engine->parts[part].sostenuto_keys, 0,
         sizeof engine->parts[part].sostenuto_keys);
  if (enabled) {
    for (i = 0; i < SC88_ENGINE_NOTE_COUNT; ++i) {
      const struct sc88_engine_note *note = engine->notes + i;
      if (note->allocated && note->key_down && note->part == part)
        engine->parts[part].sostenuto_keys[note->key >> 3] |=
          (uint8_t)(1u << (note->key & 7));
    }
  } else {
    for (i = 0; i < SC88_ENGINE_NOTE_COUNT; ++i) {
      if (engine->notes[i].allocated && engine->notes[i].part == part) {
        engine->notes[i].sostenuto_retained = false;
        sc88_engine_start_release(engine, (uint8_t)i);
      }
    }
  }
}

unsigned sc88_engine_active_slots(const struct sc88_engine *engine)
{
  return engine ? SC88_ENGINE_SLOT_COUNT - engine->free_slot_count : 0;
}

unsigned sc88_engine_released_slots(const struct sc88_engine *engine)
{
  unsigned count = 0;
  unsigned i;
  if (!engine)
    return 0;
  for (i = 0; i < SC88_ENGINE_SLOT_COUNT; ++i)
    if (engine->slots[i].allocated &&
        !engine->notes[engine->slots[i].note].key_down)
      ++count;
  return count;
}

/* The shared-oscillator table. A voice whose share byte is nonzero does not
   own its oscillator: it joins the one already running for its tone, and
   the first voice of a tone creates it. The entry outlives any single
   voice, which is what the firmware achieves by copying the oscillator's
   words into a replacement owner when the original is released. */
static int sc88_engine_shared_lfo_slot(struct sc88_engine *engine,
                                       uint32_t tone, uint32_t comp,
                                       uint8_t which)
{
  unsigned i;
  for (i = 0; i < SC88_ENGINE_SHARED_LFO_COUNT; ++i) {
    const struct sc88_engine_shared_lfo *e = engine->shared_lfo + i;
    if (e->active && e->which == which && e->tone_offset == tone &&
        e->component_offset == comp)
      return (int)i;
  }
  return -1;
}

static void sc88_engine_shared_lfo_join(struct sc88_engine *engine,
                                        uint32_t tone, uint32_t comp,
                                        uint8_t which, struct sc88_lfo *lfo)
{
  unsigned i;
  int at = sc88_engine_shared_lfo_slot(engine, tone, comp, which);
  if (at >= 0) {
    /* Adopt the running oscillator's phase and output. The ramp stays the
       voice's own: it is the note's delay and fade, not the waveform. */
    struct sc88_lfo *shared = &engine->shared_lfo[at].lfo;
    lfo->phase = shared->phase;
    lfo->output = shared->output;
    lfo->random_target = shared->random_target;
    return;
  }
  for (i = 0; i < SC88_ENGINE_SHARED_LFO_COUNT; ++i) {
    struct sc88_engine_shared_lfo *e = engine->shared_lfo + i;
    if (e->active)
      continue;
    e->active = true;
    e->used = true;
    e->which = which;
    e->tone_offset = tone;
    e->component_offset = comp;
    e->lfo = *lfo;
    return;
  }
  /* Table full: the voice keeps its own oscillator, as it would if the
     firmware's comparison had failed. */
}

static void sc88_engine_run_scheduler(struct sc88_engine *engine)
{
  unsigned elapsed;
  unsigned i;
  engine->scheduler_clocks +=
    SC88_CONTROL_TIMER_HZ / engine->renderer->output_rate;
  elapsed = (unsigned)(engine->scheduler_clocks / SC88_CONTROL_PERIOD_CLOCKS);
  if (!elapsed)
    return;
  engine->scheduler_clocks -= elapsed * SC88_CONTROL_PERIOD_CLOCKS;

  /* One oscillator per sharing tone, advanced once for the whole period
     before any voice reads it. Entries nothing used last period are
     retired, which is how a shared oscillator outlives its first owner
     and stops when the last voice of its tone does. */
  {
    unsigned k;
    for (k = 0; k < SC88_ENGINE_SHARED_LFO_COUNT; ++k) {
      struct sc88_engine_shared_lfo *e = engine->shared_lfo + k;
      if (!e->active)
        continue;
      if (!e->used) {
        e->active = false;
        continue;
      }
      e->used = false;
      (void)sc88_lfo_advance(&engine->renderer->rom, &e->lfo, 0,
                             (uint8_t)(elapsed - 1u), &engine->lfo_seed);
    }
  }
  for (i = 0; i < SC88_ENGINE_SLOT_COUNT; ++i) {
    struct sc88_engine_slot *slot = engine->slots + i;
    struct sc88_engine_note *note;
    bool tvf_retargeted = false;
    if (!slot->allocated)
      continue;
    /* The period that has just ended is the one the chip's amplitude
       register spent approaching the target composed for it, so the
       register now stands where `0x2a7` left it - two parts in a hundred
       thousand short of the target, not on it. */
    if (slot->component.release_zeroed) {
      sc88_engine_free_slot(engine, (uint8_t)i, false);
      continue;
    }
    slot->component.static_gain_current_q17 =
      sc88_render_static_gain_q17(&slot->component, 1.0);
    note = engine->notes + slot->note;
    if (slot->component.pan_position < slot->component.pan_target_position)
      ++slot->component.pan_position;
    else if (slot->component.pan_position >
             slot->component.pan_target_position)
      --slot->component.pan_position;
    (void)sc88_pan_pair_q15(&engine->renderer->rom,
                            slot->component.pan_position,
                            &slot->component.left_gain_q15,
                            &slot->component.right_gain_q15);
    if (engine->parts[note->part].tvf_dirty) {
      struct sc88_component component;
      uint32_t previous_current = slot->component.tvf.frequency_current;
      uint32_t previous_resonance = slot->component.tvf.resonance_current;
      component.bytes = engine->renderer->rom.bytes +
        slot->component.rom_component_offset;
      component.offset = slot->component.rom_component_offset;
      component.directory_offset = 0;
      if (sc88_tvf_prepare_registers(
            &engine->renderer->rom, &component,
            slot->component.tvf_key_modulation,
            &engine->parts[note->part].tvf_controls,
            &slot->component.tvf) &&
          sc88_tvf_update_frequency(
            &engine->renderer->rom,
            (int16_t)((uint16_t)sc88_engine_lfo_filter(slot) +
                      (uint16_t)slot->component.tvf_envelope.current +
                      (uint16_t)slot->component.tvf_release.current),
            &slot->component.tvf)) {
        /* prepare_registers clears the struct, so the approach carries
           its own progress across a retarget rather than restarting. */
        slot->component.tvf.frequency_current = previous_current;
        slot->component.tvf.resonance_current = previous_resonance;
        sc88_tvf_advance_registers(&slot->component.tvf, elapsed);
        tvf_retargeted = true;
      }
    }
    /* A shared oscillator was advanced once for the whole tone above; this
       voice reads it rather than running its own, which is what keeps an
       ensemble's vibrato coherent. The ramp is always the voice's own: it
       is the note's delay and fade, not the waveform. It runs on its own
       clock and is not gated by a stalled oscillator. */
    {
      uint32_t tone = engine->notes[slot->note].tone_offset;
      int at = slot->component.lfo1.share_request
        ? sc88_engine_shared_lfo_slot(engine, tone, 0, 1) : -1;
      if (at >= 0) {
        engine->shared_lfo[at].used = true;
        slot->component.lfo1.phase = engine->shared_lfo[at].lfo.phase;
        slot->component.lfo1.output = engine->shared_lfo[at].lfo.output;
        slot->component.lfo1.random_target =
          engine->shared_lfo[at].lfo.random_target;
      } else {
        (void)sc88_lfo_advance(&engine->renderer->rom,
                               &slot->component.lfo1, 0,
                               (uint8_t)(elapsed - 1u), &engine->lfo_seed);
      }
      at = slot->component.lfo2.share_request
        ? sc88_engine_shared_lfo_slot(
            engine, tone, slot->component.rom_component_offset, 2) : -1;
      if (at >= 0) {
        engine->shared_lfo[at].used = true;
        slot->component.lfo2.phase = engine->shared_lfo[at].lfo.phase;
        slot->component.lfo2.output = engine->shared_lfo[at].lfo.output;
        slot->component.lfo2.random_target =
          engine->shared_lfo[at].lfo.random_target;
      } else {
        (void)sc88_lfo_advance(&engine->renderer->rom,
                               &slot->component.lfo2, 0,
                               (uint8_t)(elapsed - 1u), &engine->lfo_seed);
      }
    }
    (void)sc88_lfo_ramp_advance(&slot->component.lfo1.ramp,
                                (uint8_t)(elapsed - 1u));
    (void)sc88_lfo_ramp_advance(&slot->component.lfo2.ramp,
                                (uint8_t)(elapsed - 1u));
    if (slot->component.envelope.active)
      (void)sc88_tva_envelope_advance(&engine->renderer->rom,
                                      &slot->component.envelope, elapsed);
    if (slot->component.pitch_envelope.active)
      (void)sc88_pitch_envelope_advance(&slot->component.pitch_envelope,
                                        elapsed);
    if (slot->component.pitch_release.active)
      (void)sc88_pitch_release_advance(&slot->component.pitch_release,
                                       elapsed);
    if (getenv("SC88_TRACE_PITCH")) {
      static unsigned n;
      if (n < 10)
        fprintf(stderr, "period %2u  pitch env current %6d stage %u "
                "phase %5u active %d\n", n++,
                slot->component.pitch_envelope.current,
                slot->component.pitch_envelope.stage,
                slot->component.pitch_envelope.phase,
                (int)slot->component.pitch_envelope.active);
      if (n == 1) {
        unsigned q;
        fprintf(stderr, "          tva stages:");
        for (q = 0; q < 4; ++q)
          fprintf(stderr, " [%u] target %5u inc %5u",
                  q, slot->component.envelope.target_attenuations[q],
                  slot->component.envelope.increments[q]);
        fprintf(stderr, "\n");
      }
      if (n <= 2)
        fprintf(stderr, "          tva stage %u atten start %5u target %5u "
                "inc %5u phase %5u gain_q17 %8u\n",
                slot->component.envelope.stage,
                slot->component.envelope.start_attenuation,
                slot->component.envelope.target_attenuations[
                  slot->component.envelope.stage],
                slot->component.envelope.increments[
                  slot->component.envelope.stage],
                slot->component.envelope.phase,
                slot->component.envelope.current_q17);
    }
    sc88_engine_update_slot_pitch(engine, slot);
    /* The approach of TVF-F and TVF-Q toward their targets is the chip's
       own interpolation and runs every period; it does not wait for the
       CPU's envelope. A tone with envelope depth 0 has no active envelope
       or release at all, and gating the approach on them left such a
       tone's resonance at the quarter-target the note opens with. */
    if (!tvf_retargeted)
      sc88_tvf_advance_registers(&slot->component.tvf, elapsed);
    if (slot->component.tvf_envelope.active)
      (void)sc88_tvf_envelope_advance(&slot->component.tvf_envelope,
                                      elapsed);
    if (slot->component.tvf_release.active)
      (void)sc88_tvf_release_advance(&slot->component.tvf_release,
                                     elapsed);
    /* The filter LFO belongs in every update, not only the ones a
       control change made dirty: left out here it reached the cutoff
       only on the periods a part parameter happened to change. */
    (void)sc88_tvf_update_frequency(
      &engine->renderer->rom,
      (int16_t)((uint16_t)sc88_engine_lfo_filter(slot) +
                (uint16_t)slot->component.tvf_envelope.current +
                (uint16_t)slot->component.tvf_release.current),
      &slot->component.tvf);
    if (!slot->component.release.active)
      continue;
    /* `7228..7232`: when the release counter underflows, the firmware
       clears it, drops the release flag and composes amplitude 0, which
       `71a9` writes as the chip's TARGET with the interpolation word
       beside it like any other. The chip glides down to it. Freeing the
       slot in the period the zero is composed cuts the voice where it
       stands instead, and what it stands at is 4/131072 - the floor the
       gain table puts under every decay (`72bf`, and `coarse[0]*fine[n]`
       quantising to 1 for every remaining headroom below about 1024).
       A steady level ending in a step is a click, and this one is on the
       end of every note. */
    if (!sc88_tva_release_advance(&slot->component.release, elapsed) ||
        !sc88_tva_gain_from_headroom_q17(
          &engine->renderer->rom, slot->component.release.current,
          &note->levels, slot->component.drum_level,
          slot->component.static_attenuation,
          &slot->component.static_gain_q17) ||
        slot->component.static_gain_q17 == 0) {
      slot->component.static_gain_q17 = 0;
      slot->component.release_zeroed = true;
    }
  }
  for (i = 0; i < SC88_ENGINE_PART_COUNT; ++i)
    engine->parts[i].tvf_dirty = false;
  if (engine->control_service)
    engine->control_service(engine->control_user, elapsed);
}

void sc88_engine_set_stage_taps(struct sc88_engine *engine,
                                const struct sc88_engine_stage_taps *taps)
{
  if (!engine)
    return;
  if (taps)
    engine->stage_taps = *taps;
  else
    memset(&engine->stage_taps, 0, sizeof engine->stage_taps);
}

void sc88_engine_render_with_send(struct sc88_engine *engine, float *stereo,
                                  float *send, float *chorus_send,
                                  float *delay_send, size_t frames)
{
  size_t frame;
  if (!engine || !engine->renderer || !stereo)
    return;
  for (frame = 0; frame < frames; ++frame) {
    float left = 0.0f;
    float right = 0.0f;
    float bus = 0.0f;
    float chorus_bus = 0.0f;
    float delay_bus = 0.0f;
    float tap_osc = 0.0f, tap_tvf = 0.0f, tap_static = 0.0f;
    float tap_tva = 0.0f, tap_lfo = 0.0f;
    unsigned i;
    sc88_engine_run_scheduler(engine);
    for (i = 0; i < SC88_ENGINE_SLOT_COUNT; ++i) {
      struct sc88_engine_slot *slot = engine->slots + i;
      float sample;
      if (!slot->allocated)
        continue;
      if (slot->component.active &&
          sc88_oscillator_next(&slot->component.oscillator, &sample)) {
        tap_osc += sample;
        if (engine->renderer->tvf_audio_transfer)
          sample = engine->renderer->tvf_audio_transfer(
            engine->renderer->tvf_audio_user, &slot->component.tvf_audio,
            &slot->component.tvf,
            engine->scheduler_clocks / SC88_CONTROL_PERIOD_CLOCKS, sample);
        tap_tvf += sample;
        double period_fraction =
          engine->scheduler_clocks / SC88_CONTROL_PERIOD_CLOCKS;
        uint32_t envelope_gain = sc88_tva_envelope_linear_q17(
          &engine->renderer->rom, &slot->component.envelope,
          period_fraction);
        /* The chip's amplitude register, not the CPU's composed target:
           the target is a step once per control period and the register
           is the ramp between two of them. */
        float static_gain = sc88_render_static_gain_q17(
          &slot->component, period_fraction) / 131072.0f;
        tap_static += sample * static_gain;
        tap_tva += sample * static_gain * (envelope_gain / 131072.0f);
        float gained = sample * static_gain *
          (envelope_gain / 131072.0f) *
          sc88_engine_lfo_amplitude(slot) *
          engine->notes[slot->note].provisional_gain;
        tap_lfo += gained;
        left += gained * (slot->component.left_gain_q15 / 32768.0f);
        right += gained * (slot->component.right_gain_q15 / 32768.0f);
        /* A rhythm note's send is its part's control combined with its own
           from its kit record, through the firmware's rounded product and
           the ROM's send curve. A melodic component carries 127 and so
           keeps its part's control unchanged. */
        if (send && slot->note < SC88_ENGINE_NOTE_COUNT) {
          uint16_t send_q15;
          uint8_t control = sc88_send_combine(
            engine->parts[engine->notes[slot->note].part].reverb_send,
            slot->component.reverb_send);
          if (sc88_control_gain_q15(&engine->renderer->rom, control,
                                    &send_q15))
            bus += gained * (send_q15 / 32768.0f);
        }
        /* The delay bus takes the part's send alone: a rhythm note's own
           delay send lives in RAM at `+0x50c`, not in the kit record. */
        if (delay_send && slot->note < SC88_ENGINE_NOTE_COUNT) {
          uint16_t send_q15;
          if (sc88_control_gain_q15(
                &engine->renderer->rom,
                engine->parts[engine->notes[slot->note].part].delay_send,
                &send_q15))
            delay_bus += gained * (send_q15 / 32768.0f);
        }
        /* The chorus bus is formed the same way, from part byte `+0e` and
           the kit's `+0x400` (`08_effects/routing.md`). */
        if (chorus_send && slot->note < SC88_ENGINE_NOTE_COUNT) {
          uint16_t send_q15;
          uint8_t control = sc88_send_combine(
            engine->parts[engine->notes[slot->note].part].chorus_send,
            slot->component.chorus_send);
          if (sc88_control_gain_q15(&engine->renderer->rom, control,
                                    &send_q15))
            chorus_bus += gained * (send_q15 / 32768.0f);
        }
        if (slot->component.oscillator.ended)
          slot->component.active = false;
      } else {
        sc88_engine_free_slot(engine, (uint8_t)i, false);
      }
    }
    /* The voices the CPU has already handed their slots back. Nothing
       composes for them any more: the amplitude register runs down to the
       zero the stop wrote, on its own clock, and the rest of the voice -
       oscillator, filter, pan - carries on from exactly where it stood. */
    for (i = 0; i < SC88_ENGINE_STOPPING_COUNT; ++i) {
      struct sc88_engine_stopping *stop = engine->stopping + i;
      uint32_t register_q17;
      float sample;
      float gained;
      if (!stop->active)
        continue;
      register_q17 =
        sc88_render_static_gain_q17(&stop->component, stop->periods);
      if (!register_q17 || !stop->component.active ||
          !sc88_oscillator_next(&stop->component.oscillator, &sample)) {
        /* The register has arrived at zero, or the sample ran out first. */
        free(stop->component.pcm24);
        memset(stop, 0, sizeof *stop);
        continue;
      }
      tap_osc += sample;
      if (engine->renderer->tvf_audio_transfer)
        sample = engine->renderer->tvf_audio_transfer(
          engine->renderer->tvf_audio_user, &stop->component.tvf_audio,
          &stop->component.tvf,
          engine->scheduler_clocks / SC88_CONTROL_PERIOD_CLOCKS, sample);
      tap_tvf += sample;
      /* One frozen product for the three amplitude taps: the envelope, the
         oscillators and the note gain no longer move apart. */
      gained = sample * (register_q17 / 131072.0f) * stop->gain;
      tap_static += gained;
      tap_tva += gained;
      tap_lfo += gained;
      left += gained * (stop->component.left_gain_q15 / 32768.0f);
      right += gained * (stop->component.right_gain_q15 / 32768.0f);
      if (send) {
        uint16_t send_q15;
        uint8_t control = sc88_send_combine(
          engine->parts[stop->part].reverb_send,
          stop->component.reverb_send);
        if (sc88_control_gain_q15(&engine->renderer->rom, control,
                                  &send_q15))
          bus += gained * (send_q15 / 32768.0f);
      }
      if (delay_send) {
        uint16_t send_q15;
        if (sc88_control_gain_q15(&engine->renderer->rom,
                                  engine->parts[stop->part].delay_send,
                                  &send_q15))
          delay_bus += gained * (send_q15 / 32768.0f);
      }
      if (chorus_send) {
        uint16_t send_q15;
        uint8_t control = sc88_send_combine(
          engine->parts[stop->part].chorus_send,
          stop->component.chorus_send);
        if (sc88_control_gain_q15(&engine->renderer->rom, control,
                                  &send_q15))
          chorus_bus += gained * (send_q15 / 32768.0f);
      }
      if (stop->component.oscillator.ended)
        stop->component.active = false;
      stop->periods += SC88_CONTROL_TIMER_HZ /
        (engine->renderer->output_rate * SC88_CONTROL_PERIOD_CLOCKS);
    }
    stereo[frame * 2] = left;
    stereo[frame * 2 + 1] = right;
    if (send)
      send[frame] = bus;
    if (engine->stage_taps.oscillator)
      engine->stage_taps.oscillator[frame] = tap_osc;
    if (engine->stage_taps.after_tvf)
      engine->stage_taps.after_tvf[frame] = tap_tvf;
    if (engine->stage_taps.after_static)
      engine->stage_taps.after_static[frame] = tap_static;
    if (engine->stage_taps.after_tva)
      engine->stage_taps.after_tva[frame] = tap_tva;
    if (engine->stage_taps.after_lfo)
      engine->stage_taps.after_lfo[frame] = tap_lfo;
    if (chorus_send)
      chorus_send[frame] = chorus_bus;
    if (delay_send)
      delay_send[frame] = delay_bus;
  }
}

void sc88_engine_render(struct sc88_engine *engine, float *stereo,
                        size_t frames)
{
  sc88_engine_render_with_send(engine, stereo, NULL, NULL, NULL, frames);
}
