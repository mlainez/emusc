/*  
 *  This file is part of libEmuSC, a Sound Canvas emulator library
 *  Copyright (C) 2022-2026  Håkon Skjelten
 *
 *  libEmuSC is free software: you can redistribute it and/or modify it
 *  under the terms of the GNU Lesser General Public License as published
 *  by the Free Software Foundation, either version 2.1 of the License, or
 *  (at your option) any later version.
 *
 *  libEmuSC is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with libEmuSC. If not, see <http://www.gnu.org/licenses/>.
 */

// Control ROM decoding is based on the SC55_Soundfont generator written by
// Kitrinx and NewRisingSun [ https://github.com/Kitrinx/SC55_Soundfont ]


#ifndef __CONTROL_ROM_H__
#define __CONTROL_ROM_H__


#include <stdint.h>

#include "device_profile.h"

#include <array>
#include <fstream>
#include <string>
#include <vector>


namespace EmuSC {

class ControlRom
{
public:
  ControlRom(std::string romPath, std::string cpuRomPath);
  ~ControlRom();

  // Internal data structures extracted from the control ROM file

  struct Sample {         // 16 bytes
    uint8_t  volume;      // Volume attenuation (0x7f - 0)
    uint32_t address;     // Bank + scrambled address on SC55.
                          // Bits above 20 are wave bank.
    uint16_t portaOffset; // Start offset for samples during portamento
    uint16_t sampleLen;   // Sample size
    uint16_t loopLen;     // Loop point, used as sample_len - loop_len - 1
    uint8_t  loopMode;    // 2 = No loop, 1 = ping-pong loop, 0 = forward loop
    uint8_t  rootKey;     // Base pitch of the sample
    uint16_t pitchInit;   // Pitch offset for first playback until loop point
    uint16_t pitchSust;   // Pitch offset used from first loop
    bool     reverse;     // JV only: play the sample backwards
  };

  struct Partial {        // 48 bytes in total
    std::string name;
    uint8_t breaks[16];   // Note breakpoints corresponding to sample addresses
    uint16_t samples[16]; // Set of addresses to the sample table. 0 is default
  };                      // and above corresponds to breakpoints

  struct InstPartial {      // 92 bytes in total
    uint8_t rootKeyOffset;  // Root key offset

    uint8_t LFO2Waveform;   // LFO2 waveform
    uint8_t LFO2Rate;       // LFO2 frequency
    uint8_t LFO2Delay;      // LFO2 delay before LFO Fade starts
    uint8_t LFO2Fade;       // LFO2 fade-in, linear increase

    uint8_t TVFFlags;       // TVF feature flags (always 0xff, used for debug?)

    uint16_t partialIndex;  // Partial table index, 0xFFFF for unused
    int8_t panpot;          // [-64, 64]. Default 0x40 (0-127)
    int8_t coarsePitch;     // Shifts pitch in semitones. Default 0x40
    int8_t finePitch;       // Shifts pitch in cents. Default 0x40
    int8_t randPitch;

    int8_t pitchKeyFlw;

    uint8_t TVPLFO1Depth;
    uint8_t TVPLFO2Depth;
    uint8_t pitchEnvDepth;
    uint8_t pitchEnvL0;     // Pitch Envelope L0
    uint8_t pitchEnvL1;     // Pitch Envelope L1
    uint8_t pitchEnvL2;     // Pitch Envelope L2
    uint8_t pitchEnvL3;     // Pitch Envelope L3 (L4 = 0)
    uint8_t pitchEnvL5;     // Pitch Envelope L5
    uint8_t pitchEnvT1;     // Pitch Envelope T1 (Attack1)
    uint8_t pitchEnvT2;     // Pitch Envelope T2 (Attack2)
    uint8_t pitchEnvT3;     // Pitch Envelope T3 (Decay1)
    uint8_t pitchEnvT4;     // Pitch Envelope T4 (Decay2)
    uint8_t pitchEnvT5;     // Pitch Envelope T5 (Release)

    uint8_t pitchETKeyFP14; // Pitch Envelope Time Key Follow Pre-calc (T1 - T4)
    uint8_t pitchETKeyFP5;  // Pitch Envelope Time Key Follow Pre-calc (T5)
    uint8_t pitchETKeyF14;  // Pitch Envelope Time Key Follow (T1 - T4)
    uint8_t pitchETKeyF5;   // Pitch Envelope Time Key Follow (T5)
    uint8_t pitchEnvVSens;  // Pitch Envelope Velocity Sensitivity
    uint8_t pitchEnvTVSens; // Pitch Envelope Time Velocity Sensitivity

