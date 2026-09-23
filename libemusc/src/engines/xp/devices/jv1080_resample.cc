/* SPDX-License-Identifier: CC0-1.0 */
/*
 *  The JV-1080 engine's own 32 kHz to host-rate converter; see the header.
 */

#include "jv1080_resample.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace EmuSC { namespace Xp {

namespace {

const double kPi = 3.14159265358979323846;
/* The Kaiser window's beta: about -90 dB of stopband. */
const double kBeta = 9.0;
/* The kernel's -6 dB point when the host runs at or above 32 kHz. With 64
   taps the transition is about 2.9 kHz wide, so the band is flat to about
   13 kHz and the machine's own images, from 16 kHz up, are under the
   stopband. Below 32 kHz the same fraction of the host's Nyquist is kept.
   A choice of this converter, not a measurement of the machine. */
const double kPassHz = 14500.0;

double bessel_i0(double x)
{
  double sum = 1.0, term = 1.0;
  for (int k = 1; k < 64; ++k) {
    double t = x / (2.0 * (double)k);
    term *= t * t;
    sum += term;
    if (term < 1e-12 * sum)
      break;
  }
  return sum;
}

}  // namespace

bool jv_resampler_init(struct JvResampler *rs, double inputRate,
                        double outputRate)
{
  if (!rs || inputRate <= 0.0 || outputRate <= 0.0)
    return false;
  std::memset(rs, 0, sizeof *rs);
  const int taps = 2 * kJvResampleHalf;
  rs->table = (float *)std::malloc((size_t)(kJvResamplePhases + 1) *
                                   (size_t)taps * sizeof *rs->table);
  if (!rs->table)
    return false;
  rs->ratio = inputRate / outputRate;
  double passHz = outputRate >= inputRate
    ? kPassHz : kPassHz * outputRate / inputRate;
  double fc = passHz / inputRate;      /* cycles per input sample */
  const double i0beta = bessel_i0(kBeta);
  for (int p = 0; p <= kJvResamplePhases; ++p) {
    double frac = (double)p / (double)kJvResamplePhases;
    float *row = rs->table + (size_t)p * (size_t)taps;
    double sum = 0.0;
    double h[2 * kJvResampleHalf];
    for (int j = 0; j < taps; ++j) {
      double d = (double)(j - kJvResampleHalf + 1) - frac;
      double x = 2.0 * fc * d;
      double sinc = std::fabs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x);
      double w = d / (double)kJvResampleHalf;
      double win = std::fabs(w) >= 1.0 ? 0.0
        : bessel_i0(kBeta * std::sqrt(1.0 - w * w)) / i0beta;
      h[j] = sinc * win;
      sum += h[j];
    }
    for (int j = 0; j < taps; ++j)
      row[j] = (float)(h[j] / sum);
  }
  jv_resampler_reset(rs);
  return true;
}

void jv_resampler_free(struct JvResampler *rs)
{
  if (!rs)
    return;
  std::free(rs->table);
  rs->table = nullptr;
}

/* The ring is primed with half a kernel of silence, so input sample 0 is
   push number `half` and the first output is centred on it: the output's
   time zero is the engine's time zero. */
void jv_resampler_reset(struct JvResampler *rs)
{
  if (!rs)
    return;
  std::memset(rs->ring, 0, sizeof rs->ring);
  rs->position = 0.0;
  rs->written = kJvResampleHalf;
}

bool jv_resampler_needs_input(const struct JvResampler *rs)
{
  long long i0 = (long long)std::floor(rs->position);
  return rs->written <= i0 + 2 * kJvResampleHalf;
}

void jv_resampler_push(struct JvResampler *rs, float left, float right)
{
  int slot = (int)(rs->written & (kJvResampleRing - 1));
  rs->ring[0][slot] = left;
  rs->ring[1][slot] = right;
  ++rs->written;
}

void jv_resampler_pull(struct JvResampler *rs, float *left, float *right)
{
  const int taps = 2 * kJvResampleHalf;
  long long i0 = (long long)std::floor(rs->position);
  double frac = rs->position - (double)i0;
  double u = frac * (double)kJvResamplePhases;
  int p = (int)u;
  if (p >= kJvResamplePhases)
    p = kJvResamplePhases - 1;
  double t = u - (double)p;
  const float *a = rs->table + (size_t)p * (size_t)taps;
  const float *b = a + taps;
  /* Push number of the first tap: input sample i0 - half + 1, which is
     push i0 + 1. */
  long long first = i0 + 1;
  double l = 0.0, r = 0.0;
  for (int j = 0; j < taps; ++j) {
    double w = (double)a[j] + t * ((double)b[j] - (double)a[j]);
    int slot = (int)((first + j) & (kJvResampleRing - 1));
    l += w * rs->ring[0][slot];
    r += w * rs->ring[1][slot];
  }
  *left = (float)l;
  *right = (float)r;
  rs->position += rs->ratio;
}

}}  // namespace EmuSC::Xp
