/*
 *  This file is part of libEmuSC, a Sound Canvas emulator library
 *
 *  Roland SC-55mkII device profile (firmware 1.01).
 *
 *  Data only. See device_profile.h for what each field means.
 */

#include "../device_profile.h"

namespace EmuSC
{

// Instrument bank offsets in the control ROM. The same eight as the SC-55's,
// repeated here rather than shared so each device file stands on its own.
static const std::vector<uint32_t> SC55MKII_BANKS =
  { 0x10000, 0x1BD00, 0x1DEC0, 0x20000, 0x2BD00, 0x2DEC0, 0x30000, 0x38000 };

static const ProgramRomMap SC55MKII_PROGRAM_MAP = {
  0x3d1e8,      // VelocityCurves
  0x3dd7c,      // KeyMapperIndex
  0x3de8c,      // KeyMapper
  0x3f814       // TVAPanKeyFollow
};

static const CpuRomMap SC55MKII_CPU_MAP = {
  // LFORate was 0x6486, which breaks the 0x100 ladder its neighbours sit on and
  // points at bytes whose values all exceed the rate clamp, so every tone's LFO
  // ran pinned at 20 Hz instead of its own rate. The table at 0x6d86 is monotone,
  // its top entry is the clamp constant itself, and it sits the same distance
  // past the envelope-time table as the SC-55's does (P-0112, P-0113).
  0x1310, 0x650e, 0x653a, 0x6c86, 0x6d86, 0x6e86, 0x6f86, 0x7086,
  0x7186, 0x7246, 0x7270, 0x7286, 0x7386, 0x7488, 0x758a, 0x763a,
  0x7650, 0x7666, 0x7766, 0x77a6, 0x652e, 0x687a, 0x6a84, 0x673a,
  0x6a03, 0x6883, 0x6903, 0x78ee, 0x7aee
};

static const SoundCanvasLayout SC55MKII_LAYOUT = {
  &SC55MKII_BANKS,
  &SC55MKII_PROGRAM_MAP,
  &SC55MKII_CPU_MAP,
  12,           // velocity curves

  0x03c028,     // drum sets run to here, in 1164-byte blocks
  1164,

  0x30000,      // the key mapper offset is relative to this bank

  0x03fff0,     // the demo songs sit late in the ROM
  true,         // and run to its end

  // Two animations laid end to end: the SC-55mkII's at 0x70000 and the
  // SC-155mkII's at 0x71280, each 0x1280 long.
  0x70000,
  0x1280,
  2
};

// A GS banner, with the version and a BCD date held elsewhere.
const RomSignature SC55MKII_SIGNATURE = {
  "SC-55mkII", 0x3d148, 32, "GS-28 VER=2.00  SC              ", 32,
  RomVersionStyle::SeparateBcd, 0xfff0
};

// The SCB-55 is the same machine on a card, and carries no version we can read.
const RomSignature SCB55_SIGNATURE = {
  "SCB-55 (SC-55mkII)", 0x3d148, 32, "GS-28 VER=2.00  LCGS-3 module   ", 32,
  RomVersionStyle::Unknown, 0
};

const DeviceProfile SC55MKII_PROFILE = {
  "SC-55mkII",

  0, 2,        // identified by signature, not by size

  28,           // max polyphony

  // 8.4 dB/ms, measured (PROVENANCE.md P-0080).
  8.4f,

  nullptr,      // not a record-addressed ROM
  &SC55MKII_LAYOUT,

  nullptr, 0,

  // The Sound Canvas accumulates attenuations in a log index domain and does not
  // use this law; left zero so a misuse is obvious rather than plausible. Its
  // envelope table is already in engine ticks, and segments of 8 ticks or fewer
  // snap - the behaviour tva.cc has always had for this generation.
  { 0, 0, 0, 0, 0, 0, 0, 0, 8, 1 }
};

}
