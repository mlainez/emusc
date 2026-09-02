/*
 *  This file is part of libEmuSC, a Sound Canvas emulator library
 *
 *  Device profiles: where each Roland ROM keeps its records and how those
 *  records are laid out.
 *
 *  The readers in control_rom.cc are generic. Everything that differs between
 *  devices - table offsets, record strides, the byte position of a field inside
 *  a record - lives here as data, selected once at initialisation from the synth
 *  model. Nothing device-specific belongs in the engine itself.
 *
 *  Every offset in device_profile.cc carries the evidence that fixed it. Those
 *  comments are the point of the file: an offset without provenance is a guess,
 *  and a guess that happens to be in range is the most expensive kind.
 */

#ifndef DEVICE_PROFILE_H
#define DEVICE_PROFILE_H

#include <cstdint>
#include <vector>

namespace EmuSC
{

// The waveform record table: one record per playable waveform.
struct WaveformTableLayout
{
  uint32_t offset;
  int      stride;
  int      nameLength;
  int      zones;
};

// The sample table: one record per sample slot referenced by a waveform zone.
struct SampleTableLayout
{
  uint32_t offset;
  int      stride;
  int      volume;                      // per-sample attenuation
};

// Byte offsets within one tone record.
struct ToneFieldMap
{
  int enabled;
  int waveform;
  int velocityLow, velocityHigh;
  int coarseTune;                       // signed semitones
  int fineTune;                         // signed, a few cents either way
  int pitchKeyFollow;                   // low nibble indexes the percent table
  int pan;                              // 0..127 = L64..63R, 128 = random
  int filterCutoff, filterResonance;
  int level, dryLevel;                // multiplied to give the tone's level
  int envTime1, envLevel1;
  int envTime2, envLevel2;
  int envTime3, envLevel3;
  int envRelease;
  int reverbSend, chorusSend;
  int tvaVelCurve;                      // +71 bits 0-2: which of the seven curves
  int tvaVelLevelSens;                  // +72 signed -63..+63; 0 = no velocity effect

  // The time-variant filter's own fields. The JV's cutoff chain reads eleven of
  // them and reading only two - cutoff and resonance - is why the filter had to
  // be left disabled (P-0390).
  //
  // filterMode and tvfVelCurve share ONE byte: bits 3-4 are the mode
  // (0 OFF, 1 LPF, 2 HPF after the shift) and bits 0-2 select the envelope's
  // velocity curve. So do resonance and its SOFT/HARD mode bit, which is why
  // resoMode names the same offset as filterResonance.
  int filterMode;                       // bits 3-4
  int resoMode;                         // bit 7 of the resonance byte
  int cutoffKeyFollow;                  // bits 0-3 index the cents-per-semitone table
  int tvfVelCurve;                      // bits 0-2 of the filterMode byte
  int tvfVelLevelSens;                  // signed; with the curve, an attenuation
  int tvfEnvDepth;                      // signed -63..+63
  int tvfEnv;                           // first of four interleaved time/level pairs
  int lfo1TvfDepth, lfo2TvfDepth;       // signed
};

// A bank of patches, and the tone records inside each patch.
// Positional initialisers: the field order here IS the initialiser order in
// devices/*.cc. Inserting a field without moving the values with it silently
// shifts every later member - it once turned `stride` into 0 and made every
// patch read from the same offset.
struct PatchLayout
{
  uint32_t bankOffset;                  // first bank; the rest follow at
  uint32_t bankStride;                  // +bankStride each
  int      banks, perBank;

  // Which MIDI bank select (CC0) reaches the preset banks, and which bank index
  // the first ROM bank answers to. A program change alone reaches the first
  // bank only; the machine puts the rest behind a bank select.
  int      midiBankFirst;               // CC0 value for bank 0 of the ROM
  int      midiBankPresets;             // CC0 value that reaches the presets
  int      stride, nameLength;
  int      level;                       // patch-common byte
  int      firstTone, toneStride, tones;
  ToneFieldMap tone;
};

// Byte offsets within one performance part record.
struct PerformancePartMap
{
  int patch;
  int level, pan;
  int coarseTune;                       // signed semitones
  int channel;                          // also carries the two effect switches
  int channelMask;
  int reverbSwitchBit, chorusSwitchBit;
};

// A performance: a common block of effect settings, then one record per part.
struct PerformanceLayout
{
  uint32_t offset;
  int      stride, commonSize, partStride, parts;
  int      bootIndex;                   // the performance the device powers on in
  int      reverbType, reverbLevel, reverbTime, reverbFeedback;

