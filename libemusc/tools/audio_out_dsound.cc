/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * This file consists entirely of AI-generated code without direct human
 * authorship and is dedicated to the public domain under CC0 1.0.
 *
 *  DirectSound stream for emusc-render's --play. See audio_out_dsound.h.
 *
 *  Device setup, the ring-buffer invariants and underrun/buffer-loss
 *  recovery follow emuscd/audio_out_dsound.cc; what differs is who drives
 *  it. There a polling loop tops the ring up through a fill callback, here
 *  the render loop hands over finished blocks and write() waits for room.
 */

#include "audio_out_dsound.h"

#include <windows.h>
#include <mmsystem.h>
#include <objbase.h>
#include <dsound.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace emusc_tools {

namespace {

typedef HRESULT (WINAPI *DirectSoundCreateFn)(LPCGUID, LPDIRECTSOUND *, LPUNKNOWN);
typedef HRESULT (WINAPI *CoInitializeExFn)(LPVOID, DWORD);
typedef HRESULT (WINAPI *CoInitializeFn)(LPVOID);
typedef void (WINAPI *CoUninitializeFn)(void);
typedef HWND (WINAPI *GetConsoleWindowFn)(void);
typedef HWND (WINAPI *GetDesktopWindowFn)(void);

template <typename Fn>
Fn load_proc(HMODULE mod, const char *name) {
  // Through a generic function pointer: a direct FARPROC-to-Fn cast trips
  // -Wcast-function-type for no real reason.
  return reinterpret_cast<Fn>(
    reinterpret_cast<void (*)(void)>(GetProcAddress(mod, name)));
}

std::string hr_text(const char *what, HRESULT hr) {
  char b[128];
  std::snprintf(b, sizeof b, "%s failed (0x%08lx)", what, (unsigned long) hr);
  return b;
}

const DWORD BYTES_PER_FRAME = 4;   // 16-bit stereo

// Minimum ring length. The amount queued is set by latencyMs, not by this:
// the ring only has to be long enough that a render stall is detected by
// wall-clock time before the play cursor could have lapped the whole ring
// and made its position deltas ambiguous.
const unsigned MIN_RING_MS = 500;

// How long GetStatus/GetCurrentPosition/Restore may keep failing inside
// write() before the output is treated as gone rather than as a transient
// buffer loss.
const DWORD MAX_FAILURE_MS = 3000;

// COM for the calling thread, balanced on destruction. A thread that already
// initialised COM in the other apartment model (RPC_E_CHANGED_MODE) is used
// as-is and left alone.
struct ComScope {
  HMODULE ole32 = nullptr;
  CoUninitializeFn uninit = nullptr;
  bool mustUninit = false;

  bool init(std::string *why) {
    ole32 = LoadLibraryA("ole32.dll");
    if (!ole32) { *why = "ole32.dll not found"; return false; }
    uninit = load_proc<CoUninitializeFn>(ole32, "CoUninitialize");
    auto initEx = load_proc<CoInitializeExFn>(ole32, "CoInitializeEx");
    auto initSta = load_proc<CoInitializeFn>(ole32, "CoInitialize");
    if (!uninit || (!initEx && !initSta)) {
      *why = "ole32.dll lacks CoInitialize/CoUninitialize";
      return false;
    }
    // DirectSound objects are free-threaded; the multithreaded apartment
    // needs no message pump on this thread. Systems without DCOM have only
    // CoInitialize (single-threaded apartment), which DirectSound also
    // accepts.
    HRESULT hr = initEx ? initEx(nullptr, COINIT_MULTITHREADED)
                        : initSta(nullptr);
    if (SUCCEEDED(hr)) {           // S_OK or S_FALSE both need balancing
      mustUninit = true;
      return true;
    }
    if (hr == RPC_E_CHANGED_MODE) return true;
    *why = hr_text("CoInitializeEx", hr);
    return false;
  }

  ~ComScope() {
    if (mustUninit) uninit();
    if (ole32) FreeLibrary(ole32);
  }
};

// Bytes from `from` forward to `to` around a ring of `size` bytes.
inline DWORD ring_dist(DWORD from, DWORD to, DWORD size) {
  return (to >= from) ? to - from : size - from + to;
}

}  // namespace

