/* SPDX-License-Identifier: CC0-1.0 */
/*
 *  Roland JV-1080 voice engine for the XP engine (engines/xp/).
 *
 *  What the shared device layer calls through XpDeviceProfile::voiceEngine:
 *  sixteen parts, a voice pool over this device's own behavioural voice
 *  model (jv1080_voice.cc), and the parameter writes that fill a part in.
 *
 *  THIS IS NOT A FIRMWARE PORT. The sibling engine in engine.h/renderer.h
 *  is one - the SC-88's firmware is dumped, so its voice state is that
 *  chip's own register and RAM layout and its scheduler is that firmware's
 *  control period. This device's synthesis engine is in the SH7034's
 *  undumped 64 KB internal mask ROM. What is exact here is everything read
 *  out of the external ROM: the parameter address map, the bit-packed
 *  records, the bank selection and the wave chain. What the voices do with
 *  those values is measured behaviour, and jv1080_voice.cc carries each
 *  law's own measurement id and says where the measurement runs out.
 *
 *  Allocation is oldest-first with no reserve. The device's own law is
 *  measured - the victim is the oldest voice among the parts at or over
 *  their voice reserve, a part inside its reserve being exempt (`M-031`,
 *  `M-072`) - but the reserve is a performance-part field and no
 *  performance is loaded here, so the exemption has nothing to test. The
 *  ordering is the measured one; the exemption is absent, not modelled
 *  wrongly.
 */
#include "jv1080.h"

#include "../packed_rom.h"
#include "../rom.h"

#include <cstdlib>
#include <cstring>

