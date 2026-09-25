/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * This file consists entirely of AI-generated code without direct human
 * authorship and is dedicated to the public domain under CC0 1.0.
 */

#include "tvf_law.h"
#include "tvf_cents_ratio_law.h"
#include "tvf_indexed_law.h"


namespace EmuSC { namespace Gp {


TvfLaw *TvfLaw::create(const DeviceProfile &device,
                       ControlRom::InstPartial &instPartial, uint8_t key,
                       uint8_t velocity, ControlRom::LookupTables &LUT,
                       TvfPartControls &controls, const int *ctrlAcc,
                       const TvfLfoInputs &lfo, SVF &svf)
{
  switch (device.tvfLawKind) {
  case TvfLawKind::JVCentsRatio:
    if (!CentsRatioTvfLaw::tables_loaded(LUT))
      return nullptr;
    return new CentsRatioTvfLaw(instPartial, key, velocity, LUT,
                                device.tvfJv, ctrlAcc, lfo);

  case TvfLawKind::SoundCanvasIndex:
    break;
  }

  return new IndexedTvfLaw(instPartial, key, velocity, LUT, device.tvfCutoff,
                           controls, lfo, svf);
}

}}  // namespace EmuSC::Gp
