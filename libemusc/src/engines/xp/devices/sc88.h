/* SPDX-License-Identifier: CC0-1.0 */
/*
 *  Roland SC-88 constants for the XP engine (engines/xp/).
 *
 *  Device-specific facts the generic engine code reads rather than hardcodes,
 *  so that a second device on this engine needs only its own file here. The
 *  top-level devices/sc88.cc carries this device's ROM signature instead;
 *  that file identifies devices in general, while this one is private to
 *  engines/xp/'s own engine.
 *
 *  Plain inline constexpr values, not extern-declared globals: several of
 *  these size the engine's own fixed C arrays, which requires the value to
 *  be a compile-time constant wherever it's used, not just at one
 *  definition site. A header of inline constants gives every consumer that
 *  directly, in both the array-sizing and the ordinary-runtime-value cases,
 *  without needing two different conventions for the two uses.
 */
#ifndef EMUSC_XP_DEVICES_SC88_H
#define EMUSC_XP_DEVICES_SC88_H

#include <stdint.h>

/* The real hardware's polyphony: 64 voices. */
inline constexpr unsigned SC88_DEFAULT_MAX_VOICES = 64u;

/* The envelope rate, key/velocity rate-scale and hold-pedal release-scale
   ROM tables, shared by the pitch, TVF and TVA envelopes alike - the same
   three addresses were independently redeclared identically in all three. */
inline constexpr uint32_t kXpEnvelopeRateTable = 0x1543eu;
inline constexpr uint32_t kXpRateScaleTable = 0x1573eu;
inline constexpr uint32_t kXpReleasePedalTable = 0x78a02u;

/* Control ROM size, and the fixed-size tone/component records rom_open_tone
   and rom_open_component step by. */
inline constexpr unsigned SC88_CONTROL_ROM_SIZE = 0x80000u;
inline constexpr unsigned SC88_TONE_COMMON_SIZE = 34u;
inline constexpr unsigned SC88_COMPONENT_SIZE = 148u;

/* Per-note SysEx overlay fields (rom_open_drum_note_overlaid), and
   sc88_drum_overlay's own array bound. */
inline constexpr unsigned SC88_DRUM_FIELDS = 10u;

/* Melodic lookup: the physical-bank pointer table, its per-bank stride, and
   the map that resolves a map/variation pair to a physical bank
   (rom_select_melodic). */
inline constexpr uint32_t kPointerTableBase = 0x20000u;
inline constexpr uint32_t kPointerBankSize = 384u;
inline constexpr uint32_t kMelodicMapBase = 0x2fc00u;

/* The tone directory and wave descriptor regions (rom_select_zone). */
inline constexpr uint32_t kDirectoryBase = 0x30000u;
inline constexpr uint32_t kDirectoryEnd = 0x3606cu;
inline constexpr uint32_t kDescriptorBase = 0x36100u;
inline constexpr uint32_t kDescriptorEnd = 0x3f714u;

/* The tone data region: common header plus components (rom_open_tone). */
inline constexpr uint32_t kToneBase = 0x40000u;
inline constexpr uint32_t kToneEnd = 0x75000u;

/* Drum kit lookup: map to physical kit index, kit pointer table, and the
   kit records themselves (rom_select_drum). */
inline constexpr uint32_t kDrumMapBase = 0x2fd00u;
inline constexpr uint32_t kDrumPointerTable = 0x2b550u;
inline constexpr uint32_t kDrumKitCount = 24u;
inline constexpr uint32_t kDrumKitStride = 0x50cu;
inline constexpr uint32_t kDrumKitBase = 0x23c30u;

/* The level-word table, converting a 0..127 level - master, secondary,
   part, expression, or a rhythm note's own level - into an attenuation
   (tva_gain_from_headroom_q17). */
inline constexpr uint32_t kLevelTable = 0x14f3eu;
/* Coarse and fine gain tables; their paired 16-bit product converts an
   attenuation into a linear gain. */
inline constexpr uint32_t kCoarseGainTable = 0x1503eu;
inline constexpr uint32_t kFineGainTable = 0x1523eu;
/* Linear (family 1) and exponential (family 0) envelope-rate curve tables,
   selected by the component's own per-stage curve-family bit. */