// Ring-buffer model. The secondary buffer loops forever; writePos is where
// the next written frame goes. Between write() calls:
//   - [play, writePos) holds audio queued but not yet played;
//   - every other byte of the ring is silence. Played audio is zeroed as the
//     play cursor passes it, so an underrun or stall plays silence instead of
//     looping stale audio.
// Before the buffer is first started, the play cursor sits at 0 and
// [0, writePos) is the queue.
struct DSoundStream::Impl {
  // ~Impl() releases the buffer and device; the members below are then
  // destroyed in reverse order, unloading dsound.dll before COM is
  // uninitialised.
  ComScope com;
  HMODULE dll = nullptr;
  HMODULE user32 = nullptr;
  IDirectSound *ds = nullptr;
  IDirectSoundBuffer *buf = nullptr;

  DWORD bufBytes = 0, blockBytes = 0, targetBytes = 0;
  DWORD writePos = 0, lastPlay = 0;
  DWORD lastPollTick = 0, stallMs = 0;
  bool started = false;
  // Some implementations hold the play cursor still when a buffer starts and
  // then advance it by one large jump. That first movement is resynchronised
  // like any underrun but not reported as one; `primed` turns true only once
  // the cursor has moved before.
  bool cursorMoved = false, primed = false;
  unsigned long underruns = 0;
  unsigned rate = 0;
  DWORD lastReportTick = 0;

  ~Impl() {
    if (buf) { buf->Stop(); buf->Release(); }
    if (ds) ds->Release();
    if (user32) FreeLibrary(user32);
    if (dll) FreeLibrary(dll);
  }

  bool lock(DWORD pos, DWORD bytes, void **p1, DWORD *n1, void **p2, DWORD *n2) {
    return SUCCEEDED(buf->Lock(pos, bytes, p1, n1, p2, n2, 0));
  }

  void zero(DWORD pos, DWORD bytes) {
    if (bytes == 0) return;
    void *p1, *p2; DWORD n1, n2;
    if (!lock(pos, bytes, &p1, &n1, &p2, &n2)) return;
    std::memset(p1, 0, n1);
    if (p2) std::memset(p2, 0, n2);
    buf->Unlock(p1, n1, p2, n2);
  }

  bool zero_all() {
    void *p1, *p2; DWORD n1, n2;
    HRESULT hr = buf->Lock(0, 0, &p1, &n1, &p2, &n2, DSBLOCK_ENTIREBUFFER);
    if (FAILED(hr)) return false;
    std::memset(p1, 0, n1);
    if (p2) std::memset(p2, 0, n2);
    buf->Unlock(p1, n1, p2, n2);
    return true;
  }

  // Copies `bytes` (a whole number of frames) to writePos and advances it.
  // Returns false, leaving writePos alone, if the ring could not be locked.
  bool copy_in(const int16_t *src, DWORD bytes) {
    void *p1, *p2; DWORD n1, n2;
    if (!lock(writePos, bytes, &p1, &n1, &p2, &n2)) return false;
    std::memcpy(p1, src, n1);
    if (p2) std::memcpy(p2, reinterpret_cast<const char *>(src) + n1, n2);
    buf->Unlock(p1, n1, p2, n2);
    writePos = (writePos + n1 + (p2 ? n2 : 0)) % bufBytes;
    return true;
  }

  bool start() {
    if (FAILED(buf->Play(0, 0, DSBPLAY_LOOPING))) return false;
    started = true;
    cursorMoved = primed = false;
    lastPollTick = GetTickCount();
    return true;
  }

  // After the buffer was lost (another application took the device with
  // DSSCL_WRITEPRIMARY): regain it and restart from silence at the write
  // cursor. Whatever was queued is gone.
  bool restore() {
    if (FAILED(buf->Restore())) return false;
    if (!zero_all()) return false;
    DWORD play, write;
    if (FAILED(buf->GetCurrentPosition(&play, &write))) return false;
    lastPlay = play - play % BYTES_PER_FRAME;
    writePos = write - write % BYTES_PER_FRAME;
    return start();
  }

