/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_XP_WAVE_CACHE_H
#define EMUSC_XP_WAVE_CACHE_H

#include "wave.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded by entry count, not by bytes: a dense passage rarely has more
   distinct hot samples than this, and a referenced entry is never
   displaced, so a miss with every entry referenced decodes into a private
   buffer instead of growing the table. */
#define XP_WAVE_CACHE_ENTRIES 32

struct xp_wave_cache_entry {
  /* Null marks an empty entry. */
  int32_t *pcm;
  /* The key: which bank's bytes, and the exact address extent
     fce_decode_storage decodes - from the 16-sample block holding
     address_a through address_c inclusive. The output depends on nothing
     else, so two descriptors with the same key decode identically. */
  const uint8_t *bank;
  uint32_t base;
  uint32_t last;
  size_t count;
  /* Live references handed out by wave_cache_acquire and not yet given
     back through wave_cache_release. */
  unsigned refs;
  uint64_t last_use;
};

struct xp_wave_cache {
  struct xp_wave_cache_entry entries[XP_WAVE_CACHE_ENTRIES];
  uint64_t clock;
};

#ifdef __cplusplus
}

namespace EmuSC { namespace Xp {

// Decoded-PCM cache for the XP voice path. FCE decoding is a pure function
// of the bank's bytes and the decoded extent, so a cached decode is the
// same samples a fresh one would produce. Not thread-safe: one cache
// belongs to one renderer and is used from the thread that renders it.

/* Hand out decoded PCM for `desc`, from the cache or freshly decoded.
 * On success `*pcm` holds `*count` samples starting at wave address
 * `*base`, and stays valid and unchanged until exactly one matching
 * wave_cache_release(cache, *pcm). A null `cache` decodes into a private
 * buffer every time, which release then frees. */
bool wave_cache_acquire(struct xp_wave_cache *cache,
                         const struct XpDeviceProfile *profile,
                         const uint8_t *bank, size_t bankSize,
                         const struct xp_wave_descriptor *desc,
                         const int32_t **pcm, uint32_t *base,
                         size_t *count);

/* Give back one reference from wave_cache_acquire. Null is a no-op. */
void wave_cache_release(struct xp_wave_cache *cache, const int32_t *pcm);

/* Free every entry. Only valid once every acquired reference has been
 * released; the cache is empty and reusable afterwards. */
void wave_cache_clear(struct xp_wave_cache *cache);

}}  // namespace EmuSC::Xp
#endif

#endif
