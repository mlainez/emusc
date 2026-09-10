/* SPDX-License-Identifier: CC0-1.0 */
/* Render a Standard MIDI File through the SC-88 device to a WAV file.
 *
 *   sc88_render --control ctl.bin --wave a.bin b.bin c.bin d.bin \
 *               --midi song.mid --out song.wav [--rate 44100] [--raw]
 *
 * The library had every part of a voice and no way to hear one: nothing
 * outside its own tests ever called sc88_device_midi or sc88_device_render.
 * This is the driver, and it is deliberately thin - it owns the file
 * formats and the clock, and no synthesis at all.
 *
 * Output is 32-bit float WAV because the engine's dry amplitude scale is
 * still provisional (`12_implementation/implementation_plan.md`): clipping a
 * take to 16 bits would destroy evidence before anyone has looked at it.
 */
#include "sc88_device.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC88_RENDER_BLOCK 256u

struct midi_event {
  uint64_t tick;
  uint32_t order;                       /* keeps a tick's events in file order */
  uint8_t status, data1, data2;
  uint32_t tempo;                       /* microseconds per quarter, or 0 */
  /* For a SysEx event, the payload inside the file's own buffer, which
     outlives playback. */
  const uint8_t *sysex;
  uint32_t sysex_len;
  uint8_t port;
};

struct midi_file {
  struct midi_event *events;
  size_t count, capacity;
  uint16_t division;
};

static void *xrealloc(void *p, size_t n)
{
  void *q = realloc(p, n);
  if (!q) {
    fprintf(stderr, "sc88_render: out of memory\n");
    exit(1);
  }
  return q;
}

