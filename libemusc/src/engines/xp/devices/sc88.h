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
 *  Most facts live in struct XpDeviceProfile, which is in profile.h because
 *  it is the engine's vocabulary type rather than this device's - see there
 *  for the struct and the injection mechanism (xp_rom::profile,
 *  xp_engine::profile, xp_profile()). A handful of constants stay as
 *  compile-time values below instead: their readers are per-sample hot paths
 *  with no rom/profile parameter in the call chain, or they size a fixed C
 *  array. The reason is written next to each, the same way the older
 *  engine's DeviceProfile::MAX_PARTIALS exception is.
 */
#ifndef EMUSC_XP_DEVICES_SC88_H
#define EMUSC_XP_DEVICES_SC88_H

#include "profile.h"

#include <stddef.h>
#include <stdint.h>

struct xp_rom;

#ifdef __cplusplus
extern "C" {
#endif

/* This device's control ROM image size. Also XpDeviceProfile::romSize
   below, which is what the engine reads; the compile-time copy sizes the
   stack/heap buffers the test suite fills with a synthetic image of this
   device. */
inline constexpr unsigned XP_CONTROL_ROM_SIZE = 0x80000u;

/* The word at ROM `71c7`, the exponential family's entry at rate index 2,
   and tva_curve_decode's fixed result for it (linear false, rate 679/64 -
   a power-of-two divisor, so this is that exact double, not an
   approximation). Stays compile-time rather than an XpDeviceProfile field:
   its one reader, xp_static_gain_progress() below, is a static inline
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

extern const struct XpDeviceProfile SC88_PROFILE;

#ifdef __cplusplus
}
#endif

#endif
