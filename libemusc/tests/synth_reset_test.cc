/* SPDX-License-Identifier: CC0-1.0 */

/* Synth::reset() on an SC-88.
 *
 * The SC-88 leaves Synth::_parts empty, so reset()'s part loop reaches none
 * of its state; what it holds lives in the Xp::Device the Synth owns. This
 * exercises the public API only - a caller cannot see that device - so the
 * proof that a reset acted is audio: a part silenced through its own volume
 * controller sounds again after reset(), and the middle phase confirms the
 * silencing took, so a reset that changed nothing cannot pass by rendering
 * silence throughout.
 *
 * The SC-88's real ROM images are needed: ControlRom and WaveRom read files,
 * and neither has a public constructor that takes bytes. Without them the
 * test reports skipped rather than passing while checking nothing.
 *   SC88_CONTROL_ROM  the control ROM
 *   SC88_WAVE_ROMS    the four wave chips, comma separated, in chip order
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
const unsigned kWindow = 8192;      /* frames each energy is measured over */
const unsigned kSettle = 64000;     /* frames given to the reverb tail: 2 s */

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
  const char *controlPath = getenv("SC88_CONTROL_ROM");
  const char *wavePaths = getenv("SC88_WAVE_ROMS");
  if (!controlPath || !wavePaths)
    return 77;

  std::vector<std::string> waveRoms = split_commas(wavePaths);

  EmuSC::ControlRom *ctrlRom = nullptr;
  EmuSC::WaveRom *waveRom = nullptr;
  try {
    /* The SC-88 path in ControlRom keeps the image and returns before it
       reads anything, so it never looks at the CPU ROM argument. */
    ctrlRom = new EmuSC::ControlRom(controlPath, "");
    if (ctrlRom->generation() != EmuSC::ControlRom::SynthGen::SC88) {
      std::cerr << "SC88_CONTROL_ROM is not an SC-88 control ROM" << std::endl;
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

  {
    /* reset() before set_audio_format(): the Synth holds no device yet, and
       the call has to be a no-op rather than a crash. A caller that resets a
       synth it has only just constructed is the ordinary case - both of the
       tools in this tree do it. */
    EmuSC::Synth synth(*ctrlRom, *waveRom, EmuSC::Synth::SoundMap::GS);
    synth.reset(EmuSC::Synth::SoundMap::GS, true);
  }

  EmuSC::Synth synth(*ctrlRom, *waveRom, EmuSC::Synth::SoundMap::GS);
  synth.set_audio_format(kRate, 2);

  /* An SC-88 Synth queues MIDI and applies it as the frames it was handed
     for come round, so every event below is followed by frames. The note is
     never released: what the phases below move is the part's level, and a
     held note keeps something for that level to act on. */
  synth.midi_input(0x90, 60, 100);
  double sounding = render_energy(synth, kWindow);
  assert(sounding > 0.0);

  /* Part volume, which the engine applies to voices already sounding. The
     settling window is the reverb's: a reset leaves the part reverb send at
     40, so the tail of what was just silenced outlives the dry signal by
     seconds and would otherwise be read as the part still sounding. */
  synth.midi_input(0xb0, 7, 0);
  render_energy(synth, kSettle);
  double silenced = render_energy(synth, kWindow);
  assert(silenced < sounding / 1000.0);

  /* The reset under test. Without it reaching the device the part is still
     at volume 0 and the held note stays silent. */
  synth.reset(EmuSC::Synth::SoundMap::GS, true);
  double restored = render_energy(synth, kWindow);
  assert(restored > silenced * 100.0);
  assert(restored > sounding / 100.0);

  /* A second reset changes nothing: the part is already at its default. */
  synth.reset(EmuSC::Synth::SoundMap::GS, true);
  double again = render_energy(synth, kWindow);
  assert(again > silenced * 100.0);

  delete waveRom;
  delete ctrlRom;
  return 0;
}
