// ROM-location defaults shared by every command-line tool in this project:
// emusc-render (this directory), and emuscd/emusc-winmidi through
// emuscd/common.h. Nothing here touches libEmuSC itself; it only decides
// which files a tool hands to it.

#pragma once

#include <sys/stat.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace emusc_tools {

// Every device a tool can be pointed at by name, and the <device> prefix of
// its ROM files (<device>_control.bin, <device>_waverom1.bin, ...).
inline const char *const SUPPORTED_DEVICES[] = { "sc55", "sc55mkii", "sc88",
                                                 "jv880", "jv1080" };

inline bool path_is(const std::string &path, unsigned type) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFMT) == type;
}

inline bool is_directory(const std::string &path) { return path_is(path, S_IFDIR); }
inline bool is_regular_file(const std::string &path) { return path_is(path, S_IFREG); }

// The ROM directory a tool uses when --rom-dir is not given, the same for all
// three tools:
//   1. $EMUSCD_ROM_DIR, if set and non-empty;
//   2. ./roms (relative to the working directory), if it is a directory -
//      where ROMs sit next to an unpacked release or in a source checkout;
//   3. otherwise /usr/share/emuscd/roms, the packaged-install location, on
//      systems that have one, and ./roms on Windows, which has no such
//      location. Returned even if it does not exist, so that the error a
//      tool then reports names the directory it looked in.
inline std::string default_rom_dir() {
  const char *env = std::getenv("EMUSCD_ROM_DIR");
  if (env && *env) return env;
  if (is_directory("roms")) return "roms";
#ifdef _WIN32
  return "roms";
#else
  return "/usr/share/emuscd/roms";
#endif
}

// The supported devices whose <device>_control.bin is present in dir, in
// SUPPORTED_DEVICES order. Only the file name is checked: the ROM's own
// identification still runs when it is loaded.
inline std::vector<std::string> devices_in_rom_dir(const std::string &dir) {
  std::vector<std::string> found;
  for (const char *d : SUPPORTED_DEVICES)
    if (is_regular_file(dir + "/" + d + "_control.bin")) found.push_back(d);
  return found;
}

}  // namespace emusc_tools
