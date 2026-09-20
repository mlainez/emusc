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

/* The reverb's delay-line graph is read out of the DSP program in the
 * control ROM (`M-173`, scdb `08_effects/dsp_program.md`). Twelve ERAM
 * buffers, eight output taps:
 *
 *   B0 B1 B2 B3   four series allpasses at g = 0.5, the input diffuser
 *   B4 B5 B6 B7   tank half 1: allpass, delay, allpass, delay
 *   B8 B9 B10 B11 tank half 2: allpass, delay, allpass, delay
 *   eight taps    read inside the tank, two per pair of program slots
 *
 * The twelve buffer heads are the twelve instructions with the ERAM write
 * enable (bit 24) set; the twenty reads have it clear, and each buffer's
 * far end sits one address below the next head. The eight taps are reads
 * that land inside a buffer rather than at its end, and they are the early
 * field. */
#define SC88_REVERB_BUFFERS 12u
#define SC88_REVERB_TAPS 8u
#define SC88_REVERB_HALF_BUFFERS 4u
/* The delay-line lengths in a character record are addresses in the XP's
 * delay memory, one unit per sample at the engine's own 32 kHz. */
#define SC88_REVERB_NATIVE_RATE 32000.0

/* One reverb character as the ROM describes it: the graph above with this
 * character's own addresses, allpass enables and damping.
 *
 * `head` and `far` are ERAM addresses relative to the character's own base,
 * so buffer i runs from head[i] to far[i] inclusive and delivers its input
 * `far[i] - head[i]` samples later. `tap` are eight more addresses, which
 * land inside the buffers. `allpass[i]` is set where the character enables
 * that section's (-0.5, +0.5) coefficient pair; a disabled section still
 * has its buffer and runs as a plain delay. */
struct sc88_reverb_character {
  uint16_t head[SC88_REVERB_BUFFERS];
  uint16_t far[SC88_REVERB_BUFFERS];
  uint16_t tap[SC88_REVERB_TAPS];
  bool allpass[SC88_REVERB_BUFFERS];
  uint8_t allpasses;                      /* how many pairs are enabled */
  uint16_t extent;                        /* the whole memory it spans */
  /* The per-half damping one-pole, from the character record's words
     16..19. Those four words are the coefficients at CRAM (59, 58) and
     (74, 75), one pair immediately before each tank half's reads, so each
     half has its own filter: on Room 1 and Plate the two differ, on Room 3,
     Hall 1 and Hall 2 they are equal. Each pair is one positive word and
     one negative word and the fields hold them in that order, so the filter
     is `y = -damp_pole*x + damp_input*y'`: the POSITIVE word is the pole
     and the negative one multiplies the input (`P-0360`). */
  float damp_input[2], damp_pole[2];
  /* The record's 53rd word, word 52, which the loader writes to an XP
     control register rather than to coefficient or program memory: the
     character's own return trim. `load_effect_character` (0x1216b) ends
     with one final direct write to `final_destination` of the layout's
     destination map - 0x3376, 0x336a and 0x3378 for layouts 0, 2 and 4 -
     which in every layout is the register two bytes below that layout's
     reverb Level (0x3378, 0x336c, 0x337a). `update_reverb_character`
     (0x121a6) clears exactly that pair, the two-word list at 0x1655e, to
     mute the module before reprogramming, and `update_effect_block_a`
     (0x12057) then restores the block with Level written LAST. So the two
     are one return path: Level carries the parameter, this word carries
     the character.
     Room 1, Room 2, Room 3, Hall 2 and Plate carry 32; Hall 1, Delay and
     Panning Delay carry 64; the two transition records carry 0, which is
     what holds the module silent across a program swap. The register's
     full scale is 512 = unity (`M-175`), so Hall 1 returns exactly twice
     what the other five reverb characters return. */
  uint16_t return_trim;
};

