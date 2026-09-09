/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_rom.h"
#include "sc88_pan.h"
#include "sc88_tva.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put24(uint8_t *p, uint32_t value)
{
  p[0] = (uint8_t)(value >> 16);
  p[1] = (uint8_t)(value >> 8);
  p[2] = (uint8_t)value;
}

static void test_held_rom(const char *path)
{
  FILE *file = fopen(path, "rb");
  uint8_t *bytes = (uint8_t *)malloc(SC88_CONTROL_ROM_SIZE);
  bool seen[SC88_CONTROL_ROM_SIZE] = {false};
  struct sc88_rom rom;
  unsigned variation;
  unsigned program;
  unsigned selection_count = 0;
  unsigned tone_count = 0;
  unsigned component_count = 0;
  const struct sc88_tva_levels levels = {127, 127, 127, 127};
  const struct sc88_pan_controls pan = {64, 64};

  assert(file && bytes);
  assert(fread(bytes, 1, SC88_CONTROL_ROM_SIZE, file) ==
         SC88_CONTROL_ROM_SIZE);
  assert(fgetc(file) == EOF);
  fclose(file);
  assert(sc88_rom_init(&rom, bytes, SC88_CONTROL_ROM_SIZE));

  for (variation = 0; variation < 128; ++variation) {
    for (program = 0; program < 128; ++program) {
      struct sc88_tone tone;
      uint32_t offset;
      unsigned component_index;

      if (!sc88_rom_select_melodic(&rom, (uint8_t)variation,
                                    (uint8_t)program, &offset))
        continue;
      ++selection_count;
      assert(sc88_rom_open_tone(&rom, offset, &tone));
      if (seen[offset])
        continue;
      seen[offset] = true;
      ++tone_count;
      component_count += tone.component_count;
      for (component_index = 0; component_index < tone.component_count;
           ++component_index) {
        struct sc88_component component;
        unsigned key;
        unsigned selected = 0;
        assert(sc88_rom_open_component(&rom, &tone, component_index,
                                       &component));
        for (key = 0; key < 128; ++key) {
          struct sc88_zone_selection zone;
          enum sc88_wave_loop_type mode;
          uint16_t attenuation;
          uint16_t left_q15;
          uint16_t right_q15;
          uint32_t gain_q17;
          uint8_t pan_position;
          struct sc88_tva_release release;
          if (!sc88_rom_select_zone(&rom, &component, (uint8_t)key, &zone))
            continue;
          assert(sc88_wave_descriptor_loop_type(&zone.descriptor, &mode));
          if (!sc88_tva_static_gain_q17(
                &rom, &tone, &component, &zone, (uint8_t)key, 100, &levels,
                &attenuation, &gain_q17)) {
            fprintf(stderr, "TVA failed tone=%#x component=%#x key=%u\n",
                    tone.offset, component.offset, key);
            assert(false);
          }
          assert(gain_q17 <= 0x1fffcu);
          assert(sc88_pan_static_q15(
            &rom, &tone, &component, (uint8_t)key, &pan, &pan_position,
            &left_q15, &right_q15));
          assert(pan_position >= 1 && pan_position <= 127);
          assert(sc88_tva_release_prepare(
            &rom, &tone, &component, (uint8_t)key, &release));
          assert(release.current == 0xffff && !release.active);
          assert(sc88_tva_release_set_pedal(
            &rom, 0, tone.common[0x15] != 0, tone.common[0x14] != 0,
            false, &release));
          assert(release.active);
          ++selected;
        }
        assert(selected > 0);
      }
    }
  }
  assert(selection_count == 418);
  assert(tone_count == 418);
  assert(component_count == 635);
  free(bytes);
}

int main(int argc, char **argv)
{
  static const uint8_t vectors[16] = {
    0x00, 0x00, 0x02, 0x00, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x01, 0xf4, 0x00, 0x00, 0x01, 0xf4
  };
  static const uint8_t directory_header[16] = {
    0x00, 0x00, 'P', 'i', 'a', 'n', 'o', ' ',
    '1', 'A', ' ', ' ', ' ', ' ', 0x03, 0xff
  };
  static const uint8_t descriptor[20] = {
    0x00, 0x01, 0xee, 0x60, 0x00, 0x20, 0x24, 0x02, 0x6f, 0x9c,
    0x00, 0x02, 0x92, 0x0d, 0x00, 0x00, 0x0a, 0x00, 0xf4, 0x30
  };
  uint8_t *bytes = (uint8_t *)calloc(SC88_CONTROL_ROM_SIZE, 1);
  struct sc88_rom rom;
  struct sc88_tone tone;
  struct sc88_component component;
  struct sc88_zone_selection zone;
  uint32_t tone_offset;
  char name[13];

  assert(bytes);
  memcpy(bytes, vectors, sizeof vectors);
  memcpy(bytes + 0x30000, directory_header, sizeof directory_header);
  bytes[0x2fc80] = 0;
  put24(bytes + 0x20000, 0x40000);
  memcpy(bytes + 0x40000, "Test Tone   ", 12);
  bytes[0x40000 + 30] = 1;
  bytes[0x40000 + 32] = 3;
  bytes[0x40000 + 34] = 0;
  bytes[0x40000 + 35] = 0;
  bytes[0x30010] = 127;
  bytes[0x30011] = 0xff;
  bytes[0x30012] = 0x12;
  bytes[0x30013] = 0x34;
  bytes[0x30014] = 0x61;
  bytes[0x30015] = 0x00;
  memcpy(bytes + 0x36100, descriptor, sizeof descriptor);

  assert(sc88_rom_init(&rom, bytes, SC88_CONTROL_ROM_SIZE));
  assert(sc88_rom_select_melodic(&rom, 0, 0, &tone_offset));
  assert(tone_offset == 0x40000);
  assert(sc88_rom_open_tone(&rom, tone_offset, &tone));
  assert(tone.component_count == 1);
  sc88_rom_tone_name(&tone, name);
  assert(strcmp(name, "Test Tone   ") == 0);
  assert(sc88_rom_open_component(&rom, &tone, 0, &component));
  assert(component.directory_offset == 0x30000);
  assert(sc88_rom_select_zone(&rom, &component, 60, &zone));
  assert(zone.boundary == 127);
  assert(zone.static_attenuation == 0x1234);
  assert(zone.descriptor_offset == 0x36100);
  assert(zone.descriptor.address_a == 0x01ee60);

  assert(!sc88_rom_select_melodic(&rom, 0, 128, &tone_offset));
  bytes[0] ^= 1;
  assert(!sc88_rom_init(&rom, bytes, SC88_CONTROL_ROM_SIZE));
  free(bytes);
  if (argc == 2)
    test_held_rom(argv[1]);
  return 0;
}
