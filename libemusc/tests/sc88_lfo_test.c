/* SPDX-License-Identifier: CC0-1.0 */
#include "engines/xp/sc88_lfo.h"

#include <assert.h>
#ifdef NDEBUG
#error "this test is assertion-driven; NDEBUG compiles it away"
#endif
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RATE_TABLE 0x29dau
#define DELAY_TABLE 0x2adau
#define SINE_TABLE 0x1492cu

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
  uint8_t common[SC88_TONE_COMMON_SIZE] = {0};
  uint8_t component_bytes[SC88_COMPONENT_SIZE] = {0};
  struct sc88_rom rom;
  struct sc88_tone tone = {common, 0x40000, 1};
  struct sc88_component component = {component_bytes, 0, 0};
  struct sc88_lfo lfo;
  struct sc88_lfo_ramp ramp;
  uint16_t phase, seed, target;
  uint8_t index;
  int16_t control, delay_index, out;
  assert(bytes);
  memcpy(bytes, vectors, sizeof vectors);
  memcpy(bytes + 0x30000, "\0\0Piano 1A    \3\377", 16);
  assert(sc88_rom_init(&rom, bytes, SC88_CONTROL_ROM_SIZE));

  /* The live rate contribution clips at +-4000 before doubling and keeps the
     signed product's high word, so the span is asymmetric: -5244..5243. */
  assert(sc88_lfo_rate_control(4000, &control) && control == 5243);
  assert(sc88_lfo_rate_control(32767, &control) && control == 5243);
  assert(sc88_lfo_rate_control(-4000, &control) && control == -5244);
  assert(sc88_lfo_rate_control(-32768, &control) && control == -5244);
  assert(sc88_lfo_rate_control(0, &control) && control == 0);

  /* Both modifiers are centred at 64, so a neutral pair leaves the tone's
     own index alone, and each end clamps rather than wrapping. */
  assert(sc88_lfo_common_rate_index(60, 64, 64, &index) && index == 60);
  assert(sc88_lfo_common_rate_index(60, 127, 127, &index) && index == 127);
  assert(sc88_lfo_common_rate_index(60, 0, 0, &index) && index == 0);
  assert(!sc88_lfo_common_rate_index(128, 64, 64, &index));
  assert(sc88_lfo_common_delay_index(40, 64, 64, &delay_index) &&
         delay_index == 40);
  /* a negative delay byte bypasses the table instead of indexing it */
  assert(sc88_lfo_common_delay_index(-1, 64, 64, &delay_index) &&
         delay_index == -1);

  /* Base and contribution add with 16-bit wrap before the signed test: a
     total at or below zero stops the oscillator, a positive one saturates. */
  assert(sc88_lfo_effective_increment(0x1000, 0x100, &phase) &&
         phase == 0x1100);
  assert(sc88_lfo_effective_increment(0x0100, -0x0100, &phase) && phase == 0);
  assert(sc88_lfo_effective_increment(0x0100, -0x0200, &phase) && phase == 0);
  assert(sc88_lfo_effective_increment(0x28f6, 1, &phase) && phase == 0x28f6);
  assert(sc88_lfo_effective_increment(0xffff, 2, &phase) && phase == 1);

  /* The procedural shapes, at the phases that define them. */
  assert(sc88_lfo_square(0) == 32767 && sc88_lfo_square(0x8000) == -32768);
  assert(sc88_lfo_triangle(0) == 0);
  assert(sc88_lfo_triangle(0x4000) == 32767);
  assert(sc88_lfo_triangle(0xc000) == -32768);
  assert(sc88_lfo_rectified_triangle(0) == 0);
  assert(sc88_lfo_rectified_triangle(0x4000) == 32767);
  /* the negative half is shifted by -32767 modulo 65536, not rectified */
  assert(sc88_lfo_rectified_triangle(0xc000) ==
         (int16_t)((uint16_t)((uint16_t)-32768 - (uint16_t)0x7fff)));

  /* The random word is a byte swap of seed plus phase. */
  assert(sc88_lfo_random_target(0x0000, 0x1234) == 0x3412);
  assert(sc88_lfo_random_target(0x00ff, 0x0001) == 0x0001);
  /* and the slew moves by at most 0x1c2, stopping exactly on the target */
  assert(sc88_lfo_slew_random(0, 32767) == 0x1c2);
  assert(sc88_lfo_slew_random(0, -32768) == -0x1c2);
  assert(sc88_lfo_slew_random(100, 150) == 150);
  assert(sc88_lfo_slew_random(-100, -150) == -150);

  /* Phase advance runs catchup+1 times and turns the random word over on
     signed overflow rather than on unsigned carry. */
  phase = 0;
  seed = 0;
  target = 0;
  assert(sc88_lfo_phase_advance(0x1000, 0, &phase, &seed, &target) &&
         phase == 0x1000 && seed == 0 && target == 0);
  assert(sc88_lfo_phase_advance(0x1000, 3, &phase, &seed, &target) &&
         phase == 0x5000);
  phase = 0x7fff;
  seed = 0x1111;
  assert(sc88_lfo_phase_advance(1, 0, &phase, &seed, &target) &&
         phase == 0x8000);
  assert(seed == sc88_lfo_random_target(0x1111, 0x8000) && target == seed);
  assert(!sc88_lfo_phase_advance(0, 0, &phase, &seed, &target));
  assert(!sc88_lfo_phase_advance(0x28f7, 0, &phase, &seed, &target));

  /* The 129-point reader selects one point at or above increment 0x0200 and
     interpolates below it with the firmware's packed multiplier. */
  put16(bytes + SINE_TABLE, 0);
  put16(bytes + SINE_TABLE + 2, 0x0100);
  assert(sc88_lfo_table_sample(&rom, SINE_TABLE, 0x0100, 0x0200, &out) &&
         out == 0);
  assert(sc88_lfo_table_sample(&rom, SINE_TABLE, 0x0100, 0x01ff, &out) &&
         out == 128);
  assert(sc88_lfo_waveform(&rom, 0x00, 0x0100, 0x01ff, 0, 0, &out) &&
         out == 128);
  /* every unassigned dispatch entry returns the phase word itself */
  assert(sc88_lfo_waveform(&rom, 0x04, 0x1234, 0x0100, 0, 0, &out) &&
         out == 0x1234);
  assert(sc88_lfo_waveform(&rom, 0x1e, 0x1234, 0x0100, 0, 0, &out) &&
         out == 0x1234);
  assert(sc88_lfo_waveform(&rom, 0x0a, 0, 0x0100, 0, 4321, &out) &&
         out == 4321);
  assert(!sc88_lfo_waveform(&rom, 0x01, 0, 0x0100, 0, 0, &out));
  assert(!sc88_lfo_waveform(&rom, 0x20, 0, 0x0100, 0, 0, &out));

  /* The ramp. The accumulator starts at the increment, so the steps to the
     carry are ceil((65536 - increment) / increment) - three of them here. */
  assert(sc88_lfo_ramp_initialize(0x4000, 0x0010, &ramp) &&
         ramp.fade == 0 && ramp.delay_phase == 0x4000);
  assert(sc88_lfo_ramp_advance(&ramp, 0) && ramp.fade == 0);
  assert(sc88_lfo_ramp_advance(&ramp, 0) && ramp.fade == 0);
  /* the step whose delay addition carries is the one that begins the fade */
  assert(sc88_lfo_ramp_advance(&ramp, 0) && ramp.fade == 0x0010);
  /* and catch-up spends the missed periods on the fade once it has begun */
  assert(sc88_lfo_ramp_initialize(0x4000, 0x0010, &ramp));
  assert(sc88_lfo_ramp_advance(&ramp, 4) && ramp.fade == 0x0030);
  assert(sc88_lfo_ramp_initialize(UINT16_MAX, 0x0010, &ramp) &&
         ramp.fade == 0x0010);
  assert(sc88_lfo_ramp_initialize(0, 0x0010, &ramp) && ramp.fade == 0);
  assert(sc88_lfo_ramp_activate_immediate(&ramp) && ramp.fade == 1);
  /* and the fade saturates on carry rather than wrapping to silence */
  assert(sc88_lfo_ramp_initialize(UINT16_MAX, 0x8000, &ramp));
  assert(sc88_lfo_ramp_advance(&ramp, 4) && ramp.fade == UINT16_MAX);

  /* Preparation from a tone-common header and from a component. */
  put16(bytes + RATE_TABLE + 60 * 2, 0x0400);
  put16(bytes + DELAY_TABLE + 40 * 2, 0x0200);
  common[0x17] = 0x06;
  common[0x18] = 0;
  common[0x19] = 0x40;
  common[0x1a] = 60;
  common[0x1b] = 40;
  put16(common + 0x1c, 0x0080);
  assert(sc88_lfo_common_prepare(&rom, &tone, 64, 64, 64, 64, &lfo));
  assert(lfo.selector == 0x06 && lfo.phase == 0x4000);
  assert(lfo.base_increment == 0x0400);
  assert(lfo.ramp.delay_increment == 0x0200 &&
         lfo.ramp.fade_increment == 0x0080 && lfo.ramp.fade == 0);
  /* a negative delay byte gives no delay increment at all */
  common[0x1b] = (uint8_t)0xff;
  assert(sc88_lfo_common_prepare(&rom, &tone, 64, 64, 64, 64, &lfo) &&
         lfo.ramp.delay_increment == 0);

  component_bytes[0x07] = 0x02;
  component_bytes[0x09] = 0x80;
  put16(component_bytes + 0x0a, 0x0300);
  put16(component_bytes + 0x0c, UINT16_MAX);
  put16(component_bytes + 0x0e, 0x0040);
  assert(sc88_lfo_local_prepare(&rom, &component, &lfo));
  assert(lfo.selector == 0x02 && lfo.phase == 0x8000);
  /* the local path has no rate table: the field is the increment */
  assert(lfo.base_increment == 0x0300);
  assert(lfo.ramp.fade == 0x0040);

  /* One service: the ramp runs, the phase advances, one callback evaluates. */
  seed = 0;
  assert(sc88_lfo_advance(&rom, &lfo, 0, 0, &seed));
  assert(lfo.phase == (uint16_t)(0x8000 + 0x0300));
  assert(lfo.output == sc88_lfo_square(lfo.phase));
  /* a contribution that cancels the base stalls it: phase and output stand */
  phase = lfo.phase;
  out = lfo.output;
  assert(sc88_lfo_advance(&rom, &lfo, -0x0300, 0, &seed));
  assert(lfo.phase == phase && lfo.output == out);

  /* The frequency the recovered timer gives each increment. */
  assert(fabs(sc88_lfo_frequency(0) - 0.0) < 1e-12);
  assert(fabs(sc88_lfo_frequency(0x0034) - 0.099172) < 5e-7);
  assert(fabs(sc88_lfo_frequency(0x28f6) - 19.998458) < 5e-7);

  free(bytes);
  return 0;
}
