/* Where a JV-1080 program change lands, what GM System On leaves, and
 * what the device powers on in.
 *
 * Every check compares two renders of the same note: a program change that
 * resolves to a patch through one route must sound exactly like the same
 * patch reached through another, and differently from a neighbouring
 * bank's patch at the same number. Nothing here fixes what a patch sounds
 * like, only which one was loaded.
 *
 * Needs the held ROMs: JV1080_CONTROL_ROM and JV1080_WAVE_ROMS (four
 * comma-separated paths). Skipped without them.
 */
#include "engines/xp/device.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

const int SKIP = 77;
const double kRate = 32000.0;
const size_t kFrames = 8192;

std::vector<uint8_t> read_all(const char *path)
{
  std::vector<uint8_t> bytes;
  FILE *f = fopen(path, "rb");
  if (!f)
    return bytes;
  int c;
  while ((c = fgetc(f)) != EOF)
    bytes.push_back((uint8_t)c);
  fclose(f);
  return bytes;
}

struct Roms {
  std::vector<uint8_t> control;
  std::vector<uint8_t> waves[XP_WAVE_CHIP_COUNT];
};

bool load_roms(Roms *roms)
{
  const char *control = getenv("JV1080_CONTROL_ROM");
  const char *csv = getenv("JV1080_WAVE_ROMS");
  if (!control || !*control || !csv || !*csv)
    return false;
  roms->control = read_all(control);
  if (roms->control.empty())
    return false;
  const char *p = csv;
  for (unsigned i = 0; i < XP_WAVE_CHIP_COUNT; ++i) {
    const char *comma = strchr(p, ',');
    std::string path = comma ? std::string(p, comma) : std::string(p);
    roms->waves[i] = read_all(path.c_str());
    if (roms->waves[i].empty())
      return false;
    if (!comma && i + 1 < XP_WAVE_CHIP_COUNT)
      return false;
    p = comma ? comma + 1 : p;
  }
  return true;
}

typedef std::function<void(EmuSC::Xp::Device *)> Setup;

void midi(EmuSC::Xp::Device *d, uint8_t status, uint8_t a, uint8_t b = 0)
{
  EmuSC::Xp::device_midi(d, 0, status, a, b);
}

/* A DT1 to the temporary performance part record `01 00 1n 00`: receive
   on, channel n, the given group type and id, patch number as two
   nibbles, then the rest as the corpus's own part block writes it. */
void part_record(EmuSC::Xp::Device *d, unsigned n, uint8_t type, uint8_t id,
                 uint8_t number)
{
  uint8_t m[] = { 0xf0, 0x41, 0x10, 0x6a, 0x12, 0x01, 0x00,
                  (uint8_t)(0x10 | n), 0x00,
                  1, (uint8_t)n, type, id,
                  (uint8_t)(number >> 4), (uint8_t)(number & 0x0f),
                  127, 64, 48, 50, 0, 127, 0, 0, 1, 1, 1, 0, 127,
                  0, 0xf7 };
  unsigned sum = 0;
  for (size_t i = 5; i + 2 < sizeof m; ++i)
    sum += m[i];
  m[sizeof m - 2] = (uint8_t)((0x80u - (sum & 0x7fu)) & 0x7fu);
  assert(EmuSC::Xp::device_sysex(d, 0, m, sizeof m));
}

std::vector<float> render(const Roms &roms, const Setup &setup,
                          uint8_t channel = 0, uint8_t key = 60)
{
  const uint8_t *chips[XP_WAVE_CHIP_COUNT];
  size_t sizes[XP_WAVE_CHIP_COUNT];
  for (unsigned i = 0; i < XP_WAVE_CHIP_COUNT; ++i) {
    chips[i] = roms.waves[i].data();
    sizes[i] = roms.waves[i].size();
  }
  EmuSC::Xp::Device *d = new EmuSC::Xp::Device();
  assert(EmuSC::Xp::device_init_raw(d, roms.control.data(),
                                    roms.control.size(), chips, sizes, kRate,
                                    XP_WRAP_FULL_CARRY));
  setup(d);
  midi(d, (uint8_t)(0x90 | channel), key, 100);
  std::vector<float> out(2 * kFrames);
  EmuSC::Xp::device_render(d, out.data(), kFrames);
  EmuSC::Xp::device_destroy(d);
  delete d;
  return out;
}

double energy(const std::vector<float> &x)
{
  double e = 0.0;
  for (float v : x)
    e += (double)v * v;
  return e;
}

void bank(EmuSC::Xp::Device *d, uint8_t msb, uint8_t lsb, uint8_t program,
          uint8_t channel = 0)
{
  midi(d, (uint8_t)(0xb0 | channel), 0, msb);
  midi(d, (uint8_t)(0xb0 | channel), 32, lsb);
  midi(d, (uint8_t)(0xc0 | channel), program);
}

/* A DT1 of one byte to tone `tone`'s record on part 1, at `field` of its
   first block. */
void tone_field(EmuSC::Xp::Device *d, unsigned tone, uint8_t field,
                uint8_t value)
{
  uint8_t m[] = { 0xf0, 0x41, 0x10, 0x6a, 0x12, 0x02, 0x00,
                  (uint8_t)(0x10 + 2 * tone), field, value, 0, 0xf7 };
  unsigned sum = 0;
  for (size_t i = 5; i + 2 < sizeof m; ++i)
    sum += m[i];
  m[sizeof m - 2] = (uint8_t)((0x80u - (sum & 0x7fu)) & 0x7fu);
  assert(EmuSC::Xp::device_sysex(d, 0, m, sizeof m));
}

/* A tone's wave number, 0x03-0x04, as the two nibbles of one DT1: a wide
   field is composed only from a write that carries both. */
void tone_wave_number(EmuSC::Xp::Device *d, unsigned tone, uint8_t number)
{
  uint8_t m[] = { 0xf0, 0x41, 0x10, 0x6a, 0x12, 0x02, 0x00,
                  (uint8_t)(0x10 + 2 * tone), 0x03,
                  (uint8_t)(number >> 4), (uint8_t)(number & 0x0f), 0, 0xf7 };
  unsigned sum = 0;
  for (size_t i = 5; i + 2 < sizeof m; ++i)
    sum += m[i];
  m[sizeof m - 2] = (uint8_t)((0x80u - (sum & 0x7fu)) & 0x7fu);
  assert(EmuSC::Xp::device_sysex(d, 0, m, sizeof m));
}

/* A DT1 of one byte to part 1's patch common, at `field`. */
void common_field(EmuSC::Xp::Device *d, uint8_t field, uint8_t value)
{
  uint8_t m[] = { 0xf0, 0x41, 0x10, 0x6a, 0x12, 0x02, 0x00, 0x00, field,
                  value, 0, 0xf7 };
  unsigned sum = 0;
  for (size_t i = 5; i + 2 < sizeof m; ++i)
    sum += m[i];
  m[sizeof m - 2] = (uint8_t)((0x80u - (sum & 0x7fu)) & 0x7fu);
  assert(EmuSC::Xp::device_sysex(d, 0, m, sizeof m));
}

/* The strongest frequency between 150 Hz and 3 kHz in the left channel,
   scanned in one-cent steps. */
