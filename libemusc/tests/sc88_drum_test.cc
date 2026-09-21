/* SPDX-License-Identifier: CC0-1.0 */
#include "engines/xp/rom.h"

#include <cassert>
#ifdef NDEBUG
#error "this test is assertion-driven; NDEBUG compiles it away"
#endif
#include <cstdlib>
#include <cstring>

using namespace EmuSC::Xp;

/* The kit structure is verified against the held control ROM, so this
   fixture reproduces the parts of it the lookup depends on: the map at
   0x2fd00, the twenty-four pointers at 0x2b550, and one kit record whose
   pointer must equal 0x23c30 + index * 0x50c or the lookup refuses it. */
#define MAP 0x2fd00u
#define POINTERS 0x2b550u
#define KIT_BASE 0x23c30u
#define STRIDE 0x50cu

static void put24(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v;
}

int main()
{
  static const uint8_t vectors[16] = {
    0, 0, 2, 0, 255, 255, 255, 255, 0, 0, 1, 244, 0, 0, 1, 244
  };
  uint8_t *bytes = (uint8_t *)calloc(XP_CONTROL_ROM_SIZE, 1);
  struct sc88_rom rom;
  struct sc88_drum_note note;
  uint32_t kit, i;
  assert(bytes);
  memcpy(bytes, vectors, sizeof vectors);
  memcpy(bytes + 0x30000, "\0\0Piano 1A    \3\377", 16);
  assert(rom_init(&rom, bytes, XP_CONTROL_ROM_SIZE));

  memset(bytes + MAP, 0xff, 256);
  for (i = 0; i < 24; ++i)
    put24(bytes + POINTERS + i * 3, KIT_BASE + i * STRIDE);
  /* program 48 selects ORCHESTRA in both maps: kit 7 of the SC-55 set and
     kit 19 of the SC-88's own - the assignment the ROM carries. */
  bytes[MAP + 48] = 7;
  bytes[MAP + 128 + 48] = 19;

  assert(rom_select_drum(&rom, 1, 48, &kit));
  assert(kit == KIT_BASE + 7 * STRIDE);
  assert(rom_select_drum(&rom, 2, 48, &kit));
  assert(kit == KIT_BASE + 19 * STRIDE);
  /* a program with no kit, and the two maps that do not exist */
  assert(!rom_select_drum(&rom, 1, 49, &kit));
  assert(!rom_select_drum(&rom, 0, 48, &kit));
  assert(!rom_select_drum(&rom, 3, 48, &kit));

  kit = KIT_BASE + 19 * STRIDE;
  /* one key with a tone, and its own per-note bytes */
  put24(bytes + kit + 36 * 3, 0x40000);
  bytes[kit + 0x180 + 36] = 24;        /* played at key 24, not key 36 */
  bytes[kit + 0x200 + 36] = 100;
  bytes[kit + 0x280 + 36] = 3;
  bytes[kit + 0x300 + 36] = 20;
  bytes[kit + 0x380 + 36] = 40;
  bytes[kit + 0x400 + 36] = 50;
  bytes[kit + 0x480 + 36] = 1;
  assert(rom_open_drum_note(&rom, kit, 36, &note));
  assert(note.tone_offset == 0x40000);
  /* the key the tone plays at comes from the kit, so a kick is not the
     sample transposed to whatever key triggered it */
  assert(note.play_note == 24);
  assert(note.level == 100 && note.assign_group == 3 && note.pan == 20);
  assert(note.reverb_send == 40 && note.chorus_send == 50);
  assert(note.flags == 1);
  /* a key with no sound in this kit is refused rather than guessed at */
  assert(!rom_open_drum_note(&rom, kit, 37, &note));
  assert(!rom_open_drum_note(&rom, kit, 128, &note));

  free(bytes);
  return 0;
}
