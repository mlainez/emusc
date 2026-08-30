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
#include <fstream>

namespace EmuSC {


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

}
