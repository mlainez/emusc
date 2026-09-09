/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_pan.h"

#include <limits.h>

#define SC88_PAN_TABLE 0x15db6u

static uint16_t sc88_pan_be16(const uint8_t *p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static int8_t sc88_pan_s8(uint8_t value)
{
  return value <= INT8_MAX
    ? (int8_t)value
    : (int8_t)(-1 - (int16_t)(UINT8_MAX - value));
}

bool sc88_pan_static_q15(const struct sc88_rom *rom,
                         const struct sc88_tone *tone,
                         const struct sc88_component *component,
                         uint8_t selector_key,
                         const struct sc88_pan_controls *controls,
                         uint8_t *position, uint16_t *left_q15,
                         uint16_t *right_q15)
{
  uint32_t key_table;
  int composed;
  uint8_t left_index;
  uint8_t right_index;

  if (!rom || !rom->bytes || !tone || !tone->common || !component ||
      !component->bytes || !controls || !position || !left_q15 ||
      !right_q15 || selector_key > 127 || controls->master < 1 ||
      controls->master > 127 || controls->part > 127 || controls->part == 0)
    return false;
  key_table = ((uint32_t)tone->common[0x21] << 16) |
    sc88_pan_be16(tone->common + 0x0e);
  if (key_table + selector_key >= rom->size ||
      SC88_PAN_TABLE + 127u * 2 > rom->size)
    return false;
  composed = controls->part + ((int)controls->master - 64) +
    sc88_pan_s8(component->bytes[0x04]) +
    sc88_pan_s8(rom->bytes[key_table + selector_key]);
  if (composed < 1)
    composed = 1;
  else if (composed > 127)
    composed = 127;
  *position = (uint8_t)composed;
  left_index = (uint8_t)(127 - composed);
  right_index = (uint8_t)(composed - 1);
  *left_q15 = sc88_pan_be16(rom->bytes + SC88_PAN_TABLE + left_index * 2);
  *right_q15 = sc88_pan_be16(rom->bytes + SC88_PAN_TABLE + right_index * 2);
  return (*left_q15 & 0x3f) == 0 && (*right_q15 & 0x3f) == 0;
}
