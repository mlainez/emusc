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

/* Portamento. The machine glides a KEY, not a pitch: `0x245a + slot` and
 * `0x24da + slot` hold one 32-bit value whose high word is the MIDI key the
 * pitch composition is run for and whose low word is the position between
 * that key and the next, so one semitone is exactly 0x10000. `0x255a + slot`
 * holds the target and two bits of `0x177c + slot` hold the state: bit 7 the
 * glide, bit 6 its direction.
 *
 * The rate is one 32-bit table entry per control period in that same unit,
 * added or subtracted with carry (`0x5ffe..0x6010`), the whole thing repeated
 * once per catch-up period by `scb/f` at `0x6018`. Nothing is divided, so
 * unlike the SC-55's 16-bit spend (emusc-match TASK-113) there is no
 * remainder to carry - provided the accumulator stays in this unit and is not
 * converted to cents on the way.
 *
 * `key_table`, `fixed`, `key_factor`, `key_transpose` and `root_key` are what
 * the static pitch word has to be recomposed from while the key moves, since
 * `0x6077` recomposes it from `0x19fc`/`0x197c` every control period rather
 * than offsetting a value latched at note-on. */
struct sc88_portamento {
  uint32_t current;             /* 16.16 MIDI key; 0x245a / 0x24da */
  uint32_t target;              /* 16.16 MIDI key; 0x255a */
  uint32_t rate;                /* the table entry, same unit per period */
  uint32_t key_table;           /* the tone's per-key pitch table in ROM */
  int32_t fixed;                /* the key-independent terms of the word */
  int16_t key_factor;           /* component +0x14 */
  int8_t key_transpose;         /* component +0x16 */
  uint8_t root_key;             /* the zone descriptor's root key */
  bool active;                  /* 0x177c bit 7 */
  bool ascending;               /* 0x177c bit 6 */
};

/* The rate table: 128 big-endian 32-bit entries at SC88-CTL 0x78502, indexed
 * by the raw CC5 byte (`0x5fdb` loads it from `DP:d6e0 + part`, `extu.b`s it
 * and shifts left twice for the four-byte stride, then reads `@(0x8502,r5)`
 * and `@(0x8504,r5)` with EP = 7). The table ends at 0x78702, where unrelated
 * data begins. Entry 0 is 0xffffffff and is never reached: both `0x5fa2` and
 * `0x5fdf` test the time byte for zero first and take the no-glide exit, so
 * CC5 = 0 does not glide at all. This returns 0 there, which
 * `sc88_portamento_advance` treats as that same exit. */
uint32_t sc88_portamento_rate(const struct sc88_rom *rom, uint8_t time);

/* One service of the glide, `0x5fd1..0x602d`, over `elapsed_periods` control
 * periods. The device compares only the high words, which is the same test as
 * comparing the pair while the target's own fraction is zero - it always is,
 * a target being a whole key. */
void sc88_portamento_advance(struct sc88_portamento *portamento,
                             unsigned elapsed_periods);

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
 * every signed contribution, with the envelope's own doubled, saturated to
 * 0x0003ffff, and with bit 0 cleared. All three are firmware law rather than
 * XP behaviour - the doubling and the saturation at 0x5e59..0x5e6a and
 * 0x5f11..0x5f25 (byte-identical clamps), and the low bit cleared before
 * upload. The saturation is one-sided on purpose: the compare is unsigned
 * and on the high word alone, so a sum that has gone negative saturates to
 * the MAXIMUM like an overlarge one, not to zero. Both the engine and a
 * renderer used on its own must arrive at the same word, so they share
 * this. */
uint32_t sc88_pitch_current_word(uint32_t base, int32_t offset,
                                 int16_t envelope_sum);

#ifdef __cplusplus
}
#endif

#endif
