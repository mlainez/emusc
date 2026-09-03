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


#include "envelope.h"

#include <cstdlib>
#include <iostream>


namespace EmuSC {


Envelope::Envelope(ControlRom::LookupTables &LUT)
  : _finished(false),
    _envelopeOut(0),
    _timeKeyFlwT1T4(256),
    _timeKeyFlwT5(256),
    _timeVelSensT1T2(256),
    _timeVelSensT3T5(256),
    _phase(Phase::Init),
    _LUT(LUT)
{}


Envelope::~Envelope()
{}


void Envelope::set_phase(enum Envelope::Phase newPhase)
{
  if (newPhase != _phase)
    _init_new_phase(newPhase);
}


// etkpROM != 0 is only possible for the TVA envelope
void Envelope::set_time_key_follow(enum Type type, bool phase, int key,
                                   int etkfROM, int etkpROM)
{
  // Skip unnecessary calculations if etkfROM value is 0 => use default values
  if (etkfROM == 0) {
    return;
  }

  int offset;
  if (type == Type::TVA)
    offset = (phase == 0) ? 16 : 32;
  else  // TVF
    offset = (phase == 0) ? 64 : 80;

  int kmIndex = _LUT.KeyMapperIndex[offset + etkpROM] - _LUT.KeyMapperOffset;
  int km = _LUT.KeyMapper[kmIndex + key];
  int tkfSens = _LUT.EnvTimeKeyFollowSens[std::abs(etkfROM)];

  if (etkfROM < 0)
    km = (km == 0) ? 255 : (256 - km) & 0xff;

  int tkfIndex = 128 + ((km - 128) * tkfSens * 2 / 256);

  if (phase == 0)
    _timeKeyFlwT1T4 = _LUT.EnvTimeScale[tkfIndex];
  else
    _timeKeyFlwT5 = _LUT.EnvTimeScale[tkfIndex];

  if (0)
    std::cout << "ETKF: TVA phase=" << std::dec << phase
              << " key=" << (int) key << " offset=" << offset
              << " etkpROM=" << etkpROM
              << " km[" << kmIndex + key << "]=" << km
              << " tkf[" << tkfIndex << "]=" << _LUT.EnvTimeScale[tkfIndex]
              << " => time change=" << _LUT.EnvTimeScale[tkfIndex] / 256.0
              << std::endl;
}


void Envelope::set_time_velocity_sensitivity(enum Type type, bool phase,
                                             int etvsROM, int velocity)
{
  int timeVelSens;
  int tvsDiv = _LUT.EnvTimeKeyFollowSens[std::abs(etvsROM)];
  int divmuliv = tvsDiv * (127 - velocity);

  if (etvsROM < 0) {
    if (divmuliv < 8001)
      timeVelSens = ((8128 - divmuliv) * 2064) >> 16;
    else
      timeVelSens = 4;

  } else if (etvsROM > 0) {
    if ((uint16_t) 0x1fc0 - divmuliv < 0x20)
        timeVelSens = 0xffff;
      else
        timeVelSens = 0x1fc000 / (uint16_t) (0x1fc0 - divmuliv);

  } else {  // etvsROM == 0
    timeVelSens = 256;
  }

  if (phase == 0)
    _timeVelSensT1T2 = timeVelSens;
  else
    _timeVelSensT3T5 = timeVelSens;

  if (0)
    std::cout << "ETVS: phase (0:T1-2 1:T3-5)=" << std::dec << phase
              << " etvsROM=" << etvsROM << " velocity=" << velocity
              << " sensitivity=" << timeVelSens << std::endl;
}


// ---------------------------------------------------------------------------
// The JV's envelope TIME-sense law (scdb devices/jv880 D-27, FW-EXACT).
//
// Each JV envelope block carries three 0-14 nibbles, 7 neutral: "T1 velocity",
// "T4 velocity" and "time KF". The firmware's rate routines - ROM1 0x1E48 for
// the pitch envelope, 0x1EF6 for the TVF's and 0x1FA4 for the TVA's, one per
// envelope and identical in structure - apply them to the segment's
// millisecond value out of the time table like this:
//
//   T1      ms = vel(ms, DEPTH[velT1], note-on velocity)          no key-follow
//   T2, T3  ms = kf(ms, KF[timeKf], key)                          no velocity
//   T4      ms = kf(vel(ms, DEPTH[velT4], note-OFF velocity), KF[timeKf], key)
//
// and a segment whose table time is already 0 is skipped before either.
//
// vel() is ROM1 0x2360 and is ADDITIVE in milliseconds:
//
//   ms' = ms + sign(d) * sign(v - 64) * ((|d| * |v - 64|) >> 8)
//
// with d out of DEPTH_5260 (-4000 .. 0 .. +4000): 984 ms either way at the
// extremes. A result of zero or below sets the V flag and the segment is
// skipped; a carry out of sixteen bits makes it the longest segment the
// stepper can run. The manual's wording matches the sign: positive "T1
// velocity" makes T1 LONGER as velocity rises (JV-880 owner's manual p.6-48).
//
// kf() is ROM1 0x22E1 and compounds per semitone group from key 60: with
// k out of PITCH_5280 (+21 .. 0 .. -21) and |key - 60| split into a remainder
// and whole octaves, each group of g semitones does
//
//   up:    ms += (|k| * g * ms) >> 8
//   down:  ms  = (ms << 8) / (256 + |k| * g)
//
// "up" when k and (key - 60) share a sign. The displayed +100 is k = -21, so a
// positive time KF makes higher keys SHORTER - the manual's "positive values:
// the higher the note number, the shorter the time of T2-T4" (p.6-49).
//
// The note-off velocity is the note-off's own data byte. A note-on with
// velocity 0 carries none, and the firmware's parser substitutes 127 for it
// (ROM1 0x6C46) before entering the note-off handler, so a running-status
// note-off is a LOUD release on this machine. A tone whose TVA Delay Time is
// KEY-OFF reads the note-on velocity instead (ROM1 0x1FCD).

// The firmware's carry case is a decrement of 1, i.e. 65535 ticks of 16 ms;
// this stepper's longest representable segment is 65535 control periods of
// 8 ms. Both are minutes and neither is reached by the factory data.
static const int JV_LONGEST_MS = 0xffff * 8;

static int jv_velocity_term(int ms, int depth, int velocity)
{
  const int s = velocity - 64;
  const int term = (std::abs(depth) * std::abs(s)) >> 8;
  if ((depth >= 0) == (s >= 0)) {
    const int v = ms + term;
    return (v > 0xffff) ? JV_LONGEST_MS : v;
  }
  const int v = ms - term;
  return (v <= 0) ? 0 : v;
}


static int jv_key_follow(int ms, int k, int key)
{
  if (k == 0 || ms == 0)
    return ms;

  const int n = key - 60;
  const bool up = (k > 0) == (n >= 0);
  const int kk = std::abs(k);
  int octaves = std::abs(n) / 12;
  int v = ms;

  for (int g = std::abs(n) % 12; ; g = 12) {
    if (up) {
      v += (kk * g * v) >> 8;
      if (v > 0xffff)
        return JV_LONGEST_MS;
    } else {
      v = (v << 8) / (256 + kk * g);
    }
    if (octaves-- == 0)
      break;
  }

  return v;
}


void Envelope::set_jv_time_sense(int velT1Idx, int velT4Idx, int timeKfIdx,
                                 int key, int velocity,
                                 bool releaseUsesNoteOnVelocity)
{
  _jvTimeSense = _LUT.hasJVEnvTimeSense;
  _jvVelT1Idx = velT1Idx & 0x0f;
  _jvVelT4Idx = velT4Idx & 0x0f;
  _jvTimeKfIdx = timeKfIdx & 0x0f;
  _jvKey = key;
  _jvVelocity = velocity & 0x7f;
  _jvReleaseVelocity = 64;
  _jvReleaseUsesNoteOn = releaseUsesNoteOnVelocity;
}


void Envelope::set_jv_release_velocity(int velocity)
{
  _jvReleaseVelocity = velocity & 0x7f;
}


int Envelope::_jv_time_sense(int ms, enum Phase phase) const
{
  if (!_jvTimeSense || ms == 0)
    return ms;

  switch (phase) {
  case Phase::Attack1:                                    // T1
    return jv_velocity_term(ms, _LUT.JVEnvTimeVelDepth[_jvVelT1Idx],
                            _jvVelocity);

  case Phase::Attack2:                                    // T2
  case Phase::Decay1:                                     // T3
    return jv_key_follow(ms, _LUT.JVEnvTimeKeyFollow[_jvTimeKfIdx], _jvKey);

  case Phase::Release: {                                  // T4
    const int vel = _jvReleaseUsesNoteOn ? _jvVelocity : _jvReleaseVelocity;
    const int v = jv_velocity_term(ms, _LUT.JVEnvTimeVelDepth[_jvVelT4Idx],
                                   vel);
    return jv_key_follow(v, _LUT.JVEnvTimeKeyFollow[_jvTimeKfIdx], _jvKey);
  }

  default:            // Decay2 is this engine's extra hold segment, not the JV's
    return ms;
  }
}

}
