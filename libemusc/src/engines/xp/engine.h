/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_XP_ENGINE_H
#define EMUSC_XP_ENGINE_H

#include "renderer.h"

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
  /* The filter LFO term standing in the slot's pre-base accumulator.
     The base recompose reads it to tell a period that changed it from
     one that did not; a note opens at zero because the oscillators open
     with a cleared waveform word (`0x250d`, `0x257a`). */
  int16_t tvf_lfo_term;
};

/* A voice the CPU has taken the slot back from while it was still
   sounding, carrying on into the chip's stop ramp.

   UNVERIFIED, and it has to be said plainly: `06_voice_engine/allocation.md`
   separates two things a same-note recycle does, and only one of them is
   recovered. The slot is freed and prepended through `0x2333` so it is the
   next slot allocated - independently traced in SC88-CTL, and the free list
   above keeps it exactly as traced. The other half is
   `request_voice_recycle` at `0x4c60`, which "either starts an XP stop
   transaction for an enabled slot or queues it, ... distinct from the
   normal envelope-release initializer", and
   `03_disassembly/functions.md` records that stop law as UNRESOLVED. What
   the chip does to the SOUND when the CPU hands a sounding voice back is
   not read off the device.

   Clearing the voice along with its slot is one reading of that law, and
   it is the reading that ends the waveform at whatever sample value the
   voice stands at. On a roll - one key struck 46 times in five seconds -
   that is a step per stroke: audible as a crackle under loud material, and
   5.6x the SCVA oracle's spikiness on the same file at the 99th percentile.
   These entries are the other reading. The CPU stops servicing the voice
   the moment it takes the slot, as the trace says, and the chip's amplitude
   register is given the target zero, the voice sounding on until the
   register arrives - which is what the note-end path at `7228..7232` does
   already, with the same interpolation word.

   Nothing here is a new shape: the ramp is `sc88_render_static_gain_q17`,
   the register model already in `renderer.h`, evaluated on this
   voice's own clock instead of the control period's. Its time constant is
   therefore not fitted - it is `0x2a7`, about 0.75 ms, the word the
   firmware writes beside every amplitude target, which is long enough to
   remove a step at 32 kHz and far too short to be heard as a decay.

   What this is not is a claim about the chip's voice channel. The slot is
   reallocated at once - that is the traced half - so on the device the stop
   and the note that took the slot contend for the same register block, and
   which of them the block is carrying while the stop runs is precisely what
   `0x4c60`'s queue decides and what is not recovered. Here the ramp runs
   beside the new voice.

   Two sibling devices support the direction and neither settles it: on the
   SC-55mkII, measured on hardware, a retriggered drum key "lets the older
   voice ring on to its natural end" (`part.cc`, P-0283), and on the JV-880,
   traced in firmware, "every drum hit holds its voice until its envelope
   ends, a repeated key included" (scdb D-44). Both hold the voice for its
   whole envelope, which is much longer than this; both are other machines.
   What would settle the SC-88's own law is recovering the XP stop
   transaction at `0x4c60` (`P-0358`).

   The pool is separate from the slots on purpose: a stopping voice is NOT
   a slot, is not counted by `sc88_engine_active_slots`, and cannot delay or
   reorder an allocation. A full pool ends the voice where it stands: the
   stop is what the song can spare, never the allocation. */
#define SC88_ENGINE_STOPPING_COUNT 32u

