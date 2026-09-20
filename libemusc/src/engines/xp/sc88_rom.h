/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_SC88_ROM_H
#define EMUSC_SC88_ROM_H

#include "sc88_wave.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SC88_CONTROL_ROM_SIZE 0x80000u
#define SC88_TONE_COMMON_SIZE 34u
#define SC88_COMPONENT_SIZE 148u

struct sc88_rom {
  const uint8_t *bytes;
  size_t size;
};

struct sc88_tone {
  const uint8_t *common;
  uint32_t offset;
  uint8_t component_count;
};

struct sc88_component {
  const uint8_t *bytes;
  uint32_t offset;
  uint32_t directory_offset;
};

struct sc88_zone_selection {
  uint8_t boundary;
  uint16_t static_attenuation;
  uint32_t descriptor_offset;
  struct sc88_wave_descriptor descriptor;
};

/* The offsets in this view are for the held SC-88 control ROM v1.01. The ROM
 * memory remains owned by the caller and must outlive the view. */
bool sc88_rom_init(struct sc88_rom *rom, const uint8_t *bytes, size_t size);

/* SC-88 native map (the firmware's map 2), GS variation/CC0 and zero-based
 * program. Null lookup entries return false; fallback policy is stateful and
 * deliberately belongs above this immutable ROM layer. */
/* A rhythm part's note. Every field is a per-note byte from the kit record,
 * whose structure is verified against the ROM: twenty-four BE24 pointers at
 * 0x2b550 all equal 0x23c30 + index * 0x50c, and all twenty-four names at
 * +0x500 match the documented kit list. */
struct sc88_drum_note {
  uint32_t tone_offset;          /* kit + note * 3; zero means an empty slot */
  uint8_t play_note;             /* +0x180, the key the tone is played at */
  uint8_t level;                 /* +0x200 */
  uint8_t assign_group;          /* +0x280 */
  uint8_t pan;                   /* +0x300 */
  uint8_t reverb_send;           /* +0x380 */
  uint8_t chorus_send;           /* +0x400 */
  uint8_t flags;                 /* +0x480, receive and exclusivity bits */
};

/* The tone map, which selects the kit set a rhythm part's program indexes
 * exactly as it selects a melodic part's bank: map 1 holds the ten SC-55
 * kits and map 2 the fourteen of the SC-88. A reset leaves it on the
 * SC-88 - the part image the firmware copies at reset begins `00 02`
 * (`131f4`), and SC88-OM printed 7-31 gives `40 4x 01` the same default.
 * It is NOT the Use For Rhythm Part setting, which selects a drum setup
 * and is a different axis; see `sc88_engine_part`.
 *
 * Both lookups take it, and the firmware resolves it once for both: the
 * melodic selector `2d3e` and the drum selector `2e7a` share the five
 * instructions that turn the part's bank word into it - subtract 0x0101,
 * take the low byte if that goes negative and the high byte otherwise,
 * refuse anything at 2 or above - so map 1 is as reachable melodically as
 * it is rhythmically, over CC32 or `40 4x 00`/`40 4x 01`. */
#define SC88_TONE_MAP_SC55 1u
#define SC88_TONE_MAP_SC88 2u

bool sc88_rom_select_drum(const struct sc88_rom *rom, uint8_t map,
                          uint8_t program, uint32_t *kit_offset);
/* A song may override any kit note's own parameters over SysEx, at
 * `41 mf rr` where m selects the drum setup, f the field and rr the note
 * (`04_protocol/sysex.md`). The kit record in ROM is read first and these
 * replace what the song has written, field by field; changing the kit
 * clears them, as the firmware does.
 */
/* Nine of these are the manual's own drum-map fields; the tenth is the
 * relative pitch the NRPN block carries, which is a centred offset
 * rather than the absolute play note field 1 sets. */
#define SC88_DRUM_FIELDS 10u
struct sc88_drum_overlay {
  uint8_t value[2][SC88_DRUM_FIELDS][128];
  uint8_t present[2][SC88_DRUM_FIELDS][128];
};

/* `setup` is the drum setup the part is using, 1 or 2, which is the half
 * of the overlay a song's `41 mf rr` writes landed in. `overlay` and
 * `setup` may be NULL and zero: then only the ROM is read. */
bool sc88_rom_open_drum_note_overlaid(
  const struct sc88_rom *rom, uint32_t kit_offset, uint8_t note,
  const struct sc88_drum_overlay *overlay, uint8_t setup,
  struct sc88_drum_note *out);

bool sc88_rom_open_drum_note(const struct sc88_rom *rom, uint32_t kit_offset,
                             uint8_t note, struct sc88_drum_note *out);

/* `map` is `SC88_TONE_MAP_SC55` or `SC88_TONE_MAP_SC88`, the row of the
 * lookup at `0x2fc00` the variation indexes. The two rows hold different
 * banks: the SC-55 row's fifteen physical banks are 0..14 and the SC-88
 * row's twenty-two are 15..36, so 259 of the 677 melodic tones - the
 * CM-64/CM-32L banks at variations 126 and 127 among them - exist on the
 * SC-55 row alone. */
bool sc88_rom_select_melodic(const struct sc88_rom *rom, uint8_t map,
                             uint8_t variation, uint8_t program,
                             uint32_t *tone_offset);
bool sc88_rom_open_tone(const struct sc88_rom *rom, uint32_t tone_offset,
                        struct sc88_tone *tone);
bool sc88_rom_open_component(const struct sc88_rom *rom,
                             const struct sc88_tone *tone, unsigned index,
                             struct sc88_component *component);

/* A component sounds only inside its own velocity range, bytes +6c and +6d,
 * both bounds inclusive. The two are a matched pair: +70, the factor that
 * maps the velocity into the 128-entry TVA curve, is exactly
 * `floor(127 * 256 / (high - low))` on every component in the ROM - 635
 * melodic and 342 rhythm, no exception - so the pair defines a window and
 * the curve spans it. Twenty-seven two-component tones give their two
 * components different windows, several as clean partitions with no overlap
 * and no gap: `Rotary Org.` 0..112 and 113..127, `Velo Harmnix` 0..120 and
 * 121..127, `Funk Gt.2` 0..115 and 116..127, `French Horns` 0..100 and
 * 101..127. libEmuSC's SC-55 path gates its partials on the same bytes, and
 * the measurement behind it is this same tone at this same boundary
 * (`Funk Gt.2` switches partials between velocity 115 and 116). */
bool sc88_rom_component_sounds(const struct sc88_component *component,
                               uint8_t velocity);

/* `selector_key` is the firmware's already transformed 0..127 directory key,
 * not necessarily the raw MIDI key. The first boundary >= it wins. */
bool sc88_rom_select_zone(const struct sc88_rom *rom,
                          const struct sc88_component *component,
                          uint8_t selector_key,
                          struct sc88_zone_selection *selection);

void sc88_rom_tone_name(const struct sc88_tone *tone, char name[13]);

#ifdef __cplusplus
}
#endif

#endif
