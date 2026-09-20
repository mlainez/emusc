/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_SC88_LFO_H
#define EMUSC_SC88_LFO_H

#include "rom.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The delay/fade ramp whose fade word multiplies every modulation depth the
 * oscillator reaches - pitch, TVF and TVA alike. */
struct sc88_lfo_ramp {
  uint16_t delay_phase;
  uint16_t delay_increment;
  uint16_t fade;
  uint16_t fade_increment;
};

/* One oscillator. A voice owns a tone-common one and a component-local one;
 * which of the two a voice owns outright and which it shares with another
 * voice on the same tone is a lifecycle question, so `share_request` is
 * carried here for the engine and nothing in this module acts on it. */
struct sc88_lfo {
  uint8_t selector;              /* even byte offset into the ROM's table */
  uint8_t share_request;         /* tone +18 / component +08 */
  uint16_t phase;
  uint16_t base_increment;
  int16_t output;
  int16_t random_target;
  struct sc88_lfo_ramp ramp;
};

/* The part-level vibrato controls, each centred at 64. Rate and delay are
 * the two modifiers `sc88_lfo_common_rate_index` and `_delay_index` take;
 * depth is added to the oscillator's own pitch depth.
 */
struct sc88_lfo_controls {
  uint8_t rate;
  uint8_t delay;
  uint8_t depth;
};

/* The live rate contribution, from the cached controller and poly-pressure
 * word the part composes. Clipped before doubling, and the signed product's
 * high word is kept, which floors rather than truncating. */
bool sc88_lfo_rate_control(int16_t routed, int16_t *out);

/* Tone-common byte +1a plus twice the two centred 7-bit modifiers, and the
 * delay index the same way - a negative +1b bypassing the table entirely. */
bool sc88_lfo_common_rate_index(unsigned tone_rate, unsigned part_rate,
                                unsigned user_rate, uint8_t *out);
bool sc88_lfo_common_delay_index(int tone_delay, unsigned part_delay,
                                 unsigned user_delay, int16_t *out);

/* The base increment and the live contribution add with 16-bit wrap before a
 * signed test: a total at or below zero stops the oscillator rather than
 * reversing it, and a positive one saturates at the table's largest entry. */
bool sc88_lfo_effective_increment(uint16_t base, int16_t control,
                                  uint16_t *out);

bool sc88_lfo_common_prepare(const struct sc88_rom *rom,
                             const struct sc88_tone *tone,
                             unsigned part_rate, unsigned user_rate,
                             unsigned part_delay, unsigned user_delay,
                             struct sc88_lfo *lfo);
bool sc88_lfo_local_prepare(const struct sc88_rom *rom,
                            const struct sc88_component *component,
                            struct sc88_lfo *lfo);

/* Advance the phase `catchup_count + 1` times, then evaluate exactly one
 * waveform callback - the order the firmware uses. `seed` is the global
 * random word, shared by every oscillator, whose power-on value is not
 * recovered. A zero effective increment leaves phase and output alone. */
bool sc88_lfo_advance(const struct sc88_rom *rom, struct sc88_lfo *lfo,
                      int16_t rate_control, uint8_t catchup_count,
                      uint16_t *seed);

/* The ramp runs on its own clock: it is not gated by a stalled oscillator. */
bool sc88_lfo_ramp_initialize(uint16_t delay_increment,
                              uint16_t fade_increment,
                              struct sc88_lfo_ramp *ramp);
bool sc88_lfo_ramp_activate_immediate(struct sc88_lfo_ramp *ramp);
bool sc88_lfo_ramp_advance(struct sc88_lfo_ramp *ramp, uint8_t catchup_count);

/* The individual shapes, exposed because the dispatch table is firmware
 * contract even where the held tone graph selects no entry for them. */
int16_t sc88_lfo_square(uint16_t phase);
int16_t sc88_lfo_triangle(uint16_t phase);
int16_t sc88_lfo_rectified_triangle(uint16_t phase);
int16_t sc88_lfo_slew_random(int16_t current, int16_t target);
uint16_t sc88_lfo_random_target(uint16_t seed, uint16_t phase);
bool sc88_lfo_phase_advance(uint16_t increment, uint8_t catchup_count,
                            uint16_t *phase, uint16_t *seed,
                            uint16_t *target);
bool sc88_lfo_table_sample(const struct sc88_rom *rom, uint32_t table,
                           uint16_t phase, uint16_t increment, int16_t *out);
bool sc88_lfo_waveform(const struct sc88_rom *rom, uint8_t selector,
                       uint16_t phase, uint16_t increment, int16_t previous,
                       int16_t target, int16_t *out);

/* i * 124.987501249875 / 65536 Hz, from the recovered timer configuration. */
double sc88_lfo_frequency(uint16_t increment);

#ifdef __cplusplus
}
#endif

#endif
