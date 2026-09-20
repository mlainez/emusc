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

#include "engines/xp/rom.h"
#include "engines/xp/tvf.h"
#include "engines/xp/tva.h"
#include "engines/xp/wave.h"

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

static unsigned comp_bytes_at(const struct sc88_component *c, unsigned off)
{
  return c && c->bytes ? c->bytes[off] : 0u;
}

static double cutoff_hz(uint32_t word)
{
  return sc88_tvf_word_to_hz(word);
}

int main(int argc, char **argv)
{
  const char *control_path = NULL;
  unsigned program = 0, variation = 0, key = 60, velocity = 100;
  unsigned periods = 250;
  bool survey = false;
  bool keyfollow = false;
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
    if (!strcmp(arg, "--survey"))
      survey = true;
    else if (!strcmp(arg, "--keyfollow"))
      keyfollow = true;
    else if (!strcmp(arg, "--control") && a + 1 < argc)
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
  if (keyfollow) {
    /* The key-follow factor every component scales the played key by, and
       the key offset added after it.  A component at 0x4000 tracks the
       keyboard one semitone per key; anything else scales a stimulus error
       along with everything else, so a reference recorded twelve keys away
       from the one we play comes back as six semitones at 0x2000 and never
       lands on an octave.  No octave test can see that, which is why the
       factor has to be listed rather than inferred from the audio. */
    unsigned v, pr;
    printf("variation\tprogram\tcomponent\tname\tfactor\tkeyfollow"
           "\tkey_offset\n");
    for (v = 0; v <= 36; ++v) {
      for (pr = 0; pr < 128; ++pr) {
        uint32_t offset;
        struct sc88_tone t;
        unsigned c;
        char n[13];
        if (!sc88_rom_select_melodic(&rom, SC88_TONE_MAP_SC88, (uint8_t)v,
                                     (uint8_t)pr, &offset) ||
            !sc88_rom_open_tone(&rom, offset, &t))
          continue;
        sc88_rom_tone_name(&t, n);
        for (c = 0; c < t.component_count; ++c) {
          struct sc88_component comp;
          int16_t factor;
          if (!sc88_rom_open_component(&rom, &t, c, &comp))
            continue;
          factor = (int16_t)((comp.bytes[0x14] << 8) | comp.bytes[0x15]);
          printf("%u\t%u\t%u\t%s\t%d\t%.5f\t%d\n", v, pr, c, n,
                 factor, factor / 16384.0, (int)(int8_t)comp.bytes[0x16]);
        }
      }
    }
    free(control);
    return 0;
  }
  if (survey) {
    /* Every melodic tone of both variation maps, so the spread of the
       per-tone cutoff parameter is a fact about the ROM rather than about
       whichever eighteen programs were sampled. Key modulation is printed
       at three keys because it moves the base value, not the index, and
       the two have to be separated before the filter is blamed or
       cleared. */
    unsigned v, pr;
    printf("variation\tprogram\tcomponent\tname\tcutoff_index"
           "\tresonance_index\tbase_value\tbase_unshifted\tkeymod36"
           "\tkeymod60\tkeymod84\tmode\tenv_depth\tt0\tt1\tt2\tt3"
           "\tinc0\tinc1\tlfo1pitch\tlfo2pitch"
           "\tc44\tc45\tc46\tc47\tc74\tc75\tc76\tc77"
           "\tshare_common\tshare_local\tlfo2_inc\tlfo2_hz\tcommon_idx\tcommon_hz\n");
    for (v = 0; v <= 36; ++v) {
      for (pr = 0; pr < 128; ++pr) {
        uint32_t offset;
        struct sc88_tone t;
        unsigned c;
        char n[13];
        if (!sc88_rom_select_melodic(&rom, SC88_TONE_MAP_SC88, (uint8_t)v,
                                     (uint8_t)pr, &offset) ||
            !sc88_rom_open_tone(&rom, offset, &t))
          continue;
        sc88_rom_tone_name(&t, n);
        for (c = 0; c < t.component_count; ++c) {
          struct sc88_component comp;
          struct sc88_tvf_registers r;
          struct sc88_tvf_envelope env;
          struct sc88_tvf_controls ctl;
          int16_t k36 = 0, k60 = 0, k84 = 0;
          ctl.part_cutoff = 64;
          ctl.secondary_cutoff = 64;
          ctl.part_resonance = 64;
          ctl.secondary_resonance = 64;
          ctl.matrix_cutoff = 0;
          if (!sc88_rom_open_component(&rom, &t, c, &comp))
            continue;
          (void)sc88_tvf_key_modulation(&rom, &t, &comp, 36, &k36);
          (void)sc88_tvf_key_modulation(&rom, &t, &comp, 60, &k60);
          (void)sc88_tvf_key_modulation(&rom, &t, &comp, 84, &k84);
          if (!sc88_tvf_prepare_registers(&rom, &comp, k60, &ctl, &r) ||
              !sc88_tvf_envelope_prepare(&rom, &t, &comp, 60, 100, false,
                                         &env))
            continue;
          printf("%u\t%u\t%u\t%s\t%u\t%u\t%u\t%u\t%d\t%d\t%d\t%d"
                 "\t%u\t%d\t%d\t%d\t%d\t%u\t%u\t%d\t%d"
                 "\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%u\t%.3f\t%u\t%.3f\n",
                 v, pr, c, n, r.cutoff_index, r.resonance_index,
                 r.base_value, r.base_unshifted, k36, k60, k84,
                 (int)(int8_t)comp.bytes[0x3e], env.depth,
                 env.targets[0], env.targets[1], env.targets[2],
                 env.targets[3], env.increments[0], env.increments[1],
                 (int)(int16_t)((comp.bytes[0x16] << 8) | comp.bytes[0x17]),
                 (int)(int16_t)((comp.bytes[0x18] << 8) | comp.bytes[0x19]),
                 (int)(int8_t)comp.bytes[0x44], (int)(int8_t)comp.bytes[0x45],
                 (int)(int8_t)comp.bytes[0x46], (int)(int8_t)comp.bytes[0x47],
                 (int)(int8_t)comp.bytes[0x74], (int)(int8_t)comp.bytes[0x75],
                 (int)(int8_t)comp.bytes[0x76], (int)(int8_t)comp.bytes[0x77],
                 (int)t.common[0x18], (int)comp.bytes[0x08],
                 (unsigned)((comp.bytes[0x0a] << 8) | comp.bytes[0x0b]),
                 ((comp.bytes[0x0a] << 8) | comp.bytes[0x0b]) *
                   124.987501249875 / 65536.0,
                 (unsigned)t.common[0x1a],
                 (double)((rom.bytes[0x29da + t.common[0x1a] * 2] << 8) |
                          rom.bytes[0x29da + t.common[0x1a] * 2 + 1]) *
                   124.987501249875 / 65536.0);
        }
      }
    }
    free(control);
    return 0;
  }
  if (!sc88_rom_select_melodic(&rom, SC88_TONE_MAP_SC88,
                               (uint8_t)variation, (uint8_t)program,
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

  /* Which sample each key actually reaches. A program whose partials sit
     far from the hardware's may be selecting the wrong zone rather than
     filtering wrongly, and the two are told apart here: a zone that does
     not change across the key range, or whose root key is far from the
     key being played, is a selection fault. */
  {
    unsigned c;
    static const unsigned probe_keys[] = {24, 36, 48, 60, 72, 84, 96};
    for (c = 0; c < tone.component_count; ++c) {
      struct sc88_component comp;
      unsigned k;
      if (!sc88_rom_open_component(&rom, &tone, c, &comp))
        continue;
      printf("\n  component %u zones by key:\n", c);
      printf("    %4s %9s %6s %9s %9s %9s %6s %6s %5s %7s %7s %5s\n", "key",
             "boundary", "root", "address_a", "loop_b", "end_c", "off16", "bank", "atten",
             "basecor", "altcor", "ctrl");
      for (k = 0; k < sizeof probe_keys / sizeof *probe_keys; ++k) {
        struct sc88_zone_selection zone;
        if (!sc88_rom_select_zone(&rom, &comp, (uint8_t)probe_keys[k],
                                  &zone)) {
          printf("    %4u  (no zone)\n", probe_keys[k]);
          continue;
        }
        printf("    %4u %9u %6u %9lx %9lx %9lx %6u %6u %5u %7d %7d %5u\n",
               probe_keys[k], zone.boundary, zone.descriptor.root_key,
               (unsigned long)zone.descriptor.address_a,
               (unsigned long)zone.descriptor.address_b,
               (unsigned long)zone.descriptor.address_c,
               (unsigned)zone.descriptor.start_offset,
               zone.descriptor.bank_select, zone.static_attenuation,
               zone.descriptor.base_pitch_correction,
               zone.descriptor.alternate_pitch_correction,
               zone.descriptor.control);
      }
    }
    printf("\n");
  }

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
    controls.matrix_cutoff = 0;

    if (getenv("SC88_DUMP_BYTES")) {
      struct sc88_component cdump;
      if (sc88_rom_open_component(&rom, &tone, i, &cdump) && cdump.bytes) {
        unsigned b;
        printf("BYTES\t%u\t%u", (unsigned)program, i);
        for (b = 0; b < SC88_COMPONENT_SIZE; ++b)
          printf("\t%u", (unsigned)cdump.bytes[b]);
        printf("\n");
      }
    }
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

    /* The amplitude envelope's own stage plateaus, so a rendered note can
       be checked against the levels the ROM asks for without needing any
       hardware: if the render does not settle where these say, the fault
       is arithmetic rather than data. */
    {
      struct sc88_tva_envelope env;
      struct sc88_tva_controls tva = {64, 64, 64, 64};
      if (sc88_tva_envelope_prepare(&rom, &tone, &component,
                                    (uint8_t)key, (uint8_t)velocity,
                                    &tva, &env)) {
        unsigned st;
        printf("    TVA stages: %-6s %10s %9s %9s %8s %8s\n", "stage",
               "atten", "gain q17", "dB", "dwell ms", "curve");
        for (st = 0; st < 4; ++st) {
          double db = env.targets_q17[st] > 0
            ? 20.0 * log10((double)env.targets_q17[st] / 131072.0)
            : -999.0;
          double ms = env.increments[st]
            ? 65536.0 / env.increments[st] * 8.0008 : 0.0;
          printf("                %-6u %10u %9u %9.1f %8.0f   0x%04x\n",
                 st, env.target_attenuations[st], env.targets_q17[st],
                 db, ms, env.curve_words[st]);
        }
        printf("                starts at stage %u\n", env.stage);
      }
    }

    /* The TVA release counter. `sc88_tva_release_advance` subtracts the
       increment from 0xffff once per control period, so 0xffff/increment
       is the whole release in periods and the headroom falls by the
       increment each one - the composed amplitude is a new target every
       period, and the chip's own ramp to it is a separate 0.75 ms glide.
       Key scaling moves the increment, so it is printed for the key asked
       for, not a stored constant. */
    {
      struct sc88_tva_release rel;
      if (sc88_tva_release_prepare(&rom, &tone, &component, (uint8_t)key,
                                   &rel)) {
        double periods = rel.increment ? 65535.0 / rel.increment : 0.0;
        double ms = rel.increment ? periods * 8.0008 : 0.0;
        printf("    TVA release: rate byte 0x%02x  key-scale byte 0x%02x"
               "  increment %u  %.2f periods  %.1f ms  step %.1f dB\n",
               comp_bytes_at(&component, 0x84),
               comp_bytes_at(&component, 0x8f),
               rel.increment, periods, ms,
               rel.increment / 4096.0 * 5.26);
      }
    }

    /* The static balance between a tone's components. A two-component
       tone whose layers sit at different levels moves timbrally as they
       cross over; if they are rendered at the wrong relative level the
       movement is lost, which is what `M-048` points at. */
    {
      struct sc88_zone_selection zone;
      struct sc88_tva_levels levels = {127, 127, 127, 127};
      uint16_t static_attenuation = 0;
      uint32_t gain = 0;
      if (sc88_rom_select_zone(&rom, &component, (uint8_t)key, &zone) &&
          sc88_tva_static_gain_q17(&rom, &tone, &component, &zone,
                                   (uint8_t)key, (uint8_t)velocity,
                                   &levels, SC88_TVA_NO_DRUM_LEVEL,
                                   &static_attenuation, &gain)) {
        printf("    pitch env: depth %6d  rates %3u %3u %3u %3u  release %3u\n",
           (int)(int16_t)((comp_bytes_at(&component, 0x1a) << 8) |
                          comp_bytes_at(&component, 0x1b)),
           comp_bytes_at(&component, 0x2a), comp_bytes_at(&component, 0x2b),
           comp_bytes_at(&component, 0x2c), comp_bytes_at(&component, 0x2d),
           comp_bytes_at(&component, 0x2e));
    printf("    static level: attenuation %u  gain q17 %u  %.1f dB\n",
               static_attenuation, gain,
               gain > 0 ? 20.0 * log10((double)gain / 131072.0) : -999.0);
      }
    }
  }
  free(control);
  return 0;
}
