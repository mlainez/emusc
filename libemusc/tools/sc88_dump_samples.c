/* SPDX-License-Identifier: CC0-1.0 */
/* Write every sample the SC-88's wave ROMs hold as its own WAV file.
 *
 * The point is a corpus that can be listened to and measured one sample at
 * a time. A fault in the sample path - a decode that integrates away the
 * top of the spectrum, a length read from the wrong field - is obvious in
 * a single drum hit and nearly invisible in a mix, so the samples are
 * worth having as files.
 *
 * The library's own descramble, FCE decoder and oscillator are used rather
 * than a reimplementation, so what lands on disk is exactly what the
 * renderer plays for a note at the sample's root key, before its envelope,
 * filter and level. Names come from the multisample directory that
 * references each sample, since that is the only name the ROM gives them.
 */
#include "engines/xp/sc88_oscillator.h"
#include "engines/xp/rom.h"
#include "engines/xp/wave.h"

#include <math.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIRECTORY_BASE 0x30000u
#define DIRECTORY_END 0x3606cu
#define DESCRIPTOR_BASE 0x36100u
#define DESCRIPTOR_END 0x3f714u
/* The delay memory and the samples both count in one sample at 32 kHz. */
#define NATIVE_RATE 32000u

static uint16_t be16(const uint8_t *p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static int bank_index(uint8_t selector)
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

static uint8_t *read_file(const char *path, size_t *size)
{
  FILE *f = fopen(path, "rb");
  uint8_t *bytes;
  long length;
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) != 0 || (length = ftell(f)) < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  bytes = (uint8_t *)malloc((size_t)length);
  if (!bytes || fread(bytes, 1, (size_t)length, f) != (size_t)length) {
    free(bytes);
    fclose(f);
    return NULL;
  }
  fclose(f);
  *size = (size_t)length;
  return bytes;
}

static void put32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

/* 24-bit mono, which is what the decoder produces - no requantising. */
static bool write_wav(const char *path, const int32_t *pcm24, size_t count,
                      unsigned rate)
{
  uint8_t header[44];
  uint8_t *raw;
  size_t i;
  FILE *f;
  uint32_t bytes = (uint32_t)(count * 3u);
  memcpy(header, "RIFF", 4);
  put32(header + 4, 36u + bytes);
  memcpy(header + 8, "WAVEfmt ", 8);
  put32(header + 16, 16);
  put16(header + 20, 1);
  put16(header + 22, 1);
  put32(header + 24, rate);
  put32(header + 28, rate * 3u);
  put16(header + 32, 3);
  put16(header + 34, 24);
  memcpy(header + 36, "data", 4);
  put32(header + 40, bytes);
  raw = (uint8_t *)malloc(bytes ? bytes : 1u);
  if (!raw)
    return false;
  for (i = 0; i < count; ++i) {
    int32_t v = pcm24[i];
    if (v > 0x7fffff)
      v = 0x7fffff;
    else if (v < -0x800000)
      v = -0x800000;
    raw[i * 3] = (uint8_t)v;
    raw[i * 3 + 1] = (uint8_t)(v >> 8);
    raw[i * 3 + 2] = (uint8_t)(v >> 16);
  }
  f = fopen(path, "wb");
  if (!f) {
    free(raw);
    return false;
  }
  fwrite(header, 1, sizeof header, f);
  fwrite(raw, 1, bytes, f);
  fclose(f);
  free(raw);
  return true;
}

/* Play the decoded sample through the library's own oscillator at the
 * rate the renderer gives it at its root key: `0x38000` plus the
 * descriptor's base pitch correction, in 16384 units per octave. A sample
 * is not at concert pitch for its nominal root and the renderer corrects
 * for that, so a dump written at the raw rate runs a few cents from a
 * render of the same tone and the two slip a full cycle at 4 kHz within
 * 100 ms - no windowed spectral comparison between them holds. Here the
 * loop, the rate and the interpolation are the oscillator's own.
 *
 * Most of these samples are loops, and 1069 of the 2598 are under 50 ms -
 * a single cycle of a square wave is 30 samples. Written as one-shots they
 * are inaudible, which makes the corpus useless for listening, so a looped
 * sample is followed for `seconds` and faded at the end so the file does
 * not click. A one-shot runs to its end.
 */
