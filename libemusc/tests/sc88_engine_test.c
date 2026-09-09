/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_engine.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void put16(uint8_t *p, uint16_t value)
{
  p[0] = (uint8_t)(value >> 8);
  p[1] = (uint8_t)value;
}

static void put24(uint8_t *p, uint32_t value)
{
  p[0] = (uint8_t)(value >> 16);
  p[1] = (uint8_t)(value >> 8);
  p[2] = (uint8_t)value;
}

static void make_fixture(uint8_t *control, uint8_t *wave,
                         struct sc88_wave_bank banks[SC88_WAVE_BANK_COUNT])
{
  static const uint8_t selectors[SC88_WAVE_BANK_COUNT] = {
    0x00, 0x01, 0x10, 0x11, 0x20, 0x21, 0x30, 0x31
  };
  static const uint8_t vectors[16] = {
    0x00, 0x00, 0x02, 0x00, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x01, 0xf4, 0x00, 0x00, 0x01, 0xf4
  };
  unsigned i;
  memcpy(control, vectors, sizeof vectors);
  memcpy(control + 0x30000, "\0\0Piano 1A    \3\377", 16);
  control[0x2fc80] = 0;
  put24(control + 0x20000, 0x40000);
  memcpy(control + 0x40000, "Test Tone   ", 12);
  control[0x40000 + 30] = 1;
  control[0x40000 + 32] = 3;
  control[0x40000 + 33] = 2;
  put16(control + 0x40000 + 0x0e, 0xbad0);
  put16(control + 0x40000 + 0x10, 0xb6d0);
  put16(control + 0x40000 + 34, 0);
  put16(control + 0x40000 + 34 + 0x14, 0x4000);
  put16(control + 0x1503e + 255 * 2, 0xffff);
  put16(control + 0x1523e + 255 * 2, 0xffff);
  put16(control + 0x15db6 + 63 * 2, 0x4c00);
  put16(control + 0x1573e + 64 * 2, 0xffff);
  control[0x30010] = 127;
  control[0x30011] = 0xff;
  put16(control + 0x30014, 0x6100);
  put24(control + 0x36101, 0x8000);
  control[0x36106] = 60;
  put24(control + 0x36107, 0x8000);
  put24(control + 0x3610b, 0x8001);
  wave[0x8000] = 1;
  wave[0x8001] = 1;
  for (i = 0; i < SC88_WAVE_BANK_COUNT; ++i) {
    banks[i].selector = selectors[i];
    banks[i].bytes = wave;
    banks[i].size = SC88_WAVE_BANK_SIZE;
  }
}

struct service_count {
  unsigned calls;
  unsigned periods;
};

static void count_service(void *user, unsigned periods)
{
  struct service_count *count = (struct service_count *)user;
  ++count->calls;
  count->periods += periods;
}

