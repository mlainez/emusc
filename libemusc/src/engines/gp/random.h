/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * This file consists entirely of AI-generated code without direct human
 * authorship and is dedicated to the public domain under CC0 1.0.
 *
 *  The GP engine's random source: random pan, random pitch, Analog Feel and
 *  the sample-and-hold / random LFO waveforms.
 *
 *  The C library's rand() cannot serve here. Its algorithm and range are
 *  implementation-defined: glibc returns 31-bit values from an additive
 *  feedback generator, while msvcrt (every Windows build) returns 15-bit values
 *  (RAND_MAX 0x7fff) from a linear congruential one. The engine masks its draws
 *  as 16- and 17-bit words, so msvcrt would leave their upper bits always zero -
 *  random pitch only ever sharp, the S&H LFO held to one half of its range -
 *  and its different sequence makes the same render diverge audibly between
 *  platforms from the first note whose sound depends on a draw.
 *
 *  This is glibc's TYPE_3 random() algorithm, reproduced exactly, so every
 *  platform produces the renders glibc's rand() does. State is process-wide,
 *  like rand()'s, and Synth::seed_random() sets it.
 */
#ifndef __GP_RANDOM_H__
#define __GP_RANDOM_H__

#include <cstdint>

namespace EmuSC
{
namespace GpRandom
{

// Largest value next() returns; every value in [0, MAX] is possible.
constexpr int32_t MAX = 0x7fffffff;

void seed(uint32_t seed);
int32_t next(void);

}
}

#endif  // __GP_RANDOM_H__
