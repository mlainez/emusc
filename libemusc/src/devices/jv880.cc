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

  // Envelope phase times: 128 big-endian 16-bit entries, 128 rising to 16127 on
  // a constant ratio of 1.0384 - a doubling every ~18 steps. The same shape and
  // magnitude as the SC-55's envelopeTime, which is what makes it recognisable:
  // sampled every 16 it reads 128, 235, 433, 796, 1464, 2693, 4953, 9109 against
  // the SC-55's 0, 159, 453, 994, 1990, 3827, 7211, 13448.
  { RomLookup::EnvelopeTime,         0x04c58, 128, 2, Monotonic::Rising },

  // The level law's own curve (P-0381), found in the firmware rather than fitted.
  // 128 big-endian words rising 0 to 65535, with its own precomputed slope table
  // beside it at 0x6360 - 127 of 127 entries within one of the exact deltas,
  // which is what identifies it. Not exponential: 0.22 dB per step at the top,
  // over 0.5 dB below index 16, 45.6 dB span. Every attempt to fit a constant
  // dB-per-step for this failed because there is not one.
  { RomLookup::JVLevel,              0x06260, 128, 2, Monotonic::Rising },

  // Seven velocity curves of 128 bytes at 0x5390 + c * 0x80, each falling 255 to
  // 0, selected per tone by record byte +55 & 7 for the TVF and +71 & 7 for the
  // TVA - the descriptor table's own velocity-curve fields. Curve 4 is 0x05590,
  // which this port had been reading as a general level index table.
  { RomLookup::JVVelCurves,          0x05390, 896, 1, Monotonic::Unchecked }
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
      71, 72
    }
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

    // The chorus block, from the firmware (P-0382). This port had been reading
    // +17 as the chorus LEVEL; +17 is the RATE. Chorus type shares byte +12 with
    // the reverb type, in bits 3-5 - across the sixteen factory performances
    // those bits never exceed 2, matching the manual's Chorus Type 0-2 - and the
    // level is +16, whose bit 7 is the Chorus Output switch. That bit is set at
    // level 100 as well as 127 in the factory data, so it is an independent flag
    // and not a side effect of a full level.
    12, 16, 17, 18, 19,

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
    }
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
    42, 43
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
  (int) (sizeof(JV880_LOOKUP_TABLES) / sizeof(JV880_LOOKUP_TABLES[0]))
};

}