int main(void)
{
  uint8_t *control = (uint8_t *)calloc(SC88_CONTROL_ROM_SIZE, 1);
  uint8_t *wave = (uint8_t *)calloc(SC88_WAVE_BANK_SIZE, 1);
  struct sc88_wave_bank banks[SC88_WAVE_BANK_COUNT];
  struct sc88_renderer renderer;
  struct sc88_engine engine;
  struct service_count count = {0, 0};
  float stereo[514];
  unsigned i;

  assert(control && wave);
  make_fixture(control, wave, banks);
  assert(sc88_renderer_init(&renderer, control, SC88_CONTROL_ROM_SIZE,
                            banks, SC88_WAVE_BANK_COUNT, 32000.0,
                            SC88_WRAP_FULL_CARRY));
  assert(sc88_engine_init(&engine, &renderer));
  assert(sc88_engine_note_on(&engine, 0, 0, 0, 60, 100, 0,
                             SC88_SAME_NOTE_SINGLE, 0.25f));
  assert(sc88_engine_active_slots(&engine) == 1);
  assert(sc88_engine_note_on(&engine, 0, 0, 0, 60, 100, 0,
                             SC88_SAME_NOTE_SINGLE, 0.25f));
  assert(sc88_engine_active_slots(&engine) == 1);
  assert(sc88_engine_note_off(&engine, 0, 60));
  assert(sc88_engine_released_slots(&engine) == 1);

  sc88_engine_hold(&engine, 0, true);
  assert(sc88_engine_note_on(&engine, 0, 0, 0, 61, 100, 0,
                             SC88_SAME_NOTE_FULL_MULTI, 0.25f));
  assert(sc88_engine_note_off(&engine, 0, 61));
  assert(sc88_engine_released_slots(&engine) == 2);
  sc88_engine_hold(&engine, 0, false);
  assert(sc88_engine_released_slots(&engine) == 2);

  {
    const struct sc88_tva_levels muted = {0, 127, 127, 127};
    bool found_muted = false;
    put16(control + 0x14f3e, 0xffff);
    assert(sc88_engine_note_on(&engine, 2, 0, 0, 63, 100, 0,
                               SC88_SAME_NOTE_FULL_MULTI, 0.25f));
    sc88_engine_set_part_levels(&engine, 2, &muted);
    sc88_engine_set_part_levels(&engine, 1, &muted);
    assert(sc88_engine_note_on(&engine, 1, 0, 0, 62, 100, 0,
                               SC88_SAME_NOTE_FULL_MULTI, 0.25f));
    for (i = 0; i < SC88_ENGINE_SLOT_COUNT; ++i)
      if (engine.slots[i].allocated &&
          (engine.notes[engine.slots[i].note].part == 1 ||
           engine.notes[engine.slots[i].note].part == 2)) {
        assert(engine.slots[i].component.static_gain_q17 == 0);
        found_muted = true;
      }
    assert(found_muted);
    put16(control + 0x14f3e, 0);
  }

  sc88_engine_set_control_service(&engine, count_service, &count);
  sc88_engine_render(&engine, stereo, 257);
  assert(count.calls == 1 && count.periods == 1);
  assert(sc88_engine_active_slots(&engine) == 2);
  engine.scheduler_clocks = 2.0 * 10001.0;
  sc88_engine_render(&engine, stereo, 1);
  assert(count.calls == 2 && count.periods == 3);
  sc88_engine_destroy(&engine);

  assert(sc88_engine_init(&engine, &renderer));
  for (i = 0; i < SC88_ENGINE_SLOT_COUNT; ++i)
    assert(sc88_engine_note_on(&engine, 0, 0, 0, (uint8_t)i, 100, 0,
                               SC88_SAME_NOTE_FULL_MULTI, 0.25f));
  assert(sc88_engine_active_slots(&engine) == SC88_ENGINE_SLOT_COUNT);
  assert(sc88_engine_note_off(&engine, 0, 0));
  assert(sc88_engine_released_slots(&engine) == 1);
  assert(sc88_engine_note_on(&engine, 0, 0, 0, 100, 100, 0,
                             SC88_SAME_NOTE_FULL_MULTI, 0.25f));
  assert(sc88_engine_active_slots(&engine) == SC88_ENGINE_SLOT_COUNT);
  assert(sc88_engine_released_slots(&engine) == 0);
  sc88_engine_destroy(&engine);

  assert(sc88_engine_init(&engine, &renderer));
  sc88_engine_hold(&engine, 0, true);
  assert(sc88_engine_note_on(&engine, 0, 0, 0, 60, 100, 0,
                             SC88_SAME_NOTE_FULL_MULTI, 0.25f));
  assert(sc88_engine_note_off(&engine, 0, 60));
  sc88_engine_render(&engine, stereo, 257);
  assert(sc88_engine_active_slots(&engine) == 1);
  sc88_engine_hold(&engine, 0, false);
  sc88_engine_render(&engine, stereo, 257);
  assert(sc88_engine_active_slots(&engine) == 0);
  sc88_engine_destroy(&engine);
  free(wave);
  free(control);
  return 0;
}
