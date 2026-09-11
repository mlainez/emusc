/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_reverb.h"

#include <math.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* SC88-CTL v1.01. The character pointers are offsets **within the 0x10000
 * page**; read as absolute addresses they land in unrelated code and decode
 * to plausible nonsense, which is the trap recorded in `M-008`. */
#define SC88_REVERB_POINTERS 0x1595eu
#define SC88_REVERB_PAGE 0x10000u
#define SC88_REVERB_PRE_LPF_TABLE 0x15972u
#define SC88_REVERB_CHARACTERS 10u
#define SC88_REVERB_RECORD_WORDS 53u
#define SC88_REVERB_ALLPASS_PAIR_A 0x3000u   /* -0.5 under the XP law */
#define SC88_REVERB_ALLPASS_PAIR_B 0x1000u   /* +0.5 */
#define SC88_REVERB_ALLPASS_G 0.5f

static uint16_t sc88_reverb_be16(const uint8_t *p)
{
  return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static int sc88_reverb_cmp(const void *a, const void *b)
{
  uint16_t x = *(const uint16_t *)a, y = *(const uint16_t *)b;
  return x < y ? -1 : x > y;
}

static const unsigned sc88_reverb_shift[4] = {0u, 1u, 2u, 4u};

static double sc88_reverb_xp(uint16_t raw)
{
  int value = raw & 0x3fff;
  if (value & 0x2000)
    value -= 0x4000;
  return (double)value * (double)(1u << sc88_reverb_shift[raw >> 14]) /
    8192.0;
}

bool sc88_reverb_read_character(const struct sc88_rom *rom, uint8_t character,
                                struct sc88_reverb_character *out)
{
  uint32_t block;
  uint16_t addr[32];
  unsigned i, n = 0;
  if (!rom || !rom->bytes || !out || character >= SC88_REVERB_CHARACTERS ||
      SC88_REVERB_POINTERS + 2u * SC88_REVERB_CHARACTERS > rom->size)
    return false;
  block = SC88_REVERB_PAGE +
    sc88_reverb_be16(rom->bytes + SC88_REVERB_POINTERS + 2u * character);
  if (block + 2u * SC88_REVERB_RECORD_WORDS > rom->size)
    return false;
  memset(out, 0, sizeof *out);
  /* words 0..15 are the eight coefficient pairs; an enabled one is exactly
     the allpass pair and a disabled one exactly zero, with nothing else
     appearing in those slots on any character (`M-008`) */
  for (i = 0; i < 8; ++i) {
    uint16_t a = sc88_reverb_be16(rom->bytes + block + 4u * i);
    uint16_t b = sc88_reverb_be16(rom->bytes + block + 4u * i + 2u);
    if (a == SC88_REVERB_ALLPASS_PAIR_A && b == SC88_REVERB_ALLPASS_PAIR_B)
      ++out->allpasses;
  }
  /* words 16..19 are two more coefficient pairs, not unassigned space:
     each is a one-pole `y = a*x - b*y'`, and the first is the damping the
     late bank needs. Its DC gain is below unity, so it absorbs as well as
     darkens (`M-023`). */
  {
    double a = sc88_reverb_xp(sc88_reverb_be16(rom->bytes + block + 32u));
    double b = sc88_reverb_xp(sc88_reverb_be16(rom->bytes + block + 34u));
    if (a > 0.0 && b < 0.0 && -b < 0.99) {
      out->damp_input = (float)a;
      out->damp_pole = (float)(-b);
    } else {
      out->damp_input = 1.0f;
      out->damp_pole = 0.0f;
    }
  }
  /* words 20..51 are 32 delay-memory addresses that **partition** the
     character's memory: sorted, the gaps between them are the line lengths
     and the one-sample steps are the boundaries between them. The record's
     own order is not the address order on every character, so it is sorted
     rather than read in pairs. */
  for (i = 0; i < 32; ++i)
    addr[i] = sc88_reverb_be16(rom->bytes + block + 2u * (20u + i));
  qsort(addr, 32, sizeof addr[0], sc88_reverb_cmp);
  for (i = 0; i + 1 < 32 && n < SC88_REVERB_LINE_MAX; ++i) {
    uint16_t gap = (uint16_t)(addr[i + 1] - addr[i]);
    if (gap > 1)                       /* a one-sample step is a boundary */
      out->lines[n++] = gap;
  }
  out->line_count = (uint8_t)n;
  out->extent = (uint16_t)(addr[31] - addr[0]);
  return n > 0;
}

bool sc88_reverb_pre_lpf(uint8_t p, float *feedback, float *input)
{
  if (!feedback || !input || p > 7)
    return false;
  *feedback = (float)p / 8.0f;
  /* p = 0 is an exact bypass; every other entry is one part in 64 short of
     unity, which is what a filter inside a feedback path needs (`M-010`) */
  *input = p == 0 ? 1.0f : (1.0f - 1.0f / 64.0f - (float)p / 8.0f);
  return true;
}

static bool sc88_reverb_alloc(struct sc88_reverb_line *line, unsigned len)
{
  line->buf = (float *)calloc(len ? len : 1u, sizeof *line->buf);
  line->len = len ? len : 1u;
  line->pos = 0;
  return line->buf != NULL;
}

bool sc88_reverb_init(struct sc88_reverb *rv, const struct sc88_rom *rom,
                      uint8_t character, double output_rate)
{
  unsigned i;
  double scale;
  if (!rv || output_rate < 8000.0 || output_rate > 192000.0)
    return false;
  memset(rv, 0, sizeof *rv);
  if (!sc88_reverb_read_character(rom, character, &rv->character))
    return false;
  rv->character_index = character;
  rv->output_rate = output_rate;
  /* the lengths are in the engine's own 32 kHz samples */
  scale = output_rate / SC88_REVERB_NATIVE_RATE;
  /* Which line each section uses, and the order of the chain, need the DSP's
     COL opcode table and are not recovered. The shortest lines are given to
     the diffuser and the longest to the late bank, which is what those
     lengths are shaped for - the character's own set runs from about 2 ms to
     29 ms - and the choice is a labelled one, not a recovered fact. */
  /* A section needs a line to run in, so the count of lines bounds the
     diffuser however many pairs the character enables. */
  for (i = 0; i < rv->character.allpasses && i < SC88_REVERB_ALLPASS_MAX &&
       i < rv->character.line_count; ++i) {
    unsigned len = (unsigned)(rv->character.lines[i] * scale);
    if (!sc88_reverb_alloc(&rv->allpass[i], len))
      return false;
    ++rv->allpass_count;
  }
  for (i = rv->allpass_count; i < rv->character.line_count; ++i) {
    unsigned len = (unsigned)(rv->character.lines[i] * scale);
    if (!sc88_reverb_alloc(&rv->comb[rv->comb_count], len))
      return false;
    ++rv->comb_count;
  }
  sc88_reverb_set_params(rv, 64, 64, 3);
  rv->active = true;
  return true;
}

void sc88_reverb_set_predelay(struct sc88_reverb *rv, uint8_t milliseconds)
{
  size_t want;
  if (!rv || milliseconds > 127)
    return;
  want = (size_t)(milliseconds * rv->output_rate / 1000.0);
  if (!rv->pre_delay_buf || want + 2u > rv->pre_delay_len) {
    size_t len = (size_t)(128.0 * rv->output_rate / 1000.0) + 2u;
    float *grown = (float *)calloc(len ? len : 1u, sizeof *grown);
    if (!grown)
      return;
    free(rv->pre_delay_buf);
    rv->pre_delay_buf = grown;
    rv->pre_delay_len = len;
    rv->pre_delay_pos = 0;
  }
  rv->pre_delay_taps = want;
}

void sc88_reverb_destroy(struct sc88_reverb *rv)
{
  unsigned i;
  if (!rv)
    return;
  free(rv->pre_delay_buf);
  rv->pre_delay_buf = NULL;
  rv->pre_delay_len = 0;
  for (i = 0; i < SC88_REVERB_ALLPASS_MAX; ++i)
    free(rv->allpass[i].buf);
  for (i = 0; i < SC88_REVERB_LINE_MAX; ++i)
    free(rv->comb[i].buf);
  memset(rv, 0, sizeof *rv);
}

void sc88_reverb_reset(struct sc88_reverb *rv)
{
  unsigned i;
  if (!rv)
    return;
  for (i = 0; i < rv->allpass_count; ++i)
    memset(rv->allpass[i].buf, 0, rv->allpass[i].len * sizeof(float));
  for (i = 0; i < rv->comb_count; ++i)
    memset(rv->comb[i].buf, 0, rv->comb[i].len * sizeof(float));
  memset(rv->comb_damp_state, 0, sizeof rv->comb_damp_state);
  rv->pre_state = 0.0f;
  if (rv->pre_delay_buf)
    memset(rv->pre_delay_buf, 0, rv->pre_delay_len * sizeof *rv->pre_delay_buf);
  rv->pre_delay_pos = 0;
}

void sc88_reverb_set_params(struct sc88_reverb *rv, uint8_t level,
                            uint8_t time, uint8_t pre_lpf)
{
  float fb, in;
  if (!rv)
    return;
  if (sc88_reverb_pre_lpf(pre_lpf > 7 ? 7 : pre_lpf, &fb, &in)) {
    rv->pre_fb = fb;
    rv->pre_in = in;
  }
  /* Level is recovered: the CPU forms 4*p, so the parameter is a linear
     level over 0..127 against a 512 full scale. */
  rv->level = (float)(4u * (unsigned)(level > 127 ? 127 : level)) / 512.0f;
  /* Time is **not** recovered as a decay: the firmware turns it into a
     register value - min(380, floor(p*380/108)) below character 6 - whose
     meaning inside the DSP is unknown, and the character blocks carry no
     coefficient anywhere near the unity a long decay needs, so there is
     nothing in the ROM to read it off.
     What is available instead is the hardware itself. Each of the seven
     demo songs sets its own reverb and then stops playing, and the decay
     after its last note is measurable in the recordings: three songs share
     character 4 at times 53, 80 and 100 and decay in 0.66, 1.52 and
     2.59 s, which is `T60 = 0.1423 * exp(0.0292 * time)` to within the
     spread of the measurement. Characters 3 and 5 each give one point and
     sit 1.9 and 4.4 times longer at the same time value, which mean line
     length does not explain, so those factors are carried as a per-
     character table. Characters 0, 1 and 2 are unmeasured and take 1.0.
     So this is a **calibration against hardware recordings** (`M-013`),
     labelled as such, and not a decode. The previous curve was neither:
     it put this song's decay at 1.4 s against a measured 2.5 s. */
  {
    static const float character_factor[10] = {
      1.0f, 1.0f, 1.0f, 1.89f, 1.0f, 4.40f, 1.0f, 1.0f, 1.0f, 1.0f};
    unsigned index = rv->character_index < 10u ? rv->character_index : 4u;
    size_t longest = 0;
    unsigned i;
    double seconds;
    for (i = 0; i < rv->comb_count; ++i)
      if (rv->comb[i].len > longest)
        longest = rv->comb[i].len;
    rv->target_t60 = character_factor[index] * 0.1423 *
      exp(0.0292 * (double)(time > 127 ? 127 : time));
    /* The tail is set by the slowest line, so the gain is chosen to give
       the longest one the decay asked for: g = 10^(-3L/T60). */
    seconds = rv->output_rate > 0.0
      ? (double)longest / rv->output_rate : 0.0;
    if (seconds > 0.0 && rv->target_t60 > 0.01) {
      double g = pow(10.0, -3.0 * seconds / rv->target_t60);

      if (g > 0.995)
        g = 0.995;
      else if (g < 0.05)
        g = 0.05;
      rv->feedback = (float)g;
    }
  }
  /* How fast the tail darkens: the pole of the character's own late-bank
     one-pole, the first coefficient pair at words 16..17 of its block
     (`08_effects/reverb.md`). The pair's DC gain is below unity and is not
     applied here - the loop gain above is the decay calibration and stands
     for every per-pass loss - so only the pole shapes the tail, and the
     one-pole keeps unity at DC. Hall 2's pole of 0.281 leaves a band at
     4 kHz ringing about two thirds as long as one at 250 Hz, which is the
     shape the hardware recordings' song endings decay with; a character
     whose pair is empty darkens nothing. */
  rv->damp = rv->character.damp_pole;
  /* Normalised by the square root of the line count. Dividing by the count
     itself, as this did, is what a bank of *identical* sources would need;
     these are decorrelated, so their sum grows as the root and dividing by
     the count threw away about 13 dB of wet level.
     The root of the **total** count rather than of each side's is the form
     that measures right: against the hardware recording of demo song 1 the
     reverb has to fill the quiet moments of the music to within a decibel,
     and per-side normalisation overshoots by about 4 dB (`M-018`). */
  rv->wet_gain_left = rv->comb_count
    ? rv->level / sqrtf((float)rv->comb_count) : 0.0f;
  rv->wet_gain_right = rv->wet_gain_left;
}

void sc88_reverb_process(struct sc88_reverb *rv, const float *send,
                         float *stereo, size_t frames)
{
  size_t k;
  unsigned i;
  if (!rv || !rv->active || !send || !stereo)
    return;
  for (k = 0; k < frames; ++k) {
    float x = send[k];
    float wet_l = 0.0f, wet_r = 0.0f;
    if (rv->pre_delay_buf && rv->pre_delay_taps) {
      size_t read = (rv->pre_delay_pos + rv->pre_delay_len -
                     rv->pre_delay_taps) % rv->pre_delay_len;
      rv->pre_delay_buf[rv->pre_delay_pos] = x;
      x = rv->pre_delay_buf[read];
      if (++rv->pre_delay_pos >= rv->pre_delay_len)
        rv->pre_delay_pos = 0;
    }
    /* the pre-LPF one-pole, on the way in */
    rv->pre_state = rv->pre_in * x + rv->pre_fb * rv->pre_state;
    x = rv->pre_state;
    /* the diffuser: allpasses at g = 0.5, the coefficient the ROM carries */
    for (i = 0; i < rv->allpass_count; ++i) {
      struct sc88_reverb_line *ln = rv->allpass + i;
      float delayed = ln->buf[ln->pos];
      float v = x + SC88_REVERB_ALLPASS_G * delayed;
      ln->buf[ln->pos] = v;
      x = delayed - SC88_REVERB_ALLPASS_G * v;
      if (++ln->pos >= ln->len)
        ln->pos = 0;
    }
    /* the late bank, split across the two channels so the return is not mono */
    for (i = 0; i < rv->comb_count; ++i) {
      float y = rv->comb[i].buf[rv->comb[i].pos];
      rv->comb_damp_state[i] = y * (1.0f - rv->damp) +
        rv->comb_damp_state[i] * rv->damp;
      rv->comb[i].buf[rv->comb[i].pos] = x + rv->comb_damp_state[i] *
        rv->feedback;
      if (++rv->comb[i].pos >= rv->comb[i].len)
        rv->comb[i].pos = 0;
      if (i & 1u)
        wet_r += y;
      else
        wet_l += y;
    }
    if (rv->comb_count) {
      /* Each side sums the combs assigned to it, and those are mutually
         decorrelated, so their sum grows as the square root of the count
         and not the count. Dividing by the count instead cost about 13 dB
         of wet level with ten lines, which measured as 1 to 2.4 dB less
         reverb filling the quiet moments of a song than the hardware puts
         there - a smaller room (`M-018`). */
      stereo[k * 2] += wet_l * rv->wet_gain_left;
      stereo[k * 2 + 1] += wet_r * rv->wet_gain_right;
    }
  }
}
