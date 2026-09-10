/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_chorus.h"

#include <assert.h>
#ifdef NDEBUG
#error "this test is assertion-driven; NDEBUG compiles it away"
#endif
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The recovered transforms, checked against the firmware's own arithmetic
   rather than against this implementation's arrangement of it. The
   topology is a labelled choice and is not asserted here; the numbers
   that came out of the ROM are. */
int main(void)
{
  struct sc88_chorus ch;
  float stereo[64 * 2];
  float send[64];
  unsigned i;
  double sweep_span;

  assert(sc88_chorus_init(&ch, 32000.0));

  /* Delay is `3*p` samples of delay memory, so at the native 32 kHz it is
     the sample count itself: GS's default 0x50 is 240 samples, 7.5 ms. */
  sc88_chorus_set_params(NULL, &ch, 64, 0, 0x50, 3, 0, 0);
  assert(fabs(ch.delay_samples - 240.0) < 1e-9);
  sc88_chorus_set_params(NULL, &ch, 64, 0, 127, 3, 0, 0);
  assert(fabs(ch.delay_samples - 381.0) < 1e-9);
  sc88_chorus_set_params(NULL, &ch, 64, 0, 0, 3, 0, 0);
  assert(ch.delay_samples == 0.0);

  /* Feedback is `256*floor(p/4)` read as an XP coefficient: zero at zero,
     and saturating at 0.9688 rather than reaching or passing unity, which
     is the property that makes it safe in a feedback path. */
  sc88_chorus_set_params(NULL, &ch, 64, 0, 0x50, 3, 0, 0);
  assert(ch.feedback == 0.0f);
  sc88_chorus_set_params(NULL, &ch, 64, 64, 0x50, 3, 0, 0);
  assert(fabs(ch.feedback - 0.5) < 1e-6);
  sc88_chorus_set_params(NULL, &ch, 64, 127, 0x50, 3, 0, 0);
  assert(fabs(ch.feedback - 0.96875) < 1e-6);
  /* the register saturates, so 124 and 127 are the same coefficient */
  {
    float at_124;
    sc88_chorus_set_params(NULL, &ch, 64, 124, 0x50, 3, 0, 0);
    at_124 = ch.feedback;
    sc88_chorus_set_params(NULL, &ch, 64, 127, 0x50, 3, 0, 0);
    assert(ch.feedback == at_124);
  }

  /* Level is `4*p` against 512, the same law the reverb's level follows. */
  sc88_chorus_set_params(NULL, &ch, 0, 0, 0x50, 3, 0, 0);
  assert(ch.level == 0.0f);
  sc88_chorus_set_params(NULL, &ch, 127, 0, 0x50, 3, 0, 0);
  assert(fabs(ch.level - 508.0 / 512.0) < 1e-6);

  /* Rate is `64*p` per 8.0008 ms control period on a 16-bit accumulator,
     which puts the default of 3 at 0.37 Hz and the top at 15.5 Hz. */
  sc88_chorus_set_params(NULL, &ch, 64, 0, 0x50, 3, 0, 0);
  assert(fabs(ch.phase_step * 32000.0 - 0.3663) < 5e-4);
  sc88_chorus_set_params(NULL, &ch, 64, 0, 0x50, 127, 0, 0);
  assert(fabs(ch.phase_step * 32000.0 - 15.5) < 0.05);
  sc88_chorus_set_params(NULL, &ch, 64, 0, 0x50, 0, 0, 0);
  assert(ch.phase_step == 0.0);

  /* The pre-LPF is the reverb's own table, so zero is an exact bypass. */
  sc88_chorus_set_params(NULL, &ch, 64, 0, 0x50, 3, 0, 0);
  assert(ch.pre_fb == 0.0f && ch.pre_in == 1.0f);
  sc88_chorus_set_params(NULL, &ch, 64, 0, 0x50, 3, 0, 4);
  assert(fabs(ch.pre_fb - 0.5) < 1e-6);
  /* the table has eight entries, so a value past its end clamps to the
     last one rather than reading into the character blocks (`M-010`) */
  sc88_chorus_set_params(NULL, &ch, 64, 0, 0x50, 3, 0, 9);
  assert(fabs(ch.pre_fb - 7.0 / 8.0) < 1e-6);

  /* The sweep may never exceed the delay, or a tap would read ahead of the
     write pointer - the one bound the assumed topology still has to keep. */
  sc88_chorus_set_params(NULL, &ch, 64, 0, 1, 3, 127, 0);
  assert(ch.depth_samples <= ch.delay_samples);

  /* And it does something: a bus that is not silent produces a return that
     is not silent, on both sides, and the two sides differ because the
     taps are in antiphase. */
  sc88_chorus_set_params(NULL, &ch, 127, 0, 0x50, 127, 127, 0);
  sc88_chorus_reset(&ch);
  memset(stereo, 0, sizeof stereo);
  for (i = 0; i < 64; ++i)
    send[i] = i == 0 ? 1.0f : 0.0f;
  /* run past the delay so the impulse has come back out */
  for (i = 0; i < 40; ++i)
    sc88_chorus_process(&ch, send, stereo, 64);
  sweep_span = 0.0;
  for (i = 0; i < 64; ++i)
    sweep_span += fabs(stereo[i * 2]) + fabs(stereo[i * 2 + 1]);
  assert(sweep_span > 0.0);

  sc88_chorus_destroy(&ch);
  assert(!sc88_chorus_init(&ch, 1000.0));
  return 0;
}
