/* SPDX-License-Identifier: CC0-1.0 */
#include "renderer.h"

#include <climits>
#include <cstdlib>
#include <cstring>

namespace EmuSC { namespace Xp {

namespace {

uint16_t be16(const uint8_t *p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

int16_t s16(uint16_t value)
{
  return value <= INT16_MAX
    ? (int16_t)value
    : (int16_t)(-1 - (int32_t)(UINT16_MAX - value));
}

int8_t s8(uint8_t value)
{
  return value <= INT8_MAX
    ? (int8_t)value
    : (int8_t)(-1 - (int16_t)(UINT8_MAX - value));
}

int32_t floorQ14(int32_t value)
{
  if (value >= 0)
    return value / 16384;
  return -(int32_t)(((uint32_t)(-value) + 16383u) / 16384u);
}

/* The same floor, one binade coarser: the key-follow product is held shifted
   left twice, so its whole-key part is the high word rather than the value
   divided by 16384. */
int32_t floorQ16(int32_t value)
{
  if (value >= 0)
    return value / 65536;
  return -(int32_t)(((uint32_t)(-(int64_t)value) + 65535u) / 65536u);
}

int32_t relativePitch(int difference)
{
  int32_t value = (int32_t)((difference * 16384) / 12);
  return value > 32767 ? 32767 : value;
}

int bankIndex(uint8_t selector)
{
  switch (selector) {
  case 0x00: return 0;
  case 0x01: return 1;
  case 0x10: return 2;
  case 0x11: return 3;
  case 0x20: return 4;
  case 0x21: return 5;
  case 0x30: return 6;
  case 0x31: return 7;
  default: return -1;
  }
}

const struct sc88_wave_bank *findBank(const struct sc88_renderer *renderer,
                                       uint8_t selector)
{
  int index = bankIndex(selector);
  return index < 0 ? nullptr : renderer->banks + index;
}

/* Centre of the 255-word bipolar pitch-control curve at 0x78304..0x78502,
 * indexed -127..127 about this address (`02_rom/tables.md`). */
constexpr uint32_t kPitchCurveCentre = 0x78402u;

/* The body both note-on entry points share. A melodic note selects its tone
 * through the variation map and plays it at the MIDI key; a rhythm note's
 * tone and key both come from its kit record, so the two differ only in
 * what they hand in here. */
bool noteOnTone(const struct sc88_renderer *renderer,
                 struct sc88_render_voice *voice, uint32_t toneOffset,
                 uint8_t key, uint8_t zoneKey, uint8_t velocity,
                 const struct sc88_tva_levels *levels, uint8_t drumLevel,
                 const struct sc88_pan_controls *pan,
                 const struct sc88_tvf_controls *tvfControls,
                 const struct sc88_tva_controls *tvaControls,
                 const struct sc88_lfo_controls *lfoControls)
{
  struct sc88_tone tone;

  if (!renderer || !voice || !levels || !pan || !tvfControls ||
      levels->master > 127 ||
      levels->secondary > 127 || levels->part > 127 ||
      levels->expression > 127 || key > 127 || zoneKey > 127 ||
      velocity > 127 ||
      !rom_open_tone(&renderer->rom, toneOffset, &tone))
    return false;
  std::memset(voice, 0, sizeof *voice);
  /* `component_count` is the number of components this note actually
     sounds, which is not the tone's own count: a component whose velocity
     window excludes this note is not prepared and takes no slot. It is
     counted up as the loop below prepares them, and the prepared ones are
     packed from index zero, so a note that sounds only the tone's second
     component holds it in `components[0]`. */
  voice->only_component = renderer->only_component;
  voice->tone_offset = toneOffset;
  voice->key = key;
  voice->velocity = velocity;
  /* melodic notes always receive Note Off; a kit may say otherwise */
  voice->ignore_note_off = false;
  voice->tvf_audio_transfer = renderer->tvf_audio_transfer;
  voice->tvf_audio_user = renderer->tvf_audio_user;

  for (unsigned i = 0; i < tone.component_count; ++i) {
    struct sc88_render_component *renderComponent;
    struct sc88_component component;
    struct sc88_zone_selection zone;
    struct sc88_wave_registers registers;
    enum sc88_wave_loop_type mode;
    const struct sc88_wave_bank *bank;
    uint32_t selectorKey;
    uint16_t keyFraction;
    uint32_t pitchWord;
    uint32_t pcmBase;
    size_t capacity;
    int16_t tvfKeyModulation;

    /* Instrumentation: sound one component of a multi-component tone, so a
       defect in how the two are balanced can be separated from a defect in
       either one. The components are numbered from ONE, and zero - the
       default - sounds every component the tone asks for. Read as a
       zero-based index instead, `--only-component 0` renders the whole
       tone and looks like proof that the second component is silent. */
    if (voice->only_component > 0 && i + 1 != voice->only_component)
      continue;
    if (!rom_open_component(&renderer->rom, &tone, i, &component))
      goto fail;
    /* The component's velocity window, +6c..+6d inclusive. Outside it the
       component does not sound at all: `rom_component_sounds` carries
       the ROM evidence. Ignoring it does not merely add a layer that should
       be absent, it adds the LOUDEST one - the firmware's velocity index is
       `(velocity - low) * factor >> 8` on a wrapping byte, so one count
       below the window the subtraction wraps to 255 and the index saturates
       at the top of the curve. French Horns at velocity 100 sounded its
       101..127 component, six cutoff indices brighter than the one the note
       asks for, at full level. */
    if (!rom_component_sounds(&component, velocity))
      continue;
    renderComponent = voice->components + voice->component_count;
    selectorKey = renderer_selector_key(&component, key);
    keyFraction = renderer_key_fraction(&component, key);
    /* The zone is chosen for the HIGHER of a glide's two ends, not for the
       note's own key: `0x602e` runs the key transform a second time on the
       target at `0x6049` and hands the zone lookup at `0x4e2c` whichever of
       the two came out higher (`0x6052`). Without that rule a downward
       glide would start above the chosen sample's root key, where the
       relative-pitch table saturates 24 semitones up, and the first part of
       the glide would not move at all. `zoneKey` is the note's own key
       whenever there is no glide, which is every note in six of the seven
       demo songs. */
    if (!rom_select_zone(&renderer->rom, &component,
                          renderer_selector_key(&component, zoneKey),
                          &zone) ||
        !wave_descriptor_loop_type(&zone.descriptor, &mode) ||
        // The descriptor's +16 word is NOT added to the start address. Adding it
        // skipped the first 4608 samples of every sample this tone plays -
        // 144 ms, which on a struck instrument is the whole strike: the
        // Xylophone's onset centroid read 1403 Hz against the reference's
        // 4603, and its timbre travel over the first 400 ms was 356 Hz against
        // 3548. Suppressed, they are 4863 Hz and 3816.
        //
        // `02_rom/wave_metadata.md` says the word is added "unless per-voice
        // state +187d bit 7 suppresses it", and that condition is NOT
        // recovered. Both branches are therefore a guess about when; this one
        // is the guess the measurements support - over seven demo songs the
        // onset excursion goes 0.54 to 0.72 and the static centroid to within
        // 45 Hz on both paths, and the 76-instrument set is unchanged at 65
        // within 6 dB, because the loop points are separate from the start
        // address and the sustain never moves.
        !wave_prepare_registers(&zone.descriptor, true, &registers) ||
        !renderer_static_pitch_word(&renderer->rom, &tone, &component,
                                     &zone.descriptor, key,
                                     (uint8_t)selectorKey, keyFraction,
                                     &pitchWord) ||
        !renderer_portamento_terms(&renderer->rom, &tone, &component,
                                    &zone.descriptor,
                                    &renderComponent->portamento) ||
        !tva_static_gain_q17(&renderer->rom, &tone, &component, &zone,
                              (uint8_t)selectorKey, velocity,
                              levels, drumLevel,
                              &renderComponent->static_attenuation,
                              &renderComponent->static_gain_q17) ||
        !pan_static_q15(&renderer->rom, &tone, &component,
                         (uint8_t)selectorKey, pan,
                         &renderComponent->pan_position,
                         &renderComponent->left_gain_q15,
                         &renderComponent->right_gain_q15) ||
        !pan_component_offset(&renderer->rom, &tone, &component,
                               (uint8_t)selectorKey,
                               &renderComponent->pan_component_offset) ||
        !tva_release_prepare(&renderer->rom, &tone, &component,
                              (uint8_t)selectorKey,
                              &renderComponent->release) ||
        !tva_envelope_prepare(&renderer->rom, &tone, &component,
                               (uint8_t)selectorKey, velocity,
                               tvaControls,
                               &renderComponent->envelope) ||
        !pitch_envelope_prepare(
          &renderer->rom, &tone, &component, (uint8_t)selectorKey,
          velocity, &renderComponent->pitch_envelope) ||
        !pitch_release_prepare(
          &renderer->rom, &tone, &component, (uint8_t)selectorKey,
          renderComponent->pitch_envelope.depth,
          &renderComponent->pitch_release) ||
        !tvf_key_modulation(&renderer->rom, &tone, &component,
                             (uint8_t)selectorKey, &tvfKeyModulation) ||
        !tvf_envelope_prepare(&renderer->rom, &tone, &component,
                              (uint8_t)selectorKey, velocity, false,
                              &renderComponent->tvf_envelope) ||
        !tvf_release_prepare(
          &renderer->rom, &tone, &component, (uint8_t)selectorKey,
          renderComponent->tvf_envelope.depth,
          &renderComponent->tvf_release) ||
        !tvf_prepare_registers(&renderer->rom, &component, tvfKeyModulation,
                                tvfControls, &renderComponent->tvf) ||
        !tvf_update_frequency(&renderer->rom,
                               renderComponent->tvf_envelope.current,
                               &renderComponent->tvf))
      goto fail;
    tvf_latch_frequency(&renderComponent->tvf);
    /* The amplitude register opens the note already at the composed
       amplitude, the way `tvf_latch_frequency` opens TVF-F: only the
       periods after note-on are an approach. Opening it at zero would make
       every note fade in over a control period. */
    renderComponent->static_gain_current_q17 =
      renderComponent->static_gain_q17;
    tvf_audio_reset(&renderComponent->tvf_audio);
    renderComponent->tvf_key_modulation = tvfKeyModulation;
    renderComponent->rom_component_offset = component.offset;
    renderComponent->drum_level = drumLevel;
    renderComponent->reverb_send = 127;
    /* Neutral part and user modifiers: the part-level rate and delay
       offsets are a controller-matrix destination this does not model
       yet, so the tone's own rate stands. */
    if (!lfo_common_prepare(&renderer->rom, &tone,
                             lfoControls ? lfoControls->rate : 64, 64,
                             lfoControls ? lfoControls->delay : 64, 64,
                             &renderComponent->lfo1))
      std::memset(&renderComponent->lfo1, 0, sizeof renderComponent->lfo1);
    if (!lfo_local_prepare(&renderer->rom, &component, &renderComponent->lfo2))
      std::memset(&renderComponent->lfo2, 0, sizeof renderComponent->lfo2);
    /* `05_data_model/partial_schema.md` and `07_synthesis/pitch.md` agree,
       against the table in `lfo.md`: the tone-common oscillator's pitch
       depth is the **signed byte at `+17`**, and it is an index rather
       than a depth. Routine `0x6326..0x639e` clamps it to -127..127,
       doubles it as a byte offset and reads the 255-word curve about its
       centre at `0x78402`. That curve is strictly increasing, exactly
       antisymmetric, zero at its centre and saturating at +/-4032 - the
       same `(127 * 127) >> 2` the local field reaches, so its output
       carries the unit the manual's anchor was measured in.

       Read instead as a `+16` BE16, the field spans -9216..+6271, which
       is what `lfo.md` records as an unestablished unit, and applying the
       anchor to those raw numbers gives `Crystal` 721 cents of vibrato. */
    {
      int idx = (int)(int8_t)component.bytes[0x17];
      if (idx < -127)
        idx = -127;
      else if (idx > 127)
        idx = 127;
      uint32_t at = (uint32_t)(kPitchCurveCentre + 2 * idx);
      renderComponent->lfo1_pitch_depth =
        at + 2 <= renderer->rom.size
          ? s16(be16(renderer->rom.bytes + at))
          : 0;
    }
    renderComponent->lfo2_pitch_depth =
      (int16_t)((uint16_t)((uint16_t)component.bytes[0x18] << 8) |
                component.bytes[0x19]);
    renderComponent->lfo1_tva_depth =
      (int16_t)((uint16_t)((uint16_t)component.bytes[0x74] << 8) |
                component.bytes[0x75]);
    renderComponent->lfo2_tva_depth =
      (int16_t)((uint16_t)((uint16_t)component.bytes[0x76] << 8) |
                component.bytes[0x77]);
    renderComponent->lfo1_tvf_depth =
      (int16_t)((uint16_t)((uint16_t)component.bytes[0x44] << 8) |
                component.bytes[0x45]);
    renderComponent->lfo2_tvf_depth =
      (int16_t)((uint16_t)((uint16_t)component.bytes[0x46] << 8) |
                component.bytes[0x47]);
    /* The part's vibrato depth is a centred modifier on the tone's own,
       in the same units (`M-021`). */
    if (lfoControls)
      renderComponent->lfo2_pitch_depth = (int16_t)(
        renderComponent->lfo2_pitch_depth +
        ((int)lfoControls->depth - 64) * 2);
    renderComponent->chorus_send = 127;
    renderComponent->pan_target_position = renderComponent->pan_position;
    renderComponent->keep_release_scale_at_zero = tone.common[0x14] != 0;
    renderComponent->continuous_hold_release = tone.common[0x15] != 0;
    bank = findBank(renderer, zone.descriptor.bank_select);
    if (!bank)
      goto fail;
    pcmBase = zone.descriptor.address_a & ~UINT32_C(0x0f);
    capacity = (size_t)(zone.descriptor.address_c - pcmBase) + 1;
    if (capacity > SIZE_MAX / sizeof *renderComponent->pcm24)
      goto fail;
    renderComponent->pcm24 = (int32_t *)std::malloc(
      capacity * sizeof *renderComponent->pcm24);
    if (!renderComponent->pcm24 ||
        !fce_decode_storage(bank->bytes, bank->size, &zone.descriptor,
                             renderComponent->pcm24, capacity, &pcmBase,
                             &renderComponent->pcm_count))
      goto fail;
    /* Thirty descriptors are read at twice the rate, and the pitch word is
       the only place that can say so: 0x4000 is one octave in the SC-88's
       own 16384-per-octave domain (`07_synthesis/pitch.md`).  The predicate
       is decided from the sample that was just decoded, never from a table
       of offsets - see `wave_loop_reads_double` for the arithmetic, the
       gaps it sits in the middle of, and the standing of the claim.  It is
       applied to the static word, so the per-period recomposition carries it
       for the life of the note. */
    if (wave_loop_reads_double(renderComponent->pcm24,
                                renderComponent->pcm_count, pcmBase,
                                &zone.descriptor)) {
      pitchWord += 0x4000u;
      if (pitchWord > 0x3ffffu)
        pitchWord = 0x3ffffu;
    }
    renderComponent->static_pitch_word = pitchWord;
    pitchWord = pitch_current_word(pitchWord, 0,
                                    renderComponent->pitch_envelope.current);
    if (!oscillator_init(&renderComponent->oscillator, renderComponent->pcm24,
                          renderComponent->pcm_count, pcmBase, &registers,
                          mode, pitchWord, renderer->output_rate,
                          renderer->wrap))
      goto fail;
    renderComponent->active = true;
    ++voice->component_count;
  }
  /* Every tone in the ROM covers every velocity: all 201 single-component
     melodic tones and every single-component rhythm tone carry the window
     0..127, and no two-component tone leaves a velocity uncovered. So zero
     here is not a ROM tone at all - it is `--only-component` naming a
     component the tone does not have. Refuse the note rather than allocate
     slots that sound nothing. */
  if (voice->component_count == 0)
    goto fail;
  return true;

fail:
  renderer_voice_destroy(voice);
  return false;
}

}  // namespace

uint8_t renderer_selector_key(const struct sc88_component *component,
                               uint8_t midiKey)
{
  if (!component || !component->bytes || midiKey > 127)
    return 0;
  int32_t factor = s16(be16(component->bytes + 0x14));
  int32_t key = 60 + floorQ14(((int32_t)midiKey - 60) * factor) +
    s8(component->bytes[0x16]);
  if (key < 0)
    return 0;
  if (key > 127)
    return 127;
  return (uint8_t)key;
}

/* Fractional remainder of the key transform, in pitch units.

   The transform at SC88-CTL 0x60c7..0x6123 holds `(midi_key - 60) * +0x14`
   as a 32-bit value shifted left twice (0x60fb..0x6101), so its high word is
   the integer key and its low word is the remainder in units of 1/65536
   semitone. 0x611e..0x6121 (`5c 05 55` `mov:i.w #0x555,r4`, `ab ac`
   `mulxu.w r3,r4`) multiply that remainder by 0x555 - 1365 pitch units, one
   semitone - and keep the high word of the product. The firmware stores it
   at RAM 0x197c (0x603f, 0x6072) and adds it into the pitch word at 0x6081
   (`f9 19 7c 23` plus `addx.w #0,r2`); nothing else reads 0x197c.

   r4 is cleared at 0x610d and the multiply is skipped on both clamp branches
   (0x6116 key <- 0, 0x611a key <- 0x7f), so a clamped key contributes no
   fraction. The multiply is unsigned, and the remainder is the distance
   above the floored key, so the term is 0..1364 and never negative. */
uint16_t renderer_key_fraction(const struct sc88_component *component,
                                uint8_t midiKey)
{
  if (!component || !component->bytes || midiKey > 127)
    return 0;
  int32_t factor = s16(be16(component->bytes + 0x14));
  int32_t product = ((int32_t)midiKey - 60) * factor;
  int32_t key = 60 + floorQ14(product) + s8(component->bytes[0x16]);
  if (key < 0 || key > 127)
    return 0;
  uint32_t remainder = ((uint32_t)product << 2) & 0xffffu;
  return (uint16_t)((0x555u * remainder) >> 16);
}

bool renderer_portamento_terms(const struct sc88_rom *rom,
                                const struct sc88_tone *tone,
                                const struct sc88_component *component,
                                const struct sc88_wave_descriptor *desc,
                                struct sc88_portamento *portamento)
{
  if (!rom || !rom->bytes || !tone || !tone->common || !component ||
      !component->bytes || !desc || !portamento)
    return false;
  uint32_t table = ((uint32_t)tone->common[0x21] << 16) |
    be16(tone->common + 0x10);
  /* Bounded once, for the highest key the table can be indexed with. */
  if (table + 127u * 2u + 2u > rom->size)
    return false;
  portamento->key_table = table;
  /* Both of the descriptor's pitch corrections, as in the static word; the
     evidence and the open condition are on `wave_pitch_correction`. */
  portamento->fixed = 0x38000 + wave_pitch_correction(desc, true) +
    s16(be16(component->bytes + 0x10));
  portamento->key_factor = s16(be16(component->bytes + 0x14));
  portamento->key_transpose = s8(component->bytes[0x16]);
  portamento->root_key = desc->root_key;
  return true;
}

bool renderer_pitch_word_at(const struct sc88_rom *rom,
                             const struct sc88_portamento *portamento,
                             uint32_t keyQ16, uint32_t *pitchWord)
{
  if (!rom || !rom->bytes || !portamento || !pitchWord ||
      portamento->key_table == 0)
    return false;
  /* The table is indexed with the key the glide is standing on, not the
     transformed one - see the note in `renderer_static_pitch_word`.
     `0x6124` reads it from `0x245a`, which is the same word `0x6063` feeds
     into the transform a few instructions earlier, so both keys of the pair
     come from the glide's current position. */
  uint32_t rawKey = keyQ16 >> 16;
  if (rawKey > 127)
    return false;
  /* SC88-CTL 0x60c7..0x6123 with a fractional key. The key-follow product is
     held shifted left twice - its high word the key, its low word the
     position between keys - and 0x6103 adds the incoming fraction into that
     low word, unscaled: key follow acts on the whole part of the key and the
     glide's sub-semitone position passes through it untouched. */
  int32_t product = (int32_t)((uint32_t)(((int32_t)(keyQ16 >> 16) - 60) *
                                         (int32_t)portamento->key_factor) << 2);
  product = (int32_t)((uint32_t)product + (keyQ16 & 0xffffu));
  int32_t key = 60 + floorQ16(product) + portamento->key_transpose;
  uint32_t selector;
  uint16_t fraction;
  if (key < 0) {
    selector = 0;
    fraction = 0;
  } else if (key > 127) {
    selector = 127;
    fraction = 0;
  } else {
    selector = (uint32_t)key;
    fraction = (uint16_t)((0x555u * ((uint32_t)product & 0xffffu)) >> 16);
  }
  int32_t pitch = portamento->fixed +
    relativePitch((int)selector - (int)portamento->root_key) +
    (int32_t)fraction +
    s16(be16(rom->bytes + portamento->key_table + rawKey * 2u));
  if (pitch < 0)
    pitch = 0;
  if (pitch > 0x3ffff)
    pitch = 0x3ffff;
  *pitchWord = (uint32_t)pitch;
  return true;
}

/* The two keys the pitch word is composed from are NOT the same key.

   `0x6077` calls the two contributors in turn. `0x6124` is the tone-common
   pitch table: `f8 24 5a 86` reads the key word at RAM `0x245a`, `ae 1a`
   doubles it for the word stride, `ed 10 84` takes the table's base from
   tone-common `+0x10` and `e5 21 8c` its page from `+0x21`, and `dc 23`
   adds the entry. `0x245a` is the key BEFORE the component's key-follow
   transform - `0x6063` reads that same word, runs it through `0x60c7` and
   stores the result somewhere else.

   Where it stores it is `0x19fc`, and that is what the other contributor
   gets: `0x607a` (`f1 19 fc 83`) loads the byte at `0x19fc` and hands it to
   `0x609a`, which subtracts the root key at `0x19fd` (`f1 19 fd 3b`, the
   descriptor's `+6` byte, put there by `0x4f10`/`0x4f13`), clamps the
   difference at 24 semitones and indexes the key-to-pitch table with it.

   So the key-difference term uses the TRANSFORMED key and the tone-common
   table uses the RAW one. We used the transformed key for both. */
bool renderer_static_pitch_word(const struct sc88_rom *rom,
                                 const struct sc88_tone *tone,
                                 const struct sc88_component *component,
                                 const struct sc88_wave_descriptor *desc,
                                 uint8_t midiKey, uint8_t selectorKey,
                                 uint16_t keyFraction, uint32_t *pitchWord)
{
  if (!rom || !rom->bytes || !tone || !tone->common || !component ||
      !component->bytes || !desc || !pitchWord || selectorKey > 127 ||
      midiKey > 127)
    return false;
  uint32_t tableOffset = ((uint32_t)tone->common[0x21] << 16) |
    be16(tone->common + 0x10);
  if (tableOffset + (uint32_t)midiKey * 2 + 2 > rom->size)
    return false;
  int32_t pitch = 0x38000 +
    relativePitch((int)selectorKey - desc->root_key) +
    /* SC88-CTL 0x6081 adds RAM 0x197c into the low word of the pitch the
       key table just produced, before the 0x6124 offsets land on it. */
    (int32_t)keyFraction +
    /* The descriptor's `+4` AND its `+14`: 0x612c loads one and 0x6135 adds
       the other, and the ROM's own loop lengths say the tuning is the sum.
       `wave_pitch_correction` carries the measurement and says plainly
       what about the firmware's gate is not recovered. */
    wave_pitch_correction(desc, true) +
    s16(be16(rom->bytes + tableOffset + (uint32_t)midiKey * 2)) +
    s16(be16(component->bytes + 0x10));
  if (pitch < 0)
    pitch = 0;
  if (pitch > 0x3ffff)
    pitch = 0x3ffff;
  *pitchWord = (uint32_t)pitch;
  return true;
}

bool renderer_init(struct sc88_renderer *renderer, const uint8_t *controlRom,
                    size_t controlRomSize, const struct sc88_wave_bank *banks,
                    size_t bankCount, double outputRate,
                    enum sc88_fractional_wrap wrap)
{
  bool occupied[SC88_WAVE_BANK_COUNT] = {false};