namespace EmuSC { namespace Xp {

namespace {

/* The device's own part count, and the pool the shared engine ceiling
   allows. Sixteen parts is this machine's, measured alongside its
   polyphony (`M-031`, `M-072`); the pool is capped by the engine's own
   slot ceiling so the two cannot disagree. */
const unsigned kParts = 16u;
const unsigned kMaxVoices = XP_ENGINE_SLOT_COUNT;

/* Which block of a temporary-patch address names what. The tone blocks are
   two apart because the machine's own address map puts them there, and the
   rhythm set lives at part index nine. */
const unsigned kBlockPatchCommon = 0x00u;
const unsigned kBlockFirstTone = 0x10u;
const unsigned kBlockToneStride = 0x02u;

struct Part {
  uint8_t common[XP_JV1080_PATCH_COMMON_FIELDS];
  uint8_t tone[XP_JV1080_TONES_PER_PATCH][XP_JV1080_TONE_FIELDS];
  /* The bank-select pair as received, resolved to a packed bank only when
     a program change arrives - which is the order the device resolves them
     in, since either may come first. */
  uint8_t bank_msb;
  uint8_t bank_lsb;
};

struct Voice {
  struct XpJv1080Voice voice;
  int32_t *pcm;
  size_t capacity;
  uint64_t serial;
  uint8_t part;
  uint8_t key;
  bool allocated;
  bool key_down;
};

struct Engine {
  struct xp_rom rom;
  const uint8_t *banks[XP_WAVE_BANK_COUNT];
  size_t bank_sizes[XP_WAVE_BANK_COUNT];
  double output_rate;
  unsigned max_voices;
  uint64_t serial;
  struct Part parts[kParts];
  struct Voice voices[kMaxVoices];
};

void free_voice(struct Voice *voice)
{
  std::free(voice->pcm);
  voice->pcm = nullptr;
  voice->capacity = 0;
  voice->allocated = false;
  voice->key_down = false;
  std::memset(&voice->voice, 0, sizeof voice->voice);
}

/* A part's power-on patch: silent, centred, at full level. Every tone
   switch is zero, so a part nothing has written to sounds nothing rather
   than sounding whatever an all-zero tone record would resolve to. */
void reset_part(const struct xp_rom *rom, struct Part *part)
{
  const struct XpDeviceProfile *profile = xp_profile(rom);
  std::memset(part, 0, sizeof *part);
  part->common[profile->patchFieldLevel] = 127u;
  part->common[profile->patchFieldPan] = 64u;
  part->bank_msb = 0xffu;
  part->bank_lsb = 0xffu;
}

/* The oldest voice, or a free one if there is a free one. Returns null
   only when the pool is empty, which cannot happen with max_voices >= 1. */
struct Voice *take_voice(struct Engine *engine)
{
  struct Voice *oldest = nullptr;
  for (unsigned i = 0; i < engine->max_voices; ++i) {
    struct Voice *voice = engine->voices + i;
    if (!voice->allocated)
      return voice;
    if (!oldest || voice->serial < oldest->serial)
      oldest = voice;
  }
  if (oldest)
    free_voice(oldest);
  return oldest;
}

bool start_tone(struct Engine *engine, unsigned part, unsigned tone,
                 unsigned key, unsigned velocity)
{
  const uint8_t *bytes = engine->parts[part].tone[tone];
  size_t samples = 0;
  if (!jv1080_voice_span(&engine->rom, bytes, key, velocity, &samples) ||
      !samples)
    return false;

  struct Voice *voice = take_voice(engine);
  if (!voice)
    return false;
  /* The decode is differential, so a sample's value depends on every delta
     before it in the block and the element's span is decoded whole at
     note-on rather than streamed. The sibling engine's renderer does the
     same thing for the same reason. */
  int32_t *pcm = (int32_t *)std::malloc(samples * sizeof *pcm);
  if (!pcm)
    return false;
  if (!jv1080_voice_start(&engine->rom, bytes,
                          engine->parts[part].common[
                            xp_profile(&engine->rom)->patchFieldLevel],
                          engine->parts[part].common[
                            xp_profile(&engine->rom)->patchFieldPan],
                          key, velocity, engine->banks, engine->bank_sizes,
                          pcm, samples, engine->output_rate,
                          &voice->voice)) {
    std::free(pcm);
    return false;
  }
  voice->pcm = pcm;
  voice->capacity = samples;
  voice->serial = ++engine->serial;
  voice->part = (uint8_t)part;
  voice->key = (uint8_t)key;
  voice->allocated = true;
  voice->key_down = true;
  return true;
}

/* Apply one DT1 payload to a part's decoded bytes. Payload byte k is field
   k, which is what "the descriptor index within a group is the SysEx
   address offset" means read from the wire's side.

   AN EIGHT-BIT FIELD ARRIVES AS TWO NIBBLES, MOST SIGNIFICANT FIRST, and
   the descriptor is what says which fields those are. Measured rather than
   assumed: over the 45 enabled tone frames of this device's own first
   factory demo song, reading the pair as `(a << 4) | b` resolves 45 of 45
   wave references to a real multisample row while reading it as
   `a | (b << 7)` resolves 11, and the 45 come out as the wave names that
   song would use. The device's own documentation states the rule for its
   one eight-bit patch-common field (the default tempo, "two nibbles most
   significant first, a nibble outside 0-15 being discarded"); this is that
   rule holding for every eight-bit field. */
void apply_block(const struct xp_rom *rom, unsigned group,
                  const uint8_t *payload, size_t count, uint8_t *fields,
                  size_t fieldCount)
{
  for (size_t k = 0; k < count && k < fieldCount; ++k) {
    if (packed_field_is_eight_bit(rom, group, (unsigned)k) && k + 1u < count) {
      uint8_t value = (uint8_t)((payload[k] << 4) | (payload[k + 1u] & 0x0fu));
      fields[k] = value;
      if (k + 1u < fieldCount)
        fields[k + 1u] = value;   /* the alias descriptor, same bits */
      ++k;
    } else {
      fields[k] = payload[k];
    }
  }
}

/* Which packed group a melodic bank's common and part blocks are. Read off
   the bank rather than written down, so the group numbering stays the
   profile's. */
bool patch_groups(const struct xp_rom *rom, unsigned *common, unsigned *tone)
{
  const struct XpDeviceProfile *profile = xp_profile(rom);
  if (!profile->packedMelodicBankCount)
    return false;
  const struct XpPackedBank &bank =
    profile->packedBanks[profile->packedMelodicBanks[0]];
  *common = bank.commonGroup;
  *tone = bank.partGroup;
  return true;
}

/* Load a factory patch into a part, which is what a program change does
   once a bank has been selected. */
bool load_patch(struct Engine *engine, unsigned part, unsigned bank,
                 unsigned program)
{
  struct xp_packed_record record;
  if (!packed_open(&engine->rom, bank, program, &record))
    return false;
  unsigned patchLevel = 0;
  unsigned patchPan = 64;
  const struct XpDeviceProfile *profile = xp_profile(&engine->rom);
  for (unsigned t = 0; t < XP_JV1080_TONES_PER_PATCH; ++t) {
    if (t >= record.part_count) {
      std::memset(engine->parts[part].tone[t], 0,
                  XP_JV1080_TONE_FIELDS);
      continue;
    }
    if (!jv1080_patch_tone(&engine->rom, &record, t,
                           engine->parts[part].tone[t], &patchLevel,
                           &patchPan))
      return false;
  }
  for (unsigned f = 0; f < XP_JV1080_PATCH_COMMON_FIELDS; ++f) {
    int value = 0;
    if (!packed_common_field(&engine->rom, &record, f, &value))
      return false;
    engine->parts[part].common[f] = (uint8_t)(value & 0xff);
  }
  (void)profile;
  return true;
}

}  // namespace

/* ---- the injected table's own functions ---------------------------- */

bool engine_create(void **state, const struct xp_rom *rom,
                    const uint8_t *const banks[], const size_t bankSizes[],
                    unsigned bankCount, double outputRate)
{
  if (!state || !rom || !rom->bytes || !banks || !bankSizes ||
      bankCount != XP_WAVE_BANK_COUNT || outputRate <= 0.0)
    return false;
  struct Engine *engine = (struct Engine *)std::calloc(1, sizeof *engine);
  if (!engine)
    return false;
  engine->rom = *rom;
  for (unsigned b = 0; b < XP_WAVE_BANK_COUNT; ++b) {
    if (!banks[b]) {
      std::free(engine);
      return false;
    }
    engine->banks[b] = banks[b];
    engine->bank_sizes[b] = bankSizes[b];
  }
  engine->output_rate = outputRate;
  engine->max_voices = xp_profile(rom)->defaultMaxVoices;
  if (!engine->max_voices || engine->max_voices > kMaxVoices)
    engine->max_voices = kMaxVoices;
  for (unsigned p = 0; p < kParts; ++p)
    reset_part(rom, engine->parts + p);
  *state = engine;
  return true;
}

void engine_free(void *state)
{
  struct Engine *engine = (struct Engine *)state;
  if (!engine)
    return;
  for (unsigned i = 0; i < kMaxVoices; ++i)
    free_voice(engine->voices + i);
  std::free(engine);
}

void engine_reset(void *state)
{
  struct Engine *engine = (struct Engine *)state;
  if (!engine)
    return;
  for (unsigned i = 0; i < kMaxVoices; ++i)
    free_voice(engine->voices + i);
  for (unsigned p = 0; p < kParts; ++p)
    reset_part(&engine->rom, engine->parts + p);
  engine->serial = 0;
}

bool engine_note_on_jv(void *state, unsigned part, unsigned key,
                        unsigned velocity)
{
  struct Engine *engine = (struct Engine *)state;
  if (!engine || part >= kParts || key > 127u || velocity == 0u ||
      velocity > 127u)
    return false;
  /* The four tones of a patch start together and in phase, which is
     measured: one to four tones sum coherently to within 0.02 dB of
     `20*log10(N)` (`M-037`). */
  unsigned started = 0;
  for (unsigned t = 0; t < XP_JV1080_TONES_PER_PATCH; ++t)
    started += start_tone(engine, part, t, key, velocity) ? 1u : 0u;
  return started != 0u;
}

bool engine_note_off_jv(void *state, unsigned part, unsigned key)
{
  struct Engine *engine = (struct Engine *)state;
  if (!engine || part >= kParts || key > 127u)
    return false;
  unsigned released = 0;
  for (unsigned i = 0; i < kMaxVoices; ++i) {
    struct Voice *voice = engine->voices + i;
    if (!voice->allocated || !voice->key_down || voice->part != part ||
        voice->key != key)
      continue;
    voice->key_down = false;
    jv1080_voice_release(&voice->voice);
    ++released;
  }
  return released != 0u;
}

bool engine_bank_select(void *state, unsigned part, unsigned msb,
                         unsigned lsb)
{
  struct Engine *engine = (struct Engine *)state;
  if (!engine || part >= kParts)
    return false;
  if (msb <= 127u)
    engine->parts[part].bank_msb = (uint8_t)msb;
  if (lsb <= 127u)
    engine->parts[part].bank_lsb = (uint8_t)lsb;
  return true;
}

bool engine_program_change(void *state, unsigned part, unsigned program)
{
  struct Engine *engine = (struct Engine *)state;
  if (!engine || part >= kParts || program > 127u)
    return false;
  const struct XpDeviceProfile *profile = xp_profile(&engine->rom);
  uint8_t msb = engine->parts[part].bank_msb;
  uint8_t lsb = engine->parts[part].bank_lsb;
  /* Absent a bank select, the device's own reset state applies; here the
     first listed pair stands in for it, which is this device's PR-A. */
  for (unsigned i = 0; i < profile->packedBankSelectCount; ++i) {
    const struct XpBankSelect &select = profile->packedBankSelect[i];
    bool wildcard = msb == 0xffu;
    if ((wildcard && i) || (!wildcard && (select.msb != msb ||
                                          select.lsb != lsb)))
      continue;
    return load_patch(engine, part, select.bank, program);
  }
  return false;                  /* a card or expansion group, unheld */
}

/* One DT1 payload. This device's parameter address is four seven-bit
   bytes - group, part, block, offset - and every block this acts on is
   addressed at its own offset zero. */
bool engine_sysex_block(void *state, const uint8_t *address,
                         unsigned addressBytes, const uint8_t *data,
                         size_t count)
{
  struct Engine *engine = (struct Engine *)state;
  if (!engine || !address || !data || !count || addressBytes != 4u)
    return false;
  unsigned commonGroup = 0;
  unsigned toneGroup = 0;
  if (!patch_groups(&engine->rom, &commonGroup, &toneGroup))
    return false;

  unsigned a1 = address[0];
  unsigned part = address[1];
  unsigned block = address[2];
  unsigned within = address[3];

  /* Only the temporary patch areas are acted on. The device addresses them
     two ways - `02 pp bb xx` for performance-mode part pp, `03 00 bb xx`
     for patch mode - and the second is the first with part zero. */
  if (a1 == 0x03u)
    part = 0u;
  else if (a1 != 0x02u)
    return false;                /* system, performance or rhythm: unheld */
  if (part >= kParts || within)
    return false;

  if (block == kBlockPatchCommon) {
    apply_block(&engine->rom, commonGroup, data, count,
                 engine->parts[part].common, XP_JV1080_PATCH_COMMON_FIELDS);
    return true;
  }
  if (block >= kBlockFirstTone &&
      block < kBlockFirstTone +
                kBlockToneStride * XP_JV1080_TONES_PER_PATCH &&
      (block - kBlockFirstTone) % kBlockToneStride == 0u) {
    unsigned tone = (block - kBlockFirstTone) / kBlockToneStride;
    apply_block(&engine->rom, toneGroup, data, count,
                 engine->parts[part].tone[tone], XP_JV1080_TONE_FIELDS);
    return true;
  }
  return false;                  /* a rhythm key, or a block not held */
}

void engine_render_jv(void *state, float *stereo, size_t frames)
{
  struct Engine *engine = (struct Engine *)state;
  if (!engine || !stereo || !frames)
    return;
  /* The caller's buffer is interleaved; the voice model writes into two
     planar spans, so the sum is built planar and interleaved once. A
     block-sized scratch pair rather than a per-sample transpose. */
  static const size_t kChunk = 512u;
  float left[kChunk];
  float right[kChunk];
  size_t done = 0;
  while (done < frames) {
    size_t n = frames - done;
    if (n > kChunk)
      n = kChunk;
    std::memset(left, 0, n * sizeof *left);
    std::memset(right, 0, n * sizeof *right);
    for (unsigned i = 0; i < kMaxVoices; ++i) {
      struct Voice *voice = engine->voices + i;
      if (!voice->allocated)
        continue;
      if (!jv1080_voice_render(&voice->voice, left, right, n))
        free_voice(voice);
    }
    for (size_t k = 0; k < n; ++k) {
      stereo[(done + k) * 2u] += left[k];
      stereo[(done + k) * 2u + 1u] += right[k];
    }
    done += n;
  }
}

bool engine_set_max_voices_jv(void *state, unsigned maxVoices)
{
  struct Engine *engine = (struct Engine *)state;
  if (!engine || !maxVoices || maxVoices > kMaxVoices)
    return false;
  for (unsigned i = maxVoices; i < kMaxVoices; ++i)
    free_voice(engine->voices + i);
  engine->max_voices = maxVoices;
  return true;
}

unsigned engine_active_voices(const void *state)
{
  const struct Engine *engine = (const struct Engine *)state;
  if (!engine)
    return 0;
  unsigned active = 0;
  for (unsigned i = 0; i < kMaxVoices; ++i)
    active += engine->voices[i].allocated ? 1u : 0u;
  return active;
}

}}  // namespace EmuSC::Xp

extern "C" {

const struct XpVoiceEngineOps JV1080_VOICE_ENGINE = {
  EmuSC::Xp::engine_create,
  EmuSC::Xp::engine_free,
  EmuSC::Xp::engine_reset,
  EmuSC::Xp::engine_note_on_jv,
  EmuSC::Xp::engine_note_off_jv,
  EmuSC::Xp::engine_bank_select,
  EmuSC::Xp::engine_program_change,
  EmuSC::Xp::engine_sysex_block,
  EmuSC::Xp::engine_render_jv,
  EmuSC::Xp::engine_set_max_voices_jv,
  EmuSC::Xp::engine_active_voices,
};

}  // extern "C"
