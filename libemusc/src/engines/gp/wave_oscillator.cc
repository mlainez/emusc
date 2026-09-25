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


#include "wave_oscillator.h"

#include <algorithm>


namespace EmuSC { namespace Gp {


namespace {

constexpr std::array<std::array<float, 128>, 3>
q12_to_float(const uint16_t (&lut)[3][128])
{
  std::array<std::array<float, 128>, 3> coeffs{};
  for (int c = 0; c < 3; c++)
    for (int r = 0; r < 128; r++)
      coeffs[c][r] = static_cast<float>(lut[c][r]) / 4096.0f;
  return coeffs;
}

constexpr bool
q12_round_trips(const std::array<std::array<float, 128>, 3> &coeffs,
                const uint16_t (&lut)[3][128])
{
  for (int c = 0; c < 3; c++)
    for (int r = 0; r < 128; r++)
      if (coeffs[c][r] * 4096.0f != static_cast<float>(lut[c][r]))
        return false;
  return true;
}

}  // namespace


constexpr std::array<std::array<float, 128>, 3>
WaveOscillator::_interpolationCoeffs = q12_to_float(_interpolationLUT);


WaveOscillator::WaveOscillator(ControlRom::Sample *ctrlSample,
                               std::vector<float> *pcmSamples,
                               std::function<void(void)> cb)
  : _pcmSamples(pcmSamples),
    _phase(0.0f),
    _loopMode{ctrlSample->loopMode},
    _firstRunCompleteCallback(cb),
    _firstRunComplete(false)
{
  // TODO: Add check if note is portamento / legato to skip attack phase
//  if (!portamento)
  _sampleStart = 0;
//  else
//    sampleStart = ctrlSample->portaOffset;

  _sampleEnd = ctrlSample->sampleLen;
  _loopStart = _sampleEnd - ctrlSample->loopLen;
  _loopLength = ctrlSample->loopLen;

  if (_loopMode == LoopMode::PingPong) {
    _sampleEnd = ctrlSample->sampleLen + _loopLength + 1;
    _loopStart = _sampleEnd - 2 * ctrlSample->loopLen - 1;
  }

  _index = _sampleStart;
}


void WaveOscillator::get_sample_set(Pitch *pitch, float pitchBend,
                                    std::array<float, 256> &dryBus)
{
  // Scaling by 2^-14 before rather than after the multiply is exact: pitchBend
  // is a positive 2^(semitones/12) factor and the phase increment a positive
  // ratio, both far from the float range where the product could underflow or
  // overflow, so the scale commutes with the product's rounding.
  const float bendScale = pitchBend / 16384.0f;

  // The decoded sample set never changes while a voice plays.
  const float *pcm = _pcmSamples->data();
  const int lastSample = static_cast<int>(_pcmSamples->size()) - 1;

  for (int i = 0; i < 256; i++) {
    float output = _interpolate(pcm, lastSample);
    dryBus[i] = output;

    _phase += bendScale * pitch->get_phase_increment();
    while (_phase >= 1.0f) {
      _phase -= 1.0f;

      if (!_firstRunComplete && _index >= _sampleEnd) {
        _firstRunComplete = true;
        if (_firstRunCompleteCallback) _firstRunCompleteCallback();
      }

      _index++;
      if (_index > _sampleEnd)
	_index = _loopStart;
    }
  }
}


// Interpolation algorithm is based on information from the Nuked-SC55 project
// by nukeykt
float WaveOscillator::_interpolate(const float *pcm, int lastSample)
{
  // Each tap wraps to the loop start past the sample end, then is clamped to
  // the decoded sample set.
  const int t0 = _index;
  const int t1 = (t0 + 1 > _sampleEnd) ? _loopStart : t0 + 1;
  const int t2 = (t1 + 1 > _sampleEnd) ? _loopStart : t1 + 1;
  const int t3 = (t2 + 1 > _sampleEnd) ? _loopStart : t2 + 1;

  float s0 = pcm[std::clamp(t0, 0, lastSample)];
  float s1 = pcm[std::clamp(t1, 0, lastSample)];
  float s2 = pcm[std::clamp(t2, 0, lastSample)];
  float s3 = pcm[std::clamp(t3, 0, lastSample)];

  // Hardware uses only the top 7 bits of the fractional phase.
  int r = static_cast<int>(_phase * 128.0f) & 127;

  static_assert(q12_round_trips(_interpolationCoeffs, _interpolationLUT),
                "Q12 interpolation coefficients must convert to float exactly");
  float c0 = _interpolationCoeffs[0][r];
  float c1 = _interpolationCoeffs[1][r];
  float c2 = _interpolationCoeffs[2][r];

  return s0 + c0 * (s1 - s0) + c1 * (s2 - s1) + c2 * (s3 - s2);
}


}}  // namespace EmuSC::Gp
