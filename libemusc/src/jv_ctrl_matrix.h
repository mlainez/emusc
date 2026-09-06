/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * This file consists entirely of AI-generated code without direct human
 * authorship and is dedicated to the public domain under CC0 1.0.
 */
// The JV's per-TONE controller matrix (scdb devices/jv880 D-79).
//
// A tone carries twelve destination/sense pairs: four for the modulation
// wheel, four for channel aftertouch and four for expression. Each slot names
// one of twelve destinations and a signed sense of -63..+63. The firmware
// turns them into twelve per-voice accumulators, one per destination, and each
// accumulator is added into the synthesis parameter it names.
//
// This header holds the two pieces of arithmetic that build them, transcribed
// from ROM1 0x617F (one controller's contribution to one destination) and
// ROM1 0x620E (the scale into the destination's own units), plus the loop the
// note-on builder at ROM1 0x56B0-0x617C runs over the three controllers.
//
// Everything here is integer and exact; nothing is approximated.

#ifndef __JV_CTRL_MATRIX_H__
#define __JV_CTRL_MATRIX_H__

#include "device_profile.h"

#include <stdint.h>

namespace EmuSC {

// The twelve destinations, in the manual's order. 0 is OFF.
enum class JvCtrlDest {
  Off = 0, Pitch, Cutoff, Resonance, Level,
  PitchLfo1, PitchLfo2, TvfLfo1, TvfLfo2, TvaLfo1, TvaLfo2,
  Lfo1Rate, Lfo2Rate, Count
};

static const int JV_CTRL_DESTS = 13;      // including OFF at index 0
static const int JV_CTRL_SLOTS = 4;       // slots per controller
static const int JV_CTRL_SOURCES = 3;     // modulation, aftertouch, expression


// ROM1 0x3843: the signed high-word product both the LFO depths and the
// controller matrix reach their targets through. Magnitudes multiplied, the
// high word taken, the sign restored - which is NOT the same as an arithmetic
// shift of a signed product, and the difference is one unit on a negative one.
inline int jv_mul_hi(int a, int b)
{
  if (!a || !b)
    return 0;
  const int m = (int) (((uint32_t) (a < 0 ? -a : a) *
                        (uint32_t) (b < 0 ? -b : b)) >> 16);
  return ((a < 0) != (b < 0)) ? -m : m;
}


// ROM1 0x617F: what ONE controller contributes to ONE destination.
//
// The senses of every slot pointing at this destination are summed. A sum of
// zero contributes nothing; a sum past +-63 hands the controller's own value
// through unscaled. Otherwise the magnitude is expanded 0..63 -> 0..255 by two
// compare-rotate-invert steps and multiplied by the controller value, and the
// high byte of the rounded product is taken - as a FLOOR, so the negative side
// is one unit further out than the positive side at the same magnitude.
inline int jv_ctrl_contribution(int dest, int ctrlValue,
                                const uint8_t *slotDest, const int8_t *slotSense)
{
  int sum = 0;
  for (int i = 0; i < JV_CTRL_SLOTS; i++)
    if (slotDest[i] == dest)
      sum += slotSense[i];

  if (sum == 0)
    return 0;
  if (sum > 63)
    return ctrlValue;
  if (sum < -63)
    return -ctrlValue;

  const int m  = (sum < 0) ? -sum : sum;
  const int t1 = 2 * m + (m >= 0x20 ? 1 : 0);
  const int t2 = 2 * t1 + (t1 >= 0x40 ? 1 : 0);
  const int p  = t2 * ctrlValue + 0x80;

  return (sum > 0) ? (p >> 8) : -((p + 0xff) >> 8);
}


// ROM1 0x620E: the three contributions summed, then scaled into the
// destination's own units. At or past +-127 the accumulator is the clamp
// exactly; below it the product is taken 8 bits down.
inline int jv_ctrl_scale(int v, int full, int clampTo)
{
  if (v >= 127)
    return clampTo;
  if (v <= -127)
    return -clampTo;
  if (v == 0)
    return 0;

  const int a = ((v < 0) ? -v : v) << 8;
  const int r = (int) (((uint32_t) a * (uint32_t) full) >> 16);
  return (v < 0) ? -r : r;
}


// The whole matrix for one tone, given the three controller values in the
// firmware's own order: modulation (@0x6136), aftertouch (@0x6106),
// expression (@0x6156). `acc` is written for destinations 1..12; index 0 is
// left alone because OFF has no accumulator.
inline void jv_ctrl_accumulate(const CtrlMatrixJvLaw &law,
                               const uint8_t slotDest[JV_CTRL_SOURCES][JV_CTRL_SLOTS],
                               const int8_t slotSense[JV_CTRL_SOURCES][JV_CTRL_SLOTS],
                               const int ctrlValue[JV_CTRL_SOURCES],
                               int acc[JV_CTRL_DESTS])
{
  for (int d = 1; d < JV_CTRL_DESTS; d++) {
    int sum = 0;
    for (int c = 0; c < JV_CTRL_SOURCES; c++) {
      if (!ctrlValue[c])                 // ROM1 0x56EF: a controller at zero is skipped
        continue;
      sum += jv_ctrl_contribution(d, ctrlValue[c], slotDest[c], slotSense[c]);
    }
    acc[d] = jv_ctrl_scale(sum, law.full[d], law.clamp[d]);
  }
}

}

#endif  // __JV_CTRL_MATRIX_H__
