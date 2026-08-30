/*
 *  This file is part of libEmuSC, a Sound Canvas emulator library
 *  Copyright (C) 2026  Marc Lainez
 *
 *  New file, not derived from any existing libEmuSC source. Licensed
 *  under the LGPL as the rest of the library is, so that it links with
 *  it; the copyright is the author's own, not upstream's.
 *
 *  libEmuSC is free software: you can redistribute it and/or modify it
 *  under the terms of the GNU Lesser General Public License as published
 *  by the Free Software Foundation, either version 3 of the License, or (at
 *  your option) any later version.
 *
 *  libEmuSC is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 *  License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with libEmuSC. If not, see <https://www.gnu.org/licenses/>.
 */

#include "jv_rom.h"

#include <cctype>
#include <string>
#include <cmath>
#include <fstream>

namespace EmuSC {


// Read from Roland's service notes, page 15, CIRCUIT DIAGRAM (EXP BASE), where
// each wave ROM socket is drawn as a table of <net WADn> <pin> <ROM pin WAm>.
// ROM address bit m is driven by tone-generator line WAD[ADDR_ORDER[m]].
// Transcription verified against the page rendered at 400 dpi.
const int JVRom::ADDR_ORDER[JVRom::WAVE_ADDR_BITS] = {
  1, 2, 0, 3, 4, 18, 17, 16, 15, 5, 6, 13, 7, 12, 10, 9, 11, 8, 14, 19, 20
};


// Both machines were mapped by measuring their own ROMs. They share every
// structure and differ only in these numbers.
//
// The JV-880's dumps read straight: identity ordering scores +0.9993 against
// +0.9990 for the JV-1080 permutation. That margin is small, and it is recorded
// as measured rather than claimed as settled - the JV-880's own service notes
// have not been consulted, and if they show a permutation this is where it goes.
const JVRom::Layout JVRom::LAYOUTS[] = {
  { Model::JV1080, "JV-1080", 1024 * 1024, 0x71008, 0x7a808, 0x56ac6, 0x075c7a, 4, true  },
  { Model::JV880,  "JV-880",   256 * 1024, 0x000004, 0,      0,       0x0028c4, 2, false },
};
const int JVRom::LAYOUT_COUNT = (int) (sizeof(LAYOUTS) / sizeof(LAYOUTS[0]));


JVRom::JVRom(std::string romPath)
  : _ok(false)
{
  // Definite even if detection fails, so model_name() is never garbage.
  _layout = { Model::Unknown, "unknown", 0, 0, 0, 0, 0, 0, false };

  std::ifstream file(romPath, std::ios::binary | std::ios::in);
  if (!file.is_open()) {
    _error = "Unable to open JV-1080 program ROM: " + romPath;
    return;
  }

  file.seekg(0, std::ios::end);
  std::streamoff len = file.tellg();
  file.seekg(0);

  _rom.resize((size_t) len);
  file.read((char *) &_rom[0], len);
  file.close();

  // Try the layout whose program ROM size matches first, then the others: size
  // narrows the guess but does not prove it, and a layout is only accepted once
  // its waveform table actually parses. That way a wrong guess fails loudly
  // instead of reading garbage at a plausible-looking offset.
  for (int pass = 0; pass < 2 && !_ok; pass++) {
    for (int i = 0; i < LAYOUT_COUNT; i++) {
      const Layout &L = LAYOUTS[i];
      bool sizeMatches = ((size_t) len == L.promSize);
      if (sizeMatches != (pass == 0))
        continue;

      _waveforms.clear();
      if (!_read_table(L.waveformHint, _waveforms))
        continue;

      _layout = L;
      _ok = true;
      break;
    }
  }

  if (!_ok) {
    _error = "No JV waveform table found in this program ROM";
    return;
  }

  // Optional: a machine without one, or a revision that moves it beyond what
  // _find_table() recovers, should not make the ROM unusable for what was found.
  if (_layout.loopHint)   _read_table(_layout.loopHint, _loops);
  if (_layout.effectHint) _read_effects(_layout.effectHint);
  _read_samples(_layout.sampleHint);
}


JVRom::~JVRom()
{}


// A record's first field is a printable, space-padded name. Requiring a few
// alphanumerics rejects the runs of punctuation that appear inside the panel
// text, which is where a naive scan goes wrong - see the false positives noted
// in docs/jv1080-rom-notes.md.
bool JVRom::_name_like(uint32_t off, int minAlnum) const
{
  if ((size_t) off + NAME_LEN > _rom.size())
    return false;

  int alnum = 0;
  for (int i = 0; i < NAME_LEN; i++) {
    uint8_t c = _rom[off + i];
    if (c < 0x20 || c > 0x7e)
      return false;
    if (isalnum(c))
      alnum++;
  }

  return alnum >= minAlnum;
}


// Walk backwards from any record inside the table to find its first record.
// The hint is a measurement from one dump, so it is treated as "somewhere in
// the table" rather than "the start of the table".
uint32_t JVRom::_find_table(uint32_t hint) const
{
  if (!_name_like(hint))
    return 0;

  uint32_t base = hint;
  while (base >= RECORD_SIZE && _name_like(base - RECORD_SIZE, 2))
    base -= RECORD_SIZE;

  return base;
}


bool JVRom::_read_table(uint32_t hint, std::vector<Waveform> &out)
{
  uint32_t base = _find_table(hint);
  if (!base)
    return false;

  for (uint32_t off = base; _name_like(off, 2); off += RECORD_SIZE) {
    struct Waveform w;

    w.name.assign((const char *) &_rom[off], NAME_LEN);
    while (!w.name.empty() && w.name.back() == ' ')   // names are space-padded
      w.name.pop_back();

    for (int i = 0; i < PARAM_LEN; i++)
      w.param[i] = _rom[off + PARAM_OFF + i];

    // Big-endian at +28. The offset is not a guess: read there, the field
    // comes out as the run 0,1,2,...,10, which is what a sequential index
    // list should look like. Read one byte earlier - the first thing this
    // parser did - it comes out as 0,0,256,512,768, and those round numbers
    // are what gave the error away.
    for (int i = 0; i < INDEX_N; i++) {
      uint32_t p = off + INDEX_OFF + i * 2;
      w.index[i] = (uint16_t) ((_rom[p] << 8) | _rom[p + 1]);
    }

    out.push_back(w);
  }

  return !out.empty();
}


// The effects table numbers itself - "01:STEREO-EQ", "02:OVERDRIVE" - so it is
// read by following that numbering rather than by trusting a record count.
bool JVRom::_read_effects(uint32_t hint)
{
  for (uint32_t off = hint; (size_t) off + EFFECT_STRIDE <= _rom.size();
       off += EFFECT_STRIDE) {
    if (!isdigit(_rom[off]) || !isdigit(_rom[off + 1]) || _rom[off + 2] != ':')
      break;

    std::string e((const char *) &_rom[off], EFFECT_STRIDE);
    while (!e.empty() && e.back() == ' ')
      e.pop_back();

    _effects.push_back(e);
  }

  return !_effects.empty();
}



// The sample table sits between the waveform table and the loop table. An entry
// is 18 bytes: three of flags, a six-byte header that is not yet decoded, then
// start, loop and end as 24-bit big-endian addresses into the 8 MB wave space.
// An entry is recognised by its own consistency - start <= loop <= end, in
// range, and a plausible length - rather than by a count, because the table
// does not fill the space available to it.
bool JVRom::_read_samples(uint32_t hint)
{
  const uint32_t WAVE_SPACE = (uint32_t) _layout.waveRoms * WAVE_ROM_SIZE;

  auto u24 = [this](uint32_t o) -> uint32_t {
    if ((size_t) o + 3 > _rom.size()) return 0;
    return ((uint32_t) _rom[o] << 16) | ((uint32_t) _rom[o + 1] << 8) | _rom[o + 2];
  };
  auto valid = [&](uint32_t o) -> bool {
    if ((size_t) o + SAMPLE_STRIDE > _rom.size()) return false;
    uint32_t s = u24(o + 9), l = u24(o + 12), e = u24(o + 15);
    return s > 0 && s <= l && l <= e && e < WAVE_SPACE &&
           (e - s) >= 32 && (e - s) < 0x40000;
  };

  // Find the table by looking for a RUN of consecutive well-formed entries, not
  // a single one: isolated triples elsewhere in the ROM can pass the test by
  // chance, and starting on one would shift every index that follows.
  const int RUN = 8;
  uint32_t base = 0;
  bool found = false;
  for (uint32_t o = (hint > 0x400 ? hint - 0x400 : 0);
       o + RUN * SAMPLE_STRIDE < _rom.size() && o < hint + 0x400; o++) {
    bool all = true;
    for (int k = 0; k < RUN && all; k++)
      all = valid(o + k * SAMPLE_STRIDE);
    if (all) { base = o; found = true; break; }
  }
  if (!found)
    return false;
  while (base >= (uint32_t) SAMPLE_STRIDE && valid(base - SAMPLE_STRIDE))
    base -= SAMPLE_STRIDE;

  // Store EVERY slot in order, valid or not. The waveform records index this
  // table positionally, so compacting it - skipping the gaps - would silently
  // renumber everything after the first gap. An unusable slot is kept with a
  // zero length and rejected at decode time instead.
  int miss = 0;
  for (uint32_t off = base; (size_t) off + SAMPLE_STRIDE <= _rom.size();
       off += SAMPLE_STRIDE) {
    struct Sample smp;
    // The header sits in the NEXT slot, not this one: a record runs
    // [start][loop][end][3 flags][6 header], so reading the header at off+3
    // pairs it with the previous record's addresses. Measured: pairing the
    // header with entry N+1's addresses gives a median pitch offset of +0.05
    // semitones across 578 samples, against +3.19 for entry N.
    for (int i = 0; i < 6; i++) {
      uint32_t ho = off + SAMPLE_STRIDE + 3 + i;
      smp.header[i] = (ho < _rom.size()) ? _rom[ho] : 0;
    }
    if (valid(off)) {
      smp.start = u24(off + 9);
      smp.loop  = u24(off + 12);
      smp.end   = u24(off + 15);
      miss = 0;
    } else {
      smp.start = smp.loop = smp.end = 0;      // placeholder, keeps alignment
      if (++miss > 24) {                        // a long dead stretch ends it
        _samples.resize(_samples.size() > (size_t) miss - 1
                        ? _samples.size() - (miss - 1) : 0);
        break;
      }
    }
    _samples.push_back(smp);
  }

  return !_samples.empty();
}


std::vector<int> JVRom::waveform_samples(int waveformIndex) const
{
  std::vector<int> out;
  if (waveformIndex < 0 || (size_t) waveformIndex >= _waveforms.size())
    return out;

  for (int i = 0; i < 11; i++) {
    uint16_t v = _waveforms[waveformIndex].index[i];
    if (v == 0xFFFF)                       // the unused marker
      continue;
    if ((size_t) v < _samples.size())
      out.push_back((int) v);
  }

  return out;
}


bool JVRom::load_wave_roms(const std::vector<std::string> &paths)
{
  _waveRoms.clear();

  if ((int) paths.size() != _layout.waveRoms) {
    _error = std::string(_layout.name) + " needs exactly " +
             std::to_string(_layout.waveRoms) + " wave ROMs";
    return false;
  }

  for (const std::string &p : paths) {
    std::ifstream f(p, std::ios::binary | std::ios::in);
    if (!f.is_open()) {
      _error = "Unable to open wave ROM: " + p;
      _waveRoms.clear();
      return false;
    }
    f.seekg(0, std::ios::end);
    std::streamoff len = f.tellg();
    f.seekg(0);
    if (len != WAVE_ROM_SIZE) {
      _error = "Wave ROM has unexpected size: " + p;
      _waveRoms.clear();
      return false;
    }
    std::vector<uint8_t> data((size_t) len);
    f.read((char *) &data[0], len);
    _waveRoms.push_back(std::move(data));
  }

  return true;
}


// One byte of the flat 8 MB wave space. The top two bits of the address pick
// the device; the remaining 21 are what the tone generator drives, and they
// reach the ROM's pins through the permutation of ADDR_ORDER.
uint8_t JVRom::_wave_byte(uint32_t addr) const
{
  size_t rom = addr >> WAVE_ADDR_BITS;
  if (rom >= _waveRoms.size())
    return 0;

  uint32_t logical = addr & ((1u << WAVE_ADDR_BITS) - 1);
  if (!_layout.permuteAddress)
    return _waveRoms[rom][logical];

  uint32_t physical = 0;
  for (int m = 0; m < WAVE_ADDR_BITS; m++)
    physical |= ((logical >> ADDR_ORDER[m]) & 1u) << m;

  return _waveRoms[rom][physical];
}


std::vector<int16_t> JVRom::decode_sample(int sampleIndex, int dcWindow) const
{
  std::vector<int16_t> pcm;
  if (sampleIndex < 0 || (size_t) sampleIndex >= _samples.size() ||
      !have_wave_roms())
    return pcm;

  const struct Sample &s = _samples[sampleIndex];
  uint32_t count = s.end - s.start;
  if (!count)
    return pcm;

  // Each byte is a signed change scaled by a per-block shift exponent, and the
  // waveform is the running sum of those. The exponent is a nibble stored at
  // (address >> 5), chosen by bit 4 of the address, and the result is shifted
  // left 14 - which is exactly the scheme this library already implements for
  // the SC-55 in wave_rom.cc. The JV-1080 uses the same encoding, so this is
  // libEmuSC's own algorithm applied to another machine, not a new one.
  //
  // Leaving the exponent out still yields a correlated waveform - lag-1 +0.998
  // either way, because a per-block gain does not change correlation - but the
  // amplitude is wrong by orders of magnitude and so is the shape. Measured on
  // waverom1: peak 1.3e4 without it against 1.1e8 with it.
  std::vector<double> x(count);
  double acc = 0.0;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t addr = s.start + i;
    size_t rom = addr >> WAVE_ADDR_BITS;
    if (rom >= _waveRoms.size()) break;

    uint32_t logical = addr & ((1u << WAVE_ADDR_BITS) - 1);
    uint32_t phys = logical;
    if (_layout.permuteAddress) {
      phys = 0;
      for (int m = 0; m < WAVE_ADDR_BITS; m++)
        phys |= ((logical >> ADDR_ORDER[m]) & 1u) << m;
    }

    int8_t   data    = (int8_t) _waveRoms[rom][phys];
    uint32_t shAddr  = ((phys & 0xFFFFF) >> 5) | (phys & 0xF00000);
    uint8_t  shByte  = _waveRoms[rom][shAddr];
    uint8_t  shift   = (phys & 0x10) ? (shByte >> 4) : (shByte & 0x0F);

    acc += (double) (((int32_t) data << shift) << 14);
    x[i] = acc;
  }

