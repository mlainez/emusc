/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_XP_ENHANCER_H
#define EMUSC_XP_ENHANCER_H

#include "rom.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* JV-1080 INSERT TYPE 6 ENHANCER (0-based 5). Bank slot 2; the compounds
 * ENHANCER->CHORUS/FLANGER (slot 19) and ENHANCER->DELAY (slot 20) carry
 * the same four filter sections word for word, and are not built here.
 *
 * THE PARAMETER BINDING is the disassembly of the updater `0x0A002C38`
 * (FW-EXACT), in stored byte order, which is the label order:
 *
 *   byte 0  Sens     `0x038832[v] >> 4` into XP 0x3330 - the register and
 *                    table OVERDRIVE's Drive uses
 *   byte 1  Mix      `0x03856C[v] >> 4` into XP 0x3338
 *   byte 2  LowGain  the shelf writer `0x0A0022CE` with RAM 0x0901F874 set
 *                    to 1, so the 400 Hz block 0x039802, into CRAM 41, 40,
 *                    42 (left) and 70, 69, 71 (right)
 *   byte 3  HiGain   the writer `0x0A002372` with 0x0901F874 = 0, so the
 *                    4 kHz block 0x0398BC, into CRAM 44, 43, 45 and 73, 72, 74
 *   byte 4  Level    `0x03856C[v] >> 4` into XP 0x3334
 *   byte 5-11        not read
 *
 * Sens and Mix go through `0x0A0020CC` with the RAM offsets 0x0901F877 and
 * 0x0901F878, taken as zero as spectrum.h says.
 *
 * WHAT THE PROGRAM IS, per channel (st 330 left, 331 right, FW-STRUCT; the
 * opcodes are silicon, `U-R5-02`):
 *
 *   x -> HP -> x Sens -> x8 x8 (CRAM 28, 30) -> clamp -> x1/8 (CRAM 32)
 *     -> LP -> x Mix, added to x -> low shelf -> high shelf -> x Level
 *
 *   HP  CRAM 22..24 (left), 51..53 (right): (-g, +g, a) with g = (1 + a) / 2
 *       to the LSB - a first-order highpass normalised to unity at Nyquist,
 *       a = 0.0985 (7000 Hz) left and 0.1483 (6500 Hz) right. The section
 *       table in `08_effects/efx_sections.md` lists these as allpasses of
 *       g 0.5492 / 0.5742: it pairs two words and assumes a third, and the
 *       third word is this pole, not a unity multiply.
 *   LP  CRAM 33..35, 62..64: (b, b, a), 2b + a = 1, a first-order lowpass,
 *       the corners crossed against the highpass: 6500 Hz left, 7000 right.
 *   The x8 x8, the x1/8 and the Sens register are the drive program's
 *   (slot 29: the same words at CRAM 28, 30 and 32, the same XP 0x3330),
 *   so the clamp between them is drive.cc's: saturation at +-1.0. No
 *   ENHANCER measurement reaches it (below).
 *   Section words are taken in program order as (b1, b0, a1), the order
 *   the shelf writers and the drive row put them in; the enhancement's sign
 *   rests on that and is MEASURED (enhancer.cc).
 *
 * The DC-blocking words at CRAM 5/7 (+0.998, -0.0039), which the drive
 * program carries at 8/10, are not modelled here; a 7 kHz highpass ahead of
 * the only nonlinearity makes them inaudible on this type.
 */

struct xp_enhancer_fo {
  float b0, b1, a1;
  float x1, y1;
};

struct xp_enhancer_channel {
  struct xp_enhancer_fo high, low, low_shelf, high_shelf;
};

#define XP_ENHANCER_PARAMETERS 5u

struct xp_enhancer {
  struct xp_enhancer_channel ch[2];
  float drive;                   /* Sens times the two x8 words */
  float trim;
  float mix;
  float level;
  uint8_t param[XP_ENHANCER_PARAMETERS];
  bool ready;
};

#ifdef __cplusplus
}

namespace EmuSC { namespace Xp {

/* Whether byte `index` (0-based, stored order) can hold `value`. */
bool enhancer_parameter_valid(unsigned index, uint8_t value);

/* Builds both channels from the ROM for the five stored bytes. Section
 * state is kept - the updater writes CRAM and registers only - so this
 * serves a parameter change as well as a first build (`*en` zeroed). False,
 * with `*en` left exactly as it was, where the ROM lacks a table or the
 * program image, or a byte is out of range. */
bool enhancer_set(const struct xp_rom *rom, struct xp_enhancer *en,
                   const uint8_t p[XP_ENHANCER_PARAMETERS]);

/* The effect, per channel: `inL`/`inR` the insert's input bus, `outL`/
 * `outR` its return, levelled. */
void enhancer_process(struct xp_enhancer *en, const float *inL,
                       const float *inR, float *outL, float *outR,
                       size_t frames);

}}  // namespace EmuSC::Xp
#endif

#endif
