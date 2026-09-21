// GetThreadId doesn't exist before Windows XP SP1, and has no equivalent at
// all on Windows 9x/ME's non-NT kernel (no ntdll, no NtQueryInformationThread
// to fall back to). MinGW's win32-gthread runtime references it anyway, from
// a single call site (__gthr_win32_equal, which resolves both of its handle
// arguments to numeric IDs before comparing them) inside gthr-win32.o, an
// archive member that bundles mutex/once/TLS/yield/equal into one
// compilation unit. Linking against any single one of those - which
// libstdc++ does unconditionally for <iostream>'s own thread-safe static-init
// guard, even in a program that never spawns a second thread - pulls in the
// whole object file, equal() included, and the executable's import table
// ends up requiring a KERNEL32 export the target Windows version never
// shipped. The loader refuses to run the program at all, before main() ever
// gets a chance to.
//
// This satisfies that import ourselves instead of asking the OS for it.
// Every call this project's own code can actually reach queries the calling
// thread's own identity (emusc-render never creates a second thread at all;
// emuscd/emusc-winmidi's own locking uses CRITICAL_SECTION directly, never
// gthread's), so always returning the calling thread's ID is exact for every
// call this project makes - a fully general GetThreadId, resolving an
// arbitrary handle to some other thread's ID, has no possible implementation
// on Windows 9x/ME at all, so this doesn't attempt one.
//
// 64-bit Windows implies XP x64 or later, which already ships GetThreadId,
// and the x86_64 calling convention has no stdcall decoration to replicate
// (see the asm-named symbol below), so this only does anything on the 32-bit
// target that actually claims Windows 98 support.
#if defined(_WIN32) && !defined(_WIN64)

#include <windows.h>

namespace {

DWORD WINAPI compat_GetThreadId(HANDLE) {
  return GetCurrentThreadId();
}

}  // namespace

extern "C" {
typedef DWORD(WINAPI *GetThreadIdFn)(HANDLE);
// Names the exact symbol MinGW's import stubs reference for a stdcall
// KERNEL32 import (__imp_ prefix, @4 suffix for one 4-byte argument),
// satisfying it from this translation unit instead of a load-time import.
__attribute__((used))
GetThreadIdFn emusc_GetThreadId_import_slot __asm__("__imp__GetThreadId@4") =
    compat_GetThreadId;
}

#endif  // defined(_WIN32) && !defined(_WIN64)
