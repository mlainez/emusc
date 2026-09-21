/* SPDX-License-Identifier: CC0-1.0 */
#include "pan.h"

#include "devices/sc88.h"

namespace EmuSC { namespace Xp {

namespace {

uint16_t be16(const uint8_t *p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

int8_t s8(uint8_t value)
{
  return value <= INT8_MAX
    ? (int8_t)value
    : (int8_t)(-1 - (int16_t)(UINT8_MAX - value));
}

}  // namespace

bool pan_component_offset(const struct sc88_rom *rom, const struct sc88_tone *tone,
                           const struct sc88_component *component,
                           uint8_t selectorKey, int16_t *offset)
{
  if (!rom || !rom->bytes || !tone || !tone->common || !component ||
      !component->bytes || !offset || selectorKey > 127)
    return false;
  uint32_t keyTable = ((uint32_t)tone->common[0x21] << 16) |
    be16(tone->common + 0x0e);
  if (keyTable + selectorKey >= rom->size)
    return false;
  *offset = (int16_t)(s8(component->bytes[0x04]) +
    s8(rom->bytes[keyTable + selectorKey]));
  return true;
}

bool control_gain_q15(const struct sc88_rom *rom, uint8_t control,
                       uint16_t *gainQ15)
{
  const struct XpDeviceProfile *profile = xp_profile(rom);
  if (!rom || !rom->bytes || !gainQ15 || control > 127 ||
      profile->sendTable + 128u * 2 > rom->size)
    return false;
  /* The word carries the gain in its top ten bits and the XP destination
     selector in its low six; the table's own low six are zero, and the
     selector this engine does not plumb. */
  uint16_t word = be16(rom->bytes + profile->sendTable + (unsigned)control * 2);
  *gainQ15 = (uint16_t)(word & 0xffc0u);
  return (word & 0x3fu) == 0;
}

uint8_t send_combine(uint8_t part, uint8_t note)
{
  unsigned p = part > 127 ? 127u : part;
  unsigned n = note > 127 ? 127u : note;
  return (uint8_t)(((p * n) + 127u) >> 7);
}

bool pan_pair_q15(const struct sc88_rom *rom, uint8_t position,
                   uint16_t *leftQ15, uint16_t *rightQ15)
{
  const struct XpDeviceProfile *profile = xp_profile(rom);
  if (!rom || !rom->bytes || !leftQ15 || !rightQ15 || position < 1 ||
      position > 127 || profile->panTable + 127u * 2 > rom->size)
    return false;
  uint8_t leftIndex = (uint8_t)(127 - position);
  uint8_t rightIndex = (uint8_t)(position - 1);
  *leftQ15 = be16(rom->bytes + profile->panTable + leftIndex * 2);
  *rightQ15 = be16(rom->bytes + profile->panTable + rightIndex * 2);
  return (*leftQ15 & 0x3f) == 0 && (*rightQ15 & 0x3f) == 0;
}

bool pan_static_q15(const struct sc88_rom *rom, const struct sc88_tone *tone,
                     const struct sc88_component *component,
                     uint8_t selectorKey, const struct sc88_pan_controls *controls,
                     uint8_t *position, uint16_t *leftQ15, uint16_t *rightQ15)
{
  if (!rom || !rom->bytes || !tone || !tone->common || !component ||
      !component->bytes || !controls || !position || !leftQ15 ||
      !rightQ15 || selectorKey > 127 || controls->master < 1 ||
      controls->master > 127 || controls->part > 127)
    return false;
  if (controls->part == 0) {
    /* A random-pan request takes the drawn position whole: composition
       returns its sentinel rather than a value for the offsets to move. */
    *position = controls->random_position < 1 ? 64u
      : (controls->random_position > 127 ? 127u : controls->random_position);
    return pan_pair_q15(rom, *position, leftQ15, rightQ15);
  }
  int16_t componentOffset;
  if (!pan_component_offset(rom, tone, component, selectorKey,
                             &componentOffset))
    return false;
  int composed = controls->part + ((int)controls->master - 64) +
    componentOffset;
  if (composed < 1)
    composed = 1;
  else if (composed > 127)
    composed = 127;
  *position = (uint8_t)composed;
  return pan_pair_q15(rom, *position, leftQ15, rightQ15);
}

}}  // namespace EmuSC::Xp
