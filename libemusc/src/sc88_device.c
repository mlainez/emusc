/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_device.h"

#include <stdlib.h>
#include <string.h>

static const uint8_t sc88_device_selectors[SC88_WAVE_BANK_COUNT] = {
  0x00, 0x01, 0x10, 0x11, 0x20, 0x21, 0x30, 0x31
};

static void sc88_device_sync_part(struct sc88_device *device, uint8_t part)
{
  const struct sc88_channel_state *channel = device->channels + part;
  struct sc88_tva_levels levels;
  struct sc88_pan_controls pan;
  levels.master = device->master_volume;
  levels.secondary = device->secondary_level;
  levels.part = channel->volume;
  levels.expression = channel->expression;
  pan.master = device->master_pan;
  /* GS pan 0 is RANDOM. The pan module refuses it rather than inventing the
     XP's random source, and a refusal there costs the whole note - a far
     larger error than a defined position. Substitute centre and count it. */
  if (channel->pan == 0) {
    pan.part = 64;
    ++device->substituted_random_pan;
  } else {
    pan.part = channel->pan;
  }
  sc88_engine_set_part_levels(&device->engine, part, &levels);
  sc88_engine_set_part_pan(&device->engine, part, &pan);
}

static void sc88_device_sync_pitch(struct sc88_device *device, uint8_t part)
{
  const struct sc88_channel_state *channel = device->channels + part;
  int64_t numerator = ((int32_t)channel->pitch_bend - 8192) *
    (int32_t)channel->pitch_bend_sensitivity * 16384;
  int32_t offset = (int32_t)(numerator / (8192 * 12));
  sc88_engine_set_part_pitch_offset(&device->engine, part, offset);
}

static void sc88_device_sync_tvf(struct sc88_device *device, uint8_t part)
{
  const struct sc88_channel_state *channel = device->channels + part;
  struct sc88_tvf_controls controls;
  controls.part_cutoff = channel->cutoff;
  controls.secondary_cutoff = 64;
  controls.part_resonance = channel->resonance;
  controls.secondary_resonance = 64;
  sc88_engine_set_part_tvf_controls(&device->engine, part, &controls);
}

static bool sc88_device_init_common(
  struct sc88_device *device, const uint8_t *control_rom,
  size_t control_rom_size,
  const uint8_t *const chips[SC88_WAVE_CHIP_COUNT],
  const size_t sizes[SC88_WAVE_CHIP_COUNT], double output_rate,
  enum sc88_fractional_wrap wrap, bool raw)
{
  unsigned chip;
  if (!device || !control_rom || !chips || !sizes ||
      control_rom_size != SC88_CONTROL_ROM_SIZE)
    return false;
  memset(device, 0, sizeof *device);
  device->control_rom = (uint8_t *)malloc(SC88_CONTROL_ROM_SIZE);
  if (!device->control_rom)
    goto fail;
  memcpy(device->control_rom, control_rom, SC88_CONTROL_ROM_SIZE);
  for (chip = 0; chip < SC88_WAVE_CHIP_COUNT; ++chip) {
    if (!chips[chip] || sizes[chip] != SC88_WAVE_CHIP_SIZE)
      goto fail;
    device->decoded_chips[chip] = (uint8_t *)malloc(SC88_WAVE_CHIP_SIZE);
    if (!device->decoded_chips[chip])
      goto fail;
    if (raw) {
      if (!sc88_wave_descramble_chip(
            chips[chip], sizes[chip], device->decoded_chips[chip],
            SC88_WAVE_CHIP_SIZE))
        goto fail;
    } else {
      memcpy(device->decoded_chips[chip], chips[chip], SC88_WAVE_CHIP_SIZE);
    }
    device->banks[chip * 2].selector = sc88_device_selectors[chip * 2];
    device->banks[chip * 2].bytes = device->decoded_chips[chip];
    device->banks[chip * 2].size = SC88_WAVE_BANK_SIZE;
    device->banks[chip * 2 + 1].selector =
      sc88_device_selectors[chip * 2 + 1];
    device->banks[chip * 2 + 1].bytes =
      device->decoded_chips[chip] + SC88_WAVE_BANK_SIZE;
    device->banks[chip * 2 + 1].size = SC88_WAVE_BANK_SIZE;
  }
  if (!sc88_renderer_init(&device->renderer, device->control_rom,
                          SC88_CONTROL_ROM_SIZE, device->banks,
                          SC88_WAVE_BANK_COUNT, output_rate, wrap))
    goto fail;
  sc88_renderer_set_tvf_audio_transfer(
    &device->renderer, sc88_tvf_audio_process_provisional, NULL);
  if (!sc88_engine_init(&device->engine, &device->renderer))
    goto fail;
  device->initialized = true;
  sc88_device_reset_controllers(device);
  return true;

fail:
  sc88_device_destroy(device);
  return false;
}

