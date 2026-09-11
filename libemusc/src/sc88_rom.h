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

/* Which drum kit set a rhythm part's program indexes. Which one a reset
 * leaves active is not recovered, so the caller chooses. */
#define SC88_RHYTHM_MAP_SC55 1u
#define SC88_RHYTHM_MAP_SC88 2u

bool sc88_rom_select_drum(const struct sc88_rom *rom, uint8_t map,
                          uint8_t program, uint32_t *kit_offset);
/* A song may override any kit note's own parameters over SysEx, at
 * `41 mf rr` where m selects the map, f the field and rr the note
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

/* `overlay` and `map` may be NULL and zero: then only the ROM is read. */
bool sc88_rom_open_drum_note_overlaid(
  const struct sc88_rom *rom, uint32_t kit_offset, uint8_t note,
  const struct sc88_drum_overlay *overlay, uint8_t map,
  struct sc88_drum_note *out);

bool sc88_rom_open_drum_note(const struct sc88_rom *rom, uint32_t kit_offset,
                             uint8_t note, struct sc88_drum_note *out);

bool sc88_rom_select_melodic(const struct sc88_rom *rom, uint8_t variation,
                             uint8_t program, uint32_t *tone_offset);
bool sc88_rom_open_tone(const struct sc88_rom *rom, uint32_t tone_offset,
                        struct sc88_tone *tone);
bool sc88_rom_open_component(const struct sc88_rom *rom,
                             const struct sc88_tone *tone, unsigned index,
                             struct sc88_component *component);

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
