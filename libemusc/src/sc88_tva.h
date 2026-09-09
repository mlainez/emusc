/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_SC88_TVA_H
#define EMUSC_SC88_TVA_H

#include "sc88_rom.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sc88_tva_levels {
  uint8_t master;
  uint8_t secondary;
  uint8_t part;
  uint8_t expression;
};

/* Exact CPU-side note-on AmpM path. The returned linear XP gain is Q17 with
 * 0x20000 as unity. Envelope Amp, modulation and XP ramp precision are
 * separate stages and are deliberately not folded into this value. */
bool sc88_tva_static_gain_q17(const struct sc88_rom *rom,
                              const struct sc88_tone *tone,
                              const struct sc88_component *component,
                              const struct sc88_zone_selection *zone,
                              uint8_t selector_key, uint8_t velocity,
                              const struct sc88_tva_levels *levels,
                              uint16_t *static_attenuation,
                              uint32_t *gain_q17);

#ifdef __cplusplus
}
#endif

#endif
