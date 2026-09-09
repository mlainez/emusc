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
  bool allocated;
  bool key_down;
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
  bool hold;
  bool sostenuto;
  uint8_t sostenuto_keys[16];
};

typedef void (*sc88_control_service_fn)(void *user,
                                        unsigned elapsed_periods);

struct sc88_engine {
  const struct sc88_renderer *renderer;
  struct sc88_engine_note notes[SC88_ENGINE_NOTE_COUNT];
  struct sc88_engine_slot slots[SC88_ENGINE_SLOT_COUNT];
  struct sc88_engine_part parts[SC88_ENGINE_PART_COUNT];
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

bool sc88_engine_note_on(struct sc88_engine *engine, uint8_t part,
                         uint8_t variation, uint8_t program,
                         uint8_t key, uint8_t velocity, uint8_t context,
                         enum sc88_same_note_mode mode,
                         float provisional_gain);
bool sc88_engine_note_off(struct sc88_engine *engine, uint8_t part,
                          uint8_t key);
void sc88_engine_hold(struct sc88_engine *engine, uint8_t part, bool enabled);
void sc88_engine_sostenuto(struct sc88_engine *engine, uint8_t part,
                           bool enabled);

unsigned sc88_engine_active_slots(const struct sc88_engine *engine);
unsigned sc88_engine_released_slots(const struct sc88_engine *engine);

/* Interleaved stereo dry output. The callback receives firmware-equivalent
 * catch-up counts whenever the 10,001-clock (8.0008 ms) service is due. */
void sc88_engine_render(struct sc88_engine *engine, float *stereo,
                        size_t frames);

#ifdef __cplusplus
}
#endif

#endif
