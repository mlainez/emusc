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


#ifndef __Pitch_H__
#define __Pitch_H__


#include "control_rom.h"
#include "envelope.h"
#include "settings.h"
#include "wave_generator.h"

#include <stdint.h>

#include <array>


namespace EmuSC {


class Pitch : public Envelope
{
public:
  Pitch(ControlRom &ctrlRom, uint16_t instrumentIndex, int partialId,
        uint8_t key, uint8_t velocity, WaveGenerator *LFO1, WaveGenerator *LFO2,
        Settings *settings, int8_t partId);
  ~Pitch();

  void update(void);

  void note_off();

  inline float get_phase_increment(void) {
    _currentInc += _deltaInc; return _currentInc; }

  inline uint16_t get_sample_id(void) { return _sampleIndex; }

  void first_sample_run_complete(void);


private:
  bool _firstUpdate;
  int _key;                   // MIDI key number for normal instruments
  int _dKey;                  // MIDI key number for drumsets
  int _drumSet;               // 0 = normal inst., 1 = drumset1, 2 = drumset2

  ControlRom &_ctrlRom;
  uint16_t _instrumentIndex;

  ControlRom::InstPartial &_instPartial;
  ControlRom::LookupTables &_LUT;

  WaveGenerator *_LFO1;
  WaveGenerator *_LFO2;

  // Analog Feel, the manual's "1/f fluctuation": a slow per-voice pitch drift.

  // The firmware keeps one signed byte per voice slot in @0x8572, stepped by

  // task 13 every 16 ms at ROM1 0x0F12-0x0F9D as two-pole filtered noise

  // sampled into a linear ramp of about 128 ms per segment, and never resets

  // it at note-on. The pitch consumer is ROM1 0x40E3 with

  //   t = drift >> 1        (-64..+63)

  //   dCents = sign(t) * ((|t| * 2*AF) >> 8)

  // giving +/-5 cents at this device's commonest non-zero setting, AF = 12,

  // and +/-62 cents at AF = 127. There is a level consumer too (0x445D) but

  // its worst case over the whole parameter space is 0.12 dB, so it is left

  // out as numerically negligible - Analog Feel is audibly a PITCH mechanism,

  // and reaches level only by decohering a stack. D-25, L-26.

  //

  // DEVIATION, stated because it is real: the device's entropy is a sound-chip

  // register read, so its drift SEQUENCE is not computable from the ROM and no

  // port can match it sample-for-sample - only its magnitude, bandwidth and

  // per-voice independence. This generator is therefore seeded per voice from

  // a monotonic counter rather than shared and never-reset. That reproduces

  // the within-note wander and the fact that two identical notes differ; it

  // does not reproduce continuity of one slot's drift across notes.

  int  _afDepth;                        // 2 * Analog Feel, 0 disables

  int  _afDrift, _afFrom, _afTo;        // current, ramp start, ramp target

  int  _afStep;                         // 8 ms ticks since the segment began

  int  _afN1, _afN2;                    // two-pole noise state

  uint32_t _afRng;

  int  _af_drift_cents10(void);


  bool _lfo1FadeComplete;
  bool _lfo2FadeComplete;
  int _lfo1Depth;
  int _lfo2Depth;

  int _envTimeKeyFlwT14;
  int _envTimeKeyFlwT5;
  int _envTimeVelSens;

  int _envPhaseRate[8];
  bool _isAscending;

  int _envVelSens;

  int _keyFollowOffset;

  int _phaseLevel[6];

  int _basePitchC;
  int _basePitchF;
  uint16_t _sampleIndex;

  int _samplePitchOffsetInit;
  int _samplePitchOffsetSust;
  int _samplePitchOffsetActive;
  int _portamentoDelta;
  int _portamentoRem;
  int _releasePitch;
  int _targetPitch;

  int _cachedPFineTune;
  int _cachedPFineTuneOffset;

  int _phaseIncrement;

  float _currentInc;
  float _deltaInc;

  Settings *_settings;
  int8_t _partId;

  // Portamento pitch values are shared among all voices / instrument partials.
  // Portamento base pitch is reused in a round robin fashion for all available
  // voices, while the portamento target pitch is a global target.
  static int _portaTargetPitch;
  static std::array<int, 28> _portaBasePitch;
  static int _pbpIndex;

  // The pitch the last note on each part settled at, which is what an ordinary
  // portamento glides FROM. The slot ring above is a voice-slot mechanism and
  // says nothing about which part a slot belonged to.
  //
  // Two arrays, not one, because a note has up to TWO partials and both must
  // glide from the SAME predecessor. With a single array the first partial
  // overwrote the source before the second one read it, so the second partial
  // glided from its own note - a delta of nearly zero - and started at the
  // target pitch while its sibling glided (PROVENANCE.md P-0278). Every
  // two-partial tone was affected; Lead 2 (sawtooth) is one, and it is the
  // tone R10's artefact glides on.
  static std::array<int, 16> _lastPitchOnPart;
  static std::array<int, 16> _pendingPitchOnPart;

  Pitch();

public:
  // Called once by Note before its partials are constructed: the pitch the
  // PREVIOUS note left behind becomes the source every partial of this note
  // glides from.
  static void begin_note(int partId);

private:

  void _init_envelope(uint8_t envelope);
  int _init_portamento(bool portamento, bool legato);

  void _init_base_pitch(void);
  void _apply_key_shifts_bp(void);
  void _apply_key_follow_bp(void);
  void _apply_scale_tuning_bp(void);

  int _calc_phase_inc_from_pitch(int frac);

  int _get_env_key_follow(int p, int kfRom);
  int _get_env_time_velocity_sensitivity(int etvsROM, int velocity);
  int _get_env_phase_rate(int etRom, int phase);

  int _get_pitch_curve_correction(int pitchCurve);

  void _init_new_phase(enum Phase newPhase);
  void _iterate_phase(void);
};

}

#endif  // __Pitch_H__
