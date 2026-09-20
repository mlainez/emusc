/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_XP_DEVICE_H
#define EMUSC_XP_DEVICE_H

#include "chorus.h"
#include "delay.h"
#include "eq.h"
#include "output.h"
#include "engine.h"
#include "reverb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SC88_WAVE_CHIP_COUNT 4u
#define SC88_MIDI_PORT_COUNT 2u

/* The controller destination matrix at `40 2x ss`, in the order
 * `04_protocol/sysex.md` prints: six source groups of eleven destinations.
 * The wire address is `group * 0x10 + destination`; the matrix's own part
 * structure holds the same six groups twelve bytes apart at +0x28, with a
 * byte at +3 this routine does not read (`05_data_model/part_state.md`). */
enum sc88_matrix_source {
  SC88_MATRIX_MODULATION = 0,
  SC88_MATRIX_PITCH_BEND = 1,
  SC88_MATRIX_CHANNEL_PRESSURE = 2,
  SC88_MATRIX_POLY_PRESSURE = 3,
  SC88_MATRIX_CC1 = 4,
  SC88_MATRIX_CC2 = 5
};
#define SC88_MATRIX_SOURCE_COUNT 6u

enum sc88_matrix_destination {
  SC88_MATRIX_PITCH = 0,
  SC88_MATRIX_CUTOFF = 1,
  SC88_MATRIX_AMPLITUDE = 2,
  SC88_MATRIX_LFO1_RATE = 3,
  SC88_MATRIX_LFO1_PITCH_DEPTH = 4,
  SC88_MATRIX_LFO1_TVF_DEPTH = 5,
  SC88_MATRIX_LFO1_TVA_DEPTH = 6,
  SC88_MATRIX_LFO2_RATE = 7,
  SC88_MATRIX_LFO2_PITCH_DEPTH = 8,
  SC88_MATRIX_LFO2_TVF_DEPTH = 9,
  SC88_MATRIX_LFO2_TVA_DEPTH = 10
};
#define SC88_MATRIX_DEST_COUNT 11u

struct sc88_channel_state {
  uint8_t variation;
  /* The two halves of the part's bank word at `d820 + 2*part`, which is
     what the tone map is: `tone_map_forced` is the byte CC32 and
     `40 4x 00` both write, 0 to defer to the part's own map and 1 or 2 to
     force the SC-55 or SC-88 one, and `tone_map_selected` is that own map,
     which only `40 4x 01` writes. `2d3e` and `2e7a` resolve the pair
     identically for melodic and rhythm parts: the forced byte when it is
     nonzero, the selected map otherwise.
     Their reset behaviour differs, which is why they are held apart: GS
     Reset clears the forced byte, while the selected map survives it, GM
     On, System Mode Set and power-on with the printed default of 02,
     SC-88 (SC88-OM printed 7-31). */
  uint8_t tone_map_forced;
  uint8_t tone_map_selected;
  uint8_t program;
  uint8_t volume;
  uint8_t expression;
  uint8_t pan;
  uint8_t hold1;
  uint8_t reverb_send;
  uint8_t chorus_send;
  uint8_t delay_send;
  uint8_t cutoff;
  uint8_t resonance;
  /* The envelope-time modifiers, centred at 64. Demo song 1 sets the
     attack twenty-two times, so a part that never receives them cannot
     sound like the recording. */
  uint8_t attack;
  uint8_t decay;
  uint8_t release;
  /* The five part-level sources the controller destination matrix reads,
     at DP:d6a1, d660, d6a0, d760 and d761 (`05_data_model/part_state.md`).
     Channel pressure is held here; poly pressure is per key and is not
     received yet. */
  uint8_t modulation;
  uint8_t channel_pressure;
  /* Part key shift, 0x28..0x58 about 0x40, so +/-24 semitones. Applied
     to the pitch rather than to the note number, which leaves zone and
     kit selection alone. TOXOPLASMA shifts a part down an octave. */
  uint8_t key_shift;
  /* The two assignable controllers and the value each carries. Their
     defaults are CC16 and CC17, and every one of their matrix depths
     is zero after a reset, so an assigned controller is received and
     changes nothing until a song sets a depth. */
  uint8_t cc1_assign, cc2_assign;
  uint8_t cc1_value, cc2_value;
  /* Received and held. Mono mode and portamento need voice-lifecycle
     work this does not do yet; holding them keeps a song's request
     from being counted as unimplemented when it is only unapplied. */
  uint8_t mono_mode;
  uint8_t vibrato_rate, vibrato_depth, vibrato_delay;
  uint8_t portamento_time, portamento_switch, portamento_control;
  uint16_t pitch_bend;
  uint8_t pitch_bend_sensitivity;
  uint8_t rpn_msb;
  uint8_t rpn_lsb;
  uint8_t nrpn_msb;
  uint8_t nrpn_lsb;
  /* The controller destination matrix, `40 2x ss`: six source groups in
     the published order, eleven depths each. Only the cutoff column is
     applied so far; the rest are held so a song's settings are not lost,
     and so the one consumer that does exist - modulation to LFO1 pitch
     depth - reads the same bytes every other destination will. */
  uint8_t matrix_depth[SC88_MATRIX_SOURCE_COUNT][SC88_MATRIX_DEST_COUNT];
  enum sc88_same_note_mode same_note_mode;
};

