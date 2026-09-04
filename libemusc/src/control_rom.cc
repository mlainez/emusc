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

// The SC-55 linueup controls the audio processing with instructions from:
//  * The internal 32k EPROM on the H8/532 main CPU (CPUROM)
//  * External 256kB (SC-55) or 512kB (SC-55mkII) EPROM (PROGROM)

// These two ROMs are very tightly connected and extends each other. They must
// therefore always be of the same ROM set / version.

// This class reads both these ROM files and combines their data for a complete
// set of control data to modify the audio stored in the PCM ROMs.

// The external EPROM (PROGROM) is encrypted. Decoding is based on the
// SC55_Soundfont generator written by Kitrinx and NewRisingSun.
// For more information, see [ https://github.com/Kitrinx/SC55_Soundfont ]


#include <cstdlib>
#include "control_rom.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <iostream>
#include <iomanip>


namespace EmuSC {


ControlRom::ControlRom(std::string romPath, std::string cpuRomPath)
  : _romPath(romPath)
{
  // No drum-set table is guaranteed. _drumSetsLUT is read from ROM only on the
  // SC-55 path, so on any other generation it would otherwise hold indeterminate
  // values, and Settings::update_drum_set() would index an empty _drumSets.
  // 0xff is the table's own "no drum set in this bank" marker.
  _drumSetsLUT.fill(0xff);

  // External EPROM containing control data
  std::ifstream romFile(romPath, std::ios::binary | std::ios::in);
  if (!romFile.is_open())
    throw(std::string("Unable to open control ROM|: ") + romPath);

  if (_identify_model(romFile))
    throw(std::string("Unknown control ROM file!"));

  _profile = _profile_for(_synthModel);

  // A ROM we can name but have no profile for is recognised, not supported: say
  // so rather than reading it with another device's offsets.
  if (!_profile)
    throw(std::string("ROM identified as ") + _model +
          std::string(", which is not supported yet!"));

  // The JV family: partials, samples and the two preset patch banks live in
  // ROM. There is no drum-set table and no separate CPU ROM holding the lookup
  // tables, so the SC-55 sequence below does not apply.
  if (_synthModel == sm_JV880 || _synthModel == sm_JV1080) {
    _read_device_waveforms();
    _read_device_samples();
    if (_profile->records->has_patches()) {
      _read_device_patches();
      _read_device_performances();
      _read_device_rhythm();
    }
    _init_device_lookup_tables();
    romFile.close();
    return;
  }

  // Read internal data structures from ROM file
  _read_instruments(romFile);
  _read_partials(romFile);
  _read_samples(romFile);
  _read_variations(romFile);
  _read_drum_sets(romFile);
  _read_lookup_tables_progrom(romFile);

  romFile.close();

  // CPU EPROM
  romFile.open(cpuRomPath, std::ios::binary | std::ios::in);
  if (!romFile.is_open())
    throw(std::string("Unable to open CPU ROM: ") + romPath);

  // Verify size (always 32kB for all SC-55 variants)
  std::streampos fileSize = romFile.tellg();
  romFile.seekg(0, std::ios::end);
  fileSize = romFile.tellg() - fileSize;
  if (fileSize != 32768)
    throw(std::string("Invalid CPU ROM (size != 32kB): ") + romPath);

  _read_lookup_tables_cpurom(romFile);

  romFile.close();

  if (0)
    std::cout << "EmuSC: Found " << _instruments.size() << " instruments, "
	      << _partials.size() << " parts, "
	      << _samples.size() << " samples and "
	      << _drumSets.size() << " drum sets" << std::endl;
}


ControlRom::~ControlRom()
{}


uint16_t ControlRom::_native_endian_uint16(uint8_t *ptr)
{
  if (_le_native())
    return (ptr[0] << 8 | ptr[1]);

  return (ptr[1] << 8 | ptr[0]);
}


uint32_t ControlRom::_native_endian_3bytes_uint32(uint8_t *ptr)
{
  uint32_t result = 0;
  uint8_t *result_ptr = (uint8_t *) &result;

  if (!_le_native()) {
    for (uint32_t x = 0; x < 3; x++) {
      result_ptr[x] = ptr[x];
    }
  } else {
    for (int x = 0; x < 3; x++) {
      result_ptr[x] = ptr[2 - x];
    }
  }

  return result;
}


uint32_t ControlRom::_native_endian_4bytes_uint32(uint8_t *ptr)
{
  uint32_t result = 0;
  uint8_t *result_ptr = (uint8_t *) &result;

  if (!_le_native()) {
    for (uint32_t x = 0; x < 4; x++) {
      result_ptr[x] = ptr[x];
    }
  } else {
    for (int x = 0; x < 4; x++) {
      result_ptr[x] = ptr[3 - x];
    }
  }

  return result;
}


// The engine's one list of devices: a signature to recognise the ROM by, and the
// model it names. Adding a device is a device file plus a row here.
const ControlRom::KnownDevice ControlRom::KNOWN_DEVICES[] = {
  { &SC55_SIGNATURE,     sm_SC55,     SynthGen::SC55    },
  { &SC55MKII_SIGNATURE, sm_SC55mkII, SynthGen::SC55mk2 },
  { &SCB55_SIGNATURE,    sm_SC55mkII, SynthGen::SC55mk2 },
  { &SCC1_SIGNATURE,     sm_SCC1,     SynthGen::SC55    },
  { &SC88_SIGNATURE,     sm_SC88,     SynthGen::SC88    }
};
const int ControlRom::KNOWN_DEVICE_COUNT =
  (int) (sizeof(KNOWN_DEVICES) / sizeof(KNOWN_DEVICES[0]));


int ControlRom::_identify_model(std::ifstream &romFile)
{
  char data[64];

  for (int i = 0; i < KNOWN_DEVICE_COUNT; i++) {
    const RomSignature &sig = *KNOWN_DEVICES[i].signature;
    if (sig.readLength > (int) sizeof(data))
      continue;

    // A short ROM leaves the stream in a fail state, which would silently
    // sink every signature tested after it.
    romFile.clear();
    romFile.seekg(sig.offset);
    romFile.read(data, sig.readLength);
    if (romFile.gcount() < sig.readLength ||
        strncmp(data, sig.match, sig.matchLength))
      continue;

    _model.assign(sig.modelName);
    _synthModel      = KNOWN_DEVICES[i].model;
    _synthGeneration = KNOWN_DEVICES[i].generation;

    switch (sig.versionStyle) {
    case RomVersionStyle::Inline:
      _version.assign(&data[3], 4);
      _date.assign(&data[24], 5);
      break;

    case RomVersionStyle::SeparateBcd: {
      romFile.seekg(sig.versionOffset);
      romFile.read(data, 10);
      _version.assign(data, 4);
      std::stringstream ss;
      ss << "19" << std::hex << (int) (uint8_t) data[7] << "-"
         << (int) (uint8_t) data[8] << "-" << (int) (uint8_t) data[9];
      _date.assign(ss.str());
      break;
    }

    case RomVersionStyle::Unknown:
      _version.assign("?");
      _date.assign("?");
      break;
    }

    return 0;
  }

  // No GS banner: the JV family identifies itself by its table structure.
  romFile.clear();
  if (_identify_device(romFile))
    return 0;

  return -1;
}


const std::vector<uint32_t> &ControlRom::_banks(void)
{
  if (_profile && _profile->soundCanvas)
    return *_profile->soundCanvas->banks;

  static const std::vector<uint32_t> none;
  return none;
}


int ControlRom::_read_instruments(std::ifstream &romFile)
{
  // ROM is split in 8 banks
  const std::vector<uint32_t> &banks = _banks();

  // Instruments are in bank 0 & 3, each instrument block using 216 bytes
  for (int32_t x = banks[0]; x < banks[4]; x += 216) {

    // Skip area between bank 0 and 3
    if (x == banks[1])
      x = banks[3];

    char data[92];
    romFile.seekg(x);
    struct Instrument i;

    // First 12 bytes are the instrument name
    romFile.read(data, 32);

    // Skip empty slots in the ROM file that have no instrument name
    if (data[0] == '\0')
      continue;

    i.name.assign(data, 12);
    i.name.erase(i.name.find_last_not_of(' ') + 1);

    i.volume       = data[12];
    i.LFO1Waveform = data[14];
    i.LFO1Rate     = data[15];
    i.LFO1Delay    = data[16];
    i.LFO1Fade     = data[17];
    i.partialsUsed = data[18] & 0x03;  // the SC-55 has two partials
    i.pitchCurve   = data[19];
    i.panKeyFlw    = data[31];

    // We have 2 partial parameters sets; starting in bank position 34 & 126
    for (int p = 0; p < 2; p++) {
      romFile.seekg(x + 32 + (p * 92));
      romFile.read(data, 92);
      i.partials[p].rootKeyOffset = data[1];
      i.partials[p].partialIndex  = _native_endian_uint16((uint8_t *) data + 2);
      i.partials[p].LFO2Waveform  = data[4];
      i.partials[p].LFO2Rate      = data[5];
      i.partials[p].LFO2Delay     = data[6];
      i.partials[p].LFO2Fade      = data[7];
      i.partials[p].TVFFlags      = data[8];
      i.partials[p].panpot        = data[9];
      i.partials[p].coarsePitch   = data[10];
      i.partials[p].finePitch     = data[11];
      i.partials[p].randPitch     = data[12];
      i.partials[p].pitchKeyFlw   = data[13];
      i.partials[p].TVPLFO1Depth  = data[14];
      i.partials[p].TVPLFO2Depth  = data[15];
      i.partials[p].pitchEnvDepth = data[16];
      i.partials[p].pitchEnvL0    = data[18];
      i.partials[p].pitchEnvL1    = data[19];
      i.partials[p].pitchEnvL2    = data[20];
      i.partials[p].pitchEnvL3    = data[21];
      i.partials[p].pitchEnvL5    = data[22];
      i.partials[p].pitchEnvT1    = data[23];
      i.partials[p].pitchEnvT2    = data[24];
      i.partials[p].pitchEnvT3    = data[25];
      i.partials[p].pitchEnvT4    = data[26];
      i.partials[p].pitchEnvT5    = data[27];
      i.partials[p].pitchETKeyFP14= data[30];
      i.partials[p].pitchETKeyFP5 = data[31];
      i.partials[p].pitchETKeyF14 = data[32];
      i.partials[p].pitchETKeyF5  = data[33];
      i.partials[p].pitchEnvVSens = data[34];
      i.partials[p].pitchEnvTVSens= data[35];
      i.partials[p].TVFCOFVelCur  = data[36];
      i.partials[p].TVFBaseFlt    = data[37];
      i.partials[p].TVFResonance  = data[38];
      i.partials[p].TVFType       = data[39];
      i.partials[p].TVFCFKeyFlwC  = data[40];
      i.partials[p].TVFCFKeyFlw   = data[41];
      i.partials[p].TVFLFO1Depth  = data[42];
      i.partials[p].TVFLFO2Depth  = data[43];
      i.partials[p].TVFEnvDepth   = data[44];
      i.partials[p].TVFEnvL1      = data[45];
      i.partials[p].TVFEnvL2      = data[46];
      i.partials[p].TVFEnvL3      = data[47];
      i.partials[p].TVFEnvL4      = data[48];
      i.partials[p].TVFEnvL5      = data[49];
      i.partials[p].TVFEnvT1      = data[50];
      i.partials[p].TVFEnvT2      = data[51];
      i.partials[p].TVFEnvT3      = data[52];
      i.partials[p].TVFEnvT4      = data[53];
      i.partials[p].TVFEnvT5      = data[54];
      i.partials[p].TVFETKeyFP14  = data[57];
      i.partials[p].TVFETKeyFP5   = data[58];
      i.partials[p].TVFETKeyF14   = data[59];
      i.partials[p].TVFETKeyF5    = data[60];
      i.partials[p].TVFCOFVSens   = data[61];
      i.partials[p].TVFETVSens12  = data[62];
      i.partials[p].TVFETVSens35  = data[63];
      i.partials[p].dryLevel      = 0x7f;   // no dry attenuator on this device
      i.partials[p].TVALvlVelCur  = data[64];
      i.partials[p].velRangeLow   = data[65];
      i.partials[p].TVALvlVSens   = data[66];
      i.partials[p].velRangeHigh  = data[67];
      i.partials[p].volume        = data[69];
      i.partials[p].TVABiasPoint  = data[70];
      i.partials[p].TVABiasLevel  = data[71];
      i.partials[p].TVALFO1Depth  = data[72];
      i.partials[p].TVALFO2Depth  = data[73];
      i.partials[p].TVAEnvL1      = data[74];
      i.partials[p].TVAEnvL2      = data[75];
      i.partials[p].TVAEnvL3      = data[76];
      i.partials[p].TVAEnvL4      = data[77];
      i.partials[p].TVAEnvT1      = data[78];
      i.partials[p].TVAEnvT2      = data[79];
      i.partials[p].TVAEnvT3      = data[80];
      i.partials[p].TVAEnvT4      = data[81];
      i.partials[p].TVAEnvT5      = data[82];
      i.partials[p].TVAETKeyFP14  = data[85];
      i.partials[p].TVAETKeyFP5   = data[86];
      i.partials[p].TVAETKeyF14   = data[87];
      i.partials[p].TVAETKeyF5    = data[88];
      i.partials[p].TVAETVSens12  = data[89];
      i.partials[p].TVAETVSens35  = data[90];
    }

    _instruments.push_back(i);

    if (0)
      std::cout << "  -> Instrument " << _instruments.size() << ": " << i.name
          << " partial0=" << (int) i.partials[0].partialIndex
          << " partial1=" << (int) i.partials[1].partialIndex
          << std::endl;
    }

  return 0;
}


int ControlRom::_read_partials(std::ifstream &romFile)
{
  // ROM is split in 8 banks
  const std::vector<uint32_t> &banks = _banks();

  // Partials are in bank 1 & 4, each partial block using 60 bytes
  for (int32_t x = banks[1]; x < banks[5]; x += 60) {

    // Skip area between bank 1 and 4
    if (x == banks[2])
      x = banks[4];

    char data[32];
    romFile.seekg(x);
    struct Partial p;

    // First 12 bytes are the partial name
    romFile.read(data, 12);
    p.name.assign(data, 12);
    p.name.erase(p.name.find_last_not_of(' ') + 1);

    // 16 byte array of break values for tone pitch
    romFile.read(data, 16);
    for (int i = 0; i < 16; i++)
      p.breaks[i] = data[i];

    // 16 2-byte array with accompanying sample IDs
    romFile.read(data, 32);
    for (int i = 0; i < 16; i++)
      p.samples[i] = _native_endian_uint16((uint8_t *) &data[2 * i]);

    // Skip empty slots in the ROM file that has no partial name
    if (p.name[0]) {
      _partials.push_back(p);

      if (0)
	std::cout << "  -> Partial group " << _partials.size() <<  ": "
		  << p.name << std::endl;
    }
  }

  return 0;
}


int ControlRom::_read_variations(std::ifstream &romFile)
{
  // ROM is split in 8 banks
  const std::vector<uint32_t> &banks = _banks();

  // Variations are in bank 6, a table of 128 x 128 2 byte values
  for (int x = 0; x < 128; x++) {
    const size_t offset = banks[6] + x * 128 * sizeof(uint16_t);
    romFile.seekg(offset);

    for (int y = 0; y < 128; y++) {
      char data[2];
      romFile.read(data, 2);
      _variations[x][y] = _native_endian_uint16(reinterpret_cast<uint8_t*>(data));
    }
  }

  if (0) {
    int i = 0;
    for (auto v : _variations) {
      std::cout << "  -> Variations " << i++ << ": ";
      for (int y = 0; y < 128; y++)
	if (v[y] == 0xffff)
	  std::cout << "-,";
	else
	  std::cout << v[y] << ",";
      std::cout << '\b' << " " << std::endl;
    }
  }

  return 0;
}


int ControlRom::_read_samples(std::ifstream &romFile)
{
  // ROM is split in 8 banks
  const std::vector<uint32_t> &banks = _banks();

  // Samples are in bank 2 & X, each sample block using 16 bytes
  for (int32_t x = banks[2]; x < banks[6]; x += 16) {

    // Skip area between bank 1 and 4
    if (x == banks[3])
      x = banks[5];

    char data[16];
    romFile.seekg(x);
    struct Sample s;

    romFile.read(data, 16);
    s.volume = data[0];
    s.address = _native_endian_3bytes_uint32((uint8_t *) &data[1]);
    s.portaOffset = _native_endian_uint16((uint8_t *) &data[4]);
    s.sampleLen = _native_endian_uint16((uint8_t *) &data[6]);
    s.loopLen = _native_endian_uint16((uint8_t *) &data[8]);
    s.loopMode = data[10];
    s.rootKey = data[11];
    s.pitchInit = _native_endian_uint16((uint8_t *) &data[12]);
    s.pitchSust = _native_endian_uint16((uint8_t *) &data[14]);
      s.reverse = false;              // SC-55 has no reverse playback
    
    if (s.sampleLen) {                          // Ignore empty parts
      _samples.push_back(s);
      
      if (0)
	std::cout << "  -> Sample " << std::setw(3) << _samples.size()
		  << ": V=" << std::setw(3) << +s.volume
		  << " AE=" << std::setw(5) << +s.portaOffset
		  << " SL=" << std::setw(5) << +s.sampleLen
		  << " LL=" << std::setw(5) << +s.loopLen
		  << " LM=" << std::setw(3) << +s.loopMode
		  << " RK=" << std::setw(3) << +s.rootKey
		  << " PI=" << std::setw(5) << +s.pitchInit - 1024
		  << " PS=" << std::setw(4) << +s.pitchSust - 1024
		  << std::endl;
    }
  }
  
  return 0;
}           


int ControlRom::_read_drum_sets(std::ifstream &romFile)
{
  // ROM is split in 8 banks
  const std::vector<uint32_t> &banks = _banks();

  // The drum sets are defined in bank 7, starting with a 128 byte lookup table
  int32_t x = banks[7];
  romFile.seekg(x);
  romFile.read(reinterpret_cast<char*>(_drumSetsLUT.data()), 128);

  // After the map array there are 14 drum set definitions in 1164 byte blocks 
  char data[128];
  const SoundCanvasLayout *sc = _profile->soundCanvas;
  for (x = banks[7] + 128; x < (int) sc->drumSetTableEnd; x += sc->drumSetStride) {
    struct DrumSet d;

    // First array is 16 bit instrument reference
    for (int i = 0; i < 128; i++) {
      romFile.read(data, 2);
      d.preset[i] = _native_endian_uint16((uint8_t *) &data[0]);
    }

    // Next 7 arrays are 8 bit data
    romFile.read((char *) d.volume, 128);
    romFile.read((char *) d.key, 128);
    romFile.read((char *) d.assignGroup, 128);
    romFile.read((char *) d.panpot, 128);
    romFile.read((char *) d.reverb, 128);
    romFile.read((char *) d.chorus, 128);
    romFile.read((char *) d.flags, 128);

    // Last 12 bytes are the drum name
    romFile.read(data, 12);
    d.name.assign(data, 12);
    d.name.erase(d.name.find_last_not_of(' ') + 1);

    // Ignore undocumented drum sets and unused memory slots
    if ((d.name.rfind("AC.", 0) == 0) || data[0] < 0)
      continue;

    _drumSets.push_back(d);

    if (0)
      std::cout << "  -> Drum " << _drumSets.size() << ": " << d.name
		<< std::endl;
  }

  return _drumSets.size();
}


int ControlRom::_read_lookup_tables_progrom(std::ifstream &romFile)
{
  if (!_profile || !_profile->soundCanvas || !_profile->soundCanvas->program) {
    std::cerr << "libEmuSC: Unsupported ROM file!" << std::endl;
    exit(0);
  }
  const ProgramRomMap *PROGmmLUT = _profile->soundCanvas->program;
  const int numVCurves = _profile->soundCanvas->velocityCurves;

  lookupTables.VelocityCurves.resize(128 * numVCurves);
  romFile.seekg(PROGmmLUT->VelocityCurves);
  romFile.read(reinterpret_cast<char*> (lookupTables.VelocityCurves.data()),
               lookupTables.VelocityCurves.size());

  _read_lut_16bit(romFile, PROGmmLUT->KeyMapperIndex,
                  lookupTables.KeyMapperIndex);

  int kmSize = 128 +
    lookupTables.KeyMapperIndex.back() - lookupTables.KeyMapperIndex.front();
  lookupTables.KeyMapper.resize(kmSize);

  romFile.seekg(PROGmmLUT->KeyMapper);
  romFile.read(reinterpret_cast<char*> (lookupTables.KeyMapper.data()), kmSize);
  lookupTables.KeyMapperOffset =
    PROGmmLUT->KeyMapper - _profile->soundCanvas->keyMapperBase;

  if (PROGmmLUT->TVAPanKeyFollow) {
    romFile.seekg(PROGmmLUT->TVAPanKeyFollow);
    romFile.read(reinterpret_cast<char*> (lookupTables.TVAPanKeyFollow.data()),
                 lookupTables.TVAPanKeyFollow.size());
  } else {
    lookupTables.TVAPanKeyFollow.fill(0x40);       // Centre for every key
  }

  return 0;
}


int ControlRom::_read_lookup_tables_cpurom(std::ifstream &romFile)
{
  if (!_profile || !_profile->soundCanvas || !_profile->soundCanvas->cpu) {
    std::cerr << "libEmuSC: Unsupported ROM file!" << std::endl;
    exit(0);
  }
  const CpuRomMap *CPUmmLUT = _profile->soundCanvas->cpu;

  // 8-bit values
  romFile.seekg(CPUmmLUT->EnvTimeKeyFollowSens);
  romFile.read(reinterpret_cast<char*> (&lookupTables.EnvTimeKeyFollowSens),21);
  romFile.seekg(CPUmmLUT->TVFResonanceFreq);
  romFile.read(reinterpret_cast<char*> (&lookupTables.TVFResonanceFreq), 256);
  romFile.seekg(CPUmmLUT->TVFResonance);
  romFile.read(reinterpret_cast<char*> (&lookupTables.TVFResonance), 128);
  romFile.seekg(CPUmmLUT->TVFEnvScale);
  romFile.read(reinterpret_cast<char*> (&lookupTables.TVFEnvScale), 64);
  romFile.seekg(CPUmmLUT->LFOSine);
  romFile.read(reinterpret_cast<char*> (&lookupTables.LFOSine), 130);
  romFile.seekg(CPUmmLUT->TVABiasLevel);
  romFile.read(reinterpret_cast<char*> (&lookupTables.TVABiasLevel), 130);
  romFile.seekg(CPUmmLUT->TVAPanpot);
  romFile.read(reinterpret_cast<char*> (&lookupTables.TVAPanpot), 129);
  romFile.seekg(CPUmmLUT->TVALevelIndex);
  romFile.read(reinterpret_cast<char*> (&lookupTables.TVALevelIndex), 128);
  romFile.seekg(CPUmmLUT->TVALevel);
  romFile.read(reinterpret_cast<char*> (&lookupTables.TVALevel), 256);
  romFile.seekg(CPUmmLUT->EnvSegmentStep);
  romFile.read(reinterpret_cast<char*> (&lookupTables.EnvSegmentStep), 12);
  romFile.seekg(CPUmmLUT->EnvSegmentCurve);
  romFile.read(reinterpret_cast<char*> (&lookupTables.EnvSegmentCurve), 9);

  // 16-bit values
  _read_lut_16bit(romFile, CPUmmLUT->PitchParamScale, lookupTables.PitchParamScale);
  _read_lut_16bit(romFile, CPUmmLUT->EnvTimeScale, lookupTables.EnvTimeScale);
  _read_lut_16bit(romFile, CPUmmLUT->PortamentoRate, lookupTables.PortamentoRate);
  _read_lut_16bit(romFile, CPUmmLUT->TVFEnvDepth, lookupTables.TVFEnvDepth);
  _read_lut_16bit(romFile, CPUmmLUT->TVFCutoffFreq, lookupTables.TVFCutoffFreq);
  _read_lut_16bit(romFile, CPUmmLUT->EnvelopeTime, lookupTables.envelopeTime);
  _read_lut_16bit(romFile, CPUmmLUT->LFORate, lookupTables.LFORate);
  _read_lut_16bit(romFile, CPUmmLUT->LFODelayTime, lookupTables.LFODelayTime);
  _read_lut_16bit(romFile, CPUmmLUT->LFOTVFDepth, lookupTables.LFOTVFDepth);
  _read_lut_16bit(romFile, CPUmmLUT->LFOTVPDepth, lookupTables.LFOTVPDepth);
  _read_lut_16bit(romFile, CPUmmLUT->PitchEnvVelSens1, lookupTables.PitchEnvVelSens1);
  _read_lut_16bit(romFile, CPUmmLUT->PitchEnvVelSens2, lookupTables.PitchEnvVelSens2);
  _read_lut_16bit(romFile, CPUmmLUT->PitchEnvDepth, lookupTables.PitchEnvDepth);
  _read_lut_16bit(romFile, CPUmmLUT->TVAEnvExpChange, lookupTables.TVAEnvExpChange);
  _read_lut_16bit(romFile, CPUmmLUT->TVFCutoffVSens, lookupTables.TVFCutoffVSens);
  _read_lut_16bit(romFile, CPUmmLUT->TVFCutoffFreqKF, lookupTables.TVFCutoffFreqKF);
  _read_lut_16bit(romFile, CPUmmLUT->PitchFineExp, lookupTables.PitchFineExp);
  _read_lut_16bit(romFile, CPUmmLUT->PitchCoarseExp, lookupTables.PitchCoarseExp);

  return 0;
}


int ControlRom::_read_lut_16bit(std::ifstream &ifs, int pos,
                                std::array<int, 11> &lut)
{
  ifs.seekg(pos);

  for (int i = 0; i < 11; i ++) {
    uint16_t value;
    if (!ifs.read(reinterpret_cast<char*>(&value), sizeof(value))) {
      std::cerr << "libEmuSC: Error reading LUT from ROM" << std::endl;
      return i;
    }

    lut[i] = static_cast<int>(_native_endian_uint16((uint8_t *) &value));
  }

  return 11;
}


int ControlRom::_read_lut_16bit(std::ifstream &ifs, int pos,
                                std::array<int, 21> &lut)
{
  ifs.seekg(pos);

  for (int i = 0; i < 21; i ++) {
    uint16_t value;
    if (!ifs.read(reinterpret_cast<char*>(&value), sizeof(value))) {
      std::cerr << "libEmuSC: Error reading LUT from ROM" << std::endl;
      return i;
    }

    lut[i] = static_cast<int>(_native_endian_uint16((uint8_t *) &value));
  }

  return 21;
}


int ControlRom::_read_lut_16bit(std::ifstream &ifs, int pos,
                                std::array<int, 47> &lut)
{
  ifs.seekg(pos);

  for (int i = 0; i < 47; i ++) {
    uint16_t value;
    if (!ifs.read(reinterpret_cast<char*>(&value), sizeof(value))) {
      std::cerr << "libEmuSC: Error reading LUT from ROM" << std::endl;
      return i;
    }

    lut[i] = static_cast<int>(_native_endian_uint16((uint8_t *) &value));
  }

  return 47;
}


int ControlRom::_read_lut_16bit(std::ifstream &ifs, int pos,
                                std::array<int, 128> &lut)
{
  ifs.seekg(pos);

  for (int i = 0; i < 128; i ++) {
    uint16_t value;
    if (!ifs.read(reinterpret_cast<char*>(&value), sizeof(value))) {
      std::cerr << "libEmuSC: Error reading LUT from ROM" << std::endl;
      return i;
    }

    lut[i] = static_cast<int>(_native_endian_uint16((uint8_t *) &value));
  }

  return 128;
}


int ControlRom::_read_lut_16bit(std::ifstream &ifs, int pos,
                                std::array<int, 129> &lut)
{
  ifs.seekg(pos);

  for (int i = 0; i < 129; i ++) {
    uint16_t value;
    if (!ifs.read(reinterpret_cast<char*>(&value), sizeof(value))) {
      std::cerr << "libEmuSC: Error reading LUT from ROM" << std::endl;
      return i;
    }

    lut[i] = static_cast<int>(_native_endian_uint16((uint8_t *) &value));
  }

  return 129;
}


int ControlRom::_read_lut_16bit(std::ifstream &ifs, int pos,
                                std::array<int, 130> &lut)
{
  ifs.seekg(pos);

  for (int i = 0; i < 130; i ++) {
    uint16_t value;
    if (!ifs.read(reinterpret_cast<char*>(&value), sizeof(value))) {
      std::cerr << "libEmuSC: Error reading LUT from ROM" << std::endl;
      return i;
    }

    lut[i] = static_cast<int>(_native_endian_uint16((uint8_t *) &value));
  }

  return 130;
}


int ControlRom::_read_lut_16bit(std::ifstream &ifs, int pos,
                                std::array<int, 136> &lut)
{
  ifs.seekg(pos);

  for (int i = 0; i < 136; i ++) {
    uint16_t value;
    if (!ifs.read(reinterpret_cast<char*>(&value), sizeof(value))) {
      std::cerr << "libEmuSC: Error reading LUT from ROM" << std::endl;
      return i;
    }

    lut[i] = static_cast<int>(_native_endian_uint16((uint8_t *) &value));
  }

  return 136;
}


int ControlRom::_read_lut_16bit(std::ifstream &ifs, int pos,
                                std::array<int, 256> &lut)
{
  ifs.seekg(pos);

  for (int i = 0; i < 256; i ++) {
    uint16_t value;
    if (!ifs.read(reinterpret_cast<char*>(&value), sizeof(value))) {
      std::cerr << "libEmuSC: Error reading LUT from ROM" << std::endl;
      return i;
    }

    lut[i] = static_cast<int>(_native_endian_uint16((uint8_t *) &value));
  }

  return 256;
}


int ControlRom::_read_lut_16bit(std::ifstream &ifs, int pos,
                                std::array<int, 257> &lut)
{
  ifs.seekg(pos);

  for (int i = 0; i < 257; i ++) {
    uint16_t value;
    if (!ifs.read(reinterpret_cast<char*>(&value), sizeof(value))) {
      std::cerr << "libEmuSC: Error reading LUT from ROM" << std::endl;
      return i;
    }

    lut[i] = static_cast<int>(_native_endian_uint16((uint8_t *) &value));
  }

  return 257;
}


const uint8_t ControlRom::max_polyphony(void)
{
  return _profile ? _profile->maxPolyphony : 0;
}


const float ControlRom::voice_damp_rate(void)
{
  return _profile ? _profile->voiceDampRate : 0.0f;
}


const uint8_t ControlRom::device_voice_reserve(int part)
{
  if (!_profile || !_profile->records)
    return 0;

  const PerformanceLayout &V = _profile->records->performance;
  if (!V.offset || !V.voiceReserve || part < 0 || part >= V.parts)
    return 0;

  const size_t at = V.offset + V.bootIndex * V.stride + V.voiceReserve + part;
  if (at >= _deviceRom.size())
    return 0;

  return _deviceRom[at];
}


int ControlRom::dump_demo_songs(std::string path)
{
  int index = 1;
  std::cout << "EmuSC: Searching for MIDI songs in control ROM..." << std::endl;

  std::ifstream romFile(_romPath, std::ios::binary | std::ios::in);
  if (!romFile.is_open()) {
    std::cerr << "Unable to open control ROM: " << _romPath << std::endl;
    return -1;
  }

  // MIDI files are placed at different places in the ROM depending on model
  const SoundCanvasLayout *sc = _profile ? _profile->soundCanvas : nullptr;
  int romIndex = sc ? (int) sc->demoSongOffset : 0;
  int romSize;
  if (sc && !sc->demoSongRunsToRomEnd) {
    romSize = _banks()[0];
  } else {
    romFile.seekg(0, std::ios::end);
    romSize = romFile.tellg();
  }

  std::vector<uint8_t> romData(romSize - romIndex);
  romFile.seekg(romIndex);
  romFile.read((char*) &romData[0], romSize);
  romFile.close();

  for (uint32_t i = 0; i < romData.size() - 6; i++) {
    if (romData[i + 0] == 0x4d &&
	romData[i + 1] == 0x54 &&
	romData[i + 2] == 0x68 &&
	romData[i + 3] == 0x64 &&
	romData[i + 4] == 0x00 &&
	romData[i + 5] == 0x00 &&
	romData[i + 6] == 0x00 &&
	romData[i + 7] == 0x06) {

      uint16_t numTracks = _native_endian_uint16(&romData[i+10]);
      uint32_t fileSize = 14;
      for (int n = 0; n < numTracks; n++) {
	if (romData[i + fileSize] == 0x4d &&
	    romData[i + fileSize + 1] == 0x54 &&
	    romData[i + fileSize + 2] == 0x72 &&
	    romData[i + fileSize + 3] == 0x6b) {
	  fileSize += _native_endian_4bytes_uint32(&romData[i + fileSize + 4]);
	  fileSize += 8;                                    // Add track header
	} else {
	  return -1;
	}
      }

      if (path.back() != '/')
	path.append("/");

      std::string fileName = "sc_song_" + std::to_string(index++) + ".mid";

      std::ofstream midiFile(path + fileName, std::ios::out | std::ios::binary);
      midiFile.write((char*) &romData[i], fileSize);
      if (midiFile.good())
	std::cout << " -> Found demo song at 0x" << std::hex << romIndex + i
		  << " (" << std::dec << (int) fileSize << " bytes)"
		  << std::endl
		  << "  -> File written to " << path + fileName << std::endl;
      else
	std::cout << " -> Error writing demo song to disk: " << path
		  << " Check write permissions and available space."
		  << std::endl;
      midiFile.close();
    }
  }

  if (index == 1)
    std::cout << "EmuSC: Control ROM contained no MIDI files " << std::endl;

  return index - 1;
}


std::vector<std::vector<std::string>> ControlRom::get_instruments_list(void)
{
  std::vector<std::vector<std::string>> instListVector;

  // First row is header
  std::vector<std::string> headerVector;
  headerVector.push_back("Name");
  headerVector.push_back("Partial 0");
  headerVector.push_back("Partial 1");
  instListVector.push_back(headerVector);

  for (struct Instrument inst: _instruments) {
    std::vector<std::string> instVector;
    instVector.push_back(inst.name);
    instVector.push_back(std::to_string(inst.partials[0].partialIndex));
    instVector.push_back(std::to_string(inst.partials[1].partialIndex));
    instListVector.push_back(instVector);
  }

  return instListVector;
}


std::vector<std::vector<std::string>> ControlRom::get_partials_list(void)
{
  std::vector<std::vector<std::string>> partListVector;

  // First row is header
  std::vector<std::string> headerVector;
  headerVector.push_back("Name");
  for (int i = 0; i < 16; i++)
    headerVector.push_back("Break " + std::to_string(i));
  for (int i = 0; i < 16; i++)
    headerVector.push_back("Sample " + std::to_string(i));
  partListVector.push_back(headerVector);

  for (struct Partial partial: _partials) {
    std::vector<std::string> partVector;
    partVector.push_back(partial.name);
    for (int i = 0; i < 16; i++)
      partVector.push_back(std::to_string(partial.breaks[i]));
    for (int i = 0; i < 16; i++)
      partVector.push_back(std::to_string(partial.samples[i]));
    partListVector.push_back(partVector);
  }

  return partListVector;
}


std::vector<std::vector<std::string>> ControlRom::get_samples_list(void)
{
  std::vector<std::vector<std::string>> samplesListVector;

  // First row is header
  std::vector<std::string> headerVector;
  headerVector.push_back("Volume");
  headerVector.push_back("Attack Start");
  headerVector.push_back("Sample Length");
  headerVector.push_back("Loop Length");
  headerVector.push_back("Loop Mode");
  headerVector.push_back("Root Key");
  headerVector.push_back("Initial Pitch");
  headerVector.push_back("Sustained Pitch");
  samplesListVector.push_back(headerVector);
  
  for (struct Sample sample: _samples) {
    std::vector<std::string> sampleVector;
    sampleVector.push_back(std::to_string(sample.volume));
    sampleVector.push_back(std::to_string(sample.portaOffset));
    sampleVector.push_back(std::to_string(sample.sampleLen));
    sampleVector.push_back(std::to_string(sample.loopLen));
    sampleVector.push_back(std::to_string(sample.loopMode));
    sampleVector.push_back(std::to_string(sample.rootKey));
    sampleVector.push_back(std::to_string(sample.pitchInit));
    sampleVector.push_back(std::to_string(sample.pitchSust));

    samplesListVector.push_back(sampleVector);
  }

  return samplesListVector;
}


bool ControlRom::intro_anim_available(void)
{
  return _profile && _profile->soundCanvas &&
         _profile->soundCanvas->introAnimOffset != 0;
}


std::vector<uint8_t> ControlRom::get_intro_anim(int animIndex)
{
  int romIndex;
  int length;

  const SoundCanvasLayout *sc = _profile ? _profile->soundCanvas : nullptr;
  if (!sc || !sc->introAnimOffset || animIndex < 0 ||
      animIndex >= sc->introAnimCount)
    return std::vector<uint8_t> {};

  length   = sc->introAnimLength;
  romIndex = sc->introAnimOffset + animIndex * length;

  std::ifstream romFile(_romPath, std::ios::binary | std::ios::in);
  if (!romFile.is_open()) {
    std::cerr << "Unable to open control ROM: " << _romPath << std::endl;
  }

  std::vector<uint8_t> romData(length);
  romFile.seekg(romIndex);
  romFile.read((char*) &romData[0], length);
  romFile.close();

  return romData;
}


// Both JV machines were mapped by measuring their own ROMs (P-0356, P-0361).
// They share every structure with each other and differ only in these numbers;
// the JV-880's dumps read straight where the JV-1080's address bus is permuted,
// which WaveRom handles.
// The only place in the engine that names a device. Everything else asks the
// profile.
const DeviceProfile *ControlRom::_profile_for(enum SynthModel model)
{
  switch (model) {
  case sm_SC55:
  case sm_SCC1:      return &SC55_PROFILE;
  case sm_SC55mkII:  return &SC55MKII_PROFILE;
  case sm_JV880:     return &JV880_PROFILE;
  case sm_JV1080:    return &JV1080_PROFILE;
  default:           return nullptr;
  }
}


const ControlRom::DeviceEntry ControlRom::DEVICES[] = {
  { sm_JV1080, SynthGen::JV1080, &JV1080_PROFILE },
  { sm_JV880,  SynthGen::JV880,  &JV880_PROFILE  },
};
const int ControlRom::DEVICE_COUNT =
  (int) (sizeof(DEVICES) / sizeof(DEVICES[0]));


// The JV control ROMs carry no GS banner, so the machine is identified by its
// tables: a run of 60-byte records whose first field is a printable name. Size
// narrows the candidates; the table must then actually parse.
bool ControlRom::_identify_device(std::ifstream &romFile)
{
  romFile.seekg(0, std::ios::end);
  size_t size = (size_t) romFile.tellg();
  romFile.seekg(0);
  _deviceRom.resize(size);
  romFile.read((char *) &_deviceRom[0], size);

  auto namelike = [this](uint32_t o) -> bool {
    if ((size_t) o + 12 > _deviceRom.size()) return false;
    int alnum = 0;
    for (int i = 0; i < 12; i++) {
      uint8_t ch = _deviceRom[o + i];
      if (ch < 0x20 || ch > 0x7e) return false;
      if (isalnum(ch)) alnum++;
    }
    return alnum >= 3;
  };

  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < DEVICE_COUNT; i++) {
      const DeviceEntry &L = DEVICES[i];
      if ((size == L.profile->romSize) != (pass == 0))
        continue;
      // require a run, not a single record: isolated printable triples occur
      int run = 0;
      for (int k = 0; k < 8; k++)
        run += namelike(L.profile->records->waveform.offset +
                        k * L.profile->records->waveform.stride) ? 1 : 0;
      if (run < 8)
        continue;
      _profile         = L.profile;
      _synthModel      = L.model;
      _synthGeneration = L.generation;
      _model.assign(L.profile->name);
      _version.assign("?");
      _date.assign("?");
      return true;
    }
  }

  _deviceRom.clear();
  return false;
}


