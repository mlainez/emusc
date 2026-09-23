/* SPDX-License-Identifier: CC0-1.0 */
#include "drive.h"

#include "common/constants.h"
#include "devices/profile.h"
#include "efx.h"

#include <cmath>
#include <cstring>

namespace EmuSC { namespace Xp {

namespace {

/* The profile's tables this effect reads (devices/jv1080.cc). */
const unsigned kLevelTable = 0u;     /* 0x03856C */
const unsigned kDriveTable = 2u;     /* 0x038832 */
const unsigned kPanTable = 13u;      /* 0x0392A0, (L, R) */
const unsigned kRowTable = 19u;      /* 0x0384C6, 2 rows x 21 */
const unsigned kAmpTable = 20u;      /* 0x03EF30, 4 types x 20 */
const unsigned kLowShelfTable = 21u; /* 0x039802, 31 x 3 */
const unsigned kHighShelfTable = 22u;/* 0x0398BC, 31 x 3 */

/* Slot 29's program image: the two gain words the row does not overwrite.
   Read from the image rather than written down, and 0xD000 (+8.0) each on
   this ROM. */
const unsigned kImageGainA = 28u;
const unsigned kImageGainB = 30u;
const unsigned kDriveSlotType = 1u;  /* OVERDRIVE; DISTORTION loads the same slot */

/* CRAM[0x5F] as `0x0A002704` writes it: 0x5000 for AmpType 0, else 0x9000. */
const uint16_t kAmpTypeZeroGain = 0x5000u;
const uint16_t kAmpTypeOtherGain = 0x9000u;

/* The 9-bit level registers take a table word shifted right four, so their
   full scale is 512 against the table's 8192. */
const double kRegisterUnity = 512.0;

/* THE NONLINEARITY: SATURATION AT +-1.0, THE DSP'S FULL SCALE.

   WHY THIS AND NOT A FITTED CURVE. The shape is not read off an opcode -
   what each instruction computes is silicon (`U-R5-02`) - so it rests on the
   program's own gain staging, which is ROM data: the value row's CRAM[26]
   (+4 OVERDRIVE, -16 DISTORTION) and the slot image's CRAM[28] and [30]
   (+8 each) put x256 and x1024 in front of the one nonlinear stage, and the
   row's CRAM[32] takes x1/32 off after it. Gains that size in a fixed-point
   program only make a drive if the arithmetic saturates at its full scale;
   without saturation the pair would be a plain x8 or x32 and the effect
   linear, which the machine is not. So the stage is a clamp at +-1.0, with
   no free parameter: its level is full scale, every gain around it is a ROM
   word, and nothing below was adjusted to meet the numbers that follow.

   HOW IT IS CHECKED, AND THE MEASURED REFERENCE IT IS CHECKED AGAINST.
   `M-089` (`11_validation/calibration/efx_transfer.json`,
   `efx02_overdrive`): the `Sine` element at key 60, flat voice, OVERDRIVE
   at its factory setting (Drive 127, AmpType 2, gains 15/15, Level 60),
   `tone_level` stepped so the input sits at -28.94, -16.90, -9.86, -4.86
   and 0 dB, reads a fundamental of -12.10, -1.05, -0.16, -0.04 and 0 dB
   against the top step. Rendered through this chain on engine bd7b467
   (`results/tmp/jv1080-drive`):

     OVERDRIVE   fundamental  -11.70  -0.95  -0.15  -0.03  0
                 THD          -35.0  -18.4   -9.5   -7.1  -6.0
                 (machine     -35.3  -20.1  -10.1   -7.7  -6.5)
     DISTORTION  fundamental  -0.97  -0.05 (machine -1.09 -0.05)
                 THD          -13.3   -1.3 (machine -15.1  -1.9)

   On both rows every harmonic of 2 to 12 stronger than -30 dB - the odd
   series that carries the sound - is within 1 to 2 dB of the take. The
   weak ones do not follow: harmonics under -35 dB scatter by up to 20 dB,
   and the even ones sit near -55 dB where the take has -60 to -70.

   The top step's absolute level lands within 0.0 dB (OVERDRIVE) and
   0.1 dB (DISTORTION) of the take's, through a dry-path calibration on
   `basic/single_note_dry`. That is a prediction from the ROM gains, since
   the clamp has no level of its own to set, and it is also what supports
   putting full scale at 1.0 in this engine's sample units: the clamp only
   bends where the machine's does if this engine's voice level stands to
   full scale as the machine's does. DISTORTION is a prediction throughout:
   nothing of its row entered the chain but its ROM words.

   The five measured points drawn straight as the curve itself read
   +2.2 dB loud at saturation and 11 dB short of THD at the -16.90 dB step,
   because five points cannot place the knee, which is why the measured
   curve is the reference here and not the code. */
const float kSaturation = 1.0f;

/* The XP coefficient law: a sign-extended 14-bit mantissa, thirteen bits
   fractional, scaled by the two-bit exponent bits 15:14 carry
   (kXpCoefficientShift, common/constants.h). On this device it is
   consistent with the ROM's own HF-damp BYPASS row and every sum-to-unity
   section (`08_effects/coefficient_tables.md`); the exponent field for a
   coefficient word is corroborated inside the ROM but not measured
   (`U-R5-01`), and the gains this effect reads - 0xC800, 0xD000, 0xE000,
   0x9000 - all rest on it. The law itself is credited to
   github.com/giulioz/roland-dsps's prose (TAINT-REGISTER T-008). */
double xp(uint16_t raw)
{
  int value = raw & 0x3fff;
  if (value & 0x2000)
    value -= 0x4000;
  return (double)value * (double)(1u << kXpCoefficientShift[raw >> 14]) / 8192.0;
}

bool word(const struct xp_rom *rom, unsigned table, unsigned index,
          unsigned column, double *out)
{
  uint16_t raw = 0;
  if (!efx_table_value(rom, table, index, column, &raw))
    return false;
  *out = xp(raw);
  return true;
}

bool reg9(const struct xp_rom *rom, unsigned table, unsigned index,
          unsigned column, float *out)
{
  uint16_t raw = 0;
  if (!efx_table_value(rom, table, index, column, &raw))
    return false;
  *out = (float)((double)(raw >> 4) / kRegisterUnity);
  return true;
}

void set_fo(struct xp_drive_fo *s, const double c[3])
{
  s->b0 = (float)c[0];
  s->b1 = (float)c[1];
  s->a1 = (float)c[2];
  s->x1 = s->y1 = 0.0f;
}

void set_bq(struct xp_drive_bq *s, const double c[5])
{
  s->b0 = (float)c[0];
  s->b1 = (float)c[1];
  s->b2 = (float)c[2];
  s->a1 = (float)c[3];
  s->a2 = (float)c[4];
  s->x1 = s->x2 = s->y1 = s->y2 = 0.0f;
}

inline float fo(struct xp_drive_fo *s, float x)
{
  float y = s->b0 * x + s->b1 * s->x1 + s->a1 * s->y1;
  s->x1 = x;
  s->y1 = y;
  return y;
}

inline float bq(struct xp_drive_bq *s, float x)
{
  float y = s->b0 * x + s->b1 * s->x1 + s->b2 * s->x2 +
    s->a1 * s->y1 + s->a2 * s->y2;
  s->x2 = s->x1;
  s->x1 = x;
  s->y2 = s->y1;
  s->y1 = y;
  return y;
}

}  // namespace

bool drive_parameter_valid(const struct xp_rom *rom, unsigned index,
                            uint8_t value)
{
  static const unsigned kTable[6] = {
    kDriveTable, kPanTable, kAmpTable, kLowShelfTable, kHighShelfTable,
    kLevelTable
  };
  unsigned count = 0;
  if (index >= 6u || !efx_table_shape(rom, kTable[index], &count, NULL))
    return false;
  return value < count && value <= 127u;
}

float drive_curve(const struct xp_drive *dr, float u)
{
  (void)dr;
  if (u > kSaturation)
    return kSaturation;
  if (u < -kSaturation)
    return -kSaturation;
  return u;
}

bool drive_set(const struct xp_rom *rom, struct xp_drive *out, unsigned row,
                const uint8_t p[6])
{
  if (!rom || !rom->bytes || !out)
    return false;
  struct xp_drive built;
  struct xp_drive *dr = &built;
  std::memset(dr, 0, sizeof *dr);
  double r[21];
  for (unsigned j = 0; j < 21u; ++j)
    if (!word(rom, kRowTable, row, j, &r[j]))
      return false;
  double a[20];
  for (unsigned j = 0; j < 20u; ++j)
    if (!word(rom, kAmpTable, p[2], j, &a[j]))
      return false;
  double lo[3], hi[3];
  for (unsigned k = 0; k < 3u; ++k)
    if (!word(rom, kLowShelfTable, p[3], k, &lo[k]) ||
        !word(rom, kHighShelfTable, p[4], k, &hi[k]))
      return false;
  float level = 0.0f;
  if (!reg9(rom, kDriveTable, p[0], 0, &dr->drive) ||
      !reg9(rom, kPanTable, p[1], 0, &dr->pan_left) ||
      !reg9(rom, kPanTable, p[1], 1, &dr->pan_right) ||
      !reg9(rom, kLevelTable, p[5], 0, &level))
    return false;
  struct xp_efx_program prog;
  if (!efx_program_load(rom, kDriveSlotType, &prog))
    return false;

  for (unsigned s = 0; s < 3u; ++s) {
    set_fo(&dr->pre[s], r + 3u * s);
    set_fo(&dr->post[s], r + 9u + 3u * s);
  }
  dr->gain = (float)(r[18] * xp(prog.cram[kImageGainA]) *
                     xp(prog.cram[kImageGainB]));
  dr->trim = (float)r[19];
  set_fo(&dr->amp_first, a);
  set_bq(&dr->amp_high, a + 3);
  set_bq(&dr->amp_peak, a + 8);
  set_bq(&dr->amp_low, a + 13);
  {
    const double out[3] = { a[18], 0.0, a[19] };
    set_fo(&dr->amp_out, out);
  }
  set_fo(&dr->low_shelf, lo);
  set_fo(&dr->high_shelf, hi);
  dr->out_gain = (float)(r[20] *
    xp(p[2] == 0u ? kAmpTypeZeroGain : kAmpTypeOtherGain) * level);
  dr->row = row;
  std::memcpy(dr->param, p, sizeof dr->param);
  dr->ready = true;
  *out = built;
  return true;
}

void drive_process(struct xp_drive *dr, const float *inL, const float *inR,
                    float *outL, float *outR, size_t frames)
{
  for (size_t k = 0; k < frames; ++k) {
    /* One pan pair for the output, so the effect runs mono. */
    float x = 0.5f * (inL[k] + inR[k]) * dr->drive;
    for (unsigned s = 0; s < 3u; ++s)
      x = fo(&dr->pre[s], x);
    float w = drive_curve(dr, x * dr->gain) * dr->trim;
    for (unsigned s = 0; s < 3u; ++s)
      w = fo(&dr->post[s], w);
    w = fo(&dr->amp_first, w);
    w = bq(&dr->amp_high, w);
    w = bq(&dr->amp_peak, w);
    w = bq(&dr->amp_low, w);
    w = fo(&dr->amp_out, w);
    w = fo(&dr->low_shelf, w);
    w = fo(&dr->high_shelf, w);
    w *= dr->out_gain;
    outL[k] = dr->pan_left * w;
    outR[k] = dr->pan_right * w;
  }
}

}}  // namespace EmuSC::Xp