  // The chorus block. chorusType shares its byte with reverbType (bits 3-5),
  // and chorusLevel's bit 7 is the Chorus Output switch: set means the chorus
  // returns into the reverb rather than the mix.
  int      chorusType, chorusLevel, chorusRate, chorusDepth, chorusFeedback;
  PerformancePartMap part;

  // The reverb TYPE records. reverbTypeTable is a table of big-endian pointers,
  // one per type; the byte at reverbReturnCoeff inside the record it points at
  // scales the return level, which the firmware forms as
  //     return = ((2 * level + 1) * coeff) >> 8        (P-0382, P-0387)
  // over a full scale of 255. Without it the return runs at the level byte
  // alone, which is about 24 dB hot at maximum: the machine's own coefficient
  // is 28..31 of 255 for its first six types. Zero disables the lookup.
  uint32_t reverbTypeTable;
  int      reverbTypeCount, reverbReturnCoeff;
};

// A rhythm set: one record per key.
struct RhythmLayout
{
  uint32_t offset;
  int      stride, keys, firstKey;
  int      enabled, waveform, playKey;
  int      level, pan;
  int      reverbSend, chorusSend;
};

// Curves the synthesis engine reads straight out of the ROM. The id says which
// engine table the bytes belong to, so a device lists what it has and the reader
// stays a switch rather than a chain of name comparisons.
enum class RomLookup
{
  TVFResonanceFreq,
  TVFResonance,
  EnvSegmentCurve,
  TVAPanKeyFollow,
  TVALevelIndex,
  EnvTimeKeyFollowSens,
  LFOSine,
  PitchCoarseExp,
  EnvelopeTime,
  JVLevel,
  JVVelCurves,

  // The JV's time-variant filter tables, all in its control ROM (P-0390). The
  // two exponential tables turn the cents accumulator into a frequency ratio;
  // TVFBase is the coefficient that ratio multiplies; the LIMIT/DAMP pairs are
  // the per-resonance clamp and damping, one pair per resonance mode; and
  // TVFCutoffKF is the manual's own key-follow list in cents per semitone.
  JVTvfExpCoarse,
  JVTvfExpFine,
  JVTvfLimitSoft,
  JVTvfDampSoft,
  JVTvfLimitHard,
  JVTvfDampHard,
  JVTvfBase,
  JVTvfCutoffKF,

  // The JV's chorus type records: three records of five big-endian words, laid
  // end to end, holding the effect-PSRAM addresses its chorus sweeps between
  // and the sweep-rate base (P-0394).
  JVChorusRecords
};

// Which way a curve must go for the reading to be trusted. Four JV tables have
// been caught by this check and two of them were entered with it switched off.
enum class Monotonic
{
  Unchecked,
  Rising,
  Falling
};

struct RomLookupTable
{
  RomLookup id;
  uint32_t  offset;
  int       entries;
  int       width;                      // bytes per entry, 1 or 2
  Monotonic shape;
};


// A ROM that carries its waveforms, patches, performances and rhythm set in one
// image, addressed by record offset. The JV family is laid out this way.
struct RecordRomLayout
{
  WaveformTableLayout waveform;
  SampleTableLayout   sample;
  PatchLayout         patch;
  PerformanceLayout   performance;
  RhythmLayout        rhythm;

  // A device whose patch bank has not been mapped yet still plays its waveforms.
  bool has_patches(void) const { return patch.bankOffset != 0; }
};


// Where the Sound Canvas family keeps the tables its program and CPU ROMs hold.
struct ProgramRomMap
{
  int VelocityCurves;
  int KeyMapperIndex;
  int KeyMapper;

  // TVA pan key follow curve, or 0 if this generation has none. The SC-55 has
  // none: no instrument in its control ROM selects one (Instrument::panKeyFlw is
  // 0 on all 386 records) and the SC-55mkII's address holds unrelated 16-bit data
  // in the SC-55 ROM. The SC-55mkII's curve is a 128-byte table of pan positions
  // biased by 0x40, preceded by a four entry selector whose first entry is 0xffff
  // and whose remaining three all point at this curve (PROVENANCE.md P-0122).
  int TVAPanKeyFollow;
};

struct CpuRomMap
{
  int PitchParamScale;
  int EnvTimeKeyFollowSens;
  int EnvTimeScale;
  int EnvelopeTime;
  int LFORate;
  int LFODelayTime;
  int LFOTVFDepth;
  int LFOTVPDepth;
  int LFOSine;
  int TVFCutoffFreqKF;
  int TVFCutoffVSens;
  int TVFEnvDepth;
  int TVFCutoffFreq;
  int TVFResonanceFreq;
  int TVFResonance;
  int PitchEnvVelSens1;
  int PitchEnvVelSens2;
  int PitchEnvDepth;
  int TVFEnvScale;
  int PortamentoRate;
  int EnvSegmentStep;
  int EnvSegmentCurve;
  int TVAEnvExpChange;
  int TVABiasLevel;
  int TVAPanpot;
  int TVALevelIndex;
  int TVALevel;
  int PitchFineExp;
  int PitchCoarseExp;
};

// A ROM split across a control ROM of banked instrument records and a separate
// CPU ROM holding the synthesis curves. The Sound Canvas family is laid out
// this way.
struct SoundCanvasLayout
{
  const std::vector<uint32_t> *banks;
  const ProgramRomMap         *program;
  const CpuRomMap             *cpu;
  int      velocityCurves;

