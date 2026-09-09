/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_rom.h"

#include <string.h>

#define SC88_POINTER_TABLE_BASE 0x20000u
#define SC88_POINTER_BANK_SIZE 384u
#define SC88_NATIVE_VARIATION_MAP 0x2fc80u
#define SC88_DIRECTORY_BASE 0x30000u
#define SC88_DIRECTORY_END 0x3606cu
#define SC88_DESCRIPTOR_BASE 0x36100u
#define SC88_DESCRIPTOR_END 0x3f714u
#define SC88_TONE_BASE 0x40000u
#define SC88_TONE_END 0x75000u

static uint16_t sc88_rom_be16(const uint8_t *p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t sc88_rom_be24(const uint8_t *p)
{
  return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static bool sc88_rom_printable(const uint8_t *p, size_t count)
{
  size_t i;
  for (i = 0; i < count; ++i)
    if (p[i] < 0x20 || p[i] > 0x7e)
      return false;
  return true;
}

bool sc88_rom_init(struct sc88_rom *rom, const uint8_t *bytes, size_t size)
{
  static const uint8_t vectors[16] = {
    0x00, 0x00, 0x02, 0x00, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x01, 0xf4, 0x00, 0x00, 0x01, 0xf4
  };
  static const uint8_t first_directory[16] = {
    0x00, 0x00, 'P', 'i', 'a', 'n', 'o', ' ',
    '1', 'A', ' ', ' ', ' ', ' ', 0x03, 0xff
  };

  if (!rom || !bytes || size != SC88_CONTROL_ROM_SIZE ||
      memcmp(bytes, vectors, sizeof vectors) != 0 ||
      memcmp(bytes + SC88_DIRECTORY_BASE, first_directory,
             sizeof first_directory) != 0)
    return false;
  rom->bytes = bytes;
  rom->size = size;
  return true;
}

bool sc88_rom_select_melodic(const struct sc88_rom *rom, uint8_t variation,
                             uint8_t program, uint32_t *tone_offset)
{
  uint8_t physical;
  uint32_t pointer_position;
  uint32_t pointer;

  if (!rom || !rom->bytes || !tone_offset || program > 127)
    return false;
  physical = rom->bytes[SC88_NATIVE_VARIATION_MAP + variation];
  if (physical == 0xff)
    return false;
  pointer_position = SC88_POINTER_TABLE_BASE +
    (uint32_t)physical * SC88_POINTER_BANK_SIZE + (uint32_t)program * 3;
  if (pointer_position + 3 > SC88_NATIVE_VARIATION_MAP)
    return false;
  pointer = sc88_rom_be24(rom->bytes + pointer_position);
  if (pointer == 0xffffff || pointer < SC88_TONE_BASE ||
      pointer >= SC88_TONE_END)
    return false;
  *tone_offset = pointer;
  return true;
}

bool sc88_rom_open_tone(const struct sc88_rom *rom, uint32_t tone_offset,
                        struct sc88_tone *tone)
{
  uint8_t count;
  uint32_t end;

  if (!rom || !rom->bytes || !tone || tone_offset < SC88_TONE_BASE ||
      tone_offset + SC88_TONE_COMMON_SIZE > SC88_TONE_END)
    return false;
  count = rom->bytes[tone_offset + 30];
  if ((count != 1 && count != 2) ||
      !sc88_rom_printable(rom->bytes + tone_offset, 12))
    return false;
  end = tone_offset + SC88_TONE_COMMON_SIZE +
    (uint32_t)count * SC88_COMPONENT_SIZE;
  if (end > SC88_TONE_END ||
      (tone_offset >> 16) != ((end - 1) >> 16))
    return false;
  tone->common = rom->bytes + tone_offset;
  tone->offset = tone_offset;
  tone->component_count = count;
  return true;
}

bool sc88_rom_open_component(const struct sc88_rom *rom,
                             const struct sc88_tone *tone, unsigned index,
                             struct sc88_component *component)
{
  uint32_t offset;
  uint32_t directory;

  if (!rom || !rom->bytes || !tone || !tone->common || !component ||
      index >= tone->component_count)
    return false;
  offset = tone->offset + SC88_TONE_COMMON_SIZE +
    (uint32_t)index * SC88_COMPONENT_SIZE;
  if (offset + SC88_COMPONENT_SIZE > rom->size)
    return false;
  directory = ((uint32_t)tone->common[32] << 16) |
    sc88_rom_be16(rom->bytes + offset);
  if (directory < SC88_DIRECTORY_BASE || directory >= SC88_DIRECTORY_END)
    return false;
  component->bytes = rom->bytes + offset;
  component->offset = offset;
  component->directory_offset = directory;
  return true;
}

bool sc88_rom_select_zone(const struct sc88_rom *rom,
                          const struct sc88_component *component,
                          uint8_t selector_key,
                          struct sc88_zone_selection *selection)
{
  const uint8_t *bytes;
  uint32_t position;
  int previous = -1;

  if (!rom || !rom->bytes || !component || !component->bytes || !selection ||
      selector_key > 127 ||
      component->directory_offset < SC88_DIRECTORY_BASE ||
      component->directory_offset + 16 > SC88_DIRECTORY_END)
    return false;
  bytes = rom->bytes;
  if (!sc88_rom_printable(bytes + component->directory_offset + 2, 12) ||
      bytes[component->directory_offset + 14] != 0x03 ||
      bytes[component->directory_offset + 15] != 0xff)
    return false;

  position = component->directory_offset + 16;
  while (position + 6 <= SC88_DIRECTORY_END) {
    uint8_t boundary = bytes[position];
    uint16_t pointer;
    uint32_t descriptor_offset;

    if (boundary <= previous || boundary > 127 || bytes[position + 1] != 0xff)
      return false;
    previous = boundary;
    if (selector_key <= boundary) {
      pointer = sc88_rom_be16(bytes + position + 4);
      if (pointer == 0xffff)
        return false;
      descriptor_offset = SC88_DIRECTORY_BASE + pointer;
      if (descriptor_offset < SC88_DESCRIPTOR_BASE ||
          descriptor_offset + SC88_WAVE_DESCRIPTOR_SIZE > SC88_DESCRIPTOR_END ||
          (descriptor_offset - SC88_DESCRIPTOR_BASE) %
            SC88_WAVE_DESCRIPTOR_SIZE != 0)
        return false;
      selection->boundary = boundary;
      selection->static_attenuation = sc88_rom_be16(bytes + position + 2);
      selection->descriptor_offset = descriptor_offset;
      return sc88_wave_descriptor_parse(bytes + descriptor_offset,
                                        SC88_WAVE_DESCRIPTOR_SIZE,
                                        &selection->descriptor);
    }
    position += 6;
    if (boundary == 127)
      break;
  }
  return false;
}

void sc88_rom_tone_name(const struct sc88_tone *tone, char name[13])
{
  if (!name)
    return;
  if (!tone || !tone->common) {
    name[0] = '\0';
    return;
  }
  memcpy(name, tone->common, 12);
  name[12] = '\0';
}