inline constexpr uint32_t kAmpCurve1Table = 0x1553eu;
inline constexpr uint32_t kAmpCurve0Table = 0x1563eu;

/* Portamento rate table: 128 big-endian 32-bit entries indexed by the raw
   CC5 byte (portamento_rate). */
inline constexpr uint32_t kPortamentoRateTable = 0x78502u;

/* The per-key cutoff (TVF-F) base table: one semitone per index, in the
   same log-sine domain kOctaveUnits/kNyquistWord below decode
   (tvf_word_to_hz). */
inline constexpr uint32_t kBaseTable = 0x78702u;
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
inline constexpr double kQUnity = 131072.0;
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
inline constexpr double kOctaveUnits = 16384.0;
inline constexpr uint32_t kNyquistWord = 0x40000;
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
inline constexpr uint32_t kLimitTable = 0x78802u;

/* Two-pole sections in cascade. The filter's order is not recovered from
   the ROM; the sibling chip measures two-pole on hardware (`M-054`). Three
   sections had been chosen by measurement while the cutoff word was read
   as a sine and sat at 8-10 kHz on every tone, a compensating fit that
   also cut a snare's content above 8 kHz from 30 % to 12 %; with the
   cutoff word read in its own log domain one section measures closest to
   the recordings (`M-105`). Resonance belongs to the first section. */
inline constexpr unsigned SC88_TVF_SECTIONS = 1u;

/* SC88-CTL v1.01 offsets. The two increment tables sit immediately after the
 * 16-entry callback dispatch table at 0x29ba. */
inline constexpr uint32_t kRateTable = 0x29dau;
inline constexpr uint32_t kDelayTable = 0x2adau;

/* The sine table and the four selectable waveform tables (lfo_waveform
   selectors 0x00, 0x10, 0x12, 0x14, 0x16), and their shared point count
   (lfo_table_sample). */
inline constexpr uint32_t kSineTable = 0x1492cu;
inline constexpr uint32_t kTable10 = 0x14524u;
inline constexpr uint32_t kTable12 = 0x14626u;
inline constexpr uint32_t kTable14 = 0x14728u;
inline constexpr uint32_t kTable16 = 0x1482au;
inline constexpr uint32_t kTablePoints = 129u;

/* The largest legal phase increment; a sum above this saturates rather than
   wrapping (lfo_effective_increment). */
inline constexpr uint16_t kMaxIncrement = UINT16_C(0x28f6);
/* Below this increment, lfo_table_sample interpolates between adjacent
   table entries instead of reading the nearer one alone. */
inline constexpr uint16_t kInterpolateBelow = UINT16_C(0x0200);
/* The fixed per-service step of the slewed-random waveform
   (lfo_slew_random). */
inline constexpr int32_t kSlewStep = INT32_C(0x1c2);

/* The pan and effect-send gain tables. The effect sends do NOT read the pan
   table. They have their own, and it is a different shape: 128 words at
   0x15eb6, indexed by the control value WHOLE rather than by `value - 1`,
   and exactly `64 * floor((value * 512 + 63) / 127)` on every one of the
   128 - a linear Q15 gain with 0x8000 for unity (`P-xxxx`). The firmware
   reaches it from a different routine than the pan pair does. The two
   tables agree at only three points, so reading one for the other is
   audible: it opens the send 1.4 dB too far around the middle of the
   range, and at control 1 the pan table's first word is 0, which closes a
   send the chip would have left open. */
inline constexpr uint32_t kPanTable = 0x15db6u;
inline constexpr uint32_t kSendTable = 0x15eb6u;

/* The EQ's four 25-record blocks, six bytes each, indexed by gain - 0x34. */
inline constexpr uint32_t kEqLow200 = 0x1609cu;
inline constexpr uint32_t kEqLow400 = 0x16132u;
inline constexpr uint32_t kEqHigh3k = 0x161c8u;
inline constexpr uint32_t kEqHigh6k = 0x1625eu;
inline constexpr uint8_t kEqGainMin = 0x34u;
inline constexpr uint8_t kEqGainMax = 0x4cu;

inline constexpr uint32_t kDelayCentreTable = 0x15fb4u;
inline constexpr uint32_t kDelayRatioTable = 0x165cau;
inline constexpr uint32_t kDelayMacroTable = 0x158beu;
/* The delay memory counts in 1/32 ms in this path, and both the centre
   time and the side taps cap at 0x7d00 above the 0x8000 base: one second. */
