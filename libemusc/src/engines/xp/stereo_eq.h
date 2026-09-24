/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_XP_STEREO_EQ_H
#define EMUSC_XP_STEREO_EQ_H

#include "rom.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* JV-1080 INSERT TYPE 1 STEREO-EQ (0-based 0): a low shelf, a high shelf
 * and two peaking bands per channel, then the output level. Bank slot 3;
 * the left chain occupies CRAM 12..33, the right one the same words +36.
 *
 * THE PARAMETER BINDING is the disassembly of the updater `0x0A002554`
 * (FW-EXACT), in stored byte order:
 *
 *   byte 0  LowFreq   0 -> shelf block 0x039748, else 0x039802, via RAM
 *                     0x0901F874, read by the shelf writer 0x0A0022CE
 *   byte 1  LowGain   the triple at block + 6v into CRAM 13, 12, 14 and
 *                     49, 48, 50
 *   byte 2  HiFreq    0 -> 0x0398BC, else 0x039976 (writer 0x0A002372)
 *   byte 3  HiGain    CRAM 16, 15, 17 and 52, 51, 53
 *   byte 4-6          P1 Freq, Q, Gain -> peaking writer 0x0A0023F8 with
 *                     the index lists 0x03CE1E (left), 0x03CE42 (right)
 *   byte 7-9          P2 Freq, Q, Gain, lists 0x03CE30 / 0x03CE54
 *   byte 10 Level     0x03856C[v] >> 4 into the level register XP 0x3334
 *   byte 11           not read
 *
 * Level is the LAST byte, not the first: the label page lists it first.
 * The machine's own range probe (`M-091`) reads the maxima 1 30 1 30 16 4
 * 30 16 4 30 127 in exactly this order, and 1080 Rave's block
 * (01 15 00 14 09 00 13 07 01 0F 7D) is in range only under it.
 *
 * THE FOUR LISTS 0x03CE1E/30/42/54 ARE NOT COEFFICIENTS. Each is nine
 * words: seven CRAM word indices t0..t6 the band writes, then two IRAM3
 * word indices the writer zeroes, which are the band's two state words.
 * The coefficients are tabulated whole, per setting (FW-EXACT):
 *
 *   t2  0x039A30[freq], 17 words, = 2 sin(pi fc / 32000) for the manual's
 *       200 .. 8000 Hz to the LSB
 *   boost (gain > 15)  t1, t3, t4 = the triple at 0x039A52 + 6 (5 freq + Q),
 *       t6 = 0x039C50[75 freq + 5 (gain - 16) + Q], t5 = +1.0, t0 = 0
 *   cut (gain < 15)    0x03A646 + 8 (75 freq + 5 gain + Q): w0 into t0 AND
 *       t1, w1 into t3, w2 into t4, w3 into t6; t5 = +1.0
 *   flat (gain = 15)   every word zero but t2
 *
 * WHAT THE WORDS COMPUTE (FW-STRUCT, then MEASURED). The DSP's opcodes are
 * silicon (`U-R5-02`), so the realisation is read off the words, and
 * stereo_eq.cc says how. Each shelf is the first-order section
 * y = b0 x + b1 x' + a1 y' the drive family already uses from the same
 * tables. Each peaking band is a two-integrator loop,
 *
 *   L = L + f1 B',   H = x - L - q B',   B = B' + f H
 *
 * with f = t2, q = -t3, f1 = -t4, returning y = x + t6 (t1 B' + t0 B).
 * On a boost t0 is zero, so the output taps only the ONE-SAMPLE-DELAYED
 * band state and a '+15 dB' setting peaks at +10.4 dB; on a cut t0 = t1
 * and the dip is the nominal one. Which words enter where is read off the
 * tables; that the machine does exactly this, boost and cut, is measured
 * (stereo_eq.cc). The opcodes themselves remain unread (`U-R5-02`).
 */

struct xp_stereo_eq_shelf {
  float b0, b1, a1;
  float x1[2], y1[2];
};

struct xp_stereo_eq_band {
  float f, q, f1;
  float tap_delayed;             /* t6 * t1 */
  float tap_current;             /* t6 * t0, zero on every boost */
  bool flat;                     /* gain 15: the band's output is its input */
  float low[2], band[2];
};

struct xp_stereo_eq {
  struct xp_stereo_eq_shelf low_shelf, high_shelf;
  struct xp_stereo_eq_band peak[2];
  float level;
  uint8_t param[11];             /* the stored bytes these came from */
  bool ready;
};

#define XP_STEREO_EQ_PARAMETERS 11u

#ifdef __cplusplus
}

