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
};

// Byte offsets within one tone record.
struct ToneFieldMap
{
  int enabled;
  int waveform;
  int velocityLow, velocityHigh;
  int coarseTune;                       // signed semitones
  int filterCutoff, filterResonance;
  int level, levelScale;                // multiplied to give the tone's level
  int envTime1, envLevel1;
  int envTime2, envLevel2;
  int envTime3, envLevel3;
  int envRelease;
  int reverbSend, chorusSend;
};

// A bank of patches, and the tone records inside each patch.
struct PatchLayout
{
  uint32_t bankOffset;                  // first bank; the rest follow at
  uint32_t bankStride;                  // +bankStride each
  int      banks, perBank;
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
  int      chorusLevel, chorusDepth;
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
  PitchCoarseExp
};

struct RomLookupTable
{
  RomLookup id;
  uint32_t  offset;
  int       entries;
  int       width;                      // bytes per entry, 1 or 2
  bool      mustRise;                   // check monotonic before trusting it
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

  uint32_t demoSongOffset;              // bundled demo songs
  bool     demoSongRunsToRomEnd;        // otherwise it ends at the first bank

  uint32_t introAnimOffset;             // 0 if this device has no intro animation
  uint32_t introAnimLength;             // each animation is this long, laid end
  int      introAnimCount;              // to end from introAnimOffset
};


// Everything the engine needs to know about one device. Exactly one of the two
// layout pointers is set: it says which family the ROM belongs to, and so which
// reader can walk it.
struct DeviceProfile
{
  const char *name;

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
};

extern const DeviceProfile JV880_PROFILE;
extern const DeviceProfile JV1080_PROFILE;
extern const DeviceProfile SC55_PROFILE;
extern const DeviceProfile SC55MKII_PROFILE;

}

#endif  // DEVICE_PROFILE_H
