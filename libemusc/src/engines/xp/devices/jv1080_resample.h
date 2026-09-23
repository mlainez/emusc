/* SPDX-License-Identifier: CC0-1.0 */
/*
 *  The JV-1080 engine's own 32 kHz to host-rate converter.
 *
 *  The machine computes its audio at 32 kHz (scdb `01_hardware`: the wave
 *  ROM's own rate and 24.576 MHz / 768), and what its filters do near the
 *  top of the band is a property of that rate (`P-xxxx`, TASK-381). So the
 *  engine runs at 32 kHz whatever the host asks for, and this carries its
 *  output to the host rate: a streaming, stereo, Kaiser-windowed sinc
 *  interpolator. It is a clean converter and models nothing of the
 *  machine's own output stage - not its images above 16 kHz, not its
 *  analogue roll-off.
 *
 *  This is the JV-1080's alone. The Sound Canvas path's resampler
 *  (engines/gp/resampler.h) carries that device's measured output droop in
 *  its kernel, which is not this machine's.
 */

#ifndef EMUSC_XP_DEVICES_JV1080_RESAMPLE_H
#define EMUSC_XP_DEVICES_JV1080_RESAMPLE_H

#include <cstddef>

namespace EmuSC { namespace Xp {

/* Taps each side of the kernel's centre, and the phase table's
   resolution. */
constexpr int kJvResampleHalf = 32;
constexpr int kJvResamplePhases = 256;
constexpr int kJvResampleRing = 128;     /* > 2 * half, a power of two */

struct JvResampler {
  double ratio;          /* input samples per output sample */
  double position;       /* where the next output sits, in input samples */
  long long written;     /* input samples pushed, the primed zeros included */
  float ring[2][kJvResampleRing];
  /* (phases + 1) rows of 2 * half taps: row p is the kernel at fraction
     p / phases, tap j weighting the input j - half + 1 places after the
     sample at or before the output position. */
  float *table;
};

/* False on an allocation failure. inputRate and outputRate are in hertz. */
bool jv_resampler_init(struct JvResampler *rs, double inputRate,
                        double outputRate);
void jv_resampler_free(struct JvResampler *rs);
/* Back to its state at init: silence before input sample 0. */
void jv_resampler_reset(struct JvResampler *rs);
/* Whether the next output needs another input sample first. */
bool jv_resampler_needs_input(const struct JvResampler *rs);
void jv_resampler_push(struct JvResampler *rs, float left, float right);
/* The next output frame; call only once needs_input is false. */
void jv_resampler_pull(struct JvResampler *rs, float *left, float *right);

}}  // namespace EmuSC::Xp

#endif
