⚠️ **Research Fork — Not the Upstream Project**

This is a separate fork of [skjelten/emusc](https://github.com/skjelten/emusc) focused on library development, reverse engineering exploration, and AI-assisted analysis of Roland Sound Canvas emulation. **This is not the main upstream EmuSC project.** This fork diverges significantly and will remain independent.

---

## libEmuSC — Headless SC-88 Emulation Library

**libEmuSC** is a C library that implements low-level emulation of the Roland Sound Canvas SC-88 (and related modules). It extracts and reimplements the behavior of the synthesizer's ROM-based voice engine, including oscillator waveforms, TVA (Time Variant Amplifier) envelopes, pan behavior, and pitch control.

This fork was created to explore SC-88 emulation at a deep technical level, with substantial AI assistance in reverse engineering ROM structures and envelope dynamics. The library serves as the research output, with a focus on understanding the original hardware behavior rather than achieving bit-perfect audio reproduction.

The upstream EmuSC project may be better suited if you want a polished, end-user GUI application for SC-55 emulation.

---

## What's Here

- **`libemusc/`** — The core C library implementing:
  - ROM-based voice tables and waveforms
  - SC-88 oscillator and synthesis engine
  - TVA envelope tracking with parameterized stages
  - Pan and pitch control
  - MIDI device interface for 64-note polyphonic playback
  
- **`libemusc/tools/`** — Utility programs:
  - ROM dumping and sample extraction
  - Oscillator testing and analysis
  - Envelope tracing and validation
  
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

## Questions?

This is exploratory research code. If you're looking for:
- **A usable SC-55 emulator:** Try [Nuked SC-55](https://github.com/nukeykt/Nuked-SC55) or [SC-55 Soundfont](https://github.com/Kitrinx/SC55_Soundfont)
- **A polished EmuSC GUI:** See the upstream [EmuSC project](https://github.com/skjelten/emusc)
- **Library integration:** Study `libemusc/` and its public headers in `libemusc/src/`
- **Reverse engineering details:** Check git history and commit messages for research notes
