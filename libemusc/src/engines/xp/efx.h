/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_XP_EFX_H
#define EMUSC_XP_EFX_H

#include "rom.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One insert-effect program, as the ROM holds it.
 *
 * A device whose insert effect is a loadable DSP program keeps a bank of
 * fixed-stride slots, each one a program image and a coefficient image, and
 * a table that maps the effect TYPE the user picks onto a slot. Several
 * types share a slot: they are the same program with different
 * coefficients, which is why the bank is smaller than the type list.
 *
 * WHAT THIS READS AND WHAT IT DOES NOT. It reads the bank's geometry, the
 * type table, and where each program instruction touches delay memory. It
 * does NOT interpret an opcode: what each of the DSP's operations computes
 * is silicon behaviour and is not recovered (`U-R5-02`), so nothing here
 * tries to run a program. This is the shape of the effect, not its sound.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Program and coefficient images are the same length on every device seen
   so far; a device with none leaves the profile's bank base at zero. */
#define XP_EFX_PROGRAM_WORDS 104u

/* One instruction's access to delay memory. The address is split across a
   PAIR of instructions - seven high bits in this one, nine low bits in the
   next - which is the firmware's own patcher's layout and is how an effect's
   delay times are changed without reloading its program. */
struct xp_efx_site {
  uint8_t instruction;
  uint16_t address;
  bool write;                    /* write enable clear = a read tap */
};

struct xp_efx_program {
  uint32_t pram[XP_EFX_PROGRAM_WORDS];
  uint16_t cram[XP_EFX_PROGRAM_WORDS];
  uint8_t slot;                  /* which bank slot the type resolved to */
};

#ifdef __cplusplus
}

namespace EmuSC { namespace Xp {

/* How many effect types this device's table holds, 0 where it has no bank. */
unsigned efx_type_count(const struct xp_rom *rom);

/* Resolve one type through the table and copy its two images. False where
   the device has no bank, the type is past the table, or the row does not
   point at a slot - a row that is not a pointer ends the table rather than
   being read as one. */
bool efx_program_load(const struct xp_rom *rom, unsigned type,
                       struct xp_efx_program *out);

/* Every instruction in the program that touches delay memory, in program
   order. Returns how many were found, which may exceed `max`; only `max`
   are written. */
unsigned efx_program_sites(const struct xp_efx_program *program,
                            struct xp_efx_site *out, unsigned max);

}}  // namespace EmuSC::Xp
#endif

#endif
