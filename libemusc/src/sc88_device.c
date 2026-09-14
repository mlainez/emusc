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
  /* GS pan 0 is RANDOM, and is passed through as zero: the engine draws a
     fresh 1..127 position for each voice, which is what spreads a part
     across the image instead of stacking it in the centre. */
  pan.part = channel->pan;
  if (channel->pan == 0)
    ++device->random_pan_requests;
  sc88_engine_set_part_levels(&device->engine, part, &levels);
  sc88_engine_set_part_pan(&device->engine, part, &pan);
}

static void sc88_device_sync_chorus(struct sc88_device *device)
{
  sc88_chorus_set_params(&device->renderer.rom, &device->chorus,
                         device->chorus_level, device->chorus_feedback,
                         device->chorus_delay, device->chorus_rate,
                         device->chorus_depth, device->chorus_pre_lpf);
}

static void sc88_device_sync_lfo(struct sc88_device *device, uint8_t part)
{
  const struct sc88_channel_state *channel = device->channels + part;
  struct sc88_lfo_controls controls;
  controls.rate = channel->vibrato_rate;
  controls.delay = channel->vibrato_delay;
  controls.depth = channel->vibrato_depth;
  sc88_engine_set_part_lfo_controls(&device->engine, part, &controls);
}

static void sc88_device_sync_eq(struct sc88_device *device)
{
  (void)sc88_eq_set_params(&device->renderer.rom, &device->eq,
                           device->eq_low_frequency, device->eq_low_gain,
                           device->eq_high_frequency, device->eq_high_gain);
}