  void note_underrun(DWORD now) {
    if (!primed) return;
    underruns++;
    if (now - lastReportTick >= 1000) {
      std::fprintf(stderr, "emusc-render: --play: DirectSound underrun (%lu so "
                   "far) - consider a larger --latency\n", underruns);
      lastReportTick = now;
    }
  }

  // Reads both cursors and brings the ring's invariants up to date with
  // them: zeroes what has played since the last poll, and resynchronises
  // writePos past the committed span if playback overtook the queue.
  // Returns false if the position could not be read, including right after
  // recovering a lost buffer; the caller retries.
  bool poll(DWORD *playOut, DWORD *writeOut) {
    DWORD status = 0;
    if (FAILED(buf->GetStatus(&status))) return false;
    if (status & DSBSTATUS_BUFFERLOST) { restore(); return false; }
    if (started && !(status & DSBSTATUS_PLAYING) && !start()) return false;

    DWORD play, write;
    if (FAILED(buf->GetCurrentPosition(&play, &write))) return false;
    // DirectSound keeps both cursors frame-aligned in practice; enforce it,
    // since everything below assumes whole frames.
    play -= play % BYTES_PER_FRAME;
    write -= write % BYTES_PER_FRAME;
    *playOut = play;
    *writeOut = write;
    if (!started) return true;

    DWORD now = GetTickCount();
    DWORD sincePoll = now - lastPollTick;
    lastPollTick = now;

    if (sincePoll > stallMs) {
      // The play cursor may have lapped the ring since the last poll, so its
      // delta says nothing. Silence everything outside the committed span
      // and restart just past it.
      zero(write, ring_dist(write, play, bufBytes));
      writePos = write;
      note_underrun(now);
    } else {
      DWORD played = ring_dist(lastPlay, play, bufBytes);
      DWORD queuedBefore = ring_dist(lastPlay, writePos, bufBytes);
      zero(lastPlay, played);
      bool overtaken = played >= queuedBefore;
      bool inCommitted = ring_dist(play, writePos, bufBytes) <
                         ring_dist(play, write, bufBytes);
      if (overtaken || inCommitted) {
        writePos = write;
        note_underrun(now);
      }
      if (cursorMoved) primed = true;
      if (played > 0) cursorMoved = true;
    }
    lastPlay = play;
    return true;
  }

  // Waits for the play cursor to pass writePos, i.e. for everything queued
  // to have played, then a little longer for the device's own output stage.
  void drain() {
    if (!started) {
      if (writePos == 0 || !start()) return;
    }
    DWORD queued = ring_dist(lastPlay, writePos, bufBytes);
    DWORD deadline = GetTickCount() + static_cast<DWORD>(
      static_cast<unsigned long long>(queued) * 1000 /
      (static_cast<unsigned long long>(rate) * BYTES_PER_FRAME)) + 1000;
    while (static_cast<LONG>(deadline - GetTickCount()) > 0) {
      DWORD play, write;
      if (FAILED(buf->GetCurrentPosition(&play, &write))) break;
      play -= play % BYTES_PER_FRAME;
      DWORD played = ring_dist(lastPlay, play, bufBytes);
      DWORD remaining = ring_dist(lastPlay, writePos, bufBytes);
      if (played >= remaining) break;
      zero(lastPlay, played);
      lastPlay = play;
      Sleep(1);
    }
    Sleep(50);   // silence from here on; lets the output stage empty
  }
};

