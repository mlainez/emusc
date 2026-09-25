/* SPDX-License-Identifier: CC0-1.0 */
#include "engines/xp/engine.h"

#include <cassert>
#ifdef NDEBUG
#error "this test is assertion-driven; NDEBUG compiles it away"
#endif
#include <cstdlib>
#include <cstring>

using namespace EmuSC::Xp;

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
                         struct xp_wave_bank banks[XP_WAVE_BANK_COUNT])
{
  static const uint8_t selectors[XP_WAVE_BANK_COUNT] = {
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
  /* The component's velocity window, +6c..+6d inclusive. A calloc'd
     fixture states 0..0, which sounds nothing: every note this file
     plays would be refused. The ROM's own tones all reach 127. */
  control[0x40000 + 34 + 0x6c] = 0;
  control[0x40000 + 34 + 0x6d] = 127;
  /* The coarse and fine level tables, as a monotone ramp. The real tables
     are a dB curve; what matters to a fixture is that an intermediate
     attenuation converts to an intermediate gain, because an envelope
     stage ramps its attenuation and reads the tables all the way along. */
  for (i = 0; i < 256; ++i) {
    put16(control + 0x1503e + i * 2, (uint16_t)((i + 1) * 256 - 1));
    put16(control + 0x1523e + i * 2, (uint16_t)((i + 1) * 256 - 1));
  }
  put16(control + 0x15db6 + 63 * 2, 0x4c00);
  /* the top of the pan curve: position 127 is unity on its own side */
  put16(control + 0x15db6 + 126 * 2, 0x8000);
  /* The send curve is a DIFFERENT table, at 0x15eb6, indexed by the control
     value whole, and linear where the pan curve is not. The fixture carries
     the firmware's own expression for it so the reader can see that the two
     disagree everywhere except three points. */
  for (i = 0; i < 128; ++i)
    put16(control + 0x15eb6 + i * 2,
          (uint16_t)(64u * (((unsigned)i * 512u + 63u) / 127u)));
  put16(control + 0x1573e + 64 * 2, 0xffff);
  put16(control + 0x1543e + 2, 0xffff);
  /* The stage's interpolation word, which the chip is handed beside the
     target: the component takes the exponential table at `0x1563e`, and
     this is the SC-88's own entry at the rate index above. Left at zero a
     fixture says "never move", and the stage would hold at its start
     level for its whole dwell. */
  put16(control + 0x1563e + 2, 0x0517);
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
  for (i = 0; i < XP_WAVE_BANK_COUNT; ++i) {
    banks[i].selector = selectors[i];
    banks[i].bytes = wave;
    banks[i].size = SC88_PROFILE.waveBankSize;
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

int main()
{
  uint8_t *control = (uint8_t *)calloc(XP_CONTROL_ROM_SIZE, 1);
  uint8_t *wave = (uint8_t *)calloc(SC88_PROFILE.waveBankSize, 1);
  struct xp_wave_bank banks[XP_WAVE_BANK_COUNT];
  struct xp_renderer renderer;
  struct xp_engine engine;
  struct service_count count = {0, 0};
  float stereo[514];
  float send[514];
  float wet;
  unsigned i;
  unsigned retarget_slot = XP_ENGINE_SLOT_COUNT;
  uint32_t standing_target;

  assert(control && wave);
  make_fixture(control, wave, banks);
  assert(renderer_init(&renderer, control, XP_CONTROL_ROM_SIZE,
                            banks, XP_WAVE_BANK_COUNT, 32000.0,
                            XP_WRAP_FULL_CARRY));
  assert(engine_init(&engine, &renderer));
  assert(engine_note_on(&engine, 0, 0, 0, 60, 100, 0,
                             XP_SAME_NOTE_SINGLE, 0.25f));
  assert(engine_active_slots(&engine) == 1);
  assert(engine_note_on(&engine, 0, 0, 0, 60, 100, 0,
                             XP_SAME_NOTE_SINGLE, 0.25f));
  assert(engine_active_slots(&engine) == 1);
  assert(engine_note_off(&engine, 0, 60));
  assert(engine_released_slots(&engine) == 1);

  engine_hold(&engine, 0, true);
  assert(engine_note_on(&engine, 0, 0, 0, 61, 100, 0,
                             XP_SAME_NOTE_FULL_MULTI, 0.25f));
  assert(engine_note_off(&engine, 0, 61));
  assert(engine_released_slots(&engine) == 2);
  engine_hold(&engine, 0, false);
  assert(engine_released_slots(&engine) == 2);

  {
    const struct xp_tva_levels muted = {0, 127, 127, 127};
    bool found_muted = false;
    put16(control + 0x14f3e, 0xffff);
    assert(engine_note_on(&engine, 2, 0, 0, 63, 100, 0,
                               XP_SAME_NOTE_FULL_MULTI, 0.25f));
    engine_set_part_levels(&engine, 2, &muted);
    engine_set_part_levels(&engine, 1, &muted);
    assert(engine_note_on(&engine, 1, 0, 0, 62, 100, 0,
                               XP_SAME_NOTE_FULL_MULTI, 0.25f));
    for (i = 0; i < XP_ENGINE_SLOT_COUNT; ++i)
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
    const struct xp_tvf_controls tvf = {127, 64, 32, 64, 0};
    for (i = 0; i < XP_ENGINE_SLOT_COUNT; ++i)
      if (engine.slots[i].allocated &&
          engine.notes[engine.slots[i].note].part == 1) {
        retarget_slot = i;
        break;
      }
    assert(retarget_slot < XP_ENGINE_SLOT_COUNT);
    standing_target =
      engine.slots[retarget_slot].component.tvf.frequency_target;
    /* The cutoff base for index 63 and the limit for resonance index 64,
       so the retarget composes a target that is not zero. Restored below:
       every note in this file was started with the tables empty and the
       rest of it reads them as it found them. */
    put16(control + 0x78702 + 63 * 2, 0x4000);
    put16(control + 0x78802 + 64 * 2, 0xffff);
    engine_set_part_tvf_controls(&engine, 1, &tvf);
    assert(engine.parts[1].tvf_dirty);
  }
  engine_set_control_service(&engine, count_service, &count);
  engine_render(&engine, stereo, 257);
  assert(count.calls == 1 && count.periods == 1);
  assert(!engine.parts[1].tvf_dirty);
  {
    bool found = false;
    for (i = 0; i < XP_ENGINE_SLOT_COUNT; ++i)
      if (engine.slots[i].allocated &&
          engine.notes[engine.slots[i].note].part == 1) {
        assert(engine.slots[i].component.tvf.cutoff_index == 63);
        assert(engine.slots[i].component.tvf.resonance_index == 64);
        found = true;
      }
    assert(found);
  }
  /* The order the registers are serviced in, which `0x695b` fixes: the
     chip's approach closes out the period that has just ended, and only
     then does the CPU compose the target for the period about to start.
     So at the boundary the register stands on the target that was
     STANDING during the period just rendered - `0x4100` covers the whole
     gap - and the newly composed target is somewhere else, with the 2 ms
     approach to it still ahead.

     Composing first and approaching afterwards puts the register on the
     new target instead, which is a cutoff step with no ramp on every
     period a part controller moved. The two asserts pin both halves: the
     register is where the old target was, and the new target is not the
     same value, so neither can pass by the retarget having done nothing. */
  assert(engine.slots[retarget_slot].component.tvf.frequency_current ==
         standing_target);
  assert(engine.slots[retarget_slot].component.tvf.frequency_target !=
         standing_target);
  put16(control + 0x78702 + 63 * 2, 0);
  put16(control + 0x78802 + 64 * 2, 0);
  /* The two released voices composed amplitude 0 in the period just
     rendered and are spending the next one gliding down to it, which is
     what the chip does with a target (`71c7`); they are still allocated
     until it ends. */
  assert(engine_active_slots(&engine) == 4);
  assert(engine_released_slots(&engine) == 2);
  engine.scheduler_clocks = 2.0 * 10001.0;
  engine_render(&engine, stereo, 1);
  assert(count.calls == 2 && count.periods == 3);
  assert(engine_active_slots(&engine) == 2);
  engine_destroy(&engine);

  assert(engine_init(&engine, &renderer));
  for (i = 0; i < XP_ENGINE_SLOT_COUNT; ++i)
    assert(engine_note_on(&engine, 0, 0, 0, (uint8_t)i, 100, 0,
                               XP_SAME_NOTE_FULL_MULTI, 0.25f));
  assert(engine_active_slots(&engine) == XP_ENGINE_SLOT_COUNT);
  /* One period so every voice's amplitude register has actually risen off
     zero - stopping a voice that has never been rendered has nothing to
     ramp down from, which would make the stolen-voice check below pass
     whether or not the steal path ever calls the stop ramp at all. */
  engine_render(&engine, stereo, 257);
  assert(engine_active_slots(&engine) == XP_ENGINE_SLOT_COUNT);
  assert(engine_note_off(&engine, 0, 0));
  assert(engine_released_slots(&engine) == 1);
  /* All 64 slots are full, so this note-on can only be served by stealing
     the one just released (TASK-189 AC#2: a stimulus that forces voice
     stealing). */
  assert(engine_note_on(&engine, 0, 0, 0, 100, 100, 0,
                             XP_SAME_NOTE_FULL_MULTI, 0.25f));
  assert(engine_active_slots(&engine) == XP_ENGINE_SLOT_COUNT);
  assert(engine_released_slots(&engine) == 0);
  /* The stolen voice must be handed to the chip's stop ramp rather than
     memset mid-note (TASK-189 AC#1, P-0358): it is still sounding down in
     engine.stopping[], not simply gone. */
  {
    unsigned stopping = 0;
    for (i = 0; i < XP_ENGINE_STOPPING_COUNT; ++i)
      if (engine.stopping[i].active)
        ++stopping;
    assert(stopping == 1);
  }
  engine_destroy(&engine);

  assert(engine_init(&engine, &renderer));
  engine_hold(&engine, 0, true);
  assert(engine_note_on(&engine, 0, 0, 0, 60, 100, 0,
                             XP_SAME_NOTE_FULL_MULTI, 0.25f));
  assert(engine_note_off(&engine, 0, 60));
  engine_render(&engine, stereo, 257);
  assert(engine_active_slots(&engine) == 1);
  engine_hold(&engine, 0, false);
  /* One period for the release to run out, one for the glide to zero. */
  engine_render(&engine, stereo, 257);
  engine_render(&engine, stereo, 257);
  assert(engine_active_slots(&engine) == 0);
  engine_destroy(&engine);

  /* A rhythm note that does not receive Note Off keeps sounding after it.
     Every drum in demo song 1 is written as a 10 ms note, so releasing on
     Note Off cuts a crash to a tick (`M-015`). */
  assert(engine_init(&engine, &renderer));
  engine_set_part_rhythm(&engine, 0, 1);
  assert(engine_note_on(&engine, 0, 0, 0, 36, 100, 0,
                             XP_SAME_NOTE_FULL_MULTI, 1.0f));
  assert(engine_note_off(&engine, 0, 36));
  /* The key is up, so the slot counts as released for stealing, but the
     envelope must not have been released - that is the whole point. */
  assert(engine_active_slots(&engine) == 1);
  assert(!engine.slots[0].component.release.active);
  engine_destroy(&engine);

  /* and the one whose kit does set the bit is released as usual */
  assert(engine_init(&engine, &renderer));
  engine_set_part_rhythm(&engine, 0, 1);
  assert(engine_note_on(&engine, 0, 0, 0, 38, 100, 0,
                             XP_SAME_NOTE_FULL_MULTI, 1.0f));
  assert(engine_note_off(&engine, 0, 38));
  assert(engine.slots[0].component.release.active);
  engine_destroy(&engine);

  /* A `41 mf rr` edit reaches the drum setup it names and no other. Demo
     song 3 addresses its kick's panpot to MAP1, which is the setup a part
     on MIDI channel 10 plays from, so a part reading the wrong half of the
     overlay loses the edit silently. Play note is the observable here
     because it moves the oscillator. */
  {
    double edited;
    double plain;
    assert(engine_init(&engine, &renderer));
    engine_set_part_rhythm(&engine, 0, 1);
    assert(engine_set_drum_parameter(&engine, 1, 1, 36, 72));
    assert(engine_note_on(&engine, 0, 0, 0, 36, 100, 0,
                               XP_SAME_NOTE_FULL_MULTI, 1.0f));
    edited = engine.slots[0].component.oscillator.step;
    engine_destroy(&engine);

    assert(engine_init(&engine, &renderer));
    engine_set_part_rhythm(&engine, 0, 2);
    assert(engine_set_drum_parameter(&engine, 1, 1, 36, 72));
    assert(engine_note_on(&engine, 0, 0, 0, 36, 100, 0,
                               XP_SAME_NOTE_FULL_MULTI, 1.0f));
    plain = engine.slots[0].component.oscillator.step;
    engine_destroy(&engine);
    assert(edited > plain * 1.5);
  }

  /* a melodic note is never exempt */
  assert(engine_init(&engine, &renderer));
  assert(engine_note_on(&engine, 0, 0, 0, 60, 100, 0,
                             XP_SAME_NOTE_FULL_MULTI, 1.0f));
  assert(engine_note_off(&engine, 0, 60));
  assert(engine.slots[0].component.release.active);
  engine_destroy(&engine);

  /* Tone-common `+0x15`, the guard `58c3` reads: the release ramps go up
     either way, but a tone carrying the byte skips the whole block from
     `58c9` - its envelopes keep running underneath the ramp instead of
     being forced to stage 4, and its pitch release ramps toward the
     destination rather than the distance from where the envelope stands.
     23 of the ROM's 922 tones carry it, nearly all of them pianos.

     The pitch envelope is given a depth for this block only, because with
     the fixture's depth of zero every pitch quantity is zero and the two
     halves below would agree while modelling nothing. */
  {
    struct xp_render_component *released;
    put16(control + 0x40000 + 34 + 0x1a, 0x4000);
    put16(control + 0x40000 + 34 + 0x1e, 0x1000);
    put16(control + 0x40000 + 34 + 0x28, 0xc000);
    /* A zero stage-1 rate makes the envelope open at stage 1 and so at the
       level the fixture leaves at zero; a nonzero one opens it at the
       initial level above, which is what gives the delta something to be
       the distance from. */
    control[0x40000 + 34 + 0x2a] = 1;

    assert(engine_init(&engine, &renderer));
    assert(engine_note_on(&engine, 0, 0, 0, 60, 100, 0,
                               XP_SAME_NOTE_FULL_MULTI, 1.0f));
    assert(engine.slots[0].component.envelope.active);
    assert(engine_note_off(&engine, 0, 60));
    released = &engine.slots[0].component;
    assert(released->release.active && released->pitch_release.active);
    assert(!released->envelope.active);
    assert(released->pitch_envelope.stage == 4);
    assert(!released->pitch_envelope.active);
    assert(released->pitch_release.delta ==
           (int16_t)((uint16_t)released->pitch_release.destination -
                     (uint16_t)released->pitch_envelope.current));
    assert(released->pitch_release.delta !=
           released->pitch_release.destination);
    engine_destroy(&engine);

    control[0x40000 + 0x15] = 1;
    assert(engine_init(&engine, &renderer));
    assert(engine_note_on(&engine, 0, 0, 0, 60, 100, 0,
                               XP_SAME_NOTE_FULL_MULTI, 1.0f));
    assert(engine.slots[0].component.continuous_hold_release);
    assert(engine_note_off(&engine, 0, 60));
    released = &engine.slots[0].component;
    assert(released->release.active && released->pitch_release.active);
    assert(released->envelope.active);
    assert(released->pitch_envelope.stage != 4);
    assert(released->pitch_envelope.active);
    assert(released->pitch_release.delta ==
           released->pitch_release.destination);
    engine_destroy(&engine);

    control[0x40000 + 0x15] = 0;
    put16(control + 0x40000 + 34 + 0x1a, 0);
    put16(control + 0x40000 + 34 + 0x1e, 0);
    put16(control + 0x40000 + 34 + 0x28, 0);
    control[0x40000 + 34 + 0x2a] = 0;
  }

  /* The TVF arm of the same block, `58da` and `58df..58e7`: an ordinary
     tone's filter envelope stops at stage 4 where the key left it and the
     release ramp covers the distance from there to the release level, so
     the two words `6967` adds end on the release level. A `+0x15` tone
     keeps its envelope running and ramps toward the release level itself.

     Depth 0x4000 with a zero stage-1 rate opens the envelope at stage 1,
     standing on the stage-1 target, so it is nonzero at note off. */
  {
    struct xp_render_component *released;
    put16(control + 0x40000 + 34 + 0x48, 0x4000);
    put16(control + 0x40000 + 34 + 0x4a, 0x4000);
    put16(control + 0x40000 + 34 + 0x52, 0xc000);

    assert(engine_init(&engine, &renderer));
    assert(engine_note_on(&engine, 0, 0, 0, 60, 100, 0,
                               XP_SAME_NOTE_FULL_MULTI, 1.0f));
    int16_t standing = engine.slots[0].component.tvf_envelope.current;
    int16_t level = engine.slots[0].component.tvf_release.target;
    assert(engine.slots[0].component.tvf_envelope.active);
    assert(standing != 0 && level < 0);
    assert(engine_note_off(&engine, 0, 60));
    released = &engine.slots[0].component;
    assert(released->tvf_release.active);
    assert(released->tvf_envelope.stage == 4);
    assert(!released->tvf_envelope.active);
    assert(released->tvf_envelope.current == standing);
    assert(released->tvf_release.target ==
           (int16_t)((uint16_t)level - (uint16_t)standing));
    for (unsigned i = 0; i < 4096 && released->tvf_release.active; ++i)
      (void)tvf_release_advance(&released->tvf_release, 1);
    assert(!released->tvf_release.active);
    assert((int16_t)((uint16_t)released->tvf_envelope.current +
                     (uint16_t)released->tvf_release.current) == level);
    engine_destroy(&engine);

    control[0x40000 + 0x15] = 1;
    assert(engine_init(&engine, &renderer));
    assert(engine_note_on(&engine, 0, 0, 0, 60, 100, 0,
                               XP_SAME_NOTE_FULL_MULTI, 1.0f));
    assert(engine_note_off(&engine, 0, 60));
    released = &engine.slots[0].component;
    assert(released->tvf_release.active);
    assert(released->tvf_envelope.stage != 4);
    assert(released->tvf_envelope.active);
    assert(released->tvf_release.target == level);
    engine_destroy(&engine);

    control[0x40000 + 0x15] = 0;
    put16(control + 0x40000 + 34 + 0x48, 0);
    put16(control + 0x40000 + 34 + 0x4a, 0);
    put16(control + 0x40000 + 34 + 0x52, 0);
  }

  /* The send combination law, on its own: the firmware's rounded product
     maps a full note send to the part's own control and a zero note send
     to silence, and 127 against 127 must not overflow to 0. */
  assert(send_combine(40, 127) == 40);
  assert(send_combine(127, 127) == 127);
  assert(send_combine(127, 0) == 0);
  assert(send_combine(0, 127) == 0);
  assert(send_combine(40, 50) == 16);
  /* and the send curve is LINEAR: control 64 is a shade over half, not the
     pan curve's -4.5 dB. The two tables are only three points apart over
     the whole range and reading the pan one here opens every send too far
     (`P-xxxx`). */
  {
    uint16_t gain;
    assert(control_gain_q15(&renderer.rom, 0, &gain) && gain == 0);
    assert(control_gain_q15(&renderer.rom, 64, &gain) &&
           gain == 0x4080);
    assert(control_gain_q15(&renderer.rom, 127, &gain) &&
           gain == 0x8000);
    /* control 1 is a small open send, not a closed one: the pan table's
       first word is zero and using it here muted the send entirely */
    assert(control_gain_q15(&renderer.rom, 1, &gain) && gain == 0x0100);
    assert(!control_gain_q15(&renderer.rom, 128, &gain));
  }

  /* The kit's per-note send reaches the bus. A dry key must put nothing in
     it while still being heard, which is what makes STANDARD 1's kick a
     kick and not a kick in a hall (`M-009`). */
  assert(engine_init(&engine, &renderer));
  engine_set_part_rhythm(&engine, 0, 1);
  engine_set_part_reverb_send(&engine, 0, 127);
  assert(engine_note_on(&engine, 0, 0, 0, 36, 100, 0,
                             XP_SAME_NOTE_FULL_MULTI, 1.0f));
  engine_render_with_send(&engine, stereo, send, NULL, NULL, 1);
  assert(stereo[0] != 0.0f || stereo[1] != 0.0f);
  assert(send[0] == 0.0f);
  engine_destroy(&engine);

  assert(engine_init(&engine, &renderer));
  engine_set_part_rhythm(&engine, 0, 1);
  engine_set_part_reverb_send(&engine, 0, 127);
  assert(engine_note_on(&engine, 0, 0, 0, 38, 100, 0,
                             XP_SAME_NOTE_FULL_MULTI, 1.0f));
  engine_render_with_send(&engine, stereo, send, NULL, NULL, 1);
  wet = send[0];
  assert(wet != 0.0f);
  engine_destroy(&engine);

  /* and the part send still scales it, so CC91 keeps authority over the
     whole kit */
  assert(engine_init(&engine, &renderer));
  engine_set_part_rhythm(&engine, 0, 1);
  engine_set_part_reverb_send(&engine, 0, 0);
  assert(engine_note_on(&engine, 0, 0, 0, 38, 100, 0,
                             XP_SAME_NOTE_FULL_MULTI, 1.0f));
  engine_render_with_send(&engine, stereo, send, NULL, NULL, 1);
  assert(send[0] == 0.0f);
  engine_destroy(&engine);

  /* A melodic note carries no per-note send, so a fully wet drum key is
     exactly as wet as the same tone played melodically: the kit bytes only
     ever take away from the part send. The bus is summed before panning,
     so this holds whatever the two notes' pans are. */
  assert(engine_init(&engine, &renderer));
  engine_set_part_reverb_send(&engine, 0, 127);
  assert(engine_note_on(&engine, 0, 0, 0, 60, 100, 0,
                             XP_SAME_NOTE_FULL_MULTI, 1.0f));
  engine_render_with_send(&engine, stereo, send, NULL, NULL, 1);
  assert(send[0] == wet);
  engine_destroy(&engine);

  /* Portamento. The switch, the time and the source all have to be there
     for a glide to start; every component of the note glides, on one
     shared source, target and rate; and the second note's source is where
     the first one's pitch stands, not where it was aimed. */
  assert(engine_init(&engine, &renderer));
  {
    unsigned slot;
    unsigned glides;
    uint8_t newest;
    uint32_t first = 0;
    /* one semitone per control period, so the arithmetic is readable */
    put16(control + 0x78502 + 4 * 40, 0x0001);
    put16(control + 0x78502 + 4 * 40 + 2, 0x0000);

    /* portamento off: the first note leaves its key behind and nothing
       glides */
    assert(engine_note_on(&engine, 0, 0, 0, 40, 100, 0,
                               XP_SAME_NOTE_FULL_MULTI, 1.0f));
    assert(engine_note_on(&engine, 0, 0, 0, 76, 100, 0,
                               XP_SAME_NOTE_FULL_MULTI, 1.0f));
    for (slot = 0; slot < XP_ENGINE_SLOT_COUNT; ++slot)
      assert(!engine.slots[slot].component.portamento.active);

    /* the switch alone is not enough: CC5 = 0 is the firmware's own
       no-glide exit */
    engine_set_part_portamento(&engine, 0, true);
    assert(engine_note_on(&engine, 0, 0, 0, 40, 100, 0,
                               XP_SAME_NOTE_FULL_MULTI, 1.0f));
    for (slot = 0; slot < XP_ENGINE_SLOT_COUNT; ++slot)
      assert(!engine.slots[slot].component.portamento.active);

    engine_set_part_portamento_time(&engine, 0, 40);
    assert(engine_note_on(&engine, 0, 0, 0, 76, 100, 0,
                               XP_SAME_NOTE_FULL_MULTI, 1.0f));
    /* EVERY component of the note glides, not one of them: the SC-55 path
       shipped twice with one partial starting at the target while the
       other glided (TASK-088, then TASK-113). */
    newest = XP_ENGINE_NONE;
    for (i = 0; i < XP_ENGINE_NOTE_COUNT; ++i)
      if (engine.notes[i].allocated && engine.notes[i].part == 0 &&
          (newest == XP_ENGINE_NONE ||
           engine.notes[i].serial > engine.notes[newest].serial))
        newest = (uint8_t)i;
    assert(newest != XP_ENGINE_NONE);
    assert(engine.notes[newest].slot_count >= 1);
    glides = 0;
    for (i = 0; i < engine.notes[newest].slot_count; ++i) {
      const struct xp_portamento *g =
        &engine.slots[engine.notes[newest].slots[i]].component.portamento;
      assert(g->active);
      assert(g->current == 40u << 16);
      assert(g->target == 76u << 16);
      assert(g->ascending);
      assert(g->rate == 0x00010000u);
      ++glides;
    }
    assert(glides == engine.notes[newest].slot_count);
    /* and they stay together: one shared source, target and rate leaves
       nothing that could make one component arrive before the other */
    for (i = 0; i < engine.notes[newest].slot_count; ++i)
      portamento_advance(
        &engine.slots[engine.notes[newest].slots[i]].component.portamento, 7);
    first = engine.slots[engine.notes[newest].slots[0]]
              .component.portamento.current;
    assert(first == 47u << 16);
    for (i = 0; i < engine.notes[newest].slot_count; ++i)
      assert(engine.slots[engine.notes[newest].slots[i]]
               .component.portamento.current == first);

    /* a note arriving mid-glide starts from where the glide stands, not
       from where the note it follows was aimed */
    assert(engine_note_on(&engine, 0, 0, 0, 100, 100, 0,
                               XP_SAME_NOTE_FULL_MULTI, 1.0f));
    newest = XP_ENGINE_NONE;
    for (i = 0; i < XP_ENGINE_NOTE_COUNT; ++i)
      if (engine.notes[i].allocated && engine.notes[i].part == 0 &&
          (newest == XP_ENGINE_NONE ||
           engine.notes[i].serial > engine.notes[newest].serial))
        newest = (uint8_t)i;
    assert(newest != XP_ENGINE_NONE);
    for (i = 0; i < engine.notes[newest].slot_count; ++i) {
      const struct xp_portamento *g =
        &engine.slots[engine.notes[newest].slots[i]].component.portamento;
      assert(g->active);
      assert(g->current == 47u << 16);
      assert(g->target == 100u << 16);
    }
  }
  engine_destroy(&engine);
  renderer_destroy(&renderer);

  free(wave);
  free(control);
  return 0;
}
