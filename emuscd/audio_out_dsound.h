/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * This file consists entirely of AI-generated code without direct human
 * authorship and is dedicated to the public domain under CC0 1.0.
 *
 *  DirectSound output for emusc-winmidi (its default --audio-api auto, or
 *  --audio-api dsound), the alternative to its built-in WinMM waveOut ring.
 */

#pragma once

#include <cstdint>
#include <memory>

namespace emuscd {

// One looping DirectSound secondary buffer, fed from the caller's own polling
// loop: pump() is called as often as the caller likes (emusc-winmidi calls it
// every ~1 ms), and tops the buffer up through the fill callback whenever the
// play cursor has made room. Every call - open, pump, destruction - must come
// from the same thread, the one COM was initialised on.
//
// dsound.dll and ole32.dll are loaded at runtime rather than linked, so the
// executable's import table is unchanged by this backend's presence and a
// system without DirectSound can still start, with open() returning nullptr.
class DSoundOut {
public:
  // Renders exactly `frames` interleaved 16-bit stereo frames into `dst`.
  using FillFn = void (*)(void *ctx, int16_t *dst, unsigned frames);

  // deviceIndex is an index into list_devices()'s output, or -1 for the
  // system default device. blockFrames and latencyMs carry the same meaning
  // as --block/--latency on the WinMM path: rendering granularity, and the
  // requested amount of audio kept queued ahead of the play cursor. Returns
  // nullptr, with the reason already printed to stderr, on any failure.
  static std::unique_ptr<DSoundOut> open(int deviceIndex, unsigned rate,
                                         unsigned blockFrames,
                                         unsigned latencyMs,
                                         FillFn fill, void *ctx);

  // Prints every DirectSound output device with its index. Returns false if
  // DirectSound itself is unavailable.
  static bool list_devices();

  ~DSoundOut();
  DSoundOut(const DSoundOut &) = delete;
  DSoundOut &operator=(const DSoundOut &) = delete;

  void pump();

  struct Impl;

private:
  explicit DSoundOut(Impl *impl) : _impl(impl) {}
  Impl *_impl;
};

}  // namespace emuscd
