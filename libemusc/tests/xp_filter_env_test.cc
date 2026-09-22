/* SPDX-License-Identifier: CC0-1.0 */
/*
 *  The JV-1080's filter envelope, against the takes it was read from.
 *
 *  None of this needs a ROM: what it checks is arithmetic over the measured
 *  tables, so it runs unskipped and a regression in the laws cannot hide
 *  behind a missing image.
 */
#include "engines/xp/devices/jv1080.h"

#include <cassert>
#ifdef NDEBUG
#error "this test is assertion-driven; NDEBUG compiles it away"
#endif
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace EmuSC::Xp;

static bool close_to(double a, double b, double tolerance)
{
  return std::fabs(a - b) <= tolerance;
}

int main()
{
  const struct XpVoiceFieldMap &tone = JV1080_PROFILE.toneFields;
  const struct XpVoiceFieldMap &rhythm = JV1080_PROFILE.rhythmNoteFields;

  /* The field map is the schema's. A wrong index here reads a neighbouring
     parameter and the envelope silently becomes something else, so the
     offsets are asserted rather than trusted (`partial_schema.md`,
     `rhythm_schema.md`). */
  assert(tone.filterEnvDepth == 0x55u);
  assert(tone.filterEnvVelCurve == 0x56u);
  assert(tone.filterEnvVelSens == 0x57u);
  assert(tone.filterEnvVelTime1 == 0x58u);
  assert(tone.filterEnvVelTime4 == 0x59u);
  assert(tone.filterEnvTimeKeyFollow == 0x5au);
  assert(tone.filterEnvTime1 == 0x5bu);
  assert(tone.filterEnvLevel1 == 0x5fu);
  assert(rhythm.filterEnvDepth == 0x1eu);
  assert(rhythm.filterEnvVelSens == 0x1fu);
  assert(rhythm.filterEnvVelTime1 == 0x20u);
  assert(rhythm.filterEnvTime1 == 0x21u);
  assert(rhythm.filterEnvLevel1 == 0x25u);
  /* A rhythm note has neither of these, and reading one would read a
     neighbour. */
  assert(rhythm.filterEnvVelCurve == XP_VOICE_FIELD_NONE);
  assert(rhythm.filterEnvVelTime4 == XP_VOICE_FIELD_NONE);
  assert(rhythm.filterEnvTimeKeyFollow == XP_VOICE_FIELD_NONE);

  /* MEASURED (`M-082`): every curve runs from 0 at velocity 1 to 1 at 127,
     and at velocity 64 they read 0.443, 0.144, 0.000, 0.718, 0.849, 0.452
     and 0.441 - the numbers that separate the seven characters. */
  static const double kAtVelocity64[7] = {
    0.443, 0.144, 0.000, 0.718, 0.849, 0.452, 0.441,
  };
  for (unsigned c = 0; c < 7u; ++c) {
    assert(jv1080_filter_env_curve(c, 1u) == 0.0);
    assert(jv1080_filter_env_curve(c, 127u) == 1.0);
    assert(close_to(jv1080_filter_env_curve(c, 64u), kAtVelocity64[c], 1e-9));
    /* All seven are monotonic in velocity. */
    double previous = -1.0;
    for (unsigned v = 1u; v <= 127u; ++v) {
      double f = jv1080_filter_env_curve(c, v);
      assert(f >= previous - 1e-12);
      previous = f;
    }
  }

  /* MEASURED (`M-082`): curve 0 is linear in velocity above 16 - its
     successive differences over 0.024, 0.164, 0.301, 0.443, 0.579, 0.721,
     0.864, 1.000 run 0.136 to 0.143 - so the table's own interpolation
     between those points must not put a kink in it. */
  for (unsigned v = 32u; v <= 96u; v += 16u) {
    double slope = (jv1080_filter_env_curve(0u, v + 8u) -
                    jv1080_filter_env_curve(0u, v - 8u)) / 16.0;
    assert(close_to(slope, 0.00872, 0.0006));
  }

  /* THE ONE CALIBRATION POINT (`M-082`): cutoff 24, F-ENV depth +30,
     velocity sensitivity 74, curve 0, at velocity 127 the envelope's whole
     travel is 6.80 octaves - 68 cutoff units at `M-012`'s 10.0 steps per
     octave. The depth-to-cutoff-unit scale is NOT recovered (jv1080_voice.cc
     bounds it 2.1 to 2.5); what this pins is that the model reproduces the
     take it was calibrated on. */
  uint8_t record[XP_JV1080_TONE_FIELDS];
  std::memset(record, 0, sizeof record);
  record[tone.filterEnvDepth] = (uint8_t)(int8_t)30;
  record[tone.filterEnvVelSens] = (uint8_t)(int8_t)74;
  record[tone.filterEnvVelCurve] = 0u;
  double travel = jv1080_filter_env_offset(&tone, record, 127u);
  assert(close_to(travel, 68.0, 0.5));
  /* And the same take's velocity-1 note sits at the record's own cutoff:
     with the sensitivity near the top of its range, velocity 1 leaves the
     envelope closed. */
  assert(close_to(jv1080_filter_env_offset(&tone, record, 1u), 0.0, 1e-9));

  /* Sensitivity 0 is FLAT - the `fenv_vel_sens_000` stimulus's own
     hypothesis - and it is the full depth, not a dead envelope. */
  record[tone.filterEnvVelSens] = 0u;
  double flat = jv1080_filter_env_offset(&tone, record, 1u);
  assert(close_to(flat, jv1080_filter_env_offset(&tone, record, 64u), 1e-12));
  assert(close_to(flat, jv1080_filter_env_offset(&tone, record, 127u), 1e-12));
  assert(flat > 60.0);

  /* The negative half inverts: a hard-struck note gets LESS sweep. */
  record[tone.filterEnvVelSens] = (uint8_t)(int8_t)-50;
  assert(jv1080_filter_env_offset(&tone, record, 1u) >
         jv1080_filter_env_offset(&tone, record, 127u));
  assert(close_to(jv1080_filter_env_offset(&tone, record, 127u), 0.0, 1e-9));

  /* A negative depth sweeps downwards, and depth 0 is no envelope at all -
     which is the case that must keep a voice on the unswept path. */
  record[tone.filterEnvVelSens] = (uint8_t)(int8_t)74;
  record[tone.filterEnvDepth] = (uint8_t)(int8_t)-30;
  assert(close_to(jv1080_filter_env_offset(&tone, record, 127u), -68.0, 0.5));
  record[tone.filterEnvDepth] = 0u;
  assert(jv1080_filter_env_offset(&tone, record, 127u) == 0.0);

  /* A rhythm note has no velocity-curve field and is rendered on curve 0;
     its depth and sensitivity read from its own offsets. */
  uint8_t note[64];
  std::memset(note, 0, sizeof note);
  note[rhythm.filterEnvDepth] = (uint8_t)(int8_t)30;
  note[rhythm.filterEnvVelSens] = (uint8_t)(int8_t)74;
  assert(close_to(jv1080_filter_env_offset(&rhythm, note, 127u), 68.0, 0.5));
  assert(close_to(jv1080_filter_env_offset(&rhythm, note, 1u), 0.0, 1e-9));

  std::printf("xp_filter_env_test: ok\n");
  return 0;
}
