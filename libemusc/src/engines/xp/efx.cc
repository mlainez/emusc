/* SPDX-License-Identifier: CC0-1.0 */
#include "efx.h"

#include "devices/profile.h"

#include <cstring>

namespace EmuSC { namespace Xp {

namespace {

uint32_t be32(const uint8_t *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
    ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

uint16_t be16(const uint8_t *p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* A table row that is not a pointer ends the table. The row after the last
   real one reads 0x7F7F7F7D on this device, which is not in the ROM's
   address space at all. */
bool row_pointer(const struct XpDeviceProfile *profile, uint32_t raw,
                  uint32_t *offset)
{
  if (raw < profile->efxPointerBase)
    return false;
  uint32_t off = raw - profile->efxPointerBase;
  uint32_t span = profile->efxSlotStride * profile->efxSlotCount;
  if (off < profile->efxBankBase || off >= profile->efxBankBase + span)
    return false;
  *offset = off;
  return true;
}

}  // namespace

unsigned efx_type_count(const struct xp_rom *rom)
{
  if (!rom || !rom->bytes)
    return 0;
  const struct XpDeviceProfile *profile = xp_profile(rom);
  return profile->efxBankBase ? profile->efxTypeCount : 0u;
}

bool efx_program_load(const struct xp_rom *rom, unsigned type,
                       struct xp_efx_program *out)
{
  if (!rom || !rom->bytes || !out)
    return false;
  const struct XpDeviceProfile *profile = xp_profile(rom);
  if (!profile->efxBankBase || type >= profile->efxTypeCount)
    return false;
  uint32_t row = profile->efxTypeTable + 8u * type;
  if (row + 8u > rom->size)
    return false;
  uint32_t pram = 0;
  uint32_t cram = 0;
  if (!row_pointer(profile, be32(rom->bytes + row), &pram) ||
      !row_pointer(profile, be32(rom->bytes + row + 4u), &cram))
    return false;
  /* The coefficient image follows its program image inside the same slot,
     so a row whose two pointers do not sit that way is not a slot pair. */
  if (cram != pram + 4u * XP_EFX_PROGRAM_WORDS)
    return false;
  if ((pram - profile->efxBankBase) % profile->efxSlotStride)
    return false;
  if (cram + 2u * XP_EFX_PROGRAM_WORDS > rom->size)
    return false;
  std::memset(out, 0, sizeof *out);
  out->slot = (uint8_t)((pram - profile->efxBankBase) /
                         profile->efxSlotStride);
  for (unsigned i = 0; i < XP_EFX_PROGRAM_WORDS; ++i) {
    out->pram[i] = be32(rom->bytes + pram + 4u * i);
    out->cram[i] = be16(rom->bytes + cram + 2u * i);
  }
  return true;
}

unsigned efx_program_sites(const struct xp_efx_program *program,
                            struct xp_efx_site *out, unsigned max)
{
  if (!program)
    return 0;
  unsigned found = 0;
  /* The last instruction cannot carry an address: its partner would be
     past the end of the program. */
  for (unsigned i = 0; i + 1u < XP_EFX_PROGRAM_WORDS; ++i) {
    uint32_t w = program->pram[i];
    if (!((w >> 23) & 1u))
      continue;
    if (out && found < max) {
      out[found].instruction = (uint8_t)i;
      out[found].address = (uint16_t)
        ((((w >> 16) & 0x7fu) << 9) | ((program->pram[i + 1u] >> 16) & 0x1ffu));
      out[found].write = ((w >> 24) & 1u) != 0u;
    }
    ++found;
  }
  return found;
}

}}  // namespace EmuSC::Xp
