/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_SC88_RENDERER_H
#define EMUSC_SC88_RENDERER_H

#include "sc88_oscillator.h"
#include "sc88_pan.h"
#include "sc88_pitch.h"
#include "sc88_rom.h"
#include "sc88_tva.h"
#include "sc88_tvf.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SC88_WAVE_BANK_COUNT 8u
#define SC88_MAX_TONE_COMPONENTS 2u

struct sc88_wave_bank {
  uint8_t selector;
  const uint8_t *bytes;
  size_t size;
};

struct sc88_renderer {
  struct sc88_rom rom;
  struct sc88_wave_bank banks[SC88_WAVE_BANK_COUNT];
  double output_rate;
  enum sc88_fractional_wrap wrap;
  struct sc88_tva_levels levels;
  struct sc88_pan_controls pan;
  struct sc88_tvf_controls tvf_controls;
  sc88_tvf_audio_transfer_fn tvf_audio_transfer;
  void *tvf_audio_user;
};

struct sc88_render_component {
  int32_t *pcm24;
  size_t pcm_count;
  struct sc88_oscillator oscillator;
  uint32_t static_pitch_word;
  struct sc88_pitch_envelope pitch_envelope;
  struct sc88_pitch_release pitch_release;
  /* A rhythm note's own reverb send from its kit record, 127 for a
     melodic note. The kits are not uniformly treated - STANDARD 1 sends
     its snare and cymbals at 127 and its kick at 0 - and flattening them
     to one part send is audible as a kit with no depth (`M-009`). */
  uint8_t reverb_send;
  /* the kit's `+0x400`, or 127 for a melodic note */
  uint8_t chorus_send;
  uint16_t static_attenuation;
  uint32_t static_gain_q17;
  struct sc88_tva_envelope envelope;
  struct sc88_tva_release release;
  struct sc88_tvf_registers tvf;
  struct sc88_tvf_envelope tvf_envelope;
  struct sc88_tvf_release tvf_release;
  struct sc88_tvf_audio_state tvf_audio;
  int16_t tvf_key_modulation;
  uint32_t rom_component_offset;
  bool continuous_hold_release;
  bool keep_release_scale_at_zero;
  int16_t pan_component_offset;
  uint8_t pan_position;
  uint8_t pan_target_position;
  uint16_t left_gain_q15;
  uint16_t right_gain_q15;
  bool active;
};

struct sc88_render_voice {
  struct sc88_render_component components[SC88_MAX_TONE_COMPONENTS];
  unsigned component_count;
  uint32_t tone_offset;
  uint8_t key;
  uint8_t velocity;
  float provisional_gain;
  /* A rhythm note whose kit record clears bit 0 of `+0x480` does not
     receive Note Off and rings to its own end. Every drum in this song is
     written as a 10 ms note, so honouring Note Off turns a crash into a
     tick (`M-015`). */
  bool ignore_note_off;
  sc88_tvf_audio_transfer_fn tvf_audio_transfer;
  void *tvf_audio_user;
};

/* Firmware key transform at SC88-CTL 0x60c7..0x6123. */
uint8_t sc88_renderer_selector_key(const struct sc88_component *component,
                                   uint8_t midi_key);

/* Static note-on pitch before controllers, LFOs and the pitch envelope. */
bool sc88_renderer_static_pitch_word(const struct sc88_rom *rom,
                                     const struct sc88_tone *tone,
                                     const struct sc88_component *component,
                                     const struct sc88_wave_descriptor *desc,
                                     uint8_t selector_key,
                                     uint32_t *pitch_word);

bool sc88_renderer_init(struct sc88_renderer *renderer,
                        const uint8_t *control_rom, size_t control_rom_size,
                        const struct sc88_wave_bank *banks, size_t bank_count,
                        double output_rate, enum sc88_fractional_wrap wrap);
void sc88_renderer_set_levels(struct sc88_renderer *renderer,
                              const struct sc88_tva_levels *levels);
void sc88_renderer_set_pan(struct sc88_renderer *renderer,
                           const struct sc88_pan_controls *pan);
void sc88_renderer_set_tvf_audio_transfer(
  struct sc88_renderer *renderer, sc88_tvf_audio_transfer_fn transfer,
  void *user);
void sc88_renderer_set_tvf_controls(
  struct sc88_renderer *renderer, const struct sc88_tvf_controls *controls);

/* The explicit gain is temporary output trim. Static TVA, release, pan and
 * exact TVF control state are native ROM paths; the audio-side TVF callback
 * and effects remain explicit seams. */
bool sc88_renderer_note_on(const struct sc88_renderer *renderer,
                           struct sc88_render_voice *voice,
                           uint8_t variation, uint8_t program,
                           uint8_t key, uint8_t velocity,
                           float provisional_gain);
bool sc88_renderer_note_on_with_levels(
  const struct sc88_renderer *renderer, struct sc88_render_voice *voice,
  uint8_t variation, uint8_t program, uint8_t key, uint8_t velocity,
  float provisional_gain, const struct sc88_tva_levels *levels);
bool sc88_renderer_note_on_with_controls(
  const struct sc88_renderer *renderer, struct sc88_render_voice *voice,
  uint8_t variation, uint8_t program, uint8_t key, uint8_t velocity,
  float provisional_gain, const struct sc88_tva_levels *levels,
  const struct sc88_pan_controls *pan);
bool sc88_renderer_note_on_with_part_controls(
  const struct sc88_renderer *renderer, struct sc88_render_voice *voice,
  uint8_t variation, uint8_t program, uint8_t key, uint8_t velocity,
  float provisional_gain, const struct sc88_tva_levels *levels,
  const struct sc88_pan_controls *pan,
  const struct sc88_tvf_controls *tvf_controls);
/* A rhythm-part note. The kit record supplies the tone, the key it is played
 * at, and this key's own level and pan, so a kick is not a sample transposed
 * to whatever key triggered it. `note` receives the whole record when the
 * caller wants its sends or assign group. */
bool sc88_renderer_note_on_drum(
  const struct sc88_renderer *renderer, struct sc88_render_voice *voice,
  uint8_t map, uint8_t program, uint8_t key, uint8_t velocity,
  float provisional_gain, const struct sc88_tva_levels *levels,
  const struct sc88_pan_controls *pan,
  const struct sc88_tvf_controls *tvf_controls,
  struct sc88_drum_note *note);

void sc88_renderer_voice_destroy(struct sc88_render_voice *voice);
bool sc88_renderer_voice_active(const struct sc88_render_voice *voice);

/* Interleaved stereo dry output. No clipping is applied because XP summing
 * precision is open. */
size_t sc88_renderer_render(struct sc88_render_voice *voice,
                            float *stereo, size_t frames);

#ifdef __cplusplus
}
#endif

#endif
