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
