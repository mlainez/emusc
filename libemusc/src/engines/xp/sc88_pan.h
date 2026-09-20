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
  /* Part pan zero is a random-pan request, not a position. The firmware
     returns ff from composition and then takes a position from an XP
     readback, masked to seven bits with zero rejected, so 1..127. The
     caller supplies that draw here; the chip's distribution and seeding
     are not in the CPU path (`09_mixer/mixer_output.md`). */
  uint8_t random_position;
};

bool sc88_pan_component_offset(const struct sc88_rom *rom,
                               const struct sc88_tone *tone,
                               const struct sc88_component *component,
                               uint8_t selector_key, int16_t *offset);
/* The 127-word curve the pan pair is read from is also the curve the
 * reverb and chorus sends are read from, once each rather than twice
 * (`08_effects/routing.md`). It is monotone from silence at control 1 to
 * unity at 127 and is deliberately not a straight line, so a send may not
 * be modelled as `control / 127`. Control 0 is silence.
 */
bool sc88_control_gain_q15(const struct sc88_rom *rom, uint8_t control,
                           uint16_t *gain_q15);

/* The 0..127 send a rhythm voice ends up with, from its part's control and
 * its own kit record's: the firmware's rounded unsigned product
 * `((part * note) + 127) >> 7`, which maps 0 to 0 and 127x127 to 127. A
 * melodic voice passes 127 as `note` and so keeps its part's control.
 */
uint8_t sc88_send_combine(uint8_t part, uint8_t note);

bool sc88_pan_pair_q15(const struct sc88_rom *rom, uint8_t position,
                       uint16_t *left_q15, uint16_t *right_q15);

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
