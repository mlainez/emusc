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

// Reader for the Roland JV family program ROMs (JV-880, JV-1080).
//
// SCOPE, AND WHY THIS IS A SEPARATE CLASS FROM ControlRom.
// These are patch synths, not Sound Canvases: four tones per patch, their own
// envelope and filter structure, and a patch/performance model that the GS part
// model does not describe. Whether libEmuSC can host that at all is an open
// question, so nothing here touches ControlRom or the SC-55 paths. If the
// answer turns out to be "yes, as a generation", this collapses into that; if
// "no", it is already separate.
//
// THE TWO MACHINES SHARE A LAYOUT, WHICH IS WHY ONE CLASS SERVES BOTH.
// Both keep a 60-byte waveform record table and an 18-byte sample table with
// three 24-bit addresses, both encode wave data the same way, and both put a
// MIDI root key in the same header byte. Only the offsets, the number of wave
// ROMs, and whether the address bus is permuted differ - so those are a per
// model table (see Layout) rather than a second class.
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

#ifndef __JV_ROM_H__
#define __JV_ROM_H__

#include <cstdint>
#include <string>
#include <vector>

namespace EmuSC {

class JVRom
{
public:
  enum class Model { Unknown, JV880, JV1080 };

  // What differs between the machines. Everything else is shared.
  struct Layout {
    Model       model;
    const char *name;
    size_t      promSize;      // program ROM size, used to pick a candidate
    uint32_t    waveformHint;  // somewhere inside the 60-byte record table
    uint32_t    loopHint;      // second bank of the same record type, 0 if none
    uint32_t    effectHint;    // self-numbering "NN:NAME" table, 0 if none
    uint32_t    sampleHint;    // somewhere inside the 18-byte sample table
    int         waveRoms;      // how many wave ROM devices the space spans
    bool        permuteAddress;// JV-1080 permutes; JV-880 dumps read straight
  };

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

  // Detects the machine from the program ROM: its size narrows the candidates
  // and the tables must then actually parse, so a wrong guess fails loudly
  // rather than reading garbage at plausible-looking offsets.
  explicit JVRom(std::string romPath);
  ~JVRom();

  Model model(void) const { return _layout.model; }
  const char *model_name(void) const { return _layout.name; }

  // Table A: the waveform multisamples (326 records in v1.0 - "Ac Piano1 A"
  // through "E.Bass"). Table B: a second bank of the same geometry, names
  // "loop01" through "Loop 7"; what distinguishes it is not yet known.
  const std::vector<Waveform> &waveforms(void) const { return _waveforms; }
  const std::vector<Waveform> &loops(void) const     { return _loops; }

  // The effects list, which numbers itself in the data ("01:STEREO-EQ" ...)
  // and so is one of the few tables that confirms its own extent.
  const std::vector<std::string> &effects(void) const { return _effects; }

  // One entry of the sample table: where a multisample lives in the wave ROMs.
  // Base 0x075c7a, 18 bytes per entry, 662 valid entries in v1.0 - all found by
  // measuring the ROM (docs/jv1080-rom-notes.md), not from any description of
  // the machine. The three addresses always satisfy start <= loop <= end and
  // fall inside the 8 MB wave space, which is what identifies the record.
  struct Sample {
    uint32_t start;                 // 24-bit, big-endian, in the 8 MB space
    uint32_t loop;                  // loop point, absolute
    uint32_t end;                   // one past the last byte
    uint8_t  header[6];             // pitch, rate and mode - NOT YET DECODED
  };

  const std::vector<Sample> &samples(void) const { return _samples; }

  // The eleven indices ending each waveform record point into samples(), and
  // 0xFFFF marks an unused slot. This returns only the used ones.
  std::vector<int> waveform_samples(int waveformIndex) const;

  // Load the four wave ROMs, in the order they occupy the 8 MB address space.
  // Returns false and sets error() if any file is missing or the wrong size.
  bool load_wave_roms(const std::vector<std::string> &paths);
  bool have_wave_roms(void) const { return (int) _waveRoms.size() == _layout.waveRoms; }

