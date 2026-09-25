/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * This file consists entirely of AI-generated code without direct human
 * authorship and is dedicated to the public domain under CC0 1.0.
 *
 *  DirectSound stream behind emusc-render's Windows AudioOut (see
 *  audio_out.h), the blocking-write counterpart of emusc-winmidi's polled
 *  DSoundOut (emuscd/audio_out_dsound.h).
 */

#ifndef EMUSC_RENDER_AUDIO_OUT_DSOUND_H
#define EMUSC_RENDER_AUDIO_OUT_DSOUND_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace emusc_tools {

// One looping DirectSound secondary buffer on the system default device,
// filled by write() calls that block until the ring has room. Every call -
// open, write, destruction - must come from the thread that opened it, the
// one COM was initialised on.
//
// dsound.dll and ole32.dll are loaded at runtime rather than linked, so the
// executable's import table is unchanged and a system without DirectSound
// only fails at open().
class DSoundStream {
public:
  // 16-bit stereo at `rate`. blockFrames and latencyMs carry AudioOut's
  // meaning: write granularity, and the amount of audio kept queued ahead
  // of the play cursor. Returns nullptr on failure, with the reason in
  // *why; prints nothing itself except non-fatal warnings.
  static std::unique_ptr<DSoundStream> open(unsigned rate, unsigned blockFrames,
                                            unsigned latencyMs, std::string *why);

  // Plays out everything already written, then stops.
  ~DSoundStream();
  DSoundStream(const DSoundStream &) = delete;
  DSoundStream &operator=(const DSoundStream &) = delete;

  // Queues interleaved frames, blocking while the ring holds its target
  // amount. Playback starts once the first target amount is queued.
  void write(const int16_t *interleaved, size_t frames);

  struct Impl;

private:
  explicit DSoundStream(Impl *impl) : _impl(impl) {}
  Impl *_impl;
};

}  // namespace emusc_tools

#endif
