# emuscd - Roland Sound Canvas Daemon

**emuscd** is a headless MIDI synthesizer daemon that emulates Roland Sound Canvas devices. Run it on any Linux machine, connect your MIDI controller, and play!

## Usage

```bash
# Start with SC-88 (default)
emuscd

# Start with SC-55
emuscd --device sc55

# Custom MIDI port name
emuscd --device sc88 --name "My Synth"
```

## Supported Devices

- `sc55` — Roland Sound Canvas SC-55
- `sc55mkii` — Roland Sound Canvas SC-55mkII
- `sc88` — Roland Sound Canvas SC-88 (default)
- `jv880` — Roland JV-880

## Connecting MIDI

### Using aconnect
```bash
# List MIDI devices
aconnect -l

# Connect your keyboard to emuscd
# Replace "Your Keyboard" with the actual port name
aconnect "Your Keyboard" "emuscd"
```

### Using JACK
If JACK is running, emuscd uses ALSA's JACK bridge:
```bash
emuscd --device sc88 --name "emuscd"
# Connect in Qjackctl or your JACK patchbay
```

## Runtime Device Switching

While emuscd is running, type a device name to switch:

```
$ emuscd --device sc55
emuscd daemon running. Type device name to switch (e.g., sc88).
sc88
Loading device: sc88
```

Type `quit` or `exit` to stop the daemon.

## Options

- `--device NAME` — Device to emulate (default: sc88)
- `--name NAME` — ALSA MIDI port name (default: emuscd)
- `--pcm DEVICE` — ALSA PCM output device (default: default)
- `--list-pcm` — List ALSA PCM devices and exit
- `--rate HZ` — Requested audio sample rate (default: 44100)
- `--latency MS` — Requested output buffer size (default: 20)
- `--block N` — Audio frames per ALSA write (default: 256)
- `--help` — Show help message

## ROM Files

emuscd requires Roland ROM files for the devices you want to emulate. Set the ROM directory:

```bash
export EMUSCD_ROM_DIR=/path/to/roms
emuscd --device sc88
```

One naming convention covers all four devices: `<device>_control.bin` is
always the control/program ROM, and `<device>_cpu.bin` is the internal CPU
ROM that only SC-55 and SC-55mkII have.

- `sc55_control.bin`, `sc55_cpu.bin`, `sc55_waverom{1,2,3}.bin` (SC-55)
- `sc55mkii_control.bin`, `sc55mkii_cpu.bin`, `sc55mkii_waverom{1,2}.bin` (SC-55mkII)
- `sc88_control.bin`, `sc88_waverom{1,2,3,4}.bin` (SC-88; no separate CPU ROM)
- `jv880_control.bin`, `jv880_waverom{1,2}.bin` (JV-880)

See the top-level [README.md](../README.md) for exact file sizes and SHA1/MD5
hashes of known-good ROM dumps.

## Audio Output

Audio is sent to your system's default ALSA PCM device (usually your speakers or JACK).

To route to a specific device:
```bash
emuscd --pcm hw:1
```

`--list-pcm` shows what's available. `ALSA_CARD=1 emuscd` (ALSA's own
environment variable) still works too.

## Building

emuscd is built as part of the main emuscd project:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/emuscd/emuscd --device sc88
```
