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


#ifndef __WAVE_GENERATOR_H__
#define __WAVE_GENERATOR_H__


#include "control_rom.h"
#include "settings.h"

#include <array>

#include <stdint.h>


namespace EmuSC {

class WaveGenerator
{
public:
  enum class Waveform {
    Sine       = 0,
    Square     = 1,
    Sawtooth   = 2,
    Triangle   = 3,
    SampleHold = 8,
    Random     = 9,
  };

  // LFO1 is defined in the Instrument section
  WaveGenerator(struct ControlRom::Instrument &instrument,
                struct ControlRom::LookupTables &LUT,
                Settings *settings, int partId);

  // LFO2s are defined in the Instrument Partial section
  WaveGenerator(struct ControlRom::InstPartial &instPartial,
                struct ControlRom::LookupTables &LUT,
                Settings *settings, int partId);
  ~WaveGenerator();

  void update(void);
  inline int16_t value(void) { return (int16_t) _currentValueNorm; }
  inline float value_float(void) { return (float) _currentValueNorm / 32767.0; }

  inline int fade(void) { return _fade; }

  // The JV's LFO (scdb devices/jv880/07_synthesis/lfo.md, D-37). One per tone
  // and per LFO: jvLfoIndex 0 builds LFO1 from the tone's LFO1 bytes, 1 LFO2.
  // value() is then the firmware's per-voice word - the waveform sample (+/-64)
  // plus offset, << 8, through the delay and fade stages - and jv_raw() the
  // same word without delay/fade, which is what the controller matrix reads.
  WaveGenerator(struct ControlRom::InstPartial &instPartial,
                struct ControlRom::LookupTables &LUT,
                Settings *settings, int partId, int jvLfoIndex);
  inline bool is_jv(void) { return _jv; }
  inline int jv_raw(void) { return _jvRaw; }
  void note_off(void);                  // arms a KEY-OFF delay

private:
  WaveGenerator();

  bool _id;
  enum Waveform _waveform;

  struct ControlRom::LookupTables &_LUT;

  int _instRate;              // LFO Rate from instrument [partial] definition
  int _rateChange;            // Change in rate due to controller input etc.

  int _delay;                 // Current delay status from 0 to 0xffff
  int _delayIncLUT;           // Delay increment @125Hz from LUT

  int _fade;                  // Current fade status from 0 to 0xffff
  int _fadeIncLUT;            // Fade increment @125Hz from LUT

  int _currentValue;          // 16 bit SC-55 scpecific LFO format
  int _currentValueNorm;      // 16 bit normalized LFO value

  uint16_t _accRate;          // Accumulated rate (+ phase shift)
  int _random;
  bool _randomFirstRun;

  Settings *_settings;
  int _partId;

  int _generate_sine(int rate);
  int _generate_square(int rate);
  int _generate_sawtooth(int rate);
  int _generate_triangle(int rate);
  int _generate_sample_hold(int rate);
  int _generate_random(int rate);

  // JV state. Task 13 steps the LFO every 16 ms, i.e. every second control
  // period of this engine, so _jvTick halves the update rate.
  bool     _jv = false;
  int      _jvForm = 0, _jvOffsetIdx = 2, _jvSync = 1, _jvFadeOut = 0;
  int      _jvRate = 0, _jvDelayKeyOff = 0;
  int      _jvDelayInc = 0x10000, _jvFadeInc = 0x10000;
  uint16_t _jvPhase = 0;
  int      _jvTick = 0;
  int      _jvStage = 0;                // 0 delay, 1 fade, 2 steady
  uint32_t _jvStageAcc = 0;
  bool     _jvKeyOff = false;
  int      _jvRnd = 0, _jvRndPrev = 0, _jvRndDelta = 0;
  uint32_t _jvRng = 1;
  int      _jvRaw = 0;

  void _jv_update(void);
  int  _jv_draw(void);
};

}

#endif  // __WAVE_GENERATOR_H__
