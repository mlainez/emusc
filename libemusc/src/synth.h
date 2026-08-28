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


#ifndef __SYNTH_H__
#define __SYNTH_H__


#include "control_rom.h"
#include "params.h"
#include "resampler.h"
#include "system_effects.h"
#include "wave_rom.h"

#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>


/* Public API for libEmuSC
 *
 * This Synth class is responsible for receiving MIDI events from client,
 * process the event based on information in Control and Wave ROMS, and
 * finally responding to audio buffer requests.
 *
 * Synth class constructor depends on a valid Control Rom and Wave ROM.
 *
 * MIDI events is sent to the emulator via the midi_input() method using the
 * three bytes from raw MIDI events.
 * 
 * Audio samples are extracted by calling the get_next_frame() method. This
 * is typically done from a callback function triggered by the OS audio
 * driver when the audio buffer is running low.
 *
 * All settings are configured through the Settings class.
 */


namespace EmuSC {

class Part;
class Settings;

class Synth
{
public:

  enum class SoundMap {
    GS,                       // Default GS settings
    GS_GM,                    // GS settings in GM mode (available on SC55mk2+)
    MT32                      // MT32 arrangement
  };

  Synth(ControlRom &cRom, WaveRom &pRom, SoundMap map = SoundMap::GS);
  ~Synth();

  // Add start() and stop()? Won't start if sampleRate is not set?

  // MIDI in.  frameOffset says how far in the future the event happens, in
  // output frames counted from the frame get_next_frame() will return next.
  // A client that plays a file or a sequence knows this and can hand events
  // over early; one that is fed a live MIDI stream leaves it at 0 and gets
  // what it always got. The synth applies a note-on at the exact sample it
  // falls on when it can, because the SC-55mkII does (PROVENANCE.md P-0229).
  void midi_input(uint8_t status, uint8_t data1, uint8_t data2,
                  uint32_t frameOffset = 0);
  void midi_input_sysex(uint8_t *data, uint16_t length,
                        uint32_t frameOffset = 0);

  int get_next_frame(float &lOut, float &rOut);
  uint32_t get_num_clipped_samples(bool reset = true);
  std::array<int, 16> get_parts_last_peak_sample(void);

  // Setting audio properties (default is 44100, 2)
  void set_audio_format(uint32_t sampleRate, uint8_t channels);

  void reset(SoundMap sm, bool resetParts = false);

  void panic(void);

  // Returns libEmuSC version as a string
  static std::string version(void);

  void set_part_instrument(uint8_t partId, uint8_t index, uint8_t bank);

  void add_part_midi_mod_callback(std::function<void(const int)> callback);
  void clear_part_midi_mod_callback(void);

  void add_part_change_callback(std::function<void(const int)> callback);
  void clear_part_change_callback(void);

  void set_part_envelope_callback(int partId,
                                  std::function<void(const float, const float,
                                                     const float, const float,
                                                     const float, const float)> callback);
  void clear_part_envelope_callback(int partId);
  void set_part_lfo_callback(int partId,
                             std::function<void(const int, const int, const int)
                             > callback);
  void clear_part_lfo_callback(int partId);

  // EmuSC clients methods for getting synth paramters
  uint8_t  get_param(enum SystemParam sp);
  uint8_t* get_param_ptr(enum SystemParam sp);
  uint16_t get_param_32nib(enum SystemParam sp);
  uint8_t  get_param(enum PatchParam pp, int8_t part = -1);
  uint8_t* get_param_ptr(enum PatchParam pp, int8_t part = -1);
  uint16_t get_param_uint14(enum PatchParam pp, int8_t part = -1);
  uint8_t  get_param_nib16(enum PatchParam pp, int8_t part = -1);
  uint8_t  get_patch_param(uint16_t address, int8_t part = -1);
  uint8_t  get_param(enum DrumParam, uint8_t map, uint8_t key);
  int8_t* get_param_ptr(enum DrumParam, uint8_t map);

  // EmuSC clients methods for setting synth paramters
  void set_param(enum SystemParam sp, uint8_t value);
  void set_param(enum SystemParam sp, uint32_t value);
  void set_param(enum SystemParam sp, uint8_t *data, uint8_t size = 1);
  void set_param_32nib(enum SystemParam sp, uint16_t value);
  void set_param(enum PatchParam pp, uint8_t value, int8_t part = -1);
  void set_param(enum PatchParam sp, uint8_t *data, uint8_t size = 1,
		 int8_t part = -1);
  void set_param_uint14(enum PatchParam pp, uint16_t value, int8_t part = -1);
  void set_param_nib16(enum PatchParam pp, uint8_t value, int8_t part = -1);
  void set_patch_param(uint16_t address, uint8_t value, int8_t part = 1);
  void set_param(enum DrumParam dp, uint8_t map, uint8_t key, uint8_t value);
  void set_param(enum DrumParam dp, uint8_t map, uint8_t *data, uint8_t length);

