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
bool sc88_fce_decode_storage(const uint8_t *bank, size_t bank_size,
                             const struct sc88_wave_descriptor *desc,
                             int32_t *output, size_t capacity,
                             uint32_t *base_address, size_t *written);

/* The thirty organ descriptors the SC-88 reads at twice the rate.

   The behaviour is settled and measured against hardware, and the mechanism
   is NOT recovered.  The Drawbar Organ C3 recording carries the nine Hammond
   drawbar footages with 40 dB holes at harmonics 5, 7, 9 and 11; the ROM loop
   the key-48 zone selects reproduces that hole pattern only when the loop is
   read at 2x: on Percussive Organ C3 the fifth harmonic is -54.2 dB against
   the recording's -55.2 read at 2x, and 16.2 dB out read at 1x.  Read at 1x, which is what the
   arithmetic in `sc88_renderer_static_pitch_word` gives, the registration
   moves half an octave down, the holes fill, and both organs sound an octave
   below the note.

   This predicate is a model of that BEHAVIOUR, not of the machine.  Neither
   the H8/520 nor the XP can compute it at note-on: it is a property of the
   decoded waveform, and the CPU never decodes a sample.  Three lanes searched
   for the mechanism and closed every source this project can read - all 160
   descriptor bits, the whole 512 KB control ROM and all eight 1 MiB wave banks
   at every base and strides 1/2/4, the SC-88 Pro's own firmware and descriptor
   table, the seven demo songs, and Sound Canvas VA's records and code.  The
   sibling device says what the missing field would look like and that the
   SC-88 does not have it: on the JV-880 the Marimba samples carry the same
   mismatch the other way up - their loops hold half a root-key period per
   waveform cycle - and its tone record reconciles it with a stored coarse
   tune of -12 semitones at +37.  The SC-88 has that field, component +0x16,
   with a -36..+24 semitone range that 65 components use; it reads zero on
   every organ component, and it is per-component while the defect is
   per-zone - E.Organ 1's single component reaches two clean zones and nine
   affected ones.

   The rule, on the loop's own decoded content, with `L` the loop length and
   `root` the descriptor's root key:

     P = 32000 / (440 * 2^((root - 69) / 12))    root-key period, in samples
     u = L / P                                   root-key cycles in the loop
     h = u / 2

   `u` must be within 0.02 of an even integer at least two, so that bin `h`
   exists.  Then, with `A_k` the loop's DFT magnitude at bin k:

     the loudest bin below h is at most -15 dB relative to A_h, and
     A_h is at least -7 dB relative to A_u.

   Together: the loop is a harmonic tone whose own fundamental sits an octave
   below its root pitch.  Both thresholds are the middle of a measured gap
   over the 113 forward-looping descriptors whose loop spans an even whole
   number of root-key cycles.  The first clause separates -26.6 dB (the worst
   of the thirty) from +4.5 dB (the best of the rest), 31.1 dB wide; the
   second separates -2.45 dB from -11.35 dB, 8.90 dB wide.  The selection is
   exactly those thirty for every first threshold in -25..-6 dB crossed with
   every second in -11.3..-2.5 dB.

   `pcm` is the decoded sample, `base_address` the wave address its first
   entry holds. */
bool sc88_wave_loop_reads_double(const int32_t *pcm, size_t count,
                                 uint32_t base_address,
                                 const struct sc88_wave_descriptor *desc);

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
