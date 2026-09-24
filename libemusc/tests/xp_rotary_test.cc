/* SPDX-License-Identifier: CC0-1.0 */
/* ROTARY (insert type 8) on its own, without the engine's dispatch.
 *
 * What this pins: the updater's binding as the ROM hands it over (speed,
 * accel and level words, the slot's two crossover filters), the speed
 * decision's two thresholds, the rotor frequency law, the exponential
 * speed ramp, and where each fixed tap lands and on which side. The fitted
 * gains and depths are not asserted to their digits here - they are
 * measurements and are expected to move when a better take arrives - only
 * the structure they sit in.
 *
 * Needs the device's own control ROM and reports skipped without it.
 */
#include "engines/xp/devices/jv1080.h"
#include "engines/xp/efx.h"
#include "engines/xp/rom.h"
#include "engines/xp/rotary.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
#include <vector>

using namespace EmuSC::Xp;

#define SKIP 77

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

static bool near(double a, double b, double tol)
{
  return fabs(a - b) <= tol;
}

/* |H(f)| of the slot's biquad and one-pole, from the built coefficients. */
static double biquad_db(const struct xp_rotary *rt, double f)
{
  double w = 2.0 * M_PI * f / 32000.0;
  double nr = rt->hp_b0 + rt->hp_b1 * cos(w) + rt->hp_b2 * cos(2 * w);
  double ni = -rt->hp_b1 * sin(w) - rt->hp_b2 * sin(2 * w);
  double dr = 1.0 - rt->hp_a1 * cos(w) - rt->hp_a2 * cos(2 * w);
  double di = rt->hp_a1 * sin(w) + rt->hp_a2 * sin(2 * w);
  return 10.0 * log10((nr * nr + ni * ni) / (dr * dr + di * di));
}

