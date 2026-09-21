/* SPDX-License-Identifier: CC0-1.0 */
#include "chorus.h"
#include "reverb.h"

#include "common/constants.h"
#include "devices/sc88.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace EmuSC { namespace Xp {

namespace {

/* The XP coefficient law (`08_effects/xp_coefficients.md`). */
double xp(uint16_t raw)
{
  int value = raw & 0x3fff;
  if (value & 0x2000)
    value -= 0x4000;
  return (double)value * (double)(1u << kXpCoefficientShift[raw >> 14]) / 8192.0;
}

float tap(const struct sc88_chorus *ch, double back)
{
  if (back < 1.0)
    back = 1.0;
  if (back > (double)(ch->len - 2u))
    back = (double)(ch->len - 2u);
  double read = (double)ch->pos - back;
  while (read < 0.0)
    read += (double)ch->len;
  size_t i0 = (size_t)read;
  double frac = read - (double)i0;
  size_t i1 = i0 + 1u >= ch->len ? 0u : i0 + 1u;
  return (float)((1.0 - frac) * ch->buf[i0] + frac * ch->buf[i1]);
}

}  // namespace

bool chorus_init(struct sc88_chorus *ch, double outputRate,
                  const struct XpDeviceProfile *profile)
{
  if (!ch || outputRate < 8000.0 || outputRate > 192000.0)
    return false;
  if (!profile)
    profile = &SC88_PROFILE;
  std::memset(ch, 0, sizeof *ch);
  ch->len = (size_t)(profile->chorusMaxMs * outputRate / 1000.0) + 4u;
  ch->buf = (float *)std::calloc(ch->len, sizeof *ch->buf);
  if (!ch->buf)
    return false;
  ch->output_rate = outputRate;
  ch->pre_in = 1.0f;
  /* GS's own defaults: level 0x40, feedback 0x08, delay 0x50, rate 0x03,
     depth 0x13, pre-LPF 0 (`04_protocol/sysex.md`). */
  chorus_set_params(nullptr, ch, 0x40, 0x08, 0x50, 0x03, 0x13, 0);
  ch->active = true;
  return true;
}

void chorus_destroy(struct sc88_chorus *ch)
{
  if (!ch)
    return;
  std::free(ch->buf);
  std::memset(ch, 0, sizeof *ch);
}

void chorus_reset(struct sc88_chorus *ch)
{
  if (!ch || !ch->buf)
    return;
  std::memset(ch->buf, 0, ch->len * sizeof *ch->buf);
  ch->pos = 0;
  ch->phase = 0.0;
  ch->pre_state = 0.0f;
  ch->fb_state_l = 0.0f;
  ch->fb_state_r = 0.0f;
}

bool chorus_macro(const struct sc88_rom *rom, uint8_t macro, uint8_t out[8])
{
  if (!rom || !rom->bytes || !out || macro > 7)
    return false;
  uint32_t base = xp_profile(rom)->chorusMacroTable + (uint32_t)macro * 8u;
  if (base + 8u > rom->size)
    return false;
  for (unsigned i = 0; i < 8; ++i)
    out[i] = rom->bytes[base + i];
  return true;
}

void chorus_set_params(const struct sc88_rom *rom, struct sc88_chorus *ch,
                        uint8_t level, uint8_t feedback, uint8_t delay,
                        uint8_t rate, uint8_t depth, uint8_t preLpf)
{
  (void)rom;
  if (!ch)
    return;
  double scale = ch->output_rate / kXpNativeRate;

  float fb, in;
  if (reverb_pre_lpf(preLpf > 7 ? 7 : preLpf, &fb, &in)) {
    ch->pre_fb = fb;
    ch->pre_in = in;
  }
  /* Recovered exactly: `3*p` samples of delay memory. */
  ch->delay_samples = 3.0 * (double)(delay > 127 ? 127 : delay) * scale;
  /* Recovered exactly: `256*floor(p/4)` is an XP coefficient. */
  ch->feedback = (float)xp(
    (uint16_t)(256u * ((unsigned)(feedback > 127 ? 127 : feedback) / 4u)));
  /* Recovered exactly: `4*p` against a 512 full scale, as the reverb's. */
  ch->level = (float)(4u * (unsigned)(level > 127 ? 127 : level)) / 512.0f;
  /* Recovered exactly: `64*p` per control period on a 16-bit accumulator,
     which puts GS's default rate of 3 at 0.37 Hz and the top of the range
     at 15.5 Hz. */
  ch->phase_step = 64.0 * (double)(rate > 127 ? 127 : rate) / 65536.0 /
    kXpControlPeriodSeconds / ch->output_rate;

  /* The register the firmware forms is exact; its unit is not. Read as
     delay-memory samples it would sweep 25 ms at GS's default depth of
     0x13, which no chorus does, so a divisor is applied to bring the
     default to about 0.2 ms of sweep. The divisor is a **labelled choice**
     and the one number here that a decode of the DSP would replace. */
  unsigned registerDepth = (unsigned)((unsigned)(depth > 127 ? 127 : depth) *
                             5080u / 127u) + 40u;
  ch->depth_samples = (double)registerDepth / 128.0 * scale;
  if (ch->depth_samples > ch->delay_samples)
    ch->depth_samples = ch->delay_samples;
}

void chorus_process(struct sc88_chorus *ch, const float *send, float *stereo,
                     size_t frames)
{
  if (!ch || !ch->active || !ch->buf || !send || !stereo)
    return;
  for (size_t k = 0; k < frames; ++k) {
    double sweep = std::sin(2.0 * 3.14159265358979323846 * ch->phase);
    float x = send[k];
    /* the pre-LPF one-pole, on the way in, as the reverb's is */
    ch->pre_state = ch->pre_in * x + ch->pre_fb * ch->pre_state;
    x = ch->pre_state;
    /* One line written once and read at two taps in antiphase. That the
       two sides are opposite ends of one sweep is the labelled part: the
       DSP's stereo phase is not decoded. */
    float left = tap(ch, ch->delay_samples + sweep * ch->depth_samples);
    float right = tap(ch, ch->delay_samples - sweep * ch->depth_samples);
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

}}  // namespace EmuSC::Xp
