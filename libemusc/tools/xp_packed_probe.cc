/* SPDX-License-Identifier: CC0-1.0 */
/* Probe for an XP-family control ROM whose preset records are
 * descriptor-packed.
 *
 * Three things need looking at directly rather than through a render, and
 * each is a mode here:
 *
 *   descramble  - write a wave chip's decoded image, so it can be compared
 *                 byte for byte against an independent reference decoder.
 *                 A descramble that is subtly wrong still produces
 *                 something that looks like audio.
 *   patch       - resolve a factory patch through the packed record
 *                 decoder and the wave chain, print what each of its tones
 *                 reached, and render one note. The chain has five links
 *                 and a fault in any of them lands on a real but wrong
 *                 sample, so the names it prints are the check.
 *   song        - play a MIDI file whose own SysEx programs the machine,
 *                 which is what the device's factory demo songs do. This
 *                 exercises the same tone fields arriving over the wire
 *                 rather than out of a packed record.
 *
 * The library's own descramble, FCE decoder, record reader and voice model
 * are used rather than a reimplementation, so what lands on disk is what
 * the engine would play.
 */
#include "engines/xp/devices/jv1080.h"
#include "engines/xp/packed_rom.h"
#include "engines/xp/rom.h"
#include "engines/xp/wave.h"

#include "smf.h"
#include "wav.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace EmuSC::Xp;

namespace {

std::vector<uint8_t> read_file(const char *path)
{
  std::FILE *f = std::fopen(path, "rb");
  if (!f) {
    std::fprintf(stderr, "cannot open %s\n", path);
    std::exit(1);
  }
  std::fseek(f, 0, SEEK_END);
  long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> bytes((size_t)size);
  if (size > 0 && std::fread(bytes.data(), 1, (size_t)size, f) != (size_t)size) {
    std::fprintf(stderr, "short read on %s\n", path);
    std::exit(1);
  }
  std::fclose(f);
  return bytes;
}

struct Chips {
  std::vector<std::vector<uint8_t>> decoded;
  const uint8_t *banks[XP_WAVE_BANK_COUNT];
  size_t bankSizes[XP_WAVE_BANK_COUNT];
};

void descramble_all(const struct xp_rom *rom, char **paths, unsigned count,
                     Chips *chips)
{
  const struct XpDeviceProfile *profile = xp_profile(rom);
  if (count != XP_WAVE_CHIP_COUNT) {
    std::fprintf(stderr, "need %u wave ROM images, got %u\n",
                 XP_WAVE_CHIP_COUNT, count);
    std::exit(1);
  }
  chips->decoded.resize(XP_WAVE_CHIP_COUNT);
  for (unsigned c = 0; c < XP_WAVE_CHIP_COUNT; ++c) {
    std::vector<uint8_t> raw = read_file(paths[c]);
    chips->decoded[c].assign(profile->waveChipSize, 0);
    if (!wave_descramble_chip(profile, raw.data(), raw.size(),
                              chips->decoded[c].data(),
                              chips->decoded[c].size())) {
      std::fprintf(stderr, "descramble refused %s\n", paths[c]);
      std::exit(1);
    }
    unsigned perChip = (unsigned)(profile->waveChipSize / profile->waveBankSize);
    for (unsigned b = 0; b < perChip; ++b) {
      chips->banks[c * perChip + b] =
        chips->decoded[c].data() + (size_t)b * profile->waveBankSize;
      chips->bankSizes[c * perChip + b] = profile->waveBankSize;
    }
  }
}

void usage(void)
{
  std::fprintf(stderr,
    "usage:\n"
    "  xp_packed_probe descramble <control.bin> <w1> <w2> <w3> <w4> "
    "<chip 0-3> <out.bin>\n"
    "  xp_packed_probe element <control.bin> <w1> <w2> <w3> <w4> "
    "<directory> <index> <out.s32>\n"
    "  xp_packed_probe fields <control.bin> <w1> <w2> <w3> <w4> "
    "<bank> <out.bin>\n"
    "  xp_packed_probe patch <control.bin> <w1> <w2> <w3> <w4> "
    "<bank> <program> <key> <velocity> <seconds> <out.wav>\n"
    "  xp_packed_probe song <control.bin> <w1> <w2> <w3> <w4> "
    "<song.mid> <seconds> <out.wav>\n");
  std::exit(1);
}

const double kRate = 44100.0;

struct Playing {
  struct XpJv1080Voice voice;
  std::vector<int32_t> pcm;
  int part;
  int key;
  bool used;
};

}  // namespace

