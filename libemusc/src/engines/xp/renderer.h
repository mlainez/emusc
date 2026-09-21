/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_XP_RENDERER_H
#define EMUSC_XP_RENDERER_H

#include "oscillator.h"
#include "pan.h"
#include "lfo.h"
#include "pitch.h"
#include "rom.h"
#include "tva.h"
#include "tvf.h"
#include "devices/sc88.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#include <cmath>
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct sc88_wave_bank {
  uint8_t selector;
  const uint8_t *bytes;
  size_t size;
};

struct sc88_renderer {
  struct sc88_rom rom;
  struct sc88_wave_bank banks[XP_WAVE_BANK_COUNT];
  double output_rate;
  enum sc88_fractional_wrap wrap;
  struct sc88_tva_levels levels;
  struct sc88_pan_controls pan;
  struct sc88_tvf_controls tvf_controls;
  struct sc88_tva_controls tva_controls;
  sc88_tvf_audio_transfer_fn tvf_audio_transfer;
  void *tvf_audio_user;
  unsigned only_component;
  /* control_gain_q15's whole domain, decoded once at init instead of
     once per active voice per output sample (reverb/chorus/delay sends
     are read up to 3x per voice per sample, and the ROM table these come
     from - XpDeviceProfile::sendTable, pan.cc - never changes after
     rom_init): index by
     the control byte directly, /32768.0f already folded in (exact, a
     power of two), rather than repeating control_gain_q15's ROM read.
     control_gain_q15's own false-return case (see pan.cc) is tracked
     alongside it so a send whose bit pattern never means a clean gain
     still gets skipped exactly as before. */
  float send_gain[128];
  bool send_ok[128];
};

/* See send_gain/send_ok above. Keeps control_gain_q15's own bounds check
   (some callers pass a raw uint8_t send byte without pre-clamping). */
static inline bool sc88_renderer_send_gain(
  const struct sc88_renderer *renderer, uint8_t control, float *gain)
{
  if (control > 127)
    return false;
  *gain = renderer->send_gain[control];
  return renderer->send_ok[control];
}