std::unique_ptr<DSoundStream> DSoundStream::open(unsigned rate,
                                                 unsigned blockFrames,
                                                 unsigned latencyMs,
                                                 std::string *why) {
  std::unique_ptr<Impl> impl(new Impl);
  impl->rate = rate;

  if (!impl->com.init(why)) return nullptr;

  impl->dll = LoadLibraryA("dsound.dll");
  if (!impl->dll) { *why = "dsound.dll not found"; return nullptr; }
  auto create = load_proc<DirectSoundCreateFn>(impl->dll, "DirectSoundCreate");
  if (!create) { *why = "dsound.dll lacks DirectSoundCreate"; return nullptr; }

  HRESULT hr = create(nullptr, &impl->ds, nullptr);
  if (FAILED(hr)) {
    impl->ds = nullptr;
    *why = hr_text("DirectSoundCreate", hr);
    return nullptr;
  }

  // DSSCL_PRIORITY is what allows setting the primary buffer's format below,
  // so DirectSound's mixer runs at this stream's rate and depth instead of a
  // legacy default (22.05 kHz 8-bit on pre-WDM drivers) it would otherwise
  // resample down to. A console process has no window of its own to give;
  // GetConsoleWindow (absent before Windows 2000) or the desktop serve, and
  // DSBCAPS_GLOBALFOCUS below keeps the stream audible regardless of which
  // window has focus. Both are looked up, like dsound.dll itself, so the
  // executable's import table gains no USER32 entry.
  HWND hwnd = nullptr;
  if (HMODULE k32 = GetModuleHandleA("kernel32.dll"))
    if (auto gcw = load_proc<GetConsoleWindowFn>(k32, "GetConsoleWindow"))
      hwnd = gcw();
  if (!hwnd) {
    impl->user32 = LoadLibraryA("user32.dll");
    if (impl->user32)
      if (auto gdw = load_proc<GetDesktopWindowFn>(impl->user32, "GetDesktopWindow"))
        hwnd = gdw();
  }
  if (!hwnd) {
    *why = "no window handle for DirectSound's cooperative level";
    return nullptr;
  }
  hr = impl->ds->SetCooperativeLevel(hwnd, DSSCL_PRIORITY);
  if (FAILED(hr)) { *why = hr_text("SetCooperativeLevel", hr); return nullptr; }

  WAVEFORMATEX wfx{};
  wfx.wFormatTag = WAVE_FORMAT_PCM;
  wfx.nChannels = 2;
  wfx.nSamplesPerSec = rate;
  wfx.wBitsPerSample = 16;
  wfx.nBlockAlign = static_cast<WORD>(wfx.nChannels * wfx.wBitsPerSample / 8);
  wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

  // DSBUFFERDESC1 (the DirectX 3 layout) is accepted by every DirectSound
  // version, where the full DSBUFFERDESC is rejected by pre-DirectX 7 ones.
  DSBUFFERDESC1 desc{};
  desc.dwSize = sizeof(desc);
  desc.dwFlags = DSBCAPS_PRIMARYBUFFER;
  IDirectSoundBuffer *primary = nullptr;
  hr = impl->ds->CreateSoundBuffer(reinterpret_cast<LPCDSBUFFERDESC>(&desc),
                                   &primary, nullptr);
  if (SUCCEEDED(hr)) {
    HRESULT fr = primary->SetFormat(&wfx);
    primary->Release();
    if (FAILED(fr))
      std::fprintf(stderr, "emusc-render: --play: DirectSound primary "
                   "SetFormat failed (0x%08lx); the mixer will resample\n",
                   (unsigned long) fr);
  } else {
    std::fprintf(stderr, "emusc-render: --play: DirectSound primary buffer "
                 "unavailable (0x%08lx); the mixer will resample\n",
                 (unsigned long) hr);
  }

  DWORD blockBytes = blockFrames * BYTES_PER_FRAME;
  DWORD targetFrames = static_cast<DWORD>(
    (static_cast<unsigned long long>(latencyMs) * rate + 999) / 1000);
  DWORD targetBlocks = (targetFrames + blockFrames - 1) / blockFrames;
  if (targetBlocks < 2) targetBlocks = 2;
  unsigned long long minRingFrames =
    (static_cast<unsigned long long>(MIN_RING_MS) * rate + 999) / 1000;
  unsigned long long ringBlocks = (minRingFrames + blockFrames - 1) / blockFrames;
  if (ringBlocks < 4ull * targetBlocks) ringBlocks = 4ull * targetBlocks;
  unsigned long long ringBytes = ringBlocks * blockBytes;
  if (ringBytes > DSBSIZE_MAX) {
    *why = "--latency/--block too large for a DirectSound buffer";
    return nullptr;
  }

  desc = DSBUFFERDESC1{};
  desc.dwSize = sizeof(desc);
  desc.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS;
  desc.dwBufferBytes = static_cast<DWORD>(ringBytes);
  desc.lpwfxFormat = &wfx;
  hr = impl->ds->CreateSoundBuffer(reinterpret_cast<LPCDSBUFFERDESC>(&desc),
                                   &impl->buf, nullptr);
  if (FAILED(hr)) {
    impl->buf = nullptr;
    *why = hr_text("CreateSoundBuffer", hr);
    return nullptr;
  }

  DSBCAPS caps{};
  caps.dwSize = sizeof(caps);
  hr = impl->buf->GetCaps(&caps);
  if (FAILED(hr) || caps.dwBufferBytes < 4 * blockBytes ||
      caps.dwBufferBytes % BYTES_PER_FRAME != 0) {
    *why = "DirectSound buffer has an unusable size";
    return nullptr;
  }

  impl->bufBytes = caps.dwBufferBytes;
  impl->blockBytes = blockBytes;
  impl->targetBytes = targetBlocks * blockBytes;
  if (impl->targetBytes > impl->bufBytes / 2)
    impl->targetBytes = impl->bufBytes / 2 / blockBytes * blockBytes;
  DWORD ringMs = static_cast<DWORD>(
    static_cast<unsigned long long>(impl->bufBytes) * 1000 /
    (static_cast<unsigned long long>(rate) * BYTES_PER_FRAME));
  impl->stallMs = ringMs / 2;

  if (!impl->zero_all()) { *why = "DirectSound buffer Lock failed"; return nullptr; }

  std::fprintf(stderr, "emusc-render: --play: DirectSound output at %u Hz, "
               "%u-frame blocks, %u ms queued in a %u ms ring\n",
               rate, blockFrames,
               (unsigned) (static_cast<unsigned long long>(impl->targetBytes) *
                           1000 / (static_cast<unsigned long long>(rate) *
                                   BYTES_PER_FRAME)),
               (unsigned) ringMs);

  return std::unique_ptr<DSoundStream>(new DSoundStream(impl.release()));
}

