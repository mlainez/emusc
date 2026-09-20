/* SPDX-License-Identifier: CC0-1.0 */
#ifndef EMUSC_XP_TVA_H
#define EMUSC_XP_TVA_H

#include "rom.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sc88_tva_levels {
  uint8_t master;
  uint8_t secondary;
  uint8_t part;
  uint8_t expression;
};

/* A rhythm note also subtracts its own level, the kit's `+0x200 + note`
 * byte, from the same headroom and through the same table (`72a3..72ab`).
 * It is a fifth source beside the four above and not one of them: those are
 * part or global controls, this is per note, and it is passed separately
 * rather than folded into the struct because a part-level change rewrites
 * the four while the note keeps this one.
 *
 * `4d76` sets the flag `7295` tests from bit 7 of the kit's `+0x280`
 * assign-group byte - clear means subtract. The melodic note-on path at
 * `1bb4` stages `0xff` there, so bit 7 is set and the term is skipped;
 * `SC88_TVA_NO_DRUM_LEVEL` is that case. */
#define SC88_TVA_NO_DRUM_LEVEL 0xffu

struct sc88_tva_release {
  uint16_t current;
  uint16_t increment;
  uint16_t scale;
  bool scale_enabled;
  bool active;
};

/* The part-level envelope modifiers, each centred at 64. Stages 0 and 1
 * take the attack pair at part `+14`, stages 2 and 3 the decay pair at
 * `+15`; the centred sum is doubled and added to the component's own rate
 * index, clamped to 0..127 (`07_synthesis/tva.md`). The secondary source
 * is not modelled and stays neutral.
 */
struct sc88_tva_controls {
  uint8_t part_attack;
  uint8_t secondary_attack;
  uint8_t part_decay;
  uint8_t secondary_decay;
};

/* The interpolation word the firmware hands the chip beside a target: at
 * `71f2` on a stage change, at `71c7` every control period. `7892..7903`
 * packs it - bit 14 carries the family (`7878` reads `component[0x85+stage]`
 * and takes the linear table at `0x1553e` when it is nonzero, the
 * exponential one at `0x1563e` when it is zero), bits 13..12 an exponent
 * and bits 11..0 a mantissa. `789e..789c` leave the exponent as
 * `(nibble mod 4) << 6`, so `or.w #0x4000` at `7885` cannot change it and
 * bit 14 is a shape flag alone.
 *
 * `value = mantissa / 2^s` with `s = 0, 3, 5, 7`, and both families are
 * phase increments over their own range: `value * k = 512` for the linear
 * table and `5606` for the exponential one, where `k` is the segment
 * length. With the stage's own dwell `D = 65536 / increment` control
 * periods (`770e..7738`), that is `q = periods * value / 64` reaching 1 at
 * the end of a linear stage and `5606/512 = 10.95` at the end of an
 * exponential one - which is why the exponential arrives within 2e-5 of
 * its target exactly as the phase counter carries, and a stage boundary is
 * not a step. */
struct sc88_tva_curve {
  /* `value / 64`: the fraction of a linear stage covered per control
     period, and the reciprocal time constant of an exponential one. */
  double rate;
  bool linear;
};

struct sc88_tva_envelope {
  uint32_t targets_q17[4];
  /* The stage words as the component stores them, attenuations at about
     -5.26 dB per 0x1000. `targets_q17` above is what the chip is actually
     handed: `76b3..7705` converts each through the coarse and fine gain
     tables and writes the GAIN to `3d5a`. These two and
     `start_attenuation` are kept for the trace. */
  uint16_t target_attenuations[4];
  uint16_t start_attenuation;
  uint16_t curve_words[4];
  struct sc88_tva_curve curves[4];
  /* Control periods elapsed inside the stage now running. The phase
     counter ends the stage; this is what the curve is evaluated on. */
  double stage_periods;
  uint16_t initial_phases[4];
  uint16_t increments[4];
  uint8_t stage;
  uint8_t saved_count;
  uint16_t phase;
  uint32_t start_q17;
  uint32_t current_q17;
  bool active;
};

/* Compatibility surface for callers not yet ported to the EmuSC::Xp API
 * below (struct sc88_device in device.h, sc88_tvf_probe.c, and
 * sc88_tva_test.c, all of which read these structs' fields directly or
 * call sc88_tva_curve_decode/_progress). Each forwards to the real
 * implementation in namespace EmuSC::Xp. */
void sc88_tva_curve_decode(uint16_t word, struct sc88_tva_curve *curve);
double sc88_tva_curve_progress(const struct sc88_tva_curve *curve,
                               double periods);
bool sc88_tva_static_gain_q17(const struct sc88_rom *rom,
                              const struct sc88_tone *tone,
                              const struct sc88_component *component,
                              const struct sc88_zone_selection *zone,
                              uint8_t selector_key, uint8_t velocity,
                              const struct sc88_tva_levels *levels,
                              uint8_t drum_level,
                              uint16_t *static_attenuation,
                              uint32_t *gain_q17);