    uint8_t TVFCOFVelCur;   // TVF Cutoff Velocity Curve
    int8_t TVFBaseFlt;
    int8_t TVFResonance;
    int8_t TVFType;         // TVF Type [ low pass | high pass | disabled ]

    uint8_t TVFCFKeyFlw;    // TVF Cutoff Frequency Key Follow
    uint8_t TVFCFKeyFlwC;   // TVF Cutoff Frequency Key Follow Curves

    uint8_t TVFLFO1Depth;
    uint8_t TVFLFO2Depth;
    uint8_t TVFEnvDepth;
    uint8_t TVFEnvL1;       // TVF Envelope L1 (L0 = 0)
    uint8_t TVFEnvL2;       // TVF Envelope L2
    uint8_t TVFEnvL3;       // TVF Envelope L3
    uint8_t TVFEnvL4;       // TVF Envelope L4
    uint8_t TVFEnvL5;       // TVF Envelope L5
    uint8_t TVFEnvT1;       // TVF Envelope T1
    uint8_t TVFEnvT2;       // TVF Envelope T2
    uint8_t TVFEnvT3;       // TVF Envelope T3
    uint8_t TVFEnvT4;       // TVF Envelope T4
    uint8_t TVFEnvT5;       // TVF Envelope T5

    uint8_t TVFETKeyFP14;   // TVF Envelope Time Key Follow Presets (T1 - T4)
    uint8_t TVFETKeyFP5;    // TVF Envelope Time Key Follow Presets (T5)
    uint8_t TVFETKeyF14;    // TVF Envelope Time Key Follow (T1 - T4)
    uint8_t TVFETKeyF5;     // TVF Envelope Time Key Follow (T5)
    uint8_t TVFCOFVSens;    // TVF Cutoff Frequency Velocity Sensitivty
    uint8_t TVFETVSens12;   // TVF Envelope Time Velocity Sensitivity (T1 - T2)
    uint8_t TVFETVSens35;   // TVF Envelope Time Velocity Sensitivity (T3 - T5)

    // Three fields the Sound Canvas ones above cannot carry, because the JV
    // spells the same parameters differently (P-0390): its cutoff key follow is
    // an INDEX into a cents-per-semitone table rather than a byte biased at
    // 0x40, its resonance byte carries a SOFT/HARD mode bit, and its envelope
    // velocity sensitivity is a plain signed byte where the Sound Canvas's is
    // biased. Given their own names so that neither chain can read the other's
    // neutral value as data.
    uint8_t TVFCOFKeyFlwIdx;   // index into LookupTables::JVTvfCutoffKF
    uint8_t TVFResoMode;       // 0 = SOFT, 1 = HARD
    int8_t  TVFEnvVelSens;     // signed -63..+63; 0 = no velocity effect

    uint8_t TVALvlVelCur;
    uint8_t velRangeLow;    // Lowest velocity at which this partial sounds.
                            // Below it the partial is silent; at or above it
                            // the partial's velocity is rescaled to
                            // (v - low) * 127 / (127 - low). Measured:
                            // PROVENANCE.md (anomalies lane pending B).
    uint8_t TVALvlVSens;    // TVA Level Velocity Sensitivity:
                            // 0 = full velocity attenuation, 127 = none
    uint8_t velRangeHigh;   // Highest velocity at which this partial sounds:
                            // a plain gate, with no rescaling of the
                            // velocities below it (same measurements).
    int8_t volume;          // Volume attenuation (0x7f - 0)
    uint8_t dryLevel;       // JV Dry Level: attenuates the DIRECT tap only.
                            // The effect sends sit in parallel with the dry
                            // pair downstream of the chip's TVA, so they do
                            // not see this (P-0382 finding 2/8). 0x7f = unity,
                            // which is what every non-JV device stores.
    uint8_t TVABiasPoint;   // TVA Bias Point, 0=V shape, 1=key>85, 2=flat curve
    uint8_t TVABiasLevel;
    uint8_t TVALFO1Depth;
    uint8_t TVALFO2Depth;
    uint8_t TVAEnvL1;       // TVA Envelope L1 (L0 = 0)
    uint8_t TVAEnvL2;       // TVA Envelope L2
    uint8_t TVAEnvL3;       // TVA Envelope L3
    uint8_t TVAEnvL4;       // TVA Envelope L4 (L5 = 0)
    uint8_t TVAEnvT1;       // TVA Envelope T1 (Attack1)
    uint8_t TVAEnvT2;       // TVA Envelope T2 (Attack2)
    uint8_t TVAEnvT3;       // TVA Envelope T3 (Decay1)
    uint8_t TVAEnvT4;       // TVA Envelope T4 (Decay2)
    uint8_t TVAEnvT5;       // TVA Envelope T5 (Release)

