/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * This file consists entirely of AI-generated code without direct human
 * authorship and is dedicated to the public domain under CC0 1.0.
 */
#include "random.h"

namespace EmuSC
{
namespace GpRandom
{

namespace
{

// glibc TYPE_3 (random_r): a 31-word table read by two cursors SEP apart;
// each step adds the rear word into the front word (mod 2^32) and returns the
// sum >> 1. The seeding recurrence and the 310 discarded outputs are part of
// the algorithm; without them seed(s) would not reproduce glibc's srand(s).
constexpr int DEG = 31;
constexpr int SEP = 3;
constexpr int DISCARD = 310;

struct State {
  uint32_t r[DEG];
  int front;
  int rear;
  bool seeded;
};

State state = { {}, SEP, 0, false };

uint32_t step(void)
{
  state.r[state.front] += state.r[state.rear];
  const uint32_t v = state.r[state.front];
  state.front = (state.front + 1) % DEG;
  state.rear = (state.rear + 1) % DEG;
  return v;
}

}  // namespace


void seed(uint32_t s)
{
  // glibc maps seed 0 to 1 and runs the recurrence on the seed as an int32_t,
  // so a seed of 2^31 or more enters it negative.
  int32_t word = (s == 0) ? 1 : (int32_t) s;
  state.r[0] = (uint32_t) word;
  for (int i = 1; i < DEG; i++) {
    // 16807 * word mod (2^31 - 1), by Schrage's method as glibc computes it.
    const int32_t hi = word / 127773;
    const int32_t lo = word % 127773;
    word = 16807 * lo - 2836 * hi;
    if (word < 0)
      word += 2147483647;
    state.r[i] = (uint32_t) word;
  }
  state.front = SEP;
  state.rear = 0;
  state.seeded = true;
  for (int i = 0; i < DISCARD; i++)
    step();
}


int32_t next(void)
{
  // Unseeded, glibc behaves as though seeded with 1.
  if (!state.seeded)
    seed(1);
  return (int32_t) (step() >> 1);
}

}
}
