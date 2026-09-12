/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_tvf.h"

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
  /* Base and limit are both halved, so 0x6000 less 0x1000 of pre-base
     modulation becomes 0x2800 against a limit of 0x2800. The two must
     share a domain or the clamp compares nothing. */
  assert(registers.combined == 0x2800);
  assert(registers.base_value == 0x2800);
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
  {
    struct sc88_tvf_audio_state audio;
    float sample;
    sc88_tvf_audio_reset(&audio);
    sample = sc88_tvf_audio_process_provisional(
      NULL, &audio, &registers, 1.0, 0.25f);
    assert(sample == 0.25f);
  }

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
  put16(component_bytes + 0x5c, 0x1100);
  put16(component_bytes + 0x40, 0x1200);
  put16(component_bytes + 0x42, 0x4000);
  put16(bytes + 0x1200 + 60 * 2, 0x4000);
  put16(bytes + 0x1543e + 2, 0x4000);
  put16(bytes + 0x1573e + 64 * 2, 0x0100);
  put16(bytes + 0x78802 + 12 * 2, 0xffff);
  {
    struct sc88_tvf_envelope envelope;
    int16_t key_modulation;
    assert(sc88_tvf_key_modulation(&rom, &tone, &component, 60,
                                    &key_modulation));
    assert(key_modulation == 0x2000);
    put16(bytes + 0x1200 + 60 * 2, 0xc000);
    assert(sc88_tvf_key_modulation(&rom, &tone, &component, 60,
                                    &key_modulation));
    assert(key_modulation == -0x2000);
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
    {
      struct sc88_tvf_audio_state audio;
      float filtered;
      unsigned n;
      sc88_tvf_audio_reset(&audio);
      /* The forward-Euler low-pass reads its integrators before it
         writes them, so a cleared filter's first output is zero: the
         section carries one sample of delay and nothing else. Asserting
         instead that the first sample is already inside (0, 1) tests the
         trapezoidal form's instantaneous path, which this topology does
         not have. */
      filtered = sc88_tvf_audio_process_provisional(
        NULL, &audio, &registers, 0.5, 1.0f);
      assert(filtered == 0.0f);
      /* The corner these registers ask for is low enough that the step
         takes tens of thousands of samples to arrive; the count is what
         it takes, not a round number. */
      for (n = 0; n < 262144; ++n) {
        filtered = sc88_tvf_audio_process_provisional(
          NULL, &audio, &registers, 0.5, 1.0f);
        assert(filtered > -1.0f && filtered < 2.0f);
      }
      /* Held at one a low-pass settles on one: the section is unity at
         DC, which is the property that says the coefficients are paired
         correctly. */
      assert(filtered > 0.999f && filtered < 1.001f);
    }
    {
      struct sc88_tvf_release release;
      component_bytes[0x58] = 1;
      put16(component_bytes + 0x52, 0xc000);
      assert(sc88_tvf_release_prepare(
        &rom, &tone, &component, 60, envelope.depth, &release));
      assert(release.target == -0x1000);
      assert(release.increment == 0x4000);
      assert(sc88_tvf_release_set_pedal(
        &rom, 0, false, false, false, &release));
      assert(!release.scale_enabled && release.active);
      assert(sc88_tvf_release_advance(&release, 1));
      assert(release.phase == 0x4000);
      assert(release.current == -0x0400);
      assert(sc88_tvf_release_advance(&release, 3));
      assert(release.current == release.target && !release.active);
    }
  }

  free(bytes);
  return 0;
}