DSoundStream::~DSoundStream() {
  _impl->drain();
  if (_impl->underruns)
    std::fprintf(stderr, "emusc-render: --play: %lu DirectSound underrun(s) "
                 "in total\n", _impl->underruns);
  delete _impl;
}

void DSoundStream::write(const int16_t *interleaved, size_t frames) {
  Impl &d = *_impl;
  // The queue never grows past this, so writePos cannot come back round to
  // the play cursor and make a full ring indistinguishable from an empty one.
  const DWORD maxQueued = d.bufBytes - d.blockBytes;
  size_t done = 0;
  DWORD failingSince = 0;
  bool failing = false;
  auto fail_and_retry = [&]() {
    DWORD now = GetTickCount();
    if (!failing) { failing = true; failingSince = now; }
    else if (now - failingSince > MAX_FAILURE_MS) {
      std::fprintf(stderr, "emusc-render: --play: DirectSound output "
                   "stopped responding\n");
      std::exit(4);
    }
    Sleep(1);
  };
  while (done < frames) {
    DWORD play = 0, write = 0;
    if (!d.poll(&play, &write)) { fail_and_retry(); continue; }

    DWORD queued = d.started ? ring_dist(play, d.writePos, d.bufBytes) : d.writePos;
    // The requested latency, or more when that would not cover the span the
    // driver has already committed plus the block being computed next.
    DWORD goal = d.targetBytes;
    if (d.started) {
      DWORD needed = ring_dist(play, write, d.bufBytes) + 2 * d.blockBytes;
      if (goal < needed) goal = needed;
    }
    if (goal > maxQueued) goal = maxQueued;

    if (queued >= goal) {
      if (!d.started) {
        if (!d.start()) {
          std::fprintf(stderr, "emusc-render: --play: DirectSound Play "
                       "failed\n");
          std::exit(4);
        }
        continue;
      }
      failing = false;
      Sleep(1);
      continue;
    }

    DWORD n = goal - queued;
    size_t left = (frames - done) * BYTES_PER_FRAME;
    if (n > left) n = static_cast<DWORD>(left);
    if (!d.copy_in(interleaved + done * 2, n)) { fail_and_retry(); continue; }
    failing = false;
    done += n / BYTES_PER_FRAME;
  }
}

}  // namespace emusc_tools