    uint8_t TVAETKeyFP14;   // TVA Envelope Time Key Follow Presets (T1 - T4)
    uint8_t TVAETKeyFP5;    // TVA Envelope Time Key Follow Presets (T5)
    uint8_t TVAETKeyF14;    // TVA Envelope Time Key Follow (T1 - T4)
    uint8_t TVAETKeyF5;     // TVA Envelope Time Key Follow (T5)
    uint8_t TVAETVSens12;   // TVA Envelope Time Velocity Sensitivity (T1 - T2)
    uint8_t TVAETVSens35;   // TVA Envelope Time Velocity Sensitivity (T3 - T5)
  };

  // A Sound Canvas instrument has two partials; a JV patch has four tones,
  // which map onto the same structure.
  static const int MAX_PARTIALS = 4;

  struct Instrument {       // 204 bytes on the SC-55
    std::string name;

    uint8_t volume;         // Volume attenuation (0x7f - 0)
    uint8_t LFO1Waveform;
    uint8_t LFO1Rate;       // LFO frequency
    uint8_t LFO1Delay;
    uint8_t LFO1Fade;
    uint8_t partialsUsed;   // One bit per partial in use (JV patches use 4)
    uint8_t pitchCurve;
    uint8_t panKeyFlw;      // Non-zero selects the TVA pan key follow curve

    struct InstPartial partials[MAX_PARTIALS];
  };

  struct DrumSet {          // 1164 bytes
    uint16_t preset[128];
    uint8_t volume[128];
    uint8_t key[128];
    uint8_t assignGroup[128];// AKA exclusive class
    uint8_t panpot[128];
    uint8_t reverb[128];
    uint8_t chorus[128];
    uint8_t flags[128];     // 0x10 -> accept note on,  0x01 -> accept note off
    std::string name;       // 12 chars
  };

  struct LookupTables {
    // PROGROM
    std::vector<uint8_t> VelocityCurves;

    std::array<int, 136> KeyMapperIndex;
    int KeyMapperOffset;
    std::vector<uint8_t> KeyMapper;

    std::array<uint8_t, 128> TVAPanKeyFollow;

    // CPUROM
    std::array<int,      21> PitchParamScale;
    std::array<uint8_t,  21> EnvTimeKeyFollowSens;
    std::array<int,     256> EnvTimeScale;
    std::array<int,     128> envelopeTime;
    std::array<int,     128> LFORate;
    std::array<int,     128> LFODelayTime;
    std::array<int,     128> LFOTVFDepth;
    std::array<int,     128> LFOTVPDepth;
    std::array<uint8_t, 130> LFOSine;
    std::array<int,      21> TVFCutoffFreqKF;
    std::array<int,      11> TVFCutoffVSens;
    std::array<int,     128> TVFEnvDepth;
    std::array<int,     129> TVFCutoffFreq;
    std::array<uint8_t, 256> TVFResonanceFreq;
    std::array<uint8_t, 128> TVFResonance;
    std::array<int,      11> PitchEnvVelSens1;
    std::array<int,      11> PitchEnvVelSens2;
    std::array<int,     128> PitchEnvDepth;
    std::array<uint8_t,  64> TVFEnvScale;
    std::array<int,     128> PortamentoRate;
    std::array<uint8_t,  12> EnvSegmentStep;
    std::array<uint8_t,   9> EnvSegmentCurve;
    std::array<int,     257> TVAEnvExpChange;
    std::array<uint8_t, 130> TVABiasLevel;
    std::array<uint8_t, 129> TVAPanpot;
    std::array<uint8_t, 128> TVALevelIndex;
    std::array<uint8_t, 256> TVALevel;

    // The JV's own level law (P-0381). T maps a 15-bit level index to a linear
    // 16-bit gain and is NOT exponential, so it cannot be fitted; the velocity
    // curves are a bank of seven, selected per tone.
    std::array<int, 128>      JVLevel;
    std::array<uint8_t, 896>  JVVelCurves;