struct sc88_device {
  uint8_t *control_rom;
  uint8_t *decoded_chips[SC88_WAVE_CHIP_COUNT];
  struct sc88_wave_bank banks[SC88_WAVE_BANK_COUNT];
  struct sc88_renderer renderer;
  struct sc88_engine engine;
  struct sc88_reverb reverb;
  struct sc88_chorus chorus;
  struct sc88_delay delay;
  struct sc88_eq eq;
  struct sc88_output output;
  /* The reverb's own parameters. Writing the macro reloads all of them from
     a ROM preset, and a GS reset is the same read with macro 4. */
  uint8_t reverb_character, reverb_level, reverb_time, reverb_pre_lpf;
  uint8_t reverb_predelay, reverb_delay_feedback;
  uint8_t reverb_macro;
  /* The chorus parameters are received and held but not yet rendered: the
     CPU-side transforms are recovered while the DSP's audio algorithm is
     not (`08_effects/chorus.md`), so a chorus here would be invented
     rather than modelled. Holding them keeps a song's settings from being
     silently discarded. */
  uint8_t chorus_macro, chorus_level, chorus_feedback, chorus_delay;
  uint8_t chorus_rate, chorus_depth, chorus_pre_lpf, chorus_send_to_reverb;
  /* 0 single module, 1 double. Several parameters exist only in one mode. */
  /* The ten delay parameters in manual order, and the macro that last
     loaded them. Single-module only. */
  uint8_t delay_params[10];
  uint8_t delay_macro;
  /* The output equaliser, on after a reset. Its four wire bytes are the
     band corners and gains; at the centre gain it is an identity. */
  uint8_t eq_low_frequency, eq_low_gain;
  uint8_t eq_high_frequency, eq_high_gain;
  uint8_t system_mode;
  /* The filter's cutoff is a fraction of the sound chip's own 32 kHz,
     so the coefficient has to be recomputed for whatever rate the
     caller renders at; the transfer function is handed this. */
  double output_rate;
  /* SysEx writes that parsed correctly but name an address this
     implementation does not act on. Counted rather than dropped quietly,
     so a render can say what it ignored. */
  unsigned long unhandled_sysex;
  /* The output is AC-coupled, as every analogue audio output is. The
     decoder integrates differences, so a sample whose deltas do not
     sum to zero leaves a standing offset, and a sustained note holds
     it: song 1 measured 0.157 of DC against the hardware's 0.001, in
     65 % of its length, and that offset is what pushed the mix into
     clipping. One pole at 10 Hz, which is -0.3 dB by 40 Hz. */
  float *send_bus;
  float *chorus_bus;
  float *delay_bus;
  size_t send_capacity;
  struct sc88_channel_state channels[SC88_ENGINE_PART_COUNT];
  uint8_t master_volume;
  uint8_t secondary_level;
  uint8_t master_pan;
  /* How many times a part's pan was substituted because it asked for GS
     random pan, which needs a sound-chip random source that is not
     recovered. Counted rather than hidden: it is a labelled divergence, and
     a render reporting zero is exact in this respect. */
  /* Parts that asked for random pan; the engine draws each voice. */
  unsigned long random_pan_requests;
  bool initialized;
};

/* Compatibility surface for callers not yet ported to the EmuSC::Xp API
 * below (synth.cc, which holds an opaque struct sc88_device* and calls
 * only these functions, and sc88_device_test.c, which reads the struct's
 * fields directly). Each forwards to the real implementation in namespace
 * EmuSC::Xp. */
