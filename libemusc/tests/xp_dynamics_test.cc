/* SPDX-License-Identifier: CC0-1.0 */
/* COMPRESSOR and LIMITER (JV-1080 insert types 9 and 10), run on the
 * effect alone and held to the machine's own steady levels.
 *
 * The stimulus is the one every take here used, reduced to a sine: key 60
 * at the peak this engine's tone-127 `Sine` reaches on the insert bus. The
 * expected numbers are the takes' (`closing/efx_sweep_09_compressor`,
 * `_10_limiter`, `efx_transfer/efx09_compressor`), as dynamics.cc quotes
 * them; the tolerances are the takes' own scatter plus the difference a
 * pure sine makes against the ROM wave.
 *
 * Needs the device's own control ROM and reports skipped without it.
 */
#include "engines/xp/devices/jv1080.h"
#include "engines/xp/dynamics.h"
#include "engines/xp/rom.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

using namespace EmuSC::Xp;

#define SKIP 77

static const double kRate = 32000.0;
static const double kKey60 = 261.6256;
static const float kReferencePeak = 0.04330f;

static std::vector<uint8_t> read_exact(const char *path, size_t expected)
{
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "cannot open %s\n", path);
    exit(1);
  }
  std::vector<uint8_t> bytes(expected);
  size_t got = fread(bytes.data(), 1, expected, f);
  int extra = fgetc(f);
  fclose(f);
  if (got != expected || extra != EOF) {
    fprintf(stderr, "%s is not %zu bytes\n", path, expected);
    exit(1);
  }
  return bytes;
}

/* One note: silence, then a sine of `amplitude` for 1.2 s. Returns the
   left output. */
static std::vector<float> run(const struct xp_rom *rom, bool limiter,
                              const uint8_t *p, float amplitude)
{
  struct xp_dynamics dy;
  memset(&dy, 0, sizeof dy);
  assert(dynamics_set(rom, &dy, limiter, p));
  const size_t lead = 3200, n = lead + (size_t)(1.2 * kRate);
  std::vector<float> in(n, 0.0f), outL(n), outR(n);
  for (size_t i = lead; i < n; ++i)
    in[i] = amplitude * (float)sin(2.0 * M_PI * kKey60 * (double)(i - lead) /
                                   kRate);
  dynamics_process(&dy, in.data(), in.data(), outL.data(), outR.data(), n);
  return outL;
}

/* Amplitude over whole periods centred on `t` seconds after the onset. */
static double amp(const std::vector<float> &y, double t, unsigned periods)
{
  const size_t lead = 3200;
  const double period = kRate / kKey60;
  size_t len = (size_t)(period * periods + 0.5);
  size_t c = lead + (size_t)(t * kRate);
  size_t a = c - len / 2;
  double s = 0.0;
  for (size_t i = a; i < a + len; ++i)
    s += (double)y[i] * y[i];
  return sqrt(2.0 * s / (double)len);
}

static double steady_db(const std::vector<float> &y)
{
  return 20.0 * log10(amp(y, 0.7, 100));
}

static void check_near(double got, double want, double tol, const char *what)
{
  printf("  %-44s %+8.2f  (machine %+8.2f)\n", what, got, want);
  assert(fabs(got - want) <= tol);
}

