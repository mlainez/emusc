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

// This is a Chamberlin (FE) 2-pole (12 dB/oct) State-Variable Filter used for
// Time-Varying Filtering (TVF). It supports low-pass and high-pass output and
// has two input parameters: cutoff frequency [0–127] and resonance (Q) [0–127].
//
// A device whose own firmware computes the two coefficients directly sets them
// with set_coefficients() instead, in the canonical units of this topology:
// F1 = 2*sin(pi*fc/fs) and Q1 = 1/Q. The JV family does - its control ROM holds
// the coefficient tables and its CPU writes the 16-bit words to the chip - and
// pushing those words back through an index approximation would throw away the
// precision they arrive with (PROVENANCE.md P-0390).


#ifndef __SVF_H__
#define __SVF_H__

#include "../common/dsp_kernels.h"


namespace EmuSC { namespace Gp {

class SVF
{
public:
  enum class Mode { LowPass, HighPass };

  SVF(Mode mode);

  void set_cutoff_freq(int coFreq);
  void set_resonance(int resonance );

  // F1 and Q1 as the topology defines them, with 1.0 meaning 1.0.
  inline void set_coefficients(float f1, float q1) { _f = f1; _q = q1; }

  inline float process_sample(float input)
  {
    float hp = svf_step(input, _f, _q, _lp, _bp);

    return (_mode == Mode::LowPass) ? _lp : hp;
  }

  void clear();

private:
  Mode  _mode;     // Low-pass or high-pass

  float _f;        // Cutoff coefficient
  float _q;        // Damping coefficient

  float _lp;       // Low-pass state
  float _bp;       // Band-pass state
};

}}  // namespace EmuSC::Gp

#endif  // __SVF_H__
