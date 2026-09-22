/* SPDX-License-Identifier: CC0-1.0 */
#include "packed_rom.h"

#include "rom.h"

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

/* A field is read as a little-endian pair, so a descriptor whose byte
   offset is the group's last byte still needs a byte after it. The widest
   field in a schema of this shape is eight bits, so the pair never has to
   grow. */
bool read_pair(const uint8_t *packed, size_t available, unsigned offset,
                uint16_t *out)
{
  if (offset >= available)
    return false;
  uint16_t low = packed[offset];
  uint16_t high = (offset + 1u < available) ? packed[offset + 1u] : 0u;
  *out = (uint16_t)(low | (uint16_t)(high << 8));
  return true;
}

const struct XpPackedGroup *group_of(const struct XpDeviceProfile *profile,
                                      unsigned group)
{
  if (group >= profile->packedGroupCount || group >= XP_PACKED_GROUP_MAX)
    return nullptr;
  return profile->packedGroups + group;
}

}  // namespace

bool packed_descriptor(const struct xp_rom *rom, unsigned index,
                        struct xp_field_descriptor *out)
{
  if (!rom || !rom->bytes || !out)
    return false;
  const struct XpDeviceProfile *profile = xp_profile(rom);
  if (!profile->packedDescriptorBase || !profile->packedDescriptorStride ||
      index >= profile->packedDescriptorCount)
    return false;
  uint32_t at = profile->packedDescriptorBase +
    index * profile->packedDescriptorStride;
  if ((size_t)at + profile->packedDescriptorStride > rom->size)
    return false;
  const uint8_t *d = rom->bytes + at;
  out->mask = be16(d + profile->packedMaskOffset);
  out->byte_offset = d[profile->packedByteOffset];
  out->shift = d[profile->packedShiftOffset];
  out->bias = (int8_t)d[profile->packedBiasOffset];
  out->minimum = d[profile->packedMinOffset];
  out->maximum = d[profile->packedMaxOffset];
  /* The shift is redundant with the mask on every descriptor of the one
     table this has been read on, and a descriptor where it is not is a
     descriptor this reader would decode wrongly rather than loudly. */
  if (!out->mask)
    return false;
  unsigned trailing = 0;
  while (!((out->mask >> trailing) & 1u))
    ++trailing;
  if (out->shift != trailing)
    return false;
  uint16_t normalised = (uint16_t)(out->mask >> trailing);
  if (normalised & (uint16_t)(normalised + 1u))
    return false;                /* the mask is not one contiguous run */
  return true;
}

bool packed_group_descriptor(const struct xp_rom *rom, unsigned group,
                              unsigned field,
                              struct xp_field_descriptor *out)
{
  if (!rom || !rom->bytes || !out)
    return false;
  const struct XpPackedGroup *g = group_of(xp_profile(rom), group);
  return g && field < g->fieldCount &&
    packed_descriptor(rom, (unsigned)g->firstDescriptor + field, out);
}

bool packed_field_is_eight_bit(const struct xp_rom *rom, unsigned group,
                                unsigned field)
{
  struct xp_field_descriptor d;
  if (!packed_group_descriptor(rom, group, field, &d))
    return false;
  return (unsigned)(d.mask >> d.shift) == 0xffu;
}

/* A PARAMETER WRITE CARRIES THE RAW VALUE, AND THE DECODED ARRAY HOLDS THE
 * BIASED ONE. THE TWO ARE NOT THE SAME BYTE.
 *
 *   A record's field decoder produces `((word & mask) >> shift) + bias`, and
 *   what the voice path reads is that array. The wire carries the value
 *   before the bias - which is also the range the parameter map documents,
 *   since a descriptor's declared raw min..max is that map's own range - so
 *   a write has to add the bias that a record read would have added. Copying
 *   the payload instead leaves every biased field wrong by its bias, and the
 *   damage is not subtle: coarse tune is biased -48 over a raw 0..96, so a
 *   wire value of 48 meaning "no transposition" reads as +48 semitones and
 *   plays the wave sixteen times too fast.
 *
 *   An eight-bit field arrives as two nibbles, most significant first, since
 *   seven bits per byte cannot carry it, and is followed by an alias
 *   descriptor over the same bits. Both get the value.
 */
