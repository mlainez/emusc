// Standard MIDI File reader for emusc-render. See smf.h.

#include "smf.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace smf {

namespace {

struct Cursor {
  const uint8_t *p;
  const uint8_t *end;
  size_t left() const { return static_cast<size_t>(end - p); }
  uint8_t u8() {
    if (p >= end) throw std::runtime_error("SMF: unexpected end of track data");
    return *p++;
  }
  uint32_t vlq() {
    // Variable-length quantity: up to 4 bytes, MSB first, high bit = continue
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
      uint8_t b = u8();
      v = (v << 7) | (b & 0x7f);
      if (!(b & 0x80)) return v;
    }
    throw std::runtime_error("SMF: variable-length quantity longer than 4 bytes");
  }
};

uint32_t be32(const uint8_t *p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint16_t be16(const uint8_t *p) { return uint16_t((p[0] << 8) | p[1]); }

int channel_data_bytes(uint8_t status) {
  switch (status & 0xf0) {
    case 0xc0: case 0xd0: return 1;
    default:              return 2;   // 8x 9x Ax Bx Ex
  }
}

struct RawEvent {
  Kind kind;
  uint64_t tick;
  int track;
  uint32_t seq;
  std::vector<uint8_t> bytes;
};

struct TempoChange {
  uint64_t tick;
  uint32_t us_per_qn;
  int track;
  uint32_t seq;
};

}  // namespace

