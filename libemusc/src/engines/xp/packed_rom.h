/* SPDX-License-Identifier: CC0-1.0 */
/*
 *  Reader for an XP-family control ROM whose preset records are
 *  descriptor-packed, and for the multisample directories such a ROM
 *  selects its waves through.
 *
 *  Two ROM layouts exist in this family. One stores a tone as a flat record
 *  of fixed byte offsets and reaches its wave through a single descriptor
 *  table; engines/xp/rom.h reads that one. The other bit-packs every preset
 *  record - a twelve-character name is 84 bits, not twelve bytes - and
 *  keeps one field descriptor table that says where each parameter's bits
 *  are, then resolves a wave through a wave-number list, a multisample
 *  record and an element directory. This file reads that one.
 *
 *  Everything it needs is XpDeviceProfile data: the descriptor table's
 *  address and geometry, the groups that partition it into record schemas,
 *  the banks the records live in, and the addresses and strides of the
 *  three wave tables. No device is named here, and a device whose
 *  packedDescriptorBase is zero simply has none of this.
 */
#ifndef EMUSC_XP_PACKED_ROM_H
#define EMUSC_XP_PACKED_ROM_H

#include "devices/profile.h"
#include "wave.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One field descriptor, as stored. The value of the field it describes is
 *
 *     ((packed[byte_offset] | packed[byte_offset + 1] << 8) & mask)
 *       >> shift) + bias
 *
 * read little-endian even where the device's own CPU is big-endian, so the
 * bit stream runs LSB-first inside each byte and a field may straddle two
 * consecutive bytes. minimum and maximum are the raw range the descriptor
 * declares, before the bias. */
struct xp_field_descriptor {
  uint16_t mask;
  uint8_t byte_offset;
  uint8_t shift;
  int8_t bias;
  uint8_t minimum;
  uint8_t maximum;
};

/* One packed record: a common block followed by part_count identical
 * sub-records, all inside one bank. */
struct xp_packed_record {
  const uint8_t *bytes;
  uint32_t offset;
  uint16_t size;
  uint8_t common_group;
  uint8_t part_group;
  uint8_t part_count;
};

/* Which multisample row a wave reference resolved to, and which of its key
 * zones a key falls in. */
struct xp_wave_zone {
  uint8_t bank;            /* multisample bank */
  uint16_t row;            /* row inside that bank */
  uint8_t zone;            /* index into the row's split points */
  uint8_t boundary;        /* the split point that won */
  uint8_t directory;       /* which element directory `element` indexes */
  uint16_t element;        /* element index, or 0xffff for an empty zone */
};

/* One wave-element record, decoded. The three addresses are in the
 * device's own wave-address space; chip and chip_start place the element
 * on a physical ROM, and bank/bank_start place it in the 1 MiB logical
 * bank the FCE decoder reads (its exponent region is per bank). */
struct xp_wave_element {
  uint32_t offset;
  uint8_t attenuation;
  uint32_t start;
  uint32_t loop;
  uint32_t end;
  uint8_t control;
  uint8_t root_key;
  uint16_t fine_tune;
  uint16_t loop_fine_tune;
  bool reverse;
  enum xp_wave_loop_type mode;
  unsigned chip;
  uint32_t chip_start;
  unsigned bank;
  uint32_t bank_start;
  uint32_t bank_loop;
  uint32_t bank_end;
};

#ifdef __cplusplus
}

namespace EmuSC { namespace Xp {

/* The descriptor table itself. */
bool packed_descriptor(const struct xp_rom *rom, unsigned index,
                        struct xp_field_descriptor *out);

/* The descriptor of field `field` of group `group`. */
bool packed_group_descriptor(const struct xp_rom *rom, unsigned group,
                              unsigned field,
                              struct xp_field_descriptor *out);

/* Whether a group's field is an eight-bit one. It matters on the wire:
 * such a field's value arrives as two nibbles, most significant first,
 * because seven bits per byte cannot carry it, and its descriptor is
 * followed by an alias describing the same bits. The descriptor says so
 * itself - a mask eight bits wide - so no field list is needed. */
bool packed_field_is_eight_bit(const struct xp_rom *rom, unsigned group,
                                unsigned field);

/* Apply one parameter-write payload, as it arrives on the wire, to a
 * group's decoded byte array - the form the voice path reads. Payload byte
 * k is field k, and each field is turned from its wire value into its
 * decoded one; see the comment on the definition for why that is not a
 * copy. Returns how many fields were written. */
size_t packed_apply_wire_block(const struct xp_rom *rom, unsigned group,
                                const uint8_t *payload, size_t count,
                                uint8_t *fields, size_t fieldCount);

/* Field `field` of a group, read out of `packed` - which must point at
 * that group's own first byte, not at the record's. */
bool packed_group_field(const struct xp_rom *rom, unsigned group,
                         const uint8_t *packed, size_t available,
                         unsigned field, int *value);

/* Record `program` of bank `bank`, both as the profile numbers them. */
bool packed_open(const struct xp_rom *rom, unsigned bank, unsigned program,
                  struct xp_packed_record *out);

/* A field of the record's common block, or of one of its sub-records. */
bool packed_common_field(const struct xp_rom *rom,
                          const struct xp_packed_record *record,
                          unsigned field, int *value);
bool packed_part_field(const struct xp_rom *rom,
                        const struct xp_packed_record *record,
                        unsigned part, unsigned field, int *value);

/* A name held as `length` consecutive character fields of the common
 * block, starting at field `first`. Writes length + 1 bytes. */
bool packed_common_name(const struct xp_rom *rom,
                         const struct xp_packed_record *record,
                         unsigned first, unsigned length, char *out);

/* The wave chain: a (wave group, wave group ID) pair selects a wave-number
 * namespace, a number in that namespace resolves to a multisample row, the
 * row's split points select a zone for the key, and the zone's element
 * reference opens an element record. */
bool wave_source_select(const struct xp_rom *rom, unsigned waveGroup,
                         unsigned waveGroupId, unsigned *source);
bool wave_number_resolve(const struct xp_rom *rom, unsigned source,
                          unsigned number, uint8_t *bank, uint16_t *row);
bool multisample_name(const struct xp_rom *rom, unsigned bank, unsigned row,
                       char *out, size_t capacity);
bool multisample_select(const struct xp_rom *rom, unsigned bank, unsigned row,
                         unsigned key, struct xp_wave_zone *out);
bool wave_element_open(const struct xp_rom *rom, unsigned directory,
                        unsigned index, struct xp_wave_element *out);

}}  // namespace EmuSC::Xp
#endif

#endif
