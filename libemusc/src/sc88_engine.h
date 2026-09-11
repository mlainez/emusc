/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_SC88_ENGINE_H
#define EMUSC_SC88_ENGINE_H

#include "sc88_renderer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SC88_ENGINE_SLOT_COUNT 64u
#define SC88_ENGINE_NOTE_COUNT 64u
#define SC88_ENGINE_PART_COUNT 32u
#define SC88_ENGINE_NONE 0xffu

enum sc88_same_note_mode {
  SC88_SAME_NOTE_SINGLE = 0,
  SC88_SAME_NOTE_LIMITED_MULTI = 1,
  SC88_SAME_NOTE_FULL_MULTI = 2
};

struct sc88_engine_note {
  uint8_t slots[SC88_MAX_TONE_COMPONENTS];
  uint8_t slot_count;
  uint8_t part;
  uint8_t key;
  uint8_t velocity;
  uint8_t context;
  uint32_t tone_offset;
  uint64_t serial;
  float provisional_gain;
  struct sc88_tva_levels levels;
  bool allocated;
  bool key_down;
  bool ignore_note_off;
  bool hold_retained;
  bool sostenuto_retained;
};

struct sc88_engine_slot {
  struct sc88_render_component component;
  uint8_t note;
  uint8_t next_free;
  uint64_t serial;
  bool allocated;
};

struct sc88_engine_part {
  struct sc88_tva_levels levels;
  struct sc88_pan_controls pan;
  struct sc88_tvf_controls tvf_controls;
  struct sc88_tva_controls tva_controls;
  struct sc88_lfo_controls lfo_controls;
  int32_t pitch_offset;
  bool tvf_dirty;
  bool hold;
  bool sostenuto;
  uint8_t sostenuto_keys[16];
  uint8_t hold_value;
  /* Nonzero makes this a rhythm part, and selects which kit set its program
     indexes: 1 for the SC-55 kits, 2 for the SC-88's own. GS calls this Use
     For Rhythm Part, and it is a property of the part rather than of a note,
     which is why it lives here. */
  uint8_t rhythm_map;
  /* The part's reverb send, 0..127 as received. The kit records carry a
     per-note send too (`M-009`), which is why this is applied per slot
     rather than to the finished mix. */
  uint8_t reverb_send;
  uint8_t chorus_send;
  uint8_t delay_send;
  /* `(depth * value) >> 2` summed over the controller matrix's
     sources, of which only modulation is modelled. */
  uint16_t lfo1_pitch_depth;
};

typedef void (*sc88_control_service_fn)(void *user,
                                        unsigned elapsed_periods);

/* `07_synthesis/lfo.md`: an oscillator whose share byte is nonzero is not
   private to its voice. The firmware compares the tone pointer and ROM page
   before sharing, and when a shared oscillator's owner is released it
   detaches and copies its words into the replacement owner - so "an
   implementation cannot give every voice an independent phase
   unconditionally". These entries are that shared state: one oscillator per
   tone (and per component, for the local one), advanced once per control
   period, read by every voice that shares it. */
#define SC88_ENGINE_SHARED_LFO_COUNT 48u

struct sc88_engine_shared_lfo {
  uint32_t tone_offset;
  uint32_t component_offset;    /* zero for the tone-common oscillator */
  uint8_t which;                /* 1 tone-common, 2 local */
  bool active;
  bool used;
  struct sc88_lfo lfo;
};

struct sc88_engine {
  const struct sc88_renderer *renderer;
  struct sc88_engine_shared_lfo shared_lfo[SC88_ENGINE_SHARED_LFO_COUNT];
  struct sc88_engine_note notes[SC88_ENGINE_NOTE_COUNT];
  struct sc88_engine_slot slots[SC88_ENGINE_SLOT_COUNT];
  struct sc88_engine_part parts[SC88_ENGINE_PART_COUNT];
  /* one random word shared by every oscillator, as the firmware has */
  uint16_t lfo_seed;
  /* Draws the position a part-pan of zero asks for. Separate from
     lfo_seed so a random pan cannot perturb a random LFO. */
  uint16_t pan_seed;
  /* Per-note kit overrides a song has written over SysEx. */
  struct sc88_drum_overlay drum_overlay;
  uint8_t free_note_head;
  uint8_t free_note_tail;
  uint8_t note_next_free[SC88_ENGINE_NOTE_COUNT];
  uint8_t free_slot_head;
  uint8_t free_slot_tail;
  unsigned free_slot_count;
  uint64_t next_serial;
  double scheduler_clocks;
  sc88_control_service_fn control_service;
  void *control_user;
};

