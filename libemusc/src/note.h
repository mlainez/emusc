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


#ifndef __NOTE_H__
#define __NOTE_H__


#include "control_rom.h"
#include "partial.h"
#include "settings.h"
#include "wave_generator.h"
#include "wave_rom.h"

#include <stdint.h>

#include <array>


namespace EmuSC {

class Note
{
public:
  // startDelay: where inside the current control period this note starts,
  // in samples [0, 256).  See Partial.
  Note(uint8_t key, uint8_t velocity, ControlRom &ctrlRom, WaveRom &waveRom,
       Settings *settings, int8_t partId, uint32_t serial = 0,
       int startDelay = 0);
  ~Note();

  void stop(void);
  void stop(uint8_t key, uint8_t releaseVelocity = 64);
  void sustain(bool state);

  // Voice allocation. A note keeps its partials until they have finished, so
  // a note in its release phase still occupies them; damp() hands them over
  // to a new note while this one fades out.
  void damp(float dBPerMillisecond);

  // The JV-880 takes voices one at a time, not a note at a time: damp_partial()
  // hands over the partial in slot p alone and the rest of the note keeps
  // sounding. partial_live() says whether slot p still sounds for this note,
  // and get_num_partials() does not count a partial that has been handed over.
  bool partial_live(int p);
  int  damp_partial(int p, float dBPerMillisecond);

  uint8_t key(void) { return _key; }
  uint32_t serial(void) { return _serial; }
  bool is_releasing(void) { return _releasing; }
  bool is_damped(void) { return _damped; }

  // Number of partials a note on this key would use in this part, without
  // creating it. Used to decide how many voices a note on needs.
  static int partial_count(ControlRom &ctrlRom, Settings *settings,
                           int8_t partId, uint8_t key);

  void update(void);

  bool get_sample_set(std::array<std::array<float, 256>, 2> &dryBus,
		      std::array<float, 256> &reverbSend,
		      std::array<float, 256> &chorusSend);

  int get_num_partials(void);

  int get_current_pitch(bool partial);
  int get_current_tvf(bool partial);
  int get_current_tva(bool partial);
  int get_current_lfo(int lfo);

private:
  uint8_t _key;

  bool _sustain;
  bool _stopped;
  bool _releasing;           // Note off has released the partials
  bool _damped;              // Partials handed over to another note
  uint8_t _releaseVelocity = 64;  // From the note off; 64 is neutral

  const uint32_t _serial;    // Note on order, for voice allocation

  static uint16_t _instrument_index(ControlRom &ctrlRom, Settings *settings,
                                    int8_t partId, uint8_t key);

  // Each drum instrument sends to the effects at its own depth, so the two
  // sends have to be scaled per note rather than per part. 1.0 on a tonal part.
  float _reverbDepth;
  float _chorusDepth;

  const double _7bScale;     // Constant: 1 / 127

  WaveGenerator *_LFO1;

  struct Partial *_partial[ControlRom::MAX_PARTIALS];

  Settings *_settings;
  int8_t _partId;
};

}

#endif  // __NOTE_H__