// A 60-byte waveform record is a Partial: a name, note breakpoints (0x7f
// padded, all values 20..120, i.e. MIDI keys) and indices into the sample
// table, 0xFFFF marking an unused zone.
int ControlRom::_read_device_waveforms(void)
{
  const WaveformTableLayout &W = _profile->records->waveform;
  const int STRIDE = W.stride, NAME = W.nameLength, ZONES = W.zones;
  auto namelike = [this, NAME](uint32_t o) -> bool {
    if ((size_t) o + NAME > _deviceRom.size()) return false;
    int alnum = 0;
    for (int i = 0; i < NAME; i++) {
      uint8_t ch = _deviceRom[o + i];
      if (ch < 0x20 || ch > 0x7e) return false;
      if (isalnum(ch)) alnum++;
    }
    return alnum >= 2;
  };

  uint32_t base = W.offset;
  while (base >= (uint32_t) STRIDE && namelike(base - STRIDE))
    base -= STRIDE;

  for (uint32_t off = base; namelike(off); off += STRIDE) {
    struct Partial p;
    p.name.assign((const char *) &_deviceRom[off], NAME);
    p.name.erase(p.name.find_last_not_of(' ') + 1);

      // Only ZONES entries are real breakpoints; the SC-55 pads the rest with
      // 127 and the voice code relies on that, so pad the same way rather than
      // letting a trailing non-breakpoint byte read as a key range.
      for (int i = 0; i < 16; i++)
        p.breaks[i] = (i < ZONES) ? _deviceRom[off + NAME + i] : 127;

    for (int i = 0; i < 16; i++) {
      if (i >= ZONES) { p.samples[i] = 0xFFFF; continue; }
      uint32_t q = off + NAME + 16 + i * 2;
      p.samples[i] = (uint16_t) ((_deviceRom[q] << 8) | _deviceRom[q + 1]);
    }

    _partials.push_back(p);
  }

  return 0;
}


