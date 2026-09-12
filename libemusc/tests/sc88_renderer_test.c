/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_renderer.h"
#include "sc88_engine.h"

#include <assert.h>
#ifdef NDEBUG
#error "this test is assertion-driven; NDEBUG compiles it away"
#endif
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

static void test_held_rom(char **paths)
{
  static const uint8_t selectors[SC88_WAVE_BANK_COUNT] = {
    0x00, 0x01, 0x10, 0x11, 0x20, 0x21, 0x30, 0x31
  };
  struct sc88_wave_bank banks[SC88_WAVE_BANK_COUNT];
  struct sc88_renderer renderer;
  struct sc88_render_voice voice = {0};
  struct sc88_engine engine;
  uint8_t *control = read_exact(paths[0], SC88_CONTROL_ROM_SIZE);
  uint8_t *chips[4];
  float output[32];
  float engine_output[8192];
  double energy = 0.0;
  size_t i;

  for (i = 0; i < 4; ++i) {
    chips[i] = read_exact(paths[i + 1], SC88_WAVE_CHIP_SIZE);
    banks[i * 2].selector = selectors[i * 2];
    banks[i * 2].bytes = chips[i];
    banks[i * 2].size = SC88_WAVE_BANK_SIZE;
    banks[i * 2 + 1].selector = selectors[i * 2 + 1];
    banks[i * 2 + 1].bytes = chips[i] + SC88_WAVE_BANK_SIZE;
    banks[i * 2 + 1].size = SC88_WAVE_BANK_SIZE;
  }
  assert(sc88_renderer_init(&renderer, control, SC88_CONTROL_ROM_SIZE,
                            banks, SC88_WAVE_BANK_COUNT, 48000.0,
                            SC88_WRAP_FULL_CARRY));
  assert(sc88_renderer_note_on(&renderer, &voice, 0, 0, 60, 100, 0.25f));
  assert(voice.components[0].static_gain_q17 > 0);
  assert(voice.components[0].tvf.frequency_interpolation == 0x4100);
  assert(voice.components[0].tvf.resonance_interpolation == 0x095f);
  assert(sc88_renderer_render(&voice, output, 16) == 16);
  assert(sc88_renderer_voice_active(&voice));
  sc88_renderer_voice_destroy(&voice);
  assert(sc88_engine_init(&engine, &renderer));
  assert(sc88_engine_note_on(&engine, 0, 0, 0, 60, 100, 0,
                             SC88_SAME_NOTE_FULL_MULTI, 1.0f));
  sc88_engine_render(&engine, engine_output, 4096);
  for (i = 0; i < 8192; ++i)
    energy += fabs(engine_output[i]);
  assert(energy > 0.0);
  assert(sc88_engine_note_off(&engine, 0, 60));
  sc88_engine_destroy(&engine);
  for (i = 0; i < 4; ++i)
    free(chips[i]);
  free(control);
}

