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
#include "../reverb.h"
#include "../chorus.h"
#include "../common/constants.h"

#include "../packed_rom.h"
#include "../rom.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace EmuSC { namespace Xp {

namespace {

/* The device's own part count, and the pool the shared engine ceiling
   allows. Sixteen parts is this machine's, measured alongside its
   polyphony (`M-031`, `M-072`); the pool is capped by the engine's own
   slot ceiling so the two cannot disagree. */
/* The performance common block, and the fields of it this engine reads:
   the reverb block at 0x28..0x2C - type, level, time, HF damp, and a
   feedback this engine does not plumb. */
const unsigned kPerfCommonFields = 66u;
const unsigned kReverbTypeField = 0x28u;
const unsigned kReverbLevelField = 0x29u;
const unsigned kReverbTimeField = 0x2au;
const unsigned kReverbDampField = 0x2bu;
const unsigned kChorusLevelField = 0x22u;
const unsigned kChorusRateField = 0x23u;
const unsigned kChorusDepthField = 0x24u;
const unsigned kChorusPreDelayField = 0x25u;
const unsigned kChorusOutputField = 0x27u;
/* MIX / REVERB / MIX+REV, the machine's own order (`02_rom/strings.md`
   `0x057109`). */
const unsigned kChorusOutMix = 0u;
const unsigned kChorusOutReverb = 1u;

const unsigned kParts = 16u;
const unsigned kMaxVoices = XP_ENGINE_SLOT_COUNT;

/* Which block of a temporary-patch address names what. The tone blocks are
   two apart because the machine's own address map puts them there, and the
   rhythm set lives at part index nine. */
const unsigned kBlockPatchCommon = 0x00u;
const unsigned kBlockFirstTone = 0x10u;
const unsigned kBlockToneStride = 0x02u;

/* A block addresses 128 parameters - the offset byte is seven bits - which
   is why a 130-parameter tone needs two of them. */
const unsigned kBlockParameters = 0x80u;

/* The performance-part block is twenty decoded bytes; the engine reads four
   of them by role and keeps the rest so a write is not discarded. */
const unsigned kPartFields = 20u;

/* The rhythm set: one common block and one record per key it holds. */
const unsigned kRhythmCommonFields = 13u;
const unsigned kRhythmNoteFields = 59u;
const unsigned kRhythmKeys = 64u;

struct Part {
  uint8_t common[XP_JV1080_PATCH_COMMON_FIELDS];
  uint8_t tone[XP_JV1080_TONES_PER_PATCH][XP_JV1080_TONE_FIELDS];
  uint8_t part[kPartFields];
  /* The bank-select pair as received, resolved to a packed bank only when
     a program change arrives - which is the order the device resolves them
     in, since either may come first. */
  uint8_t bank_msb;
  uint8_t bank_lsb;
  /* MEASURED (`M-081`): CC7 indexes the same square law with a floor -
     values 0, 1 and 2 all give what the law gives for 1.15. Held per part
     because it is received per channel. */
  uint8_t volume;
};

struct Voice {
  struct XpJv1080Voice voice;
  int32_t *pcm;
  size_t capacity;
  uint64_t serial;
  uint8_t part;
  uint8_t key;
  /* Nonzero on a rhythm voice, and the group it belongs to: striking
     another key of the same group stops this one, which is what a hi-hat
     pair does. */
  uint8_t mute_group;
  /* This voice's share of each send bus, from its part at note-on. */
  float reverb_send;
  float chorus_send;
  bool allocated;
  bool key_down;
};

/* THE OUTPUT IS AC-COUPLED, AS EVERY ANALOGUE AUDIO OUTPUT IS.
 *
 *   The sample decoder integrates differences, so an element whose deltas do
 *   not sum to zero leaves a standing offset and a sustained note holds it.
 *   Measured on this device's own first factory song: without this the render
 *   carries 7.8 % of its energy below 20 Hz against the hardware take's
 *   0.1 %, and a per-100 ms offset six times the hardware's. One pole at
 *   10 Hz, which is 0.3 dB down by 40 Hz - inaudible, and it is what takes
 *   the offset out of the headroom.
 */
const double kDcBlockerHz = 10.0;

struct DcBlocker {
  double x1, y1, coefficient;
};

void dc_blocker_init(struct DcBlocker *dc, double rate)
{
  dc->x1 = 0.0;
  dc->y1 = 0.0;
  /* One-pole highpass, the usual difference form: the pole sits at
     exp(-2*pi*fc/fs), which at 10 Hz and any rate this renders at is a hair
     inside one. */
  dc->coefficient = std::exp(-2.0 * 3.14159265358979323846 *
                              kDcBlockerHz / rate);
}

float dc_blocker_step(struct DcBlocker *dc, double x)
{
  double y = x - dc->x1 + dc->coefficient * dc->y1;
  dc->x1 = x;
  dc->y1 = y;
  return (float)y;
}

struct Rhythm {
  uint8_t common[kRhythmCommonFields];
  uint8_t note[kRhythmKeys][kRhythmNoteFields];
};

struct Engine {
  struct xp_rom rom;
  const uint8_t *banks[XP_WAVE_BANK_COUNT];
  size_t bank_sizes[XP_WAVE_BANK_COUNT];
  double output_rate;
  unsigned max_voices;
  uint64_t serial;
  struct Part parts[kParts];
  struct Rhythm rhythm;
  struct Voice voices[kMaxVoices];
  struct DcBlocker dc_left;
  struct DcBlocker dc_right;
  /* The performance common block, which carries this device's reverb
     parameters, and the reverb itself. */
  uint8_t common[kPerfCommonFields];
  struct xp_reverb reverb;
  bool reverb_ready;
  uint8_t reverb_character;
  struct xp_chorus chorus;
  bool chorus_ready;
};

/* THE PART'S REVERB SEND IS THE LIVE ONE, not the tone's (`M-039`,
   `M-052`), so a voice carries its part's send from note-on. */
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
void reset_part(const struct xp_rom *rom, struct Part *part, unsigned index)
{
  const struct XpDeviceProfile *profile = xp_profile(rom);
  std::memset(part, 0, sizeof *part);
  part->common[profile->patchFieldLevel] = 127u;
  part->common[profile->patchFieldPan] = 64u;
  part->part[profile->partFieldLevel] = 127u;
  part->part[profile->partFieldPan] = 64u;
  /* Until a performance says otherwise a part receives on the channel of
     its own number, which is the state a caller with no performance to
     load - a bank select and a program change - needs. */
  part->part[profile->partFieldReceiveChannel] = (uint8_t)index;
  part->bank_msb = 0xffu;
  part->bank_lsb = 0xffu;
  part->volume = 127u;
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

/* THE JV-1080'S OWN REVERB PARAMETERS, against the shared reverb's fields.
   Each one is this device's, read from its own tables:

   TYPE picks the character record; this device has eight.

   LEVEL indexes the CRAM-destined level table, 128 monotonic words against
   8192 = unity. That is not the SC-88's linear 4*p: `M-045` measures tails
   of -99.8, -65.0 and -57.0 dBFS at levels 0, 64 and 127, and the table's
   own 0, 3424 and 8191 give 7.6 dB between the last two where a linear law
   gives 6.0.

   TIME reaches the tank's per-pass loop gain through a coefficient rather
   than the SC-88's register - `48 * v / 8192`, capped at 0.7441 - which
   `reverb.cc`'s own note on the decay register already records as this
   device's form of the same quantity, applied once per tank half.

   HF DAMP is `M-064`, exact to the unit: 18 rows of (a, 0x1FFF - a) where
   the one-pole is `y = a*x + (1-a)*y'` with its -3 dB corner at the row's
   own frequency, so the runtime's pole is 1 - a. Row 17 is BYPASS, and it
   only reads as one under the shift-field law - `0x5000` decodes to unity
   there and to 2.5 without it, which is a third witness for that half of
   `U-R5-01` on top of the 33 coefficient images.

   FEEDBACK is not plumbed: `M-045` measures only 3 dB across its whole
   range (-46.8, -46.2, -43.8 dBFS at 0, 64, 127) and what it does to the
   network is not recovered. */
void reverb_apply_params(struct Engine *engine)
{
  if (!engine->reverb_ready)
    return;
  const struct XpDeviceProfile *profile = xp_profile(&engine->rom);
  struct xp_reverb *rv = &engine->reverb;
  const uint8_t *common = engine->common;
  unsigned level = common[kReverbLevelField];
  unsigned time = common[kReverbTimeField];
  unsigned damp = common[kReverbDampField];

  if (profile->reverbLevelTable && level < 128u) {
    uint32_t a = profile->reverbLevelTable + 2u * level;
    if (a + 2u <= engine->rom.size)
      rv->level = (float)(((unsigned)engine->rom.bytes[a] << 8 |
                            engine->rom.bytes[a + 1u]) / 8192.0);
  }
  double g = 48.0 * (double)(time > 127u ? 127u : time) / 8192.0;
  if (g > 0.7441)
    g = 0.7441;
  if (g < 0.001)
    g = 0.001;
  unsigned samples[2] = { 0u, 0u };
  for (unsigned h = 0; h < 2; ++h)
    for (unsigned i = 0; i < XP_REVERB_HALF_BUFFERS; ++i) {
      unsigned b = XP_REVERB_HALF_BUFFERS * (h + 1u) + i;
      samples[h] += rv->far[b] - rv->head[b];
    }
  for (unsigned h = 0; h < 2; ++h)
    rv->decay[h] = (float)g;
  double seconds = rv->output_rate > 0.0
    ? (double)(samples[0] + samples[1]) / rv->output_rate : 0.0;
  rv->target_t60 = 3.0 * seconds / (2.0 * std::log10(1.0 / g));

  float pole = 0.0f;
  if (profile->reverbDampTable && damp < 18u) {
    uint32_t a = profile->reverbDampTable + 4u * damp;
    if (a + 2u <= engine->rom.size) {
      uint16_t raw = (uint16_t)((unsigned)engine->rom.bytes[a] << 8 |
                                 engine->rom.bytes[a + 1u]);
      /* The same coefficient law the rest of this engine reads CRAM with,
         credited where it is declared: a sign-extended 14-bit mantissa
         scaled by the two-bit field above it, against 8192 = unity. */
      int mantissa = raw & 0x3fff;
      if (mantissa & 0x2000)
        mantissa -= 0x4000;
      double coeff = (double)mantissa *
        (double)(1u << kXpCoefficientShift[raw >> 14]) / 8192.0;
      if (coeff > 0.0 && coeff < 1.0)
        pole = (float)(1.0 - coeff);
    }
  }
  rv->damp[0] = pole;
  rv->damp[1] = pole;
  /* The return path. This device has no per-character trim word - its
     return is the 9-bit level register the level table feeds - so the
     character returns unattenuated and `level` carries it. */
  rv->trim = (float)rv->character.return_trim * 16.0f / 512.0f;
  rv->wet_gain_left =
    rv->level * rv->trim / std::sqrt((float)XP_REVERB_TAPS);
  rv->wet_gain_right = rv->wet_gain_left;
}

/* A monotonic u16 table against 8192 = unity, which is how this device
   scales every level-like quantity. */
double table_unit(const struct Engine *engine, uint32_t base, unsigned v)
{
  if (!base || v > 127u)
    return 0.0;
  uint32_t a = base + 2u * v;
  if (a + 2u > engine->rom.size)
    return 0.0;
  return (double)(((unsigned)engine->rom.bytes[a] << 8) |
                   engine->rom.bytes[a + 1u]) / 8192.0;
}

/* THE JV-1080'S OWN CHORUS LAWS, each measured; the reasoning and the
   numbers are in jv1080.cc beside the tables they read. */
void chorus_refresh(struct Engine *engine)
{
  const struct XpDeviceProfile *profile = xp_profile(&engine->rom);
  if (!profile->chorusRateTable)
    return;
  if (!engine->chorus_ready) {
    engine->chorus_ready =
      chorus_init(&engine->chorus, engine->output_rate, profile);
    if (!engine->chorus_ready)
      return;
  }
  const uint8_t *common = engine->common;
  double scale = engine->output_rate / kXpNativeRate;
  /* the pre-delay IS the nominal delay, in samples at the wave rate */
  double delay = 0.0;
  {
    unsigned p = common[kChorusPreDelayField];
    if (p > 127u) p = 127u;
    uint32_t a = profile->chorusPreDelayTable + 2u * p;
    if (a + 2u <= engine->rom.size)
      delay = (double)(((unsigned)engine->rom.bytes[a] << 8) |
                        engine->rom.bytes[a + 1u]) * scale;
  }
  /* the sweep, peak to peak, from the depth table's shape */
  double top = table_unit(engine, profile->chorusDepthTable, 127u);
  double shape = top > 0.0
    ? table_unit(engine, profile->chorusDepthTable,
                  common[kChorusDepthField]) / top
    : 0.0;
  double depth = profile->chorusDepthMaxMs * shape *
    engine->output_rate / 1000.0;
  /* the modulation, as a phase step per output sample */
  double hz = 0.0;
  {
    unsigned p = common[kChorusRateField];
    if (p > 127u) p = 127u;
    uint32_t a = profile->chorusRateTable + 2u * p;
    if (a + 2u <= engine->rom.size && profile->chorusRateAccumulator > 0.0)
      hz = (double)(((unsigned)engine->rom.bytes[a] << 8) |
                     engine->rom.bytes[a + 1u]) /
        profile->chorusRateAccumulator / kXpControlPeriodSeconds;
  }
  float level = (float)table_unit(engine, profile->chorusLevelTable,
                                   common[kChorusLevelField]);
  chorus_set_runtime(&engine->chorus, delay, depth,
                      hz / engine->output_rate, 0.0f, level);
}

void reverb_refresh(struct Engine *engine)
{
  const struct XpDeviceProfile *profile = xp_profile(&engine->rom);
  if (!profile->reverbCharacters)
    return;
  unsigned type = engine->common[kReverbTypeField];
  if (type >= profile->reverbCharacters)
    type = 0u;
  if (!engine->reverb_ready || type != engine->reverb_character) {
    if (engine->reverb_ready)
      reverb_destroy(&engine->reverb);
    engine->reverb_ready = reverb_init(&engine->reverb, &engine->rom,
                                        (uint8_t)type, engine->output_rate);
    engine->reverb_character = (uint8_t)type;
  }
  reverb_apply_params(engine);
}

void part_controls(const struct Engine *engine, unsigned part,
                    struct XpJv1080PartControls *out)
{
  const struct XpDeviceProfile *profile = xp_profile(&engine->rom);
  const struct Part &p = engine->parts[part];
  out->patch_level = p.common[profile->patchFieldLevel];
  out->patch_pan = p.common[profile->patchFieldPan];
  out->part_level = p.part[profile->partFieldLevel];
  out->part_pan = p.part[profile->partFieldPan];
  out->volume = p.volume;
  out->key_shift = (int)(int8_t)p.part[profile->partFieldKeyShift];
  out->fine_tune = profile->partFieldFineTune == XP_VOICE_FIELD_NONE
    ? 0
    : (int)(int8_t)p.part[profile->partFieldFineTune];
  out->patch_octave = profile->patchFieldOctaveShift == XP_VOICE_FIELD_NONE
    ? 0
    : (int)(int8_t)p.common[profile->patchFieldOctaveShift];
}

bool start_record(struct Engine *engine, unsigned part,
                   const struct XpVoiceFieldMap *fields, const uint8_t *bytes,
                   unsigned key, unsigned velocity)
{
  size_t samples = 0;
  if (!jv1080_voice_span(&engine->rom, fields, bytes, key, velocity,
                         &samples) || !samples)
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
  struct XpJv1080PartControls controls;
  part_controls(engine, part, &controls);
  if (!jv1080_voice_start(&engine->rom, fields, bytes, &controls, key,
                          velocity, engine->banks, engine->bank_sizes, pcm,
                          samples, engine->output_rate, &voice->voice)) {
    std::free(pcm);
    return false;
  }
  voice->pcm = pcm;
  voice->capacity = samples;
  voice->serial = ++engine->serial;
  voice->part = (uint8_t)part;
  voice->reverb_send = 0.0f;
  voice->chorus_send = 0.0f;
  {
    const struct XpDeviceProfile *prof = xp_profile(&engine->rom);
    if (prof->partFieldReverbSend != XP_VOICE_FIELD_NONE)
      voice->reverb_send =
        (float)engine->parts[part].part[prof->partFieldReverbSend] / 127.0f;
    if (prof->partFieldChorusSend != XP_VOICE_FIELD_NONE)
      voice->chorus_send =
        (float)engine->parts[part].part[prof->partFieldChorusSend] / 127.0f;
  }
  voice->key = (uint8_t)key;
  voice->mute_group = fields->muteGroup == XP_VOICE_FIELD_NONE
    ? 0u : bytes[fields->muteGroup];
  voice->allocated = true;
  voice->key_down = true;
  return true;
}

/* THE PART IS NOT THE CHANNEL. Every part carries its own receive channel,
   and more than one may carry the same one - which is how two patches
   layer, and this device's own first factory song does exactly that: part
   12 and part 16 both listen on channel 16 and nothing listens on channel
   12. Calls `visit` for each part receiving on `channel` and returns how
   many there were. */
template <typename F>
unsigned for_each_part_on(struct Engine *engine, unsigned channel, F visit)
{
  const struct XpDeviceProfile *profile = xp_profile(&engine->rom);
  unsigned found = 0;
  for (unsigned p = 0; p < kParts; ++p) {
    if (engine->parts[p].part[profile->partFieldReceiveChannel] != channel)
      continue;
    visit(p);
    ++found;
  }
  return found;
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
  dc_blocker_init(&engine->dc_left, outputRate);
  dc_blocker_init(&engine->dc_right, outputRate);
  engine->max_voices = xp_profile(rom)->defaultMaxVoices;
  if (!engine->max_voices || engine->max_voices > kMaxVoices)
    engine->max_voices = kMaxVoices;
  for (unsigned p = 0; p < kParts; ++p)
    reset_part(rom, engine->parts + p, p);
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
  if (engine->reverb_ready)
    reverb_destroy(&engine->reverb);
  if (engine->chorus_ready)
    chorus_destroy(&engine->chorus);
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
    reset_part(&engine->rom, engine->parts + p, p);
  std::memset(&engine->rhythm, 0, sizeof engine->rhythm);
  /* A reset silences the effects too, or a tail outlives the notes that
     made it. The settings are kept: they belong to the performance
     common, which a reset does not rewrite. */
  if (engine->reverb_ready)
    reverb_reset(&engine->reverb);
  if (engine->chorus_ready)
    chorus_reset(&engine->chorus);
  dc_blocker_init(&engine->dc_left, engine->output_rate);
  dc_blocker_init(&engine->dc_right, engine->output_rate);
  engine->serial = 0;
}

bool engine_note_on_jv(void *state, unsigned channel, unsigned key,
                        unsigned velocity)
{
  struct Engine *engine = (struct Engine *)state;
  if (!engine || channel >= kParts || key > 127u || velocity == 0u ||
      velocity > 127u)
    return false;
  /* The four tones of a patch start together and in phase, which is
     measured: one to four tones sum coherently to within 0.02 dB of
     `20*log10(N)` (`M-037`). */
  const struct XpDeviceProfile *profile = xp_profile(&engine->rom);
  unsigned started = 0;
  for_each_part_on(engine, channel, [&](unsigned part) {
    if (part == profile->rhythmPartIndex) {
      /* The rhythm part plays one record per key, not a four-tone patch,
         and the key selects the record rather than the pitch. */
      if (key < profile->rhythmFirstKey ||
          key >= (unsigned)profile->rhythmFirstKey + kRhythmKeys)
        return;
      unsigned index = key - profile->rhythmFirstKey;
      const uint8_t *note = engine->rhythm.note[index];
      unsigned group = note[profile->rhythmNoteFields.muteGroup];
      /* A mute group stops every other rhythm voice in it - one hi-hat
         closing the other. Group zero is "no group" and mutes nothing. */
      if (group) {
        for (unsigned i = 0; i < kMaxVoices; ++i) {
          struct Voice *other = engine->voices + i;
          if (other->allocated && other->mute_group == group)
            jv1080_voice_release(&other->voice);
        }
      }
      started += start_record(engine, part, &profile->rhythmNoteFields, note,
                               key, velocity) ? 1u : 0u;
      return;
    }
    for (unsigned t = 0; t < XP_JV1080_TONES_PER_PATCH; ++t)
      started += start_record(engine, part, &profile->toneFields,
                               engine->parts[part].tone[t], key,
                               velocity) ? 1u : 0u;
  });
  return started != 0u;
}

bool engine_note_off_jv(void *state, unsigned channel, unsigned key)
{
  struct Engine *engine = (struct Engine *)state;
  if (!engine || channel >= kParts || key > 127u)
    return false;
  unsigned released = 0;
  for_each_part_on(engine, channel, [&](unsigned part) {
    for (unsigned i = 0; i < kMaxVoices; ++i) {
      struct Voice *voice = engine->voices + i;
      if (!voice->allocated || !voice->key_down || voice->part != part ||
          voice->key != key)
        continue;
      voice->key_down = false;
      jv1080_voice_release(&voice->voice);
      ++released;
    }
  });
  return released != 0u;
}

/* The controllers this device acts on. CC7 is measured (`M-081`); the bank
   pair is held until a program change resolves it, since either may arrive
   first. CC11 expression is received and ignored on purpose: it is measured
   INERT on this machine, under 0.1 dB across seventeen values (`M-048`). */
bool engine_control_change(void *state, unsigned channel, unsigned controller,
                            unsigned value)
{
  struct Engine *engine = (struct Engine *)state;
  if (!engine || channel >= kParts || value > 127u)
    return false;
  return for_each_part_on(engine, channel, [&](unsigned part) {
    switch (controller) {
    case 0: engine->parts[part].bank_msb = (uint8_t)value; break;
    case 32: engine->parts[part].bank_lsb = (uint8_t)value; break;
    case 7: engine->parts[part].volume = (uint8_t)value; break;
    default: break;
    }
  }) != 0u;
}

bool engine_program_change_one(struct Engine *engine, unsigned part,
                                unsigned program)
{
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

bool engine_program_change(void *state, unsigned channel, unsigned program)
{
  struct Engine *engine = (struct Engine *)state;
  if (!engine || channel >= kParts || program > 127u)
    return false;
  unsigned loaded = 0;
  for_each_part_on(engine, channel, [&](unsigned part) {
    loaded += engine_program_change_one(engine, part, program) ? 1u : 0u;
  });
  return loaded != 0u;
}

/* One DT1 payload. This device's parameter address is four seven-bit
   bytes - group, part, block, offset - and a write may start at any
   parameter of a block and run past its end, which is the ordinary
   editing case: the front panel and every editor address one parameter at
   a time. `device_spec.md` S10 records that a DT1 may run past a block
   boundary and that the machine relies on it - the tone record is 130
   parameters where a block addresses 128, so its last two live in the
   next block along. */
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
  const struct XpDeviceProfile *profile = xp_profile(&engine->rom);

  unsigned a1 = address[0];
  unsigned part = address[1];
  unsigned block = address[2];
  unsigned within = address[3];

  /* Only the temporary patch areas are acted on. The device addresses them
     two ways - `02 pp bb xx` for performance-mode part pp, `03 00 bb xx`
     for patch mode - and the second is the first with part zero. */
  /* The temporary performance's own part blocks, `01 00 1n xx`: where each
     part's receive channel, level, pan and key shift come from. */
  if (a1 == 0x01u && part == 0x00u && (block & 0xf0u) == 0x10u) {
    unsigned index = block & 0x0fu;
    if (index >= kParts || within >= kPartFields)
      return false;
    packed_apply_wire_block(&engine->rom,
                             xp_profile(&engine->rom)
                               ->packedPerformancePartGroup,
                             within, data, count, engine->parts[index].part,
                             kPartFields);
    return true;
  }
  /* The temporary performance's common block, `01 00 00 xx`: this device
     puts its reverb parameters there rather than in the patch. */
  if (a1 == 0x01u && part == 0x00u && block == 0x00u) {
    if (within >= kPerfCommonFields)
      return false;
    packed_apply_wire_block(&engine->rom,
                             profile->packedPerformanceCommonGroup,
                             within, data, count, engine->common,
                             kPerfCommonFields);
    reverb_refresh(engine);
    chorus_refresh(engine);
    return true;
  }
  if (a1 == 0x03u)
    part = 0u;
  else if (a1 != 0x02u)
    return false;                /* system or performance common: unheld */

  /* The rhythm set has its own part index: `02 09 00 xx` is its common
     block and `02 09 kk xx` its record for key kk. */
  if (a1 == 0x02u && part == profile->rhythmPartIndex) {
    if (!block) {
      if (within >= kRhythmCommonFields)
        return false;
      packed_apply_wire_block(&engine->rom, profile->packedRhythmCommonGroup,
                               within, data, count, engine->rhythm.common,
                               kRhythmCommonFields);
      return true;
    }
    if (block < profile->rhythmFirstKey ||
        block >= (unsigned)profile->rhythmFirstKey + kRhythmKeys ||
        within >= kRhythmNoteFields)
      return false;
    packed_apply_wire_block(&engine->rom, profile->packedRhythmNoteGroup,
                             within, data, count,
                             engine->rhythm.note[block -
                                                 profile->rhythmFirstKey],
                             kRhythmNoteFields);
    return true;
  }
  if (part >= kParts)
    return false;

  if (block == kBlockPatchCommon) {
    if (within >= XP_JV1080_PATCH_COMMON_FIELDS)
      return false;
    packed_apply_wire_block(&engine->rom, commonGroup, within, data, count,
                             engine->parts[part].common,
                             XP_JV1080_PATCH_COMMON_FIELDS);
    return true;
  }
  /* A tone's 130 parameters span two blocks, so the block index carries
     both which tone and which page of it: the low bit of the offset from
     the first tone block is the page, and the rest is the tone. */
  if (block >= kBlockFirstTone &&
      block < kBlockFirstTone +
                kBlockToneStride * XP_JV1080_TONES_PER_PATCH) {
    unsigned relative = block - kBlockFirstTone;
    unsigned tone = relative / kBlockToneStride;
    unsigned page = relative % kBlockToneStride;
    unsigned offset = page * kBlockParameters + within;
    if (offset >= XP_JV1080_TONE_FIELDS)
      return false;
    packed_apply_wire_block(&engine->rom, toneGroup, offset, data, count,
                             engine->parts[part].tone[tone],
                             XP_JV1080_TONE_FIELDS);
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
     block-sized scratch pair rather than a per-sample transpose.

     A voice that sends to the reverb is rendered into its own scratch pair
     first, because the send bus needs that voice on its own: its share of
     the bus is its PART's send, which is the live one on this device and
     the tone's is inert (`M-039`, `M-052`). A voice with no send is summed
     straight into the mix as before. */
  static const size_t kChunk = 512u;
  float left[kChunk];
  float right[kChunk];
  float vl[kChunk];
  float vr[kChunk];
  float send[kChunk];
  float csend[kChunk];
  float cwet[kChunk * 2u];
  size_t done = 0;
  while (done < frames) {
    size_t n = frames - done;
    if (n > kChunk)
      n = kChunk;
    std::memset(left, 0, n * sizeof *left);
    std::memset(right, 0, n * sizeof *right);
    std::memset(send, 0, n * sizeof *send);
    std::memset(csend, 0, n * sizeof *csend);
    bool sending = false;
    for (unsigned i = 0; i < kMaxVoices; ++i) {
      struct Voice *voice = engine->voices + i;
      if (!voice->allocated)
        continue;
      if ((engine->reverb_ready && voice->reverb_send > 0.0f) ||
          (engine->chorus_ready && voice->chorus_send > 0.0f)) {
        std::memset(vl, 0, n * sizeof *vl);
        std::memset(vr, 0, n * sizeof *vr);
        if (!jv1080_voice_render(&voice->voice, vl, vr, n)) {
          free_voice(voice);
        }
        for (size_t k = 0; k < n; ++k) {
          float mono = 0.5f * (vl[k] + vr[k]);
          left[k] += vl[k];
          right[k] += vr[k];
          send[k] += voice->reverb_send * mono;
          csend[k] += voice->chorus_send * mono;
        }
        sending = true;
        continue;
      }
      if (!jv1080_voice_render(&voice->voice, left, right, n))
        free_voice(voice);
    }
    for (size_t k = 0; k < n; ++k) {
      stereo[(done + k) * 2u] +=
        dc_blocker_step(&engine->dc_left, left[k]);
      stereo[(done + k) * 2u + 1u] +=
        dc_blocker_step(&engine->dc_right, right[k]);
    }
    /* The chorus's wet signal is formed once and then returned through up
       to two paths, because that is what the firmware does: the level
       lands in the dry-mix return register `XP 0x333E`, the reverb-send
       register `XP 0x333C`, or both, and the output assign is what picks
       (`08_effects/chorus.md`, `FW-EXACT`). Routing it into the reverb is
       the only way a chorused note leaves a tail - `M-044` measured MIX at
       -99.7 dBFS after the note against -60.7 for REVERB and MIX+REV. */
    if (engine->chorus_ready) {
      unsigned assign = engine->common[kChorusOutputField];
      std::memset(cwet, 0, n * 2u * sizeof *cwet);
      chorus_process(&engine->chorus, csend, cwet, n);
      if (assign != kChorusOutReverb)
        for (size_t k = 0; k < n * 2u; ++k)
          stereo[done * 2u + k] += cwet[k];
      if (assign != kChorusOutMix) {
        for (size_t k = 0; k < n; ++k)
          send[k] += 0.5f * (cwet[k * 2u] + cwet[k * 2u + 1u]);
        sending = true;
      }
    }
    /* The reverb adds its stereo return to the dry mix already in place.
       It is stepped whenever it holds a tail, not only while something is
       sending, or a note's reverb would stop with the note. */
    if (engine->reverb_ready && (sending || engine->reverb.active))
      reverb_process(&engine->reverb, send, stereo + done * 2u, n);
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
  EmuSC::Xp::engine_control_change,
  EmuSC::Xp::engine_program_change,
  EmuSC::Xp::engine_sysex_block,
  EmuSC::Xp::engine_render_jv,
  EmuSC::Xp::engine_set_max_voices_jv,
  EmuSC::Xp::engine_active_voices,
};

}  // extern "C"
