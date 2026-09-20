// Pieces of the Roland Sound Canvas daemon shared between its ALSA backend
// (main.cc, Linux) and its WinMM backend (main_winmidi.cc, Windows): ROM
// filename resolution and raw-MIDI-byte parsing, neither of which touches
// either platform's audio/MIDI API.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>

#include "../libemusc/src/synth.h"

namespace emuscd {

inline const char *SUPPORTED_DEVICES[] = { "sc55", "sc55mkii", "sc88", "jv880" };

// The sound map emuscd runs in. Named once so that the value handed to the
// Synth constructor and the value handed to its power-on reset cannot drift
// apart: reset(sm, ...) re-applies the map, so passing a different one there
// would silently switch the device into another mode. GS is the Synth default
// and the mode all four supported devices are addressed in; emuscd exposes no
// option to change it.
inline const EmuSC::Synth::SoundMap SOUND_MAP = EmuSC::Synth::SoundMap::GS;

inline bool device_supported(const std::string &dev) {
  for (const char *d : SUPPORTED_DEVICES)
    if (dev == d) return true;
  return false;
}

struct DeviceRoms {
  std::string control_rom, cpu_rom;
  std::vector<std::string> wave_roms;
};

// One naming convention for all four devices, shared with emusc-render's
// --device preset (libemusc/tools/main.cc): <device>_control.bin is always
// the control/program ROM, <device>_cpu.bin is the internal CPU ROM that
// only SC-55 and SC-55mkII have. See README.md for exact file hashes.
inline DeviceRoms resolve_device_roms(const std::string &device, const std::string &rom_dir) {
  DeviceRoms r;
  r.control_rom = rom_dir + "/" + device + "_control.bin";
  if (device == "sc55" || device == "sc55mkii")
    r.cpu_rom = rom_dir + "/" + device + "_cpu.bin";

  int n = (device == "sc55") ? 3 : (device == "sc88") ? 4 : 2;  // sc55mkii, jv880
  for (int k = 1; k <= n; k++)
    r.wave_roms.push_back(rom_dir + "/" + device + "_waverom" + std::to_string(k) + ".bin");
  return r;
}

inline int16_t to_i16(float x) {
  if (x > 1.0f) x = 1.0f;
  if (x < -1.0f) x = -1.0f;
  long v = std::lrintf(x * 32767.0f);
  if (v > 32767) v = 32767;
  if (v < -32767) v = -32767;
  return static_cast<int16_t>(v);
}

// Assembles raw MIDI bytes into complete channel messages, honouring running
// status so a stream of note-on/note-off pairs (which omit the repeated
// status byte) is parsed correctly instead of misreading data as a status.
// System real-time bytes (0xF8-0xFF) pass through without disturbing state,
// since they can be interleaved into another message.
//
// SysEx is assembled whole, 0xF0 through the terminating 0xF7, and handed over
// with both framing bytes: Synth::_apply_midi_sysex rejects any message whose
// first byte is not 0xF0 and whose last is not 0xF7. This is how a GS reset and
// every part parameter a song states up front reach the synth; on the Sound
// Canvas family that is most of how a song describes itself.
//
// A status byte other than 0xF7 arriving mid-message abandons the SysEx rather
// than terminating it, which is what the receiver is meant to do when a sender
// is interrupted, and keeps a truncated message from being acted on. Messages
// longer than the uint16_t length Synth::midi_input_sysex takes are dropped for
// the same reason: forwarding a prefix would apply a partial parameter block.
class MidiParser {
public:
  template <typename F, typename G>
  void feed(uint8_t byte, F &&on_message, G &&on_sysex) {
    if (byte >= 0xf8)
      return;

    if (_inSysex) {
      if (byte == 0xf7) {
        _inSysex = false;
        if (!_sysexOverflow) {
          _sysex.push_back(0xf7);
          on_sysex(_sysex.data(), static_cast<uint16_t>(_sysex.size()));
        }
        _sysex.clear();
        return;
      }
      if (byte & 0x80) {           // abandoned by the sender
        _inSysex = false;
        _sysex.clear();
        // fall through and parse this byte as the status it is
      } else {
        if (_sysex.size() >= 0xffff)
          _sysexOverflow = true;
        else
          _sysex.push_back(byte);
        return;
      }
    }

    if (byte == 0xf0) {
      // A System Common message cancels running status (MIDI 1.0), so the next
      // data byte cannot be taken for a continuation of whatever preceded it.
      _runningStatus = 0;
      _data.clear();
      _inSysex = true;
      _sysexOverflow = false;
      _sysex.clear();
      _sysex.push_back(0xf0);
      return;
    }
    if (byte == 0xf7)              // stray end-of-exclusive
      return;

    if (byte & 0x80) {
      if (byte >= 0xf1 && byte <= 0xf6) {
        _runningStatus = 0;
        return;
      }
      _runningStatus = byte;
      _data.clear();
      return;
    }
    if (!_runningStatus)
      return;
    _data.push_back(byte);
    if (_data.size() >= _data_bytes_needed(_runningStatus)) {
      uint8_t d1 = _data.size() > 0 ? _data[0] : 0;
      uint8_t d2 = _data.size() > 1 ? _data[1] : 0;
      on_message(_runningStatus, d1, d2);
      _data.clear();
    }
  }

private:
  static size_t _data_bytes_needed(uint8_t status) {
    switch (status & 0xf0) {
    case 0xc0: case 0xd0: return 1;   // program change, channel pressure
    default:              return 2;
    }
  }

  uint8_t _runningStatus = 0;
  std::vector<uint8_t> _data;
  bool _inSysex = false;
  bool _sysexOverflow = false;
  std::vector<uint8_t> _sysex;
};

}  // namespace emuscd
