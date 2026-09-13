/* SPDX-License-Identifier: CC0-1.0 */
#include "sc88_wave.h"

#include <limits.h>
#include <math.h>

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

static uint8_t sc88_wave_descramble_byte(uint8_t value)
{
  static const uint8_t input_bit[8] = {2, 0, 4, 5, 7, 6, 3, 1};
  uint8_t result = 0;
  unsigned bit;
  for (bit = 0; bit < 8; ++bit)
    result |= (uint8_t)(((value >> input_bit[bit]) & 1u) << bit);
  return result;
}

static uint32_t sc88_wave_descramble_address(uint32_t value)
{
  static const uint8_t input_bit[21] = {
    0, 4, 2, 3, 1, 13, 7, 12, 5, 10, 16,
    9, 6, 8, 14, 17, 11, 15, 18, 19, 20
  };
  uint32_t result = 0;
  unsigned bit;
  for (bit = 0; bit < 21; ++bit)
    result |= ((value >> input_bit[bit]) & 1u) << bit;
  return result;
}

bool sc88_wave_descramble_chip(const uint8_t *raw, size_t raw_size,
                               uint8_t *decoded, size_t decoded_size)
{
  uint32_t source;

  if (!raw || !decoded || raw == decoded || raw_size != SC88_WAVE_CHIP_SIZE ||
      decoded_size < SC88_WAVE_CHIP_SIZE)
    return false;
  for (source = 0; source < SC88_WAVE_CHIP_SIZE; ++source) {
    const bool header = source < 0x20 ||
      (source >= SC88_WAVE_BANK_SIZE &&
       source < SC88_WAVE_BANK_SIZE + 0x20);
    decoded[header ? source : sc88_wave_descramble_address(source)] =
      header ? raw[source] : sc88_wave_descramble_byte(raw[source]);
  }
  return true;
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
    /* Ping-pong, and the ROM says so by itself.

       Read forward - which is what this returned until TASK-183 - every
       control-0x10 loop jumps in phase once per traversal, and that is the
       pop the owner reported on the French Horn for weeks. It is not one
       instrument: over the whole wave ROM, the correlation ACROSS the join
       of a loop played forward is +0.968 at the median for the 1518
       descriptors that carry 0x00 and +0.197 for the 215 that carry 0x10.
       The 0x00 loops tile; the 0x10 loops do not, and 0x10 is on exactly
       the material a designer cannot cut to a whole number of periods -
       Choir, Strings, Church Organ, French Horn, Tremolo Strings,
       Overdrive, Timpani.

       But a ping-pong turn in a DIFFERENTIAL format is not a time reversal.
       The decoder is an accumulator over deltas; running the address back
       down the stream while still adding what it reads gives

           y[m] = x[c] + d[c] + ... + d[c-m+1] = 2*x[c] - x[c-m]

       - the loop backwards AND reflected about the value it turned at,
       which is continuous in value and in SLOPE. A plain time reversal
       leaves a corner instead, and measures worse than the forward read,
       which is how the forward reading came to look right.

       The ROM is cut for the reflected turn: over all 1733 looping
       descriptors the deltas from address_b to address_c sum to EXACTLY
       zero, so x[c] == x[b-1] without a single exception. That is what
       lands the reflected pass on x[b-1] as its address reaches b-1, so
       the cycle closes with no step and no drift.

       Measured, French Horn C4, worst period-to-period correlation through
       the sustain: forward 0.780, plain time reversal -0.424, reflected
       0.987, against the hardware recording's 0.992. Over the eighteen
       control-0x10 instruments that have an SC-88 recording, p50 goes from
       +0.370 to +0.941 where the hardware's own is +0.926, and where the
       hardware dips we now dip with it. The 0x00 renders are unchanged
       byte for byte, and the corrected 63-instrument audit does not move:
       median MAD 2.1 dB, 46 within 3 dB, 7 past 6. */
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

bool sc88_fce_decode_storage(const uint8_t *bank, size_t bank_size,
                             const struct sc88_wave_descriptor *desc,
                             int32_t *output, size_t capacity,
                             uint32_t *base_address, size_t *written)
{
  struct sc88_fce_decoder decoder;
  uint32_t base;
  size_t count;
  size_t index;

  if (base_address)
    *base_address = 0;
  if (written)
    *written = 0;
  if (!bank || !desc || !output || !base_address || !written ||
      desc->address_a > desc->address_c ||
      desc->address_c >= SC88_WAVE_BANK_SIZE)
    return false;
  base = desc->address_a & ~UINT32_C(0x0f);
  count = (size_t)(desc->address_c - base) + 1;
  if (capacity < count || !sc88_fce_decoder_reset(&decoder, desc->address_a))
    return false;
  for (index = 0; index < count; ++index)
    if (!sc88_fce_decoder_read(&decoder, bank, bank_size, output + index))
      return false;
  *base_address = base;
  *written = count;
  return true;
}

/* Goertzel magnitude squared of `x[0..count)` at DFT bin `k` of a
   `count`-point transform.  Stable where a complex recurrence over several
   thousand samples is not. */
static double sc88_wave_bin_power(const int32_t *x, size_t count, size_t k)
{
  static const double two_pi = 6.283185307179586476925286766559;
  double omega = two_pi * (double)k / (double)count;
  double coeff = 2.0 * cos(omega);
  double s1 = 0.0;
  double s2 = 0.0;
  size_t n;

  for (n = 0; n < count; ++n) {
    double s0 = (double)x[n] + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

/* -15 dB and -7 dB as power ratios; see the note in the header for the
   measured gaps these sit in the middle of. */
#define SC88_WAVE_BELOW_POWER 0.0316227766016838
#define SC88_WAVE_PARTIAL_POWER 0.199526231496888

bool sc88_wave_loop_reads_double(const int32_t *pcm, size_t count,
                                 uint32_t base_address,
                                 const struct sc88_wave_descriptor *desc)
{
  enum sc88_wave_loop_type mode;
  double period;
  double cycles;
  double rounded;
  double at_half;
  double at_root;
  size_t length;
  size_t first;
  size_t half;
  size_t whole;
  size_t k;

  if (!pcm || !desc || !sc88_wave_descriptor_loop_type(desc, &mode) ||
      mode != SC88_WAVE_FORWARD_LOOP || desc->root_key > 127 ||
      desc->address_c <= desc->address_b || desc->address_b < base_address)
    return false;
  first = (size_t)(desc->address_b - base_address);
  length = (size_t)(desc->address_c - desc->address_b) + 1u;
  if (length < 16u || first > count || count - first < length)
    return false;

  /* How many periods of the note the descriptor claims fit in the loop. */
  period = (double)SC88_WAVE_SAMPLE_RATE /
    (440.0 * pow(2.0, ((double)desc->root_key - 69.0) / 12.0));
  cycles = (double)length / period;
  rounded = floor(cycles + 0.5);
  if (rounded < 2.0 || fabs(cycles - rounded) > 0.02)
    return false;
  whole = (size_t)rounded;
  if (whole % 2u)
    return false;
  half = whole / 2u;
  if (whole >= length / 2u)
    return false;

  at_half = sc88_wave_bin_power(pcm + first, length, half);
  at_root = sc88_wave_bin_power(pcm + first, length, whole);
  if (at_half <= 0.0)
    return false;

  /* Nothing below the half-root partial: it is the loop's own fundamental
     and not a bin of some inharmonic body.  This is what keeps the Melodic
     Tom out - its loop spans thirty root-key cycles and carries 37 dB more
     energy below bin fifteen than in it. */
  for (k = 1u; k < half; ++k) {
    if (sc88_wave_bin_power(pcm + first, length, k) >
        at_half * SC88_WAVE_BELOW_POWER)
      return false;
  }
  /* And it is a partial, not a seam artefact: the clean low zones of these
     same organ sets carry a bin there 30 dB down. */
  return at_half >= at_root * SC88_WAVE_PARTIAL_POWER;
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