double strongest_hz(const std::vector<float> &x)
{
  double best = 0.0, bestHz = 0.0;
  for (double cents = 0.0; cents < 1200.0 * std::log2(3000.0 / 150.0);
       cents += 1.0) {
    double hz = 150.0 * std::pow(2.0, cents / 1200.0);
    double w = 2.0 * M_PI * hz / kRate, re = 0.0, im = 0.0;
    for (size_t i = 0; i < kFrames; ++i) {
      double win = 0.5 - 0.5 * std::cos(2.0 * M_PI * (double)i / kFrames);
      re += win * x[2 * i] * std::cos(w * (double)i);
      im += win * x[2 * i] * std::sin(w * (double)i);
    }
    if (re * re + im * im > best) {
      best = re * re + im * im;
      bestHz = hz;
    }
  }
  return bestHz;
}

/* A DT1 of `data` at the four-byte address `a`. */
void dt1(EmuSC::Xp::Device *d, const uint8_t a[4],
         const std::vector<uint8_t> &data)
{
  std::vector<uint8_t> m = { 0xf0, 0x41, 0x10, 0x6a, 0x12,
                             a[0], a[1], a[2], a[3] };
  m.insert(m.end(), data.begin(), data.end());
  unsigned sum = 0;
  for (size_t i = 5; i < m.size(); ++i)
    sum += m[i];
  m.push_back((uint8_t)((0x80u - (sum & 0x7fu)) & 0x7fu));
  m.push_back(0xf7);
  assert(EmuSC::Xp::device_sysex(d, 0, m.data(), m.size()));
}

const uint8_t kGmOn[] = { 0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7 };

}  // namespace