  if (!renderer || !banks || bankCount != SC88_WAVE_BANK_COUNT ||
      outputRate <= 0.0 || wrap < SC88_WRAP_FULL_CARRY ||
      wrap > SC88_WRAP_FRACTION_ONLY)
    return false;
  std::memset(renderer, 0, sizeof *renderer);
  if (!rom_init(&renderer->rom, controlRom, controlRomSize))
    return false;
  for (unsigned c = 0; c < 128; ++c) {
    uint16_t raw = 0;
    renderer->send_ok[c] = control_gain_q15(&renderer->rom, (uint8_t)c, &raw);
    renderer->send_gain[c] = raw / 32768.0f;
  }
  for (size_t i = 0; i < bankCount; ++i) {
    int index = bankIndex(banks[i].selector);
    if (index < 0 || occupied[index] || !banks[i].bytes ||
        banks[i].size != SC88_WAVE_BANK_SIZE)
      return false;
    occupied[index] = true;
    renderer->banks[index] = banks[i];
  }
  renderer->output_rate = outputRate;
  renderer->wrap = wrap;
  renderer->levels.master = 127;
  renderer->levels.secondary = 127;
  renderer->levels.part = 127;
  renderer->levels.expression = 127;
  renderer->pan.master = 64;
  renderer->pan.part = 64;
  renderer->tvf_controls.part_cutoff = 64;
  renderer->tvf_controls.secondary_cutoff = 64;
  renderer->tvf_controls.part_resonance = 64;
  renderer->tvf_controls.secondary_resonance = 64;
  renderer->tvf_controls.matrix_cutoff = 0;
  renderer->tva_controls.part_attack = 64;
  renderer->tva_controls.secondary_attack = 64;
  renderer->tva_controls.part_decay = 64;
  renderer->tva_controls.secondary_decay = 64;
  return true;
}

void renderer_set_pan(struct sc88_renderer *renderer,
                       const struct sc88_pan_controls *pan)
{
  if (!renderer || !pan || pan->master < 1 || pan->master > 127 ||
      pan->part > 127)
    return;
  renderer->pan = *pan;
}

void renderer_set_levels(struct sc88_renderer *renderer,
                          const struct sc88_tva_levels *levels)
{
  if (!renderer || !levels || levels->master > 127 ||
      levels->secondary > 127 || levels->part > 127 ||
      levels->expression > 127)
    return;
  renderer->levels = *levels;
}

void renderer_set_only_component(struct sc88_renderer *renderer,
                                  unsigned which)
{
  if (renderer)
    renderer->only_component = which;
}

void renderer_set_tvf_audio_transfer(struct sc88_renderer *renderer,
                                      sc88_tvf_audio_transfer_fn transfer,
                                      void *user)
{
  if (!renderer)
    return;
  renderer->tvf_audio_transfer = transfer;
  renderer->tvf_audio_user = user;
}

void renderer_set_tvf_controls(struct sc88_renderer *renderer,
                                const struct sc88_tvf_controls *controls)
{
  if (!renderer || !controls || controls->part_cutoff > 127 ||
      controls->secondary_cutoff > 127 || controls->part_resonance > 127 ||
      controls->secondary_resonance > 127)
    return;
  renderer->tvf_controls = *controls;
}

void renderer_voice_destroy(struct sc88_render_voice *voice)
{
  if (!voice)
    return;
  for (unsigned i = 0; i < SC88_MAX_TONE_COMPONENTS; ++i)
    std::free(voice->components[i].pcm24);
  std::memset(voice, 0, sizeof *voice);
}

bool renderer_note_on(const struct sc88_renderer *renderer,
                       struct sc88_render_voice *voice, uint8_t variation,
                       uint8_t program, uint8_t key, uint8_t velocity)
{
  if (!renderer)
    return false;
  return renderer_note_on_with_levels(renderer, voice, variation, program,
                                       key, velocity, &renderer->levels);
}

bool renderer_note_on_with_levels(const struct sc88_renderer *renderer,
                                   struct sc88_render_voice *voice,
                                   uint8_t variation, uint8_t program,
                                   uint8_t key, uint8_t velocity,
                                   const struct sc88_tva_levels *levels)
{
  if (!renderer)
    return false;
  return renderer_note_on_with_controls(renderer, voice, variation, program,
                                         key, velocity, levels,
                                         &renderer->pan);
}

bool renderer_note_on_with_controls(const struct sc88_renderer *renderer,
                                     struct sc88_render_voice *voice,
                                     uint8_t variation, uint8_t program,
                                     uint8_t key, uint8_t velocity,
                                     const struct sc88_tva_levels *levels,
                                     const struct sc88_pan_controls *pan)
{
  if (!renderer)
    return false;
  return renderer_note_on_with_part_controls(
    renderer, voice, SC88_TONE_MAP_SC88, variation, program, key, velocity,
    levels, pan, &renderer->tvf_controls, &renderer->tva_controls, nullptr);
}

bool renderer_note_on_with_glide(
  const struct sc88_renderer *renderer, struct sc88_render_voice *voice,
  uint8_t map, uint8_t variation, uint8_t program, uint8_t key,
  uint8_t zoneKey, uint8_t velocity, const struct sc88_tva_levels *levels,
  const struct sc88_pan_controls *pan,
  const struct sc88_tvf_controls *tvfControls,
  const struct sc88_tva_controls *tvaControls,
  const struct sc88_lfo_controls *lfoControls)
{
  uint32_t toneOffset;
  if (!renderer ||
      !rom_select_melodic(&renderer->rom, map, variation, program,
                           &toneOffset))
    return false;
  return noteOnTone(renderer, voice, toneOffset, key, zoneKey, velocity,
                     levels, SC88_TVA_NO_DRUM_LEVEL, pan, tvfControls,
                     tvaControls, lfoControls);
}

bool renderer_note_on_with_part_controls(
  const struct sc88_renderer *renderer, struct sc88_render_voice *voice,
  uint8_t map, uint8_t variation, uint8_t program, uint8_t key,
  uint8_t velocity, const struct sc88_tva_levels *levels,
  const struct sc88_pan_controls *pan,
  const struct sc88_tvf_controls *tvfControls,
  const struct sc88_tva_controls *tvaControls,
  const struct sc88_lfo_controls *lfoControls)
{
  return renderer_note_on_with_glide(renderer, voice, map, variation, program,
                                      key, key, velocity, levels, pan,
                                      tvfControls, tvaControls, lfoControls);
}

bool renderer_note_on_drum(
  const struct sc88_renderer *renderer, struct sc88_render_voice *voice,
  uint8_t map, uint8_t program, uint8_t key, uint8_t velocity,
  const struct sc88_tva_levels *levels,
  const struct sc88_pan_controls *pan,
  const struct sc88_tvf_controls *tvfControls,
  const struct sc88_tva_controls *tvaControls,
  const struct sc88_lfo_controls *lfoControls,
  const struct sc88_drum_overlay *overlay, uint8_t setup,
  struct sc88_drum_note *note)
{
  struct sc88_drum_note slot;
  uint32_t kit;
  if (!renderer || !levels || !pan ||
      !rom_select_drum(&renderer->rom, map, program, &kit) ||
      !rom_open_drum_note_overlaid(&renderer->rom, kit, key, overlay, setup,
                                    &slot))
    return false;
  /* The kit record carries this key's own level and pan, and the key the
     tone is actually played at - a kick is not the sample transposed to the
     key that triggered it.
     The level is a **per-note** property, so it does not belong in any of
     the four part-level sources, all of which are part or global controls;
     hijacking the secondary level for it both under-drove the kit and threw
     away whatever that control was doing. It is a fifth source of its own:
     `72a3..72ab` subtracts it from the same headroom, through the same
     table, before the component's static attenuation at `72b2`, so it
     travels to `tva_gain_from_headroom_q17` beside the other four.
     `4d76` gates it on bit 7 of this note's `+0x280` assign-group byte
     being clear, which is the state of every sounding slot in all 24
     kits. */
  struct sc88_tva_levels drumLevels = *levels;
  struct sc88_pan_controls drumPan = *pan;
  uint8_t drumLevel = ((slot.assign_group & 0x80u) == 0u && slot.level <= 127)
    ? slot.level : (uint8_t)SC88_TVA_NO_DRUM_LEVEL;
  if (slot.pan >= 1 && slot.pan <= 127)
    drumPan.part = slot.pan;
  if (note)
    *note = slot;
  if (!noteOnTone(renderer, voice, slot.tone_offset,
                  slot.play_note <= 127 ? slot.play_note : key,
                  slot.play_note <= 127 ? slot.play_note : key,
                  velocity, &drumLevels, drumLevel, &drumPan, tvfControls,
                  tvaControls, lfoControls))
    return false;
  for (unsigned i = 0; i < voice->component_count; ++i) {
    voice->components[i].reverb_send = slot.reverb_send;
    voice->components[i].chorus_send = slot.chorus_send;
  }
  voice->ignore_note_off = (slot.flags & 0x01u) == 0u;
  return true;
}

bool renderer_voice_active(const struct sc88_render_voice *voice)
{
  if (!voice)
    return false;
  for (unsigned i = 0; i < voice->component_count; ++i)
    if (voice->components[i].active &&
        !voice->components[i].oscillator.ended)
      return true;
  return false;
}

size_t renderer_render(struct sc88_render_voice *voice, float *stereo,
                        size_t frames)
{
  size_t frame;
  if (!voice || !stereo)
    return 0;
  for (frame = 0; frame < frames; ++frame) {
    float left = 0.0f;
    float right = 0.0f;
    bool active = false;
    for (unsigned i = 0; i < voice->component_count; ++i) {
      struct sc88_render_component *component = voice->components + i;
      float sample;
      if (component->active &&
          oscillator_next(&component->oscillator, &sample)) {
        if (voice->tvf_audio_transfer)
          sample = voice->tvf_audio_transfer(
            voice->tvf_audio_user, &component->tvf_audio, &component->tvf,
            1.0, sample);
        float gained = sample *
          (sc88_render_static_gain_q17(component, 1.0) / 131072.0f);
        left += gained * (component->left_gain_q15 / 32768.0f);
        right += gained * (component->right_gain_q15 / 32768.0f);
        active = true;
        if (component->oscillator.ended)
          component->active = false;
      } else {
        component->active = false;
      }
    }
    if (!active)
      break;
    stereo[frame * 2] = left;
    stereo[frame * 2 + 1] = right;
  }
  return frame;
}

}}  // namespace EmuSC::Xp

// Compatibility shims for callers not yet ported to the EmuSC::Xp API.
extern "C" {

uint8_t sc88_renderer_selector_key(const struct sc88_component *component,
                                   uint8_t midi_key)
{
  return EmuSC::Xp::renderer_selector_key(component, midi_key);
}

uint16_t sc88_renderer_key_fraction(const struct sc88_component *component,
                                    uint8_t midi_key)
{
  return EmuSC::Xp::renderer_key_fraction(component, midi_key);
}

bool sc88_renderer_portamento_terms(const struct sc88_rom *rom,
                                    const struct sc88_tone *tone,
                                    const struct sc88_component *component,
                                    const struct sc88_wave_descriptor *desc,
                                    struct sc88_portamento *portamento)
{
  return EmuSC::Xp::renderer_portamento_terms(rom, tone, component, desc,
                                               portamento);
}

bool sc88_renderer_pitch_word_at(const struct sc88_rom *rom,
                                 const struct sc88_portamento *portamento,
                                 uint32_t key_q16, uint32_t *pitch_word)
{
  return EmuSC::Xp::renderer_pitch_word_at(rom, portamento, key_q16,
                                            pitch_word);
}

bool sc88_renderer_static_pitch_word(const struct sc88_rom *rom,
                                     const struct sc88_tone *tone,
                                     const struct sc88_component *component,
                                     const struct sc88_wave_descriptor *desc,
                                     uint8_t midi_key,
                                     uint8_t selector_key,
                                     uint16_t key_fraction,
                                     uint32_t *pitch_word)
{
  return EmuSC::Xp::renderer_static_pitch_word(
    rom, tone, component, desc, midi_key, selector_key, key_fraction,
    pitch_word);
}

bool sc88_renderer_init(struct sc88_renderer *renderer,
                        const uint8_t *control_rom, size_t control_rom_size,
                        const struct sc88_wave_bank *banks, size_t bank_count,
                        double output_rate, enum sc88_fractional_wrap wrap)
{
  return EmuSC::Xp::renderer_init(renderer, control_rom, control_rom_size,
                                   banks, bank_count, output_rate, wrap);
}

void sc88_renderer_set_levels(struct sc88_renderer *renderer,
                              const struct sc88_tva_levels *levels)
{
  EmuSC::Xp::renderer_set_levels(renderer, levels);
}

void sc88_renderer_set_pan(struct sc88_renderer *renderer,
                           const struct sc88_pan_controls *pan)
{
  EmuSC::Xp::renderer_set_pan(renderer, pan);
}

void sc88_renderer_set_only_component(struct sc88_renderer *renderer,
                                      unsigned which)
{
  EmuSC::Xp::renderer_set_only_component(renderer, which);
}

void sc88_renderer_set_tvf_audio_transfer(
  struct sc88_renderer *renderer, sc88_tvf_audio_transfer_fn transfer,
  void *user)
{
  EmuSC::Xp::renderer_set_tvf_audio_transfer(renderer, transfer, user);
}

void sc88_renderer_set_tvf_controls(
  struct sc88_renderer *renderer, const struct sc88_tvf_controls *controls)
{
  EmuSC::Xp::renderer_set_tvf_controls(renderer, controls);
}

bool sc88_renderer_note_on(const struct sc88_renderer *renderer,
                           struct sc88_render_voice *voice,
                           uint8_t variation, uint8_t program,
                           uint8_t key, uint8_t velocity)
{
  return EmuSC::Xp::renderer_note_on(renderer, voice, variation, program,
                                      key, velocity);
}

bool sc88_renderer_note_on_with_levels(
  const struct sc88_renderer *renderer, struct sc88_render_voice *voice,
  uint8_t variation, uint8_t program, uint8_t key, uint8_t velocity,
  const struct sc88_tva_levels *levels)
{
  return EmuSC::Xp::renderer_note_on_with_levels(
    renderer, voice, variation, program, key, velocity, levels);
}

bool sc88_renderer_note_on_with_controls(
  const struct sc88_renderer *renderer, struct sc88_render_voice *voice,
  uint8_t variation, uint8_t program, uint8_t key, uint8_t velocity,
  const struct sc88_tva_levels *levels,
  const struct sc88_pan_controls *pan)
{
  return EmuSC::Xp::renderer_note_on_with_controls(
    renderer, voice, variation, program, key, velocity, levels, pan);
}

bool sc88_renderer_note_on_with_part_controls(
  const struct sc88_renderer *renderer, struct sc88_render_voice *voice,
  uint8_t map, uint8_t variation, uint8_t program, uint8_t key,
  uint8_t velocity,
  const struct sc88_tva_levels *levels,
  const struct sc88_pan_controls *pan,
  const struct sc88_tvf_controls *tvf_controls,
  const struct sc88_tva_controls *tva_controls,
  const struct sc88_lfo_controls *lfo_controls)
{
  return EmuSC::Xp::renderer_note_on_with_part_controls(
    renderer, voice, map, variation, program, key, velocity, levels, pan,
    tvf_controls, tva_controls, lfo_controls);
}

bool sc88_renderer_note_on_with_glide(
  const struct sc88_renderer *renderer, struct sc88_render_voice *voice,
  uint8_t map, uint8_t variation, uint8_t program, uint8_t key,
  uint8_t zone_key, uint8_t velocity,
  const struct sc88_tva_levels *levels,
  const struct sc88_pan_controls *pan,
  const struct sc88_tvf_controls *tvf_controls,
  const struct sc88_tva_controls *tva_controls,
  const struct sc88_lfo_controls *lfo_controls)
{
  return EmuSC::Xp::renderer_note_on_with_glide(
    renderer, voice, map, variation, program, key, zone_key, velocity,
    levels, pan, tvf_controls, tva_controls, lfo_controls);
}

bool sc88_renderer_note_on_drum(
  const struct sc88_renderer *renderer, struct sc88_render_voice *voice,
  uint8_t map, uint8_t program, uint8_t key, uint8_t velocity,
  const struct sc88_tva_levels *levels,
  const struct sc88_pan_controls *pan,
  const struct sc88_tvf_controls *tvf_controls,
  const struct sc88_tva_controls *tva_controls,
  const struct sc88_lfo_controls *lfo_controls,
  const struct sc88_drum_overlay *overlay, uint8_t setup,
  struct sc88_drum_note *note)
{
  return EmuSC::Xp::renderer_note_on_drum(
    renderer, voice, map, program, key, velocity, levels, pan, tvf_controls,
    tva_controls, lfo_controls, overlay, setup, note);
}

void sc88_renderer_voice_destroy(struct sc88_render_voice *voice)
{
  EmuSC::Xp::renderer_voice_destroy(voice);
}

bool sc88_renderer_voice_active(const struct sc88_render_voice *voice)
{
  return EmuSC::Xp::renderer_voice_active(voice);
}

size_t sc88_renderer_render(struct sc88_render_voice *voice,
                            float *stereo, size_t frames)
{
  return EmuSC::Xp::renderer_render(voice, stereo, frames);
}

}  // extern "C"
