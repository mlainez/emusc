/* SPDX-License-Identifier: CC0-1.0 */
#include "rom.h"

#include <array>
#include <cstring>

namespace EmuSC { namespace Xp {

namespace {

constexpr uint32_t kPointerTableBase = 0x20000u;
constexpr uint32_t kPointerBankSize = 384u;
constexpr uint32_t kMelodicMapBase = 0x2fc00u;
constexpr uint32_t kDirectoryBase = 0x30000u;
constexpr uint32_t kDirectoryEnd = 0x3606cu;
constexpr uint32_t kDescriptorBase = 0x36100u;
constexpr uint32_t kDescriptorEnd = 0x3f714u;
constexpr uint32_t kToneBase = 0x40000u;
constexpr uint32_t kToneEnd = 0x75000u;

constexpr uint32_t kDrumMapBase = 0x2fd00u;
constexpr uint32_t kDrumPointerTable = 0x2b550u;
constexpr uint32_t kDrumKitCount = 24u;
constexpr uint32_t kDrumKitStride = 0x50cu;
constexpr uint32_t kDrumKitBase = 0x23c30u;

uint16_t be16(const uint8_t *p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

uint32_t be24(const uint8_t *p)
{
  return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

bool printable(const uint8_t *p, size_t count)
{
  for (size_t i = 0; i < count; ++i)
    if (p[i] < 0x20 || p[i] > 0x7e)
      return false;
  return true;
}

}  // namespace

bool rom_init(struct sc88_rom *rom, const uint8_t *bytes, size_t size)
{
  static constexpr std::array<uint8_t, 16> vectors = {
    0x00, 0x00, 0x02, 0x00, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x01, 0xf4, 0x00, 0x00, 0x01, 0xf4
  };
  static constexpr std::array<uint8_t, 16> firstDirectory = {
    0x00, 0x00, 'P', 'i', 'a', 'n', 'o', ' ',
    '1', 'A', ' ', ' ', ' ', ' ', 0x03, 0xff
  };

  if (!rom || !bytes || size != SC88_CONTROL_ROM_SIZE ||
      std::memcmp(bytes, vectors.data(), vectors.size()) != 0 ||
      std::memcmp(bytes + kDirectoryBase, firstDirectory.data(),
                  firstDirectory.size()) != 0)
    return false;
  rom->bytes = bytes;
  rom->size = size;
  return true;
}

bool rom_select_drum(const struct sc88_rom *rom, uint8_t map,
                      uint8_t program, uint32_t *kitOffset)
{
  if (!rom || !rom->bytes || !kitOffset || program > 127 ||
      map < 1 || map > 2 ||
      kDrumPointerTable + kDrumKitCount * 3 > rom->size)
    return false;
  uint8_t index = rom->bytes[kDrumMapBase + ((unsigned)map - 1u) * 128u +
                            program];
  if (index >= kDrumKitCount)
    return false;                /* ff marks a program with no kit */
  uint32_t pointer = be24(rom->bytes + kDrumPointerTable +
                          (uint32_t)index * 3);
  if (pointer != kDrumKitBase + (uint32_t)index * kDrumKitStride ||
      pointer + kDrumKitStride > rom->size)
    return false;
  *kitOffset = pointer;
  return true;
}

bool rom_open_drum_note_overlaid(
  const struct sc88_rom *rom, uint32_t kitOffset, uint8_t note,
  const struct sc88_drum_overlay *overlay, uint8_t setup,
  struct sc88_drum_note *out)
{
  if (!rom_open_drum_note(rom, kitOffset, note, out))
    return false;
  if (!overlay || setup < 1 || setup > 2)
    return true;
  unsigned m = (unsigned)setup - 1u;
  /* the manual's field order: 1 play note, 2 level, 3 assign group,
     4 pan, 5 reverb send, 6 chorus send, 7 receive Note Off,
     8 receive Note On, 9 delay send */
  if (overlay->present[m][0][note])
    out->play_note = overlay->value[m][0][note];
  if (overlay->present[m][1][note])
    out->level = overlay->value[m][1][note];
  if (overlay->present[m][2][note])
    out->assign_group = overlay->value[m][2][note];
  if (overlay->present[m][3][note])
    out->pan = overlay->value[m][3][note];
  if (overlay->present[m][4][note])
    out->reverb_send = overlay->value[m][4][note];
  if (overlay->present[m][5][note])
    out->chorus_send = overlay->value[m][5][note];
  /* The NRPN block's pitch is centred at 64 and relative, so it moves the
     play note rather than replacing it. */
  if (overlay->present[m][9][note]) {
    int shifted = (int)out->play_note +
      ((int)overlay->value[m][9][note] - 64);
    out->play_note = (uint8_t)(shifted < 0 ? 0
                               : shifted > 127 ? 127 : shifted);
  }
  /* Receive Note Off is bit 0 of the kit's own flags byte, so a write to
     field 7 replaces that bit and leaves the rest of the byte alone. */
  if (overlay->present[m][6][note])
    out->flags = (uint8_t)((out->flags & (uint8_t)~1u) |
                           (overlay->value[m][6][note] ? 1u : 0u));
  return true;
}

bool rom_open_drum_note(const struct sc88_rom *rom, uint32_t kitOffset,
                         uint8_t note, struct sc88_drum_note *out)
{
  if (!rom || !rom->bytes || !out || note > 127 ||
      kitOffset + kDrumKitStride > rom->size)
    return false;
  uint32_t tone = be24(rom->bytes + kitOffset + (uint32_t)note * 3);
  if (tone == 0xffffffu || tone < kToneBase || tone >= kToneEnd)
    return false;                /* this key has no sound in this kit */
  out->tone_offset = tone;
  out->play_note = rom->bytes[kitOffset + 0x180u + note];
  out->level = rom->bytes[kitOffset + 0x200u + note];
  out->assign_group = rom->bytes[kitOffset + 0x280u + note];
  out->pan = rom->bytes[kitOffset + 0x300u + note];
  out->reverb_send = rom->bytes[kitOffset + 0x380u + note];
  out->chorus_send = rom->bytes[kitOffset + 0x400u + note];
  out->flags = rom->bytes[kitOffset + 0x480u + note];
  return true;
}

bool rom_select_melodic(const struct sc88_rom *rom, uint8_t map,
                         uint8_t variation, uint8_t program,
                         uint32_t *toneOffset)
{
  /* `2d3e` refuses a resolved map of 2 or above at `2d59` and returns no
     tone at all, which is what a false here is. */
  if (!rom || !rom->bytes || !toneOffset || program > 127 ||
      variation > 127 || map < SC88_TONE_MAP_SC55 ||
      map > SC88_TONE_MAP_SC88)
    return false;
  uint8_t physical = rom->bytes[kMelodicMapBase + ((unsigned)map - 1u) * 128u +
                                variation];
  if (physical == 0xff)
    return false;
  uint32_t pointerPosition = kPointerTableBase +
    (uint32_t)physical * kPointerBankSize + (uint32_t)program * 3;
  if (pointerPosition + 3 > kMelodicMapBase)
    return false;
  uint32_t pointer = be24(rom->bytes + pointerPosition);
  if (pointer == 0xffffff || pointer < kToneBase || pointer >= kToneEnd)
    return false;
  *toneOffset = pointer;
  return true;
}

bool rom_open_tone(const struct sc88_rom *rom, uint32_t toneOffset,
                    struct sc88_tone *tone)
{
  if (!rom || !rom->bytes || !tone || toneOffset < kToneBase ||
      toneOffset + SC88_TONE_COMMON_SIZE > kToneEnd)
    return false;
  uint8_t count = rom->bytes[toneOffset + 30];
  if ((count != 1 && count != 2) ||
      !printable(rom->bytes + toneOffset, 12))
    return false;
  uint32_t end = toneOffset + SC88_TONE_COMMON_SIZE +
    (uint32_t)count * SC88_COMPONENT_SIZE;
  if (end > kToneEnd || (toneOffset >> 16) != ((end - 1) >> 16))
    return false;
  tone->common = rom->bytes + toneOffset;
  tone->offset = toneOffset;
  tone->component_count = count;
  return true;
}

bool rom_open_component(const struct sc88_rom *rom, const struct sc88_tone *tone,
                         unsigned index, struct sc88_component *component)
{
  if (!rom || !rom->bytes || !tone || !tone->common || !component ||
      index >= tone->component_count)
    return false;
  uint32_t offset = tone->offset + SC88_TONE_COMMON_SIZE +
    (uint32_t)index * SC88_COMPONENT_SIZE;
  if (offset + SC88_COMPONENT_SIZE > rom->size)
    return false;
  uint32_t directory = ((uint32_t)tone->common[32] << 16) |
    be16(rom->bytes + offset);
  if (directory < kDirectoryBase || directory >= kDirectoryEnd)
    return false;
  component->bytes = rom->bytes + offset;
  component->offset = offset;
  component->directory_offset = directory;
  return true;
}

bool rom_component_sounds(const struct sc88_component *component,
                           uint8_t velocity)
{
  if (!component || !component->bytes)
    return false;
  uint8_t low = component->bytes[0x6c];
  uint8_t high = component->bytes[0x6d];
  return velocity >= low && velocity <= high;
}

bool rom_select_zone(const struct sc88_rom *rom,
                      const struct sc88_component *component,
                      uint8_t selectorKey,
                      struct sc88_zone_selection *selection)
{
  if (!rom || !rom->bytes || !component || !component->bytes || !selection ||
      selectorKey > 127 ||
      component->directory_offset < kDirectoryBase ||
      component->directory_offset + 16 > kDirectoryEnd)
    return false;
  const uint8_t *bytes = rom->bytes;
  if (!printable(bytes + component->directory_offset + 2, 12) ||
      bytes[component->directory_offset + 14] != 0x03 ||
      bytes[component->directory_offset + 15] != 0xff)
    return false;

  uint32_t position = component->directory_offset + 16;
  int previous = -1;
  while (position + 6 <= kDirectoryEnd) {
    uint8_t boundary = bytes[position];

    if (boundary <= previous || boundary > 127 || bytes[position + 1] != 0xff)
      return false;
    previous = boundary;
    if (selectorKey <= boundary) {
      uint16_t pointer = be16(bytes + position + 4);
      if (pointer == 0xffff)
        return false;
      uint32_t descriptorOffset = kDirectoryBase + pointer;
      if (descriptorOffset < kDescriptorBase ||
          descriptorOffset + SC88_WAVE_DESCRIPTOR_SIZE > kDescriptorEnd ||
          (descriptorOffset - kDescriptorBase) %
            SC88_WAVE_DESCRIPTOR_SIZE != 0)
        return false;
      selection->boundary = boundary;
      selection->static_attenuation = be16(bytes + position + 2);
      selection->descriptor_offset = descriptorOffset;
      return wave_descriptor_parse(bytes + descriptorOffset,
                                    SC88_WAVE_DESCRIPTOR_SIZE,
                                    &selection->descriptor);
    }
    position += 6;
    if (boundary == 127)
      break;
  }
  return false;
}

void rom_tone_name(const struct sc88_tone *tone, char name[13])
{
  if (!name)
    return;
  if (!tone || !tone->common) {
    name[0] = '\0';
    return;
  }
  std::memcpy(name, tone->common, 12);
  name[12] = '\0';
}

}}  // namespace EmuSC::Xp

// Compatibility shims for callers not yet ported to the EmuSC::Xp API.
extern "C" {

bool sc88_rom_init(struct sc88_rom *rom, const uint8_t *bytes, size_t size)
{
  return EmuSC::Xp::rom_init(rom, bytes, size);
}

bool sc88_rom_select_drum(const struct sc88_rom *rom, uint8_t map,
                          uint8_t program, uint32_t *kit_offset)
{
  return EmuSC::Xp::rom_select_drum(rom, map, program, kit_offset);
}

bool sc88_rom_open_drum_note_overlaid(
  const struct sc88_rom *rom, uint32_t kit_offset, uint8_t note,
  const struct sc88_drum_overlay *overlay, uint8_t setup,
  struct sc88_drum_note *out)
{
  return EmuSC::Xp::rom_open_drum_note_overlaid(rom, kit_offset, note,
                                                 overlay, setup, out);
}

bool sc88_rom_open_drum_note(const struct sc88_rom *rom, uint32_t kit_offset,
                             uint8_t note, struct sc88_drum_note *out)
{
  return EmuSC::Xp::rom_open_drum_note(rom, kit_offset, note, out);
}

bool sc88_rom_select_melodic(const struct sc88_rom *rom, uint8_t map,
                             uint8_t variation, uint8_t program,
                             uint32_t *tone_offset)
{
  return EmuSC::Xp::rom_select_melodic(rom, map, variation, program,
                                        tone_offset);
}

bool sc88_rom_open_tone(const struct sc88_rom *rom, uint32_t tone_offset,
                        struct sc88_tone *tone)
{
  return EmuSC::Xp::rom_open_tone(rom, tone_offset, tone);
}

bool sc88_rom_open_component(const struct sc88_rom *rom,
                             const struct sc88_tone *tone, unsigned index,
                             struct sc88_component *component)
{
  return EmuSC::Xp::rom_open_component(rom, tone, index, component);
}

bool sc88_rom_component_sounds(const struct sc88_component *component,
                               uint8_t velocity)
{
  return EmuSC::Xp::rom_component_sounds(component, velocity);
}

bool sc88_rom_select_zone(const struct sc88_rom *rom,
                          const struct sc88_component *component,
                          uint8_t selector_key,
                          struct sc88_zone_selection *selection)
{
  return EmuSC::Xp::rom_select_zone(rom, component, selector_key, selection);
}

void sc88_rom_tone_name(const struct sc88_tone *tone, char name[13])
{
  EmuSC::Xp::rom_tone_name(tone, name);
}

}  // extern "C"
