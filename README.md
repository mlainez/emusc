![EmuSC_logo](https://raw.githubusercontent.com/wiki/skjelten/emusc/images/emusc-logo.png)
---

EmuSC is a software synthesizer that aims to emulate the Roland Sound Canvas SC-55 lineup to recreate the original sounds of these '90s era synthesizers. Emulation is done by extracting relevant information from the original control and PCM ROMs and reimplement the synth's behavior in modern C++.

This project has been in development since 2022 and is currently able to reproduce audio that is relatively similar to the original synths. There are still some significant shortcomings in the generated audio, varying on which instruments and settings are being used, but the goal is to be able to reproduce sounds that will make it very difficult to notice the difference.

If you are looking for the best possible SC-55 emulation today you might want to try the [Nuked SC-55](https://github.com/nukeykt/Nuked-SC55) project, or to use a sound font based on the SC-55, such as [SC-55 sound font](https://github.com/Kitrinx/SC55_Soundfont) made by Kitrinx and NewRisingSun.

The EmuSC project is split into two parts:
* [EmuSC](./emusc): A desktop application that serves as a frontend to libEmuSC.
* [libEmuSC](./libemusc): A library that implements all the Sound Canvas emulation.

Note that this project is in no way endorsed by or affiliated with Roland Corp.

![Screenshot of EmuSC v0.1.0](https://raw.githubusercontent.com/wiki/skjelten/emusc/images/Screenshot_EmuSC_0_1_0.png)


## Getting started

The quickest way to test EmuSC is to install precompiled packages of the [latest release](https://github.com/skjelten/emusc/releases/latest). If no packages are available for your platform, or you want to test the latest code changes, have a look at the detailed build instructions in the [Wiki](https://github.com/skjelten/emusc/wiki/Build-Instructions).

If you run into any problems please read the [troubleshooting guide](https://github.com/skjelten/emusc/wiki/Troubleshooting-Guide). If that did not help, or you have any other questions or feedback, feel free to start a new [discussion](https://github.com/skjelten/emusc/discussions) or create a [new issue](https://github.com/skjelten/emusc/issues).


## Contributing

Interested in C++ programming, reverse engineering, audio synthesis or synthesizers in general? We welcome anyone who wants to learn more and perhaps could contribute to the project. To get started we suggest that you:
* Download or fork the source code and have a look
* Read the [wiki](https://github.com/skjelten/emusc/wiki) for more information about the ROM files and other core parts of the emulator
* Create an issue if you have any questions or suggestions and we will do our best to help you out getting the hang of how it all works!


## License

EmuSC is free software and released under the GNU general public license:
* EmuSC is released under the GPLv3+ license.
* libEmuSC is released under the LGPLv2.1+ license.

### Copyright in this fork

Those licences stand, and the combined work keeps them. What follows describes
only the files this fork added, so a reader can tell whose work is whose.

**Files carried over from upstream, and files that relocate upstream code,
keep their original notice** — `Copyright (C) 2022-2026  Håkon Skjelten`,
LGPL-2.1-or-later. Modifying a file does not change who wrote it, and neither
does moving its contents into a new filename. That includes
`libemusc/src/device_profile.h`, `libemusc/src/devices/sc55.cc`,
`devices/sc55mkii.cc` and `devices/sound_canvas_default.cc`, which exist
because device constants were lifted out of the engine into per-device
profiles.

**Files written from scratch here are dedicated to the public domain** under
CC0 1.0 and carry `SPDX-License-Identifier: CC0-1.0`:

* `libemusc/src/analog_stage.cc` and `.h` — the post-chip output stage
* `libemusc/src/devices/jv880.cc` — the JV-880 device profile
* `libemusc/src/jv_velocity.h`, `libemusc/src/jv_ctrl_matrix.h` — JV tables
  read out of ROM
* `libemusc/src/sc88_wave.c` / `.h`, `libemusc/src/sc88_rom.c` / `.h`,
  `libemusc/src/sc88_oscillator.c` / `.h`, `libemusc/src/sc88_renderer.c` /
  `.h`, `libemusc/src/sc88_tva.c` / `.h`, `libemusc/src/sc88_pan.c` / `.h`,
  `libemusc/src/sc88_engine.c` / `.h`, and their tests — the SC-88 immutable
  ROM graph, descriptor, FCE-DPCM, parameterized oscillator, first dry-render
  path, ROM-table static TVA/pan gains, TVA release countdown and 64-slot
  note/voice engine

CC0 is compatible with the GNU licences. It grants no patent rights, which the
FSF notes as a reason to prefer a permissive software licence instead; that is
an accepted trade here rather than an oversight.

Two of those files are largely Roland ROM content tabulated. CC0 there waives
whatever rights might attach to the tabulation — it asserts no authorship of
Roland's data.

**AI involvement is disclosed, not credited as authorship.** Much of this
fork's work was produced by an AI assistant under human direction. Commits
carry a single `Assisted-by:` trailer, following the kernel's
coding-assistants convention; the human contributor is the author of record
and takes responsibility. `Co-Authored-By:` is deliberately not used.

The research behind these changes — ROM maps, measurements, and the divergence
register that drove them — is a separate repository under CC0.