  // Drum sets follow the drum map array in equal blocks, ending here.
  uint32_t drumSetTableEnd;
  int      drumSetStride;

  // The key mapper is stored at a ROM address; the engine wants it relative to
  // the bank this base names.
  uint32_t keyMapperBase;

  uint32_t demoSongOffset;              // bundled demo songs
  bool     demoSongRunsToRomEnd;        // otherwise it ends at the first bank

  uint32_t introAnimOffset;             // 0 if this device has no intro animation
  uint32_t introAnimLength;             // each animation is this long, laid end
  int      introAnimCount;              // to end from introAnimOffset
};


// How a ROM announces itself, and where its version string lives.
enum class RomVersionStyle
{
  Unknown,        // the ROM does not carry one we can read
  Inline,         // version and date sit inside the signature block itself
  SeparateBcd     // version elsewhere, followed by a BCD year/month/day
};

struct RomSignature
{
  const char     *modelName;
  uint32_t        offset;
  int             readLength;
  const char     *match;
  int             matchLength;
  RomVersionStyle versionStyle;
  uint32_t        versionOffset;        // SeparateBcd only
};


// The arithmetic of a device's level law. The engine does the algebra; these say
// what the device's firmware shifts and scales by, so no device constant sits in
// engine code. The JV's values come from its disassembly (P-0381).
struct LevelLaw
{
  int toneIndexShift;        // (toneLevel x sampleLevel) >> this, to index the curve
  int dynamicsShift;         // (partLevel x patchLevel) >> this
  int staticShift;           // (gain x dynamics) >> this, to an 8-bit static level
  int velocityShift;         // (sensitivity x (pivot - velocity)) >> this
  int velocityPivot;         // full-scale velocity, the point of no attenuation
  int keyFollowUnitsPerPct;  // engine key-follow units per ten percent
  int envelopeFullScale;     // envelope level that means "no attenuation"

  // What the device's envelope time table holds. The JV's is in MILLISECONDS
  // (P-0383); the Sound Canvas's is already in engine control ticks, which is
  // what 0 means here. The engine's tick is 256 samples of its 32 kHz clock, 8 ms
  // - the same service period the JV's firmware uses, measured independently.
  int envelopeTimeUsPerUnit; // microseconds per table entry, or 0 if already ticks
  int envelopeInstantTicks;  // segment this short snaps instantly

  // Whether the generic dynamic-level path may apply the part level. A device
  // whose own level law already multiplies the part level in (the JV reads
  // T[(partLevel x patchLevel) >> 7]) must say 0 here, or the attenuation is
  // applied twice and the part-level curve comes out about twice as steep in
  // decibels. 1 = the Sound Canvas behaviour, part level applied here.
  int partLevelInDynamics;
};


// Which arithmetic a device's level chain uses. The families differ in kind, not
// in constants: the Sound Canvas accumulates attenuations in a log index domain
// and subtracts them, while the JV forms a product of two curve lookups and
// keeps the envelope as a separate multiplier (PROVENANCE.md P-0381).
enum class LevelLawKind
{
  SoundCanvasLogIndex,
  JVCurveProduct
};


// How a sample address in a waveform record maps onto the wave ROM images.
struct WaveAddressing
{
  bool     flat;          // one flat space, no bank bits: the JV family
  uint32_t bank2Offset;   // where bank id 2 lands; the mkII differs from the mk1
};


// The reverb unit's device-specific numbers.
//
// The network in reverb.cc is SHARED, and that is a hardware fact rather than a
// convenience. The SC-55 mk1, the SC-55mkII and the JV-880 all drive the same
// reverb DSP: Roland's JV-880 service notes state that its TC6116AF is the mk1's
// TC24SC201AF PCM engine packaged together with the mk1's HG62E11B23FS I/O gate
// array, and the SC-55mkII's parts list carries that same Roland part number
// (15239229). So one sibling differs from another only in the numbers below,
// never in the topology.
struct ReverbLaw
{
  // Reverb time -> loop gain, a rounded straight line saturating at timeCap.
  // Inverted from our own measured T60(gLoop) curve against each machine's own
  // T60; the mk1 decays about 1.09x slower than the mkII at the same reverb
  // time, so one line cannot fit both (P-0296, P-0300, P-0303, P-0304).
  float timeSlope, timeOffset;
  int   timeCap;

