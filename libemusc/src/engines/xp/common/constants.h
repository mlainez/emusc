/* SPDX-License-Identifier: CC0-1.0 */
/*
 *  Constants believed shared across the XP engine's whole chip family, or
 *  currently duplicated across engines/xp/*.cc with no device-specific
 *  reasoning behind the duplication - centralized here regardless of
 *  whether a future device on this engine ever needs a different value.
 *  A genuine per-device override, if one is ever needed, belongs in that
 *  device's own file under engines/xp/devices/, not here.
 */
#ifndef EMUSC_XP_COMMON_CONSTANTS_H
#define EMUSC_XP_COMMON_CONSTANTS_H

#include <stdint.h>

/* The chip's native sample rate. Independently redeclared five times before
   this file existed (TVF's sine table, the reverb and chorus DSPs, the
   output DAC, and the wave ROM's own playback rate) - all 32000.0. */
inline constexpr double kXpNativeRate = 32000.0;

/* The voice-control service period: one tick every 10001 clocks of a
   1.25 MHz timer, ~124.9875 times a second, ~8.0008 ms. kXpControlPeriodHz
   and kXpControlPeriodSeconds are the same fact in the two other units
   consumers need, derived so they can never drift from the clock pair. */
inline constexpr double kXpControlTimerHz = 1250000.0;
inline constexpr double kXpControlPeriodClocks = 10001.0;
inline constexpr double kXpControlPeriodHz =
  kXpControlTimerHz / kXpControlPeriodClocks;
inline constexpr double kXpControlPeriodSeconds =
  kXpControlPeriodClocks / kXpControlTimerHz;

/* The XP pitch-register format: 0x38000 is unity playback, 0x4000 pitch
   units make one octave, a register saturates at 0x3ffff, and 0x555 pitch
   units make one semitone (the remainder step in the portamento/pitch-bend
   glide law). oscillator.h cites this as cross-checked on the JV-1080's
   dumped wave ROM, since it carries the identical part - the strongest
   evidence in the tree for this format, for either device. */
inline constexpr uint32_t kXpPitchUnity = 0x38000u;
inline constexpr uint32_t kXpPitchUnitsPerOctave = 0x4000u;
inline constexpr uint32_t kXpPitchSaturation = 0x3ffffu;
inline constexpr uint32_t kXpPitchRemainderStep = 0x555u;

/* The fixed-point coefficient decode's exponent table, independently
   redeclared identically in the EQ, delay, reverb and chorus DSP blocks:
   value * 2^kXpCoefficientShift[raw >> 14], the shared 2-bit-exponent /
   14-bit-mantissa encoding those four ROM tables all use. */
inline constexpr unsigned kXpCoefficientShift[4] = {0u, 1u, 2u, 4u};

#endif