// An 18-byte sample record is a Sample. The ROM stores start, loop and end as
// absolute 24-bit addresses; the header - which holds the root key - sits in
// the NEXT slot (P-0362). Every slot is kept, valid or not, because the
// partials index this table positionally.
int ControlRom::_read_device_samples(void)
{
  const SampleTableLayout &S = _profile->records->sample;
  // The sample table sits immediately after the waveform records. Each entry is
  // 18 bytes (P-0371):
  //
  //   +0      unknown
  //   +1..3   start, 24-bit big-endian
  //   +4..6   loop
  //   +7..9   end
  //   +10..11 zero
  //   +12     flag, 0/1/2 - most likely loop mode
  //   +13     root key
  //   +14..17 unidentified
  //
  // The entry count is not stored: it is what the waveform records reference,
  // so it is derived from them. An earlier reading had the base 149.56 records
  // out - a FRACTIONAL offset, so it read across record boundaries and produced
  // plausible addresses that were wrong. Deriving the count is what catches
  // that: a correct table yields exactly as many entries as are referenced.
  int waveRoms = 2;
  for (int i = 0; i < DEVICE_COUNT; i++)
    if (DEVICES[i].model == _synthModel)
      waveRoms = DEVICES[i].profile->waveRomBanks;
  const uint32_t WAVE_SPACE = (uint32_t) waveRoms * 2 * 1024 * 1024;

  size_t needed = 0;
  for (const struct Partial &p : _partials)
    for (int i = 0; i < 16; i++)
      if (p.samples[i] != 0xFFFF && p.samples[i] + 1u > needed)
        needed = p.samples[i] + 1u;
  if (!needed)
    return -1;

  auto u24 = [this](uint32_t o) -> uint32_t {
    if ((size_t) o + 3 > _deviceRom.size()) return 0;
    return ((uint32_t) _deviceRom[o] << 16) | ((uint32_t) _deviceRom[o+1] << 8) | _deviceRom[o+2];
  };

  for (size_t i = 0; i < needed; i++) {
    uint32_t o = S.offset + (uint32_t) i * S.stride;
    if ((size_t) o + 18 > _deviceRom.size())
      break;

    struct Sample s = {};
    uint32_t s0 = u24(o + 1), l0 = u24(o + 4), e0 = u24(o + 7);
    if (s0 > 0 && s0 <= l0 && l0 <= e0 && e0 < WAVE_SPACE) {
      s.address   = s0;
      s.sampleLen = (uint16_t) (e0 - s0);
      s.loopLen   = (uint16_t) (e0 - l0);
    // Bit 2 of the flag byte plays the sample backwards: every forward /
    // "REV ..." pair in the waveform list shares one address and length and
    // differs only in this bit (P-0372). The low two bits carry the SC-55's
    // own loop semantics, so they are passed through unchanged.
    s.loopMode  = _deviceRom[o + 12] & 0x03;
    s.reverse   = _deviceRom[o + 12] & 0x04;
    }
    s.rootKey = _deviceRom[o + 13];
      s.volume    = _deviceRom[o + S.volume] & 0x7f;
                             // under the TVA level law, not "neutral"
      // +14 and +16 ARE the SC-55's pitch_init / pitch_sust after all: a signed
      // offset biased at 1024, 0.1 cent per unit, added to the pitch. Across 366
      // of the 420 forward-looped records, +14 cancels the pitch of an
      // (end - loop) + 1 sample loop against the root key to within one cent,
      // which is what identifies the field (scdb
      // devices/jv880/notes/track_pitch_bench_wip_2026-09-03.md R1).
      //
      // Pinned at 1024 - on the belief that "the JV table has no such field" -
      // they left every zone of every multisample detuned by its own word, up
      // to 75 cents and 8.7 cents sharp on average, and made the boot
      // performance's three channel-1 octave layers beat against each other
      // instead of coinciding. D-34.
      s.pitchInit = ((uint16_t) _deviceRom[o + 14] << 8) | _deviceRom[o + 15];
      s.pitchSust = ((uint16_t) _deviceRom[o + 16] << 8) | _deviceRom[o + 17];
    _samples.push_back(s);
  }

  return _samples.empty() ? -1 : 0;
}


