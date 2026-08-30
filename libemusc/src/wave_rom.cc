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


#include "wave_rom.h"

#include <algorithm>

#include <cmath>
#include <fstream>
#include <iostream>


namespace EmuSC {


WaveRom::WaveRom(std::vector<std::string> romPath, ControlRom &ctrlRom)
{
  std::vector<char> romData;

  if (romPath.empty())
    throw (std::string("No wave ROM file specified"));

  for (auto rp : romPath) {
    std::ifstream romFile(rp, std::ios::binary | std::ios::in);
    if (!romFile.is_open()) {
      throw(std::string("Unable to open wave ROM file: ") + rp);
    }

    std::vector<char> encBuf((std::istreambuf_iterator<char>(romFile)),
			     std::istreambuf_iterator<char>());

    if (encBuf.size() % 0x100000)
      throw (std::string("Incorrect file size of Wave ROM file ") + rp +
	     std::string(". Wave ROM files are always a factor of 1 MB"));

    uint32_t offset = romData.size();
    romData.resize(romData.size() + encBuf.size());

    const enum ControlRom::SynthGen gen = ctrlRom.generation();

    for (int m = 0; m < encBuf.size(); m += 0x100000) {
      for (uint32_t i = 0; i < 0x100000; i++) {
	romData[_unscramble_address(i, gen) + m + offset] =
	  i >= 0x20 ? _unscramble_data(encBuf[i + m], gen) : encBuf[i + m];
      }
    }

    romFile.close();
  }

  // Debug: Dump complete decrypted ROM to file
  if (0) {
    std::ofstream dump("/tmp/wave_rom.bin", std::ios::binary);
    dump.write(&romData[0], romData.size());
    dump.close();
  }

  // Read through the entire memory and extract sample sets
  _sampleSets.reserve(ctrlRom.numSampleSets());

  for (int i = 0; i < ctrlRom.numSampleSets(); i ++)
    _read_samples(romData, ctrlRom.sample(i), ctrlRom.generation());

  _version = std::string(&romData[0x1c], 4);
  _date = std::string(&romData[0x30], 10);
}


// Discovered and written by NewRisingSun
uint32_t WaveRom::_unscramble_address(uint32_t address,
                                      enum ControlRom::SynthGen synthGen)
{
  // The JV family scrambles the CONTENTS of its wave ROMs exactly as the SC-55
  // does, so there is no JV branch here. The service-note schematic showing the
  // tone generator's WAD lines reaching the ROM's WA pins in a permuted order
  // describes the WIRING, which the dumps have already undone (P-0370); it says
  // nothing about the content scrambling below.


  uint32_t newAddress = 0;
  if (address >= 0x20) {	// The first 32 bytes are not encrypted
    static const int addressOrder [20] =
      { 0x02, 0x00, 0x03, 0x04,0x01, 0x09, 0x0D, 0x0A, 0x12,
        0x11, 0x06, 0x0F, 0x0B, 0x10, 0x08, 0x05, 0x0C, 0x07, 0x0E, 0x13 };
    for (uint32_t bit = 0; bit < 20; bit++) {
      newAddress |= ((address >> addressOrder[bit]) & 1) << bit;
    }
  } else {
    newAddress = address;
  }

  return newAddress;
}



int8_t WaveRom::_unscramble_data(int8_t byte,
                                 enum ControlRom::SynthGen synthGen)
{
  uint8_t byteOrder[8] = {2, 0, 4, 5, 7, 6, 3, 1};
  uint32_t newByte = 0;

  for (uint32_t bit = 0; bit < 8; bit++) {
    newByte |= ((byte >> byteOrder[bit]) & 1) << bit;
  }

  return newByte;
}


uint32_t WaveRom::_find_samples_rom_address(uint32_t address,
                                           enum ControlRom::SynthGen synthGen)
{
  // The JV addresses a flat space across its wave ROMs: no bank encoding.
  if (synthGen == ControlRom::SynthGen::JV880 ||
      synthGen == ControlRom::SynthGen::JV1080)
    return address;

  uint32_t bank = 0;
  switch ((address & 0x700000) >> 20)
    {
    case 0:
      bank = 0x000000;
      break;
    case 1:
      bank = 0x100000;
      break;
    case 2:
      bank = (synthGen == ControlRom::SynthGen::SC55mk2) ? 0x200000 : 0x100000;
      break;
    case 4:
      bank = 0x200000;
      break;
    default:
      throw(std::string("Unknown bank ID in WaveRom::get_sample(): ") +
	    std::to_string(address & 0x700000));
    }

  return (address & 0xFFFFF) | bank;
}


int WaveRom::_read_samples(std::vector<char> &romData,
                           struct ControlRom::Sample &ctrlSample,
                           enum ControlRom::SynthGen synthGen)
{
  struct Samples s;
  float sample = 0;

  const int sampleLen = ctrlSample.sampleLen;
  const int loopStart = sampleLen - ctrlSample.loopLen;
  const int span      = ctrlSample.loopLen;
  const bool pingPong = (ctrlSample.loopMode == 1);

  const uint32_t romAddress =
    _find_samples_rom_address(ctrlSample.address, synthGen);

  s.samplesF.reserve(sampleLen + 1 + (pingPong ? span + 1 : 0));

  // Forward decode for all loop variations
  for (int i = 0; i <= sampleLen; i++) {
    uint32_t sAddress = romAddress + i;
    int8_t  data    = romData[sAddress];
      uint8_t sByte   = romData[((sAddress & 0xFFFFF) >> 5) | (sAddress & 0xF00000)];
    uint8_t sNibble = (sAddress & 0x10) ? (sByte >> 4) : (sByte & 0x0F);
    int32_t final   = ((data << sNibble) << 14);

    // Normalize the accumulated delta to [-1, 1). The divisor must be a
    // positive constant: written as "1 << 31" it overflows a signed int and
    // becomes -2^31, which negates every delta and therefore inverts the
    // polarity of every sample set - and with it all audio output. Measured
    // against reference output on 42 isolated notes and drum hits, all of
    // which came out sign reversed (emusc-match finding P-0040).
    sample += (float) final / 2147483648.0f;         // 2^31
    s.samplesF.push_back(sample);
  }

  // A JV "REV ..." waveform is its forward twin's PCM played backwards: same
  // address, same length, only the flag byte differs (P-0372). Reversing the
  // decoded buffer here means the voice code needs no JV special case.
  if (ctrlSample.reverse)
    std::reverse(s.samplesF.begin(), s.samplesF.end());


  // Ping-pong loop: Append the turn + reflected reverse segment
  // (span + 1 extra entries; total cycle = 2*span + 2 positions)
  if (pingPong && span > 0) {
    const float sL = (loopStart > 0) ? s.samplesF[loopStart - 1] : 0.0f;
    const float sE   = s.samplesF[sampleLen - 1];
    const float sEd  = s.samplesF[sampleLen];
    s.samplesF.push_back(sL + (sEd - sE));

    // Reverse pass: vertical reflection about s[L]: 2*s[L] - s[E-m]
    for (int m = 1; m < span; m++)
      s.samplesF.push_back(2.0f * sL - s.samplesF[sampleLen - m - 1]);

    // Bottom-turn stall value: s[L].
    s.samplesF.push_back(sL);
  }

  _sampleSets.push_back(s);
  return s.samplesF.size();
}


}