int main(void)
{
  const char *controlPath = getenv("JV1080_CONTROL_ROM");
  if (!controlPath || !*controlPath) {
    printf("JV1080_CONTROL_ROM unset - skipping\n");
    return SKIP;
  }
  std::vector<uint8_t> control =
    read_exact(controlPath, JV1080_PROFILE.romSize);
  struct xp_rom rom;
  assert(rom_init(&rom, control.data(), control.size()));

  /* The factory rows: Power Grand (COMPRESSOR) and Velo-Rez Clv (LIMITER). */
  const uint8_t comp[XP_COMPRESSOR_PARAMETERS] = { 100, 127, 64, 0, 15, 26, 85 };
  const uint8_t lim[XP_LIMITER_PARAMETERS] = { 84, 0, 3, 64, 2, 15, 15, 127 };

  /* Ranges, and a byte past one refused with the effect left as it was. */
  assert(dynamics_parameter_valid(&rom, false, 3u, 3u));
  assert(!dynamics_parameter_valid(&rom, false, 3u, 4u));
  assert(!dynamics_parameter_valid(&rom, true, 2u, 4u));
  assert(!dynamics_parameter_valid(&rom, true, 5u, 31u));
  assert(!dynamics_parameter_valid(&rom, false, 7u, 0u));
  assert(dynamics_parameter_valid(&rom, true, 7u, 127u));
  {
    struct xp_dynamics dy;
    memset(&dy, 0, sizeof dy);
    assert(dynamics_set(&rom, &dy, true, lim));
    struct xp_dynamics before = dy;
    uint8_t bad[XP_LIMITER_PARAMETERS];
    memcpy(bad, lim, sizeof bad);
    bad[2] = 4u;
    assert(!dynamics_set(&rom, &dy, true, bad));
    assert(memcmp(&before, &dy, sizeof dy) == 0);
  }

  /* The Threshold/Attack table is one exponential: 0.4724 dB a step. */
  {
    struct xp_dynamics a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    uint8_t p[XP_COMPRESSOR_PARAMETERS];
    memcpy(p, comp, sizeof p);
    p[0] = 0;
    assert(dynamics_set(&rom, &a, false, p));
    p[0] = 127;
    assert(dynamics_set(&rom, &b, false, p));
    assert(fabs(a.pre_gain - 131.0 / 8192.0) < 1e-7);
    assert(fabs(b.pre_gain - 8191.0 * 16.0 / 8192.0) < 1e-4);
    assert(fabs(20.0 * log10(b.pre_gain / a.pre_gain) / 127.0 - 0.4724) < 0.001);
  }

  printf("LIMITER, Threshold (dB against the unlimited level)\n");
  {
    const int thr[5] = { 0, 32, 64, 95, 127 };
    const double want[5] = { -37.38, -22.48, -7.39, 0.0, 0.0 };
    uint8_t p[XP_LIMITER_PARAMETERS];
    memcpy(p, lim, sizeof p);
    p[0] = 127;
    double top = steady_db(run(&rom, true, p, kReferencePeak));
    /* The unlimited return against the input, per channel: PostGain x4,
       Level 127, pan centre - the machine reads +9.40 dB. */
    check_near(top - 20.0 * log10(kReferencePeak), 9.40, 0.1,
               "unlimited, output against input");
    for (int i = 0; i < 5; ++i) {
      p[0] = (uint8_t)thr[i];
      char what[64];
      snprintf(what, sizeof what, "Threshold %d", thr[i]);
      check_near(steady_db(run(&rom, true, p, kReferencePeak)) - top, want[i],
                 0.25, what);
    }
    /* The factory Threshold, 84, is past the reference note's knee. */
    p[0] = 84;
    check_near(steady_db(run(&rom, true, p, kReferencePeak)) - top, 0.0, 0.05,
               "Threshold 84 (factory)");
    /* PostGain sits after the detector: its top step, 6 dB louder than
       the factory's, still does not reach the knee 2 dB away. */
    double last = 0.0;
    for (int g = 0; g < 4; ++g) {
      p[4] = (uint8_t)g;
      double v = steady_db(run(&rom, true, p, kReferencePeak));
      if (g)
        check_near(v - last, 6.02, 0.05, "PostGain step");
      last = v;
    }
  }

  printf("COMPRESSOR, factory, input stepped (dB against the top step)\n");
  {
    const double in[5] = { -28.94, -16.90, -9.86, -4.86, 0.0 };
    const double want[5] = { -5.42, -1.33, -0.91, -0.51, 0.0 };
    double got[5];
    for (int i = 0; i < 5; ++i)
      got[i] = steady_db(run(&rom, false, comp,
                             kReferencePeak * (float)pow(10.0, in[i] / 20.0)));
    for (int i = 0; i < 5; ++i) {
      char what[64];
      snprintf(what, sizeof what, "input %+.2f dB", in[i]);
      check_near(got[i] - got[4], want[i], 0.3, what);
    }
    check_near((in[4] - in[0]) - (got[4] - got[0]), 23.52, 0.3, "compression");
    /* The return against the input below threshold: Attack 0 is a pre-gain
       of -35.92 dB, and the machine reads -37.06 dB in all. */
    uint8_t p[XP_COMPRESSOR_PARAMETERS];
    memcpy(p, comp, sizeof p);
    p[0] = 0;
    check_near(steady_db(run(&rom, false, p, kReferencePeak)) -
               20.0 * log10(kReferencePeak), -37.06, 0.1,
               "Attack 0, output against input");
  }

  printf("COMPRESSOR, Attack at full input (dB against Attack 100)\n");
  {
    const int att[5] = { 0, 32, 64, 95, 127 };
    const double want[5] = { -23.55, -8.41, -1.33, -0.25, 1.82 };
    double ref = steady_db(run(&rom, false, comp, kReferencePeak));
    uint8_t p[XP_COMPRESSOR_PARAMETERS];
    memcpy(p, comp, sizeof p);
    for (int i = 0; i < 5; ++i) {
      p[0] = (uint8_t)att[i];
      char what[64];
      snprintf(what, sizeof what, "Attack %d", att[i]);
      check_near(steady_db(run(&rom, false, p, kReferencePeak)) - ref, want[i],
                 0.3, what);
    }
  }

  printf("Onset (dB over the settled level)\n");
  {
    std::vector<float> y = run(&rom, false, comp, kReferencePeak);
    double ss = steady_db(y);
    check_near(20.0 * log10(amp(y, 0.004, 1)) - ss, 3.9, 1.0, "COMPRESSOR at 4 ms");
    check_near(20.0 * log10(amp(y, 0.010, 1)) - ss, 1.2, 0.5, "COMPRESSOR at 10 ms");
    check_near(20.0 * log10(amp(y, 0.030, 1)) - ss, 0.0, 0.3, "COMPRESSOR at 30 ms");
    uint8_t p[XP_LIMITER_PARAMETERS];
    memcpy(p, lim, sizeof p);
    p[0] = 0;
    y = run(&rom, true, p, kReferencePeak);
    ss = steady_db(y);
    check_near(20.0 * log10(amp(y, 0.004, 1)) - ss, 2.8, 1.0, "LIMITER Threshold 0 at 4 ms");
    check_near(20.0 * log10(amp(y, 0.010, 1)) - ss, 1.3, 0.5, "LIMITER Threshold 0 at 10 ms");
    check_near(20.0 * log10(amp(y, 0.030, 1)) - ss, 0.2, 0.3, "LIMITER Threshold 0 at 30 ms");
  }

  printf("ok\n");
  return 0;
}
