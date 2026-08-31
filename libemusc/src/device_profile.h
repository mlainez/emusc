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
  JVVelCurves
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
  int envelopeInstantTicks;  // a segment this short or shorter snaps instantly
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
};

extern const RomSignature SC55_SIGNATURE;
extern const RomSignature SCC1_SIGNATURE;
extern const RomSignature SC55MKII_SIGNATURE;
extern const RomSignature SCB55_SIGNATURE;
extern const RomSignature SC88_SIGNATURE;

extern const DeviceProfile JV880_PROFILE;
extern const DeviceProfile JV1080_PROFILE;
extern const DeviceProfile SC55_PROFILE;
extern const DeviceProfile SC55MKII_PROFILE;

}

#endif  // DEVICE_PROFILE_H
