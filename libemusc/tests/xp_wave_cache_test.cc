/* SPDX-License-Identifier: CC0-1.0 */
#include "engines/xp/wave_cache.h"

#include <cassert>
#ifdef NDEBUG
#error "this test is assertion-driven; NDEBUG compiles it away"
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace EmuSC::Xp;

static struct xp_wave_descriptor extent(uint32_t first, uint32_t last)
{
  struct xp_wave_descriptor desc;
  std::memset(&desc, 0, sizeof desc);
  desc.address_a = first;
  desc.address_c = last;
  return desc;
}

/* The samples a fresh decode of `desc` gives, compared against `pcm`. */
static bool matches_fresh(const uint8_t *bank, const struct xp_wave_descriptor *desc,
                          const int32_t *pcm, uint32_t base, size_t count)
{
  int32_t *fresh = (int32_t *)std::malloc(count * sizeof *fresh);
  uint32_t freshBase;
  size_t freshCount;
  assert(fresh);
  bool ok = fce_decode_storage(&SC88_PROFILE, bank, SC88_PROFILE.waveBankSize,
                                desc, fresh, count, &freshBase, &freshCount) &&
    freshBase == base && freshCount == count &&
    std::memcmp(fresh, pcm, count * sizeof *fresh) == 0;
  std::free(fresh);
  return ok;
}

/* Key i: a distinct extent per index, well inside the bank. */
static struct xp_wave_descriptor key(unsigned i)
{
  return extent(0x10000u + i * 0x400u, 0x10000u + i * 0x400u + 0x1ffu);
}