inline constexpr double kDelayUnitsPerMs = 32.0;
inline constexpr unsigned kDelayMaxUnits = 0x7d00u;
inline constexpr double kDelayMaxMs = 1000.0;

/* SC88-CTL v1.01. The character pointers are offsets **within the 0x10000
 * page**; read as absolute addresses they land in unrelated code and decode
 * to plausible nonsense, which is the trap recorded in `M-008`. */
inline constexpr uint32_t kReverbPointers = 0x1595eu;
inline constexpr uint32_t kReverbPage = 0x10000u;
/* The eight macro presets, 8 bytes each, read by SC88-CTL handler 0x3388 and
 * by the power-on loader at 0x4476. The reset image at ROM 0x13104 - the
 * patch common block whose first sixteen bytes are the default patch name -
 * carries macro 4 and that macro's own seven bytes, so a GS reset is this
 * table's Hall 2 row and not a separate set of defaults. */
inline constexpr uint32_t kReverbMacroTable = 0x1583eu;
inline constexpr uint16_t kReverbAllpassPairA = 0x3000u;   /* -0.5 under the XP law */
inline constexpr uint16_t kReverbAllpassPairB = 0x1000u;   /* +0.5 */
inline constexpr float kReverbAllpassG = 0.5f;
/* The single-module DSP image and its coefficient RAM. CRAM[i] is the
 * coefficient of PRAM[i] - no field selects it (`M-173`) - so the gain of a
 * tap is the word at the tap's own instruction index. */
inline constexpr uint32_t kReverbImage0Cram = 0x78b02u + 0x480u;

/* The reverb's delay-line graph, read out of the DSP program in the control
 * ROM (`M-173`, scdb `08_effects/dsp_program.md`). Twelve ERAM buffers,
 * eight output taps:
 *
 *   B0 B1 B2 B3   four series allpasses at g = 0.5, the input diffuser
 *   B4 B5 B6 B7   tank half 1: allpass, delay, allpass, delay
 *   B8 B9 B10 B11 tank half 2: allpass, delay, allpass, delay
 *   eight taps    read inside the tank, two per pair of program slots
 *
 * The twelve buffer heads are the twelve instructions with the ERAM write
 * enable (bit 24) set; the twenty reads have it clear, and each buffer's
 * far end sits one address below the next head. The eight taps are reads
 * that land inside a buffer rather than at its end, and they are the early
 * field. */
inline constexpr unsigned SC88_REVERB_BUFFERS = 12u;
inline constexpr unsigned SC88_REVERB_TAPS = 8u;
inline constexpr unsigned SC88_REVERB_HALF_BUFFERS = 4u;

/* Which of the record's 32 delay-memory addresses is which. The record
 * always carries them in program order of the instructions they patch -
 * writes at 45 49 53 57 65 67 69 71 81 83 85 87, far-end reads at
 * 41 43 47 51 59 61 63 55 75 77 79 73 and taps at 89 91 93 95 97 99 101 103
 * for the first module, the same sequence shifted for the other two layouts.
 * These three tables are that order read back as buffer roles. */
inline constexpr uint8_t kHeadWord[SC88_REVERB_BUFFERS] = {
  0u, 2u, 4u, 6u, 8u, 10u, 14u, 16u, 20u, 22u, 26u, 28u};
inline constexpr uint8_t kFarWord[SC88_REVERB_BUFFERS] = {
  1u, 3u, 5u, 7u, 9u, 13u, 15u, 19u, 21u, 25u, 27u, 31u};
inline constexpr uint8_t kTapWord[SC88_REVERB_TAPS] = {
  11u, 17u, 23u, 29u, 12u, 18u, 24u, 30u};
/* The eight coefficient pairs belong to the eight buffers whose write
 * instruction carries +0.5, in the same order the record lists them. */
inline constexpr uint8_t kAllpassBuffer[8] = {0u, 1u, 2u, 3u, 4u, 6u, 8u, 10u};
/* PRAM indices of the eight taps in the single-module image. */
inline constexpr uint8_t kTapInstruction[SC88_REVERB_TAPS] = {
  131u, 133u, 135u, 137u, 139u, 141u, 143u, 145u};

