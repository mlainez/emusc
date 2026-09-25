/* SPDX-License-Identifier: CC0-1.0 */

/* GpRandom must give the same sequence on every platform, and that sequence
 * is glibc's rand(). The expected values below were produced by glibc's
 * srand()/rand(); this test runs on every target, including Windows, where the
 * C library's own rand() gives entirely different, 15-bit values.
 */
#include "engines/gp/random.h"

#include <cassert>
#ifdef NDEBUG
#error "this test is assertion-driven; NDEBUG compiles it away"
#endif
#include <cstdint>

using namespace EmuSC;

namespace {

struct Expected {
  uint32_t seed;
  int32_t first[6];
  int32_t at9999;
};

const Expected EXPECTED[] = {
  { 1u,
    { 1804289383, 846930886, 1681692777, 1714636915, 1957747793, 424238335 },
    1908609430 },
  { 12345u,
    { 383100999, 858300821, 357768173, 455528251, 133005921, 116285904 },
    468472226 },
  { 2147483648u,
    { 1336741213, 1210407648, 1447044896, 337392383, 82502902, 538660432 },
    30485069 },
};

void check_sequence(const Expected &e)
{
  GpRandom::seed(e.seed);
  for (int i = 0; i < 6; i++)
    assert(GpRandom::next() == e.first[i]);
  for (int i = 6; i < 9999; i++)
    GpRandom::next();
  assert(GpRandom::next() == e.at9999);
}

}  // namespace

int main()
{
  // Unseeded is seed 1, as for rand().
  assert(GpRandom::next() == EXPECTED[0].first[0]);

  for (const Expected &e : EXPECTED)
    check_sequence(e);

  // Seed 0 is seed 1, as for srand().
  GpRandom::seed(0);
  assert(GpRandom::next() == EXPECTED[0].first[0]);

  // The engine masks draws to 16 and 17 bits and takes their sign from bit 15
  // or bit 16, so those bits must actually vary; a 15-bit source never sets
  // them.
  GpRandom::seed(1);
  int32_t ored = 0, anded = GpRandom::MAX;
  for (int i = 0; i < 1000; i++) {
    const int32_t v = GpRandom::next();
    assert(v >= 0 && v <= GpRandom::MAX);
    ored |= v;
    anded &= v;
  }
  assert(ored == GpRandom::MAX);
  assert(anded == 0);

  return 0;
}
