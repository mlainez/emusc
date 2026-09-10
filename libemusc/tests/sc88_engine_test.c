/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_engine.h"

#include <assert.h>
#ifdef NDEBUG
#error "this test is assertion-driven; NDEBUG compiles it away"
#endif
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
  /* A stage level word is an **attenuation**: zero is full level. Storing
     0xffff here once looked like full level because the conversion was
     inverted, which the ROM disproves - a piano's last two stages store
     0xffff and its tail is silent. */
  put16(control + 0x40000 + 34 + 0x78, 0x0000);
  control[0x40000 + 34 + 0x80] = 1;
  put16(control + 0x1503e + 255 * 2, 0xffff);
  put16(control + 0x1523e + 255 * 2, 0xffff);
  put16(control + 0x15db6 + 63 * 2, 0x4c00);
  /* the top of the send/pan curve: control 127 is unity, which is what a
     fully wet drum key and a part send of 127 both resolve to */
  put16(control + 0x15db6 + 126 * 2, 0x8000);
  put16(control + 0x1573e + 64 * 2, 0xffff);
  put16(control + 0x1543e + 2, 0xffff);
  control[0x30010] = 127;
  control[0x30011] = 0xff;
  put16(control + 0x30014, 0x6100);
  put24(control + 0x36101, 0x8000);
  control[0x36106] = 60;
  put24(control + 0x36107, 0x8000);
  put24(control + 0x3610b, 0x8001);
  /* A rhythm kit with two keys that differ only in their own reverb send:
     key 36 is dry like STANDARD 1's kick, key 38 fully wet like its
     snare. Both play the same tone, so any difference in the send bus is
     the per-note send and nothing else. */
  control[0x2fd00] = 0;
  put24(control + 0x2b550, 0x23c30);
  put24(control + 0x23c30 + 36 * 3, 0x40000);
  put24(control + 0x23c30 + 38 * 3, 0x40000);
  control[0x23c30 + 0x180 + 36] = 60;
  control[0x23c30 + 0x180 + 38] = 60;
  control[0x23c30 + 0x200 + 36] = 127;
  control[0x23c30 + 0x200 + 38] = 127;
  control[0x23c30 + 0x280 + 36] = 0;
  control[0x23c30 + 0x280 + 38] = 0;
  control[0x23c30 + 0x300 + 36] = 64;
  control[0x23c30 + 0x300 + 38] = 64;
  control[0x23c30 + 0x380 + 36] = 0;
  control[0x23c30 + 0x380 + 38] = 127;
  /* bit 0 of `+0x480` is Rx. Note Off: key 36 clears it and so rings to
     its own end, key 38 sets it and is released like any other note */
  control[0x23c30 + 0x480 + 36] = 0x10;
  control[0x23c30 + 0x480 + 38] = 0x11;
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
  float send[514];
  float wet;
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

  {
    const struct sc88_tvf_controls tvf = {127, 64, 32, 64};
    sc88_engine_set_part_tvf_controls(&engine, 1, &tvf);
    assert(engine.parts[1].tvf_dirty);
  }
  sc88_engine_set_control_service(&engine, count_service, &count);
  sc88_engine_render(&engine, stereo, 257);
  assert(count.calls == 1 && count.periods == 1);
  assert(!engine.parts[1].tvf_dirty);
  {
    bool found = false;
    for (i = 0; i < SC88_ENGINE_SLOT_COUNT; ++i)
      if (engine.slots[i].allocated &&
          engine.notes[engine.slots[i].note].part == 1) {
        assert(engine.slots[i].component.tvf.cutoff_index == 63);
        assert(engine.slots[i].component.tvf.resonance_index == 64);
        found = true;
      }
    assert(found);
  }
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

  /* A rhythm note that does not receive Note Off keeps sounding after it.
     Every drum in demo song 1 is written as a 10 ms note, so releasing on
     Note Off cuts a crash to a tick (`M-015`). */
  assert(sc88_engine_init(&engine, &renderer));
  sc88_engine_set_part_rhythm(&engine, 0, SC88_RHYTHM_MAP_SC88);
  assert(sc88_engine_note_on(&engine, 0, 0, 0, 36, 100, 0,
                             SC88_SAME_NOTE_FULL_MULTI, 1.0f));
  assert(sc88_engine_note_off(&engine, 0, 36));
  /* The key is up, so the slot counts as released for stealing, but the
     envelope must not have been released - that is the whole point. */
  assert(sc88_engine_active_slots(&engine) == 1);
  assert(!engine.slots[0].component.release.active);
  sc88_engine_destroy(&engine);

  /* and the one whose kit does set the bit is released as usual */
  assert(sc88_engine_init(&engine, &renderer));
  sc88_engine_set_part_rhythm(&engine, 0, SC88_RHYTHM_MAP_SC88);
  assert(sc88_engine_note_on(&engine, 0, 0, 0, 38, 100, 0,
                             SC88_SAME_NOTE_FULL_MULTI, 1.0f));
  assert(sc88_engine_note_off(&engine, 0, 38));
  assert(engine.slots[0].component.release.active);
  sc88_engine_destroy(&engine);

  /* a melodic note is never exempt */
  assert(sc88_engine_init(&engine, &renderer));
  assert(sc88_engine_note_on(&engine, 0, 0, 0, 60, 100, 0,
                             SC88_SAME_NOTE_FULL_MULTI, 1.0f));
  assert(sc88_engine_note_off(&engine, 0, 60));
  assert(engine.slots[0].component.release.active);
  sc88_engine_destroy(&engine);

  /* The send combination law, on its own: the firmware's rounded product
     maps a full note send to the part's own control and a zero note send
     to silence, and 127 against 127 must not overflow to 0. */
  assert(sc88_send_combine(40, 127) == 40);
  assert(sc88_send_combine(127, 127) == 127);
  assert(sc88_send_combine(127, 0) == 0);
  assert(sc88_send_combine(0, 127) == 0);
  assert(sc88_send_combine(40, 50) == 16);
  /* and the curve is a curve: control 64 is -4.5 dB, not half */
  {
    uint16_t gain;
    assert(sc88_control_gain_q15(&renderer.rom, 0, &gain) && gain == 0);
    assert(sc88_control_gain_q15(&renderer.rom, 64, &gain) &&
           gain == 0x4c00);
    assert(sc88_control_gain_q15(&renderer.rom, 127, &gain) &&
           gain == 0x8000);
    assert(!sc88_control_gain_q15(&renderer.rom, 128, &gain));
  }

  /* The kit's per-note send reaches the bus. A dry key must put nothing in
     it while still being heard, which is what makes STANDARD 1's kick a
     kick and not a kick in a hall (`M-009`). */
  assert(sc88_engine_init(&engine, &renderer));
  sc88_engine_set_part_rhythm(&engine, 0, SC88_RHYTHM_MAP_SC88);
  sc88_engine_set_part_reverb_send(&engine, 0, 127);
  assert(sc88_engine_note_on(&engine, 0, 0, 0, 36, 100, 0,
                             SC88_SAME_NOTE_FULL_MULTI, 1.0f));
  sc88_engine_render_with_send(&engine, stereo, send, NULL, 1);
  assert(stereo[0] != 0.0f || stereo[1] != 0.0f);
  assert(send[0] == 0.0f);
  sc88_engine_destroy(&engine);

  assert(sc88_engine_init(&engine, &renderer));
  sc88_engine_set_part_rhythm(&engine, 0, SC88_RHYTHM_MAP_SC88);
  sc88_engine_set_part_reverb_send(&engine, 0, 127);
  assert(sc88_engine_note_on(&engine, 0, 0, 0, 38, 100, 0,
                             SC88_SAME_NOTE_FULL_MULTI, 1.0f));
  sc88_engine_render_with_send(&engine, stereo, send, NULL, 1);
  wet = send[0];
  assert(wet != 0.0f);
  sc88_engine_destroy(&engine);

  /* and the part send still scales it, so CC91 keeps authority over the
     whole kit */
  assert(sc88_engine_init(&engine, &renderer));
  sc88_engine_set_part_rhythm(&engine, 0, SC88_RHYTHM_MAP_SC88);
  sc88_engine_set_part_reverb_send(&engine, 0, 0);
  assert(sc88_engine_note_on(&engine, 0, 0, 0, 38, 100, 0,
                             SC88_SAME_NOTE_FULL_MULTI, 1.0f));
  sc88_engine_render_with_send(&engine, stereo, send, NULL, 1);
  assert(send[0] == 0.0f);
  sc88_engine_destroy(&engine);

  /* A melodic note carries no per-note send, so a fully wet drum key is
     exactly as wet as the same tone played melodically: the kit bytes only
     ever take away from the part send. The bus is summed before panning,
     so this holds whatever the two notes' pans are. */
  assert(sc88_engine_init(&engine, &renderer));
  sc88_engine_set_part_reverb_send(&engine, 0, 127);
  assert(sc88_engine_note_on(&engine, 0, 0, 0, 60, 100, 0,
                             SC88_SAME_NOTE_FULL_MULTI, 1.0f));
  sc88_engine_render_with_send(&engine, stereo, send, NULL, 1);
  assert(send[0] == wet);
  sc88_engine_destroy(&engine);

  free(wave);
  free(control);
  return 0;
}