struct sc88_engine_stopping {
  struct sc88_render_component component;
  /* Everything the CPU had composed for this voice except the amplitude
     register - the TVA envelope, the amplitude oscillators and the note's
     own gain - frozen at the instant of the stop, because after it nothing
     services the voice. Holding the product is also what makes the first
     ramped sample continue the last serviced one exactly. */
  float gain;
  /* Control periods since the stop was written: the clock the register's
     approach to zero runs on. */
  double periods;
  /* The part whose effect sends the voice still goes through. Kept here
     because the note record is freed with the slot. */
  uint8_t part;
  bool active;
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
  /* Nonzero makes this a rhythm part, and names which of the machine's two
     drum setups it plays from: GS's Use For Rhythm Part, `40 1x 15`,
     00 off / 01 MAP1 / 02 MAP2. A drum setup is a working copy of a kit
     with its own per-note overrides, so this is what `41 mf rr` and the
     NRPN drum block address - it does NOT choose between the SC-55 and
     SC-88 kits, which is `tone_map` below. The firmware keeps the two
     apart the same way: `474b` turns the value into bits 4 and 5 of the
     part's flags byte and `4789` uses bit 5 alone to point the part at one
     of two kit working areas. */
  uint8_t rhythm_setup;
  /* Which kit set a rhythm part's program indexes, and which bank a
     melodic one's does: `SC88_TONE_MAP_SC55` or `SC88_TONE_MAP_SC88`.
     The firmware derives it from the part's bank word at `d820` - the
     forcing byte CC32 writes if that is nonzero, otherwise the part's
     selected map - and the drum lookup at `2e7a` indexes
     `0x2fd00 + (map - 1) * 128 + program` with it. */
  uint8_t tone_map;
  /* The part's reverb send, 0..127 as received. The kit records carry a
     per-note send too (`M-009`), which is why this is applied per slot
     rather than to the finished mix. */
  uint8_t reverb_send;
  uint8_t chorus_send;
  uint8_t delay_send;
  /* `(depth * value) >> 2` summed over the controller matrix's
     sources, of which only modulation is modelled. */
  uint16_t lfo1_pitch_depth;
  /* Portamento, as the firmware holds it: the switch is bit 5 of the part
     flags at `DP:e59e + part` (`0x1eb0`/`0x1eb5`), the time is the raw CC5
     byte at `DP:d6e0 + part` (`0x31a6`) and `portamento_control` is CC84's
     source key at `DP:d8a0 + part` (`0x3224`), 0xff when unset. That one is
     a ONE-SHOT: `0x2c72` reads it into the queued note event and `0x2c76`
     puts 0xff back the same instant, so it names the source of exactly one
     note. */
  bool portamento;
  uint8_t portamento_time;
  uint8_t portamento_control;
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

/* Per-stage taps for one render, each a mono sum over all sounding
 * components at that point in the chain. They exist to locate a defect
 * at a stage instead of inferring it from the output: the difference
 * between two adjacent taps is exactly what that stage did.
 *
 * A NULL pointer skips that tap; the struct itself may be NULL.
 */
struct sc88_engine_stage_taps {
  float *oscillator;   /* sample as the oscillator produced it */
  float *after_tvf;    /* ... through the filter */
  float *after_static; /* ... times the component's static gain */
  float *after_tva;    /* ... times the TVA envelope */
  float *after_lfo;    /* ... times the amplitude LFO and note gain */
};

struct sc88_engine {
  const struct sc88_renderer *renderer;
  struct sc88_engine_shared_lfo shared_lfo[SC88_ENGINE_SHARED_LFO_COUNT];
  struct sc88_engine_note notes[SC88_ENGINE_NOTE_COUNT];
  struct sc88_engine_slot slots[SC88_ENGINE_SLOT_COUNT];
  struct sc88_engine_part parts[SC88_ENGINE_PART_COUNT];
  struct sc88_engine_stopping stopping[SC88_ENGINE_STOPPING_COUNT];
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
  struct sc88_engine_stage_taps stage_taps;
  void *control_user;
};

/* Compatibility surface for callers not yet ported to the EmuSC::Xp API
 * below (sc88_device.c, which embeds struct sc88_engine by value, and
 * sc88_engine_test.c). Each forwards to the real implementation in
 * namespace EmuSC::Xp. */
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
                                 uint8_t setup);
void sc88_engine_set_part_tone_map(struct sc88_engine *engine, uint8_t part,
                                   uint8_t map);
bool sc88_engine_set_drum_parameter(struct sc88_engine *engine,
                                    uint8_t setup, uint8_t field,
                                    uint8_t note, uint8_t value);
void sc88_engine_clear_drum_overlay(struct sc88_engine *engine,
                                    uint8_t setup);
void sc88_engine_set_part_delay_send(struct sc88_engine *engine,
                                     uint8_t part, uint8_t send);
void sc88_engine_set_part_chorus_send(struct sc88_engine *engine,
                                     uint8_t part, uint8_t send);
void sc88_engine_set_part_lfo1_pitch_depth(struct sc88_engine *engine,
                                           uint8_t part, uint16_t depth);
void sc88_engine_set_part_reverb_send(struct sc88_engine *engine,
                                      uint8_t part, uint8_t send);
void sc88_engine_set_part_portamento(struct sc88_engine *engine, uint8_t part,
                                     bool enabled);
void sc88_engine_set_part_portamento_time(struct sc88_engine *engine,
                                          uint8_t part, uint8_t time);
void sc88_engine_set_part_portamento_control(struct sc88_engine *engine,
                                             uint8_t part, uint8_t key);
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
void sc88_engine_render(struct sc88_engine *engine, float *stereo,
                        size_t frames);
void sc88_engine_set_stage_taps(struct sc88_engine *engine,
                                const struct sc88_engine_stage_taps *taps);
void sc88_engine_render_with_send(struct sc88_engine *engine, float *stereo,
                                  float *send, float *chorus_send,
                                  float *delay_send, size_t frames);

#ifdef __cplusplus
}

