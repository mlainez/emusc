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

struct sc88_tva_release {
  uint16_t current;
  uint16_t increment;
  uint16_t scale;
  bool scale_enabled;
  bool active;
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
bool sc88_tva_gain_from_headroom_q17(const struct sc88_rom *rom,
                                     uint16_t headroom,
                                     const struct sc88_tva_levels *levels,
                                     uint16_t static_attenuation,
                                     uint32_t *gain_q17);
bool sc88_tva_release_prepare(const struct sc88_rom *rom,
                              const struct sc88_tone *tone,
                              const struct sc88_component *component,
                              uint8_t selector_key,
                              struct sc88_tva_release *release);
bool sc88_tva_release_set_pedal(const struct sc88_rom *rom,
                                uint8_t hold1, bool continuous_hold,
                                bool keep_scale_at_zero,
                                bool sostenuto_retained,
                                struct sc88_tva_release *release);
bool sc88_tva_release_advance(struct sc88_tva_release *release,
                              unsigned elapsed_periods);

#ifdef __cplusplus
}
#endif

#endif
