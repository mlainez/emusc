/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_SC88_REVERB_H
#define EMUSC_SC88_REVERB_H

#include "sc88_rom.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SC88_REVERB_ALLPASS_MAX 8u
#define SC88_REVERB_LINE_MAX 16u
/* The delay-line lengths in a character record are addresses in the XP's
 * delay memory, one unit per sample at the engine's own 32 kHz. */
#define SC88_REVERB_NATIVE_RATE 32000.0

/* One reverb character as the ROM describes it. `allpasses` is how many of
 * the eight coefficient pairs are enabled - seven for Room 1/2/3 and Hall 1,
 * eight for Hall 2 and Plate, none for the two delays - and every enabled
 * pair is (-0.5, +0.5), an allpass at g = 0.5 (`M-008`). */
struct sc88_reverb_character {
  uint8_t allpasses;
  uint8_t line_count;
  uint16_t lines[SC88_REVERB_LINE_MAX];   /* lengths in 32 kHz samples */
  uint16_t extent;                        /* the whole memory it spans */
};

bool sc88_reverb_read_character(const struct sc88_rom *rom, uint8_t character,
                                struct sc88_reverb_character *out);

/* The pre-LPF's eight settings, as a one-pole (`M-010`): the feedback
 * coefficient is p/8 and the input coefficient is 1 - 1/64 - p/8, so p = 0 is
 * an exact bypass and every other entry leaks one part in 64. */
bool sc88_reverb_pre_lpf(uint8_t p, float *feedback, float *input);

struct sc88_reverb_line {
  float *buf;
  unsigned len, pos;
};

struct sc88_reverb {
  struct sc88_reverb_character character;
  struct sc88_reverb_line allpass[SC88_REVERB_ALLPASS_MAX];
  struct sc88_reverb_line comb[SC88_REVERB_LINE_MAX];
  unsigned allpass_count, comb_count;
  float comb_damp_state[SC88_REVERB_LINE_MAX];
  float pre_fb, pre_in, pre_state;
  float feedback;                /* provisional: see the note in the source */
  float damp;                    /* provisional */
  float level;
  double output_rate;
  bool active;
};

bool sc88_reverb_init(struct sc88_reverb *rv, const struct sc88_rom *rom,
                      uint8_t character, double output_rate);
void sc88_reverb_destroy(struct sc88_reverb *rv);
void sc88_reverb_reset(struct sc88_reverb *rv);

/* `level`, `time` and `pre_lpf` are the GS parameters as received. The level
 * law is recovered (4*p); the mapping from `time` to a decay is not, and is
 * labelled provisional where it is applied. */
void sc88_reverb_set_params(struct sc88_reverb *rv, uint8_t level,
                            uint8_t time, uint8_t pre_lpf);

/* Adds the reverb's stereo return to `stereo`, an interleaved buffer that
 * already holds the dry mix, from a mono send bus. */
void sc88_reverb_process(struct sc88_reverb *rv, const float *send,
                         float *stereo, size_t frames);

#ifdef __cplusplus
}
#endif

#endif
