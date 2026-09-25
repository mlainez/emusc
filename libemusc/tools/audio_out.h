// Live audio output for emusc-render's --play, paced by the device itself -
// the same principle emuscd's blocking snd_pcm_writei (Linux) and
// emusc-winmidi's polled buffer ring (Windows) already use for real-time
// playback elsewhere in this project.
//
// One implementation per platform, audio_out_alsa.cc or audio_out_winmm.cc,
// selected by CMakeLists.txt exactly like emuscd/main.cc vs
// emuscd/main_winmidi.cc. Windows has two output APIs to choose between
// (WinMM, and DirectSound in audio_out_dsound.cc), so its constructor alone
// takes that choice.

#ifndef EMUSC_RENDER_AUDIO_OUT_H
#define EMUSC_RENDER_AUDIO_OUT_H

#include <cstddef>
#include <cstdint>

class AudioOut {
public:
  // Opens 16-bit stereo interleaved playback at the given rate. blockFrames
  // and latencyMs carry the same meaning as emuscd's/emusc-winmidi's own
  // --block/--latency: audio frames per device write, and the requested
  // total output buffer depth in milliseconds. Exits the process with code
  // 4 on failure, matching this tool's own die() for every other
  // unrecoverable startup error.
#ifdef _WIN32
  // Auto tries DirectSound and falls back to WinMM, saying why on stderr,
  // if DirectSound cannot be opened; only a WinMM failure is then fatal.
  // WinMM and DSound use that API alone, and any failure of it is fatal.
  enum class Api { Auto, WinMM, DSound };
  AudioOut(unsigned rate, unsigned blockFrames, unsigned latencyMs, Api api);
#else
  AudioOut(unsigned rate, unsigned blockFrames, unsigned latencyMs);
#endif
  ~AudioOut();

  AudioOut(const AudioOut &) = delete;
  AudioOut &operator=(const AudioOut &) = delete;

  // Blocks until the device has accepted (Linux) or queued (Windows) these
  // frames. That block is what paces the render loop to real time instead
  // of rendering as fast as possible.
  void write(const int16_t *interleaved, size_t frames);

private:
  struct Impl;
  Impl *_impl;
};

#endif
