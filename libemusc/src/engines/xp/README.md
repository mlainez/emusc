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

## Two voice paths, one device layer

What the two cannot share is the voice. One device's voice state IS its
chip's register and RAM layout, read out of its own firmware, and its
scheduler is that firmware's control period (`engine.h`, `renderer.h`).
The other's is a behavioural model with no control period, no amplitude
register and no ROM table to read (`devices/jv1080_engine.cc`). A shared
per-voice struct would be a lie about both.

So a device injects its own voice engine through
`XpDeviceProfile::voiceEngine`, and `device.cc` - the top-level MIDI
device, its channel state and the output mixing, all genuinely shared -
calls through that table. It asks whether a profile *has* one, never
which device it is. A profile leaving it null gets the firmware port,
which is where this family started. `sysexModelId` and
`sysexAddressBytes` are injected the same way, because the two devices
differ there too: model `42` with a three-byte parameter address against
model `6a` with a four-byte one.

## What JV-1080 does not do yet

It plays end to end through `emusc-render` and `emuscd`. The gaps are
listed here rather than left to be discovered as silence:

- **The insert, chorus and reverb effects are bypassed**, not
  approximated, and so is the output stage - `output.cc` is the other
  device's measured analogue front end, not this one's. All forty insert
  types are characterised behaviourally in the research, but their DSP
  topology needs the chip's instruction set and is open; a bypass is
  honest where a guessed topology would not be.
- **The two LFOs, the pitch and filter envelopes, FXM, the booster, the
  ten structures, tone delay, the TVA bias and every key-follow field**
  are not modelled. Nor are TVA velocity curves 1 to 6, which are
  measured but not yet transcribed here, so a tone selecting one is
  rendered on curve 0. The filter envelope is the one with a measured
  spectral cost: against a hardware take of the first factory song, this
  renders 1.8 % of its energy between 2 and 8 kHz where the machine puts
  35.5 % there, and a static cutoff where the machine sweeps one is the
  obvious candidate.
- **A ping-pong loop is read forward.** The sibling device's own measured
  answer is in `wave.cc`: the turn in a differential format is a
  reflection, not a time reversal, and reading such a loop forward jumps
  the phase once per traversal. Whether this device's loop-type 1 means
  the same thing is an open question in the research, so the carry-over
  is not made. It falls on this device's cymbals - Crash, Ride, Ride
  Bell, China Cym and the open hi-hat are all loop type 1.
- **Performances are read only for their part blocks** - receive channel,
  level, pan and key shift. A part's patch comes from a program change or
  from the device's own temporary-patch parameter writes, which is what
  its factory demo songs use.
- **Voice stealing has no reserve.** The order is the measured one,
  oldest first; the measured exemption for a part still inside its voice
  reserve needs a performance loaded and has nothing to act on yet.
- **The voice-to-mix scale is not recovered**, only its order; see
  `XpDeviceProfile::voiceMixScale` for what bounds it and why.
