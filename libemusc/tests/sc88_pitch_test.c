/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_pitch.h"

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
  assert(sc88_pitch_release_activate(&rom, 0, false, false, false,
                                     envelope.current, &release));
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
  /* and both ends saturate rather than wrapping */
  assert(sc88_pitch_current_word(0x3fffe, 0, 0x4000) == 0x3fffe);
  assert(sc88_pitch_current_word(0x3ffff, 0, 0) == 0x3fffe);
  assert(sc88_pitch_current_word(0x100, -0x4000, 0) == 0);
  assert(sc88_pitch_current_word(0, 0, -0x4000) == 0);

  free(bytes);
  return 0;
}