namespace EmuSC { namespace Xp {

/* Whether byte `index` (0-based, stored order) can hold `value`. */
bool stereo_eq_parameter_valid(unsigned index, uint8_t value);

/* Builds both chains from the ROM for the eleven stored bytes. A peaking
 * band whose (Freq, Q, Gain) moved has its state cleared, as the writer
 * zeroes the band's two IRAM3 words; the shelves keep theirs, as their
 * writers touch only CRAM. `*eq` must be zeroed (or built) before the first
 * call. False, with `*eq` left exactly as it was, where the ROM lacks a
 * table or a byte is out of range. */
bool stereo_eq_set(const struct xp_rom *rom, struct xp_stereo_eq *eq,
                    const uint8_t p[XP_STEREO_EQ_PARAMETERS]);

/* Clears every section's state, as a type change clears IRAM3 16..37. */
void stereo_eq_clear(struct xp_stereo_eq *eq);

/* ONE PEAKING BAND, as the writer `0x0A0023F8` builds it, for the other
 * updater that calls that writer (SPECTRUM, engines/xp/spectrum.h).
 * `freq` indexes the 17-word frequency table, `q` the five Q values and
 * `gain` 0..30 with 15 flat. `clear` zeroes the band's state, as the
 * writer zeroes the band's two IRAM3 words. False, with `*band` left as it
 * was, where the ROM lacks a table or an index is out of range. */
bool stereo_eq_band_set(const struct xp_rom *rom,
                         struct xp_stereo_eq_band *band, uint8_t freq,
                         uint8_t q, uint8_t gain, bool clear);

/* One sample of `band` on state channel `c` (0 or 1). */
float stereo_eq_band_run(struct xp_stereo_eq_band *band, unsigned c,
                          float x);

/* ONE SHELF, as the writers `0x0A0022CE` (low, `high` false) and
 * `0x0A002372` (high) build it, for every other updater that calls them.
 * Each writer takes three CRAM indices and a gain 0..30 (15 flat) and
 * copies the triple at `block + 6 gain`; the block is chosen by the RAM
 * byte 0x0901F874, which `upperCorner` stands for: nonzero selects
 * 0x039802 (400 Hz) or 0x039976 (8 kHz), zero 0x039748 (200 Hz) or
 * 0x0398BC (4 kHz) (FW-EXACT). The coefficients change and the state is
 * kept, as the writers touch only CRAM. False, with `*s` left as it was,
 * where the ROM lacks the table or the gain is past 30. */
bool stereo_eq_shelf_set(const struct xp_rom *rom, bool high,
                          bool upperCorner, uint8_t gain,
                          struct xp_stereo_eq_shelf *s);

/* One sample of `s` on state channel `c` (0 or 1). */
float stereo_eq_shelf_run(struct xp_stereo_eq_shelf *s, unsigned c, float x);

/* The effect, per channel: `inL`/`inR` the insert's input bus, `outL`/
 * `outR` its return, levelled. */
void stereo_eq_process(struct xp_stereo_eq *eq, const float *inL,
                        const float *inR, float *outL, float *outR,
                        size_t frames);

}}  // namespace EmuSC::Xp
#endif

#endif