static void sc88_device_sync_pitch(struct sc88_device *device, uint8_t part)
{
  const struct sc88_channel_state *channel = device->channels + part;
  int64_t numerator = ((int32_t)channel->pitch_bend - 8192) *
    (int32_t)channel->pitch_bend_sensitivity * 16384;
  int32_t offset = (int32_t)(numerator / (8192 * 12));
  /* 0x4000 pitch-word units to the octave */
  offset += ((int32_t)channel->key_shift - 64) * 16384 / 12;
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

static void sc88_device_sync_tva(struct sc88_device *device, uint8_t part)
{
  const struct sc88_channel_state *channel = device->channels + part;
  struct sc88_tva_controls controls;
  controls.part_attack = channel->attack;
  controls.secondary_attack = 64;
  controls.part_decay = channel->decay;
  controls.secondary_decay = 64;
  sc88_engine_set_part_tva_controls(&device->engine, part, &controls);
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
  device->output_rate = output_rate;
  sc88_output_init(&device->output, output_rate);
  sc88_renderer_set_tvf_audio_transfer(
    &device->renderer, sc88_tvf_audio_process_provisional,
    &device->output_rate);
  if (!sc88_chorus_init(&device->chorus, output_rate))
    goto fail;
  if (!sc88_delay_init(&device->delay, output_rate))
    goto fail;
  sc88_eq_init(&device->eq);
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
  free(device->chorus_bus);
  free(device->delay_bus);
  sc88_chorus_destroy(&device->chorus);
  sc88_delay_destroy(&device->delay);
  for (chip = 0; chip < SC88_WAVE_CHIP_COUNT; ++chip)
    free(device->decoded_chips[chip]);
  free(device->control_rom);
  memset(device, 0, sizeof *device);
}

static bool sc88_device_load_reverb_macro(struct sc88_device *device,
                                          uint8_t macro);

void sc88_device_reset_controllers(struct sc88_device *device)
{
  unsigned part;
  if (!device || !device->initialized)
    return;
  device->master_volume = 127;
  device->secondary_level = 127;
  device->master_pan = 64;
  /* The reverb block a reset leaves behind is macro 4's own preset row.
     The power-on image at ROM 0x13104 - the patch common block, recognisable
     by the default patch name in its first sixteen bytes - holds
     `04 04 00 40 40 00 00 00` there: the macro, then the seven bytes macro 4
     copies. So this is a table read, not a second set of constants. */
  (void)sc88_device_load_reverb_macro(device, 4);
  sc88_reverb_reset(&device->reverb);
  /* The manual's own chorus defaults. */
  device->chorus_macro = 2;
  device->chorus_pre_lpf = 0;
  device->chorus_level = 64;
  device->chorus_feedback = 8;
  device->chorus_delay = 80;
  device->chorus_rate = 3;
  device->chorus_depth = 19;
  device->chorus_send_to_reverb = 0;
  sc88_device_sync_chorus(device);
  sc88_chorus_reset(&device->chorus);
  /* Delay macro 0 and the ten bytes it copies over pre-LPF through reverb
     send, which is what writing the macro address does. */
  /* Both corners at their first setting and both gains at the centre,
     which is the identity, and enabled - as `40 4x 20` defaults to. */
  device->eq_low_frequency = 0;
  device->eq_high_frequency = 0;
  device->eq_low_gain = 0x40;
  device->eq_high_gain = 0x40;
  device->eq.enabled = true;
  sc88_device_sync_eq(device);
  sc88_eq_reset(&device->eq);
  sc88_output_reset(&device->output);
  device->delay_macro = 0;
  if (sc88_delay_macro(&device->renderer.rom, 0, device->delay_params))
    (void)sc88_delay_set_params(&device->renderer.rom, &device->delay,
                                device->delay_params);
  sc88_delay_reset(&device->delay);
  for (part = 0; part < SC88_ENGINE_PART_COUNT; ++part) {
    struct sc88_channel_state *channel = device->channels + part;
    channel->variation = 0;
    channel->map_lsb = 0;
    channel->program = 0;
    channel->volume = 100;
    channel->expression = 127;
    channel->pan = 64;
    channel->hold1 = 0;
    /* GS's own default part reverb send is 40, and its chorus send 0
       (SC88-OM), so a song that wants chorus has to ask for it. */
    channel->reverb_send = 40;
    sc88_engine_set_part_reverb_send(&device->engine, (uint8_t)part, 40);
    channel->chorus_send = 0;
    sc88_engine_set_part_chorus_send(&device->engine, (uint8_t)part, 0);
    channel->delay_send = 0;
    sc88_engine_set_part_delay_send(&device->engine, (uint8_t)part, 0);
    channel->cutoff = 64;
    channel->resonance = 64;
    channel->attack = 64;
    channel->decay = 64;
    channel->release = 64;
    channel->modulation = 0;
    channel->key_shift = 64;
    channel->cc1_assign = 0x10;
    channel->cc2_assign = 0x11;
    channel->cc1_value = 0;
    channel->cc2_value = 0;
    channel->mono_mode = 1;
    channel->vibrato_rate = 64;
    channel->vibrato_depth = 64;
    channel->vibrato_delay = 64;
    sc88_device_sync_lfo(device, (uint8_t)part);
    channel->portamento_time = 0;
    channel->portamento_switch = 0;
    channel->portamento_control = 0;
    /* SC88-OM's initial modulation depths: LFO1 pitch 0x0a, the rest zero */
    channel->mod_lfo1_pitch_depth = 0x0a;
    sc88_engine_set_part_lfo1_pitch_depth(&device->engine, (uint8_t)part, 0);
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
    sc88_engine_hold(&device->engine, (uint8_t)part, false);
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
  /* A render that has the effects switched off keeps them off across a
     character change; a freshly built reverb comes up enabled. */
  replacement.active = device->reverb.active;
  sc88_reverb_destroy(&device->reverb);
  device->reverb = replacement;
  device->reverb_character = character;
  sc88_reverb_set_params(&device->reverb, device->reverb_level,
                         device->reverb_time, device->reverb_pre_lpf);
  return true;
}

/* Writing the reverb macro address copies the preset's seven bytes over
   character, pre-LPF, level, time, delay feedback, the reserved byte and
   predelay. SC88-CTL handler 0x3388 is the delay handler 0x342b's sibling:
   both clamp the written byte through a bounds record, store it, and then,
   only when the index is zero, copy the rest of the block out of a ROM table
   (`0x1583e + 8*macro` here, `0x158be + 16*macro` there) through the same
   pair of copy helpers. The power-on loader at 0x4476 reads the same table. */
static bool sc88_device_load_reverb_macro(struct sc88_device *device,
                                          uint8_t macro)
{
  uint8_t p[7];
  if (macro > 7 || !sc88_reverb_macro(&device->renderer.rom, macro, p))
    return false;
  device->reverb_macro = macro;
  if (!sc88_device_set_reverb_character(device, p[0] > 7 ? 7 : p[0]))
    return false;
  device->reverb_pre_lpf = p[1] > 7 ? 7 : p[1];
  device->reverb_level = p[2];
  device->reverb_time = p[3];
  device->reverb_delay_feedback = p[4];
  /* p[5] is the block's reserved byte; `40 01 36` has no parameter and the
     firmware refuses a write to it. */
  device->reverb_predelay = p[6];
  sc88_reverb_set_predelay(&device->reverb, device->reverb_predelay);
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
  case 0x400130:
    /* The macro reloads the whole block from its preset; see the loader. */
    return value <= 7 && sc88_device_load_reverb_macro(device, value);
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
  case 0x400137:
    /* Predelay, in milliseconds, single-module only. */
    if (value > 127)
      return false;
    device->reverb_predelay = value;
    sc88_reverb_set_predelay(&device->reverb, value);
    return true;
  case 0x400135:
    device->reverb_delay_feedback = value;
    return true;
  case 0x400134:
    device->reverb_time = value;
    sc88_reverb_set_params(&device->reverb, device->reverb_level,
                           device->reverb_time, device->reverb_pre_lpf);
    return true;
  /* The macro's eight names are the manual's chorus types. Which fields
     each preset loads is not recovered - the delay macro demonstrably
     copies a ten-byte preset, and this one may too - so the macro is held
     and the fields it would carry are left to the song, which sends them. */
  case 0x400200:
    if (value > 1)
      return false;
    device->eq_low_frequency = value;
    sc88_device_sync_eq(device);
    return true;
  case 0x400201:
    if (value < 0x34 || value > 0x4c)
      return false;
    device->eq_low_gain = value;
    sc88_device_sync_eq(device);
    return true;
  case 0x400202:
    if (value > 1)
      return false;
    device->eq_high_frequency = value;
    sc88_device_sync_eq(device);
    return true;
  case 0x400203:
    if (value < 0x34 || value > 0x4c)
      return false;
    device->eq_high_gain = value;
    sc88_device_sync_eq(device);
    return true;
  case 0x400150:
    /* Writing the macro copies its ten bytes over pre-LPF through reverb
       send; writing any other field changes only that field. */
    if (value > 9)
      return false;
    device->delay_macro = value;
    if (!sc88_delay_macro(&device->renderer.rom, value,
                          device->delay_params))
      return false;
    return sc88_delay_set_params(&device->renderer.rom, &device->delay,
                                 device->delay_params);
  case 0x400151: case 0x400152: case 0x400153: case 0x400154:
  case 0x400155: case 0x400156: case 0x400157: case 0x400158:
  case 0x400159: case 0x40015a:
    device->delay_params[address - 0x400151u] = value;
    return sc88_delay_set_params(&device->renderer.rom, &device->delay,
                                 device->delay_params);
  case 0x400138:
    device->chorus_macro = value;
    return true;
  case 0x400139:
    if (value > 7)
      return false;
    device->chorus_pre_lpf = value;
    sc88_device_sync_chorus(device);
    return true;
  case 0x40013a:
    device->chorus_level = value;
    sc88_device_sync_chorus(device);
    return true;
  case 0x40013b:
    device->chorus_feedback = value;
    sc88_device_sync_chorus(device);
    return true;
  case 0x40013c:
    device->chorus_delay = value;
    sc88_device_sync_chorus(device);
    return true;
  case 0x40013d:
    device->chorus_rate = value;
    sc88_device_sync_chorus(device);
    return true;
  case 0x40013e:
    device->chorus_depth = value;
    sc88_device_sync_chorus(device);
    return true;
  case 0x40013f:
    device->chorus_send_to_reverb = value;
    return true;
  default:
    break;
  }
  /* `41 mf rr`: one per-note kit parameter. m is the map, f the field,
     rr the note; `41 m0 00` is the kit's twelve-byte name, which has no
     bearing on the sound. */
  if ((address & 0xff0000u) == 0x410000u) {
    uint8_t map = (uint8_t)(((address >> 12) & 0x0fu) + 1u);
    uint8_t field = (uint8_t)((address >> 8) & 0x0fu);
    uint8_t note = (uint8_t)(address & 0xffu);
    if (field == 0)
      return true;                     /* the name */
    return sc88_engine_set_drum_parameter(&device->engine, map, field,
                                          note, value);
  }
  if ((address & 0xf0ff00u) == 0x402000u && (address & 0xffu) == 0x20u) {
    /* `40 4x 20`, the equaliser switch. It is one global effect, so any
       block's write governs it. */
    if (value > 1)
      return false;
    device->eq.enabled = value != 0;
    return true;
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
    case 0x30:
      state->vibrato_rate = value;
      sc88_device_sync_lfo(device, part);
      return true;
    case 0x31:
      state->vibrato_depth = value;
      sc88_device_sync_lfo(device, part);
      return true;
    case 0x37:
      state->vibrato_delay = value;
      sc88_device_sync_lfo(device, part);
      return true;
    case 0x13:
      if (value > 1)
        return false;
      state->mono_mode = value;
      return true;
    case 0x1f:
      if (value > 0x5f)
        return false;
      state->cc1_assign = value;
      return true;
    case 0x20:
      if (value > 0x5f)
        return false;
      state->cc2_assign = value;
      return true;
    case 0x16:
      if (value < 0x28 || value > 0x58)
        return false;
      state->key_shift = value;
      sc88_device_sync_pitch(device, part);
      return true;
    case 0x19:
      state->volume = value;
      sc88_device_sync_part(device, part);
      return true;
    case 0x1c:
      state->pan = value;
      sc88_device_sync_part(device, part);
      return true;
    case 0x34:
      state->attack = value;
      sc88_device_sync_tva(device, part);
      return true;
    case 0x35:
      state->decay = value;
      sc88_device_sync_tva(device, part);
      return true;
    case 0x36:
      state->release = value;
      return true;
    case 0x21:
      state->chorus_send = value;
      sc88_engine_set_part_chorus_send(&device->engine, part, value);
      return true;
    case 0x2c:
      state->delay_send = value;
      sc88_engine_set_part_delay_send(&device->engine, part, value);
      return true;
    case 0x22:
      state->reverb_send = value;
      sc88_engine_set_part_reverb_send(&device->engine, part, value);
      return true;
    case 0x25:
      /* Not in any held parameter map. Skatey Eight writes it once. It is
         accepted and counted so the packet is not refused wholesale, and
         so the count says something is being carried and discarded. */
      ++device->unhandled_sysex;
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
    case 1:
      /* The controller matrix forms each LFO depth destination as the sum
         of `depth * value` over its sources, shifted right by two
         (`04_protocol/controllers.md`). Modulation is the only source
         modelled, so the sum is the one term. */
      state->modulation = data2;
      sc88_engine_set_part_lfo1_pitch_depth(
        &device->engine, part,
        (uint16_t)(((unsigned)state->mod_lfo1_pitch_depth * data2) >> 2));
      return true;
    case 91:
      state->reverb_send = data2;
      sc88_engine_set_part_reverb_send(&device->engine, part, data2);
      return true;
    case 94:
      state->delay_send = data2;
      sc88_engine_set_part_delay_send(&device->engine, part, data2);
      return true;
    case 5:
      state->portamento_time = data2;
      return true;
    case 65:
      state->portamento_switch = data2;
      return true;
    case 84:
      state->portamento_control = data2;
      return true;
    case 126:
      state->mono_mode = 0;
      return true;
    case 127:
      state->mono_mode = 1;
      return true;
    case 93:
      state->chorus_send = data2;
      sc88_engine_set_part_chorus_send(&device->engine, part, data2);
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
      /* The envelope-time modifiers. `07_synthesis/tva.md` gives the law:
         stages 0 and 1 take the attack modifier, stages 2 and 3 the decay
         one, doubled about their centre. Release is held but not applied -
         the release path's own part modifier is not recovered. */
      if (state->nrpn_msb == 1 && state->nrpn_lsb == 0x63) {
        state->attack = data2;
        sc88_device_sync_tva(device, part);
        return true;
      }
      if (state->nrpn_msb == 1 && state->nrpn_lsb == 0x64) {
        state->decay = data2;
        sc88_device_sync_tva(device, part);
        return true;
      }
      if (state->nrpn_msb == 1 && state->nrpn_lsb == 0x66) {
        state->release = data2;
        return true;
      }
      /* The drum-instrument block: the MSB names the parameter and the LSB
         is the note. It reaches the same per-note overrides the `41 mf rr`
         addresses do, except that its pitch is a centred offset. */
      if (state->nrpn_msb >= 0x18 && state->nrpn_msb <= 0x1f) {
        static const uint8_t field_of[8] = {
          10u,  /* 18 pitch, relative */
          0u,   /* 19 unassigned */
          2u,   /* 1a TVA level */
          0u,   /* 1b unassigned */
          4u,   /* 1c panpot */
          5u,   /* 1d reverb send */
          6u,   /* 1e chorus send */
          9u    /* 1f delay send */
        };
        uint8_t field = field_of[state->nrpn_msb - 0x18u];
        uint8_t map = device->engine.parts[part].rhythm_map;
        if (!field || !map)
          return false;
        return sc88_engine_set_drum_parameter(&device->engine, map, field,
                                              state->nrpn_lsb, data2);
      }
      if (state->nrpn_msb == 1 && state->nrpn_lsb == 0x08) {
        state->vibrato_rate = data2;
        sc88_device_sync_lfo(device, part);
        return true;
      }
      if (state->nrpn_msb == 1 && state->nrpn_lsb == 0x09) {
        state->vibrato_depth = data2;
        sc88_device_sync_lfo(device, part);
        return true;
      }
      if (state->nrpn_msb == 1 && state->nrpn_lsb == 0x0a) {
        state->vibrato_delay = data2;
        sc88_device_sync_lfo(device, part);
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
      /* Two things, and only the second was being done: the pedal both
         retains notes past Note Off and scales their release. */
      state->hold1 = data2;
      sc88_engine_hold(&device->engine, part, data2 >= 64);
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
      /* An ordinary controller below CC120 is compared with this part's
         CC1/CC2 assignment before being refused, which is the order
         `04_protocol/controllers.md` gives. Every matrix depth those two
         feed is zero after a reset, so this receives them and applies the
         zero. */
      if (data1 < 120) {
        if (data1 == state->cc1_assign) {
          state->cc1_value = data2;
          return true;
        }
        if (data1 == state->cc2_assign) {
          state->cc2_value = data2;
          return true;
        }
      }
      return false;
    }
  case 0xc0:
    if (device->engine.parts[part].rhythm_map &&
        data1 != state->program)
      sc88_engine_clear_drum_overlay(&device->engine,
                                     device->engine.parts[part].rhythm_map);
    /* A rhythm part's program change is honoured whatever the bank MSB is.
       `04_protocol/program_bank.md` records the firmware as ignoring it
       while the MSB is nonzero, but the music contradicts that: Brass
       Nation is a full orchestra - tuba, horns, trombone, five string
       parts, flute, oboe, clarinet, bassoon, piccolo, harp, timpani,
       tubular bell, celesta, xylophone, glockenspiel - and its drum part
       asks for program 48, which is the ORCHESTRA kit in both maps. Opus 88
       asks for program 49, whose ETHNIC kit exists only in the SC-88 map,
       with a zero MSB. Honouring it satisfies both; ignoring it leaves an
       orchestra playing a pop kit (`M-024`). */
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
    float *grown_chorus;
    if (!grown) {
      sc88_engine_render(&device->engine, stereo, frames);
      return;
    }
    device->send_bus = grown;
    grown_chorus = (float *)realloc(device->chorus_bus,
                                    frames * sizeof *grown_chorus);
    if (!grown_chorus) {
      sc88_engine_render(&device->engine, stereo, frames);
      return;
    }
    device->chorus_bus = grown_chorus;
    {
      float *grown_delay = (float *)realloc(device->delay_bus,
                                            frames * sizeof *grown_delay);
      if (!grown_delay) {
        sc88_engine_render(&device->engine, stereo, frames);
        return;
      }
      device->delay_bus = grown_delay;
    }
    device->send_capacity = frames;
  }
  memset(device->send_bus, 0, frames * sizeof *device->send_bus);
  memset(device->chorus_bus, 0, frames * sizeof *device->chorus_bus);
  memset(device->delay_bus, 0, frames * sizeof *device->delay_bus);
  sc88_engine_render_with_send(&device->engine, stereo, device->send_bus,
                               device->chorus_bus, device->delay_bus,
                               frames);
  /* The delay runs before the reverb reads its bus, because it has its own
     send into the reverb and that send is recovered. */
  if (device->delay.active)
    sc88_delay_process(&device->delay, device->delay_bus, stereo,
                       device->send_bus, frames);
  /* The chorus runs before the reverb reads its bus, because the chorus
     has its own send into the reverb; that send is received and held but
     not yet routed, so the two effects are still parallel here. */
  if (device->chorus.active)
    sc88_chorus_process(&device->chorus, device->chorus_bus, stereo, frames);
  if (device->reverb.active)
    sc88_reverb_process(&device->reverb, device->send_bus, stereo, frames);
  sc88_eq_process(&device->eq, stereo, frames);
  /* Last: the output stage. Everything above this line is the digital
     machine; sc88_output.c is the one place that carries behaviour we
     have measured but not derived. */
  sc88_output_process(&device->output, stereo, frames);
}
