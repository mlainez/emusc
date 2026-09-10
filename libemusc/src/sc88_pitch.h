/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_SC88_PITCH_H
#define EMUSC_SC88_PITCH_H

#include "sc88_rom.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sc88_pitch_envelope {
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

struct sc88_pitch_release {
  int16_t destination;
  int16_t delta;
  int16_t current;
  uint16_t phase;
  uint16_t increment;
  uint16_t scale;
  bool scale_enabled;
  bool active;
};

/* The unresolved XP random readback is neutral (zero) in this entry point. */
bool sc88_pitch_envelope_prepare(const struct sc88_rom *rom,
                                 const struct sc88_tone *tone,
                                 const struct sc88_component *component,
                                 uint8_t selector_key, uint8_t velocity,
                                 struct sc88_pitch_envelope *envelope);
bool sc88_pitch_envelope_advance(struct sc88_pitch_envelope *envelope,
                                 unsigned elapsed_periods);
bool sc88_pitch_release_prepare(const struct sc88_rom *rom,
                                const struct sc88_tone *tone,
                                const struct sc88_component *component,
                                uint8_t selector_key, uint16_t envelope_depth,
                                struct sc88_pitch_release *release);
bool sc88_pitch_release_activate(const struct sc88_rom *rom,
                                 uint8_t hold1, bool continuous_hold,
                                 bool keep_scale_at_zero,
                                 bool sostenuto_retained,
                                 int16_t envelope_current,
                                 struct sc88_pitch_release *release);
bool sc88_pitch_release_advance(struct sc88_pitch_release *release,
                                unsigned elapsed_periods);

int16_t sc88_pitch_envelope_sum(const struct sc88_pitch_envelope *envelope,
                                const struct sc88_pitch_release *release);

/* The pitch word uploaded as the XP's current value: the static word plus
 * every signed contribution, with the envelope's own doubled, capped at
 * 0x0003ffff, and with bit 0 cleared. All three are firmware law rather than
 * XP behaviour - the doubling and the cap at 0x5e59..0x5e6a and
 * 0x5f11..0x5f25, and the low bit cleared before upload. Both the engine and
 * a renderer used on its own must arrive at the same word, so they share
 * this. */
uint32_t sc88_pitch_current_word(uint32_t base, int32_t offset,
                                 int16_t envelope_sum);

#ifdef __cplusplus
}
#endif

#endif
