/* SPDX-License-Identifier: CC0-1.0 */

/* A device whose SysEx receiver takes Roland messages only
 * (DeviceProfile::rolandSysExOnly, gsScaleTuningOnly): the JV-880.
 *
 * Its firmware drops GM System On, universal Master Volume and the GS Reset
 * (ROM1 0x6EBF, ROM2 0x2F817), so none of them may touch a part. Of the GS
 * DT1 map it acts on Scale Tuning alone (ROM2 0x2F7DE): 40 0x/1x 40 with
 * x = 0-7 and 12 data bytes, landing on part ((x - 1) & 7). The part's
 * bank flag is the sensitive state: CC0 81 sets it, it persists across
 * program changes, and a reset would put the part back on the reset
 * Performance's patch and - through the GM path - switch Rx Bank Select off.
 * Each message below is sent between two program changes whose outcome
 * depends on that flag surviving.
 *
 * A GS_GM host reset must leave Rx Bank Select on as well: the device has no
 * GM mode to enter.
 *
 *   JV880_CONTROL_ROM  the control ROM
 *   JV880_WAVE_ROMS    the two wave chips, comma separated, in chip order
 */

#include "synth.h"
#include "control_rom.h"
#include "wave_rom.h"

#include <cassert>
#ifdef NDEBUG
#error "this test is assertion-driven; NDEBUG compiles it away"
#endif
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

const uint32_t kRate = 32000;

std::vector<std::string> split_commas(const std::string &s)
{
  std::vector<std::string> out;
  std::string::size_type start = 0;
  while (start <= s.size()) {
    std::string::size_type comma = s.find(',', start);
    if (comma == std::string::npos) {
      out.push_back(s.substr(start));
      break;
    }
    out.push_back(s.substr(start, comma - start));
    start = comma + 1;
  }
  return out;
}

/* Events are queued and applied as frames are rendered. */
void run(EmuSC::Synth &synth, unsigned frames = kRate / 10)
{
  float l, r;
  for (unsigned i = 0; i < frames; i++)
    synth.get_next_frame(l, r);
}

void sysex(EmuSC::Synth &synth, std::vector<uint8_t> msg)
{
  synth.midi_input_sysex(msg.data(), (uint16_t) msg.size());
  run(synth);
}

void program(EmuSC::Synth &synth, uint8_t pc)
{
  synth.midi_input(0xc0, pc, 0);
  run(synth);
}

/* A GS (model 0x42) DT1 to address a0 a1 a2, checksum appended. */
std::vector<uint8_t> gs_dt1(uint8_t a0, uint8_t a1, uint8_t a2,
                            const std::vector<uint8_t> &data)
{
  std::vector<uint8_t> m = { 0xf0, 0x41, 0x10, 0x42, 0x12, a0, a1, a2 };
  int sum = a0 + a1 + a2;
  for (uint8_t d : data) {
    m.push_back(d);
    sum += d;
  }
  m.push_back((uint8_t) ((128 - (sum & 0x7f)) & 0x7f));
  m.push_back(0xf7);
  return m;
}

uint8_t scale(EmuSC::Synth &s, int part, int note)
{
  return s.get_param((EmuSC::PatchParam)
                     ((int) EmuSC::PatchParam::ScaleTuningC + note), part);
}

bool scale_is(EmuSC::Synth &s, int part, const std::vector<uint8_t> &v)
{
  for (int n = 0; n < 12; n++)
    if (scale(s, part, n) != v[n])
      return false;
  return true;
}

uint8_t bank(EmuSC::Synth &s)
{ return s.get_param(EmuSC::PatchParam::ToneNumber, 0); }

uint8_t prog(EmuSC::Synth &s)
{ return s.get_param(EmuSC::PatchParam::ToneNumber2, 0); }

}  // namespace

