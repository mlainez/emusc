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

## Expectations

This is active reverse-engineering research, not a finished emulator, and fidelity
varies by device and by subsystem as the underlying ROM analysis progresses. A
static feature checklist goes stale faster than the code does and can't capture
which gaps apply to which device, so there isn't one here: read `libemusc/src/`
or recent commit messages for what's actually implemented right now.

---

## ROM Files

ROMs are never distributed with this project — you must supply your own dumps from hardware you own. Once you obtain ROM files, place them in a directory and pass that directory to either `emusc-render` or `emuscd`.

### Naming convention

All four supported devices use the same unified naming scheme:

- **Control/program ROM:** `<device>_control.bin` (required for all devices)
- **CPU ROM:** `<device>_cpu.bin` (required only for SC-55 and SC-55mkII)
- **Wave/PCM ROMs:** `<device>_waverom1.bin`, `<device>_waverom2.bin`, etc., in bank order (required for all devices)

`<device>` is one of: `sc55`, `sc55mkii`, `sc88`, `jv880`.

Both `emusc-render` (via `--device X --rom-dir DIR`) and `emuscd` (via `--device X` with `$EMUSCD_ROM_DIR`) resolve ROM files using this same convention.

### Verified ROM dumps

You can verify your ROM dumps match known-good versions by checking their SHA1 and MD5 hashes:

#### SC-55
| File | Size | SHA1 | MD5 |
|---|---|---|---|
| `sc55_cpu.bin` | 32768 | `dd01ec54027751c2f2f2e47bbb7a0bf3d1ca8ae2` | `462cb3a2ce9e54f4e54a52f643931ffd` |
| `sc55_control.bin` | 262144 | `9c17f85e784dc1549ac1f98d457b353393331f6b` | `6b61186953b50d900e430ae6a996bda7` |
| `sc55_waverom1.bin` | 1048576 | `8cc3c0d7ec0993df81d4ca1970e01a4b0d8d3775` | `5f40d5297f47358ddea43b8d225874cc` |
| `sc55_waverom2.bin` | 1048576 | `80e6eb130c18c09955551563f78906163c55cc11` | `f78fb079a7d8c5f1eb04e0fd5aedbc9e` |
| `sc55_waverom3.bin` | 1048576 | `7454b817778179806f3f9d1985b3a2ef67ace76f` | `53c013d4bad0337385cfda2631e5b78e` |

#### SC-55mkII
| File | Size | SHA1 | MD5 |
|---|---|---|---|
| `sc55mkii_cpu.bin` | 32768 | `b91bb1d9dccffe831b7cfde7800a3fe32b2fbda6` | `4ca058f7db05f51e97bb30a162e9610a` |
| `sc55mkii_control.bin` | 524288 | `078cb5feea05e80bb9a1bb857a2163ee434fd053` | `63b24c7193ce34afefce9cec32ac39f0` |
| `sc55mkii_waverom1.bin` | 2097152 | `96708cb21381c2fd03de4babbf7aea301c7594a6` | `30df645acf7f1b621d5f2891ea53e00b` |
| `sc55mkii_waverom2.bin` | 1048576 | `4d91cdeaed048d653dbf846a221003c3a3f08279` | `f86e0433a11707a048a8a79e8d98a3be` |

#### SC-88 (no separate CPU ROM)
| File | Size | SHA1 | MD5 |
|---|---|---|---|
| `sc88_control.bin` | 524288 | `3bc9a68703bd09459283b6b45d01d08feaffb744` | `0ac771782ea58a53af590ebdf140d517` |
| `sc88_waverom1.bin` | 2097152 | `860dcd9804ded4cd46aab38bfc3764a1ad7a6b65` | `6a92b7de3ac7b8205d29ec4497644beb` |
| `sc88_waverom2.bin` | 2097152 | `ac95c26c46c40aacb944f5d45634c93bee9e6d90` | `d98f4b255d3a7dc830d92c71c25ce2eb` |
| `sc88_waverom3.bin` | 2097152 | `d88bf13c3d74097991b783295d95ccfae2c9282d` | `c05b103d4db110b3962431173cc72967` |
| `sc88_waverom4.bin` | 2097152 | `03e70ae2efd190f41af24be01b1abaa84bfa93d9` | `bccc26c34cac0d5509e8efb645043b5b` |

#### JV-880 (no separate CPU ROM used by this project)
| File | Size | SHA1 | MD5 |
|---|---|---|---|
| `jv880_control.bin` | 262144 | `282116e8e8471053cf159d22675931592b7f7c8f` | `06d10ee0657359e030e2cb0e5b1a4a20` |
| `jv880_waverom1.bin` | 2097152 | `37e28498351fb502f6d43398d288a026c02b446d` | `da2349eee9a070af536479ddb2a6b259` |
| `jv880_waverom2.bin` | 2097152 | `963ce75b6668dab377d3a2fd895630a745491be5` | `357f717bba6ea84028ec53d6d35c2d6d` |

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

**Requirements for Linux:**
- CMake 3.12+
- GCC or Clang
- ALSA development libraries (for `emuscd`)

