/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_XP_SPECTRUM_H
#define EMUSC_XP_SPECTRUM_H

#include "rom.h"
#include "stereo_eq.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* JV-1080 INSERT TYPE 5 SPECTRUM (0-based 4): eight peaking bands on one
 * mono chain, then an output pan and level. Bank slot 14.
 *
 * THE PARAMETER BINDING is the disassembly of the updater `0x0A002AB8`
 * (FW-EXACT), in stored byte order:
 *
 *   byte 0-7  Band1..Band8 gain, 0..30 with 15 flat. Band i, when its byte
 *             moved, goes to STEREO-EQ's peaking writer `0x0A0023F8` with
 *             the index list `*(0x059884 + 4i)` (0x03CE66 + 18i), the
 *             frequency index byte `0x03CEF6[i]` = 1 4 7 8 10 12 13 16
 *             (250, 500, 1000, 1250, 2000, 3150, 4000, 8000 Hz) and byte 8
 *             as its Q index
 *   byte 8    Width, 0..4 = Q 0.5, 1, 2, 4, 9; when it moved all eight
 *             bands are rewritten
 *   byte 9    Pan: the pair at `0x0392A0 + 4v`, each >> 4, into the level
 *             registers XP 0x3338 / 0x333A
 *   byte 10   Level: `0x03856C[v] >> 4` into XP 0x3334
 *   byte 11   not read
 *
 * Pan and Level each go through `0x0A0020CC`, which adds a byte from RAM
 * (0x0901F877 for Pan, 0x0901F878 for Level) and clamps the sum to
 * 0..127. What writes those two bytes is not traced here; they are taken
 * as zero, as the drive family takes the same bytes.
 *
 * WHAT THE PROGRAM IS (FW-STRUCT, read off the slot image, the opcodes
 * being silicon, `U-R5-02`): instructions 0..2 read the two input words
 * (st 330, 331) under CRAM +0.5 and +0.5, the same mono sum the drive
 * program opens with; the eight index lists name one chain of CRAM words,
 * 35..98 plus 9, 12 .. 30, with no second channel; and each band carries its
 * own +1.0 pass word, so the bands are in series, not summed. One chain
 * and one pan pair: the effect is mono in, panned out.
 *
 * WHAT THE BAND IS: STEREO-EQ's, word for word and tap for tap, since it
 * is the same writer - stereo_eq.h and stereo_eq.cc carry the words and the
 * hardware-measured tap (boosts take the delayed band state, cuts add the
 * current one). MEASURED against this type on its own, spectrum.cc.
 */

#define XP_SPECTRUM_BANDS 8u
#define XP_SPECTRUM_PARAMETERS 11u

struct xp_spectrum {
  struct xp_stereo_eq_band band[XP_SPECTRUM_BANDS];
  float pan_left, pan_right;
  float level;
  uint8_t param[XP_SPECTRUM_PARAMETERS];   /* the stored bytes these came from */
  bool ready;
};

#ifdef __cplusplus
}

namespace EmuSC { namespace Xp {

/* Whether byte `index` (0-based, stored order) can hold `value`. */
bool spectrum_parameter_valid(unsigned index, uint8_t value);

/* Builds the chain from the ROM for the eleven stored bytes. A band whose
 * gain moved, or every band when Width moved, has its state cleared, as the
 * writer zeroes the band's two IRAM3 words. `*sp` must be zeroed (or
 * built) before the first call. False, with `*sp` left exactly as it was,
 * where the ROM lacks a table or a byte is out of range. */
bool spectrum_set(const struct xp_rom *rom, struct xp_spectrum *sp,
                   const uint8_t p[XP_SPECTRUM_PARAMETERS]);

/* The effect: `inL`/`inR` the insert's input bus, `outL`/`outR` its
 * return, panned and levelled. */
void spectrum_process(struct xp_spectrum *sp, const float *inL,
                       const float *inR, float *outL, float *outR,
                       size_t frames);

}}  // namespace EmuSC::Xp
#endif

#endif
