/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_SC88_WAVE_H
#define EMUSC_SC88_WAVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SC88_WAVE_DESCRIPTOR_SIZE 20u
#define SC88_WAVE_BANK_SIZE 0x100000u
#define SC88_WAVE_CHIP_SIZE 0x200000u
#define SC88_WAVE_SAMPLE_RATE 32000u

enum sc88_wave_loop_type {
  SC88_WAVE_FORWARD_LOOP = 0,
  SC88_WAVE_PING_PONG_LOOP = 1,
  SC88_WAVE_FORWARD_ONE_SHOT = 2,
  SC88_WAVE_REVERSE_ONE_SHOT = 6
};

struct sc88_wave_descriptor {
  uint8_t bank_select;
  uint32_t address_a;
  int16_t base_pitch_correction;
  uint8_t root_key;
  uint32_t address_b;
  uint8_t control;
  uint32_t address_c;
  int16_t alternate_pitch_correction;
  uint16_t start_offset;
  int16_t state_a;
};

struct sc88_wave_registers {
  uint32_t bank_flags;
  uint32_t start;
  uint32_t loop;
  uint32_t end;
  uint32_t state_a;
  uint32_t initial_state;
};

struct sc88_fce_decoder {
  uint32_t next_address;
  int64_t accumulator;
};

struct sc88_wave_cursor {
  uint32_t position;
  uint32_t loop;
  uint32_t end;
  enum sc88_wave_loop_type mode;
  int direction;
  bool ended;
};

bool sc88_wave_descriptor_parse(const uint8_t *raw, size_t size,
                                struct sc88_wave_descriptor *out);
bool sc88_wave_descriptor_loop_type(const struct sc88_wave_descriptor *desc,
                                    enum sc88_wave_loop_type *out);
int16_t sc88_wave_pitch_correction(const struct sc88_wave_descriptor *desc,
                                   bool alternate);
bool sc88_wave_prepare_registers(const struct sc88_wave_descriptor *desc,
                                 bool suppress_start_offset,
                                 struct sc88_wave_registers *out);

/* Undo the SC-88 board's 21 address-line and eight data-line permutations.
 * One physical dump contains two logical one-MiB banks. Source and destination
 * must not overlap. The two 32-byte plaintext headers pass through unchanged. */
bool sc88_wave_descramble_chip(const uint8_t *raw, size_t raw_size,
                               uint8_t *decoded, size_t decoded_size);

bool sc88_fce_decoder_reset(struct sc88_fce_decoder *decoder,
                            uint32_t sample_start);
bool sc88_fce_decoder_read(struct sc88_fce_decoder *decoder,
                           const uint8_t *bank, size_t bank_size,
                           int32_t *pcm24);
bool sc88_fce_decode_descriptor(const uint8_t *bank, size_t bank_size,
                                const struct sc88_wave_descriptor *desc,
                                int32_t *output, size_t capacity,
                                size_t *written);

/* Integer-address playback only. Fractional phase conversion, interpolation
 * rounding and reverse one-shot termination deliberately remain outside this
 * API until their XP semantics are recovered or explicitly parameterized. */
bool sc88_wave_cursor_init(struct sc88_wave_cursor *cursor,
                           const struct sc88_wave_registers *registers,
                           enum sc88_wave_loop_type mode);
bool sc88_wave_cursor_current(const struct sc88_wave_cursor *cursor,
                              uint32_t *address);
bool sc88_wave_cursor_advance(struct sc88_wave_cursor *cursor);

#ifdef __cplusplus
}
#endif

#endif
