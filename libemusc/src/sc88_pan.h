/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_SC88_PAN_H
#define EMUSC_SC88_PAN_H

#include "sc88_rom.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sc88_pan_controls {
  uint8_t master;
  uint8_t part;
};

/* Fixed melodic pan only. Part pan zero requests XP-derived random pan and
 * returns false until that sound-chip random source is implemented. */
bool sc88_pan_static_q15(const struct sc88_rom *rom,
                         const struct sc88_tone *tone,
                         const struct sc88_component *component,
                         uint8_t selector_key,
                         const struct sc88_pan_controls *controls,
                         uint8_t *position, uint16_t *left_q15,
                         uint16_t *right_q15);

#ifdef __cplusplus
}
#endif

#endif