  double peak = 0.0;
  for (uint32_t i = 0; i < count; i++)
    if (std::fabs(x[i]) > peak) peak = std::fabs(x[i]);
  if (peak <= 0.0)
    return pcm;

  pcm.resize(count);
  for (uint32_t i = 0; i < count; i++)
    pcm[i] = (int16_t) (x[i] / peak * 30000.0);

  return pcm;
}



// Pick the multisample whose root key is closest to the note being played. A
// real JV chooses by key range; root-key proximity is the same choice for a
// well-formed multisample set and needs no field we have not identified.
std::vector<int16_t> JVRom::render_note(int waveformIndex, int note,
                                        double seconds, int outRate) const
{
  std::vector<int16_t> out;
  std::vector<int> idx = waveform_samples(waveformIndex);
  if (idx.empty() || !have_wave_roms() || outRate <= 0)
    return out;

  int best = -1, bestDist = 1 << 30;
  for (size_t k = 0; k < idx.size(); k++) {
    const struct Sample &s = _samples[idx[k]];
    if (s.end <= s.start)                       // a table gap
      continue;
    int dist = s.header[0] - note;
    if (dist < 0) dist = -dist;
    if (dist < bestDist) { bestDist = dist; best = idx[k]; }
  }
  if (best < 0)
    return out;

  std::vector<int16_t> pcm = decode_sample(best);
  if (pcm.size() < 2)
    return out;

  const struct Sample &s = _samples[best];
  double step = std::pow(2.0, (note - (int) s.header[0]) / 12.0) *
                ((double) WAVE_RATE / (double) outRate);

  // Loop bounds, in frames from the sample's start.
  double loopStart = (double) (s.loop - s.start);
  double loopEnd   = (double) pcm.size() - 1.0;
  bool   loops     = loopEnd - loopStart > 8.0;

  size_t frames = (size_t) (seconds * outRate);
  out.reserve(frames);

  double pos = 0.0;
  for (size_t i = 0; i < frames; i++) {
    if (pos >= loopEnd) {
      if (!loops) break;                        // one-shot: stop at the end
      pos = loopStart + std::fmod(pos - loopStart, loopEnd - loopStart);
    }
    size_t j = (size_t) pos;
    double f = pos - (double) j;
    double a = pcm[j];
    double b = pcm[j + 1 < pcm.size() ? j + 1 : j];
    out.push_back((int16_t) (a + (b - a) * f));
    pos += step;
  }

  return out;
}

}
