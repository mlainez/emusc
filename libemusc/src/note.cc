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


#include "note.h"
#include "pitch.h"

#include <bitset>
#include <iostream>
#include <cmath>

namespace EmuSC {


// Instrument index a note on this key uses in this part.
// Note: toneBank is used as drumSet index for rhythm parts
uint16_t Note::_instrument_index(ControlRom &ctrlRom, Settings *settings,
                                 int8_t partId, uint8_t key)
{
  uint8_t toneBank = settings->get_param(PatchParam::ToneNumber, partId);
  uint8_t toneIndex = settings->get_param(PatchParam::ToneNumber2, partId);

  if (settings->get_param(PatchParam::UseForRhythm, partId) == 0)
    return ctrlRom.variation(toneBank)[toneIndex];

  // ToneNumber holds the drum set index on a rhythm part, but Part's control
  // change handler writes the raw bank-select value there for every part, and
  // set_program() only repairs it when update_drum_set() succeeds. A bank
  // select followed by a program that is not a drum program therefore leaves a
  // value of up to 127 here, and _drumSets holds about ten entries - so this
  // read went out of bounds. It was reachable from an ordinary MIDI stream:
  // the same illegal program produced three different renders for three
  // different bank-select values, two of 256 stimuli were not reproducible
  // between runs of the same binary, and one render crashed.
  //
  // An index with no drum set behind it has no sound to make, so it takes the
  // same path as an undefined instrument: partial_count() and the Note
  // constructor already treat 0xffff as silent.
  if (toneBank >= ctrlRom.numDrumSets())
    return 0xffff;

  return ctrlRom.drumSet(toneBank).preset[key];
}


int Note::partial_count(ControlRom &ctrlRom, Settings *settings,
                        int8_t partId, uint8_t key)
{
  uint16_t instrumentIndex = _instrument_index(ctrlRom, settings, partId, key);
  if (instrumentIndex == 0xffff)       // Undefined instrument / drum: silent
    return 0;

  return std::bitset<ControlRom::MAX_PARTIALS>
    (ctrlRom.instrument(instrumentIndex).partialsUsed).count();
}


Note::Note(uint8_t key, uint8_t velocity, ControlRom &ctrlRom, WaveRom &waveRom,
	   Settings *settings, int8_t partId, uint32_t serial, int startDelay)
  : _key(key),
    _sustain(false),
    _stopped(false),
    _releasing(false),
    _damped(false),
    _serial(serial),
    _7bScale(1/127.0),
    _settings(settings),
    _partId(partId)
{
  for (int p = 0; p < ControlRom::MAX_PARTIALS; p++)
    _partial[p] = NULL;

  // Every drum instrument carries its own effect depths in the drum set, and
  // the part's send is scaled by them. Measured on the SC-55mkII (emusc-match
  // PROVENANCE.md P-0239): with the part's reverb send full open, the two
  // kicks and the three hi-hats sit 11.7-12.2 dB below the snare, the crashes
  // and the rides, which agree with us to 0.7 dB. The control ROM gives those
  // instruments Reverb Depth 0x20 and 0x1f against 0x7f for the rest, and
  // 20*log10(32/127) is -11.98 dB. libEmuSC already reads this table into
  // Settings and reads it back for a SysEx write - it just never applied it.
  //
  // A tonal part has no such table and sends at the part's level alone.
  _reverbDepth = _chorusDepth = 1.0f;
  for (int p = 0; p < ControlRom::MAX_PARTIALS; p++)
    _partialRevShare[p] = _partialChoShare[p] = 1.0f;
  uint8_t rhythm = settings->get_param(PatchParam::UseForRhythm, partId);
  if (rhythm != 0) {
    _reverbDepth =
      settings->get_param(DrumParam::ReverbDepth, rhythm - 1, key) / 127.0f;
    _chorusDepth =
      settings->get_param(DrumParam::ChorusDepth, rhythm - 1, key) / 127.0f;
  }

  // 1. Find correct instrument index for note
  uint16_t instrumentIndex = _instrument_index(ctrlRom, settings, partId, key);

  if (instrumentIndex == 0xffff)        // Ignore undefined instruments / drums
    return;

  // LFO1 is shared between partials
  _LFO1 = new WaveGenerator(ctrlRom.instrument(instrumentIndex),
                            ctrlRom.lookupTables, settings, partId);

  // Every instrument in the Sound Canvas line has up to two partials.
  // But there are instances where there is a mismatch in the Control ROM,
  // where a defined partial have fewer sample IDs than break points. This is
  // most likely bugs by Roland, but the SC-55 simply ignores these partials.
  // Each partial carries a velocity range (bytes 65 and 67 of its 92-byte
  // record) and does not sound at all outside it. Measured on the SC-55mkII:
  // E.Piano 2v below velocity 75 is its first partial alone (the candidate
  // playing both partials was +6..+7 dB loud), and Funk Gt.2 switches
  // partials between velocities 115 and 116 (PROVENANCE.md, anomalies lane
  // pending B; P-0101). The level velocity rescale above the low bound is
  // TVA's (tva.cc), which sees the raw velocity here.
  // Once per note, before any partial: fix the pitch that every partial of
  // this note will glide from (PROVENANCE.md P-0278).
  Pitch::begin_note(partId);

  // The part's send level is the loudest tone's, so a tone's own share of it
  // is its send divided by that maximum: the two multiply back to the tone's
  // send at the device's scale, and the part-level parameter keeps the meaning
  // every other caller gives it. A patch whose tones all send alike - SAW Lead
  // sends 127 four times - gets 1.0 on every partial and does not move.
  const auto instSend = ctrlRom.instrument_send(instrumentIndex);

  std::bitset<ControlRom::MAX_PARTIALS>
    partialBits(ctrlRom.instrument(instrumentIndex).partialsUsed);
  for (int p = 0; p < ControlRom::MAX_PARTIALS; p++) {
    if (!partialBits.test(p))
      continue;
    const ControlRom::InstPartial &instPartial =
      ctrlRom.instrument(instrumentIndex).partials[p];
    // The Velocity Range is consulted only when the patch's own Velocity
    // Switch is set. The JV's firmware tests that bit first and jumps past
    // both comparisons when it is clear (ROM1 0xBDF), so on such a patch the
    // range bytes are dead data - 131 of the 192 factory patches. Devices with
    // no such switch leave velSwitch 0 and keep the unconditional behaviour
    // they had, which is the Sound Canvas's. Both bounds are inclusive.
    // scdb D-54.
    const bool gated = (ctrlRom.instrument(instrumentIndex).velSwitch != 0) ||
                       (settings->generation() != ControlRom::SynthGen::JV880);
    if (gated) {
      uint8_t low = instPartial.velRangeLow;
      uint8_t high = (instPartial.velRangeHigh > 0) ? instPartial.velRangeHigh
                                                    : 127;
      if (velocity < low || velocity > high)
        continue;
    }
    if (instPartial.revSend >= 0 && instSend.first > 0)
      _partialRevShare[p] = instPartial.revSend / (float) instSend.first;
    if (instPartial.choSend >= 0 && instSend.second > 0)
      _partialChoShare[p] = instPartial.choSend / (float) instSend.second;

    try {
      _partial[p] = new Partial(p, key, velocity, instrumentIndex, ctrlRom,
                                waveRom, _LFO1, settings, partId, startDelay);
    } catch (std::string errorMsg) {
      _partial[p] = NULL;
    }
  }
}


Note::~Note()
{
  for (int p = 0; p < ControlRom::MAX_PARTIALS; p++)
    delete _partial[p];

  delete _LFO1;
}


void Note::stop(void)
{
  if (_sustain) {                       // Hold pedal (hold1) or Sostenuto
    _stopped = true;

  } else {
    _releasing = true;

    for (int p = 0; p < ControlRom::MAX_PARTIALS; p++)
      if (_partial[p])
        _partial[p]->stop(_releaseVelocity);
  }
}


// The release velocity is kept: a note held by the pedal releases later with
// the velocity its note off carried, which is what the JV-880 does (its
// note-off handler stores the byte per key and the release reads it back).
void Note::stop(uint8_t key, uint8_t releaseVelocity)
{
  if (key == _key) {
    _releaseVelocity = releaseVelocity;

    if (_sustain)                       // Hold pedal (hold1) or Sostenuto
      _stopped = true;

    _releasing = true;

    for (int p = 0; p < ControlRom::MAX_PARTIALS; p++)
      if (_partial[p])
        _partial[p]->stop(_releaseVelocity);
  }
}


void Note::damp(float dBPerMillisecond)
{
  _damped = true;

  for (int p = 0; p < ControlRom::MAX_PARTIALS; p++)
    if (_partial[p])
      _partial[p]->damp(dBPerMillisecond);
}


bool Note::partial_live(int p)
{
  return p >= 0 && p < ControlRom::MAX_PARTIALS &&
         _partial[p] && !_partial[p]->is_damping();
}


int Note::damp_partial(int p, float dBPerMillisecond)
{
  if (!partial_live(p))
    return 0;

  _partial[p]->damp(dBPerMillisecond);
  return 1;
}


void Note::sustain(bool state)
{
  _sustain = state;

  if (state == false && _stopped == true)
    stop(_key, _releaseVelocity);
}


void Note::update(void)
{
  if (_LFO1) _LFO1->update();
  for (int p = 0; p < ControlRom::MAX_PARTIALS; p++)
    if (_partial[p]) _partial[p]->update();
}


bool Note::get_sample_set(std::array<std::array<float, 256>, 2> &dryBus,
			  std::array<float, 256> &reverbSend,
			  std::array<float, 256> &chorusSend)
{
  bool finished[ControlRom::MAX_PARTIALS] = {0};

  // Each partial writes its send into a block of its own, so the drum
  // instrument's depths AND the tone's own share of the part's send can be
  // applied before it joins the part's send. One block per partial rather than
  // one per note is what makes the per-tone send possible (scdb D-40); a
  // device without per-tone sends leaves every share at 1.0 and the sum is
  // identical to the single-block form it replaces.
  for (int p = 0; p < ControlRom::MAX_PARTIALS; p ++) {
    if  (_partial[p] == NULL) {
      finished[p] = 1;
      continue;
    }
    std::array<float, 256> partialSend = {};
    finished[p] = _partial[p]->get_sample_set(dryBus, partialSend);

    const float rd = _reverbDepth * _partialRevShare[p];
    const float cd = _chorusDepth * _partialChoShare[p];
    for (int i = 0; i < 256; i ++) {
      reverbSend[i] += partialSend[i] * rd;
      chorusSend[i] += partialSend[i] * cd;
    }
  }

  for (int p = 0; p < ControlRom::MAX_PARTIALS; p ++)
    if (finished[p] == false)
      return 0;

  return 1;
}


int Note::get_num_partials()
{
  int numPartials = 0;
  for (int p = 0; p < ControlRom::MAX_PARTIALS; p ++)
    if (_partial[p] && !_partial[p]->is_damping())
      numPartials ++;

  return numPartials;
}


int Note::get_current_lfo(int lfo)
{
  if (lfo == 0)
    return _LFO1->value();
  if (lfo == 1 && _partial[0])
    return _partial[0]->get_current_lfo();
  if (lfo == 2 && _partial[1])
    return _partial[1]->get_current_lfo();

  return 0;
}


int Note::get_current_pitch(bool partial)
{
  if (!_partial[partial])
    return 0;

  return _partial[partial]->get_current_pitch();
}


int Note::get_current_tvf(bool partial)
{
  if (!_partial[partial])
    return 0;

  return _partial[partial]->get_current_tvf();
}


int Note::get_current_tva(bool partial)
{
  if (!_partial[partial])
    return 0;

  return _partial[partial]->get_current_tva();
}

}