    // The JV's time-variant filter tables (P-0390). Read from its own control
    // ROM, each reproducing a closed form exactly - see devices/jv880.cc.
    // JVTvfExpCoarse is indexed by a SIGNED byte: entry 128 is -128.
    std::array<int, 256> JVTvfExpCoarse;
    std::array<int, 256> JVTvfExpFine;
    std::array<int, 128> JVTvfLimitSoft;
    std::array<int, 128> JVTvfDampSoft;
    std::array<int, 128> JVTvfLimitHard;
    std::array<int, 128> JVTvfDampHard;
    std::array<int, 128> JVTvfBase;
    std::array<int,  16> JVTvfCutoffKF;   // signed, cents per semitone
    std::array<int, 256> PitchFineExp;
    std::array<int, 47> PitchCoarseExp;
  };
  struct LookupTables lookupTables;

  enum class SynthGen {
    SC55    = 0,
    SC55mk2 = 1,
    SC88    = 2,
    SC88Pro = 3,
    JV880   = 4,
    JV1080  = 5
  };

  int dump_demo_songs(std::string path);
  bool intro_anim_available(void);
  std::vector<uint8_t> get_intro_anim(int animIndex = 0);

  std::string model(void) { return _model; }
  std::string version(void) { return _version; }
  std::string date(void) { return _date; }
  enum SynthGen generation(void) { return _synthGeneration; }

  // JV only: which patch each MIDI channel plays, taken from the performance.
  // -1 means no part of the performance listens on that channel.
  inline const std::array<int, 16>& device_channel_patch(void) { return _channelPatch; }
  inline int device_drum_channel(void) { return _deviceDrumChannel; }

  inline const std::array<int, 16>& device_channel_level(void) { return _channelLevel; }
  inline const std::array<int, 16>& device_channel_pan(void) { return _channelPan; }
  inline const std::array<int, 16>& device_channel_key_shift(void) { return _channelKeyShift; }

  // The JV's eight performance parts, as parts rather than as a channel map.
  // Two parts may share a MIDI channel to layer two patches, which a
  // channel-keyed map cannot express and the machine's own demo relies on.
  struct DevicePart { int patch, channel, level, pan, keyShift, reverb, chorus; bool rhythm; };

  // The performance's own effect settings. The manual's Performance Common
  // table gives Reverb Type at SysEx 0x0D, Level 0x0E, Time 0x0F, Feedback
  // 0x10, then the chorus; in the ROM record they sit one lower, from +12.
  struct DeviceEffects { int reverbType, reverbLevel, reverbTime, reverbFeedback,
                     chorusType, chorusLevel, chorusRate, chorusDepth,
                     chorusFeedback, chorusToReverb; };
  inline const DeviceEffects& device_effects(void) { return _deviceEffects; }
  inline const std::array<DevicePart, 8>& device_parts(void) { return _deviceParts; }
  inline const std::array<int, 16>& device_channel_reverb(void) { return _channelReverb; }
  inline const std::array<int, 16>& device_channel_chorus(void) { return _channelChorus; }
  const std::array<uint8_t, 128>& get_drum_sets_LUT(void) { return _drumSetsLUT; }
  const uint8_t max_polyphony(void);

  // Rate at which a partial whose voice has been taken is faded out
  const float voice_damp_rate(void);

  std::vector<std::vector<std::string>> get_instruments_list(void);
  std::vector<std::vector<std::string>> get_partials_list(void);
  std::vector<std::vector<std::string>> get_samples_list(void);

  inline struct Instrument& instrument(int i) { return _instruments[i]; }
  inline struct Partial& partial(int p) { return _partials[p]; }
  inline struct Sample& sample(int s) { return _samples[s]; }
  inline struct DrumSet& drumSet(int ds) { return _drumSets[ds]; }
  inline const std::array<std::array<uint16_t, 128>, 128>& variations() { return _variations; }
  inline const std::array<uint16_t, 128>& variation(int v) const { return _variations[v]; }

  inline int numPartials(void) { return _partials.size(); }
  inline int numSampleSets(void) { return _samples.size(); }
  inline int numInstruments(void) { return _instruments.size(); }
  inline int numDrumSets(void) { return _drumSets.size(); }

  inline std::vector<DrumSet> &get_drumsets_ref(void) { return _drumSets; }

private:
  std::string _romPath;

  std::string _model;
  std::string _version;
  std::string _date;

  enum SynthModel {
    sm_SC55,              // Original Sound Canvas
    sm_SC55mkII,          // Upgraded model
    sm_SCC1,              // ISA card version
    sm_SC88,
    sm_SC88Pro,
    sm_JV880,
    sm_JV1080,
  };
  enum SynthModel _synthModel;

  enum SynthGen _synthGeneration;

