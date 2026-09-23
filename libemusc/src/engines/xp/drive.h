/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_XP_DRIVE_H
#define EMUSC_XP_DRIVE_H

#include "rom.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* THE NONLINEAR DRIVE FAMILY: JV-1080 insert types 2 OVERDRIVE and
 * 3 DISTORTION (0-based 1 and 2).
 *
 * ONE PROGRAM, ONE CODE PATH. Both types load bank slot 29, and their
 * updaters differ in exactly one argument: `0x0A0028FC` calls the 21-word
 * CRAM writer `0x0A0020EC(0, 0)` and `0x0A002918` calls `0x0A0020EC(0, 1)`,
 * then both branch to the shared parameter body `0x0A002704`
 * (`08_effects/dsp_program.md`, FW-EXACT). So the only thing that
 * separates them here is which row of the value table at `0x0384C6` the
 * sections below are built from.
 *
 * WHAT IS READ OFF THE ROM (FW-EXACT words, decoded with the XP
 * coefficient law - see drive.cc for how far that law is established):
 *
 *   value row, 21 words   three first-order sections, the row gain, the
 *                         trim, three more first-order sections and the
 *                         output gain (`coefficient_tables.md`, "The
 *                         21-word CRAM block"). Value order is (b0, b1, a1)
 *                         per section, for `y = b0 x + b1 x' + a1 y'`
 *   slot image CRAM 28/30 `0xD000` = +8.0 each; the row leaves them alone
 *   Drive   p1            `0x038832[v] >> 4` into the 9-bit register
 *                         `XP 0x3330`: 51..8191, linear
 *   Pan     p2            the pair at `0x0392A0 + 4v`, each `>> 4` into
 *                         `XP 0x3338` / `0x333A`
 *   AmpType p3            20 words from `0x03EF30 + 40t` into the CRAM
 *                         words `0x03EF08` lists, and CRAM[0x5F] = 0x5000
 *                         (+1.0) for type 0, 0x9000 (+2.0) otherwise
 *   LowGain p4            the triple at `0x039802 + 6v` into CRAM 86..88
 *   HiGain  p5            the triple at `0x0398BC + 6v` into CRAM 89..91
 *   Level   p6            `0x03856C[v] >> 4` into `XP 0x3334`
 *
 * The p1..p6 identities are the disassembly of `0x0A002704`, which is
 * also what settles p2 as Pan and p6 as Level: the table each byte indexes
 * says what it is. The factory patches agree - p2 is 64 in all five that
 * use OVERDRIVE.
 *
 * WHAT THE SECTIONS ARE is FW-STRUCT, read off the arithmetic of the
 * words: every triple of the value row sums to 1 (a one-pole/one-zero
 * section with unity DC gain or a DC blocker); the AmpType words carry the
 * exact RBJ signatures of a second-order highpass (b0 = b2 = -b1/2), a
 * peaking section (b1 = a1) and a second-order lowpass (b0 = b2 = b1/2),
 * after one first-order section; both shelves reach exactly +-15 dB at
 * their ends and are an identity at 15.
 *
 * WHAT IS NOT RECOVERED, AND IS MINE:
 *
 *   The ORDER. What each DSP instruction computes is silicon (`U-R5-02`),
 *   so the chain below takes the sections in PROGRAM order - the order of
 *   the CRAM words they occupy, since `CRAM[i]` belongs to `PRAM[i]`:
 *   S1..S3 (14..22), row gain and the two image gains (26, 28, 30), the
 *   nonlinearity, trim (32), S4..S6 (33..41), the AmpType sections
 *   (54..71), their mix (82/83), the shelves (86..91), the output gains
 *   (93, 95). Two measurements support the part of that order which
 *   matters, the part in front of the nonlinearity: the Drive sweep
 *   (`M-096`) reads as a linear pre-gain to 0.1 dB, and DISTORTION's
 *   lowest step (-28.94 dB in, row gain 12 dB above OVERDRIVE's) reads
 *   -1.09 dB where OVERDRIVE's -16.90 dB step reads -1.05.
 *
 *   CRAM 82 and 83 - A KNOWN LIMITATION. Read as one more first-order
 *   section, 83 its b0 and 82 its pole, the form every other section here
 *   has. Only AmpType 3 puts anything in 82, and there this reading does
 *   NOT reproduce the machine: the AmpType sweep (`M-096`,
 *   `closing/efx_sweep_02`) reads types 0, 1 and 2 at -3.3, -4.3 and 0.0 dB
 *   against the hardware's -3.3, -4.3 and 0.0, but type 3 at +1.4 against
 *   -1.4, with its harmonics 2.4 dB short. Four readings of the pair were
 *   tried - this one, its two words swapped, a path around the AmpType
 *   sections weighted by 82, and 82 ignored - and all four missed type 3's
 *   level by 1.6 to 4.4 dB without moving its harmonics, which says type 3
 *   changes something this chain does not have. Unresolved.
 *
 *   THE NONLINEARITY'S SHAPE. A clamp at the DSP's full scale, justified by
 *   the program's own gain staging around it and checked against the
 *   measured static transfer curve (`M-089`); drive.cc says both.
 */

struct xp_drive_fo {
  float b0, b1, a1;
  float x1, y1;
};

struct xp_drive_bq {
  float b0, b1, b2, a1, a2;
  float x1, x2, y1, y2;
};

struct xp_drive {
  float drive;
  struct xp_drive_fo pre[3];
  float gain;                    /* row gain times the two image gains */
  float trim;
  struct xp_drive_fo post[3];
  struct xp_drive_fo amp_first;
  struct xp_drive_bq amp_high, amp_peak, amp_low;
  struct xp_drive_fo amp_out;
  struct xp_drive_fo low_shelf, high_shelf;
  float out_gain;
  float pan_left, pan_right;
  unsigned row;
  uint8_t param[6];              /* the parameters these sections came from */
  bool ready;
};

#ifdef __cplusplus
}

namespace EmuSC { namespace Xp {

/* Builds every section from the ROM for one of the two value rows and the
 * type's six parameters, in their stored order (Drive, Pan, AmpType,
 * LowGain, HiGain, Level). Section state is cleared, so this is for a type
 * or parameter change, not per sample. False, with `*dr` left exactly as it
 * was, where the ROM lacks a table or a parameter is past its table. */
bool drive_set(const struct xp_rom *rom, struct xp_drive *dr, unsigned row,
                const uint8_t p[6]);

/* Whether parameter `index` (0-based, stored order) can take `value`:
 * whether its table has that entry. */
bool drive_parameter_valid(const struct xp_rom *rom, unsigned index,
                            uint8_t value);

/* The effect: `inL`/`inR` are the insert's input bus, `outL`/`outR` its
 * return, already panned and levelled. */
void drive_process(struct xp_drive *dr, const float *inL, const float *inR,
                    float *outL, float *outR, size_t frames);

/* The nonlinear stage alone, for tests: its output for input `u`. */
float drive_curve(const struct xp_drive *dr, float u);

}}  // namespace EmuSC::Xp
#endif

#endif
