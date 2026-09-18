# emuscd - Roland Sound Canvas Daemon

**emuscd** is a headless MIDI synthesizer daemon that emulates Roland Sound Canvas devices. Run it on any Linux machine, connect your MIDI controller, and play!

## Usage

```bash
# Start with SC-88 (default)
emuscd

# Start with SC-55
emuscd --device sc55

# Custom MIDI port name
emuscd --device sc88 --port-name "My Synth"
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
emuscd --device sc88 --port-name "emuscd"
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
- `--port-name NAME` — ALSA MIDI port name (default: emuscd)
- `--help` — Show help message

## ROM Files

emuscd requires Roland ROM files for the devices you want to emulate. Set the ROM directory:

```bash
export EMUSCD_ROM_DIR=/path/to/roms
emuscd --device sc88
```

ROM files should be named:
- `sc55_rom1.bin` and `sc55_rom2.bin` (SC-55)
- `sc55mkii_rom1.bin` and `sc55mkii_rom2.bin` (SC-55mkII)
- `sc88_rom1.bin` and `sc88_rom2.bin` (SC-88)
- `jv880_rom1.bin` and `jv880_wave.bin` (JV-880)

## Audio Output

Audio is sent to your system's default ALSA PCM device (usually your speakers or JACK).

To route to a specific ALSA device:
```bash
ALSA_CARD=1 emuscd
```

## Building

emuscd is built as part of the main emuscd project:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/emuscd/emuscd --device sc88
```
