// Windows implementation of AudioOut (see audio_out.h): DirectSound through
// audio_out_dsound.cc, or the same WinMM waveOut setup as
// emuscd/main_winmidi.cc's open_wave_out()/pump_wave_buffers(), reduced to
// what a single-stream batch renderer needs: no device-switching, no MIDI
// input, and a blocking write instead of a polled main loop, since this
// tool has nothing else to service while it waits.

#include "audio_out.h"
#include "audio_out_dsound.h"
#include "win_realtime.h"

#include <windows.h>
#include <mmsystem.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

struct AudioOut::Impl {
  // Set when DirectSound is in use; the WinMM members are then unused.
  std::unique_ptr<emusc_tools::DSoundStream> dsound;

  HWAVEOUT hwo = nullptr;
  struct Buf { WAVEHDR hdr; std::vector<int16_t> data; };
  std::vector<Buf> buffers;
  unsigned next = 0;
  unsigned blockFrames = 0;
  // Released only after the destructor has drained the output.
  std::unique_ptr<emusc_tools::SystemTimerResolution> timerRes;

  void open_winmm(unsigned rate, unsigned blockFrames, unsigned latencyMs);
};

void AudioOut::Impl::open_winmm(unsigned rate, unsigned blockFrames_,
                                unsigned latencyMs) {
  blockFrames = blockFrames_;

  WAVEFORMATEX wfx{};
  wfx.wFormatTag = WAVE_FORMAT_PCM;
  wfx.nChannels = 2;
  wfx.nSamplesPerSec = rate;
  wfx.wBitsPerSample = 16;
  wfx.nBlockAlign = static_cast<WORD>(wfx.nChannels * wfx.wBitsPerSample / 8);
  wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

  MMRESULT r = waveOutOpen(&hwo, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
  if (r != MMSYSERR_NOERROR) {
    std::fprintf(stderr, "emusc-render: --play: waveOutOpen failed (error %u)\n",
                 (unsigned) r);
    std::exit(4);
  }

  // Same derivation as emusc-winmidi's own open_wave_out(): enough buffers of
  // blockFrames each to cover the requested latency, at least 2 so one can
  // always be filling while the other plays.
  unsigned numBuffers = (latencyMs * rate) / 1000 / blockFrames;
  if (numBuffers < 2) numBuffers = 2;
  buffers.resize(numBuffers);
  for (auto &b : buffers) {
    b.data.assign(static_cast<size_t>(blockFrames) * 2, 0);
    std::memset(&b.hdr, 0, sizeof(WAVEHDR));
    b.hdr.lpData = reinterpret_cast<LPSTR>(b.data.data());
    b.hdr.dwBufferLength = static_cast<DWORD>(b.data.size() * sizeof(int16_t));
    waveOutPrepareHeader(hwo, &b.hdr, sizeof(WAVEHDR));
    b.hdr.dwFlags |= WHDR_DONE;   // free at startup, nothing queued yet
  }
  std::fprintf(stderr, "emusc-render: --play: WinMM output at %u Hz, "
               "%u x %u-frame buffers\n", rate, numBuffers, blockFrames);
}

AudioOut::AudioOut(unsigned rate, unsigned blockFrames, unsigned latencyMs,
                   Api api)
    : _impl(new Impl) {
  if (api != Api::WinMM) {
    std::string why;
    _impl->dsound = emusc_tools::DSoundStream::open(rate, blockFrames,
                                                    latencyMs, &why);
    if (!_impl->dsound) {
      if (api == Api::DSound) {
        std::fprintf(stderr, "emusc-render: --play: DirectSound unavailable: "
                     "%s\n", why.c_str());
        std::exit(4);
      }
      std::fprintf(stderr, "emusc-render: --play: DirectSound unavailable (%s); "
                   "falling back to WinMM\n", why.c_str());
    }
  }
  if (!_impl->dsound) _impl->open_winmm(rate, blockFrames, latencyMs);

  // This tool is single-threaded: whichever thread constructs AudioOut is
  // the one that renders and refills the output in write(), whose Sleep(1)
  // waits need the raised timer resolution with either API.
  _impl->timerRes.reset(new emusc_tools::SystemTimerResolution(1));
  emusc_tools::raise_audio_thread_priority();
}

AudioOut::~AudioOut() {
  // Both paths let whatever is already queued finish rather than cutting it
  // off.
  _impl->dsound.reset();
  if (_impl->hwo) {
    for (auto &b : _impl->buffers)
      while (!(b.hdr.dwFlags & WHDR_DONE)) Sleep(1);
    for (auto &b : _impl->buffers)
      waveOutUnprepareHeader(_impl->hwo, &b.hdr, sizeof(WAVEHDR));
    waveOutClose(_impl->hwo);
  }
  delete _impl;
}

void AudioOut::write(const int16_t *interleaved, size_t frames) {
  if (_impl->dsound) { _impl->dsound->write(interleaved, frames); return; }

  size_t done = 0;
  while (done < frames) {
    Impl::Buf &b = _impl->buffers[_impl->next];
    while (!(b.hdr.dwFlags & WHDR_DONE)) Sleep(1);

    size_t n = frames - done;
    if (n > _impl->blockFrames) n = _impl->blockFrames;
    std::memcpy(b.data.data(), interleaved + done * 2,
                n * 2 * sizeof(int16_t));
    b.hdr.dwBufferLength = static_cast<DWORD>(n * 2 * sizeof(int16_t));
    b.hdr.dwFlags &= ~WHDR_DONE;
    waveOutWrite(_impl->hwo, &b.hdr, sizeof(WAVEHDR));

    _impl->next = (_impl->next + 1) % _impl->buffers.size();
    done += n;
  }
}
