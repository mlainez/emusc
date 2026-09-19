// Minimal stereo RIFF/WAVE writer for emusc-render: 16-bit PCM, or IEEE
// float32 for measurement.
//
// 16 bits is not enough to measure a reverb tail. Round-to-nearest leaves a
// quantisation floor at -101 dB relative to full scale, which the side
// channel (L-R)/2 carries at -104 and an octave band at the top of the
// spectrum reads at about -108; a tail that starts 25 dB below a note that
// itself peaks 20 dB below full scale reaches that floor inside a second and
// every slope fitted past it is the floor's, not the reverb's. Float32 output
// has no such floor.

#ifndef EMUSC_RENDER_WAV_H
#define EMUSC_RENDER_WAV_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

class WavWriter {
public:
  WavWriter(const std::string &path, uint32_t rate, uint16_t channels,
            bool as_float = false)
    : _rate(rate), _channels(channels), _float(as_float) {
    _f = std::fopen(path.c_str(), "wb");
    if (!_f) throw std::runtime_error("cannot create output WAV: " + path);
    write_header(0);   // placeholder, patched in close()
  }
  ~WavWriter() { if (_f) close(); }

  void write(const int16_t *frames, size_t nframes) {
    size_t n = nframes * _channels;
    // Little-endian on the wire regardless of host
    std::vector<uint8_t> buf(n * 2);
    for (size_t i = 0; i < n; i++) {
      uint16_t u = static_cast<uint16_t>(frames[i]);
      buf[2 * i]     = uint8_t(u & 0xff);
      buf[2 * i + 1] = uint8_t(u >> 8);
    }
    if (std::fwrite(buf.data(), 1, buf.size(), _f) != buf.size())
      throw std::runtime_error("short write to output WAV");
    _frames += nframes;
  }

  void write(const float *frames, size_t nframes) {
    size_t n = nframes * _channels;
    std::vector<uint8_t> buf(n * 4);
    for (size_t i = 0; i < n; i++) {
      uint32_t u;
      std::memcpy(&u, &frames[i], 4);
      buf[4 * i]     = uint8_t(u & 0xff);
      buf[4 * i + 1] = uint8_t((u >> 8) & 0xff);
      buf[4 * i + 2] = uint8_t((u >> 16) & 0xff);
      buf[4 * i + 3] = uint8_t(u >> 24);
    }
    if (std::fwrite(buf.data(), 1, buf.size(), _f) != buf.size())
      throw std::runtime_error("short write to output WAV");
    _frames += nframes;
  }

  uint64_t frames() const { return _frames; }

  void close() {
    if (!_f) return;
    uint64_t data_bytes = _frames * _channels * sample_bytes();
    if (data_bytes > 0xffffffffull - 44)
      throw std::runtime_error("output too large for a RIFF/WAVE file");
    std::fseek(_f, 0, SEEK_SET);
    write_header(static_cast<uint32_t>(data_bytes));
    if (std::fclose(_f) != 0) { _f = nullptr; throw std::runtime_error("error closing output WAV"); }
    _f = nullptr;
  }

private:
  std::FILE *_f = nullptr;
  uint32_t _rate;
  uint16_t _channels;
  bool _float = false;
  uint64_t _frames = 0;

  uint16_t sample_bytes() const { return _float ? 4 : 2; }

  void put32(uint32_t v) { uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; std::fwrite(b, 1, 4, _f); }
  void put16(uint16_t v) { uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)}; std::fwrite(b, 1, 2, _f); }
  void write_header(uint32_t data_bytes) {
    const uint16_t sb = sample_bytes();
    std::fwrite("RIFF", 1, 4, _f);
    put32(36 + data_bytes);
    std::fwrite("WAVE", 1, 4, _f);
    std::fwrite("fmt ", 1, 4, _f);
    put32(16);                          // fmt chunk size
    put16(_float ? 3 : 1);              // WAVE_FORMAT_IEEE_FLOAT or _PCM
    put16(_channels);
    put32(_rate);
    put32(_rate * _channels * sb);      // byte rate
    put16(_channels * sb);              // block align
    put16(sb * 8);                      // bits per sample
    std::fwrite("data", 1, 4, _f);
    put32(data_bytes);
  }
};

#endif