**Requirements for Windows (cross-compilation from Linux):**
- `mingw-w64` cross-compiler packages:
  - 64-bit: `gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64`
  - 32-bit: `gcc-mingw-w64-i686 g++-mingw-w64-i686`
- CMake 3.12+

### Linux native build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --build build --target test  # Run tests
```

The library is built as `build/libemusc/src/libemusc_static.a` (static) and `libemusc.so` (shared), and tools as `build/libemusc/tools/emusc-render` and `build/emuscd/emuscd`.

### Windows binaries (cross-compiled from Linux)

**64-bit:**
```bash
cmake -B build-win64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-win64 --target emusc-render
```

**32-bit:**
```bash
cmake -B build-win32 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w32.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-win32 --target emusc-render
```

Both builds produce a file named `emusc-render.exe` (the CMake target name doesn't
change with architecture); CI renames the 32-bit one to `emusc-render32.exe` when
it publishes both side by side, matching scva-headless's own `-render`/`-render32`
convention.

The resulting `.exe` files are self-contained, statically linked (`-static-libgcc -static-libstdc++`), and depend only on core Windows DLLs (KERNEL32.dll and msvcrt.dll), so they run on Windows 98 onwards without any additional runtime installation.

**Note:** `emuscd` (the realtime ALSA MIDI daemon) is Linux-only - it's built on
top of ALSA, which doesn't exist on Windows - and not built for Windows at all.
There is no Windows equivalent of it yet; only `emusc-render` (the offline
MIDI-to-WAV renderer) is available there.

### Nightly builds

CI (`.github/workflows/main.yml`) automatically builds all three platform variants (Linux, Windows x64, Windows x86) on every push. Nightly binaries are published as GitHub release assets under the `nightly` tag: `emusc-render` and `emuscd` for Linux, `emusc-render.exe` (64-bit) and `emusc-render32.exe` (32-bit) for Windows. Each nightly publish replaces the previous release outright, so it always reflects exactly the latest push rather than accumulating older builds' files alongside newer ones.

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

Once ROMs are obtained and placed in a directory, use `emusc-render` to render a Standard MIDI File to WAV:

**Render an SC-88 MIDI file:**
```bash
./build/libemusc/tools/emusc-render \
  --device sc88 \
  --rom-dir /path/to/roms \
  --rate 44100 \
  input.mid output.wav
```

**Render an SC-55 MIDI file:**
```bash
./build/libemusc/tools/emusc-render \
  --device sc55 \
  --rom-dir /path/to/roms \
  --rate 44100 \
  input.mid output.wav
```

**Alternative: use named flags for scripting:**
```bash
./build/libemusc/tools/emusc-render \
  --device sc88 \
  --rom-dir /path/to/roms \
  --rate 44100 \
  --midi input.mid \
  --out output.wav
```

See `emusc-render --help` for all options: `--reset` (GM/GS mode), `--tail` (trailing silence), `--seed`, `--bits` (16-bit or 32-bit float), `--float`, `--verbose`, and more.

---

## emuscd Quick Reference

**emuscd** is a realtime ALSA MIDI daemon for Linux that synthesizes MIDI input through libEmuSC and outputs audio to your sound card. It runs in the background, allowing you to play MIDI files or live input through a standard ALSA MIDI sequencer port.

Set the `$EMUSCD_ROM_DIR` environment variable to your ROM directory, then start the daemon:

```bash
export EMUSCD_ROM_DIR=/path/to/roms
./build/emuscd/emuscd --device sc88 --name emuscd
```

Common options:

```
  --device NAME       Device to emulate (default: sc88); sc55, sc55mkii, sc88, jv880
  --name NAME         ALSA MIDI port name (default: emuscd)
  --pcm DEVICE        ALSA PCM output device (default: default)
  --list-pcm          List ALSA PCM devices and exit
  --rate HZ           Requested audio sample rate (default: 44100)
  --latency MS        Requested output buffer size in ms (default: 20)
  --block N           Audio frames per ALSA write (default: 256)
  --help              Show full help
```

You can then connect MIDI input using `aconnect`:

```bash
aconnect -l                          # List ALSA ports
aconnect 24:0 'emuscd':0             # Connect a sequencer port to emuscd
aplaymidi -p 'emuscd' song.mid       # Play a MIDI file
```

For detailed configuration and advanced options, see `emuscd/README.md`.

---

## Questions?

This is exploratory research code. For questions about:
- **The library API:** Read `libemusc/src/synth.h` (public interface)
- **ROM extraction:** See the corpus and PROVENANCE documentation
- **Reverse engineering:** Check git history and commit messages for detailed analysis notes
- **SC-88 specific:** Look at `libemusc/src/sc88_*.c` files and their corresponding research artifacts
- **Alternatives:** [Nuked SC-55](https://github.com/nukeykt/Nuked-SC55) or [SC-55 Soundfont](https://github.com/Kitrinx/SC55_Soundfont) for other approaches
- **Upstream EmuSC:** See https://github.com/skjelten/emusc for the polished GUI application
