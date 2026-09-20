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
opaque `struct sc88_device*` rather than through `Part`/`Note`/`Partial`.

**Why it's SC-88-shaped today:** SC-88 is currently the only Class-X
member with usable research in this project. That is a fact about where
the research stands, not a design decision - see
`scdb/devices/jv1080/12_implementation/implementation_plan.md` for
JV-1080's own, substantial but not yet complete, research and
implementation plan. Nothing here is built ahead of that research
landing. If and when it does, expect a sibling implementation in this
directory rather than a shared class hierarchy: JV-1080's planned
approach is a behavioral model fit to measured hardware transfer
functions, not a port of disassembled firmware the way SC-88's is, even
though both run the same chip.
