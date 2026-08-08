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

// This is the slew level implementation that is used to calculate the slew
// rate for TVA dynamic volume, TVA envelope, TVF envelope and reverb
// loop-gain. On the Sound Canvas this slew calculation is done on the external
// audio chip. By using the 16 bit "mode" variable the subsystems uses only MSB
// of the target value combined with a "speed" byte to set gain for each
// sample.
// 
// Short description of common encodings:
//  0x0a   Linear, 0.625 level-units/tick
//  0xaf   Fast to target, land exactly and pin
//  0xb4   Exponential, tau = 64 ticks (2.0 ms)
//  0xb6   Exponential, tau = 16 ticks (0.5 ms)
//  0xba   Instant jump
//  0xff00 Speed = 0x00, no slew, used when |delta| < 0x10
//
// This implementation is based on the reverse-engineering work done by nukeykt
// as part of the Nuked-SC55 project (https://github.com/nukeykt/Nuked-SC55).


#ifndef _SLEW_CALC_H_
#define _SLEW_CALC_H_


#include <cstdint>


namespace EmuSC {


struct SlewCalc {
  uint32_t tv = 0x3fff;    // Dither/divider counter; decrement once per tick

  // Slew algorithm based on information from the Nuked-SC55 project by nukeykt
  // e: 0 = TVA dynamic volume, 1 = TVA envelope, 2 = TVF cutoff
  // Returns the instantaneous output multiplier
  inline int tick(uint16_t adjust, uint16_t &levelRef, int e,
                  bool active = true)
  {
    int level  = levelRef & 0x7fff;
    int speed  = adjust & 0xff;
    int target = (adjust >> 8) & 0xff;

    bool w1 = (speed & 0xf0) == 0;
    bool w2 = w1 || (speed & 0x10) != 0;
    bool w3 = (speed & 0x80) == 0 ||
              ((speed & 0x40) == 0 && (!w2 || (speed & 0x20) == 0));

    int type = (w2 ? 1 : 0) | (w3 ? 8 : 0);
    if (speed & 0x20) type |= 2;
    if ((speed & 0x80) == 0 || (speed & 0x40) == 0) type |= 4;

    bool write = !active;
    int  addlow = 0;
    auto rev4 = [&](int b3, int b2, int b1, int b0) {
      int a = 0;
      if (tv & b3) a |= 1;  if (tv & b2) a |= 2;
      if (tv & b1) a |= 4;  if (tv & b0) a |= 8;
      return a;
    };
    if (type & 4) {
      addlow = rev4(8, 4, 2, 1);
      write = true;
    } else {
      switch (type & 3)
        {
        case 0: addlow = rev4(0x20,0x10,8,4);
          write |= (tv & 3) == 0;   break;
        case 1: addlow = rev4(0x80,0x40,0x20,0x10);
          write |= (tv & 15) == 0;  break;
        case 2: addlow = rev4(0x200,0x100,0x80,0x40);
          write |= (tv & 63) == 0;  break;
        case 3: addlow = rev4(0x800,0x400,0x200,0x100);
          write |= (tv & 127) == 0; break;
        }
    }

    int volmul;
    if ((type & 8) == 0) {                        // mode A: Exponential
      int shift = (10 - (speed & 15)) & 15;
      int sum1 = (target << 11);
      if (e != 2 || active)
        sum1 -= (level << 4);

      sum1 = (sum1 << 12) >> 12;
      int shifted = (sum1 >> shift) - sum1;
      int sum2 = (target << 11) + addlow + shifted;
      if (write)
        level = (sum2 >> 4) & 0x7fff;

      volmul = (sum2 >> 4) & 0x7ffe;

    } else {                                      // mode B: Linear
      int shift = ((speed >> 4) & 14) | (w2 ? 1 : 0);
      shift = (10 - shift) & 15;

      int sum1 = target << 11;
      if (e != 2 || active)
        sum1 -= (level << 4);

      sum1 = (sum1 << 12) >> 12;

      bool neg = sum1 < 0;
      int preshift = (speed & 15) << 9;
      if (!w1)
        preshift |= 0x2000;
      if (neg)
        preshift ^= ~0x3f;

      int shifted = preshift >> shift;
      int sum2 = shifted;
      if (e != 2 || active)
        sum2 += (level << 4) | addlow;

      int sum2_l = sum2 >> 4;
      int sum3 = (target << 11) - (sum2_l << 4);
      sum3 = (sum3 << 12) >> 12;
      bool xnor = !((sum3 < 0) ^ neg);
      if (write)
        level = xnor ? (sum2_l & 0x7fff) : (target << 7);

      volmul = (e != 1 || xnor) ? (sum2_l & 0x7ffe) : (target << 7);
    }

    levelRef = (uint16_t)level;
    tv = (tv - 1) & 0x3fff;

    return volmul;
  }

  // |delta| < 0x10 => no slew, just use the new value directly
  static inline void set_level_direct(uint16_t &levelRef, uint16_t levelEC)
  {
    levelRef = levelEC & 0x7fff;
  }
};


}  // namespace EmuSC


#endif  // _SLEW_CALC_H_
