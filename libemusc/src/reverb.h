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

// All SC-55+ variations handle system effects, such as reverb, using an
// external audio chip. This chip's ERAM operates as a single circular
// 16384-word buffer where all delay lines are managed via pointer offsets.
// Each individual reverb character (e.g., Room 1) is simply driven by a
// specific register set running a fixed program.
//
// While the first version of EmuSC's reverb engine was inspired by Freeverb,
// the current version is a direct implementation of this SC-55 hardware
// behavior. This implementation is based on the reverse-engineering work done
// by nukeykt as part of the Nuked-SC55 project
// (https://github.com/nukeykt/Nuked-SC55).
//
// The JV-880 runs a DIFFERENT network on the same chip and takes a separate
// path, selected by ReverbNetworkKind::JVMultiTapLine: one mono recirculating
// delay line read by nine independent stereo tap pairs, with the loop closed
// from a tenth tap. It shares none of the register program above - no comb
// bank, no allpass diffuser, no tank - and every number in it is read from
// that device's own ROM2 type records rather than measured (P-0399). Its
// provenance is the JV-880's firmware and Roland's own service notes, not the
// project named above.


#ifndef __REVERB_H__
#define __REVERB_H__


#include "control_rom.h"
#include "settings.h"

#include <array>
#include <cstdint>


namespace EmuSC {


class Reverb
{
public:
  // The control ROM's lookup tables come in for one reason: a device whose
  // return law is ReverbReturnLaw::JVTypeCoefficient reads a per-type
  // coefficient out of its own reverb records (P-0395).
  Reverb(Settings *settings, const struct ControlRom::LookupTables &LUT);

  void process_sample(float input, float output[2]);
  void update(void);

private:
  Reverb();

  Settings *_settings;
  const struct ControlRom::LookupTables &_LUT;

  static constexpr int rBufferSize = 16384;
  static constexpr int rBufferMask = rBufferSize - 1;
  std::array<float, rBufferSize> _rBuffer;
  int _sweepIndex;

  // Reverb chip registers per character
  struct _CharacterRegs {
    uint16_t p28[12];   // ram2[28][0..11]: Buffer pointers (writers + taps)
    uint16_t p29[9];    // ram2[29][0..8]
    uint16_t c4, c5;    // 30][4] Diffuser gain/mix, 30][5] stage-4 coef/mode
    uint16_t c6;        // 30][6] Tank allpass a (hi, signed) / b (lo)
    uint16_t c7, c8;    // 30][7], 30][8] Damping A / B (hi pole, lo signed in)
  };

  _CharacterRegs _activeCharRegs;

  // Measured character regiser sets (delay and panning delay share regisers)
  static constexpr _CharacterRegs _crRoom1 = {
    { 0x0000, 0x0143, 0x0232, 0x02c1, 0x02c2, 0x03b9, 0x02c1, 0x02c1, 0x06f4,
      0x08ee, 0x0c9c, 0x0a09 },
    { 0x0f8d, 0x1098, 0x112e, 0x1289, 0x1417, 0x15ea, 0x16ef, 0x194f, 0x1c07},
    0x0820, 0x0000, 0xe020, 0x10e6, 0x1fee
  };

  static constexpr _CharacterRegs _crRoom2 = {
    { 0x0000, 0x0395, 0x050f, 0x057e, 0x05cb, 0x0865, 0x0590, 0x0590, 0x0fdf,
      0x130d, 0x1556, 0x1a74},
    { 0x1c31, 0x1f18, 0x20f6, 0x2526, 0x2771, 0x2a7f, 0x2cb1, 0x319c, 0x3348},
    0x0820, 0x0000, 0xe020, 0x08e1, 0x1fee
  };

  static constexpr _CharacterRegs _crRoom3 = {
    { 0x0000, 0x0395, 0x050f, 0x057e, 0x05cb, 0x0865, 0x0590, 0x0590, 0x0fdf,
      0x130d, 0x1556, 0x1a74},
    { 0x1c31, 0x1f18, 0x20f6, 0x2526, 0x2771, 0x2a7f, 0x2cb1, 0x319c, 0x3348},
    0x0820, 0x0000, 0xe020, 0x1fee, 0x1fee
  };

  static constexpr _CharacterRegs _crHall1 = {
    { 0x0000, 0x0395, 0x050f, 0x057e, 0x05cb, 0x0865, 0x0a43, 0x0e73, 0x0fdf,
      0x130d, 0x1556, 0x1a74},
    { 0x1c31, 0x1f18, 0x212e, 0x25da, 0x2771, 0x2a7f, 0x2cb1, 0x319c, 0x3348},
    0x1020, 0x0000, 0xe020, 0x1fee, 0x1fee
  };

  static constexpr _CharacterRegs _crHall2 = {
    { 0x0000, 0x039e, 0x051b, 0x058b, 0x058c, 0x0892, 0x0abe, 0x0f9d, 0x1145,
      0x14fb, 0x17a5, 0x1d9f },
    { 0x1fa6, 0x2302, 0x256c, 0x2ad5, 0x2cac, 0x3039, 0x31b9, 0x36ff, 0x37fe},
    0x0820, 0x0020, 0xe020, 0x1fee, 0x1fee
  };

