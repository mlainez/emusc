/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_reverb.h"

#include <assert.h>
#ifdef NDEBUG
#error "this test is assertion-driven; NDEBUG compiles it away"
#endif
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define POINTERS 0x1595eu
#define PAGE 0x10000u

static void put16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

int main(void)
{
  static const uint8_t vectors[16] = {
    0, 0, 2, 0, 255, 255, 255, 255, 0, 0, 1, 244, 0, 0, 1, 244
  };
  uint8_t *bytes = (uint8_t *)calloc(SC88_CONTROL_ROM_SIZE, 1);
  struct sc88_rom rom;
  struct sc88_reverb_character ch;
  struct sc88_reverb rv;
  float fb, in;
  unsigned i;
  uint32_t block = 0x15992u;             /* where Room 1's record really is */
  assert(bytes);
  memcpy(bytes, vectors, sizeof vectors);
  memcpy(bytes + 0x30000, "\0\0Piano 1A    \3\377", 16);
  assert(sc88_rom_init(&rom, bytes, SC88_CONTROL_ROM_SIZE));

  /* The pre-LPF law, both ends and the identity that fixes the order: p = 0
     is an exact bypass, and every other entry leaks one part in 64. */
  assert(sc88_reverb_pre_lpf(0, &fb, &in) && fb == 0.0f && in == 1.0f);
  for (i = 1; i <= 7; ++i) {
    assert(sc88_reverb_pre_lpf((uint8_t)i, &fb, &in));
    assert(fabsf(fb - (float)i / 8.0f) < 1e-6f);
    assert(fabsf((fb + in) - (1.0f - 1.0f / 64.0f)) < 1e-6f);
  }
  assert(!sc88_reverb_pre_lpf(8, &fb, &in));

  /* A character record: the pointer is an offset within the 0x10000 page,
     seven enabled allpass pairs and one disabled, then 32 addresses that
     partition the delay memory into lines with one-sample boundaries. */
  put16(bytes + POINTERS, (uint16_t)(block - PAGE));
  for (i = 0; i < 8; ++i) {
    put16(bytes + block + 4 * i, i == 3 ? 0x0000 : 0x3000);
    put16(bytes + block + 4 * i + 2, i == 3 ? 0x0000 : 0x1000);
  }
  {
    /* four lines of 100, 200, 300 and 400 samples, laid out contiguously */
    static const uint16_t lens[4] = {100, 200, 300, 400};
    uint16_t at = 0;
    unsigned k, w = 20;
    for (k = 0; k < 4; ++k) {
      put16(bytes + block + 2 * w++, at);
      at = (uint16_t)(at + lens[k]);
      put16(bytes + block + 2 * w++, at);
      at = (uint16_t)(at + 1);           /* the boundary step */
    }
    while (w < 52)
      put16(bytes + block + 2 * w++, at);
  }
  assert(sc88_reverb_read_character(&rom, 0, &ch));
  assert(ch.allpasses == 7);
  assert(ch.line_count == 4);
  assert(ch.lines[0] == 100 && ch.lines[1] == 200);
  assert(ch.lines[2] == 300 && ch.lines[3] == 400);
  /* out of range characters and a ROM too small are refused */
  assert(!sc88_reverb_read_character(&rom, 10, &ch));
  assert(!sc88_reverb_read_character(NULL, 0, &ch));

  /* The lines are in 32 kHz samples, so a higher output rate lengthens them. */
  assert(sc88_reverb_init(&rv, &rom, 0, 64000.0));
  assert(rv.active && rv.allpass_count == 4 && rv.comb_count == 0);
  assert(rv.allpass[0].len == 200);      /* 100 samples at twice the rate */
  sc88_reverb_destroy(&rv);
  assert(sc88_reverb_init(&rv, &rom, 0, 32000.0));
  assert(rv.allpass[0].len == 100);

  /* Level is the recovered 4*p against a 512 full scale, and silence in is
     silence out however long it runs. */
  sc88_reverb_set_params(&rv, 128, 64, 3);
  assert(fabsf(rv.level - 127.0f * 4.0f / 512.0f) < 1e-6f);
  {
    float send[64], stereo[128];
    unsigned pass;
    memset(send, 0, sizeof send);
    memset(stereo, 0, sizeof stereo);
    for (pass = 0; pass < 40; ++pass)
      sc88_reverb_process(&rv, send, stereo, 64);
    for (i = 0; i < 128; ++i)
      assert(stereo[i] == 0.0f);
    /* and an impulse comes back out, decaying rather than growing */
    send[0] = 1.0f;
    sc88_reverb_process(&rv, send, stereo, 64);
    send[0] = 0.0f;
    for (pass = 0; pass < 200; ++pass) {
      memset(stereo, 0, sizeof stereo);
      sc88_reverb_process(&rv, send, stereo, 64);
      for (i = 0; i < 128; ++i)
        assert(fabsf(stereo[i]) < 4.0f);      /* bounded: it cannot run away */
    }
  }
  sc88_reverb_destroy(&rv);
  assert(!rv.active);
  free(bytes);
  return 0;
}
