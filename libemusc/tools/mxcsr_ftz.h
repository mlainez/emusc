// Sets SSE flush-to-zero (FTZ) at process start, for every host tool that
// renders audio (emuscd, emusc-render, emusc-winmidi). XP's delay effect
// (engines/xp/delay.cc) has a float feedback line and a one-pole pre-filter
// that decay geometrically and never reach exact zero, so quiet or decaying
// material spends a measurable fraction of frames with a denormal operand -
// on SC-88 Y4002_03.MID @44100, 4.0% (293,778/7,421,900). x86 FPUs handle a
// denormal operand through a slow microcode-assist path regardless of
// whether the result is affected (denormal * 0 is exactly 0 either way), so
// this is a real, pre-existing performance cost, not a correctness one. FTZ
// flushes the RESULT of an operation that underflows to a denormal, which
// breaks the loop: once the feedback line's own output is a hard zero
// instead of an ever-shrinking denormal, nothing downstream sees one either.
//
// FTZ only, deliberately not DAZ (denormals-are-zero, which additionally
// flushes denormal INPUTS): DAZ is SSE2-and-later, and an ldmxcsr with it
// set raises #GP on a Pentium III or Athlon XP - exactly the hardware this
// matters for. FTZ alone is SSE1-era safe.
//
// Process-level CPU state, so it lives in each host tool's own startup, not
// in the library. set_flush_denormals_to_zero() only touches the FTZ bit -
// _MM_SET_FLUSH_ZERO_MODE is a read-modify-write, so rounding mode and the
// exception masks are left exactly as the runtime set them.

#ifndef EMUSC_TOOLS_MXCSR_FTZ_H
#define EMUSC_TOOLS_MXCSR_FTZ_H

#if defined(__SSE__)
#include <xmmintrin.h>

inline void set_flush_denormals_to_zero(void)
{
  _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
}
#else
inline void set_flush_denormals_to_zero(void) {}
#endif

#endif
