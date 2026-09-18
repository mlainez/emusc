// Standard MIDI File reader for emusc-render.
//
// Parses SMF format 0 and 1 (MIDI 1.0 Detailed Specification, "Standard
// MIDI Files 1.0"), merges all tracks, resolves the tempo map and converts
// every event's absolute tick to an absolute time in microseconds using
// exact integer arithmetic (no accumulated floating-point drift).
//
// This file knows nothing about any synthesizer; it is a generic SMF
// parser.

#ifndef EMUSC_RENDER_SMF_H
#define EMUSC_RENDER_SMF_H

#include <cstdint>
#include <string>
#include <vector>

namespace smf {

enum class Kind {
  Channel,   // 0x80..0xEF: status + 1 or 2 data bytes
  SysEx,     // complete F0 ... F7 message (multi-packet messages reassembled)
  Meta,      // FF type len data  (kept for tempo / end-of-track bookkeeping)
};

struct Event {
  Kind     kind;
  uint64_t tick;        // absolute tick within its track
  uint64_t time_num;    // absolute time as a rational: time_s = time_num / time_den
  int      track;       // track index in file order
  uint32_t seq;         // sequence number within track (for stable ordering)
  std::vector<uint8_t> bytes;  // Channel: status, d1[, d2]; SysEx: F0..F7; Meta: type, data...
};

struct File {
  int      format   = 0;
  uint16_t ntracks  = 0;
  // Time base. For metrical time (the common case) ticks per quarter note;
  // for SMPTE time, frames per second and ticks per frame.
  bool     smpte    = false;
  uint16_t ppqn     = 0;
  int      fps      = 0;      // 24, 25, 29 (29.97 drop-frame), 30
  int      ticks_per_frame = 0;

  // Denominator shared by every Event::time_num so that
  //   seconds = time_num / time_den
  // For metrical time: time_den = ppqn * 1'000'000 (time_num is in
  // tick-microseconds). For SMPTE 29.97: time_den = 30000*tpf, else fps*tpf.
  uint64_t time_den = 1;

  std::vector<Event> events;          // all tracks merged, sorted by time, then track, then seq
  uint64_t end_of_track_num = 0;      // latest End Of Track meta, same units as time_num
  size_t   n_tempo_changes  = 0;
};

// Parse an SMF from memory. Throws std::runtime_error with a descriptive
// message on malformed input.
File parse(const std::vector<uint8_t> &data);

// Load a file from disk and parse it.
File load(const std::string &path);

// Convert a rational time (num/den seconds) to a sample index at the given
// rate, rounding to nearest (ties away from zero). Exact for all inputs that
// fit in 128-bit intermediate arithmetic.
uint64_t to_sample(uint64_t num, uint64_t den, uint64_t rate);

}  // namespace smf

#endif
