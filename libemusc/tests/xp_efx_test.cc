/* SPDX-License-Identifier: CC0-1.0 */
/* The insert effect's program bank, checked against a held image.
 *
 * These are the invariants a subtly wrong reader satisfies only by
 * accident: that the bank's own arithmetic lands exactly on the type
 * table's address, that every program word fits the 28 bits the RAM test
 * implies, that every row of the table resolves to a slot base with its
 * coefficient image exactly one program image above it, and that the row
 * after the last is not an address at all.
 *
 * No opcode is interpreted here and none should be: what a DSP operation
 * computes is silicon behaviour and is not recovered (`U-R5-02`). This
 * checks the shape of the bank, not the sound of anything in it.
 *
 * Needs the device's own control ROM and reports skipped without it.
 */
#include "engines/xp/devices/jv1080.h"
#include "engines/xp/efx.h"
#include "engines/xp/rom.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

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

  /* The bank tiles up to the type table with nothing left over, and one
     slot holds exactly one program image and one coefficient image. */
  assert(profile->efxBankBase +
         (uint32_t)profile->efxSlotStride * profile->efxSlotCount ==
         profile->efxTypeTable);
  assert(4u * XP_EFX_PROGRAM_WORDS + 2u * XP_EFX_PROGRAM_WORDS ==
         profile->efxSlotStride);

  /* Every program word in every slot fits 28 bits. */
  for (unsigned s = 0; s < profile->efxSlotCount; ++s) {
    uint32_t base = profile->efxBankBase +
      (uint32_t)profile->efxSlotStride * s;
    for (unsigned i = 0; i < XP_EFX_PROGRAM_WORDS; ++i) {
      const uint8_t *p = rom.bytes + base + 4u * i;
      uint32_t w = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8) | p[3];
      assert((w >> 28) == 0u);
    }
  }

  /* Every type resolves, and between them they reach more than one slot
     but fewer than all of them - types share programs. */
  assert(efx_type_count(&rom) == profile->efxTypeCount);
  bool seen[256] = { false };
  unsigned distinct = 0;
  unsigned withSites = 0;
  for (unsigned t = 0; t < profile->efxTypeCount; ++t) {
    struct xp_efx_program program;
    assert(efx_program_load(&rom, t, &program));
    assert(program.slot < profile->efxSlotCount);
    if (!seen[program.slot]) {
      seen[program.slot] = true;
      ++distinct;
    }
    unsigned n = efx_program_sites(&program, NULL, 0);
    if (n)
      ++withSites;
    std::vector<struct xp_efx_site> sites(n ? n : 1);
    assert(efx_program_sites(&program, sites.data(), n) == n);
    for (unsigned i = 0; i < n; ++i)
      assert(sites[i].instruction + 1u < XP_EFX_PROGRAM_WORDS);
  }
  assert(distinct > 1 && distinct < profile->efxTypeCount);
  /* Some programs reach delay memory and some do not - an equaliser has no
     delay line - so neither extreme would be right. */
  assert(withSites > 0 && withSites < profile->efxTypeCount);

  /* The row after the last is not an address, so a reader walking past the
     table stops rather than loading rubbish. */
  {
    struct xp_efx_program program;
    assert(!efx_program_load(&rom, profile->efxTypeCount, &program));
  }

  /* THE OUTPUT ASSIGN'S SEND MASK, which is a real pass/fail rule that
     needs no effect to exist: routed anywhere but MIX, the insert's own
     chorus and reverb sends are EXACTLY zero, not merely small. */
  {
    bool anyNonZero = false;
    for (unsigned v = 0; v <= 127u; ++v) {
      unsigned mix = efx_send_level(&rom, XP_EFX_ASSIGN_MIX, v);
      if (mix)
        anyNonZero = true;
      /* the level itself is what MIX lets through, unmasked */
      assert(mix == efx_output_level(&rom, v));
      for (unsigned assign = 1u; assign <= 2u; ++assign)
        assert(efx_send_level(&rom, assign, v) == 0u);
    }
    /* and the table really does carry something, so the zeros above are
       the mask's doing and not an empty table's */
    assert(anyNonZero);
    assert(efx_output_level(&rom, 127u) > efx_output_level(&rom, 0u));
    /* out-of-range parameters read as nothing rather than off the end */
    assert(efx_output_level(&rom, 128u) == 0u);
  }

  printf("efx: %u types over %u slots, %u of them reaching delay memory; "
         "sends masked to zero off MIX at all 128 levels\n",
         profile->efxTypeCount, distinct, withSites);
  return 0;
}