bool sc88_device_init_raw(struct sc88_device *device,
                          const uint8_t *control_rom,
                          size_t control_rom_size,
                          const uint8_t *const raw_chips[SC88_WAVE_CHIP_COUNT],
                          const size_t raw_sizes[SC88_WAVE_CHIP_COUNT],
                          double output_rate,
                          enum sc88_fractional_wrap wrap);
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
bool sc88_device_sysex(struct sc88_device *device, uint8_t port,
                       const uint8_t *data, size_t size);
bool sc88_device_midi(struct sc88_device *device, uint8_t port,
                      uint8_t status, uint8_t data1, uint8_t data2);
int16_t sc88_device_matrix_cutoff_word(
  const struct sc88_channel_state *channel);
void sc88_device_render(struct sc88_device *device, float *stereo,
                        size_t frames);

#ifdef __cplusplus
}

namespace EmuSC { namespace Xp {

// Top-level MIDI device (lifecycle, SysEx/MIDI protocol decoding, the
// controller-to-engine parameter-sync bridge, and effects-chain ownership)
// for the XP-generation-1 engine (see engines/xp/README.md). The plain C
// types above are shared, unrenamed, with synth.cc (opaque pointer only)
// and sc88_device_test.c (direct field access).
//
// This is the last engines/xp/*.c module to convert, and the first one
// where nothing outside this file's own tests embeds these structs by
// value any more - the ABI constraint every earlier hub task (tvf,
// renderer, engine) deferred its SRP split on is gone. It stays a single
// file here anyway, matching every other hub task's own house style
// rather than restructuring on the strength of a constraint lifting: a
// three-way split into MidiDecoder (sysex/midi protocol decode),
// EffectsBus (reverb/chorus/delay/eq/output ownership and the render
// chain) and a retained Device facade is the natural next step, and the
// boundary is visible below in this file's own section comments. Protocol
// decoding is plausibly generic XP SysEx/MIDI shape, shared with a future
// JV-1080; the parameter-sync bridge (GS controller state -> engine
// registers) is plausibly SC-88-specific. That split, and the
// synth.h/synth.cc change from an opaque struct sc88_device* to a real
// EmuSC::Xp::Device*, is what the backlog's optional final task retires
// the extern "C" shims for.

bool device_init_raw(struct sc88_device *device, const uint8_t *controlRom,
                      size_t controlRomSize,
                      const uint8_t *const rawChips[SC88_WAVE_CHIP_COUNT],
                      const size_t rawSizes[SC88_WAVE_CHIP_COUNT],
                      double outputRate, enum sc88_fractional_wrap wrap);
bool device_init_decoded(
  struct sc88_device *device, const uint8_t *controlRom,
  size_t controlRomSize,
  const uint8_t *const decodedChips[SC88_WAVE_CHIP_COUNT],
  const size_t decodedSizes[SC88_WAVE_CHIP_COUNT], double outputRate,
  enum sc88_fractional_wrap wrap);
void device_destroy(struct sc88_device *device);
void device_reset_controllers(struct sc88_device *device);
void device_set_master_volume(struct sc88_device *device, uint8_t value);
void device_set_master_pan(struct sc88_device *device, uint8_t value);

/* One System Exclusive message, with or without its leading `f0` and
 * trailing `f7`. Returns false for a message that is not a well-formed GS
 * DT1 for this device - a wrong manufacturer or model, a bad checksum, a
 * truncated packet - and true once the packet has been applied, whether or
 * not every address in it was one this implementation acts on. */
bool device_sysex(struct sc88_device *device, uint8_t port,
                   const uint8_t *data, size_t size);

/* MIDI port 0 addresses parts 0..15 and port 1 addresses parts 16..31.
 * Supported channel messages: note on/off, pitch bend, program change,
 * CC0/6/7/10/11/32/64/66/98..101/121, RPN 00/00 bend sensitivity, and
 * NRPN 01/20..21 cutoff/resonance. Unsupported messages return false
 * without changing state. */
bool device_midi(struct sc88_device *device, uint8_t port, uint8_t status,
                  uint8_t data1, uint8_t data2);

/* One part's cached controller-matrix cutoff word, the value the firmware
 * holds at DP:1c34 + part. Exposed so the arithmetic can be checked on its
 * own; the device recomposes it whenever one of its inputs moves. */
int16_t device_matrix_cutoff_word(const struct sc88_channel_state *channel);

void device_render(struct sc88_device *device, float *stereo, size_t frames);

}}  // namespace EmuSC::Xp
#endif

#endif
