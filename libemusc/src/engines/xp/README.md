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

It plays end to end through `emusc-render` and `emuscd`. This list moved
fast this session and will again, so where a fraction or a count would be
stale within days, it names what to check in the source instead of
asserting a number.

- **The insert effect is dry for any type its own dispatch does not
  name** - `efx_algorithm_refresh` in `jv1080_engine.cc` resets
  `efx_ready` to false and only a matched type sets it back, so an
  unmatched one bypasses rather than approximates, same as before. What
  changed is how much is matched: by display number, STEREO-EQ 1,
  OVERDRIVE 2, DISTORTION 3, PHASER 4, SPECTRUM 5, ENHANCER 6, ROTARY 8,
  COMPRESSOR 9, LIMITER 10, HEXA-CHORUS 11, TREMOLO-CHORUS 12, SPACE-D 13,
  STEREO-CHORUS 14, STEREO-FLANGER 15, STEREO-DELAY 17, MODULATION-DELAY
  18, TRIPLE-TAP-DELAY 19, TIME-CONTROL-DELAY 21, REVERB 24 and
  GATE-REVERB 25 now run real, measured DSP; the rest of the forty do
  not. This roster will itself be behind by the time it is read - the
  `kEfxType*` constants and `efx_algorithm_refresh` just above them in
  `devices/jv1080_engine.cc` are the live list. The output stage is a
  separate gap that does not move with this one: `output.cc` is the
  other device's measured analogue front end, and `device.cc`'s early
  return for a device with its own voice path (`device->voice_ops`)
  never reaches it.
- **The chorus and reverb sends are no longer bypassed** - each is this
  device's own parameter mapping over the shared runtime, read from its
  own tables in `jv1080_engine.cc`, not routed through `device.cc`'s
  generic path. (These are the always-on sends; REVERB and GATE-REVERB
  above are two of the insert effect's forty types and a different
  thing.) What is still open is the modulation matrix's own source list:
  `matrix_source()` answers the CC-backed sources but reads BENDER,
  LFO1, LFO2, VELOCITY, KEYFOLLOW and PLAY-MATE as a flat, unmeasured
  zero - not to be confused with the per-parameter pitch/cutoff/pan/time
  key-follow fields, which are modelled (`XpDeviceProfile::keyFollowTable`,
  `...::timeKeyFollowTable`) and read by every field that names one.
- **The two LFOs, the pitch and filter envelopes, FXM, the booster and
  all ten structures are modelled**, each against its own measurement id
  in `jv1080_voice.cc` and `jv1080_engine.cc`. So is the TVA key bias, and
  so are the A-ENV and F-ENV velocity curves - a record naming curve 1 to
  6 no longer renders on curve 0.
- **The tone delay runs all seven panel modes**, each against its own
  measurement and its own documented residual (the `kDelay*` constants
  and the switch that follows them in `jv1080_engine.cc`) - except
  CLOCK-SYNC and TAP-SYNC, which still start at once because this engine
  keeps no clock to time them against.
- **A ping-pong loop turns as a reflection**, not read forward: the
  sibling device's law was carried over as a labelled cross-device lead
  and then confirmed directly on this device, from where the reflected
  and forward reads disagree on hardware-recorded rhythm-kit keys (ten of
  eleven favour the reflected reading; the eleventh does not resolve
  either way).
- **Performances are read well past their part blocks now** - a loaded
  performance's common block supplies the effect source, type and
  parameters, the reverb and chorus sends, and the voice reserves. The
  part block itself stays narrow: it is twenty decoded bytes and the
  engine still reads four of them by role, keeping the rest so a write is
  not discarded.
- **Voice stealing has its reserve exemption**: a part sitting at or
  under its own reserve is skipped in the first pass, per the measured
  law. One constant in it is not recovered - the threshold a part must
  clear past its reserve before the first pass will touch it, bounded 0
  to 7 by the same measurement, is taken as 0 here and stated as a guess
  rather than fitted quietly.
- **The voice-to-mix scale is not recovered**, only its order; see
  `XpDeviceProfile::voiceMixScale` for what bounds it and why.
