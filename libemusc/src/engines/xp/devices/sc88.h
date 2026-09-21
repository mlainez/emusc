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
 *  Most facts live in struct XpDeviceProfile, injected at runtime through
 *  sc88_rom::profile / sc88_engine::profile - the same shape as the older
 *  Part/Note engine's DeviceProfile, selected once and read everywhere
 *  through a pointer rather than hardcoded in generic code. A handful of
 *  constants stay as compile-time values below instead: they size this
 *  engine's own fixed C arrays, which requires the value to be known at
 *  compile time wherever it is used, not just at one definition site. The
 *  older engine has the identical exception (DeviceProfile::MAX_PARTIALS) -
 *  this isn't a compromise unique to this engine.
 */
#ifndef EMUSC_XP_DEVICES_SC88_H
#define EMUSC_XP_DEVICES_SC88_H

#include <stddef.h>
#include <stdint.h>

struct sc88_rom;

#ifdef __cplusplus
extern "C" {
#endif

/* This engine's own fixed array sizes and hard ceilings: concurrently
   active slots, notes and parts, tone components per note, wave chips
   and banks, reverb buffers/taps, output filter sections, TVF sections,
   the per-tone controller matrix, and the two record sizes used to size
   a stack buffer in the test suite. Regardless of which device's default
   polyphony is in effect (see XpDeviceProfile::defaultMaxVoices) - these
   are the engine's, not the device's. */
inline constexpr unsigned XP_ENGINE_SLOT_COUNT = 64u;
inline constexpr unsigned XP_ENGINE_NOTE_COUNT = 64u;
inline constexpr unsigned XP_ENGINE_PART_COUNT = 32u;
inline constexpr unsigned XP_MAX_TONE_COMPONENTS = 2u;
inline constexpr unsigned XP_WAVE_CHIP_COUNT = 4u;
inline constexpr unsigned XP_WAVE_BANK_COUNT = 8u;
inline constexpr unsigned XP_MIDI_PORT_COUNT = 2u;
inline constexpr unsigned XP_MATRIX_SOURCE_COUNT = 6u;
inline constexpr unsigned XP_MATRIX_DEST_COUNT = 11u;
inline constexpr unsigned XP_REVERB_BUFFERS = 12u;
inline constexpr unsigned XP_REVERB_TAPS = 8u;
inline constexpr unsigned XP_REVERB_HALF_BUFFERS = 4u;
inline constexpr int XP_OUTPUT_HOLD_TAPS = 31;
inline constexpr int XP_OUTPUT_ANALOG_SECTIONS = 5;
inline constexpr int XP_OUTPUT_MAX_SECTIONS = 4;
inline constexpr unsigned XP_TVF_SECTIONS = 1u;
inline constexpr unsigned XP_DRUM_FIELDS = 10u;
inline constexpr unsigned XP_CONTROL_ROM_SIZE = 0x80000u;

/* The word at ROM `71c7`, the exponential family's entry at rate index 2,
   and tva_curve_decode's fixed result for it (linear false, rate 679/64 -
   a power-of-two divisor, so this is that exact double, not an
   approximation). Stays compile-time rather than an XpDeviceProfile field:
   its one reader, sc88_static_gain_progress() below, is a static inline
   hot-path function taking only a period fraction, with no rom/profile
   parameter to thread one through without touching the whole per-sample
   render call chain. */
inline constexpr unsigned XP_STATIC_AMPLITUDE_CURVE_WORD = 0x02a7u;
inline constexpr double kXpStaticAmplitudeRate = 679.0 / 64.0;

/* The largest legal LFO phase increment, and the slewed-random waveform's
   fixed per-service step. Stay compile-time rather than XpDeviceProfile
   fields: their readers (lfo_effective_increment(), lfo_phase_advance(),
   lfo_slew_random()) operate on plain phase/increment values with no
   rom/profile parameter - unlike lfo_table_sample()/lfo_waveform(), which
   do take rom and read the rest of this device's LFO facts through it. */
inline constexpr uint16_t kXpLfoMaxIncrement = UINT16_C(0x28f6);
inline constexpr int32_t kXpLfoSlewStep = INT32_C(0x1c2);

/* [FW-EXACT] TVF-Q current is the companion word << 2, i.e.
   resonance_index << 11, and one unit of damping is 131072 - that is,
   q = resonance_index / 64.

   The scaling is fixed by the chip's own limit table; see the note on
   XpDeviceProfile::limitTable. Read with q = index/64 that table is
   exactly f*f + f*q = 2 on all 128 entries; read with index/32 it spans
   2.000..3.458 and with index/128 1.270..2.000. The value libEmuSC's
   SC-55 path uses (svf.cc's set_resonance, q = resonance/64, which the
   SC-55's own two stability tables fix to a rounding unit in P-0130 and
   P-0131) is the value the SC-88's ROM asks for too.

   [FW-EXACT] The TVF-F register is a log-frequency word in the XP pitch
   register's own domain. `07_synthesis/pitch.md` has the pitch word at
   16384 units per octave, 18 bits, unity playback at 0x38000; routine
   67a8 forms TVF-F as the same 18-bit high/low pair in the same scratch
   tuple with the same interpolation word 0x4100, and the base table at
   0x78702 steps by exactly 16384/12 per index once expanded - one
   semitone per index. That is the slope. The anchor - which frequency a
   register value names - is the ROM's too, and the limit table fixes it
   in integer arithmetic.

   The limit table's law is f*f + f*q = 2 (see XpDeviceProfile::limitTable),
   which gives f = sqrt(2) at resonance index 0 and f = 1 at index 64. With
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
   is recovered as well: see XpDeviceProfile::limitTable.

   Stay compile-time rather than XpDeviceProfile fields: their readers
   (tvf_word_to_hz(), tvf_audio_process_provisional()) are per-sample hot
   paths with no rom/profile parameter to thread one through without
   touching the whole render call chain - the same exception as
   kXpStaticAmplitudeRate above. */
inline constexpr double kXpTvfQUnity = 131072.0;
inline constexpr double kXpTvfOctaveUnits = 16384.0;
inline constexpr uint32_t XP_TVF_NYQUIST_WORD = 0x40000u;

/* Every other SC-88 fact: ROM table addresses, measured/fitted values and
   record sizes. See devices/sc88.cc for the values and their provenance
   comments (each field below was a standalone named constant there before
   this struct existed; the comments moved with them). */
struct XpDeviceProfile {
  /* Identification: rom_init() reads these, never a device's own bytes -
     the size, and two 16-byte signatures memcmp'd at offset 0 and at
     directoryBase, matching this data against the incoming ROM image.
     This is the only role these three fields play; everything else in
     the struct is read after identification has already chosen this
     profile. */
  size_t romSize;
  uint8_t identVectors[16];
  uint8_t identFirstDirectory[16];

  unsigned defaultMaxVoices;

  uint32_t envelopeRateTable;
  uint32_t rateScaleTable;
  uint32_t releasePedalTable;

  unsigned toneCommonSize;
  unsigned componentSize;

  uint32_t pointerTableBase;
  uint32_t pointerBankSize;
  uint32_t melodicMapBase;

  uint32_t directoryBase;
  uint32_t directoryEnd;
  uint32_t descriptorBase;
  uint32_t descriptorEnd;

  uint32_t toneBase;
  uint32_t toneEnd;

  uint32_t drumMapBase;
  uint32_t drumPointerTable;
  uint32_t drumKitCount;
  uint32_t drumKitStride;
  uint32_t drumKitBase;

  uint32_t levelTable;
  uint32_t coarseGainTable;
  uint32_t fineGainTable;
  uint32_t ampCurve1Table;
  uint32_t ampCurve0Table;

  uint32_t portamentoRateTable;

  uint32_t baseTable;
  uint32_t limitTable;

  uint32_t rateTable;
  uint32_t delayTable;
  uint32_t sineTable;
  uint32_t table10;
  uint32_t table12;
  uint32_t table14;
  uint32_t table16;
  uint32_t tablePoints;

  uint16_t interpolateBelow;

  uint32_t panTable;
  uint32_t sendTable;

  uint32_t eqLow200;
  uint32_t eqLow400;
  uint32_t eqHigh3k;
  uint32_t eqHigh6k;
  uint8_t eqGainMin;
  uint8_t eqGainMax;

  uint32_t delayCentreTable;
  uint32_t delayRatioTable;
  uint32_t delayMacroTable;
  double delayUnitsPerMs;
  unsigned delayMaxUnits;
  double delayMaxMs;

  uint32_t reverbPointers;
  uint32_t reverbPage;
  uint32_t reverbMacroTable;
  uint16_t reverbAllpassPairA;
  uint16_t reverbAllpassPairB;
  float reverbAllpassG;
  uint32_t reverbImage0Cram;
  uint8_t headWord[XP_REVERB_BUFFERS];
  uint8_t farWord[XP_REVERB_BUFFERS];
  uint8_t tapWord[XP_REVERB_TAPS];
  uint8_t allpassBuffer[8];
  uint8_t tapInstruction[XP_REVERB_TAPS];

  uint32_t chorusMacroTable;
  double chorusMaxMs;

  uint32_t pitchCurveCentre;

  unsigned waveDescriptorSize;
  unsigned waveBankSize;
  unsigned waveChipSize;

  uint8_t waveDataLinePermutation[8];
  uint8_t waveAddressLinePermutation[21];

  double belowPower;
  double partialPower;

  uint8_t selectors[XP_WAVE_BANK_COUNT];
};

extern const struct XpDeviceProfile SC88_PROFILE;

/* Never-null: falls back to SC88_PROFILE, since this engine has exactly
   one device today and several existing tests build a bare struct
   sc88_rom by hand (bypassing rom_init(), so .profile is never set).
   Mirrors ControlRom::device()'s own fallback (the older engine's), not
   its nullable profile(). A second XP-family device would set .profile
   explicitly and this would return that instead. */
const struct XpDeviceProfile *xp_profile(const struct sc88_rom *rom);

#ifdef __cplusplus
}
#endif

#endif
