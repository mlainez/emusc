// A minimal std::mutex substitute for the 32-bit Windows build.
//
// std::mutex's constructor calls into libstdc++'s gthread mutex-init
// function, which - like <iostream>, <fstream>, <sstream> and <thread> -
// links against native condition-variable and thread-identity APIs that
// don't exist before Windows Vista and XP SP1 respectively. CRITICAL_SECTION
// has existed since Windows 95 and needs none of that.
//
// std::scoped_lock/std::lock_guard are templates that only need their
// argument to provide lock()/unlock(), so they work unmodified with this
// type - only the mutex's own declared type changes at each use.
//
// 64-bit Windows implies XP x64 or later, which already ships everything
// std::mutex needs, so this only replaces anything on the 32-bit target that
// actually claims Windows 98 support; everywhere else (including Linux)
// keeps using std::mutex itself.
#pragma once

#if defined(_WIN32) && !defined(_WIN64)

#include <windows.h>

// windows.h's windef.h defines the legacy 16-bit-segmented-memory-model
// macros "far" and "near" as empty tokens. This header is included from
// synth.h/part.h, both widely included across the engine, and the SC-88 path
// (engines/xp/reverb.h) declares fields literally named "far" - left defined,
// this macro silently deletes that identifier and breaks the parse.
#undef far
#undef near

namespace EmuSC {

class SimpleMutex {
public:
  SimpleMutex() { InitializeCriticalSection(&_cs); }
  ~SimpleMutex() { DeleteCriticalSection(&_cs); }

  SimpleMutex(const SimpleMutex &) = delete;
  SimpleMutex &operator=(const SimpleMutex &) = delete;

  void lock() { EnterCriticalSection(&_cs); }
  void unlock() { LeaveCriticalSection(&_cs); }

private:
  CRITICAL_SECTION _cs;
};

}  // namespace EmuSC

#else

#include <mutex>

namespace EmuSC {
using SimpleMutex = std::mutex;
}  // namespace EmuSC

#endif  // defined(_WIN32) && !defined(_WIN64)
