/* SPDX-License-Identifier: CC0-1.0 */

/* Synth::reset() on a device that reaches its multitimbral state through its
 * own control channel (PerformanceLayout::resetSelector).
 *
 * The JV-880 powers on in a layered Performance that answers on MIDI channels
 * 1-4 and 10 only, so an ordinary file's channel 5 is silent until something
 * selects a multitimbral Performance. The device's own mechanism for that is a
 * program change on its control channel, and reset() makes the same call, so
 * the proof is audio: a note on channel 5 sounds nothing on a synth that has
 * only been constructed, and sounds after reset(). The first phase is what
 * keeps a reset that changed nothing from passing - it would render silence in
 * both.
 *
 * The real ROM images are needed: ControlRom and WaveRom read files, and
 * neither has a public constructor that takes bytes. Without them the test
 * reports skipped rather than passing while checking nothing.
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
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

const uint32_t kRate = 32000;
const unsigned kWindow = 32000;     /* one second of frames per phase */

/* MIDI channel 5, 0-based 4: unused by the boot Performance's part map
   [1,1,1,2,2,3,4,10] and carrying one part in the Performance a reset
   selects. */
const uint8_t kNoteOnCh5 = 0x94;

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

/* Sum of |sample| over both channels of `frames` frames. */
double render_energy(EmuSC::Synth &synth, unsigned frames)
{
  double energy = 0.0;
  for (unsigned i = 0; i < frames; i++) {
    float l = 0.0f, r = 0.0f;
    synth.get_next_frame(l, r);
    energy += std::fabs(l) + std::fabs(r);
  }
  return energy;
}

}  // namespace

int main(void)
{
  const char *controlPath = getenv("JV880_CONTROL_ROM");
  const char *wavePaths = getenv("JV880_WAVE_ROMS");
  if (!controlPath || !wavePaths)
    return 77;

  std::vector<std::string> waveRoms = split_commas(wavePaths);

  EmuSC::ControlRom *ctrlRom = nullptr;
  EmuSC::WaveRom *waveRom = nullptr;
  try {
    /* The JV-880's second physical chip is never read, so there is no CPU ROM
       argument to give. */
    ctrlRom = new EmuSC::ControlRom(controlPath, "");
    if (ctrlRom->generation() != EmuSC::ControlRom::SynthGen::JV880) {
      std::cerr << "JV880_CONTROL_ROM is not a JV-880 control ROM" << std::endl;
      delete ctrlRom;
      return 77;
    }
    waveRom = new EmuSC::WaveRom(waveRoms, *ctrlRom);
  } catch (const std::string &e) {
    std::cerr << "ROM load failed: " << e << std::endl;
    delete waveRom;
    delete ctrlRom;
    return 77;
  }

  EmuSC::Synth synth(*ctrlRom, *waveRom, EmuSC::Synth::SoundMap::GS);
  synth.set_audio_format(kRate, 2);

  /* Phase 1: the boot Performance. Nothing answers on channel 5. */
  synth.midi_input(kNoteOnCh5, 60, 100);
  double boot = render_energy(synth, kWindow);

  /* Phase 2: the reset selects the Performance the device's own control
     channel would, and the same note now sounds. */
  synth.reset(EmuSC::Synth::SoundMap::GS, true);
  synth.midi_input(kNoteOnCh5, 60, 100);
  double afterReset = render_energy(synth, kWindow);

  std::cout << "channel 5 energy: boot Performance " << boot
            << ", after reset() " << afterReset << std::endl;

  assert(afterReset > 0.0);
  /* Two orders of magnitude apart, not a threshold fitted to one render: the
     boot Performance leaves the channel with no part at all, so its phase is
     the engine's own noise floor. */
  assert(boot * 100.0 < afterReset);

  delete waveRom;
  delete ctrlRom;

  return 0;
}
