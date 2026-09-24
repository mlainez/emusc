/* SPDX-License-Identifier: CC0-1.0 */
/*
 *  The JV-1080's filter envelope, the PKG section at the top of the cutoff
 *  range, and the amplitude envelope's velocity-time law, against the takes
 *  they were read from.
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
#include <initializer_list>

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

  /* MEASURED (`M-082`, P-xxxx TASK-407), in cutoff units at sensitivity
     +50: every curve reaches 1 at velocity 127, curves 0, 1, 2 and 5 start
     at 0, and at velocity 64 they read 0.510, 0.240, 0.062, 0.748, 0.877,
     0.522 and 0.504 - the numbers that separate the seven characters. */
  static const double kAtVelocity64[7] = {
    0.510, 0.240, 0.062, 0.748, 0.877, 0.522, 0.504,
  };
  for (unsigned c = 0; c < 7u; ++c) {
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
  static const unsigned kClosedAtOne[4] = { 0u, 1u, 2u, 5u };
  for (unsigned c : kClosedAtOne)
    assert(jv1080_filter_env_curve(c, 1u) == 0.0);

  /* Curve 0 is linear in velocity from 32 up - 0.124 per 16 - so the
     table's own interpolation must not put a kink in it. */
  for (unsigned v = 40u; v <= 96u; v += 16u) {
    double slope = (jv1080_filter_env_curve(0u, v + 8u) -
                    jv1080_filter_env_curve(0u, v - 8u)) / 16.0;
    assert(close_to(slope, 0.00775, 0.0003));
  }

  /* THE DEPTH SCALE, measured directly on the device at 2.771 cutoff
     units per depth unit in this engine's own frequency law: depth +30 at
     velocity 127 travels 30 * 2.771 = 83.13 cutoff units. The shallow
     curve take (cutoff 24, depth +30, sensitivity +50, curve 0,
     resonance 80) puts its velocity-127 peak at 4567 Hz, which
     tvf_natural_hz places at cutoff 107.07: 83.07 units over the record's
     cutoff, within 0.1 unit of this. */
  uint8_t record[XP_JV1080_TONE_FIELDS];
  std::memset(record, 0, sizeof record);
  record[tone.filterEnvDepth] = (uint8_t)(int8_t)30;
  record[tone.filterEnvVelSens] = (uint8_t)(int8_t)50;
  record[tone.filterEnvVelCurve] = 0u;
  double travel = jv1080_filter_env_offset(&tone, record, 127u);
  assert(close_to(travel, 83.13, 0.5));
  /* At +50 the travel is the curve itself, and velocity 1 leaves the
     envelope closed. */
  assert(close_to(jv1080_filter_env_offset(&tone, record, 64u),
                  travel * 0.510, 1e-9));
  assert(close_to(jv1080_filter_env_offset(&tone, record, 1u), 0.0, 1e-9));

  /* Sensitivity 0 is FLAT (`tvf/fenv_vel_sens_000`) and it is the full
     depth, not a dead envelope. */
  record[tone.filterEnvVelSens] = 0u;
  double flat = jv1080_filter_env_offset(&tone, record, 1u);
  assert(close_to(flat, jv1080_filter_env_offset(&tone, record, 64u), 1e-12));
  assert(close_to(flat, travel, 1e-12));

  /* `tvf/fenv_vel_sens_p75` (cutoff 40, depth +63, curve 0): +75 reads the
     curve at 127 - 2 (127 - v). Velocity 64 stays within 2 units of the
     record's cutoff (the take: 41.5 over 40) and 96 passes the 7809 Hz
     ceiling, about 75 units up (the take: at the ceiling). */
  record[tone.filterEnvDepth] = (uint8_t)(int8_t)63;
  record[tone.filterEnvVelSens] = (uint8_t)(int8_t)75;
  assert(jv1080_filter_env_offset(&tone, record, 32u) == 0.0);
  assert(jv1080_filter_env_offset(&tone, record, 64u) < 2.0);
  assert(jv1080_filter_env_offset(&tone, record, 96u) > 75.0);

  /* `tvf/fenv_vel_sens_m50`: -50 turns the +50 result over - velocity 96
     at 1 - 0.758 of the travel (the take: 42.1 units), 127 closed. */
  record[tone.filterEnvVelSens] = (uint8_t)(int8_t)-50;
  assert(close_to(jv1080_filter_env_offset(&tone, record, 96u), 42.1, 0.5));
  assert(close_to(jv1080_filter_env_offset(&tone, record, 127u), 0.0, 1e-9));
  assert(jv1080_filter_env_offset(&tone, record, 1u) >
         jv1080_filter_env_offset(&tone, record, 96u));
  record[tone.filterEnvDepth] = (uint8_t)(int8_t)30;

  /* A negative depth sweeps downwards, and depth 0 is no envelope at all -
     which is the case that must keep a voice on the unswept path. */
  record[tone.filterEnvVelSens] = (uint8_t)(int8_t)50;
  record[tone.filterEnvDepth] = (uint8_t)(int8_t)-30;
  assert(close_to(jv1080_filter_env_offset(&tone, record, 127u), -83.13, 0.5));
  record[tone.filterEnvDepth] = 0u;
  assert(jv1080_filter_env_offset(&tone, record, 127u) == 0.0);

  /* A rhythm note has no velocity-curve field and is rendered on curve 0;
     its depth and sensitivity read from its own offsets. */
  uint8_t note[64];
  std::memset(note, 0, sizeof note);
  note[rhythm.filterEnvDepth] = (uint8_t)(int8_t)30;
  note[rhythm.filterEnvVelSens] = (uint8_t)(int8_t)50;
  assert(close_to(jv1080_filter_env_offset(&rhythm, note, 127u), 83.13, 0.5));
  assert(close_to(jv1080_filter_env_offset(&rhythm, note, 1u), 0.0, 1e-9));

  /* THE TIME LAW. The rate each segment moves the resonant peak at, read
     on `tvf/fenv_t1_sweep` and `gaps/fenv_t4_hold` at depth +63, is a
     full traverse of 174.6 units in these times (T1 and T4 averaged at 32,
     48 and 64; T1 alone at 96) - each within 4 % here. */
  static const struct { unsigned value; double seconds; } kTraverse[] = {
    { 32u, 0.325 }, { 48u, 0.764 }, { 64u, 1.663 }, { 96u, 7.46 },
  };
  for (const auto &p : kTraverse) {
    double t = jv1080_filter_env_segment_seconds(p.value);
    assert(std::fabs(t / p.seconds - 1.0) < 0.04);
  }
  /* And a longer time field is never faster. */
  for (unsigned v = 1u; v <= 127u; ++v)
    assert(jv1080_filter_env_segment_seconds(v) >=
           jv1080_filter_env_segment_seconds(v - 1u));

  /* The A-ENV's velocity-time sensitivity (P-xxxx, TASK-402): tone byte
     0x6B, the rhythm note's 0x2B. The time-4 field (0x6C) is measured not
     to follow the note-on velocity and is not mapped. */
  assert(tone.ampEnvVelTime1 == 0x6bu);
  assert(rhythm.ampEnvVelTime1 == 0x2bu);
  /* `envelopes/aenv_vel_t1_i14` as a ratio to index 7's 1.042 s rise, at
     velocities 1, 16, 32, 64, 96 and 127; index 0 is its mirror about 64
     and index 7 is flat. Each within 6 %. */
  static const struct { unsigned velocity; double ratio; } kIndex14[] = {
    { 1u, 1.541 }, { 16u, 1.420 }, { 32u, 1.276 },
    { 64u, 0.987 }, { 96u, 0.683 }, { 127u, 0.415 },
  };
  for (const auto &p : kIndex14) {
    double r = jv1080_amp_env_velocity_time_scale(14u, p.velocity);
    assert(std::fabs(r / p.ratio - 1.0) < 0.06);
    assert(close_to(jv1080_amp_env_velocity_time_scale(0u, 128u - p.velocity),
                    r, 1e-12));
    assert(jv1080_amp_env_velocity_time_scale(7u, p.velocity) == 1.0);
  }

  /* PKG at the saturated top, resonance 0 (P-xxxx, TASK-429): the machine's
     `tvf/cutoff_pkg_res000` slots at 112, 120 and 127, each against the
     same take's cutoff-0 slot (the filter OFF), in sixth-octave bands at
     its 32 kHz. A high shelf, still climbing at 12.8 kHz; each point
     within 0.25 dB. */
  static const struct { double hz; double db; } kPkgTop[] = {
    { 504.0, 0.08 }, { 1008.0, 0.34 }, { 2016.0, 1.26 }, { 4032.0, 4.08 },
    { 6400.0, 7.56 }, { 8063.0, 9.63 }, { 10159.0, 11.66 },
    { 12800.0, 13.22 },
  };
  for (double cutoff : { 112.0, 120.0, 127.0 })
    for (const auto &p : kPkgTop)
      assert(close_to(jv1080_tvf_response_db(4, cutoff, 0u, 32000.0, p.hz),
                      p.db, 0.25));
  /* Unity at DC: the shelf lifts the top, not the level. */
  assert(close_to(jv1080_tvf_response_db(4, 127.0, 0u, 32000.0, 20.0), 0.0,
                  0.05));
  /* Below 112, and with resonance, PKG stays the two-pole bump, which
     returns toward 0 dB above its peak: the take has no slot between 104
     and 112, and the resonance-0 shelf is not applied past what it
     measured. */
  assert(jv1080_tvf_response_db(4, 111.0, 0u, 32000.0, 12800.0) < 6.0);
  assert(jv1080_tvf_response_db(4, 127.0, 1u, 32000.0, 12800.0) < 6.0);
  /* The low-pass at the same settings is still the filter OFF. */
  assert(close_to(jv1080_tvf_response_db(1, 127.0, 0u, 32000.0, 12800.0),
                  0.0, 1e-9));

  std::printf("xp_filter_env_test: ok\n");
  return 0;
}
