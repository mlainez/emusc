/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_engine.h"

#include <stdlib.h>
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
  engine->next_serial = 1;
  for (i = 0; i < SC88_ENGINE_NOTE_COUNT; ++i)
    engine->note_next_free[i] = i + 1 < SC88_ENGINE_NOTE_COUNT
      ? (uint8_t)(i + 1) : SC88_ENGINE_NONE;
  for (i = 0; i < SC88_ENGINE_SLOT_COUNT; ++i) {
    engine->slots[i].note = SC88_ENGINE_NONE;
    engine->slots[i].next_free = i + 1 < SC88_ENGINE_SLOT_COUNT
      ? (uint8_t)(i + 1) : SC88_ENGINE_NONE;
  }
  for (i = 0; i < SC88_ENGINE_PART_COUNT; ++i) {
    engine->parts[i].levels.master = 127;
    engine->parts[i].levels.secondary = 127;
    engine->parts[i].levels.part = 127;
    engine->parts[i].levels.expression = 127;
    engine->parts[i].pan.master = 64;
    engine->parts[i].pan.part = 64;
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
    engine->parts[note->part].pitch_offset,
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

void sc88_engine_set_part_reverb_send(struct sc88_engine *engine,
                                      uint8_t part, uint8_t send)
{
  if (!engine || part >= SC88_ENGINE_PART_COUNT || send > 127)
    return;
  engine->parts[part].reverb_send = send;
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
        component->static_attenuation, &component->static_gain_q17);
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

static void sc88_engine_reclaim_slots(struct sc88_engine *engine,
                                      unsigned count)
{
  while (count--) {
    uint8_t slot = sc88_engine_oldest_slot(engine, true);
    if (slot == SC88_ENGINE_NONE)
      slot = sc88_engine_oldest_slot(engine, false);
    if (slot == SC88_ENGINE_NONE)
      return;
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

static void sc88_engine_recycle_note(struct sc88_engine *engine,
                                     uint8_t note_index)
{
  uint8_t slots[SC88_MAX_TONE_COMPONENTS];
  unsigned i;
  memcpy(slots, engine->notes[note_index].slots, sizeof slots);
  for (i = 0; i < SC88_MAX_TONE_COMPONENTS; ++i) {
    uint8_t slot = slots[SC88_MAX_TONE_COMPONENTS - 1 - i];
    if (slot != SC88_ENGINE_NONE)
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

  if (!engine || !engine->renderer || part >= SC88_ENGINE_PART_COUNT ||
      mode > SC88_SAME_NOTE_FULL_MULTI || velocity == 0 ||
      !(engine->parts[part].rhythm_map
          ? sc88_renderer_note_on_drum(
              engine->renderer, &voice, engine->parts[part].rhythm_map,
              program, key, velocity, provisional_gain,
              &engine->parts[part].levels, &engine->parts[part].pan,
              &engine->parts[part].tvf_controls, NULL)
          : sc88_renderer_note_on_with_part_controls(
              engine->renderer, &voice, variation, program, key, velocity,
              provisional_gain, &engine->parts[part].levels,
              &engine->parts[part].pan,
              &engine->parts[part].tvf_controls)))
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
  for (i = 0; i < SC88_ENGINE_SLOT_COUNT; ++i) {
    struct sc88_engine_slot *slot = engine->slots + i;
    struct sc88_engine_note *note;
    bool tvf_retargeted = false;
    if (!slot->allocated)
      continue;
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
      uint32_t previous_target = slot->component.tvf.frequency_target;
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
            (int16_t)((uint16_t)slot->component.tvf_envelope.current +
                      (uint16_t)slot->component.tvf_release.current),
            &slot->component.tvf)) {
        slot->component.tvf.frequency_current = previous_target;
        tvf_retargeted = true;
      }
    }
    if (slot->component.envelope.active)
      (void)sc88_tva_envelope_advance(&engine->renderer->rom,
                                      &slot->component.envelope, elapsed);
    if (slot->component.pitch_envelope.active)
      (void)sc88_pitch_envelope_advance(&slot->component.pitch_envelope,
                                        elapsed);
    if (slot->component.pitch_release.active)
      (void)sc88_pitch_release_advance(&slot->component.pitch_release,
                                       elapsed);
    sc88_engine_update_slot_pitch(engine, slot);
    if (slot->component.tvf_envelope.active ||
        slot->component.tvf_release.active) {
      if (!tvf_retargeted)
        sc88_tvf_latch_frequency(&slot->component.tvf);
      if (slot->component.tvf_envelope.active)
        (void)sc88_tvf_envelope_advance(&slot->component.tvf_envelope,
                                        elapsed);
      if (slot->component.tvf_release.active)
        (void)sc88_tvf_release_advance(&slot->component.tvf_release,
                                       elapsed);
      (void)sc88_tvf_update_frequency(
        &engine->renderer->rom,
        (int16_t)((uint16_t)slot->component.tvf_envelope.current +
                  (uint16_t)slot->component.tvf_release.current),
        &slot->component.tvf);
    }
    if (!slot->component.release.active)
      continue;
    if (!sc88_tva_release_advance(&slot->component.release, elapsed) ||
        !sc88_tva_gain_from_headroom_q17(
          &engine->renderer->rom, slot->component.release.current,
          &note->levels, slot->component.static_attenuation,
          &slot->component.static_gain_q17) ||
        slot->component.static_gain_q17 == 0) {
      sc88_engine_free_slot(engine, (uint8_t)i, false);
    }
  }
  for (i = 0; i < SC88_ENGINE_PART_COUNT; ++i)
    engine->parts[i].tvf_dirty = false;
  if (engine->control_service)
    engine->control_service(engine->control_user, elapsed);
}

void sc88_engine_render_with_send(struct sc88_engine *engine, float *stereo,
                                  float *send, float *chorus_send,
                                  size_t frames)
{
  size_t frame;
  if (!engine || !engine->renderer || !stereo)
    return;
  for (frame = 0; frame < frames; ++frame) {
    float left = 0.0f;
    float right = 0.0f;
    float bus = 0.0f;
    float chorus_bus = 0.0f;
    unsigned i;
    sc88_engine_run_scheduler(engine);
    for (i = 0; i < SC88_ENGINE_SLOT_COUNT; ++i) {
      struct sc88_engine_slot *slot = engine->slots + i;
      float sample;
      if (!slot->allocated)
        continue;
      if (slot->component.active &&
          sc88_oscillator_next(&slot->component.oscillator, &sample)) {
        if (engine->renderer->tvf_audio_transfer)
          sample = engine->renderer->tvf_audio_transfer(
            engine->renderer->tvf_audio_user, &slot->component.tvf_audio,
            &slot->component.tvf,
            engine->scheduler_clocks / SC88_CONTROL_PERIOD_CLOCKS, sample);
        uint32_t envelope_gain = sc88_tva_envelope_linear_q17(
          &engine->renderer->rom, &slot->component.envelope,
          engine->scheduler_clocks / SC88_CONTROL_PERIOD_CLOCKS);
        float gained = sample *
          (slot->component.static_gain_q17 / 131072.0f) *
          (envelope_gain / 131072.0f) *
          engine->notes[slot->note].provisional_gain;
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
    stereo[frame * 2] = left;
    stereo[frame * 2 + 1] = right;
    if (send)
      send[frame] = bus;
    if (chorus_send)
      chorus_send[frame] = chorus_bus;
  }
}

void sc88_engine_render(struct sc88_engine *engine, float *stereo,
                        size_t frames)
{
  sc88_engine_render_with_send(engine, stereo, NULL, NULL, frames);
}
