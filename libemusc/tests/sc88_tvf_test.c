/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_tvf.h"

#include <assert.h>
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
    0x00, 0x00, 0x02, 0x00, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x01, 0xf4, 0x00, 0x00, 0x01, 0xf4
  };
  static const uint8_t first_directory[16] = {
    0x00, 0x00, 'P', 'i', 'a', 'n', 'o', ' ',
    '1', 'A', ' ', ' ', ' ', ' ', 0x03, 0xff
  };
  uint8_t *bytes = (uint8_t *)calloc(SC88_CONTROL_ROM_SIZE, 1);
  uint8_t component_bytes[SC88_COMPONENT_SIZE] = {0};
  struct sc88_rom rom;
  struct sc88_component component = {component_bytes, 0, 0};
  uint8_t tone_common[SC88_TONE_COMMON_SIZE] = {0};
  struct sc88_tone tone = {tone_common, 0x40000, 1};
  const struct sc88_tvf_controls neutral = {64, 64, 64, 64};
  struct sc88_tvf_registers registers;

  assert(bytes);
  memcpy(bytes, vectors, sizeof vectors);
  memcpy(bytes + 0x30000, first_directory, sizeof first_directory);
  assert(sc88_rom_init(&rom, bytes, SC88_CONTROL_ROM_SIZE));

  component_bytes[0x3c] = 60;
  component_bytes[0x3d] = 12;
  component_bytes[0x3e] = 4;
  put16(bytes + 0x78702 + 60 * 2, 0x6000);
  put16(bytes + 0x78802 + 12 * 2, 0x5000);
  assert(sc88_tvf_prepare_registers(&rom, &component, -0x1000,
                                    &neutral, &registers));
  assert(registers.cutoff_index == 60);
  assert(registers.resonance_index == 12);
  assert(registers.combined == 0x2800);
  assert(registers.frequency_current == 0x14000);
  assert(registers.frequency_target == 0x14000);
  assert(registers.frequency_interpolation == 0x4100);
  assert(registers.resonance_current == 12u << 11);
  assert(registers.resonance_target == 12u << 13);
  assert(registers.resonance_interpolation == 0x095f);
  assert(registers.filter_select == 0x0400);
  assert(!registers.fixed_tuple);

  component_bytes[0x3e] = 0xff;
  assert(sc88_tvf_prepare_registers(&rom, &component, 0, &neutral,
                                    &registers));
  assert(registers.frequency_current == 0);
  assert(registers.frequency_target == 0);
  assert(registers.resonance_current == 0x20000);
  assert(registers.resonance_target == 0x80000);
  assert(registers.filter_select == 0x0800);
  assert(registers.fixed_tuple);

  component_bytes[0x3e] = 0;
  put16(component_bytes + 0x3a, 0x1000);
  put16(component_bytes + 0x48, 0x4000);
  put16(component_bytes + 0x4a, 0x4000);
  put16(component_bytes + 0x4c, 0x2000);
  put16(component_bytes + 0x4e, 0xe000);
  put16(component_bytes + 0x50, 0);
  component_bytes[0x54] = 0;
  component_bytes[0x55] = 1;
  component_bytes[0x56] = 1;
  component_bytes[0x57] = 1;
  put16(component_bytes + 0x5a, 0x1100);
  put16(bytes + 0x1543e + 2, 0x4000);
  put16(bytes + 0x1573e + 64 * 2, 0x0100);
  put16(bytes + 0x78802 + 12 * 2, 0xffff);
  {
    struct sc88_tvf_envelope envelope;
    assert(sc88_tvf_envelope_prepare(&rom, &tone, &component, 60, 100,
                                      false, &envelope));
    assert(envelope.depth == 0x3fff);
    assert(envelope.targets[0] == 0x0fff);
    assert(envelope.targets[1] == 0x07ff);
    assert(envelope.targets[2] == -0x0800);
    assert(envelope.stage == 1);
    assert(envelope.base == envelope.targets[0]);
    assert(envelope.current == envelope.targets[0]);
    assert(envelope.increments[1] == 0x4000);
    assert(sc88_tvf_envelope_advance(&envelope, 1));
    assert(envelope.current < envelope.targets[0]);
    assert(sc88_tvf_prepare_registers(&rom, &component, 0, &neutral,
                                      &registers));
    assert(sc88_tvf_update_frequency(&rom, envelope.current, &registers));
    assert(registers.frequency_target != registers.frequency_current);
  }

  free(bytes);
  return 0;
}