bool sc88_device_init_raw(struct sc88_device *device,
                          const uint8_t *control_rom,
                          size_t control_rom_size,
                          const uint8_t *const raw_chips[SC88_WAVE_CHIP_COUNT],
                          const size_t raw_sizes[SC88_WAVE_CHIP_COUNT],
                          double output_rate,
                          enum sc88_fractional_wrap wrap)
{
  return sc88_device_init_common(device, control_rom, control_rom_size,
                                 raw_chips, raw_sizes, output_rate, wrap, true);
}

bool sc88_device_init_decoded(
  struct sc88_device *device, const uint8_t *control_rom,
  size_t control_rom_size,
  const uint8_t *const decoded_chips[SC88_WAVE_CHIP_COUNT],
  const size_t decoded_sizes[SC88_WAVE_CHIP_COUNT], double output_rate,
  enum sc88_fractional_wrap wrap)
{
  return sc88_device_init_common(device, control_rom, control_rom_size,
                                 decoded_chips, decoded_sizes, output_rate,
                                 wrap, false);
}

void sc88_device_destroy(struct sc88_device *device)
{
  unsigned chip;
  if (!device)
    return;
  if (device->initialized)
    sc88_engine_destroy(&device->engine);
  for (chip = 0; chip < SC88_WAVE_CHIP_COUNT; ++chip)
    free(device->decoded_chips[chip]);
  free(device->control_rom);
  memset(device, 0, sizeof *device);
}

void sc88_device_reset_controllers(struct sc88_device *device)
{
  unsigned part;
  if (!device || !device->initialized)
    return;
  device->master_volume = 127;
  device->secondary_level = 127;
  device->master_pan = 64;
  for (part = 0; part < SC88_ENGINE_PART_COUNT; ++part) {
    struct sc88_channel_state *channel = device->channels + part;
    channel->variation = 0;
    channel->map_lsb = 0;
    channel->program = 0;
    channel->volume = 100;
    channel->expression = 127;
    channel->pan = 64;
    channel->hold1 = 0;
    channel->cutoff = 64;
    channel->resonance = 64;
    channel->pitch_bend = 8192;
    channel->pitch_bend_sensitivity = 2;
    channel->rpn_msb = 127;
    channel->rpn_lsb = 127;
    channel->nrpn_msb = 127;
    channel->nrpn_lsb = 127;
    channel->same_note_mode = SC88_SAME_NOTE_LIMITED_MULTI;
    /* GS puts the rhythm part on MIDI channel 10, i.e. part 9 of each port,
       and a GS reset restores exactly that. Which kit set a reset leaves
       selected is not recovered - the active map selector goes to zero and
       zero is not documented to mean the SC-88 - so an SC-88 device defaults
       to its own kits and says so. */
    sc88_engine_set_part_rhythm(&device->engine, (uint8_t)part,
                                (part % 16u) == 9u ? SC88_RHYTHM_MAP_SC88 : 0);
    sc88_engine_hold_value(&device->engine, (uint8_t)part, 0);
    sc88_engine_sostenuto(&device->engine, (uint8_t)part, false);
    sc88_device_sync_part(device, (uint8_t)part);
    sc88_device_sync_pitch(device, (uint8_t)part);
    sc88_device_sync_tvf(device, (uint8_t)part);
  }
}

void sc88_device_set_master_volume(struct sc88_device *device, uint8_t value)
{
  unsigned part;
  if (!device || !device->initialized || value > 127)
    return;
  device->master_volume = value;
  for (part = 0; part < SC88_ENGINE_PART_COUNT; ++part)
    sc88_device_sync_part(device, (uint8_t)part);
}