File parse(const std::vector<uint8_t> &data) {
  File f;
  if (data.size() < 14 || std::string(data.begin(), data.begin() + 4) != "MThd")
    throw std::runtime_error("SMF: missing MThd header");
  uint32_t hlen = be32(&data[4]);
  if (hlen < 6 || 8 + hlen > data.size())
    throw std::runtime_error("SMF: bad MThd length");
  f.format  = be16(&data[8]);
  f.ntracks = be16(&data[10]);
  uint16_t division = be16(&data[12]);

  if (f.format != 0 && f.format != 1)
    throw std::runtime_error("SMF: only format 0 and 1 are supported (got format " +
                             std::to_string(f.format) + ")");

  if (division & 0x8000) {
    // SMPTE time: high byte is negative frames-per-second, low byte ticks/frame
    f.smpte = true;
    int neg = static_cast<int8_t>(division >> 8);
    f.fps = -neg;
    f.ticks_per_frame = division & 0xff;
    if (f.ticks_per_frame == 0 ||
        !(f.fps == 24 || f.fps == 25 || f.fps == 29 || f.fps == 30))
      throw std::runtime_error("SMF: unsupported SMPTE division");
    // 29 means 29.97 (30000/1001) drop-frame. seconds = tick / (fps * tpf);
    // for 29.97: seconds = tick * 1001 / (30000 * tpf)
    if (f.fps == 29) f.time_den = 30000ull * f.ticks_per_frame;
    else             f.time_den = uint64_t(f.fps) * f.ticks_per_frame;
  } else {
    f.ppqn = division;
    if (f.ppqn == 0) throw std::runtime_error("SMF: division of zero ticks per quarter note");
    f.time_den = uint64_t(f.ppqn) * 1000000ull;
  }

  // ---- Pass 1: read every track into raw events with absolute ticks --------
  std::vector<RawEvent>    raw;
  std::vector<TempoChange> tempi;
  std::vector<std::pair<uint64_t, int>> eots;   // (tick, track)

  size_t pos = 8 + hlen;
  int track_no = 0;
  while (pos + 8 <= data.size() && track_no < f.ntracks) {
    std::string tag(data.begin() + pos, data.begin() + pos + 4);
    uint32_t len = be32(&data[pos + 4]);
    if (pos + 8 + len > data.size())
      throw std::runtime_error("SMF: chunk '" + tag + "' runs past end of file");
    if (tag != "MTrk") {          // alien chunks are to be skipped (spec)
      pos += 8 + len;
      continue;
    }

    Cursor c{&data[pos + 8], &data[pos + 8 + len]};
    uint64_t tick = 0;
    uint8_t running = 0;
    uint32_t seq = 0;
    std::vector<uint8_t> pending_sysex;   // multi-packet F0 ... F7 reassembly
    bool ended = false;

    while (c.left() > 0 && !ended) {
      tick += c.vlq();
      uint8_t b = c.u8();
      uint8_t status;
      if (b & 0x80) {
        status = b;
      } else {
        if (running == 0) throw std::runtime_error("SMF: data byte without running status");
        status = running;
        c.p--;          // re-read b as the first data byte
      }

      if (status < 0xf0) {
        running = status;
        int n = channel_data_bytes(status);
        RawEvent e{Kind::Channel, tick, track_no, seq++, {}};
        e.bytes.push_back(status);
        for (int i = 0; i < n; i++) {
          uint8_t d = c.u8();
          if (d & 0x80) throw std::runtime_error("SMF: status byte where data byte expected");
          e.bytes.push_back(d);
        }
        raw.push_back(std::move(e));
      } else if (status == 0xf0) {
        // System exclusive: F0 <len> <bytes>. Complete iff it ends in F7.
        // Running status is cancelled by SysEx and meta events.
        running = 0;
        uint32_t n = c.vlq();
        if (n > c.left()) throw std::runtime_error("SMF: SysEx length runs past track end");
        pending_sysex.clear();
        pending_sysex.push_back(0xf0);
        pending_sysex.insert(pending_sysex.end(), c.p, c.p + n);
        c.p += n;
        if (!pending_sysex.empty() && pending_sysex.back() == 0xf7) {
          raw.push_back(RawEvent{Kind::SysEx, tick, track_no, seq++, pending_sysex});
          pending_sysex.clear();
        }
        // else: continued by following F7 packets
      } else if (status == 0xf7) {
        // Either a continuation packet of a multi-packet SysEx, or an
        // "escape" carrying arbitrary bytes (which we do not forward).
        running = 0;
        uint32_t n = c.vlq();
        if (n > c.left()) throw std::runtime_error("SMF: F7 length runs past track end");
        if (!pending_sysex.empty()) {
          pending_sysex.insert(pending_sysex.end(), c.p, c.p + n);
          if (!pending_sysex.empty() && pending_sysex.back() == 0xf7) {
            // Delivered at the time of the packet that completes it.
            raw.push_back(RawEvent{Kind::SysEx, tick, track_no, seq++, pending_sysex});
            pending_sysex.clear();
          }
        }
        c.p += n;
      } else if (status == 0xff) {
        running = 0;
        uint8_t type = c.u8();
        uint32_t n = c.vlq();
        if (n > c.left()) throw std::runtime_error("SMF: meta length runs past track end");
        std::vector<uint8_t> bytes;
        bytes.push_back(type);
        bytes.insert(bytes.end(), c.p, c.p + n);
        c.p += n;
        if (type == 0x51) {
          if (n != 3) throw std::runtime_error("SMF: Set Tempo meta with length != 3");
          uint32_t us = (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) | bytes[3];
          if (us == 0) throw std::runtime_error("SMF: tempo of 0 microseconds per quarter note");
          tempi.push_back(TempoChange{tick, us, track_no, seq});
        } else if (type == 0x2f) {
          eots.emplace_back(tick, track_no);
          ended = true;
        }
        raw.push_back(RawEvent{Kind::Meta, tick, track_no, seq++, std::move(bytes)});
      } else {
        // System common / realtime bytes (F1..F6, F8..FE) are not legal in
        // an SMF track outside an F7 escape; treat as malformed.
        throw std::runtime_error("SMF: unexpected status byte 0x" +
                                 std::to_string(status) + " in track");
      }
    }
    if (!pending_sysex.empty())
      throw std::runtime_error("SMF: unterminated multi-packet SysEx at end of track");
    if (!ended) eots.emplace_back(tick, track_no);   // track without End Of Track meta

    pos += 8 + len;
    track_no++;
  }
  if (track_no != f.ntracks)
    throw std::runtime_error("SMF: header announces " + std::to_string(f.ntracks) +
                             " tracks but " + std::to_string(track_no) + " found");

  // ---- Pass 2: tempo map -> absolute time for every event -------------------
  // Tempo changes from all tracks apply globally (format 1 puts them in track 0
  // by convention, but merging is the safe general rule). Order by tick, then
  // by track/sequence so simultaneous changes resolve deterministically.
  std::stable_sort(tempi.begin(), tempi.end(), [](const TempoChange &a, const TempoChange &b) {
    if (a.tick != b.tick) return a.tick < b.tick;
    if (a.track != b.track) return a.track < b.track;
    return a.seq < b.seq;
  });
  f.n_tempo_changes = tempi.size();

  // time_of(tick): for metrical time, sum over tempo segments of
  //   delta_ticks * us_per_qn  (units: tick-microseconds; divide by ppqn*1e6)
  // For SMPTE time the tempo map is irrelevant: time is tick / (fps*tpf),
  // with 29.97 handled by the 1001/30000 scaling in time_den.
  auto time_of = [&](uint64_t tick) -> uint64_t {
    if (f.smpte) return (f.fps == 29) ? tick * 1001ull : tick;
    uint64_t acc = 0, last_tick = 0;
    uint32_t tempo = 500000;   // SMF default: 120 bpm
    for (const TempoChange &tc : tempi) {
      if (tc.tick >= tick) break;
      acc += (tc.tick - last_tick) * uint64_t(tempo);
      last_tick = tc.tick;
      tempo = tc.us_per_qn;
    }
    acc += (tick - last_tick) * uint64_t(tempo);
    return acc;
  };

  f.events.reserve(raw.size());
  for (RawEvent &r : raw) {
    Event e;
    e.kind = r.kind;
    e.tick = r.tick;
    e.track = r.track;
    e.seq = r.seq;
    e.time_num = time_of(r.tick);
    e.bytes = std::move(r.bytes);
    f.events.push_back(std::move(e));
  }
  std::stable_sort(f.events.begin(), f.events.end(), [](const Event &a, const Event &b) {
    if (a.time_num != b.time_num) return a.time_num < b.time_num;
    if (a.track != b.track) return a.track < b.track;
    return a.seq < b.seq;
  });

  for (auto &eot : eots)
    f.end_of_track_num = std::max(f.end_of_track_num, time_of(eot.first));

  return f;
}

File load(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open MIDI file: " + path);
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return parse(data);
}

uint64_t to_sample(uint64_t num, uint64_t den, uint64_t rate) {
#ifdef __SIZEOF_INT128__
  unsigned __int128 n = static_cast<unsigned __int128>(num) * rate;
  unsigned __int128 d = den;
  return static_cast<uint64_t>((2 * n + d) / (2 * d));   // round half up
#else
  long double s = static_cast<long double>(num) * rate / den;
  return static_cast<uint64_t>(s + 0.5L);
#endif
}

}  // namespace smf
