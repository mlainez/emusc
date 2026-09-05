/*
 *  This file is part of libEmuSC, a Sound Canvas emulator library
 *
 *  See analog_stage.h for what this is and why it is not device-specific code.
 */

#include "analog_stage.h"

#include <cmath>
#include <cstdlib>

namespace EmuSC
{

AnalogStage::AnalogStage(const AnalogStageProfile &profile)
  : _profile(profile),
    _trim(1.0f),
    _active(false)
{}


AnalogStage::Biquad AnalogStage::_design(const OutputSection &s,
                                         double sampleRate)
{
  const double A  = std::pow(10.0, s.gainDb / 40.0);
  const double w  = 2.0 * M_PI * s.freq / sampleRate;
  const double cw = std::cos(w), sw = std::sin(w);
  const double al = sw / (2.0 * (s.q > 0.0 ? s.q : 0.7071));
  double b[3], a[3];

  switch (s.kind) {
  case OutputSection::Peaking:
    b[0] = 1 + al * A;  b[1] = -2 * cw;  b[2] = 1 - al * A;
    a[0] = 1 + al / A;  a[1] = -2 * cw;  a[2] = 1 - al / A;
    break;

  case OutputSection::LowShelf: {
    const double t = 2 * std::sqrt(A) * al;
    b[0] = A * ((A + 1) - (A - 1) * cw + t);
    b[1] = 2 * A * ((A - 1) - (A + 1) * cw);
    b[2] = A * ((A + 1) - (A - 1) * cw - t);
    a[0] = (A + 1) + (A - 1) * cw + t;
    a[1] = -2 * ((A - 1) + (A + 1) * cw);
    a[2] = (A + 1) + (A - 1) * cw - t;
    break;
  }

  case OutputSection::HighShelf:
  default: {
    const double t = 2 * std::sqrt(A) * al;
    b[0] = A * ((A + 1) + (A - 1) * cw + t);
    b[1] = -2 * A * ((A - 1) + (A + 1) * cw);
    b[2] = A * ((A + 1) + (A - 1) * cw - t);
    a[0] = (A + 1) - (A - 1) * cw + t;
    a[1] = 2 * ((A - 1) - (A + 1) * cw);
    a[2] = (A + 1) - (A - 1) * cw - t;
    break;
  }
  }

  Biquad q;
  q.b0 = b[0] / a[0];  q.b1 = b[1] / a[0];  q.b2 = b[2] / a[0];
  q.a1 = a[1] / a[0];  q.a2 = a[2] / a[0];
  q.s1[0] = q.s1[1] = q.s2[0] = q.s2[1] = 0.0;
  return q;
}


void AnalogStage::set_sample_rate(uint32_t sampleRate)
{
  _sections.clear();
  _trim = std::pow(10.0f, _profile.trimDb / 20.0f);
  _active = false;

  if (sampleRate == 0 || _profile.sections <= 0 || !_profile.response)
    return;

  if (getenv("EMUSC_NO_ANALOG_STAGE"))
    return;

  for (int i = 0; i < _profile.sections; i++) {
    const OutputSection &s = _profile.response[i];

    // A section whose centre has run past Nyquist cannot be realised, and the
    // bilinear transform would fold it back somewhere else rather than fail.
    // At any host rate this engine is asked for it is the high shelf that is
    // at risk, and dropping it is right: the band it lifts is not there.
    if (s.freq >= 0.49 * sampleRate)
      continue;

    _sections.push_back(_design(s, sampleRate));
  }

  _active = !_sections.empty() || _profile.trimDb != 0.0f;
}


void AnalogStage::reset(void)
{
  for (auto &q : _sections)
    q.s1[0] = q.s1[1] = q.s2[0] = q.s2[1] = 0.0;
}


void AnalogStage::process(float *left, float *right, int frames)
{
  if (!_active)
    return;

  float *ch[2] = { left, right };

  for (auto &q : _sections) {
    for (int c = 0; c < 2; c++) {
      double s1 = q.s1[c], s2 = q.s2[c];
      float *p = ch[c];

      for (int i = 0; i < frames; i++) {
        const double x = p[i];
        const double y = q.b0 * x + s1;
        s1 = q.b1 * x - q.a1 * y + s2;
        s2 = q.b2 * x - q.a2 * y;
        p[i] = (float) y;
      }

      q.s1[c] = s1;
      q.s2[c] = s2;
    }
  }

  if (_trim != 1.0f)
    for (int c = 0; c < 2; c++)
      for (int i = 0; i < frames; i++)
        ch[c][i] *= _trim;
}

}