  static constexpr _CharacterRegs _crPlate = {
    { 0x0000, 0x0143, 0x0232, 0x02c1, 0x02c2, 0x03b9, 0x0584, 0x0443, 0x06f4,
      0x08ee, 0x0c9c, 0x0a09},
    { 0x0f8d, 0x1098, 0x112e, 0x1289, 0x1417, 0x15ea, 0x16ef, 0x194f, 0x1c07},
    0x0820, 0x0020, 0xe020, 0x08e1, 0x10e6
  };

  static constexpr _CharacterRegs _crDelayBase = {
    { 0x0000, 0x0001, 0x0003, 0x0005, 0x0007, 0x0009, 0x1c16, 0x1c16, 0x000b,
      0x000d, 0x1c16, 0x1c16},
    { 0x000f, 0x0011, 0x1c16, 0x1c16, 0x0013, 0x0015, 0x1c16, 0x1c16, 0x1c16},
    0x1000, 0x0000, 0x0000, 0x0000, 0x01e1
  };

  static constexpr const _CharacterRegs *_charRegs[6] =
    { &_crRoom1, &_crRoom2, &_crRoom3, &_crHall1, &_crHall2, &_crPlate };

  // THE JV-880's NETWORK - one mono recirculating delay line, nine stereo tap
  // pairs, the loop closed from a tenth tap. Every field is loaded from the
  // selected type's own ROM record (ControlRom::LookupTables::JVReverbRecord,
  // P-0399); nothing here is a constant of this program. Selected by
  // ReverbNetworkKind::JVMultiTapLine and untouched on every other device.
  static constexpr int _jvTaps = 9;
  uint16_t _jvTapL[_jvTaps], _jvTapR[_jvTaps];
  float    _jvGain[_jvTaps];        // signed Q6, 64 = 1.0
  bool     _jvActive[_jvTaps];      // gain == 0 is a MUTED pair, not a quiet one
  float    _jvInGain;               // record word 22's high byte, signed Q6
  uint16_t _jvLoopTap;              // record word 20: the loop period
  uint16_t _jvPreDelay;             // record word 1, read and reported
  uint16_t _jvTapBase;              // record word 13 + 1: the delay arm's base
  int      _jvTimeScale;            // record[+0x36]

  float _preLpfState;
  float _preLpfA, _preLpfB;
  float _dampA, _dampB;          // 1-pole damping states (per branch)
  float _gLoop;                  // Loop gain (signed hi byte of 30][9] / 64)
  float _outGain;                // Level / 32 (30][2]/[3] hi / 32)

  int _character;
  int _pendingCharacter;

  // A character change does not take effect at once: the machine fades the
  // old tail out, goes silent, and only then starts the new character.
  // Measured on the SC-55mkII, both counted in samples at 32 kHz.
  static constexpr int _fadeOutSamples = 5280;    // 165 ms
  static constexpr int _silenceSamples = 9760;    // to 470 ms after the write
  int _fadeRemaining;
  int _silenceRemaining;

  int _preLPF;
  int _reverbTime;
  int _delayFeedback;

  // Which network this device runs, read once from the profile. A per-sample
  // dispatch, so it is not looked up per sample.
  const bool _jvNetwork;

  void _set_character(int character);
  void _set_jv_character(int character);
  void _process_sample_jv(float input, float output[2]);
  void _set_reverb_time(int reverbTime);
  void _set_pre_lpf(int preLPF);
  void _set_delay_feedback(int delayFeedback);
  void _set_level(int level);
  float _fade_step(void);
  float _jv_loop_gain(int expandedTime) const;

  static inline int fbToTarget(int f)
  { return std::min(2 * std::clamp(f, 0, 127) + 2, 191); }

  static inline float uByte(uint16_t v, bool hi)       // Unsigned byte / 64
  { return (hi ? (v >> 8) : (v & 0xff)) / 64.0f; }

  static inline float sByte(uint16_t v, bool hi)       // Signed byte / 64
  {
    int b = hi ? (v >> 8) : (v & 0xff);
    if (b > 127) b -= 256;
    return b / 64.0f;
  }

  // Measured time values, equal for all 6 non-delay characters
  static constexpr uint8_t _timeTargetLUT[128] = {
      0,   1,   3,   5,   7,   8,  10,  12,
     14,  15,  17,  19,  21,  23,  24,  26,
     28,  30,  31,  33,  35,  37,  39,  40,
     42,  44,  46,  47,  49,  51,  53,  54,
     56,  58,  60,  62,  63,  65,  67,  69,
     70,  72,  74,  76,  78,  79,  81,  83,
     85,  86,  88,  90,  92,  93,  95,  97,
     99, 101, 102, 104, 106, 108, 109, 111,
    113, 115, 117, 118, 120, 122, 124, 125,
    127, 129, 131, 133, 134, 136, 138, 140,
    141, 143, 145, 147, 148, 150, 152, 154,
    156, 157, 159, 161, 163, 164, 166, 168,
    170, 172, 173, 175, 177, 179, 180, 182,
    184, 186, 187, 189, 191, 191, 191, 191,
    191, 191, 191, 191, 191, 191, 191, 191,
    191, 191, 191, 191, 191, 191, 191, 191
  };

};

}  // namespace EmuSC

#endif  // __REVERB_H__
