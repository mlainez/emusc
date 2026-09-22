/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_XP_EQ_H
#define EMUSC_XP_EQ_H

#include "rom.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The two-band output equaliser, which is on after a reset.
 *
 * Recovered exactly (`08_effects/eq.md`): each band is the first-order
 * shelf `y = c0*x + c1*x' + c2*y'`, its three coefficients read from one
 * of four 25-record blocks - low 200 Hz at `0x1609c`, low 400 Hz at
 * `0x16132`, high 3 kHz at `0x161c8`, high 6 kHz at `0x1625e` - indexed by
 * the gain byte from `0x34`, which is -12 dB, to `0x4c`, which is +12. At
 * the centre `0x40` every block has `c0 = 1` and `c1 = -c2`, so the
 * recurrence is an exact identity and a reset leaves the output untouched.
 *
 * Demo song 3 asks for +7 dB at 400 Hz and +3 dB at 3 kHz, so a device
 * that ignores this plays it with the wrong balance throughout.
 */

struct xp_eq_band {
  float c0, c1, c2;
  float x1[2], y1[2];
};

struct xp_eq {
  struct xp_eq_band low, high;
  bool enabled;
};

#ifdef __cplusplus
}

namespace EmuSC { namespace Xp {

// Two-band output equaliser for the XP-generation-1 engine (see
// engines/xp/README.md). The plain xp_eq struct above is shared,
// unrenamed, with EmuSC::Xp::Device (device.h), which embeds it by
// value, and with xp_eq_test.c, which reads it directly.

/* `lowFrequency` and `highFrequency` select the band corner: 0 is 200 Hz
 * and 400 Hz respectively for low, 3 kHz and 6 kHz for high. The gains are
 * the wire bytes, `0x34`..`0x4c`. */
/* Both bands at the identity and the equaliser enabled. A ROM that does
 * not carry the coefficient records then leaves the output untouched
 * instead of silencing it, and eq_set_params leaves the bands alone when
 * it cannot read a record. */
void eq_init(struct xp_eq *eq);

bool eq_set_params(const struct xp_rom *rom, struct xp_eq *eq,
                    uint8_t lowFrequency, uint8_t lowGain,
                    uint8_t highFrequency, uint8_t highGain);
void eq_reset(struct xp_eq *eq);

/* In place, on an interleaved stereo buffer. */
void eq_process(struct xp_eq *eq, float *stereo, size_t frames);

}}  // namespace EmuSC::Xp
#endif

#endif
