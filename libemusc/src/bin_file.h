// A minimal std::ifstream/std::ofstream substitute backed by plain C stdio,
// covering only the subset of their interface this project uses.
//
// <fstream> (like <iostream>, <sstream>, <mutex> and <thread>) links against
// libstdc++'s ios_base/threading internals, which on Windows call native
// condition-variable and thread-identity APIs that don't exist before
// Windows Vista and XP SP1 respectively. Plain FILE* has no such dependency
// on any platform, and every one of this project's own ROM/WAV file
// accesses only ever needs open/read/write/seek/tell.
#pragma once

#include <cstdio>
#include <cstddef>
#include <string>
#include <vector>

namespace EmuSC {

class BinFile {
public:
  enum Whence { kBeg = SEEK_SET, kCur = SEEK_CUR, kEnd = SEEK_END };

  BinFile() = default;
  BinFile(const std::string &path, const char *mode) { open(path, mode); }
  ~BinFile() { close(); }

  BinFile(const BinFile &) = delete;
  BinFile &operator=(const BinFile &) = delete;

  void open(const std::string &path, const char *mode) {
    close();
    _fp = std::fopen(path.c_str(), mode);
  }
  bool is_open() const { return _fp != nullptr; }
  void close() { if (_fp) { std::fclose(_fp); _fp = nullptr; } }

  // Mirrors std::istream::read()'s use as a boolean condition
  // (`if (!f.read(buf, n))`): true iff every requested byte was read.
  bool read(char *buf, size_t n) {
    _last_read = _fp ? std::fread(buf, 1, n, _fp) : 0;
    return _last_read == n;
  }
  size_t gcount() const { return _last_read; }

  void write(const char *buf, size_t n) {
    if (_fp) std::fwrite(buf, 1, n, _fp);
  }

  void seekg(long pos, int whence = kBeg) { if (_fp) std::fseek(_fp, pos, whence); }
  long tellg() const { return _fp ? std::ftell(_fp) : -1; }

  bool good() const { return _fp && !std::ferror(_fp); }
  void clear() { if (_fp) std::clearerr(_fp); }

  // Matches the std::vector<char> data((std::istreambuf_iterator<char>(f)),
  // std::istreambuf_iterator<char>()) idiom: everything from the current
  // position to end of file.
  std::vector<char> read_all() {
    std::vector<char> data;
    if (!_fp) return data;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), _fp)) > 0)
      data.insert(data.end(), buf, buf + n);
    return data;
  }

private:
  FILE *_fp = nullptr;
  size_t _last_read = 0;
};

}  // namespace EmuSC
