/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_XP_PHASER_H
#define EMUSC_XP_PHASER_H

#include "rom.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* JV-1080 INSERT TYPE 4 PHASER (0-based 3). Bank slot 12.
 *
 * THE PARAMETER BINDING is the disassembly of the updater `0x0A002938`
 * (FW-EXACT), in stored byte order. None of it writes an allpass
 * coefficient: the swept coefficient is formed inside the DSP program.
 *
 *   byte 0  Manual: `0x0A0020CC(byte[0x0901F877], v, 125)` then the word
 *           at `0x038A32 + 2v`, written RAW to the register XP 0x3338
 *   byte 1  Rate: `0x0A0020CC(byte[0x0901F878], v, 125)` then
 *           `0x038B2E[v]` into CRAM 14
 *   byte 2  Depth: `0x038732[v]` into CRAM 17
 *   byte 3  Res: `0x038932[v]` into CRAM 7
 *   byte 4  Mix: `0x03856C[v]` into CRAM 97
 *   byte 5  Pan: the pair at `0x0392A0 + 4v` into CRAM 99 and 100
 *   byte 6  Level: `0x03856C[v] >> 4` into XP 0x3334
 *   7..11   not read
 *
 * `0x0A0020CC` adds its first argument to the value and clamps the sum to
 * 0..max. The two RAM bytes it adds are taken as zero, as the drive
 * family takes them.
 *
 * THE MANUAL WORD IS A COEFFICIENT, READ OFF THE TABLE ALONE. Taken as a
 * 10-bit two's-complement number r, `0x038A32` holds
 * r = round(-512 a(f)) for the owner's manual's own Manual list (p.85:
 * 100-290 Hz in 10 Hz steps, 300-980 in 20, 1k-8k in 100), where a(f) is
 * the first-order allpass coefficient whose phase lag at f is exactly
 * 22.5 degrees - pi/8, which puts the FIRST NOTCH of an eight-stage chain at
 * f. Over all 126 entries the residual is 0.29 counts rms, 0.52 at
 * worst, which is rounding; the same test at 45, 67.5 and 90 degrees
 * misses by 33 to 65 counts rms. So the table itself says eight stages and
 * a coefficient scale of 512.
 *
 * WHAT THE PROGRAM IS (FW-STRUCT, the opcodes being silicon, `U-R5-02`):
 * instructions 0..2 read the two input words under CRAM +0.5 and +0.5, the
 * mono sum the drive and SPECTRUM programs open with; instructions 27..74
 * are one 12-word block four times over, each holding two sections - eight
 * stages. One pan pair: mono in, panned out.
 *
 * WHAT IS MEASURED ON THE MACHINE (phaser.cc has the numbers):
 *
 *   the topology  out = x + mix * y,  y = AP(a)^8 [x + g y[n-1]], with
 *                 AP(a) = (a + z^-1) / (1 + a z^-1), g = CRAM 7, mix =
 *                 CRAM 97. The feedback takes the chain's PREVIOUS output
 *                 sample; without that delay the peaks sit 10-14 % off
 *   the sweep     a = 0.900 * clamp(-r/512 - d * tri, -1, +1), d = CRAM 17,
 *                 tri a unipolar triangle, 0 at Manual: the LFO sweeps DOWN
 *                 from the Manual frequency, linearly in the coefficient,
 *                 and parks at a = -0.9 where the sum would pass -1
 *   the rate      CRAM 14 * 32000 / 2^24 Hz, the chorus LFO's law
 *
 * WHAT IS NOT RECOVERED, AND IS MINE:
 *
 *   The 0.900 is MEASURED, not read: nine static settings put the
 *   hardware's coefficient at 0.8945 to 0.9083 of -r/512, and a coefficient
 *   of exactly zero at r = 0, so it is a scale and not an offset. The slot
 *   image carries 0x1CCD = 0.9000 at CRAM 20; whether that word is the
 *   scale is not established.
 *
 *   The LFO's phase when the effect loads is not measured. It starts at 0
 *   (at Manual) and free-runs across parameter writes, as the modulated
 *   types' LFO does on this device.
 *
 *   A DT1 writing Manual 0 is discarded (`efx_pnoise`'s Manual-0 slot
 *   changed nothing; the manual gives EFX parameter 1 a floor of 32), while
 *   factory patches store 31 (PR-B 055). Only the updater's clamp to
 *   0..125 is applied here, so a patch's 31 is kept and a DT1's 0 is not
 *   refused.
 */

#define XP_PHASER_PARAMETERS 7u
#define XP_PHASER_STAGES 8u

struct xp_phaser {
  float stage_x[XP_PHASER_STAGES];
  float stage_y[XP_PHASER_STAGES];
  float feedback_state;          /* the chain's previous output */
  float manual;                  /* -r / 512 */
  float depth;                   /* CRAM 17 */
  float feedback;                /* CRAM 7 */
  float mix;                     /* CRAM 97 */
  float pan_left, pan_right;     /* CRAM 99, 100 */
  float level;                   /* XP 0x3334 */
  uint32_t lfo_phase;            /* 24-bit accumulator */
  uint32_t lfo_step;             /* CRAM 14 */
  uint8_t param[XP_PHASER_PARAMETERS];
  bool ready;
};

#ifdef __cplusplus
}

namespace EmuSC { namespace Xp {

/* Whether byte `index` (0-based, stored order) can hold `value`. */
bool phaser_parameter_valid(unsigned index, uint8_t value);

/* Builds the effect from the ROM for the seven stored bytes. A phaser that
 * is already built keeps its stage state and LFO phase, so this serves
 * both a type change (pass a zeroed `*ph`) and a parameter write. False,
 * with `*ph` left exactly as it was, where the ROM lacks a table or a byte
 * is out of range. */
bool phaser_set(const struct xp_rom *rom, struct xp_phaser *ph,
                 const uint8_t p[XP_PHASER_PARAMETERS]);

/* The coefficient every stage runs at for LFO position `tri` (0..1). */
float phaser_coefficient(const struct xp_phaser *ph, float tri);

/* The effect: `inL`/`inR` the insert's input bus, `outL`/`outR` its
 * return, panned and levelled. */
void phaser_process(struct xp_phaser *ph, const float *inL, const float *inR,
                     float *outL, float *outR, size_t frames);

}}  // namespace EmuSC::Xp
#endif

#endif
