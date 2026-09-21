/* SPDX-License-Identifier: CC0-1.0 */
#include "tvf.h"

#include "tva.h"

#include <cmath>
#include <cstring>
#include <climits>

namespace EmuSC { namespace Xp {

namespace {

constexpr uint32_t kBaseTable = 0x78702u;
/* [FW-EXACT] TVF-Q current is the companion word << 2, i.e.
   resonance_index << 11, and one unit of damping is 131072 - that is,
   q = resonance_index / 64.

   The scaling is fixed by the chip's own limit table; see the note on
   kLimitTable. Read with q = index/64 that table is exactly
   f*f + f*q = 2 on all 128 entries; read with index/32 it spans
   2.000..3.458 and with index/128 1.270..2.000. The value libEmuSC's
   SC-55 path uses (svf.cc's set_resonance, q = resonance/64, which the
   SC-55's own two stability tables fix to a rounding unit in P-0130 and
   P-0131) is the value the SC-88's ROM asks for too. */
constexpr double kQUnity = 131072.0;
/* [FW-EXACT] The TVF-F register is a log-frequency word in the XP pitch
   register's own domain. `07_synthesis/pitch.md` has the pitch word at
   16384 units per octave, 18 bits, unity playback at 0x38000; routine
   67a8 forms TVF-F as the same 18-bit high/low pair in the same scratch
   tuple with the same interpolation word 0x4100, and the base table at
   0x78702 steps by exactly 16384/12 per index once expanded - one
   semitone per index. That is the slope. The anchor - which frequency a
   register value names - is the ROM's too, and the limit table fixes it
   in integer arithmetic.

   The limit table's law is f*f + f*q = 2 (see kLimitTable), which gives
   f = sqrt(2) at resonance index 0 and f = 1 at index 64. With
   f = 2*sin(pi*fc/fs) those two ceilings are fc = fs/4 and fs/6 exactly,
   so their sines are sqrt(2)/2 and 1/2 and their words sit 16384/2 and
   16384 units below the word whose sine is one. The entries are 0xf800
   and 0xf000, which the firmware expands ((e >> 1) << 3) to 0x3e000 and
   0x3c000:

       0x3e000 + 8192  = 0x40000
       0x3c000 + 16384 = 0x40000

   Neither equation rounds. Those two are the only entries in either
   table whose exact word is a multiple of four, so they are the only
   two the table's floor leaves untouched, and both name 0x40000 - the
   word one past the 18-bit range - as the chip's Nyquist.

   With that anchor both tables decode to the bit:

       entry = floor((0x40000 + 16384*log2(sin(pi*f/fs))) / 4)

   the base table over f = 440*2^((i - 64)/12) for all 127 indices below
   Nyquist, and the limit table over the stability law for all 128. So
   the base table is a note table whose index 64 is A440; index 127 asks
   for 16744 Hz, past the fold, and holds 0xffff instead. floor is what
   reproduces them - round gets 64 and 57, ceil 0 and 2 - and moving the
   anchor by one word unit breaks at least 28 entries per table. The
   limit table runs fs/4 at resonance index 0 through fs/6 at 64 to
   3835 Hz at 127, and 0x38000 - unity playback in the pitch register -
   is 2573.8 Hz here.

   fc and fs enter only as sin(pi*fc/fs), so the ROM fixes their ratio
   and nothing more. Fit the base table with the anchor and the absolute
   scale both free and index 64 comes out at 439.9996 Hz, 1sd 0.05 cent,
   which is a second reading of the 32.000 kHz rate - `M-166` has it from
   the DAC image mirror. The coefficient the chip derives from the word
   is recovered as well: see kLimitTable. */
constexpr double kOctaveUnits = 16384.0;
constexpr uint32_t kNyquistWord = 0x40000;
/* [FW-EXACT] The limit table is the filter's own topology, written down.
 *
 * 0x78802 holds one cutoff ceiling per resonance index. Read in the base
 * table's domain - f = 2*sin(pi*fc/32000), the Chamberlin coefficient the
 * base table already is - and with q = resonance_index/64, every one of
 * its 128 entries satisfies
 *
 *     f*f + f*q = 2
 *
 * to a rounding unit: the residual against the exact law is -0.53 +- 0.29
 * table units and lies in [-0.99, 0], which is the signature of a floor(),
 * not of a fit. In octaves that is 0.33 cent of cutoff.
 *
 * That expression is not a general fact about two-pole filters. It is the
 * trace of the state matrix of the forward-Euler state-variable filter
 *
 *     lp[n] = lp[n-1] + f*bp[n-1]
 *     hp[n] = x[n] - lp[n] - q*bp[n-1]
 *     bp[n] = bp[n-1] + f*hp[n]
 *
 * whose trace is 2 - f*f - f*q and whose determinant is 1 - f*q. Setting
 * the trace to zero puts the pole pair at exactly +-90 degrees: the chip
 * refuses to let the filter's realised resonance climb past fs/4, and the
 * ceiling it writes for each resonance is the cutoff at which that
 * happens. A trapezoidal (bilinear) realisation is stable for every
 * positive coefficient and has no such ceiling to tabulate; its own
 * coefficient g = tan(pi*fc/fs) puts g*g + g*q at 0.906..1.000 over the
 * same table, and reading the word as a frequency instead of a sine puts
 * it at 3.211..3.414. The topology is forward-Euler.
 *
 * This is the SC-55's filter. libEmuSC's svf.cc is the same three lines,
 * and the SC-55's control ROM carries the same law twice: TVFResonance[]
 * is 128*(sqrt(q*q+4) - q) and TVFResonanceFreq[] is 32*(4-f*f)/f, the
 * two sides of that state matrix's stability edge, with q = r/64 and
 * f = x/128 (P-0130, P-0131). The SC-88 tabulates the same topology at a
 * tighter bound.
 *
 * Two things follow that the audio path could not have been told any
 * other way. The coefficient is the ROM's own word and is not warped
 * again; and a tone whose computed cutoff is clamped to this ceiling is
 * a tone the chip has been asked to leave alone - at q = 1 the law gives
 * f = 1 exactly, where the difference equations above collapse to
 * y[n] = x[n-1], a bare sample of delay. "Wide open" is realised as
 * transparent, which is what asking for a cutoff above the ceiling
 * should mean. */
constexpr uint32_t kLimitTable = 0x78802u;
/* The sound chip's own sample rate, which the cutoff word is a
   fraction of. */
constexpr double kNativeRate = 32000.0;
constexpr uint32_t kEnvelopeRateTable = 0x1543eu;
constexpr uint32_t kRateScaleTable = 0x1573eu;
constexpr uint32_t kReleasePedalTable = 0x78a02u;

uint16_t be16(const uint8_t *p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

int8_t s8(uint8_t value)
{
  return value <= INT8_MAX
    ? (int8_t)value
    : (int8_t)(-1 - (int16_t)(UINT8_MAX - value));
}

int16_t s16(uint16_t value)
{
  return value <= INT16_MAX
    ? (int16_t)value
    : (int16_t)(-1 - (int32_t)(UINT16_MAX - value));
}

int32_t floorDivPow2(int32_t value, unsigned shift)
{
  if (value >= 0)
    return value / (INT32_C(1) << shift);
  return -(int32_t)(((uint32_t)(-value) +
    ((UINT32_C(1) << shift) - 1)) >> shift);
}

int clampIndex(int value)
{
  if (value < 0)
    return 0;
  if (value > 127)
    return 127;
  return value;
}

/* `0x6bb3`..`0x6bd3` and `0x6c97`..`0x6cb7`. The firmware multiplies by
 * magnitude and keeps the unsigned product's high word. Where exactly
 * one operand is negative it negates that word and then subtracts the
 * borrow left by restoring the register it negated, so the result is one
 * below the magnitude's high word negated - floor, except where the
 * product is an exact multiple of 65536. Restoring a zero leaves no
 * borrow, so a zero waveform gives a zero term rather than -1. */
int32_t signedProductHigh(int32_t left, int16_t right)
{
  uint32_t magnitude =
    (uint32_t)(left < 0 ? -left : left) *
    (uint32_t)(right < 0 ? -(int32_t)right : (int32_t)right);

  if ((left < 0) == (right < 0))
    return (int32_t)(magnitude >> 16);
  if (right == 0)
    return 0;
  return -(int32_t)(magnitude >> 16) - 1;
}

bool keyRateScale(const struct sc88_rom *rom, const struct sc88_tone *tone,
                   const struct sc88_component *component,
                   uint8_t selectorKey, uint16_t pointerAt, uint8_t factorAt,
                   uint16_t *scale)
{
  if (!rom || !rom->bytes || !tone || !tone->common || !component ||
      !component->bytes || !scale || selectorKey > 127)
    return false;
  uint32_t curve = ((uint32_t)tone->common[0x21] << 16) |
    be16(component->bytes + pointerAt);
  if (curve + selectorKey >= rom->size ||
      kRateScaleTable + 129u * 2 > rom->size)
    return false;
  int keyValue = s8(rom->bytes[curve + selectorKey]);
  int factor = s8((uint8_t)(0u - component->bytes[factorAt]));
  int index = floorDivPow2(keyValue * factor, 8) + 64;
  if (index < 0 || index > 128)
    return false;
  *scale = be16(rom->bytes + kRateScaleTable + (uint32_t)index * 2);
  return true;
}

bool velocityRateScale(const struct sc88_rom *rom, uint8_t velocity,
                        int factor, uint16_t *scale)
{
  if (!rom || !rom->bytes || !scale || velocity > 127 ||
      factor < -128 || factor > 127 ||
      kRateScaleTable + 129u * 2 > rom->size)
    return false;
  int index = floorDivPow2((2 * ((int)velocity - 64)) * factor, 8) + 64;
  if (index < 0 || index > 128)
    return false;
  *scale = be16(rom->bytes + kRateScaleTable + (uint32_t)index * 2);
  return true;
}

bool envelopeDepth(const struct sc88_rom *rom, const struct sc88_tone *tone,
                    const struct sc88_component *component, uint8_t velocity,
                    bool softPedal, uint16_t *depth)
{
  if (!rom || !rom->bytes || !tone || !tone->common || !component ||
      !component->bytes || !depth || velocity > 127)
    return false;
  uint16_t input = be16(component->bytes + 0x48);
  if (input == 0) {
    *depth = 0;
    return true;
  }
  if (softPedal)
    velocity = (uint8_t)(((uint32_t)velocity * UINT16_C(0xb76f)) >> 16);
  uint32_t curve = ((uint32_t)tone->common[0x21] << 16) |
    be16(component->bytes + 0x3a);
  if (curve + velocity >= rom->size)
    return false;
  unsigned curveValue = rom->bytes[curve + velocity];
  int factor = s16(be16(component->bytes + 0x60));
  uint32_t magnitude;
  if (factor < 0) {
    magnitude = (uint32_t)(-factor);
    curveValue = (unsigned)(uint8_t)(0u - curveValue) & 0x7fu;
  } else {
    magnitude = (uint32_t)factor;
  }
  curveValue = (unsigned)(uint8_t)(~curveValue) & 0x7fu;
  uint16_t complement = (uint16_t)~(uint16_t)(magnitude * curveValue);
  *depth = (uint16_t)(((uint32_t)input * complement) >> 16);
  return true;
}

void prepareIncrement(uint16_t tableRate, uint16_t scale, uint16_t *phase,
                       uint16_t *increment)
{
  if (tableRate < 16)
    tableRate = UINT16_MAX;
  uint32_t product = (uint32_t)tableRate * scale;
  if (product >= UINT32_C(0x01000000)) {
    *phase = UINT16_MAX;
    *increment = UINT16_MAX;
  } else {
    *phase = 0;
    *increment = (uint16_t)(product >> 8);
  }
}

int16_t scaleTarget(int16_t target, uint16_t depth)
{
  return s16((uint16_t)floorDivPow2((int32_t)target * depth, 16));
}

/* `67a8` writes the TVF-F target and current value at scratch `1a7c`, and
 * the interpolation pair beside them; `683f: ea 2a 07 41 00` puts
 * `#0x4100` in the low word, which `tva_curve_decode` reads as the LINEAR
 * family at value 256 and `q = 4 * periods` - see tva.cc. */
double frequencyProgress(const struct sc88_tvf_registers *registers,
                          double periods)
{
  /* frequency_interpolation has exactly one writer, a few lines below in
     tvf_prepare_registers: always the literal 0x4100 (also asserted by
     sc88_rom_test.c, sc88_tvf_test.c and sc88_renderer_test.c). Decoding
     that fixed word is always the linear family at rate 256/64 = 4.0
     exactly - a power-of-two divisor, so this is that double, not an
     approximation of it - which is what tva_curve_decode/_progress
     reduce to below. Called once per voice per output sample, this skips
     redoing that decode from scratch every time. */
  const double q = 4.0 * periods;
  return q >= 1.0 ? 1.0 : q;
}

}  // namespace

/* The word is the log of sin(pi * f / fs), not the log of f.

   Both machines hold the same cutoff table and the SC-55 holds it in the
   clear. The SC-55 mk1 CPU ROM at 0x7612 is 32768 * sin(pi * f / 32000)
   for f = 440 * 2^((index - 64)/12): fitted over its 116 pre-saturation
   entries the residual is 0.435 LSB rms and the anchor lands on 440.0 Hz
   at index 64 - one semitone per index, saturating where the sine folds.

   The SC-88's base table at 0x78702 is that same quantity in the XP pitch
   register's log domain and decodes to the bit; see the note on
   kNyquistWord. Reading the word as a log FREQUENCY instead leaves an sd
   of 2072 word units and up to 0.67 octave, because it has no account of
   why the table's steps shrink from 1368 to 0 over its last twelve
   entries. That compression is the sine approaching one; the frequency
   underneath it is a clean note table.

   Hardware agrees, on two stimuli each with its own control and with the
   corner fitted through the realisation the chip has rather than an
   analog prototype. Hardware corner over computed cutoff is 0.97
   (quartiles 0.90/1.10, n = 13) measured against our own --no-filter
   render, and 1.04 (0.99/1.25, n = 9) on a self-differential of two
   instants inside one held note, which carries no render of ours at all.
   Their controls - the same fit on our own output, whose corner is at
   the computed cutoff by construction - read 1.05 and 1.00, so the
   method's own bias is the size of the disagreement.

   0x40000 still means the top of the range and still means fs/2; what
   runs exponentially between is sin(pi * f / fs). Below about 4 kHz the
   two readings differ by exactly pi/2 - 0.651 octave - converging at the
   top. */
double tvf_word_to_hz(uint32_t word)
{
  double sine = std::exp2(((double)word - kNyquistWord) / kOctaveUnits);

  if (sine >= 1.0)
    return 0.5 * kNativeRate;
  return (kNativeRate / 3.14159265358979323846) * std::asin(sine);
}

/* `0x6ad0`..`0x6aff`. The matrix's cached word is a controller reading,
   not yet a cutoff term: the firmware clamps it to `0xf060..0x0fa0`
   (`0x6ad4`, `0x6ade`), shifts it left three (`0x6ae6`), multiplies by
   `0x8312` keeping only the signed high word (`0x6aec`..`0x6afb`) and
   halves that (`0x6afd`). Full scale reaches 8191, which is two octaves
   in the base table's 4096-per-octave word - the constant was chosen for
   exactly that, as were the LFO paths' own `+-0xfc0` and `0x8208`.

   A cached word of zero short-circuits at `0x6ad2`, which this returns
   the same value for, so the branch is arithmetic rather than a case. */
int16_t tvf_matrix_cutoff_term(int16_t cached)
{
  int32_t value = cached;

  if (value < -4000)
    value = -4000;
  else if (value > 4000)
    value = 4000;
  value = floorDivPow2((value << 3) * INT32_C(0x8312), 16);
  return (int16_t)floorDivPow2(value, 1);
}

/* `0x6b7a`..`0x6bd7` and `0x6c58`..`0x6cbb`, the two LFO filter terms.
   Each oscillator's faded depth word is a modulation reading, not yet a
   cutoff term: the firmware short-circuits a zero (`0x6b7c`, `0x6c5a`),
   clamps to `0xf040..0x0fc0` (`0x6b7e`, `0x6b88`), shifts left three
   (`0x6b90`), multiplies by `0x8208` keeping the signed high word
   (`0x6b96`..`0x6ba5`), halves that (`0x6ba7`) and multiplies the result
   by the oscillator's own waveform word.

   The clamped depth reaches 8191, two octaves in the base table's 4096
   per octave, and a full-scale waveform brings the term to 4095 - one
   octave before the accumulator's shift right one, half an octave
   after it. */
int16_t tvf_lfo_filter_term(int16_t fadedDepth, int16_t waveform)
{
  int32_t value = fadedDepth;

  if (value == 0)
    return 0;
  if (value < -4032)
    value = -4032;
  else if (value > 4032)
    value = 4032;
  value = floorDivPow2((value << 3) * INT32_C(0x8208), 16);
  value = floorDivPow2(value, 1);
  return (int16_t)signedProductHigh(value, waveform);
}

bool tvf_prepare_registers(const struct sc88_rom *rom,
                            const struct sc88_component *component,
                            int16_t preBaseModulation,
                            const struct sc88_tvf_controls *controls,
                            struct sc88_tvf_registers *registers)
{
  if (!rom || !rom->bytes || rom->size < kLimitTable + 256u ||
      !component || !component->bytes || !controls || !registers ||
      controls->part_cutoff > 127 || controls->secondary_cutoff > 127 ||
      controls->part_resonance > 127 ||
      controls->secondary_resonance > 127)
    return false;