size_t packed_apply_wire_block(const struct xp_rom *rom, unsigned group,
                                const uint8_t *payload, size_t count,
                                uint8_t *fields, size_t fieldCount)
{
  if (!rom || !payload || !fields)
    return 0;
  size_t written = 0;
  for (size_t k = 0; k < count && k < fieldCount; ++k) {
    struct xp_field_descriptor d;
    if (!packed_group_descriptor(rom, group, (unsigned)k, &d))
      break;                     /* past this group's own fields */
    unsigned raw = payload[k];
    bool wide = (unsigned)(d.mask >> d.shift) == 0xffu;
    if (wide && k + 1u < count)
      raw = (unsigned)((payload[k] << 4) | (payload[k + 1u] & 0x0fu));
    fields[k] = (uint8_t)((int)raw + (int)d.bias);
    ++written;
    if (wide && k + 1u < count) {
      if (k + 1u < fieldCount) {
        fields[k + 1u] = fields[k];
        ++written;
      }
      ++k;
    }
  }
  return written;
}

bool packed_group_field(const struct xp_rom *rom, unsigned group,
                         const uint8_t *packed, size_t available,
                         unsigned field, int *value)
{
  if (!rom || !rom->bytes || !packed || !value)
    return false;
  const struct XpPackedGroup *g = group_of(xp_profile(rom), group);
  if (!g || field >= g->fieldCount)
    return false;
  struct xp_field_descriptor d;
  if (!packed_descriptor(rom, (unsigned)g->firstDescriptor + field, &d))
    return false;
  uint16_t pair;
  if (!read_pair(packed, available, d.byte_offset, &pair))
    return false;
  unsigned raw = (unsigned)((pair & d.mask) >> d.shift);
  *value = (int)raw + (int)d.bias;
  return true;
}

bool packed_open(const struct xp_rom *rom, unsigned bank, unsigned program,
                  struct xp_packed_record *out)
{
  if (!rom || !rom->bytes || !out)
    return false;
  const struct XpDeviceProfile *profile = xp_profile(rom);
  if (bank >= profile->packedBankCount || bank >= XP_PACKED_BANK_MAX)
    return false;
  const struct XpPackedBank &b = profile->packedBanks[bank];
  if (!b.recordSize || program >= b.count)
    return false;
  uint32_t at = b.base + (uint32_t)program * b.recordSize;
  if ((size_t)at + b.recordSize > rom->size)
    return false;

  /* The record's declared size must be the one its own schema implies, or
     one of the two is wrong and every field past the first sub-record
     would be read at the wrong address. */
  const struct XpPackedGroup *common = group_of(profile, b.commonGroup);
  if (!common)
    return false;
  uint32_t implied = common->packedSize;
  if (b.partCount) {
    const struct XpPackedGroup *part = group_of(profile, b.partGroup);
    if (!part)
      return false;
    implied += (uint32_t)b.partCount * part->packedSize;
  }
  if (implied != b.recordSize)
    return false;

  out->bytes = rom->bytes + at;
  out->offset = at;
  out->size = b.recordSize;
  out->common_group = b.commonGroup;
  out->part_group = b.partGroup;
  out->part_count = b.partCount;
  return true;
}

bool packed_common_field(const struct xp_rom *rom,
                          const struct xp_packed_record *record,
                          unsigned field, int *value)
{
  if (!record || !record->bytes)
    return false;
  const struct XpPackedGroup *common =
    group_of(xp_profile(rom), record->common_group);
  if (!common)
    return false;
  return packed_group_field(rom, record->common_group, record->bytes,
                             common->packedSize, field, value);
}

bool packed_part_field(const struct xp_rom *rom,
                        const struct xp_packed_record *record,
                        unsigned part, unsigned field, int *value)
{
  if (!record || !record->bytes || part >= record->part_count)
    return false;
  const struct XpDeviceProfile *profile = xp_profile(rom);
  const struct XpPackedGroup *common = group_of(profile, record->common_group);
  const struct XpPackedGroup *sub = group_of(profile, record->part_group);
  if (!common || !sub)
    return false;
  uint32_t at = (uint32_t)common->packedSize +
    (uint32_t)part * sub->packedSize;
  if (at + sub->packedSize > record->size)
    return false;
  return packed_group_field(rom, record->part_group, record->bytes + at,
                             sub->packedSize, field, value);
}

bool packed_common_name(const struct xp_rom *rom,
                         const struct xp_packed_record *record,
                         unsigned first, unsigned length, char *out)
{
  if (!out)
    return false;
  out[0] = '\0';
  for (unsigned i = 0; i < length; ++i) {
    int value = 0;
    if (!packed_common_field(rom, record, first + i, &value))
      return false;
    if (value < 0x20 || value > 0x7e)
      return false;
    out[i] = (char)value;
  }
  out[length] = '\0';
  return true;
}

