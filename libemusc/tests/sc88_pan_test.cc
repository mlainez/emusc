/* SPDX-License-Identifier: CC0-1.0 */
#include "engines/xp/pan.h"

#include <cassert>
#ifdef NDEBUG
#error "this test is assertion-driven; NDEBUG compiles it away"
#endif
#include <cstdlib>
#include <cstring>

using namespace EmuSC::Xp;

#define PAN_TABLE 0x15db6u
#define SEND_TABLE 0x15eb6u

static void put16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

int main()
{
  uint8_t *bytes = (uint8_t *)calloc(SC88_CONTROL_ROM_SIZE, 1);
  struct sc88_rom rom;
  uint16_t left, right, gain;
  uint8_t position;
  assert(bytes);
  rom.bytes = bytes;
  rom.size = SC88_CONTROL_ROM_SIZE;

  /* sc88_pan_pair_q15: position 1 reads left from the table's LAST entry
     and right from its FIRST - the pan curve runs in opposite directions
     for the two channels - and position 127 the other way round. */
  put16(bytes + PAN_TABLE + 0 * 2, 0x8000);      /* index 0: unity */
  put16(bytes + PAN_TABLE + 126 * 2, 0x0000);    /* index 126: silence */
  assert(pan_pair_q15(&rom, 1, &left, &right));
  assert(left == 0x0000 && right == 0x8000);
  assert(pan_pair_q15(&rom, 127, &left, &right));
  assert(left == 0x8000 && right == 0x0000);
  /* out of range on both sides */
  assert(!pan_pair_q15(&rom, 0, &left, &right));
  assert(!pan_pair_q15(&rom, 128, &left, &right));
  /* the low six bits are a destination selector this engine doesn't plumb;
     a dirty one is reported false, and (unlike sc88_control_gain_q15) the
     word is returned raw, unmasked */
  put16(bytes + PAN_TABLE + 63 * 2, 0x4001);
  assert(!pan_pair_q15(&rom, 64, &left, &right));
  assert(left == 0x4001 && right == 0x4001);

  /* sc88_control_gain_q15: control 0 is silence, 127 unity, and the send
     table is indexed by the control value whole, unlike the pan table. */
  put16(bytes + SEND_TABLE + 0 * 2, 0x0000);
  put16(bytes + SEND_TABLE + 127 * 2, 0x8000);
  assert(control_gain_q15(&rom, 0, &gain) && gain == 0x0000);
  assert(control_gain_q15(&rom, 127, &gain) && gain == 0x8000);
  put16(bytes + SEND_TABLE + 5 * 2, 0x1234);
  assert(!control_gain_q15(&rom, 5, &gain));
  assert(gain == (0x1234 & 0xffc0));

  /* sc88_send_combine: the rounded product maps 0 to 0 and 127x127 to 127,
     and a melodic voice's note=127 keeps its part's control exactly. */
  assert(send_combine(0, 0) == 0);
  assert(send_combine(127, 127) == 127);
  assert(send_combine(64, 127) == 64);
  assert(send_combine(1, 127) == 1);

  /* sc88_pan_component_offset: the key-table pointer is a 24-bit address
     split across common+0x0e (low 16) and common+0x21 (high byte); the
     offset is the component's own byte plus the ROM's per-key byte. */
  {
    struct sc88_tone tone;
    struct sc88_component component;
    int16_t offset;
    tone.common = bytes + 0x40000;
    tone.offset = 0x40000;
    tone.component_count = 1;
    component.bytes = bytes + 0x40100;
    component.offset = 0x40100;
    component.directory_offset = 0;

    bytes[0x40000 + 0x21] = 0x04;              /* key-table high byte */
    put16(bytes + 0x40000 + 0x0e, 0x0200);     /* key-table low 16 bits */
    /* key_table == 0x40200 */
    bytes[0x40100 + 0x04] = 10;                /* component's own offset */
    bytes[0x40200 + 5] = 0xfd;                 /* per-key byte, s8 -3 */
    assert(pan_component_offset(&rom, &tone, &component, 5, &offset));
    assert(offset == 7);
    assert(!pan_component_offset(&rom, &tone, &component, 200, &offset));

    /* sc88_pan_static_q15: composition clamps to 1..127, and a part pan of
       zero takes the caller's drawn random position wholesale instead. */
    put16(bytes + PAN_TABLE + (127 - 67) * 2, 0x1000);
    put16(bytes + PAN_TABLE + (67 - 1) * 2, 0x2000);
    {
      struct sc88_pan_controls controls;
      controls.master = 64;
      controls.part = 60;
      controls.random_position = 0;
      assert(pan_static_q15(&rom, &tone, &component, 5, &controls,
                            &position, &left, &right));
      assert(position == 67 && left == 0x1000 && right == 0x2000);
    }
    {
      struct sc88_pan_controls controls;
      controls.master = 1;
      controls.part = 1;
      controls.random_position = 0;
      assert(pan_static_q15(&rom, &tone, &component, 5, &controls,
                            &position, &left, &right));
      assert(position == 1);       /* composed to -55, clamped up to 1 */
    }
    {
      struct sc88_pan_controls controls;
      controls.master = 127;
      controls.part = 127;
      controls.random_position = 0;
      assert(pan_static_q15(&rom, &tone, &component, 5, &controls,
                            &position, &left, &right));
      assert(position == 127);     /* composed to 197, clamped down to 127 */
    }
    /* Clean the deliberately dirty entry at index 63 from the masking
       check above: position 64 (a zero random draw) reads it on both
       sides. */
    put16(bytes + PAN_TABLE + 63 * 2, 0x0000);
    {
      struct sc88_pan_controls controls;
      controls.master = 64;
      controls.part = 0;
      controls.random_position = 0;
      assert(pan_static_q15(&rom, &tone, &component, 5, &controls,
                            &position, &left, &right));
      assert(position == 64);      /* a zero draw floors to centre */
      controls.random_position = 200;
      assert(pan_static_q15(&rom, &tone, &component, 5, &controls,
                            &position, &left, &right));
      assert(position == 127);
      controls.random_position = 50;
      assert(pan_static_q15(&rom, &tone, &component, 5, &controls,
                            &position, &left, &right));
      assert(position == 50);
    }
  }

  free(bytes);
  return 0;
}
