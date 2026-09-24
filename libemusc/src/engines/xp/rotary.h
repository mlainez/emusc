/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_XP_ROTARY_H
#define EMUSC_XP_ROTARY_H

#include "rom.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* JV-1080 INSERT TYPE 8 ROTARY (0-based 7). Bank slot 13.
 *
 * THE PARAMETER BINDING is the disassembly of the updater `0x0A002F94`
 * (FW-EXACT), in stored byte order. Each byte is compared with its cache
 * at `0x0901F858 + i` and written only when it changed:
 *
 *   byte 0  HiSlow:  `0x038C2E[v]` to XP 0x3338 while the speed is slow
 *   byte 1  LowSlow: `0x038C2E[v]` to XP 0x333A while the speed is slow
 *   byte 2  HiFast:  `0x038C2E[v]` to XP 0x3338 while the speed is fast
 *   byte 3  LowFast: `0x038C2E[v]` to XP 0x333A while the speed is fast
 *   byte 4  Speed:   `[0x0901F875] = v ? 127 : 0`, then
 *           `0x0A0020CC([0x0901F877], that, 127) > 64` picks the fast
 *           pair and rewrites both registers
 *   byte 5  HiAccl:  `0x03EEE8[v]` to XP 0x3928
 *   byte 6  LowAccl: `0x03EEE8[v]` to XP 0x392A
 *   byte 7  HiLvl:   `0x03856C[v] >> 1` into CRAM 19
 *   byte 8  LowLvl:  `0x03856C[v] >> 1` into CRAM 30
 *   byte 9  Separation: `0x03856C[v]` into CRAM 69 and 76, and
 *           `0x8000 | ((0x1FFF - 0x03856C[v]) >> 3)` into CRAM 70 and 77
 *   byte 10 Level:   `0x03856C[v] >> 4` into XP 0x3334
 *   byte 11 not read
 *
 * On a reload the updater first writes 0x012C to 0x3928 and 0x392A and
 * 0x2000 to IRAM3 word 28 (`0x3270`).
 *
 * WHICH PAIR IS WHICH ROTOR IS MEASURED, not read. Bytes 0/1 are both slow
 * and 2/3 both fast by the code alone. On the machine (`efx_params`), the
 * slot with byte 8 at 0 takes a 262 Hz sine down 29 dB, and the program
 * applies CRAM 30 to the signal it writes to the line after its 2 kHz
 * lowpass, CRAM 19 to the one after its 2 kHz highpass: so 7/8 are Hi/Low
 * and the 0x3338 pair is the horn. The 262 Hz sine's slow cycle, 0.0923 Hz
 * fitted over 27 s, is byte 1's 0x038C2E[1] = 3, 0.0916 Hz. The stored
 * order is therefore Hi-first, not the manual's display order.
 *
 * WHAT THE PROGRAM IS (FW-STRUCT; the opcodes are silicon, `U-R5-02`):
 * instructions 1..2 take the mono sum under CRAM +0.5 and +0.5; CRAM 13..17
 * are a biquad, (b0, b1, b2, a1, a2) = (0.9265, -1.8530, 0.9265, 1.7795,
 * -0.9263), a highpass with its poles at 1992 Hz, r = 0.962 (+14 dB at
 * 2 kHz); CRAM 26..28 are a one-pole lowpass at 2 kHz, (b1, b0, a1) =
 * (0.1659, 0.1659, 0.6682). Two delay lines are written, at 21 (after the
 * highpass and CRAM 19) and at 32 (after the lowpass and CRAM 30), and read
 * at four fixed addresses: 123 and 901 samples on the high line, 328 and
 * 1188 on the low line.
 *
 * WHAT IS MEASURED ON THE MACHINE (rotary.cc has the numbers):
 *
 *   the taps     the four ROM addresses are FIXED taps, one per side: 123
 *                and 328 reach the left, 901 and 1188 the right. The
 *                doppler is on three further, near-zero taps the addresses
 *                do not name: one per side on the high line, one shared by
 *                both sides on the low line
 *   the doppler  a tap delay of M ((1 + cos phi) / 2)^s samples, s 1.37
 *                on the horn and 1 on the drum, the horn's two sides half a
 *                turn apart
 *   the AM       on the horn only: its fixed taps and its doppler taps
 *   the speed    rotor Hz = register * 32000 / 2^20, the table's own law
 *                (the manual's 0.05..10 Hz); the register moves toward its
 *                target exponentially
 *
 * WHAT IS NOT RECOVERED, AND IS MINE:
 *
 *   Every tap gain, both doppler depths and shapes, the AM depths and
 *   phases and the 3.53-sample latency are fitted to the corpus takes; none
 *   is read. The doppler waveforms are what fits each take best, not what
 *   the program is known to compute; that the two rotors come out with
 *   different shapes is itself unexplained.
 *
 *   The ramp's time constant is measured at ONE accel value (byte 6 = 9,
 *   word 10: tau 0.861 s) and scaled as 1/word for the others.
 *
 *   SEPARATION IS NOT MODELLED. Only the factory 99 is in the corpus, and
 *   there the take shows no crossfeed of the doppler taps; what CRAM 69/70
 *   and 76/77 act on is not established. The horn's doppler AM depth, 0.717,
 *   equals `0x03856C[99] / 8192` = 0.716 - a lead that Separation scales it,
 *   not a finding.
 *
 *   The rotors' angles at load are measured (the drum's agrees between two
 *   takes to 0.05 rad), what IRAM3 word 28 holds is not. The two start
 *   0.8 rad apart, which is 0x2000 of a 16-bit turn: a lead, nothing more.
 *   A freshly built rotary starts at its target speeds; the machine zeroes
 *   the speed registers on a reload and ramps them at word 0x012C for a
 *   wait whose length is not traced.
 */

