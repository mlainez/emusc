/* SPDX-License-Identifier: CC0-1.0 */
#include "engines/xp/tva.h"

#include <cassert>
#ifdef NDEBUG
#error "this test is assertion-driven; NDEBUG compiles it away"
#endif
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace EmuSC::Xp;

/* The layout is checked without a ROM; the sweep over the curve and rate
   tables needs one, and its path is read from the environment at run time,
   so ctest's own environment carries it whatever the tree was configured
   with:
     SC88_CONTROL_ROM  the control ROM
   Without it the test reports skipped rather than passing while checking
   only the layout. */

#define RATE_TABLE 0x1543eu
#define LINEAR_TABLE 0x1553eu
#define EXPONENTIAL_TABLE 0x1563eu

static uint16_t be16(const uint8_t *at)
{
  return (uint16_t)((uint16_t)at[0] << 8 | at[1]);
}

/* The decode `7892..7903` performs, stated in one line so the ROM sweep
   below can be run against a wrong one and be seen to fail. */
static double decode(uint16_t word, const unsigned *shift)
{
  return (double)(word & 0x0fff) /
    (double)(1u << shift[(word >> 12) & 3]);
}

/* A stage's dwell: `770e..7738` adds the increment to a sixteen-bit phase
   once per control period and ends the stage on the carry. */
static double dwell(uint16_t increment)
{
  return 65536.0 / (double)increment;
}

int main()
{
  static const unsigned firmware_shift[4] = {0, 3, 5, 7};
  static const unsigned control_shift[3][4] = {
    {0, 2, 4, 6}, {0, 3, 6, 9}, {0, 4, 8, 12}
  };
  const char *romPath;
  struct sc88_tva_curve curve;
  uint8_t *bytes;
  size_t size;
  FILE *file;
  unsigned i;
  unsigned c;
  unsigned linear_ok;
  unsigned exponential_ok;
  unsigned control_ok[3];

  /* The layout, without a ROM. Bit 14 is the shape alone; bits 13..12 are
     the exponent; `rate` is the decoded value over 64. */
  tva_curve_decode(0x4517, &curve);
  assert(curve.linear);
  assert(fabs(curve.rate - 1303.0 / 64.0) < 1e-9);
  tva_curve_decode(0x0517, &curve);
  assert(!curve.linear);
  assert(fabs(curve.rate - 1303.0 / 64.0) < 1e-9);
  tva_curve_decode(0x1022, &curve);
  assert(fabs(curve.rate - (34.0 / 8.0) / 64.0) < 1e-9);
  tva_curve_decode(0x2022, &curve);
  assert(fabs(curve.rate - (34.0 / 32.0) / 64.0) < 1e-9);
  tva_curve_decode(0x3022, &curve);
  assert(fabs(curve.rate - (34.0 / 128.0) / 64.0) < 1e-9);

  /* A linear stage arrives when `q` reaches 1, an exponential one is
     10.95 time constants long and so has arrived by then. */
  tva_curve_decode(0x4040, &curve);        /* linear, value 64 */
  assert(fabs(tva_curve_progress(&curve, 0.5) - 0.5) < 1e-9);
  assert(tva_curve_progress(&curve, 1.0) == 1.0);
  assert(tva_curve_progress(&curve, 2.0) == 1.0);
  tva_curve_decode(0x0040, &curve);        /* exponential, value 64 */
  assert(fabs(tva_curve_progress(&curve, 1.0) -
              (1.0 - exp(-1.0))) < 1e-9);
  assert(tva_curve_progress(&curve, 0.0) == 0.0);
  /* `0x0fff` closes the gap within one period either way. */
  tva_curve_decode(0x0fff, &curve);
  assert(tva_curve_progress(&curve, 1.0) == 1.0);
  tva_curve_decode(0x4fff, &curve);
  assert(tva_curve_progress(&curve, 1.0) == 1.0);

  romPath = getenv("SC88_CONTROL_ROM");
  if (!romPath)
    return 77;                          /* no ROM given: skip the sweep */
  file = fopen(romPath, "rb");
  if (!file)
    return 77;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return 77;
  }
  size = (size_t)ftell(file);
  rewind(file);
  bytes = (uint8_t *)malloc(size);
  assert(bytes);
  assert(fread(bytes, 1, size, file) == size);
  fclose(file);
  assert(size > EXPONENTIAL_TABLE + 128u * 2);

  /* The identity, against the SC-88's own tables. For every rate index the
     two curve tables and the rate table describe the SAME segment: with
     `q = periods * value / 64`, a linear stage reaches `q = 1` and an
     exponential one `q = 5606/512 = 10.95` exactly as the phase counter
     carries. The linear side is stated on the mantissa, where the stored
     word must be within one step of `64 * 2^s / periods`; the exponential
     side on `q`, which is 10.95 time constants and so lands between 10
     and 12 whatever the mantissa rounds to. */
  linear_ok = 0;
  exponential_ok = 0;
  control_ok[0] = control_ok[1] = control_ok[2] = 0;
  for (i = 4; i < 128; ++i) {
    uint16_t rate = be16(bytes + RATE_TABLE + i * 2);
    uint16_t word = be16(bytes + LINEAR_TABLE + i * 2);
    double periods = dwell(rate < 16 ? 0xffff : rate);
    double q_exponential =
      periods *
      decode(be16(bytes + EXPONENTIAL_TABLE + i * 2), firmware_shift)
      / 64.0;
    if (fabs((double)(word & 0x0fff) -
             64.0 * (1u << firmware_shift[(word >> 12) & 3]) / periods)
        <= 1.0)
      ++linear_ok;
    if (q_exponential > 10.0 && q_exponential < 12.0)
      ++exponential_ok;
    for (c = 0; c < 3; ++c)
      if (fabs((double)(word & 0x0fff) -
               64.0 * (1u << control_shift[c][(word >> 12) & 3]) / periods)
          <= 1.0)
        ++control_ok[c];
  }
  printf("linear %u of 124, exponential %u of 124; controls %u %u %u\n",
         linear_ok, exponential_ok, control_ok[0], control_ok[1],
         control_ok[2]);
  assert(linear_ok == 124);
  assert(exponential_ok == 124);
  /* The same sweep with the wrong exponent shifts fails, which is what
     makes the result above worth having. */
  for (c = 0; c < 3; ++c)
    assert(control_ok[c] < 40);

  free(bytes);
  return 0;
}
