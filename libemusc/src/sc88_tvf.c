/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_tvf.h"

#include <limits.h>
#include <string.h>

#define SC88_TVF_BASE_TABLE 0x78702u
#define SC88_TVF_LIMIT_TABLE 0x78802u

static uint16_t sc88_tvf_be16(const uint8_t *p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static int8_t sc88_tvf_s8(uint8_t value)
{
  return value <= INT8_MAX
    ? (int8_t)value
    : (int8_t)(-1 - (int16_t)(UINT8_MAX - value));
}

static int sc88_tvf_clamp_index(int value)
{
  if (value < 0)
    return 0;
  if (value > 127)
    return 127;
  return value;
}

bool sc88_tvf_prepare_registers(const struct sc88_rom *rom,
                                const struct sc88_component *component,
                                int16_t accumulated_modulation,
                                const struct sc88_tvf_controls *controls,
                                struct sc88_tvf_registers *registers)
{
  int mode;
  int cutoff_index;
  int resonance_index;
  int resonance_floor;
  int32_t combined;
  uint16_t limit;

  if (!rom || !rom->bytes || rom->size < SC88_TVF_LIMIT_TABLE + 256u ||
      !component || !component->bytes || !controls || !registers ||
      controls->part_cutoff > 127 || controls->secondary_cutoff > 127 ||
      controls->part_resonance > 127 ||
      controls->secondary_resonance > 127)
    return false;

  memset(registers, 0, sizeof *registers);
  registers->frequency_interpolation = 0x4100;
  registers->resonance_interpolation = 0x095f;
  mode = sc88_tvf_s8(component->bytes[0x3e]);
  if (mode < 0) {
    registers->resonance_current = 0x20000;
    registers->resonance_target = 0x80000;
    registers->filter_select = 0x0800;
    registers->fixed_tuple = true;
    return true;
  }

  cutoff_index = sc88_tvf_clamp_index(
    component->bytes[0x3c] + controls->part_cutoff +
    controls->secondary_cutoff - 128);
  resonance_index = sc88_tvf_clamp_index(
    component->bytes[0x3d] - 2 * (controls->part_resonance +
                                  controls->secondary_resonance - 128));
  resonance_floor = component->bytes[0x3d] < 4
    ? component->bytes[0x3d] : 4;
  if (resonance_index < resonance_floor)
    resonance_index = resonance_floor;

  combined = sc88_tvf_be16(rom->bytes + SC88_TVF_BASE_TABLE +
                           (unsigned)cutoff_index * 2u);
  combined += accumulated_modulation;
  if (combined < 0)
    combined = 0;
  else if (combined > UINT16_MAX)
    combined = UINT16_MAX;
  combined >>= 1;
  limit = (uint16_t)(sc88_tvf_be16(
    rom->bytes + SC88_TVF_LIMIT_TABLE + (unsigned)resonance_index * 2u) >> 1);
  if (combined > limit)
    combined = limit;

  registers->cutoff_index = (uint8_t)cutoff_index;
  registers->resonance_index = (uint8_t)resonance_index;
  registers->combined = (uint16_t)combined;
  registers->frequency_current = (uint32_t)combined << 3;
  registers->frequency_target = registers->frequency_current;
  registers->resonance_current = (uint32_t)resonance_index << 11;
  registers->resonance_target = (uint32_t)resonance_index << 13;
  registers->filter_select = (uint16_t)((unsigned)mode << 8);
  return true;
}