int main(void)
{
  const char *controlPath = getenv("JV880_CONTROL_ROM");
  const char *wavePaths = getenv("JV880_WAVE_ROMS");
  if (!controlPath || !wavePaths)
    return 77;

  EmuSC::ControlRom *ctrlRom = nullptr;
  EmuSC::WaveRom *waveRom = nullptr;
  try {
    ctrlRom = new EmuSC::ControlRom(controlPath, "");
    if (ctrlRom->generation() != EmuSC::ControlRom::SynthGen::JV880) {
      std::cerr << "JV880_CONTROL_ROM is not a JV-880 control ROM" << std::endl;
      delete ctrlRom;
      return 77;
    }
    waveRom = new EmuSC::WaveRom(split_commas(wavePaths), *ctrlRom);
  } catch (const std::string &e) {
    std::cerr << "ROM load failed: " << e << std::endl;
    delete waveRom;
    delete ctrlRom;
    return 77;
  }

  const std::vector<uint8_t> gmOn = { 0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7 };
  const std::vector<uint8_t> gsReset =
    { 0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7 };
  const std::vector<uint8_t> masterVolume =
    { 0xf0, 0x7f, 0x7f, 0x04, 0x01, 0x00, 0x10, 0xf7 };

  {
    EmuSC::Synth synth(*ctrlRom, *waveRom, EmuSC::Synth::SoundMap::GS);
    synth.set_audio_format(kRate, 2);
    synth.reset(EmuSC::Synth::SoundMap::GS, true);
    run(synth);

    /* CC0 81 + PC 24: Preset A 24. */
    synth.midi_input(0xb0, 0, 81);
    program(synth, 24);
    const uint8_t presets = bank(synth);
    assert(prog(synth) == 24);

    /* GM System On changes nothing, not even the receive switch. */
    sysex(synth, gmOn);
    assert(bank(synth) == presets && prog(synth) == 24);
    assert(synth.get_param(EmuSC::PatchParam::RxBankSelect, 0) != 0);

    /* The flag survived: a bare PC 0 is Preset A 0, PC 64 Preset B 0. */
    program(synth, 0);
    assert(bank(synth) == presets && prog(synth) == 0);
    sysex(synth, gmOn);
    program(synth, 64);
    assert(bank(synth) == presets && prog(synth) == 64);

    /* Nor does the GS Reset. */
    program(synth, 5);
    sysex(synth, gsReset);
    assert(bank(synth) == presets && prog(synth) == 5);

    /* Nor universal Master Volume. */
    const uint8_t volume = synth.get_param(EmuSC::SystemParam::Volume);
    sysex(synth, masterVolume);
    assert(synth.get_param(EmuSC::SystemParam::Volume) == volume);

    /* No GS DT1 but Scale Tuning acts: not master volume, reverb macro,
       chorus level or a part's level. */
    sysex(synth, gs_dt1(0x40, 0x00, 0x04, { (uint8_t) (volume ^ 0x40) }));
    assert(synth.get_param(EmuSC::SystemParam::Volume) == volume);

    const uint8_t revMacro = synth.get_param(EmuSC::PatchParam::ReverbMacro);
    const uint8_t revChar = synth.get_param(EmuSC::PatchParam::ReverbCharacter);
    sysex(synth, gs_dt1(0x40, 0x01, 0x30, { (uint8_t) (revChar ^ 0x07) }));
    assert(synth.get_param(EmuSC::PatchParam::ReverbMacro) == revMacro);
    assert(synth.get_param(EmuSC::PatchParam::ReverbCharacter) == revChar);

    const uint8_t chorus = synth.get_param(EmuSC::PatchParam::ChorusLevel);
    sysex(synth, gs_dt1(0x40, 0x01, 0x3a, { (uint8_t) (chorus ^ 0x40) }));
    assert(synth.get_param(EmuSC::PatchParam::ChorusLevel) == chorus);

    const uint8_t level = synth.get_param(EmuSC::PatchParam::PartLevel, 0);
    sysex(synth, gs_dt1(0x40, 0x11, 0x19, { (uint8_t) (level ^ 0x40) }));
    assert(synth.get_param(EmuSC::PatchParam::PartLevel, 0) == level);

    /* Scale Tuning, 40 1x 40: x = 1 is part 1. */
    const std::vector<uint8_t> flat(12, 0x40);
    const std::vector<uint8_t> tuneA =
      { 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b };
    const std::vector<uint8_t> tuneB =
      { 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b };
    for (int p = 0; p < 8; p++)
      assert(scale_is(synth, p, flat));
    sysex(synth, gs_dt1(0x40, 0x11, 0x40, tuneA));
    assert(scale_is(synth, 0, tuneA));
    for (int p = 1; p < 8; p++)
      assert(scale_is(synth, p, flat));

    /* 40 0x 40 is accepted too, and x = 0 is the eighth part, not part 10. */
    sysex(synth, gs_dt1(0x40, 0x00, 0x40, tuneB));
    assert(scale_is(synth, 7, tuneB));
    assert(scale_is(synth, 9, flat));

    /* x = 8-F fails the firmware's 0xE8 mask; so does a wrong length. */
    sysex(synth, gs_dt1(0x40, 0x18, 0x40, tuneA));
    sysex(synth, gs_dt1(0x40, 0x1a, 0x40, tuneA));
    assert(scale_is(synth, 7, tuneB));
    assert(scale_is(synth, 9, flat));
    sysex(synth, gs_dt1(0x40, 0x12, 0x40,
                        std::vector<uint8_t>(tuneA.begin(), tuneA.end() - 1)));
    assert(scale_is(synth, 1, flat));
  }

  {
    EmuSC::Synth synth(*ctrlRom, *waveRom, EmuSC::Synth::SoundMap::GS_GM);
    synth.set_audio_format(kRate, 2);
    synth.reset(EmuSC::Synth::SoundMap::GS_GM, true);
    run(synth);
    for (int p = 0; p < 16; p++)
      assert(synth.get_param(EmuSC::PatchParam::RxBankSelect, p) != 0);
  }

  std::cout << "GM System On, GS Reset, Master Volume and non-Scale-Tuning "
               "GS DT1 ignored" << std::endl;

  delete waveRom;
  delete ctrlRom;
  return 0;
}