bool sc88_engine_init(struct sc88_engine *engine,
                      const struct sc88_renderer *renderer);
void sc88_engine_destroy(struct sc88_engine *engine);
void sc88_engine_set_control_service(struct sc88_engine *engine,
                                     sc88_control_service_fn service,
                                     void *user);
void sc88_engine_set_part_levels(struct sc88_engine *engine, uint8_t part,
                                 const struct sc88_tva_levels *levels);
void sc88_engine_set_part_pan(struct sc88_engine *engine, uint8_t part,
                              const struct sc88_pan_controls *pan);
void sc88_engine_set_part_pitch_offset(struct sc88_engine *engine,
                                       uint8_t part, int32_t pitch_offset);
void sc88_engine_set_part_lfo_controls(
  struct sc88_engine *engine, uint8_t part,
  const struct sc88_lfo_controls *controls);
void sc88_engine_set_part_tva_controls(
  struct sc88_engine *engine, uint8_t part,
  const struct sc88_tva_controls *controls);
void sc88_engine_set_part_tvf_controls(
  struct sc88_engine *engine, uint8_t part,
  const struct sc88_tvf_controls *controls);

void sc88_engine_set_part_rhythm(struct sc88_engine *engine, uint8_t part,
                                 uint8_t map);
/* One `41 mf rr` write: `map` 1 or 2, `field` 1..9, `note` 0..127. */
bool sc88_engine_set_drum_parameter(struct sc88_engine *engine,
                                    uint8_t map, uint8_t field,
                                    uint8_t note, uint8_t value);
/* Changing a rhythm part's kit clears them, as the firmware does. */
void sc88_engine_clear_drum_overlay(struct sc88_engine *engine,
                                    uint8_t map);
void sc88_engine_set_part_delay_send(struct sc88_engine *engine,
                                     uint8_t part, uint8_t send);
void sc88_engine_set_part_chorus_send(struct sc88_engine *engine,
                                     uint8_t part, uint8_t send);
void sc88_engine_set_part_lfo1_pitch_depth(struct sc88_engine *engine,
                                           uint8_t part, uint16_t depth);
void sc88_engine_set_part_reverb_send(struct sc88_engine *engine,
                                      uint8_t part, uint8_t send);
bool sc88_engine_note_on(struct sc88_engine *engine, uint8_t part,
                         uint8_t variation, uint8_t program,
                         uint8_t key, uint8_t velocity, uint8_t context,
                         enum sc88_same_note_mode mode,
                         float provisional_gain);
bool sc88_engine_note_off(struct sc88_engine *engine, uint8_t part,
                          uint8_t key);
void sc88_engine_hold(struct sc88_engine *engine, uint8_t part, bool enabled);
void sc88_engine_hold_value(struct sc88_engine *engine, uint8_t part,
                            uint8_t value);
void sc88_engine_sostenuto(struct sc88_engine *engine, uint8_t part,
                           bool enabled);

unsigned sc88_engine_active_slots(const struct sc88_engine *engine);
unsigned sc88_engine_released_slots(const struct sc88_engine *engine);

/* Interleaved stereo dry output. The callback receives firmware-equivalent
 * catch-up counts whenever the 10,001-clock (8.0008 ms) service is due. */
void sc88_engine_render(struct sc88_engine *engine, float *stereo,
                        size_t frames);
/* As above, and also accumulates the two mono effect send buses, each of
 * which must hold `frames` samples when given. Either may be NULL. */
void sc88_engine_render_with_send(struct sc88_engine *engine, float *stereo,
                                  float *send, float *chorus_send,
                                  float *delay_send, size_t frames);

#ifdef __cplusplus
}
#endif

#endif
