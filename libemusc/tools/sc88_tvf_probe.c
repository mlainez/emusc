/* SPDX-License-Identifier: LGPL-3.0-or-later */

/* Print what the filter actually does to one note, period by period.
 *
 * The onset-aligned spectral centroid of the hardware demo recordings
 * swings by hundreds of hertz in the first 30 ms of every note while ours
 * barely moves, so the question is whether the cutoff word moves at all
 * and, if it does, whether the clamp in `sc88_tvf_update_frequency` eats
 * the movement. Both are internal values that no measurement of the
 * rendered audio can separate.
 *
 *   sc88_tvf_probe --control control.bin --program 56 [--variation 0]
 *                  [--key 60] [--velocity 100] [--periods 250]
 */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sc88_rom.h"
#include "sc88_tvf.h"

#define SC88_TVF_LIMIT_TABLE 0x78802u
#define SC88_TVF_NATIVE_RATE 32000.0

static uint8_t *read_file(const char *path, size_t *size)
{
  FILE *f = fopen(path, "rb");
  uint8_t *data;
  long end;
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) != 0 || (end = ftell(f)) < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  data = malloc((size_t)end);
  if (!data || fread(data, 1, (size_t)end, f) != (size_t)end) {
    free(data);
    fclose(f);
    return NULL;
  }
  fclose(f);
  *size = (size_t)end;
  return data;
}

static double cutoff_hz(uint32_t word)
{
  double f1 = word / 262144.0;
  double sine;
  if (f1 > 1.998)
    f1 = 1.998;
  sine = f1 * 0.5;
  if (sine > 1.0)
    sine = 1.0;
  return asin(sine) * SC88_TVF_NATIVE_RATE / 3.14159265358979323846;
}

int main(int argc, char **argv)
{
  const char *control_path = NULL;
  unsigned program = 0, variation = 0, key = 60, velocity = 100;
  unsigned periods = 250;
  size_t control_size = 0;
  uint8_t *control;
  struct sc88_rom rom;
  struct sc88_tone tone;
  uint32_t tone_offset;
  char name[13];
  unsigned i;
  int a;

  for (a = 1; a < argc; ++a) {
    const char *arg = argv[a];
    if (!strcmp(arg, "--control") && a + 1 < argc)
      control_path = argv[++a];
    else if (!strcmp(arg, "--program") && a + 1 < argc)
      program = (unsigned)strtoul(argv[++a], NULL, 0);
    else if (!strcmp(arg, "--variation") && a + 1 < argc)
      variation = (unsigned)strtoul(argv[++a], NULL, 0);
    else if (!strcmp(arg, "--key") && a + 1 < argc)
      key = (unsigned)strtoul(argv[++a], NULL, 0);
    else if (!strcmp(arg, "--velocity") && a + 1 < argc)
      velocity = (unsigned)strtoul(argv[++a], NULL, 0);
    else if (!strcmp(arg, "--periods") && a + 1 < argc)
      periods = (unsigned)strtoul(argv[++a], NULL, 0);
    else {
      fprintf(stderr, "unknown argument: %s\n", arg);
      return 2;
    }
  }
  if (!control_path) {
    fprintf(stderr, "usage: sc88_tvf_probe --control FILE --program N\n");
    return 2;
  }
  control = read_file(control_path, &control_size);
  if (!control || !sc88_rom_init(&rom, control, control_size)) {
    fprintf(stderr, "cannot read %s\n", control_path);
    return 1;
  }
  if (!sc88_rom_select_melodic(&rom, (uint8_t)variation, (uint8_t)program,
                               &tone_offset) ||
      !sc88_rom_open_tone(&rom, tone_offset, &tone)) {
    fprintf(stderr, "no tone for variation %u program %u\n",
            variation, program);
    return 1;
  }
  sc88_rom_tone_name(&tone, name);
  printf("variation %u program %u  \"%s\"  %u component(s)"
         "  key %u velocity %u\n",
         variation, program, name, tone.component_count, key, velocity);

  for (i = 0; i < tone.component_count; ++i) {
    struct sc88_component component;
    struct sc88_tvf_registers regs;
    struct sc88_tvf_envelope env;
    struct sc88_tvf_controls controls;
    int16_t key_modulation = 0;
    uint16_t limit;
    unsigned p;
    double lowest = 1e9, highest = -1e9;

    controls.part_cutoff = 64;
    controls.secondary_cutoff = 64;
    controls.part_resonance = 64;
    controls.secondary_resonance = 64;

    if (!sc88_rom_open_component(&rom, &tone, i, &component) ||
        !sc88_tvf_key_modulation(&rom, &tone, &component, (uint8_t)key,
                                 &key_modulation) ||
        !sc88_tvf_envelope_prepare(&rom, &tone, &component, (uint8_t)key,
                                   (uint8_t)velocity, false, &env) ||
        !sc88_tvf_prepare_registers(&rom, &component, key_modulation,
                                    &controls, &regs) ||
        !sc88_tvf_update_frequency(&rom, env.current, &regs)) {
      printf("  component %u: prepare failed\n", i);
      continue;
    }
    limit = (uint16_t)((rom.bytes[SC88_TVF_LIMIT_TABLE +
                                  regs.resonance_index * 2] << 8) |
                       rom.bytes[SC88_TVF_LIMIT_TABLE +
                                 regs.resonance_index * 2 + 1]);
    printf("\n  component %u: cutoff_index %u resonance_index %u"
           " base_value %u limit %u fixed %d\n", i, regs.cutoff_index,
           regs.resonance_index, regs.base_value, limit, regs.fixed_tuple);
    printf("    envelope depth %u  targets %d %d %d %d"
           "  increments %u %u %u %u\n", env.depth,
           env.targets[0], env.targets[1], env.targets[2], env.targets[3],
           env.increments[0], env.increments[1], env.increments[2],
           env.increments[3]);
    printf("    %6s %8s %8s %8s %9s %s\n",
           "ms", "env", "combined", "clamped", "cutoff Hz", "stage");
    for (p = 0; p < periods; ++p) {
      double hz;
      bool clamped;
      sc88_tvf_update_frequency(&rom, env.current, &regs);
      clamped = (uint16_t)(regs.base_value + (uint16_t)env.current) > limit;
      hz = cutoff_hz(regs.frequency_target);
      if (hz < lowest)
        lowest = hz;
      if (hz > highest)
        highest = hz;
      if (p < 16 || p % 25 == 0)
        printf("    %6.1f %8d %8u %8s %9.0f %u\n", p * 8.0008,
               env.current, regs.combined, clamped ? "yes" : "", hz,
               env.stage);
      if (!sc88_tvf_envelope_advance(&env, 1))
        break;
    }
    printf("    swing over %u periods (%.0f ms): %.0f to %.0f Hz"
           " = %.0f Hz\n", periods, periods * 8.0008, lowest, highest,
           highest - lowest);
  }
  free(control);
  return 0;
}
