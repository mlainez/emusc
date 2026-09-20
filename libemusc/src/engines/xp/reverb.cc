/* SPDX-License-Identifier: CC0-1.0 */
#include "reverb.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace EmuSC { namespace Xp {

namespace {

/* SC88-CTL v1.01. The character pointers are offsets **within the 0x10000
 * page**; read as absolute addresses they land in unrelated code and decode
 * to plausible nonsense, which is the trap recorded in `M-008`. */
constexpr uint32_t kReverbPointers = 0x1595eu;
constexpr uint32_t kReverbPage = 0x10000u;
/* The eight macro presets, 8 bytes each, read by SC88-CTL handler 0x3388 and
 * by the power-on loader at 0x4476. The reset image at ROM 0x13104 - the
 * patch common block whose first sixteen bytes are the default patch name -
 * carries macro 4 and that macro's own seven bytes, so a GS reset is this
 * table's Hall 2 row and not a separate set of defaults. */
constexpr uint32_t kReverbMacroTable = 0x1583eu;
constexpr unsigned kReverbCharacters = 10u;
constexpr unsigned kReverbRecordWords = 53u;
constexpr uint16_t kReverbAllpassPairA = 0x3000u;   /* -0.5 under the XP law */
constexpr uint16_t kReverbAllpassPairB = 0x1000u;   /* +0.5 */
constexpr float kReverbAllpassG = 0.5f;
/* The single-module DSP image and its coefficient RAM. CRAM[i] is the
 * coefficient of PRAM[i] - no field selects it (`M-173`) - so the gain of a
 * tap is the word at the tap's own instruction index. */
constexpr uint32_t kReverbImage0Cram = 0x78b02u + 0x480u;

uint16_t be16(const uint8_t *p)
{
  return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

constexpr unsigned kShift[4] = {0u, 1u, 2u, 4u};

double xp(uint16_t raw)
{
  int value = raw & 0x3fff;
  if (value & 0x2000)
    value -= 0x4000;
  return (double)value * (double)(1u << kShift[raw >> 14]) / 8192.0;
}

/* Which of the record's 32 delay-memory addresses is which. The record
 * always carries them in program order of the instructions they patch -
 * writes at 45 49 53 57 65 67 69 71 81 83 85 87, far-end reads at
 * 41 43 47 51 59 61 63 55 75 77 79 73 and taps at 89 91 93 95 97 99 101 103
 * for the first module, the same sequence shifted for the other two layouts.
 * These three tables are that order read back as buffer roles. */
constexpr uint8_t kHeadWord[SC88_REVERB_BUFFERS] = {
  0u, 2u, 4u, 6u, 8u, 10u, 14u, 16u, 20u, 22u, 26u, 28u};
constexpr uint8_t kFarWord[SC88_REVERB_BUFFERS] = {
  1u, 3u, 5u, 7u, 9u, 13u, 15u, 19u, 21u, 25u, 27u, 31u};
constexpr uint8_t kTapWord[SC88_REVERB_TAPS] = {
  11u, 17u, 23u, 29u, 12u, 18u, 24u, 30u};
/* The eight coefficient pairs belong to the eight buffers whose write
 * instruction carries +0.5, in the same order the record lists them. */
constexpr uint8_t kAllpassBuffer[8] = {0u, 1u, 2u, 3u, 4u, 6u, 8u, 10u};
/* PRAM indices of the eight taps in the single-module image. */
constexpr uint8_t kTapInstruction[SC88_REVERB_TAPS] = {
  131u, 133u, 135u, 137u, 139u, 141u, 143u, 145u};

unsigned scaleAddr(unsigned addr, double scale)
{
  return (unsigned)((double)addr * scale + 0.5);
}

float eramRead(const struct sc88_reverb *rv, unsigned a)
{
  unsigned i = rv->eram_pos + a;
  if (i >= rv->eram_len)
    i -= rv->eram_len;
  return rv->eram[i];
}

void eramWrite(struct sc88_reverb *rv, unsigned a, float v)
{
  unsigned i = rv->eram_pos + a;
  if (i >= rv->eram_len)
    i -= rv->eram_len;
  rv->eram[i] = v;
}

/* One buffer as the program runs it: the far end is already read, and the
 * section is an allpass where the character enables its pair and a plain
 * delay where it does not. */
float section(struct sc88_reverb *rv, unsigned b, float x, float delayed)
{
  if (rv->character.allpass[b]) {
    float v = x + kReverbAllpassG * delayed;
    eramWrite(rv, rv->head[b], v);
    return delayed - kReverbAllpassG * v;
  }
  eramWrite(rv, rv->head[b], x);
  return delayed;
}

}  // namespace

bool reverb_tap_gains(const struct sc88_rom *rom, float gains[SC88_REVERB_TAPS])
{
  if (!rom || !rom->bytes || !gains ||
      kReverbImage0Cram + 2u * 288u > rom->size)
    return false;
  for (unsigned i = 0; i < SC88_REVERB_TAPS; ++i) {
    double g = xp(be16(rom->bytes + kReverbImage0Cram +
      2u * kTapInstruction[i]));
    /* Seven of the eight are exactly +1 and the first is +0.500122. A word
       outside this range is not a gain and the caller is told so rather
       than handed a number chosen here. */
    if (!(g > 0.0 && g <= 2.0))
      return false;
    gains[i] = (float)g;
  }
  return true;
}

bool reverb_read_character(const struct sc88_rom *rom, uint8_t character,
                            struct sc88_reverb_character *out)
{
  if (!rom || !rom->bytes || !out || character >= kReverbCharacters ||
      kReverbPointers + 2u * kReverbCharacters > rom->size)
    return false;
  uint32_t block = kReverbPage +
    be16(rom->bytes + kReverbPointers + 2u * character);
  if (block + 2u * kReverbRecordWords > rom->size)
    return false;
  std::memset(out, 0, sizeof *out);
  /* words 0..15 are the eight coefficient pairs; an enabled one is exactly
     the allpass pair and a disabled one exactly zero, with nothing else
     appearing in those slots on any character (`M-008`). A disabled section
     still has its buffer, and runs as the plain delay that buffer is. */
  for (unsigned i = 0; i < 8; ++i) {
    uint16_t a = be16(rom->bytes + block + 4u * i);
    uint16_t b = be16(rom->bytes + block + 4u * i + 2u);
    if (a == kReverbAllpassPairA && b == kReverbAllpassPairB) {
      out->allpass[kAllpassBuffer[i]] = true;
      ++out->allpasses;
    }
  }
  /* words 16..19 are the two damping pairs, one per tank half: they are the
     coefficients at CRAM (59, 58) and (74, 75), the slots immediately before
     each half's reads. Each pair is a one-pole whose POLE is its positive
     word and whose input coefficient is its negative one, and the two are
     kept here in the record's own order. Room 1 and Plate give their two
     halves different filters; Room 3, Hall 1 and Hall 2 give them the same
     one. A pair that is not two words of those two signs is not a filter,
     and the half then runs undamped rather than on coefficients chosen
     here - which is what Delay and Panning Delay leave behind. */
  for (unsigned i = 0; i < 2; ++i) {
    double a = xp(be16(rom->bytes + block + 32u + 4u * i));
    double b = xp(be16(rom->bytes + block + 34u + 4u * i));
    if (a > 0.0 && a < 0.99 && b < 0.0) {
      out->damp_input[i] = (float)a;
      out->damp_pole[i] = (float)(-b);
    } else {
      out->damp_input[i] = 0.0f;
      out->damp_pole[i] = 1.0f;
    }
  }
  /* words 20..51 are the 32 delay-memory addresses, in the program order of
     the instructions they patch. Twelve are buffer heads, twelve are the
     far-end reads that set the buffer lengths, and eight are the output
     taps. Which is which is the ERAM write-enable bit read off the program
     image (`M-173`), not a guess from the address values - that is the whole
     difference from sorting them and calling every gap a line. */
  uint16_t addr[32];
  for (unsigned i = 0; i < 32; ++i)
    addr[i] = be16(rom->bytes + block + 2u * (20u + i));
  unsigned lo = 0xffffu;
  unsigned hi = 0u;
  for (unsigned i = 0; i < 32; ++i) {
    if (addr[i] < lo)
      lo = addr[i];
    if (addr[i] > hi)
      hi = addr[i];
  }
  for (unsigned i = 0; i < SC88_REVERB_BUFFERS; ++i) {
    unsigned h = addr[kHeadWord[i]] - lo;
    unsigned f = addr[kFarWord[i]] - lo;
    out->head[i] = (uint16_t)h;
    out->far[i] = (uint16_t)(f < h ? h : f);
  }
  for (unsigned i = 0; i < SC88_REVERB_TAPS; ++i)
    out->tap[i] = (uint16_t)(addr[kTapWord[i]] - lo);
  out->extent = (uint16_t)(hi - lo);
  /* word 52, the one value the loader writes to a control register: this
     character's own return trim. See the field's note in the header. */
  out->return_trim = be16(rom->bytes + block + 2u * 52u);
  return hi > lo;
}

bool reverb_pre_lpf(uint8_t p, float *feedback, float *input)
{
  if (!feedback || !input || p > 7)
    return false;
  *feedback = (float)p / 8.0f;
  /* p = 0 is an exact bypass; every other entry is one part in 64 short of
     unity, which is what a filter inside a feedback path needs (`M-010`) */
  *input = p == 0 ? 1.0f : (1.0f - 1.0f / 64.0f - (float)p / 8.0f);
  return true;
}

/* The eight reverb macro presets, one 8-byte record each, of which the
 * firmware copies the first seven bytes over character..predelay. */
bool reverb_macro(const struct sc88_rom *rom, uint8_t macro, uint8_t out[7])
{
  if (!rom || !rom->bytes || !out || macro > 7)
    return false;
  uint32_t base = kReverbMacroTable + (uint32_t)macro * 8u;
  if (base + 7u > rom->size)
    return false;
  for (unsigned i = 0; i < 7; ++i)
    out[i] = rom->bytes[base + i];
  return true;
}

bool reverb_init(struct sc88_reverb *rv, const struct sc88_rom *rom,
                  uint8_t character, double outputRate)
{
  if (!rv || outputRate < 8000.0 || outputRate > 192000.0)
    return false;
  std::memset(rv, 0, sizeof *rv);
  if (!reverb_read_character(rom, character, &rv->character))
    return false;
  rv->character_index = character;
  rv->output_rate = outputRate;
  /* the addresses are in the engine's own 32 kHz samples */
  double scale = outputRate / SC88_REVERB_NATIVE_RATE;
  unsigned top = 0;
  for (unsigned i = 0; i < SC88_REVERB_BUFFERS; ++i) {
    rv->head[i] = scaleAddr(rv->character.head[i], scale);
    rv->far[i] = scaleAddr(rv->character.far[i], scale);
    if (rv->far[i] < rv->head[i])
      rv->far[i] = rv->head[i];
    if (rv->far[i] > top)
      top = rv->far[i];
  }
  for (unsigned i = 0; i < SC88_REVERB_TAPS; ++i) {
    rv->tap[i] = scaleAddr(rv->character.tap[i], scale);
    if (rv->tap[i] > top)
      top = rv->tap[i];
  }
  /* the whole graph lives in one delay memory, the way the chip holds it */
  rv->eram_len = top + 1u;
  rv->eram = (float *)std::calloc(rv->eram_len, sizeof *rv->eram);
  if (!rv->eram)
    return false;
  rv->eram_pos = 0;
  /* The eight tap gains are in the program's coefficient RAM, not in the
     character record: +0.500122 on the first and exactly +1 on the other
     seven. A ROM that does not carry them leaves every tap at unity rather
     than at a set chosen here. */
  if (!reverb_tap_gains(rom, rv->tap_gain))
    for (unsigned i = 0; i < SC88_REVERB_TAPS; ++i)
      rv->tap_gain[i] = 1.0f;
  reverb_set_params(rv, 64, 64, 3);
  rv->active = true;
  return true;
}

void reverb_set_predelay(struct sc88_reverb *rv, uint8_t milliseconds)
{
  if (!rv || milliseconds > 127)
    return;
  size_t want = (size_t)(milliseconds * rv->output_rate / 1000.0);
  if (!rv->pre_delay_buf || want + 2u > rv->pre_delay_len) {
    size_t len = (size_t)(128.0 * rv->output_rate / 1000.0) + 2u;
    float *grown = (float *)std::calloc(len ? len : 1u, sizeof *grown);
    if (!grown)
      return;
    std::free(rv->pre_delay_buf);
    rv->pre_delay_buf = grown;
    rv->pre_delay_len = len;
    rv->pre_delay_pos = 0;
  }
  rv->pre_delay_taps = want;
}

void reverb_destroy(struct sc88_reverb *rv)
{
  if (!rv)
    return;
  std::free(rv->pre_delay_buf);
  std::free(rv->eram);
  std::memset(rv, 0, sizeof *rv);
}

void reverb_reset(struct sc88_reverb *rv)
{
  if (!rv)
    return;
  if (rv->eram)
    std::memset(rv->eram, 0, rv->eram_len * sizeof *rv->eram);
  rv->eram_pos = 0;
  rv->damp_state[0] = rv->damp_state[1] = 0.0f;
  rv->tank_return = 0.0f;
  rv->pre_state = 0.0f;
  if (rv->pre_delay_buf)
    std::memset(rv->pre_delay_buf, 0,
                rv->pre_delay_len * sizeof *rv->pre_delay_buf);
  rv->pre_delay_pos = 0;
}

void reverb_set_params(struct sc88_reverb *rv, uint8_t level, uint8_t time,
                        uint8_t preLpf)
{
  if (!rv)
    return;
  float fb, in;
  if (reverb_pre_lpf(preLpf > 7 ? 7 : preLpf, &fb, &in)) {
    rv->pre_fb = fb;
    rv->pre_in = in;
  }
  /* Level is recovered: the CPU forms 4*p, so the parameter is a linear
     level over 0..127 against a 512 full scale. */
  rv->level = (float)(4u * (unsigned)(level > 127 ? 127 : level)) / 512.0f;
  /* Below character 6, Time IS the decay and the decay is a chip register
     (`P-0359`). The firmware forms `min(380, floor(p * 380 / 108))` and
     writes it to XP 0x337a / 0x336e / 0x337e depending on which program
     layout is loaded - a 9-bit control register in the 0x3364..337e bank,
     not coefficient memory - and that bank's full scale is 512, the same
     scale the Level register runs on. The tank's per-pass loop gain is
     therefore `register / 512`, linear, 0 to 0.7422, with nothing fitted.
     Two separately compiled firmwares for this part agree on the top of
     that range: the JV-1080 reaches the same quantity through a coefficient
     instead, `48 * v / 8192`, and caps it at 0.7441.
     It is applied ONCE PER TANK HALF. That is how the JV holds the same
     quantity - not as a register but as a coefficient, in two slots, one
     inside each half of its own tank - and it is where this program has
     room for it: the only two coefficients in the whole reverb block that
     nothing else explains are the unities at CRAM 68 and 84, one per half.
     So the round trip through both halves carries the register twice, and
     the T60 that falls out is a fact about the character's own line
     lengths and not a number chosen here.
     From character 6 up the firmware does something else entirely with
     Time - it patches delay addresses, and the register carries Feedback,
     which this engine does not plumb - so those characters keep the decay
     measured off the demo songs' own endings (`M-013`). */
  {
    unsigned index = rv->character_index < 10u ? rv->character_index : 4u;
    unsigned p = time > 127 ? 127u : (unsigned)time;
    unsigned samples[2];
    for (unsigned h = 0; h < 2; ++h) {
      samples[h] = 0u;
      for (unsigned i = 0; i < SC88_REVERB_HALF_BUFFERS; ++i) {
        unsigned b = SC88_REVERB_HALF_BUFFERS * (h + 1u) + i;
        samples[h] += rv->far[b] - rv->head[b];
      }
    }
    double seconds = rv->output_rate > 0.0
      ? (double)(samples[0] + samples[1]) / rv->output_rate : 0.0;
    if (index < 6u) {
      unsigned reg = p * 380u / 108u;
      double g = (double)(reg > 380u ? 380u : reg) / 512.0;
      if (g < 0.001)
        g = 0.001;
      for (unsigned h = 0; h < 2; ++h)
        rv->decay[h] = (float)g;
      /* the decay the register and the lines come to, for reporting */
      rv->target_t60 = 3.0 * seconds / (2.0 * std::log10(1.0 / g));
    } else {
      rv->target_t60 = 0.1423 * std::exp(0.0292 * (double)p);
      for (unsigned h = 0; h < 2; ++h) {
        double half = rv->output_rate > 0.0
          ? (double)samples[h] / rv->output_rate : 0.0;
        double g = 0.0;
        if (half > 0.0 && rv->target_t60 > 0.01)
          g = std::pow(10.0, -3.0 * half / rv->target_t60);
        if (g > 0.995)
          g = 0.995;
        else if (g < 0.05)
          g = 0.05;
        rv->decay[h] = (float)g;
      }
    }
  }
  /* How fast the tail darkens: the pole of each half's own one-pole, which
     the ROM puts one per half and not one per line. The pole is the pair's
     POSITIVE word (`P-0360`). Program order cannot say which word is which,
     because the two multiplies land in one accumulator and commute - and
     the character record proves the compiler used that freedom, writing the
     pair to CRAM (59, 58) for half 1 and (74, 75) for half 2, the two
     orders opposed. Four things fix the role on the sign instead. The chip's
     other one-pole in this same block settles the form: the pre-LPF pair is
     `(pole, input)` with both words positive and `p = 0` giving (0, 1), an
     exact bypass, so the recurrence adds `coefficient * y'` with the
     coefficient as stored and a positive word is a lowpass pole. A negative
     word in that place would be a treble-lifting pole, which is not what a
     damping filter in a feedback loop is. Read this way the DC gain
     `input / (1 - pole)` is -0.5417, -0.5536 and -0.5455 over the three
     distinct pairs - a design constant held inside 2 % while the pole moves
     by a factor of four - where the other assignment spreads it over 2.8x.
     And it puts the poles where a reverb designer would: 0.484375 on Room 3,
     Hall 1 and Hall 2, 0.25 and 0.125 on the Rooms' and Plate's first
     halves, so the halls damp hardest and the plate least.
     The DC gain is NOT applied here and the one-pole is normalised to unity
     at DC. It cannot be an uncompensated per-pass loss: with it in the loop
     the decay register's own full scale caps Hall 2 at a 1.65 s T60
     (measured, at Time 127), and the hardware needs more than that - 2.59 s
     on the demo song at Time 100 and about 2 s on the single-note reference.
     Where the chip restores it is in the accumulator and register file,
     which are not recovered. */
  rv->damp[0] = rv->character.damp_input[0];
  rv->damp[1] = rv->character.damp_input[1];
  /* The character's own return trim, the record's 53rd word, read against
     the 512 full scale the reverb's register bank runs on (`M-175`). Room
     1/2/3, Hall 2 and Plate carry 32 and Hall 1 carries 64, so this factor
     is exactly 1 on five of the six reverb characters and exactly 2 on
     Hall 1 - and on Delay and Panning Delay, which carry 64 as well.
     The 16 is the ROM's, not a reference chosen here to leave the other
     characters where they are: the output tap chain ends on CRAM 0xe000 =
     -16.0 at PRAM 153 of the single-module image, and 32/512 * 16 is 1.0
     exactly. What the engine does not have is the accumulator scaling
     between the two, so the absolute level still rests on the tap
     normalisation below; the ratio between characters does not. */
  rv->trim = (float)rv->character.return_trim * 16.0f / 512.0f;
  /* Normalised by the square root of the number of taps summed, which is
     the form `M-018` settled: the taps are mutually decorrelated, so their
     sum grows as the root of the count and not as the count, and the root
     of the total rather than of each side's is what measures right against
     the hardware recording of demo song 1. */
  rv->wet_gain_left = rv->level * rv->trim / std::sqrt((float)SC88_REVERB_TAPS);
  rv->wet_gain_right = rv->wet_gain_left;
}

void reverb_process(struct sc88_reverb *rv, const float *send, float *stereo,
                     size_t frames)
{
  if (!rv || !rv->active || !send || !stereo || !rv->eram)
    return;
  for (size_t k = 0; k < frames; ++k) {
    float x = send[k];
    float wetL = 0.0f, wetR = 0.0f;
    float tail[2], r[SC88_REVERB_BUFFERS];
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
    /* The instructions run in PRAM index order, and every far end is read
       before the writes of its own group, so read them all first. */
    for (unsigned i = 0; i < SC88_REVERB_BUFFERS; ++i)
      r[i] = eramRead(rv, rv->far[i]);
    /* instructions 41..57: the four series allpasses of the input diffuser */
    for (unsigned i = 0; i < 4; ++i)
      x = section(rv, i, x, r[i]);
    /* Instruction 55 reads half 1's last delay before half 1's own writes,
       and instruction 73 reads half 2's last delay before half 2's - the
       two feedback returns. WHICH ACCUMULATOR EACH RETURN REACHES IS NOT
       RECOVERED (`M-173`): the ROM fixes the graph and the order, not the
       routing of the multiplies. This is the figure-of-eight the two-half
       shape is named for - half 1 takes the input and half 2's return,
       half 2 takes half 1's. Half 2's return is read after half 1's writes
       in program order, so it arrives one sample later, which is what a
       pipelined read-before-write does with it. */
    tail[0] = r[7];
    tail[1] = r[11];
    for (unsigned i = 0; i < 2; ++i) {
      float back = i == 0 ? rv->tank_return : tail[0];
      unsigned b = SC88_REVERB_HALF_BUFFERS * (i + 1u);
      back *= rv->decay[i];
      /* the half's own damping one-pole, normalised to unity at DC */
      rv->damp_state[i] = back * (1.0f - rv->damp[i]) +
        rv->damp_state[i] * rv->damp[i];
      back = rv->damp_state[i];
      if (i == 0)
        back += x;
      float y = section(rv, b, back, r[b]);          /* allpass */
      y = section(rv, b + 1u, y, r[b + 1u]);          /* delay */
      y = section(rv, b + 2u, y, r[b + 2u]);          /* allpass */
      (void)section(rv, b + 3u, y, r[b + 3u]);        /* delay */
    }
    rv->tank_return = tail[1];
    /* Instructions 89..103: the eight output taps, read after the writes,
       at the addresses the character carries and with the gains the
       program's own coefficient RAM carries. Four to a side is the only
       split the program order offers - the record interleaves the taps in
       two groups of four, one from each pair of slots - and like the
       returns above it is routing, which is not recovered. */
    for (unsigned i = 0; i < SC88_REVERB_TAPS; ++i) {
      float v = rv->tap_gain[i] * eramRead(rv, rv->tap[i]);
      if (i < SC88_REVERB_TAPS / 2u)
        wetL += v;
      else
        wetR += v;
    }
    stereo[k * 2] += wetL * rv->wet_gain_left;
    stereo[k * 2 + 1] += wetR * rv->wet_gain_right;
    /* the delay memory's base pointer steps back one sample per sample */
    rv->eram_pos = rv->eram_pos ? rv->eram_pos - 1u : rv->eram_len - 1u;
  }
}

}}  // namespace EmuSC::Xp

