// Shared output-gain helper for emusc-render, emuscd and emusc-winmidi: one
// dB-to-linear conversion and one apply-in-place function, so the three thin
// CLI/daemon front ends over libEmuSC treat an operator-requested output
// gain (--gain-db) identically. This is a listening-convenience knob only -
// it runs after everything libEmuSC itself computes and never feeds back
// into the synth.

#pragma once

#include <cmath>

namespace emusc_tools {

// 10^(dB/20), the linear amplitude multiplier for a gain given in decibels.
// 0 dB converts to exactly 1.0f.
inline float gain_db_to_linear(double db) {
  return static_cast<float>(std::pow(10.0, db / 20.0));
}

// Scales one stereo frame by `gain` in place. gain == 1.0f (0 dB, what every
// caller here uses when --gain-db is left unset) is skipped entirely, so
// leaving the option off never touches a sample - not even with a
// multiply-by-one that IEEE 754 would leave unchanged anyway.
inline void apply_gain(float &l, float &r, float gain) {
  if (gain == 1.0f) return;
  l *= gain;
  r *= gain;
}

}  // namespace emusc_tools
