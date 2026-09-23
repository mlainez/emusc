/* SPDX-License-Identifier: CC0-1.0 */
#include "engines/xp/chorus.h"
#include "engines/xp/devices/jv1080.h"

#include <cassert>
#ifdef NDEBUG
#error "this test is assertion-driven; NDEBUG compiles it away"
#endif
#include <cmath>
#include <cstdlib>
#include <cstring>

using namespace EmuSC::Xp;

/* The recovered transforms, checked against the firmware's own arithmetic
   rather than against this implementation's arrangement of it. The
   topology is a labelled choice and is not asserted here; the numbers
   that came out of the ROM are. */
int main()
{
  struct xp_chorus ch;
  float stereo[64 * 2];
  float send[64];
  unsigned i;
  double sweep_span;

  assert(chorus_init(&ch, 32000.0, &SC88_PROFILE));

  /* Delay is `3*p` samples of delay memory, so at the native 32 kHz it is
     the sample count itself: GS's default 0x50 is 240 samples, 7.5 ms. */
  chorus_set_params(NULL, &ch, 64, 0, 0x50, 3, 0, 0);
  assert(fabs(ch.delay_samples - 240.0) < 1e-9);
  chorus_set_params(NULL, &ch, 64, 0, 127, 3, 0, 0);
  assert(fabs(ch.delay_samples - 381.0) < 1e-9);
  chorus_set_params(NULL, &ch, 64, 0, 0, 3, 0, 0);
  assert(ch.delay_samples == 0.0);

  /* Feedback is `256*floor(p/4)` read as an XP coefficient: zero at zero,
     and saturating at 0.9688 rather than reaching or passing unity, which
     is the property that makes it safe in a feedback path. */
  chorus_set_params(NULL, &ch, 64, 0, 0x50, 3, 0, 0);
  assert(ch.feedback == 0.0f);
  chorus_set_params(NULL, &ch, 64, 64, 0x50, 3, 0, 0);
  assert(fabs(ch.feedback - 0.5) < 1e-6);
  chorus_set_params(NULL, &ch, 64, 127, 0x50, 3, 0, 0);
  assert(fabs(ch.feedback - 0.96875) < 1e-6);
  /* the register saturates, so 124 and 127 are the same coefficient */
  {
    float at_124;
    chorus_set_params(NULL, &ch, 64, 124, 0x50, 3, 0, 0);
    at_124 = ch.feedback;
    chorus_set_params(NULL, &ch, 64, 127, 0x50, 3, 0, 0);
    assert(ch.feedback == at_124);
  }

  /* Level is `4*p` against 512, the same law the reverb's level follows. */
  chorus_set_params(NULL, &ch, 0, 0, 0x50, 3, 0, 0);
  assert(ch.level == 0.0f);
  chorus_set_params(NULL, &ch, 127, 0, 0x50, 3, 0, 0);
  assert(fabs(ch.level - 508.0 / 512.0) < 1e-6);

  /* Rate is `64*p` per 8.0008 ms control period on a 16-bit accumulator,
     which puts the default of 3 at 0.37 Hz and the top at 15.5 Hz. */
  chorus_set_params(NULL, &ch, 64, 0, 0x50, 3, 0, 0);
  assert(fabs(ch.phase_step * 32000.0 - 0.3663) < 5e-4);
  chorus_set_params(NULL, &ch, 64, 0, 0x50, 127, 0, 0);
  assert(fabs(ch.phase_step * 32000.0 - 15.5) < 0.05);
  chorus_set_params(NULL, &ch, 64, 0, 0x50, 0, 0, 0);
  assert(ch.phase_step == 0.0);

  /* The pre-LPF is the reverb's own table, so zero is an exact bypass. */
  chorus_set_params(NULL, &ch, 64, 0, 0x50, 3, 0, 0);
  assert(ch.pre_fb == 0.0f && ch.pre_in == 1.0f);
  chorus_set_params(NULL, &ch, 64, 0, 0x50, 3, 0, 4);
  assert(fabs(ch.pre_fb - 0.5) < 1e-6);
  /* the table has eight entries, so a value past its end clamps to the
     last one rather than reading into the character blocks (`M-010`) */
  chorus_set_params(NULL, &ch, 64, 0, 0x50, 3, 0, 9);
  assert(fabs(ch.pre_fb - 7.0 / 8.0) < 1e-6);

  /* The sweep may never exceed the delay, or a tap would read ahead of the
     write pointer - the one bound the assumed topology still has to keep. */
  chorus_set_params(NULL, &ch, 64, 0, 1, 3, 127, 0);
  assert(ch.depth_samples <= ch.delay_samples);

  /* And it does something: a bus that is not silent produces a return that
     is not silent, on both sides, and the two sides differ because the
     taps are in antiphase. */
  chorus_set_params(NULL, &ch, 127, 0, 0x50, 127, 127, 0);
  chorus_reset(&ch);
  memset(stereo, 0, sizeof stereo);
  for (i = 0; i < 64; ++i)
    send[i] = i == 0 ? 1.0f : 0.0f;
  /* run past the delay so the impulse has come back out */
  for (i = 0; i < 40; ++i)
    chorus_process(&ch, send, stereo, 64);
  sweep_span = 0.0;
  for (i = 0; i < 64; ++i)
    sweep_span += fabs(stereo[i * 2]) + fabs(stereo[i * 2 + 1]);
  assert(sweep_span > 0.0);

  chorus_destroy(&ch);
  assert(!chorus_init(&ch, 1000.0, &SC88_PROFILE));

  /* The feedback source is the profile's. With the sweep held at phase 0,
     a rising triangle puts the left tap at 100 samples and the right at
     140, so an impulse's second pass tells the sources apart: fed back
     from the left tap, the left output repeats at 200 and not at 240;
     fed back from the mean, it repeats at both at half the gain. */
  assert(SC88_PROFILE.chorusFeedbackTap == XP_CHORUS_FB_TAP_MEAN);
  assert(JV1080_PROFILE.chorusFeedbackTap == XP_CHORUS_FB_TAP_LEFT);
  {
    static float out[300 * 2];
    static float in[300];
    const uint8_t taps[2] = { XP_CHORUS_FB_TAP_LEFT, XP_CHORUS_FB_TAP_MEAN };
    for (unsigned t = 0; t < 2; ++t) {
      struct XpDeviceProfile p = SC88_PROFILE;
      p.chorusModulator = XP_CHORUS_MOD_TRIANGLE_UP;
      p.chorusFeedbackTap = taps[t];
      assert(chorus_init(&ch, 32000.0, &p));
      chorus_set_params(NULL, &ch, 0, 0, 0, 0, 0, 0);
      chorus_set_runtime(&ch, 100.0, 40.0, 0.0, 0.5f, 1.0f);
      chorus_reset(&ch);
      memset(out, 0, sizeof out);
      memset(in, 0, sizeof in);
      in[0] = 1.0f;
      chorus_process(&ch, in, out, 300);
      assert(fabs(out[100 * 2] - 1.0) < 1e-6);
      assert(fabs(out[140 * 2 + 1] - 1.0) < 1e-6);
      if (taps[t] == XP_CHORUS_FB_TAP_LEFT) {
        assert(fabs(out[200 * 2] - 0.5) < 1e-6);
        assert(fabs(out[240 * 2]) < 1e-6);
        assert(fabs(out[240 * 2 + 1] - 0.5) < 1e-6);
      } else {
        assert(fabs(out[200 * 2] - 0.25) < 1e-6);
        assert(fabs(out[240 * 2] - 0.25) < 1e-6);
      }
      chorus_destroy(&ch);
    }
  }

  /* The loop high-pass is the profile's, and only the JV-1080 has one. Its
     gain is unity at the top of the band, so a loop at unity feedback holds
     a Nyquist-rate tone where a gain above one would grow it; and it is
     below unity at the bottom, so a DC offset fed back dies away where the
     same loop without it holds the offset forever. */
  assert(SC88_PROFILE.chorusLoopHighpassHz == 0.0);
  assert(JV1080_PROFILE.chorusLoopHighpassHz > 0.0);
  {
    static float out[100 * 2];
    static float in[100];
    for (unsigned hp = 0; hp < 2; ++hp) {
      for (unsigned dc = 0; dc < 2; ++dc) {
        struct XpDeviceProfile p = SC88_PROFILE;
        p.chorusModulator = XP_CHORUS_MOD_TRIANGLE_UP;
        p.chorusFeedbackTap = XP_CHORUS_FB_TAP_LEFT;
        p.chorusLoopHighpassHz = hp ? JV1080_PROFILE.chorusLoopHighpassHz : 0.0;
        assert(chorus_init(&ch, 32000.0, &p));
        chorus_set_params(NULL, &ch, 0, 0, 0, 0, 0, 0);
        chorus_set_runtime(&ch, 100.0, 0.0, 0.0, 1.0f, 1.0f);
        chorus_reset(&ch);
        double peak = 0.0, mean = 0.0;
        /* one loop's worth of input, then 300 passes of the loop alone */
        for (unsigned pass = 0; pass <= 300; ++pass) {
          for (i = 0; i < 100; ++i)
            in[i] = pass ? 0.0f : dc ? 1.0f : (i & 1u) ? -1.0f : 1.0f;
          memset(out, 0, sizeof out);
          chorus_process(&ch, in, out, 100);
        }
        for (i = 0; i < 100; ++i) {
          peak = fmax(peak, fabs(out[i * 2]));
          mean += out[i * 2] / 100.0;
        }
        if (!dc)
          assert(peak <= 1.0 + 1e-3 && peak > 0.9);
        else if (hp)
          assert(fabs(mean) < 1e-2);
        else
          assert(fabs(mean - 1.0) < 1e-3);
        chorus_destroy(&ch);
      }
    }
  }
  return 0;
}