int main(int argc, char **argv)
{
  static const uint8_t selectors[SC88_WAVE_BANK_COUNT] = {
    0x00, 0x01, 0x10, 0x11, 0x20, 0x21, 0x30, 0x31
  };
  static const uint8_t vectors[16] = {
    0x00, 0x00, 0x02, 0x00, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x01, 0xf4, 0x00, 0x00, 0x01, 0xf4
  };
  uint8_t *control = (uint8_t *)calloc(SC88_CONTROL_ROM_SIZE, 1);
  uint8_t *wave = (uint8_t *)calloc(SC88_WAVE_BANK_SIZE, 1);
  struct sc88_wave_bank banks[SC88_WAVE_BANK_COUNT];
  struct sc88_renderer renderer;
  struct sc88_render_voice voice;
  struct sc88_component component;
  float output[4];
  size_t i;

  assert(control && wave);
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
  put16(control + 0x40000 + 34 + 0x10, 0);
  put16(control + 0x40000 + 34 + 0x14, 0x4000);
  /* A stage level word is an **attenuation**: zero is full level. Storing
     0xffff here once looked like full level because the conversion was
     inverted, which the ROM disproves - a piano's last two stages store
     0xffff and its tail is silent. */
  put16(control + 0x40000 + 34 + 0x78, 0x0000);
  /* and the opposite end, so the direction cannot invert again unnoticed */
  put16(control + 0x40000 + 34 + 0x7a, 0xffff);
  control[0x40000 + 34 + 0x80] = 1;
  /* The component's velocity window, +6c..+6d inclusive. A calloc'd
     fixture states 0..0, which sounds nothing: every note this file
     plays would be refused. The ROM's own tones all reach 127. */
  control[0x40000 + 34 + 0x6c] = 0;
  control[0x40000 + 34 + 0x6d] = 127;
  /* The coarse and fine level tables, as a monotone ramp. The real tables
     are a dB curve; what matters to a fixture is that an intermediate
     attenuation converts to an intermediate gain, because an envelope
     stage ramps its attenuation and reads the tables all the way along. */
  for (i = 0; i < 256; ++i) {
    put16(control + 0x1503e + i * 2, (uint16_t)((i + 1) * 256 - 1));
    put16(control + 0x1523e + i * 2, (uint16_t)((i + 1) * 256 - 1));
  }
  put16(control + 0x15db6 + 63 * 2, 0x4c00);
  put16(control + 0x1573e + 64 * 2, 0xffff);
  put16(control + 0x1543e + 2, 0xffff);
  control[0x30010] = 127;
  control[0x30011] = 0xff;
  put16(control + 0x30014, 0x6100);
  control[0x36100] = 0;
  put24(control + 0x36101, 0x8000);
  control[0x36106] = 60;
  put24(control + 0x36107, 0x8000);
  control[0x3610a] = 0x80;
  put24(control + 0x3610b, 0x8001);
  wave[0x400] = 0;
  wave[0x8000] = 1;
  wave[0x8001] = 1;

  component.bytes = control + 0x40000 + 34;
  component.offset = 0x40000 + 34;
  component.directory_offset = 0x30000;
  assert(sc88_renderer_selector_key(&component, 72) == 72);
  put16(control + 0x40000 + 34 + 0x14, 0x2000);
  assert(sc88_renderer_selector_key(&component, 59) == 59);
  assert(sc88_renderer_selector_key(&component, 72) == 66);
  put16(control + 0x40000 + 34 + 0x14, 0x4000);

  for (i = 0; i < SC88_WAVE_BANK_COUNT; ++i) {
    banks[i].selector = selectors[i];
    banks[i].bytes = wave;
    banks[i].size = SC88_WAVE_BANK_SIZE;
  }
  assert(sc88_renderer_init(&renderer, control, SC88_CONTROL_ROM_SIZE,
                            banks, SC88_WAVE_BANK_COUNT, 32000.0,
                            SC88_WRAP_FULL_CARRY));
  /* The velocity window gates the component, both bounds inclusive: inside
     it the note sounds, one count outside it there is no component to
     sound and the note is refused. Ignoring the window does not merely add
     a layer, it adds the loudest one - the firmware's velocity index runs
     on a wrapping byte, so one count below the window it saturates at the
     top of the curve. */
  control[0x40000 + 34 + 0x6c] = 100;
  control[0x40000 + 34 + 0x6d] = 110;
  memset(&voice, 0, sizeof voice);
  assert(!sc88_renderer_note_on(&renderer, &voice, 0, 0, 60, 99, 0.5f));
  assert(!sc88_renderer_note_on(&renderer, &voice, 0, 0, 60, 111, 0.5f));
  memset(&voice, 0, sizeof voice);
  assert(sc88_renderer_note_on(&renderer, &voice, 0, 0, 60, 110, 0.5f));
  assert(voice.component_count == 1);
  sc88_renderer_voice_destroy(&voice);
  control[0x40000 + 34 + 0x6c] = 0;
  control[0x40000 + 34 + 0x6d] = 127;

  memset(&voice, 0, sizeof voice);
  assert(sc88_renderer_note_on(&renderer, &voice, 0, 0, 60, 100, 0.5f));
  assert(sc88_renderer_voice_active(&voice));
  assert(voice.components[0].envelope.stage == 0);
  assert(voice.components[0].envelope.increments[0] == 0xffff);
  assert(voice.components[0].envelope.phase == 0xffff);
  assert(voice.components[0].envelope.targets_q17[0] == 0x1fffcu);
  /* zero attenuation is unity; full attenuation is silence, not the reverse */
  assert(voice.components[0].envelope.targets_q17[1] < 0x100u);
  assert(sc88_tva_envelope_linear_q17(
           &renderer.rom, &voice.components[0].envelope, 0.5) > 0);
  assert(sc88_tva_envelope_advance(&renderer.rom,
                                   &voice.components[0].envelope, 1));
  assert(voice.components[0].envelope.stage == 1);
  assert(voice.components[0].envelope.current_q17 == 0x1fffcu);
  assert(sc88_renderer_render(&voice, output, 2) == 2);
  assert(fabs(output[0] - (64.0 / 8388608.0) *
         (32767.0 / 32768.0) * (0x4c00 / 32768.0)) < 1e-9);
  assert(output[0] == output[1]);
  assert(output[2] > output[0]);
  assert(output[2] == output[3]);
  assert(!sc88_renderer_voice_active(&voice));
  sc88_renderer_voice_destroy(&voice);

  {
    const struct sc88_pan_controls hard_left = {64, 1};
    put16(control + 0x15db6 + 126 * 2, 0x8000);
    sc88_renderer_set_pan(&renderer, &hard_left);
    assert(sc88_renderer_note_on(&renderer, &voice, 0, 0, 60, 100, 0.5f));
    assert(sc88_renderer_render(&voice, output, 1) == 1);
    assert(output[0] > 0.0f && output[1] == 0.0f);
    sc88_renderer_voice_destroy(&voice);
  }

  {
    const struct sc88_tva_levels muted = {0, 127, 127, 127};
    put16(control + 0x14f3e, 0xffff);
    sc88_renderer_set_levels(&renderer, &muted);
    assert(sc88_renderer_note_on(&renderer, &voice, 0, 0, 60, 100, 0.5f));
    assert(voice.components[0].static_gain_q17 == 0);
    sc88_renderer_voice_destroy(&voice);
  }
  free(wave);
  free(control);
  if (argc == 6)
    test_held_rom(argv + 1);
  return 0;
}
