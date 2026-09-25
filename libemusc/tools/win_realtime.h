/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * This file consists entirely of AI-generated code without direct human
 * authorship and is dedicated to the public domain under CC0 1.0.
 *
 *  Windows scheduling settings shared by the host tools that feed a WinMM
 *  waveOut queue by polling it with Sleep(1) (emusc-winmidi, emusc-render's
 *  --play).
 */

#ifndef EMUSC_TOOLS_WIN_REALTIME_H
#define EMUSC_TOOLS_WIN_REALTIME_H

#include <windows.h>
#include <mmsystem.h>

namespace emusc_tools {

// Holds a timeBeginPeriod() request for its lifetime, so Sleep(1) is not
// rounded up to the default system timer tick (~10-15.6 ms), which can
// exceed a whole low-latency wave-buffer queue. timeEndPeriod() must only
// balance a request that succeeded.
class SystemTimerResolution {
public:
  explicit SystemTimerResolution(UINT ms)
      : _ms(ms), _active(timeBeginPeriod(ms) == TIMERR_NOERROR) {}
  ~SystemTimerResolution() { if (_active) timeEndPeriod(_ms); }
  SystemTimerResolution(const SystemTimerResolution &) = delete;
  SystemTimerResolution &operator=(const SystemTimerResolution &) = delete;

private:
  UINT _ms;
  bool _active;
};

// Call from the thread that refills the wave-out queue. Process-wide
// REALTIME_PRIORITY_CLASS is deliberately avoided: on the single-core
// target it would starve the OS and the game this runs alongside.
inline void raise_audio_thread_priority() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

}  // namespace emusc_tools

#endif
