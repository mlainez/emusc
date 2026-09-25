/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * This file consists entirely of AI-generated code without direct human
 * authorship and is dedicated to the public domain under CC0 1.0.
 *
 *  DirectSound output for emusc-winmidi. See audio_out_dsound.h.
 *
 *  DirectSound rather than XAudio2: XAudio2 only entered the DirectX SDK in
 *  2008 and needs either Vista's own copy or the separately installed DirectX
 *  End-User Runtime on XP, while DirectSound ships with every DirectX release
 *  since 1996 and is therefore present on every system the 64-bit build can
 *  run on at all (XP x64 and later), and on every Windows 98 install too.
 */

#include "audio_out_dsound.h"

#include <windows.h>
#include <mmsystem.h>
#include <objbase.h>
#include <dsound.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace emuscd {

namespace {

typedef HRESULT (WINAPI *DirectSoundCreateFn)(LPCGUID, LPDIRECTSOUND *, LPUNKNOWN);
typedef HRESULT (WINAPI *DirectSoundEnumerateAFn)(LPDSENUMCALLBACKA, LPVOID);
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

const DWORD BYTES_PER_FRAME = 4;   // 16-bit stereo

// Minimum ring length. The amount of audio actually queued is set by
// --latency, not by this: the ring only has to be long enough that a main
// loop stall (a device switch loading ROMs, say) is detected by wall-clock
// time before the play cursor could have lapped the whole ring and made its
// position deltas ambiguous.
const unsigned MIN_RING_MS = 500;

struct EnumEntry {
  bool hasGuid;
  GUID guid;
  std::string desc;
};

BOOL CALLBACK enum_callback(LPGUID guid, LPCSTR desc, LPCSTR, LPVOID ctx) {
  auto *out = static_cast<std::vector<EnumEntry> *>(ctx);
  EnumEntry e{};
  e.hasGuid = guid != nullptr;
  if (guid) e.guid = *guid;
  e.desc = desc ? desc : "";
  out->push_back(e);
  return TRUE;
}

// Owns dsound.dll for the duration of one enumeration or one DSoundOut.
struct DSoundLib {
  HMODULE dll = nullptr;
  DirectSoundCreateFn create = nullptr;
  DirectSoundEnumerateAFn enumerate = nullptr;

  bool load() {
    dll = LoadLibraryA("dsound.dll");
    if (!dll) {
      std::fprintf(stderr, "emusc-winmidi: dsound.dll not found - DirectSound "
                   "is not installed; use --audio-api winmm\n");
      return false;
    }
    create = load_proc<DirectSoundCreateFn>(dll, "DirectSoundCreate");
    enumerate = load_proc<DirectSoundEnumerateAFn>(dll, "DirectSoundEnumerateA");
    if (!create || !enumerate) {
      std::fprintf(stderr, "emusc-winmidi: dsound.dll lacks DirectSoundCreate/"
                   "DirectSoundEnumerateA\n");
      return false;
    }
    return true;
  }

  bool list(std::vector<EnumEntry> &out) const {
    HRESULT hr = enumerate(&enum_callback, &out);
    if (FAILED(hr)) {
      std::fprintf(stderr, "emusc-winmidi: DirectSoundEnumerate failed "
                   "(0x%08lx)\n", (unsigned long) hr);
      return false;
    }
    return true;
  }

  ~DSoundLib() { if (dll) FreeLibrary(dll); }
};

// COM for the calling thread, balanced on destruction. A thread that already
// initialised COM in the other apartment model (RPC_E_CHANGED_MODE) is used
// as-is and left alone.
struct ComScope {
  HMODULE ole32 = nullptr;
  CoUninitializeFn uninit = nullptr;
  bool mustUninit = false;

