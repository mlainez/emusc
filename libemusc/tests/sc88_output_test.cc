/* SPDX-License-Identifier: CC0-1.0 */
#include "engines/xp/output.h"

#include <cassert>
#ifdef NDEBUG
#error "this test is assertion-driven; NDEBUG compiles it away"
#endif
#include <cmath>
#include <cstring>

using namespace EmuSC::Xp;

int main()
{
  struct sc88_output out;
  double rate = 44100.0;
  double pi = 3.14159265358979323846;
  double sum;
  unsigned i;
  float y;

  /* Poisoned with values init must overwrite rather than merely leave
     alone: an uninitialised local's stack garbage could otherwise land on
     0 by accident and let a missing assignment pass unnoticed, since 0 is
     also several of the values checked below. */
  memset(&out, 0, sizeof out);
  out.enabled = false;
  out.sections = 99;
  out.hold_taps = 99;
  out.hold_pos = 99;

  /* The response list is empty by design (output.cc's own finding), so
     init designs no biquad section - only the converter hold and the DC
     blocker. */
  output_init(&out, rate);
  assert(out.enabled);
  assert(out.sections == SC88_OUTPUT_RESPONSE_SECTIONS);
  assert(out.sections == 0);
  assert(out.hold_taps == SC88_OUTPUT_HOLD_TAPS);
  assert(out.hold_pos == 0);

  /* The hold FIR is normalised to unity DC gain by construction: each tap
     is h[k] divided by the sum of every h[k] before division. */
  sum = 0.0;
  for (i = 0; i < out.hold_taps; ++i)
    sum += out.hold[i];
  assert(fabs(sum - 1.0) < 1e-5);

  /* The DC blocker's pole, `1 - 2*pi*10/rate`, recovered exactly from the
     10 Hz single-pole design target. */
  assert(fabs((double)out.dc_pole - (1.0 - 2.0 * pi * 10.0 / rate)) < 1e-6);

  /* The analog board's five one-pole sections, read off the schematic:
     R146||C152, R140||C144, R138+C141, R127||C131, R101+C113. */
  assert(SC88_OUTPUT_ANALOG_SECTIONS == 5);
  assert(SC88_OUTPUT_ANALOG[0].r_ohm == 4.7e3 &&
         SC88_OUTPUT_ANALOG[0].c_farad == 100e-12);
  assert(SC88_OUTPUT_ANALOG[2].r_ohm == 100.0 &&
         SC88_OUTPUT_ANALOG[2].c_farad == 680e-12);
  assert(SC88_OUTPUT_ANALOG[4].r_ohm == 1.8e3 &&
         SC88_OUTPUT_ANALOG[4].c_farad == 1000e-12);

  /* With no biquad section, a held DC input passes through the hold FIR
     (unity DC gain) and then only the DC blocker acts on it - and a
     one-pole DC blocker's own steady-state gain at DC is exactly zero, so
     a long-held constant decays toward silence. */
  {
    float stereo[2];
    unsigned n;
    for (n = 0; n < 20000; ++n) {
      stereo[0] = 1.0f;
      stereo[1] = -1.0f;
      output_process(&out, stereo, 1);
    }
    assert(fabsf(stereo[0]) < 1e-3f);
    assert(fabsf(stereo[1]) < 1e-3f);
  }

  /* The design is zero-phase: every frequency-sampling term is built from
     cos(2*pi*f*k/rate) and every window term from k*k, both even in k, so
     the tap at +k must equal the tap at -k about the centre. A phase bug
     in the frequency-sampling loop (a sign error, an off-by-one against
     `half`) breaks this symmetry without necessarily moving the sum
     checked above. */
  for (i = 0; i < out.hold_taps / 2; ++i)
    assert(fabsf(out.hold[i] - out.hold[out.hold_taps - 1 - i]) < 1e-6f);

  /* Reset clears every piece of running state, not just the parts that
     happen to matter for the DC-decay check above. */
  output_reset(&out);
  assert(out.hold_pos == 0);
  assert(out.dc_x[0] == 0.0f && out.dc_x[1] == 0.0f);
  assert(out.dc_y[0] == 0.0f && out.dc_y[1] == 0.0f);
  for (i = 0; i < out.hold_taps; ++i) {
    assert(out.hold_z[0][i] == 0.0f);
    assert(out.hold_z[1][i] == 0.0f);
  }

  /* A null output disables processing rather than reporting failure - it
     is not a code path a caller has to guard defensively. */
  {
    struct sc88_output disabled;
    float stereo[2] = {0.5f, 0.25f};
    output_init(&disabled, rate);
    disabled.enabled = false;
    output_process(&disabled, stereo, 1);
    assert(stereo[0] == 0.5f && stereo[1] == 0.25f);
  }

  return 0;
}
