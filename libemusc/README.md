# libEmuSC — Core Library for emuscd

**libEmuSC** is the C/C++ core library powering **emuscd**, the headless Roland Sound Canvas emulation project. It implements low-level synthesis for multiple Roland devices and supports both traditional and ROM-driven synthesis paths.

## Supported Devices

- **SC-55** (firmware 1.21) — Classic Sound Canvas; TVA working, TVF in progress
- **SC-55mkII** (firmware 1.01) — Enhanced SC-55 with GM mode support
- **SC-88** — Full ROM-driven engine with 64-voice polyphony and dedicated synthesis path
- **JV-880** — Sampled synth module; device profile and synthesis in progress
- **JV-1080** — 64-voice synth module; behavioural model of its voice path (synthesis firmware undumped), not firmware-exact; no GS mode, powers on in patch mode

See the top-level [README.md](../README.md) for the full project overview and emulation status.

## Status

libEmuSC is currently able to read ROM files from the SC-55 family and SC-88 to reproduce similar audio output. TVA envelope tracking works across all devices. Significant refinements are in progress:
- TVF (filter) envelope curves being extracted and modeled
- Effects processing (reverb, chorus) partially implemented
- LFO and effects routing being reverse engineered

Note that this project is in no way endorsed by or affiliated with Roland Corp.

## Dependencies

libEmuSC requires:
- C++17 compiler with thread support
- CMake 3.12+
- Standard C library for low-level utilities (ROM parsing, audio rendering)

## Building

See [../README.md](../README.md) for build instructions and examples.

## License

libEmuSC is released under the LGPLv2.1+ license. See [COPYING](COPYING) for the full text and detailed attribution of original code vs. new contributions to this fork.
