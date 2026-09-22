/* SPDX-License-Identifier: CC0-1.0 */
/* The descriptor-packed ROM layout, checked against a held image.
 *
 * The invariants here are the ones that a subtly wrong bit-field reader
 * still satisfies by accident only with vanishing probability: that every
 * descriptor's shift is its mask's trailing zero count, that each group's
 * masks tile its record with no hole and no overlap bar its one declared
 * alias pair, that the group sizes add up to the loaders' own record sizes,
 * and that every factory patch's name comes out printable and every enabled
 * tone's wave reference resolves to a named multisample row.
 *
 * Needs the device's own ROM images and reports skipped without them, since
 * the point is the held image and not a synthetic one. The paths are read
 * from the environment at run time.
 */
#include "engines/xp/devices/jv1080.h"
#include "engines/xp/packed_rom.h"
#include "engines/xp/rom.h"
#include "engines/xp/wave.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

using namespace EmuSC::Xp;

#define SKIP 77

static std::vector<uint8_t> read_exact(const char *path, size_t expected)
{
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "cannot open %s\n", path);
    exit(1);
  }
  std::vector<uint8_t> bytes(expected);
  size_t got = fread(bytes.data(), 1, expected, f);
  int extra = fgetc(f);
  fclose(f);
  if (got != expected || extra != EOF) {
    fprintf(stderr, "%s is not %zu bytes\n", path, expected);
    exit(1);
  }
  return bytes;
}

