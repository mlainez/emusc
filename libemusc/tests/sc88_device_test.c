/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_device.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put16(uint8_t *p, uint16_t value)
{
  p[0] = (uint8_t)(value >> 8);
  p[1] = (uint8_t)value;
}

static void put24(uint8_t *p, uint32_t value)
{
  p[0] = (uint8_t)(value >> 16);
  p[1] = (uint8_t)(value >> 8);
  p[2] = (uint8_t)value;
}

static uint8_t *read_exact(const char *path, size_t size)
{
  FILE *file = fopen(path, "rb");
  uint8_t *bytes = (uint8_t *)malloc(size);
  assert(file && bytes);
  assert(fread(bytes, 1, size, file) == size);
  assert(fgetc(file) == EOF);
  fclose(file);
  return bytes;
}

static void make_control(uint8_t *control)
{
  static const uint8_t vectors[16] = {
    0x00, 0x00, 0x02, 0x00, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x01, 0xf4, 0x00, 0x00, 0x01, 0xf4
  };
  memcpy(control, vectors, sizeof vectors);
  memcpy(control + 0x30000, "\0\0Piano 1A    \3\377", 16);
  control[0x2fc80] = 0;
  put24(control + 0x20000, 0x40000);
  memcpy(control + 0x40000, "Test Tone   ", 12);
  control[0x40000 + 30] = 1;
  control[0x40000 + 32] = 3;
  control[0x40000 + 33] = 2;
  put16(control + 0x40000 + 0x0e, 0xbad0);
  put16(control + 0x40000 + 0x10, 0xb6d0);
  put16(control + 0x40000 + 34, 0);
  put16(control + 0x40000 + 34 + 0x14, 0x4000);
  put16(control + 0x40000 + 34 + 0x78, 0xffff);
  control[0x40000 + 34 + 0x80] = 1;
  control[0x30010] = 127;
  control[0x30011] = 0xff;
  put16(control + 0x30014, 0x6100);
  put24(control + 0x36101, 0x8000);
  control[0x36106] = 60;
  put24(control + 0x36107, 0x8000);
  put24(control + 0x3610b, 0x8001);
  put16(control + 0x1503e + 255 * 2, 0xffff);
  put16(control + 0x1523e + 255 * 2, 0xffff);
  put16(control + 0x15db6 + 63 * 2, 0x4c00);
  put16(control + 0x1573e + 64 * 2, 0xffff);
  put16(control + 0x1543e + 2, 0xffff);
}

static void test_held_raw(char **paths)
{
  uint8_t *control = read_exact(paths[0], SC88_CONTROL_ROM_SIZE);
  uint8_t *chips[SC88_WAVE_CHIP_COUNT];
  const uint8_t *inputs[SC88_WAVE_CHIP_COUNT];
  size_t sizes[SC88_WAVE_CHIP_COUNT];
  struct sc88_device device;
  float output[8192];
  double energy = 0.0;
  unsigned i;

  for (i = 0; i < SC88_WAVE_CHIP_COUNT; ++i) {
    chips[i] = read_exact(paths[i + 1], SC88_WAVE_CHIP_SIZE);
    inputs[i] = chips[i];
    sizes[i] = SC88_WAVE_CHIP_SIZE;
  }
  assert(sc88_device_init_raw(&device, control, SC88_CONTROL_ROM_SIZE,
                              inputs, sizes, 48000.0,
                              SC88_WRAP_FULL_CARRY));
  assert(sc88_device_midi(&device, 0, 0x90, 60, 100));
  sc88_device_render(&device, output, 4096);
  for (i = 0; i < 8192; ++i)
    energy += fabs(output[i]);
  assert(energy > 0.0);
  sc88_device_destroy(&device);
  for (i = 0; i < SC88_WAVE_CHIP_COUNT; ++i)
    free(chips[i]);
  free(control);
}

int main(int argc, char **argv)
{
  uint8_t *control = (uint8_t *)calloc(SC88_CONTROL_ROM_SIZE, 1);
  uint8_t *chip = (uint8_t *)calloc(SC88_WAVE_CHIP_SIZE, 1);
  const uint8_t *chips[SC88_WAVE_CHIP_COUNT] = {chip, chip, chip, chip};
  const size_t sizes[SC88_WAVE_CHIP_COUNT] = {
    SC88_WAVE_CHIP_SIZE, SC88_WAVE_CHIP_SIZE,
    SC88_WAVE_CHIP_SIZE, SC88_WAVE_CHIP_SIZE
  };
  struct sc88_device device;
  float output[514];

  assert(control && chip);
  make_control(control);
  chip[0x8000] = 1;
  chip[0x8001] = 1;
  assert(sc88_device_init_decoded(&device, control, SC88_CONTROL_ROM_SIZE,
                                  chips, sizes, 32000.0,
                                  SC88_WRAP_FULL_CARRY));
  assert(device.channels[0].volume == 100);
  assert(sc88_device_midi(&device, 0, 0x90, 60, 100));
  assert(sc88_engine_active_slots(&device.engine) == 1);
  {
    double unity_step = device.engine.slots[0].component.oscillator.step;
    assert(sc88_device_midi(&device, 0, 0xe0, 127, 127));
    assert(device.engine.slots[0].component.oscillator.step > unity_step);
    assert(sc88_device_midi(&device, 0, 0xb0, 121, 0));
    assert(fabs(device.engine.slots[0].component.oscillator.step -
                unity_step) < 1e-12);
  }
  sc88_device_render(&device, output, 257);
  assert(output[0] > 0.0f && output[0] == output[1]);

  put16(device.control_rom + 0x14f3e, 0xffff);
  assert(sc88_device_midi(&device, 0, 0xb0, 7, 0));
  sc88_device_render(&device, output, 1);
  assert(output[0] == 0.0f && output[1] == 0.0f);
  put16(device.control_rom + 0x14f3e, 0);
  assert(sc88_device_midi(&device, 0, 0xb0, 7, 100));
  assert(sc88_device_midi(&device, 0, 0xb0, 64, 127));
  assert(sc88_device_midi(&device, 0, 0x80, 60, 64));
  sc88_device_render(&device, output, 257);
  assert(sc88_engine_active_slots(&device.engine) == 1);
  assert(sc88_device_midi(&device, 0, 0xb0, 64, 0));
  sc88_device_render(&device, output, 257);
  assert(sc88_engine_active_slots(&device.engine) == 0);
  assert(sc88_device_midi(&device, 1, 0xc0, 0, 0));
  assert(!sc88_device_midi(&device, 2, 0x90, 60, 100));
  sc88_device_destroy(&device);
  free(chip);
  free(control);
  if (argc == 6)
    test_held_raw(argv + 1);
  return 0;
}
