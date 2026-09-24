/* SPDX-License-Identifier: CC0-1.0 */

/* A device whose SysEx receiver takes Roland messages only
 * (DeviceProfile::rolandSysExOnly, ignoresGsReset): the JV-880.
 *
 * Its firmware drops GM System On, universal Master Volume and the GS Reset
 * (ROM1 0x6EBF, ROM2 0x2F817), so none of them may touch a part. The part's
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
  }

  {
    EmuSC::Synth synth(*ctrlRom, *waveRom, EmuSC::Synth::SoundMap::GS_GM);
    synth.set_audio_format(kRate, 2);
    synth.reset(EmuSC::Synth::SoundMap::GS_GM, true);
    run(synth);
    for (int p = 0; p < 16; p++)
      assert(synth.get_param(EmuSC::PatchParam::RxBankSelect, p) != 0);
  }

  std::cout << "GM System On, GS Reset and Master Volume ignored" << std::endl;

  delete waveRom;
  delete ctrlRom;
  return 0;
}