bool sc88_reverb_read_character(const struct sc88_rom *rom, uint8_t character,
                                struct sc88_reverb_character *out);

/* The pre-LPF's eight settings, as a one-pole (`M-010`): the feedback
 * coefficient is p/8 and the input coefficient is 1 - 1/64 - p/8, so p = 0 is
 * an exact bypass and every other entry leaks one part in 64. */
bool sc88_reverb_pre_lpf(uint8_t p, float *feedback, float *input);

/* The eight tap gains, read from the DSP program's own coefficient RAM at
 * the tap instructions. They are not part of the character record - every
 * character shares them, and both program images carry the same eight. */
bool sc88_reverb_tap_gains(const struct sc88_rom *rom,
                           float gains[SC88_REVERB_TAPS]);

struct sc88_reverb {
  struct sc88_reverb_character character;
  /* One shared delay memory, addressed the way the chip addresses it: a
     base pointer that steps back one sample per sample, so a read at
     address R of something written at address W comes back R - W samples
     later whatever buffer the two belong to. Tap addresses then need no
     assignment to a buffer - they are just reads. */
  float *eram;
  unsigned eram_len, eram_pos;
  unsigned head[SC88_REVERB_BUFFERS], far[SC88_REVERB_BUFFERS];
  unsigned tap[SC88_REVERB_TAPS];
  float tap_gain[SC88_REVERB_TAPS];
  uint8_t character_index;
  float damp_state[2];
  float tank_return;             /* half 2's output, held for half 1 */
  float pre_fb, pre_in, pre_state;
  /* Predelay, single-module only. The firmware patches ERAM address
     `0x3000 + 32*p`, and one address unit is one sample at 32 kHz, so
     32 units is a millisecond and the parameter is its own value in
     milliseconds - 0..127, as the manual prints it. */
  float *pre_delay_buf;
  size_t pre_delay_len, pre_delay_pos, pre_delay_taps;
  /* set from a decay time measured on hardware, not from a guessed curve;
     see the note in the source */
  float decay[2];
  float damp[2];                 /* the pole of each half's one-pole */
  /* the decay the parameters ask for, in seconds, for reporting */
  double target_t60;
  float level;
  float trim;                    /* the character's own return trim */
  float wet_gain_left, wet_gain_right;
  double output_rate;
  bool active;
};

/* One of the eight macro presets at `0x1583e + 8*macro`: character, pre-LPF,
 * level, time, delay feedback, the reserved byte the reverb block has at
 * `40 01 36`, and predelay, in that order. Writing the reverb macro address
 * copies these seven bytes over the rest of the block, exactly as writing
 * the delay macro copies its ten - SC88-CTL handler 0x3388 is the reverb
 * sibling of the delay's 0x342b and calls the same copy helper. */
bool sc88_reverb_macro(const struct sc88_rom *rom, uint8_t macro,
                       uint8_t out[7]);

bool sc88_reverb_init(struct sc88_reverb *rv, const struct sc88_rom *rom,
                      uint8_t character, double output_rate);
void sc88_reverb_destroy(struct sc88_reverb *rv);
void sc88_reverb_reset(struct sc88_reverb *rv);

/* `level`, `time` and `pre_lpf` are the GS parameters as received. The level
 * law is recovered (4*p); the mapping from `time` to a decay is not, and is
 * labelled provisional where it is applied. */
void sc88_reverb_set_params(struct sc88_reverb *rv, uint8_t level,
                            uint8_t time, uint8_t pre_lpf);
/* 0..127 milliseconds. */
void sc88_reverb_set_predelay(struct sc88_reverb *rv, uint8_t milliseconds);

/* Adds the reverb's stereo return to `stereo`, an interleaved buffer that
 * already holds the dry mix, from a mono send bus. */
void sc88_reverb_process(struct sc88_reverb *rv, const float *send,
                         float *stereo, size_t frames);

#ifdef __cplusplus
}
#endif

#endif
