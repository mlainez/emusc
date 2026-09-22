/* SPDX-License-Identifier: CC0-1.0 */
#include "engines/xp/rom.h"
#include "engines/xp/pan.h"
#include "engines/xp/pitch.h"
#include "engines/xp/tva.h"
#include "engines/xp/tvf.h"

#include <cassert>
#ifdef NDEBUG
#error "this test is assertion-driven; NDEBUG compiles it away"
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace EmuSC::Xp;

static void put24(uint8_t *p, uint32_t value)
{
  p[0] = (uint8_t)(value >> 16);
  p[1] = (uint8_t)(value >> 8);
  p[2] = (uint8_t)value;
}

/* The held control ROM's own path is read from the environment at run
   time, so ctest's own environment carries it whatever the tree was
   configured with:
     SC88_CONTROL_ROM  the control ROM
   Without it the caller skips rather than passing while checking only
   the synthetic fixtures above. */
static void test_held_rom(const char *path)
{
  FILE *file = fopen(path, "rb");
  uint8_t *bytes = (uint8_t *)malloc(XP_CONTROL_ROM_SIZE);
  bool seen[XP_CONTROL_ROM_SIZE] = {false};
  struct xp_rom rom;
  unsigned map;
  unsigned variation;
  unsigned program;
  unsigned selection_count = 0;
  unsigned tone_count = 0;
  unsigned component_count = 0;
  const struct xp_tva_levels levels = {127, 127, 127, 127};
  const struct xp_pan_controls pan = {64, 64};
  const struct xp_tvf_controls tvf_controls = {64, 64, 64, 64, 0};

  assert(file && bytes);
  assert(fread(bytes, 1, XP_CONTROL_ROM_SIZE, file) ==
         XP_CONTROL_ROM_SIZE);
  assert(fgetc(file) == EOF);
  fclose(file);
  assert(rom_init(&rom, bytes, XP_CONTROL_ROM_SIZE));

  /* Both rows of the lookup, in order, so that the counts below separate
     the SC-55 map's tones from the ones only the SC-88 map reaches. */
  for (map = XP_TONE_MAP_SC55; map <= XP_TONE_MAP_SC88; ++map) {
    for (variation = 0; variation < 128; ++variation) {
      for (program = 0; program < 128; ++program) {
        struct xp_tone tone;
        uint32_t offset;
        unsigned component_index;

        if (!rom_select_melodic(&rom, (uint8_t)map, (uint8_t)variation,
                                 (uint8_t)program, &offset))
          continue;
        ++selection_count;
        assert(rom_open_tone(&rom, offset, &tone));
        if (seen[offset])
          continue;
        seen[offset] = true;
        ++tone_count;
        component_count += tone.component_count;
        for (component_index = 0; component_index < tone.component_count;
             ++component_index) {
          struct xp_component component;
          unsigned key;
          unsigned selected = 0;
          assert(rom_open_component(&rom, &tone, component_index,
                                    &component));
          for (key = 0; key < 128; ++key) {
            struct xp_zone_selection zone;
            enum xp_wave_loop_type mode;
            uint16_t attenuation;
            uint16_t left_q15;
            uint16_t right_q15;
            uint32_t gain_q17;
            uint8_t pan_position;
            struct xp_tva_release release;
            struct xp_tva_envelope envelope;
            struct xp_tvf_registers tvf;
            struct xp_tvf_envelope tvf_envelope;
            struct xp_tvf_release tvf_release;
            struct xp_pitch_envelope pitch_envelope;
            struct xp_pitch_release pitch_release;
            int16_t tvf_key_modulation;
            if (!rom_select_zone(&rom, &component, (uint8_t)key, &zone))
              continue;
            assert(wave_descriptor_loop_type(&zone.descriptor, &mode));
            if (!tva_static_gain_q17(
                  &rom, &tone, &component, &zone, (uint8_t)key, 100, &levels,
                  XP_TVA_NO_DRUM_LEVEL, &attenuation, &gain_q17)) {
              fprintf(stderr, "TVA failed tone=%#x component=%#x key=%u\n",
                      tone.offset, component.offset, key);
              assert(false);
            }
            assert(gain_q17 <= 0x1fffcu);
            assert(pan_static_q15(
              &rom, &tone, &component, (uint8_t)key, &pan, &pan_position,
              &left_q15, &right_q15));
            assert(pan_position >= 1 && pan_position <= 127);
            assert(tva_release_prepare(
              &rom, &tone, &component, (uint8_t)key, &release));
            assert(release.current == 0xffff && !release.active);
            assert(tva_release_set_pedal(
              &rom, 0, tone.common[0x15] != 0, tone.common[0x14] != 0,
              false, &release));
            assert(release.active);
            assert(tva_envelope_prepare(
              &rom, &tone, &component, (uint8_t)key, 100, NULL, &envelope));
            assert(envelope.stage <= 1 && envelope.active);
            assert(tvf_prepare_registers(
              &rom, &component, 0, &tvf_controls, &tvf));
            assert(tvf.frequency_interpolation == 0x4100);
            assert(tvf.resonance_interpolation == 0x095f);
            assert(tvf.frequency_target == tvf.frequency_current);
            assert(tvf.resonance_target >> 2 == tvf.resonance_current);
            assert(tvf_envelope_prepare(
              &rom, &tone, &component, (uint8_t)key, 100, false,
              &tvf_envelope));
            assert(tvf_envelope.depth == 0 || tvf_envelope.stage <= 1);
            assert(EmuSC::Xp::tvf_key_modulation(
              &rom, &tone, &component, (uint8_t)key,
              &tvf_key_modulation));
            assert(tvf_release_prepare(
              &rom, &tone, &component, (uint8_t)key, tvf_envelope.depth,
              &tvf_release));
            assert(pitch_envelope_prepare(
              &rom, &tone, &component, (uint8_t)key, 100,
              &pitch_envelope));
            assert(pitch_envelope.stage <= 1 && pitch_envelope.active);
            assert(pitch_release_prepare(
              &rom, &tone, &component, (uint8_t)key, pitch_envelope.depth,
              &pitch_release));
            ++selected;
          }
          assert(selected > 0);
        }
      }
    }
    /* The SC-55 row comes first: its 418 selections resolve 354 distinct
       tones of 469 components. The SC-88 row's own 418 selections are all
       distinct, and 95 of them are tones the SC-55 row already reached, so
       it adds 323 - leaving 259 tones the SC-55 row alone can play. */
    assert(selection_count == (map == XP_TONE_MAP_SC55 ? 418u : 836u));
    assert(tone_count == (map == XP_TONE_MAP_SC55 ? 354u : 677u));
    assert(component_count == (map == XP_TONE_MAP_SC55 ? 469u : 973u));
  }
  free(bytes);
}