/* The eight macro presets, 8 bytes each, read by SC88-CTL handler 0x3400 and
 * by the power-on loader at 0x44a8. The reset image at ROM 0x13104 carries
 * macro 2 and that macro's own eight bytes, which are the manual's printed
 * chorus defaults byte for byte. */
inline constexpr uint32_t kChorusMacroTable = 0x1587eu;
/* `3*p` reaches 381 samples and the sweep is added on top, so the chorus
   line is sized for the longest delay the register can ask for plus the
   deepest sweep, with a margin for interpolation. Both the chorus and
   delay lines count in samples at 32 kHz, the rate the sound chip runs its
   lines at, so every recovered length is converted from that. */
inline constexpr double kChorusMaxMs = 64.0;

/* The output stage's fixed sizes: 31-tap converter hold FIR, five analog-
   board sections, four EQ/response biquad sections. */
inline constexpr int SC88_OUTPUT_HOLD_TAPS = 31;
inline constexpr int SC88_OUTPUT_ANALOG_SECTIONS = 5;
inline constexpr int SC88_OUTPUT_MAX_SECTIONS = 4;

/* Centre of the 255-word bipolar pitch-control curve at 0x78304..0x78502,
 * indexed -127..127 about this address (`02_rom/tables.md`). */
inline constexpr uint32_t kPitchCurveCentre = 0x78402u;

/* The word at `71c7`, the exponential family's entry at rate index 2. */
inline constexpr unsigned SC88_STATIC_AMPLITUDE_CURVE_WORD = 0x02a7u;
/* tva_curve_decode's result for SC88_STATIC_AMPLITUDE_CURVE_WORD never
   changes (linear false, rate 679/64 - a power-of-two divisor, so this is
   that exact double, not an approximation of it). */
inline constexpr double kRate = 679.0 / 64.0;

inline constexpr unsigned SC88_WAVE_BANK_COUNT = 8u;
inline constexpr unsigned SC88_MAX_TONE_COMPONENTS = 2u;

inline constexpr unsigned SC88_WAVE_DESCRIPTOR_SIZE = 20u;
inline constexpr unsigned SC88_WAVE_BANK_SIZE = 0x100000u;
inline constexpr unsigned SC88_WAVE_CHIP_SIZE = 0x200000u;

/* The wave ROM board's bit-permutation tables (wave_descramble_chip). */
inline constexpr uint8_t kWaveDataLinePermutation[8] = {
  2u, 0u, 4u, 5u, 7u, 6u, 3u, 1u};
inline constexpr uint8_t kWaveAddressLinePermutation[21] = {
  0u, 4u, 2u, 3u, 1u, 13u, 7u, 12u, 5u, 10u, 16u,
  9u, 6u, 8u, 14u, 17u, 11u, 15u, 18u, 19u, 20u};

/* -15 dB and -7 dB as power ratios; see wave_loop_reads_double's own
   comment for the measured gaps these sit in the middle of. */
inline constexpr double kBelowPower = 0.0316227766016838;
inline constexpr double kPartialPower = 0.199526231496888;

/* The wave-bank selector byte for each of the eight banks across the four
   wave ROM chips. */
inline constexpr uint8_t kSelectors[SC88_WAVE_BANK_COUNT] = {
  0x00u, 0x01u, 0x10u, 0x11u, 0x20u, 0x21u, 0x30u, 0x31u};
inline constexpr unsigned SC88_WAVE_CHIP_COUNT = 4u;
inline constexpr unsigned SC88_MIDI_PORT_COUNT = 2u;
inline constexpr unsigned SC88_MATRIX_SOURCE_COUNT = 6u;
inline constexpr unsigned SC88_MATRIX_DEST_COUNT = 11u;

/* This engine's own fixed array sizes and hard ceilings for concurrently
   active slots, notes and parts, regardless of which device's default
   polyphony is in effect (see SC88_DEFAULT_MAX_VOICES above). */
inline constexpr unsigned SC88_ENGINE_SLOT_COUNT = 64u;
inline constexpr unsigned SC88_ENGINE_NOTE_COUNT = 64u;
inline constexpr unsigned SC88_ENGINE_PART_COUNT = 32u;

#endif
