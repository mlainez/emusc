/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_tvf.h"

#include <assert.h>
#ifdef NDEBUG
#error "this test is assertion-driven; NDEBUG compiles it away"
#endif
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void put16(uint8_t *p, uint16_t value)
{
  p[0] = (uint8_t)(value >> 8);
  p[1] = (uint8_t)value;
}

/* The cutoff word's anchor, from the two limit-table entries that fix it
   without rounding. f*f + f*q = 2 gives f = sqrt(2) at resonance index 0
   and f = 1 at index 64; with f = 2*sin(pi*fc/fs) those ceilings are
   fs/4 and fs/6 exactly. The ROM stores them as 0xf800 and 0xf000, which
   the firmware expands to the words below, so a word_to_hz that does not
   return fs/4 and fs/6 there is reading the table at the wrong anchor.
   Both equations are exact in integers, so the tolerance only has to
   clear double precision: 1e-6 octave is 60 times tighter than the one
   word unit that already breaks 28 entries of each table, and 1e9 times
   looser than the arithmetic's own error. */
static void check_anchor(void)
{
  const double fs = 32000.0;
  const double tol = 1e-6;                  /* octaves */

  assert(fabs(log2(sc88_tvf_word_to_hz(0x3e000) / (fs / 4.0))) < tol);
  assert(fabs(log2(sc88_tvf_word_to_hz(0x3c000) / (fs / 6.0))) < tol);
  assert(sc88_tvf_word_to_hz(0x40000) == fs / 2.0);
  assert(sc88_tvf_word_to_hz(0x40000 + 1) == fs / 2.0);

  /* One octave of word is one octave of sine, not of frequency: halving
     the sine from 1/2 does not halve 5333 Hz. */
  assert(sc88_tvf_word_to_hz(0x3c000 - 16384) <
         0.5 * sc88_tvf_word_to_hz(0x3c000));
}

int main(void)
{
  check_anchor();

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
  const struct sc88_tvf_controls neutral = {64, 64, 64, 64, 0};
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

  /* `0x6ad0`..`0x6aff`, the matrix's cutoff term. The clamp is signed and
     symmetric at +-4000; full scale is two octaves of the base table's
     4096-per-octave word, which is what the constant 0x8312 was chosen
     for (`07_synthesis/tvf.md`). */
  assert(sc88_tvf_matrix_cutoff_term(0) == 0);
  assert(sc88_tvf_matrix_cutoff_term(4000) == 8191);
  assert(sc88_tvf_matrix_cutoff_term(-4000) == -8192);
  assert(sc88_tvf_matrix_cutoff_term(32767) == 8191);
  assert(sc88_tvf_matrix_cutoff_term(-32768) == -8192);
  assert(sc88_tvf_matrix_cutoff_term(2000) == 4095);
  assert(sc88_tvf_matrix_cutoff_term(-2000) == -4096);
  /* Both shifts floor, so the two signs are not mirror images; an
     implementation that rounded toward zero would read -2 and -4 here. */
  assert(sc88_tvf_matrix_cutoff_term(1) == 2);
  assert(sc88_tvf_matrix_cutoff_term(-1) == -3);
  assert(sc88_tvf_matrix_cutoff_term(-2) == -5);

  /* It enters where the key term does, before the base table's entry is
     added and before the halving - `0x6cbd`..`0x6cc9` builds RAM 30da
     from both. So one unit of matrix cutoff weighs exactly what one unit
     of key modulation does. */
  {
    struct sc88_tvf_controls matrix = {64, 64, 64, 64, 0};
    struct sc88_tvf_registers with_key;
    struct sc88_tvf_registers with_matrix;
    component_bytes[0x3e] = 4;
    /* 0x6ad4..0x6afd turns 1000 into 2047. */
    matrix.matrix_cutoff = 1000;
    assert(sc88_tvf_matrix_cutoff_term(1000) == 2047);
    assert(sc88_tvf_prepare_registers(&rom, &component, 2047, &neutral,
                                      &with_key));
    assert(sc88_tvf_prepare_registers(&rom, &component, 0, &matrix,
                                      &with_matrix));
    assert(with_key.base_unshifted == with_matrix.base_unshifted);
    assert(with_key.base_value == with_matrix.base_value);
    /* And the two add, wrapping, rather than one replacing the other. */
    assert(sc88_tvf_prepare_registers(&rom, &component, -2047, &matrix,
                                      &with_matrix));
    assert(with_matrix.base_unshifted == 0x6000);
    /* A neutral matrix leaves the word exactly where it was. */
    matrix.matrix_cutoff = 0;
    assert(sc88_tvf_prepare_registers(&rom, &component, -0x1000, &matrix,
                                      &with_matrix));
    assert(with_matrix.combined == 0x2800);
  }

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
      /* Type code 2 takes the high-pass output of the same section, and
         a high-pass is zero at DC. Same registers, same input, opposite
         limit: this is what separates the two readings of the code, and
         it fails if code 2 ever falls back to the low-pass. */
      registers.filter_select = 0x0800;
      sc88_tvf_audio_reset(&audio);
      for (n = 0; n < 262144; ++n) {
        filtered = sc88_tvf_audio_process_provisional(
          NULL, &audio, &registers, 0.5, 1.0f);
        assert(filtered > -1.0f && filtered < 2.0f);
      }
      assert(filtered > -0.001f && filtered < 0.001f);
      /* Code 0 and code 1 keep the low-pass. */
      registers.filter_select = 0x0400;
      sc88_tvf_audio_reset(&audio);
      for (n = 0; n < 262144; ++n)
        filtered = sc88_tvf_audio_process_provisional(
          NULL, &audio, &registers, 0.5, 1.0f);
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