namespace EmuSC { namespace Xp {

// Voice engine (allocation, scheduling, mixing) for the XP-generation-1
// engine (see engines/xp/README.md). The plain C types above are shared,
// unrenamed, with sc88_device.c, which embeds struct sc88_engine by value
// and is not yet converted to C++.
//
// Three concerns share this file rather than splitting into PartState,
// VoiceAllocator and VoiceScheduler, as originally hypothesised: all three
// read and write the same sc88_engine_note/sc88_engine_slot/
// sc88_engine_part arrays, which are shaped by the ABI constraint above,
// not by a design choice this file is free to make. The boundary is
// visible in this file's own section comments instead - allocation
// (init, note_on/note_off, free/pop list bookkeeping), part state
// (set_part_*, held for sc88_device.c's controller writes), and the
// scheduler (run_scheduler, render_with_send, the shared-LFO table) - so
// that splitting them out is mechanical once sc88_device.c itself
// converts (T17) and this struct can become real member state.

bool engine_init(struct sc88_engine *engine,
                  const struct sc88_renderer *renderer);
void engine_destroy(struct sc88_engine *engine);
void engine_set_control_service(struct sc88_engine *engine,
                                 sc88_control_service_fn service, void *user);
void engine_set_part_levels(struct sc88_engine *engine, uint8_t part,
                             const struct sc88_tva_levels *levels);
void engine_set_part_pan(struct sc88_engine *engine, uint8_t part,
                          const struct sc88_pan_controls *pan);
void engine_set_part_pitch_offset(struct sc88_engine *engine, uint8_t part,
                                   int32_t pitchOffset);
void engine_set_part_lfo_controls(struct sc88_engine *engine, uint8_t part,
                                   const struct sc88_lfo_controls *controls);
void engine_set_part_tva_controls(struct sc88_engine *engine, uint8_t part,
                                   const struct sc88_tva_controls *controls);
void engine_set_part_tvf_controls(struct sc88_engine *engine, uint8_t part,
                                   const struct sc88_tvf_controls *controls);
/* `setup` is Use For Rhythm Part: 0 off, 1 MAP1, 2 MAP2. */
void engine_set_part_rhythm(struct sc88_engine *engine, uint8_t part,
                             uint8_t setup);
void engine_set_part_tone_map(struct sc88_engine *engine, uint8_t part,
                               uint8_t map);
/* One `41 mf rr` write: `setup` 1 or 2, `field` 1..9, `note` 0..127. */
bool engine_set_drum_parameter(struct sc88_engine *engine, uint8_t setup,
                                uint8_t field, uint8_t note, uint8_t value);
/* Changing a rhythm part's kit clears them, as the firmware does. */
void engine_clear_drum_overlay(struct sc88_engine *engine, uint8_t setup);
void engine_set_part_delay_send(struct sc88_engine *engine, uint8_t part,
                                 uint8_t send);
void engine_set_part_chorus_send(struct sc88_engine *engine, uint8_t part,
                                  uint8_t send);
void engine_set_part_lfo1_pitch_depth(struct sc88_engine *engine,
                                       uint8_t part, uint16_t depth);
void engine_set_part_reverb_send(struct sc88_engine *engine, uint8_t part,
                                  uint8_t send);
/* CC65 past its 64 threshold, CC5 raw, and CC84's source key (0xff none). */
void engine_set_part_portamento(struct sc88_engine *engine, uint8_t part,
                                 bool enabled);
void engine_set_part_portamento_time(struct sc88_engine *engine, uint8_t part,
                                      uint8_t time);
void engine_set_part_portamento_control(struct sc88_engine *engine,
                                         uint8_t part, uint8_t key);
bool engine_note_on(struct sc88_engine *engine, uint8_t part,
                     uint8_t variation, uint8_t program, uint8_t key,
                     uint8_t velocity, uint8_t context,
                     enum sc88_same_note_mode mode, float provisionalGain);
bool engine_note_off(struct sc88_engine *engine, uint8_t part, uint8_t key);
void engine_hold(struct sc88_engine *engine, uint8_t part, bool enabled);
void engine_hold_value(struct sc88_engine *engine, uint8_t part,
                        uint8_t value);
void engine_sostenuto(struct sc88_engine *engine, uint8_t part, bool enabled);

unsigned engine_active_slots(const struct sc88_engine *engine);
unsigned engine_released_slots(const struct sc88_engine *engine);

/* Interleaved stereo dry output. The callback receives firmware-equivalent
 * catch-up counts whenever the 10,001-clock (8.0008 ms) service is due. */
void engine_render(struct sc88_engine *engine, float *stereo, size_t frames);

void engine_set_stage_taps(struct sc88_engine *engine,
                            const struct sc88_engine_stage_taps *taps);

/* As above, and also accumulates the two mono effect send buses, each of
 * which must hold `frames` samples when given. Either may be NULL. */
void engine_render_with_send(struct sc88_engine *engine, float *stereo,
                              float *send, float *chorusSend,
                              float *delaySend, size_t frames);

}}  // namespace EmuSC::Xp
#endif

#endif