bool wave_source_select(const struct xp_rom *rom, unsigned waveGroup,
                         unsigned waveGroupId, unsigned *source)
{
  if (!rom || !rom->bytes || !source)
    return false;
  const struct XpDeviceProfile *profile = xp_profile(rom);
  if (!profile->waveGroupSourceTable || !profile->waveGroupSourceCount ||
      waveGroupId >= profile->waveGroupSourceCount)
    return false;
  /* Group ID zero selects nothing that sounds - measured, by sweeping
     every group ID on the device rather than inferred from the table -
     so it is refused here rather than resolved to the same source its
     neighbour shares the table's first entry with. */
  if (!waveGroupId)
    return false;
  /* One row of selectors per wave group, laid end to end. */
  uint32_t at = profile->waveGroupSourceTable +
    (uint32_t)waveGroup * profile->waveGroupSourceCount + waveGroupId;
  if ((size_t)at >= rom->size)
    return false;
  unsigned selected = rom->bytes[at];
  if (selected >= profile->waveSourceCount)
    return false;               /* a card or expansion source, not in ROM */
  *source = selected;
  return true;
}

bool wave_number_resolve(const struct xp_rom *rom, unsigned source,
                          unsigned number, uint8_t *bank, uint16_t *row)
{
  if (!rom || !rom->bytes || !bank || !row)
    return false;
  const struct XpDeviceProfile *profile = xp_profile(rom);
  if (source >= profile->waveSourceCount || source >= XP_WAVE_SOURCE_MAX)
    return false;
  const struct XpWaveSource &s = profile->waveSources[source];
  if (!s.listRowTable || !s.listBankTable || number >= s.listCount)
    return false;
  uint32_t bankAt = s.listBankTable + number;
  uint32_t rowAt = s.listRowTable + number * 2u;
  if ((size_t)bankAt >= rom->size || (size_t)rowAt + 2u > rom->size)
    return false;
  uint8_t b = rom->bytes[bankAt];
  if (b >= profile->multisampleBankCount)
    return false;
  uint16_t r = be16(rom->bytes + rowAt);
  if (r >= profile->multisampleBanks[b].count)
    return false;
  *bank = b;
  *row = r;
  return true;
}

namespace {

const uint8_t *multisample_row(const struct xp_rom *rom, unsigned bank,
                                unsigned row)
{
  const struct XpDeviceProfile *profile = xp_profile(rom);
  if (bank >= profile->multisampleBankCount || bank >= XP_MULTISAMPLE_BANK_MAX)
    return nullptr;
  const struct XpRecordTable &t = profile->multisampleBanks[bank];
  if (!t.stride || row >= t.count)
    return nullptr;
  uint32_t at = t.base + (uint32_t)row * t.stride;
  if ((size_t)at + t.stride > rom->size)
    return nullptr;
  return rom->bytes + at;
}

}  // namespace

bool multisample_name(const struct xp_rom *rom, unsigned bank, unsigned row,
                       char *out, size_t capacity)
{
  if (!rom || !rom->bytes || !out)
    return false;
  const struct XpMultisampleLayout &l = xp_profile(rom)->multisampleLayout;
  if (!l.nameLength || capacity < (size_t)l.nameLength + 1u)
    return false;
  const uint8_t *record = multisample_row(rom, bank, row);
  if (!record)
    return false;
  for (unsigned i = 0; i < l.nameLength; ++i) {
    uint8_t ch = record[l.name + i];
    if (ch < 0x20 || ch > 0x7e)
      return false;
    out[i] = (char)ch;
  }
  out[l.nameLength] = '\0';
  return true;
}

/* A MULTISAMPLE'S SPLIT POINTS ARE UPPER BOUNDS, AND THE FIRST ONE AT OR
 * ABOVE THE KEY WINS.
 *
 *   The row holds splitCount non-decreasing bounds and refCount element
 *   references in the same order, with 0xffff marking a zone that holds no
 *   element. A row that covers the whole keyboard with one element writes
 *   127 in every bound, so scanning for the first bound at or above the key
 *   lands on zone zero for every key - which is also what makes the scan
 *   safe on a row whose tail is padding.
 */
bool multisample_select(const struct xp_rom *rom, unsigned bank, unsigned row,
                         unsigned key, struct xp_wave_zone *out)
{
  if (!rom || !rom->bytes || !out || key > 127u)
    return false;
  const struct XpDeviceProfile *profile = xp_profile(rom);
  const struct XpMultisampleLayout &l = profile->multisampleLayout;
  const uint8_t *record = multisample_row(rom, bank, row);
  if (!record || !l.splitCount || !l.refCount)
    return false;

  unsigned previous = 0;
  for (unsigned zone = 0; zone < l.splitCount && zone < l.refCount; ++zone) {
    uint8_t boundary = record[l.splitPoints + zone];
    if (boundary == 0xffu)
      break;                     /* the row's own end-of-splits sentinel */
    if (boundary < previous)
      return false;              /* not a non-decreasing split list */
    previous = boundary;
    if (key <= boundary) {
      uint16_t element = be16(record + l.elementRefs + zone * 2u);
      out->bank = (uint8_t)bank;
      out->row = (uint16_t)row;
      out->zone = (uint8_t)zone;
      out->boundary = boundary;
      out->directory = (uint8_t)bank;
      out->element = element;
      return element != 0xffffu;
    }
  }
  return false;
}

