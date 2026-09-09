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

/* Recompose TVF-F from an already prepared accumulator while retaining the
 * note-start Q/type tuple. Fixed negative-mode tuples remain unchanged. */
bool sc88_tvf_update_frequency(const struct sc88_rom *rom,
                               int16_t post_base_modulation,
                               struct sc88_tvf_registers *registers);
void sc88_tvf_latch_frequency(struct sc88_tvf_registers *registers);

#ifdef __cplusplus
}
#endif

#endif
