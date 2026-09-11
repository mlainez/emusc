/* Where does a single note's amplitude actually come from? */
#include "sc88_device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned g_calls, g_periods;
static void service(void *user, unsigned elapsed)
{
  (void)user; ++g_calls; g_periods += elapsed;
}

static uint8_t *slurp(const char *p, size_t *n)
{
  FILE *f = fopen(p, "rb"); uint8_t *b; long L;
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); L = ftell(f); rewind(f);
  b = malloc((size_t)L); if (fread(b, 1, (size_t)L, f) != (size_t)L) { free(b); fclose(f); return NULL; }
  fclose(f); *n = (size_t)L; return b;
}

int main(int argc, char **argv)
{
  struct sc88_device d;
  const uint8_t *cv[4]; uint8_t *c, *w[4]; size_t cs, ws[4];
  float block[512];
  unsigned i, sec, prog = argc > 6 ? (unsigned)atoi(argv[6]) : 0;
  c = slurp(argv[1], &cs);
  for (i = 0; i < 4; ++i) { w[i] = slurp(argv[2 + i], &ws[i]); cv[i] = w[i]; }
  if (!c || !w[3]) { fprintf(stderr, "roms?\n"); return 1; }
  if (!sc88_device_init_decoded(&d, c, cs, cv, ws, 44100.0,
                                SC88_WRAP_FULL_CARRY)) { fprintf(stderr, "init\n"); return 1; }
  sc88_engine_set_control_service(&d.engine, service, NULL);
  sc88_device_midi(&d, 0, 0xc0, (uint8_t)prog, 0);
  sc88_device_midi(&d, 0, 0xb0, 7, 127);
  sc88_device_midi(&d, 0, 0xb0, 11, 127);
  sc88_device_midi(&d, 0, 0x90, 60, 100);
  printf("program %u\n", prog);
  for (sec = 0; sec < 12; ++sec) {
    unsigned f;
    for (f = 0; f < 44100 / 256 / 4; ++f)
      sc88_device_render(&d, block, 256);
    for (i = 0; i < SC88_ENGINE_SLOT_COUNT; ++i) {
      const struct sc88_engine_slot *s = d.engine.slots + i;
      if (!s->allocated) continue;
      if (sec == 0) {
        printf("    component at 0x%05x, static_att %u\n",
               s->component.rom_component_offset,
               s->component.static_attenuation);
      }
      if (sec == 0)
        printf("    targets_q17 %u %u %u %u   increments %u %u %u %u\n",
               s->component.envelope.targets_q17[0],
               s->component.envelope.targets_q17[1],
               s->component.envelope.targets_q17[2],
               s->component.envelope.targets_q17[3],
               s->component.envelope.increments[0],
               s->component.envelope.increments[1],
               s->component.envelope.increments[2],
               s->component.envelope.increments[3]);
      printf("  t=%4.2f slot %2u  tva stage %u active %d current %6d "
             "target?%d  static_gain_q17 %8u  static_att %5u  step %.6f  ended %d\n",
             sec * 0.25, i, s->component.envelope.stage,
             (int)s->component.envelope.active, (int)s->component.envelope.current_q17,
             0, s->component.static_gain_q17, s->component.static_attenuation,
             s->component.oscillator.step, (int)s->component.oscillator.ended);
      break;
    }
    printf("        services this quarter-second: %u calls, %u periods\n",
           g_calls, g_periods);
    g_calls = g_periods = 0;
  }
  sc88_device_destroy(&d);
  return 0;
}