int main()
{
  uint8_t *bank = (uint8_t *)std::malloc(SC88_PROFILE.waveBankSize);
  struct xp_wave_cache cache;
  const int32_t *pcm;
  const int32_t *again;
  uint32_t base;
  size_t count;

  assert(bank);
  for (size_t i = 0; i < SC88_PROFILE.waveBankSize; ++i)
    bank[i] = (uint8_t)((i * 2654435761u) >> 13);
  std::memset(&cache, 0, sizeof cache);

  /* A repeat of the same extent is the same buffer, and it holds exactly
     what a fresh decode produces. */
  struct xp_wave_descriptor a = extent(0x2345, 0x2800);
  assert(wave_cache_acquire(&cache, &SC88_PROFILE, bank,
                            SC88_PROFILE.waveBankSize, &a, &pcm, &base,
                            &count));
  assert(base == 0x2340 && count == 0x2800 - 0x2340 + 1);
  assert(matches_fresh(bank, &a, pcm, base, count));
  assert(wave_cache_acquire(&cache, &SC88_PROFILE, bank,
                            SC88_PROFILE.waveBankSize, &a, &again, &base,
                            &count));
  assert(again == pcm);

  /* Another start in the same 16-sample block decodes the same extent, so
     it shares the entry and the samples still match its own decode. */
  struct xp_wave_descriptor sameBlock = extent(0x234f, 0x2800);
  assert(wave_cache_acquire(&cache, &SC88_PROFILE, bank,
                            SC88_PROFILE.waveBankSize, &sameBlock, &again,
                            &base, &count));
  assert(again == pcm);
  assert(matches_fresh(bank, &sameBlock, again, base, count));
  /* A different end is a different entry. */
  struct xp_wave_descriptor longer = extent(0x2345, 0x2801);
  assert(wave_cache_acquire(&cache, &SC88_PROFILE, bank,
                            SC88_PROFILE.waveBankSize, &longer, &again,
                            &base, &count));
  assert(again != pcm && count == 0x2801 - 0x2340 + 1);
  wave_cache_release(&cache, again);

  /* A descriptor the decoder refuses is refused even though an entry with
     the same block and end exists. */
  struct xp_wave_descriptor small = extent(0x3000, 0x3003);
  struct xp_wave_descriptor inverted = extent(0x3008, 0x3003);
  assert(wave_cache_acquire(&cache, &SC88_PROFILE, bank,
                            SC88_PROFILE.waveBankSize, &small, &again,
                            &base, &count));
  wave_cache_release(&cache, again);
  assert(!wave_cache_acquire(&cache, &SC88_PROFILE, bank,
                             SC88_PROFILE.waveBankSize, &inverted, &again,
                             &base, &count));
  assert(again == nullptr);

  wave_cache_release(&cache, pcm);
  wave_cache_release(&cache, pcm);
  wave_cache_release(&cache, pcm);
  wave_cache_clear(&cache);

  /* Every entry referenced: a miss must not displace any of them. It gets
     a private buffer, and every held buffer keeps its samples. */
  const int32_t *held[XP_WAVE_CACHE_ENTRIES];
  uint32_t heldBase[XP_WAVE_CACHE_ENTRIES];
  size_t heldCount[XP_WAVE_CACHE_ENTRIES];
  for (unsigned i = 0; i < XP_WAVE_CACHE_ENTRIES; ++i) {
    struct xp_wave_descriptor d = key(i);
    assert(wave_cache_acquire(&cache, &SC88_PROFILE, bank,
                              SC88_PROFILE.waveBankSize, &d, &held[i],
                              &heldBase[i], &heldCount[i]));
  }
  struct xp_wave_descriptor extra = key(XP_WAVE_CACHE_ENTRIES);
  const int32_t *priv;
  const int32_t *priv2;
  assert(wave_cache_acquire(&cache, &SC88_PROFILE, bank,
                            SC88_PROFILE.waveBankSize, &extra, &priv, &base,
                            &count));
  assert(matches_fresh(bank, &extra, priv, base, count));
  /* Private buffers are not entries: the same extent again is another
     private buffer, not a hit on the first. */
  assert(wave_cache_acquire(&cache, &SC88_PROFILE, bank,
                            SC88_PROFILE.waveBankSize, &extra, &priv2, &base,
                            &count));
  assert(priv2 != priv);
  for (unsigned i = 0; i < XP_WAVE_CACHE_ENTRIES; ++i) {
    struct xp_wave_descriptor d = key(i);
    assert(cache.entries[i].pcm == held[i] && cache.entries[i].refs == 1);
    assert(matches_fresh(bank, &d, held[i], heldBase[i], heldCount[i]));
  }
  wave_cache_release(&cache, priv);
  wave_cache_release(&cache, priv2);

  /* A second holder of one entry keeps it through the first holder's
     release, and the least recently used unreferenced entry is the one a
     miss replaces. Keys 3 and 7 go unreferenced, 3 used first. */
  {
    struct xp_wave_descriptor d5 = key(5);
    assert(wave_cache_acquire(&cache, &SC88_PROFILE, bank,
                              SC88_PROFILE.waveBankSize, &d5, &again, &base,
                              &count));
    assert(again == held[5] && cache.entries[5].refs == 2);
    wave_cache_release(&cache, held[5]);
    assert(cache.entries[5].refs == 1);
  }
  wave_cache_release(&cache, held[3]);
  wave_cache_release(&cache, held[7]);
  const int32_t *newcomer;
  assert(wave_cache_acquire(&cache, &SC88_PROFILE, bank,
                            SC88_PROFILE.waveBankSize, &extra, &newcomer,
                            &base, &count));
  assert(cache.entries[3].pcm == newcomer && cache.entries[3].refs == 1);
  assert(cache.entries[7].pcm == held[7] && cache.entries[7].refs == 0);
  {
    struct xp_wave_descriptor d7 = key(7);
    assert(wave_cache_acquire(&cache, &SC88_PROFILE, bank,
                              SC88_PROFILE.waveBankSize, &d7, &again, &base,
                              &count));
    assert(again == held[7]);
  }
  for (unsigned i = 0; i < XP_WAVE_CACHE_ENTRIES; ++i) {
    struct xp_wave_descriptor d = key(i);
    if (i != 3)
      assert(matches_fresh(bank, &d, held[i], heldBase[i], heldCount[i]));
  }
  wave_cache_release(&cache, newcomer);
  for (unsigned i = 0; i < XP_WAVE_CACHE_ENTRIES; ++i)
    if (i != 3)
      wave_cache_release(&cache, held[i]);
  for (unsigned i = 0; i < XP_WAVE_CACHE_ENTRIES; ++i)
    assert(cache.entries[i].refs == 0);
  wave_cache_clear(&cache);
  for (unsigned i = 0; i < XP_WAVE_CACHE_ENTRIES; ++i)
    assert(cache.entries[i].pcm == nullptr);

  /* Without a cache every acquire is a private decode. */
  assert(wave_cache_acquire(nullptr, &SC88_PROFILE, bank,
                            SC88_PROFILE.waveBankSize, &a, &priv, &base,
                            &count));
  assert(matches_fresh(bank, &a, priv, base, count));
  wave_cache_release(nullptr, priv);

  std::free(bank);
  std::puts("xp_wave_cache_test: ok");
  return 0;
}
