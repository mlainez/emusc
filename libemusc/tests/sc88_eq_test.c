/* SPDX-License-Identifier: CC0-1.0 */
#include "engines/xp/sc88_eq.h"

#include <assert.h>
#ifdef NDEBUG
#error "this test is assertion-driven; NDEBUG compiles it away"
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The properties `08_effects/eq.md` establishes for every one of the 100
   records, checked against the held ROM rather than against a fixture: at
   the centre gain the recurrence is an identity, a low-band row's DC gain
   is its labelled gain while its Nyquist gain is unity, and a high-band
   row is the other way round.

   The ROM's path is read from the environment at run time, so ctest's own
   environment carries it whatever the tree was configured with:
     SC88_CONTROL_ROM  the control ROM
   Without it the test reports skipped rather than passing while checking
   nothing. */
static double dc_gain(const struct sc88_eq_band *b)
{
  return (b->c0 + b->c1) / (1.0 - b->c2);
}

static double nyquist_gain(const struct sc88_eq_band *b)
{
  return (b->c0 - b->c1) / (1.0 + b->c2);
}

int main(void)
{
  const char *romPath;
  uint8_t *bytes;
  size_t size;
  struct sc88_rom rom;
  struct sc88_eq eq;
  FILE *file;
  unsigned gain;
  romPath = getenv("SC88_CONTROL_ROM");
  if (!romPath)
    return 77;                          /* no ROM given: skip */
  file = fopen(romPath, "rb");
  if (!file)
    return 77;
  bytes = (uint8_t *)malloc(SC88_CONTROL_ROM_SIZE);
  assert(bytes);
  size = fread(bytes, 1, SC88_CONTROL_ROM_SIZE, file);
  fclose(file);
  if (size != SC88_CONTROL_ROM_SIZE || !sc88_rom_init(&rom, bytes, size)) {
    free(bytes);
    return 77;
  }
  memset(&eq, 0, sizeof eq);

  /* the centre is an exact identity, which is why a reset is inaudible */
  assert(sc88_eq_set_params(&rom, &eq, 0, 0x40, 0, 0x40));
  assert(fabs(eq.low.c0 - 1.0) < 1e-6 &&
         fabs(eq.low.c1 + eq.low.c2) < 1e-6);
  assert(fabs(eq.high.c0 - 1.0) < 1e-6 &&
         fabs(eq.high.c1 + eq.high.c2) < 1e-6);

  /* every gain: the low band shelves at DC and stays flat at Nyquist */
  for (gain = 0x34; gain <= 0x4c; ++gain) {
    double want = (double)gain - 0x40;   /* one decibel per step */
    assert(sc88_eq_set_params(&rom, &eq, 0, (uint8_t)gain, 0, 0x40));
    assert(fabs(20.0 * log10(dc_gain(&eq.low)) - want) < 0.05);
    assert(fabs(20.0 * log10(nyquist_gain(&eq.low))) < 0.01);
    assert(sc88_eq_set_params(&rom, &eq, 1, (uint8_t)gain, 0, 0x40));
    assert(fabs(20.0 * log10(dc_gain(&eq.low)) - want) < 0.05);
    /* and the high band the other way round */
    assert(sc88_eq_set_params(&rom, &eq, 0, 0x40, 0, (uint8_t)gain));
    assert(fabs(20.0 * log10(nyquist_gain(&eq.high)) - want) < 0.05);
    assert(fabs(20.0 * log10(dc_gain(&eq.high))) < 0.01);
    assert(sc88_eq_set_params(&rom, &eq, 0, 0x40, 1, (uint8_t)gain));
    assert(fabs(20.0 * log10(nyquist_gain(&eq.high)) - want) < 0.05);
  }

  /* outside the wire range nothing is invented */
  assert(!sc88_eq_set_params(&rom, &eq, 0, 0x33, 0, 0x40));
  assert(!sc88_eq_set_params(&rom, &eq, 0, 0x4d, 0, 0x40));
  assert(!sc88_eq_set_params(&rom, &eq, 2, 0x40, 0, 0x40));

  /* disabled, it does not touch the buffer */
  {
    float buf[4] = {0.5f, -0.5f, 0.25f, -0.25f};
    eq.enabled = false;
    sc88_eq_process(&eq, buf, 2);
    assert(buf[0] == 0.5f && buf[3] == -0.25f);
  }
  free(bytes);
  return 0;
}
