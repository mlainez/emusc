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
#define IMAGE0_CRAM (0x78b02u + 0x480u)

static void put16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

/* The record's 32 address words in the program order the firmware patches
 * them in: head, far end, and the eight taps interleaved where the tank's
 * delay instructions sit. The table mirrors the one in the source, so a
 * change to either is caught by the lengths asserted below. */
static const unsigned head_word[12] = {0, 2, 4, 6, 8, 10, 14, 16, 20, 22,
                                       26, 28};
static const unsigned far_word[12] = {1, 3, 5, 7, 9, 13, 15, 19, 21, 25,
                                      27, 31};
static const unsigned tap_word[8] = {11, 17, 23, 29, 12, 18, 24, 30};

int main(void)
{
  static const uint8_t vectors[16] = {
    0, 0, 2, 0, 255, 255, 255, 255, 0, 0, 1, 244, 0, 0, 1, 244
  };
  /* twelve buffers, laid out contiguously with a one-address boundary */
  static const unsigned len[12] = {100, 80, 40, 20, 60, 300, 50, 400,
                                   70, 320, 55, 380};
  uint8_t *bytes = (uint8_t *)calloc(SC88_CONTROL_ROM_SIZE, 1);
  struct sc88_rom rom;
  struct sc88_reverb_character ch;
  struct sc88_reverb rv;
  float fb, in, gains[SC88_REVERB_TAPS];
  unsigned i, head[12], at = 0;
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

  /* The tap gains live in the program's coefficient RAM: +0.500122 at the
     first tap instruction and exactly +1 at the other seven. */
  assert(!sc88_reverb_tap_gains(&rom, gains));   /* an empty CRAM is refused */
  put16(bytes + IMAGE0_CRAM + 2 * 131, 0x1001);
  for (i = 1; i < 8; ++i)
    put16(bytes + IMAGE0_CRAM + 2 * (131 + 2 * i), 0x5000);
  assert(sc88_reverb_tap_gains(&rom, gains));
  assert(fabsf(gains[0] - 4097.0f / 8192.0f) < 1e-6f);
  for (i = 1; i < 8; ++i)
    assert(gains[i] == 1.0f);

  /* A character record: the pointer is an offset within the 0x10000 page,
     seven enabled allpass pairs and one disabled, two damping pairs, then
     the 32 addresses in program order. */
  put16(bytes + POINTERS, (uint16_t)(block - PAGE));
  for (i = 0; i < 8; ++i) {
    put16(bytes + block + 4 * i, i == 3 ? 0x0000 : 0x3000);
    put16(bytes + block + 4 * i + 2, i == 3 ? 0x0000 : 0x1000);
  }
  put16(bytes + block + 32, 0x0800);     /* half 1: input 0.25 */
  put16(bytes + block + 34, 0x3300);     /*         pole 0.40625 */
  put16(bytes + block + 36, 0x0f80);     /* half 2: input 0.484375 */
  put16(bytes + block + 38, 0x3700);     /*         pole 0.28125 */
  for (i = 0; i < 12; ++i) {
    head[i] = at;
    put16(bytes + block + 2 * (20 + head_word[i]), (uint16_t)at);
    at += len[i];
    put16(bytes + block + 2 * (20 + far_word[i]), (uint16_t)(at - 1));
  }
  /* two taps inside each of the four tank delays, B5 B7 B9 B11 */
  for (i = 0; i < 8; ++i) {
    unsigned b = 5 + 2 * (i % 4);
    put16(bytes + block + 2 * (20 + tap_word[i]),
          (uint16_t)(head[b] + (i < 4 ? 17 : 29)));
  }

  put16(bytes + block + 2 * 52, 32);     /* the character's return trim */

  assert(sc88_reverb_read_character(&rom, 0, &ch));
  assert(ch.allpasses == 7);
  /* the enabled pairs land on the eight buffers whose write carries +0.5,
     and the fourth diffuser section is the one this record disables */
  assert(ch.allpass[0] && ch.allpass[1] && ch.allpass[2] && !ch.allpass[3]);
  assert(ch.allpass[4] && ch.allpass[6] && ch.allpass[8] && ch.allpass[10]);
  assert(!ch.allpass[5] && !ch.allpass[7] && !ch.allpass[9] &&
         !ch.allpass[11]);
  for (i = 0; i < 12; ++i) {
    assert(ch.head[i] == head[i]);
    assert(ch.far[i] - ch.head[i] == len[i] - 1);
  }
  for (i = 0; i < 8; ++i)
    assert(ch.tap[i] == head[5 + 2 * (i % 4)] + (i < 4 ? 17 : 29));
  /* the two halves keep their own damping filter */
  assert(fabsf(ch.damp_pole[0] - 0.40625f) < 1e-6f);
  assert(fabsf(ch.damp_pole[1] - 0.28125f) < 1e-6f);
  assert(fabsf(ch.damp_input[0] - 0.25f) < 1e-6f);
  assert(fabsf(ch.damp_input[1] - 0.484375f) < 1e-6f);
  /* out of range characters and a ROM too small are refused */
  assert(!sc88_reverb_read_character(&rom, 10, &ch));
  assert(!sc88_reverb_read_character(NULL, 0, &ch));

  /* The addresses are in 32 kHz samples, so a higher output rate stretches
     the whole delay memory. */
  assert(sc88_reverb_init(&rv, &rom, 0, 64000.0));
  assert(rv.active && rv.far[0] - rv.head[0] == 2 * (len[0] - 1));
  sc88_reverb_destroy(&rv);
  assert(sc88_reverb_init(&rv, &rom, 0, 32000.0));
  assert(rv.far[0] - rv.head[0] == len[0] - 1);
  assert(rv.eram_len == ch.extent + 1u);
  assert(fabsf(rv.tap_gain[0] - 4097.0f / 8192.0f) < 1e-6f);

  /* Level is the recovered 4*p against a 512 full scale, and silence in is
     silence out however long it runs. */
  sc88_reverb_set_params(&rv, 128, 64, 3);
  assert(fabsf(rv.level - 127.0f * 4.0f / 512.0f) < 1e-6f);
  /* The character's own return trim, the record's 53rd word against the
     same 512 full scale: 32 is unity, which is what five of the six
     reverb characters carry. */
  assert(ch.return_trim == 32);
  assert(fabsf(rv.trim - 1.0f) < 1e-6f);
  assert(fabsf(rv.wet_gain_left - rv.level / sqrtf((float)SC88_REVERB_TAPS))
         < 1e-6f);
  {
    float send[64], stereo[128];
    unsigned pass;
    double first = 0.0, later = 0.0;
    memset(send, 0, sizeof send);
    memset(stereo, 0, sizeof stereo);
    for (pass = 0; pass < 40; ++pass)
      sc88_reverb_process(&rv, send, stereo, 64);
    for (i = 0; i < 128; ++i)
      assert(stereo[i] == 0.0f);
    /* an impulse comes back out, decaying rather than growing */
    send[0] = 1.0f;
    sc88_reverb_process(&rv, send, stereo, 64);
    send[0] = 0.0f;
    for (pass = 0; pass < 200; ++pass) {
      memset(stereo, 0, sizeof stereo);
      sc88_reverb_process(&rv, send, stereo, 64);
      for (i = 0; i < 128; ++i) {
        assert(fabsf(stereo[i]) < 4.0f);        /* bounded: it cannot run away */
        if (pass < 20)
          first += (double)stereo[i] * stereo[i];
        else if (pass >= 150)
          later += (double)stereo[i] * stereo[i];
      }
    }
    /* and the tail really is a tail */
    assert(first > 0.0 && later < first);
  }
  sc88_reverb_destroy(&rv);
  assert(!rv.active);

  /* The eight taps are inside the tank, so the response carries an early
     field: the first 20 ms of it is not silence, which is what a network
     that only reads the ends of its delay lines gives. */
  {
    static float send[6400], stereo[12800];
    double early = 0.0, total = 0.0;
    assert(sc88_reverb_init(&rv, &rom, 0, 32000.0));
    sc88_reverb_set_params(&rv, 127, 64, 0);
    memset(send, 0, sizeof send);
    memset(stereo, 0, sizeof stereo);
    send[0] = 1.0f;                             /* 200 ms of response */
    sc88_reverb_process(&rv, send, stereo, 6400);
    for (i = 0; i < 12800; ++i) {
      double e = (double)stereo[i] * stereo[i];
      total += e;
      if (i < 2 * 640)                          /* the first 20 ms at 32 kHz */
        early += e;
    }
    assert(total > 0.0 && early > 0.01 * total);
    sc88_reverb_destroy(&rv);
  }

  /* Hall 1's record carries 64 where the other five reverb characters
     carry 32, so its return is exactly twice theirs at the same Level -
     and a record carrying 0, which is what the two transition characters
     hold, returns nothing at all. */
  {
    float unity;
    assert(sc88_reverb_init(&rv, &rom, 0, 32000.0));
    sc88_reverb_set_params(&rv, 100, 64, 0);
    unity = rv.wet_gain_left;
    sc88_reverb_destroy(&rv);
    put16(bytes + block + 2 * 52, 64);
    assert(sc88_reverb_init(&rv, &rom, 0, 32000.0));
    sc88_reverb_set_params(&rv, 100, 64, 0);
    assert(fabsf(rv.trim - 2.0f) < 1e-6f);
    assert(fabsf(rv.wet_gain_left - 2.0f * unity) < 1e-6f);
    sc88_reverb_destroy(&rv);
    put16(bytes + block + 2 * 52, 0);
    assert(sc88_reverb_init(&rv, &rom, 0, 32000.0));
    sc88_reverb_set_params(&rv, 100, 64, 0);
    assert(rv.wet_gain_left == 0.0f && rv.wet_gain_right == 0.0f);
    sc88_reverb_destroy(&rv);
    put16(bytes + block + 2 * 52, 32);
  }

  /* The macro preset table: eight records of eight bytes at 0x1583e, of
     which seven are copied. The stride and the count are what the firmware
     handler's own copy helper fixes, so pin both, and pin that a macro
     above seven is refused rather than read past the table. */
  {
    uint8_t got[7];
    unsigned m, j;
    for (m = 0; m < 8; ++m)
      for (j = 0; j < 8; ++j)
        bytes[0x1583e + m * 8 + j] = (uint8_t)(0x10 * m + j);
    for (m = 0; m < 8; ++m) {
      assert(sc88_reverb_macro(&rom, (uint8_t)m, got));
      for (j = 0; j < 7; ++j)
        assert(got[j] == (uint8_t)(0x10 * m + j));
    }
    assert(!sc88_reverb_macro(&rom, 8, got));
    assert(!sc88_reverb_macro(NULL, 0, got));
  }

  free(bytes);
  return 0;
}
