// Windows 98 compatibility shims for KERNEL32 exports that MinGW's win32
// gthread runtime references but that don't exist before XP SP1 (GetThreadId)
// or Vista (the condition-variable API). This project's own code never calls
// either directly.
//
// The reference comes from libstdc++'s own precompiled thread-safe-statics
// guard runtime (libstdc++.a's guard.o, the __cxa_guard_acquire/release
// implementation behind every function-local static with a non-trivial
// constructor). guard.o unconditionally references both
// __gthr_win32_once (gthr-win32.o) and __gthr_win32_cond_init_function
// (gthr-win32-cond.o), and since neither of libgcc's gthr-win32*.o objects
// has per-function sections, needing any one symbol from either links in the
// whole object - equal() and the four condition-variable functions included.
// Confirmed with an isolated repro: a program that only throws and catches a
// std::runtime_error, with no iostream, fstream, sstream, mutex, thread, or
// function-local static of its own anywhere in it, still imports
// GetThreadId; removing the three magic statics this project's own code did
// have (debug-flag caches in synth.cc/tvf.cc, gated behind getenv() calls)
// made no difference, confirming the reference lives inside libstdc++ itself
// rather than in anything this project controls. The executable's import
// table ends up requiring exports the target Windows version never shipped,
// and the loader refuses to run the program at all, before main() ever gets
// a chance to.
//
// This project's own code no longer uses <iostream>, <fstream>, <sstream>,
// <mutex> or <thread> anywhere (Windows 98 support was the reason), so this
// guard-runtime reference - present in some form in essentially any
// non-trivial C++ program - is the only remaining source of this dependency.
//
// Each symbol is satisfied here instead of asking the OS for it, scoped to
// i686 only: 64-bit Windows implies XP x64 or later, which already ships
// all of these, and the x86_64 ABI has no stdcall decoration to replicate
// (see the asm-named symbols below).
#if defined(_WIN32) && !defined(_WIN64)

#include <windows.h>

