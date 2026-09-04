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


#include "part.h"

#include <algorithm>
#include <array>
#include <map>
#include <cmath>
#include <iostream>


namespace EmuSC {

Part::Part(uint8_t id, Settings *settings, ControlRom &ctrlRom, WaveRom &waveRom)
  : _id(id),
    _settings(settings),
    _lastPeakSample(0),
    _pendingBank(-1),
    _ctrlRom(ctrlRom),
    _waveRom(waveRom),
    _lastPitchBendRange(2)
{
  // TODO: Rename mode => synthMode and set proper defaults for MT32 mode
  _notesMutex = new std::mutex();

  _partialReserve = 2;           // TODO: Add this to settings with propoer val
}


Part::~Part()
{
  delete_all_notes();
  delete _notesMutex;
}


// All Sound Canvas modules generates 256 samples per control update.
int Part::get_sample_set(std::array<std::array<float, 256>, 2> &dryBus,
			 std::array<float, 256> &chorusBus,
			 std::array<float, 256> &reverbBus)
{
  _notesMutex->lock();

  // Only process notes if we have any
  if (_notes.size() > 0) {

    // TODO: Figure out a proper way to efficiently calculate new controller
    //       values when needed. Is PitchBend the only one that needs this?
    uint8_t pbRng = _settings->get_param(PatchParam::PB_PitchControl, _id) - 0x40;
    if (pbRng != _lastPitchBendRange) {
      _lastPitchBendRange = pbRng;
      _settings->update_pitchBend_factor(_id);
    }

    // Each part sends its OWN signal to the effect buses, at its own send
    // level, and the buses accumulate across parts. Collecting this part's
    // notes in a local block first is what makes that possible: the shared dry
    // bus already holds every part processed before this one, so scaling it
    // would send those parts at this part's send level and would let the last
    // sounding part overwrite what the others sent.
    //
    // Measured on the SC-55mkII (PROVENANCE.md P-0175): two parts struck
    // together, one with reverb send 127 and the other with send 0, produce
    // exactly the reverb of the sending part alone - the same level to 0.00 dB
    // and the same spectral centroid as that part rendered on its own - in
    // either part order. Sends are per part and the bus is a sum.
    std::array<std::array<float, 256>, 2> partBus = {};

    // The send is taken from the part's signal as it stands before the part's
    // panner, so it is collected separately from the panned stereo block.
    // Measured on the SC-55mkII (PROVENANCE.md P-0182): with a fixed reverb
    // send, sweeping CC10 from 0 to 127 leaves the reverb's level unchanged to
    // 0.00 dB at every one of nine pan positions, and the chorus with it,
    // while the dry signal pans as it should.
    // Two of them, because a drum instrument's reverb and chorus depths are
    // separate bytes in the drum set and need not agree.
    std::array<float, 256> partSendReverb = {};
    std::array<float, 256> partSendChorus = {};

    // Get next sample from active notes, delete those which are finished
    std::list<Note*>::iterator itr = _notes.begin();
    while (itr != _notes.end()) {
      bool finished = (*itr)->get_sample_set(partBus, partSendReverb,
                                            partSendChorus);

      if (finished) {
 //      std::cout << "Both partials have finished -> delete note" << std::endl;
        delete *itr;
        itr = _notes.erase(itr);
      } else {
        ++itr;
      }
    }

    // Store last (highest) value for future queries (typically for bar display)
    auto itL = std::max_element(partBus[0].begin(), partBus[0].end());
    _lastPeakSample = *itL;
    auto itR = std::max_element(partBus[1].begin(), partBus[1].end());
    _lastPeakSample = std::max(_lastPeakSample, *itR);


    // The send scale is the DEVICE's, not a constant. On the JV a byte-sized
    // coefficient field has 64 = unity - established four independent ways in
    // its firmware (P-0395), and the same scale its reverb RETURN uses - so a
    // send of 127 means 1.98, not 0.99. Dividing by 128 here made every JV
    // send half what the firmware asks for, which is the whole of D-39's
    // 6.06 dB quiet reverb: measured tail 39.60 dB against the reference's
    // 45.66, and 45.62 once corrected.
    const float sendDiv = _settings->device()->reverb.sendDivisor;
    // The chorus send takes the same scale as the reverb send, by the same
    // argument: 64 = unity is this chip's scale for a byte-sized coefficient
    // field, established four independent ways in the JV's firmware (P-0395),
    // and a chorus send is such a field. Worth +0.45 dB on SAW Lead, whose
    // tones send 127. NOT independently proven for the chorus specifically -
    // it rests on the chip-wide property rather than on its own trace - and it
    // is measurably NOT the cause of Glass Pad's effects deficit, which does
    // not move at all with it.
    const float choDiv = _settings->device()->reverb.chorusSendDivisor > 0.0f
                       ? _settings->device()->reverb.chorusSendDivisor : sendDiv;
    float chorusSL = _settings->get_param(PatchParam::ChorusSendLevel, _id) / choDiv;
    float reverbSL = _settings->get_param(PatchParam::ReverbSendLevel, _id) / sendDiv;

    for (int i = 0; i < 256; i++) {
      dryBus[0][i] += partBus[0][i];
      dryBus[1][i] += partBus[1][i];
      chorusBus[i] += partSendChorus[i] * chorusSL;
      reverbBus[i] += partSendReverb[i] * reverbSL;
    }
  }

  // Export envelopes and LFOs to external client
  if (_envelopeCallback && !_notes.empty())
    _envelopeCallback(_notes.front()->get_current_pitch(0),
		      _notes.front()->get_current_pitch(1),
		      _notes.front()->get_current_tvf(0),
		      _notes.front()->get_current_tvf(1),
		      _notes.front()->get_current_tva(0),
		      _notes.front()->get_current_tva(1));

  if (_lfoCallback) {
    if (!_notes.empty())
      _lfoCallback(_notes.front()->get_current_lfo(0),
		   _notes.front()->get_current_lfo(1),
		   _notes.front()->get_current_lfo(2));
    else
      _lfoCallback(0, 0, 0);
  }

  _notesMutex->unlock();

  return 0;
}


void Part::update(void)
{
  for (auto &n : _notes)
    n->update();
}


int Part::get_last_peak_sample(void)
{
  if (_settings->get_param(PatchParam::Mute, _id) ||
      _settings->get_param(SystemParam::Mute))
    return -1;

  // Formula for peak display level: bars = (max(TVA) * PartScale) / 8;

  // Calculate part scale
  int scale = _settings->get_param(PatchParam::Expression, _id) *
              _settings->get_param(PatchParam::PartLevel, _id) *
              _settings->get_param(SystemParam::Volume);
  scale = (((4 * scale) >> 8) & 0xffff);
  scale = ((scale * 0x8208) >> 16);
  scale = ((scale * 2) & 0xffff) + 0xff;
  if (scale >= 0x8000) scale = 0x7f00;
  scale >>= 8;

  int tvaMax = 0;
  _notesMutex->lock();

  for (auto &n: _notes)
    tvaMax = std::max({tvaMax, n->get_current_tva(0), n->get_current_tva(1)});

  _notesMutex->unlock();

  if (0)
    std::cout << "PartScale=" << std::hex << scale
              << " tvaMax=" << tvaMax
              << " => " << ((scale * tvaMax) >> 11)
              << std::endl;

  return (scale * tvaMax) >> 11;
}


// How many partials this part occupies for VOICE ALLOCATION. On a normal
// part every sounding (non-damped) note counts. On a rhythm part only the
// NEWEST sounding voice of each drum key counts: when a drum key is
// retriggered, the machine lets the older voice ring on to its natural end
// but no longer counts it against the polyphony budget, on any gap from
// 0.1 s to 2 s and for every older voice of the key, while a lone drum
// voice is counted for its whole life (about 5.2 s for Ride Cymbal 1) and
// voices on DIFFERENT drum keys are all counted. Measured on the SC-55mkII
// with a refusal probe whose every verdict was cross-checked against this
// code's own allocation log (PROVENANCE.md P-0283): 26 reserved sines plus
// ride+ride left room for a 27th-reserve probe note where ride+crash and a
// restruck melodic vibraphone did not, and with four ride voices and 24
// sines the machine accepted exactly three more notes before its self-steal
// began - the arithmetic of "newest per key", not "all" (ours before this
// change) and not "none". The reference demonstrably RENDERS more than 28
// voices at once in these scenes, so the 28 is an allocation budget, not a
// render limit, and the uncounted voices must keep sounding.
int Part::get_num_partials(void)
{
  if (_notes.size() == 0)
    return 0;

  int numPartials = 0;

  // On the JV-880 every drum hit holds its voice until its envelope ends, a
  // repeated key included: the rhythm note-on path (ROM1 0xB4D -> 0xBCD ->
  // 0xCA4) re-uses a sounding voice only for a Mute Group sibling (@0x8419),
  // never for the same key. So its rhythm part is counted like a tonal one
  // (scdb D-44).
  if (_settings->get_param(PatchParam::UseForRhythm, _id) == mode_Norm ||
      _settings->generation() == ControlRom::SynthGen::JV880) {
    for (auto &n: _notes)
      if (!n->is_damped())
        numPartials += n->get_num_partials();
  } else {
    // _notes is kept in note-on order, so the last sounding note of a key
    // is the newest and its partial count survives.
    std::map<uint8_t, int> newestOfKey;
    for (auto &n: _notes)
      if (!n->is_damped())
        newestOfKey[n->key()] = n->get_num_partials();
    for (auto &kv : newestOfKey)
      numPartials += kv.second;
  }

  return numPartials;
}


// Whether a note on this key would be accepted by this part at all. A note
// the part discards must not take a voice from another one.
bool Part::accepts_note(uint8_t key)
{
  // Check if part is muted or rxNoteMessage is disabled
  if (_settings->get_param(PatchParam::Mute, _id) ||
      _settings->get_param(SystemParam::Mute) ||
      !_settings->get_param(PatchParam::RxNoteMessage, _id))
    return false;

  // Check if key is outside part configured key range
  if (key < _settings->get_param(PatchParam::KeyRangeLow, _id) ||
      key > _settings->get_param(PatchParam::KeyRangeHigh, _id))
    return false;

  // If note is a drum -> check if drum accepts note on
  uint8_t rhythm = _settings->get_param(PatchParam::UseForRhythm, _id);
  if (rhythm != mode_Norm &&
      !(_settings->get_param(DrumParam::RxNoteOn, rhythm - 1, key)))
    return false;

  return true;
}


int Part::get_note_partials(uint8_t key)
{
  if (!accepts_note(key))
    return 0;

  return Note::partial_count(_ctrlRom, _settings, _id, key);
}


// The voice this part gives up when one is taken from it. Measured on both
// models: a voice already in its release phase goes before a voice that is
// still held, however much older the held one is, and within each group the
// oldest goes first (PROVENANCE.md P-0078).
bool Part::steal_candidate(uint32_t &serial, bool &releasing)
{
  bool found = false;
  releasing = false;

  _notesMutex->lock();

  for (auto &n : _notes) {              // _notes is in note on order
    if (n->is_damped() || n->get_num_partials() == 0)
      continue;

    if (!found) {                       // Oldest voice of this part
      serial = n->serial();
      found = true;
    }

    if (n->is_releasing()) {            // Oldest voice already released
      serial = n->serial();
      releasing = true;
      break;
    }
  }

  _notesMutex->unlock();

  return found;
}


void Part::live_partials(std::vector<LivePartial> &out)
{
  _notesMutex->lock();

  for (auto &n : _notes) {              // _notes is in note on order
    if (n->is_damped())
      continue;
    for (int slot = ControlRom::MAX_PARTIALS - 1; slot >= 0; slot--)
      if (n->partial_live(slot))
        out.push_back({n->serial(), slot});
  }

  _notesMutex->unlock();
}


int Part::damp_partial(uint32_t serial, int slot, float dBPerMillisecond)
{
  int released = 0;

  _notesMutex->lock();

  for (auto &n : _notes) {
    if (n->serial() == serial && !n->is_damped()) {
      released = n->damp_partial(slot, dBPerMillisecond);
      break;
    }
  }

  _notesMutex->unlock();

  return released;
}


// Key Assign SOLO on the JV-880: the part's sounding note gives its voices to
// the new key. The firmware re-programs the same voices in place (ROM1 0xC84
// -> 0xDA2 -> 0xE7E); here the old note fades at the device's damp rate while
// the new one starts, and its partials count as free at once. Returns the
// partials released.
int Part::solo_release(float dBPerMillisecond)
{
  int released = 0;

  _notesMutex->lock();

  for (auto &n : _notes)
    if (!n->is_damped()) {
      released += n->get_num_partials();
      n->damp(dBPerMillisecond);
    }

  _notesMutex->unlock();

  return released;
}


int Part::steal_voice(uint32_t serial, float dBPerMillisecond)
{
  int numPartials = 0;

  _notesMutex->lock();

  for (auto &n : _notes) {
    if (n->serial() == serial && !n->is_damped()) {
      numPartials = n->get_num_partials();
      n->damp(dBPerMillisecond);
      break;
    }
  }

  _notesMutex->unlock();

  return numPartials;
}


// A drum set gives some of its sounds an assign group, and two sounds in the
// same group cannot be heard at once: the SC-55mkII owner's manual prints
// "[EXC n]" against them and says "Percussion sound of the same number will
// not be heard at the same time" (p.88, and p.89 for the SFX and CM-64/32L
// sets). The number it prints is the drum set's assign group byte, which
// ControlRom already reads and Settings already publishes as
// DrumParam::AssignGroupNumber, so the parameter is taken from Settings and
// a GS drum setup edit changes the grouping as it should.
//
// Measured on both models: the sounding note is not stopped but faded out,
// at 9.4 dB/ms on the SC-55mkII and 17.9 dB/ms on the SC-55, which is the
// fade a voice that has been taken away gets (P-0080), so the same rate is
// used here. Every ordered pair within a group behaves the same way, a note
// against itself included: a closed hi-hat cuts an open one, an open one
// cuts a closed one, and a second closed hi-hat cuts the first. Notes in
// different groups, and notes with no group, never touch each other, and the
// grouping does not reach across parts - a second part set to rhythm cuts
// nothing on this one (PROVENANCE.md P-0085).
int Part::choke_assign_group(uint8_t key, float dBPerMillisecond)
{
  uint8_t rhythm = _settings->get_param(PatchParam::UseForRhythm, _id);
  if (rhythm == mode_Norm)
    return 0;

  uint8_t group = _settings->get_param(DrumParam::AssignGroupNumber,
                                       rhythm - 1, key);
  if (group == 0)                       // 0 means the sound is not grouped
    return 0;

  int numPartials = 0;

  _notesMutex->lock();

  for (auto &n : _notes) {
    if (n->is_damped())
      continue;

    if (_settings->get_param(DrumParam::AssignGroupNumber,
                             rhythm - 1, n->key()) != group)
      continue;

    numPartials += n->get_num_partials();
    n->damp(dBPerMillisecond);
  }

  _notesMutex->unlock();

  return numPartials;
}


// Note: Mute cancels all active keys in part, and all new keys are ignored
int Part::add_note(uint8_t key, uint8_t keyVelocity, uint32_t serial,
                   int startDelay)
{
  // 1., 2. & 4. Mute, rxNoteMessage, key range and the drum's note on flag
  if (!accepts_note(key))
    return 0;

  // 5. Calculate corrected key velocity based on velocity sens depth & offset
  //    according to description in SC-55 owner's manual page 38
  uint8_t velSensDepth =
    _settings->get_param(PatchParam::VelocitySenseDepth, _id);
  uint8_t velSensOffset =
    _settings->get_param(PatchParam::VelocitySenseOffset, _id);
  float v = keyVelocity * (velSensDepth / 64.0);
  if (velSensOffset >= 64)
    v += velSensOffset - 64;
  else
    v *= (velSensOffset + 64) / 127.0;
  uint8_t velocity = (v <= 127) ? std::roundf(v) : 127;

  // 6. Remove all existing notes if part is in mono mode according to the
  //    SC-55 owner's manual page 39
  //    The JV-880 does not: its SOLO note has already been handed over in
  //    Synth::_add_note (solo_release) and fades while the new one starts.
  if (_settings->get_param(PatchParam::PolyMode, _id) == false &&
      _settings->get_param(PatchParam::UseForRhythm, _id) == mode_Norm &&
      _settings->generation() != ControlRom::SynthGen::JV880)
    delete_all_notes();

  _notesMutex->lock();

  Note *n = new Note(key, velocity, _ctrlRom, _waveRom, _settings, _id, serial,
                     startDelay);
  _notes.push_back(n);

  _notesMutex->unlock();

  if (_settings->get_param(PatchParam::Hold1, _id))
      n->sustain(true);

  if (0)
    std::cout << "EmuSC: New note [ part=" << (int) _id
	      << " key=" << (int) key
	      << " velocity=" << (int) velocity
	      << " ]" << std::endl;
  
  return 1;
}


int Part::stop_note(uint8_t key, uint8_t releaseVelocity)
{
  for (auto &n : _notes)
    n->stop(key, releaseVelocity);

  return 0;
}


int Part::stop_all_notes(void)
{
  int i = _notes.size();
  for (auto n : _notes)
    n->stop();

  return i;
}


int Part::delete_all_notes(void)
{
  _notesMutex->lock();

  int i = _notes.size();
  for (auto n : _notes)
    delete n;

  _notes.clear();

  _notesMutex->unlock();

  return i;
}


int Part::control_change(uint8_t msgId, uint8_t value)
{
  // RxControlChange does not affect Channel Mode messages
  if (!_settings->get_param(PatchParam::RxControlChange, _id) && msgId < 120)
    return 0;

  bool updateGUI = false;

  if (msgId == 0) {                                    // Bank select
    // LATCHED, not applied. The hardware keeps the tone it is playing until
    // the next program change, and the SC-55mkII owner's manual (p.90) says
    // so. Measured on the SC-55mkII: with the part on bank 0 program 4 and
    // bank 8 defined for it (Detuned EP 1), a file that sends the program and
    // then a bare CC0 = 8 renders BYTE-IDENTICALLY to one that never sent the
    // bank at all, while a file that sends the bank and then a program change
    // - in either order relative to each other - gets the bank-8 tone. Writing
    // ToneNumber here made the bare bank select take effect at the next NOTE,
    // so five corpus files played the wrong instrument at 29 places.
    // TODO: This check is only available for SC-55mkII+
    if (_settings->get_param(PatchParam::RxBankSelect, _id)) {
      if (_settings->generation() == ControlRom::SynthGen::JV880) {
        // JV-880 firmware, ROM1 0x749C-0x74EA: the handler stores a one-bit
        // bank flag, and only for two values - CC0 = 80 clears it (Internal
        // or Card), CC0 = 81 sets it (Presets). Any other CC0 value returns
        // without storing, so the flag keeps its previous value. The flag is
        // a per-part byte (@0x65B8[part]) that stays until the next CC0 = 80
        // or 81; a program change does not spend it. It lives in _pendingBank
        // here, mapped onto the rows of the variation table.
        // devices/jv880/03_disassembly/midi_handlers.md, D-43.
        const auto &P = _ctrlRom.profile()->records->patch;
        if (value == 80)
          _pendingBank = P.midiBankFirst;
        else if (value == 81)
          _pendingBank = P.midiBankPresets;
      } else {
        _pendingBank = value;
      }
    }

  } else if (msgId == 1) {                             // Modulation
    if (_settings->get_param(PatchParam::RxModulation, _id))
      _settings->set_param(PatchParam::Modulation, value, _id);

  } else if (msgId == 5) {                             // Portamento time
    _settings->set_param(PatchParam::PortamentoTime, value, _id);

  } else if (msgId == 6) {                             // Data entry MSB
    // RPN
    uint8_t msb = _settings->get_param(PatchParam::RPN_MSB, _id);
    uint8_t lsb = _settings->get_param(PatchParam::RPN_LSB, _id);
    if (msb != 0x7f && lsb != 0x7f)
      if (msb == 0 && lsb == 0 && value <= 24) {         // Pitch bend range
	_settings->set_param(PatchParam::PB_PitchControl, value + 0x40, _id);
      } else if (msb == 0 && lsb == 1) {                 // Master fine tuning
	_settings->set_param(PatchParam::PitchFineTune, value, _id);
      } else if (msb == 0 && lsb == 2) {                 // Master coarse tuning
	_settings->set_param(PatchParam::PitchCoarseTune, value, _id);
      }
    // NRPN
    msb = _settings->get_param(PatchParam::NRPN_MSB, _id);
    lsb = _settings->get_param(PatchParam::NRPN_LSB, _id);
    // The NRPN 01 xx parameters are documented with a data range of
    // 0E-72 (+/-50 around 40), and the machine CLAMPS a value outside it to
    // the nearest bound rather than discarding the write: on the SC-55mkII,
    // renders with data 7F are bit-identical to renders with data 72, and
    // renders with 00 to renders with 0E, for every parameter guarded below
    // (vibrato rate/depth/delay, TVF cutoff/resonance, and the three
    // envelope times; emusc-match P-0258). Rejecting instead of clamping
    // silently dropped e.g. a decay time of 7F, which on a trumpet leaves
    // the TVF's decay at its tone default while the machine holds the filter
    // open ~8 dB brighter at 4 kHz through the whole sustain.
    uint8_t clamped = std::clamp<uint8_t>(value, 0x0e, 0x72);
    if (msb != 0x7f && lsb != 0x7f)
      if (msb == 0x01 && lsb == 0x08) {
	_settings->set_param(PatchParam::VibratoRate, clamped, _id);
      } else if (msb == 0x01 && lsb == 0x09) {
	_settings->set_param(PatchParam::VibratoDepth, clamped, _id);
      } else if (msb == 0x01 && lsb == 0x0a) {
	_settings->set_param(PatchParam::VibratoDelay, clamped, _id);
      } else if (msb == 0x01 && lsb == 0x20) {
	_settings->set_param(PatchParam::TVFCutoffFreq, clamped, _id);
      } else if (msb == 0x01 && lsb == 0x21) {
	_settings->set_param(PatchParam::TVFResonance, clamped, _id);
      } else if (msb == 0x01 && lsb == 0x63) {
	_settings->set_param(PatchParam::TVFAEnvAttack, clamped, _id);
      } else if (msb == 0x01 && lsb == 0x64) {
	_settings->set_param(PatchParam::TVFAEnvDecay, clamped, _id);
      } else if (msb == 0x01 && lsb == 0x66) {
	_settings->set_param(PatchParam::TVFAEnvRelease, clamped, _id);
      } else if (msb == 0x18) {
	int map = _settings->get_param(PatchParam::UseForRhythm, _id) - 1;
	if (map == 0 || map == 1)
	  _settings->set_param(DrumParam::PlayKeyNumber, map, lsb, value);
      } else if (msb == 0x1a) {
	int map = _settings->get_param(PatchParam::UseForRhythm, _id) - 1;
	if (map == 0 || map == 1)
	  _settings->set_param(DrumParam::Level, map, lsb, value);
      } else if (msb == 0x1c) {
	int map = _settings->get_param(PatchParam::UseForRhythm, _id) - 1;
	if (map == 0 || map == 1)
	  _settings->set_param(DrumParam::Panpot, map, lsb, value);
      } else if (msb == 0x1d) {
	int map = _settings->get_param(PatchParam::UseForRhythm, _id) - 1;
	if (map == 0 || map == 1)
	  _settings->set_param(DrumParam::ReverbDepth, map, lsb, value);
      }

  } else if (msgId == 7) {                             // Volume
    if (_settings->get_param(PatchParam::RxVolume, _id)) {
      // Under GS, CC7 IS the part level - one parameter, and writing it here is
      // correct. On a device that keeps them apart it is NOT: the JV holds the
      // Performance part level and CC7 in different state and multiplies them
      // into one level index, so writing CC7 over the part level destroys the
      // performance's own balance. The demo does exactly that - CC7 = 100 on
      // every channel flattened part levels 110/127/78 to 100 and left
      // channel 1 +2.7 dB hot (scdb D-28; the composition is ROM1 0x44be).
      _settings->set_param(_settings->device()->level.volumeIndexShift
                             ? PatchParam::PartVolume : PatchParam::PartLevel,
                           value, _id);
      updateGUI = true;
    }

  } else if (msgId == 10) {                            // Panpot
    if (_settings->get_param(PatchParam::RxPanpot, _id)) {
      // Value 0 of the PART PARAMETER means random pan, and Roland documents
      // it that way - but CC10 = 0 is hard left, and the two write the same
      // parameter. Measured on the SC-55mkII: CC10 = 0 and CC10 = 1 produce
      // the SAME render, -30.8 dB left and silence right, while the SysEx
      // parameter set to 0 does randomise. So the controller is clamped to a
      // minimum of 1 on its way in, and only the SysEx path can ask for RND.
      _settings->set_param(PatchParam::PartPanpot,
                           (uint8_t) std::max<int>(value, 1), _id);
      updateGUI = true;
    }

  } else if (msgId == 11) {                            // Expression
    if (_settings->get_param(PatchParam::RxExpression, _id))
      _settings->set_param(PatchParam::Expression, value, _id);

  } else if (msgId == 38) {                            // Data entry LSB
    // Only RPN #1
    if (_settings->get_param(PatchParam::RPN_MSB, _id) == 0 &&
	_settings->get_param(PatchParam::RPN_LSB, _id) == 1) {
      _settings->set_param(PatchParam::PitchFineTune2, value, _id);
    }

  } else if (msgId == 64) {                            // Hold1
    if (_settings->get_param(PatchParam::RxHold1, _id)) {
      if (value < 64) {
	_settings->set_param(PatchParam::Hold1, (uint8_t) 0, _id);
      } else {
	_settings->set_param(PatchParam::Hold1, (uint8_t) 1, _id);
      }

      for (auto &n : _notes)
	n->sustain(_settings->get_param(PatchParam::Hold1, _id));

    } // Note: SC-88 Pro seems to use full 7 bit value for Hold1

  } else if (msgId == 65) {                            // Portamento
    if (_settings->get_param(PatchParam::RxPortamento, _id)) {
      if (value < 64)
	_settings->set_param(PatchParam::Portamento, (uint8_t)0,_id);
      else
	_settings->set_param(PatchParam::Portamento, (uint8_t)1,_id);
    }

  } else if (msgId == 66) {                            // Sostenuto
    if (_settings->get_param(PatchParam::RxSostenuto, _id)) {
      if (value < 64)
	_settings->set_param(PatchParam::Sostenuto, (uint8_t)0,_id);
      else
	_settings->set_param(PatchParam::Sostenuto, (uint8_t)1,_id);

      for (auto &n : _notes)
	n->sustain(_settings->get_param(PatchParam::Sostenuto, _id));
    }

  } else if (msgId == 67) {                            // Soft
    if (_settings->get_param(PatchParam::RxSoft, _id)) {
      if (value < 64)
	_settings->set_param(PatchParam::Soft, (uint8_t)0,_id);
      else
	_settings->set_param(PatchParam::Soft, (uint8_t)1,_id);
    }

  } else if (msgId == 84) {                            // Portamento control
    _settings->set_param(PatchParam::PortamentoControl, value, _id);

  } else if (msgId == 91) {                            // Reverb
    _settings->set_param(PatchParam::ReverbSendLevel, value, _id);
    updateGUI = true;

  } else if (msgId == 93) {                            // Chorus
    _settings->set_param(PatchParam::ChorusSendLevel, value, _id);
    updateGUI = true;

  } else if (msgId == 98) {                            // NRPN LSB
    if (_settings->get_param(PatchParam::RxNRPN, _id)) {
      _settings->set_param(PatchParam::NRPN_LSB, value, _id);
      _settings->set_param(PatchParam::RPN_MSB, (uint8_t) 0x7f, _id);
      _settings->set_param(PatchParam::RPN_LSB, (uint8_t) 0x7f, _id);
    }

  } else if (msgId == 99) {                            // NRPN MSB
    if (_settings->get_param(PatchParam::RxNRPN, _id)) {
      _settings->set_param(PatchParam::NRPN_MSB, value, _id);
      _settings->set_param(PatchParam::RPN_MSB, (uint8_t) 0x7f, _id);
      _settings->set_param(PatchParam::RPN_LSB, (uint8_t) 0x7f, _id);
    }

  // Selecting an RPN deselects the NRPN and the other way round: there is one
  // data-entry target, not two. Without this a file that used an NRPN and
  // later an RPN left both selected, and the next CC6 wrote BOTH of them -
  // measured at 2203.88 cents of pitch error when it fires (G-029).
  } else if (msgId == 100) {                           // RPN LSB
    if (_settings->get_param(PatchParam::RxRPN, _id)) {
      _settings->set_param(PatchParam::RPN_LSB, value, _id);
      _settings->set_param(PatchParam::NRPN_MSB, (uint8_t) 0x7f, _id);
      _settings->set_param(PatchParam::NRPN_LSB, (uint8_t) 0x7f, _id);
    }

  } else if (msgId == 101) {                           // RPN MSB
    if (_settings->get_param(PatchParam::RxRPN, _id)) {
      _settings->set_param(PatchParam::RPN_MSB, value, _id);
      _settings->set_param(PatchParam::NRPN_MSB, (uint8_t) 0x7f, _id);
      _settings->set_param(PatchParam::NRPN_LSB, (uint8_t) 0x7f, _id);
    }

  // Channel Mode messages
  } else if (msgId == 120) {                           // All Sounds Off
    delete_all_notes();

  } else if (msgId == 121) {                           // Reset All Controllers
    pitch_bend_change(0x00, 0x40, true);
    _settings->set_param(PatchParam::PolyKeyPressure, 0, (int8_t) _id);
    _settings->set_param(PatchParam::ChannelPressure, 0, (int8_t) _id);
    _settings->set_param(PatchParam::Modulation, 0, (int8_t) _id);
    _settings->set_param(PatchParam::Expression, 127, (int8_t) _id);
    _settings->set_param(PatchParam::Hold1, 0, (int8_t) _id);
    _settings->set_param(PatchParam::Portamento, 0, (int8_t) _id);
    _settings->set_param(PatchParam::Sostenuto, 0, (int8_t) _id);
    _settings->set_param(PatchParam::Soft, 0, (int8_t) _id);
    // RPN & NRPN LSB/MSB -> 0x7f?

  } else if (msgId == 123) {                           // All Notes Off
    stop_all_notes();

  } else if (msgId == 124) {                           // OMNI Off
    stop_all_notes();

  } else if (msgId == 125) {                           // OMNI On
    stop_all_notes();

  } else if (msgId == 126) {                           // Mono (-> Mode 4)
    stop_all_notes();
    _settings->set_param(PatchParam::PolyMode, (uint8_t) 0, (int8_t) _id);

  } else if (msgId == 127) {                           // Poly (-> Mode 3)
    stop_all_notes();
    _settings->set_param(PatchParam::PolyMode, (uint8_t) 1, (int8_t) _id);
  }

  // Update CC1 and CC2 based on configured controller inputs
  if (_settings->get_param(PatchParam::CC1ControllerNumber, _id) == msgId)
    _settings->set_param(PatchParam::CC1Controller, value, _id);

  if (_settings->get_param(PatchParam::CC2ControllerNumber, _id) == msgId)
    _settings->set_param(PatchParam::CC2Controller, value, _id);

  return updateGUI;
}


int Part::poly_key_pressure(uint8_t key, uint8_t value)
{
  std::cout << "Polyphonic key pressure not implemented (ch="
	    << _settings->get_param(PatchParam::RxChannel, _id)
	    << ", key=" << (int) key << ", value=" << value << ")"
	    << std::endl;

  return 0;
}


int Part::channel_pressure(uint8_t value)
{
  if (_settings->get_param(PatchParam::RxChPressure, _id))
    _settings->set_param(PatchParam::ChannelPressure, value, _id);

  return 0;
}


int Part::pitch_bend_change(uint8_t lsb, uint8_t msb, bool force)
{
  if (!force && !_settings->get_param(PatchParam::RxPitchBend, _id))
    return -1;
  
  _settings->set_patch_param((uint16_t) PatchParam::PitchBend,
			     (uint8_t) ((msb & 0x7f) >> 1), _id);

  // SC-55 line has 12 bit resolution on pitch wheel (that is 14 bit)
  //	  if (_ctrlRom.synthModel == ControlRom::ss_SC55)
  _settings->set_patch_param((uint16_t) PatchParam::PitchBend + 1,
			     (uint8_t) ((lsb & 0x7c) | (msb << 7)), _id);
  //	    else               // SC-88 line has normal 14 bit resolution
  //	    set_patch_param((uint16_t) PatchParam::PitchBend + 1,
  //			    (uint8_t) ((lsb & 0x7f) | (msb << 7)), _id);

  // Update PitchBend factor
  _settings->update_pitchBend_factor(_id);

  return 0;
}


// TODO: Remove all unnecessary variables and initialization
void Part::reset(void)
{
  delete_all_notes();

  _partialReserve = 2;

  _lastPeakSample = 0;
  _pendingBank = -1;
}


// [index, bank] is the [x,y] coordinate in the variation table
// For drum sets, index is the program number in the drum set bank
int Part::set_program(uint8_t index, int8_t bank, bool ignRxFlags)
{
  if (!ignRxFlags && (!_settings->get_param(PatchParam::RxProgramChange, _id) ||
		      !_settings->get_param(SystemParam::RxInstrumentChange)))
    return -1;

  const bool jv880 = (_settings->generation() == ControlRom::SynthGen::JV880);

  if (bank < 0) {
    // A latched bank select is spent here, and nowhere else - on the Sound
    // Canvas. The JV-880's bank flag persists (see set_controller).
    bank = (_pendingBank >= 0) ? _pendingBank
                               : _settings->get_param(PatchParam::ToneNumber, _id);
    if (!jv880)
      _pendingBank = -1;
  }

  int rhythm = _settings->get_param(PatchParam::UseForRhythm, _id);

  // JV-880 firmware validates the selection before writing anything. ROM2
  // 0x30487 takes the bank from the selector byte (flag | program): 0x00-0x3F
  // Internal, 0x40-0x7F Card, 0x80-0xBF Preset A, 0xC0-0xFF Preset B. For the
  // Card bank it requires a card whose page-0x0E signature and checksum pass
  // (0x3049B, pjsr 0x314C7); with no card it returns carry set, and every
  // caller (patch mode 0x3036C, performance part 0x30315, performance select
  // 0x301B4) skips the copy: the part record's patch byte, the bank flag and
  // the temporary patch stay as they were and no voice event is posted. So a
  // bare program change 64-127 on a JV-880 without a card is REJECTED and the
  // part keeps the patch it has. The variation table holds 0xffff exactly
  // where the machine has no bank, so here 0xffff means "reject", not
  // "silence". Measured on the reference: CC0 81 + PC 64 (Preset B 0) then
  // CC0 80 + PC 64 stays on Preset B 0; PC 64 alone renders identically to no
  // program change. devices/jv880/12_implementation/
  // implementation_divergences.md D-43.
  if (jv880 && rhythm == mode_Norm &&
      _ctrlRom.variation(bank)[index] == 0xffff)
    return 0;

  _settings->set_param(PatchParam::ToneNumber2, index, _id);

  // Finds correct instrument variation from variations table
  // Implemented according to SC-55 Owner's Manual page 42-45
  if (rhythm == mode_Norm) {
    // What the synth does when the variation table has no tone at
    // [index, bank] is one of the places the two generations disagree
    // (P-0190, P-0192). The SC-55 substitutes the base of the variation's
    // own group of eight - the bank number with its low three bits cleared -
    // and the capital if that is undefined as well; but it does so only for
    // bank numbers up to 63 and program numbers up to 120. Above either
    // bound, and on the SC-55mkII at every bank and program, nothing is
    // substituted: the bank is left as it was received, Note finds 0xffff in
    // the variation table and starts a note with no partials, so the part is
    // silent until another bank or program is selected. Notes that are
    // already sounding keep the instrument they started with and are not
    // affected either way (P-0190).
    if (_ctrlRom.variation(bank)[index] == 0xffff &&
	_settings->generation() == ControlRom::SynthGen::SC55 &&
	bank < 64 && index < 120) {
      int8_t groupBase = bank & ~7;
      bank = (_ctrlRom.variation(groupBase)[index] != 0xffff) ? groupBase : 0;
    }

    _settings->set_param(PatchParam::ToneNumber, bank, _id);

    // A patch carries its own effect send depths, and selecting a patch brings
    // them with it; the performance part contributes only the on/off switch.
    // Without this a part kept sending at the depth of whatever patch the
    // performance had loaded - on "for CompuMix", part 2 went on sending at
    // reverb 124 after the song selected a patch whose own send is 0, and part
    // 4 at 60 where the new patch asks for 127. scdb D-55.
    if (jv880 && _id < (int) _ctrlRom.device_parts().size()) {
      const auto &dp = _ctrlRom.device_parts()[_id];
      const auto snd = _ctrlRom.instrument_send(
          _ctrlRom.variation(bank)[index] != 0xffff
          ? (int) _ctrlRom.variation(bank)[index] : -1);
      _settings->set_param(PatchParam::ReverbSendLevel,
                           (uint8_t) (dp.revSwitch ? snd.first  : 0), _id);
      _settings->set_param(PatchParam::ChorusSendLevel,
                           (uint8_t) (dp.choSwitch ? snd.second : 0), _id);
    }

    // Key Assign travels with the patch too, for the same reason the sends do.
    // A patch carries POLY or SOLO in its common +0x18 bit 7, and the part's
    // allocator flag was written ONLY where a performance is loaded - so a
    // part kept the Key Assign of whatever patch the performance had put
    // there, however many program changes later. Both directions were wrong,
    // and the loud one is inherited SOLO: the boot performance's SAW Lead is
    // SOLO on parts 1-6, so any program change on those parts still played one
    // note at a time. An eight-note cluster on Brass Sect 1 sounded its last
    // two notes where the reference sounds all eight, and the part's level did
    // not rise at all from one note to eight where the reference gains the
    // 8.0 dB an incoherent sum of eight asks for. In the other direction the
    // demo songs select "for CompuMix", whose eight parts are all POLY, and
    // then ask for patches of their own - 11 of the 192 factory patches are
    // SOLO, House Bass among them - which we were playing polyphonically.
    // scdb D-59.
    if (jv880) {
      const auto &PL = _ctrlRom.device()->records->patch;
      const int linear = (bank == PL.midiBankPresets) ? PL.perBank + index
                                                      : index;
      _settings->set_param(PatchParam::PolyMode,
                           (uint8_t) (_ctrlRom.device_patch_solo(linear) ? 0 : 1),
                           _id);
    }

  // If part is used for drums, select correct drum set
  } else {
    // The JV selects a rhythm SET BY BANK: the selector's top two bits name the
    // memory bank and the program number inside it is ignored (ROM2 0x30446).
    // Every demo song relies on this - songs 1 and 7 ask for Preset A with
    // PC 0, songs 3 and 6 for Preset B with PC 64 and PC 126 - and routing them
    // through the program-indexed table gave all of them the Internal kit.
    // scdb D-52.
    int dsIndex;
    if (jv880) {
      const int presets = _ctrlRom.device()->records->patch.midiBankPresets;
      const int flag = (bank == presets) ? 0x80 : 0x00;
      dsIndex = _settings->update_drum_set_bank(rhythm - 1, flag | index);
      if (dsIndex < 0)
        return 0;                    // absent bank: the part keeps its kit
    } else {
      dsIndex = _settings->update_drum_set(rhythm - 1, index);
    }
    if (dsIndex < 0) {
      std::cerr << "libEmuSC: Illegal program for drum set ("
		<< (int) index << ")" << std::endl;
      return 0;
    }

    // Note: ToneNumber (bank) is used as drumSet index for rhythm parts
    _settings->set_param(PatchParam::ToneNumber, dsIndex, _id);
  }

  // Send "change" callback for frontend
  if (_changeCallback) _changeCallback(_id);

  return 1;
}


void Part::set_change_callback(std::function<void(const int)> cb)
{
  _changeCallback = cb;
}


void Part::clear_change_callback(void)
{
  _changeCallback = NULL;
}


void Part::set_envelope_callback(std::function<void(const float, const float,
                                                    const float, const float,
                                                    const float, const float)> cb)
{
  _envelopeCallback = cb;
}


void Part::clear_envelope_callback(void)
{
  _envelopeCallback = NULL;
}


void Part::set_lfo_callback(std::function<void(const int, const int, const int)
                            > cb)
{
  _lfoCallback = cb;
}


void Part::clear_lfo_callback(void)
{
  _lfoCallback = NULL;
}

}