struct sc88_render_component {
  int32_t *pcm24;
  size_t pcm_count;
  struct sc88_oscillator oscillator;
  uint32_t static_pitch_word;
  struct sc88_pitch_envelope pitch_envelope;
  struct sc88_pitch_release pitch_release;
  /* The glide, and the terms its moving key is recomposed through. It is
     per SLOT because the device holds it per slot: both components of a
     two-component tone are given the same source, target and rate at note
     on, so they glide together. The SC-55 path had to be fixed twice for
     letting one partial glide and the other start at the target
     (emusc-match TASK-088, TASK-113); nothing here can drift apart, because
     no per-partial write feeds the next note's source. */
  struct sc88_portamento portamento;
  /* A rhythm note's own reverb send from its kit record, 127 for a
     melodic note. The kits are not uniformly treated - STANDARD 1 sends
     its snare and cymbals at 127 and its kick at 0 - and flattening them
     to one part send is audible as a kit with no depth (`M-009`). */
  /* The tone-common oscillator. Its rate, waveform and delay/fade ramp
     are exact; what the depth word means in cents is calibrated from
     the manual's one published figure (`M-019`). */
  struct sc88_lfo lfo1;
  /* The component's own oscillator, and its pitch depth from `+18/+19`.
     This is where a tone's built-in vibrato lives: the controller matrix
     contributes nothing until a mod wheel moves (`M-020`). */
  struct sc88_lfo lfo2;
  /* The tone-common oscillator's pitch depth. Component byte `+17` is a
     signed INDEX, not a depth: it selects from the 255-word curve at
     `0x78304`, and that curve's output is the depth, in the same unit as
     the local field. Stored here already resolved. */
  int16_t lfo1_pitch_depth;
  int16_t lfo2_pitch_depth;
  /* Each oscillator's amplitude and filter depths (`M-021`). The amplitude
     pair is in attenuation-word units, the unit the envelope's own stage
     words use, which is what makes Vibraphone's 604 worth about 0.8 dB of
     peak tremolo against the 1.1 dB peak-to-peak its hardware recording
     shows at the rate the ROM asks for. The filter pair is in cutoff-word
     units, as the filter envelope's targets are. */
  int16_t lfo1_tva_depth, lfo2_tva_depth;
  int16_t lfo1_tvf_depth, lfo2_tvf_depth;
  uint8_t reverb_send;
  /* the kit's `+0x400`, or 127 for a melodic note */
  uint8_t chorus_send;
  uint16_t static_attenuation;
  /* The rhythm kit's own level for this note, or `XP_TVA_NO_DRUM_LEVEL`
     on a melodic one. It is a fifth term in the composed amplitude, so it
     is kept beside the attenuation it is subtracted with: the release and
     a part-level change both recompose that amplitude from the stored
     terms, and a note that dropped this one would rise to full kit level
     the moment either happened. */
  uint8_t drum_level;
  /* The composed amplitude is handed to the XP chip as a TARGET with an
     interpolation word beside it, not as a value to latch: `71a9` writes
     the four-word block {zero, interpolation, target high, target low} at
     the voice's register base every control period, with `#0x2a7` as the
     interpolation word at `71c7`. The register therefore moves toward the
     composed amplitude over the period; it is never a staircase.

     `static_gain_q17` is that target. `static_gain_current_q17` is where
     the register stood when the period opened, and the audio path reads
     the point between them - the same current/target pair, read the same
     way, that `sc88_tvf_registers` uses for TVF-F.

     `0x2a7` is the exponential family's entry at rate index 2, so the
     approach is one of about ten time constants per control period:
     `tva_curve_decode` gives it `rate = 679/64`, a time constant of
     0.75 ms. It is a de-click, not a glide. */
  uint32_t static_gain_q17;
  uint32_t static_gain_current_q17;
  /* Amplitude 0 has been written as the target and the period it glides
     over is running. `7228..7232` writes that target when the release
     counter underflows; the voice ends when the register arrives, not
     when the CPU composes the zero. */
  bool release_zeroed;
  struct sc88_tva_envelope envelope;
  struct sc88_tva_release release;
  struct sc88_tvf_registers tvf;
  struct sc88_tvf_envelope tvf_envelope;
  struct sc88_tvf_release tvf_release;
  struct sc88_tvf_audio_state tvf_audio;
  int16_t tvf_key_modulation;
  uint32_t rom_component_offset;
  bool continuous_hold_release;
  bool keep_release_scale_at_zero;
  int16_t pan_component_offset;
  uint8_t pan_position;
  uint8_t pan_target_position;
  uint16_t left_gain_q15;
  uint16_t right_gain_q15;
  bool active;
};

/* tva_curve_decode's result for XP_STATIC_AMPLITUDE_CURVE_WORD never
   changes (linear false, rate 679/64 - a power-of-two divisor, so this
   is that exact double, not an approximation of it), so this inlines
   tva_curve_progress's non-linear branch directly instead of redoing
   both from scratch every call. Exposed separately from
   sc88_render_static_gain_q17 below so a caller whose voices share one
   period_fraction this sample - engine_render_with_send's per-voice
   loop - can compute it once per sample instead of once per voice: at
   period_fraction 0.0 this returns exactly 0.0 (exp(-0.0) is exactly
   1.0), which sc88_render_static_gain_q17_from_progress below turns
   back into component->static_gain_current_q17 unchanged, so no
   separate period_fraction <= 0.0 case is needed here. */
static inline double sc88_static_gain_progress(double period_fraction)
{
  const double q = kXpStaticAmplitudeRate * period_fraction;
  return q >= 40.0 ? 1.0 : 1.0 - std::exp(-q);
}

static inline uint32_t sc88_render_static_gain_q17_from_progress(
  const struct sc88_render_component *component, double progress)
{
  const double from = (double)component->static_gain_current_q17;
  const double to = (double)component->static_gain_q17;
  const double value = from + progress * (to - from);
  return value <= 0.0 ? 0u : (uint32_t)(value + 0.5);
}

/* Where the chip's amplitude register stands `period_fraction` of the way
   through the control period. */
static inline uint32_t sc88_render_static_gain_q17(
  const struct sc88_render_component *component, double period_fraction)
{
  if (period_fraction <= 0.0)
    return component->static_gain_current_q17;
  return sc88_render_static_gain_q17_from_progress(
    component, sc88_static_gain_progress(period_fraction));
}

