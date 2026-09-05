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

  // Analog Feel (patch common +0x14), scdb devices/jv880 D-25 / L-26. The
  // firmware keeps one drift generator per voice slot in RTOS task 13 (16 ms),
  // ROM1 0x0F12-0x0F9D, and this is that machine byte for byte: a signed-byte
  // drift ramps toward a signed-byte target at a step of (target - drift) >> 3
  // per tick (byte arithmetic, so a target more than 127 away drives it the
  // wrong way into the clamp), and when it arrives or the byte overflows a new
  // target is drawn by pushing a sound-chip word through two IIR poles and
  // taking the low byte. Pitch takes (|drift >> 1| * 2 * AF) >> 8 cents.
  //
  // The chip word (ROM1 0x382C, a read of unused voice slot 30) is the one
  // input the ROM cannot supply. Its SEQUENCE is unknowable; its statistics
  // were measured on the reference from a single voice at Analog Feel 127 and
  // 12 (scdb notes/track_d41_wip_2026-09-04.md). An arbitrary full-range word
  // per read, pushed through this machine WITH its byte wraps, reproduces the
  // drift's rms (33 cents at 127), its p99 (61), its autocorrelation at every
  // lag to 0.5 s, its ramp-step statistics and the 3 % of time it spends at
  // the clamp. The wraps are load-bearing: a target more than 127 away sends
  // the ramp the wrong way into the clamp for a tick or two and a fresh draw,
  // which is what slows the drift to the machine's 0.24 s correlation time;
  // a smooth (random-walk) word instead parks the drift at the clamp for
  // seconds, and a machine without the wraps sweeps the whole range in eight
  // ticks - neither is seen on the reference. MEASURED (proxy).
  //
  // Per voice rather than per slot: this engine has no persistent slot state,
  // so each voice warms its own machine up from a per-voice seed instead of
  // inheriting the slot's, which reproduces "two identical notes differ" and
  // the arbitrary drift phase a note starts with on the device.
  int      _afDepth;                    // 2 * Analog Feel, 0 disables
  int8_t   _afDrift;                    // @0x8572[slot]
  int8_t   _afStepPerTick;              // @0x85FE[slot]
  int8_t   _afTarget;                   // @0x861A[slot]
  int16_t  _afA, _afB;                  // @0x858E / @0x85C6, the two filter words
  int16_t  _afNoise;                    // the modelled chip word
  bool     _afTickPhase;                // the 16 ms task runs every second 8 ms call
  uint32_t _afRng;
  void _af_tick(void);
  int  _af_drift_cents10(void);

  // The JV's pitch envelope and LFO pitch depths (scdb devices/jv880 D-37,
  // FW-EXACT). The firmware's pitch word is in cents; this engine's
  // _targetPitch is in tenths of a cent, so the term lands x10. Segments run
  // 0 -> L1 (T1), L1 -> L2 (T2), L2 -> L3 (T3), hold L3, and on note off the
  // current level -> L4 (T4); the level word is L << 8 and the decrement per
  // 8 ms tick is 2^19 / ms (ROM1 0x1E48 via 0x227B), so a segment lasts its
  // milliseconds. The combine (ROM1 0x40AB) attenuates the level by the
  // velocity word, multiplies by the depth word and adds the two LFO products.
  bool _jv = false;
  int  _jvDepthWord = 0;                // @0x910A[v]
  int  _jvVelAtten = 0;                 // @0x8FBA[v]
  int  _jvLfoDepth[2] = { 0, 0 };       // @0x9452[v], @0x948A[v]
  int  _jvRandCents10 = 0;              // @0x995A[v], the random-pitch draw
  int  _jvLevel[5] = { 0, 0, 0, 0, 0 }; // 0, L1..L4 as L << 8
  int  _jvTime[4] = { 0, 0, 0, 0 };
  int  _jvSeg = 5;                      // 0..2 ramps, 3 hold L3, 4 release, 5 hold L4
  int  _jvPos = 0, _jvDec = 0, _jvFrom = 0, _jvTo = 0, _jvEnvWord = 0;
  void _jv_init(uint8_t velocity);
  void _jv_start_segment(int seg);
  void _jv_step(void);
  int  _jv_cents10(void);


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
