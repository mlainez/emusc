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

    // The block the address permutation spans: 20 bits on the SC-55 family,
    // 21 on the JV-1080, whose service-note wiring covers a whole 2 MB device.
    const enum ControlRom::SynthGen gen = ctrlRom.generation();
    const bool isJV = (gen == ControlRom::SynthGen::JV880 ||
                       gen == ControlRom::SynthGen::JV1080);

    if (isJV) {
      // Stored raw: the JV's address permutation is applied per device when a
      // sample is read, not once over the whole concatenation.
      std::copy(encBuf.begin(), encBuf.end(), romData.begin() + offset);
    } else {
      for (int m = 0; m < encBuf.size(); m += 0x100000) {
        for (uint32_t i = 0; i < 0x100000; i++) {
	  romData[_unscramble_address(i, gen) + m + offset] =
	    i >= 0x20 ? _unscramble_data(encBuf[i + m], gen) : encBuf[i + m];
        }
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
  // JV-1080: the tone generator's WAD lines reach the ROM's WA pins through
  // this permutation, read from Roland's service notes p.15 (P-0357). The
  // JV-880's dumps read straight through.
  if (synthGen == ControlRom::SynthGen::JV1080) {
    static const int wadOrder [21] =
      { 1, 2, 0, 3, 4, 18, 17, 16, 15, 5, 6, 13, 7, 12, 10, 9, 11, 8, 14, 19, 20 };
    // The constructor stores as romData[_unscramble_address(i)] = raw[i], so
    // this must map a RAW offset to its logical place - the inverse of reading
    // rom[perm(logical)].
    uint32_t low = address & 0x1FFFFF;
    uint32_t out = 0;
    for (uint32_t bit = 0; bit < 21; bit++)
      out |= ((low >> bit) & 1) << wadOrder[bit];
    return (address & ~0x1FFFFFu) | out;
  }
  if (synthGen == ControlRom::SynthGen::JV880)
    return address;

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


uint32_t WaveRom::_jv_phys(uint32_t address,
                           enum ControlRom::SynthGen synthGen)
{
  if (synthGen != ControlRom::SynthGen::JV1080)
    return address;                       // the JV-880 dumps read straight

  static const int wadOrder [21] =
    { 1, 2, 0, 3, 4, 18, 17, 16, 15, 5, 6, 13, 7, 12, 10, 9, 11, 8, 14, 19, 20 };
  uint32_t dev   = address & ~0x1FFFFFu;
  uint32_t local = address &  0x1FFFFFu;
  uint32_t out   = 0;
  for (uint32_t bit = 0; bit < 21; bit++)
    out |= ((local >> wadOrder[bit]) & 1) << bit;
  return dev | out;
}



int8_t WaveRom::_unscramble_data(int8_t byte,
                                 enum ControlRom::SynthGen synthGen)
{
  // The JV wave ROMs carry their data straight; only the address bus is wired
  // through a permutation (service notes p.15, and the data lines are drawn
  // WD7->WD7 down to WD0->WD0).
  if (synthGen == ControlRom::SynthGen::JV880 ||
      synthGen == ControlRom::SynthGen::JV1080)
    return byte;

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
      // The shift exponent lives in the SAME device as the sample. On the
      // SC-55 that is implicit; the JV spans several 2 MB wave ROMs, so the
      // lookup must stay inside the containing one or it reads another
      // device's nibbles.
      uint32_t shiftAddr;
      if (synthGen == ControlRom::SynthGen::JV880 ||
          synthGen == ControlRom::SynthGen::JV1080) {
        uint32_t dev   = sAddress & ~0x1FFFFFu;
        uint32_t local = sAddress &  0x1FFFFFu;
        shiftAddr = dev | ((local & 0xFFFFF) >> 5) | (local & 0x100000);
      } else {
        shiftAddr = ((sAddress & 0xFFFFF) >> 5) | (sAddress & 0xF00000);
      }
      uint8_t sByte   = romData[shiftAddr];
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

  // The JV delta stream drifts: where its encoder resets the integrator is not
  // known (P-0360), so the running sum wanders and a loop join becomes a step.
  // A moving average approximates the drift and is subtracted off - a stand-in
  // for the block structure, not the format. JV only; the SC-55 path is
  // untouched and its output stays bit-identical.
  if (synthGen == ControlRom::SynthGen::JV880 ||
      synthGen == ControlRom::SynthGen::JV1080) {
    const size_t win = 4096;
    const size_t n = s.samplesF.size();
    if (n > win) {
      std::vector<float> smooth(n);
      double run = 0.0;
      for (size_t i = 0; i < n; i++) {
        run += s.samplesF[i];
        if (i >= win) run -= s.samplesF[i - win];
        smooth[i] = (float) (run / (double) (i < win ? i + 1 : win));
      }
      const size_t half = win / 2;
      for (size_t i = 0; i < n; i++)
        s.samplesF[i] -= smooth[i + half < n ? i + half : n - 1];
    }
  }

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
