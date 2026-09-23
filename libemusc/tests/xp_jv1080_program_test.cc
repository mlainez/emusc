/* Where a JV-1080 program change lands, and what GM System On leaves.
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

#include <cassert>
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

const uint8_t kGmOn[] = { 0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7 };

}  // namespace

int main(void)
{
  Roms roms;
  if (!load_roms(&roms)) {
    printf("JV1080_CONTROL_ROM / JV1080_WAVE_ROMS unset - skipping\n");
    return SKIP;
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
     nothing and the part stays silent. */
  assert(energy(render(roms, [](EmuSC::Xp::Device *d) {
    part_record(d, 0, 2, 1, 0);
    bank(d, 0, 0, 69);
  })) == 0.0);

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
  assert(energy(render(roms, [](EmuSC::Xp::Device *) {}, 9, 36)) == 0.0);

  /* Only the broadcast ID is received. */
  {
    const uint8_t unit[] = { 0xf0, 0x7e, 0x10, 0x09, 0x01, 0xf7 };
    assert(energy(render(roms, [&](EmuSC::Xp::Device *d) {
      assert(!EmuSC::Xp::device_sysex(d, 0, unit, sizeof unit));
    })) == 0.0);
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
