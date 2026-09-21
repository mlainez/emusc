/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_XP_CHORUS_H
#define EMUSC_XP_CHORUS_H

#include "rom.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What is recovered here and what is not.
 *
 * The CPU-side parameter transforms are read out of the firmware
 * (`08_effects/chorus.md`) and three of them decode into physical units
 * with no freedom left:
 *
 *   delay     `3*p` in the delay memory's own unit of one sample at
 *             32 kHz, so 0 to 11.9 ms; GS's default 0x50 is 7.5 ms
 *   feedback  `256*floor(p/4)` read as an XP coefficient, 0 to 0.969 -
 *             monotone and bounded below unity, as a feedback path needs
 *   rate      `64*p` added per 8.0008 ms control period to a 16-bit phase
 *             accumulator, so 0.37 Hz at GS's default 3 up to 15.5 Hz
 *   pre-LPF   the same eight-entry one-pole table the reverb reads (`M-010`)
 *   level     `4*p` against a 512 full scale, as the reverb's level is
 *
 * What is **not** recovered is the DSP's audio algorithm: the startup
 * program, the instruction patches, the LFO shape and the stereo phase are
 * all undecoded. So the topology below - one modulated delay per side,
 * fed from a mono bus, with the two sides in antiphase - is a **labelled
 * choice**, not a ROM fact, and so is the scale that turns the depth
 * register into a sweep. Both are marked provisional where they are used.
 */

struct sc88_chorus {
  float *buf;                    /* one delay line, read at two taps */
  size_t len;
  size_t pos;
  double output_rate;
  /* controls, in output-rate samples and linear gains */
  double delay_samples;
  double depth_samples;          /* provisional scale, see above */
  double phase;                  /* 0..1 */
  double phase_step;             /* per output sample */
  float feedback;
  float level;
  float pre_fb, pre_in;
  float pre_state;
  float fb_state_l, fb_state_r;
  bool active;
};

#ifdef __cplusplus
}

namespace EmuSC { namespace Xp {

// Chorus for the XP-generation-1 engine (see engines/xp/README.md). The
// plain sc88_chorus struct above is shared, unrenamed, with
// EmuSC::Xp::Device (device.h), which embeds it by value, and with
// device_test.cc and sc88_chorus_test.c, which read it directly.

/* One of the eight macro presets at `0x1587e + 8*macro`: pre-LPF, level,
 * feedback, delay, rate, depth, send to reverb and send to delay, in that
 * order. Writing the chorus macro address copies these eight bytes over the
 * rest of the block - SC88-CTL handler 0x3400 is the reverb handler 0x3388's
 * sibling and calls the same copy helper. */
bool chorus_macro(const struct sc88_rom *rom, uint8_t macro, uint8_t out[8]);

bool chorus_init(struct sc88_chorus *ch, double outputRate,
                  const struct XpDeviceProfile *profile);
void chorus_destroy(struct sc88_chorus *ch);
void chorus_reset(struct sc88_chorus *ch);

/* The GS parameters as received, 0..127 each (`preLpf` 0..7). */
void chorus_set_params(const struct sc88_rom *rom, struct sc88_chorus *ch,
                        uint8_t level, uint8_t feedback, uint8_t delay,
                        uint8_t rate, uint8_t depth, uint8_t preLpf);

/* Adds the chorus's stereo return to `stereo`, which already holds the dry
 * mix, from a mono send bus. */
void chorus_process(struct sc88_chorus *ch, const float *send, float *stereo,
                     size_t frames);

}}  // namespace EmuSC::Xp
#endif

#endif
