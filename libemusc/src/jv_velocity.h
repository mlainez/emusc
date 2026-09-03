/*
 *  This file is part of libEmuSC, a Sound Canvas emulator library
 *  Copyright (C) 2022-2026  Håkon Skjelten
 *
 *  libEmuSC is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published
 *  by the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  libEmuSC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with libEmuSC. If not, see <http://www.gnu.org/licenses/>.
 */

// The JV's velocity attenuation, one routine for three sensitivities.
//
// The JV-880's firmware has a single helper (ROM1 0x6236) that turns a tone's
// signed velocity sensitivity byte, its velocity-curve selector and the note-on
// velocity into a 16-bit ATTENUATION factor, and calls it three times per note
// on: for the pitch envelope's velocity sensitivity (with curve 0), the TVF
// envelope's (tone byte +0x37 & 7 selects the curve) and the TVA level's (tone
// byte +0x47 & 7). Each consumer then takes `x -= (x * w) >> 16`. The seven
// 128-byte curves at ROM2 0x5390 fall from ~255 at velocity 0 to 0 at 127, so
// a positive sensitivity means "quiet notes are attenuated more".
//
// Read from the instruction bytes (scdb devices/jv880, D-35 track note §8):
//
//   0 < sens < 32 : w = expand3(sens)  * CURVE[c][vel]
//   sens >= 32    : t = (sens * (127 - vel)) >> 5
//                   w = t >= 0x80 ? 0xFFFF : CURVE[c][127 - t] << 8 | sext
//   -32 < sens < 0: w = expand3(-sens) * (255 - CURVE[c][vel])
//   sens <= -32   : t = (-sens * vel) >> 5
//                   w = t >= 0x80 ? 0xFFFF : (255 - CURVE[c][t]) << 8 | sext
//
// where expand3 doubles a 0..31 value three times, carrying in the bit it
// crossed each time (cmp/rotxl/bnot on thresholds 0x10, 0x20, 0x40), so it maps
// 0..31 onto 0..255; and `| sext` is the byte's own sign extension (0x00 or
// 0xFF) left in the low half by the firmware's `exts.b; swap.b`, at most 255 of
// 65535. The two high arms saturate to FULL attenuation - the firmware's
// 0x6281 - so a strong positive sensitivity silences soft notes and a strong
// negative one silences loud ones.
//
// Two earlier readings in this port were wrong and are replaced by this one:
// the TVA used the positive high arm for every positive sensitivity (right for
// SAW Lead's +35, 1.8 dB too quiet for Glass Pad's +13 on curve 2), and the
// TVF's positive high arm was a mirror guessed from the negative one.

#ifndef __JV_VELOCITY_H__
#define __JV_VELOCITY_H__

#include <array>
#include <cstdint>

namespace EmuSC {

inline int jv_velocity_attenuation(const std::array<uint8_t, 896> &curves,
                                   int curve, int sens, int velocity)
{
  if (sens == 0)                             // the callers skip the helper
    return 0;

  const int banked = (int) (curves.size() / 128);
  const int base = ((curve < 0) ? 0 : (curve < banked) ? curve : banked - 1)
                   * 128;
  const int vel = (velocity < 0) ? 0 : (velocity > 127) ? 127 : velocity;

  auto expand3 = [](int v) {
    for (int threshold : { 0x10, 0x20, 0x40 })
      v = 2 * v + (v >= threshold ? 1 : 0);
    return v;
  };
  auto byte_high = [](int b) {               // exts.b ; swap.b
    return (b << 8) | ((b & 0x80) ? 0xff : 0x00);
  };

  if (sens >= 32) {
    const int t = (sens * (127 - vel)) >> 5;
    return (t >= 0x80) ? 0xffff : byte_high(curves[base + 127 - t]);
  }
  if (sens > 0)
    return expand3(sens) * curves[base + vel];
  if (sens > -32)
    return expand3(-sens) * (255 - curves[base + vel]);

  const int t = ((-sens) * vel) >> 5;
  return (t >= 0x80) ? 0xffff : byte_high(255 - curves[base + t]);
}

}

#endif  // __JV_VELOCITY_H__