  bool init() {
    ole32 = LoadLibraryA("ole32.dll");
    if (!ole32) {
      std::fprintf(stderr, "emusc-winmidi: ole32.dll not found\n");
      return false;
    }
    uninit = load_proc<CoUninitializeFn>(ole32, "CoUninitialize");
    auto initEx = load_proc<CoInitializeExFn>(ole32, "CoInitializeEx");
    auto initSta = load_proc<CoInitializeFn>(ole32, "CoInitialize");
    if (!uninit || (!initEx && !initSta)) {
      std::fprintf(stderr, "emusc-winmidi: ole32.dll lacks CoInitialize/"
                   "CoUninitialize\n");
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
    std::fprintf(stderr, "emusc-winmidi: CoInitializeEx failed (0x%08lx)\n",
                 (unsigned long) hr);
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
// the next rendered block goes. Two invariants hold between pump() calls:
//   - [play, writePos) holds audio rendered but not yet played, and is kept
//     at least as long as [play, write) - the span DirectSound has already
//     committed and that must not be written - plus one block;
//   - every other byte of the ring is silence. Played audio is zeroed as the
//     play cursor passes it, so an underrun or stall plays silence instead of
//     looping stale audio.
struct DSoundOut::Impl {
  // ~Impl() releases the buffer and device; the members below are then
  // destroyed in reverse order, unloading dsound.dll before COM is
  // uninitialised.
  ComScope com;
  DSoundLib lib;
  HMODULE user32 = nullptr;
  IDirectSound *ds = nullptr;
  IDirectSoundBuffer *buf = nullptr;

  FillFn fill = nullptr;
  void *ctx = nullptr;

  DWORD bufBytes = 0, blockBytes = 0, targetBytes = 0;
  DWORD writePos = 0, lastPlay = 0;
  DWORD lastPollTick = 0, stallMs = 0;
  DWORD peakPollMs = 0, peakPollTick = 0;
  // Some implementations hold the play cursor still when a buffer starts and
  // then advance it by one large jump, overtaking the silent prefill. That
  // first movement is resynchronised like any underrun but not reported as
  // one, since nothing audible was lost; `primed` turns true only once the
  // cursor has moved before.
  bool cursorMoved = false, primed = false;
  unsigned long underruns = 0;
  unsigned rate = 0;
  DWORD lastReportTick = 0;

  ~Impl() {
    if (buf) { buf->Stop(); buf->Release(); }
    if (ds) ds->Release();
    if (user32) FreeLibrary(user32);
  }

  // Lock [pos, pos+bytes) of the ring, which DirectSound hands back as up to
  // two spans when it wraps.
  bool lock(DWORD pos, DWORD bytes, void **p1, DWORD *n1, void **p2, DWORD *n2) {
    HRESULT hr = buf->Lock(pos, bytes, p1, n1, p2, n2, 0);
    return SUCCEEDED(hr);
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

  // Renders `bytes` (a whole number of frames) at writePos and advances it.
  void render(DWORD bytes) {
    if (bytes == 0) return;
    void *p1, *p2; DWORD n1, n2;
    if (!lock(writePos, bytes, &p1, &n1, &p2, &n2)) return;
    fill(ctx, static_cast<int16_t *>(p1), n1 / BYTES_PER_FRAME);
    if (p2) fill(ctx, static_cast<int16_t *>(p2), n2 / BYTES_PER_FRAME);
    buf->Unlock(p1, n1, p2, n2);
    writePos = (writePos + n1 + (p2 ? n2 : 0)) % bufBytes;
  }

  // After the buffer was lost (another application took the device with
  // DSSCL_WRITEPRIMARY): regain it, restart from silence at the write cursor.
  bool restore() {
    if (FAILED(buf->Restore())) return false;   // still lost; retry next pump
    if (!zero_all()) return false;
    DWORD play, write;
    if (FAILED(buf->GetCurrentPosition(&play, &write))) return false;
    lastPlay = play;
    writePos = write;
    lastPollTick = GetTickCount();
    cursorMoved = primed = false;
    return SUCCEEDED(buf->Play(0, 0, DSBPLAY_LOOPING));
  }

  void note_underrun(DWORD now) {
    if (!primed) return;
    underruns++;
    if (now - lastReportTick >= 1000) {
      std::fprintf(stderr, "emusc-winmidi: DirectSound underrun (%lu so far) "
                   "- consider a larger --latency\n", underruns);
      lastReportTick = now;
    }
  }

  void pump() {
    DWORD status = 0;
    if (FAILED(buf->GetStatus(&status))) return;
    if (status & DSBSTATUS_BUFFERLOST) { restore(); return; }
    if (!(status & DSBSTATUS_PLAYING)) {
      if (FAILED(buf->Play(0, 0, DSBPLAY_LOOPING))) return;
      cursorMoved = primed = false;
    }

    DWORD play, write;
    if (FAILED(buf->GetCurrentPosition(&play, &write))) return;
    // DirectSound keeps both cursors frame-aligned in practice; enforce it,
    // since everything below assumes whole frames.
    play -= play % BYTES_PER_FRAME;
    write -= write % BYTES_PER_FRAME;

    DWORD now = GetTickCount();
    DWORD sincePoll = now - lastPollTick;
    lastPollTick = now;

    bool lost = true;
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
      } else {
        lost = false;
      }
      if (cursorMoved) primed = true;
      if (played > 0) cursorMoved = true;
    }
    lastPlay = play;

    // The longest poll interval of roughly the last two seconds. Intervals
    // that ended in an underrun are stalls, handled by resynchronising rather
    // than planned for, so they are left out.
    if (!lost && (sincePoll >= peakPollMs || now - peakPollTick > 2000)) {
      peakPollMs = sincePoll;
      peakPollTick = now;
    }

    // Keep `goal` bytes queued ahead of the play cursor: the requested
    // latency, or more when it would not last until the next poll - the
    // span the driver has already committed, plus the recent peak poll
    // interval (Sleep(1) can take a full 10-16 ms scheduler tick on systems
    // without a raised timer resolution, and the play cursor itself may move
    // in steps that coarse), plus one block.
    DWORD committed = ring_dist(play, write, bufBytes);
    DWORD pollFrames = static_cast<DWORD>(
      (static_cast<unsigned long long>(peakPollMs) * rate + 999) / 1000);
    DWORD goal = targetBytes;
    DWORD needed = committed + pollFrames * BYTES_PER_FRAME + blockBytes;
    if (goal < needed) goal = needed;
    DWORD queued = ring_dist(play, writePos, bufBytes);
    if (queued >= goal || queued + blockBytes >= bufBytes) return;

    DWORD n = (goal - queued + blockBytes - 1) / blockBytes * blockBytes;
    // Never let writePos come back round to the play cursor: that would make
    // a full ring indistinguishable from an empty one.
    DWORD room = bufBytes - blockBytes - queued;
    if (n > room) n = room / blockBytes * blockBytes;
    render(n);
  }
};

bool DSoundOut::list_devices() {
  DSoundLib lib;
  if (!lib.load()) return false;
  std::vector<EnumEntry> entries;
  if (!lib.list(entries)) return false;
  if (entries.empty()) { std::printf("(no DirectSound output devices found)\n"); return true; }
  for (size_t i = 0; i < entries.size(); i++)
    std::printf("%u: %s\n", (unsigned) i, entries[i].desc.c_str());
  return true;
}

std::unique_ptr<DSoundOut> DSoundOut::open(int deviceIndex, unsigned rate,
                                           unsigned blockFrames,
                                           unsigned latencyMs,
                                           FillFn fill, void *ctx) {
  std::unique_ptr<Impl> impl(new Impl);
  impl->fill = fill;
  impl->ctx = ctx;
  impl->rate = rate;

  if (!impl->com.init()) return nullptr;
  if (!impl->lib.load()) return nullptr;

  GUID guid{};
  bool haveGuid = false;
  std::string devName = "default device";
  if (deviceIndex >= 0) {
    std::vector<EnumEntry> entries;
    if (!impl->lib.list(entries)) return nullptr;
    if (static_cast<size_t>(deviceIndex) >= entries.size()) {
      std::fprintf(stderr, "emusc-winmidi: --dsound-out %d out of range "
                   "(%u devices); see --list-dsound-out\n", deviceIndex,
                   (unsigned) entries.size());
      return nullptr;
    }
    haveGuid = entries[deviceIndex].hasGuid;
    guid = entries[deviceIndex].guid;
    devName = entries[deviceIndex].desc;
  }

  HRESULT hr = impl->lib.create(haveGuid ? &guid : nullptr, &impl->ds, nullptr);
  if (FAILED(hr)) {
    impl->ds = nullptr;
    std::fprintf(stderr, "emusc-winmidi: DirectSoundCreate failed (0x%08lx)\n",
                 (unsigned long) hr);
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
    std::fprintf(stderr, "emusc-winmidi: no window handle for DirectSound's "
                 "cooperative level\n");
    return nullptr;
  }
  hr = impl->ds->SetCooperativeLevel(hwnd, DSSCL_PRIORITY);
  if (FAILED(hr)) {
    std::fprintf(stderr, "emusc-winmidi: DirectSound SetCooperativeLevel "
                 "failed (0x%08lx)\n", (unsigned long) hr);
    return nullptr;
  }

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
      std::fprintf(stderr, "emusc-winmidi: DirectSound primary SetFormat "
                   "failed (0x%08lx); the mixer will resample\n",
                   (unsigned long) fr);
  } else {
    std::fprintf(stderr, "emusc-winmidi: DirectSound primary buffer "
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
    std::fprintf(stderr, "emusc-winmidi: --latency/--block too large for a "
                 "DirectSound buffer\n");
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
    std::fprintf(stderr, "emusc-winmidi: DirectSound CreateSoundBuffer failed "
                 "(0x%08lx)\n", (unsigned long) hr);
    return nullptr;
  }

  DSBCAPS caps{};
  caps.dwSize = sizeof(caps);
  hr = impl->buf->GetCaps(&caps);
  if (FAILED(hr) || caps.dwBufferBytes < 2 * blockBytes ||
      caps.dwBufferBytes % BYTES_PER_FRAME != 0) {
    std::fprintf(stderr, "emusc-winmidi: DirectSound buffer has an unusable "
                 "size\n");
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

  if (!impl->zero_all()) {
    std::fprintf(stderr, "emusc-winmidi: DirectSound buffer Lock failed\n");
    return nullptr;
  }
  impl->writePos = 0;
  impl->lastPlay = 0;
  impl->render(impl->targetBytes);
  if (impl->writePos != impl->targetBytes % impl->bufBytes) {
    std::fprintf(stderr, "emusc-winmidi: DirectSound buffer Lock failed\n");
    return nullptr;
  }

  hr = impl->buf->Play(0, 0, DSBPLAY_LOOPING);
  if (FAILED(hr)) {
    std::fprintf(stderr, "emusc-winmidi: DirectSound Play failed (0x%08lx)\n",
                 (unsigned long) hr);
    return nullptr;
  }
  impl->lastPollTick = GetTickCount();
  impl->peakPollTick = impl->lastPollTick;

  std::fprintf(stderr, "emusc-winmidi: DirectSound output '%s' at %u Hz, "
               "%u-frame blocks, %u ms queued in a %u ms ring\n",
               devName.c_str(), rate, blockFrames,
               (unsigned) (static_cast<unsigned long long>(impl->targetBytes) *
                           1000 / (static_cast<unsigned long long>(rate) *
                                   BYTES_PER_FRAME)),
               (unsigned) ringMs);

  return std::unique_ptr<DSoundOut>(new DSoundOut(impl.release()));
}

DSoundOut::~DSoundOut() { delete _impl; }

void DSoundOut::pump() { _impl->pump(); }

}  // namespace emuscd
