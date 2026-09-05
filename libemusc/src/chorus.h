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

// All SC-55+ variations handle system effects, such as chorus, using an
// external audio chip. This chip's ERAM operates as a single circular
// 16384-word buffer that is shared with the reverb effect. The chorus bus
// passes a 1-pole low-pass filter and is written to the ring buffer; two taps
// are read, each at two adjacent positions and linearly interpolated. The LFO
// output (which actually is a full voice running in ping-ping loop mode) is
// one of the taps, while the other tap is its mirror about the loop midpoint.

// While the first version of chorus in libEmuSC was a generic implementation
// based on general principles and adapted by listening tests, the current
// version is a direct implementation of this SC-55 hardware behavior. This
// implementation is based on the reverse-engineering work done by nukeykt as
// part of the Nuked-SC55 project (https://github.com/nukeykt/Nuked-SC55).

// The JV family drives the same chip from a different program, and it is a
// different mechanism rather than different constants: its firmware hands the
// chip a start address, an end address, a position and a read-rate increment out
// of a three-entry table in its own control ROM, and the chorus is that read
// pointer sweeping between the two addresses - there is no LFO, no delay
// parameter and no reverb-send level, and the MIX/REVERB return is a hard
// switch. All of that is in _update_jv(), read from the JV's firmware and its
// ROM records (PROVENANCE.md P-0394); the sweep automaton itself is shared and
// unchanged. Which mechanism a device uses is a field of its profile,
// ChorusLawKind, exactly as its level, reverb and filter laws are.


#ifndef __CHORUS_H__
#define __CHORUS_H__

#include "settings.h"

#include <array>
#include <cstdint>


namespace EmuSC {

class Chorus
{
 public:
  Chorus(Settings *settings, const struct ControlRom::LookupTables &LUT);

  void process_sample(float input, float output[2], float *reverbSend);
  void update(void);   // call at control rate (every 256 samples)

 private:
  Chorus();

  // The two mechanisms this unit is driven by. process_sample is SHARED - it is
  // the chip's own sweep, interpolation and return matrix, and the two families
  // put the same chip behind their chorus - so a device differs only in what
  // these two write into the geometry and the eight return gains.
  void _update_sound_canvas(void);
  void _update_jv(void);

  Settings *_settings;
  const struct ControlRom::LookupTables &_LUT;

  // Set once, from the device profile: the JV sweeps a read pointer between two
  // addresses out of a per-type ROM record instead of modulating a delay length
  // from an LFO (PROVENANCE.md P-0394).
  bool _jv;
  const struct ChorusJvLaw *_jvLaw;
  int _jvType;                // the record currently loaded, -1 if none
  unsigned _jvLoadSeen;       // the performance load the sweep was started by
  int _jvHold;                // samples left of the post-load silence

  static constexpr int rBufferSize = 16384;
  static constexpr int rBufferMask = rBufferSize - 1;
  std::array<float, rBufferSize> _rBuffer;
  int _sweepIndex;

  uint16_t _pIn;              // [29][9] Input write pointer
  uint16_t _pTap1, _pTap2;    // [29][10], [29][11] (firmware-swept)
  uint16_t _phase;            // [31][8] LFO phase (firmware-stepped)
  uint16_t _v9, _v10;         // [31][9], [31][10] triangle values
  float _preA, _preB;         // [31][1] Pre-LPF coefficients
  float _g2L, _g2S;           // [31][2] Tap1 -> L gain, -> reverb send
  float _g3R, _g3F;           // [31][3] Tap1 -> R gain, -> feedback
  float _g4L, _g4S;           // [31][4] Tap2 -> L gain, -> reverb send
  float _g5R, _g5F;           // [31][5] Tap2 -> R gain, -> feedback

  float _preLpfState;
  float _fbSample;            // Feedback re-injected into the bus (1-tick)

  uint16_t _phaseInc;         // Pitch: sub_phase increment per tick
  int  _subPhase = 0;         // 14-bit fraction (ram2[31][8] low bits)
  int  _sAddress;             // Sweeping address (ram1[31][4])

  bool _dir = false;          // Ping-pong direction (bit 15)
  int  _loopOfs, _span;       // loop geometry (Delay / Depth)

  int _chorusMacroSeen;

};

}  // namespace EmuSC

#endif  // __CHORUS_H__
