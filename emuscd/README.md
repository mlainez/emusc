# emuscd — Roland Sound Canvas Daemon

This directory builds two realtime MIDI-in/audio-out front ends for libEmuSC:
**emuscd** on Linux (ALSA) and **emusc-winmidi** on Windows (WinMM). Only one
of the two is built for a given target - see the [top-level
README](../README.md#building) for build and cross-compilation instructions.

## Supported Devices

- `sc55` — Roland Sound Canvas SC-55
- `sc55mkii` — Roland Sound Canvas SC-55mkII
- `sc88` — Roland Sound Canvas SC-88 (default)
- `jv880` — Roland JV-880

## ROM Files

Both binaries need Roland ROM files for the devices they emulate. Set
`$EMUSCD_ROM_DIR` to the directory holding them:

- `sc55_control.bin`, `sc55_cpu.bin`, `sc55_waverom{1,2,3}.bin` (SC-55)
- `sc55mkii_control.bin`, `sc55mkii_cpu.bin`, `sc55mkii_waverom{1,2}.bin` (SC-55mkII)
- `sc88_control.bin`, `sc88_waverom{1,2,3,4}.bin` (SC-88; no separate CPU ROM)
- `jv880_control.bin`, `jv880_waverom{1,2}.bin` (JV-880)

See the top-level [README.md](../README.md) for exact file sizes and
SHA1/MD5 hashes of known-good ROM dumps.

If `$EMUSCD_ROM_DIR` is unset: `emuscd` looks in `/usr/share/emuscd/roms`;
`emusc-winmidi` looks in `.\roms`, relative to its working directory.

---

## Linux (emuscd)

emuscd creates its own ALSA sequencer MIDI port.

### Usage

```bash
emuscd --device sc88 --name "My Synth"
```

### Options

```
--device NAME       Device to emulate (default: sc88); sc55, sc55mkii, sc88, jv880
--name NAME         ALSA MIDI port name (default: emuscd)
--pcm DEVICE        ALSA PCM output device (default: default)
--list-pcm          List ALSA PCM devices and exit
--rate HZ           Requested audio sample rate (default: 48000)
--latency MS        Requested output buffer size in ms (default: 20)
--block N           Audio frames per ALSA write (default: 256)
--help              Show full help
```

### Connecting MIDI

```bash
aconnect -l                          # List ALSA ports
aconnect "Your Keyboard" "emuscd"    # Connect your keyboard to emuscd
aplaymidi -p 'emuscd' song.mid       # Play a MIDI file
```

If JACK is running, emuscd uses ALSA's JACK bridge automatically - connect it
from Qjackctl or your JACK patchbay the same way.

### Audio output

Audio goes to `--pcm`'s device, `default` if not given - usually your
speakers, or wherever ALSA/PipeWire/JACK routes `default`. Route to a
specific device instead:

```bash
emuscd --pcm hw:1
```

`--list-pcm` shows what's available; ALSA's own `ALSA_CARD` environment
variable still works too.

### Runtime device switching

While emuscd is running, type a device name at its stdin to switch, or
`quit`/`exit` to stop it:

```
$ emuscd --device sc55
ALSA MIDI input opened: emuscd
ALSA audio output 'default' initialized at 48000 Hz, ~20 ms buffer
Loaded device: sc55 (SC-55 v1.21)
emuscd daemon running (device: sc55). Type a device name to switch, or 'quit' to exit.
sc88
Loaded device: sc88 (SC-88 v...)
```

---

## Windows (emusc-winmidi)

emusc-winmidi is emuscd's Windows counterpart, built on WinMM instead of
ALSA. Unlike emuscd it does not create its own MIDI port - WinMM's `midiIn`
API only opens an *existing* one - so route MIDI into it with a virtual MIDI
cable (loopMIDI on Windows 7+, Maple Virtual MIDI Cable on 95/98/ME/2000/XP),
then point `--midi-in` at that port.

### Usage

```bat
emusc-winmidi.exe --list-midi-in
emusc-winmidi.exe --device sc88 --midi-in 1
```

### Options

```
--device NAME       Device to emulate (default: sc88); sc55, sc55mkii, sc88, jv880
--midi-in N          MIDI input device index (default: 0)
--wave-out N         Wave output device index (default: system default)
--list-midi-in       List MIDI input devices and exit
--list-wave-out      List wave output devices and exit
--rate HZ            Audio sample rate (default: 48000)
--block N            Audio frames per wave buffer (default: 256)
--latency MS         Requested output buffer size in ms (default: 20)
--help               Show full help
```

### Runtime device switching

Same mechanism as emuscd: type a device name at the console it's running in
to switch, or `quit`/`exit` to stop it (Ctrl+C also works).
