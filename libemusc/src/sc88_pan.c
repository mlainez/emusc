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
  int16_t component_offset;
  int composed;

  if (!rom || !rom->bytes || !tone || !tone->common || !component ||
      !component->bytes || !controls || !position || !left_q15 ||
      !right_q15 || selector_key > 127 || controls->master < 1 ||
      controls->master > 127 || controls->part > 127)
    return false;
  if (controls->part == 0) {
    /* A random-pan request takes the drawn position whole: composition
       returns its sentinel rather than a value for the offsets to move. */
    *position = controls->random_position < 1 ? 64u
      : (controls->random_position > 127 ? 127u : controls->random_position);
    return sc88_pan_pair_q15(rom, *position, left_q15, right_q15);
  }
  if (!sc88_pan_component_offset(rom, tone, component, selector_key,
                                 &component_offset))
    return false;
  composed = controls->part + ((int)controls->master - 64) +
    component_offset;
  if (composed < 1)
    composed = 1;
  else if (composed > 127)
    composed = 127;
  *position = (uint8_t)composed;
  return sc88_pan_pair_q15(rom, *position, left_q15, right_q15);
}

bool sc88_pan_component_offset(const struct sc88_rom *rom,
                               const struct sc88_tone *tone,
                               const struct sc88_component *component,
                               uint8_t selector_key, int16_t *offset)
{
  uint32_t key_table;
  if (!rom || !rom->bytes || !tone || !tone->common || !component ||
      !component->bytes || !offset || selector_key > 127)
    return false;
  key_table = ((uint32_t)tone->common[0x21] << 16) |
    sc88_pan_be16(tone->common + 0x0e);
  if (key_table + selector_key >= rom->size)
    return false;
  *offset = (int16_t)(sc88_pan_s8(component->bytes[0x04]) +
    sc88_pan_s8(rom->bytes[key_table + selector_key]));
  return true;
}

bool sc88_control_gain_q15(const struct sc88_rom *rom, uint8_t control,
                           uint16_t *gain_q15)
{
  if (!rom || !rom->bytes || !gain_q15 || control > 127 ||
      SC88_PAN_TABLE + 127u * 2 > rom->size)
    return false;
  if (control == 0) {
    *gain_q15 = 0;
    return true;
  }
  *gain_q15 = sc88_pan_be16(rom->bytes + SC88_PAN_TABLE + (control - 1) * 2);
  return (*gain_q15 & 0x3f) == 0;
}

uint8_t sc88_send_combine(uint8_t part, uint8_t note)
{
  unsigned p = part > 127 ? 127u : part;
  unsigned n = note > 127 ? 127u : note;
  return (uint8_t)(((p * n) + 127u) >> 7);
}

bool sc88_pan_pair_q15(const struct sc88_rom *rom, uint8_t position,
                       uint16_t *left_q15, uint16_t *right_q15)
{
  uint8_t left_index;
  uint8_t right_index;
  if (!rom || !rom->bytes || !left_q15 || !right_q15 || position < 1 ||
      position > 127 || SC88_PAN_TABLE + 127u * 2 > rom->size)
    return false;
  left_index = (uint8_t)(127 - position);
  right_index = (uint8_t)(position - 1);
  *left_q15 = sc88_pan_be16(rom->bytes + SC88_PAN_TABLE + left_index * 2);
  *right_q15 = sc88_pan_be16(rom->bytes + SC88_PAN_TABLE + right_index * 2);
  return (*left_q15 & 0x3f) == 0 && (*right_q15 & 0x3f) == 0;
}