  // The only engine-side mapping from a device to its data.
  struct KnownDevice {
    const RomSignature *signature;
    enum SynthModel     model;
    enum SynthGen       generation;
  };
  static const KnownDevice KNOWN_DEVICES[];
  static const int         KNOWN_DEVICE_COUNT;

  static const DeviceProfile *_profile_for(enum SynthModel model);
  const RomLookupTable *_find_lookup(RomLookup id);

  int _read_lookup_tables_progrom(std::ifstream &romFile);
  int _read_lookup_tables_cpurom(std::ifstream &romFile);
  int _read_lut_16bit(std::ifstream &ifs, int pos, std::array<int, 11> &lut);
  int _read_lut_16bit(std::ifstream &ifs, int pos, std::array<int, 21> &lut);
  int _read_lut_16bit(std::ifstream &ifs, int pos, std::array<int, 47> &lut);
  int _read_lut_16bit(std::ifstream &ifs, int pos, std::array<int, 128> &lut);
  int _read_lut_16bit(std::ifstream &ifs, int pos, std::array<int, 129> &lut);
  int _read_lut_16bit(std::ifstream &ifs, int pos, std::array<int, 130> &lut);
  int _read_lut_16bit(std::ifstream &ifs, int pos, std::array<int, 136> &lut);
  int _read_lut_16bit(std::ifstream &ifs, int pos, std::array<int, 256> &lut);
  int _read_lut_16bit(std::ifstream &ifs, int pos, std::array<int, 257> &lut);

  int _identify_model(std::ifstream &romFile);
  const std::vector<uint32_t> &_banks(void);

  // To be replaced with std::endian::native from C++20
  inline bool _le_native(void) { uint16_t n = 1; return (*(uint8_t *) & n); } 

  uint16_t _native_endian_uint16(uint8_t *ptr);
  uint32_t _native_endian_3bytes_uint32(uint8_t *ptr);
  uint32_t _native_endian_4bytes_uint32(uint8_t *ptr);

    // The JV family has no drum-set or variation table in ROM, but it does keep
    // two banks of 64 preset patches there. They are read into the same
    // _partials, _samples and _instruments the SC-55 path fills.
  // Which device a ROM is, and where to find its records. The layout identifies
  // the device; the profile carries every offset and stride the readers need, so
  // adding a device is a data change and not a code change.
  struct DeviceEntry {
    enum SynthModel      model;
    enum SynthGen        generation;
    const DeviceProfile *profile;
  };
  static const DeviceEntry DEVICES[];
  static const int      DEVICE_COUNT;

  bool _identify_device(std::ifstream &romFile);
  void _init_neutral_partial(struct InstPartial &ip);
  int  _read_device_waveforms(void);
  int  _read_device_samples(void);
  int  _read_device_patches(void);
  void _init_device_lookup_tables(void);
  int  _read_device_performances(void);
  int  _read_device_rhythm(void);

  const DeviceProfile *_profile = nullptr;
public:
  const DeviceProfile *profile(void) const { return _profile; }

  // The loaded device's profile, never null. A ROM whose layout is not mapped
  // falls back to the Sound Canvas defaults, so the synthesis engine needs no
  // device branch of its own.
  const DeviceProfile *device(void) const
  { return _profile ? _profile : &SOUND_CANVAS_DEFAULT_PROFILE; }
private:   // set when the device is identified
  std::vector<uint8_t> _deviceRom;       // whole JV control ROM, for table walking
  std::array<int, 16> _channelPatch;
  std::array<int, 16> _channelLevel;
  std::array<int, 16> _channelPan;
  std::array<int, 16> _channelKeyShift;
  std::array<DevicePart, 8> _deviceParts;
  DeviceEffects _deviceEffects = { 4, 0x40, 0x40, 0, 0x40, 0x13 };
  std::vector<std::pair<uint8_t,uint8_t>> _instrumentSend;  // reverb, chorus per instrument
  std::array<int, 16> _channelReverb;
  std::array<int, 16> _channelChorus;
  int _deviceDrumChannel = 9;

  int _read_instruments(std::ifstream &romFile);
  int _read_partials(std::ifstream &romFile);
  int _read_variations(std::ifstream &romFile);
  int _read_samples(std::ifstream &romFile);
  int _read_drum_sets(std::ifstream &romFile);

  std::array<uint8_t, 128> _drumSetsLUT;

  std::vector<Instrument> _instruments;
  std::vector<Partial> _partials;
  std::vector<Sample> _samples;
  std::vector<DrumSet> _drumSets;
  std::array<std::array<uint16_t, 128>, 128> _variations;

  ControlRom();

};

}

#endif  // __CONTROL_ROM_H__