int main(void)
{
  const char *controlPath = getenv("JV1080_CONTROL_ROM");
  const char *wavePaths = getenv("JV1080_WAVE_ROMS");
  if (!controlPath || !*controlPath) {
    printf("JV1080_CONTROL_ROM unset - skipping\n");
    return SKIP;
  }

  std::vector<uint8_t> control =
    read_exact(controlPath, JV1080_PROFILE.romSize);
  struct xp_rom rom;
  assert(rom_init(&rom, control.data(), control.size()));
  const struct XpDeviceProfile *profile = xp_profile(&rom);
  assert(profile == &JV1080_PROFILE);

  /* Every descriptor: the shift is the mask's trailing zero count and the
     mask is one contiguous run. packed_descriptor() refuses anything else,
     so reading all of them is the check. */
  for (unsigned i = 0; i < profile->packedDescriptorCount; ++i) {
    struct xp_field_descriptor d;
    assert(packed_descriptor(&rom, i, &d));
    assert(d.minimum <= d.maximum);
  }
  {
    struct xp_field_descriptor d;
    assert(!packed_descriptor(&rom, profile->packedDescriptorCount, &d));
  }

  /* Every group tiles its own record. The alias pair is the one exception
     the schema declares, so at most one overlapping descriptor per group. */
  unsigned firstAfter = 0;
  for (unsigned g = 0; g < profile->packedGroupCount; ++g) {
    const struct XpPackedGroup &group = profile->packedGroups[g];
    assert(group.firstDescriptor == firstAfter);
    firstAfter = group.firstDescriptor + group.fieldCount;
    std::vector<uint8_t> claimed((size_t)group.packedSize + 1u, 0);
    unsigned overlaps = 0;
    for (unsigned f = 0; f < group.fieldCount; ++f) {
      struct xp_field_descriptor d;
      assert(packed_descriptor(&rom, group.firstDescriptor + f, &d));
      assert(d.byte_offset < group.packedSize);
      unsigned width = 0;
      for (unsigned bit = 0; bit < 16u; ++bit)
        width += (d.mask >> bit) & 1u;
      assert(width >= 1u && width <= 8u);
      for (unsigned bit = 0; bit < 16u; ++bit) {
        if (!((d.mask >> bit) & 1u))
          continue;
        unsigned byte = d.byte_offset + bit / 8u;
        uint8_t bitMask = (uint8_t)(1u << (bit % 8u));
        if (claimed[byte] & bitMask)
          ++overlaps;
        claimed[byte] |= bitMask;
      }
    }
    assert(overlaps <= 8u);      /* the one alias pair, eight bits wide */
    /* No hole: every bit of every byte the group declares is claimed by
       some descriptor, up to the last byte a field reaches into. */
    for (unsigned byte = 0; byte + 1u < group.packedSize; ++byte)
      assert(claimed[byte] == 0xffu);
  }
  assert(firstAfter == profile->packedDescriptorCount);

  /* Each bank's record size is what its own groups add up to, which
     packed_open() checks; opening one record of every bank is the check. */
  for (unsigned b = 0; b < profile->packedBankCount; ++b) {
    struct xp_packed_record record;
    assert(packed_open(&rom, b, 0, &record));
    assert(!packed_open(&rom, b, profile->packedBanks[b].count, &record));
  }

  /* Every factory patch in every melodic bank: a printable name, and every
     enabled tone's wave reference resolving to a named multisample row. */
  unsigned patches = 0;
  unsigned enabled = 0;
  unsigned resolved = 0;
  for (unsigned m = 0; m < profile->packedMelodicBankCount; ++m) {
    unsigned bank = profile->packedMelodicBanks[m];
    for (unsigned n = 0; n < profile->packedBanks[bank].count; ++n) {
      struct xp_packed_record record;
      assert(packed_open(&rom, bank, n, &record));
      char name[16];
      assert(packed_common_name(&rom, &record, profile->patchFieldName,
                                 profile->patchFieldNameLength, name));
      assert(strlen(name) == profile->patchFieldNameLength);
      ++patches;
      for (unsigned t = 0; t < record.part_count; ++t) {
        int on = 0;
        int group = 0;
        int groupId = 0;
        int number = 0;
        assert(packed_part_field(&rom, &record, t,
                                  profile->toneFields.enable, &on));
        assert(packed_part_field(&rom, &record, t,
                                  profile->toneFields.waveGroup, &group));
        assert(packed_part_field(&rom, &record, t,
                                  profile->toneFields.waveGroupId, &groupId));
        assert(packed_part_field(&rom, &record, t,
                                  profile->toneFields.waveNumber, &number));
        if (!on)
          continue;
        ++enabled;
        unsigned source = 0;
        uint8_t msBank = 0;
        uint16_t msRow = 0;
        char wave[16];
        if (wave_source_select(&rom, (unsigned)group, (unsigned)groupId,
                                &source) &&
            wave_number_resolve(&rom, source, (unsigned)(number & 0xff),
                                 &msBank, &msRow) &&
            multisample_name(&rom, msBank, msRow, wave, sizeof wave))
          ++resolved;
      }
    }
  }
  printf("%u patches, %u enabled tones, %u resolving to a named "
         "multisample row\n", patches, enabled, resolved);
  assert(patches > 0);
  assert(enabled > 0);
  /* EVERY enabled tone resolves - 1328 of 1328 on the held image. The one
     tone in that image whose wave group ID names no internal wave list has
     its own switch off, so it never reaches this loop. */
  assert(resolved == enabled);

  /* A PARAMETER WRITE IS NOT A MEMCPY, AND THIS IS THE CHECK THAT SAYS SO.
     A write carries the raw value and the decoded array holds the biased
     one, so for every field of the tone and patch-common groups, applying
     a payload of that field's own raw minimum and maximum must land on the
     decoded range the descriptor declares. A reader that copies the
     payload instead passes nothing below. */
  {
    const struct XpPackedBank &melodic =
      profile->packedBanks[profile->packedMelodicBanks[0]];
    const unsigned groups[2] = { melodic.commonGroup, melodic.partGroup };
    for (unsigned g = 0; g < 2u; ++g) {
      const struct XpPackedGroup &group = profile->packedGroups[groups[g]];
      std::vector<uint8_t> low(group.fieldCount, 0);
      std::vector<uint8_t> high(group.fieldCount, 0);
      std::vector<uint8_t> decodedLow(group.fieldCount, 0);
      std::vector<uint8_t> decodedHigh(group.fieldCount, 0);
      for (unsigned f = 0; f < group.fieldCount; ++f) {
        struct xp_field_descriptor d;
        assert(packed_group_descriptor(&rom, groups[g], f, &d));
        low[f] = d.minimum;
        high[f] = d.maximum;
      }
      assert(packed_apply_wire_block(&rom, groups[g], low.data(), low.size(),
                                      decodedLow.data(), decodedLow.size()));
      assert(packed_apply_wire_block(&rom, groups[g], high.data(),
                                      high.size(), decodedHigh.data(),
                                      decodedHigh.size()));
      for (unsigned f = 0; f < group.fieldCount; ++f) {
        struct xp_field_descriptor d;
        assert(packed_group_descriptor(&rom, groups[g], f, &d));
        if ((unsigned)(d.mask >> d.shift) == 0xffu)
          continue;              /* an eight-bit field spans two payload
                                    bytes and is checked by its own pair */
        assert((int8_t)decodedLow[f] == (int)d.minimum + (int)d.bias);
        assert((int8_t)decodedHigh[f] == (int)d.maximum + (int)d.bias);
      }
    }
    /* The field the bug was found on, named so a regression is obvious: a
       tone's coarse tune is biased -48 over a raw 0..96, so the wire value
       48 means no transposition and must decode to zero. */
    const struct XpPackedBank &melodicBank =
      profile->packedBanks[profile->packedMelodicBanks[0]];
    std::vector<uint8_t> payload(
      profile->packedGroups[melodicBank.partGroup].fieldCount, 0);
    std::vector<uint8_t> decoded(payload.size(), 0);
    payload[profile->toneFields.coarseTune] = 48u;
    assert(packed_apply_wire_block(&rom, melodicBank.partGroup,
                                    payload.data(), payload.size(),
                                    decoded.data(), decoded.size()));
    assert((int8_t)decoded[profile->toneFields.coarseTune] == 0);
  }

  /* The wave ROMs, if they were given: the descramble must reveal each
     chip's own plaintext-free header, which is what says the address and
     data permutations are both right. */
  if (!wavePaths || !*wavePaths) {
    printf("JV1080_WAVE_ROMS unset - wave checks skipped\n");
    return 0;
  }
  std::vector<std::string> paths;
  {
    std::string all(wavePaths);
    size_t at = 0;
    while (at <= all.size()) {
      size_t comma = all.find(',', at);
      if (comma == std::string::npos)
        comma = all.size();
      paths.push_back(all.substr(at, comma - at));
      at = comma + 1u;
    }
  }
  assert(paths.size() == XP_WAVE_CHIP_COUNT);

  unsigned headers = 0;
  std::vector<std::vector<uint8_t>> decoded(XP_WAVE_CHIP_COUNT);
  for (unsigned c = 0; c < XP_WAVE_CHIP_COUNT; ++c) {
    std::vector<uint8_t> raw =
      read_exact(paths[c].c_str(), profile->waveChipSize);
    decoded[c].assign(profile->waveChipSize, 0);
    assert(wave_descramble_chip(profile, raw.data(), raw.size(),
                                decoded[c].data(), decoded[c].size()));
    /* The plaintext header passes through unchanged, and the descrambled
       header that follows it names the wave set and its date. */
    assert(memcmp(raw.data(), decoded[c].data(), profile->waveHeaderBytes) == 0);
    if (memcmp(decoded[c].data() + 0x20, "INT100", 6) == 0 &&
        memcmp(decoded[c].data() + 0x30, "1994-05-22", 10) == 0)
      ++headers;
  }
  assert(headers == XP_WAVE_CHIP_COUNT);

  /* Every element of both directories opens, and its addresses land inside
     the chip the directory's own slot base names. */
  unsigned elements = 0;
  for (unsigned d = 0; d < profile->elementDirectoryCount; ++d)
    for (unsigned i = 0; i < profile->elementDirectories[d].count; ++i) {
      struct xp_wave_element element;
      if (!wave_element_open(&rom, d, i, &element))
        continue;              /* two records in the held image read
                                  start > loop and are refused, which is
                                  1385 of its 1387 */
      assert(element.chip < XP_WAVE_CHIP_COUNT);
      assert(element.bank < XP_WAVE_BANK_COUNT);
      assert(element.bank_end < profile->waveBankSize);
      ++elements;
    }
  printf("%u wave elements open and place on a chip\n", elements);
  assert(elements > 1300u);

  /* And one of them decodes to a bounded waveform rather than to a
     saturated or silent one, which is what a wrong descramble gives. */
  struct xp_wave_element first;
  assert(wave_element_open(&rom, 0, 0, &first));
  uint32_t base = first.bank_start & ~UINT32_C(0x0f);
  size_t count = (size_t)(first.bank_end - base) + 1u;
  std::vector<int32_t> pcm(count, 0);
  struct xp_fce_decoder decoder;
  assert(fce_decoder_reset(profile, &decoder, first.bank_start));
  unsigned chipBanks =
    (unsigned)(profile->waveChipSize / profile->waveBankSize);
  const uint8_t *bank = decoded[first.bank / chipBanks].data() +
    (size_t)(first.bank % chipBanks) * profile->waveBankSize;
  for (size_t i = 0; i < count; ++i)
    assert(fce_decoder_read(&decoder, bank, profile->waveBankSize,
                            pcm.data() + i));
  int32_t peak = 0;
  double energy = 0.0;
  for (size_t i = 0; i < count; ++i) {
    int32_t magnitude = pcm[i] < 0 ? -pcm[i] : pcm[i];
    if (magnitude > peak)
      peak = magnitude;
    energy += (double)pcm[i] * pcm[i];
  }
  double peakFs = (double)peak / 8388608.0;
  double rmsFs = (count ? sqrt(energy / (double)count) : 0.0) / 8388608.0;
  printf("element 0/0: %zu samples, peak %.4f FS, rms %.4f FS\n", count,
         peakFs, rmsFs);
  assert(peakFs > 0.1 && peakFs < 0.95);
  assert(rmsFs > 0.01);

  printf("xp_packed_rom_test: all checks passed\n");
  return 0;
}