static int32_t *play(const int32_t *pcm, size_t count, uint32_t base,
                     const struct sc88_wave_descriptor *desc,
                     enum sc88_wave_loop_type loop, uint32_t pitch_word,
                     unsigned rate, double seconds, size_t *out_count)
{
  struct sc88_wave_registers registers;
  struct sc88_oscillator oscillator;
  size_t target = (size_t)(seconds * rate);
  size_t initial, frames, fade, i;
  int32_t *out;

  if (!sc88_wave_prepare_registers(desc, false, &registers) ||
      !sc88_oscillator_init(&oscillator, pcm, count, base, &registers, loop,
                            pitch_word, rate, SC88_WRAP_FULL_CARRY) ||
      oscillator.step <= 0.0)
    return NULL;
  /* the first pass in output frames, so a long sample is never cut */
  initial = (size_t)ceil((double)oscillator.initial_count /
                         oscillator.step) + 1u;
  frames = oscillator.cycle_count && target > initial ? target : initial;
  out = (int32_t *)malloc(frames * sizeof *out);
  if (!out)
    return NULL;
  for (i = 0; i < frames; ++i) {
    float sample;
    if (!sc88_oscillator_next(&oscillator, &sample))
      break;
    out[i] = (int32_t)lrint((double)sample * 8388608.0);
  }
  frames = i;
  if (oscillator.cycle_count && frames) {
    /* a 10 ms fade, so the file ends instead of stopping */
    fade = rate / 100u;
    if (fade > frames)
      fade = frames;
    for (i = 0; i < fade; ++i) {
      double g = (double)(fade - i) / (double)fade;
      out[frames - fade + i] = (int32_t)((double)out[frames - fade + i] * g);
    }
  }
  *out_count = frames;
  return out;
}

static void sanitise(char *s)
{
  size_t i;
  size_t end;
  for (i = 0; s[i]; ++i)
    if (s[i] == '/' || s[i] == ' ' || s[i] == '.' || s[i] == '\\')
      s[i] = '_';
  /* trailing fill from the fixed-width name field */
  end = strlen(s);
  while (end > 0 && s[end - 1] == '_')
    s[--end] = '\0';
}

static void usage(const char *argv0)
{
  fprintf(stderr,
    "usage: %s --control FILE --wave A B C D [--out-dir DIR]\n"
    "       [--loop-seconds S] [--no-pitch-correction]\n"
    "       [--pitch-word-offset N]\n"
    "  --no-pitch-correction  play at the raw ROM rate, ignoring the\n"
    "                         descriptor's base pitch correction\n"
    "  --pitch-word-offset N  add N to the pitch word (16384 per octave),\n"
    "                         e.g. a tone's own per-key and component\n"
    "                         pitch terms, to match a specific render\n",
    argv0);
}