  std::memset(registers, 0, sizeof *registers);
  registers->frequency_interpolation = 0x4100;
  registers->resonance_interpolation = 0x095f;
  int mode = s8(component->bytes[0x3e]);
  if (mode < 0) {
    registers->resonance_current = 0x20000;
    registers->resonance_target = 0x80000;
    registers->filter_select = 0x0800;
    registers->fixed_tuple = true;
    return true;
  }

  int cutoffIndex = clampIndex(
    component->bytes[0x3c] + controls->part_cutoff +
    controls->secondary_cutoff - 128);
  int resonanceIndex = clampIndex(
    component->bytes[0x3d] - 2 * (controls->part_resonance +
                                  controls->secondary_resonance - 128));
  int resonanceFloor = component->bytes[0x3d] < 4
    ? component->bytes[0x3d] : 4;
  if (resonanceIndex < resonanceFloor)
    resonanceIndex = resonanceFloor;

  /* Base and limit share one domain: both halved. Routine 6ccd shifts
   * the saturated table-plus-modulation sum right one (6d21) and the
   * limit is read from 78802 and shifted right one (68b4, 6984) before
   * the unsigned compare. Halved on both sides the base spans
   * 12561..32767 against a limit of 29811..31744, so the clamp bites
   * only for the top dozen indices. */
  /* `0x6aff`..`0x6cc9` builds RAM 30da as the controller term, the two
     LFO terms and the key term added into one another with plain 16-bit
     `add:g.w`, so the accumulator wraps before the table entry is added
     to it. The caller supplies the key and LFO terms; the controller
     term is the matrix's. */
  int16_t accumulator = s16(
    (uint16_t)((uint16_t)preBaseModulation +
               (uint16_t)tvf_matrix_cutoff_term(controls->matrix_cutoff)));
  int32_t combined = be16(rom->bytes + kBaseTable + (unsigned)cutoffIndex * 2u);
  combined += accumulator;
  if (combined < 0)
    combined = 0;
  else if (combined > UINT16_MAX)
    combined = UINT16_MAX;
  registers->base_unshifted = (uint16_t)combined;
  combined >>= 1;
  registers->base_value = (uint16_t)combined;
  uint16_t limit = (uint16_t)(be16(
    rom->bytes + kLimitTable + (unsigned)resonanceIndex * 2u) >> 1);
  if (combined > limit)
    combined = limit;

