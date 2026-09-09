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

/* accumulated_modulation is the signed word prepared before the base-table
 * lookup (key, envelope and dynamic modulation). Supplying zero preserves an
 * explicit seam while those producers are integrated. */
bool sc88_tvf_prepare_registers(const struct sc88_rom *rom,
                                const struct sc88_component *component,
                                int16_t accumulated_modulation,
                                const struct sc88_tvf_controls *controls,
                                struct sc88_tvf_registers *registers);

#ifdef __cplusplus
}
#endif

#endif
