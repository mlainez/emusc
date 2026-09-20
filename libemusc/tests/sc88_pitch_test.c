/* SPDX-License-Identifier: CC0-1.0 */
#include "engines/xp/pitch.h"

#include <assert.h>
#ifdef NDEBUG
#error "this test is assertion-driven; NDEBUG compiles it away"
#endif
#include <stdlib.h>
#include <string.h>

static void put16(uint8_t *p, uint16_t value)
{
  p[0] = (uint8_t)(value >> 8);
  p[1] = (uint8_t)value;
}

int main(void)
{
  static const uint8_t vectors[16] = {
    0, 0, 2, 0, 255, 255, 255, 255, 0, 0, 1, 244, 0, 0, 1, 244
  };
  uint8_t *bytes = (uint8_t *)calloc(SC88_CONTROL_ROM_SIZE, 1);
  uint8_t component_bytes[SC88_COMPONENT_SIZE] = {0};
  uint8_t common[SC88_TONE_COMMON_SIZE] = {0};
  struct sc88_rom rom;
  struct sc88_tone tone = {common, 0x40000, 1};
  struct sc88_component component = {component_bytes, 0, 0};
  struct sc88_pitch_envelope envelope;
  struct sc88_pitch_release release;
  assert(bytes);
  memcpy(bytes, vectors, sizeof vectors);
  memcpy(bytes + 0x30000, "\0\0Piano 1A    \3\377", 16);
  assert(sc88_rom_init(&rom, bytes, SC88_CONTROL_ROM_SIZE));
  put16(component_bytes + 0x1a, 0x4000);
  put16(component_bytes + 0x1e, 0x1000);
  put16(component_bytes + 0x20, 0x4000);
  put16(component_bytes + 0x22, 0x2000);
  put16(component_bytes + 0x28, 0xc000);
  component_bytes[0x2a] = 1;
  component_bytes[0x2b] = 1;
  component_bytes[0x2c] = 1;
  component_bytes[0x2d] = 1;
  component_bytes[0x2e] = 1;
  put16(component_bytes + 0x30, 0x1000);
  put16(component_bytes + 0x32, 0x1000);
  put16(bytes + 0x1543e + 2, 0x4000);
  put16(bytes + 0x1573e + 64 * 2, 0x0100);
  assert(sc88_pitch_envelope_prepare(&rom, &tone, &component, 60, 64,
                                     &envelope));
  assert(envelope.depth == 0x3fff);
  assert(envelope.current == 0x03ff);
  assert(envelope.targets[0] == 0x0fff);
  assert(envelope.increments[0] == 0x4000);
  assert(sc88_pitch_envelope_advance(&envelope, 1));
  assert(envelope.current == 0x06ff);
  assert(sc88_pitch_release_prepare(&rom, &tone, &component, 60,
                                    envelope.depth, &release));
  assert(release.destination == -0x1000);
  /* `6559` writes the destination into the same word `58ce` later turns
     into the distance left to it, so prepare leaves the two equal and a
     note off that never reaches `58ce` ramps toward the destination. */
  assert(release.delta == release.destination);
  assert(sc88_pitch_release_activate(&rom, 0, false, false, false,
                                     &release));
  /* `58ce..58d6`, which `sc88_engine_start_release` performs for a tone
     whose tone-common `+0x15` is clear. */
  release.delta = (int16_t)((uint16_t)release.destination -
                            (uint16_t)envelope.current);
  envelope.active = false;
  envelope.stage = 4;
  assert(sc88_pitch_release_advance(&release, 1));
  assert(sc88_pitch_envelope_sum(&envelope, &release) < envelope.current);

  /* The word the XP is given, whose three rules are firmware law: the
     envelope's contribution is doubled, the sum is capped at 0x0003ffff, and
     the low bit of the current value is cleared before upload. */
  assert(sc88_pitch_current_word(0x38000, 0, 0) == 0x38000);
  assert(sc88_pitch_current_word(0x38000, 0, 0x100) == 0x38200);
  assert(sc88_pitch_current_word(0x38000, 0x4000, 0) == 0x3c000);
  assert(sc88_pitch_current_word(0x38000, -0x4000, 0) == 0x34000);
  /* the part offset and the doubled envelope add in the same domain */
  assert(sc88_pitch_current_word(0x38000, 0x1000, 0x800) == 0x3a000);
  /* an odd result is rounded down, never up */
  assert(sc88_pitch_current_word(0x38001, 0, 0) == 0x38000);
  assert(sc88_pitch_current_word(0x38003, 0, 0) == 0x38002);
  /* and BOTH ends saturate to the same place - the top.

     `0x5e59` compares the high word of the 32-bit sum against 3 UNSIGNED
     (`4c 00 03` `cmp:i.w #3,r4`, `23 06` `bls.b`) and the not-taken path
     writes r4 = 3, r5 = 0xffff. A sum that has gone below zero has a high
     word of 0xffff, fails that compare like an overlarge one, and is given
     the MAXIMUM. Saturating such a sum to zero instead drops the note an
     octave or more, once, on the one period the sum is negative - a single
     wrong note rather than a steady error, which is what makes it worth a
     test of its own. The same clamp, byte for byte, is at 0x5f11. */
  assert(sc88_pitch_current_word(0x3fffe, 0, 0x4000) == 0x3fffe);
  assert(sc88_pitch_current_word(0x3ffff, 0, 0) == 0x3fffe);
  assert(sc88_pitch_current_word(0x100, -0x4000, 0) == 0x3fffe);
  assert(sc88_pitch_current_word(0, 0, -0x4000) == 0x3fffe);
  /* One unit either side of the top of the unsaturated range, and one unit
     either side of zero, so a clamp written as a signed compare fails here. */
  assert(sc88_pitch_current_word(0x40000, 0, 0) == 0x3fffe);
  assert(sc88_pitch_current_word(1, -1, 0) == 0);
  assert(sc88_pitch_current_word(0, -1, 0) == 0x3fffe);
  /* The combination the board can actually reach: a component sitting low
     in the range, a full downward bend in the part offset and a pitch
     envelope still pulling down. The sum is negative, so the chip is given
     the top of the range. */
  {
    /* base + offset + 2 * envelope_sum = 0x02000 - 0x03000 - 0x02000 */
    uint32_t low_base = 0x02000;
    int32_t full_bend_down = -0x3000;
    int16_t envelope_down = -0x1000;
    assert((int64_t)low_base + full_bend_down + 2 * (int64_t)envelope_down < 0);
    assert(sc88_pitch_current_word(low_base, full_bend_down,
                                   envelope_down) == 0x3fffe);
  }

  /* Portamento. The table's four-byte stride and big-endian halves, the
     zero entry that is never reached, and the glide itself: one semitone
     per period at a rate of 0x10000, direction fixed at the start, and a
     snap to the target rather than an overshoot. */
  bytes[0x78502 + 4 * 1] = 0x00;
  bytes[0x78502 + 4 * 1 + 1] = 0x12;
  bytes[0x78502 + 4 * 1 + 2] = 0x34;
  bytes[0x78502 + 4 * 1 + 3] = 0x56;
  put16(bytes + 0x78502 + 4 * 70, 0x0001);
  put16(bytes + 0x78502 + 4 * 70 + 2, 0x0000);
  assert(sc88_portamento_rate(&rom, 1) == 0x00123456u);
  assert(sc88_portamento_rate(&rom, 70) == 0x00010000u);
  assert(sc88_portamento_rate(&rom, 0) == 0);

  {
    struct sc88_portamento glide;
    memset(&glide, 0, sizeof glide);
    glide.current = 40u << 16;
    glide.target = 76u << 16;
    glide.rate = 0x00010000u;
    glide.ascending = true;
    glide.active = true;
    sc88_portamento_advance(&glide, 10);
    assert(glide.current == 50u << 16);
    assert(glide.active);
    /* the catch-up count is periods, not periods minus one */
    sc88_portamento_advance(&glide, 1);
    assert(glide.current == 51u << 16);
    /* and the far end snaps exactly, whatever the step would have done */
    sc88_portamento_advance(&glide, 100);
    assert(glide.current == glide.target);
    assert(!glide.active);
    /* a further service does nothing at all */
    sc88_portamento_advance(&glide, 10);
    assert(glide.current == glide.target);

    memset(&glide, 0, sizeof glide);
    glide.current = 76u << 16;
    glide.target = 40u << 16;
    glide.rate = 0x00008000u;
    glide.ascending = false;
    glide.active = true;
    sc88_portamento_advance(&glide, 4);
    assert(glide.current == 74u << 16);
    sc88_portamento_advance(&glide, 1000);
    assert(glide.current == glide.target);
    assert(!glide.active);

    /* CC5 = 0 does not glide: the firmware tests the time byte before it
       ever reaches the table, whose entry 0 is 0xffffffff. */
    memset(&glide, 0, sizeof glide);
    glide.current = 40u << 16;
    glide.target = 76u << 16;
    glide.rate = 0;
    glide.ascending = true;
    glide.active = true;
    sc88_portamento_advance(&glide, 1);
    assert(glide.current == glide.target);
    assert(!glide.active);
  }

  free(bytes);
  return 0;
}
