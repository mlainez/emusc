// Windows implementation of AudioOut (see audio_out.h), the same WinMM
// waveOut setup as emuscd/main_winmidi.cc's open_wave_out()/pump_wave_buffers(),
// reduced to what a single-stream batch renderer needs: no device-switching,
// no MIDI input, and a blocking write instead of a polled main loop, since
// this tool has nothing else to service while it waits.

#include "audio_out.h"

#include <windows.h>
#include <mmsystem.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

struct AudioOut::Impl {
  HWAVEOUT hwo = nullptr;
  struct Buf { WAVEHDR hdr; std::vector<int16_t> data; };
  std::vector<Buf> buffers;
  unsigned next = 0;
  unsigned blockFrames = 0;
};

AudioOut::AudioOut(unsigned rate, unsigned blockFrames, unsigned latencyMs)
    : _impl(new Impl) {
  _impl->blockFrames = blockFrames;

  WAVEFORMATEX wfx{};
  wfx.wFormatTag = WAVE_FORMAT_PCM;
  wfx.nChannels = 2;
  wfx.nSamplesPerSec = rate;
  wfx.wBitsPerSample = 16;
  wfx.nBlockAlign = static_cast<WORD>(wfx.nChannels * wfx.wBitsPerSample / 8);
  wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

  MMRESULT r = waveOutOpen(&_impl->hwo, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
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
  _impl->buffers.resize(numBuffers);
  for (auto &b : _impl->buffers) {
    b.data.assign(static_cast<size_t>(blockFrames) * 2, 0);
    std::memset(&b.hdr, 0, sizeof(WAVEHDR));
    b.hdr.lpData = reinterpret_cast<LPSTR>(b.data.data());
    b.hdr.dwBufferLength = static_cast<DWORD>(b.data.size() * sizeof(int16_t));
    waveOutPrepareHeader(_impl->hwo, &b.hdr, sizeof(WAVEHDR));
    b.hdr.dwFlags |= WHDR_DONE;   // free at startup, nothing queued yet
  }
  std::fprintf(stderr, "emusc-render: --play: WinMM output at %u Hz, "
               "%u x %u-frame buffers\n", rate, numBuffers, blockFrames);
}

AudioOut::~AudioOut() {
  if (_impl->hwo) {
    // Let whatever is already queued finish rather than cutting it off.
    for (auto &b : _impl->buffers)
      while (!(b.hdr.dwFlags & WHDR_DONE)) Sleep(1);
    for (auto &b : _impl->buffers)
      waveOutUnprepareHeader(_impl->hwo, &b.hdr, sizeof(WAVEHDR));
    waveOutClose(_impl->hwo);
  }
  delete _impl;
}

void AudioOut::write(const int16_t *interleaved, size_t frames) {
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