struct sc88_render_voice {
  struct sc88_render_component components[XP_MAX_TONE_COMPONENTS];
  unsigned component_count;
  uint32_t tone_offset;
  uint8_t key;
  uint8_t velocity;
  /* A rhythm note whose kit record clears bit 0 of `+0x480` does not
     receive Note Off and rings to its own end. Every drum in this song is
     written as a 10 ms note, so honouring Note Off turns a crash into a
     tick (`M-015`). */
  bool ignore_note_off;
  sc88_tvf_audio_transfer_fn tvf_audio_transfer;
  void *tvf_audio_user;
  unsigned only_component;
};

#ifdef __cplusplus
}

namespace EmuSC { namespace Xp {

// Renderer (per-voice tone resolution and rendering) for the
// XP-generation-1 engine (see engines/xp/README.md). The plain C types
// above are shared, unrenamed, with EmuSC::Xp::Device (device.h), which
// embeds sc88_render_component and sc88_render_voice by value through
// engine.h, and with sc88_renderer_test.c, which reads their fields
// directly.
//
// Two concerns share this file rather than splitting into a ToneSelector
// and a Voice, as originally hypothesised: tone/zone resolution
// (renderer_note_on_tone, private below) directly fills in the very
// sc88_render_component fields the per-sample render loop reads, and both
// sides are struct-shaped by the same ABI constraint above. A split
// becomes natural whenever the test file's own direct field access is
// retired and these structs can become real member state instead of a
// shared C layout.

/* Firmware key transform at SC88-CTL 0x60c7..0x6123. */
uint8_t renderer_selector_key(const struct sc88_component *component,
                               uint8_t midiKey);

/* The remainder that transform drops, in pitch units (0..1364):
   SC88-CTL 0x611e..0x6121 scale it by 0x555, one semitone. */
uint16_t renderer_key_fraction(const struct sc88_component *component,
                                uint8_t midiKey);

/* Everything renderer_pitch_word_at needs to recompose the static pitch
   word for a key that is moving, taken from the same note-on inputs
   renderer_static_pitch_word is given. */
bool renderer_portamento_terms(const struct sc88_rom *rom,
                                const struct sc88_tone *tone,
                                const struct sc88_component *component,
                                const struct sc88_wave_descriptor *desc,
                                struct sc88_portamento *portamento);

/* The static pitch word for a fractional MIDI key, 16.16 - what SC88-CTL
   `0x6063` and `0x6077` recompose every control period while a glide runs.
   At a whole key it returns exactly what renderer_static_pitch_word
   composed for that key, so a finished glide lands on the note-on value
   rather than near it. */
bool renderer_pitch_word_at(const struct sc88_rom *rom,
                             const struct sc88_portamento *portamento,
                             uint32_t keyQ16, uint32_t *pitchWord);

/* Static note-on pitch before controllers, LFOs and the pitch envelope.
 * It needs BOTH keys: `0x6124` indexes the tone-common pitch table with the
 * raw key at RAM `0x245a`, while `0x609a` takes the root key off the
 * transformed one at `0x19fc`. */
bool renderer_static_pitch_word(const struct sc88_rom *rom,
                                 const struct sc88_tone *tone,
                                 const struct sc88_component *component,
                                 const struct sc88_wave_descriptor *desc,
                                 uint8_t midiKey, uint8_t selectorKey,
                                 uint16_t keyFraction, uint32_t *pitchWord);

bool renderer_init(struct sc88_renderer *renderer, const uint8_t *controlRom,
                    size_t controlRomSize, const struct sc88_wave_bank *banks,
                    size_t bankCount, double outputRate,
                    enum sc88_fractional_wrap wrap);
void renderer_set_levels(struct sc88_renderer *renderer,
                          const struct sc88_tva_levels *levels);
void renderer_set_pan(struct sc88_renderer *renderer,
                       const struct sc88_pan_controls *pan);
/* Instrumentation: 1 sounds only the tone's first component, 2 only its
 * second, 0 (the default) sounds them all. */
void renderer_set_only_component(struct sc88_renderer *renderer,
                                  unsigned which);
void renderer_set_tvf_audio_transfer(struct sc88_renderer *renderer,
                                      sc88_tvf_audio_transfer_fn transfer,
                                      void *user);
void renderer_set_tvf_controls(struct sc88_renderer *renderer,
                                const struct sc88_tvf_controls *controls);

/* There is no gain argument and no gain field on the voice. The firmware
 * composes a voice's amplitude in exactly one place - `compose_voice_amplitude`
 * subtracts its five level sources and the component's static attenuation from
 * one headroom - so a float multiply beside it is an escape hatch with no
 * counterpart in the device, and anything put through it is not being modelled.
 * The kit's per-note level was applied that way and, because the engine's mix
 * reads its own copy of the caller's trim rather than the voice's, it reached
 * renderer_render and no song. Output trim belongs to the caller.
 *
 * Static TVA, release, pan and exact TVF control state are native ROM paths;
 * the audio-side TVF callback and effects remain explicit seams. */
bool renderer_note_on(const struct sc88_renderer *renderer,
                       struct sc88_render_voice *voice, uint8_t variation,
                       uint8_t program, uint8_t key, uint8_t velocity);
bool renderer_note_on_with_levels(const struct sc88_renderer *renderer,
                                   struct sc88_render_voice *voice,
                                   uint8_t variation, uint8_t program,
                                   uint8_t key, uint8_t velocity,
                                   const struct sc88_tva_levels *levels);
bool renderer_note_on_with_controls(const struct sc88_renderer *renderer,
                                     struct sc88_render_voice *voice,
                                     uint8_t variation, uint8_t program,
                                     uint8_t key, uint8_t velocity,
                                     const struct sc88_tva_levels *levels,
                                     const struct sc88_pan_controls *pan);
/* `map` is the tone map, which chooses the row of the variation lookup the
 * bank is taken from, exactly as it chooses a rhythm part's kit set. The
 * three entry points above have no part state to take it from and use the
 * reset default, `XP_TONE_MAP_SC88`. */
bool renderer_note_on_with_part_controls(
  const struct sc88_renderer *renderer, struct sc88_render_voice *voice,
  uint8_t map, uint8_t variation, uint8_t program, uint8_t key,
  uint8_t velocity, const struct sc88_tva_levels *levels,
  const struct sc88_pan_controls *pan,
  const struct sc88_tvf_controls *tvfControls,
  const struct sc88_tva_controls *tvaControls,
  const struct sc88_lfo_controls *lfoControls);
/* As above, and `zoneKey` chooses the zone instead of `key`. A portamento
 * note hands in the higher of the glide's two ends, which is what SC88-CTL
 * `0x602e` computes and `0x4e2c` is given. */
bool renderer_note_on_with_glide(
  const struct sc88_renderer *renderer, struct sc88_render_voice *voice,
  uint8_t map, uint8_t variation, uint8_t program, uint8_t key,
  uint8_t zoneKey, uint8_t velocity, const struct sc88_tva_levels *levels,
  const struct sc88_pan_controls *pan,
  const struct sc88_tvf_controls *tvfControls,
  const struct sc88_tva_controls *tvaControls,
  const struct sc88_lfo_controls *lfoControls);
/* A rhythm-part note. The kit record supplies the tone, the key it is played
 * at, and this key's own level and pan, so a kick is not a sample transposed
 * to whatever key triggered it. `note` receives the whole record when the
 * caller wants its sends or assign group.
 * `map` is the tone map, which chooses the kit set; `setup` is the drum
 * setup the part plays from, which chooses the half of `overlay` its own
 * `41 mf rr` edits are in. They are separate axes on this machine. */
bool renderer_note_on_drum(
  const struct sc88_renderer *renderer, struct sc88_render_voice *voice,
  uint8_t map, uint8_t program, uint8_t key, uint8_t velocity,
  const struct sc88_tva_levels *levels,
  const struct sc88_pan_controls *pan,
  const struct sc88_tvf_controls *tvfControls,
  const struct sc88_tva_controls *tvaControls,
  const struct sc88_lfo_controls *lfoControls,
  const struct sc88_drum_overlay *overlay, uint8_t setup,
  struct sc88_drum_note *note);

void renderer_voice_destroy(struct sc88_render_voice *voice);
bool renderer_voice_active(const struct sc88_render_voice *voice);

/* Interleaved stereo dry output. No clipping is applied because XP summing
 * precision is open. */
size_t renderer_render(struct sc88_render_voice *voice, float *stereo,
                        size_t frames);

}}  // namespace EmuSC::Xp
#endif

#endif
