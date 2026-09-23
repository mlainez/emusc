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

  printf("ok\n");
  return 0;
}
