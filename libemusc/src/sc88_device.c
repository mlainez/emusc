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
  /* Hall 2 is the character a reset selects; the reverb reads its own delay
     lines and diffuser count out of the ROM (`M-008`). A ROM that carries no
     character records is not a reason to refuse the device: an effect is not
     a precondition for the voice path, so the device renders dry and says so
     through `reverb.active`. */
  (void)sc88_reverb_init(&device->reverb, &device->renderer.rom, 4,
                         output_rate);
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
  sc88_reverb_destroy(&device->reverb);
  free(device->send_bus);
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
  /* Hall 2 with the manual's own default level, time and pre-LPF. */
  device->reverb_character = 4;
  device->reverb_level = 64;
  device->reverb_time = 64;
  device->reverb_pre_lpf = 0;
  sc88_reverb_set_params(&device->reverb, device->reverb_level,
                         device->reverb_time, device->reverb_pre_lpf);
  sc88_reverb_reset(&device->reverb);
  for (part = 0; part < SC88_ENGINE_PART_COUNT; ++part) {
    struct sc88_channel_state *channel = device->channels + part;
    channel->variation = 0;
    channel->map_lsb = 0;
    channel->program = 0;
    channel->volume = 100;
    channel->expression = 127;
    channel->pan = 64;
    channel->hold1 = 0;
    /* GS's own default part reverb send is 40 (SC88-OM). */
    channel->reverb_send = 40;
    sc88_engine_set_part_reverb_send(&device->engine, (uint8_t)part, 40);
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


/* Both the manual's block numbering and the reverb macro's meaning are in
   `04_protocol/sysex.md`: for the sixteen blocks of a group, x=1..9 selects
   parts 1..9, x=0 selects part 10 and x=a..f selects parts 11..16. */
static bool sc88_device_block_part(uint8_t block, uint8_t port,
                                   uint8_t *part)
{
  uint8_t index;
  if (block <= 0x09)
    index = block == 0 ? 9u : (uint8_t)(block - 1);
  else if (block <= 0x0f)
    index = block;
  else
    return false;
  *part = (uint8_t)(port * 16u + index);
  return true;
}

static bool sc88_device_set_reverb_character(struct sc88_device *device,
                                             uint8_t character)
{
  struct sc88_reverb replacement;
  if (character == device->reverb_character)
    return true;
  /* A character is a different set of delay lines read out of the ROM, so
     it is a new reverb rather than a new parameter. The old one is kept
     until the new one is known to have been built. */
  if (!sc88_reverb_init(&replacement, &device->renderer.rom, character,
                        device->reverb.output_rate))
    return false;
  sc88_reverb_destroy(&device->reverb);
  device->reverb = replacement;
  device->reverb_character = character;
  sc88_reverb_set_params(&device->reverb, device->reverb_level,
                         device->reverb_time, device->reverb_pre_lpf);
  return true;
}

/* One address of a DT1 packet. `true` means the write was acted on. */
static bool sc88_device_sysex_write(struct sc88_device *device, uint8_t port,
                                   uint32_t address, uint8_t value)
{
  uint8_t part;
  if (address == 0x00007f) {
    /* System Mode Set. Which parameters exist depends on it, so it is
       held even though nothing yet varies with it. */
    if (value > 1)
      return false;
    device->system_mode = value;
    return true;
  }
  if (address == 0x40007f) {
    sc88_device_reset_controllers(device);
    return true;
  }
  switch (address) {
  case 0x400004:
    sc88_device_set_master_volume(device, value);
    return true;
  case 0x400006:
    sc88_device_set_master_pan(device, value);
    return true;
  /* The macro and the character are printed with the same eight names and
     the same default, so a macro write selects that character. Whether the
     macro also reloads the rest of the reverb block from a preset - as the
     delay macro demonstrably does - is not recovered, so nothing else is
     touched here. A song that means a preset sends its fields anyway. */
  case 0x400130:
  case 0x400131:
    return value <= 7 && sc88_device_set_reverb_character(device, value);
  case 0x400132:
    if (value > 7)
      return false;
    device->reverb_pre_lpf = value;
    sc88_reverb_set_params(&device->reverb, device->reverb_level,
                           device->reverb_time, device->reverb_pre_lpf);
    return true;
  case 0x400133:
    device->reverb_level = value;
    sc88_reverb_set_params(&device->reverb, device->reverb_level,
                           device->reverb_time, device->reverb_pre_lpf);
    return true;
  case 0x400134:
    device->reverb_time = value;
    sc88_reverb_set_params(&device->reverb, device->reverb_level,
                           device->reverb_time, device->reverb_pre_lpf);
    return true;
  case 0x400138:
    device->chorus_macro = value;
    return true;
  case 0x400139:
    device->chorus_pre_lpf = value;
    return true;
  case 0x40013a:
    device->chorus_level = value;
    return true;
  case 0x40013b:
    device->chorus_feedback = value;
    return true;
  case 0x40013c:
    device->chorus_delay = value;
    return true;
  case 0x40013d:
    device->chorus_rate = value;
    return true;
  case 0x40013e:
    device->chorus_depth = value;
    return true;
  case 0x40013f:
    device->chorus_send_to_reverb = value;
    return true;
  default:
    break;
  }
  /* Patch-part block. Family 40 addresses the group on the same side as the
     port the message arrived on and family 50 the opposite group, so the
     caller's port is what decides which sixteen parts `1x` counts within. */
  if ((address & 0xf0f000u) == 0x401000u &&
      sc88_device_block_part((uint8_t)((address >> 8) & 0x0f), port, &part)) {
    struct sc88_channel_state *state = device->channels + part;
    switch (address & 0xffu) {
    case 0x14:
      if (value > 2)
        return false;
      state->same_note_mode = value == 0 ? SC88_SAME_NOTE_SINGLE
        : value == 1 ? SC88_SAME_NOTE_LIMITED_MULTI
        : SC88_SAME_NOTE_FULL_MULTI;
      return true;
    case 0x15:
      /* Off, kit set 1 or kit set 2 - the same switch CC32 reaches from
         the channel side, and the only way a song can put drums on a part
         other than 10. */
      if (value > 2)
        return false;
      sc88_engine_set_part_rhythm(&device->engine, part, value);
      return true;
    case 0x19:
      state->volume = value;
      sc88_device_sync_part(device, part);
      return true;
    case 0x1c:
      state->pan = value;
      sc88_device_sync_part(device, part);
      return true;
    case 0x21:
      /* Chorus send is held for the same reason the chorus block is. */
      return true;
    case 0x22:
      state->reverb_send = value;
      sc88_engine_set_part_reverb_send(&device->engine, part, value);
      return true;
    default:
      return false;
    }
  }
  return false;
}

bool sc88_device_sysex(struct sc88_device *device, uint8_t port,
                       const uint8_t *data, size_t size)
{
  uint32_t address;
  uint8_t group;
  size_t i;
  unsigned sum;
  if (!device || !device->initialized || !data ||
      port >= SC88_MIDI_PORT_COUNT)
    return false;
  /* accept the message with or without its framing bytes */
  if (size && data[0] == 0xf0) {
    ++data;
    --size;
  }
  if (size && data[size - 1] == 0xf7)
    --size;
  /* 41 dev 42 12, three address bytes, at least one data byte, checksum */
  if (size < 9 || data[0] != 0x41 || data[2] != 0x42 || data[3] != 0x12)
    return false;
  /* the unit's own device ID, or the broadcast ID every unit answers */
  if (data[1] != 0x10 && data[1] != 0x7f)
    return false;
  for (i = 4; i < size; ++i)
    if (data[i] > 0x7f)
      return false;
  sum = 0;
  for (i = 4; i + 1 < size; ++i)
    sum += data[i];
  if (((unsigned)(-(int)sum) & 0x7fu) != data[size - 1])
    return false;
  address = ((uint32_t)data[4] << 16) | ((uint32_t)data[5] << 8) | data[6];
  /* Family 50 is the same map addressed to the other group. Normalising it
     to 40 against the opposite port keeps one set of address cases. */
  group = port;
  if ((address & 0xf00000u) == 0x500000u) {
    address = (address & 0x0fffffu) | 0x400000u;
    group = (uint8_t)(SC88_MIDI_PORT_COUNT - 1u - port);
  }
  /* A packet may carry several consecutive addresses. Each is applied in
     turn, so a packet naming one address this implementation does not know
     does not discard the rest of it. */
  for (i = 7; i + 1 < size; ++i)
    if (!sc88_device_sysex_write(device, group,
                                 address + (uint32_t)(i - 7), data[i]))
      ++device->unhandled_sysex;
  return true;
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
    case 91:
      state->reverb_send = data2;
      sc88_engine_set_part_reverb_send(&device->engine, part, data2);
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
    /* On a rhythm part a program change is ignored while the bank MSB is
       nonzero (SC88-OM, `04_protocol/program_bank.md`). The demo songs rely
       on it: they send CC0 48 and then program 48 on channel 10, which on
       hardware leaves the kit alone and here was switching it to ORCHESTRA. */
    if (device->engine.parts[part].rhythm_map && state->variation != 0)
      return true;
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
  /* The send bus grows to whatever block the caller asks for and is kept,
     so a render loop does not allocate per block. */
  if (frames > device->send_capacity) {
    float *grown = (float *)realloc(device->send_bus, frames * sizeof *grown);
    if (!grown) {
      sc88_engine_render(&device->engine, stereo, frames);
      return;
    }
    device->send_bus = grown;
    device->send_capacity = frames;
  }
  memset(device->send_bus, 0, frames * sizeof *device->send_bus);
  sc88_engine_render_with_send(&device->engine, stereo, device->send_bus,
                               frames);
  if (device->reverb.active)
    sc88_reverb_process(&device->reverb, device->send_bus, stereo, frames);
}
