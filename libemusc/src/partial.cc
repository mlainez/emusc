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

// Pitch corrections that must be calculated for each partial
// Static corrections:
//  - Key difference between rootkey and actual key (drum is similar) [semitone]
//  - Sample pitch correction as stored with sample control data [?]
//  - Scale tuning (seems to affect drums also in some unkown manner) [cent]
//  - Master key shift (not for drums) [semitone]
//  - Part key shift (on drums only for SC-55mk2+) [semitone]
//  - Master Coarse Tuning (RPN #2) [semitone]
//  - PitchKeyFollow from partial definition
// Dynamic corrections:
//  - Master tune (SysEx) [cent]
//  - Master fine tuning (RPN #1) [cent]
//  - Fine tune offset [Hz]
//  - Pitch bend

// Some radnom notes:
// - All coarse tune variables are in semitones. They are all added to the key
//   to find the correct rootkey. They are also only calculated once and do not
//   change over the time of a partial.
// - No key shifts affects drum parts on SC-55 (SC-55 OM page 17 & 24), but
//   part key shift affects drum parts on SC-55mk2+ (SC-55mkII OM page 21)
// - Pitch corrections in this class should perhaps be moved to the TVP class


#include "partial.h"

#include <iostream>
#include <cmath>


namespace EmuSC {


Partial::Partial(int partialId, uint8_t key, uint8_t velocity,
		 uint16_t instrumentIndex, ControlRom &ctrlRom, WaveRom &waveRom,
		 WaveGenerator *LFO1, Settings *settings, int8_t partId,
		 int startDelay)
  : _instPartial(ctrlRom.instrument(instrumentIndex).partials[partialId]),
    _settings(settings),
    _partId(partId),
    _sampleRunComplete(false),
    _startDelay((startDelay < 0) ? 0 : ((startDelay > 255) ? 255 : startDelay)),
    _delayDrained(false),
    _damping(false),
    _dampComplete(false),
    _dampGain(1),
    _dampFactor(1),
    _LFO2(NULL),
    _pitch(NULL),
    _tvf(NULL),
    _tva(NULL),
    _pitchAdj(0)
{
  _delayL.fill(0.0f);
  _delayR.fill(0.0f);
  _delayS.fill(0.0f);

  _drumSet = settings->get_param(PatchParam::UseForRhythm, partId);
  if (_drumSet)
    _drumRxNoteOff = _settings->get_param(DrumParam::RxNoteOff, _drumSet-1,key);

  _LFO2 = new WaveGenerator(_instPartial, ctrlRom.lookupTables,settings,partId);

  _pitch = new Pitch(ctrlRom, instrumentIndex, partialId, key, velocity, LFO1,
                     _LFO2, settings, partId);

  _tvf = new TVF(_instPartial, key, velocity, LFO1, _LFO2,
                 ctrlRom.lookupTables, settings, partId);

  int sampleIndex = _pitch->get_sample_id();
  _tva = new TVA(ctrlRom, key, velocity, sampleIndex, LFO1, _LFO2, settings,
                 partId, instrumentIndex, partialId);

  _ctrlSample = &ctrlRom.sample(sampleIndex);
  _pcmSamples = &waveRom.samples(sampleIndex).samplesF;
  _waveOscillator = new WaveOscillator(_ctrlSample, _pcmSamples,
                                       std::bind(&Partial::first_run_cb, this));

  // FIXME: A few sample definitions in the SC-55 ROM have loop length >
  // sample length. This makes EmuSC crash as it loops outside range. The
  // following hack prevents a crash, but audio is wrong for these samples.
  // TODO: Figure out why this works on the real hardware.
  // Example: Concert Cym. (Con_sym), #59 of Orchestra drumkit
  if (_ctrlSample->loopLen > _ctrlSample->sampleLen) {
    std::cerr << "libEmuSC: Internal error, loop length > sample length!"
              << std::endl << " => loop length = sample length" << std::endl;
    _ctrlSample->loopLen = _ctrlSample->sampleLen;
  }
}


Partial::~Partial()
{
  delete _pitch;
  delete _tvf;
  delete _tva;

  delete _waveOscillator;

  delete _LFO2;
}


bool Partial::get_sample_set(std::array<std::array<float, 256>, 2> &dryBus,
			     std::array<float, 256> &sendBus)
{
  const bool finished = (_tva->finished() || _dampComplete);

  // A finished voice generates nothing, but one that started inside a control
  // period still owes the samples its start delay pushed past the end of the
  // last period. Emit those once, then report the voice as done.
  if (finished && (_startDelay == 0 || _delayDrained))
    return 1;

  std::array<std::array<float, 256>, 2> partialBuf = {};
  std::array<float, 256> partialSend = {};

  if (finished) {
    _delayDrained = true;
  } else {
    _waveOscillator->get_sample_set(_pitch,
                                    _settings->get_pitchBend_factor(_partId),
                                    partialBuf[0]);

    _tvf->apply_sample_set(partialBuf[0]);
    _tva->apply_sample_set(partialBuf, partialSend);

    // The oscillator reports the end of a non-looping sample while it is still
    // filling the current control block. Terminating the TVA from inside that
    // report sets the envelope and dynamic levels to zero before the block is
    // amplified, so the samples the oscillator had already produced are
    // multiplied by zero and lost. Samples shorter than one control block are
    // lost in their entirety, which is why "Square Click" and the other short
    // one-shot percussion sounds rendered as digital silence (PROVENANCE.md
    // P-0039). Terminate the voice only once the block has been amplified.
    if (_sampleRunComplete) {
      _sampleRunComplete = false;
      _tva->set_phase(Envelope::Phase::Terminated);
    }

    // A partial whose voice was taken by another note is faded out rather than
    // cut off; the fade is a fixed rate per model and is independent of the
    // tone's own release (PROVENANCE.md P-0080).
    if (_damping) {
      for (int i = 0; i < 256; i++) {
        partialBuf[0][i] *= _dampGain;
        partialBuf[1][i] *= _dampGain;
        partialSend[i]   *= _dampGain;
        _dampGain *= _dampFactor;
      }

      if (_dampGain < 1e-4)              // -80 dB, below a 16 bit sample step
        _dampComplete = true;
    }
  }

  if (_startDelay == 0) {
    for (int i = 0; i < 256; i++) {
      dryBus[0][i] += partialBuf[0][i];
      dryBus[1][i] += partialBuf[1][i];
      sendBus[i]   += partialSend[i];
    }

    return 0;
  }

  // The voice started _startDelay samples into some control period, so what
  // it plays in this one is the tail held over from the last period followed
  // by the head of what it has just generated; the rest is held over again.
  const int d = _startDelay;

  for (int i = 0; i < d; i++) {
    dryBus[0][i] += _delayL[i];
    dryBus[1][i] += _delayR[i];
    sendBus[i]   += _delayS[i];
  }
  for (int i = d; i < 256; i++) {
    dryBus[0][i] += partialBuf[0][i - d];
    dryBus[1][i] += partialBuf[1][i - d];
    sendBus[i]   += partialSend[i - d];
  }
  for (int i = 0; i < d; i++) {
    _delayL[i] = partialBuf[0][256 - d + i];
    _delayR[i] = partialBuf[1][256 - d + i];
    _delayS[i] = partialSend[256 - d + i];
  }

  return _delayDrained ? 1 : 0;
}


void Partial::stop(uint8_t releaseVelocity)
{
  // Ignore note off for uninterruptible drums (set by drum set flag)
  if (!(_drumSet && !_drumRxNoteOff)) {
    if (_pitch) _pitch->note_off();
    if (_tvf) _tvf->note_off(releaseVelocity);
    if (_tva) _tva->note_off(releaseVelocity);
  }
}


// Give up this partial's voice: fade it out at a fixed rate and terminate it.
// Unlike a note off this is not the tone's release; the rate is a property of
// the synth, measured on both models (PROVENANCE.md P-0080).
void Partial::damp(float dBPerMillisecond)
{
  if (_damping)
    return;

  // The fade steps once per sample of the voice loop, which runs at libEmuSC's
  // fixed 32 kHz internal rate (resampler.h), not at the host's output rate.
  const float internalRate = 32000.0;

  float dBPerSample = dBPerMillisecond * 1000.0 / internalRate;
  _dampFactor = std::pow(10.0, -dBPerSample / 20.0);
  _dampGain = 1;
  _damping = true;
}


// Update parameters every 256th sample @32k
void Partial::update(void)
{
  if (_pitch) _pitch->update();
  if (_tvf) _tvf->update();
  if (_tva) _tva->update();

  if (_LFO2) _LFO2->update();
}


void Partial::first_run_cb(void)
{
  // Looping samples have their pitch tuned after the first loop point
  // Non-looping samples shall be terminated (if not complete already)
  if (_ctrlSample->loopMode != 2)
    _pitch->first_sample_run_complete();
  else
    _sampleRunComplete = true;
}

}
