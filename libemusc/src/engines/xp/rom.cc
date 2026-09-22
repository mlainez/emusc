/* SPDX-License-Identifier: CC0-1.0 */
#include "rom.h"

#include "devices/jv1080.h"
#include "devices/sc88.h"

#include <cstring>

namespace EmuSC { namespace Xp {

namespace {

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

/* Whether an incoming ROM image matches a candidate profile's own
   identification data - never a device's bytes hardcoded here. */
bool matches(const struct XpDeviceProfile &profile, const uint8_t *bytes,
             size_t size)
{
  return size == profile.romSize &&
    (size_t)profile.identSecondOffset + sizeof profile.identFirstDirectory
      <= size &&
    std::memcmp(bytes, profile.identVectors,
                sizeof profile.identVectors) == 0 &&
    std::memcmp(bytes + profile.identSecondOffset,
                profile.identFirstDirectory,
                sizeof profile.identFirstDirectory) == 0;
}

/* Every device this engine can identify. The only place that names one -
   mirrors ControlRom::_profile_for() on the older engine. Adding another
   XP-family device means adding its profile here, not editing rom_init(). */
constexpr const struct XpDeviceProfile *kKnownProfiles[] = {
  &SC88_PROFILE, &JV1080_PROFILE };

}  // namespace

bool rom_init(struct xp_rom *rom, const uint8_t *bytes, size_t size)
{
  if (!rom || !bytes)
    return false;
  for (const struct XpDeviceProfile *profile : kKnownProfiles) {
    if (matches(*profile, bytes, size)) {
      rom->bytes = bytes;
      rom->size = size;
      rom->profile = profile;
      return true;
    }
  }
  return false;
}

bool rom_select_drum(const struct xp_rom *rom, uint8_t map,
                      uint8_t program, uint32_t *kitOffset)
{
  const struct XpDeviceProfile *profile = xp_profile(rom);
  if (!rom || !rom->bytes || !kitOffset || program > 127 ||
      map < 1 || map > 2 ||
      profile->drumPointerTable + profile->drumKitCount * 3 > rom->size)
    return false;
  uint8_t index = rom->bytes[profile->drumMapBase + ((unsigned)map - 1u) * 128u +
                            program];
  if (index >= profile->drumKitCount)
    return false;                /* ff marks a program with no kit */
  uint32_t pointer = be24(rom->bytes + profile->drumPointerTable +
                          (uint32_t)index * 3);
  if (pointer != profile->drumKitBase + (uint32_t)index * profile->drumKitStride ||
      pointer + profile->drumKitStride > rom->size)
    return false;
  *kitOffset = pointer;
  return true;
}

bool rom_open_drum_note_overlaid(
  const struct xp_rom *rom, uint32_t kitOffset, uint8_t note,
  const struct xp_drum_overlay *overlay, uint8_t setup,
  struct xp_drum_note *out)
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

bool rom_open_drum_note(const struct xp_rom *rom, uint32_t kitOffset,
                         uint8_t note, struct xp_drum_note *out)
{
  const struct XpDeviceProfile *profile = xp_profile(rom);
  if (!rom || !rom->bytes || !out || note > 127 ||
      kitOffset + profile->drumKitStride > rom->size)
    return false;
  uint32_t tone = be24(rom->bytes + kitOffset + (uint32_t)note * 3);
  if (tone == 0xffffffu || tone < profile->toneBase || tone >= profile->toneEnd)
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

bool rom_select_melodic(const struct xp_rom *rom, uint8_t map,
                         uint8_t variation, uint8_t program,
                         uint32_t *toneOffset)
{
  /* `2d3e` refuses a resolved map of 2 or above at `2d59` and returns no
     tone at all, which is what a false here is. */
  const struct XpDeviceProfile *profile = xp_profile(rom);
  if (!rom || !rom->bytes || !toneOffset || program > 127 ||
      variation > 127 || map < XP_TONE_MAP_SC55 ||
      map > XP_TONE_MAP_SC88)
    return false;
  uint8_t physical = rom->bytes[profile->melodicMapBase +
                                ((unsigned)map - 1u) * 128u + variation];
  if (physical == 0xff)
    return false;
  uint32_t pointerPosition = profile->pointerTableBase +
    (uint32_t)physical * profile->pointerBankSize + (uint32_t)program * 3;
  if (pointerPosition + 3 > profile->melodicMapBase)
    return false;
  uint32_t pointer = be24(rom->bytes + pointerPosition);
  if (pointer == 0xffffff || pointer < profile->toneBase ||
      pointer >= profile->toneEnd)
    return false;
  *toneOffset = pointer;
  return true;
}

bool rom_open_tone(const struct xp_rom *rom, uint32_t toneOffset,
                    struct xp_tone *tone)
{
  const struct XpDeviceProfile *profile = xp_profile(rom);
  if (!rom || !rom->bytes || !tone || toneOffset < profile->toneBase ||
      toneOffset + profile->toneCommonSize > profile->toneEnd)
    return false;
  uint8_t count = rom->bytes[toneOffset + 30];
  if ((count != 1 && count != 2) ||
      !printable(rom->bytes + toneOffset, 12))
    return false;
  uint32_t end = toneOffset + profile->toneCommonSize +
    (uint32_t)count * profile->componentSize;
  if (end > profile->toneEnd || (toneOffset >> 16) != ((end - 1) >> 16))
    return false;
  tone->common = rom->bytes + toneOffset;
  tone->offset = toneOffset;
  tone->component_count = count;
  return true;
}

bool rom_open_component(const struct xp_rom *rom, const struct xp_tone *tone,
                         unsigned index, struct xp_component *component)
{
  if (!rom || !rom->bytes || !tone || !tone->common || !component ||
      index >= tone->component_count)
    return false;
  const struct XpDeviceProfile *profile = xp_profile(rom);
  uint32_t offset = tone->offset + profile->toneCommonSize +
    (uint32_t)index * profile->componentSize;
  if (offset + profile->componentSize > rom->size)
    return false;
  uint32_t directory = ((uint32_t)tone->common[32] << 16) |
    be16(rom->bytes + offset);
  if (directory < profile->directoryBase || directory >= profile->directoryEnd)
    return false;
  component->bytes = rom->bytes + offset;
  component->offset = offset;
  component->directory_offset = directory;
  return true;
}

bool rom_component_sounds(const struct xp_component *component,
                           uint8_t velocity)
{
  if (!component || !component->bytes)
    return false;
  uint8_t low = component->bytes[0x6c];
  uint8_t high = component->bytes[0x6d];
  return velocity >= low && velocity <= high;
}

bool rom_select_zone(const struct xp_rom *rom,
                      const struct xp_component *component,
                      uint8_t selectorKey,
                      struct xp_zone_selection *selection)
{
  const struct XpDeviceProfile *profile = xp_profile(rom);
  if (!rom || !rom->bytes || !component || !component->bytes || !selection ||
      selectorKey > 127 ||
      component->directory_offset < profile->directoryBase ||
      component->directory_offset + 16 > profile->directoryEnd)
    return false;
  const uint8_t *bytes = rom->bytes;
  if (!printable(bytes + component->directory_offset + 2, 12) ||
      bytes[component->directory_offset + 14] != 0x03 ||
      bytes[component->directory_offset + 15] != 0xff)
    return false;

  uint32_t position = component->directory_offset + 16;
  int previous = -1;
  while (position + 6 <= profile->directoryEnd) {
    uint8_t boundary = bytes[position];

    if (boundary <= previous || boundary > 127 || bytes[position + 1] != 0xff)
      return false;
    previous = boundary;
    if (selectorKey <= boundary) {
      uint16_t pointer = be16(bytes + position + 4);
      if (pointer == 0xffff)
        return false;
      uint32_t descriptorOffset = profile->directoryBase + pointer;
      if (descriptorOffset < profile->descriptorBase ||
          descriptorOffset + profile->waveDescriptorSize >
            profile->descriptorEnd ||
          (descriptorOffset - profile->descriptorBase) %
            profile->waveDescriptorSize != 0)
        return false;
      selection->boundary = boundary;
      selection->static_attenuation = be16(bytes + position + 2);
      selection->descriptor_offset = descriptorOffset;
      return wave_descriptor_parse(profile, bytes + descriptorOffset,
                                    profile->waveDescriptorSize,
                                    &selection->descriptor);
    }
    position += 6;
    if (boundary == 127)
      break;
  }
  return false;
}

void rom_tone_name(const struct xp_tone *tone, char name[13])
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