namespace {

// ---- GetThreadId (gthr-win32.o, __gthr_win32_equal) ------------------------
//
// GetThreadId doesn't exist before Windows XP SP1, and has no equivalent at
// all on Windows 9x/ME's non-NT kernel (no ntdll, no NtQueryInformationThread
// to fall back to). The only caller in the linked-in object resolves both of
// its handle arguments to numeric IDs before comparing them for equality.
//
// Every call this project's own code can actually reach queries the calling
// thread's own identity (emusc-render never creates a second thread at all;
// emuscd/emusc-winmidi's own locking uses CRITICAL_SECTION directly, never
// gthread's, and its one real thread is created with raw CreateThread, not
// std::thread), so always returning the calling thread's ID is exact for
// every call this project makes - a fully general GetThreadId, resolving an
// arbitrary handle to some other thread's ID, has no possible implementation
// on Windows 9x/ME at all, so this doesn't attempt one.
DWORD WINAPI compat_GetThreadId(HANDLE) {
  return GetCurrentThreadId();
}

// ---- Condition variables (gthr-win32-cond.o) -------------------------------
//
// InitializeConditionVariable/SleepConditionVariableCS/WakeConditionVariable/
// WakeAllConditionVariable are the native Vista+ condition-variable API,
// which libstdc++'s guard implementation (the blocking path taken when a
// second thread reaches a function-local static while a first thread is
// already running its initializer - the correctness case -fthreadsafe-statics
// exists for) calls directly. This project's own code never creates a
// std::condition_variable, and confirmed by an isolated repro, this path is
// reachable purely as a byproduct of exception-handling support being linked
// in - not because anything here actually waits or wakes at runtime. It is
// nonetheless emulated with real condition-variable semantics rather than
// approximated, in case a future change ever does exercise it for real: the
// classic semaphore + manual-reset-event construction (Schmidt & Pyarali,
// "Strategies for Implementing POSIX Condition Variables on Win32"), built
// entirely from primitives that have existed since Windows 95
// (CRITICAL_SECTION, CreateSemaphoreA, CreateEventA, WaitForSingleObject).
//
// A CONDITION_VARIABLE is a single pointer-sized field (see winnt.h) that
// real Windows also treats as an opaque, lazily-initialized slot - a
// zero/NULL CONDITION_VARIABLE is documented as valid and equivalent to a
// freshly-initialized one. This stores a pointer to a heap-allocated
// CondState there instead, installed with a lock-free compare-and-swap
// (a GCC/mingw builtin lowering to a plain `lock cmpxchg`, needing no KERNEL32
// import of its own) so concurrent lazy-initializers can't race.
struct CondState {
  LONG waiters;
  CRITICAL_SECTION waiters_lock;
  HANDLE sema;         // released once per thread that should wake up
  HANDLE waiters_done; // signaled by the last waiter a broadcast released
  BOOL was_broadcast;
};

CondState *cond_state_for(PCONDITION_VARIABLE cv) {
  void *volatile *slot = reinterpret_cast<void *volatile *>(cv);
  void *existing = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
  if (existing) return static_cast<CondState *>(existing);

  CondState *fresh = new CondState();
  fresh->waiters = 0;
  InitializeCriticalSection(&fresh->waiters_lock);
  fresh->sema = CreateSemaphoreA(nullptr, 0, 0x7fffffff, nullptr);
  fresh->waiters_done = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  fresh->was_broadcast = FALSE;

  void *none = nullptr;
  if (__atomic_compare_exchange_n(slot, &none, fresh, FALSE, __ATOMIC_ACQ_REL,
                                   __ATOMIC_ACQUIRE)) {
    return fresh;
  }
  // Another thread won the race to initialize this CV; drop our copy and
  // use theirs (left in `none` by the failed compare-exchange).
  DeleteCriticalSection(&fresh->waiters_lock);
  CloseHandle(fresh->sema);
  CloseHandle(fresh->waiters_done);
  delete fresh;
  return static_cast<CondState *>(none);
}

VOID WINAPI compat_InitializeConditionVariable(PCONDITION_VARIABLE cv) {
  *reinterpret_cast<void **>(cv) = nullptr;
}

BOOL WINAPI compat_SleepConditionVariableCS(PCONDITION_VARIABLE cv,
                                             PCRITICAL_SECTION cs,
                                             DWORD timeout_ms) {
  CondState *s = cond_state_for(cv);

  EnterCriticalSection(&s->waiters_lock);
  s->waiters++;
  LeaveCriticalSection(&s->waiters_lock);

  // SleepConditionVariableCS's documented contract: atomically release the
  // caller's critical section while waiting, and always reacquire it before
  // returning, timeout or not.
  LeaveCriticalSection(cs);
  DWORD wait_result = WaitForSingleObject(s->sema, timeout_ms);

  EnterCriticalSection(&s->waiters_lock);
  s->waiters--;
  BOOL last_waiter = s->was_broadcast && s->waiters == 0;
  LeaveCriticalSection(&s->waiters_lock);

  if (last_waiter) SetEvent(s->waiters_done);

  EnterCriticalSection(cs);

  if (wait_result == WAIT_TIMEOUT) {
    SetLastError(ERROR_TIMEOUT);
    return FALSE;
  }
  return TRUE;
}

VOID WINAPI compat_WakeConditionVariable(PCONDITION_VARIABLE cv) {
  CondState *s = cond_state_for(cv);
  EnterCriticalSection(&s->waiters_lock);
  BOOL have_waiters = s->waiters > 0;
  LeaveCriticalSection(&s->waiters_lock);
  if (have_waiters) ReleaseSemaphore(s->sema, 1, nullptr);
}

VOID WINAPI compat_WakeAllConditionVariable(PCONDITION_VARIABLE cv) {
  CondState *s = cond_state_for(cv);
  EnterCriticalSection(&s->waiters_lock);
  LONG count = s->waiters;
  BOOL have_waiters = count > 0;
  if (have_waiters) s->was_broadcast = TRUE;
  LeaveCriticalSection(&s->waiters_lock);

  if (have_waiters) {
    ReleaseSemaphore(s->sema, count, nullptr);
    // Block until the last released waiter has actually woken and decremented
    // `waiters`, so a WakeAllConditionVariable that races with a fresh
    // SleepConditionVariableCS call can't consume a semaphore count that
    // belongs to this broadcast rather than the new wait.
    WaitForSingleObject(s->waiters_done, INFINITE);
    s->was_broadcast = FALSE;
  }
}

}  // namespace

// Names the exact symbols MinGW's import stubs reference for a stdcall
// KERNEL32 import (__imp_ prefix, @N suffix for N bytes of arguments),
// satisfying each from this translation unit instead of a load-time import.
extern "C" {

typedef DWORD(WINAPI *GetThreadIdFn)(HANDLE);
__attribute__((used))
GetThreadIdFn emusc_GetThreadId_import_slot __asm__("__imp__GetThreadId@4") =
    compat_GetThreadId;

typedef VOID(WINAPI *InitializeConditionVariableFn)(PCONDITION_VARIABLE);
__attribute__((used))
InitializeConditionVariableFn emusc_InitializeConditionVariable_import_slot
    __asm__("__imp__InitializeConditionVariable@4") =
        compat_InitializeConditionVariable;

typedef BOOL(WINAPI *SleepConditionVariableCSFn)(PCONDITION_VARIABLE,
                                                  PCRITICAL_SECTION, DWORD);
__attribute__((used))
SleepConditionVariableCSFn emusc_SleepConditionVariableCS_import_slot
    __asm__("__imp__SleepConditionVariableCS@12") =
        compat_SleepConditionVariableCS;

typedef VOID(WINAPI *WakeConditionVariableFn)(PCONDITION_VARIABLE);
__attribute__((used))
WakeConditionVariableFn emusc_WakeConditionVariable_import_slot
    __asm__("__imp__WakeConditionVariable@4") = compat_WakeConditionVariable;

typedef VOID(WINAPI *WakeAllConditionVariableFn)(PCONDITION_VARIABLE);
__attribute__((used))
WakeAllConditionVariableFn emusc_WakeAllConditionVariable_import_slot
    __asm__("__imp__WakeAllConditionVariable@4") =
        compat_WakeAllConditionVariable;

}  // extern "C"

#endif  // defined(_WIN32) && !defined(_WIN64)
