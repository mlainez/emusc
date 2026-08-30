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

#include "jv1080_rom.h"

#include <cctype>
#include <cmath>
#include <fstream>

namespace EmuSC {


// Read from Roland's service notes, page 15, CIRCUIT DIAGRAM (EXP BASE), where
// each wave ROM socket is drawn as a table of <net WADn> <pin> <ROM pin WAm>.
// ROM address bit m is driven by tone-generator line WAD[ADDR_ORDER[m]].
// Transcription verified against the page rendered at 400 dpi.
const int JV1080Rom::ADDR_ORDER[JV1080Rom::WAVE_ADDR_BITS] = {
  1, 2, 0, 3, 4, 18, 17, 16, 15, 5, 6, 13, 7, 12, 10, 9, 11, 8, 14, 19, 20
};


JV1080Rom::JV1080Rom(std::string romPath)
  : _ok(false)
{
  std::ifstream file(romPath, std::ios::binary | std::ios::in);
  if (!file.is_open()) {
    _error = "Unable to open JV-1080 program ROM: " + romPath;
    return;
  }

  file.seekg(0, std::ios::end);
  std::streamoff len = file.tellg();
  file.seekg(0);

  // The one dump we hold is 1 MB. A different size is not fatal - the tables
  // are located by structure - but it is worth refusing something clearly
  // wrong, such as a wave ROM handed in by mistake.
  if (len < 0x80000 || len > 0x400000) {
    _error = "Unexpected JV-1080 program ROM size";
    return;
  }

  _rom.resize((size_t) len);
  file.read((char *) &_rom[0], len);
  file.close();

  if (!_read_table(WAVEFORM_HINT, _waveforms)) {
    _error = "No waveform table found in JV-1080 program ROM";
    return;
  }

  // Both of these are optional: a firmware revision that lacks one, or that
  // moves it beyond what _find_table() can recover, should not make the ROM
  // unusable for the table that was found.
  _read_table(LOOP_HINT, _loops);
  _read_effects(EFFECT_HINT);
  _read_samples(SAMPLE_HINT);

  _ok = true;
}


JV1080Rom::~JV1080Rom()
{}


// A record's first field is a printable, space-padded name. Requiring a few
// alphanumerics rejects the runs of punctuation that appear inside the panel
// text, which is where a naive scan goes wrong - see the false positives noted
// in docs/jv1080-rom-notes.md.
bool JV1080Rom::_name_like(uint32_t off, int minAlnum) const
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
uint32_t JV1080Rom::_find_table(uint32_t hint) const
{
  if (!_name_like(hint))
    return 0;

  uint32_t base = hint;
  while (base >= RECORD_SIZE && _name_like(base - RECORD_SIZE, 2))
    base -= RECORD_SIZE;

  return base;
}


bool JV1080Rom::_read_table(uint32_t hint, std::vector<Waveform> &out)
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
bool JV1080Rom::_read_effects(uint32_t hint)
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
bool JV1080Rom::_read_samples(uint32_t hint)
{
  const uint32_t WAVE_SPACE = (uint32_t) WAVE_ROMS * WAVE_ROM_SIZE;

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

  if (!valid(hint))
    return false;

  uint32_t base = hint;
  while (base >= (uint32_t) SAMPLE_STRIDE && valid(base - SAMPLE_STRIDE))
    base -= SAMPLE_STRIDE;

  // Tolerate a single unusable slot before giving up: the table has gaps.
  for (uint32_t off = base; ; off += SAMPLE_STRIDE) {
    if (!valid(off)) {
      if (!valid(off + SAMPLE_STRIDE))
        break;
      continue;
    }
    struct Sample smp;
    for (int i = 0; i < 6; i++)
      smp.header[i] = _rom[off + 3 + i];
    smp.start = u24(off + 9);
    smp.loop  = u24(off + 12);
    smp.end   = u24(off + 15);
    _samples.push_back(smp);
  }

  return !_samples.empty();
}


std::vector<int> JV1080Rom::waveform_samples(int waveformIndex) const
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


bool JV1080Rom::load_wave_roms(const std::vector<std::string> &paths)
{
  _waveRoms.clear();

  if ((int) paths.size() != WAVE_ROMS) {
    _error = "JV-1080 needs exactly 4 wave ROMs";
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
uint8_t JV1080Rom::_wave_byte(uint32_t addr) const
{
  size_t rom = addr >> WAVE_ADDR_BITS;
  if (rom >= _waveRoms.size())
    return 0;

  uint32_t logical = addr & ((1u << WAVE_ADDR_BITS) - 1);
  uint32_t physical = 0;
  for (int m = 0; m < WAVE_ADDR_BITS; m++)
    physical |= ((logical >> ADDR_ORDER[m]) & 1u) << m;

  return _waveRoms[rom][physical];
}


std::vector<int16_t> JV1080Rom::decode_sample(int sampleIndex, int dcWindow) const
{
  std::vector<int16_t> pcm;
  if (sampleIndex < 0 || (size_t) sampleIndex >= _samples.size() ||
      !have_wave_roms())
    return pcm;

  const struct Sample &s = _samples[sampleIndex];
  uint32_t count = s.end - s.start;
  if (!count)
    return pcm;

  // Each byte is a signed change, so the waveform is their running sum.
  std::vector<double> x(count);
  double acc = 0.0;
  for (uint32_t i = 0; i < count; i++) {
    acc += (double) (int8_t) _wave_byte(s.start + i);
    x[i] = acc;
  }

  // Without the encoder's reset points the sum drifts. A moving average
  // approximates the drift and is subtracted off. This is a stand-in for the
  // block structure, not the format - see the note in the header.
  if (dcWindow > 1 && (uint32_t) dcWindow < count) {
    std::vector<double> smooth(count);
    double run = 0.0;
    int half = dcWindow / 2;
    for (uint32_t i = 0; i < count; i++) {
      run += x[i];
      if (i >= (uint32_t) dcWindow) run -= x[i - dcWindow];
      uint32_t n = (i < (uint32_t) dcWindow) ? i + 1 : (uint32_t) dcWindow;
      smooth[i] = run / n;
    }
    for (uint32_t i = 0; i < count; i++) {
      uint32_t j = (i + half < count) ? i + half : count - 1;
      x[i] -= smooth[j];
    }
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

}