// Neutral values for the modulation the device path does not read yet. Zero is
// not neutral for any of these: the key-follow and sensitivity fields are read
// as (value - 0x40), so 0x40 is what "no effect" looks like, and a TVABiasLevel
// of 0x40 or more would select the attenuating branch.
// The table a device lists for one engine curve, or null if it lists none.
const RomLookupTable *ControlRom::_find_lookup(RomLookup id)
{
  if (!_profile)
    return nullptr;
  for (int i = 0; i < _profile->lookupTableCount; i++)
    if (_profile->lookupTables[i].id == id)
      return &_profile->lookupTables[i];
  return nullptr;
}


void ControlRom::_init_neutral_partial(struct InstPartial &ip)
{
  ip.panpot          = 0x40;
  ip.coarsePitch     = 0x40;
  ip.finePitch       = 0x40;
  ip.volume          = 0x7f;
  ip.dryLevel        = 0x7f;
  ip.velRangeLow     = 0;
  ip.velRangeHigh    = 127;
  ip.TVALvlVSens     = 0;
  ip.TVFType         = 2;                  // 2 = disabled
  ip.pitchKeyFlw     = 0x4a;
  ip.rootKeyOffset   = 64;
  ip.TVAETKeyF14     = ip.TVAETKeyF5    = 0x40;
  ip.TVAETVSens12    = ip.TVAETVSens35  = 0x40;
  ip.TVAETKeyFP14    = ip.TVAETKeyFP5   = 0;
  ip.TVFETKeyF14     = ip.TVFETKeyF5    = 0x40;
  ip.TVFETVSens12    = ip.TVFETVSens35  = 0x40;
  ip.TVFETKeyFP14    = ip.TVFETKeyFP5   = 0;
  ip.pitchETKeyF14   = ip.pitchETKeyF5  = 0x40;
  ip.pitchETKeyFP14  = ip.pitchETKeyFP5 = 0;
  ip.pitchEnvTVSens  = 0x40;
  ip.pitchEnvL0 = ip.pitchEnvL1 = ip.pitchEnvL2 = 0x40;
  ip.pitchEnvL3 = ip.pitchEnvL5 = 0x40;
  ip.pitchEnvVSens   = 0x40;
  ip.TVFCFKeyFlw     = 0x40;
  ip.TVFCOFVSens     = 0x40;
  ip.TVABiasLevel    = 0;

  // The JV's envelope time-sense nibbles: 7 is the neutral index (both of its
  // tables hold 0 there), and no tone is a KEY-OFF delay tone.
  ip.TVAJVVelT1 = ip.TVAJVVelT4 = ip.TVAJVTimeKF = 7;
  ip.TVFJVVelT1 = ip.TVFJVVelT4 = ip.TVFJVTimeKF = 7;
  ip.PitchJVVelT1 = ip.PitchJVVelT4 = ip.PitchJVTimeKF = 7;
  ip.JVDelayKeyOff   = 0;
}


