/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_XP_WAVE_H
#define EMUSC_XP_WAVE_H

#include "devices/sc88.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum xp_wave_loop_type {
  XP_WAVE_FORWARD_LOOP = 0,
  XP_WAVE_PING_PONG_LOOP = 1,
  XP_WAVE_FORWARD_ONE_SHOT = 2,
  XP_WAVE_REVERSE_ONE_SHOT = 6
};

struct xp_wave_descriptor {
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

struct xp_wave_registers {
  uint32_t bank_flags;
  uint32_t start;
  uint32_t loop;
  uint32_t end;
  uint32_t state_a;
  uint32_t initial_state;
};

struct xp_fce_decoder {
  uint32_t next_address;
  int64_t accumulator;
  const struct XpDeviceProfile *profile;
};

struct xp_wave_cursor {
  uint32_t position;
  uint32_t loop;
  uint32_t end;
  enum xp_wave_loop_type mode;
  int direction;
  bool ended;
};

#ifdef __cplusplus
}

namespace EmuSC { namespace Xp {

// Wave ROM decode/descramble toolkit for the XP-generation-1 engine
// (see engines/xp/README.md). Every law here comes from the SC-88's own
// dumped firmware and measured hardware; nothing is XP-generation-1
// behavior known to transfer to a sibling until measured on one.
//
// The plain C types above (xp_wave_descriptor and friends) are shared,
// unrenamed, with device_test.cc, which reads them directly.

bool wave_descriptor_parse(const struct XpDeviceProfile *profile,
                            const uint8_t *raw, size_t size,
                            struct xp_wave_descriptor *out);
bool wave_descriptor_loop_type(const struct xp_wave_descriptor *desc,
                                enum xp_wave_loop_type *out);
int16_t wave_pitch_correction(const struct xp_wave_descriptor *desc,
                               bool alternate);
bool wave_prepare_registers(const struct XpDeviceProfile *profile,
                             const struct xp_wave_descriptor *desc,
                             bool suppressStartOffset,
                             struct xp_wave_registers *out);

/* Undo the SC-88 board's 21 address-line and eight data-line permutations.
 * One physical dump contains two logical one-MiB banks. Source and
 * destination must not overlap. The two 32-byte plaintext headers pass
 * through unchanged. */
bool wave_descramble_chip(const struct XpDeviceProfile *profile,
                           const uint8_t *raw, size_t rawSize,
                           uint8_t *decoded, size_t decodedSize);

bool fce_decoder_reset(const struct XpDeviceProfile *profile,
                        struct xp_fce_decoder *decoder,
                        uint32_t sampleStart);
bool fce_decoder_read(struct xp_fce_decoder *decoder,
                       const uint8_t *bank, size_t bankSize,
                       int32_t *pcm24);
bool fce_decode_descriptor(const struct XpDeviceProfile *profile,
                            const uint8_t *bank, size_t bankSize,
                            const struct xp_wave_descriptor *desc,
                            int32_t *output, size_t capacity,
                            size_t *written);
bool fce_decode_storage(const struct XpDeviceProfile *profile,
                         const uint8_t *bank, size_t bankSize,
                         const struct xp_wave_descriptor *desc,
                         int32_t *output, size_t capacity,
                         uint32_t *baseAddress, size_t *written);

/* The thirty organ descriptors the SC-88 reads at twice the rate.
   See wave.cc for the full measurement this predicate encodes: it models
   the decoded waveform's own harmonic content, not the chip. */
bool wave_loop_reads_double(const struct XpDeviceProfile *profile,
                             const int32_t *pcm, size_t count,
                             uint32_t baseAddress,
                             const struct xp_wave_descriptor *desc);

/* Integer-address playback only. Fractional phase conversion, interpolation
 * rounding and reverse one-shot termination deliberately remain outside this
 * API until their XP semantics are recovered or explicitly parameterized. */
bool wave_cursor_init(const struct XpDeviceProfile *profile,
                       struct xp_wave_cursor *cursor,
                       const struct xp_wave_registers *registers,
                       enum xp_wave_loop_type mode);
bool wave_cursor_current(const struct xp_wave_cursor *cursor,
                          uint32_t *address);
bool wave_cursor_advance(struct xp_wave_cursor *cursor);

}}  // namespace EmuSC::Xp
#endif

#endif