static double onepole_db(const struct xp_rotary *rt, double f)
{
  double w = 2.0 * M_PI * f / 32000.0;
  double nr = rt->lp_b0 + rt->lp_b1 * cos(w);
  double ni = -rt->lp_b1 * sin(w);
  double dr = 1.0 - rt->lp_a1 * cos(w);
  double di = rt->lp_a1 * sin(w);
  return 10.0 * log10((nr * nr + ni * ni) / (dr * dr + di * di));
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

  /* The factory row, stored order: HiSlow LowSlow HiFast LowFast Speed
     HiAccl LowAccl HiLvl LowLvl Separation Level. */
  const uint8_t factory[XP_ROTARY_PARAMETERS] = {
    10, 1, 115, 118, 0, 6, 9, 96, 127, 99, 113
  };
  std::unique_ptr<struct xp_rotary> owned(new struct xp_rotary());
  struct xp_rotary *rt = owned.get();
  memset(rt, 0, sizeof *rt);
  assert(rotary_set(&rom, rt, factory));

  /* The binding, word for word: 0x038C2E[10, 1, 115, 118] = 18, 3, 216,
     226; 0x03EEE8[6, 9] = 7, 10; 0x03856C[96, 127, 113] = 5641, 8191,
     6986. */
  assert(rt->slow_word[0] == 18 && rt->slow_word[1] == 3);
  assert(rt->fast_word[0] == 216 && rt->fast_word[1] == 226);
  assert(!rt->fast && rt->target[0] == 18.0f && rt->target[1] == 3.0f);
  assert(rt->speed[0] == 18.0f && rt->speed[1] == 3.0f);
  assert(near(rt->ramp[1] / rt->ramp[0], 10.0 / 7.0, 1e-6));
  assert(rt->hi_gain == (float)((5641 >> 1) / 8192.0));
  assert(rt->lo_gain == (float)((8191 >> 1) / 8192.0));
  assert(rt->level == (float)((6986 >> 4) / 512.0));

  /* The crossover, CRAM 13..17 and 26..28 of slot 13. The highpass has an
     exact zero at DC (b0 + b1 + b2 is zero in the raw words) and its
     resonance at 2 kHz; the one-pole passes DC to one LSB and is -3 dB at
     its 2 kHz corner. */
  assert(near(rt->hp_b0, 0.926514, 1e-5) && near(rt->hp_b1, -1.853027, 1e-5) &&
         near(rt->hp_b2, 0.926514, 1e-5));
  assert(near(rt->hp_a1, 1.779541, 1e-5) && near(rt->hp_a2, -0.926270, 1e-5));
  assert(rt->hp_b0 + rt->hp_b1 + rt->hp_b2 == 0.0f);
  assert(near(rt->lp_b0, 0.165894, 1e-5) && rt->lp_b0 == rt->lp_b1 &&
         near(rt->lp_a1, 0.668213, 1e-5));
  assert(near(rt->lp_b0 + rt->lp_b1 + rt->lp_a1, 1.0, 1.0 / 8192.0));
  assert(near(biquad_db(rt, 2000.0), 13.98, 0.05));
  assert(biquad_db(rt, 262.0) < -34.0 && biquad_db(rt, 12000.0) > -0.2);
  assert(near(onepole_db(rt, 2000.0), -3.0, 0.1));

  /* The rotor law: 328 is 10 Hz, 2 is the manual's bottom step. */
  assert(near(rotary_speed_hz(328.0), 10.0, 0.01));
  assert(near(rotary_speed_hz(3.0), 0.0916, 0.0001));

  /* The two thresholds: the updater's > 64 and the realtime handler's
     > 63, over the clamped sum of the control offset and the Speed byte. */
  assert(!rotary_speed_fast(0, 0) && rotary_speed_fast(1, 0));
  assert(!rotary_speed_fast(0, 64) && rotary_speed_fast(0, 65));
  assert(rotary_control_fast(0, 64) && !rotary_control_fast(0, 63));
  assert(!rotary_speed_fast(1, -63) && rotary_speed_fast(1, -62));

  /* The doppler waveform: longest at phi = 0, zero half a turn later. */
  for (unsigned r = 0; r < 2u; ++r) {
    assert(rotary_doppler(r, 0.0f) > 20.0f && rotary_doppler(r, 0.0f) < 45.0f);
    assert(rotary_doppler(r, (float)M_PI) < 1e-6f);
    assert(rotary_doppler(r, 1.0f) < rotary_doppler(r, 0.5f));
  }

  /* A byte past its range is refused and leaves the effect as it was. */
  {
    std::unique_ptr<struct xp_rotary> before(new struct xp_rotary(*rt));
    uint8_t bad[XP_ROTARY_PARAMETERS];
    memcpy(bad, factory, sizeof bad);
    bad[0] = 126;
    assert(!rotary_set(&rom, rt, bad));
    assert(memcmp(before.get(), rt, sizeof *rt) == 0);
    assert(!rotary_parameter_valid(4, 2) && rotary_parameter_valid(6, 15) &&
           !rotary_parameter_valid(6, 16));
  }

  /* WHERE THE FIXED TAPS LAND. An impulse through the factory effect: the
     low line's fixed taps reach the left at 328 samples and the right at
     1188, the high line's at 123 and 901, each side only its own (plus the
     3.53-sample latency). With the high level at zero only the drum is
     left, and its shared doppler tap is the same on both sides. */
  {
    uint8_t lowOnly[XP_ROTARY_PARAMETERS];
    memcpy(lowOnly, factory, sizeof lowOnly);
    lowOnly[7] = 0;
    std::unique_ptr<struct xp_rotary> d(new struct xp_rotary());
    memset(d.get(), 0, sizeof *d);
    assert(rotary_set(&rom, d.get(), lowOnly));
    const size_t n = 1400;
    std::vector<float> in(n, 0.0f), L(n), R(n);
    in[0] = 1.0f;
    rotary_process(d.get(), in.data(), in.data(), L.data(), R.data(), n);
    auto energy = [](const std::vector<float> &v, size_t a, size_t b) {
      double e = 0.0;
      for (size_t i = a; i < b; ++i)
        e += (double)v[i] * v[i];
      return e;
    };
    auto peak = [](const std::vector<float> &v, size_t a, size_t b) {
      size_t at = a;
      for (size_t i = a; i < b; ++i)
        if (fabsf(v[i]) > fabsf(v[at]))
          at = i;
      return at;
    };
    size_t pl = peak(L, 300, 400), pr = peak(R, 1100, 1300);
    assert(pl >= 331 && pl <= 336 && pr >= 1191 && pr <= 1196);
    assert(energy(L, 1100, 1300) < 1e-9 * energy(L, 328, 360));
    assert(energy(R, 300, 400) < 1e-9 * energy(R, 1188, 1220));
    /* the shared doppler tap: identical on both sides */
    assert(energy(L, 0, 60) > 1e-4);
    for (size_t i = 0; i < 60; ++i)
      assert(L[i] == R[i]);

    uint8_t highOnly[XP_ROTARY_PARAMETERS];
    memcpy(highOnly, factory, sizeof highOnly);
    highOnly[8] = 0;
    std::unique_ptr<struct xp_rotary> h(new struct xp_rotary());
    memset(h.get(), 0, sizeof *h);
    assert(rotary_set(&rom, h.get(), highOnly));
    rotary_process(h.get(), in.data(), in.data(), L.data(), R.data(), n);
    pl = peak(L, 100, 400);
    pr = peak(R, 800, 1000);
    assert(pl >= 126 && pl <= 132 && pr >= 904 && pr <= 910);
    assert(energy(L, 895, 940) < 1e-6 * energy(L, 126, 160));
    assert(energy(R, 120, 160) < 1e-3 * energy(R, 904, 940));
  }

  /* THE RAMP. Speed 1 on a built effect retargets both rotors to their
     fast words and the registers close the gap exponentially: after the
     measured 0.861 s at accel word 10 the drum has covered 1 - 1/e of it,
     the horn (word 7) 1 - e^-0.7. */
  {
    uint8_t fast[XP_ROTARY_PARAMETERS];
    memcpy(fast, factory, sizeof fast);
    fast[4] = 1;
    assert(rotary_set(&rom, rt, fast));
    assert(rt->fast && rt->target[0] == 216.0f && rt->target[1] == 226.0f);
    assert(rt->speed[1] == 3.0f);
    const size_t block = 256;
    std::vector<float> zero(block, 0.0f), L(block), R(block);
    size_t total = (size_t)(0.861 * 32000.0);
    for (size_t done = 0; done < total; done += block) {
      size_t m = total - done < block ? total - done : block;
      rotary_process(rt, zero.data(), zero.data(), L.data(), R.data(), m);
    }
    double drum = (rt->speed[1] - 3.0) / (226.0 - 3.0);
    double horn = (rt->speed[0] - 18.0) / (216.0 - 18.0);
    assert(near(drum, 1.0 - exp(-1.0), 0.002));
    assert(near(horn, 1.0 - exp(-0.7), 0.002));

    /* A control offset that drops the sum to 63 or less brings it back. */
    rotary_control(rt, -64);
    assert(!rt->fast && rt->target[1] == 3.0f);
  }

  printf("xp_rotary_test: ok\n");
  return 0;
}