  // Decode one sample to signed 16-bit PCM.
  //
  // TWO STEPS, AND ONLY THE FIRST IS FULLY UNDERSTOOD.
  //
  // The address permutation is read from Roland's own service notes, page 15,
  // where each wave ROM socket is drawn as a table mapping the tone generator's
  // WAD lines to the ROM's WA pins (docs/service-notes/jv1080.md, F-1). It is
  // verified: the diagram and the data agree.
  //
  // The samples are delta-coded - each byte is a change, not a value - which
  // was established by our own measurement: integrating the byte stream lifts
  // lag-1 autocorrelation from +0.008 to +0.998 and yields tonal audio (F-5).
  // Each delta is scaled by a per-block shift exponent before accumulating, the
  // same scheme this library already uses for the SC-55 (wave_rom.cc).
  // What is NOT known is where the encoder resets its integrator. Without that,
  // the running sum drifts, and `dcWindow` suppresses the drift with a moving
  // average instead. That is a WORKAROUND, not the format: it is why our decode
  // is slightly less clean than it should be, and it will be replaced when the
  // block structure is found.
  //
  // PITCH: byte 0 of the entry header is a MIDI root key (100% of values fall
  // in 0..127 across the 201 multisample waveforms), and the wave data is 32 kHz
  // - the same rate this library runs internally. Together those give pitch:
  // play at 2^((note - root)/12). Bytes 1-2 remain unidentified; they are NOT
  // the rate.
  std::vector<int16_t> decode_sample(int sampleIndex, int dcWindow = 4096) const;

  // Play one waveform at a MIDI note, for `seconds`, at `outRate`.
  //
  // This is a MINIMAL voice and is honest about it: it picks the multisample
  // whose root key is nearest the note, resamples it linearly, and loops
  // between loop and end for as long as asked. There is no envelope, no filter,
  // no LFO and no patch structure - those belong to a synth, and whether
  // libEmuSC can host this family's synth at all is still open. What this does
  // give is the first actual SOUND out of these ROMs at a chosen pitch, which
  // is what a single-note comparison against hardware would need.
  //
  // Pitch comes from the entry's root key and a 32 kHz wave rate: the sample is
  // stepped at 2^((note - root)/12) * WAVE_RATE / outRate per output frame.
  std::vector<int16_t> render_note(int waveformIndex, int note,
                                   double seconds = 2.0,
                                   int outRate = 32000) const;

  static constexpr int WAVE_RATE = 32000;

  int wave_roms(void) const { return _layout.waveRoms; }

  static constexpr int MAX_WAVE_ROMS = 4;
  static constexpr int WAVE_ROM_SIZE = 2 * 1024 * 1024;
  static constexpr int WAVE_ADDR_BITS = 21;

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
  std::vector<Sample> _samples;
  std::vector<std::vector<uint8_t>> _waveRoms;
  bool _ok;
  std::string _error;

  // Offsets measured from the one dump we hold. They are starting points, not
  // trusted constants: each is validated, and _find_table() re-locates the
  // table by structure if validation fails, so a different firmware revision
  // does not silently parse garbage the way a hardcoded stride would.
  static constexpr int      EFFECT_STRIDE = 23;
  static constexpr int      SAMPLE_STRIDE = 18;

  static const Layout LAYOUTS[];
  static const int    LAYOUT_COUNT;
  Layout _layout;

  // Service notes p.15: ROM address bit m is driven by tone-generator line
  // WAD[ADDR_ORDER[m]]. Roland's own diagram of Roland's own board.
  static const int ADDR_ORDER[WAVE_ADDR_BITS];

  static constexpr int PARAM_OFF  = NAME_LEN;          // 12
  static constexpr int PARAM_LEN  = 16;
  static constexpr int INDEX_OFF  = NAME_LEN + 16;     // 28
  static constexpr int INDEX_N    = 11;

  bool _name_like(uint32_t off, int minAlnum = 3) const;
  bool _read_table(uint32_t hint, std::vector<Waveform> &out);
  bool _read_effects(uint32_t hint);
  bool _read_samples(uint32_t hint);
  uint8_t _wave_byte(uint32_t addr) const;
  uint32_t _find_table(uint32_t hint) const;
};

}

#endif  // __JV_ROM_H__