void sc88_device_set_master_pan(struct sc88_device *device, uint8_t value)
{
  unsigned part;
  if (!device || !device->initialized || value < 1 || value > 127)
    return;
  device->master_pan = value;
  for (part = 0; part < SC88_ENGINE_PART_COUNT; ++part)
    sc88_device_sync_part(device, (uint8_t)part);
}

bool sc88_device_midi(struct sc88_device *device, uint8_t port,
                      uint8_t status, uint8_t data1, uint8_t data2)
{
  uint8_t channel;
  uint8_t part;
  struct sc88_channel_state *state;
  if (!device || !device->initialized || port >= SC88_MIDI_PORT_COUNT ||
      (status & 0x80) == 0 || data1 > 127 || data2 > 127)
    return false;
  channel = status & 0x0f;
  part = (uint8_t)(port * 16 + channel);
  state = device->channels + part;
  switch (status & 0xf0) {
  case 0x80:
    return sc88_engine_note_off(&device->engine, part, data1);
  case 0x90:
    if (data2 == 0)
      return sc88_engine_note_off(&device->engine, part, data1);
    if (state->map_lsb == 1)
      return false;
    return sc88_engine_note_on(
      &device->engine, part, state->variation, state->program, data1, data2,
      0, state->same_note_mode, 1.0f);
  case 0xb0:
    switch (data1) {
    case 0:
      state->variation = data2;
      return true;
    case 6:
      if (state->rpn_msb == 0 && state->rpn_lsb == 0) {
        state->pitch_bend_sensitivity = data2 > 24 ? 24 : data2;
        sc88_device_sync_pitch(device, part);
        return true;
      }
      if (state->nrpn_msb == 1 && state->nrpn_lsb == 0x20) {
        state->cutoff = data2;
        sc88_device_sync_tvf(device, part);
        return true;
      }
      if (state->nrpn_msb == 1 && state->nrpn_lsb == 0x21) {
        state->resonance = data2;
        sc88_device_sync_tvf(device, part);
        return true;
      }
      return false;
    case 7:
      state->volume = data2;
      sc88_device_sync_part(device, part);
      return true;
    case 10:
      state->pan = data2;
      sc88_device_sync_part(device, part);
      return true;
    case 11:
      state->expression = data2;
      sc88_device_sync_part(device, part);
      return true;
    case 32:
      /* The native renderer currently treats map 0 and explicit map 2 as
       * SC-88. Explicit SC-55 map 1 is preserved but cannot be rendered. */
      if (data2 > 2)
        return false;
      state->map_lsb = data2;
      return true;
    case 64:
      state->hold1 = data2;
      sc88_engine_hold_value(&device->engine, part, data2);
      return true;
    case 66:
      sc88_engine_sostenuto(&device->engine, part, data2 >= 64);
      return true;
    case 98:
      state->nrpn_lsb = data2;
      state->rpn_msb = 127;
      state->rpn_lsb = 127;
      return true;
    case 99:
      state->nrpn_msb = data2;
      state->rpn_msb = 127;
      state->rpn_lsb = 127;
      return true;
    case 100:
      state->rpn_lsb = data2;
      state->nrpn_msb = 127;
      state->nrpn_lsb = 127;
      return true;
    case 101:
      state->rpn_msb = data2;
      state->nrpn_msb = 127;
      state->nrpn_lsb = 127;
      return true;
    case 121:
      state->expression = 127;
      state->hold1 = 0;
      state->pitch_bend = 8192;
      state->rpn_msb = 127;
      state->rpn_lsb = 127;
      state->nrpn_msb = 127;
      state->nrpn_lsb = 127;
      sc88_engine_hold_value(&device->engine, part, 0);
      sc88_engine_sostenuto(&device->engine, part, false);
      sc88_device_sync_part(device, part);
      sc88_device_sync_pitch(device, part);
      return true;
    default:
      return false;
    }
  case 0xc0:
    state->program = data1;
    return true;
  case 0xe0:
    state->pitch_bend = (uint16_t)(data1 | ((uint16_t)data2 << 7));
    sc88_device_sync_pitch(device, part);
    return true;
  default:
    return false;
  }
}

void sc88_device_render(struct sc88_device *device, float *stereo,
                        size_t frames)
{
  if (!device || !device->initialized || !stereo)
    return;
  sc88_engine_render(&device->engine, stereo, frames);
}
