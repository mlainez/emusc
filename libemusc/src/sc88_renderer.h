/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_SC88_RENDERER_H
#define EMUSC_SC88_RENDERER_H

#include "sc88_oscillator.h"
#include "sc88_rom.h"
#include "sc88_tva.h"

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
};

struct sc88_render_component {
  int32_t *pcm24;
  size_t pcm_count;
  struct sc88_oscillator oscillator;
  uint16_t static_attenuation;
  uint32_t static_gain_q17;
  bool active;
};

struct sc88_render_voice {
  struct sc88_render_component components[SC88_MAX_TONE_COMPONENTS];
  unsigned component_count;
  uint32_t tone_offset;
  uint8_t key;
  uint8_t velocity;
  float provisional_gain;
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

/* This first vertical path deliberately accepts an explicit dry gain. It does
 * not claim to replace the pending TVA, pan, filter, allocator or effects.
 * A gain of 1.0 passes one component at decoded full scale. */
bool sc88_renderer_note_on(const struct sc88_renderer *renderer,
                           struct sc88_render_voice *voice,
                           uint8_t variation, uint8_t program,
                           uint8_t key, uint8_t velocity,
                           float provisional_gain);
bool sc88_renderer_note_on_with_levels(
  const struct sc88_renderer *renderer, struct sc88_render_voice *voice,
  uint8_t variation, uint8_t program, uint8_t key, uint8_t velocity,
  float provisional_gain, const struct sc88_tva_levels *levels);
void sc88_renderer_voice_destroy(struct sc88_render_voice *voice);
bool sc88_renderer_voice_active(const struct sc88_render_voice *voice);

/* Interleaved stereo dry output. Components are summed equally to both
 * channels; no clipping is applied because XP summing precision is open. */
size_t sc88_renderer_render(struct sc88_render_voice *voice,
                            float *stereo, size_t frames);

#ifdef __cplusplus
}
#endif

#endif