  /* End of public API. Below are internal data structures only */

private:
  Settings *_settings;
  
  uint32_t _sampleRate;
  uint8_t _channels;

  std::atomic<uint32_t> _numClippedSamples;

  std::mutex midiMutex;

  struct std::vector<Part> _parts;
  uint32_t _noteSerial = 0;   // Note on order, for voice allocation
  std::vector<std::function<void(const int)>> _partMidiModCallbacks;
  std::vector<std::function<void(const int)>> _partChangeCallbacks;

  ControlRom &_ctrlRom;
  WaveRom &_waveRom;

  float _phase;               // Fractional SC-55 sample position
  float _phaseIncrement;      // SC-55 samples per host sample
  int   _updateCounter;       // Counts SC-55 samples, fires at 256

  // Event scheduling, on the internal 32 kHz timeline.  _blockStart is the
  // first sample of the control period about to be generated and
  // _framesDelivered counts the output frames get_next_frame() has returned,
  // which is what a client's frameOffset is measured from.
  uint64_t _blockStart;
  uint64_t _framesDelivered;

  // The input is read at one MIDI byte per 100 us, so a message whose bytes
  // arrive while an earlier message is still being read waits for it. This is
  // the internal sample at which the input becomes free again.
  double _midiInputFree;

  struct PendingEvent {
    uint64_t applyAt;         // internal sample this event acts on
    bool     isSysEx;
    int      startDelay;      // sub-period offset, note-on only
    uint8_t  status, data1, data2;
    std::vector<uint8_t> sysex;
  };
  std::deque<PendingEvent> _eventQueue;

  // MIDI bytes per second on the way in, and the fixed delay from the last
  // byte of a note-on to the first sample of the voice it starts, in internal
  // samples. Both measured on the SC-55mkII (PROVENANCE.md P-0229).
  static constexpr double midiByteSamples = 32000.0 * 0.0001;   // 100 us
  static constexpr double noteOnDelaySamples = 32000.0 * 0.0020; // 2.0 ms

  std::vector<float> _hostSampleBufL;
  std::vector<float> _hostSampleBufR;
  int _hostSampleBufRIndex;
  int _hostSampleBufWIndex;

  std::array<std::array<float, 256>, 2> _dryBus;

  // Mono: each part contributes its signal from before its own panner
  // (PROVENANCE.md P-0182), and each effect takes one input sample per frame.
  std::array<float, 256> _chorusBus;
  std::array<float, 256> _reverbBus;

  std::array<std::array<float, 256>, 2> _chorusOut;
  std::array<std::array<float, 256>, 2> _reverbOut;

  SystemEffects *_systemEffects;
  Resampler *_resampler;

  // MIDI message types
  static const uint8_t midi_NoteOff         = 0x80;
  static const uint8_t midi_NoteOn          = 0x90;
  static const uint8_t midi_PolyKeyPressure = 0xa0;
  static const uint8_t midi_CtrlChange      = 0xb0;
  static const uint8_t midi_PrgChange       = 0xc0;
  static const uint8_t midi_ChPressure      = 0xd0;
  static const uint8_t midi_PitchBend       = 0xe0;

  void _init_parts(void);
// int _export_sample_24(std::vector<int32_t> &sampleSet, std::string filename);
  void _add_note(uint8_t midiChannel, uint8_t key, uint8_t velocity,
                 int startDelay);

  // Put an event on the queue with the internal sample it acts on, and take
  // the queue's head events off again when the period they belong to is
  // generated.
  void _queue_event(uint8_t status, uint8_t data1, uint8_t data2,
                    const uint8_t *sysex, uint16_t sysexLength,
                    uint32_t frameOffset);
  void _dispatch_events(void);
  void _apply_midi(uint8_t status, uint8_t data1, uint8_t data2,
                   int startDelay);
  void _apply_midi_sysex(uint8_t *data, uint16_t length);
  int _partials_in_use(void);
  int _steal_partials(void);

  void _midi_input_sysex_DT1(uint8_t model, uint8_t *data, uint16_t length);

  void _process_samples(void);

  Synth();
};

}

#endif  // __SYNTH_H__