int main(void)
{
  Roms roms;
  if (!load_roms(&roms)) {
    printf("JV1080_CONTROL_ROM / JV1080_WAVE_ROMS unset - skipping\n");
    return SKIP;
  }

  /* Power-on: USER:001 on part 1, receiving on channel 1 - the patch a
     program change on a USER latch reaches at number 0, and not USER:014,
     which is what the system image stores before the boot routine
     overrides it. Nothing listens on channel 2. */
  Setup none = [](EmuSC::Xp::Device *) {};
  std::vector<float> boot = render(roms, none);
  assert(energy(boot) > 0.0);
  assert(boot == render(roms, [](EmuSC::Xp::Device *d) {
    bank(d, 80, 0, 0);
  }));
  assert(boot != render(roms, [](EmuSC::Xp::Device *d) {
    bank(d, 80, 0, 13);
  }));
  assert(energy(render(roms, none, 1)) == 0.0);

  /* A host reset puts it back. */
  assert(boot == render(roms, [](EmuSC::Xp::Device *d) {
    bank(d, 81, 0, 69);
    EmuSC::Xp::device_reset_controllers(d);
  }));

  /* Pitch key follow, tone field 0x40, on PR-A 001's one sounding tone
     (tone 2): index 10 is +50 %, so key 72 sounds six semitones under
     index 12's +100 %, and the two meet at the pivot, key 60. */
  auto kf = [](uint8_t index) {
    return [index](EmuSC::Xp::Device *d) {
      bank(d, 81, 0, 0);
      tone_field(d, 1, 0x40, index);
    };
  };
  assert(render(roms, kf(10)) == render(roms, kf(12)));
  {
    double full = strongest_hz(render(roms, kf(12), 0, 72));
    double half = strongest_hz(render(roms, kf(10), 0, 72));
    assert(full > 0.0 && half > 0.0);
    assert(std::fabs(1200.0 * std::log2(full / half) - 600.0) < 5.0);
  }

  /* Cutoff key follow, tone field 0x52, on the same tone, whose filter is
     a low-pass at cutoff 58, its F-ENV depth zeroed (wire 63) so that the
     envelope does not carry the corner to the top of its range: index 5
     (0 %) and 12 (+100 %) meet at key 60, and two octaves up, where the
     note's fundamental sits above both corners, +100 %'s corner is two
     octaves higher and passes far more of it. */
  auto ckf = [](uint8_t index) {
    return [index](EmuSC::Xp::Device *d) {
      bank(d, 81, 0, 0);
      tone_field(d, 1, 0x55, 63);
      tone_field(d, 1, 0x52, index);
    };
  };
  assert(render(roms, ckf(5)) == render(roms, ckf(12)));
  assert(energy(render(roms, ckf(12), 0, 84)) >
         10.0 * energy(render(roms, ckf(5), 0, 84)));

  /* Tone delay, fields 0x09 (mode) and 0x0A (time), on the same tone.
     NORMAL at time 16 starts it 170 ms after the note-on (`M-016`); a
     KEY-OFF tone does not sound while the key is held; and HOLD cancels a
     tone whose key comes up before its delay has run. */
  auto delay = [](uint8_t mode, uint8_t time) {
    return [mode, time](EmuSC::Xp::Device *d) {
      bank(d, 81, 0, 0);
      tone_field(d, 1, 0x09, mode);
      tone_field(d, 1, 0x0a, time);
    };
  };
  {
    auto first = [](const std::vector<float> &x) {
      size_t i = 0;
      while (i < kFrames && x[2 * i] == 0.0f && x[2 * i + 1] == 0.0f)
        ++i;
      return i;
    };
    size_t at0 = first(render(roms, delay(0, 0)));
    assert(at0 < 64);
    assert(first(render(roms, delay(0, 16))) ==
           at0 + (size_t)std::lround(0.170 * kRate));
    assert(render(roms, delay(0, 0)) == render(roms, [](EmuSC::Xp::Device *d) {
      bank(d, 81, 0, 0);
    }));
    assert(energy(render(roms, delay(5, 0))) == 0.0);
    assert(energy(render(roms, delay(6, 0))) == 0.0);
    std::vector<float> out(2 * kFrames);
    auto early_release = [&](uint8_t mode) {
      const uint8_t *chips[XP_WAVE_CHIP_COUNT];
      size_t sizes[XP_WAVE_CHIP_COUNT];
      for (unsigned i = 0; i < XP_WAVE_CHIP_COUNT; ++i) {
        chips[i] = roms.waves[i].data();
        sizes[i] = roms.waves[i].size();
      }
      EmuSC::Xp::Device *d = new EmuSC::Xp::Device();
      assert(EmuSC::Xp::device_init_raw(d, roms.control.data(),
                                        roms.control.size(), chips, sizes,
                                        kRate, XP_WRAP_FULL_CARRY));
      delay(mode, 16)(d);
      midi(d, 0x90, 60, 100);
      std::fill(out.begin(), out.end(), 0.0f);
      EmuSC::Xp::device_render(d, out.data(), 1024);  /* 32 ms, inside it */
      midi(d, 0x80, 60, 0);
      EmuSC::Xp::device_render(d, out.data() + 2048, kFrames - 1024);
      EmuSC::Xp::device_destroy(d);
      delete d;
      return energy(out);
    };
    assert(early_release(1) == 0.0);    /* HOLD: cancelled */
    assert(early_release(0) > 0.0);     /* NORMAL: sounds, postponed */
    /* KEY-OFF-NORMAL and KEY-OFF-DECAY (`M-113`) sound after the note-off
       and its delay, and not before: the first 170 ms after it are
       silent. */
    for (uint8_t mode : { (uint8_t)5, (uint8_t)6 }) {
      assert(early_release(mode) > 0.0);
      double early = 0.0;
      for (size_t i = 0; i < 2 * (1024 + 5440); ++i)
        early += (double)out[i] * out[i];
      assert(early == 0.0);
    }

    /* KEY-INTERVAL, mode 2 (`M-020`): a note with no previous note-on is
       silent, and a later note is delayed by the interval since it,
       whatever the delay time field says. Two notes 2000 frames apart:
       nothing sounds before frame 4000, then the second note's tone does.
       The previous note-on is the same key's; another key's does not
       count (the modelled choice, see the engine). */
    assert(energy(render(roms, delay(2, 64))) == 0.0);
    auto two_notes = [&](uint8_t firstKey) {
      const uint8_t *chips[XP_WAVE_CHIP_COUNT];
      size_t sizes[XP_WAVE_CHIP_COUNT];
      for (unsigned i = 0; i < XP_WAVE_CHIP_COUNT; ++i) {
        chips[i] = roms.waves[i].data();
        sizes[i] = roms.waves[i].size();
      }
      EmuSC::Xp::Device *d = new EmuSC::Xp::Device();
      assert(EmuSC::Xp::device_init_raw(d, roms.control.data(),
                                        roms.control.size(), chips, sizes,
                                        kRate, XP_WRAP_FULL_CARRY));
      delay(2, 64)(d);
      std::fill(out.begin(), out.end(), 0.0f);
      midi(d, 0x90, firstKey, 100);
      EmuSC::Xp::device_render(d, out.data(), 512);
      midi(d, 0x80, firstKey, 0);
      EmuSC::Xp::device_render(d, out.data() + 2 * 512, 2000 - 512);
      midi(d, 0x90, 60, 100);
      EmuSC::Xp::device_render(d, out.data() + 2 * 2000, kFrames - 2000);
      EmuSC::Xp::device_destroy(d);
      delete d;
    };
    two_notes(60);
    {
      double before = 0.0, after = 0.0;
      for (size_t i = 0; i < 2 * 4000; ++i)
        before += (double)out[i] * out[i];
      for (size_t i = 2 * 4000; i < 2 * 4064; ++i)
        after += (double)out[i] * out[i];
      assert(before == 0.0);
      assert(after > 0.0);
    }
    two_notes(62);
    assert(energy(out) == 0.0);
  }

  /* The rhythm note's envelope mode, field 0x08. PR-A's kit is mode 0,
     NO-SUSTAIN, on every key: a note-off inside the first three segments
     waits for their end, so on key 59, whose time 2 is a long fall to
     zero, a 10 ms gate and a 40 ms gate render the same hit. Written to
     mode 1, SUSTAIN, the two differ. */
  {
    auto gated = [&](uint8_t mode, size_t gate) {
      const uint8_t *chips[XP_WAVE_CHIP_COUNT];
      size_t sizes[XP_WAVE_CHIP_COUNT];
      for (unsigned i = 0; i < XP_WAVE_CHIP_COUNT; ++i) {
        chips[i] = roms.waves[i].data();
        sizes[i] = roms.waves[i].size();
      }
      EmuSC::Xp::Device *d = new EmuSC::Xp::Device();
      assert(EmuSC::Xp::device_init_raw(d, roms.control.data(),
                                        roms.control.size(), chips, sizes,
                                        kRate, XP_WRAP_FULL_CARRY));
      bank(d, 81, 0, 0, 9);
      if (mode != 0xff) {
        uint8_t m[] = { 0xf0, 0x41, 0x10, 0x6a, 0x12, 0x02, 0x09, 59, 0x08,
                        mode, 0, 0xf7 };
        unsigned sum = 0;
        for (size_t i = 5; i + 2 < sizeof m; ++i)
          sum += m[i];
        m[sizeof m - 2] = (uint8_t)((0x80u - (sum & 0x7fu)) & 0x7fu);
        assert(EmuSC::Xp::device_sysex(d, 0, m, sizeof m));
      }
      std::vector<float> x(2 * kFrames);
      midi(d, 0x99, 59, 100);
      EmuSC::Xp::device_render(d, x.data(), gate);
      midi(d, 0x89, 59, 0);
      EmuSC::Xp::device_render(d, x.data() + 2 * gate, kFrames - gate);
      EmuSC::Xp::device_destroy(d);
      delete d;
      return x;
    };
    const size_t ms10 = 320, ms40 = 1280;
    std::vector<float> a = gated(0xff, ms10);
    assert(energy(a) > 0.0);
    assert(a == gated(0xff, ms40));
    assert(a == gated(0, ms10));
    assert(gated(1, ms10) != gated(1, ms40));
  }

  /* The bender, on PR-A 001, whose range is 2 up: full deflection before
     the note sounds it 200 cents up (`M-014`), and a bend arriving while
     the note sounds moves it. */
  {
    double centre = strongest_hz(render(roms, [](EmuSC::Xp::Device *d) {
      bank(d, 81, 0, 0);
    }, 0, 72));
    double up = strongest_hz(render(roms, [](EmuSC::Xp::Device *d) {
      bank(d, 81, 0, 0);
      midi(d, 0xe0, 0x7f, 0x7f);
    }, 0, 72));
    assert(std::fabs(1200.0 * std::log2(up / centre) - 200.0) < 5.0);
  }

  /* Live controllers, rendered in two halves around a message. */
  auto two_halves = [&](const Setup &before, const Setup &between) {
    const uint8_t *chips[XP_WAVE_CHIP_COUNT];
    size_t sizes[XP_WAVE_CHIP_COUNT];
    for (unsigned i = 0; i < XP_WAVE_CHIP_COUNT; ++i) {
      chips[i] = roms.waves[i].data();
      sizes[i] = roms.waves[i].size();
    }
    EmuSC::Xp::Device *d = new EmuSC::Xp::Device();
    assert(EmuSC::Xp::device_init_raw(d, roms.control.data(),
                                      roms.control.size(), chips, sizes,
                                      kRate, XP_WRAP_FULL_CARRY));
    bank(d, 81, 0, 0);
    before(d);
    midi(d, 0x90, 60, 100);
    std::vector<float> x(2 * kFrames);
    EmuSC::Xp::device_render(d, x.data(), kFrames / 2);
    between(d);
    EmuSC::Xp::device_render(d, x.data() + kFrames, kFrames / 2);
    EmuSC::Xp::device_destroy(d);
    delete d;
    return x;
  };
  Setup nothing = [](EmuSC::Xp::Device *) {};
  assert(two_halves(nothing, [](EmuSC::Xp::Device *d) {
    midi(d, 0xe0, 0x7f, 0x7f);
  }) != two_halves(nothing, nothing));

  /* The hold pedal: a note-off under it changes nothing until the pedal
     comes up. */
  {
    Setup pedal = [](EmuSC::Xp::Device *d) { midi(d, 0xb0, 64, 127); };
    std::vector<float> held = two_halves(pedal, nothing);
    assert(held == two_halves(pedal, [](EmuSC::Xp::Device *d) {
      midi(d, 0x80, 60, 0);
    }));
    assert(held != two_halves(nothing, [](EmuSC::Xp::Device *d) {
      midi(d, 0x80, 60, 0);
    }));
    assert(held != two_halves(pedal, [](EmuSC::Xp::Device *d) {
      midi(d, 0x80, 60, 0);
      midi(d, 0xb0, 64, 0);
    }));
  }

  /* The release, on PR-A 001's tone with its envelope squared to full
     sustain. At time 4 = 64 the table reads 667 ms per 20 dB, so the level
     falls about 9.0 dB between 0.1-0.2 s and 0.4-0.5 s after the note-off;
     at time 4 = 8 it reads 19.7 ms, about 20 dB between the first 5 ms
     and the 5 ms from 20 ms on, where the old fitted law gave 11. Each is
     taken over the same stretch of the held note. */
  {
    auto drop = [&](uint8_t t4, bool release, size_t a0, size_t a1,
                    size_t b0, size_t b1) {
      const uint8_t *chips[XP_WAVE_CHIP_COUNT];
      size_t sizes[XP_WAVE_CHIP_COUNT];
      for (unsigned i = 0; i < XP_WAVE_CHIP_COUNT; ++i) {
        chips[i] = roms.waves[i].data();
        sizes[i] = roms.waves[i].size();
      }
      EmuSC::Xp::Device *d = new EmuSC::Xp::Device();
      assert(EmuSC::Xp::device_init_raw(d, roms.control.data(),
                                        roms.control.size(), chips, sizes,
                                        kRate, XP_WRAP_FULL_CARRY));
      bank(d, 81, 0, 0);
      for (uint8_t f = 0x6e; f <= 0x70; ++f)
        tone_field(d, 1, f, 0);            /* times 1-3: straight to sustain */
      for (uint8_t f = 0x72; f <= 0x74; ++f)
        tone_field(d, 1, f, 127);          /* levels 1-3 */
      tone_field(d, 1, 0x71, t4);          /* time 4 */
      midi(d, 0x90, 60, 100);
      std::vector<float> x(2 * 16000);
      EmuSC::Xp::device_render(d, x.data(), 3200);        /* 0.1 s */
      if (release)
        midi(d, 0x80, 60, 0);
      EmuSC::Xp::device_render(d, x.data(), 16000);       /* 0.5 s */
      EmuSC::Xp::device_destroy(d);
      delete d;
      auto level = [&](size_t from, size_t to) {
        double e = 0.0;
        for (size_t i = 2 * from; i < 2 * to; ++i)
          e += (double)x[i] * x[i];
        return 10.0 * std::log10(e);
      };
      return level(a0, a1) - level(b0, b1);
    };
    double slow = drop(64, true, 3200, 6400, 12800, 16000) -
                  drop(64, false, 3200, 6400, 12800, 16000);
    assert(std::fabs(slow - 9.0) < 2.0);
    double fast = drop(8, true, 0, 160, 640, 800) -
                  drop(8, false, 0, 160, 640, 800);
    assert(std::fabs(fast - 20.3) < 3.0);
  }

  /* A fall lasts one duration whatever it falls to, and moves linearly in
     level units. T2 = 60 from 127 to 64, taken over the same note held at
     127 so the wave's own decay drops out: the table's 551.8 ms for 20 dB
     over the 40 % of a full traverse gives 1.38 s, so halfway, at 0.69 s,
     the level stands at 95.5 units - -12.3 dB - and by 1.5 s it has
     arrived at 64's -24.9. Timed by its span in dB it would have arrived by
     0.66 s. */
  {
    auto env_db = [&](uint8_t l2) {
      const uint8_t *chips[XP_WAVE_CHIP_COUNT];
      size_t sizes[XP_WAVE_CHIP_COUNT];
      for (unsigned i = 0; i < XP_WAVE_CHIP_COUNT; ++i) {
        chips[i] = roms.waves[i].data();
        sizes[i] = roms.waves[i].size();
      }
      EmuSC::Xp::Device *d = new EmuSC::Xp::Device();
      assert(EmuSC::Xp::device_init_raw(d, roms.control.data(),
                                        roms.control.size(), chips, sizes,
                                        kRate, XP_WRAP_FULL_CARRY));
      bank(d, 81, 0, 0);
      const uint8_t times[4] = { 0, 60, 0, 0 };
      const uint8_t levels[3] = { 127, l2, l2 };
      for (unsigned i = 0; i < 4; ++i)
        tone_field(d, 1, (uint8_t)(0x6e + i), times[i]);
      for (unsigned i = 0; i < 3; ++i)
        tone_field(d, 1, (uint8_t)(0x72 + i), levels[i]);
      midi(d, 0x90, 60, 100);
      std::vector<float> x(2 * 51200);
      EmuSC::Xp::device_render(d, x.data(), 51200);     /* 1.6 s */
      EmuSC::Xp::device_destroy(d);
      delete d;
      return x;
    };
    std::vector<float> fall = env_db(64), flat = env_db(127);
    auto ratio_db = [&](double at) {
      size_t a = (size_t)(at * kRate), b = a + 320;     /* 10 ms */
      double e0 = 0.0, e1 = 0.0;
      for (size_t i = 2 * a; i < 2 * b; ++i) {
        e0 += (double)fall[i] * fall[i];
        e1 += (double)flat[i] * flat[i];
      }
      return 10.0 * std::log10(e0 / e1);
    };
    assert(std::fabs(ratio_db(0.69) + 12.3) < 1.0);
    assert(std::fabs(ratio_db(1.50) + 24.9) < 0.5);
  }

  /* The A-ENV's velocity sensitivity, tone field 0x6A, wire value = the
     setting + 50: at +50 velocity 20 reads the level law's
     40 log10(20/127) = -32.1 dB below velocity 127, and at -50 the law
     turns over, so velocity 108 - 128 less 20 - reads that far below
     velocity 1. */
  {
    auto at_velocity = [&](uint8_t wire, uint8_t velocity) {
      const uint8_t *chips[XP_WAVE_CHIP_COUNT];
      size_t sizes[XP_WAVE_CHIP_COUNT];
      for (unsigned i = 0; i < XP_WAVE_CHIP_COUNT; ++i) {
        chips[i] = roms.waves[i].data();
        sizes[i] = roms.waves[i].size();
      }
      EmuSC::Xp::Device *d = new EmuSC::Xp::Device();
      assert(EmuSC::Xp::device_init_raw(d, roms.control.data(),
                                        roms.control.size(), chips, sizes,
                                        kRate, XP_WRAP_FULL_CARRY));
      bank(d, 81, 0, 0);
      tone_field(d, 1, 0x69, 0);           /* curve 0, the level law */
      tone_field(d, 1, 0x6a, wire);
      midi(d, 0x90, 60, velocity);
      std::vector<float> x(2 * kFrames);
      EmuSC::Xp::device_render(d, x.data(), kFrames);
      EmuSC::Xp::device_destroy(d);
      delete d;
      return 10.0 * std::log10(energy(x));
    };
    /* Each against the same two velocities at sensitivity 0, which takes
       out the patch's other velocity terms - its filter envelope opens
       with velocity too. */
    double up = (at_velocity(100, 20) - at_velocity(100, 127)) -
                (at_velocity(50, 20) - at_velocity(50, 127));
    assert(std::fabs(up + 32.1) < 0.3);
    double down = (at_velocity(0, 108) - at_velocity(0, 1)) -
                  (at_velocity(50, 108) - at_velocity(50, 1));
    assert(std::fabs(down + 32.1) < 0.3);
  }

  /* The A-ENV's velocity curves, tone field 0x69, at sensitivity +50: curve
     2 reads velocity 64 48.4 dB under velocity 127 where curve 0 reads
     11.9, and curve 4 only 2.2 (`M-029`). Each is taken against curve 0 at
     the same two velocities, which cancels the patch's other velocity
     terms. */
  {
    auto at_curve = [&](uint8_t curve, uint8_t velocity) {
      const uint8_t *chips[XP_WAVE_CHIP_COUNT];
      size_t sizes[XP_WAVE_CHIP_COUNT];
      for (unsigned i = 0; i < XP_WAVE_CHIP_COUNT; ++i) {
        chips[i] = roms.waves[i].data();
        sizes[i] = roms.waves[i].size();
      }
      EmuSC::Xp::Device *d = new EmuSC::Xp::Device();
      assert(EmuSC::Xp::device_init_raw(d, roms.control.data(),
                                        roms.control.size(), chips, sizes,
                                        kRate, XP_WRAP_FULL_CARRY));
      bank(d, 81, 0, 0);
      tone_field(d, 1, 0x6a, 100);         /* sensitivity +50 */
      tone_field(d, 1, 0x69, curve);
      midi(d, 0x90, 60, velocity);
      std::vector<float> x(2 * kFrames);
      EmuSC::Xp::device_render(d, x.data(), kFrames);
      EmuSC::Xp::device_destroy(d);
      delete d;
      return 10.0 * std::log10(energy(x));
    };
    double c0 = at_curve(0, 64) - at_curve(0, 127);
    assert(std::fabs((at_curve(2, 64) - at_curve(2, 127)) - c0 -
                     (-48.4 + 11.9)) < 0.5);
    assert(std::fabs((at_curve(4, 64) - at_curve(4, 127)) - c0 -
                     (-2.2 + 11.9)) < 0.5);
  }

  /* The channel mode messages (`M-049`): All Notes Off and the two Omni
     messages release the part's keys exactly as their note-offs do, and
     change nothing under the hold pedal; All Sound Off cuts. */
  {
    Setup noteOff = [](EmuSC::Xp::Device *d) { midi(d, 0x80, 60, 0); };
    std::vector<float> released = two_halves(nothing, noteOff);
    for (uint8_t cc : { (uint8_t)123, (uint8_t)124, (uint8_t)125 })
      assert(released == two_halves(nothing, [cc](EmuSC::Xp::Device *d) {
        midi(d, 0xb0, cc, 0);
      }));
    Setup pedal = [](EmuSC::Xp::Device *d) { midi(d, 0xb0, 64, 127); };
    assert(two_halves(pedal, nothing) ==
           two_halves(pedal, [](EmuSC::Xp::Device *d) {
             midi(d, 0xb0, 123, 0);
           }));
    std::vector<float> cut = two_halves(nothing, [](EmuSC::Xp::Device *d) {
      midi(d, 0xb0, 120, 0);
    });
    /* What follows the cut is the output stage's 10 Hz DC blocker
       settling - one smooth exponential, the same in both channels - so it
       is given 4000 frames, about eight of its time constants, before the
       rest is required to be 100 dB under the first half. */
    double before = 0.0, after = 0.0;
    for (size_t i = 0; i < kFrames; ++i)
      before += (double)cut[i] * cut[i];
    for (size_t i = kFrames + 2 * 4000; i < cut.size(); ++i)
      after += (double)cut[i] * cut[i];
    assert(before > 0.0 && after < before * 1e-10);
  }

  /* CC7 reaches a note already sounding (`closeout/cc7_residual`): a
     change to 64 halfway through a held note drops the second half by the
     law's 40 log10(64/127) = -11.9 dB against the same note left alone. */
  {
    std::vector<float> alone = two_halves(nothing, nothing);
    std::vector<float> ridden = two_halves(nothing, [](EmuSC::Xp::Device *d) {
      midi(d, 0xb0, 7, 64);
    });
    double e0 = 0.0, e1 = 0.0;
    for (size_t i = kFrames + 2 * 256; i < alone.size(); ++i) {
      e0 += (double)alone[i] * alone[i];
      e1 += (double)ridden[i] * ridden[i];
    }
    assert(std::fabs(10.0 * std::log10(e1 / e0) + 11.9) < 0.1);
    /* CC7 0, 1 and 2 are one floor (`M-081`). */
    auto at = [&](uint8_t v) {
      return two_halves(nothing, [v](EmuSC::Xp::Device *d) {
        midi(d, 0xb0, 7, v);
      });
    };
    assert(at(0) == at(2) && at(1) == at(2) && at(2) != at(3));
  }

  /* RPN 0/2 and 0/1 (`M-084`): coarse tune msb - 64 semitones, fine tune
     (msb - 64) * 50/32 cents, on the notes that follow. */
  {
    auto tuned = [](uint8_t lsb, uint8_t msb) {
      return [lsb, msb](EmuSC::Xp::Device *d) {
        bank(d, 81, 0, 0);
        midi(d, 0xb0, 101, 0);
        midi(d, 0xb0, 100, lsb);
        midi(d, 0xb0, 6, msb);
      };
    };
    double base = strongest_hz(render(roms, tuned(2, 64), 0, 60));
    double up = strongest_hz(render(roms, tuned(2, 76), 0, 60));
    assert(std::fabs(1200.0 * std::log2(up / base) - 1200.0) < 5.0);
    double fine = strongest_hz(render(roms, tuned(1, 96), 0, 60));
    assert(std::fabs(1200.0 * std::log2(fine / base) - 50.0) < 5.0);
    /* A data entry of 0 changes nothing. */
    assert(render(roms, tuned(2, 0), 0, 60) == render(roms, tuned(2, 64), 0, 60));
  }

  /* LFO 1 on the amplitude (`M-114`): triangle, key trigger on, rate 64
     (1.1415 Hz), depth +32, whose law is an attenuation of up to twice
     1.253 dB while the waveform is negative. A quarter cycle in, at the
     waveform's top, the level is the unmodulated one; three quarters in,
     at its bottom, it is 2.51 dB down.
     Each against the same note with depth 0. */
  {
    auto lfo_level = [&](uint8_t wire_depth, double at) {
      const uint8_t *chips[XP_WAVE_CHIP_COUNT];
      size_t sizes[XP_WAVE_CHIP_COUNT];
      for (unsigned i = 0; i < XP_WAVE_CHIP_COUNT; ++i) {
        chips[i] = roms.waves[i].data();
        sizes[i] = roms.waves[i].size();
      }
      EmuSC::Xp::Device *d = new EmuSC::Xp::Device();
      assert(EmuSC::Xp::device_init_raw(d, roms.control.data(),
                                        roms.control.size(), chips, sizes,
                                        kRate, XP_WRAP_FULL_CARRY));
      bank(d, 81, 0, 0);
      const uint8_t lfo[8] = { 0, 1, 64, 2, 0, 0, 0, 0 };
      for (unsigned i = 0; i < 8; ++i)
        tone_field(d, 1, (uint8_t)(0x2d + i), lfo[i]);
      tone_field(d, 1, 0x75, wire_depth);
      midi(d, 0x90, 60, 100);
      std::vector<float> x(2 * 25600);
      EmuSC::Xp::device_render(d, x.data(), 25600);     /* 0.8 s */
      EmuSC::Xp::device_destroy(d);
      delete d;
      size_t a = (size_t)(at * kRate), b = a + 32;      /* 1 ms */
      double e = 0.0;
      for (size_t i = 2 * a; i < 2 * b; ++i)
        e += (double)x[i] * x[i];
      return 10.0 * std::log10(e);
    };
    const double period = 1.0 / (0.0494 * std::pow(2.0, 64.0 / 14.13));
    double top = lfo_level(63 + 32, 0.25 * period) - lfo_level(63, 0.25 * period);
    double bottom = lfo_level(63 + 32, 0.75 * period) - lfo_level(63, 0.75 * period);
    assert(std::fabs(top) < 0.1);
    assert(std::fabs(bottom + 2.51) < 0.1);
    /* The whole positive half is left alone (the phase takes of
       2026-09-23): a tenth of a cycle in, the waveform at +0.4, the level
       is still the unmodulated one. */
    double early = lfo_level(63 + 32, 0.1 * period) - lfo_level(63, 0.1 * period);
    assert(std::fabs(early) < 0.1);
  }

  /* The pitch envelope: depth +12 at level +63 is exactly +1200 cents, and
     depth -6 at level +63 is -600; time 0 reaches the level at once. */
  {
    auto penv = [&](uint8_t depthWire) {
      return strongest_hz(render(roms, [depthWire](EmuSC::Xp::Device *d) {
        bank(d, 81, 0, 0);
        tone_field(d, 1, 0x41, depthWire);
        for (uint8_t f = 0x46; f <= 0x49; ++f)
          tone_field(d, 1, f, 0);
        for (uint8_t f = 0x4a; f <= 0x4d; ++f)
          tone_field(d, 1, f, 63 + 63);
      }));
    };
    double still = penv(12);
    assert(std::fabs(1200.0 * std::log2(penv(24) / still) - 1200.0) < 5.0);
    assert(std::fabs(1200.0 * std::log2(penv(6) / still) + 600.0) < 5.0);
  }

  /* The controller matrix (`M-116`), on PR-A 001's one sounding tone with
     its own slots and LFO depths cleared. */
  {
    auto common_field = [](EmuSC::Xp::Device *d, uint8_t field, uint8_t value) {
      uint8_t m[] = { 0xf0, 0x41, 0x10, 0x6a, 0x12, 0x02, 0x00, 0x00, field,
                      value, 0, 0xf7 };
      unsigned sum = 0;
      for (size_t i = 5; i + 2 < sizeof m; ++i)
        sum += m[i];
      m[sizeof m - 2] = (uint8_t)((0x80u - (sum & 0x7fu)) & 0x7fu);
      assert(EmuSC::Xp::device_sysex(d, 0, m, sizeof m));
    };
    auto bench = [common_field](EmuSC::Xp::Device *d) {
      bank(d, 81, 0, 0);
      for (uint8_t f = 0x15; f <= 0x2c; f += 2) {
        tone_field(d, 1, f, 0);
        tone_field(d, 1, (uint8_t)(f + 1), 63);
      }
      for (uint8_t f : { 0x4e, 0x4f, 0x63, 0x64, 0x75, 0x76, 0x7b, 0x7c })
        tone_field(d, 1, f, 63);
      common_field(d, 0x3a, 3);          /* controller 2: MODULATION */
      common_field(d, 0x3b, 2);          /* controller 3: SYS-CTRL2 */
    };
    /* LEV adds d/63 of full scale to the tone level's square law: at tone
       level 32, depth +16 at full CC1 is (0.0635 + 0.254) / 0.0635 = +13.98
       dB, and nothing without CC1. */
    auto lev = [&](uint8_t cc1) {
      return energy(render(roms, [&, cc1](EmuSC::Xp::Device *d) {
        bench(d);
        tone_field(d, 1, 0x65, 32);
        tone_field(d, 1, 0x1d, 4);
        tone_field(d, 1, 0x1e, 63 + 16);
        midi(d, 0xb0, 1, cc1);
      }));
    };
    assert(std::fabs(10.0 * std::log10(lev(127) / lev(0)) - 13.98) < 0.1);
    /* Controller 1 is CC1, fixed: PCH +20 at full CC1 is 0.31 * 400 = 124
       cents, and channel aftertouch does not move it. */
    auto pitch = [&](uint8_t slot, uint8_t status, uint8_t a, uint8_t b) {
      return strongest_hz(render(roms, [&, slot, status, a, b](EmuSC::Xp::Device *d) {
        bench(d);
        tone_field(d, 1, slot, 1);
        tone_field(d, 1, (uint8_t)(slot + 1), 63 + 20);
        midi(d, status, a, b);
      }));
    };
    double still = pitch(0x15, 0xb0, 1, 0);
    assert(std::fabs(1200.0 * std::log2(pitch(0x15, 0xb0, 1, 127) / still) - 124.0) < 5.0);
    assert(std::fabs(1200.0 * std::log2(pitch(0x15, 0xd0, 127, 0) / still)) < 2.0);
    /* SYS-CTRL2 reads CC11 by default; a System DT1 re-points it at CC1. */
    assert(std::fabs(1200.0 * std::log2(pitch(0x25, 0xb0, 11, 127) /
                                        pitch(0x25, 0xb0, 11, 0)) - 124.0) < 5.0);
    double moved = strongest_hz(render(roms, [&](EmuSC::Xp::Device *d) {
      bench(d);
      uint8_t m[] = { 0xf0, 0x41, 0x10, 0x6a, 0x12, 0x00, 0x00, 0x00, 0x13, 1,
                      0, 0xf7 };
      unsigned sum = 0;
      for (size_t i = 5; i + 2 < sizeof m; ++i)
        sum += m[i];
      m[sizeof m - 2] = (uint8_t)((0x80u - (sum & 0x7fu)) & 0x7fu);
      assert(EmuSC::Xp::device_sysex(d, 0, m, sizeof m));
      tone_field(d, 1, 0x25, 1);
      tone_field(d, 1, 0x26, 63 + 20);
      midi(d, 0xb0, 11, 0);
      midi(d, 0xb0, 1, 127);
    }));
    assert(std::fabs(1200.0 * std::log2(moved / still) - 124.0) < 5.0);
    /* A source moving under a sounding note: CC1 to full halfway through
       lifts the second half by the LEV law, against the note left alone. */
    Setup lev32 = [&](EmuSC::Xp::Device *d) {
      bench(d);
      tone_field(d, 1, 0x65, 32);
      tone_field(d, 1, 0x1d, 4);
      tone_field(d, 1, 0x1e, 63 + 16);
    };
    auto held = [&](const Setup &between) {
      const uint8_t *chips[XP_WAVE_CHIP_COUNT];
      size_t sizes[XP_WAVE_CHIP_COUNT];
      for (unsigned i = 0; i < XP_WAVE_CHIP_COUNT; ++i) {
        chips[i] = roms.waves[i].data();
        sizes[i] = roms.waves[i].size();
      }
      EmuSC::Xp::Device *d = new EmuSC::Xp::Device();
      assert(EmuSC::Xp::device_init_raw(d, roms.control.data(),
                                        roms.control.size(), chips, sizes,
                                        kRate, XP_WRAP_FULL_CARRY));
      lev32(d);
      midi(d, 0x90, 60, 100);
      std::vector<float> x(2 * kFrames);
      EmuSC::Xp::device_render(d, x.data(), kFrames / 2);
      between(d);
      EmuSC::Xp::device_render(d, x.data() + kFrames, kFrames / 2);
      EmuSC::Xp::device_destroy(d);
      delete d;
      double e = 0.0;
      for (size_t i = kFrames + 2 * 256; i < x.size(); ++i)
        e += (double)x[i] * x[i];
      return e;
    };
    double alone = held([](EmuSC::Xp::Device *) {});
    double lifted = held([](EmuSC::Xp::Device *d) { midi(d, 0xb0, 1, 127); });
    assert(std::fabs(10.0 * std::log10(lifted / alone) - 13.98) < 0.1);
  }

  /* A part whose record names PR-B: a bare program change lands in PR-B,
     exactly as an explicit CC0 81 / CC32 1 does, and not in PR-A. */
  std::vector<float> prb69 = render(roms, [](EmuSC::Xp::Device *d) {
    part_record(d, 0, 0, 4, 0);
    midi(d, 0xc0, 69);
  });
  std::vector<float> prb69_explicit = render(roms, [](EmuSC::Xp::Device *d) {
    part_record(d, 0, 0, 3, 0);
    bank(d, 81, 1, 69);
  });
  std::vector<float> pra69 = render(roms, [](EmuSC::Xp::Device *d) {
    part_record(d, 0, 0, 4, 0);
    bank(d, 81, 0, 69);
  });
  assert(energy(prb69) > 0.0 && energy(pra69) > 0.0);
  assert(prb69 == prb69_explicit);
  assert(prb69 != pra69);

  /* A latch that names no group defers to the record. */
  assert(prb69 == render(roms, [](EmuSC::Xp::Device *d) {
    part_record(d, 0, 0, 4, 0);
    bank(d, 0, 0, 69);
  }));

  /* A CC32 alone moves only the LSB of the latch the record seeded, so
     81 from PR-B plus a new LSB of 2 is PR-C. */
  assert(render(roms, [](EmuSC::Xp::Device *d) {
    part_record(d, 0, 0, 4, 0);
    midi(d, 0xb0, 32, 2);
    midi(d, 0xc0, 69);
  }) == render(roms, [](EmuSC::Xp::Device *d) {
    bank(d, 81, 2, 69);
  }));

  /* A pair that names a group with no image here is rejected, leaving the
     loaded patch alone rather than falling back to the record. */
  std::vector<float> prb5 = render(roms, [](EmuSC::Xp::Device *d) {
    part_record(d, 0, 0, 4, 0);
    midi(d, 0xc0, 5);
  });
  assert(prb5 == render(roms, [](EmuSC::Xp::Device *d) {
    part_record(d, 0, 0, 4, 0);
    midi(d, 0xc0, 5);
    bank(d, 82, 0, 69);           /* CARD */
  }));

  /* A record whose type names an expansion board resolves to nothing, so
     once the latch names no group either, the program change loads
     nothing and the part keeps its power-on patch. */
  assert(boot == render(roms, [](EmuSC::Xp::Device *d) {
    part_record(d, 0, 2, 1, 0);
    bank(d, 0, 0, 69);
  }));

  /* With no performance written, every record is zero: type 0 id 0, which
     the device's own table resolves to USER. */
  std::vector<float> fresh3 = render(roms, [](EmuSC::Xp::Device *d) {
    midi(d, 0xc0, 3);
  });
  assert(fresh3 == render(roms, [](EmuSC::Xp::Device *d) {
    bank(d, 80, 0, 3);
  }));
  assert(fresh3 != render(roms, [](EmuSC::Xp::Device *d) {
    bank(d, 81, 0, 3);
  }));

  /* GM System On: every part plays GM 001 on its own channel, whatever
     bank is then selected; part 10 has the GM drum set; the host's GM
     reset and the message do the same thing; and the part's CC7 is the
     mode's own. */
  Setup gm = [](EmuSC::Xp::Device *d) {
    assert(EmuSC::Xp::device_sysex(d, 0, kGmOn, sizeof kGmOn));
  };
  std::vector<float> gm1 = render(roms, gm);
  assert(energy(gm1) > 0.0);
  assert(gm1 == render(roms, [](EmuSC::Xp::Device *d) {
    assert(EmuSC::Xp::device_gm_system_on(d));
  }));
  assert(gm1 == render(roms, [&](EmuSC::Xp::Device *d) {
    gm(d);
    bank(d, 81, 0, 0);            /* forced to the GM bank */
  }));
  assert(gm1 != render(roms, [&](EmuSC::Xp::Device *d) {
    gm(d);
    midi(d, 0xc0, 1);             /* GM 002 is not GM 001 */
  }));
  assert(gm1 == render(roms, [&](EmuSC::Xp::Device *d) {
    gm(d);
    midi(d, 0xb0, 7, 100);
  }));
  assert(render(roms, gm, 5) == gm1);   /* part 6, on channel 6 */
  assert(energy(render(roms, gm, 9, 36)) > 0.0);
  assert(energy(render(roms, none, 9, 36)) == 0.0);

  /* Only the broadcast ID is received. */
  {
    const uint8_t unit[] = { 0xf0, 0x7e, 0x10, 0x09, 0x01, 0xf7 };
    assert(boot == render(roms, [&](EmuSC::Xp::Device *d) {
      assert(!EmuSC::Xp::device_sysex(d, 0, unit, sizeof unit));
    }));
  }

  /* A reset leaves GM mode: a bank select reaches PR-A again. */
  assert(render(roms, [&](EmuSC::Xp::Device *d) {
    gm(d);
    EmuSC::Xp::device_reset_controllers(d);
    bank(d, 81, 0, 69);
  }) == render(roms, [](EmuSC::Xp::Device *d) {
    bank(d, 81, 0, 69);
  }));

  /* A reversed element sounds from the note-on (`M-090`). INT-B wave 124
     `REV Orch.Hit` holds the same span as wave 59 `Orch. Hit` with the
     reverse flag set, so on PR-A 001's tone 2 alone, filter off and
     envelope flat, the forward one starts on its attack and falls, and the
     reversed one starts on the sample's tail and rises. */
  {
    auto wave = [](uint8_t number) {
      return [number](EmuSC::Xp::Device *d) {
        bank(d, 81, 0, 0);
        tone_field(d, 1, 0x01, 0);
        tone_field(d, 1, 0x02, 2);
        tone_wave_number(d, 1, number);
        for (unsigned t : {0u, 2u, 3u})
          tone_field(d, t, 0x00, 0);        /* only tone 2 sounds */
        common_field(d, 0x44, 0);           /* structure 1-2: type 1 */
        tone_field(d, 1, 0x50, 0);          /* filter off */
        tone_field(d, 1, 0x6e, 0);          /* A-ENV T1 0, L1-L3 127 */
        for (uint8_t f = 0x72; f <= 0x74; ++f)
          tone_field(d, 1, f, 127);
      };
    };
    auto halves = [](const std::vector<float> &x, double *early,
                     double *late) {
      *early = energy(std::vector<float>(x.begin(), x.begin() + kFrames));
      *late = energy(std::vector<float>(x.begin() + kFrames, x.end()));
    };
    double fe, fl, re, rl;
    halves(render(roms, wave(59)), &fe, &fl);
    halves(render(roms, wave(124)), &re, &rl);
    assert(re > 0.0);
    assert(fe > fl);
    assert(rl > 4.0 * re);
  }

  /* With a part as the effect source, the effect's output block - assign,
     level and the two sends - is that part's patch's, as its type and
     parameters are (`0x0A00931C`). The performance's own sends then do
     nothing, and the patch's do. Part 1 on PR-A 001, assigned to the
     insert, which runs DISTORTION; chorus and reverb returns full. */
  auto efxSends = [](uint8_t performance, uint8_t patch) {
    return [performance, patch](EmuSC::Xp::Device *d) {
      bank(d, 81, 0, 0);
      const uint8_t record[] = { 0x01, 0x00, 0x10, 0x0a };
      dt1(d, record, { 1 });
      const uint8_t common[] = { 0x01, 0x00, 0x00, 0x0c };
      dt1(d, common, { 1, 2, 127, 64, 2, 15, 15, 127, 0, 0, 0, 0, 0, 0,
                       0, 127, performance, performance, 0, 64, 0, 64,
                       127, 3, 97, 0, 0, 0, 2, 127, 41, 15, 0 });
      const uint8_t own[] = { 0x02, 0x00, 0x00, 0x0c };
      dt1(d, own, { 2, 127, 64, 2, 15, 15, 127, 0, 0, 0, 0, 0, 0,
                    0, 127, patch, patch });
    };
  };
  {
    std::vector<float> dry = render(roms, efxSends(0, 0));
    assert(energy(dry) > 0.0);
    assert(render(roms, efxSends(127, 0)) == dry);
    assert(render(roms, efxSends(0, 127)) != dry);
  }

  /* EFX:Output Level scales the insert's return to the mix (CRAM 246 and
     249). At 0 its coefficient is exactly 0, so with both sends also at 0
     a part that plays only through the insert is silent. */
  auto efxLevel = [](uint8_t level) {
    return [level](EmuSC::Xp::Device *d) {
      bank(d, 81, 0, 0);
      const uint8_t record[] = { 0x01, 0x00, 0x10, 0x0a };
      dt1(d, record, { 1 });
      const uint8_t common[] = { 0x01, 0x00, 0x00, 0x0c };
      dt1(d, common, { 1, 2, 127, 64, 2, 15, 15, 127, 0, 0, 0, 0, 0, 0,
                       0, 127, 0, 0 });
      const uint8_t own[] = { 0x02, 0x00, 0x00, 0x0c };
      dt1(d, own, { 2, 127, 64, 2, 15, 15, 127, 0, 0, 0, 0, 0, 0,
                    0, level, 0, 0 });
    };
  };
  {
    std::vector<float> full = render(roms, efxLevel(127));
    assert(energy(full) > 0.0);
    std::vector<float> half = render(roms, efxLevel(64));
    assert(energy(half) > 0.0 && energy(half) < energy(full));
    assert(energy(render(roms, efxLevel(0))) == 0.0);
  }

  /* A program change on the effect's source part reloads the effect from
     the patch it loads (`0x0A007D78` posts the DSP task's reload). Part 1
     is the source; its own block is rewritten to DISTORTION, and a
     program change back to PR-A 001 must restore PR-A 001's own effect,
     as if the rewrite had never happened. The same change on part 2,
     which is not the source, leaves the rewritten effect in force. */
  {
    auto setup = [](bool rewrite, int change) {
      return [rewrite, change](EmuSC::Xp::Device *d) {
        bank(d, 81, 0, 0);
        const uint8_t record[] = { 0x01, 0x00, 0x10, 0x0a };
        dt1(d, record, { 1 });
        const uint8_t common[] = { 0x01, 0x00, 0x00, 0x0c };
        dt1(d, common, { 1 });
        if (rewrite) {
          const uint8_t own[] = { 0x02, 0x00, 0x00, 0x0c };
          dt1(d, own, { 2, 127, 64, 2, 15, 15, 127, 0, 0, 0, 0, 0, 0 });
        }
        if (change >= 0)
          bank(d, 81, 0, 0, (uint8_t)change);
      };
    };
    std::vector<float> own = render(roms, setup(false, -1));
    std::vector<float> rewritten = render(roms, setup(true, -1));
    assert(energy(own) > 0.0);
    assert(rewritten != own);
    assert(render(roms, setup(true, 0)) == own);
    assert(render(roms, setup(true, 1)) == rewritten);
  }

  /* The voice reserve decides the steal victim (`M-072`'s three takes).
     Part 2 takes 32 keys first, part 1 then 32 more filling the pool, and
     part 1 asks for `extra` beyond it; PR-A 001 sounds one tone a key
     over this range.
     Part 1 is turned down by CC7 0, over 80 dB, so the render is part 2
     to within that: whatever part 2 keeps is what is heard. */
  {
    auto steal = [&roms](uint8_t reserve1, uint8_t reserve2, int extra) {
      const uint8_t *chips[XP_WAVE_CHIP_COUNT];
      size_t sizes[XP_WAVE_CHIP_COUNT];
      for (unsigned i = 0; i < XP_WAVE_CHIP_COUNT; ++i) {
        chips[i] = roms.waves[i].data();
        sizes[i] = roms.waves[i].size();
      }
      EmuSC::Xp::Device *d = new EmuSC::Xp::Device();
      assert(EmuSC::Xp::device_init_raw(d, roms.control.data(),
                                        roms.control.size(), chips, sizes,
                                        kRate, XP_WRAP_FULL_CARRY));
      part_record(d, 0, 0, 3, 0);
      part_record(d, 1, 0, 3, 0);
      bank(d, 81, 0, 0, 0);
      bank(d, 81, 0, 0, 1);
      const uint8_t reserves[] = { 0x01, 0x00, 0x00, 0x30 };
      dt1(d, reserves, { reserve1, reserve2, 0, 0, 0, 0, 0, 0,
                         0, 0, 0, 0, 0, 0, 0, 0 });
      midi(d, 0xb0, 7, 0);
      for (uint8_t k = 40; k < 72; ++k)
        midi(d, 0x91, k, 100);
      for (int k = 36; k < 68 + extra; ++k)
        midi(d, 0x90, (uint8_t)k, 100);
      std::vector<float> out(2 * kFrames);
      EmuSC::Xp::device_render(d, out.data(), kFrames);
      EmuSC::Xp::device_destroy(d);
      delete d;
      return out;
    };
    std::vector<float> alone = steal(0, 32, 0);
    assert(energy(alone) > 0.0);
    /* Reserves 0/32: part 2 holds exactly its reserve and is exempt, so
       part 1 loses its own oldest and part 2 is untouched. */
    auto residue = [](const std::vector<float> &a,
                      const std::vector<float> &b) {
      double e = 0.0;
      for (size_t i = 0; i < a.size(); ++i)
        e += ((double)a[i] - b[i]) * ((double)a[i] - b[i]);
      return e / energy(b);
    };
    assert(residue(steal(0, 32, 8), alone) < 1e-6);
    /* Reserves 0/0: part 2's are the oldest voices and go. */
    std::vector<float> stolen = steal(0, 0, 8);
    assert(residue(stolen, alone) > 0.05);
    /* Reserves 32/0: part 1 holds 40 against 32, over its floor and
       unprotected, so part 2 loses the same eight as at 0/0. */
    assert(steal(32, 0, 8) == stolen);
  }

  printf("ok\n");
  return 0;
}
