/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_wave.h"

#include <limits.h>

static uint16_t sc88_be16(const uint8_t *p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t sc88_be24(const uint8_t *p)
{
  return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static int16_t sc88_s16(uint16_t value)
{
  return value <= INT16_MAX
    ? (int16_t)value
    : (int16_t)(-1 - (int32_t)(UINT16_MAX - value));
}

bool sc88_wave_descriptor_parse(const uint8_t *raw, size_t size,
                                struct sc88_wave_descriptor *out)
{
  if (!raw || !out || size < SC88_WAVE_DESCRIPTOR_SIZE)
    return false;

  out->bank_select = raw[0];
  out->address_a = sc88_be24(raw + 1);
  out->base_pitch_correction = sc88_s16(sc88_be16(raw + 4));
  out->root_key = raw[6];
  out->address_b = sc88_be24(raw + 7);
  out->control = raw[10];
  out->address_c = sc88_be24(raw + 11);
  out->alternate_pitch_correction = sc88_s16(sc88_be16(raw + 14));
  out->start_offset = sc88_be16(raw + 16);
  out->state_a = sc88_s16(sc88_be16(raw + 18));
  return true;
}

bool sc88_wave_descriptor_loop_type(const struct sc88_wave_descriptor *desc,
                                    enum sc88_wave_loop_type *out)
{
  if (!desc || !out)
    return false;

  switch (desc->control) {
  case 0x00:
    *out = SC88_WAVE_FORWARD_LOOP;
    return true;
  case 0x10:
    *out = SC88_WAVE_PING_PONG_LOOP;
    return true;
  case 0x80:
    *out = SC88_WAVE_FORWARD_ONE_SHOT;
    return true;
  case 0x88:
    *out = SC88_WAVE_REVERSE_ONE_SHOT;
    return true;
  default:
    return false;
  }
}

int16_t sc88_wave_pitch_correction(const struct sc88_wave_descriptor *desc,
                                   bool alternate)
{
  uint16_t value;

  if (!desc)
    return 0;
  value = (uint16_t)desc->base_pitch_correction;
  if (alternate)
    value = (uint16_t)(value + (uint16_t)desc->alternate_pitch_correction);
  return sc88_s16(value);
}

bool sc88_wave_prepare_registers(const struct sc88_wave_descriptor *desc,
                                 bool suppress_start_offset,
                                 struct sc88_wave_registers *out)
{
  uint32_t first;
  uint32_t third;

  if (!desc || !out || desc->address_a >= SC88_WAVE_BANK_SIZE ||
      desc->address_b >= SC88_WAVE_BANK_SIZE ||
      desc->address_c >= SC88_WAVE_BANK_SIZE)
    return false;

  out->bank_flags = UINT32_C(0x8000) |
    ((uint32_t)(desc->control & 0x7f) << 8) | desc->bank_select;
  out->state_a = 0;

  if (desc->control & 0x08) {
    if ((desc->control & 0x80) && desc->address_a == 0)
      return false;
    first = desc->address_c;
    third = desc->address_a;
    if (desc->control & 0x80)
      --third;
  } else {
    first = desc->address_a;
    if (!suppress_start_offset) {
      if (desc->start_offset >= SC88_WAVE_BANK_SIZE - first)
        return false;
      first += desc->start_offset;
      out->state_a = ((desc->state_a < 0 ? UINT32_C(3) : 0) << 16) |
        (uint16_t)desc->state_a;
    }
    third = desc->address_c;
  }

  out->start = first;
  out->end = third;
  out->loop = (desc->control & 0x80) ? third : desc->address_b;
  out->initial_state = UINT32_C(0x18);
  return true;
}

bool sc88_fce_decoder_reset(struct sc88_fce_decoder *decoder,
                            uint32_t sample_start)
{
  if (!decoder || sample_start >= SC88_WAVE_BANK_SIZE)
    return false;
  decoder->next_address = sample_start & ~UINT32_C(0x0f);
  decoder->accumulator = 0;
  return true;
}

bool sc88_fce_decoder_read(struct sc88_fce_decoder *decoder,
                           const uint8_t *bank, size_t bank_size,
                           int32_t *pcm24)
{
  uint32_t address;
  uint8_t packed;
  unsigned exponent;
  int32_t delta;
  int64_t scaled;

  if (!decoder || !bank || !pcm24 || bank_size < SC88_WAVE_BANK_SIZE ||
      decoder->next_address >= SC88_WAVE_BANK_SIZE)
    return false;

  address = decoder->next_address++;
  delta = bank[address] <= INT8_MAX
    ? bank[address]
    : (int32_t)bank[address] - 256;
  packed = bank[address >> 5];
  exponent = (address & 0x10) ? packed >> 4 : packed & 0x0f;
  decoder->accumulator += (int64_t)delta * (INT64_C(1) << exponent);
  scaled = decoder->accumulator * INT64_C(128);
  if (scaled > INT32_C(0x7fffff))
    scaled = INT32_C(0x7fffff);
  else if (scaled < -INT32_C(0x800000))
    scaled = -INT32_C(0x800000);
  *pcm24 = (int32_t)scaled;
  return true;
}

bool sc88_fce_decode_descriptor(const uint8_t *bank, size_t bank_size,
                                const struct sc88_wave_descriptor *desc,
                                int32_t *output, size_t capacity,
                                size_t *written)
{
  struct sc88_fce_decoder decoder;
  uint32_t address;
  size_t count;
  size_t index = 0;
  int32_t sample;

  if (written)
    *written = 0;
  if (!bank || !desc || !output || !written ||
      desc->address_a > desc->address_c ||
      desc->address_c >= SC88_WAVE_BANK_SIZE)
    return false;

  count = (size_t)(desc->address_c - desc->address_a) + 1;
  if (capacity < count ||
      !sc88_fce_decoder_reset(&decoder, desc->address_a))
    return false;

  for (address = decoder.next_address; address <= desc->address_c; ++address) {
    if (!sc88_fce_decoder_read(&decoder, bank, bank_size, &sample))
      return false;
    if (address >= desc->address_a)
      output[index++] = sample;
  }
  *written = index;
  return true;
}

bool sc88_wave_cursor_init(struct sc88_wave_cursor *cursor,
                           const struct sc88_wave_registers *registers,
                           enum sc88_wave_loop_type mode)
{
  if (!cursor || !registers || registers->start >= SC88_WAVE_BANK_SIZE ||
      registers->loop >= SC88_WAVE_BANK_SIZE ||
      registers->end >= SC88_WAVE_BANK_SIZE ||
      mode == SC88_WAVE_REVERSE_ONE_SHOT)
    return false;
  if (registers->start > registers->end ||
      (mode != SC88_WAVE_FORWARD_ONE_SHOT && registers->loop > registers->end))
    return false;

  cursor->position = registers->start;
  cursor->loop = registers->loop;
  cursor->end = registers->end;
  cursor->mode = mode;
  cursor->direction = 1;
  cursor->ended = false;
  return true;
}

bool sc88_wave_cursor_current(const struct sc88_wave_cursor *cursor,
                              uint32_t *address)
{
  if (!cursor || !address || cursor->ended)
    return false;
  *address = cursor->position;
  return true;
}

bool sc88_wave_cursor_advance(struct sc88_wave_cursor *cursor)
{
  if (!cursor || cursor->ended)
    return false;

  switch (cursor->mode) {
  case SC88_WAVE_FORWARD_LOOP:
    cursor->position = cursor->position == cursor->end
      ? cursor->loop : cursor->position + 1;
    return true;

  case SC88_WAVE_PING_PONG_LOOP:
    if (cursor->direction > 0) {
      if (cursor->position == cursor->end)
        cursor->direction = -1;
      else
        ++cursor->position;
    } else {
      if (cursor->position == cursor->loop)
        cursor->direction = 1;
      else
        --cursor->position;
    }
    return true;

  case SC88_WAVE_FORWARD_ONE_SHOT:
    if (cursor->position == cursor->end)
      cursor->ended = true;
    else
      ++cursor->position;
    return true;

  case SC88_WAVE_REVERSE_ONE_SHOT:
    break;
  }
  return false;
}
