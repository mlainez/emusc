// Linux implementation of AudioOut (see audio_out.h), the same ALSA
// PCM-output setup and blocking-write pacing as emuscd/main.cc's
// init_alsa_audio()/write_block(), reduced to what a single-stream batch
// renderer needs: one playback device, no device-switching, no MIDI input.

#include "audio_out.h"

#include <alsa/asoundlib.h>

#include <cstdlib>
#include <iostream>

struct AudioOut::Impl {
  snd_pcm_t *pcm = nullptr;
};

// blockFrames isn't used here: main.cc's own render loop already controls
// write() granularity via --block (the same way emuscd's write_block() does
// on the daemon side), and snd_pcm_writei() itself handles any frame count,
// unlike WinMM's fixed-size WAVEHDR buffers. Only latencyMs maps onto
// anything ALSA-specific, the same way it does in emuscd's init_alsa_audio().
AudioOut::AudioOut(unsigned rate, unsigned /*blockFrames*/, unsigned latencyMs)
    : _impl(new Impl) {
  int err = snd_pcm_open(&_impl->pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
  if (err < 0) {
    std::cerr << "emusc-render: --play: PCM open error: " << snd_strerror(err)
              << std::endl;
    std::exit(4);
  }

  snd_pcm_hw_params_t *hw;
  snd_pcm_hw_params_alloca(&hw);
  snd_pcm_hw_params_any(_impl->pcm, hw);
  snd_pcm_hw_params_set_access(_impl->pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  snd_pcm_hw_params_set_format(_impl->pcm, hw, SND_PCM_FORMAT_S16_LE);
  snd_pcm_hw_params_set_channels(_impl->pcm, hw, 2);
  unsigned r = rate;
  snd_pcm_hw_params_set_rate_near(_impl->pcm, hw, &r, 0);
  unsigned buffer_time_us = latencyMs * 1000;
  snd_pcm_hw_params_set_buffer_time_near(_impl->pcm, hw, &buffer_time_us, 0);

  if (snd_pcm_hw_params(_impl->pcm, hw) < 0) {
    std::cerr << "emusc-render: --play: could not set PCM parameters"
              << std::endl;
    std::exit(4);
  }
  std::cerr << "emusc-render: --play: ALSA output at " << r << " Hz"
            << std::endl;
}

AudioOut::~AudioOut() {
  if (_impl->pcm) {
    snd_pcm_drain(_impl->pcm);   // let what's already queued finish playing
    snd_pcm_close(_impl->pcm);
  }
  delete _impl;
}

void AudioOut::write(const int16_t *interleaved, size_t frames) {
  size_t offset = 0;
  while (offset < frames) {
    snd_pcm_sframes_t written = snd_pcm_writei(
      _impl->pcm, interleaved + offset * 2, frames - offset);
    if (written < 0) {
      if (snd_pcm_recover(_impl->pcm, static_cast<int>(written), 1) < 0) {
        std::cerr << "emusc-render: --play: PCM write error: "
                  << snd_strerror(static_cast<int>(written)) << std::endl;
        return;
      }
      continue;
    }
    offset += static_cast<size_t>(written);
  }
}
