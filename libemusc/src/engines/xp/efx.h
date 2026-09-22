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

/* THE OUTPUT ASSIGN'S SEND MASK. The insert effect's own chorus and reverb
 * sends are written as `table[v] & mask`, and `mask` is 0xFFFF only while
 * the assign says MIX - 0x0000 for OUTPUT1 and OUTPUT2. So routing the
 * effect to a separate output forces both sends to EXACTLY zero in
 * hardware, which is the firmware implementing the manual's own rule
 * rather than anyone's reading of it (`08_effects/routing.md`, FW-EXACT).
 *
 * Returns the coefficient the firmware would write, 0 on a device with no
 * such table. `efx_output_level` carries no mask: the effect's own output
 * level is not silenced by where it is routed. */
unsigned efx_send_level(const struct xp_rom *rom, unsigned assign,
                         unsigned value);
unsigned efx_output_level(const struct xp_rom *rom, unsigned value);

/* The assign value that means MIX, which is the only one that lets the
 * sends through. */
#define XP_EFX_ASSIGN_MIX 0u

/* THE PARAMETER CONVERSION TABLES. A device keeps one run of words per
 * family of parameter, and an effect's updater indexes the run with the
 * parameter value. Reading them is what makes a per-effect parameter map
 * small: the law is nearly always "this table, at this value".
 *
 * Only the tables whose meaning is established get a name. The rest are
 * reachable by index and this header makes no claim about what they
 * convert - a name would be a guess wearing a constant's clothes. */
#define XP_EFX_TABLE_LEVEL 0u      /* master level / gain, 0..0x1FFF */
#define XP_EFX_TABLE_LFO_RATE 5u
#define XP_EFX_TABLE_PRE_DELAY 9u  /* samples at the wave rate */
#define XP_EFX_TABLE_DELAY 10u
#define XP_EFX_TABLE_PAN 13u       /* (L, R) */
#define XP_EFX_TABLE_BALANCE 14u   /* (wet, dry) */
#define XP_EFX_TABLE_HF_DAMP 15u   /* (a, 0x1FFF - a), and a bypass row */

unsigned efx_table_count(const struct xp_rom *rom);

/* How long a table is and how many words one of its entries takes. */
bool efx_table_shape(const struct xp_rom *rom, unsigned table,
                      unsigned *count, unsigned *columns);

/* One word: entry `index`, column 0 or 1. False where the device has no
 * such table, the index is past its end, or the column is past its
 * width - a caller reading off the end is told so rather than handed a
 * neighbouring table's bytes. */
bool efx_table_value(const struct xp_rom *rom, unsigned table,
                      unsigned index, unsigned column, uint16_t *out);

}}  // namespace EmuSC::Xp
#endif

#endif
