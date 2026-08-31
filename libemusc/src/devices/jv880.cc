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
  { RomLookup::TVFResonanceFreq,     0x3e9c4, 256, 1, false },
  { RomLookup::TVFResonance,         0x054be, 128, 1, false },
  { RomLookup::EnvSegmentCurve,      0x055f5,   9, 1, false },
  { RomLookup::TVAPanKeyFollow,      0x3e931, 128, 1, false },
  { RomLookup::TVALevelIndex,        0x05590, 128, 1, false },
  { RomLookup::EnvTimeKeyFollowSens, 0x3ff49,  21, 1, true  },
  { RomLookup::LFOSine,              0x04edf, 130, 1, false }
};

const DeviceProfile JV880_PROFILE = {

  // Waveform record table. 129 records of 60 bytes from the very start of the
  // ROM: a 12-byte name, then 11 key zones.
  { 0x000004, 60, 12, 11 },

  // Sample table. 577 records of 18 bytes.
  { 0x001e40, 18 },

  {
    // Patches. Internal at 0x08ce0, then Preset A and Preset B at +0x8000 each,
    // 64 patches of 362 bytes per bank: a 12-byte name, a 26-byte common block,
    // then four 84-byte tones.
    0x008ce0, 0x8000, 3, 64, 362, 12,

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
      82, 83
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
    12, 13, 14, 15, 17, 18,

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
  },

  JV880_LOOKUP_TABLES,
  (int) (sizeof(JV880_LOOKUP_TABLES) / sizeof(JV880_LOOKUP_TABLES[0]))
};

}