int ControlRom::_read_device_patches(void)
{
  const PatchLayout &P = _profile->records->patch;
  const LevelLaw    &P_LAW = _profile->level;
  const ToneFieldMap &F = P.tone;

  for (int bank = 0; bank < P.banks; bank++) {
    for (int n = 0; n < P.perBank; n++) {
      uint32_t off = P.bankOffset + bank * P.bankStride + n * P.stride;
      if ((size_t) off + P.stride > _deviceRom.size())
        return _instruments.size();

      struct Instrument in = {};
      in.name.assign((const char *) &_deviceRom[off], P.nameLength);
      in.name.erase(in.name.find_last_not_of(' ') + 1);
      in.volume = _deviceRom[off + P.level] & 0x7f;
      in.analogFeel = P.analogFeel
        ? (_deviceRom[off + P.analogFeel] & 0x7f) : 0;
      in.partialsUsed = 0;

      for (int t = 0; t < P.tones; t++) {
        const uint8_t *tb = &_deviceRom[off + P.firstTone + t * P.toneStride];
        struct InstPartial &ip = in.partials[t];

        ip.partialIndex = 0xFFFF;
        // The Tone Switch is one BIT of that byte - the rest of it is the wave
        // group and SysEx 0x73 - so a truthiness test reports a switched-off
        // tone as on as soon as a card or user patch sets either (D-08). On the
        // factory data the byte is only ever 0x00 or 0x80 and the two tests
        // agree exactly, 539 enabled either way, so this changes no output.
        if (!(tb[F.enabled] & (1 << F.enabledBit)))   // tone switched off
          continue;
        if (tb[F.waveform] >= _partials.size())  // an expansion wave we cannot play
          continue;

        in.partialsUsed |= 1 << t;
        _init_neutral_partial(ip);
        ip.partialIndex = tb[F.waveform];
        ip.coarsePitch  =
          (int8_t) std::clamp(0x40 + (int) (int8_t) tb[F.coarseTune], 0, 127);
        ip.finePitch    =
          (uint8_t) std::clamp(0x40 + (int) (int8_t) tb[F.fineTune], 0, 127);

        // The two machines agree on what random pan is and spell it differently:
        // the JV writes 128, the Sound Canvas writes 0 and randomises in tva.cc.
        // A JV 0 is hard LEFT, so it must not become random - but it must not be
        // nudged to 1 either, which is what this did until 2026-09-03. Pan table
        // entry 1 is 0x7f01, so a nudged hard-left note leaked its right channel
        // at 1/127 = -42.07 dB, and the reference puts EXACT digital zero there
        // (measured: Closed HAT 1 with the effects closed, reference R-L
        // -237.4 dB against our -42.07). Random now has its own sentinel so that
        // 0 can mean what the device means by it.
        {
          const int pv = tb[F.pan];
          ip.panpot = (uint8_t) (pv >= 128 ? PANPOT_RANDOM : pv);
        }

        // Key follow: the engine's units put 100% at 0x4a and neutral at 0x40, so
        // one unit is ten percent. LEAD, not a finding - the mapping is inferred
        // from that single anchor, and only 32 of 539 tones differ from +100%, so
        // it is weakly exercised by the factory data.
        {
          static const int KF_PCT[16] =
            { -100, -70, -50, -30, -10, 0, 10, 20, 30, 40, 50, 70, 100, 120, 150, 200 };
          const int pct = KF_PCT[tb[F.pitchKeyFollow] & 0x0f];
          ip.pitchKeyFlw = (uint8_t)
            std::clamp(0x40 + pct / P_LAW.keyFollowUnitsPerPct, 0, 127);
        }

        ip.velRangeLow  = tb[F.velocityLow]  & 0x7f;
        ip.velRangeHigh = tb[F.velocityHigh] & 0x7f;
        if (ip.velRangeHigh < ip.velRangeLow) {
          ip.velRangeLow = 0; ip.velRangeHigh = 127;
        }

        // The tone level, and the TVA envelope: three time/level pairs and a
        // release. L4 holds L3 because the JV envelope has one stage fewer than
        // the Sound Canvas one.
        // The tone level alone. ROM +81 is the Dry Level (P-0378), and the
        // firmware applies it at the chip's dry output register 0xf012, in
        // parallel with the send register 0xf014 -- so folding it into the
        // tone level here attenuated the sends with it and silenced the five
        // tones that are send-only at dry level 0 (P-0382 findings 2 and 8).
        ip.volume   = (uint8_t) (tb[F.level] & 0x7f);
        ip.dryLevel = (uint8_t) (tb[F.dryLevel] & 0x7f);
        ip.TVAEnvT1 = tb[F.envTime1]  & 0x7f;
        ip.TVAEnvL1 = tb[F.envLevel1] & 0x7f;
        ip.TVAEnvT2 = tb[F.envTime2]  & 0x7f;
        ip.TVAEnvL2 = tb[F.envLevel2] & 0x7f;
        ip.TVAEnvT3 = tb[F.envTime3]  & 0x7f;
        ip.TVAEnvL3 = tb[F.envLevel3] & 0x7f;
        ip.TVAEnvL4 = ip.TVAEnvL3;
        ip.TVAEnvT4 = 0x7f;
        ip.TVAEnvT5 = tb[F.envRelease] & 0x7f;

        // NOT 127. tva.cc computes the velocity-driven level as
        //   127 - ((127 - velocity) * (127 - TVALvlVSens)) / 127
        // so 127 makes the second term zero and every note plays at full level
        // whatever the velocity - no dynamics at all, which the owner heard as
        // everything sounding "flat". 0 passes velocity straight through. The
        // tone's own sensitivity byte is not identified yet, so full response is
        // Velocity, now read rather than assumed: the tone's own sensitivity at
        // +72 (signed) and its curve selector at +71. The firmware applies the
        // curve multiplicatively and skips it entirely at sensitivity 0.
        //
        // The sensitivity byte is SIGNED (descriptor param 0x65, bias 192), so
        // 0xff is -1 and not 255. It is stored as a two's-complement byte and
        // every JV consumer recovers it with an (int8_t) cast; the cast here
        // says so at the point of reading rather than leaving it to be noticed
        // downstream. Exactly 1 of the 539 enabled factory tones is negative,
        // so no audible claim is made for it (D-09).
        ip.TVALvlVSens  = (uint8_t) (int8_t) tb[F.tvaVelLevelSens];
        ip.TVALvlVelCur = tb[F.tvaVelCurve] & 0x07;

        // The envelope TIME-sense triple (scdb D-27, FW-EXACT): "T1 velocity"
        // in bits 0-3 and "T4 velocity" in bits 4-7 of one byte, "time KF" in
        // the high nibble of another - +73/+70 for the TVA, +57/+54 for the
        // TVF - each 0-14 with 7 neutral. The firmware copies these bytes
        // verbatim into the voice descriptor (ROM1 0x4B19-0x4B3A) and its rate
        // routines read the nibbles from there. Off-neutral on 64/48/103 of
        // the 539 enabled factory tones for the TVA and 79/14/148 for the TVF;
        // the two demo patches are neutral throughout, so this cannot move the
        // demo. Delay Time (+69) bit 7 is KEY-OFF, six factory tones, whose
        // release reads the note-on velocity instead of the note-off's.
        if (F.tvaTimeVelocity) {
          ip.TVAJVVelT1  = tb[F.tvaTimeVelocity] & 0x0f;
          ip.TVAJVVelT4  = (tb[F.tvaTimeVelocity] >> 4) & 0x0f;
          ip.TVAJVTimeKF = (tb[F.tvaTimeKeyFollow] >> 4) & 0x0f;
        }
        if (F.tvfTimeVelocity) {
          ip.TVFJVVelT1  = tb[F.tvfTimeVelocity] & 0x0f;
          ip.TVFJVVelT4  = (tb[F.tvfTimeVelocity] >> 4) & 0x0f;
          ip.TVFJVTimeKF = (tb[F.tvfTimeKeyFollow] >> 4) & 0x0f;
        }
        if (F.tvaDelayTime)
          ip.JVDelayKeyOff = (tb[F.tvaDelayTime] >> 7) & 0x01;

        // The filter. It was read but left DISABLED until now, because the
        // cutoff is assembled from fields this path did not fill and a filter
        // right in colour and 35 dB wrong in level is worse than none. All of
        // them are now read (P-0390).
        //
        // Filter Mode is two bits of +55. The engine's own spelling puts
        // low-pass first, so the device's 0/1/2 = OFF/LPF/HPF is translated
        // here rather than in the engine. Mode OFF must reach the engine as
        // "disabled": on the hardware nothing is written to the filter at all
        // for such a tone, and 14.3 % of the factory tones depend on it.
        {
          const int mode = (tb[F.filterMode] >> 3) & 0x03;
          ip.TVFType = (mode == 1) ? 0 : (mode == 2) ? 1 : 2;
        }
        ip.TVFBaseFlt   = (int8_t) (tb[F.filterCutoff]    & 0x7f);
        ip.TVFResonance = (int8_t) (tb[F.filterResonance] & 0x7f);
        ip.TVFResoMode  = (uint8_t) (tb[F.resoMode] >> 7);
        ip.TVFCOFKeyFlwIdx = (uint8_t) (tb[F.cutoffKeyFollow] & 0x0f);
        ip.TVFCOFVelCur = (uint8_t) (tb[F.tvfVelCurve] & 0x07);
        ip.TVFEnvVelSens = (int8_t) tb[F.tvfVelLevelSens];

        // Signed, and stored raw: the engine's JV chain reads it back as an
        // int8_t. The Sound Canvas chain uses this field as an index into a
        // depth table instead, which is why the two must never share a path.
        ip.TVFEnvDepth  = tb[F.tvfEnvDepth];

        // Four INTERLEAVED time/level pairs from +59: T1 L1 T2 L2 T3 L3 T4 L4.
        // The engine's envelope has one segment more than the JV's filter
        // envelope, so the JV's three attack/decay segments map onto the first
        // three and its release onto the engine's release. L4 carries the
        // release LEVEL, which for the filter is a real target and not zero -
        // unlike the TVA's, which always releases to silence.
        ip.TVFEnvT1 = tb[F.tvfEnv + 0] & 0x7f;
        ip.TVFEnvL1 = tb[F.tvfEnv + 1] & 0x7f;
        ip.TVFEnvT2 = tb[F.tvfEnv + 2] & 0x7f;
        ip.TVFEnvL2 = tb[F.tvfEnv + 3] & 0x7f;
        ip.TVFEnvT3 = tb[F.tvfEnv + 4] & 0x7f;
        ip.TVFEnvL3 = tb[F.tvfEnv + 5] & 0x7f;
        ip.TVFEnvT5 = tb[F.tvfEnv + 6] & 0x7f;
        ip.TVFEnvL5 = tb[F.tvfEnv + 7] & 0x7f;

        // The engine's fourth segment is not the JV's, so it is made a no-op:
        // same level as the third, and a duration the sustain never reaches.
        ip.TVFEnvL4 = ip.TVFEnvL3;
        ip.TVFEnvT4 = 0x7f;

        ip.TVFLFO1Depth = tb[F.lfo1TvfDepth];
        ip.TVFLFO2Depth = tb[F.lfo2TvfDepth];

        // The pitch block (scdb D-37, FW-EXACT). Depth is signed -12..+12 and
        // each unit is 100 cents at level 63; the four levels are signed; the
        // velocity sense always takes curve 0 (ROM1 0x487B clears the curve
        // index). Non-zero depth on 68 of the 539 factory tones.
        if (F.pitchEnv) {
          ip.PitchJVDepth   = (int8_t) tb[F.pitchEnvDepth];
          ip.PitchJVVelSens = (int8_t) tb[F.pitchEnvVelSens];
          ip.PitchJVVelT1   = tb[F.pitchTimeVelocity] & 0x0f;
          ip.PitchJVVelT4   = (tb[F.pitchTimeVelocity] >> 4) & 0x0f;
          ip.PitchJVTimeKF  = (tb[F.pitchTimeKeyFollow] >> 4) & 0x0f;
          for (int i = 0; i < 4; i++) {
            ip.PitchJVT[i] = tb[F.pitchEnv + 2 * i] & 0x7f;
            ip.PitchJVL[i] = (int8_t) tb[F.pitchEnv + 2 * i + 1];
          }
        }

        // The two LFOs, per tone. One byte carries form (bits 0-2), offset
        // (3-5), synchro (6) and fade polarity (7); the delay byte's bit 7 is
        // KEY-OFF. The pitch depths are signed (D-37); the TVF depths were read
        // above; the TVA depths have no decoded law yet and are not read.
        if (F.lfo1Rate) {
          const int prm[2] = { F.lfo1Params, F.lfo2Params };
          const int rat[2] = { F.lfo1Rate,   F.lfo2Rate };
          const int dly[2] = { F.lfo1Delay,  F.lfo2Delay };
          const int fad[2] = { F.lfo1Fade,   F.lfo2Fade };
          const int pdp[2] = { F.lfo1PitchDepth, F.lfo2PitchDepth };
          for (int l = 0; l < 2; l++) {
            ip.JVLfoForm[l]        = tb[prm[l]] & 0x07;
            ip.JVLfoOffset[l]      = (tb[prm[l]] >> 3) & 0x07;
            ip.JVLfoSync[l]        = (tb[prm[l]] >> 6) & 0x01;
            ip.JVLfoFadeOut[l]     = (tb[prm[l]] >> 7) & 0x01;
            ip.JVLfoRate[l]        = tb[rat[l]] & 0x7f;
            ip.JVLfoDelay[l]       = tb[dly[l]] & 0x7f;
            ip.JVLfoDelayKeyOff[l] = (tb[dly[l]] >> 7) & 0x01;
            ip.JVLfoFade[l]        = tb[fad[l]] & 0x7f;
            ip.JVLfoPitchDepth[l]  = (int8_t) tb[pdp[l]];
          }
          ip.hasJVLfo = 1;
        }

      }

      // libEmuSC sends per part, the JV per tone. The loudest enabled tone
      // decides, so a patch with one wet tone is not made dry by its dry ones.
      uint8_t reverb = 0, chorus = 0;
      for (int t = 0; t < P.tones; t++) {
        const uint8_t *tb = &_deviceRom[off + P.firstTone + t * P.toneStride];
        if (!tb[F.enabled])
          continue;
        reverb = std::max(reverb, (uint8_t) (tb[F.reverbSend] & 0x7f));
        chorus = std::max(chorus, (uint8_t) (tb[F.chorusSend] & 0x7f));
      }
      _instrumentSend.push_back({reverb, chorus});

      _instruments.push_back(in);
    }
  }

  // The engine resolves a program change as _variations[bank][program], so this
  // table IS the device's MIDI patch map and it has to answer the same program
  // change the machine answers. It had been filled as a flat identity over
  // 0..127, which put Preset A where the machine has the Card bank, left Preset B
  // unreachable altogether, and made bank select 81 - which the machine honours -
  // render silence.
  for (int v = 0; v < 128; v++)
    for (int i = 0; i < 128; i++)
      _variations[v][i] = 0xffff;

  const int per = P.perBank;
  for (int b = 0; b < P.banks; b++) {
    // Bank 0 of the ROM answers a bare program change; the rest sit behind the
    // preset bank select, two ROM banks to its 128 programs.
    const int cc0  = (b == 0) ? P.midiBankFirst : P.midiBankPresets;
    const int base = (b == 0) ? 0 : (b - 1) * per;
    for (int i = 0; i < per; i++) {
      const int inst = b * per + i;
      if (inst < (int) _instruments.size() && base + i < 128)
        _variations[cc0][base + i] = (uint16_t) inst;
    }
  }

  return _instruments.size();
}




