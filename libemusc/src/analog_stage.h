/*
 *  This file is part of libEmuSC, a Sound Canvas emulator library
 *
 *  The analog stage: what a device does to the finished mix after the chip.
 *
 *  Everything else in libEmuSC models the DIGITAL machine - the records the
 *  firmware reads, the arithmetic it does, the samples its DSP writes. A real
 *  module then puts those samples through a converter and an analog board, and
 *  the board is part of the instrument's sound rather than a detail of its
 *  packaging. On the JV-880 it is worth about +3 dB below 100 Hz and +4 dB
 *  above 10 kHz, which is audible and which no chip-level emulation of the
 *  digital side can produce.
 *
 *  THIS IS THE SINGLE PLACE for that class of thing. It is deliberately not a
 *  JV-880 feature: a device declares what its board does as DATA in its
 *  AnalogStageProfile and the code here never learns which device it is
 *  serving, so the next module's output stage is a few numbers in
 *  devices/<model>.cc and no change at all here or in synth.cc. A device that
 *  has not been measured declares nothing and its mix is passed through
 *  untouched - which is where every Sound Canvas profile stands today, and
 *  must stay until someone measures one.
 *
 *  Where it runs matters. The stage is applied at the HOST sample rate, after
 *  the resampler, because that is where the analog board sits: after the
 *  converter. Building it at the engine's 32 kHz would put the top of the
 *  audible band at Nyquist and there is no room for a high shelf there.
 *
 *  EMUSC_NO_ANALOG_STAGE=1 in the environment defeats it, so the raw digital
 *  output can still be rendered for measurement.
 */

#ifndef ANALOG_STAGE_H
#define ANALOG_STAGE_H

#include "device_profile.h"

#include <cstdint>
#include <vector>

namespace EmuSC
{

class AnalogStage
{
public:
  AnalogStage(const AnalogStageProfile &profile);

  // Rebuilds the filter for a new host rate and clears its state. Must be
  // called before the first process() - a stage that has never been given a
  // rate is inactive and passes the mix through.
  void set_sample_rate(uint32_t sampleRate);

  // In place, on `frames` samples of each channel.
  void process(float *left, float *right, int frames);

  void reset(void);

  bool active(void) const { return _active; }

private:
  // Transposed direct form II, with the state in double. The JV's low shelf
  // sits at 118 Hz with a Q of 0.72, which puts its poles close enough to DC
  // that single-precision state leaves audible noise in a quiet passage; the
  // filter itself is three sections on two channels, so the cost is nothing.
  struct Biquad
  {
    double b0, b1, b2, a1, a2;
    double s1[2], s2[2];                // one state pair per channel
  };

  const AnalogStageProfile &_profile;
  std::vector<Biquad> _sections;
  float _trim;
  bool  _active;

  // The RBJ audio-EQ cookbook forms. They are what the profile's numbers were
  // FITTED with (scdb devices/jv880/tools/analysis/output_response_fit.py), so
  // any other realisation of "a low shelf at 118 Hz" would be a different
  // curve from the one that was measured.
  static Biquad _design(const OutputSection &s, double sampleRate);
};

}

#endif  // ANALOG_STAGE_H