bool wave_element_open(const struct xp_rom *rom, unsigned directory,
                        unsigned index, struct xp_wave_element *out)
{
  if (!rom || !rom->bytes || !out)
    return false;
  const struct XpDeviceProfile *profile = xp_profile(rom);
  if (directory >= profile->elementDirectoryCount ||
      directory >= XP_MULTISAMPLE_BANK_MAX)
    return false;
  const struct XpRecordTable &t = profile->elementDirectories[directory];
  if (!t.stride || index >= t.count)
    return false;
  uint32_t at = t.base + (uint32_t)index * t.stride;
  if ((size_t)at + t.stride > rom->size)
    return false;
  const struct XpElementLayout &l = profile->elementLayout;
  const uint8_t *r = rom->bytes + at;

  std::memset(out, 0, sizeof *out);
  out->offset = at;
  out->attenuation = r[l.attenuation];
  out->start = be24(r + l.start);
  out->loop = be24(r + l.loop);
  out->end = be24(r + l.end);
  out->control = r[l.control];
  out->root_key = r[l.rootKey];
  out->fine_tune = be16(r + l.fineTune);
  out->loop_fine_tune = be16(r + l.loopFineTune);
  out->reverse = (out->control & l.reverseMask) != 0u;

  /* THE ELEMENT'S OWN BYTE SAYS WHICH WAY IT PLAYS, AND ITS NAME DOES NOT.
   *
   *   The control byte carries the loop mode in one field and a reverse
   *   flag in another. On the device this layout was read from the flag is
   *   set on every element of all 46 multisamples whose name begins REV and
   *   on exactly one that does not, so a name-based rule gets that one
   *   wrong; the byte gets all 48 right. Mode 2 is a one-shot, and the
   *   records that carry it put the loop point on the end.
   */
  switch (out->control & l.loopModeMask) {
  case 0: out->mode = XP_WAVE_FORWARD_LOOP; break;
  case 1: out->mode = XP_WAVE_PING_PONG_LOOP; break;
  case 2: out->mode = XP_WAVE_FORWARD_ONE_SHOT; break;
  default: return false;
  }
  if (out->reverse && out->mode == XP_WAVE_FORWARD_ONE_SHOT)
    out->mode = XP_WAVE_REVERSE_ONE_SHOT;

  if (out->start > out->end)
    return false;
  if (out->mode != XP_WAVE_FORWARD_ONE_SHOT &&
      out->mode != XP_WAVE_REVERSE_ONE_SHOT &&
      (out->loop < out->start || out->loop > out->end))
    return false;

  /* Place the element on a chip and inside a logical bank. The element's
     addresses are in the wave-address space this directory's source reads,
     which is cut into chip-sized slots; a bank is the unit the sample
     decoder works in, because each bank carries its own exponent region. */
  if (!profile->waveSourceSlotSize || !profile->waveBankSize)
    return false;
  uint32_t slot = out->start / profile->waveSourceSlotSize;
  uint32_t slotBase = slot * profile->waveSourceSlotSize;
  if (out->end >= slotBase + profile->waveSourceSlotSize)
    return false;                /* no element may cross a physical ROM */
  out->chip = profile->elementDirectoryChipBase[directory] + slot;
  if (out->chip >= XP_WAVE_CHIP_COUNT)
    return false;
  out->chip_start = out->start - slotBase;
  uint32_t withinChip = out->chip_start / profile->waveBankSize;
  uint32_t bankBase = slotBase + withinChip * profile->waveBankSize;
  if (out->end >= bankBase + profile->waveBankSize)
    return false;                /* nor a bank, for the same reason */
  out->bank = out->chip * (unsigned)(profile->waveChipSize /
                                      profile->waveBankSize) + withinChip;
  if (out->bank >= XP_WAVE_BANK_COUNT)
    return false;
  out->bank_start = out->start - bankBase;
  out->bank_loop = out->loop - bankBase;
  out->bank_end = out->end - bankBase;
  return true;
}

}}  // namespace EmuSC::Xp
