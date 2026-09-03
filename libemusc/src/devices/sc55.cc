/*
 *  This file is part of libEmuSC, a Sound Canvas emulator library
 *
 *  Roland SC-55 device profile (firmware 1.21).
 *
 *  Data only. See device_profile.h for what each field means.
 */

#include "../device_profile.h"

namespace EmuSC
{

// Instrument bank offsets in the control ROM. Shared with the SC-55mkII, and
// repeated in that device's file rather than hoisted: eight numbers are cheaper
// to duplicate than to couple two devices together.
static const std::vector<uint32_t> SC55_BANKS =
  { 0x10000, 0x1BD00, 0x1DEC0, 0x20000, 0x2BD00, 0x2DEC0, 0x30000, 0x38000 };

static const ProgramRomMap SC55_PROGRAM_MAP = {
  0x3d1e8,      // VelocityCurves
  0x3dc72,      // KeyMapperIndex
  0x3dd82,      // KeyMapper
  0             // TVAPanKeyFollow - the SC-55 generation has none
};

static const CpuRomMap SC55_CPU_MAP = {
  0x14c6, 0x679a, 0x67c6, 0x6f12, 0x7012, 0x7112, 0x7212, 0x7312,
  0x7412, 0x74d2, 0x74fc, 0x7512, 0x7612, 0x7714, 0x7816, 0x78c6,
  0x78dc, 0x78f2, 0x79f2, 0x7a32, 0x67ba, 0x6b06, 0x6d10, 0x69c6,
  0x6c8f, 0x6b0f, 0x6b8f, 0x7b7a, 0x7d7a
};

static const SoundCanvasLayout SC55_LAYOUT = {
  &SC55_BANKS,
  &SC55_PROGRAM_MAP,
  &SC55_CPU_MAP,
  10,           // velocity curves

  0x03c028,     // drum sets run to here, in 1164-byte blocks
  1164,

  0x30000,      // the key mapper offset is relative to this bank

  0,            // the demo songs start at the head of the ROM
  false,        // and end where the first instrument bank begins

  0, 0, 0       // no intro animation
};

// "Ver" then a version and a date, in ASCII, in the same block.
const RomSignature SC55_SIGNATURE = {
  "SC-55", 0xf380, 29, "Ver", 3, RomVersionStyle::Inline, 0
};

// The SCC-1 is an SC-55 on an ISA card: same banks, same tables, same limits.
const RomSignature SCC1_SIGNATURE = {
  "SCC-1", 0x3d155, 29, "VER", 3, RomVersionStyle::Inline, 0
};

const DeviceProfile SC55_PROFILE = {
  "SC-55",

  0, 2,        // identified by signature, not by size

  24,           // max polyphony

  // 18.0 dB/ms, measured (PROVENANCE.md P-0080).
  18.0f,

  nullptr,      // not a record-addressed ROM
  &SC55_LAYOUT,

  nullptr, 0,   // its curves come from the CPU ROM map above, not a table list

  // The Sound Canvas accumulates attenuations in a log index domain and does
  // not use this law; left zero so a misuse is obvious rather than plausible.
  // Its envelope table is already in engine ticks, and segments of 8 ticks or
  // fewer snap - the behaviour tva.cc has always had for this generation.
  { 0, 0, 0, 0, 0, 0, 0, 0, 8, 1 },

  LevelLawKind::SoundCanvasLogIndex,

  {
    // The mk1's reverb decays about 1.09x slower than the mkII's at the same
    // reverb time, so it needs its own line (P-0304, TASK-077).
    1.860f, -8.8f, 190,

    // Return level, pre-LPF pair and delay taps. Shared across the family
    // because the reverb DSP is: see ReverbLaw. levelDivisor is not the
    // firmware's return law and ReverbLaw says why it still stands here.
    64.0f,
    ReverbReturnLaw::LevelOverDivisor,
    8, 0x3f, 4,
    0x16, 112
  },

  { false, 0x10, 0x50 },

  // Bank id 2 lands in the second megabyte on this generation.
  { false, 0x100000 }
};

}
