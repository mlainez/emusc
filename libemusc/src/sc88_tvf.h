/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_SC88_TVF_H
#define EMUSC_SC88_TVF_H

#include "sc88_rom.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sc88_tvf_controls {
  uint8_t part_cutoff;
  uint8_t secondary_cutoff;
  uint8_t part_resonance;
  uint8_t secondary_resonance;
};

/* Exact CPU-prepared XP register state. The physical cutoff, resonance and
 * filter transfer represented by these words remain deliberately unnamed. */
struct sc88_tvf_registers {
  uint8_t cutoff_index;
  uint8_t resonance_index;
  uint16_t base_value;
  /* The saturated table entry plus key, controller and LFO terms as
     routine 6ccd holds it before the shift right one. The envelope and
     release are not part of it: the firmware adds those to the halved
     word (sc88_tvf_update_frequency). */
  uint16_t base_unshifted;
  uint16_t combined;
  uint32_t frequency_current;
  uint32_t frequency_target;
  uint16_t frequency_interpolation;
  uint32_t resonance_current;
  uint32_t resonance_target;
  uint16_t resonance_interpolation;
  uint16_t filter_select;
  bool fixed_tuple;
};

struct sc88_tvf_envelope {
  int16_t targets[4];
  uint16_t initial_phases[4];
  uint16_t increments[4];
  uint16_t depth;
  uint8_t stage;
  uint8_t saved_count;
  uint16_t phase;
  int16_t base;
  int16_t delta;
  int16_t current;
  bool active;
};

struct sc88_tvf_release {
  int16_t target;
  int16_t current;
  uint16_t phase;
  uint16_t increment;
  uint16_t scale;
  bool scale_enabled;
  bool active;
};

/* Replaceable audio-side interpretation of the still-undecoded XP words.
 * This state-variable topology is intentionally separate from the exact CPU
 * state above. */
/* Two-pole sections in cascade. The filter's order is not recovered from
   the ROM; the sibling chip measures two-pole on hardware (`M-054`). Three
   sections had been chosen by measurement while the cutoff word was read
   as a sine and sat at 8-10 kHz on every tone, a compensating fit that
   also cut a snare's content above 8 kHz from 30 % to 12 %; with the
   cutoff word read in its own log domain one section measures closest to
   the recordings (`M-105`). Resonance belongs to the first section. */
#define SC88_TVF_SECTIONS 1

struct sc88_tvf_audio_state {
  float integrator_band;
  float integrator_low;
  float section_band[SC88_TVF_SECTIONS];
  float section_low[SC88_TVF_SECTIONS];
};

typedef float (*sc88_tvf_audio_transfer_fn)(
  void *user, struct sc88_tvf_audio_state *state,
  const struct sc88_tvf_registers *registers,
  double period_fraction, float input);

/* pre_base_modulation is the wrapped key/dynamic word prepared before the
 * base-table lookup. Envelope and release modulation are added afterward by
 * sc88_tvf_update_frequency. */
bool sc88_tvf_prepare_registers(const struct sc88_rom *rom,
                                const struct sc88_component *component,
                                int16_t pre_base_modulation,
                                const struct sc88_tvf_controls *controls,
                                struct sc88_tvf_registers *registers);

/* Signed key-table word times signed component factor, retaining the product
 * high word and applying the firmware's final doubling. */
bool sc88_tvf_key_modulation(const struct sc88_rom *rom,
                             const struct sc88_tone *tone,
                             const struct sc88_component *component,
                             uint8_t selector_key, int16_t *modulation);

/* Exact note-on depth, key/velocity rate scaling, targets and CPU clock.
 * Attack/decay controller modifiers are neutral; soft_pedal selects the
 * recovered velocity reduction before the depth curve lookup. */
bool sc88_tvf_envelope_prepare(const struct sc88_rom *rom,
                               const struct sc88_tone *tone,
                               const struct sc88_component *component,
                               uint8_t selector_key, uint8_t velocity,
                               bool soft_pedal,
                               struct sc88_tvf_envelope *envelope);
bool sc88_tvf_envelope_advance(struct sc88_tvf_envelope *envelope,
                               unsigned elapsed_periods);
bool sc88_tvf_release_prepare(const struct sc88_rom *rom,
                              const struct sc88_tone *tone,
                              const struct sc88_component *component,
                              uint8_t selector_key, uint16_t envelope_depth,
                              struct sc88_tvf_release *release);
bool sc88_tvf_release_set_pedal(const struct sc88_rom *rom,
                                uint8_t hold1, bool continuous_hold,
                                bool keep_scale_at_zero,
                                bool sostenuto_retained,
                                struct sc88_tvf_release *release);
bool sc88_tvf_release_advance(struct sc88_tvf_release *release,
                              unsigned elapsed_periods);

/* Recompose TVF-F from an already prepared accumulator while retaining the
 * note-start Q/type tuple. Fixed negative-mode tuples remain unchanged. */
bool sc88_tvf_update_frequency(const struct sc88_rom *rom,
                               int16_t post_base_modulation,
                               struct sc88_tvf_registers *registers);
void sc88_tvf_latch_frequency(struct sc88_tvf_registers *registers);

/* Move the frequency and resonance registers toward their targets by their
 * own interpolation words, once per elapsed control period. */
void sc88_tvf_advance_registers(struct sc88_tvf_registers *registers,
                                unsigned periods);

/* The frequency a TVF-F register value asks for, in hertz at the chip's
 * own rate. Slope from the ROM, anchor inferred - see sc88_tvf.c. */
double sc88_tvf_word_to_hz(uint32_t word);

void sc88_tvf_audio_reset(struct sc88_tvf_audio_state *state);
float sc88_tvf_audio_process_provisional(
  void *user, struct sc88_tvf_audio_state *state,
  const struct sc88_tvf_registers *registers,
  double period_fraction, float input);

#ifdef __cplusplus
}
#endif

#endif