// The JV family has no CPU EPROM holding the SC-55's ~34 lookup tables, and the
// voice path reads them unconditionally: two of them are std::vector and are
// empty here, which is a segfault rather than a wrong note. Give them defined
// neutral values so the engine runs.
//
// NEUTRAL, NOT CORRECT. Every curve below is a placeholder chosen to be
// transparent - unity gain, no key follow, no bias - so that what comes out is
// the JV's samples at the JV's pitch with no shaping, which is exactly what the
// owner heard and described as "no envelope or effects". The real curves are
// TASK-141's subject. Nothing here should be mistaken for a measurement.
void ControlRom::_init_device_lookup_tables(void)
{
  struct LookupTables &t = lookupTables;

  // Tables read from the JV's OWN ROM rather than fitted from the SC-55's.
  //
  // Found by matching numerically instead of by shape: for each SC-55 table,
  // scan the JV ROM for a run of the same length whose values agree entry by
  // entry. The owner's insight is what made this work - a filter or pan curve
  // has no reason to differ much between two Roland machines of the same era,
  // so the SC-55's tables are usable as templates to search WITH, not just as
  // stand-ins to substitute. EnvSegmentCurve matches exactly, PitchCoarseExp to
  // 1.4%, and the rest to between 8% and 15%.
  //
  // Each is still a LEAD until the code that reads it is found: a numerical
  // match is strong evidence and not proof. tools/romdis/catalog.py lists what
  // else in the ROM has the same shape.
  auto rom8 = [this](uint32_t base, int n, auto &dst) {
    if ((size_t) base + n > _deviceRom.size()) return false;
    for (int i = 0; i < n; i++) dst[i] = _deviceRom[base + i];
    return true;
  };
  auto rom16 = [this](uint32_t base, int n, auto &dst) {
    if ((size_t) base + 2*n > _deviceRom.size()) return false;
    for (int i = 0; i < n; i++)
      dst[i] = (_deviceRom[base + i*2] << 8) | _deviceRom[base + i*2 + 1];
    return true;
  };
  // Two's complement, for a table that holds negative numbers: the cutoff key
  // follow list runs from -100 cents per semitone and would otherwise read as
  // 65436 (P-0390).
  auto rom16s = [this](uint32_t base, int n, auto &dst) {
    if ((size_t) base + 2*n > _deviceRom.size()) return false;
    for (int i = 0; i < n; i++)
      dst[i] = (int16_t) ((_deviceRom[base + i*2] << 8) |
                           _deviceRom[base + i*2 + 1]);
    return true;
  };
  // Signed bytes, for the same reason: the envelope time key-follow table
  // runs +21 .. -21 (scdb D-27).
  auto rom8s = [this](uint32_t base, int n, auto &dst) {
    if ((size_t) base + n > _deviceRom.size()) return false;
    n = std::min<int>(n, (int) dst.size());
    for (int i = 0; i < n; i++) dst[i] = (int8_t) _deviceRom[base + i];
    return true;
  };
  bool haveRom = _deviceRom.size() > 0x40000 - 1;
  bool jvTimeVelDepth = false, jvTimeKeyFollow = false;
  bool jvLfoRate = false, jvLfoOffset = false, jvLfoWaves = false;


  // Key follow: one flat map, so every key maps to bias 0 and no table lookup
  // can leave its bounds. 136 is the index table's size; the mapper is indexed
  // by kmIndex + key, so it must cover the whole key range.
  // KeyMapper is read two different ways and its neutral differs between them.
  // tva.cc and pitch.cc take a BYTE and want 0 (no bias, no key follow), but
  // tvf.cc takes a 16-BIT value and computes ((km - 0x4000) * cofkf) >> 8, so
  // zero there is a large NEGATIVE key follow that slams the cutoff shut - it
  // cost 35 dB and made the filter unusable. 0x4000 is its centre.
  //
  // So the table carries both: a zero region the byte readers use, and a
  // 0x4000-filled region that KeyMapperIndex points the TVF reader at.
  t.KeyMapperOffset = 0;
  t.KeyMapperIndex.fill(0);
  t.KeyMapper.assign(1024, 0);
  for (size_t i = 256; i + 1 < t.KeyMapper.size(); i += 2) {
    t.KeyMapper[i]     = 0x40;          // 0x4000 big-endian, the centre
    t.KeyMapper[i + 1] = 0x00;
  }
  // tvf.cc indexes this table as KeyMapperIndex[48 + TVFCFKeyFlwC]
  for (int i = 48; i < 64; i++) t.KeyMapperIndex[i] = 256;

  // Velocity curves: identity, so velocity passes through unshaped. tvf.cc
  // bounds-checks this one by size, so a single 128-entry curve is enough.
  t.VelocityCurves.resize(128);
  for (int i = 0; i < 128; i++) t.VelocityCurves[i] = (uint8_t) i;

  // TVA level. levelIndex starts at 0xff - TVALevelIndex[volume], so an
  // identity-complement here means "volume 127 = no attenuation", and TVALevel
  // maps an index straight back to a level.
  // Modelled on the SC-55's own tables, read out of an mk2 ROM and fitted:
  //   TVALevelIndex[v] = 121 * log10(127/v)   (255,109,73,52,36,25,15,7,0)
  //   TVALevel[i]      = 255 * 10^(-(255-i) * 0.332/20)   (~0.332 dB per step)
  // These are the SC-55's curves, not the JV's, and are a placeholder until the
  // JV's own are found. They are used because the shape matters: an identity
  // curve renders ~63 dB below where the machine plays.
  t.TVALevelIndex[0] = 255;
  for (int i = 1; i < 128; i++) {
    double a = 121.0 * std::log10(127.0 / (double) i);
    t.TVALevelIndex[i] = (uint8_t) std::min(255.0, std::max(0.0, std::round(a)));
  }
  for (int i = 0; i < 256; i++) {
    double v = 255.0 * std::pow(10.0, -((255 - i) * 0.332) / 20.0);
    t.TVALevel[i] = (uint8_t) std::min(255.0, std::max(0.0, std::round(v)));
  }

  // Envelope phase times, same treatment: roughly a doubling every 16 steps,
  // fitted to the SC-55's (0, 159, 453, 994, 1990, 3827, 7211, 13448).
  // Envelope times: the JV has its own table, and it is in the control ROM at
  // 0x04c58 - 128 big-endian 16-bit entries, 128 rising to 16127 on a constant
  // ratio of 1.0384 (a doubling every ~18 steps). It is the same shape and
  // magnitude as the SC-55's envelopeTime, which is what makes it recognisable:
  // sampled every 16 it reads 128, 235, 433, 796, 1464, 2693, 4953, 9109
  // against the SC-55's 0, 159, 453, 994, 1990, 3827, 7211, 13448.
  // Milliseconds per engine control tick: 256 samples of the internal 32 kHz
  // clock. The JV's firmware services its envelopes on the same 8 ms period.
  const int TICK_US = 256 * 1000000 / 32000;

  const RomLookupTable *envTime = _find_lookup(RomLookup::EnvelopeTime);
  if (envTime && _deviceRom.size() >= envTime->offset + envTime->entries * 2) {
    const int usPer = _profile->level.envelopeTimeUsPerUnit;
    for (int i = 0; i < envTime->entries && i < 128; i++) {
      const int raw = (int) ((_deviceRom[envTime->offset + i * 2] << 8) |
                              _deviceRom[envTime->offset + i * 2 + 1]);
      t.envelopeTime[i] = usPer ? (raw * usPer) / TICK_US : raw;
    }
  } else {
    t.envelopeTime[0] = 0;
    for (int i = 1; i < 128; i++)
      t.envelopeTime[i] = (int) std::round(13448.0 * std::pow(2.0, (i - 112) / 16.6));
  }

  t.TVAPanKeyFollow.fill(0);
  t.TVABiasLevel.fill(0);      // the bias CURVE, not a centred value
  // A pan LAW, not a constant. Filled with 0x40 it centred every voice no
  // matter what pan was asked for, which is why the drums played in mono while
  // the reference puts Closed HAT 1 37 dB to the left. tva.cc reads it as
  // TVAPanpot[pan] against TVAPanpot[0x80 - pan], so it wants a curve rising
  // from 0 to 127. Constant power: 127 * sin(pan/128 * pi/2). The SC-55's own
  // reads 0, 16, 35, 56, 75, 94, 109, 120, 127 at every sixteenth entry, so this
  // sits about 1.6 dB hot at centre and is otherwise the same shape.
  for (int i = 0; i < 129; i++)
    t.TVAPanpot[i] = (uint8_t) std::lround(127.0 * std::sin((i / 128.0) * M_PI / 2.0));

  // Off unless a device profile names a packed pan table below. The Sound Canvas
  // reads its own 129-byte ramp through the CPU map, so this fallback is only
  // ever the law on a device that has neither.
  t.hasJVPanLaw = false;
  t.JVPanLawL.fill(0);
  t.JVPanLawR.fill(0);

  // Key follow. PitchParamScale scales |key - 60| into the base pitch, so zero
  // here means a note sounds at the same pitch whatever key is played - which is
  // exactly what it did. The SC-55's is a straight ramp, 0..65535 over 21 steps.
  for (int i = 0; i < 21; i++)
    t.PitchParamScale[i] = (int) std::round(i * (65535.0 / 20.0));
  for (int i = 0; i < 21; i++)
    t.EnvTimeKeyFollowSens[i] = (uint8_t) std::round(i * (128.0 / 20.0));
  t.EnvTimeScale.fill(0);
  t.PitchEnvVelSens1.fill(0);
  t.PitchEnvVelSens2.fill(0);
  t.PitchEnvDepth.fill(0);
  // Pitch. These two decide the playback rate, so zero here is not "neutral" -
  // it means the sample never advances and no note is ever heard. Fitted to the
  // SC-55's own tables: PitchCoarseExp spans an octave from 32768 (its measured
  // 32768..64694), PitchFineExp is near-linear to 62237.
  for (int i = 0; i < 47; i++)
    t.PitchCoarseExp[i] = (int) std::round(32768.0 * std::pow(2.0, i / 46.87));
  for (int i = 0; i < 256; i++)
    t.PitchFineExp[i] = (int) std::round(i * (62237.0 / 255.0));
  t.PortamentoRate.fill(0);

  // The chorus type records, if this device has them: filled from the ROM in the
  // lookup-table pass below, and left zero otherwise so the chorus can tell that
  // it has no records to sweep between.
  t.JVChorusRecords.fill(0);

  // The reverb type records' return coefficients, likewise: filled just after
  // the lookup-table pass below and left zero otherwise, which is what tells
  // reverb.cc that this device has no per-type return coefficient to apply.
  t.JVReverbReturnCoeff.fill(0);
  t.JVReverbTapScale.fill(0);
  t.JVReverbRecord.fill(0);

  t.LFORate.fill(0);
  t.LFODelayTime.fill(0);
  t.LFOTVFDepth.fill(0);
  t.LFOTVPDepth.fill(0);
  t.LFOSine.fill(0);

  // Filter curves, fitted to the SC-55's own as the level curves were:
  //   TVFCutoffFreq[i]  = 35 * 1.0592^i, saturating at 32767
  //                       (its 35, 88, 223, 562, 1415, 3560, 8875, 20887)
  //   TVFResonance[i]   = 255 * 0.99226^i   (its 255, 225, 199, ... 116)
  //   TVFCutoffFreqKF[i]= i * 512/20        (its 0, 26, 51, 77, ... 512)
  // The JV's own cutoff table, if EMUSC_JV_COF_TABLE names its offset; the
  // catalogue in tools/romdis/catalog.py lists the candidates. Otherwise the
  // SC-55's shape, fitted.
  {
    const char *ct = getenv("EMUSC_JV_COF_TABLE");
    uint32_t base = ct ? (uint32_t) strtoul(ct, nullptr, 0) : 0;
    if (base && (size_t) base + 258 <= _deviceRom.size()) {
      for (int i = 0; i < 129; i++)
        t.TVFCutoffFreq[i] = (_deviceRom[base + i*2] << 8) | _deviceRom[base + i*2 + 1];
    } else {
      for (int i = 0; i < 129; i++)
        t.TVFCutoffFreq[i] = (int) std::min(32767.0, std::round(35.0 * std::pow(1.0592, i)));
    }
  }
  for (int i = 0; i < 128; i++)
    t.TVFResonance[i] = (uint8_t) std::round(255.0 * std::pow(0.99226, i));
  for (int i = 0; i < 21; i++)
    t.TVFCutoffFreqKF[i] = (int) std::round(i * (512.0 / 20.0));
  // The filter's coefficient and depth curves, fitted to the SC-55's the same
  // way. TVFResonanceFreq is what shapes the filter itself, and leaving it zero
  // is why enabling the filter cost 25 dB.
  //   TVFResonanceFreq  flat 127 to ~110, then rolling to 0 by 256
  //                     (its 127... then 118, 96, 77, 62, 49, 37, 26, 17, 8)
  //   TVFEnvDepth[i]    = i * 193.5        (its 0, 3096, 6192, ... 21673)
  //   TVFCutoffVSens[i] = i * 25.8         (its 0, 26, 52, 77, ... 258)
  //   TVFEnvScale[i]    = i * 2            (its 0, 2, 4, ... 30)
  for (int i = 0; i < 256; i++) {
    double v = (i <= 110) ? 127.0
                          : 127.0 * std::pow(std::max(0.0, 1.0 - (i - 110) / 146.0), 1.6);
    t.TVFResonanceFreq[i] = (uint8_t) std::round(std::clamp(v, 0.0, 127.0));
  }

    // The three curves above were described in that comment and never assigned,
    // so they stayed zero. tvf.cc takes _envDepth from TVFEnvDepth and scale
    // from TVFEnvScale and multiplies them, so two zeroed tables left the whole
    // TVF envelope inert: rendering one instrument with TVFEnvDepth forced to 0
    // and then to 127 gave byte-identical audio. Every attempt at the filter was
    // therefore a static filter parked at the tone's base cutoff, which is why
    // low-cutoff patches went dark where the machine keeps them bright.
    for (size_t i = 0; i < t.TVFEnvDepth.size(); i++)
      t.TVFEnvDepth[i] = (int) std::lround(i * 193.5);
    for (size_t i = 0; i < t.TVFCutoffVSens.size(); i++)
      t.TVFCutoffVSens[i] = (int) std::lround(i * 25.8);
    for (size_t i = 0; i < t.TVFEnvScale.size(); i++)
      t.TVFEnvScale[i] = (uint8_t) std::min<int>(255, (int) (i * 2));
  // Tables read from the JV's own ROM, as a table rather than as inline calls
  // so the device's data stays data. Each was found by matching the SC-55's
  // equivalent numerically and each is CHECKED here for the shape it must have,
  // because three earlier entries were wrong and one of them broke stereo:
  // TVAPanpot came back non-monotonic (0, 107, 5, 54, ...) where a pan law has
  // to rise, and every drum played centre. A numerical match is a lead; the
  // shape check is what makes it a finding.

  auto shape_ok = [](const uint8_t *v, int n, int width, Monotonic want) -> bool {
      int inversions = 0;
      int prev = width == 2 ? ((v[0] << 8) | v[1]) : v[0];
      for (int k = 1; k < n; k++) {
        int cur = width == 2 ? ((v[k * 2] << 8) | v[k * 2 + 1]) : v[k];
        if (want == Monotonic::Rising  && cur < prev) inversions++;
        if (want == Monotonic::Falling && cur > prev) inversions++;
        prev = cur;
      }
      return inversions == 0;
    };

  if (haveRom) {
    for (int i = 0; i < _profile->lookupTableCount; i++) {
      const RomLookupTable &rt = _profile->lookupTables[i];
      if ((size_t) rt.offset + (size_t) rt.entries * rt.width > _deviceRom.size())
        continue;
      if (rt.shape != Monotonic::Unchecked &&
          !shape_ok(&_deviceRom[rt.offset], rt.entries, rt.width, rt.shape))
        continue;

      switch (rt.id) {
      case RomLookup::TVFResonanceFreq:
        rom8(rt.offset, rt.entries, t.TVFResonanceFreq);     break;
      case RomLookup::TVFResonance:
        rom8(rt.offset, rt.entries, t.TVFResonance);         break;
      case RomLookup::EnvSegmentCurve:
        rom8(rt.offset, rt.entries, t.EnvSegmentCurve);      break;
      case RomLookup::TVAPanKeyFollow:
        rom8(rt.offset, rt.entries, t.TVAPanKeyFollow);      break;
      case RomLookup::TVALevelIndex:
        rom8(rt.offset, rt.entries, t.TVALevelIndex);        break;
      case RomLookup::EnvTimeKeyFollowSens:
        rom8(rt.offset, rt.entries, t.EnvTimeKeyFollowSens); break;
      case RomLookup::LFOSine:
        rom8(rt.offset, rt.entries, t.LFOSine);              break;
      case RomLookup::PitchCoarseExp:
        rom16(rt.offset, rt.entries, t.PitchCoarseExp);      break;
      case RomLookup::JVLevel:
        rom16(rt.offset, rt.entries, t.JVLevel);             break;
      case RomLookup::JVLevelEnv:
        rom16(rt.offset, rt.entries, t.JVLevelEnv);          break;
      case RomLookup::JVLevelEnvSlope:
        rom16(rt.offset, rt.entries, t.JVLevelEnvSlope);     break;
      case RomLookup::JVVelCurves:
        rom8(rt.offset, rt.entries, t.JVVelCurves);          break;
      case RomLookup::JVTvfExpCoarse:
        rom16(rt.offset, rt.entries, t.JVTvfExpCoarse);      break;
      case RomLookup::JVTvfExpFine:
        rom16(rt.offset, rt.entries, t.JVTvfExpFine);        break;
      case RomLookup::JVTvfLimitSoft:
        rom16(rt.offset, rt.entries, t.JVTvfLimitSoft);      break;
      case RomLookup::JVTvfDampSoft:
        rom16(rt.offset, rt.entries, t.JVTvfDampSoft);       break;
      case RomLookup::JVTvfLimitHard:
        rom16(rt.offset, rt.entries, t.JVTvfLimitHard);      break;
      case RomLookup::JVTvfDampHard:
        rom16(rt.offset, rt.entries, t.JVTvfDampHard);       break;
      case RomLookup::JVTvfBase:
        rom16(rt.offset, rt.entries, t.JVTvfBase);           break;
      case RomLookup::LFORateTable:
        rom16(rt.offset, rt.entries, t.LFORate);             break;
      case RomLookup::JVPanLaw:
        // Packed big-endian (L << 8) | R, split into two independent channels.
        // Verified against jv880_rom2 at 0x6B8A: W[0] = 0x7f00 (hard left),
        // W[64] = 0x585a = (88, 90) - the asymmetric centre that rules out a
        // mirrored single table - and W[127] = 0x007f (hard right).
        for (int e = 0; e < rt.entries && e < 128; e++) {
          t.JVPanLawL[e] = _deviceRom[rt.offset + 2 * e];
          t.JVPanLawR[e] = _deviceRom[rt.offset + 2 * e + 1];
        }
        t.hasJVPanLaw = true;
        break;
      case RomLookup::JVTvfCutoffKF:
        rom16s(rt.offset, rt.entries, t.JVTvfCutoffKF);      break;
      case RomLookup::JVChorusRecords:
        rom16(rt.offset, rt.entries, t.JVChorusRecords);     break;
      case RomLookup::JVEnvTimeVelDepth:
        jvTimeVelDepth = rom16s(rt.offset, rt.entries, t.JVEnvTimeVelDepth);
        break;
      case RomLookup::JVEnvTimeKeyFollow:
        jvTimeKeyFollow = rom8s(rt.offset, rt.entries, t.JVEnvTimeKeyFollow);
        break;
      case RomLookup::JVTvaAttackCurve:
        rom8(rt.offset, rt.entries, t.JVTvaAttackCurve);
        t.hasJVTvaAttackCurve = true;
        break;
      case RomLookup::JVLfoRate:
        jvLfoRate = rom16(rt.offset, rt.entries, t.JVLfoRate);      break;
      case RomLookup::JVLfoOffset:
        jvLfoOffset = rom8s(rt.offset, rt.entries, t.JVLfoOffset);  break;
      case RomLookup::JVLfoWaves:
        jvLfoWaves = rom8s(rt.offset, rt.entries, t.JVLfoWaves);    break;
      case RomLookup::JVLfoPitchDepth:
        t.hasJVLfoPitchDepth = rom16(rt.offset, rt.entries, t.JVLfoPitchDepth);
        break;
      case RomLookup::EnvelopeTime:
        break;                       // read separately, into a fixed-size array
      }
    }
  }
  // The envelope time-sense law needs both of its tables; with either missing
  // the envelopes keep their table times unscaled.
  t.hasJVEnvTimeSense = jvTimeVelDepth && jvTimeKeyFollow;
  // The JV LFO needs its rate table, its offsets and its waveforms; with any
  // of them missing the engine keeps the Sound Canvas generator, which on this
  // device is inert.
  t.hasJVLfo = jvLfoRate && jvLfoOffset && jvLfoWaves;

  // The reverb RETURN coefficients, one per reverb type. They are not a table:
  // the firmware reaches them through the pointer table at ROM2 0x4800, whose
  // entries are spaced 0x3C for the six reverbs, 0x38 for the two delays and
  // 0x0A for the three chorus records that follow, so nothing with a stride can
  // read them and the pointers have to be followed one at a time (P-0395).
  //
  // ROM2 0x71EC-0x7227 reads byte +0x38 of the selected record; reverb.cc turns
  // it into the return gain. Read here rather than there so the ROM stays in
  // the ROM reader.
  if (haveRom && _profile->records) {
    const PerformanceLayout &V = _profile->records->performance;
    const int n = std::min((int) t.JVReverbReturnCoeff.size(), V.reverbTypeCount);
    if (V.reverbTypeTable) {
      for (int type = 0; type < n; type++) {
        const uint32_t po = V.reverbTypeTable + 2u * (uint32_t) type;
        if ((size_t) po + 1 >= _deviceRom.size())
          break;
        const uint32_t rec = ((uint32_t) _deviceRom[po] << 8) | _deviceRom[po + 1];
        if ((size_t) rec + V.reverbReturnCoeff >= _deviceRom.size())
          break;
        t.JVReverbReturnCoeff[type] = _deviceRom[rec + V.reverbReturnCoeff];

        // ...and the whole record, words 0..27, which is the reverb NETWORK
        // itself: the nine stereo tap pairs, their Q6 gains, the input gain,
        // the loop tap, the pre-LPF pair and the Time scale (P-0399). The
        // network in reverb.cc is driven from these words on a device whose
        // ReverbNetworkKind is JVMultiTapLine; nothing is embedded there.
        {
          const int nw = LookupTables::JVReverbRecordWords;
          if ((size_t) rec + 2 * nw <= _deviceRom.size()) {
            for (int w = 0; w < nw; w++)
              t.JVReverbRecord[nw * type + w] =
                ((int) _deviceRom[rec + 2 * w] << 8) | _deviceRom[rec + 2 * w + 1];
          }
        }

        // ...and, out of the same record, the two delay-time scale words the
        // delay arm multiplies the Time parameter by (P-0397). Read for every
        // type because they are read from every record; only 6 and 7 use them.
        if (V.reverbTapScaleB &&
            (size_t) rec + V.reverbTapScaleB + 1 < _deviceRom.size()) {
          t.JVReverbTapScale[2 * type] =
            ((int) _deviceRom[rec + V.reverbTapScaleA] << 8) |
            _deviceRom[rec + V.reverbTapScaleA + 1];
          t.JVReverbTapScale[2 * type + 1] =
            ((int) _deviceRom[rec + V.reverbTapScaleB] << 8) |
            _deviceRom[rec + V.reverbTapScaleB + 1];
        }
      }
    }
  }

  // Envelope segment shape and the TVA's exponential change table, all read off
  // the SC-55 and reproduced rather than invented.
  {
    const uint8_t step[12]  = { 0, 16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 255 };
    const uint8_t curve[9]  = { 10, 10, 9, 9, 8, 8, 8, 7, 7 };
    for (int i = 0; i < 12; i++) t.EnvSegmentStep[i]  = step[i];
    for (int i = 0; i <  9; i++) t.EnvSegmentCurve[i] = curve[i];
  }
  for (int i = 0; i < 257; i++)
    t.TVAEnvExpChange[i] = (int) std::min(65535.0, std::round(std::pow(2.0, i / 16.0)));
}



