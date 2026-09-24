/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_XP_DYNAMICS_H
#define EMUSC_XP_DYNAMICS_H

#include "rom.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* JV-1080 INSERT TYPES 9 COMPRESSOR AND 10 LIMITER (0-based 8 and 9).
 *
 * ONE PROGRAM. Both types load bank slot 25; what separates them is the
 * coefficients their updaters write (`0x0A00326A` COMPRESSOR,
 * `0x0A00345C` LIMITER, FW-EXACT):
 *
 *   COMPRESSOR, stored order
 *     byte 0  Attack    `0x03EBB8[v]` into CRAM 13 and 65
 *     byte 1  Sustain   `0x03ECB8[v]` into CRAM 29
 *     byte 2  Pan       the pair at `0x0392A0 + 4v`, each `>> 4`, into
 *                       XP 0x3338 / 0x333A
 *     byte 3  PostGain  CRAM 76 = 0x5000 / 0x9000 / 0xC800 / 0xD000
 *     byte 4  LowGain   the shelf writer `0x0A0022CE`, 400 Hz block
 *     byte 5  HiGain    the shelf writer `0x0A002372`, 4 kHz block
 *     byte 6  Level     `0x03856C[v] >> 4` into XP 0x3334
 *
 *   LIMITER, stored order
 *     byte 0  Threshold `0x03EBB8[127 - v]` into CRAM 13 and 65 - the
 *                       COMPRESSOR's Attack words, index reversed
 *     byte 1  Release   `0x03EDB8[v]` into CRAM 26 and 27
 *     byte 2  Ratio     six words at `0x03EEB8 + 12v` into CRAM 41, 40,
 *                       47, 46, 53, 52
 *     byte 3  Pan, byte 4 PostGain, bytes 5/6 the shelves, byte 7 Level,
 *                       as the COMPRESSOR's
 *     and, on its first call, CRAM 29 = 0x0010, CRAM 62/63 = 0x1FFF / 0
 *     (the image holds 0 / 0x1FFF there) and IRAM3 word 17 = 0x400000.
 *
 * `0x03EBB8` IS A COEFFICIENT under the XP law: its top two bits are the
 * exponent, and decoded it is one exponential from 0.01599 to 15.998,
 * 60 dB in 127 steps of 0.4724 dB. The program image already holds the
 * LIMITER's Ratio-3 row at CRAM 40/41 (0 and +1.0), and the COMPRESSOR
 * never writes those words.
 *
 * THE SLOT-25 IMAGE CARRIES NO INPUT DC BLOCKER. The 0x1FF0 0x0009 0x3FE0
 * 0x0009 block that opens the drive program (drive.cc) is absent from
 * slots 0, 3, 4, 5, 14, 18, 25 and 26 of the bank, this one included, so
 * nothing of it is modelled here.
 *
 * WHAT IS MEASURED ON THE MACHINE (dynamics.cc has the numbers): the
 * signal path, the threshold, the COMPRESSOR's static curve at its factory
 * Sustain, the LIMITER's 100:1 law, both types' path gains, and the onset
 * transient at the factory time settings.
 *
 * WHAT IS NOT RECOVERED, AND IS MINE:
 *
 *   The detector's structure and its release. The opcodes are silicon
 *   (`U-R5-02`); the detector below is built to reproduce the measured
 *   onset and steady state, and its release time is bounded by
 *   measurement but not recovered.
 *
 *   COMPRESSOR Sustain and LIMITER Release are accepted, range-checked and
 *   stored, and change nothing: no capture varies either with the
 *   threshold crossed.
 *
 *   LIMITER Ratios 0-2 follow the owner's manual's 1.5:1, 2:1 and 4:1
 *   (p.87). Only Ratio 3 (100:1) is measured. What the three two-word
 *   pairs per row at `0x03EEB8` compute is not decoded.
 */

#define XP_COMPRESSOR_PARAMETERS 7u
#define XP_LIMITER_PARAMETERS 8u
#define XP_DYNAMICS_PARAMETERS 8u

struct xp_dynamics_shelf {
  float b0, b1, a1;
  float x1, y1;
};

struct xp_dynamics {
  bool limiter;
  float pre_gain;                /* CRAM 13: `0x03EBB8` */
  float exponent;                /* 1 - 1/ratio, LIMITER */
  float path_gain;               /* the effect's own gain on its signal */
  float post_gain;               /* CRAM 76 */
  float level;                   /* XP 0x3334 */
  float pan_left, pan_right;     /* XP 0x3338 / 0x333A */
  float attack_blend;            /* the smoothed share of the envelope */
  float attack_rate;             /* per sample */
  float release_rate;            /* per sample */
  struct xp_dynamics_shelf low_shelf, high_shelf;
  float peak;                    /* detector state */
  float smooth;
  uint8_t param[XP_DYNAMICS_PARAMETERS];
  bool ready;
};

#ifdef __cplusplus
}

namespace EmuSC { namespace Xp {

/* Whether byte `index` (0-based, stored order) of the COMPRESSOR
 * (`limiter` false) or LIMITER can hold `value`. */
bool dynamics_parameter_valid(const struct xp_rom *rom, bool limiter,
                               unsigned index, uint8_t value);

/* Builds the effect from the ROM: seven stored bytes for the COMPRESSOR,
 * eight for the LIMITER. A built effect of the same type keeps its
 * detector state, so this serves both a type change (pass a zeroed `*dy`)
 * and a parameter write. False, with `*dy` left exactly as it was, where
 * the ROM lacks a table or a byte is out of range. */
bool dynamics_set(const struct xp_rom *rom, struct xp_dynamics *dy,
                   bool limiter, const uint8_t *p);

/* The static gain the effect applies at detector envelope `envelope`,
 * where the envelope is taken on the pre-gained signal. */
float dynamics_static_gain(const struct xp_dynamics *dy, float envelope);

/* The detector's threshold, in this engine's sample units. */
float dynamics_threshold(void);

/* The effect: `inL`/`inR` the insert's input bus, `outL`/`outR` its
 * return, panned and levelled. */
void dynamics_process(struct xp_dynamics *dy, const float *inL,
                       const float *inR, float *outL, float *outR,
                       size_t frames);

}}  // namespace EmuSC::Xp
#endif

#endif