  // Return level: gain = level / levelDivisor.
  //
  // This is NOT the firmware's law for either family, and knowingly so. The mk1
  // writes the return as (level << 8) into a 16-bit register; the JV-880 forms
  // ((2 * level + 1) * coeff) >> 8 out of 255, with coeff from the reverb TYPE
  // record - some 24 dB below this divisor at full level. Applying the JV's own
  // law moved its return from 13.3 dB hot to 12.7 dB quiet (P-0387), and those
  // two figures bracket the truth: the residual is this network's internal gain,
  // not the return law. Since the DSP is shared, correcting it means building the
  // network from the traced tap set both devices upload - not a fudge factor.
  float levelDivisor;

  // Pre-LPF: a complementary one-pole pair whose halves sum to pairSum, stepped
  // by stepPerLevel and capped at maxLevel.
  int preLpfStep, preLpfPairSum, preLpfMaxLevel;

  // Delay and panning-delay taps: tap = base + perTime * reverbTime. Panning
  // delay puts its left tap at half the spacing.
  int delayTapBase, delayTapPerTime;
};


// Which mechanism a device's chorus is. As with the level and filter laws the
// two families differ in KIND, not in constants: the Sound Canvas modulates a
// delay LENGTH from an LFO whose rate comes from a rate law, while the JV sweeps
// a READ POINTER between two effect-PSRAM addresses taken from a per-type record
// and lets the sweep speed set the period (PROVENANCE.md P-0394).
//
// Driving the JV's bytes through the Sound Canvas law is what the owner heard as
// "ça saccade": it modulated at 15.9 Hz where the machine modulates at 2 Hz, and
// its span reached 39.7 ms against CHORUS1's real 5.75 ms (P-0392).
enum class ChorusLawKind
{
  SoundCanvasLfo,
  JVSweptPointer
};


// The constants of the JV's chorus driver, so the engine holds the arithmetic
// and the device holds its numbers. Every one is an instruction's immediate in
// ROM2 0x745B-0x74E3 or 0x738C-0x73A4 (P-0394); the per-type records themselves
// come from the ROM through RomLookup::JVChorusRecords.
//
// The driver hands the chip a start address, an end address, a position and a
// read-rate increment on pseudo-voice slot 0x1F:
//
//   span    = w3 - w2
//   window  = 2 * hi16(f * (span - hi16(2 * Rate * rateScale * span)))
//   incr    = 2 * hi16(f * w4)          with f = Depth * depthScale + depthOffset
//
// so f(Depth) = 2 * (Depth * 225 + 4096) / 65536 scales BOTH the window and the
// increment while g(Rate) = 1 - 2 * Rate * 241 / 65536 shrinks the window alone.
// The modulation period is window / increment, so f cancels and the period
// depends only on Rate; the excursion IS the window and depends only on Depth.
// That exact decoupling is what identifies the mechanism - no coincidence
// produces it - and it is why Rate must reach g() and Depth f(): see the
// performance field map in devices/jv880.cc.
struct ChorusJvLaw
{
  int   types;              // type records in the table, five words each
  int   depthScale;         // f = Depth * this + depthOffset
  int   depthOffset;
  int   rateScale;          // g = 1 - (2 * Rate * this) / 65536
  int   levelShift;         // wet gain = ((level & 0x7f) >> this) / coeffUnity
  int   feedbackShift;      // feedback  = (feedback >> this) / coeffUnity
  float coeffUnity;         // the byte coefficient that means 1.0
};


// How a part's TVF Cutoff Frequency parameter reaches the partial's cutoff.
struct TvfCutoffLaw
{
  // Whether a POSITIVE parameter value raises the partial's base cutoff. On the
  // mkII generation it does, one cutoff step per parameter step, and the
  // resonance the cutoff admits is read at the raised position. On the mk1 a
  // positive value does nothing at all. Measured on Warm Pad, C4, over the whole
  // parameter range (PROVENANCE.md P-0133).
  bool offsetRaises;