  registers->cutoff_index = (uint8_t)cutoffIndex;
  registers->resonance_index = (uint8_t)resonanceIndex;
  registers->combined = (uint16_t)combined;
  registers->frequency_current = (uint32_t)combined << 3;
  registers->frequency_target = registers->frequency_current;
  registers->resonance_current = (uint32_t)resonanceIndex << 11;
  registers->resonance_target = (uint32_t)resonanceIndex << 13;
  registers->filter_select = (uint16_t)((unsigned)mode << 8);
  return true;
}

bool tvf_key_modulation(const struct sc88_rom *rom, const struct sc88_tone *tone,
                         const struct sc88_component *component,
                         uint8_t selectorKey, int16_t *modulation)
{
  if (!rom || !rom->bytes || !tone || !tone->common || !component ||
      !component->bytes || !modulation || selectorKey > 127)
    return false;
  uint32_t table = ((uint32_t)tone->common[0x21] << 16) |
    be16(component->bytes + 0x40);
  if (table + (uint32_t)selectorKey * 2 + 2 > rom->size)
    return false;
  int16_t keyValue = s16(be16(rom->bytes + table + (uint32_t)selectorKey * 2));
  int16_t factor = s16(be16(component->bytes + 0x42));
  int32_t high = floorDivPow2((int32_t)keyValue * factor, 16);
  *modulation = s16((uint16_t)((uint16_t)high << 1));
  return true;
}

bool tvf_envelope_prepare(const struct sc88_rom *rom, const struct sc88_tone *tone,
                           const struct sc88_component *component,
                           uint8_t selectorKey, uint8_t velocity, bool softPedal,
                           struct sc88_tvf_envelope *envelope)
{
  if (!rom || !rom->bytes || !tone || !component || !component->bytes ||
      !envelope || selectorKey > 127 || velocity > 127 ||
      kEnvelopeRateTable + 128u * 2 > rom->size ||
      !envelopeDepth(rom, tone, component, velocity, softPedal,
                     &envelope->depth))
    return false;
  {
    uint16_t depth = envelope->depth;
    std::memset(envelope, 0, sizeof *envelope);
    envelope->depth = depth;
  }
  envelope->stage = 4;
  if (envelope->depth == 0)
    return true;
  uint16_t keyScale;
  if (!keyRateScale(rom, tone, component, selectorKey, 0x5a, 0x5e, &keyScale))
    return false;
  for (unsigned stage = 0; stage < 4; ++stage) {
    int factor = s8(component->bytes[stage < 2 ? 0x62 : 0x63]);
    uint16_t velocityScale;
    if (!velocityRateScale(rom, velocity, factor, &velocityScale))
      return false;
    uint16_t finalScale = (uint16_t)(((uint32_t)keyScale * velocityScale) >> 8);
    uint16_t tableRate = be16(rom->bytes + kEnvelopeRateTable +
      (uint32_t)component->bytes[0x54 + stage] * 2);
    prepareIncrement(tableRate, finalScale, envelope->initial_phases + stage,
                      envelope->increments + stage);
    envelope->targets[stage] = scaleTarget(
      s16(be16(component->bytes + 0x4a + stage * 2)), envelope->depth);
  }
  envelope->stage = component->bytes[0x54] == 0 ? 1 : 0;
  envelope->base = envelope->stage == 1 ? envelope->targets[0] : 0;
  envelope->current = envelope->base;
  envelope->delta = s16((uint16_t)(
    (uint16_t)envelope->targets[envelope->stage] - (uint16_t)envelope->base));
  envelope->phase = envelope->initial_phases[envelope->stage];
  envelope->active = true;
  return true;
}

bool tvf_envelope_advance(struct sc88_tvf_envelope *envelope,
                           unsigned elapsedPeriods)
{
  if (!envelope || !envelope->active || envelope->stage >= 4 ||
      elapsedPeriods == 0)
    return false;
  uint8_t catchup = (uint8_t)(elapsedPeriods - 1);
  uint16_t remaining = (uint16_t)(envelope->saved_count +
    (catchup <= 127 ? (int)catchup : (int)catchup - 256));
  uint16_t working = envelope->phase;
  uint16_t increment = envelope->increments[envelope->stage];
  for (;;) {
    uint16_t next = (uint16_t)(working + increment);
    if (next < working) {
      envelope->saved_count = (uint8_t)remaining;
      ++envelope->stage;
      if (envelope->stage == 4) {
        envelope->current = s16((uint16_t)(
          (uint16_t)envelope->base + (uint16_t)envelope->delta));
        envelope->phase = 0;
        envelope->active = false;
      } else {
        envelope->base = s16((uint16_t)(
          (uint16_t)envelope->base + (uint16_t)envelope->delta));
        envelope->current = envelope->base;
        envelope->delta = s16((uint16_t)(
          (uint16_t)envelope->targets[envelope->stage] -
          (uint16_t)envelope->base));
        envelope->phase = envelope->initial_phases[envelope->stage];
      }
      return true;
    }
    working = next;
    --remaining;
    if (remaining == UINT16_MAX)
      break;
  }
  envelope->phase = working;
  envelope->saved_count = 0;
  envelope->current = s16((uint16_t)(
    (uint16_t)envelope->base + (uint16_t)floorDivPow2(
      (int32_t)envelope->delta * working, 16)));
  return true;
}

bool tvf_release_prepare(const struct sc88_rom *rom, const struct sc88_tone *tone,
                          const struct sc88_component *component,
                          uint8_t selectorKey, uint16_t envelopeDepthValue,
                          struct sc88_tvf_release *release)
{
  if (!rom || !rom->bytes || !tone || !component || !component->bytes ||
      !release || selectorKey > 127 ||
      kEnvelopeRateTable + 128u * 2 > rom->size)
    return false;
  std::memset(release, 0, sizeof *release);
  release->scale = UINT16_MAX;
  if (envelopeDepthValue == 0)
    return true;
  uint16_t keyScale;
  if (!keyRateScale(rom, tone, component, selectorKey, 0x5c, 0x5f, &keyScale))
    return false;
  uint16_t tableRate = be16(rom->bytes + kEnvelopeRateTable +
                            (uint32_t)component->bytes[0x58] * 2);
  uint16_t initialPhase;
  prepareIncrement(tableRate, keyScale, &initialPhase, &release->increment);
  release->phase = initialPhase;
  release->target = scaleTarget(
    s16(be16(component->bytes + 0x52)), envelopeDepthValue);
  return true;
}

bool tvf_release_set_pedal(const struct sc88_rom *rom, uint8_t hold1,
                            bool continuousHold, bool keepScaleAtZero,
                            bool sostenutoRetained,
                            struct sc88_tvf_release *release)
{
  if (!rom || !rom->bytes || !release || hold1 > 127)
    return false;
  release->scale_enabled = true;
  if (sostenutoRetained) {
    release->scale = 0;
  } else {
    release->scale = UINT16_MAX;
    unsigned effective = continuousHold ? hold1 : (hold1 >= 64 ? 127u : 0u);
    if (effective == 0) {
      if (!keepScaleAtZero)
        release->scale_enabled = false;
    } else {
      uint32_t offset = kReleasePedalTable + (127u - effective) * 2;
      if (offset + 2 > rom->size)
        return false;
      release->scale = be16(rom->bytes + offset);
    }
  }
  release->active = true;
  return true;
}

bool tvf_release_advance(struct sc88_tvf_release *release,
                          unsigned elapsedPeriods)
{
  if (!release || !release->active || elapsedPeriods == 0)
    return false;
  uint16_t step = release->scale_enabled
    ? (uint16_t)(((uint32_t)release->increment * release->scale) >> 16)
    : release->increment;
  uint8_t periods = (uint8_t)elapsedPeriods;
  uint32_t product = (uint32_t)step * periods;
  uint16_t next = (uint16_t)(release->phase + (uint16_t)product);
  if ((product >> 16) != 0 || next < release->phase) {
    release->current = release->target;
    release->active = false;
  } else {
    release->phase = next;
    release->current = scaleTarget(release->target, next);
  }
  return true;
}

bool tvf_update_frequency(const struct sc88_rom *rom,
                           int16_t postBaseModulation,
                           struct sc88_tvf_registers *registers)
{
  if (!rom || !rom->bytes || !registers || rom->size < kLimitTable + 256u)
    return false;
  if (registers->fixed_tuple)
    return true;
  /* The envelope and release outputs join the word after the halving.
     Routine 6ccd saturates the table entry plus the key, controller and
     LFO terms, shifts the sum right one and stores it; 68a1, 68e3 and
     6971 then add the envelope output (and, in the update path, the
     release ramp) to that stored word with a plain 16-bit add, and the
     sum is compared unsigned against the halved limit. One unit of
     envelope is therefore 1/2048 octave in the register - twice the
     weight of one unit of table or key term. The add does not saturate;
     over the held bank the halved word never falls below 12561 and no
     envelope target reaches -9101 from there, so it never wraps. */
  uint16_t combined = (uint16_t)((uint16_t)registers->base_value +
                                 (uint16_t)postBaseModulation);
  uint16_t limit = (uint16_t)(be16(
    rom->bytes + kLimitTable + (uint32_t)registers->resonance_index * 2) >> 1);
  if (combined > limit)
    combined = limit;
  registers->combined = combined;
  registers->frequency_target = (uint32_t)combined << 3;
  return true;
}

void tvf_latch_frequency(struct sc88_tvf_registers *registers)
{
  if (registers)
    registers->frequency_current = registers->frequency_target;
}

void tvf_advance_registers(struct sc88_tvf_registers *registers,
                            unsigned periods)
{
  if (!registers || periods == 0)
    return;
  double value = (double)registers->frequency_current +
    frequencyProgress(registers, (double)periods) *
    ((double)registers->frequency_target - registers->frequency_current);
  registers->frequency_current = value <= 0.0 ? 0u : (uint32_t)(value + 0.5);
  /* TVF-Q is not approached: its target is the same value as its current
     with two more fraction bits (see the damping note in the audio path). */
}

void tvf_audio_reset(struct sc88_tvf_audio_state *state)
{
  if (state)
    std::memset(state, 0, sizeof *state);
}

float tvf_audio_process_provisional(void *user, struct sc88_tvf_audio_state *state,
                                     const struct sc88_tvf_registers *registers,
                                     double periodFraction, float input)
{
  if (!state || !registers)
    return input;
  /* A negative mode byte installs the fixed tuple: zero cutoff, type word
     0x0800 - type code 2, the high-pass. A high-pass at zero cutoff
     passes everything, so the tuple is a bypass by its own terms; see the
     type-code note below. */
  if (registers->fixed_tuple)
    return input;
  if (periodFraction < 0.0)
    periodFraction = 0.0;
  else if (periodFraction > 1.0)
    periodFraction = 1.0;
  /* Where the register stands this far into the period, by its own
     interpolation word rather than by a ramp spread over the whole of it. */
  double word = (double)registers->frequency_current +
    frequencyProgress(registers, periodFraction) *
    ((double)registers->frequency_target - registers->frequency_current);
  double g;
  {
    const uint32_t wordInt = (uint32_t)(word + 0.5);
    /* g is a pure function of wordInt at a fixed host rate, and the host
       rate is set once per Device and never changes mid-render (it comes
       from renderer->tvf_audio_user, wired up once at device init) - so
       memoizing on wordInt alone, per voice, is exact, not an
       approximation of the rate-dependent case. */
    if (state->memo_valid && state->memo_word == wordInt) {
      g = state->memo_g;
    } else {
      /* The coefficient the ROM's word already is. At the chip's own rate
         this is 2 * exp2((word - 0x40000)/16384) exactly - the sine is
         taken out of the word by tvf_word_to_hz and put straight back -
         and at any other host rate it is the same analog corner retuned
         to that rate, which is the only part of this that is a choice. */
      double rate = user ? *(const double *)user : kNativeRate;
      double cutoff = tvf_word_to_hz(wordInt);
      double nyquist = rate * 0.5;
      if (cutoff > nyquist * 0.99)
        cutoff = nyquist * 0.99;
      g = 2.0 * std::sin(3.14159265358979323846 * cutoff / rate);
      state->memo_word = wordInt;
      state->memo_g = g;
      state->memo_valid = true;
    }
  }
  /* Damping from the register the ROM supplies. `07_synthesis/tvf.md`:
   * the companion table at 0x78902 is exactly index * 512, expanded left
   * 2 into TVF-Q current and left 4 into TVF-Q target, and the target is
   * shifted right two bits before use - the target carries two extra
   * fraction bits, so current and target name the same value and Q does
   * not move after note-on. Approaching current toward target as if they
   * shared units ramped Q fourfold over 220 ms and opened every note with
   * a +6 dB peak at the cutoff, which set the drum path 200 Hz too bright
   * (`M-105`). The fixed-tuple path's TVF-Q current 0x20000 is what index
   * 64 produces, so 64 is the neutral resonance; the index runs opposite
   * to the player's setting, so a low index is a high Q. */
  double damping = (double)registers->resonance_current / kQUnity;
  /* The register runs to index 127, which asks for 3.97. The topology is
     stable for any positive damping, so the only bound needed is one that
     keeps the denominator away from zero; refusing the overdamped end
     flattened 13 components onto the neutral value. */
  if (damping < 0.05)
    damping = 0.05;
  else if (damping > 4.0)
    damping = 4.0;
  /* The two poles are realised with forward-Euler integrators, the
     topology the chip's limit table names (see kLimitTable) and the one
     libEmuSC's SC-55 path already runs in svf.cc. The trapezoidal form
     this replaced is a bilinear transform: it leaves a double zero at
     Nyquist, so the two poles the ROM asks for rolled off like three near
     the top of the band - 0.5 dB darker than its own analog prototype at
     twice the corner for a tone cut off at 1.6 kHz, 3.0 dB at 3.7 kHz and
     7.5 dB at 5.3 kHz. Forward Euler has no zero but the one sample of
     delay, and its error runs the other way. */
  unsigned section;
  float signal = input;
  float high = 0.0f;
  float band = 0.0f;
  float low = 0.0f;
  for (section = 0; section < SC88_TVF_SECTIONS; ++section) {
    float d = section == 0 ? (float)damping : 2.0f;
    float f = (float)g;
    float sb = section == 0 ? state->integrator_band
                            : state->section_band[section];
    float sl = section == 0 ? state->integrator_low
                            : state->section_low[section];
    /* The chip's own limit table keeps f*f + f*d at or below 2, which is
       well inside this bound; the bound is here because the damping floor
       above and a host sample rate other than the chip's are not the ROM's
       doing and must not be able to put a pole outside the unit circle.
       Run in float: the persistent state either side of this loop is
       already float, and the section arithmetic itself only ever differs
       from a double computation by <=1 LSB of 16-bit output, on 0.045% of
       samples in a 137s SC-88 reference render - below the noise floor of
       the 16-bit format this ships as. */
    float bound = 0.99f * (std::sqrt(d * d + 4.0f) - d);
    if (f > bound)
      f = bound;
    low = sl + f * sb;
    high = signal - low - d * sb;
    band = sb + f * high;
    if (section == 0) {
      state->integrator_band = band;
      state->integrator_low = low;
    } else {
      state->section_band[section] = band;
      state->section_low[section] = low;
    }
    /* XP bits 10..11 carry a type code - 0 on 1193 components, 1 on 12,
       2 on 38 - and the name binding proposed from JV-1080
       documentation is LPF/BPF/HPF for 0/1/2. Code 2 takes the
       high-pass output, which the SC-88's own archive recordings show
       directly.

       Breath Noise is the case that cannot be read two ways: its one
       component is code 2 with a static 2.7 kHz cutoff and no envelope,
       and it is broadband, so the filter's shape is the tone. Against
       the archive recording at C4, level-matched over 17 log bands, the
       high-pass output lands within 1.1 dB from 256 Hz to 7.2 kHz. The
       same render with the filter removed is 36 dB too loud at 256 Hz
       and 22 dB at 590 Hz: the hardware has less low end than the raw
       sample, which neither a low-pass nor a bypass can produce, and
       the low-pass output is 20 dB short at 8.9 kHz on top of that.
       Seashore, also code 2, moves from 20.0 to 1.6 dB median band
       error at the same key. In the 63-note set the Fiddle goes from
       19.19 to 1.75 dB and Halo Pad from 8.34 to 2.24.

       The earlier reading - every code takes the low-pass output -
       rested on M-100, which put the Fiddle's cutoff at 8.1 kHz, where
       a high-pass would indeed have removed its fundamental. The
       firmware-exact cutoff law puts that same component at 439 Hz at
       C5, where a two-pole high-pass leaves the 523 Hz fundamental at
       +1.0 dB and everything above it flat. M-100's measurement stands;
       what it refutes does not survive the law it was read under.

       This also settles the negative-mode tuple, which installs type
       code 2 with a cutoff word of zero: a high-pass at zero passes
       everything, so `fixed_tuple` returning the input untouched is the
       type code's own behaviour rather than an assumption about it.

       Code 1 keeps the low-pass output. No component in any archive
       recording carries it - the only one in the GM bank is Slap Bass
       2, which is not in the single-note set - so band-pass is
       unverified here and is not adopted on the strength of the
       binding alone. */
    signal = ((registers->filter_select >> 10) & 3u) == 2u ? high : low;
  }
  return signal;
}

}}  // namespace EmuSC::Xp
