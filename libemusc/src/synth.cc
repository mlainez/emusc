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


#include "synth.h"
#include "part.h"
#include "settings.h"

#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <algorithm>
#include <iomanip>

#include "config.h"


namespace EmuSC {

Synth::Synth(ControlRom &controlRom, WaveRom &waveRom, SoundMap map)
  : _sampleRate(0),
    _channels(0),
    _numClippedSamples(0),
    _ctrlRom(controlRom),
    _waveRom(waveRom),
    _phase(0.0),
    _updateCounter(0),
    _hostSampleBufRIndex(0),
    _hostSampleBufWIndex(0),
    _blockStart(0),
    _framesDelivered(0),
    _midiInputFree(0.0)
{
  srand (static_cast<unsigned>(time(0)));

  _settings = new Settings(controlRom);

  _parts.reserve(16);

  if (map == SoundMap::GS) {
    std::cout << "libEmuSC: GS sound map initialized" << std::endl;
  } else if (map == SoundMap::GS_GM) {
    std::cout << "libEmuSC: GS (GM system) sound map initialized" << std::endl;
    _settings->set_gm_mode();
  } else if (map == SoundMap::MT32) {
    _settings->set_map_mt32();
    std::cout << "libEmuSC: MT-32 sound map initialized" << std::endl;
  }

  _systemEffects = new SystemEffects(_settings, _ctrlRom);
  _resampler = new Resampler();
}


Synth::~Synth()
{
  _parts.clear();
  delete _settings;
  delete _systemEffects;
  delete _resampler;
}


void Synth::_init_parts(void)
{
  for (int i = 0; i < 16; i++)
    _parts.emplace_back(i, _settings, _ctrlRom, _waveRom);
}


// The queue is deliberately left alone here. A reset arrives as an event of
// its own and is applied in order with the rest, so anything still queued
// belongs after it -- a file whose GS reset is followed by a program change
// may well have both in hand at once. It also runs with midiMutex already
// held, from midi_input_sysex()'s own locking.
void Synth::reset(SoundMap sm, bool resetParts)
{
  if (resetParts)
    for (auto &p : _parts) p.reset();   //? TODO: CLEAN UP

  _settings->reset();
  if (sm == SoundMap::GS_GM) {
    _settings->set_gm_mode();
  } else if (sm == SoundMap::MT32) {
    _settings->set_map_mt32();
  }
}


int Synth::_partials_in_use(void)
{
  int partialsUsed = 0;
  for (auto &p: _parts)
    partialsUsed += p.get_num_partials();

  return partialsUsed;
}


// Take one partial from a sounding voice so that a new note can have it.
// Returns the number of partials released, 0 if none may be taken. Measured
// on both models (PROVENANCE.md P-0077..P-0080 and the cross-part entries
// this change cites):
//
//  * the victim is chosen among the parts using more partials than their own
//    voice reserve, and it is the one with the highest part number, counted
//    in the order of the voice-reserve table (40 01 10, SC-55mkII OM p.98):
//    part 16 first, down through part 1, with the rhythm part -- part 10,
//    the table's first byte -- given up last of all. The part asking for the
//    voice gets no preference, and neither note age nor release state plays
//    any role in choosing the part;
//  * within the chosen part, a voice already in its release phase goes
//    before a voice still held, and the oldest goes first in each group;
//  * a part at or below its reserve is never taken from: when no part is
//    over its reserve, there is nothing to take and the new note does not
//    sound at all.
//
// The reserve defaults sum to 24 on both models, so on the SC-55mkII four
// partials are free for whichever part asks first (SC-55 OM p.79,
// SC-55mkII OM p.98; PROVENANCE.md P-0077, P-0078, P-0079).
int Synth::_steal_partials(Part &requester)
{
  if (_settings->generation() == ControlRom::SynthGen::JV880)
    return _steal_partial_jv(requester);

  Part *victim = NULL;
  int victimRank = -1;
  uint32_t victimSerial = 0;

  for (auto &p: _parts) {
    if (p.get_num_partials() <= _settings->get_partial_reserve(p.id()))
      continue;

    uint32_t s = 0;
    bool r = false;
    if (!p.steal_candidate(s, r))
      continue;

    // The part's number in the voice-reserve table's order: part 10 (the
    // rhythm part, id 9) is the table's byte 0, parts 1-9 are bytes 1-9 and
    // parts 11-16 are bytes 10-15.
    int rank = (p.id() == 9) ? 0 : (p.id() < 9 ? p.id() + 1 : p.id());

    (void) r;                           // the release state picks the voice
                                        // within the part, never the part
    if (rank > victimRank) {
      victim = &p;
      victimRank = rank;
      victimSerial = s;
    }
  }

  if (!victim)
    return 0;

  return victim->steal_voice(victimSerial, _ctrlRom.voice_damp_rate());
}


// The JV-880's policy, from its firmware (scdb devices/jv880/06_voice_engine/
// allocation.md, D-44 and D-67). Task 4 (ROM1 0x0892) takes a voice from lists
// that Task 8 (ROM1 0x1AC8) rebuilds each round from a master list of all 28
// voices: first a free voice; then the head of list B, which holds, for every
// part, its first (active - reserve) voices in MASTER order; then the
// requesting part's own list; and when the part has none of its own, no voice
// at all.
//
// The master list is NOT plain allocation order. Task 8 sorts it by two of the
// allocator's masks before walking it (ROM1 0x1C31-0x1C89): voices whose GATE
// bit @0x8128 is clear go first, then gated voices whose key-down bit @0x812C
// is clear, then the rest - each group in allocation order. The allocator sets
// both bits for a new voice (0xAFE, 0xB0D); the note-off routine (0x1878-
// 0x19FC) clears the gate, or only the key-down bit when the tone's Hold-1
// switch is on and the part's hold pedal (@0x6176) is down; the envelope task
// reads the gate as its release trigger (0x2612). So a RELEASED voice is taken
// before a pedal-held one, and a pedal-held one before a key that is still
// down, oldest first inside each group. A released voice stays in the pool
// until its T4 countdown ends (0x2821), so it still counts against the 28.
//
// Measured on the reference (scdb M-061, D-67): with 28 voices sounding and
// one of them released 0.1-2 s earlier, a new note takes the released voice
// and cuts its tail; with all 28 held it takes the oldest. Demo song 3
// "Synthony" plays 0.1 s chords whose tails run 2.5 s under a held pad, and
// ordering by age alone stole the pad 283 times where the device never does.
//
//  * one VOICE is taken, not a note: the note it came from keeps its other
//    tones (ROM1 0xE1B re-links the voice into the new note's chain and the
//    old note's chain closes around the gap). Tone 4 of a note was allocated
//    first (ROM1 0xA72, r3 = 3 down to 0), so it is the oldest of its note;
//  * the reserve is the loaded performance's Voice Reserve (Performance
//    Common +0x14..+0x1B, copied to @0x8C53 at ROM1 0x2BA6), a floor no other
//    part can reach under. With every part at or under its reserve the
//    requesting part recycles the head of its own list rather than staying
//    silent.
//
// The firmware's list is rebuilt once per Task 8 round rather than per voice;
// recomputing it here for every request idealises that. What the chip does to
// a voice re-keyed while it sounds is silicon, so the stolen partial fades at
// the device's damp rate.
int Synth::_steal_partial_jv(Part &requester)
{
  // EMUSC_DEBUG_STEAL prints every steal with the internal time, the pool
  // occupancy, the requesting part and the victim, so a mix that loses a voice
  // can be told apart from one that never had it. Diagnostics only.
  static const bool dbg = getenv("EMUSC_DEBUG_STEAL") != nullptr;
  struct Candidate { int gate; uint32_t serial; int slot; Part *part; };
  std::vector<Candidate> master;
  std::map<Part *, int> excess;

  for (auto &p : _parts) {
    std::vector<Part::LivePartial> live;
    p.live_partials(live);                       // oldest first
    for (auto &l : live)
      master.push_back({l.gate, l.serial, l.slot, &p});
    excess[&p] = (int) live.size() - _ctrlRom.device_voice_reserve(p.id());
  }

  // Master order: released, then pedal-held, then key down; allocation order
  // inside each group (the lowest note serial, and within one note the
  // highest slot, which was allocated first).
  std::stable_sort(master.begin(), master.end(),
                   [](const Candidate &a, const Candidate &b) {
                     if (a.gate != b.gate) return a.gate < b.gate;
                     if (a.serial != b.serial) return a.serial < b.serial;
                     return a.slot > b.slot;
                   });

  // List B: walking the master order, a voice is stealable while its part is
  // still over its reserve. The head of that list is the victim.
  const Candidate *victim = nullptr;
  for (auto &c : master) {
    int &ex = excess[c.part];
    if (ex > 0) { ex--; victim = &c; break; }
  }

  // The requesting part's own list: its first voice in master order.
  const Candidate *own = nullptr;
  if (!victim)
    for (auto &c : master)
      if (c.part == &requester) { own = &c; break; }

  if (dbg) {
    std::cerr << "steal t=" << (_blockStart / 32000.0) << "s inUse="
              << _partials_in_use() << " req=part" << requester.id() + 1;
    if (victim)
      std::cerr << " victim=part" << victim->part->id() + 1 << " serial="
                << victim->serial << " slot=" << victim->slot
                << " gate=" << victim->gate;
    else if (own)
      std::cerr << " victim=OWN serial=" << own->serial << " gate=" << own->gate;
    else
      std::cerr << " victim=NONE";
    std::cerr << " live/reserve=";
    for (auto &p : _parts) {
      std::vector<Part::LivePartial> live;
      p.live_partials(live);
      if (!live.empty() || _ctrlRom.device_voice_reserve(p.id()))
        std::cerr << " p" << p.id() + 1 << ":" << live.size() << "/"
                  << (int) _ctrlRom.device_voice_reserve(p.id());
    }
    std::cerr << std::endl;
  }

  if (victim)
    return victim->part->damp_partial(victim->serial, victim->slot,
                                      _ctrlRom.voice_damp_rate());

  if (own)
    return requester.damp_partial(own->serial, own->slot,
                                  _ctrlRom.voice_damp_rate());

  return 0;
}


void Synth::_add_note(uint8_t midiChannel, uint8_t key, uint8_t velocity,
                     int startDelay)
{
  // The Sound Canvas has a fixed number of partials: 24 on the SC-55 ("Maximum
  // Polyphony 24 (partials)", SC-55 OM p.86) and 28 on the SC-55mkII
  // (SC-55mkII OM p.98). A note needs one partial per partial of its tone, and
  // when they are all in use it takes them from voices that are sounding.
  const int maxPolyphony = _ctrlRom.max_polyphony();

  for (auto &p: _parts) {
    if (p.midi_channel() != midiChannel)
      continue;

    int needed = p.get_note_partials(key);
    if (needed <= 0)                     // Undefined tone: nothing will sound
      continue;

    // A drum sound in an assign group silences the others in its group before
    // it takes a voice of its own, so the partials it frees are available to
    // it (SC-55mkII OM p.88-89; PROVENANCE.md P-0085)
    p.choke_assign_group(key, _ctrlRom.voice_damp_rate());

    // A JV-880 part whose patch is Key Assign SOLO plays one note at a time:
    // the note it is sounding gives up its voices to the new key before the
    // pool is consulted (ROM1 0x9D6 path; scdb devices/jv880/06_voice_engine/
    // note_lifecycle.md, D-44). Solo Legato ON (re-pitch without retrigger)
    // and the hand-back to a still-held key on release are not modelled yet.
    if (_settings->generation() == ControlRom::SynthGen::JV880 &&
        _settings->get_param(PatchParam::PolyMode, p.id()) == 0 &&
        _settings->get_param(PatchParam::UseForRhythm, p.id()) == 0)
      p.solo_release(_ctrlRom.voice_damp_rate());

    bool haveVoices = true;
    while (_partials_in_use() + needed > maxPolyphony) {
      if (_steal_partials(p) <= 0) {
        haveVoices = false;
        break;
      }
    }

    if (haveVoices)
      p.add_note(key, velocity, ++_noteSerial, startDelay);
  }
}

/* Not used -> WaveRom as part of sample dump to disk
int Synth::_export_sample_24(std::vector<int32_t> &sampleSet,
			     std::string filename)
{
  int bytesPerSample = 3;
  int numSamples = sampleSet.size();
  uint8_t waveHeader[] = { 'R', 'I', 'F', 'F',      0x00, 0x00, 0x00, 0x00,
                           'W', 'A', 'V', 'E',       'f',  'm',  't',  ' ',
			   0x10, 0x00, 0x00, 0x00,  0x01, 0x00, 0x01, 0x00,
		           0x00, 0x7d, 0x00, 0x00,  0x00, 0x77, 0x01, 0x00,
	      	           0x03, 0x00, 0x18, 0x00,   'd',  'a',  't',  'a',
       		           0x00, 0x00, 0x00, 0x00 };

  waveHeader[4] = ((numSamples * bytesPerSample) + 36) & 0xff;
  waveHeader[5] = (((numSamples * bytesPerSample) + 36) >> 8) & 0xff;
  waveHeader[6] = (((numSamples * bytesPerSample) + 36) >> 16) & 0xff;
  waveHeader[7] = (((numSamples * bytesPerSample) + 36) >> 24) & 0xff;

  waveHeader[40] = (numSamples * bytesPerSample) & 0xff;
  waveHeader[41] = ((numSamples * bytesPerSample) >> 8) & 0xff;
  waveHeader[42] = ((numSamples * bytesPerSample) >> 16) & 0xff;
  waveHeader[43] = ((numSamples * bytesPerSample) >> 24) & 0xff;

  std::ofstream waveFile(filename, std::ios::binary | std::ios::out);
  if (!waveFile.is_open())
    return -1;

  waveFile.write((char *) waveHeader, 44);

  uint8_t sample[bytesPerSample];
  for (uint32_t s : sampleSet) {
    sample[0] = (uint8_t) (s >> 8); 
    sample[1] = (uint8_t) (s >> 16);
    sample[2] = (uint8_t) (s >> 24);
    
    waveFile.write((char *) sample, 3);
  }

  waveFile.close();
  if (!waveFile)
    return -1;

  return 0;
}
*/


// True only for the JV-880's control channel, which the device profile names
// (0-based; 16 = off, as the firmware's own 0x10 does). Never true on a Sound
// Canvas, so the SC-55 path is untouched.
bool Synth::_jv_control_channel(uint8_t channel) const
{
  if (_settings->generation() != ControlRom::SynthGen::JV880)
    return false;
  const auto *rec = _ctrlRom.device()->records;
  if (!rec) return false;
  const int cc = rec->performance.controlChannel;
  return cc >= 0 && cc < 16 && channel == (uint8_t) cc;
}


void Synth::midi_input(uint8_t status, uint8_t data1, uint8_t data2,
                       uint32_t frameOffset)
{
  midiMutex.lock();
  _queue_event(status, data1, data2, NULL, 0, frameOffset);
  midiMutex.unlock();
}


// Where on the internal timeline an event handed over now, with this offset,
// belongs.  Resampler::output_advance() is the constant by which the output
// stream runs ahead of internal time; adding it here and letting the
// resampler take it out again makes the delays below plain output-domain
// latencies rather than numbers that only make sense inside the engine.
void Synth::_queue_event(uint8_t status, uint8_t data1, uint8_t data2,
                         const uint8_t *sysex, uint16_t sysexLength,
                         uint32_t frameOffset)
{
  if (_sampleRate == 0)
    return;

  const double arrives =
    (double) (_framesDelivered + frameOffset) * 32000.0 / (double) _sampleRate
    + Resampler::output_advance();

  // One byte per 100 us, and the message acts when its last byte is in.
  const int bytes = sysex ? (int) sysexLength
                          : (((status & 0xf0) == 0xc0 ||
                              (status & 0xf0) == 0xd0) ? 2 : 3);
  double ready = ((arrives > _midiInputFree) ? arrives : _midiInputFree)
                 + bytes * midiByteSamples;
  _midiInputFree = ready;

  PendingEvent e;
  e.isSysEx = (sysex != NULL);
  e.status = status;
  e.data1 = data1;
  e.data2 = data2;
  if (sysex)
    e.sysex.assign(sysex, sysex + sysexLength);

  // A note-on starts a voice, and a voice can start on any sample: it acts a
  // fixed delay after its last byte. Everything else changes a parameter, and
  // the SC-55mkII applies those on its control period like this engine does,
  // so they take effect on the next period boundary at or after the message.
  const bool noteOn = ((status & 0xf0) == 0x90) && (data2 != 0);
  if (noteOn) {
    e.applyAt = (uint64_t) llround(ready + noteOnDelaySamples);
  } else {
    uint64_t n = (uint64_t) llround(ready);
    e.applyAt = ((n + 255) / 256) * 256;
  }
  e.startDelay = 0;

  _eventQueue.push_back(e);
}


// Take off the queue everything that belongs to the control period about to
// be generated. An event that was handed over too late to be placed exactly
// acts at the start of the period, which is what this engine has always done.
//
// The events are taken off the queue under the lock and applied without it,
// because applying one can re-enter the synth: a GS reset arrives as a system
// exclusive message and calls reset(), which touches the queue itself.
void Synth::_dispatch_events(void)
{
  const uint64_t blockEnd = _blockStart + 256;
  std::vector<PendingEvent> due;

  midiMutex.lock();
  while (!_eventQueue.empty() && _eventQueue.front().applyAt < blockEnd) {
    due.push_back(_eventQueue.front());
    _eventQueue.pop_front();
  }
  midiMutex.unlock();

  for (PendingEvent &e : due) {
    int startDelay = (e.applyAt > _blockStart)
                     ? (int) (e.applyAt - _blockStart) : 0;

    if (e.isSysEx)
      _apply_midi_sysex(e.sysex.data(), (uint16_t) e.sysex.size());
    else
      _apply_midi(e.status, e.data1, e.data2, startDelay);
  }
}


void Synth::_apply_midi(uint8_t status, uint8_t data1, uint8_t data2,
                        int startDelay)
{
  uint8_t channel = status & 0x0f;

  switch (status & 0xf0)
    {
    case midi_NoteOff:
      // The note-off's data byte is the release velocity. Only the JV's
      // envelope time sense consumes it so far (scdb D-27).
      for (auto &p: _parts)
	if (p.midi_channel() == channel)
	  p.stop_note(data1, data2);
      break;

    case midi_NoteOn:
      if (!data2) {                   // Note On with velocity = 0 => Note Off
	// This form carries no release velocity. The JV-880's parser writes
	// 127 into the byte before entering its note-off handler (ROM1
	// 0x6C46), so that is what its release sees; nothing else reads it.
	for (auto &p: _parts)
	  if (p.midi_channel() == channel)
	    p.stop_note(data1, 127);
      } else {
	_add_note(channel, data1, data2, startDelay);
      }
      break;

    case midi_PolyKeyPressure:
      for (auto &p: _parts) {
	if (p.midi_channel() == channel)
	  p.poly_key_pressure(data1, data2);
      }
      break;

    case midi_CtrlChange:
      // Bank select on the control channel sets the flag the next Performance
      // select uses. The firmware acts on two values only - CC0 80 stores 0x00
      // and 81 stores 0x80 - and the flag PERSISTS across program changes
      // (ROM1 0x749C, kept in @0x65B7 for this destination).
      if (data1 == 0 && _jv_control_channel(channel)) {
	if (data2 == 80)      _perfBankFlag = 0x00;
	else if (data2 == 81) _perfBankFlag = 0x80;
	break;
      }

      for (auto &p: _parts) {
	if (p.midi_channel() == channel) {
	  if (p.control_change(data1, data2)) {
	    for (const auto &cb : _partMidiModCallbacks)
	      cb(p.id());
	  }
	}
      }
      break;

    case midi_PrgChange:
      // On the JV-880 the control channel is not a part. A program change there
      // selects a whole PERFORMANCE - every part's patch, receive channel,
      // level, pan, coarse tune and the common effects at once (ROM1 0x6D83,
      // ROM2 0x301B1). Every demo song opens with CC0 81 then PC 72 here, which
      // is Preset B 8 "for CompuMix". scdb D-46.
      if (_jv_control_channel(channel)) {
	if (_settings->select_performance(_perfBankFlag | data1))
	  for (auto &p : _parts)
	    for (const auto &cb : _partMidiModCallbacks)
	      cb(p.id());
	break;
      }

      for (auto &p : _parts)
	if (p.midi_channel() == channel) {
	  p.set_program(data1);

	  for (const auto &cb : _partMidiModCallbacks)
	    cb(p.id());
	}
      break;

    case midi_ChPressure:
      for (auto &p: _parts) {
	if (p.midi_channel() == channel)
	  p.channel_pressure(data1);
      }
      break;

    case midi_PitchBend:
      for (auto &p: _parts) {
	if (p.midi_channel() == channel)
	  p.pitch_bend_change(data1, data2);
      }
      break;

    default:
      std::cout << "EmuSC MIDI: Unknown event received" << std::endl;
      break;
    }
}


void Synth::midi_input_sysex(uint8_t *data, uint16_t length,
                             uint32_t frameOffset)
{
  midiMutex.lock();
  _queue_event(0xf0, 0, 0, data, length, frameOffset);
  midiMutex.unlock();
}


void Synth::_apply_midi_sysex(uint8_t *data, uint16_t length)
{
  // First check if SysEx messages has been disabled
  if (!_settings->get_param(SystemParam::RxSysEx))
    return;
  
  if (data[0] != 0xf0 || data[length - 1] != 0xf7)
    return;

  // Universal non-realtime "Turn General MIDI System On":
  //   F0H 7EH 7FH 09H 01H F7H
  // 7EH universal non-realtime, 7FH broadcast, sub-ID#1 09H General MIDI
  // message, sub-ID#2 01H General MIDI On (SC-55mkII Owner's Manual p.93).
  // The manual states the message sets every internal parameter to the
  // General MIDI System Level 1 defaults, takes about 50 ms to execute, and
  // is ignored when Rx.GM On is off. Only the broadcast device ID triggers
  // it; a message carrying any other device ID is ignored (P-0093).
  //
  // What it leaves behind differs from the GS reset below only in the two
  // receive switches Settings::set_gm_mode() handles -- see the comment
  // there.
  if (length == 6 && data[1] == 0x7e && data[2] == 0x7f &&
      data[3] == 0x09 && data[4] == 0x01) {
    if (!_settings->get_param(SystemParam::RxGMOn))
      return;

    midiMutex.lock();
    reset(SoundMap::GS_GM, true);
    midiMutex.unlock();
    return;
  }

  // Universal realtime "Master Volume":
  //   F0H 7FH 7FH 04H 01H llH mmH F7H
  // 7FH universal realtime, 7FH broadcast, sub-ID#1 04H Device Control,
  // sub-ID#2 01H Master Volume, "The LSB (llH) is ignored (value = 0)"
  // (SC-55mkII Owner's Manual p.93; the byte list on that page misprints
  // sub-ID#2 as 02H, but the message line above it reads 01H and only 01H
  // is acted upon -- P-0109).
  //
  // The same manual's system parameter table prints this message as a second
  // way of writing the GS master volume: "40 00 04 ... MASTER VOLUME 0 - 127
  // (= F0 7F 7F 04 01 00 vv F7)" (mkII p.97), and measurement agrees. The two
  // share one register, one level curve and one default, so acting on the
  // message is a write to SystemParam::Volume, which every part already
  // scales its level by (P-0108). Only the broadcast device ID triggers it,
  // as for the message above, and the LSB is discarded: the rendered audio is
  // byte-identical for every llH (P-0107).
  //
  // The SC-55 ignores this message while honouring the GS parameter, so it is
  // gated on the generation (P-0109). Its manual is consistent: it lists
  // MASTER VOLUME 40 00 04 with no universal equivalent (SC-55 OM p.78) and
  // its exclusive section covers manufacturer ID 41H only (p.77).
  if (length == 8 && data[1] == 0x7f && data[2] == 0x7f &&
      data[3] == 0x04 && data[4] == 0x01) {
    if (_ctrlRom.generation() == ControlRom::SynthGen::SC55)
      return;

    midiMutex.lock();
    _settings->set_param(SystemParam::Volume, data[6]);

    for (const auto &cb : _partMidiModCallbacks)     // Update user interface
      cb(-1);

    midiMutex.unlock();
    return;
  }

  // Everything below is a Roland exclusive message: Manufacturer ID 0x41
  if (data[1] != 0x41)
    return;

  // Verify correct SysEx Device ID
  if (data[2] != _settings->get_param(SystemParam::DeviceID) - 1)
    return;
  
  // Verify valid Model IDs: GSstandard (0x42), SC-55/88 (0x45) or JV-880
  // (0x46). The JV's own ID was missing, so every SysEx a JV file sends was
  // rejected here before the DT1 handler could see it - and two demo songs
  // depend on those writes. scdb D-57.
  if (data[3] != 0x42 && data[3] != 0x45 && data[3] != 0x46)
    return;

  // Verify checksum (assuming 1 byte Device ID)
  int checksum = 0;
  for (int i = 5; i < length - 2; i++)
    checksum += (int) data[i];
  while (checksum >= 128)
    checksum -= 128;
  // The manual's rule (SC-55mkII OM p.104) gives 128 when the remainder is 0,
  // but a transmitted byte is 7 bit, so the byte sent in that case is 0 - the
  // only 7-bit value that satisfies the property the SC-55 OM p.73 states,
  // "the least significant 7 bits are zero when values for an address, size,
  // and that checksum are summed". Comparing against an unmasked 128 rejected
  // every message whose checksum lands on zero, one address/value combination
  // in 128. Measured on the SC-55mkII (PROVENANCE.md P-0180): reverb time 0x0b
  // (address 40 01 34, checksum 00) is accepted by the hardware and shortens
  // the tail to T60 = 0.42 s, between the 0.40 s of time 0x0a and the 0.44 s
  // of time 0x0c, while libEmuSC discarded it and stayed at the default 1.93 s.
  if (data[length - 2] != ((128 - checksum) & 0x7f)) {
    std::cerr << "libEmuSC: Roland SysEx message received with corrupt "
	      << "checksum. Message discarded." << std::endl;
    return;
  }

  if (1) {
    std::cout << "libEmuSC: Valid SysEx  message received: ";
    for (int i = 0; i < length; i ++)
      std::cout << std::hex << (int) data[i] << " " << std::flush;
    std::cout << std::endl;
  }

  if (data[4] == 0x11) {
    std::cerr << "SysEx responses are not implemented yet" << std::endl;
    return;
  }

  midiMutex.lock();

  // Request data 1 (RQ1)
//  if (data[4] == 0x11)
//  _midi_input_sysex_RQ1(&data[5], length - 5 - 2); // Add reply data buffer

  // Data set 1 (DT1)
  if (data[4] == 0x12)
    _midi_input_sysex_DT1(data[3], &data[5], length - 5 - 2);

  midiMutex.unlock();
}


int Synth::get_next_frame(float &lOut, float &rOut)
{
  // If samplerate is not set, just return silence
  if (_sampleRate == 0) {
    lOut = rOut = 0;
    return 0;
  }

  // We are out of samples, trigger new control update + 256 samples @ 32 kHz
  if (_hostSampleBufWIndex == _hostSampleBufRIndex) {
    _process_samples();
    _hostSampleBufRIndex = 0;
  }

  // Check if sound is too loud => clipping
  if (_hostSampleBufL[_hostSampleBufRIndex] > 1.0f ||
      _hostSampleBufL[_hostSampleBufRIndex] < -1.0f)
    _numClippedSamples.fetch_add(1, std::memory_order_relaxed);

  if (_hostSampleBufR[_hostSampleBufRIndex] > 1.0f ||
      _hostSampleBufR[_hostSampleBufRIndex] < -1.0f)
    _numClippedSamples.fetch_add(1, std::memory_order_relaxed);

  lOut = std::clamp(_hostSampleBufL[_hostSampleBufRIndex], -1.0f, 1.0f);
  rOut = std::clamp(_hostSampleBufR[_hostSampleBufRIndex], -1.0f, 1.0f);
  _hostSampleBufRIndex++;
  _framesDelivered++;

  return 0;
}


uint32_t Synth::get_num_clipped_samples(bool reset)
{
  if (reset)
    return _numClippedSamples.exchange(0, std::memory_order_relaxed);

  return _numClippedSamples.load(std::memory_order_relaxed);
}


// Do a control update and read 256 samples
void Synth::_process_samples(void)
{
  // Everything the client has handed over that belongs to this control period
  // is applied first, so the update below sees it - the order this engine has
  // always used, now with the events sorted onto the period they belong to.
  _dispatch_events();

  // Start all samples processings with a control updates
  for (auto &p : _parts)
    p.update();

  _systemEffects->update();

  // Clear all relative buffers before accumulating new samples
  for (int i = 0; i < 2; i++)
    _dryBus[i].fill(0.0f);
  _chorusBus.fill(0.0f);
  _reverbBus.fill(0.0f);
  std::fill(_hostSampleBufL.begin(), _hostSampleBufL.end(), 0.0f);
  std::fill(_hostSampleBufR.begin(), _hostSampleBufR.end(), 0.0f);

  midiMutex.lock();

  _hostSampleBufWIndex = 0;

  // Iterate all parts and ask for next sample
  for (auto &p : _parts) {
    p.get_sample_set(_dryBus, _chorusBus, _reverbBus);
  }

  // Add system effects
  _systemEffects->apply(_chorusBus, _reverbBus, _chorusOut, _reverbOut);

  // Work through the dryBus and genereate samples adapted to host's sample rate
  for (int i = 0; i < 256; i++) {

    float l = _dryBus[0][i] + _chorusOut[0][i] + _reverbOut[0][i];
    float r = _dryBus[1][i] + _chorusOut[1][i] + _reverbOut[1][i];
    _resampler->push(l, r);

    float hostL = 0, hostR = 0;
    while (_resampler->get_next_sample(hostL, hostR)) {
      if (_hostSampleBufWIndex < _hostSampleBufL.size()) {
	_hostSampleBufL[_hostSampleBufWIndex] += hostL;
	_hostSampleBufR[_hostSampleBufWIndex] += hostR;
	_hostSampleBufWIndex++;
      }
    }
  }

  midiMutex.unlock();

  _blockStart += 256;
}


std::array<int, 16> Synth::get_parts_last_peak_sample(void)
{
  std::array<int, 16> partVolumes;

  int i = 0;
  for (auto &p : _parts)
    partVolumes[i++] = p.get_last_peak_sample();

  return partVolumes;
}


void Synth::set_audio_format(uint32_t sampleRate, uint8_t channels)
{
  _resampler->set_sample_rate(sampleRate);
  _settings->set_sample_rate(sampleRate);
  _settings->set_channels(channels);

  _sampleRate = sampleRate;
  _channels = channels;

  midiMutex.lock();
  _eventQueue.clear();
  _blockStart = 0;
  _framesDelivered = 0;
  _midiInputFree = 0.0;
  midiMutex.unlock();

  _init_parts();

  _hostSampleBufL.resize(std::ceil(256 * sampleRate / 32000.0) + 1);
  _hostSampleBufR.resize(std::ceil(256 * sampleRate / 32000.0) + 1);
}


std::string Synth::version(void)
{
  return VERSION;
}


void Synth::panic(void)
{
  midiMutex.lock();
  _eventQueue.clear();
  midiMutex.unlock();

  for (auto &p : _parts)
    p.delete_all_notes();
}


void Synth::set_part_instrument(uint8_t partId, uint8_t index, uint8_t bank)
{
  _parts[partId].set_program(index, bank, true);
}


void Synth::add_part_midi_mod_callback(std::function<void(const int)> callback)
{
  _partMidiModCallbacks.push_back(callback);
}


void Synth::clear_part_midi_mod_callback(void)
{
  _partMidiModCallbacks.clear();
}


void Synth::add_part_change_callback(std::function<void(const int)> callback)
{
  _settings->set_part_callback(callback);
}


void Synth::clear_part_change_callback()
{
  _settings->clear_part_callback();
}


void Synth::set_part_envelope_callback(int partId,
                                       std::function<void(const float, const float, const float, const float, const float, const float)> callback)
{
  _parts[partId].set_envelope_callback(callback);
}

void Synth::clear_part_envelope_callback(int partId)
{
  _parts[partId].clear_envelope_callback();
}


void Synth::set_part_lfo_callback(int partId,
                                  std::function<void(const int, const int,
                                                     const int)> callback)
{
  _parts[partId].set_lfo_callback(callback);
}

void Synth::clear_part_lfo_callback(int partId)
{
  _parts[partId].clear_lfo_callback();
}


uint8_t Synth::get_param(enum SystemParam sp)
{
  return _settings->get_param(sp);
}


uint8_t* Synth::get_param_ptr(enum SystemParam sp)
{
  return _settings->get_param_ptr(sp);
}


uint16_t Synth::get_param_32nib(enum SystemParam sp)
{
  return _settings->get_param_32nib(sp);
}


uint8_t  Synth::get_param(enum PatchParam pp, int8_t part)
{
  return _settings->get_param(pp, part);
}


uint8_t* Synth::get_param_ptr(enum PatchParam pp, int8_t part)
{
  return _settings->get_param_ptr(pp, part);
}


uint16_t Synth::get_param_uint14(enum PatchParam pp, int8_t part)
{
  return _settings->get_param_uint14(pp, part);
}


uint8_t Synth::get_param_nib16(enum PatchParam pp, int8_t part)
{
  return _settings->get_param_nib16(pp, part);
}


uint8_t Synth::get_patch_param(uint16_t address, int8_t part)
{
  return _settings->get_patch_param(address, part);
}


uint8_t Synth::get_param(enum DrumParam dp, uint8_t map, uint8_t key)
{
  return _settings->get_param(dp, map, key);
}


int8_t* Synth::get_param_ptr(enum DrumParam dp, uint8_t map)
{
  return _settings->get_param_ptr(dp, map);
}


void Synth::set_param(enum SystemParam sp, uint8_t value)
{
  _settings->set_param(sp, value);
}


void Synth::set_param(enum SystemParam sp, uint32_t value)
{
  _settings->set_param(sp, value);
}


void Synth::set_param(enum SystemParam sp, uint8_t *data, uint8_t size)
{
  _settings->set_param(sp, data, size);

}


void Synth::set_param_32nib(enum SystemParam sp, uint16_t value)
{
  _settings->set_param_32nib(sp, value);
}


void Synth::set_param(enum PatchParam pp, uint8_t value, int8_t part)
{
  _settings->set_param(pp, value, part);
}


void Synth::set_param(enum PatchParam pp, uint8_t *data, uint8_t size,
		      int8_t part)
{
  _settings->set_param(pp, data, size, part);
}


void Synth::set_param_uint14(enum EmuSC::PatchParam pp, uint16_t value,
			     int8_t part)
{
  _settings->set_param_uint14(pp, value, part);
}


void Synth::set_param_nib16(enum PatchParam pp, uint8_t value, int8_t part)
{
  _settings->set_param_nib16(pp, value, part);
}


void Synth::set_patch_param(uint16_t address, uint8_t value, int8_t part)
{
  _settings->set_patch_param(address, value, part);
}


void Synth::set_param(enum DrumParam dp, uint8_t map, uint8_t key,uint8_t value)
{
  _settings->set_param(dp, map, key, value);
}


void Synth::set_param(enum DrumParam dp, uint8_t map, uint8_t *data,
		      uint8_t length)
{
  _settings->set_param(dp, map, data, length);
}


// TODO: Verify length when expexted data length is known!!
// The JV-880's DT1 address map, for the blocks this port can act on. The
// address is four bytes: 00 00 10 pp is Performance Common parameter pp, and
// 00 00 18+n pp is Performance PART n+1 (n = 0..7).
//
// Nothing of this was reaching the engine: the DT1 handler only ever accepted
// model 0x42, the Sound Canvas, so every SysEx a JV file sends was discarded.
// Two demo songs depend on it. "The Race" opens by writing Part Level 105 to
// all eight parts, and ignoring that left our render 6.27 dB hot; putting the
// same 105 in the ROM so both sides see it brings the gap to 1.91 dB, so this
// one parameter was worth 4.36 dB. "Lost Weekend" writes part parameter 0x17
// on seven parts. scdb D-57.
//
// Only parameters this project has identified are acted on. An unknown one is
// ignored rather than guessed at, because a wrong write is worse than none.
static bool jv_dt1_common(Settings *settings, uint8_t prm, uint8_t val)
{
  switch (prm) {
    case 0x0d: settings->set_param(PatchParam::ReverbCharacter, (uint8_t)(val & 0x07)); return true;
    case 0x0e: settings->set_param(PatchParam::ReverbLevel, val); return true;
    case 0x0f: settings->set_param(PatchParam::ReverbTime, val); return true;
    case 0x10: settings->set_param(PatchParam::ReverbDelayFeedback, val); return true;
    case 0x11: settings->set_param(PatchParam::ChorusMacro, (uint8_t)(val & 0x07)); return true;
    case 0x12: settings->set_param(PatchParam::ChorusLevel, val); return true;
    case 0x13: settings->set_param(PatchParam::ChorusDepth, val); return true;
    case 0x14: settings->set_param(PatchParam::ChorusRate, val); return true;
    case 0x15: settings->set_param(PatchParam::ChorusFeedback, val); return true;
    case 0x16: settings->set_param(PatchParam::ChorusSendToReverb,
                                   (uint8_t)(val ? 0x7f : 0)); return true;
    default: return false;
  }
}


static bool jv_dt1_part(Settings *settings, int part, uint8_t prm, uint8_t val)
{
  switch (prm) {
    case 0x19: settings->set_param(PatchParam::PartLevel, val, part); return true;
    case 0x1a: settings->set_param(PatchParam::PartPanpot, val, part); return true;
    default: return false;
  }
}


void Synth::_midi_input_sysex_DT1(uint8_t model, uint8_t *data, uint16_t length)
{
  if (length < 4)
    return;

  int p = 0;
  if (model == 0x46) {
    // Four address bytes, then a BLOCK of consecutive parameters - Roland's
    // DT1 writes from the address onward, one byte per parameter, and the JV
    // demos rely on that: "Lost Weekend" sends a single message per part
    // carrying the patch-number nibble pair (0x17, 0x18) AND the Part Level
    // (0x19), so a handler that read one byte saw only the patch nibble and
    // ignored the level. Only 00 00 xx is mapped here.
    if (length < 5 || data[0] != 0x00 || data[1] != 0x00)
      return;
    const uint8_t blk = data[2];
    bool acted = false;
    for (int i = 4; i < length; i++) {
      const uint8_t prm = (uint8_t) (data[3] + (i - 4));
      const uint8_t val = data[i] & 0x7f;
      if (blk == 0x10)
        acted |= jv_dt1_common(_settings, prm, val);
      else if (blk >= 0x18 && blk <= 0x1f)
        acted |= jv_dt1_part(_settings, blk - 0x18, prm, val);
    }
    if (acted)
      for (const auto &cb : _partMidiModCallbacks)
        cb(-1);
    return;
  }

  if (model == 0x42) {

    // System parameters
    if (data[0] == 0x40 && data[1] == 0x00) {

      // First handle the special case: Reset to the GSstandard mode message
      if (data[2] == 0x7f) {
	reset(SoundMap::GS, true);
	return;
      }

      uint8_t dataLength;             // Based on SysEx chart in Owner's Manual
      if (data[2] == (int) SystemParam::Tune)
	dataLength = 4;
      else
	dataLength = 1;

      if (length - dataLength != 3) {
	std::cerr << "libemusc: Roland SysEx message has invalid data length! "
		  << "Message discarded." << std::endl;
	return;
      }

      _settings->set_system_param(data[2], &data[3], dataLength);

      for (const auto &cb : _partMidiModCallbacks)     // Update user interface
	cb(-1);

    // Patch parameters part 1: Address space 40 01 XX
    } else if (data[0] == 0x40 && data[1] == 0x01) {

      uint8_t dataLength = 1;          // Based on SysEx chart in Owner's Manual
      if (data[2] == 0x00 || data[2] == 0x10)
	dataLength = 0x10;

      if (length - dataLength != 3) {
	std::cerr << "libemusc: Roland SysEx message has invalid data length! "
		  << "Message discarded." << std::endl;
	return;
      }

      uint16_t a = data[2] | (data[1] << 8);
      _settings->set_patch_param(a, &data[3], dataLength);

      for (const auto &cb : _partMidiModCallbacks)     // Update user interface
	cb(-1);

    // Patch parameters part 2: Address spcae 40 1P XX (P = Part)
    } else if (data[0] == 0x40 && (data[1] & 0x10)) {

      // Set length of message based on SysEx chart in Owner's Manual
      uint8_t dataLength;
      switch (data[2])
	{
	case 0x00:
	case 0x17:
	  dataLength = 2;
	  break;
	case 0x40:
	  dataLength = 12;
	  break;
	default:
	  dataLength = 1;
	}

      if (length - dataLength != 3) {
	std::cerr << "libemusc: Roland SysEx message has invalid data length! "
		  << "Message discarded." << std::endl;
	return;
      }

      uint16_t address = data[2] | (data[1] << 8);
      _settings->set_patch_param(address, &data[3], dataLength);

      // Update user interface
      for (const auto &cb : _partMidiModCallbacks)
	cb(Settings::convert_from_roland_part_id(data[1] & 0x0f));

    // Part parameters, Block 2/2: Address 40 2P XX (P = Part)
    } else if (data[0] == 0x40 && (data[1] & 0x20)) {

      // All relevant messages has data length of 1 byte
      if (length != 4) {
	std::cerr << "libemusc: Roland SysEx message has invalid data length! "
		  << "Message discarded." << std::endl;
	return;
      }

      // Verify that entire address is actually valid
      if (data[2] > 0x5a) {
	std::cerr << "libemusc: Roland SysEx message has invalid address! "
		  << "Message discarded." << std::endl;
	return;
      }

      uint16_t address = data[2] | (data[1] << 8);
      _settings->set_patch_param(address, &data[3], 1);

    // Drum parameters: Address 41 MX XX (M = Map)
    } else if (data[0] == 0x41 && !(data[1] & 0xe0)) {
      // Set length of message based on SysEx chart in Owner's Manual
      uint8_t dataLength;
      switch (data[1] & 0x0f)
	{
	case 0x00:
	  dataLength = 12;
	  break;
	default:
	  dataLength = 1;
	}

      if (length - dataLength != 3) {
	std::cerr << "libemusc: Roland SysEx message has invalid data length! "
		  << "Message discarded." << std::endl;
	return;
      }

      _settings->set_drum_param(data[2] | data[1] << 8, &data[3], dataLength);
    }
  }
}



}