  // The two ceilings this parameter is clamped to: on (parameter - 0x40) where
  // the partial's base cutoff is offset, and on the parameter itself where the
  // phase iterator recomputes it.
  //
  // The mk1 restricts this parameter where later devices do not. Its ceiling is
  // 0x50 against the mkII's 0x7f - and that restriction is the mk1's own, not
  // the family's: the SCC-1 generation's firmware allows 0x72, both in the ROM
  // byte and in its manual's own parameter chart. libEmuSC currently answers an
  // SCC-1 ROM with this profile, so it inherits the mk1's narrower ceiling; that
  // is a KNOWN DIVERGENCE, not a measurement, and correcting it needs an SCC-1
  // reference render rather than a change made on paper.
  int offsetMax;
  int paramMax;
};


// Which arithmetic a device's filter chain uses. As with the level law the two
// families differ in kind, not in constants: the Sound Canvas assembles a cutoff
// INDEX and looks the coefficient up, while the JV accumulates modulation in
// CENTS, exponentiates it and multiplies the base coefficient by the result
// (PROVENANCE.md P-0390). An earlier attempt to drive the JV's fields through
// the Sound Canvas chain came out close in colour and 35 dB wrong in level.
enum class TvfLawKind
{
  SoundCanvasIndex,
  JVCentsRatio
};


// The constants of the JV's filter chain, so the engine holds the arithmetic and
// the device holds its numbers. Every one is read from the firmware (P-0390).
struct TvfJvLaw
{
  int envDepthScale;      // TVF-ENV Depth: sign(d) * ((|2d| << 8) * this >> 16)
  int lfoDepthScale;      // LFO -> TVF depth: int8 * this
  int zeroResLimit;       // at resonance 0 the cutoff word is capped here
  int zeroResDampBase;    // and the damping is this minus (word >> 3),
  int zeroResDampFloor;   // never below this
  int resSlewPerTick;     // resonance moves at most this far per envelope tick
  int cutoffUnity;        // cutoff word that means F1 = 1.0
  int dampUnity;          // damping word that means Q1 = 1.0
  int envTickPeriods;     // engine control periods per filter envelope tick
};


// Everything the engine needs to know about one device. Exactly one of the two
// layout pointers is set: it says which family the ROM belongs to, and so which
// reader can walk it.
struct DeviceProfile
{
  const char *name;

  // Size of the control ROM image, and how many 2 MB wave ROM banks the device
  // addresses. Used to tell one device of a family from another.
  size_t romSize;
  int    waveRomBanks;

  uint8_t maxPolyphony;

  // Fade applied to a partial when its voice is given to another note, in dB per
  // millisecond. Measured by taking a sounding voice away and comparing the
  // render against one where it was left alone: the level falls linearly in dB,
  // independent of the tone's own release (PROVENANCE.md P-0080).
  float voiceDampRate;

  const RecordRomLayout   *records;
  const SoundCanvasLayout *soundCanvas;

  const RomLookupTable *lookupTables;
  int                   lookupTableCount;

  LevelLaw level;

  LevelLawKind   levelLawKind;
  ReverbLaw      reverb;
  TvfCutoffLaw   tvfCutoff;
  WaveAddressing wave;

  // Last, so that a device profile written before these existed still compiles
  // and still means what it meant: an omitted initialiser leaves the kind at
  // SoundCanvasIndex and the JV law zeroed and unread.
  TvfLawKind     tvfLawKind;
  TvfJvLaw       tvfJv;

  // The same reasoning for the chorus: an omitted initialiser leaves the kind at
  // SoundCanvasLfo, which is the behaviour every Sound Canvas profile had before
  // this field existed.
  ChorusLawKind  chorusLawKind;
  ChorusJvLaw    chorusJv;
};

extern const RomSignature SC55_SIGNATURE;
extern const RomSignature SCC1_SIGNATURE;
extern const RomSignature SC55MKII_SIGNATURE;
extern const RomSignature SCB55_SIGNATURE;
extern const RomSignature SC88_SIGNATURE;

// The Sound Canvas family's shared behaviour, for a generation whose ROM layout
// has not been mapped yet and so has no profile of its own. It carries only what
// the synthesis engine reads - never a ROM offset - and its values are exactly
// what the engine used to hold in its own else branches.
extern const DeviceProfile SOUND_CANVAS_DEFAULT_PROFILE;

extern const DeviceProfile JV880_PROFILE;
extern const DeviceProfile JV1080_PROFILE;
extern const DeviceProfile SC55_PROFILE;
extern const DeviceProfile SC55MKII_PROFILE;

}

#endif  // DEVICE_PROFILE_H