// A JV performance says which patch each of its eight parts plays and which MIDI
// channel that part answers to. A file like the machine's own demo sends no
// program change at all and relies entirely on this; without it every part falls
// back to one default instrument and the whole piece plays on the wrong sound.
//
// The record is 204 bytes: a 12-byte name, 16 bytes of common data, then eight
// parts of 22. Within a part, +16 is the patch number and +21 carries the
// receive channel in its low nibble. Established by name agreement rather than
// by probe, because performances live in battery-backed RAM and a ROM edit
// cannot reach them - performance 0 is named "Syn Lead" and its parts all carry
// patch 21, which is "SAW Lead"; performance 1, "Encounter X", carries JV
// Heaven, Analog Pad 2, Arctic Winds, WhistlinAtom, X/Y/Z, Ice Hall and
// DistanceCall. See docs/service-notes/jv880-owner.md.
int ControlRom::_read_device_performances(void)
{
  const PerformanceLayout   &V = _profile->records->performance;
  const PerformancePartMap  &M = V.part;

  _channelPatch.fill(-1);
  _channelLevel.fill(100);
  _channelReverb.fill(0);
  _channelChorus.fill(0);
  _channelPan.fill(0x40);
  _channelKeyShift.fill(0);

  if (!V.offset || (size_t) V.offset + (V.bootIndex + 1) * V.stride > _deviceRom.size())
    return -1;

  const uint8_t *p = &_deviceRom[V.offset + V.bootIndex * V.stride];

  _deviceEffects.reverbType     = p[V.reverbType] & 0x07;
  _deviceEffects.reverbLevel    = p[V.reverbLevel] & 0x7f;

  // The reverb RETURN is not the level byte, but the conversion does not belong
  // here: it depends on the reverb TYPE, and a type change through SysEx would
  // never reach a number baked into the performance's level. The Level
  // parameter is passed through as the machine stores it and reverb.cc applies
  // the firmware's law (ReverbReturnLaw::JVTypeCoefficient, P-0395).
  _deviceEffects.reverbTime     = p[V.reverbTime] & 0x7f;
  _deviceEffects.reverbFeedback = p[V.reverbFeedback] & 0x7f;
  _deviceEffects.chorusType     = (p[V.chorusType] >> 3) & 0x07;
  _deviceEffects.chorusLevel    = p[V.chorusLevel] & 0x7f;
  _deviceEffects.chorusToReverb = (p[V.chorusLevel] >> 7) & 0x01;
  _deviceEffects.chorusRate     = p[V.chorusRate] & 0x7f;
  _deviceEffects.chorusDepth    = p[V.chorusDepth] & 0x7f;
  _deviceEffects.chorusFeedback = p[V.chorusFeedback] & 0x7f;

  for (int t = 0; t < V.parts; t++) {
    const uint8_t *pt = p + V.commonSize + t * V.partStride;

    // Part 8 is the rhythm part, not a patch part: the manual's signal diagram
    // shows parts 1-7 taking a Patch and part 8 taking a Rhythm set, and its
    // patch byte is 0 in every factory performance rather than a patch number.
    const bool rhythm = (t == V.parts - 1);

    const int  chan   = pt[M.channel] & M.channelMask;
    const bool revSw  = (pt[M.channel] >> M.reverbSwitchBit) & 1;
    const bool choSw  = (pt[M.channel] >> M.chorusSwitchBit) & 1;

    // The machine numbers patches I01-I64, C01-C64, A01-A64, B01-B64 across
    // 0..255 - Card second - while our instrument list is Internal, Preset A,
    // Preset B across 0..191, because a bare JV-880 has no card. Translate,
    // rather than letting a Preset B patch index off the end and be dropped:
    // performance 4's third part asks for 204, which is B13.
    int patch = pt[M.patch];
    if (patch >= 192)      patch = 128 + (patch - 192);   // Preset B
    else if (patch >= 128) patch =  64 + (patch - 128);   // Preset A
    else if (patch >= 64)  patch = -1;                    // Card: not present

    // Keep the parts as parts. Several may share one MIDI channel - performance
    // 7 layers patches 1 and 26 on channel 1 - and a channel-keyed map silently
    // drops all but the first, which is one patch of two on the demo's melody.
    _deviceParts[t] = { patch, chan,
                    pt[M.level] & 0x7f, pt[M.pan] & 0x7f,
                    (int8_t) pt[M.coarseTune], 0, 0, rhythm };

    if (rhythm) {
      _deviceDrumChannel = chan;
      // The rhythm part has no patch to take a send from, so the switch alone
      // decides and the rhythm note's own send sets the depth per instrument.
      // Left at 0 it multiplied out every per-note depth downstream and the kit
      // rendered with no tail at all.
      _deviceParts[t].reverb = revSw ? 127 : 0;
      _deviceParts[t].chorus = choSw ? 127 : 0;
      continue;
    }

    if (patch >= 0 && patch < (int) _instrumentSend.size()) {
      _deviceParts[t].reverb = revSw ? _instrumentSend[patch].first  : 0;
      _deviceParts[t].chorus = choSw ? _instrumentSend[patch].second : 0;
    }

    if (patch >= (int) _instruments.size())
      continue;

    // Earlier parts win: several parts may share a channel to layer sounds, and
    // libEmuSC has one instrument per part where the JV has eight.
    if (_channelPatch[chan] < 0) {
      _channelPatch[chan]    = patch;
      _channelLevel[chan]    = pt[M.level] & 0x7f;
      _channelPan[chan]      = pt[M.pan] & 0x7f;
      _channelKeyShift[chan] = (int8_t) pt[M.coarseTune];
      if (patch < (int) _instrumentSend.size()) {
        _channelReverb[chan] = _instrumentSend[patch].first;
        _channelChorus[chan] = _instrumentSend[patch].second;
      }
    }
  }

  return 0;
}



