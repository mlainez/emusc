/*
 *  This file is part of libEmuSC, a Sound Canvas emulator library
 *
 *  Roland JV-880 device profile.
 *
 *  Data only. Every offset carries the evidence that fixed it: an offset without
 *  provenance is a guess, and a guess that happens to be in range is the most
 *  expensive kind. See device_profile.h for what each field means.
 */

#include "../device_profile.h"

namespace EmuSC
{


// Curves read straight out of the JV-880's ROM. Each was found by matching the
// SC-55's own table numerically and then checked for shape, because a numerical
// match is a lead and only the shape check makes it a finding.
//
// PitchCoarseExp is deliberately absent. The match at 0x06b2c passes the rise
// check - the shape is right - but it starts at 29794 where the SC-55's starts at
// 32768, and that table's first entry IS unity: 29794/32768 is 0.9092, or -1.65
// semitones applied to every note on every part. The open hat measured -1.66
// semitones against the reference, which is that number. A shape check does not
// catch a wrong base, so the anchor is checked too.
//
// Also absent, and why: TVAPanpot (0x3e946), TVFEnvScale (0x3fc79) and
// TVFCutoffVSens (0x02d15) all failed the rise check - 11, 4 and 4 inversions in
// curves that must be monotonic. They keep their fitted SC-55 shape instead of a
// wrong reading.
static const RomLookupTable JV880_LOOKUP_TABLES[] = {
  // 0x3e9c4 is the wrong data and the check now says so. The SC-55's own
  // TVFResonanceFreq, in its CPU ROM at 0x7714, is flat 127 across the first
  // ninety entries and then strictly falling to 0 - 115 falls, 0 rises. This
  // window has 102 rises and 2 falls: the opposite shape, and a curve that rises
  // where the reference falls boosts where it should attenuate. Marked Falling so
  // the reading is REJECTED and the fitted SC-55 curve stands instead, which is
  // a guess but a guess of the right shape. The JV's own table is not identified:
  // the best strictly non-increasing 256-byte candidate is 0x05290 at deviation
  // 0.1782, not good enough to claim.
  { RomLookup::TVFResonanceFreq,     0x3e9c4, 256, 1, Monotonic::Falling },
  // Was 0x054be, which is 46 bytes INSIDE the table that starts at 0x05490 -
  // entered with the shape check off. The region maps cleanly by its own
  // boundaries, every table starting at 255 and ending at 0: 128-byte tables at
  // 0x05490, 0x05510 and 0x05590. 0x05510 is the one whose shape matches the
  // SC-55's TVFResonance - strictly falling, 0 rises and 123 falls against its
  // 0 and 115, deviation 0.0810 once normalised - and it is the right length for
  // a resonance of 0..127.
  { RomLookup::TVFResonance,         0x05510, 128, 1, Monotonic::Falling },
  { RomLookup::EnvSegmentCurve,      0x055f5,   9, 1, Monotonic::Unchecked },
  { RomLookup::TVAPanKeyFollow,      0x3e931, 128, 1, Monotonic::Unchecked },
  { RomLookup::TVALevelIndex,        0x05590, 128, 1, Monotonic::Falling },
  { RomLookup::EnvTimeKeyFollowSens, 0x3ff49,  21, 1, Monotonic::Rising },
  { RomLookup::LFOSine,              0x04edf, 130, 1, Monotonic::Unchecked },

  // The LFO rate table, ROM2 0x4C58: 128 big-endian words rising 128 to 16127
  // on a constant ratio, doubling every 19 steps - verified here at v = 32, 64
  // and 96 alike, so it is a clean exponential and Rising is a real check.
  // scdb's lfo.md has it FW-EXACT as `inc = LFO_RATE[rate]` reached from
  // descriptor 0x18, and the lane that identified it found its only four
  // readers are all the same LFO phase accumulator.
  //
  // This port filled LFORate with ZEROS, so the JV's LFO never advanced and
  // every LFO term - pitch, TVF and TVA - contributed nothing. Measured
  // consequence (D-38): SAW Lead carries 10.4 of 1-3 Hz modulation energy
  // where the reference carries 27.8. Note 0x4C58 was once misread as the
  // envelope TIME table and corrected; this is the same address in its real
  // role, not a revival of that mistake.
  { RomLookup::LFORateTable,         0x04c58, 128, 2, Monotonic::Rising },

  // Envelope phase times: 128 big-endian 16-bit entries, 128 rising to 16127 on
  // a constant ratio of 1.0384 - a doubling every ~18 steps. The same shape and
  // magnitude as the SC-55's envelopeTime, which is what makes it recognisable:
  // sampled every 16 it reads 128, 235, 433, 796, 1464, 2693, 4953, 9109 against
  // the SC-55's 0, 159, 453, 994, 1990, 3827, 7211, 13448.
  // The envelope segment times, in MILLISECONDS, 0 to 30000 over 128 entries
  // with entry 0 meaning "skip this segment instantly" (P-0383). This port had
  // been reading 0x04c58 for this, which is the LFO RATE table - nothing in the
  // firmware indexes it for an envelope - and whose entry 0 of 128 became a
  // ~400 ms attack where the machine plays none.
  // The envelope segment times: rom2 0x5160, 128 entries rising 0 to 30000, in
  // MILLISECONDS, entry 0 meaning the segment is skipped outright (P-0383).
  // 0x04c58, which this port read before, is the LFO RATE table - the lane
  // found its only four readers and all four are the same LFO phase
  // accumulator.
  //
  // Used RAW, because this engine's envelope-time unit is already the
  // millisecond. tva.cc sets _phaseStepSize = (8 << 16) / D and runs
  // _phasePosition from 0 to 0xffff, so a segment takes D/8 control updates,
  // and a control update is 8 ms (256 samples at the internal 32 kHz) - so the
  // segment lasts exactly D milliseconds. The Sound Canvas's own table says the
  // same thing: its index 0 is 125 and its index 127 is 25148, which are 125 ms
  // and 25 s. Converting these milliseconds into "ticks" by dividing by 8 made
  // every note decay eight times too fast, which the owner heard immediately as
  // notes far shorter than the machine's.
  { RomLookup::EnvelopeTime,         0x05160, 128, 2, Monotonic::Rising },

  // The level law's own curve (P-0381), found in the firmware rather than fitted.
  // 128 big-endian words rising 0 to 65535, with its own precomputed slope table
  // beside it at 0x6360 - 127 of 127 entries within one of the exact deltas,
  // which is what identifies it. Not exponential: 0.22 dB per step at the top,
  // over 0.5 dB below index 16, 45.6 dB span. Every attempt to fit a constant
  // dB-per-step for this failed because there is not one.
  { RomLookup::JVLevel,              0x06260, 128, 2, Monotonic::Rising },

  // The SECOND level curve, and it is not the same table. ROM1 0x4451 converts
  // the static level product through 0x6260 and writes the result's high byte to
  // the chip's F016; ROM1 0x44e7 converts the running TVA envelope value through
  // THIS pair and writes its high byte to F018 (ROM1 0x4583/0x4593 read
  // 0x6160/0x6060, the store is at ROM1 0x3da6, the register writes at
  // 0x38a4-0x38b8). The two registers multiply. 0.2945 dB per step against
  // 0x6260's 0.2297, both topping out at 65535: 0 rises inverted over 128
  // entries, identical in ROM2 v1.0.0 and v1.0.1. Entry 127 >> 8 is 255, which
  // is the +5.99 dB that was missing from every JV voice (P-0398).
  { RomLookup::JVLevelEnv,           0x06060, 128, 2, Monotonic::Rising },

  // The slope half of the envelope pair, read by the same idiom at ROM1 0x4583.
  // Monotonic::Unchecked: it is a table of per-step DELTAS of the curve above,
  // not a curve, so a rising check would mean nothing - and the deltas of an
  // exponential do rise, which would make the check pass for the wrong reason.
  { RomLookup::JVLevelEnvSlope,      0x06160, 128, 2, Monotonic::Unchecked },

  // Seven velocity curves of 128 bytes at 0x5390 + c * 0x80, each falling 255 to
  // 0, selected per tone by record byte +55 & 7 for the TVF and +71 & 7 for the
  // TVA - the descriptor table's own velocity-curve fields. Curve 4 is 0x05590,
  // which this port had been reading as a general level index table.
  { RomLookup::JVVelCurves,          0x05390, 896, 1, Monotonic::Unchecked },

  // The time-variant filter's own tables (P-0390). These are not fitted from
  // the SC-55's and not matched numerically against them: the firmware's cutoff
  // routine reads each one at the offset given here, and each reproduces a
  // closed form exactly, which is a far stronger check than a shape test.
  //
  //   ExpCoarse   256 * 2^(256 * signed(i) / 1200)        256/256 exact
  //   ExpFine     65536 * (2^(i/1200) - 1)                256/256 exact
  //   DampSoft    floor(16384 * 2^(-r/64))                128/128 exact
  //   DampHard    floor(16384 * 2^(-r/32)) = DampSoft[2r]  128/128 exact
  //   LimitHard   = LimitSoft[2r] for every r < 64        no exceptions
  //   Base        0x100 * (cutoff + 1) below 32, then a curve with no closed
  //               form, reaching 0xFFFF at 127. The values are the datum.
  //   CutoffKF    the manual's published key-follow list, verbatim, as CENTS
  //               PER SEMITONE: -100 -70 -50 -30 -10 0 +10 +20 +30 +40 +50
  //               +70 +100 +120 +150 +200
  //
  // The two LIMIT/DAMP pairs also satisfy F^2 + F*Q1 = 2 across all 256 rows,
  // to within 2.4e-4, with F = LIMIT/2^15 and Q1 = DAMP/2^14 - the pole angle
  // held at exactly fs/4. That identity is what pins the two register scales,
  // and it is why the words can be handed to svf.h as coefficients rather than
  // converted to indices.
  //
  // Monotonic::Unchecked on all eight, deliberately: the shape check exists for
  // a table found by numerical similarity, and none of these was. ExpCoarse is
  // not monotonic in its index at all - the index is SIGNED, so entry 128 is
  // -128 coarse units and the table falls at the wrap.
  { RomLookup::JVTvfExpCoarse,       0x05c60, 256, 2, Monotonic::Unchecked },
  { RomLookup::JVTvfExpFine,         0x05e60, 256, 2, Monotonic::Unchecked },
  { RomLookup::JVTvfLimitSoft,       0x0668a, 128, 2, Monotonic::Unchecked },
  { RomLookup::JVTvfDampSoft,        0x0678a, 128, 2, Monotonic::Unchecked },
  { RomLookup::JVTvfLimitHard,       0x0688a, 128, 2, Monotonic::Unchecked },
  { RomLookup::JVTvfDampHard,        0x0698a, 128, 2, Monotonic::Unchecked },
  { RomLookup::JVTvfBase,            0x06a8a, 128, 2, Monotonic::Unchecked },
  // Unchecked, and this one is a trap worth naming: the values are SIGNED and
  // the shape check compares them unsigned, so -100 reads as 65436 and the
  // table looks like it rises to entry 4 and then collapses to 0. Marked
  // Rising it would be REJECTED and left as zeros - a silent no-key-follow.
  // The check it passes instead is the strongest available: it IS the manual's
  // published list, entry for entry.
  { RomLookup::JVTvfCutoffKF,        0x057be,  16, 2, Monotonic::Unchecked },
  // The pan law: 128 big-endian words at ROM2 0x6B8A, packed (L << 8) | R.
  // The candidate at 0x3e946 named above was rejected for failing a RISE
  // check, but that check was the wrong test: this is not a mirrored single
  // ramp. Its centre is (88, 90) of 127, asymmetric, and its power sum
  // L^2 + R^2 spans 1.000 to 1.189 of centre, so it is neither equal-power
  // nor equal-gain and cannot be synthesised. Verified byte-exact against
  // jv880_rom2: 0x7f00 hard left, 0x585a centre, 0x007f hard right.
  { RomLookup::JVPanLaw, 0x6B8A, 128, 2, Monotonic::Unchecked },

  // The chorus type records (P-0394). Three records of five big-endian words,
  // contiguous from 0x49EE, which is also how the firmware reaches them: the
  // effect pointer table at 0x4800 has ELEVEN entries - eight 60-byte reverb
  // records and these three 10-byte ones - and the chorus driver indexes it as
  // @(0x4810, type*2), i.e. entries 8, 9 and 10 (ROM2 0x73AA-0x73B5).
  //
  //   CHORUS1  3d00 1828 3f40 3ff8 0084   window span 184 samples = 5.75 ms
  //   CHORUS2  3d00 2818 3d0e 3ff4 004c               742          23.2 ms
  //   CHORUS3  3d00 003f 3d08 3fce 03b1               710          22.2 ms
  //
  // w2 and w3 are the sweep window's start and end as WORD offsets into the
  // 16384-word effect PSRAM, w4 the sweep-rate base, and w0 - identical in all
  // three, so not a per-type control - the input write pointer. The addresses
  // sit at the TOP of the PSRAM, 0x3D08-0x3FF8, where the reverb uses the
  // bottom. Byte-identical in ROM2 v1.0.0 and v1.0.1.
  //
  // Monotonic::Unchecked, and not from laziness: these are five heterogeneous
  // fields per record, not a curve, so a shape check would mean nothing. What
  // stands in for it is that the three records agree with the driver's own use
  // of them - w3 > w2 in all three, both inside the PSRAM, w0 below both.
  { RomLookup::JVChorusRecords,      0x049ee,  15, 2, Monotonic::Unchecked },

  // The envelope TIME-sense tables (scdb devices/jv880 D-27, FW-EXACT). The
  // first is what the tones' "T1 velocity" and "T4 velocity" nibbles index:
  // sixteen signed words, -4000 -2800 -2000 -1600 -1000 -400 -200 0 +200 +400
  // +1000 +1600 +2000 +2800 +4000 +4000, and ROM1 0x2360 ADDS
  // (d * |velocity - 64|) >> 8 milliseconds to the segment - up to 984 ms
  // either way at the extremes, not a ratio. The second is what "time KF"
  // indexes: sixteen signed bytes +21 +14 +10 +8 +6 +4 +2 0 -2 -4 -6 -8 -10
  // -14 -21 -21, the 1/256-per-semitone rate ROM1 0x22E1 compounds from key
  // 60, so the displayed +100 (index 14, -21) halves T2-T4 per octave up.
  // Entry 7 of both is 0: that is why 7 is the neutral nibble. Both are
  // SIGNED, so Unchecked - the shape check compares unsigned and would reject
  // them the way it nearly rejected JVTvfCutoffKF.
  { RomLookup::JVEnvTimeVelDepth,    0x05260,  16, 2, Monotonic::Unchecked },
  { RomLookup::JVEnvTimeKeyFollow,   0x05280,  16, 1, Monotonic::Unchecked },

  // The TVA attack shape (scdb D-35). ROM1 0x2984-0x2997, the TVA block of the
  // envelope stepper and no other, reads this table for segment 0 only:
  //   r3 = remaining >> 8; dp = 4; r1 = @(0x5290, r3); r1 *= descriptor[+0x15]
  // (descriptor +0x15 is the TVA L1 byte). 256 bytes, 254 at index 0 falling to
  // 0 at index 255, identical in ROM2 v1.0.0 and v1.0.1 (TM-018). Falling is the
  // right check: the counter it is indexed by runs 0xFFFF -> 0, so the level it
  // produces rises. Without it every JV attack longer than one tick was a linear
  // pre-curve ramp through the exponential register curve, i.e. concave, and
  // `Glass Pad`'s 1.45 s attack sat 18 dB under the reference in its first half.
  { RomLookup::JVTvaAttackCurve,     0x05290, 256, 1, Monotonic::Falling },

  // The LFO (scdb devices/jv880/07_synthesis/lfo.md, FW-EXACT, all four read
  // by RTOS task 13 at ROM1 0x0FF6/0x10C6, 0x1309, 0x102F). The rate table is
  // 128 u16 phase increments per 16 ms tick, 128 -> 16127, i.e. 0.122 Hz to
  // 15.37 Hz - the table this port had once mistaken for the envelope times.
  // The offsets are -63 -31 0 +31 +63 -1 0 -128 (the manual's -100..+100 in
  // the first five), signed, hence Unchecked. The four waveforms are 256
  // signed bytes each, +/-64, in the parameter order TRI SIN SAW SQR.
  { RomLookup::JVLfoRate,            0x04c58, 128, 2, Monotonic::Rising },
  { RomLookup::JVLfoOffset,          0x04c52,   8, 1, Monotonic::Unchecked },
  { RomLookup::JVLfoWaves,           0x04d60,1024, 1, Monotonic::Unchecked },

  // LFO -> pitch depth (scdb D-37, TM-036m): 64 u16 indexed by |depth| at
  // ROM1 0x47AB, sign restored after; rising 0 -> 2418, no closed form.
  // Multiplied by the LFO word (sample << 8, +/-0x4000 full scale) and the
  // high word taken, so the peak deviation is table/4 cents: 604 at 63.
  { RomLookup::JVLfoPitchDepth,      0x06c8a,  64, 2, Monotonic::Rising }
};

static const RecordRomLayout JV880_RECORDS = {

  // Waveform record table. 129 records of 60 bytes from the very start of the
  // ROM: a 12-byte name, then 11 key zones.
  { 0x000004, 60, 12, 11 },

  // Sample table. 577 records of 18 bytes. +0 is the per-sample attenuation,
  // the field the Sound Canvas describes as "Volume attenuation (0x7f - 0)":
  // range 90..127 across the 577 records, median 127, topping out at exactly
  // 0x7f and never above it, in 21 distinct steps. It had been pinned at 0x7f -
  // no attenuation at all - as a placeholder.
  { 0x001e40, 18, 0 },

  {
    // Patches. Internal at 0x08ce0, then Preset A and Preset B at +0x8000 each,
    // 64 patches of 362 bytes per bank: a 12-byte name, a 26-byte common block,
    // then four 84-byte tones.
    0x008ce0, 0x8000, 3, 64,

    // Program change alone reaches Internal 01-64; the presets are behind bank
    // select CC0 = 81. Measured on the reference through its public MIDI input:
    // with no bank select, PC 0 and PC 63 give distinct patches while PC 64 and
    // PC 127 give byte-identical output - the Card bank is absent, so everything
    // above Internal collapses to one fallback. With CC0 = 81, PC 0, 63, 64 and
    // 127 are all four distinct, so that bank spans 128 patches: Preset A in
    // 0-63 and Preset B in 64-127. CC0 values 0, 1, 2, 3 and 80 all behave as no
    // bank select at all.
    //
    // Identity confirmed rather than assumed: CC0 = 81 with PC 0 renders at
    // rms 1636.5 against our own Preset A 01 at 1620.3, a tenth of a decibel
    // apart. The 1284-cent centroid difference is the disabled filter.
    0, 81,

    362, 12,

    // Patch Level, common byte +21. Without it a four-tone patch plays all four
    // at full gain: SAW Lead, which the demo's melody uses, has four tones
    // enabled at level 127 with no velocity split, and rendered 19.6 dB louder
    // than the machine on that channel alone. +21 is the only common byte that
    // FALLS as the tone count rises (correlation -0.300 across 192 patches),
    // which is what a level compensating for layering has to do. Range 44..127,
    // median 118.
    21,

    26, 84, 4,

    {
      // +0 tone on/off, +1 waveform index.
      0, 1,

      // Velocity range, +3 and +4. Sounding every tone regardless of velocity is
      // why layered patches were too loud and wrong in timbre: 22% of tones are
      // velocity-limited layers meant to sound only part of the time, and the
      // Internal bank averages 3.41 tones per patch against Preset A's 2.44 -
      // which is exactly why Internal measured 5 dB louder than Preset A. The
      // pair passes the structural test on all 539 enabled tones: lower is never
      // above upper.
      3, 4,

      // Coarse tune, +37, in SIGNED semitones. Not applying it left 209 of the
      // 539 enabled tones an octave high and 29 of them two octaves high -
      // "Slap !!!" carries -12, which is why the demo's bass peaked at 220 Hz
      // where the machine peaks at 110. The column reads as musical intervals
      // and nothing else: -24 on 29 tones, -12 on 209, 0 on 230, +12 on 34,
      // +24 on 3.
      37,

      // +38 fine tune, signed like the coarse tune. Non-zero on a great many
      // tones - THE STRINGS carries -3, -3, +1, +6 across its four - and holding
      // it at neutral is why the owner hears multi-tone patches as having "a
      // sound component that is out of tune".
      38,

      // +40 low nibble indexes the manual's key-follow percentages
      // (-100..+200 in sixteen steps, the list verified verbatim in the ROM at
      // 0x057be). +100% on 507 of the 539 enabled tones, which is what the
      // hardcoded 0x4a approximated - but Jazz Organ 3's third tone is +10%, a
      // near-fixed-pitch component that we were making track the keyboard.
      40,

      // +68 pan. The map gives "0 - 128 (L64 - 63R, RND)", so 128 is RANDOM, not
      // an out-of-range 127. THE STRINGS' four tones read 128, 0, 128, 127 - two
      // random, one hard left, one hard right - which is the stereo movement the
      // owner hears on the machine and not in ours.
      68,

      // Filter cutoff +52 and resonance +53, adjacent as the manual has them
      // (SysEx 0x4A, 0x4B). +52 is confirmed on the reference: driving it 0 to
      // 127 moves that patch's spectral centroid from 99 Hz to 441 Hz, the
      // largest and cleanest swing of any byte tested.
      52, 53,


      // The tone's level, +67 scaled by +81.
      67, 81,

      // TVA envelope: three time/level pairs from +74, then the release at +80.
      74, 75, 76, 77, 78, 79, 80,

      // Effect sends, +82 and +83.
      82, 83,

      // +71 bits 0-2 select which of the seven velocity curves this tone uses,
      // and +72 is its sensitivity, signed -63..+63 (P-0381). A sensitivity of 0
      // means the firmware skips the velocity helper entirely - no velocity
      // effect - which is why reinterpreting the old hardcoded 0 as -64 silenced
      // every instrument.
      71, 72,

      // The filter's remaining fields (P-0390). Filter Mode is +55 bits 3-4 and
      // the census over the 539 enabled factory tones is what confirms the mask:
      // OFF 77 (14.3 %), LPF 445 (82.6 %), HPF 17 (3.2 %), and NOT ONE tone
      // above 2. A wrong mask puts values there, so the census is the falsifier
      // rather than a summary.
      //
      // +53 carries resonance in bits 0-6 and the SOFT/HARD mode in bit 7 -
      // HARD on 58 of the 539, and HARD is exactly "twice the resonance index":
      // LIMIT_HARD[r] == LIMIT_SOFT[2r] for every r below 64, with no
      // exceptions, so Q doubles every 64 steps instead of every 128.
      //
      // +54 bits 0-3 index the key-follow table, which holds the manual's own
      // percentages read as CENTS PER SEMITONE: +100 is the ROM value 100, i.e.
      // literally 1:1 tracking from note 60. 354 of 539 tones sit at 0.
      //
      // +58 is the envelope depth, SIGNED: -40..+63 across the factory set,
      // non-zero on 402 of 539 - so a port that leaves it at 0, as this one
      // did, has a static filter on three quarters of the machine's tones.
      //
      // +59 begins four INTERLEAVED time/level pairs, T1 L1 T2 L2 T3 L3 T4 L4.
      // The first three are the attack and decay segments and L3 is the sustain
      // level; T4/L4 are the release. Interleaved is not the order the Sound
      // Canvas uses and reading it as times-then-levels would swap the two.
      //
      // +32 and +35 are the two LFO -> TVF depths, signed. They are read and
      // applied, but this port's JV LFO rate table is still a placeholder of
      // zeros, so the modulator does not move yet and the term contributes
      // nothing.
      55, 53, 54, 55, 56, 58, 59, 32, 35,

      // Tone Switch is BIT 7 of +0 (firmware descriptor param 0x03: shift 7,
      // mask 0x7f). The same byte carries Wave Group in bits 0-1 and SysEx
      // 0x73 in bit 4, so the whole-byte truthiness test this port used reports
      // a switched-off tone as ON the moment a card or user patch selects an
      // EXP/PCM wave group. On the FACTORY data the two agree exactly - the
      // byte is only ever 0x00 or 0x80, 539 enabled either way, zero phantom
      // tones - so this cannot change factory output and no audible claim is
      // made for it (D-08).
      7,

      // The envelope TIME-sense nibbles (scdb D-27, FW-EXACT, from the Patch
      // Tone descriptor table at ROM2 0x3A524 and the voice-descriptor builder
      // at ROM1 0x4B19-0x4B3A). +73 holds "A-ENV T1 velocity" in bits 0-3 and
      // "A-ENV T4 velocity" in bits 4-7 (SysEx 0x66/0x67); +70 bits 4-7 is
      // "A-ENV time KF" (0x68). +57 and +54 are the TVF's (0x50/0x51, 0x52).
      // 7 is neutral: 693, 700 and 612 of the 768 factory tones sit there.
      0x49, 0x46, 0x39, 0x36,

      // +69 TVA Delay Time; bit 7 set is KEY-OFF (the nibble-pair value 128).
      0x45,

      // The pitch block (scdb D-37, FW-EXACT: descriptor table ROM2 0x3A524,
      // readers ROM1 0x487F / 0x48BF / 0x4A8F-0x4ABC, combine 0x40AB). +0x2C
      // starts T1 L1 T2 L2 T3 L3 T4 L4 interleaved with SIGNED levels; +0x2B is
      // the depth (-12..+12, 100 cents per unit at level 63); +0x29 the
      // velocity level sense (curve 0 always); +0x2A the T1/T4 velocity
      // nibbles and +0x28 bits 4-7 the time KF. The five bytes +0x1F/+0x29/
      // +0x2B/+0x2D/+0x2E were the unread ones: on Glass Pad they are what
      // separates the two tones the mix was cancelling between.
      0x2c, 0x2b, 0x29, 0x2a, 0x28,

      // The two LFOs (lfo.md): +0x17 form/offset/sync/fade-polarity, +0x18
      // rate, +0x19 delay (bit 7 KEY-OFF), +0x1A fade time; LFO2 at +0x1B..+0x1E.
      0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e,

      // LFO-1 / LFO-2 Pitch Depth, signed, through ROM2 0x6C8A (ROM1 0x47A4 /
      // 0x47C7). Non-zero on 115 / 80 of the 539 factory tones.
      0x1f, 0x22
    },

    // Analog Feel, patch common +0x14 - the manual's "1/f fluctuation". Named
    // by its neighbours: +0x14, +0x15 and +0x16 are three adjacent 0-127 bytes
    // and the manual lists Analog Feel, Patch Level, Patch Pan in that order.
    // Non-zero on 119 of the 192 factory patches. L-26 / D-25.
    0x14,

    // +0x18: Key Assign in bit 7. Eleven factory patches are SOLO, among them
    // patch 21 SAW Lead (byte 0xD2), which the boot performance puts on parts
    // 1-6 - so channels 1-3 play one note at a time on the machine (D-44).
    0x18
  },

  {
    // Performances. 16 records of 204 bytes ending where the Internal patch bank
    // begins: a 28-byte common block, then eight 22-byte parts. The device powers
    // on in performance 1.
    0x008020, 204, 28, 22, 8, 0,

    // Effects, from the common block. The columns identify themselves across the
    // sixteen performances: +12's low three bits are always 0..7 (a type), +13
    // runs 92..127 and +14 74..127 (a level and a time), +15 0..68 (a feedback).
    // That is the manual's order for Performance Common, ROM offset = SysEx - 1.
    12, 13, 14, 15,

    // The chorus block, from the firmware (P-0382, P-0394). Chorus type shares
    // byte +12 with the reverb type, in bits 3-5 - across the sixteen factory
    // performances those bits never exceed 2, matching the manual's Chorus Type
    // 0-2 - and the level is +16, whose bit 7 is the Chorus Output switch. That
    // bit is set at level 100 as well as 127 in the factory data, so it is an
    // independent flag and not a side effect of a full level.
    //
    // RATE IS +18 AND DEPTH IS +17, the other way round from what this port read
    // (P-0394). Settled by the driver rather than by a name: it loads the four
    // bytes as r2 = (+16 << 8) | +17 and r3 = (+18 << 8) | +19 (ROM2
    // 0x7357-0x7364), keeps +17 in @0x844E and +18 in @0x844F (0x73D1, 0x73D5),
    // and then @0x844E is what scales BOTH the sweep window and the read-rate
    // increment by 0xE1 (0x7468, 0x74C9) while @0x844F only shrinks the window
    // by 0xF1 (0x745B). Scaling both is the depth role; shrinking the window
    // alone is the rate role - see ChorusJvLaw. The manual's own Chorus Depth /
    // Chorus Rate order agrees. The boot performance carries 127 and 45 in those
    // two bytes, and the mechanism gives 10.9 Hz at a 0.13 ms excursion the
    // wrong way round against 1.03 Hz at 3.81 ms the right way round.
    12, 16, 18, 17, 19,

    {
      // +16 patch number, then Part Level at +17 and Part Pan at +18, following
      // the manual's order (Patch Number, Part Level 0x19, Part Pan 0x1A) once
      // its split pair at 16/17 is collapsed to one byte.
      16, 17, 18,

      // +19 part coarse tune, in SIGNED semitones. Leaving it out put the demo's
      // melody exactly one octave high - the owner heard it as the wrong musical
      // key. Its values across the sixteen performances are all intervals a
      // musician would choose: -12, +12, -7, -8, -2, +20, +24, -29.
      19,

      // +21 carries the MIDI channel in its low nibble and two effect switches
      // above it. In Performance mode the part has a reverb/chorus SWITCH rather
      // than a send level - the manual lists Reverb Switch at 0x1D and Chorus
      // Switch at 0x1E, both "0000 000a" - and +21 reads 0xA0/0xC0/0xE0 across
      // the factory performances, so bit 6 is reverb and bit 5 chorus. The depth
      // comes from the patch tone send, or for the rhythm part from the rhythm
      // note's own send.
      21, 0x0f, 6, 5
    },

    // The reverb type records, reached through the pointer table at ROM2 0x4800.
    // It has ELEVEN entries, not eight: the eight reverb records (spaced 0x3C
    // for the six reverbs and 0x38 for the two delays, since the delays do not
    // own the four bytes past +0x37) and then the three 0x0A-byte CHORUS
    // records at 0x49EE/0x49F8/0x4A02, which RomLookup::JVChorusRecords reads
    // and which must not be treated as reverb. Hence the count of 8.
    //
    // Byte +0x38 of each record scales the return: 31, 29, 31, 28, 28, 28, 0,
    // 61 for ROOM1..PAN-DLY, read back from this ROM and verified against
    // scdb 08_effects/reverb.md. reverb.cc applies it (P-0395).
    //
    // The predecessor of this entry was 0, 0, 0 - switched off after an attempt
    // that measured 12.7 dB QUIET. That attempt divided the firmware's target a
    // second time, by 255, where the field's own full scale is 64; the factor
    // it introduced, 255/64 = 12.5 dB, is the entire discrepancy it reported.
    //
    // Words +0x0C and +0x0E of the same records are the delay-time SCALE pair
    // the delay arm multiplies Reverb Time by (P-0397). 0x3CF0/0x3CF0 for
    // DELAY, 0x1E7D/0x3CF0 for PAN-DLY, so the panning type's left tap is at
    // half the right's spacing - the 2:1 ratio is DATA here, not a division in
    // the engine. Measured on the reference at nine Delay Times on both types,
    // every echo lands on this arithmetic to the sample.
    0x004800, 8, 0x38, 0x0c, 0x0e,

    // Voice Reserve, eight bytes from +0x14. The boot performance "Syn Lead"
    // carries 4,4,4,4,4,0,0,0 (sum 20); every factory sum is 28 or less, which
    // is the rule ROM2 0x30207 enforces by zeroing the eight when it is not.
    0x14,

    // Preset A and Preset B performance banks, and the control channel.
    // The bank table at ROM2 0x30000 gives page 0x01 base 0x0020 for Preset A
    // and 0x8020 for Preset B, which are ROM2 0x10020 and 0x18020; sixteen
    // records of 204 at each. Preset A 0-15 are "Jazz Split".."PopOrchestra",
    // Preset B 0-15 "GTR Players".."NewListening", and B8 is "for CompuMix",
    // the one every demo song selects.
    //
    // The control channel is the machine's own system setting (battery-backed
    // page 0x0C, byte +0x0C), not a fixed constant; this NVRAM holds 0x0F, so
    // channel 16. It is given here as a profile default because libEmuSC does
    // not read the system area.
    0x010020, 0x018020, 15
  },

  {
    // Rhythm set. 61 keys from 36, 44 bytes each, right after the Internal patch
    // bank.
    0x00e760, 44, 61, 36,

    // +0 on/off and +1 waveform, as in a patch tone - the names settle it: key 36
    // Bright Kick, 38 90's Snare, 42 Closed HAT 1, 46 Open HAT 1.
    0, 1,

    // +3 play key. Every drum had been fixed at 60, the Sound Canvas convention,
    // and the residual pitch errors predicted this column exactly: kick 58 for
    // our +1.82 semitones, crash 62 for our -1.99, ride 61 for our -1.01, hats 60
    // for their 0.00.
    3,

    // +30 level (75..127, median 127) and +31 pan (0..128, median 64 - centred,
    // which is what a pan is). The manual lists Level then Pan adjacent at SysEx
    // 0x24 and 0x25.
    30, 31,

    // +42 reverb send and +43 chorus send. The manual's rhythm note map runs
    // Level 0x24, Pan 0x25, ... Dry 0x30, Reverb 0x31, Chorus 0x32. Level and Pan
    // fix the offset at -6; Pan is nibble-split across 0x25/0x26, so everything
    // after it shifts one further, putting Dry/Reverb/Chorus at +41/+42/+43 - and
    // +43 is exactly the last byte of the record. The columns read as their
    // names: dry 110..127 (pinned near max), reverb 0..127 varying per instrument
    // (key 36 Bright Kick 0 - a dry kick - key 38 snare 120), chorus mostly 0.
    // A constant in any of them would have proved nothing.
    42, 43,

    // The other forty-four parameters (D-01). Every offset, shift and mask
    // below is the firmware's own, out of the 52-entry field-descriptor table
    // the DT1 rhythm arm walks at ROM2 0x3A8C4 - eight bytes per SysEx
    // parameter giving mode, min, max, bias, shift, destination mask, record
    // offset and recalc bit. They are not read off the manual's address column:
    // the manual gives the NAMES and the printed value lists, the ROM gives the
    // positions, and 9150 field-range checks over the three factory sets pass
    // with no violations against the descriptors' own min/max.
    //
    // Ten of the fields carry descriptor bias 192, which is what makes the
    // stored byte a plain two's-complement signed value: SysEx sends 1..127 for
    // -63..+63, the store subtracts 64, and the record byte is the signed
    // number itself. 0xCE is -50 cents, not 206.

    // Tone Switch, bit 7 of +0 (descriptor param 0x03). Bits 0-1 are Wave Group
    // and bit 4 Output Select, so a whole-byte test is wrong for card and user
    // data even though the factory sets never expose it (D-08).
    7,

    // Mute Group, +2 bits 0-4 - the choke group. It is why a closed hi-hat cuts
    // an open one, and the factory sets use it exactly there: keys 42, 44 and 46
    // (Closed HAT 1, Closed HAT 2, Open HAT 1) share group 1 in ALL THREE sets,
    // plus three Spectrum keys in group 2 (Internal) and two in group 3
    // (Preset B). 14 of 183 notes grouped, 169 ungrouped.
    2,

    // +4 pitch fine tune, signed cents. Zero on 155 of 183 notes and +/-50 on
    // 23 of them - half a semitone, which is not a rounding error.
    4,

    // +41 Dry Level. 127 on 182 of 183 notes and 110 on one, so it is nearly a
    // constant here; read because it is the same attenuator the patch tones use
    // and a user kit can move it.
    41,

    // Filter Mode is +0x14 BITS 4-5, not the patch tone's bits 3-4 - the two
    // records spell it differently, which is why the shift is data. The census
    // over the 183 factory notes confirms the mask: LPF 176, HPF 4, OFF 3, and
    // not one value above 2. A wrong shift puts values there.
    0x14, 4,

    // +0x11 cutoff, +0x12 resonance in bits 0-6 with the SOFT/HARD mode in bit
    // 7. In the INTERNAL set - the one this device plays - keys 36 to 74 are all
    // cutoff 127 / resonance 0 / depth 0, i.e. a wide-open static filter, and
    // every filtered note is in the 75-96 range: the 808 Kicks at cutoff 11 /
    // resonance 88 HARD / depth 58, the Pop Voices at 10/125 HARD, and the
    // filtered Open HAT 1 at 41/127 HARD. So the filter is what those keys
    // sound like and nearly nothing on the rest.
    0x11, 0x12, 0x12,

    // +0x13 TVF-ENV velocity level sense and +0x15 TVF-ENV depth, both signed.
    // Depth is non-zero on 20 of 183.
    0x13, 0x15,

    // +0x16 begins four INTERLEAVED time/level pairs, T1 L1 T2 L2 T3 L3 T4 L4,
    // in the same order as the patch tone's filter envelope.
    0x16,

    // +0x20 TVA-ENV velocity level sense, SIGNED. This is the drums' whole
    // dynamic range and it was being held at 0 - no velocity response at all.
    // The factory value is 32 on 148 of the 183 notes, spanning 5..56.
    0x20,

    // +0x22 the TVA envelope: T1 L1 T2 L2 T3 L3 and then +0x28 T4, the fourth
    // segment, which the engine takes as its release. L3 is 0 on all 183 notes,
    // so the note reaches silence at the end of segment 3 and the release is a
    // formality - which is what NO-SUSTAIN means, and all 183 are NO-SUSTAIN.
    // The hardcoded shape this replaces was T2 = 64, L2 = 96, T3 = 80 against
    // the data's T2 = 40, L2 = 127, T3 = 40 on most of the kit: 1677 ms falling
    // to three quarters and then 3546 ms to silence, where the machine holds
    // full level for 505 ms and then falls to silence in another 505.
    0x22,

    // The JV's cutoff key-follow table is the manual's own percentage list read
    // as cents per semitone, and its entry 0 is -100. A rhythm note has no key
    // follow at all, so the neutral entry is index 5, which holds 0.
    5,

    // The envelope time-velocity nibbles. The rhythm descriptor table at ROM2
    // 0x3A8C4 puts SysEx 0x28 on record byte +0x21 and 0x1a on +0x14, both
    // 0-14 in the LOW nibble (their clear-mask is 0xf0) - the TVA's and the
    // TVF's "time velo". There is a third at +0x05 for the PITCH envelope
    // (SysEx 0x0b), left unwired because this engine has no JV pitch envelope
    // yet. The time key-follow is deliberately absent: the firmware forces it
    // neutral for rhythm notes, which _init_neutral_partial() already does.
    0x21, 0x14,

    // Rhythm banks. ROM2 keeps one set per memory bank at the same 0x8000
    // stride as the patch banks: 0x0E760 (above) is the Internal FACTORY set,
    // 0x16760 Preset A, 0x1E760 Preset B. A program change on the rhythm part
    // picks between them by its bank bits alone (ROM2 0x30446).
    //
    // With one set loaded, every demo song played the wrong kit: songs 1 and 7
    // ask for Preset A (PC 0 under the preset flag), songs 3 and 6 for Preset B
    // (PC 64 and PC 126). Song 3's rhythm channel measured +20.37 dB against
    // the oracle on that alone. scdb D-52.
    3, 0x8000
  }
};


const DeviceProfile JV880_PROFILE = {
  "JV-880",

  256 * 1024, 2,

  28,          // max polyphony

  // Not measured on the JV; it uses the SC-55mkII figure until it is.
  8.4f,

  &JV880_RECORDS,
  nullptr,     // not a Sound Canvas layout

  JV880_LOOKUP_TABLES,
  (int) (sizeof(JV880_LOOKUP_TABLES) / sizeof(JV880_LOOKUP_TABLES[0])),

  // The level law's arithmetic, from the firmware (P-0381). The index is a
  // 15-bit product taken down to the curve's 7-bit index; part and patch level
  // combine as a 7-bit product; the two curve lookups multiply and the high byte
  // survives to the chip. keyFollowUnitsPerPct is a LEAD - it comes from one
  // anchor, the engine's 0x4a meaning +100%.
  {
    8,      // toneIndexShift
    7,      // dynamicsShift
    24,     // staticShift
    10,     // keyFollowUnitsPerPct
    // envelopeFullScale: 0, meaning "not this device". The envelope's level does
    // not scale the static level linearly here - it goes through ROM2 0x6060 and
    // into its own chip register, F018 (P-0398, RomLookup::JVLevelEnv above).
    0,

    // The JV's envelope times are milliseconds, and only a table entry of 0 - a
    // time byte of 0 - snaps instantly. Every tone in the instruments the owner
    // heard as having too soft an attack carries T1 = 0.
    0,      // envelopeTimeUsPerUnit: 0 = the table is already in engine units
    0,      // envelopeInstantTicks

    // The law above already multiplies the part level in, so the generic
    // dynamic-level path must not apply it a second time. It was: the
    // part-level curve came out about twice as steep in decibels as the
    // machine's, which is most of why every instrument measured too quiet.
    0,      // partLevelInDynamics

    // CC7 Volume is NOT the part level on this machine, and mistaking it for one
    // was the single largest error in the JV port: the boot performance's part
    // levels 110/127/78 all collapsed to 100 the moment the demo sent CC7 = 100,
    // leaving channel 1 +2.7 dB hot (scdb D-28).
    //
    // ROM1 0x44be-0x44d4 forms the index. The level product byte @0x9a1e is
    // widened 0..127 -> 0..255 by `2*L + (L >= 64)` and multiplied by the raw
    // CC7 data byte held per part at @0x0A6146, then shifted down EIGHT. Traced,
    // not fitted: the writers of @0x9a1e are ROM1 0x4641-0x4648 (patch part) and
    // 0x4c2a (rhythm part), the CC7 handler is ROM1 0x75a9, and the multiply is
    // gated by the Tone's Volume switch, which the manual (p.6-16) documents in
    // the same words the ROM implements. Predicting the seven oracle
    // measurements from this law and the real ROM tables lands every one of them
    // within 0.12 dB.
    8       // volumeIndexShift
  },

  LevelLawKind::JVCurveProduct,

  {
    // UNMEASURED for this device: these are the SC-55mkII's numbers, which is
    // what the engine applied to the JV before they moved here. The reverb DSP
    // is shared with the Sound Canvas family, so the line's SHAPE is right and
    // only its two constants are open - they need this machine's own T60
    // against reverb time, the way P-0304 did the other two.
    1.829f, -11.9f, 187,

    // Return level, pre-LPF pair and delay taps. The 64 = unity scale is shared
    // across the family because the reverb DSP is (see ReverbLaw), but the JV
    // forms its numerator from the reverb TYPE's own record byte rather than
    // from the level alone - which is the whole of D-05/D-17 (P-0395).
    64.0f,
    ReverbReturnLaw::JVTypeCoefficient,
    8, 0x3f, 4,

    // The delay taps. `delayTapBase` is this ENGINE's register layout - the
    // address one past reverb.cc's highest delay write pointer, 0x15 - and not
    // a device number; the JV's own records carry 10 for the same reason,
    // because their program has ten write pointers where ours has twenty-two.
    // `delayTapPerTime` is unused under JVRecordScale: the JV takes the slope
    // from the selected type record's words +0x0C and +0x0E, which is what
    // makes PAN-DLY's two taps a 2:1 pair rather than a halving in code
    // (P-0397). It is left at the Sound Canvas value rather than zeroed so the
    // fallback path stays meaningful if the law is ever switched back.
    0x16, 112,
    ReverbDelayTapLaw::JVRecordScale,

    // THE NETWORK. This device does not run the Sound Canvas reverb program:
    // its TC6116AF reverb is one mono recirculating delay line read by nine
    // independent stereo tap pairs, and every number in it - the taps, their Q6
    // gains, the input gain, the loop tap, the pre-LPF pair - is in this
    // device's own ROM2 records, read through
    // ControlRom::LookupTables::JVReverbRecord (P-0399).
    //
    // So under JVMultiTapLine the three ReverbLaw constants above that belong
    // to the Sound Canvas program are DEAD on this device: timeSlope/timeOffset/
    // timeCap (the loop gain comes from the record and the Time law below), the
    // pre-LPF triple (the record carries a fixed pair per type; the JV has no
    // Pre-LPF parameter at all) and delayTapBase (the record's own word +0x1A
    // plus one). They are left at their values rather than zeroed so the
    // Sound Canvas path stays meaningful if the network is ever switched back,
    // exactly as delayTapPerTime already is.
    ReverbNetworkKind::JVMultiTapLine,
    ReverbFeedbackLaw::JVFirmwareRegister,

    // sendDivisor. A byte-sized coefficient on this chip has 64 = unity,
    // established four independent ways in the firmware (P-0395) and already
    // used by this same law's return level above. So a reverb send of 127 is
    // 1.98, not the 0.99 that dividing by 128 gives. Halving every send is the
    // whole of D-39: the reverb tail measured 39.60 dB where the reference
    // gives 45.66, and 45.62 with this in place - a 0.03 dB residual.
    64.0f
  },

  { true,  0x3f, 0x7f },

  // A flat address space across the four wave ROMs; no bank bits.
  { true, 0 },

  TvfLawKind::JVCentsRatio,

  // The filter chain's constants, all from the firmware (P-0390).
  {
    // 0x9994 scales the envelope depth. It is not an arbitrary gain: depth +63
    // at envelope level 127 gives 0x7f00 * 19351 >> 16 = 9600 cents, exactly
    // eight octaves, and the exponential table saturates just above that. The
    // constant and the table's clip point were chosen together.
    0x9994,

    // 0x99 scales an LFO depth byte into the same cents domain.
    0x99,

    // At resonance 0 the chip is driven from a separate rule rather than from
    // the LIMIT/DAMP tables: the cutoff word is capped at 0x8000 and the
    // damping falls from 0x4A48 by an eighth of the word, never below 0x4000.
    // Its boundary agrees with the tables exactly - LIMIT_SOFT[0] is 0x8000 and
    // DAMP_SOFT[0] is 0x4000 - which is an independent check that the rule and
    // the tables are one law.
    0x8000, 0x4a48, 0x4000,

    // Resonance is slew-limited to 16 of 127 per filter envelope tick, so a
    // full sweep takes 8 ticks: 128 ms, and audibly a glide.
    16,

    // The two register scales the F^2 + F*Q1 = 2 identity pins.
    0x8000, 0x4000,

    // The filter envelope steps every SECOND service period. The firmware
    // services it on alternate wakes of a 8 ms task, i.e. every 16 ms, and this
    // engine's control period is 256 samples of its internal 32 kHz clock -
    // the same 8 ms. So two.
    2
  },

  ChorusLawKind::JVSweptPointer,

  // The chorus driver's own constants (P-0394), all immediates in ROM2
  // 0x738C-0x74E3. See ChorusJvLaw for what each one multiplies.
  {
    3,          // three type records
    0xe1,       // f = Depth * 225 + 4096, doubled and taken as a 16-bit fraction
    0x1000,
    0xf1,       // g = 1 - (2 * Rate * 241) / 65536
    1,          // wet gain = ((level & 0x7f) >> 1) / 64, so at most 63/64
    2,          // feedback  = (feedback >> 2) / 64,      so at most 31/64
    64.0f       // 64 = unity for every byte coefficient this chip takes
  },

  // The pitch envelope's depth scale (scdb D-37, ROM1 0x48CC): the TVF's
  // 0x9994 has a pitch twin, 0xCB2C, and with it depth +12 at level +63 comes
  // out at 0x3F00 * 4876 >> 16 = 1200 cents, one octave exactly.
  { 0xcb2c }
};

}
