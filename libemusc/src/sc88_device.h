/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_SC88_DEVICE_H
#define EMUSC_SC88_DEVICE_H

#include "sc88_engine.h"
#include "sc88_reverb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SC88_WAVE_CHIP_COUNT 4u
#define SC88_MIDI_PORT_COUNT 2u

struct sc88_channel_state {
  uint8_t variation;
  uint8_t map_lsb;
  uint8_t program;
  uint8_t volume;
  uint8_t expression;
  uint8_t pan;
  uint8_t hold1;
  uint8_t reverb_send;
  uint8_t cutoff;
  uint8_t resonance;
  uint16_t pitch_bend;
  uint8_t pitch_bend_sensitivity;
  uint8_t rpn_msb;
  uint8_t rpn_lsb;
  uint8_t nrpn_msb;
  uint8_t nrpn_lsb;
  enum sc88_same_note_mode same_note_mode;
};

struct sc88_device {
  uint8_t *control_rom;
  uint8_t *decoded_chips[SC88_WAVE_CHIP_COUNT];
  struct sc88_wave_bank banks[SC88_WAVE_BANK_COUNT];
  struct sc88_renderer renderer;
  struct sc88_engine engine;
  struct sc88_reverb reverb;
  /* The reverb's own parameters. A GS reset leaves the character and its
     level, time and pre-LPF at the values the manual prints for Hall 2. */
  uint8_t reverb_character, reverb_level, reverb_time, reverb_pre_lpf;
  /* The chorus parameters are received and held but not yet rendered: the
     CPU-side transforms are recovered while the DSP's audio algorithm is
     not (`08_effects/chorus.md`), so a chorus here would be invented
     rather than modelled. Holding them keeps a song's settings from being
     silently discarded. */
  uint8_t chorus_macro, chorus_level, chorus_feedback, chorus_delay;
  uint8_t chorus_rate, chorus_depth, chorus_pre_lpf, chorus_send_to_reverb;
  /* 0 single module, 1 double. Several parameters exist only in one mode. */
  uint8_t system_mode;
  /* SysEx writes that parsed correctly but name an address this
     implementation does not act on. Counted rather than dropped quietly,
     so a render can say what it ignored. */
  unsigned long unhandled_sysex;
  float *send_bus;
  size_t send_capacity;
  struct sc88_channel_state channels[SC88_ENGINE_PART_COUNT];
  uint8_t master_volume;
  uint8_t secondary_level;
  uint8_t master_pan;
  /* How many times a part's pan was substituted because it asked for GS
     random pan, which needs a sound-chip random source that is not
     recovered. Counted rather than hidden: it is a labelled divergence, and
     a render reporting zero is exact in this respect. */
  unsigned long substituted_random_pan;
  bool initialized;
};

/* Input buffers are copied. Raw images are the four physical two-MiB dumps
 * in selector order 00/01, 10/11, 20/21, 30/31. */
bool sc88_device_init_raw(struct sc88_device *device,
                          const uint8_t *control_rom,
                          size_t control_rom_size,
                          const uint8_t *const raw_chips[SC88_WAVE_CHIP_COUNT],
                          const size_t raw_sizes[SC88_WAVE_CHIP_COUNT],
                          double output_rate,
                          enum sc88_fractional_wrap wrap);

/* Same ownership contract, for already descrambled two-MiB chip images. */
bool sc88_device_init_decoded(
  struct sc88_device *device, const uint8_t *control_rom,
  size_t control_rom_size,
  const uint8_t *const decoded_chips[SC88_WAVE_CHIP_COUNT],
  const size_t decoded_sizes[SC88_WAVE_CHIP_COUNT], double output_rate,
  enum sc88_fractional_wrap wrap);

void sc88_device_destroy(struct sc88_device *device);
void sc88_device_reset_controllers(struct sc88_device *device);
void sc88_device_set_master_volume(struct sc88_device *device, uint8_t value);
void sc88_device_set_master_pan(struct sc88_device *device, uint8_t value);

/* MIDI port 0 addresses parts 0..15 and port 1 addresses parts 16..31.
 * Supported channel messages: note on/off, pitch bend, program change,
 * CC0/6/7/10/11/32/64/66/98..101/121, RPN 00/00 bend sensitivity, and
 * NRPN 01/20..21 cutoff/resonance.
 * Unsupported messages return false without changing state. */
/* One System Exclusive message, with or without its leading `f0` and
 * trailing `f7`. Returns false for a message that is not a well-formed GS
 * DT1 for this device - a wrong manufacturer or model, a bad checksum, a
 * truncated packet - and true once the packet has been applied, whether or
 * not every address in it was one this implementation acts on.
 */
bool sc88_device_sysex(struct sc88_device *device, uint8_t port,
                       const uint8_t *data, size_t size);

bool sc88_device_midi(struct sc88_device *device, uint8_t port,
                      uint8_t status, uint8_t data1, uint8_t data2);

void sc88_device_render(struct sc88_device *device, float *stereo,
                        size_t frames);

#ifdef __cplusplus
}
#endif

#endif