// The JV's drums. Part 8 of a performance takes a Rhythm set rather than a
// patch, and the set is one 44-byte record per key from 36 (C2) upward, 61 of
// them, sitting immediately after the Internal patch bank - the two are
// contiguous, as the performance table and the patch bank are.
//
// Byte +1 is the waveform, which the names settle beyond argument: key 36 is
// "Bright Kick", 38 "90's Snare", 42 "Closed HAT 1", 46 "Open HAT 1".
//
// The record holds 52 parameters, not eight, and it is BIT-PACKED: the
// firmware unpacks it through an 8-byte descriptor per SysEx address. Eight
// were read here and the other forty-four were not, which left every drum on
// one hardcoded envelope with no filter and no velocity response at all (D-01).
// The positions are profile data (devices/jv880.cc) because they are this
// device's, and they are the firmware's own rather than the manual's address
// column.
//
// What is still NOT read, and why, so the next session inherits a decision
// rather than an omission:
//
//   pitch envelope (+0x07..+0x10)  the engine's pitch-envelope path is the
//                                  Sound Canvas's and indexes CPU-EPROM tables
//                                  the JV family does not have. 180 of 183
//                                  factory notes carry depth 0, so the gap is
//                                  three notes wide.
//   random pitch depth (+5 bits 4-7)  an index into the manual's cents list
//                                  (0,5,10,20,...,1200); the engine's randPitch
//                                  is a raw multiplier on a different scale and
//                                  the conversion is not established. 12 of 183.
//   pitch bend range (+6 bits 0-3)  per-key on the device, per-part in this
//                                  engine. 7 of 183 carry 12, the rest 0.
//   the three velocity TIME senses  the JV branch of TVA::_init_envelope
//     (+5, +0x14, +0x21 low nibbles) returns before the engine's
//                                  set_time_velocity_sensitivity, so there is
//                                  no consumer to feed.
//   envelope mode (+2 bit 7)       NO-SUSTAIN on all 183 notes, and a rhythm
//                                  key here accepts note on only, so the
//                                  envelope already plays out. Nothing to do.
//   output select (+0 bit 4)       MAIN on all 183; this engine has no second
//                                  output pair.
//   wave group (+0 bits 0-1)       INT on all 183; the other pools are card and
//                                  expansion waves we do not have.
//
// libEmuSC's drum path wants an INSTRUMENT per key, so each rhythm note becomes
// one, appended after the patches. Their names come from the waveform, which
// makes a drum map readable in the instrument list.
int ControlRom::_read_device_rhythm(void)
{
  const RhythmLayout &R = _profile->records->rhythm;

  if (!R.offset || (size_t) R.offset + R.keys * R.stride > _deviceRom.size())
    return -1;

  struct DrumSet ds = {};
  ds.name = "Rhythm";

  for (int k = 0; k < 128; k++) {
    ds.preset[k] = 0xffff;
    ds.volume[k] = 0x7f;
    ds.key[k]    = 60;
    ds.panpot[k] = 0x40;
    ds.flags[k]  = 0x10;
  }

  for (int k = 0; k < R.keys; k++) {
    const uint8_t *r = &_deviceRom[R.offset + k * R.stride];

    // The Tone Switch is one BIT of a byte that also carries the wave group and
    // the output select, so the whole byte is not the switch (D-08).
    if (!(r[R.enabled] & (1 << R.enabledBit)))
      continue;
    if (r[R.waveform] >= _partials.size())
      continue;

    struct Instrument in = {};
    in.name         = _partials[r[R.waveform]].name;
    in.volume       = 0x7f;
    in.partialsUsed = 1;
    for (int t = 0; t < 4; t++)
      in.partials[t].partialIndex = 0xFFFF;

    struct InstPartial &ip = in.partials[0];
    _init_neutral_partial(ip);
    ip.partialIndex = r[R.waveform];

    // Pitch fine tune, signed cents, into the engine's 0x40-biased field - the
    // same conversion the patch tones get.
    ip.finePitch =
      (uint8_t) std::clamp(0x40 + (int) (int8_t) r[R.fineTune], 0, 127);

    // The TVA envelope, read rather than invented. The record gives three
    // time/level pairs and a fourth time; the engine has one segment more, so
    // its fourth is made a no-op holding L3 and its release takes the record's
    // T4. L3 is the JV's own sustain level and it is 0 on every factory note,
    // which is what NO-SUSTAIN (envelope mode 0 on all 183) sounds like: the
    // note reaches silence at the end of segment 3 whether or not a note off
    // ever arrives, and on this device none does - a rhythm key accepts note on
    // only (flags 0x10 above).
    // The envelope time-sense, D-27. A rhythm note has ONE "time velo" nibble
    // per envelope where a patch tone has a T1/T4 pair, and ROM1 0x4E85-0x4E8D
    // mirrors it into both slots while forcing the time key-follow to neutral.
    // _init_neutral_partial() has already left the key-follow indices at 7, so
    // only the mirroring is needed here.
    if (R.tvaTimeVelocity)
      ip.TVAJVVelT1 = ip.TVAJVVelT4 = r[R.tvaTimeVelocity] & 0x0f;
    if (R.tvfTimeVelocity)
      ip.TVFJVVelT1 = ip.TVFJVVelT4 = r[R.tvfTimeVelocity] & 0x0f;

    ip.TVAEnvT1 = r[R.tvaEnv + 0] & 0x7f;
    ip.TVAEnvL1 = r[R.tvaEnv + 1] & 0x7f;
    ip.TVAEnvT2 = r[R.tvaEnv + 2] & 0x7f;
    ip.TVAEnvL2 = r[R.tvaEnv + 3] & 0x7f;
    ip.TVAEnvT3 = r[R.tvaEnv + 4] & 0x7f;
    ip.TVAEnvL3 = r[R.tvaEnv + 5] & 0x7f;
    ip.TVAEnvL4 = ip.TVAEnvL3;
    ip.TVAEnvT4 = 0x7f;
    ip.TVAEnvT5 = r[R.tvaEnv + 6] & 0x7f;

    // TVA level velocity sensitivity, SIGNED. Held at 0 before this, which in
    // the JV level law means the velocity helper is skipped entirely - so every
    // drum played at exactly one level whatever the velocity. The record has no
    // velocity CURVE field of its own, unlike a patch tone (+71 bits 0-2), so
    // the curve stays 0. That is the LINEAR one - the bank's curve 0 is
    // 254 - 2*v exactly, over all 128 entries - which is the neutral reading of
    // a field the record does not carry. Which curve the firmware's own rhythm
    // voice uses is NOT established here; see PROVENANCE P-0396.
    ip.TVALvlVSens  = (uint8_t) (int8_t) r[R.tvaVelLevelSens];
    ip.TVALvlVelCur = 0;

    // Dry Level: the attenuator on the direct output register only, as on a
    // patch tone. The sends sit in parallel with it and do not see it.
    ip.dryLevel = r[R.dryLevel] & 0x7f;

    // The filter. Filter Mode is two bits of +0x14 - a different position from
    // the patch tone's +55 bits 3-4, which is why the shift is profile data.
    // The engine's spelling puts low-pass first, so the device's 0/1/2 =
    // OFF/LPF/HPF is translated here; OFF must reach the engine as "disabled"
    // rather than as an open filter.
    {
      const int mode = (r[R.filterMode] >> R.filterModeShift) & 0x03;
      ip.TVFType = (mode == 1) ? 0 : (mode == 2) ? 1 : 2;
    }
    ip.TVFBaseFlt      = (int8_t) (r[R.filterCutoff]    & 0x7f);
    ip.TVFResonance    = (int8_t) (r[R.filterResonance] & 0x7f);
    ip.TVFResoMode     = (uint8_t) (r[R.resoMode] >> 7);
    ip.TVFEnvVelSens   = (int8_t) r[R.tvfVelLevelSens];
    ip.TVFEnvDepth     = r[R.tvfEnvDepth];

    // A rhythm note has no cutoff key follow and no TVF velocity curve, so both
    // take the value that means "no effect": the table entry holding 0 cents
    // per semitone, and curve 0.
    ip.TVFCOFKeyFlwIdx = (uint8_t) R.cutoffKeyFollowNeutral;
    ip.TVFCOFVelCur    = 0;

    ip.TVFEnvT1 = r[R.tvfEnv + 0] & 0x7f;
    ip.TVFEnvL1 = r[R.tvfEnv + 1] & 0x7f;
    ip.TVFEnvT2 = r[R.tvfEnv + 2] & 0x7f;
    ip.TVFEnvL2 = r[R.tvfEnv + 3] & 0x7f;
    ip.TVFEnvT3 = r[R.tvfEnv + 4] & 0x7f;
    ip.TVFEnvL3 = r[R.tvfEnv + 5] & 0x7f;
    ip.TVFEnvT5 = r[R.tvfEnv + 6] & 0x7f;
    ip.TVFEnvL5 = r[R.tvfEnv + 7] & 0x7f;
    ip.TVFEnvL4 = ip.TVFEnvL3;
    ip.TVFEnvT4 = 0x7f;

    const int key = R.firstKey + k;
    ds.key[key]    = r[R.playKey] & 0x7f;

    // Rhythm Note Level goes in as the TONE level, through the device's own
    // level law, and the drum-set level is held at full scale. It had been
    // going in as DrumParam::Level, which the engine consumes in the Sound
    // Canvas's dynamic-level chain - and on this device that chain no longer
    // runs at all, because the dynamic register carries the level law's static
    // byte (tva.cc, P-0398). The firmware has ONE per-voice level routine,
    // ROM1 0x4451, whose first argument is a per-voice word at @0x90d2; no
    // second level path exists for a rhythm voice, and a Rhythm Note's Level
    // (+0x1E, SysEx 0x24, 0-127) is the same field in the same role as a Patch
    // Tone's Level (D-21). The two laws differ by only 0.5 to 1.0 dB across
    // the factory range 75..127, so this is the reading with ROM support and
    // not a measurable preference.
    if (_profile->levelLawKind == LevelLawKind::JVCurveProduct) {
      ip.volume      = (uint8_t) (r[R.level] & 0x7f);
      ds.volume[key] = 0x7f;
    } else {
      ds.volume[key] = r[R.level] & 0x7f;
    }

    // Pan. The range is 0..128 and the manual prints the list as "L64 - 63R,
    // RND", so 128 is RANDOM and not an out-of-range hard right (D-02). The two
    // machines agree on what random pan is and spell it differently: the JV
    // writes 128, the Sound Canvas writes 0 and randomises in tva.cc. A JV 0 is
    // hard left, so it moves to 1 rather than becoming random - the reference
    // pans Closed HAT 1 and Open HAT 1, both of which carry 0 here, 37.2 dB and
    // 26.8 dB to the left. 18 of the 183 factory notes carry 128, 13 of them in
    // Preset B's kit, and clamping turned every one of them hard right.
    {
      const int pv = r[R.pan];
      ds.panpot[key] = (uint8_t) (pv >= 128 ? PANPOT_RANDOM : pv);
    }

    // The choke group. 0 means ungrouped; the engine's own assign-group
    // mechanism does the rest, and its semantics are the manual's - triggering
    // a note silences the sounding notes of the same group, itself included.
    ds.assignGroup[key] = r[R.muteGroup] & 0x1f;

    ds.reverb[key] = r[R.reverbSend] & 0x7f;
    ds.chorus[key] = r[R.chorusSend] & 0x7f;

    ds.preset[key] = (uint16_t) _instruments.size();
    _instruments.push_back(in);
  }

  _drumSets.push_back(ds);
  _drumSetsLUT.fill(0);              // one rhythm set, reachable from every bank

  return 0;
}




}