static uint8_t *read_file(const char *path, size_t *size)
{
  FILE *f = fopen(path, "rb");
  uint8_t *bytes;
  long length;
  if (!f) {
    fprintf(stderr, "sc88_render: cannot open %s\n", path);
    return NULL;
  }
  if (fseek(f, 0, SEEK_END) != 0 || (length = ftell(f)) < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  bytes = (uint8_t *)malloc((size_t)length ? (size_t)length : 1);
  if (!bytes || fread(bytes, 1, (size_t)length, f) != (size_t)length) {
    free(bytes);
    fclose(f);
    fprintf(stderr, "sc88_render: cannot read %s\n", path);
    return NULL;
  }
  fclose(f);
  *size = (size_t)length;
  return bytes;
}

static uint32_t be32(const uint8_t *p)
{
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
         (uint32_t)p[2] << 8 | p[3];
}

static uint16_t be16(const uint8_t *p)
{
  return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

/* A variable-length quantity, refusing to run off the end of the track. */
static bool vlq(const uint8_t *p, size_t size, size_t *i, uint32_t *out)
{
  uint32_t value = 0;
  unsigned n;
  for (n = 0; n < 4; ++n) {
    uint8_t byte;
    if (*i >= size)
      return false;
    byte = p[(*i)++];
    value = (value << 7) | (byte & 0x7fu);
    if (!(byte & 0x80u)) {
      *out = value;
      return true;
    }
  }
  return false;
}

static void push(struct midi_file *mf, const struct midi_event *event)
{
  if (mf->count == mf->capacity) {
    mf->capacity = mf->capacity ? mf->capacity * 2 : 1024;
    mf->events = (struct midi_event *)xrealloc(
      mf->events, mf->capacity * sizeof *mf->events);
  }
  mf->events[mf->count] = *event;
  mf->events[mf->count].order = (uint32_t)mf->count;
  ++mf->count;
}

/* Which of the device's two MIDI ports a track plays into.
 *
 * The standard mechanism is the `FF 21` port meta event. Roland's own demo
 * SMFs carry no such event and instead name their tracks `PartA 1ch.`
 * through `PartB 16ch.`, which is the only statement of port in the file -
 * and it has to be honoured, because a 32-part song otherwise puts two
 * different instruments on one channel: Brass Nation asks for a synth drum
 * and a flute on channel 11, and whichever program change lands last wins
 * for both tracks' notes.
 */
static uint8_t track_name_port(const uint8_t *name, size_t len)
{
  size_t i;
  for (i = 0; i + 5 <= len; ++i)
    if (name[i] == 'P' && name[i + 1] == 'a' && name[i + 2] == 'r' &&
        name[i + 3] == 't' && (name[i + 4] == 'B' || name[i + 4] == 'b'))
      return 1u;
  return 0u;
}

static bool parse_track(struct midi_file *mf, const uint8_t *p, size_t size)
{
  uint64_t tick = 0;
  uint8_t running = 0;
  size_t i = 0;
  uint8_t port = 0;
  /* A track states its port in a meta event, which is normally its first;
     anything pushed before that statement is corrected when it arrives, so
     the order the file chose does not matter. */
  size_t first = mf->count;
  while (i < size) {
    struct midi_event event;
    uint32_t delta, length;
    uint8_t status;
    if (!vlq(p, size, &i, &delta) || i >= size)
      return false;
    tick += delta;
    status = p[i];
    if (status & 0x80u) {
      ++i;
      if (status < 0xf0u)
        running = status;               /* running status, channel voice only */
    } else {
      status = running;
      if (!status)
        return false;
    }
    memset(&event, 0, sizeof event);
    event.tick = tick;
    event.status = status;
    event.port = port;
    if (status == 0xffu) {              /* meta */
      uint8_t type;
      if (i >= size)
        return false;
      type = p[i++];
      if (!vlq(p, size, &i, &length) || i + length > size)
        return false;
      if (type == 0x03u && length) {
        uint8_t named = track_name_port(p + i, length);
        if (named != port) {
          size_t k;
          port = named;
          for (k = first; k < mf->count; ++k)
            mf->events[k].port = port;
        }
      } else if (type == 0x21u && length == 1) {
        port = p[i] < SC88_MIDI_PORT_COUNT ? p[i] : 0u;
        {
          size_t k;
          for (k = first; k < mf->count; ++k)
            mf->events[k].port = port;
        }
      }
      if (type == 0x51u && length == 3) {
        event.tempo = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8 |
                      p[i + 2];
        push(mf, &event);
      }
      i += length;
      if (type == 0x2fu)
        break;                          /* end of track */
    } else if (status == 0xf0u || status == 0xf7u) {
      if (!vlq(p, size, &i, &length) || i + length > size)
        return false;
      /* The payload is kept where it lies and played in tick order with
         everything else: a song's reverb and chorus settings arrive as
         SysEx, and dropping it silently renders the wrong effect. */
      event.sysex = p + i;
      event.sysex_len = (uint32_t)length;
      push(mf, &event);
      i += length;
    } else {
      unsigned wanted = (status & 0xe0u) == 0xc0u ? 1u : 2u;
      if (i + wanted > size)
        return false;
      event.data1 = p[i];
      event.data2 = wanted == 2 ? p[i + 1] : 0;
      i += wanted;
      push(mf, &event);
    }
  }
  return true;
}

static int compare(const void *a, const void *b)
{
  const struct midi_event *x = (const struct midi_event *)a;
  const struct midi_event *y = (const struct midi_event *)b;
  if (x->tick != y->tick)
    return x->tick < y->tick ? -1 : 1;
  return x->order < y->order ? -1 : x->order > y->order;
}

static bool parse_midi(struct midi_file *mf, const uint8_t *p, size_t size)
{
  uint16_t tracks, format;
  size_t i = 14;
  unsigned t;
  memset(mf, 0, sizeof *mf);
  if (size < 14 || memcmp(p, "MThd", 4) != 0 || be32(p + 4) < 6)
    return false;
  format = be16(p + 8);
  tracks = be16(p + 10);
  mf->division = be16(p + 12);
  if (format > 2 || (mf->division & 0x8000u) || mf->division == 0) {
    fprintf(stderr, "sc88_render: unsupported format %u or SMPTE division\n",
            format);
    return false;
  }
  i = 8 + be32(p + 4);
  for (t = 0; t < tracks && i + 8 <= size; ++t) {
    uint32_t length = be32(p + i + 4);
    if (memcmp(p + i, "MTrk", 4) != 0 || i + 8 + length > size)
      return false;
    if (!parse_track(mf, p + i + 8, length))
      return false;
    i += 8 + length;
  }
  qsort(mf->events, mf->count, sizeof *mf->events, compare);
  return mf->count > 0;
}

static void write_le32(FILE *f, uint32_t v)
{
  uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                  (uint8_t)(v >> 24)};
  fwrite(b, 1, 4, f);
}

static void write_le16(FILE *f, uint16_t v)
{
  uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
  fwrite(b, 1, 2, f);
}

/* A 32-bit float stereo WAV, header patched once the length is known. */
static void write_wav_header(FILE *f, unsigned rate, uint32_t frames)
{
  uint32_t data = frames * 2u * 4u;
  fwrite("RIFF", 1, 4, f);
  write_le32(f, 36u + data);
  fwrite("WAVEfmt ", 1, 8, f);
  write_le32(f, 16);
  write_le16(f, 3);                     /* IEEE float */
  write_le16(f, 2);
  write_le32(f, rate);
  write_le32(f, rate * 2u * 4u);
  write_le16(f, 8);
  write_le16(f, 32);
  fwrite("data", 1, 4, f);
  write_le32(f, data);
}

static void usage(void)
{
  fprintf(stderr,
    "usage: sc88_render --control FILE --wave A B C D --midi FILE "
    "--out FILE\n"
    "                   [--rate HZ] [--raw] [--tail SECONDS]\n"
    "                   [--wrap carry|reset|fraction] [--trace]\n"
    "  --raw   the wave images are undescrambled chip dumps\n"
    "  --wrap  oscillator fractional wrap, an open question: state it\n");
}

int main(int argc, char **argv)
{
  const char *control_path = NULL, *midi_path = NULL, *out_path = NULL;
  const char *wave_paths[SC88_WAVE_CHIP_COUNT] = {NULL, NULL, NULL, NULL};
  double rate = 44100.0, tail = 3.0;
  /* The oscillator's fractional wrap/stop behaviour is an open question the
     implementation plan requires a take to state rather than assume, so it
     is a switch with a named default and it is printed with every render. */
  enum sc88_fractional_wrap wrap = SC88_WRAP_FULL_CARRY;
  const char *wrap_name = "carry";
  bool raw = false, trace = false;
  uint8_t *control = NULL, *chips[SC88_WAVE_CHIP_COUNT] = {0};
  const uint8_t *chip_view[SC88_WAVE_CHIP_COUNT];
  size_t control_size = 0, chip_sizes[SC88_WAVE_CHIP_COUNT] = {0};
  uint8_t *midi_bytes = NULL;
  size_t midi_size = 0;
  struct midi_file mf;
  struct sc88_device device;
  FILE *out;
  float block[SC88_RENDER_BLOCK * 2];
  uint64_t frame = 0, next_frame;
  uint32_t total = 0, tempo = 500000;
  uint64_t last_tick = 0;
  double frames_per_tick;
  size_t event_index = 0, i;
  int accepted = 0, rejected = 0;
  uint64_t trace_at = 0;
  float trace_peak = 0.0f;
  /* A rejected message is not noise: it is a control this device does not
     implement yet, and the tally says which to add next. Indexed by status
     nibble, and by controller number for the control changes. */
  unsigned rejected_status[8] = {0};
  unsigned rejected_sysex = 0;
  unsigned rejected_cc[128] = {0};
  unsigned note_ons = 0;
  /* A rejected note is the worst artifact there is, so say which voice was
     asked for: the channel's variation and program at the time. */
  unsigned rejected_program[128] = {0};
  unsigned rejected_variation[128] = {0};
  unsigned rejected_channel[16] = {0};
  float peak = 0.0f;

  for (i = 1; (int)i < argc; ++i) {
    const char *a = argv[i];
    if (!strcmp(a, "--control") && (int)i + 1 < argc)
      control_path = argv[++i];
    else if (!strcmp(a, "--midi") && (int)i + 1 < argc)
      midi_path = argv[++i];
    else if (!strcmp(a, "--out") && (int)i + 1 < argc)
      out_path = argv[++i];
    else if (!strcmp(a, "--rate") && (int)i + 1 < argc)
      rate = atof(argv[++i]);
    else if (!strcmp(a, "--tail") && (int)i + 1 < argc)
      tail = atof(argv[++i]);
    else if (!strcmp(a, "--raw"))
      raw = true;
    else if (!strcmp(a, "--trace"))
      trace = true;
    else if (!strcmp(a, "--wrap") && (int)i + 1 < argc) {
      wrap_name = argv[++i];
      if (!strcmp(wrap_name, "carry"))
        wrap = SC88_WRAP_FULL_CARRY;
      else if (!strcmp(wrap_name, "reset"))
        wrap = SC88_WRAP_FULL_RESET;
      else if (!strcmp(wrap_name, "fraction"))
        wrap = SC88_WRAP_FRACTION_ONLY;
      else {
        usage();
        return 2;
      }
    }
    else if (!strcmp(a, "--wave") && (int)i + 4 < argc) {
      unsigned k;
      for (k = 0; k < SC88_WAVE_CHIP_COUNT; ++k)
        wave_paths[k] = argv[++i];
    } else {
      usage();
      return 2;
    }
  }
  if (!control_path || !midi_path || !out_path || !wave_paths[3] ||
      rate < 8000.0 || rate > 192000.0 || tail < 0.0) {
    usage();
    return 2;
  }

  control = read_file(control_path, &control_size);
  midi_bytes = read_file(midi_path, &midi_size);
  if (!control || !midi_bytes)
    return 1;
  for (i = 0; i < SC88_WAVE_CHIP_COUNT; ++i) {
    chips[i] = read_file(wave_paths[i], &chip_sizes[i]);
    if (!chips[i])
      return 1;
    chip_view[i] = chips[i];
  }
  if (!parse_midi(&mf, midi_bytes, midi_size)) {
    fprintf(stderr, "sc88_render: %s is not a MIDI file this can play\n",
            midi_path);
    return 1;
  }

  if (!(raw ? sc88_device_init_raw(&device, control, control_size, chip_view,
                                   chip_sizes, rate, wrap)
            : sc88_device_init_decoded(&device, control, control_size,
                                       chip_view, chip_sizes, rate, wrap))) {
    fprintf(stderr, "sc88_render: the device rejected these ROMs\n");
    return 1;
  }
  for (i = 0; i < SC88_WAVE_CHIP_COUNT; ++i)
    free(chips[i]);
  free(control);

  out = fopen(out_path, "wb");
  if (!out) {
    fprintf(stderr, "sc88_render: cannot write %s\n", out_path);
    return 1;
  }
  write_wav_header(out, (unsigned)rate, 0);

  frames_per_tick = rate * (double)tempo / (1e6 * mf.division);
  next_frame = 0;
  while (event_index < mf.count || frame < next_frame + (uint64_t)(tail * rate)) {
    size_t want = SC88_RENDER_BLOCK, n;
    /* dispatch everything due before the end of this block */
    while (event_index < mf.count) {
      const struct midi_event *event = mf.events + event_index;
      uint64_t at = next_frame +
        (uint64_t)((double)(event->tick - last_tick) * frames_per_tick);
      if (at > frame + want) {
        if (at - frame < want)
          want = (size_t)(at - frame);
        break;
      }
      next_frame = at;
      last_tick = event->tick;
      if (event->tempo) {
        tempo = event->tempo;
        frames_per_tick = rate * (double)tempo / (1e6 * mf.division);
      } else if (event->sysex_len) {
        if (sc88_device_sysex(&device, event->port, event->sysex,
                              event->sysex_len))
          ++accepted;
        else {
          ++rejected;
          ++rejected_sysex;
        }
      } else if (sc88_device_midi(&device, event->port, event->status,
                                 event->data1, event->data2)) {
        ++accepted;
        if ((event->status & 0xf0u) == 0x90u && event->data2)
          ++note_ons;
      } else {
        ++rejected;
        ++rejected_status[(event->status >> 4) & 7u];
        if ((event->status & 0xf0u) == 0xb0u)
          ++rejected_cc[event->data1 & 0x7fu];
        if ((event->status & 0xf0u) == 0x90u && event->data2) {
          /* the part, not the channel: the two ports have their own */
          uint8_t pt = (uint8_t)(event->port * 16u +
                                 (event->status & 0x0fu));
          ++rejected_program[device.channels[pt].program & 0x7fu];
          ++rejected_variation[device.channels[pt].variation & 0x7fu];
          ++rejected_channel[event->status & 0x0fu];
        }
      }
      ++event_index;
    }
    if (want == 0)
      want = 1;
    if (want > SC88_RENDER_BLOCK)
      want = SC88_RENDER_BLOCK;
    sc88_device_render(&device, block, want);
    for (n = 0; n < want * 2; ++n) {
      float v = block[n];
      float m = v < 0.0f ? -v : v;
      if (m > peak)
        peak = m;
    }
    fwrite(block, sizeof(float), want * 2, out);
    for (n = 0; n < want * 2; ++n) {
      float m = block[n] < 0.0f ? -block[n] : block[n];
      if (m > trace_peak)
        trace_peak = m;
    }
    frame += want;
    if (trace && frame >= trace_at) {
      printf("  t=%6.1f s  active slots %2u  peak %.5f  events %zu/%zu\n",
             (double)frame / rate,
             sc88_engine_active_slots(&device.engine), trace_peak,
             event_index, mf.count);
      trace_peak = 0.0f;
      trace_at = frame + (uint64_t)rate;
    }
    total += (uint32_t)want;
    if (event_index >= mf.count && frame > next_frame + (uint64_t)(tail * rate))
      break;
  }

  rewind(out);
  write_wav_header(out, (unsigned)rate, total);
  fclose(out);
  free(mf.events);
  free(midi_bytes);
  printf("%s: %u frames at %.0f Hz, %.1f s, peak %.4f, wrap %s, "
         "%d messages accepted, %d rejected\n",
         out_path, total, rate, total / rate, peak, wrap_name, accepted,
         rejected);
  printf("  note-ons %u\n", note_ons);
  if (device.unhandled_sysex)
    printf("  sysex addresses received but not acted on: %lu\n",
           device.unhandled_sysex);
  if (device.substituted_random_pan)
    printf("  substituted a defined pan for GS random pan %lu times\n",
           device.substituted_random_pan);
  if (rejected) {
    static const char *const names[8] = {
      "note off", "note on", "poly pressure", "control change",
      "program change", "channel pressure", "pitch bend", "system"};
    printf("  unimplemented:");
    for (i = 0; i < 8; ++i)
      if (rejected_status[i])
        printf(" %s x%u", names[i], rejected_status[i]);
    for (i = 0; i < 128; ++i)
      if (rejected_cc[i])
        printf(" cc%u x%u", (unsigned)i, rejected_cc[i]);
    if (rejected_sysex)
      printf(" malformed or foreign sysex x%u", rejected_sysex);
    printf("\n");
  }
  if (rejected_status[1]) {
    printf("  dropped notes asked for program:");
    for (i = 0; i < 128; ++i)
      if (rejected_program[i])
        printf(" %u x%u", (unsigned)i, rejected_program[i]);
    printf("\n  on channel:");
    for (i = 0; i < 16; ++i)
      if (rejected_channel[i])
        printf(" %u x%u", (unsigned)i + 1, rejected_channel[i]);
    printf("\n  and variation:");
    for (i = 0; i < 128; ++i)
      if (rejected_variation[i])
        printf(" %u x%u", (unsigned)i, rejected_variation[i]);
    printf("\n");
  }
  sc88_device_destroy(&device);
  return 0;
}