int main(int argc, char **argv)
{
  const char *control_path = NULL;
  const char *wave_paths[4] = {NULL, NULL, NULL, NULL};
  const char *out_dir = ".";
  double loop_seconds = 1.5;
  bool pitch_correction = true;
  long pitch_word_offset = 0;
  uint8_t *control = NULL;
  uint8_t *chips[4] = {NULL, NULL, NULL, NULL};
  uint8_t *decoded[4] = {NULL, NULL, NULL, NULL};
  const uint8_t *banks[8];
  size_t control_size = 0, chip_size[4] = {0, 0, 0, 0};
  struct sc88_rom rom;
  uint32_t directory;
  unsigned written = 0, refused = 0;
  int i;
  FILE *index;
  char index_path[1024];

  for (i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--control") && i + 1 < argc)
      control_path = argv[++i];
    else if (!strcmp(argv[i], "--wave") && i + 4 < argc) {
      wave_paths[0] = argv[++i]; wave_paths[1] = argv[++i];
      wave_paths[2] = argv[++i]; wave_paths[3] = argv[++i];
    } else if (!strcmp(argv[i], "--out-dir") && i + 1 < argc)
      out_dir = argv[++i];
    else if (!strcmp(argv[i], "--loop-seconds") && i + 1 < argc)
      loop_seconds = atof(argv[++i]);
    else if (!strcmp(argv[i], "--no-pitch-correction"))
      pitch_correction = false;
    else if (!strcmp(argv[i], "--pitch-word-offset") && i + 1 < argc)
      pitch_word_offset = atol(argv[++i]);
    else {
      usage(argv[0]);
      return 2;
    }
  }
  if (!control_path || !wave_paths[3]) {
    usage(argv[0]);
    return 2;
  }

  control = read_file(control_path, &control_size);
  if (!control || !sc88_rom_init(&rom, control, control_size)) {
    fprintf(stderr, "the control ROM was refused\n");
    return 1;
  }
  for (i = 0; i < 4; ++i) {
    chips[i] = read_file(wave_paths[i], &chip_size[i]);
    decoded[i] = (uint8_t *)malloc(SC88_WAVE_CHIP_SIZE);
    if (!chips[i] || !decoded[i] ||
        !sc88_wave_descramble_chip(chips[i], chip_size[i], decoded[i],
                                   SC88_WAVE_CHIP_SIZE)) {
      fprintf(stderr, "wave image %d was refused\n", i);
      return 1;
    }
    banks[i * 2] = decoded[i];
    banks[i * 2 + 1] = decoded[i] + SC88_WAVE_BANK_SIZE;
  }

  snprintf(index_path, sizeof index_path, "%s/index.tsv", out_dir);
  index = fopen(index_path, "w");
  if (!index) {
    fprintf(stderr, "cannot write %s\n", index_path);
    return 1;
  }
  fprintf(index, "file\tdirectory\tkey_high\troot_key\tbank\tsamples\t"
                 "rom_seconds\tloop_type\tstart\tloop\tend\t"
                 "attenuation\tfile_seconds\tpitch_correction\t"
                 "pitch_word\tcents\n");

  for (directory = DIRECTORY_BASE; directory + 16 <= DIRECTORY_END; ) {
    char name[13];
    uint32_t position;
    int previous = -1;
    unsigned zone = 0;
    if (control[directory + 14] != 0x03 || control[directory + 15] != 0xff) {
      ++directory;
      continue;
    }
    memcpy(name, control + directory + 2, 12);
    name[12] = '\0';
    sanitise(name);
    position = directory + 16;
    while (position + 6 <= DIRECTORY_END) {
      uint8_t boundary = control[position];
      uint16_t pointer = be16(control + position + 4);
      uint32_t descriptor_offset = DIRECTORY_BASE + pointer;
      struct sc88_wave_descriptor desc;
      enum sc88_wave_loop_type loop;
      int index_of_bank;
      if (boundary <= previous || boundary > 127 ||
          control[position + 1] != 0xff)
        break;
      previous = boundary;
      if (pointer != 0xffff &&
          descriptor_offset >= DESCRIPTOR_BASE &&
          descriptor_offset + SC88_WAVE_DESCRIPTOR_SIZE <= DESCRIPTOR_END &&
          (descriptor_offset - DESCRIPTOR_BASE) %
            SC88_WAVE_DESCRIPTOR_SIZE == 0 &&
          sc88_wave_descriptor_parse(control + descriptor_offset,
                                     SC88_WAVE_DESCRIPTOR_SIZE, &desc) &&
          (index_of_bank = bank_index(desc.bank_select)) >= 0) {
        uint32_t base = desc.address_a & ~UINT32_C(0x0f);
        size_t capacity = (size_t)(desc.address_c - base) + 1u;
        int32_t *pcm = (int32_t *)malloc(capacity * sizeof *pcm);
        size_t count = 0;
        uint32_t decoded_base = base;
        if (pcm && desc.address_c > base &&
            sc88_fce_decode_storage(banks[index_of_bank],
                                    SC88_WAVE_BANK_SIZE, &desc, pcm,
                                    capacity, &decoded_base, &count) &&
            count) {
          char path[1024];
          const char *loop_name;
          int32_t *played;
          size_t played_count = count;
          int16_t correction;
          long pitch_word;
          if (!sc88_wave_descriptor_loop_type(&desc, &loop))
            loop = SC88_WAVE_FORWARD_ONE_SHOT;
          loop_name =
            loop == SC88_WAVE_FORWARD_LOOP ? "forward"
            : loop == SC88_WAVE_PING_PONG_LOOP ? "ping-pong"
            : loop == SC88_WAVE_FORWARD_ONE_SHOT ? "one-shot"
            : "reverse-one-shot";
          correction = pitch_correction
            ? sc88_wave_pitch_correction(&desc, true) : 0;
          pitch_word = 0x38000 + (long)correction + pitch_word_offset;
          if (pitch_word < 0)
            pitch_word = 0;
          if (pitch_word > 0x3ffff)
            pitch_word = 0x3ffff;
          played = play(pcm, count, decoded_base, &desc, loop,
                        (uint32_t)pitch_word, NATIVE_RATE, loop_seconds,
                        &played_count);
          snprintf(path, sizeof path, "%s/%05x_%s_k%03u.wav", out_dir,
                   (unsigned)descriptor_offset, name, (unsigned)boundary);
          if (played && write_wav(path, played, played_count, NATIVE_RATE)) {
            fprintf(index,
                    "%05x_%s_k%03u.wav\t%s\t%u\t%u\t0x%02x\t%zu\t%.4f\t%s\t"
                    "0x%06x\t0x%06x\t0x%06x\t0x%04x\t%.4f\t%d\t"
                    "0x%05lx\t%+.2f\n",
                    (unsigned)descriptor_offset, name, (unsigned)boundary,
                    name, (unsigned)boundary, desc.root_key,
                    desc.bank_select, count,
                    (double)count / NATIVE_RATE, loop_name,
                    desc.address_a, desc.address_b, desc.address_c,
                    (unsigned)be16(control + position + 2),
                    (double)played_count / NATIVE_RATE, (int)correction,
                    pitch_word, (pitch_word - 0x38000) * 1200.0 / 16384.0);
            ++written;
          } else {
            ++refused;
          }
          free(played);
        } else {
          ++refused;
        }
        free(pcm);
        ++zone;
      } else {
        ++refused;
      }
      position += 6;
      if (boundary == 127)
        break;
    }
    directory = zone ? position : directory + 1;
  }

  fclose(index);
  printf("%u samples written to %s, %u zones refused\n", written, out_dir,
         refused);
  for (i = 0; i < 4; ++i) {
    free(chips[i]);
    free(decoded[i]);
  }
  free(control);
  return 0;
}
