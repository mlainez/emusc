⚠️ **Research Fork — Not the Upstream Project**

This is a separate fork of [skjelten/emusc](https://github.com/skjelten/emusc) focused on library development, reverse engineering exploration, and AI-assisted analysis of Roland Sound Canvas emulation. **This is not the main upstream EmuSC project.** This fork diverges significantly and will remain independent.

---

## emuscd — Headless Roland Sound Canvas Emulation

**emuscd** (the library `libEmuSC` + headless tools) emulates the low-level synthesis behavior of Roland's Sound Canvas family of synthesizers. It extracts and reimplements ROM-based voice engines, including oscillator waveforms, TVA (Time Variant Amplifier) envelopes, effects processing, and MIDI voice allocation.

This fork was created to explore Sound Canvas emulation at a deep technical level, with substantial AI assistance in reverse engineering ROM structures, envelope dynamics, and oscillator characteristics. The library serves as the research output, with a focus on understanding the original hardware behavior rather than achieving bit-perfect reproduction.

---

## Supported Devices

The following Roland Sound Canvas and synthesizer modules are emulated:

| Device | Status | Details |
|--------|--------|---------|
| **SC-55** | Partial | Firmware 1.21; oscillator and TVA working; TVF/reverb incomplete |
| **SC-55mkII** | Partial | Firmware 1.01; enhanced from SC-55; GM mode support |
| **SC-88** | Partial | Full ROM-driven engine with 64-voice polyphony; SC-88's own synthesis path |
| **JV-880** | Partial | Sampled sound module; device profile extracted; synthesis in progress |

**Emulation scope:**
- ✅ ROM-based voice waveforms and tables
- ✅ MIDI channel and note-on/off control
- ✅ Pitch bend, modulation, volume, pan, expression
- ✅ Program change and bank selection
- ✅ TVA envelope (attack, decay, sustain, release)
- ✅ Oscillator frequency and modulation
- ⏳ TVF (filter) envelope curves (in progress)
- ⏳ Reverb and chorus effects (partial)
- ⏳ LFO shape and routing
- ⏳ Velocity response curves (partial)

---

## What's Here

- **`libemusc/`** — The core C library implementing:
  - ROM-based voice tables, waveforms, and device profiles
  - Synthesis engines (SC-55 generation and SC-88)
  - TVA envelope tracking with parameterized stages
  - Pan, pitch, and expression control
  - MIDI device interface for polyphonic playback
  
- **`libemusc/tools/emusc-render`** — Headless MIDI-to-WAV renderer:
  - Parse Standard MIDI Files (SMF format 0 & 1)
  - Deterministic, sample-accurate event scheduling
  - Load SC-55/SC-88 ROM sets automatically
  - Configurable output rate and GM/GS reset mode
  - Reproducible output via fixed random seed
  
- **`libemusc/tools/`** — Analysis and test utilities:
  - ROM dumping and sample extraction
  - Oscillator testing and validation
  - Envelope tracing and curve fitting
  - TVF probe and characterization
  
- **`libemusc/tests/`** — Unit tests for core synthesis components

---

## Building

**Requirements:**
- CMake 3.12+
- A C compiler (GCC, Clang, MSVC)
- Optional: C++ compiler for tests/tools

**Build:**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --build build --target test  # Run tests
```

The library is built as `build/libemusc/libemusc.a` (or `.so`/`.dylib`/`.lib` depending on platform).

---

## About This Fork

This fork exists because exploration of the SC-88 emulation required detailed ROM analysis, measurement of envelope behavior, and reverse engineering of oscillator characteristics. This work was substantially assisted by Claude (an AI assistant), which helped:

- Analyze ROM structures and extract device profiles
- Test and validate TVA envelope curve fitting
- Implement parameterized oscillator stages
- Trace MIDI voice allocation behavior

**AI Involvement:** Following the [kernel.org coding-assistants convention](https://www.kernel.org/doc/html/latest/process/coding-assistants.html), all commits carrying AI assistance include an `Assisted-by: Claude:<model>` trailer. The human author retains full responsibility and is listed as the author of record.

**Why separate from upstream?** The upstream EmuSC project focuses on a polished GUI application for end-users. This fork prioritizes library development, low-level analysis, and exploration driven by reverse engineering. The two projects have different goals and audiences.

---

## Licensing

- **`libemusc/` library:** LGPL-2.1-or-later (from upstream)
- **New code in this fork:** Dedicated to the public domain under CC0 1.0

See `libemusc/COPYING` and `README.md` in that directory for detailed attribution.

---

## Relationship to Upstream

This is **not** affiliated with or endorsed by:
- Roland Corporation (original hardware manufacturer)
- The upstream EmuSC project (https://github.com/skjelten/emusc)

This fork respects the upstream's open-source licenses (LGPL/GPL) and retains proper attribution to original code and authors.

---

## Current Development Focus

This research fork is actively exploring:
- SC-88 immutable ROM graph and parameterized voice engine
- TVA envelope behavior across different devices
- TVF (filter) curve extraction and modeling
- Oscillator stage interpolation and coefficient precision
- MIDI voice allocation and priority behavior
- Effects processing (reverb, chorus, EQ)

---

## Example: Render a MIDI File

Once ROMs are obtained (not included), render a Standard MIDI File to WAV:

```bash
./build/libemusc/tools/emusc-render \
  --romset sc88 \
  --rom-dir /path/to/sc88/roms \
  --rate 44100 \
  input.mid output.wav
```

For SC-55:
```bash
./build/libemusc/tools/emusc-render \
  --romset mk1 \
  --rate 44100 \
  input.mid output.wav
```

---

## Questions?

This is exploratory research code. For questions about:
- **The library API:** Read `libemusc/src/synth.h` (public interface)
- **ROM extraction:** See the corpus and PROVENANCE documentation
- **Reverse engineering:** Check git history and commit messages for detailed analysis notes
- **SC-88 specific:** Look at `libemusc/src/sc88_*.c` files and their corresponding research artifacts
- **Alternatives:** [Nuked SC-55](https://github.com/nukeykt/Nuked-SC55) or [SC-55 Soundfont](https://github.com/Kitrinx/SC55_Soundfont) for other approaches
- **Upstream EmuSC:** See https://github.com/skjelten/emusc for the polished GUI application
