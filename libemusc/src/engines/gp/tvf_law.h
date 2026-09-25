/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * This file consists entirely of AI-generated code without direct human
 * authorship and is dedicated to the public domain under CC0 1.0.
 *
 *  The interface between TVF and the arithmetic a device's filter chain uses.
 *
 *  The devices this engine serves build their filter control in two ways that
 *  differ in kind, not in constants (TvfLawKind, device_profile.h): one
 *  assembles a cutoff-table INDEX and looks the coefficient up, the other
 *  accumulates modulation in CENTS and multiplies a base coefficient by its
 *  exponential. Each is one TvfLaw. TVF owns the filter core and picks the law
 *  once, when the note starts; from then on it only forwards to it.
 *
 *  A law is also its own envelope, because the two chains do not share one:
 *  their segment counts, rate arithmetic and tick periods all differ.
 */


#ifndef __TVF_LAW_H__
#define __TVF_LAW_H__


#include "envelope.h"
#include "svf.h"
#include "../../control_rom.h"
#include "../../device_profile.h"
#include "../../params.h"
#include "../../settings.h"

#include <array>
#include <cstdint>


namespace EmuSC { namespace Gp {


// One LFO as a filter law reads it in one control period. Every field is a
// plain read of the LFO's own state, taken by TVF before the law runs; a law
// never steps an LFO.
struct TvfLfoState
{
  int16_t value;     // WaveGenerator::value()
  int     fade;      // WaveGenerator::fade()
  int     raw;       // WaveGenerator::jv_raw()
};

struct TvfLfoInputs
{
  TvfLfoState lfo1;
  TvfLfoState lfo2;
};


// The part parameters a filter law reads while the note plays. They are read
// live, at the moment the law needs them - a phase change at note off reads
// the release time as it stands at note off - so they cannot be handed over as
// a snapshot.
class TvfPartControls
{
public:
  virtual ~TvfPartControls() = default;

  virtual uint8_t patch_param(enum PatchParam pp) = 0;
  virtual int controller(enum Settings::ControllerParam cp) = 0;
};


class TvfLaw : public Envelope
{
public:
  explicit TvfLaw(ControlRom::LookupTables &LUT) : Envelope(LUT) {}
  ~TvfLaw() override = default;

  // The law device's TvfLawKind names, started on this note: its first
  // coefficient is computed here, and svf is left set up for it. Null when LUT
  // lacks the tables that law reads, and the filter is then to stay disabled.
  // ctrlAcc is the per-tone controller matrix's destination accumulators
  // (scdb D-79), or null where the tone has none.
  static TvfLaw *create(const DeviceProfile &device,
                        ControlRom::InstPartial &instPartial, uint8_t key,
                        uint8_t velocity, ControlRom::LookupTables &LUT,
                        TvfPartControls &controls, const int *ctrlAcc,
                        const TvfLfoInputs &lfo, SVF &svf);

  // Once per control period, with the LFOs as they stand in that period.
  virtual void update(const TvfLfoInputs &lfo, SVF &svf) = 0;

  // Runs one control period's 256 samples through the filter, in place.
  virtual void apply_sample_set(SVF &svf, std::array<float, 256> &dryBus) = 0;

  void note_off(uint8_t releaseVelocity)
  {
    set_jv_release_velocity(releaseVelocity);
    set_phase(Phase::Release);
  }

  enum Phase phase(void) const { return _phase; }

protected:
  TvfLfoInputs _lfo = {};
};

}}  // namespace EmuSC::Gp

#endif  // __TVF_LAW_H__
