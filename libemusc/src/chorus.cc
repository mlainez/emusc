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

#include "chorus.h"

#include <algorithm>
#include <cmath>


namespace EmuSC {


Chorus::Chorus(Settings *settings, const struct ControlRom::LookupTables &LUT)
  : _settings(settings),
    _LUT(LUT),
    _jv(false),
    _jvLaw(nullptr),
    _jvType(-1),
    _jvLoadSeen(0),
    _jvHold(0),
    _sweepIndex(0),
    _pIn(0x3800),
    _pTap1(0x3800 + 0x1e1 + 200), _pTap2(0x3800 + 0x1e1),
    _phase(0), _v9(0), _v10(0),
    _preA(0.0f), _preB(1.0f),
    _g2L(1.0f), _g2S(0.0f), _g3R(0.0f), _g3F(0.0f),
    _g4L(0.0f), _g4S(0.0f), _g5R(1.0f), _g5F(0.0f),
    _preLpfState(0.0f), _fbSample(0.0f),
    _phaseInc(0x47),
    _sAddress(0x1e1),
    _loopOfs(0x1e1), _span(200),
    _chorusMacroSeen(-1)
{
  _rBuffer.fill(0.0f);

  if (_settings->device()->chorusLawKind == ChorusLawKind::JVSweptPointer) {
    // A device that says it sweeps a read pointer needs the records to sweep
    // between. Without them there is no geometry, so it keeps the generic path
    // rather than sweeping between two zeros.
    const ChorusJvLaw &law = _settings->device()->chorusJv;
    const int n = law.types * 5;
    if (n > 0 && n <= (int) _LUT.JVChorusRecords.size()) {
      _jv = true;
      for (int t = 0; t < law.types; t++) {
        const int *r = &_LUT.JVChorusRecords[t * 5];
        if (r[3] <= r[2] || r[2] <= r[0] || r[4] == 0)
          _jv = false;
      }
      if (_jv)
        _jvLaw = &law;
    }
  }
}


void Chorus::update(void)
{
  if (_jv)
    _update_jv();
  else
    _update_sound_canvas();
}


// The JV-880's chorus, from its own firmware (PROVENANCE.md P-0394).
//
// The driver reads a five-word record for the selected type and computes, with
// hi16() the high word of a 32-bit product and every multiply truncating:
//
//   span    = w3 - w2
//   span'   = span - hi16((2 * Rate * 0xF1) * span)
//   addr2   = w2 + 2 * hi16((Depth * 0xE1 + 0x1000) * span')
//   F00A = w2   F00E = addr2   F006 = w2 + 1   F010 = 2 * hi16(f * w4)
//
// on pseudo-voice slot 0x1F - the same register block that addresses wave ROM
// for a note voice. So w2..addr2 is the window the read pointer sweeps and F010
// is how fast it sweeps. Two consequences the arithmetic hands over directly:
// the excursion is the window, which Depth scales from 12.5 % to 99.7 % of the
// type's span, and the period is window/increment, in which the shared f(Depth)
// cancels - so Rate alone sets the period, through g(Rate), while also
// shortening the window to as little as 6.6 % of the span.
void Chorus::_update_jv(void)
{
  const ChorusJvLaw &law = *_jvLaw;

  int type = std::clamp((int) _settings->get_param(PatchParam::ChorusMacro),
                        0, law.types - 1);
  const int *rec = &_LUT.JVChorusRecords[type * 5];
  const int w0 = rec[0], w1 = rec[1], w2 = rec[2], w3 = rec[3], w4 = rec[4];

  int rate  = std::clamp((int) _settings->get_param(PatchParam::ChorusRate), 0, 127);
  int depth = std::clamp((int) _settings->get_param(PatchParam::ChorusDepth), 0, 127);

  // The window. The rate term is a BYTE doubling in the firmware, which is why
  // the parameter's own 0..127 range is what it is: 2 * 127 is the last value
  // that still fits.
  const int f     = depth * law.depthScale + law.depthOffset;
  const int span  = w3 - w2;
  const int span2 = span - (int) ((((int64_t) ((2 * rate) & 0xff) * law.rateScale)
                                   * span) >> 16);
  const int addr2 = w2 + 2 * (int) (((int64_t) f * span2) >> 16);

  // The record's addresses are absolute words of the effect PSRAM and w0 is the
  // input's write pointer, so the window becomes delays of w2 - w0 upward: the
  // base delay is the type's, not a parameter. That is the JV's answer to the
  // Sound Canvas's ChorusDelay, which this machine does not have - and it is
  // what makes CHORUS2 a flanger, since its base delay is 14 samples.
  _loopOfs  = w2 - w0;
  _span     = addr2 - w2;
  _phaseInc = (uint16_t) (2 * (((int64_t) f * w4) >> 16));

  // A low Depth against a high Rate truncates the window to nothing, and a read
  // pointer whose start and end are the same address cannot move: hold it still
  // rather than letting an increment drive a sweep with nowhere to go.
  if (_span <= 0) {
    _span = 0;
    _phaseInc = 0;
  }

  // A new record restarts the sweep at the position the driver writes, w2 + 1.
  // A performance load does the same whether or not the type changes, and
  // then holds: the effect drivers rebuild reverb and chorus with waits
  // between their register writes, and the return is silent and the pointer
  // parked until they finish (ChorusJvLaw::loadHoldMs). The ~50 ms mute across
  // a type change alone is not modelled.
  const unsigned loads = _settings->device_performance_loads();
  if (type != _jvType || loads != _jvLoadSeen) {
    if (loads != _jvLoadSeen)
      _jvHold = law.loadHoldMs * 32;           // the automaton runs at 32 kHz
    _jvType = type;
    _jvLoadSeen = loads;
    _sAddress = _loopOfs + 1;
    _dir = false;
    _subPhase = 0;
  }

  // The pre-LPF, PER TYPE. The JV has no chorus pre-LPF parameter, but it does
  // not need one: the driver writes record word w1 straight to slot 0x1F F012
  // (ROM2 0x7409-0x743E), and F012 is this chip's pre-LPF coefficient pair. The
  // three records carry 0x1828 / 0x2818 / 0x003F, and the halves are
  // complementary - 0x18 + 0x28 = 0x40, 0x00 + 0x3F = 0x3F - which is the
  // one-pole (a, 64 - a) signature. The reverb driver uses the same register the
  // same way, writing record word 21 as an (a, 64 - a) mix pair to slot 0x1E
  // F012 (ROM2 0x70EE).
  //
  // The byte order is forced rather than chosen: CHORUS3's 0x003F is
  // byte-for-byte what the Sound Canvas law below produces for NO filtering
  // (k = 0 gives _preA = 0, _preB = 0x3f/64), so the HIGH byte is _preA. Taken
  // the other way round CHORUS3 would be (0x3F, 0x00), which that law cannot
  // produce and which would silence the chorus.
  //
  // So CHORUS1 low-passes at a 0.375 pole, CHORUS2 at 0.625 - they are the same
  // pair with the roles exchanged, which is why the two record words are
  // byte-swapped copies - and CHORUS3 runs open. This had been pinned at
  // CHORUS3's bypass for all three types, so the boot performance's CHORUS1 ran
  // with no filter where the machine darkens the wet signal.
  //
  // Not modelled: the driver walks the LOW byte up from 1 to its stored value,
  // one step per busy-wait, so the input coefficient FADES IN over 40 / 24 / 63
  // steps on a type change (ROM2 0x7411-0x7446). Steady state is what matters
  // for a render that does not change chorus type.
  _preA = (w1 >> 8) / law.coeffUnity;
  _preB = (w1 & 0xff) / law.coeffUnity;

  // The return matrix: eight BYTE writes, F014..F01B = (mix, rev, 0, fb) then
  // (0, rev, mix, fb), where 64 is unity. Chorus Output is a HARD SWITCH - the
  // wet level goes to the mix pair or the reverb pair with a zero in the other,
  // never to both - and our ChorusSendToReverb carries that bit rather than a
  // level, because the JV has no send level either.
  const int lvl = (_settings->get_param(PatchParam::ChorusLevel) & 0x7f)
                  >> law.levelShift;
  const int fb  = (_settings->get_param(PatchParam::ChorusFeedback) & 0x7f)
                  >> law.feedbackShift;
  const bool toReverb = _settings->get_param(PatchParam::ChorusSendToReverb) != 0;

  const float mix = toReverb ? 0.0f : lvl / law.coeffUnity;
  const float rev = toReverb ? lvl / law.coeffUnity : 0.0f;
  const float fbg = fb / law.coeffUnity;

  _g2L = mix;   _g2S = rev;   _g3R = 0.0f;  _g3F = fbg;
  _g4L = 0.0f;  _g4S = rev;   _g5R = mix;   _g5F = fbg;
}


void Chorus::_update_sound_canvas(void)
{
  // Pre-LPF: 1-pole lowpass filter 0-7, but capped at 4 (same as reverb)
  int k = std::clamp((int) _settings->get_param(PatchParam::ChorusPreLPF), 0, 4);

  _preA = (8 * k) / 64.0f;
  _preB = (0x3f - 8 * k) / 64.0f;

  // Depth: loop span = 10*n samples (minimum 2)
  int depth = std::clamp((int) _settings->get_param(PatchParam::ChorusDepth), 0, 127);
  _span = std::max(2, 10 * depth);

  // Delay: loop start = input + 1 + 6*n samples
  int delay = std::clamp((int) _settings->get_param(PatchParam::ChorusDelay), 0, 127);
  _loopOfs = 1 + 6 * delay;

  // Rate: measured from the reference machine (PROVENANCE.md P-0274), the LFO
  // frequency follows
  //
  //   f = 0.00531706 * floor(23.75 * rate) * span / (span + 11)  [Hz]
  //
  // to better than 0.1 % over rate 1..127 and depth 1..127, and the LFO is
  // frozen at rate 0.  floor(23.75 * rate) is (95 * rate) / 4 exactly in
  // integer arithmetic.  The sweep automaton below ticks at 32000 Hz, takes
  // one address step per 0x4000 of sub-phase, and consumes 2*span + 2 steps
  // per triangle cycle (one step is spent turning around at each edge), so
  // the increment that lands it on f is f * 0x4000 * (2*span + 2) / 32000.
  int rp = std::clamp((int) _settings->get_param(PatchParam::ChorusRate), 0, 127);
  int rateUnits = (95 * rp) / 4;
  double fLfo = 0.00531706 * rateUnits * _span / (double) (_span + 11);
  _phaseInc = (uint16_t) lround(fLfo * 16384.0 * (2 * _span + 2) / 32000.0);

  int feedback = std::clamp((int) _settings->get_param(PatchParam::ChorusFeedback), 0, 127);
  _g3F = (feedback >> 1) / 64.0f;
  _g5F = 0.0f;

  int level = std::clamp((int) _settings->get_param(PatchParam::ChorusLevel), 0, 127);
  _g2L = level / 64.0f;  _g3R = 0.0f;
  _g4L = 0.0f;         _g5R = level / 64.0f;

  int send = std::clamp((int) _settings->get_param(PatchParam::ChorusSendToReverb), 0, 127);
  _g2S = (send >> 1) / 64.0f;
  _g4S = 0.0f;
}


// Chorus algorithm based on information from the Nuked-SC55 project by nukeykt
void Chorus::process_sample(float input, float output[2], float *reverbSend)
{
  auto read = [&](int delay, int ofs) -> float {
    return _rBuffer[(_pIn + _sweepIndex + delay + ofs) & rBufferMask];
  };

  auto write = [&](uint16_t base, float v) {
    _rBuffer[(base + _sweepIndex) & rBufferMask] = v;
  };

  // The JV's post-load hold: the driver has cleared the input coefficient, the
  // read-rate increment and the return bytes, so the line is written with
  // silence, the pointer stays where it was parked and nothing comes back.
  if (_jvHold > 0) {
    _jvHold--;
    _preLpfState = 0.0f;
    _fbSample = 0.0f;
    write(_pIn, 0.0f);
    output[0] = output[1] = 0.0f;
    *reverbSend = 0.0f;
    _sweepIndex = (_sweepIndex - 1) & rBufferMask;
    return;
  }

  // loop and end are offsets from the write head, so _sAaddress is an offset
  // too, and the taps come out as delays in [0..span]
  {
    int loop = _loopOfs, end = _loopOfs + _span;
    int sp = (_subPhase & 0x3fff) + _phaseInc;
    int of = (sp >> 14) & 7;
    _subPhase = sp & 0x3fff;
    for (int k = 0; k < of; k++) {
      bool atEdge = _dir ? (_sAddress == loop) : (_sAddress == end);
      if (atEdge) _dir = !_dir;
      else        _sAddress += _dir ? -1 : 1;
    }
    if (_sAddress < loop) _sAddress = loop;
    if (_sAddress > end)  _sAddress = end;

    _pTap2 = (uint16_t) (_sAddress);                // Delay of tap 2: 0..span
    _pTap1 = (uint16_t) (loop + end - _sAddress);   // Delay of tap 1: span..0

    uint16_t P = (uint16_t) (_subPhase | (_dir ? 0x8000 : 0));
    if (P & 0x8000) _v9  = P & 0x7fff; else _v10 = P & 0x7fff;
    uint16_t d = (uint16_t) (0x4000 - P);
    if (d & 0x8000) _v10 = d & 0x7fff; else _v9  = d & 0x7fff;
  }

  // Pre-LPF on (bus input + one-tick feedback).
  float bus = input + _fbSample;
  _preLpfState = _preA * _preLpfState + _preB * bus;

  write(_pIn, _preLpfState);

  // Tap 1: Interpolate current(+0) toward older(+1) by fraction f1
  float f1 = (float) (_v9 >> 8) / 64.0f;
  float tap1 = read(_pTap1, 0) * (1.0f - f1) + read(_pTap1, 1) * f1;

  // Tap 2 with the complementary fraction
  float f2 = (float) (_v10 >> 8) / 64.0f;
  float tap2 = read(_pTap2, 0) * (1.0f - f2) + read(_pTap2, 1) * f2;

  // Output matrix + bus sends.
  output[0]   = tap1 * _g2L + tap2 * _g4L;
  output[1]   = tap1 * _g3R + tap2 * _g5R;
  *reverbSend = tap1 * _g2S + tap2 * _g4S;
  _fbSample   = tap1 * _g3F + tap2 * _g5F;

  _sweepIndex = (_sweepIndex - 1) & rBufferMask;
}

}  // namespace EmuSC