bool sc88_tva_gain_from_headroom_q17(const struct sc88_rom *rom,
                                     uint16_t headroom,
                                     const struct sc88_tva_levels *levels,
                                     uint8_t drum_level,
                                     uint16_t static_attenuation,
                                     uint32_t *gain_q17);
bool sc88_tva_release_prepare(const struct sc88_rom *rom,
                              const struct sc88_tone *tone,
                              const struct sc88_component *component,
                              uint8_t selector_key,
                              struct sc88_tva_release *release);
bool sc88_tva_release_set_pedal(const struct sc88_rom *rom,
                                uint8_t hold1, bool continuous_hold,
                                bool keep_scale_at_zero,
                                bool sostenuto_retained,
                                struct sc88_tva_release *release);
bool sc88_tva_release_advance(struct sc88_tva_release *release,
                              unsigned elapsed_periods);
bool sc88_tva_envelope_prepare(const struct sc88_rom *rom,
                               const struct sc88_tone *tone,
                               const struct sc88_component *component,
                               uint8_t selector_key, uint8_t velocity,
                               const struct sc88_tva_controls *controls,
                               struct sc88_tva_envelope *envelope);
bool sc88_tva_envelope_advance(const struct sc88_rom *rom,
                               struct sc88_tva_envelope *envelope,
                               unsigned elapsed_periods);
uint32_t sc88_tva_envelope_linear_q17(const struct sc88_rom *rom,
  const struct sc88_tva_envelope *envelope, double period_fraction);
void sc88_tva_envelope_freeze(const struct sc88_rom *rom,
                              struct sc88_tva_envelope *envelope,
                              double period_fraction);

#ifdef __cplusplus
}

namespace EmuSC { namespace Xp {

// TVA (amplitude envelope, level composition, release) for the
// XP-generation-1 engine (see engines/xp/README.md). The plain C types
// above are shared, unrenamed, with sibling engines/xp/*.c modules not
// yet ported.

void tva_curve_decode(uint16_t word, struct sc88_tva_curve *curve);
/* How much of the gap the curve has closed after `periods` periods. */
double tva_curve_progress(const struct sc88_tva_curve *curve, double periods);

/* Exact CPU-side note-on AmpM path. The returned linear XP gain is Q17 with
 * 0x20000 as unity. Envelope Amp, modulation and XP ramp precision are
 * separate stages and are deliberately not folded into this value. */
bool tva_static_gain_q17(const struct sc88_rom *rom, const struct sc88_tone *tone,
                          const struct sc88_component *component,
                          const struct sc88_zone_selection *zone,
                          uint8_t selectorKey, uint8_t velocity,
                          const struct sc88_tva_levels *levels,
                          uint8_t drumLevel, uint16_t *staticAttenuation,
                          uint32_t *gainQ17);
bool tva_gain_from_headroom_q17(const struct sc88_rom *rom, uint16_t headroom,
                                 const struct sc88_tva_levels *levels,
                                 uint8_t drumLevel, uint16_t staticAttenuation,
                                 uint32_t *gainQ17);
bool tva_release_prepare(const struct sc88_rom *rom, const struct sc88_tone *tone,
                          const struct sc88_component *component,
                          uint8_t selectorKey, struct sc88_tva_release *release);
bool tva_release_set_pedal(const struct sc88_rom *rom, uint8_t hold1,
                            bool continuousHold, bool keepScaleAtZero,
                            bool sostenutoRetained,
                            struct sc88_tva_release *release);
bool tva_release_advance(struct sc88_tva_release *release,
                          unsigned elapsedPeriods);

/* Four-stage firmware clock, target gains, rate scaling and the packed XP
 * curve word each stage is approached with. Attack/decay modifiers are
 * neutral in this entry point. `controls` may be null, which is the same
 * as every modifier centred. */
bool tva_envelope_prepare(const struct sc88_rom *rom, const struct sc88_tone *tone,
                           const struct sc88_component *component,
                           uint8_t selectorKey, uint8_t velocity,
                           const struct sc88_tva_controls *controls,
                           struct sc88_tva_envelope *envelope);
bool tva_envelope_advance(const struct sc88_rom *rom,
                           struct sc88_tva_envelope *envelope,
                           unsigned elapsedPeriods);
uint32_t tva_envelope_linear_q17(const struct sc88_rom *rom,
  const struct sc88_tva_envelope *envelope, double periodFraction);
void tva_envelope_freeze(const struct sc88_rom *rom,
                          struct sc88_tva_envelope *envelope,
                          double periodFraction);

}}  // namespace EmuSC::Xp
#endif

#endif