#define XP_ROTARY_PARAMETERS 11u
#define XP_ROTARY_LINE 2048u        /* power of two past 1188 + the reach */

struct xp_rotary {
  float line_hi[XP_ROTARY_LINE];
  float line_lo[XP_ROTARY_LINE];
  uint32_t pos;
  float hp_x1, hp_x2, hp_y1, hp_y2;
  float lp_x1, lp_y1;
  float hp_b0, hp_b1, hp_b2, hp_a1, hp_a2;   /* CRAM 13..17 */
  float lp_b0, lp_b1, lp_a1;                 /* CRAM 27, 26, 28 */
  float hi_gain, lo_gain;                    /* CRAM 19, CRAM 30 */
  float level;                               /* XP 0x3334 */
  double phase[2];                           /* rotor turns, 0 horn 1 drum */
  float speed[2];                            /* register, 0x038C2E units */
  float target[2];
  float ramp[2];                             /* approach per sample */
  uint16_t slow_word[2], fast_word[2];
  int8_t control;                            /* [0x0901F877] */
  bool fast;
  uint8_t param[XP_ROTARY_PARAMETERS];
  bool ready;
};

#ifdef __cplusplus
}

namespace EmuSC { namespace Xp {

/* Whether byte `index` (0-based, stored order) can hold `value`. */
bool rotary_parameter_valid(unsigned index, uint8_t value);

/* The updater's speed decision: `0x0A0020CC(control, speed ? 127 : 0, 127)
 * > 64`. `control` is the EFX control offset `[0x0901F877]`, 0 with no
 * control source. */
bool rotary_speed_fast(uint8_t speed, int control);

/* The realtime handler `0x0A007BC6`'s decision for a control change, which
 * compares against 63, not 64: `0x0A0020CC(control, speed ? 127 : 0, 127)
 * > 63`. */
bool rotary_control_fast(uint8_t speed, int control);

/* Rotor frequency in Hz for a speed-register value. */
double rotary_speed_hz(double word);

/* Builds the effect from the ROM for the eleven stored bytes. A rotary that
 * is already built keeps its lines, filters, rotor phases and speeds, so a
 * speed change ramps; pass a zeroed `*rt` for a type change. False, with
 * `*rt` left exactly as it was, where the ROM lacks a table or the slot, or
 * a byte is out of range. */
bool rotary_set(const struct xp_rom *rom, struct xp_rotary *rt,
                const uint8_t p[XP_ROTARY_PARAMETERS]);

/* A change of the EFX control offset, as the realtime handler applies it:
 * the speed targets follow `rotary_control_fast`. */
void rotary_control(struct xp_rotary *rt, int control);

/* The doppler tap delay, in samples, of rotor `rotor` (0 horn, 1 drum) at
 * angle `phi` (radians): longest at 0, zero at half a turn. */
float rotary_doppler(unsigned rotor, float phi);

/* The effect: `inL`/`inR` the insert's input bus, `outL`/`outR` its
 * return, levelled. */
void rotary_process(struct xp_rotary *rt, const float *inL, const float *inR,
                    float *outL, float *outR, size_t frames);

}}  // namespace EmuSC::Xp
#endif

#endif
