/*
 *  This file is part of libEmuSC, a Sound Canvas emulator library
 *  Copyright (C) 2022-2026  Håkon Skjelten
 *
 *  libEmuSC is free software: you can redistribute it and/or modify it
 *  under the terms of the GNU Lesser General Public License as published
 *  by the Free Software Foundation, either version 2.1 of the License, or
 *  (at your option) any later version.
 *
 *  libEmuSC is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with libEmuSC. If not, see <http://www.gnu.org/licenses/>.
 */

// DSP building blocks shared by more than one synthesis engine. Relocated
// from engines/gp/resampler.cc (bessel_i0, there Resampler::_i0) and
// engines/gp/svf.h (svf_step, there SVF::process_sample); both call through
// to here now, and engines/xp/'s own independently-arrived-at copies
// (output.cc's besselI0, tvf.cc's per-section filter loop) do too.

#ifndef EMUSC_COMMON_DSP_KERNELS_H
#define EMUSC_COMMON_DSP_KERNELS_H

// Modified Bessel function I0 via polynomial series, used to build a Kaiser
// window.
inline double bessel_i0(double x)
{
  double sum = 1.0;
  double term = 1.0;
  double xh = x * 0.5;

  for (int k = 1; k <= 25; ++k) {
    term *= xh / k;
    sum  += term * term;
  }

  return sum;
}

// One sample of a Chamberlin (forward-Euler) 2-pole state-variable filter.
// f and q are the topology's own coefficients (F1 = 2*sin(pi*fc/fs), Q1 =
// 1/Q); lp and bp carry the filter's state between calls and are updated in
// place. Returns the high-pass output; the caller reads the updated lp for
// the low-pass output.
inline float svf_step(float input, float f, float q, float &lp, float &bp)
{
  float lp_new = lp + f * bp;
  float hp     = input - lp_new - q * bp;
  float bp_new = bp + f * hp;

  lp = lp_new;
  bp = bp_new;

  return hp;
}

#endif  // EMUSC_COMMON_DSP_KERNELS_H
