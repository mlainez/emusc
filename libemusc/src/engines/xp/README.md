# The XP engine family

Roland built the Sound Canvas / JV line on exactly two sound-generator
silicon designs, and the boundary between them doesn't follow the
product names. First-party service-manual parts lists (see
`scdb/docs/sound_chip_families.md` in the research corpus this project is
built on) put every device into one of:

- **Class G** (`GP-4`/`TC6116AF`): SC-55, SC-55mkII, JV-880, JV-80, SCC-1.
  This is `libemusc/src/`'s existing shared C++ pipeline
  (`Part`/`Note`/`Partial`, driven by a `DeviceProfile` per device).
- **Class X** (`XP`, "a family of generations"): JV-1080, SC-88, SC-88VL
  (generation 1); SC-88Pro, SC-8820 (later, two-chip generations). SC-88
  and JV-1080 carry the identical Roland part number - one engine - and
  SC-88 is the readable half of that pair: its firmware is dumped and
  disassembles, JV-1080's mask ROM isn't. Laws measured on one generation
  are not assumed to hold for a later one without their own measurement.

This directory is Class X's home. It exists because SC-88's engine is
architecturally nothing like Class G's: it reads the control ROM itself,
owns every table it needs, and is driven from `synth.cc` through an
opaque `Xp::Device*` rather than through `Part`/`Note`/`Partial`.

## Two devices, two kinds of knowledge

SC-88 came first and the shared code still carries its shape. Its
firmware is dumped and disassembles, so its engine is a **port**: every
table it reads is the ROM's own and every law is firmware-exact.

JV-1080 is the second member here, and it cannot be a port. Its
synthesis engine lives in the SH7034's 64 KB internal mask ROM, which
has never been dumped. What its readable external ROM holds - the
bit-packed preset records behind a 468-entry field descriptor table, the
parameter map, the wave chain, the effect coefficient data - is read
from it and is exact. Everything the voice path does with those values
is a **behavioural model** fitted to laws measured on the owner's own
unit, each carrying its measurement id: a transfer function that
responds the way the machine does, not the arithmetic the machine uses.

**Nothing about JV-1080 in this tree may be cited as firmware-exact**,
and a law measured on one generation is not assumed to hold on another
without its own measurement.

The two share what genuinely is shared and nothing else. One descramble
serves both boards, described by the width of their chips rather than by
their names (`XpDeviceProfile::waveUnitBytes` and the two line counts).
The two ROM layouts live in two readers - `rom.h` for flat fixed-offset
records, `packed_rom.h` for descriptor-packed ones - and both are
generic, taking every address, stride and record layout from the
profile. Neither engine file names a device.

## What JV-1080 does not do yet

The gaps are listed here rather than left to be discovered as silence:

- **`Synth` cannot play it.** `ControlRom` identifies it, `WaveRom`
  keeps its chips and `Xp::Device` opens on it without complaint, but
  the MIDI and note-on path in `device.cc`/`engine.cc`/`renderer.cc` is
  the SC-88's - it resolves a tone through `rom_select_melodic` and
  `rom_open_tone`, which this device has no counterpart for. The result
  is digital silence, not a crash and not noise. Its voice path is
  reached through `engines/xp/devices/jv1080.h` instead.
- **The insert, chorus and reverb effects are bypassed**, not
  approximated. All forty insert types are characterised behaviourally
  in the research, but their DSP topology needs the chip's instruction
  set and is open; a bypass is honest where a guessed topology would
  not be.
- **The two LFOs, the pitch and filter envelopes, FXM, the booster, the
  ten structures, tone delay, the TVA bias and every key-follow field**
  are not modelled. Nor are TVA velocity curves 1 to 6, which are
  measured but not yet transcribed here, so a tone selecting one is
  rendered on curve 0.
- **The rhythm part** is decoded but not played.
