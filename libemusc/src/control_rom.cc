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


#include "control_rom.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <iostream>
#include <iomanip>


namespace EmuSC {


const std::vector<uint32_t> ControlRom::_banksSC55 =
  { 0x10000, 0x1BD00, 0x1DEC0, 0x20000, 0x2BD00, 0x2DEC0, 0x30000, 0x38000 };

// Only a placeholder, SC-88 layout is currently unkown
const std::vector<uint32_t> ControlRom::_banksSC88 =
  { 0x10000, 0x1BD00, 0x1DEC0, 0x20000, 0x2BD00, 0x2DEC0, 0x30000, 0x38000 };

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

  // Temporarily block SC-88 ROMs since we don't know how to read them yet
  if (_model == "SC-88")
    throw(std::string("SC-88 ROM files are not supported yet!"));

  // The JV family: partials, samples and the two preset patch banks live in
  // ROM. There is no drum-set table and no separate CPU ROM holding the lookup
  // tables, so the SC-55 sequence below does not apply.
  if (_synthModel == sm_JV880 || _synthModel == sm_JV1080) {
    for (int i = 0; i < JV_LAYOUT_COUNT; i++) {
      if (JV_LAYOUTS[i].model != _synthModel)
        continue;
      _read_jv_partials(romFile, JV_LAYOUTS[i].partialHint);
      _read_jv_samples(romFile, JV_LAYOUTS[i].sampleHint);
      if (JV_LAYOUTS[i].patchBankA)
        _read_jv_patches(romFile, JV_LAYOUTS[i].patchBankA);
        _read_jv_performances(romFile, JV_LAYOUTS[i].performances);
        _read_jv_rhythm(romFile, JV_LAYOUTS[i].rhythm);
      break;
    }
      _init_jv_lookup_tables();
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


int ControlRom::_identify_model(std::ifstream &romFile)
{
  char data[32];

  // Search for SC-55 control ROM files
  romFile.seekg(0xf380);
  romFile.read(data, 29);
  if (!strncmp(data, "Ver", 3)) {
    _version.assign(&data[3], 4);
    _date.assign(&data[24], 5);
    _model.assign("SC-55");
    _synthModel = sm_SC55;
    _synthGeneration = SynthGen::SC55;

    return 0;
  }

  // Search for SC-55mkII control ROM files
  romFile.seekg(0x3d148);
  romFile.read(data, 32);
  if (!strncmp(&data[0], "GS-28 VER=2.00  SC              ", 32)) {
    romFile.seekg(0xfff0);
    romFile.read(data, 10);
    _version.assign(data, 4);
    int year = (uint8_t) data[7];
    int month = (uint8_t) data[8];
    int day = (uint8_t) data[9];
    std::stringstream ss;
    ss << "19" << std::hex << year << "-" << month << "-" << day;
    _date.assign(ss.str());
    _model.assign("SC-55mkII");
    _synthModel = sm_SC55mkII;
    _synthGeneration = SynthGen::SC55mk2;

    return 0;
    
  } else if (!strncmp(&data[0], "GS-28 VER=2.00  LCGS-3 module   ", 32)) {
    _version.assign("?");
    _date.assign("?");
    _model.assign("SCB-55 (SC-55mkII)");
    _synthModel = sm_SC55mkII;
    _synthGeneration = SynthGen::SC55mk2;

    return 0;
  }

  // Search for SCC-1 control ROM files
  romFile.seekg(0x3D155);
  romFile.read(data, 29);
  if (!strncmp(data, "VER", 3)) {
    _version.assign(&data[3], 4);
    _date.assign(&data[24], 5);
    _model.assign("SCC-1");
    _synthModel = sm_SCC1;
    _synthGeneration = SynthGen::SC55;
  }

  // Search for SC-88 control ROM files
  romFile.seekg(0x7fc0);
  romFile.read(data, 24);
  if (!strncmp(&data[0], "GS-64 VER=3.00  SC-88   ", 24)) {
    _version.assign("?");
    _date.assign("?");
    _model.assign("SC-88");
    _synthModel = sm_SC88;
    _synthGeneration = SynthGen::SC88;
  }

  // No GS banner: the JV family identifies itself by its table structure.
  if (_model.empty() && _identify_jv(romFile))
    return 0;

  if (_model.empty())        // No valid ROM file found    TODO: SC88 ??
    return -1;

  return 0;
}


const std::vector<uint32_t> &ControlRom::_banks(void)
{
  switch(_synthModel)
    {
    case sm_SC55:
    case sm_SCC1:
    case sm_SC55mkII:
      return _banksSC55;

    case sm_SC88:                       // No work has been done here yet
      return _banksSC88;
    }

  throw(std::string("No ROM banks defined for this model"));
}


// Note: instrument partials (instPartial) contains 90 unused bytes! ADSR?
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
  for (x = banks[7] + 128; x < 0x03c028; x += 1164) {
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
  int numVCurves = 10;
  const struct _ProgMemoryMapLUT *PROGmmLUT;
  switch(_synthModel)
    {
    case SynthModel::sm_SC55:
      PROGmmLUT = &SC55_1_21_Prog_LUT;
      numVCurves = 10;
      break;
    case SynthModel::sm_SC55mkII:
      PROGmmLUT = &SC55mkII_1_01_Prog_LUT;
      numVCurves = 12;
      break;
    default:
      std::cerr << "libEmuSC: Unsupported ROM file!" << std::endl;
      exit(0);
    }

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
  lookupTables.KeyMapperOffset = PROGmmLUT->KeyMapper - 0x30000;

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
  const struct _CPUMemoryMapLUT *CPUmmLUT;
  switch(_synthModel)
    {
    case SynthModel::sm_SC55:
      CPUmmLUT = &SC55_1_21_CPU_LUT;
      break;
    case SynthModel::sm_SC55mkII:
      CPUmmLUT = &SC55mkII_1_01_CPU_LUT;
      break;
    default:
      std::cerr << "libEmuSC: Unsupported ROM file!" << std::endl;
      exit(0);
    }

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
  switch (_synthModel)
    {
    case sm_SC55:
    case sm_SCC1:
      return _maxPolyphonySC55;

    case sm_SC55mkII:
      return _maxPolyphonySC55mkII;

    case sm_SC88:
    case sm_SC88Pro:
      return _maxPolyphonySC88;

      case sm_JV880:
        return _maxPolyphonyJV880;

      case sm_JV1080:
        return _maxPolyphonyJV1080;
    }

  throw(std::string("No maximum polyphony defined for this model"));
}


const float ControlRom::voice_damp_rate(void)
{
  switch (_synthModel)
    {
    case sm_SC55:
    case sm_SCC1:
      return _voiceDampRateSC55;

    case sm_SC55mkII:
    case sm_SC88:
    case sm_SC88Pro:
      return _voiceDampRateSC55mkII;
    }

  return _voiceDampRateSC55mkII;
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
  int romIndex;
  int romSize;
  if (_synthModel == sm_SC55) {
    romIndex = 0;
    romSize = _banks()[0];
  } else if (_synthModel == sm_SC55mkII) {
    romIndex = 0x03fff0;
    romFile.seekg(0, std::ios::end);
    romSize = romFile.tellg();
  } else {          // Unkown structures for SC-88, just read entire ROM
    romIndex = 0;
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
  // TODO: Use SHA256 and proper ROM list to identify ROMs with intro animations
  if (_synthModel == sm_SC55mkII)
    return true;

  return false;
}


std::vector<uint8_t> ControlRom::get_intro_anim(int animIndex)
{
  int romIndex;
  int length;

  if (_synthModel == sm_SC55mkII) {
    if (animIndex == 0)
      romIndex = 0x70000;               // SC-55mkII
    else if (animIndex == 1)
      romIndex = 0x71280;               // SC-155mkII
    else
      return std::vector<uint8_t> {};

    length = 0x1280;

  } else {
    return std::vector<uint8_t> {};
  }

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
const ControlRom::JVLayout ControlRom::JV_LAYOUTS[] = {
  { sm_JV1080, "JV-1080", 1024 * 1024, 0x71008, 0x075c7a, 0,        0, 0, 4, SynthGen::JV1080 },
  { sm_JV880,  "JV-880",   256 * 1024, 0x000004, 0x001e40, 0x008ce0, 0x008020, 0x00e760, 2, SynthGen::JV880  },
};
const int ControlRom::JV_LAYOUT_COUNT =
  (int) (sizeof(JV_LAYOUTS) / sizeof(JV_LAYOUTS[0]));


// The JV control ROMs carry no GS banner, so the machine is identified by its
// tables: a run of 60-byte records whose first field is a printable name. Size
// narrows the candidates; the table must then actually parse.
bool ControlRom::_identify_jv(std::ifstream &romFile)
{
  romFile.seekg(0, std::ios::end);
  size_t size = (size_t) romFile.tellg();
  romFile.seekg(0);
  _jvRom.resize(size);
  romFile.read((char *) &_jvRom[0], size);

  auto namelike = [this](uint32_t o) -> bool {
    if ((size_t) o + 12 > _jvRom.size()) return false;
    int alnum = 0;
    for (int i = 0; i < 12; i++) {
      uint8_t ch = _jvRom[o + i];
      if (ch < 0x20 || ch > 0x7e) return false;
      if (isalnum(ch)) alnum++;
    }
    return alnum >= 3;
  };

  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < JV_LAYOUT_COUNT; i++) {
      const JVLayout &L = JV_LAYOUTS[i];
      if ((size == L.romSize) != (pass == 0))
        continue;
      // require a run, not a single record: isolated printable triples occur
      int run = 0;
      for (int k = 0; k < 8; k++)
        run += namelike(L.partialHint + k * 60) ? 1 : 0;
      if (run < 8)
        continue;
      _synthModel      = L.model;
      _synthGeneration = L.generation;
      _model.assign(L.name);
      _version.assign("?");
      _date.assign("?");
      return true;
    }
  }

  _jvRom.clear();
  return false;
}


// A 60-byte waveform record is a Partial: a name, note breakpoints (0x7f
// padded, all values 20..120, i.e. MIDI keys) and indices into the sample
// table, 0xFFFF marking an unused zone.
int ControlRom::_read_jv_partials(std::ifstream &romFile, uint32_t hint)
{
  const int STRIDE = 60, NAME = 12, ZONES = 11;
  auto namelike = [this](uint32_t o) -> bool {
    if ((size_t) o + NAME > _jvRom.size()) return false;
    int alnum = 0;
    for (int i = 0; i < NAME; i++) {
      uint8_t ch = _jvRom[o + i];
      if (ch < 0x20 || ch > 0x7e) return false;
      if (isalnum(ch)) alnum++;
    }
    return alnum >= 2;
  };

  uint32_t base = hint;
  while (base >= (uint32_t) STRIDE && namelike(base - STRIDE))
    base -= STRIDE;

  for (uint32_t off = base; namelike(off); off += STRIDE) {
    struct Partial p;
    p.name.assign((const char *) &_jvRom[off], NAME);
    p.name.erase(p.name.find_last_not_of(' ') + 1);

      // Only ZONES entries are real breakpoints; the SC-55 pads the rest with
      // 127 and the voice code relies on that, so pad the same way rather than
      // letting a trailing non-breakpoint byte read as a key range.
      for (int i = 0; i < 16; i++)
        p.breaks[i] = (i < ZONES) ? _jvRom[off + NAME + i] : 127;

    for (int i = 0; i < 16; i++) {
      if (i >= ZONES) { p.samples[i] = 0xFFFF; continue; }
      uint32_t q = off + NAME + 16 + i * 2;
      p.samples[i] = (uint16_t) ((_jvRom[q] << 8) | _jvRom[q + 1]);
    }

    _partials.push_back(p);
  }

  return 0;
}


// An 18-byte sample record is a Sample. The ROM stores start, loop and end as
// absolute 24-bit addresses; the header - which holds the root key - sits in
// the NEXT slot (P-0362). Every slot is kept, valid or not, because the
// partials index this table positionally.
int ControlRom::_read_jv_samples(std::ifstream &romFile, uint32_t hint)
{
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
  for (int i = 0; i < JV_LAYOUT_COUNT; i++)
    if (JV_LAYOUTS[i].model == _synthModel) waveRoms = JV_LAYOUTS[i].waveRoms;
  const uint32_t WAVE_SPACE = (uint32_t) waveRoms * 2 * 1024 * 1024;

  size_t needed = 0;
  for (const struct Partial &p : _partials)
    for (int i = 0; i < 16; i++)
      if (p.samples[i] != 0xFFFF && p.samples[i] + 1u > needed)
        needed = p.samples[i] + 1u;
  if (!needed)
    return -1;

  auto u24 = [this](uint32_t o) -> uint32_t {
    if ((size_t) o + 3 > _jvRom.size()) return 0;
    return ((uint32_t) _jvRom[o] << 16) | ((uint32_t) _jvRom[o+1] << 8) | _jvRom[o+2];
  };

  for (size_t i = 0; i < needed; i++) {
    uint32_t o = hint + (uint32_t) i * 18;
    if ((size_t) o + 18 > _jvRom.size())
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
    s.loopMode  = _jvRom[o + 12] & 0x03;
    s.reverse   = _jvRom[o + 12] & 0x04;
    }
    s.rootKey = _jvRom[o + 13];
      s.volume    = 0x7f;   // no per-sample attenuation: 0 would mean silence
                             // under the TVA level law, not "neutral"
      s.pitchInit = 0x0400;  // 0x0400 is the SC-55's neutral pitch offset; the
      s.pitchSust = 0x0400;  // JV table has no such field, and 0 detunes hard
    _samples.push_back(s);
  }

  return _samples.empty() ? -1 : 0;
}


int ControlRom::_read_jv_patches(std::ifstream &romFile, uint32_t bankA)
{
  // A patch record is 362 bytes: 26 of common data, the 12-byte name among
  // them, then four 84-byte tones. Byte +0 of a tone switches it on and byte
  // +1 selects a waveform; the rest is not identified yet (P-0373), so the
  // fields below carry neutral values rather than guesses.
  const int STRIDE = 362, NAME = 12, TONE0 = 26, TONE = 84, TONES = 4;
  const int PER_BANK = 64, BANK_STRIDE = 0x8000;

  for (int bank = 0; bank < 3; bank++) {
    for (int p = 0; p < PER_BANK; p++) {
      uint32_t off = bankA + bank * BANK_STRIDE + p * STRIDE;
      if ((size_t) off + STRIDE > _jvRom.size())
        return _instruments.size();

      struct Instrument in = {};
      in.name.assign((const char *) &_jvRom[off], NAME);
      in.name.erase(in.name.find_last_not_of(' ') + 1);
        // Patch Level, common byte +21. Without it a four-tone patch plays all
        // four at full gain: SAW Lead, which the demo's melody uses, has four
        // tones enabled at level 127 with no velocity split, and rendered
        // 19.6 dB louder than the machine on that channel alone. +21 is the only
        // common byte that FALLS as the tone count rises (correlation -0.300
        // across 192 patches), which is what a level compensating for layering
        // has to do. Range 44..127, median 118.
        in.volume = _jvRom[off + 21] & 0x7f;
      in.partialsUsed = 0;

      for (int t = 0; t < TONES; t++) {
        const uint8_t *tb = &_jvRom[off + TONE0 + t * TONE];
        struct InstPartial &ip = in.partials[t];

        ip.partialIndex = 0xFFFF;
        if (!tb[0])                      // tone switched off
          continue;
        if (tb[1] >= _partials.size())   // an expansion waveform we cannot play
          continue;

        in.partialsUsed |= 1 << t;
        ip.partialIndex = tb[1];
        ip.panpot       = 0x40;
        ip.coarsePitch  = 0x40;
        ip.finePitch    = 0x40;
        ip.volume       = 0x7f;
          // Velocity range, +3 and +4. Sounding every tone regardless of
          // velocity is why layered patches were too loud and wrong in timbre:
          // 22% of tones are velocity-limited layers meant to sound only part
          // of the time, and the Internal bank averages 3.41 tones per patch
          // against Preset A's 2.44 - which is exactly why Internal measured
          // 5 dB louder than Preset A. The pair passes the structural test on
          // all 539 enabled tones: lower is never above upper.
          ip.velRangeLow  = tb[3] & 0x7f;
          ip.velRangeHigh = tb[4] & 0x7f;
          if (ip.velRangeHigh < ip.velRangeLow) {
            ip.velRangeLow = 0; ip.velRangeHigh = 127;
          }
          // NOT 127. tva.cc computes the velocity-driven level as
          //   127 - ((127 - velocity) * (127 - TVALvlVSens)) / 127
          // so 127 makes the second term zero and every note plays at full
          // level whatever the velocity - no dynamics at all, which is what
          // the owner heard as everything sounding "flat". 0 passes velocity
          // straight through. The tone's own sensitivity byte is not
          // identified yet, so full response is the honest default: wrong
          // dynamics beat none.
          ip.TVALvlVSens  = 0;
          // The filter. +52 is the cutoff and +53 the resonance, adjacent as the
          // manual has them (SysEx 0x4A, 0x4B), and +52 is confirmed on the
          // oracle: driving it 0 to 127 moves that patch's spectral centroid from
          // 99 Hz to 441 Hz, the largest and cleanest swing of any byte tested.
          // Without any filter our centroid sat at 745 Hz against the oracle's 282.
          //
          // The TVF envelope is held flat: its own bytes are not identified, and a
          // sweeping filter guessed at would be worse than a static one read.
          // LEFT OFF, deliberately. Enabling it costs 25 dB: median level over
          // Preset A 01-08 falls from -8.8 dB to -33 dB against the oracle while
          // the centroid improves (PR-A 08 goes 689 -> 264 Hz against 299). The
          // cause is not the cutoff value but the curves underneath it -
          // TVFResonanceFreq shapes the filter coefficients and is still zero
          // here, because the JV's own filter tables have not been found. A
          // filter that is right in colour and 25 dB wrong in level is worse
          // than none, so this waits for those tables. TASK-147.
          // LEFT DISABLED, and this is now a measured decision rather than a
          // missing table. With TVFResonanceFreq, TVFEnvDepth, TVFCutoffVSens and
          // TVFEnvScale all fitted from the SC-55's, the colour comes out close -
          // PR-A 01 renders a 244 Hz centroid against the oracle's 282 - and the
          // level still collapses 35 dB, at any resonance from 0 to the patch's
          // own. So the fault is not the coefficient curves but the cutoff INDEX:
          // it is assembled from several fields the JV path does not fill, and
          // TVFCFKeyFlwC and TVFCOFVelCur among them are still zero. Finding what
          // the JV puts there needs the code that reads them, not another fit.
          ip.TVFType      = 2;                    // DISABLED - see below
          // The filter is off, and this is measured rather than cautious. Left
          // on with the cutoff byte as the ROM stores it, the patch tracks lose
          // their entire top: the demo's melody renders 10.8 dB down at 125 Hz
          // and 63.0 dB down at 2 kHz against the machine. The owner heard it
          // immediately - "everything is more muffled than the oracle, which is
          // super crisp and bright" - and the drums, which take a different path
          // with no filter, were the only track within 1 dB across the band.
          //
          // The cutoff byte has a median of 15 across Preset A where the SC-55's
          // equivalent is 62, so the SC-55 cutoff arithmetic reads it as almost
          // shut. Scaling it into range is a fudge that was tried and removed;
          // what is needed is the JV's own cutoff law, not a rescaled byte.
          // The JV's cutoff byte is not on the SC-55's scale: its median across
          // Preset A is 15 where the SC-55's TVFBaseFlt is 62, and feeding it
          // straight in gives a nearly shut filter - 35 dB of level. Mapped
          // Used exactly as the ROM stores it. An earlier version scaled and
          // offset this to make the filter behave, and that was the wrong
          // instinct: a table that needs a fudge factor to work is either the
          // wrong table or is being fed to the wrong arithmetic.
          ip.TVFBaseFlt   = (int8_t) (tb[52] & 0x7f);
          ip.TVFResonance = (int8_t) (tb[53] & 0x7f);
          ip.TVFEnvDepth  = 0;
          ip.TVFEnvL1 = ip.TVFEnvL2 = ip.TVFEnvL3 = ip.TVFEnvL4 = ip.TVFEnvL5 = 0x7f;
          ip.TVFEnvT1 = 0;
          ip.TVFEnvT2 = ip.TVFEnvT3 = ip.TVFEnvT4 = ip.TVFEnvT5 = 0x7f;
          // Key follow, at the SC-55's normal setting. pitchKeyFlw indexes a
          // 21-entry table as |pitchKeyFlw - 0x49|, so 0 is both wrong and out
          // of bounds; 0x4a and rootKeyOffset 64 are what SC-55 partials carry.
          ip.pitchKeyFlw    = 0x4a;
          ip.rootKeyOffset  = 64;

          // Every "key follow" and "velocity sensitivity" field is read as
          // (value - 0x40), so leaving them zero is not neutral - it is the
          // maximum NEGATIVE adjustment, and it crushed every envelope time to
          // nothing. A.Piano 1 fell to a tenth of its peak in 16 ms where the
          // machine takes 896 ms, which is why every Preset A patch measured
          // 20 dB quiet. 0x40 is the centre these are measured from.
          ip.TVAETKeyF14 = ip.TVAETKeyF5  = 0x40;
          ip.TVAETVSens12 = ip.TVAETVSens35 = 0x40;
          ip.TVAETKeyFP14 = ip.TVAETKeyFP5 = 0;
          ip.TVFETKeyF14 = ip.TVFETKeyF5  = 0x40;
          ip.TVFETVSens12 = ip.TVFETVSens35 = 0x40;
          ip.TVFETKeyFP14 = ip.TVFETKeyFP5 = 0;
          ip.pitchETKeyF14 = ip.pitchETKeyF5 = 0x40;
          ip.pitchETKeyFP14 = ip.pitchETKeyFP5 = 0;
          ip.pitchEnvTVSens = 0x40;

    // Everything else the engine reads as (value - 0x40). The audit in
    // tools/jv1080/jv-zero-audit.py lists them; leaving any at zero is the
    // maximum negative setting, which is how the envelope times were lost.
    ip.pitchEnvL0 = ip.pitchEnvL1 = ip.pitchEnvL2 = 0x40;
    ip.pitchEnvL3 = ip.pitchEnvL5 = 0x40;
    ip.pitchEnvVSens = 0x40;
    ip.TVFCFKeyFlw   = 0x40;
    ip.TVFCOFVSens   = 0x40;
    ip.TVABiasLevel  = 0;

          // Everything else the engine reads as (value - 0x40). The audit in
          // tools/jv1080/jv-zero-audit.py lists them; leaving any at zero is the
          // maximum negative setting, which is how the envelope times were lost.
          ip.pitchEnvL0 = ip.pitchEnvL1 = ip.pitchEnvL2 = 0x40;
          ip.pitchEnvL3 = ip.pitchEnvL5 = 0x40;
          ip.pitchEnvVSens = 0x40;
          ip.TVFCFKeyFlw   = 0x40;
          ip.TVFCOFVSens   = 0x40;
          // NOT 0x40. TVA::_init_envelope branches on (TVABiasLevel >= 0x40) and
          // takes the ATTENUATING branch when it does, so centring this one costs
          // 21 dB - measured, median level over Preset A fell -8.8 -> -30.1 dB.
          // Its neutral is 0, unlike its neighbours. Zero is not always wrong
          // either.
          ip.TVABiasLevel  = 0;


          // The TVA envelope, from Roland's own parameter address map in the
          // JV-880 owner's manual (docs/service-notes/jv880-owner.md): seven
          // INTERLEAVED bytes, T1 L1 T2 L2 T3 L3 T4, with no L4 - T4 is the
          // release to silence. Confirmed against the oracle by rewriting each
          // byte and rendering: +74 collapses the peak to 0.035 while stretching
          // the attack 6.3x (a long attack time), +78 moves attack monotonically,
          // +80 nearly triples the release, and +81 raises the peak.
          ip.TVAEnvT1 = tb[74] & 0x7f;
          ip.TVAEnvL1 = tb[75] & 0x7f;
          ip.TVAEnvT2 = tb[76] & 0x7f;
          ip.TVAEnvL2 = tb[77] & 0x7f;
          ip.TVAEnvT3 = tb[78] & 0x7f;
          ip.TVAEnvL3 = tb[79] & 0x7f;

          // libEmuSC has one phase more than the JV: L1-L4 with T1-T5, where T5
          // is the release. The JV sustains at L3 and releases over T4, so hold
          // the fourth phase at L3 and give the release the JV's T4.
          ip.TVAEnvL4 = ip.TVAEnvL3;
          ip.TVAEnvT4 = 0x7f;
          ip.TVAEnvT5 = tb[80] & 0x7f;

          // Level. Two bytes carry it and libEmuSC's partial has one field, so
          // they combine: +67 is the tone's TVA Level - the strongest single
          // result of the whole probe sweep, peak monotonic with Spearman +0.975
          // - and +81 is the Dry Level the manual puts beside the sends. Using
          // only +81 leaves the render hot, because 94% of tones set it to 127
          // where only 52% set +67 there.
          ip.volume   = (uint8_t) (((tb[67] & 0x7f) * (tb[81] & 0x7f)) / 127);
      }

      _instruments.push_back(in);
    }
  }

  // Programs 0-63 select Preset A and 64-127 Preset B. The JV is not a GM
  // machine and has no variation table of its own, so this mapping is ours.
  for (int v = 0; v < 128; v++)
    for (int i = 0; i < 128; i++)
      _variations[v][i] = 0xffff;
  for (int i = 0; i < (int) _instruments.size() && i < 128; i++)
    _variations[0][i] = i;

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
void ControlRom::_init_jv_lookup_tables(void)
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
    if ((size_t) base + n > _jvRom.size()) return false;
    for (int i = 0; i < n; i++) dst[i] = _jvRom[base + i];
    return true;
  };
  auto rom16 = [this](uint32_t base, int n, auto &dst) {
    if ((size_t) base + 2*n > _jvRom.size()) return false;
    for (int i = 0; i < n; i++)
      dst[i] = (_jvRom[base + i*2] << 8) | _jvRom[base + i*2 + 1];
    return true;
  };
  bool haveRom = _jvRom.size() > 0x40000 - 1;


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
  const uint32_t ENV_TIME_TABLE = 0x04c58;
  if (_jvRom.size() >= ENV_TIME_TABLE + 256) {
    for (int i = 0; i < 128; i++)
      t.envelopeTime[i] = (int) ((_jvRom[ENV_TIME_TABLE + i * 2] << 8) |
                                  _jvRom[ENV_TIME_TABLE + i * 2 + 1]);
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
    if (base && (size_t) base + 258 <= _jvRom.size()) {
      for (int i = 0; i < 129; i++)
        t.TVFCutoffFreq[i] = (_jvRom[base + i*2] << 8) | _jvRom[base + i*2 + 1];
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
  // Tables read from the JV's own ROM, as a table rather than as inline calls
  // so the device's data stays data. Each was found by matching the SC-55's
  // equivalent numerically and each is CHECKED here for the shape it must have,
  // because three earlier entries were wrong and one of them broke stereo:
  // TVAPanpot came back non-monotonic (0, 107, 5, 54, ...) where a pan law has
  // to rise, and every drum played centre. A numerical match is a lead; the
  // shape check is what makes it a finding.
  struct RomTable { uint32_t off; int n; int width; bool mustRise; const char *name; };
  static const RomTable jvTables[] = {
    { 0x3e9c4, 256, 1, false, "TVFResonanceFreq" },
    { 0x054be, 128, 1, false, "TVFResonance"     },
    { 0x055f5,   9, 1, false, "EnvSegmentCurve"  },
    { 0x3e931, 128, 1, false, "TVAPanKeyFollow"  },
    { 0x05590, 128, 1, false, "TVALevelIndex"    },
    { 0x3ff49,  21, 1, true,  "EnvTimeKeyFollowSens" },
    { 0x04edf, 130, 1, false, "LFOSine"          },
    { 0x06b2c,  47, 2, true,  "PitchCoarseExp"   },
    // Dropped, and why: TVAPanpot (0x3e946), TVFEnvScale (0x3fc79) and
    // TVFCutoffVSens (0x02d15) all failed the rise check - 11, 4 and 4
    // inversions in curves that must be monotonic. They keep the fitted
    // SC-55 shape instead of a wrong reading.
  };

  auto rise_ok = [](const uint8_t *v, int n, int width) {
    int inv = 0;
    for (int i = 0; i + 1 < n; i++) {
      int a = width == 1 ? v[i]   : (v[i*2] << 8)   | v[i*2+1];
      int b = width == 1 ? v[i+1] : (v[i*2+2] << 8) | v[i*2+3];
      if (b < a) inv++;
    }
    return inv <= n / 20;
  };

  if (haveRom) {
    for (const RomTable &rt : jvTables) {
      if ((size_t) rt.off + (size_t) rt.n * rt.width > _jvRom.size()) continue;
      const uint8_t *v = &_jvRom[rt.off];
      if (rt.mustRise && !rise_ok(v, rt.n, rt.width)) continue;
      if (!strcmp(rt.name, "TVFResonanceFreq")) rom8 (rt.off, rt.n, t.TVFResonanceFreq);
      else if (!strcmp(rt.name, "TVFResonance")) rom8 (rt.off, rt.n, t.TVFResonance);
      else if (!strcmp(rt.name, "EnvSegmentCurve")) rom8 (rt.off, rt.n, t.EnvSegmentCurve);
      else if (!strcmp(rt.name, "TVAPanKeyFollow")) rom8 (rt.off, rt.n, t.TVAPanKeyFollow);
      else if (!strcmp(rt.name, "TVALevelIndex")) rom8 (rt.off, rt.n, t.TVALevelIndex);
      else if (!strcmp(rt.name, "EnvTimeKeyFollowSens")) rom8 (rt.off, rt.n, t.EnvTimeKeyFollowSens);
      else if (!strcmp(rt.name, "LFOSine")) rom8 (rt.off, rt.n, t.LFOSine);
      else if (!strcmp(rt.name, "PitchCoarseExp")) rom16(rt.off, rt.n, t.PitchCoarseExp);
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
int ControlRom::_read_jv_performances(std::ifstream &romFile, uint32_t base)
{
  const int STRIDE = 204, COMMON = 28, PART = 22, PARTS = 8;
  const int P_PATCH = 16, P_CHAN = 21;

  _jvChannelPatch.fill(-1);
  _jvChannelLevel.fill(100);
  _jvChannelReverb.fill(0);
  _jvChannelChorus.fill(0);
  _jvChannelPan.fill(0x40);
  _jvChannelKeyShift.fill(0);
  if (!base || (size_t) base + STRIDE > _jvRom.size())
    return -1;

  // Which performance the machine powers on with is held in NVRAM, at byte
  // 0x04 and one-based, and we deliberately do not require an NVRAM file
  // (P-0374). So it has to be defaulted, and the default is not a guess:
  //
  //   - the reference's NVRAM holds 0x07 there, which is performance 7, index 6;
  //   - and index 6 is, independently, the one of the sixteen whose render
  //     matches the reference. Sweeping all of them on the demo's melody channel
  //     alone: index 6 gives -1.4 dB and 9.20 dB spectral distance where index 0
  //     - the obvious assumption, and the one used until now - gives +19.1 dB and
  //     11.61 dB. On the whole demo, +2.6 dB and 4.86 dB against +17.5 and 7.35.
  //
  // Index 0 was assumed because it is the first, and the owner's ear caught it
  // before any measurement did: "it still doesn't feel like the right
  // instruments, and now the melody is very loud". Index 6 puts BrightGuitar,
  // Brass Sect., SA Rhodes and Pan Pipe on the parts, which is what a
  // multi-timbral demo looks like; index 0 puts SAW Lead on every part.
  int which = 6;
  const char *pe = getenv("EMUSC_JV_PERF");
  if (pe) { int v = atoi(pe); if (v >= 0 && v < 16) which = v; }
  if ((size_t) base + (which + 1) * STRIDE > _jvRom.size()) which = 0;
  const uint8_t *p = &_jvRom[base + which * STRIDE];

  for (int t = 0; t < PARTS; t++) {
    const uint8_t *pt = p + COMMON + t * PART;
    int patch = pt[P_PATCH];
    int chan  = pt[P_CHAN] & 0x0f;

    // Keep the parts as parts. Several may share one MIDI channel - performance
    // 7 layers patches 1 and 26 on channel 1 - and a channel-keyed map silently
    // drops all but the first, which is one patch of two on the demo's melody.
    _jvParts[t] = { patch, chan, pt[17] & 0x7f, pt[18] & 0x7f,
                    (int8_t) pt[19], 0, 0, t == PARTS - 1 };
    if (t != PARTS - 1 && patch < (int) _jvInstSend.size()) {
      _jvParts[t].reverb = _jvInstSend[patch].first;
      _jvParts[t].chorus = _jvInstSend[patch].second;
    }

    // Part 8 is the rhythm part, not a patch part: the manual's own signal
    // diagram shows parts 1-7 taking a Patch and part 8 taking a Rhythm set, and
    // its +16 is 0 in every factory performance rather than a patch number. Its
    // channel is the drum channel - 0xE9, channel 9, in all sixteen.
    if (t == PARTS - 1) {
      _jvDrumChannel = chan;
      continue;
    }
    if (patch >= (int) _instruments.size())
      continue;
    // Earlier parts win: several parts may share a channel to layer sounds, and
    // libEmuSC has one instrument per part where the JV has eight.
    if (_jvChannelPatch[chan] < 0) {
      _jvChannelPatch[chan] = patch;
      // +17 is the part level and +18 its pan, following +16 in the same order
      // the manual gives (Patch Number, then Part Level at 0x19, Part Pan at
      // 0x1A) once the split pair at 16/17 is collapsed to one byte.
      _jvChannelLevel[chan] = pt[17] & 0x7f;
      _jvChannelPan[chan]   = pt[18] & 0x7f;
      // +19 is the part's coarse tune, in SIGNED semitones. Leaving it out put
      // the demo's melody exactly one octave high - the owner heard it as the
      // wrong musical key. Its values across the sixteen performances are all
      // intervals a musician would choose: -12, +12, -7, -8, -2, +20, +24, -29.
      _jvChannelKeyShift[chan] = (int8_t) pt[19];
      if (patch < (int) _jvInstSend.size()) {
        _jvChannelReverb[chan] = _jvInstSend[patch].first;
        _jvChannelChorus[chan] = _jvInstSend[patch].second;
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
// "Bright Kick", 38 "90's Snare", 42 "Closed HAT 1", 46 "Open HAT 1". Byte +0
// is the on/off switch, as it is in a patch tone.
//
// libEmuSC's drum path wants an INSTRUMENT per key, so each rhythm note becomes
// one, appended after the patches. Their names come from the waveform, which
// makes a drum map readable in the instrument list.
int ControlRom::_read_jv_rhythm(std::ifstream &romFile, uint32_t base)
{
  const int STRIDE = 44, KEYS = 61, FIRST_KEY = 36;

  if (!base || (size_t) base + KEYS * STRIDE > _jvRom.size())
    return -1;

  struct DrumSet ds = {};
  ds.name = "JV Rhythm";
  for (int k = 0; k < 128; k++) {
    ds.preset[k] = 0xffff;
    ds.volume[k] = 0x7f;
    // Every drum plays at its own natural pitch, not at the key struck: the
    // SC-55's own drum sets put 60 in every entry of this table, and using the
    // key instead transposes a kick down until it is muffled noise - which is
    // what it sounded like. Flags 0x10 is note-on only; a drum is one-shot and
    // must ignore note-off, which 0x11 does not.
    ds.key[k]    = 60;
    ds.panpot[k] = 0x40;
    ds.flags[k]  = 0x10;
  }

  for (int k = 0; k < KEYS; k++) {
    const uint8_t *r = &_jvRom[base + k * STRIDE];
    if (!r[0] || r[1] >= _partials.size())
      continue;

    struct Instrument in = {};
    in.name = _partials[r[1]].name;
    in.volume = 0x7f;
    in.partialsUsed = 1;
    for (int t = 0; t < 4; t++)
      in.partials[t].partialIndex = 0xFFFF;

    struct InstPartial &ip = in.partials[0];
    ip.partialIndex   = r[1];
    ip.panpot         = 0x40;
    ip.coarsePitch    = 0x40;
    ip.finePitch      = 0x40;
    ip.volume         = 0x7f;
    ip.velRangeLow    = 0;
    ip.velRangeHigh   = 127;
    ip.TVALvlVSens    = 0;
    ip.TVFType        = 2;
    ip.pitchKeyFlw    = 0x4a;
    ip.rootKeyOffset  = 64;

    // Every "key follow" and "velocity sensitivity" field is read as
    // (value - 0x40), so leaving them zero is not neutral - it is the
    // maximum NEGATIVE adjustment, and it crushed every envelope time to
    // nothing. A.Piano 1 fell to a tenth of its peak in 16 ms where the
    // machine takes 896 ms, which is why every Preset A patch measured
    // 20 dB quiet. 0x40 is the centre these are measured from.
    ip.TVAETKeyF14 = ip.TVAETKeyF5  = 0x40;
    ip.TVAETVSens12 = ip.TVAETVSens35 = 0x40;
    ip.TVAETKeyFP14 = ip.TVAETKeyFP5 = 0;
    ip.TVFETKeyF14 = ip.TVFETKeyF5  = 0x40;
    ip.TVFETVSens12 = ip.TVFETVSens35 = 0x40;
    ip.TVFETKeyFP14 = ip.TVFETKeyFP5 = 0;
    ip.pitchETKeyF14 = ip.pitchETKeyF5 = 0x40;
    ip.pitchETKeyFP14 = ip.pitchETKeyFP5 = 0;
    ip.pitchEnvTVSens = 0x40;


    // A drum is one shot: reach full level at once and hold, and let the sample
    // end the note. The rhythm record's own envelope bytes are not identified -
    // its 44 bytes are not the patch tone's 84, so the +74..+80 map does not
    // carry over.
    // A drum must DECAY - held flat it rings for ever, which is what the owner
    // heard as "cymbals never seem to be released". EMUSC_JV_RHY_ENV names the
    // record offset of the seven interleaved envelope bytes to try.
    {
      const char *re = getenv("EMUSC_JV_RHY_ENV");
      int eo = re ? atoi(re) : -1;
      if (eo >= 0 && eo + 7 <= STRIDE) {
        ip.TVAEnvT1 = r[eo+0] & 0x7f; ip.TVAEnvL1 = r[eo+1] & 0x7f;
        ip.TVAEnvT2 = r[eo+2] & 0x7f; ip.TVAEnvL2 = r[eo+3] & 0x7f;
        ip.TVAEnvT3 = r[eo+4] & 0x7f; ip.TVAEnvL3 = r[eo+5] & 0x7f;
        ip.TVAEnvL4 = ip.TVAEnvL3;
        ip.TVAEnvT4 = 0x7f;
        ip.TVAEnvT5 = r[eo+6] & 0x7f;
      } else {
        ip.TVAEnvL1 = 0x7f; ip.TVAEnvL2 = 0x60;
        ip.TVAEnvL3 = ip.TVAEnvL4 = 0x00;   // decay to silence
        ip.TVAEnvT1 = 0x00; ip.TVAEnvT2 = 0x40;
        ip.TVAEnvT3 = 0x50; ip.TVAEnvT4 = 0x7f; ip.TVAEnvT5 = 0x20;
      }
    }

    // Per-key level and pan. The manual lists Level then Pan adjacent in the
    // Rhythm Note table (SysEx 0x24, 0x25); in the ROM record they land at +30
    // and +31, the record being packed as the tone record is rather than
    // matching SysEx offsets one for one. The distributions say the same thing:
    // +30 runs 75..127 with a median of 127, which is a level, and +31 runs
    // 0..128 with a median of 64, which is a pan because it is CENTRED.
    //
    // And the values are musically right, which no arbitrary column would be:
    // kick and snare centred at 64, both hats hard left at 0, ride at 118.
    ds.volume[FIRST_KEY + k] = r[30] & 0x7f;
    // Clamped away from 0. The Sound Canvas reads a drum pan of 0 as RND and
    // randomises the note (tva.cc), but the JV means hard left by it: the
    // reference pans Closed HAT 1 and Open HAT 1 - both of which carry 0 here -
    // 37.2 dB and 26.8 dB to the left, while ours came out dead centre because
    // the random pan averages there. 1 is hard left without tripping RND.
    ds.panpot[FIRST_KEY + k] = (uint8_t) std::clamp<int>(r[31], 1, 127);

    ds.preset[FIRST_KEY + k] = (uint16_t) _instruments.size();
    _instruments.push_back(in);
  }

  _drumSets.push_back(ds);
  _drumSetsLUT.fill(0);               // one rhythm set, reachable from every bank

  return 0;
}




}
