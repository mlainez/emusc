/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_XP_DELAY_H
#define EMUSC_XP_DELAY_H

#include "rom.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The separate delay block, which is a different effect from the reverb and
 * exists only in single-module mode.
 *
 * Almost all of it is recovered exactly (`08_effects/reverb.md`, its
 * "Separate delay block" section):
 *
 *   centre time  the 115 words at `0x15fb4 + 2*p` for `p = 1..0x73`, each
 *                exactly `0x8000 + floor(ms * 32)`, so one delay-memory
 *                unit is 1/32 ms and the range is 0.09 ms to 1 s
 *   L/R ratio    `0x165ca + 2*p` is `floor(p * 32 / 3)`, a Q8 percentage
 *                where `0x18` is unity and `0x78` is five times; each side
 *                tap is the centre time scaled by it and capped at one
 *                second
 *   levels       centre, left and right are `64*p` read as XP
 *                coefficients, so `p/128`; the overall level is `4*p`
 *                against 512, the same law the reverb's level follows
 *   feedback     `(124*p + 0x2100) & 0x3fff` as an XP coefficient, which
 *                is **bipolar about p = 64**: exactly zero there, -0.969
 *                at 0 and +0.954 at 127, never reaching unity
 *   pre-LPF      the same eight-entry one-pole table the reverb reads
 *
 * What is not recovered is which tap feeds the loop back, so that is a
 * labelled choice: the centre time is the delay's own period and is used.
 */

struct sc88_delay {
  float *buf;
  size_t len;
  size_t pos;
  double output_rate;
  float centre_samples, left_samples, right_samples;
  float centre_level, left_level, right_level;
  float overall, feedback;
  float pre_fb, pre_in, pre_state;
  uint8_t reverb_send;
  bool active;
};

#ifdef __cplusplus
}

namespace EmuSC { namespace Xp {

// Separate delay block for the XP-generation-1 engine (see
// engines/xp/README.md). The plain sc88_delay struct above is shared,
// unrenamed, with EmuSC::Xp::Device (device.h), which embeds it by
// value, and with sc88_delay_test.c, which reads it directly.

bool delay_init(struct sc88_delay *dl, double outputRate);
void delay_destroy(struct sc88_delay *dl);
void delay_reset(struct sc88_delay *dl);

/* One of the ten macro presets at `0x158be + 16*macro`, copied over
 * pre-LPF through reverb send exactly as writing the macro address does. */
bool delay_macro(const struct sc88_rom *rom, uint8_t macro, uint8_t out[10]);

/* The ten public parameters in their manual order: pre-LPF, centre time,
 * left ratio, right ratio, centre level, left level, right level, overall
 * level, feedback, reverb send. */
bool delay_set_params(const struct sc88_rom *rom, struct sc88_delay *dl,
                       const uint8_t p[10]);

/* Adds the delay's stereo return to `stereo` from a mono send bus, and
 * accumulates its own reverb send into `toReverb` when that is given. */
void delay_process(struct sc88_delay *dl, const float *send, float *stereo,
                    float *toReverb, size_t frames);

}}  // namespace EmuSC::Xp
#endif

#endif
