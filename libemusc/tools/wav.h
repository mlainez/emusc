// Minimal 16-bit PCM stereo RIFF/WAVE writer for emusc-render.

#ifndef EMUSC_RENDER_WAV_H
#define EMUSC_RENDER_WAV_H

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

class WavWriter {
public:
  WavWriter(const std::string &path, uint32_t rate, uint16_t channels)
    : _rate(rate), _channels(channels) {
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

  uint64_t frames() const { return _frames; }

  void close() {
    if (!_f) return;
    uint64_t data_bytes = _frames * _channels * 2;
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
  uint64_t _frames = 0;

  void put32(uint32_t v) { uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; std::fwrite(b, 1, 4, _f); }
  void put16(uint16_t v) { uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)}; std::fwrite(b, 1, 2, _f); }
  void write_header(uint32_t data_bytes) {
    std::fwrite("RIFF", 1, 4, _f);
    put32(36 + data_bytes);
    std::fwrite("WAVE", 1, 4, _f);
    std::fwrite("fmt ", 1, 4, _f);
    put32(16);                          // PCM fmt chunk size
    put16(1);                           // WAVE_FORMAT_PCM
    put16(_channels);
    put32(_rate);
    put32(_rate * _channels * 2);       // byte rate
    put16(_channels * 2);               // block align
    put16(16);                          // bits per sample
    std::fwrite("data", 1, 4, _f);
    put32(data_bytes);
  }
};

#endif