int main(int argc, char **argv)
{
  if (argc < 7)
    usage();
  const std::string mode = argv[1];
  std::vector<uint8_t> control = read_file(argv[2]);

  struct xp_rom rom;
  if (!rom_init(&rom, control.data(), control.size())) {
    std::fprintf(stderr, "no profile matches %s (%zu bytes)\n", argv[2],
                 control.size());
    return 1;
  }
  const struct XpDeviceProfile *profile = xp_profile(&rom);
  std::printf("control ROM identified: %zu bytes, %u packed descriptors, "
              "%u groups, %u banks\n",
              profile->romSize, profile->packedDescriptorCount,
              profile->packedGroupCount, profile->packedBankCount);
  if (!profile->packedDescriptorBase) {
    std::fprintf(stderr, "this device's records are not descriptor-packed\n");
    return 1;
  }

  Chips chips;
  descramble_all(&rom, argv + 3, 4, &chips);

  if (mode == "descramble") {
    if (argc < 9)
      usage();
    unsigned chip = (unsigned)std::atoi(argv[7]);
    if (chip >= XP_WAVE_CHIP_COUNT)
      usage();
    std::FILE *out = std::fopen(argv[8], "wb");
    if (!out) {
      std::fprintf(stderr, "cannot create %s\n", argv[8]);
      return 1;
    }
    std::fwrite(chips.decoded[chip].data(), 1, chips.decoded[chip].size(), out);
    std::fclose(out);
    std::printf("wrote chip %u decoded image, %zu bytes\n", chip,
                chips.decoded[chip].size());
    return 0;
  }

  if (mode == "element") {
    if (argc < 10)
      usage();
    unsigned directory = (unsigned)std::atoi(argv[7]);
    unsigned index = (unsigned)std::atoi(argv[8]);
    struct xp_wave_element element;
    if (!wave_element_open(&rom, directory, index, &element)) {
      std::fprintf(stderr, "directory %u element %u does not open\n",
                   directory, index);
      return 1;
    }
    std::printf("element %u/%u at 0x%06x: %06x..%06x loop %06x root %u "
                "mode %u%s chip W%u bank %u\n", directory, index,
                element.offset, element.start, element.end, element.loop,
                element.root_key, (unsigned)element.mode,
                element.reverse ? " REVERSE" : "", element.chip + 1,
                element.bank);
    uint32_t base = element.bank_start & ~UINT32_C(0x0f);
    size_t count = (size_t)(element.bank_end - base) + 1u;
    std::vector<int32_t> pcm(count, 0);
    struct xp_fce_decoder decoder;
    if (!fce_decoder_reset(profile, &decoder, element.bank_start)) {
      std::fprintf(stderr, "decoder refused this element\n");
      return 1;
    }
    for (size_t i = 0; i < count; ++i)
      if (!fce_decoder_read(&decoder, chips.banks[element.bank],
                            chips.bankSizes[element.bank], pcm.data() + i)) {
        std::fprintf(stderr, "decode stopped at sample %zu\n", i);
        return 1;
      }
    int32_t peak = 0;
    double sum = 0.0;
    double mean = 0.0;
    for (size_t i = 0; i < count; ++i) {
      int32_t a = pcm[i] < 0 ? -pcm[i] : pcm[i];
      if (a > peak)
        peak = a;
      sum += (double)pcm[i] * pcm[i];
      mean += pcm[i];
    }
    std::printf("%zu samples, peak %d (%.4f FS), rms %.4f FS, dc %.5f FS\n",
                count, peak, (double)peak / 8388608.0,
                std::sqrt(sum / (double)count) / 8388608.0,
                mean / (double)count / 8388608.0);
    std::FILE *out = std::fopen(argv[9], "wb");
    if (!out) {
      std::fprintf(stderr, "cannot create %s\n", argv[9]);
      return 1;
    }
    std::fwrite(pcm.data(), sizeof(int32_t), count, out);
    std::fclose(out);
    return 0;
  }

  /* Everything below plays notes, so it needs to know which groups a patch
     record is made of. The banks say: a melodic bank's own common and part
     groups are the patch common and the tone. */
  if (!profile->packedMelodicBankCount) {
    std::fprintf(stderr, "this device has no melodic packed banks\n");
    return 1;
  }
  const struct XpPackedBank &melodic =
    profile->packedBanks[profile->packedMelodicBanks[0]];
  const unsigned patchCommonGroup = melodic.commonGroup;
  const unsigned toneGroup = melodic.partGroup;

  /* Every decoded field of a whole bank, in record order, so the decoder can
     be diffed field by field against an independent extractor. A bit-field
     reader that is subtly wrong still returns values inside their declared
     ranges. */
  if (mode == "fields") {
    if (argc < 9)
      usage();
    unsigned bank = (unsigned)std::atoi(argv[7]);
    if (bank >= profile->packedBankCount)
      usage();
    const struct XpPackedBank &b = profile->packedBanks[bank];
    const struct XpPackedGroup &cg = profile->packedGroups[b.commonGroup];
    const struct XpPackedGroup &pg = profile->packedGroups[b.partGroup];
    std::FILE *out = std::fopen(argv[8], "wb");
    if (!out) {
      std::fprintf(stderr, "cannot create %s\n", argv[8]);
      return 1;
    }
    unsigned records = 0;
    for (unsigned n = 0; n < b.count; ++n) {
      struct xp_packed_record record;
      if (!packed_open(&rom, bank, n, &record)) {
        std::fprintf(stderr, "record %u does not open\n", n);
        return 1;
      }
      for (unsigned f = 0; f < cg.fieldCount; ++f) {
        int value = 0;
        if (!packed_common_field(&rom, &record, f, &value)) {
          std::fprintf(stderr, "record %u common field %u\n", n, f);
          return 1;
        }
        uint8_t byte = (uint8_t)(value & 0xff);
        std::fwrite(&byte, 1, 1, out);
      }
      for (unsigned t = 0; t < record.part_count; ++t)
        for (unsigned f = 0; f < pg.fieldCount; ++f) {
          int value = 0;
          if (!packed_part_field(&rom, &record, t, f, &value)) {
            std::fprintf(stderr, "record %u part %u field %u\n", n, t, f);
            return 1;
          }
          uint8_t byte = (uint8_t)(value & 0xff);
          std::fwrite(&byte, 1, 1, out);
        }
      ++records;
    }
    std::fclose(out);
    std::printf("bank %u: %u records, %u common + %u x %u part fields each\n",
                bank, records, cg.fieldCount, b.partCount, pg.fieldCount);
    return 0;
  }

  if (mode == "patch") {
    if (argc < 13)
      usage();
    unsigned bank = (unsigned)std::atoi(argv[7]);
    unsigned program = (unsigned)std::atoi(argv[8]);
    unsigned key = (unsigned)std::atoi(argv[9]);
    unsigned velocity = (unsigned)std::atoi(argv[10]);
    double seconds = std::atof(argv[11]);
    const char *outPath = argv[12];

    struct xp_packed_record patch;
    if (!packed_open(&rom, bank, program, &patch)) {
      std::fprintf(stderr, "bank %u program %u does not open\n", bank, program);
      return 1;
    }
    char name[16];
    if (packed_common_name(&rom, &patch, profile->patchFieldName,
                            profile->patchFieldNameLength, name))
      std::printf("bank %u program %u at 0x%06x: \"%s\"\n", bank, program,
                  patch.offset, name);

    std::vector<Playing> voices(patch.part_count);
    size_t sounding = 0;
    for (unsigned t = 0; t < patch.part_count; ++t) {
      uint8_t tone[XP_JV1080_TONE_FIELDS];
      unsigned patchLevel = 0;
      unsigned patchPan = 64;
      if (!jv1080_patch_tone(&rom, &patch, t, tone, &patchLevel, &patchPan)) {
        std::fprintf(stderr, "tone %u does not decode\n", t + 1);
        return 1;
      }
      /* Print the whole chain, because a fault anywhere in it lands on a
         real but wrong sample rather than on nothing. */
      unsigned source = 0;
      uint8_t msBank = 0;
      uint16_t msRow = 0;
      char wave[16] = "";
      struct xp_wave_zone zone;
      struct xp_wave_element element;
      bool resolved =
        wave_source_select(&rom, tone[profile->toneFields.waveGroup],
                            tone[profile->toneFields.waveGroupId], &source) &&
        wave_number_resolve(&rom, source, tone[profile->toneFields.waveNumber],
                             &msBank, &msRow) &&
        multisample_name(&rom, msBank, msRow, wave, sizeof wave) &&
        multisample_select(&rom, msBank, msRow, key, &zone) &&
        wave_element_open(&rom, zone.directory, zone.element, &element);
      std::printf("  tone %u: switch %u level %3u pan %3u cutoff %3u res %3u "
                  "type %u coarse %+d fine %+d\n",
                  t + 1, tone[profile->toneFields.enable],
                  tone[profile->toneFields.level], tone[profile->toneFields.pan],
                  tone[profile->toneFields.cutoff],
                  tone[profile->toneFields.resonance],
                  tone[profile->toneFields.filterType],
                  (int)(int8_t)tone[profile->toneFields.coarseTune],
                  (int)(int8_t)tone[profile->toneFields.fineTune]);
      if (resolved)
        std::printf("          wave group %u id %u number %3u -> "
                    "bank %u row %3u \"%s\" zone %u (<= key %3u) element %4u "
                    "-> chip W%u bank %u %06x..%06x loop %06x root %3u "
                    "mode %u%s\n",
                    tone[profile->toneFields.waveGroup],
                    tone[profile->toneFields.waveGroupId],
                    tone[profile->toneFields.waveNumber], msBank, msRow, wave,
                    zone.zone, zone.boundary, zone.element, element.chip + 1,
                    element.bank, element.start, element.end, element.loop,
                    element.root_key, (unsigned)element.mode,
                    element.reverse ? " REVERSE" : "");
      else
        std::printf("          wave does not resolve for this key\n");

      struct XpJv1080PartControls controls = {};
      controls.patch_level = patchLevel;
      controls.patch_pan = patchPan;
      controls.part_level = 127u;
      controls.part_pan = 64u;
      controls.volume = 127u;
      controls.key_shift = 0;
      voices[t].pcm.assign(1u << 21, 0);
      voices[t].used = jv1080_voice_start(&rom, &profile->toneFields, tone, &controls,
                                           key, velocity, chips.banks,
                                           chips.bankSizes,
                                           voices[t].pcm.data(),
                                           voices[t].pcm.size(), kRate,
                                           &voices[t].voice);
      if (voices[t].used)
        ++sounding;
    }
    std::printf("  %zu of %u tones sound at key %u velocity %u\n", sounding,
                patch.part_count, key, velocity);
    if (!sounding)
      return 1;

    size_t frames = (size_t)(seconds * kRate);
    size_t release = frames / 2u;
    std::vector<float> l(frames, 0.0f);
    std::vector<float> r(frames, 0.0f);
    for (auto &v : voices) {
      if (!v.used)
        continue;
      jv1080_voice_render(&v.voice, l.data(), r.data(), release);
      jv1080_voice_release(&v.voice);
      jv1080_voice_render(&v.voice, l.data() + release, r.data() + release,
                           frames - release);
    }

    double peak = 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < frames; ++i) {
      double m = std::fabs(l[i]) > std::fabs(r[i]) ? std::fabs(l[i])
                                                    : std::fabs(r[i]);
      if (m > peak)
        peak = m;
      sum += (double)l[i] * l[i] + (double)r[i] * r[i];
    }
    std::printf("  peak %.5f (%.2f dBFS), rms %.5f\n", peak,
                20.0 * std::log10(peak > 0.0 ? peak : 1e-12),
                std::sqrt(sum / (double)(frames * 2u)));

    std::vector<float> interleaved(frames * 2u);
    for (size_t i = 0; i < frames; ++i) {
      interleaved[i * 2u] = l[i];
      interleaved[i * 2u + 1u] = r[i];
    }
    WavWriter wav(outPath, (uint32_t)kRate, 2, true);
    wav.write(interleaved.data(), frames);
    return 0;
  }

  if (mode == "song") {
    if (argc < 10)
      usage();
    const char *songPath = argv[7];
    double seconds = std::atof(argv[8]);
    const char *outPath = argv[9];

    std::vector<uint8_t> songBytes = read_file(songPath);
    smf::File song = smf::parse(songBytes);
    std::printf("song: %zu events, %u tracks, ppqn %u\n", song.events.size(),
                (unsigned)song.ntracks, (unsigned)song.ppqn);

    /* Sixteen parts, each a patch image the song's own SysEx fills in. */
    const unsigned kParts = 16u;
    std::vector<std::vector<uint8_t>> tone(
      kParts * XP_JV1080_TONES_PER_PATCH,
      std::vector<uint8_t>(XP_JV1080_TONE_FIELDS, 0));
    std::vector<std::vector<uint8_t>> common(
      kParts, std::vector<uint8_t>(XP_JV1080_PATCH_COMMON_FIELDS, 0));
    for (auto &c : common) {
      c[profile->patchFieldLevel] = 127u;
      c[profile->patchFieldPan] = 64u;
    }

    size_t frames = (size_t)(seconds * kRate);
    std::vector<float> l(frames, 0.0f);
    std::vector<float> r(frames, 0.0f);

    /* One voice per note-on, held until its note-off or the end. A pool
       rather than the device's own 64-voice allocator: stealing is measured
       (`M-031`, `M-072`) and is not what this probe is checking. */
    std::vector<Playing> pool;
    pool.reserve(512);
    size_t cursor = 0;            /* frames already rendered */
    unsigned frames_written = 0;
    unsigned applied = 0;
    unsigned started = 0;
    unsigned refused = 0;

    auto advance = [&](size_t target) {
      if (target > frames)
        target = frames;
      if (target <= cursor)
        return;
      size_t n = target - cursor;
      for (auto &p : pool) {
        if (!p.used)
          continue;
        if (!jv1080_voice_render(&p.voice, l.data() + cursor,
                                 r.data() + cursor, n))
          p.used = false;
      }
      cursor = target;
      frames_written = (unsigned)cursor;
    };

    for (const smf::Event &e : song.events) {
      double at = (double)e.time_num / (double)song.time_den;
      if (at > seconds)
        break;
      advance((size_t)(at * kRate));

      if (e.kind == smf::Kind::SysEx) {
        /* F0 41 dd 6A 12 a1 a2 a3 a4 <data> sum F7 - the device's own
           model id and the DT1 command. */
        const std::vector<uint8_t> &s = e.bytes;
        if (s.size() < 12 || s[0] != 0xf0 || s[1] != 0x41 || s[3] != 0x6a ||
            s[4] != 0x12)
          continue;
        unsigned a1 = s[5];
        unsigned part = s[6];
        unsigned block = s[7];
        const uint8_t *payload = s.data() + 9;
        size_t count = s.size() - 11u;   /* strip the checksum and F7 */
        if (a1 != 0x02u || part >= kParts)
          continue;                      /* not a temporary patch write */
        if (!block) {
          packed_apply_wire_block(&rom, patchCommonGroup, payload, count,
                                  common[part].data(), common[part].size());
          ++applied;
        } else if ((block & 0x10u) && !(block & 0x01u) && block <= 0x16u) {
          unsigned index = (block - 0x10u) / 2u;
          if (index < XP_JV1080_TONES_PER_PATCH) {
            packed_apply_wire_block(
              &rom, toneGroup, payload, count,
              tone[part * XP_JV1080_TONES_PER_PATCH + index].data(),
              XP_JV1080_TONE_FIELDS);
            ++applied;
          }
        }
        continue;
      }

      if (e.kind != smf::Kind::Channel || e.bytes.empty())
        continue;
      unsigned status = e.bytes[0];
      unsigned channel = status & 0x0fu;
      unsigned kind = status & 0xf0u;
      if (kind == 0x90u && e.bytes.size() >= 3 && e.bytes[2]) {
        unsigned key = e.bytes[1];
        unsigned velocity = e.bytes[2];
        for (unsigned t = 0; t < XP_JV1080_TONES_PER_PATCH; ++t) {
          Playing p;
          p.part = (int)channel;
          p.key = (int)key;
          p.pcm.assign(1u << 19, 0);
          struct XpJv1080PartControls controls = {};
          controls.patch_level = common[channel][profile->patchFieldLevel];
          controls.patch_pan = common[channel][profile->patchFieldPan];
          controls.part_level = 127u;
          controls.part_pan = 64u;
          controls.volume = 127u;
          controls.key_shift = 0;
          p.used = jv1080_voice_start(
            &rom, &profile->toneFields,
            tone[channel * XP_JV1080_TONES_PER_PATCH + t].data(),
            &controls, key, velocity,
            chips.banks, chips.bankSizes, p.pcm.data(), p.pcm.size(), kRate,
            &p.voice);
          if (p.used) {
            pool.push_back(std::move(p));
            ++started;
          } else {
            ++refused;
          }
        }
      } else if (kind == 0x80u ||
                 (kind == 0x90u && e.bytes.size() >= 3 && !e.bytes[2])) {
        unsigned key = e.bytes[1];
        for (auto &p : pool)
          if (p.used && p.part == (int)channel && p.key == (int)key)
            jv1080_voice_release(&p.voice);
      }
    }
    advance(frames);

    double peak = 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < frames; ++i) {
      double m = std::fabs(l[i]) > std::fabs(r[i]) ? std::fabs(l[i])
                                                    : std::fabs(r[i]);
      if (m > peak)
        peak = m;
      sum += (double)l[i] * l[i] + (double)r[i] * r[i];
    }
    std::printf("applied %u DT1 frames, started %u voices, %u tone slots "
                "silent, rendered %u frames\n", applied, started, refused,
                frames_written);
    std::printf("peak %.5f (%.2f dBFS), rms %.5f\n", peak,
                20.0 * std::log10(peak > 0.0 ? peak : 1e-12),
                std::sqrt(sum / (double)(frames * 2u)));

    std::vector<float> interleaved(frames * 2u);
    for (size_t i = 0; i < frames; ++i) {
      interleaved[i * 2u] = l[i];
      interleaved[i * 2u + 1u] = r[i];
    }
    WavWriter wav(outPath, (uint32_t)kRate, 2, true);
    wav.write(interleaved.data(), frames);
    return 0;
  }

  usage();
  return 1;
}
