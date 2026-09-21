/* SPDX-License-Identifier: CC0-1.0 */
#include "engines/xp/delay.h"

#include <cassert>
#ifdef NDEBUG
#error "this test is assertion-driven; NDEBUG compiles it away"
#endif
#include <cmath>
#include <cstdlib>
#include <cstring>

using namespace EmuSC::Xp;

/* The recovered laws, against the firmware's own arithmetic. The topology
   is a labelled choice and is not asserted; the numbers are. */
int main()
{
  uint8_t *bytes = (uint8_t *)calloc(XP_CONTROL_ROM_SIZE, 1);
  static const uint8_t vectors[16] = {
    0, 0, 2, 0, 255, 255, 255, 255, 0, 0, 1, 244, 0, 0, 1, 244
  };
  struct sc88_rom rom;
  struct sc88_delay dl;
  uint8_t p[10];
  uint8_t macro[10];
  unsigned i;

  assert(bytes);
  memcpy(bytes, vectors, sizeof vectors);
  memcpy(bytes + 0x30000, "\0\0Piano 1A    \3\377", 16);
  assert(rom_init(&rom, bytes, XP_CONTROL_ROM_SIZE));

  /* the centre-time table is `0x8000 + floor(ms * 32)` over its 115 public
     entries, so 100 ms at index 80 and one second at index 0x73 */
  bytes[0x15fb4 + 80 * 2] = 0x8c;
  bytes[0x15fb4 + 80 * 2 + 1] = 0x80;          /* 100.00 ms */
  bytes[0x165ca + 0x18 * 2] = 0x01;
  bytes[0x165ca + 0x18 * 2 + 1] = 0x00;        /* unity */
  bytes[0x165ca + 0x30 * 2] = 0x02;
  bytes[0x165ca + 0x30 * 2 + 1] = 0x00;        /* twice */

  assert(delay_init(&dl, 32000.0, &SC88_PROFILE));

  p[0] = 0; p[1] = 80; p[2] = 0x18; p[3] = 0x30;
  p[4] = 127; p[5] = 127; p[6] = 127; p[7] = 127; p[8] = 64; p[9] = 0;
  assert(delay_set_params(&rom, &dl, p));
  /* 100 ms at 32 kHz is 3200 samples; the right tap is twice the centre */
  assert(fabs(dl.centre_samples - 3200.0) < 1e-6);
  assert(fabs(dl.left_samples - 3200.0) < 1e-6);
  assert(fabs(dl.right_samples - 6400.0) < 1e-6);
  /* level 127 is 127/128, and the overall level follows the reverb's law */
  assert(fabs(dl.centre_level - 127.0 / 128.0) < 1e-6);
  assert(fabs(dl.overall - 508.0 / 512.0) < 1e-6);
  /* feedback is bipolar about 64 and exactly zero there */
  assert(dl.feedback == 0.0f);
  p[8] = 0;
  assert(delay_set_params(&rom, &dl, p));
  assert(fabs(dl.feedback + 0.96875) < 1e-6);
  p[8] = 127;
  assert(delay_set_params(&rom, &dl, p));
  assert(dl.feedback > 0.95f && dl.feedback < 1.0f);

  /* index zero is outside the public centre-time range and is refused
     rather than treated as a time */
  p[1] = 0;
  assert(!delay_set_params(&rom, &dl, p));
  p[1] = 0x74;
  assert(!delay_set_params(&rom, &dl, p));

  /* a macro is ten bytes copied over pre-LPF through reverb send */
  for (i = 0; i < 10; ++i)
    bytes[0x158be + 16 * 4 + i] = (uint8_t)(i + 1);
  assert(delay_macro(&rom, 4, macro));
  for (i = 0; i < 10; ++i)
    assert(macro[i] == (uint8_t)(i + 1));
  assert(!delay_macro(&rom, 10, macro));

  delay_destroy(&dl);
  assert(!delay_init(&dl, 100.0, &SC88_PROFILE));
  free(bytes);
  return 0;
}
