// Windows implementation of AudioOut (see audio_out.h), the same WinMM
// waveOut setup as emuscd/main_winmidi.cc's open_wave_out()/pump_wave_buffers(),
// reduced to what a single-stream batch renderer needs: no device-switching,
// no MIDI input, and a blocking write instead of a polled main loop, since
// this tool has nothing else to service while it waits.

#include "audio_out.h"

#include <windows.h>
#include <mmsystem.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
// 4 buffers of 2048 frames each is ~170 ms of queue depth at 48 kHz - enough
// that Sleep(1)'s ~1-15 ms scheduler granularity on Windows 9x/ME can't
// starve the device between polls.
constexpr unsigned kBlockFrames = 2048;
constexpr unsigned kNumBuffers = 4;
}  // namespace

struct AudioOut::Impl {
  HWAVEOUT hwo = nullptr;
  struct Buf { WAVEHDR hdr; std::vector<int16_t> data; };
  std::vector<Buf> buffers;
  unsigned next = 0;
};

AudioOut::AudioOut(unsigned rate) : _impl(new Impl) {
  WAVEFORMATEX wfx{};
  wfx.wFormatTag = WAVE_FORMAT_PCM;
  wfx.nChannels = 2;
  wfx.nSamplesPerSec = rate;
  wfx.wBitsPerSample = 16;
  wfx.nBlockAlign = static_cast<WORD>(wfx.nChannels * wfx.wBitsPerSample / 8);
  wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

  MMRESULT r = waveOutOpen(&_impl->hwo, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
  if (r != MMSYSERR_NOERROR) {
    std::cerr << "emusc-render: --play: waveOutOpen failed (error " << r
              << ")" << std::endl;
    std::exit(4);
  }

  _impl->buffers.resize(kNumBuffers);
  for (auto &b : _impl->buffers) {
    b.data.assign(static_cast<size_t>(kBlockFrames) * 2, 0);
    std::memset(&b.hdr, 0, sizeof(WAVEHDR));
    b.hdr.lpData = reinterpret_cast<LPSTR>(b.data.data());
    b.hdr.dwBufferLength = static_cast<DWORD>(b.data.size() * sizeof(int16_t));
    waveOutPrepareHeader(_impl->hwo, &b.hdr, sizeof(WAVEHDR));
    b.hdr.dwFlags |= WHDR_DONE;   // free at startup, nothing queued yet
  }
  std::cerr << "emusc-render: --play: WinMM output at " << rate << " Hz"
            << std::endl;
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
    if (n > kBlockFrames) n = kBlockFrames;
    std::memcpy(b.data.data(), interleaved + done * 2,
                n * 2 * sizeof(int16_t));
    b.hdr.dwBufferLength = static_cast<DWORD>(n * 2 * sizeof(int16_t));
    b.hdr.dwFlags &= ~WHDR_DONE;
    waveOutWrite(_impl->hwo, &b.hdr, sizeof(WAVEHDR));

    _impl->next = (_impl->next + 1) % kNumBuffers;
    done += n;
  }
}
