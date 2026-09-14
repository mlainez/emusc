/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_chorus.h"
#include "sc88_reverb.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The delay memory counts in samples at 32 kHz, the rate the sound chip
   runs its lines at, so every recovered length is converted from that. */
/* The eight macro presets, 8 bytes each, read by SC88-CTL handler 0x3400 and
 * by the power-on loader at 0x44a8. The reset image at ROM 0x13104 carries
 * macro 2 and that macro's own eight bytes, which are the manual's printed
 * chorus defaults byte for byte. */
#define SC88_CHORUS_MACRO_TABLE 0x1587eu
#define SC88_CHORUS_NATIVE_RATE 32000.0
/* The voice-control task wakes every 8.0008 ms (`M-006`), which is the
   period the rate register is added over. */
#define SC88_CHORUS_PERIOD 0.0080008
/* `3*p` reaches 381 samples and the sweep is added on top, so the line is
   sized for the longest delay the register can ask for plus the deepest
   sweep, with a margin for interpolation. */
#define SC88_CHORUS_MAX_MS 64.0

static const unsigned sc88_chorus_shift[4] = {0u, 1u, 2u, 4u};

/* The XP coefficient law (`08_effects/xp_coefficients.md`). */
static double sc88_chorus_xp(uint16_t raw)
{
  int value = raw & 0x3fff;
  if (value & 0x2000)
    value -= 0x4000;
  return (double)value * (double)(1u << sc88_chorus_shift[raw >> 14]) /
    8192.0;
}

bool sc88_chorus_init(struct sc88_chorus *ch, double output_rate)
{
  if (!ch || output_rate < 8000.0 || output_rate > 192000.0)
    return false;
  memset(ch, 0, sizeof *ch);
  ch->len = (size_t)(SC88_CHORUS_MAX_MS * output_rate / 1000.0) + 4u;
  ch->buf = (float *)calloc(ch->len, sizeof *ch->buf);
  if (!ch->buf)
    return false;
  ch->output_rate = output_rate;
  ch->pre_in = 1.0f;
  /* GS's own defaults: level 0x40, feedback 0x08, delay 0x50, rate 0x03,
     depth 0x13, pre-LPF 0 (`04_protocol/sysex.md`). */
  sc88_chorus_set_params(NULL, ch, 0x40, 0x08, 0x50, 0x03, 0x13, 0);
  ch->active = true;
  return true;
}

void sc88_chorus_destroy(struct sc88_chorus *ch)
{
  if (!ch)
    return;
  free(ch->buf);
  memset(ch, 0, sizeof *ch);
}

void sc88_chorus_reset(struct sc88_chorus *ch)
{
  if (!ch || !ch->buf)
    return;
  memset(ch->buf, 0, ch->len * sizeof *ch->buf);
  ch->pos = 0;
  ch->phase = 0.0;
  ch->pre_state = 0.0f;
  ch->fb_state_l = 0.0f;
  ch->fb_state_r = 0.0f;
}

bool sc88_chorus_macro(const struct sc88_rom *rom, uint8_t macro,
                       uint8_t out[8])
{
  uint32_t base;
  unsigned i;
  if (!rom || !rom->bytes || !out || macro > 7)
    return false;
  base = SC88_CHORUS_MACRO_TABLE + (uint32_t)macro * 8u;
  if (base + 8u > rom->size)
    return false;
  for (i = 0; i < 8; ++i)
    out[i] = rom->bytes[base + i];
  return true;
}

void sc88_chorus_set_params(const struct sc88_rom *rom,
                            struct sc88_chorus *ch, uint8_t level,
                            uint8_t feedback, uint8_t delay, uint8_t rate,
                            uint8_t depth, uint8_t pre_lpf)
{
  double scale;
  float fb, in;
  unsigned register_depth;
  (void)rom;
  if (!ch)
    return;
  scale = ch->output_rate / SC88_CHORUS_NATIVE_RATE;

  if (sc88_reverb_pre_lpf(pre_lpf > 7 ? 7 : pre_lpf, &fb, &in)) {
    ch->pre_fb = fb;
    ch->pre_in = in;
  }
  /* Recovered exactly: `3*p` samples of delay memory. */
  ch->delay_samples = 3.0 * (double)(delay > 127 ? 127 : delay) * scale;
  /* Recovered exactly: `256*floor(p/4)` is an XP coefficient. */
  ch->feedback = (float)sc88_chorus_xp(
    (uint16_t)(256u * ((unsigned)(feedback > 127 ? 127 : feedback) / 4u)));
  /* Recovered exactly: `4*p` against a 512 full scale, as the reverb's. */
  ch->level = (float)(4u * (unsigned)(level > 127 ? 127 : level)) / 512.0f;
  /* Recovered exactly: `64*p` per control period on a 16-bit accumulator,
     which puts GS's default rate of 3 at 0.37 Hz and the top of the range
     at 15.5 Hz. */
  ch->phase_step = 64.0 * (double)(rate > 127 ? 127 : rate) / 65536.0 /
    SC88_CHORUS_PERIOD / ch->output_rate;

  /* The register the firmware forms is exact; its unit is not. Read as
     delay-memory samples it would sweep 25 ms at GS's default depth of
     0x13, which no chorus does, so a divisor is applied to bring the
     default to about 0.2 ms of sweep. The divisor is a **labelled choice**
     and the one number here that a decode of the DSP would replace. */
  register_depth = (unsigned)((unsigned)(depth > 127 ? 127 : depth) *
                             5080u / 127u) + 40u;
  ch->depth_samples = (double)register_depth / 128.0 * scale;
  if (ch->depth_samples > ch->delay_samples)
    ch->depth_samples = ch->delay_samples;
}

static float sc88_chorus_tap(const struct sc88_chorus *ch, double back)
{
  double read;
  size_t i0, i1;
  double frac;
  if (back < 1.0)
    back = 1.0;
  if (back > (double)(ch->len - 2u))
    back = (double)(ch->len - 2u);
  read = (double)ch->pos - back;
  while (read < 0.0)
    read += (double)ch->len;
  i0 = (size_t)read;
  frac = read - (double)i0;
  i1 = i0 + 1u >= ch->len ? 0u : i0 + 1u;
  return (float)((1.0 - frac) * ch->buf[i0] + frac * ch->buf[i1]);
}

void sc88_chorus_process(struct sc88_chorus *ch, const float *send,
                         float *stereo, size_t frames)
{
  size_t k;
  if (!ch || !ch->active || !ch->buf || !send || !stereo)
    return;
  for (k = 0; k < frames; ++k) {
    double sweep = sin(2.0 * 3.14159265358979323846 * ch->phase);
    float left, right;
    float x = send[k];
    /* the pre-LPF one-pole, on the way in, as the reverb's is */
    ch->pre_state = ch->pre_in * x + ch->pre_fb * ch->pre_state;
    x = ch->pre_state;
    /* One line written once and read at two taps in antiphase. That the
       two sides are opposite ends of one sweep is the labelled part: the
       DSP's stereo phase is not decoded. */
    left = sc88_chorus_tap(ch, ch->delay_samples + sweep * ch->depth_samples);
    right = sc88_chorus_tap(ch,
                            ch->delay_samples - sweep * ch->depth_samples);
    ch->buf[ch->pos] = x + ch->feedback * 0.5f * (left + right);
    if (++ch->pos >= ch->len)
      ch->pos = 0;
    ch->phase += ch->phase_step;
    if (ch->phase >= 1.0)
      ch->phase -= 1.0;
    stereo[k * 2] += ch->level * left;
    stereo[k * 2 + 1] += ch->level * right;
  }
}
