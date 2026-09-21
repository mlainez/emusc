// Live audio output for emusc-render's --play, paced by the device itself -
// the same principle emuscd's blocking snd_pcm_writei (Linux) and
// emusc-winmidi's polled buffer ring (Windows) already use for real-time
// playback elsewhere in this project.
//
// One implementation per platform, audio_out_alsa.cc or audio_out_winmm.cc,
// selected by CMakeLists.txt exactly like emuscd/main.cc vs
// emuscd/main_winmidi.cc - so this header and its caller stay free of
// platform #ifdefs.

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
  AudioOut(unsigned rate, unsigned blockFrames, unsigned latencyMs);
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