// Compatibility shims for callers not yet ported to the EmuSC::Xp API.
extern "C" {

bool sc88_reverb_read_character(const struct sc88_rom *rom, uint8_t character,
                                struct sc88_reverb_character *out)
{
  return EmuSC::Xp::reverb_read_character(rom, character, out);
}

bool sc88_reverb_pre_lpf(uint8_t p, float *feedback, float *input)
{
  return EmuSC::Xp::reverb_pre_lpf(p, feedback, input);
}

bool sc88_reverb_tap_gains(const struct sc88_rom *rom,
                           float gains[SC88_REVERB_TAPS])
{
  return EmuSC::Xp::reverb_tap_gains(rom, gains);
}

bool sc88_reverb_macro(const struct sc88_rom *rom, uint8_t macro,
                       uint8_t out[7])
{
  return EmuSC::Xp::reverb_macro(rom, macro, out);
}

bool sc88_reverb_init(struct sc88_reverb *rv, const struct sc88_rom *rom,
                      uint8_t character, double output_rate)
{
  return EmuSC::Xp::reverb_init(rv, rom, character, output_rate);
}

void sc88_reverb_destroy(struct sc88_reverb *rv)
{
  EmuSC::Xp::reverb_destroy(rv);
}

void sc88_reverb_reset(struct sc88_reverb *rv)
{
  EmuSC::Xp::reverb_reset(rv);
}

void sc88_reverb_set_params(struct sc88_reverb *rv, uint8_t level,
                            uint8_t time, uint8_t pre_lpf)
{
  EmuSC::Xp::reverb_set_params(rv, level, time, pre_lpf);
}

void sc88_reverb_set_predelay(struct sc88_reverb *rv, uint8_t milliseconds)
{
  EmuSC::Xp::reverb_set_predelay(rv, milliseconds);
}

void sc88_reverb_process(struct sc88_reverb *rv, const float *send,
                         float *stereo, size_t frames)
{
  EmuSC::Xp::reverb_process(rv, send, stereo, frames);
}

}  // extern "C"
