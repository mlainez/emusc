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

// Reader for the Roland JV-1080 program ROM.
//
// SCOPE, AND WHY THIS IS A SEPARATE CLASS FROM ControlRom.
// The JV-1080 is a patch synth, not a Sound Canvas: four tones per patch, its
// own envelope and filter structure, and a patch/performance model that the GS
// part model does not describe. Whether libEmuSC can host that at all is an
// open question, so nothing here touches ControlRom or the SC-55 paths. If the
// answer turns out to be "yes, as a generation", this collapses into that; if
// "no", it is already separate.
//
// WHAT IS ESTABLISHED HERE AND WHAT IS NOT.
// The table offsets, the record stride and the field boundaries below were
// recovered by measurement from the ROM itself (docs/jv1080-rom-notes.md), and
// they reproduce: the names come out clean and the record chain is unbroken.
// The MEANING of two fields is inference from shape and is NOT established -
// they are named `splitPoints` and `sampleIndex` because that is what their
// shape suggests, and both are marked INFERRED below. The oracle does not
// emulate this machine, so no render can currently confirm either reading.
// Treat them as hypotheses that a later reference will confirm or move.

#ifndef __JV1080_ROM_H__
#define __JV1080_ROM_H__

#include <cstdint>
#include <string>
#include <vector>

namespace EmuSC {

class JV1080Rom
{
public:
  // A 60-byte record: 12-byte name, 15-byte curve, 11 x uint16, 11 x 0xff.
  struct Waveform {
    std::string name;               // 12 bytes, space-padded ASCII
    uint8_t     param[16];          // NOT YET DECODED - see note below
    uint16_t    index[11];          // INFERRED: sample indices, big-endian
  };

  // The 16-byte param block is left opaque on purpose. It reads as a run of
  // ascending values padded with 0x7f and closed by 0x00, and the run length
  // differs between records - "Ac Piano1 A" carries ten values, "Ac Piano1 B"
  // nine - which is the shape a variable-length key-split table would have.
  // But not every record starts the run in the same place, so the field
  // boundary inside the block is NOT established and naming its parts would be
  // guessing. It is stored whole until something can confirm the reading.

  explicit JV1080Rom(std::string romPath);
  ~JV1080Rom();

  // Table A: the waveform multisamples (326 records in v1.0 - "Ac Piano1 A"
  // through "E.Bass"). Table B: a second bank of the same geometry, names
  // "loop01" through "Loop 7"; what distinguishes it is not yet known.
  const std::vector<Waveform> &waveforms(void) const { return _waveforms; }
  const std::vector<Waveform> &loops(void) const     { return _loops; }

  // The effects list, which numbers itself in the data ("01:STEREO-EQ" ...)
  // and so is one of the few tables that confirms its own extent.
  const std::vector<std::string> &effects(void) const { return _effects; }

  int  size(void) const { return (int) _rom.size(); }
  bool ok(void) const   { return _ok; }
  const std::string &error(void) const { return _error; }

  static constexpr int RECORD_SIZE = 60;
  static constexpr int NAME_LEN    = 12;

private:
  std::vector<uint8_t> _rom;
  std::vector<Waveform> _waveforms;
  std::vector<Waveform> _loops;
  std::vector<std::string> _effects;
  bool _ok;
  std::string _error;

  // Offsets measured from the one dump we hold. They are starting points, not
  // trusted constants: each is validated, and _find_table() re-locates the
  // table by structure if validation fails, so a different firmware revision
  // does not silently parse garbage the way a hardcoded stride would.
  static constexpr uint32_t WAVEFORM_HINT = 0x71008;
  static constexpr uint32_t LOOP_HINT     = 0x7a808;
  static constexpr uint32_t EFFECT_HINT   = 0x56ac6;
  static constexpr int      EFFECT_STRIDE = 23;

  static constexpr int PARAM_OFF  = NAME_LEN;          // 12
  static constexpr int PARAM_LEN  = 16;
  static constexpr int INDEX_OFF  = NAME_LEN + 16;     // 28
  static constexpr int INDEX_N    = 11;

  bool _name_like(uint32_t off, int minAlnum = 3) const;
  bool _read_table(uint32_t hint, std::vector<Waveform> &out);
  bool _read_effects(uint32_t hint);
  uint32_t _find_table(uint32_t hint) const;
};

}

#endif  // __JV1080_ROM_H__
