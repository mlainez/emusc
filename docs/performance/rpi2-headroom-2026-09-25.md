# Measured headroom on a Raspberry Pi 2

All figures measured on the hardware, not calculated. This is input for
`arm-engine-performance-plan.md`: a Pi 2 v1.1 is exactly the ARMv7 hard-float
baseline that plan targets, and it is the slowest board din5 ships an image
for.

## Device

| | |
|---|---|
| Board | Raspberry Pi 2 Model B **v1.1** (revision `a01041`) |
| SoC | BCM2836, 4x Cortex-A7 |
| Clock | 900 MHz, all 4 cores, governor `performance` (stock, no overclock) |
| RAM | 931 MB total |
| Kernel | Linux 6.12.61-v7 armv7l (din5's own build) |
| Userland | Buildroot 2026.05.2, din5 appliance image |
| `isolcpus` | `3` — core 3 is reserved by the kernel and idle for this test; only cores 0-2 are in the default scheduling domain |
| Audio out | 3.5 mm jack, ALSA `default`, 32000 Hz |

## Binary under test

Built from the `mlainez/emuscd` fork (renamed from `emusc`), nightly CI
artifact `emusc-linux-arm32.tar.gz`, tag `nightly` at commit
`2f5a4bebb1ed5bcf41f02fbaf1c7a9e9ac5ad2b2`. `emusc-render --version` on the
device reports `libEmuSC commit unknown` — the release build is not embedding
its own source commit, which is itself worth fixing so a binary can be traced
back to what produced it.

`libemusc` is statically linked into `emuscd`/`emusc-render` (no
`libemusc.so` in `NEEDED`), so this is exactly what a from-source build
against the same commit would measure.

## Method

`emusc-render --device <dev> --rom-dir <dir> --play --midi <file>`, run
directly on the box over SSH, audio out the headphone jack. `--play` paces
itself against the real ALSA device and prints `CPU busy % of wall time
(window)` and `(overall)` as it goes — window is the more useful number, since
an overall average hides a stall the same way din5's own "worst-second" figures
are designed not to.

Real music, not a synthetic stress file: the SC-55/SC-88 tracks are the
"Jazz Lagoon" GS demo song, JV-880 is factory demo 1 ("Intro"), JV-1080 is its
own ROM demo song ("RISE"), extracted straight from the ROM used in the test
(`scdb/devices/jv1080/tools/extraction/extract_demo_songs.py`). ROMs: SC-55
v1.21 (mk1), SC-88 (control ROM identity not resolved by libEmuSC — reports
`v? (?)`), JV-880 (control ROM identity likewise unresolved, wave ROM v1.00),
JV-1080 v1.02.

## Results

| device | song | length | worst window | overall | verdict |
|---|---|---|---|---|---|
| sc55 | Jazz Lagoon | 87.3 s | ~39% | ~22% | comfortable, ~2.5x headroom even at the worst moment |
| sc88 | Jazz Lagoon | 87.3 s | 99-100% | 99% | **no headroom** — pegged at the ceiling for nearly the whole track |
| jv880 | Intro | 24.8 s | ~40% | ~27% | comfortable |
| jv1080 | RISE | 101.5 s | 100% (sustained ~80 s) | 93% | **fails to hold realtime** once the song leaves its quiet setup section |

`0 clipped samples` on every run — clipping is a separate, unrelated
measurement from CPU headroom and both engines that pegged at 100% still
reported none, so "no dropouts audible" cannot be assumed from that field.

RISE's own structure explains its curve: the file opens with ~13% busy during
its setup block (ticks 64-1724, mostly SysEx/DT1 patch loads, little voice
activity), then jumps to 100% the moment the music itself starts (~frame
70000) and stays there for the rest of the piece.

## Core utilization — the actual finding

**Both engines are single-threaded.** `/proc/<pid>/status` reports `Threads:
1` throughout a run, on both the light (sc55, jv880) and saturated (sc88,
jv1080) cases. The kernel command line pins core 3 aside (`isolcpus=3`) for
something else entirely (din5's own `sc55d`/Nuked-SC55 renderer thread, per
`din5/docs/tuning.md`) and this workload never touches it; of the 3 remaining
cores, only one carries the render thread's load at any time — the other two
sit idle the whole time, on every device tested including the two that pegged
at 100%.

That means the 100% figures for sc88 and jv1080 are 100% of **one core out of
four**, not 100% of the machine. On paper there is 3x more raw compute sitting
idle on this exact board whenever the render thread saturates — sc88 and
jv1080 are exactly the two devices where parallelizing the voice/partial
render loop across even 2 threads would plausibly take them from "at the
ceiling" to comfortably realtime, without touching a single float operation.

## Suggested next steps

1. Confirm whether SC-88's and JV-1080's per-voice rendering (`Xp` namespace
   for JV-1080, per the symbol names) has any cross-voice or cross-partial
   dependency that would block splitting the render loop across 2-3 worker
   threads, one per group of parts/voices, joined once per audio block.
2. If so, that is a bigger and more valuable win on ARMv7 than the
   lossless single-thread changes in `arm-engine-performance-plan.md` —
   those help every device a little, this would be the difference between
   SC-88/JV-1080 working on a Pi 2 at all or not.
3. Embed the real source commit in release builds (`libEmuSC commit unknown`
   above) so a nightly artifact can be matched back to what produced it —
   currently the only way to know is the GitHub release tag's own commit SHA.
4. Re-run this same method once ROM identity is fixed for SC-88 and JV-880
   (`control ROM ... v? (?)` above) — not a performance issue, but it means
   two of the four devices under test could not be confirmed to be running
   the exact ROM revision this document claims.
