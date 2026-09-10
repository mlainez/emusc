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

struct sc88_tva_envelope {
  uint32_t targets_q17[4];
  /* The stage words are attenuations, and the level tables convert them at
     about -5.26 dB per 0x1000 - so a stage ramps its **attenuation**
     linearly, which is an exponential decay in amplitude. Interpolating
     the gains instead leaves a five-second cymbal stage still at half
     amplitude after two and a half seconds (`M-016`). */
  uint16_t target_attenuations[4];
  uint16_t start_attenuation;
  uint16_t curve_words[4];
  uint16_t initial_phases[4];
  uint16_t increments[4];
  uint8_t stage;
  uint8_t saved_count;
  uint16_t phase;
  uint32_t start_q17;
  uint32_t current_q17;
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

/* Four-stage firmware clock, target gains, rate scaling and packed XP curve
 * words. Attack/decay modifiers are neutral in this entry point. The
 * continuous curve between service points is an XP operation and is exposed
 * separately as a provisional linear transfer. */
bool sc88_tva_envelope_prepare(const struct sc88_rom *rom,
                               const struct sc88_tone *tone,
                               const struct sc88_component *component,
                               uint8_t selector_key, uint8_t velocity,
                               struct sc88_tva_envelope *envelope);
bool sc88_tva_envelope_advance(const struct sc88_rom *rom,
                               struct sc88_tva_envelope *envelope,
                               unsigned elapsed_periods);
uint32_t sc88_tva_envelope_linear_q17(const struct sc88_rom *rom,
  const struct sc88_tva_envelope *envelope, double period_fraction);
void sc88_tva_envelope_freeze(const struct sc88_rom *rom,
                              struct sc88_tva_envelope *envelope,
                              double period_fraction);

#ifdef __cplusplus
}
#endif

#endif