int main()
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
  uint8_t *bytes = (uint8_t *)calloc(XP_CONTROL_ROM_SIZE, 1);
  struct xp_rom rom;
  struct xp_tone tone;
  struct xp_component component;
  struct xp_zone_selection zone;
  uint32_t tone_offset;
  char name[13];

  assert(bytes);
  memcpy(bytes, vectors, sizeof vectors);
  memcpy(bytes + 0x30000, directory_header, sizeof directory_header);
  /* The two rows of the variation lookup: the SC-55 one is emptied at
     variation 0 so that the map can be told apart from the variation. */
  bytes[0x2fc00] = 0xff;
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

  assert(rom_init(&rom, bytes, XP_CONTROL_ROM_SIZE));
  assert(rom_select_melodic(&rom, XP_TONE_MAP_SC88, 0, 0,
                            &tone_offset));
  assert(tone_offset == 0x40000);
  assert(rom_open_tone(&rom, tone_offset, &tone));
  assert(tone.component_count == 1);
  rom_tone_name(&tone, name);
  assert(strcmp(name, "Test Tone   ") == 0);
  assert(rom_open_component(&rom, &tone, 0, &component));
  assert(component.directory_offset == 0x30000);
  assert(rom_select_zone(&rom, &component, 60, &zone));
  assert(zone.boundary == 127);
  assert(zone.static_attenuation == 0x1234);
  assert(zone.descriptor_offset == 0x36100);
  assert(zone.descriptor.address_a == 0x01ee60);

  /* The map is an axis of its own. The same variation and program on the
     SC-55 row resolve nothing while that row is empty and the tone once it
     is filled; a map outside 1..2 is what `2d59` refuses outright. */
  assert(!rom_select_melodic(&rom, XP_TONE_MAP_SC55, 0, 0,
                             &tone_offset));
  bytes[0x2fc00] = 0;
  assert(rom_select_melodic(&rom, XP_TONE_MAP_SC55, 0, 0,
                            &tone_offset));
  assert(tone_offset == 0x40000);
  assert(!rom_select_melodic(&rom, 0, 0, 0, &tone_offset));
  assert(!rom_select_melodic(&rom, 3, 0, 0, &tone_offset));
  assert(!rom_select_melodic(&rom, XP_TONE_MAP_SC88, 0, 128,
                             &tone_offset));
  assert(!rom_select_melodic(&rom, XP_TONE_MAP_SC88, 128, 0,
                             &tone_offset));
  bytes[0] ^= 1;
  assert(!rom_init(&rom, bytes, XP_CONTROL_ROM_SIZE));
  free(bytes);

  {
    const char *rom_path = getenv("SC88_CONTROL_ROM");
    FILE *probe;
    if (!rom_path)
      return 77;                          /* no ROM given: skip */
    probe = fopen(rom_path, "rb");
    if (!probe)
      return 77;
    fclose(probe);
    test_held_rom(rom_path);
  }
  return 0;
}
