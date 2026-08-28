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


#ifndef __PART_H__
#define __PART_H__


#include "control_rom.h"
#include "note.h"
#include "settings.h"
#include "wave_rom.h"

#include <stdint.h>

#include <array>
#include <functional>
#include <list>
#include <mutex>
#include <vector>


namespace EmuSC {

class Part
{
public:
  Part(uint8_t id, Settings *settings, ControlRom &cRom, WaveRom &wRom);
  ~Part();

  // The dry bus is stereo; the two effect buses are mono, because the send is
  // taken from the part's signal before its panner (PROVENANCE.md P-0182) and
  // the chorus and the reverb each take a single input sample.
  int get_sample_set(std::array<std::array<float, 256>, 2> &dryBus,
		     std::array<float, 256> &chorusBus,
		     std::array<float, 256> &reverbBus);
  void update(void);

  int get_last_peak_sample(void);

  // Partials this part is currently using. Partials that have been handed
  // over to another note (damped) are no longer this part's and are not
  // counted.
  int get_num_partials(void);

  // Whether a note on this key would sound on this part at all, and the
  // number of partials it would need
  bool accepts_note(uint8_t key);
  int get_note_partials(uint8_t key);

  // Voice allocation. steal_candidate() names the voice this part would give
  // up if one had to be taken from it: the oldest voice already in its
  // release phase, or, if it has none, its oldest voice. steal_voice() damps
  // that voice and returns the number of partials it released.
  bool steal_candidate(uint32_t &serial, bool &releasing);
  int steal_voice(uint32_t serial, float dBPerMillisecond);

  // Rhythm parts only: silence every note sounding on this part that shares
  // the new key's assign group. Returns the number of partials released.
  int choke_assign_group(uint8_t key, float dBPerMillisecond);

  // MIDI Channel Voice Messages
  int set_program(uint8_t index, int8_t bank = -1, bool ignRxPC = false);
  // startDelay: where inside the current control period the note starts, in
  // samples [0, 256).  See Partial.
  int add_note(uint8_t key, uint8_t velocity, uint32_t serial = 0,
               int startDelay = 0);
  int stop_note(uint8_t key);
  int control_change(uint8_t msgId, uint8_t value);
  int channel_pressure(uint8_t value);
  int poly_key_pressure(uint8_t key, uint8_t value);
  int pitch_bend_change(uint8_t lsb, uint8_t msb, bool force = false);

  // MIDI Channel Mode Messages
  int delete_all_notes(void);
  int stop_all_notes(void);

  void reset(void);

  uint8_t id(void) { return _id; }

  uint8_t midi_channel(void) { return _settings->get_param(PatchParam::RxChannel, _id); }

  // Define callback functions for frontends
  void set_change_callback(std::function<void(const int)> cb);
  void clear_change_callback(void);
  void set_envelope_callback(std::function<void(const float, const float,
                                                const float, const float,
                                                const float, const float)> cb);
  void clear_envelope_callback(void);
  void set_lfo_callback(std::function<void(const int, const int, const int)>cb);
  void clear_lfo_callback(void);

private:
  const uint8_t _id;          // Part id: [0-15] on SC-55, [0-31] on SC-88

  Settings *_settings;

  uint16_t _instrument;       // [0-127] -> variation table
  int8_t _drumSet;            // [0-13] drumSet (SC-55)

  uint8_t _partialReserve;    // [0-24] Default 2

  float _lastPeakSample;

  enum Mode {
    mode_Norm  = 0,
    mode_Drum1 = 1,
    mode_Drum2 = 2
  };

  struct std::list<Note*> _notes;
  std::mutex *_notesMutex;

  ControlRom &_ctrlRom;
  WaveRom &_waveRom;

  // Calculated controller values (minimize number of calculations)
  // TODO: Figure out how to do this properly. Only relevant for pitchBend?
  uint8_t _lastPitchBendRange;

  // Envelopes and LFOs callback for external clients
  std::function<void(const int)> _changeCallback = NULL;
  std::function<void(const float, const float,
                     const int, const int,
                     const float, const float)> _envelopeCallback = NULL;
  std::function<void(const int, const int, const int)> _lfoCallback = NULL;
};

}

#endif  // __PART_H__
