/* SPDX-License-Identifier: CC0-1.0 */

/* The GP engine's two filter laws, driven directly: no ROM, no Settings, no
 * WaveGenerator. Each test builds the lookup tables it needs - the cents-ratio
 * law's from the closed forms engines/gp/devices/jv880.cc documents - and the
 * expected values are worked out by hand from those tables, not recomputed
 * through the law's own code.
 *
 * Where a coefficient can only be seen through the filter, the law's output
 * is compared sample for sample against a bare SVF driven with the
 * coefficients the law should have chosen.
 */
#include "engines/gp/tvf_law.h"
#include "engines/gp/tvf_cents_ratio_law.h"
#include "engines/gp/tvf_indexed_law.h"
#include "engines/gp/ctrl_matrix.h"
#include "device_profile.h"

#include <cassert>
#ifdef NDEBUG
#error "this test is assertion-driven; NDEBUG compiles it away"
#endif
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

using namespace EmuSC;
using namespace EmuSC::Gp;

namespace {

typedef std::array<float, 256> Block;
typedef Envelope::Phase Phase;


// Part parameters at their neutral values unless a test sets them.
class FakeControls : public TvfPartControls
{
public:
  std::map<PatchParam, uint8_t> params;
  std::map<Settings::ControllerParam, int> controllers;

  uint8_t patch_param(enum PatchParam pp) override
  {
    auto it = params.find(pp);
    return (it == params.end()) ? 0x40 : it->second;
  }
  int controller(enum Settings::ControllerParam cp) override
  {
    auto it = controllers.find(cp);
    return (it == controllers.end()) ? 0 : it->second;
  }
};


TvfLfoInputs still_lfos(void)
{
  TvfLfoInputs in = {};
  in.lfo1.fade = 0xffff;
  in.lfo2.fade = 0xffff;
  return in;
}


Block impulse(void)
{
  Block b = {};
  b[0] = 1.0f;
  b[17] = -0.5f;
  b[100] = 0.25f;
  return b;
}


bool same_bits(const Block &a, const Block &b)
{
  return std::memcmp(a.data(), b.data(), sizeof(float) * a.size()) == 0;
}


// ---------------------------------------------------------------------------
// The indexed law's tables. Cutoff index i reads coefficient 256 * i, so a
// cutoff index is visible directly in the level the law settles on (twice the
// coefficient); every other table is neutral.

std::unique_ptr<ControlRom::LookupTables> indexed_tables(void)
{
  std::unique_ptr<ControlRom::LookupTables> t(new ControlRom::LookupTables{});

  t->VelocityCurves.resize(128);
  for (int i = 0; i < 128; i++)
    t->VelocityCurves[i] = (uint8_t) i;

  // Key follow reads a 16-bit big-endian word; 0x4000 is its centre.
  t->KeyMapperOffset = 0;
  t->KeyMapperIndex.fill(0);
  t->KeyMapper.assign(512, 0);
  for (size_t i = 0; i + 1 < t->KeyMapper.size(); i += 2)
    t->KeyMapper[i] = 0x40;

  t->EnvTimeScale.fill(256);
  for (int i = 0; i < 129; i++)
    t->TVFCutoffFreq[i] = 256 * i;
  t->TVFResonanceFreq.fill(0x7f);
  t->TVFResonance.fill(0xff);
  t->EnvSegmentCurve.fill(8);
  t->EnvSegmentStep.fill(0x10);
  t->TVFEnvScale.fill(0x80);
  t->TVFEnvScale[0] = 0;
  return t;
}


ControlRom::InstPartial indexed_partial(int base, int resonance)
{
  ControlRom::InstPartial ip = {};
  ip.TVFType = 0;
  ip.TVFBaseFlt = (int8_t) base;
  ip.TVFResonance = (int8_t) resonance;
  ip.TVFCFKeyFlw = 0x40;
  ip.TVFCOFVSens = 0x40;
  ip.TVFEnvL1 = ip.TVFEnvL2 = ip.TVFEnvL3 = ip.TVFEnvL4 = ip.TVFEnvL5 = 0x40;
  ip.TVFETKeyF14 = ip.TVFETKeyF5 = 0x40;
  ip.TVFETVSens12 = ip.TVFETVSens35 = 0x40;
  return ip;
}


// A still partial settles on twice its base cutoff's coefficient, and the
// filter runs at exactly that coefficient and the partial's resonance.
void test_indexed_static_cutoff(void)
{
  auto lut = indexed_tables();
  ControlRom::InstPartial ip = indexed_partial(60, 0x20);
  FakeControls ctl;
  SVF svf(SVF::Mode::LowPass);
  IndexedTvfLaw law(ip, 60, 100, *lut, SC55MKII_PROFILE.tvfCutoff, ctl,
                    still_lfos(), svf);

  assert(law.cutoff_level() == 2 * 60 * 256);
  assert(law.resonance() == 0x20);
  assert(law.get_envelope_value() == 60);

  Block out = impulse(), want = impulse();
  law.apply_sample_set(svf, out);

  SVF ref(SVF::Mode::LowPass);
  ref.set_resonance(0x20);
  for (int i = 0; i < 256; i++) {
    ref.set_cutoff_freq(2 * 60 * 256);
    want[i] = ref.process_sample(want[i]);
  }
  assert(same_bits(out, want));

  // Nothing moves, so nothing changes.
  for (int i = 0; i < 20; i++)
    law.update(still_lfos(), svf);
  assert(law.cutoff_level() == 2 * 60 * 256);
  assert(law.phase() == Phase::Sustain);
}


// The part's TVF Cutoff Frequency parameter, through each generation's own
// TvfCutoffLaw: a positive offset raises the cutoff on the mkII and does
// nothing on the mk1, a negative one lowers both, and the parameter is
// clamped to each device's range (PROVENANCE.md P-0133).
int indexed_level_with_param(const TvfCutoffLaw &cutoffLaw, int param,
                             int base = 60)
{
  auto lut = indexed_tables();
  ControlRom::InstPartial ip = indexed_partial(base, 0x20);
  FakeControls ctl;
  ctl.params[PatchParam::TVFCutoffFreq] = (uint8_t) param;
  SVF svf(SVF::Mode::LowPass);
  IndexedTvfLaw law(ip, 60, 100, *lut, cutoffLaw, ctl, still_lfos(), svf);
  return law.cutoff_level();
}

void test_indexed_cutoff_parameter(void)
{
  const TvfCutoffLaw &mk1 = SC55_PROFILE.tvfCutoff;
  const TvfCutoffLaw &mk2 = SC55MKII_PROFILE.tvfCutoff;

  assert(indexed_level_with_param(mk2, 0x40 + 10) == 2 * 70 * 256);
  assert(indexed_level_with_param(mk1, 0x40 + 10) == 2 * 60 * 256);

  assert(indexed_level_with_param(mk2, 0x40 - 10) == 2 * 50 * 256);
  assert(indexed_level_with_param(mk1, 0x40 - 10) == 2 * 50 * 256);

  // The floor is the parameter value 0x0e on both: 0x40 - 50.
  assert(indexed_level_with_param(mk2, 0x00) == 2 * 10 * 256);
  assert(indexed_level_with_param(mk1, 0x00) == 2 * 10 * 256);

  // At the top of the range the mkII raises by the full +63; the mk1 still
  // does not raise at all.
  assert(indexed_level_with_param(mk2, 0x7f, 20) == 2 * 83 * 256);
  assert(indexed_level_with_param(mk1, 0x7f, 20) == 2 * 20 * 256);
}


// The parameter is read live: a change during the note moves the cutoff at
// the next control period, not only at the next note.
void test_indexed_parameter_is_live(void)
{
  auto lut = indexed_tables();
  ControlRom::InstPartial ip = indexed_partial(60, 0x20);
  FakeControls ctl;
  SVF svf(SVF::Mode::LowPass);
  IndexedTvfLaw law(ip, 60, 100, *lut, SC55MKII_PROFILE.tvfCutoff, ctl,
                    still_lfos(), svf);
  for (int i = 0; i < 10; i++)
    law.update(still_lfos(), svf);
  assert(law.cutoff_level() == 2 * 60 * 256);

  ctl.params[PatchParam::TVFCutoffFreq] = 0x40 - 20;
  law.update(still_lfos(), svf);
  assert(law.cutoff_level() == 2 * 40 * 256);
}


// The cutoff controller adds to the index below the table step and the law
// interpolates between entries; it adds its MAGNITUDE, so a negative value
// raises the cutoff too.
void test_indexed_interpolation(void)
{
  for (int sign : { +1, -1 }) {
    auto lut = indexed_tables();
    ControlRom::InstPartial ip = indexed_partial(60, 0x20);
    FakeControls ctl;
    ctl.controllers[Settings::ControllerParam::TVFCutoff] = sign * 0x80;
    SVF svf(SVF::Mode::LowPass);
    IndexedTvfLaw law(ip, 60, 100, *lut, SC55MKII_PROFILE.tvfCutoff, ctl,
                      still_lfos(), svf);
    // Half-way from entry 60 (15360) to entry 61 (15616), doubled.
    assert(law.cutoff_level() == 2 * 15488);
  }
}


// Two ceilings: the coefficient 0xe600, and the resonance table's cap.
void test_indexed_ceilings(void)
{
  {
    auto lut = indexed_tables();
    ControlRom::InstPartial ip = indexed_partial(127, 0x20);
    FakeControls ctl;
    SVF svf(SVF::Mode::LowPass);
    IndexedTvfLaw law(ip, 60, 100, *lut, SC55MKII_PROFILE.tvfCutoff, ctl,
                      still_lfos(), svf);
    assert(law.cutoff_level() == 0xe600);
  }
  {
    auto lut = indexed_tables();
    lut->TVFResonance.fill(0x40);
    ControlRom::InstPartial ip = indexed_partial(60, 0x20);
    FakeControls ctl;
    SVF svf(SVF::Mode::LowPass);
    IndexedTvfLaw law(ip, 60, 100, *lut, SC55MKII_PROFILE.tvfCutoff, ctl,
                      still_lfos(), svf);
    assert(law.cutoff_level() == 0x4000);
  }
}


// Resonance moves one step per control period towards what the part asks for,
// and never goes below 8.
void test_indexed_resonance(void)
{
  auto lut = indexed_tables();
  ControlRom::InstPartial ip = indexed_partial(60, 20);
  FakeControls ctl;
  SVF svf(SVF::Mode::LowPass);
  IndexedTvfLaw law(ip, 60, 100, *lut, SC55MKII_PROFILE.tvfCutoff, ctl,
                    still_lfos(), svf);
  assert(law.resonance() == 20);

  // Each step down of the parameter asks for two more.
  ctl.params[PatchParam::TVFResonance] = 0x40 - 5;
  for (int n = 1; n <= 12; n++) {
    law.update(still_lfos(), svf);
    assert(law.resonance() == std::min(20 + n, 30));
  }

  ControlRom::InstPartial flat = indexed_partial(60, 0);
  FakeControls ctl0;
  IndexedTvfLaw floor(flat, 60, 100, *lut, SC55MKII_PROFILE.tvfCutoff, ctl0,
                      still_lfos(), svf);
  assert(floor.resonance() == 8);
}


// The envelope level's side of 0x40 decides the direction: above it raises
// the cutoff by the depth, below it lowers it by the same amount.
void test_indexed_envelope_direction(void)
{
  auto lut = indexed_tables();
  lut->TVFEnvDepth[1] = 0x100;
  FakeControls ctl;
  SVF svf(SVF::Mode::LowPass);

  // depth = 0x100 * 0x7fff * 2, whose high word 0xff scaled by 0x80 and
  // doubled is 0xff00, i.e. 255 cutoff units.
  ControlRom::InstPartial up = indexed_partial(60, 0x20);
  up.TVFEnvDepth = 1;
  up.TVFEnvL1 = 0x7f;
  IndexedTvfLaw rising(up, 60, 100, *lut, SC55MKII_PROFILE.tvfCutoff, ctl,
                       still_lfos(), svf);
  // 15360 + 255 units along the 256-unit step to entry 61, doubled.
  assert(rising.cutoff_level() == 2 * (15360 + 255));

  ControlRom::InstPartial down = indexed_partial(60, 0x20);
  down.TVFEnvDepth = 1;
  down.TVFEnvL1 = 0x00;
  IndexedTvfLaw falling(down, 60, 100, *lut, SC55MKII_PROFILE.tvfCutoff, ctl,
                        still_lfos(), svf);
  // 255 units below entry 60: entry 59 plus 1.
  assert(falling.cutoff_level() == 2 * (15104 + 1));
}


// A cutoff change during the note is walked across the control period in a
// line that lands exactly on the target, rather than stepped at its start.
void test_indexed_ramp(void)
{
  auto lut = indexed_tables();
  ControlRom::InstPartial ip = indexed_partial(60, 0x20);
  FakeControls ctl;
  SVF svf(SVF::Mode::LowPass);
  IndexedTvfLaw law(ip, 60, 100, *lut, SC55MKII_PROFILE.tvfCutoff, ctl,
                    still_lfos(), svf);
  SVF ref(SVF::Mode::LowPass);
  ref.set_resonance(0x20);

  Block out, want;
  for (int i = 0; i < 6; i++) {
    law.update(still_lfos(), svf);
    out = impulse();
    want = impulse();
    law.apply_sample_set(svf, out);
    for (int s = 0; s < 256; s++) {
      ref.set_cutoff_freq(2 * 60 * 256);
      want[s] = ref.process_sample(want[s]);
    }
    assert(same_bits(out, want));
  }
  assert(law.phase() == Phase::Sustain);

  ctl.controllers[Settings::ControllerParam::TVFCutoff] = 0x200;
  law.update(still_lfos(), svf);
  const int from = 2 * 60 * 256, to = 2 * 62 * 256;
  assert(law.cutoff_level() == to);
  // A speed that is neither "unchanged" nor "land at once".
  assert((law.cutoff_mode() & 0xff) == 0x10);

  out = impulse();
  want = impulse();
  law.apply_sample_set(svf, out);

  SVF stepped = ref;
  Block step = impulse();
  float level = from;
  for (int s = 0; s < 256; s++) {
    level += (to - from) / 256.0f;
    ref.set_cutoff_freq(s == 255 ? to : (int) level);
    want[s] = ref.process_sample(want[s]);
    stepped.set_cutoff_freq(to);
    step[s] = stepped.process_sample(step[s]);
  }
  assert(same_bits(out, want));
  assert(!same_bits(out, step));
}


// ---------------------------------------------------------------------------
// The cents-ratio law's tables, from the closed forms in
// engines/gp/devices/jv880.cc. Base follows its documented 0x100 * (c + 1)
// all the way up rather than the ROM's curve above 31, which has no closed
// form, and ends on the ROM's 0xffff. LIMIT is left out of the way (0xffff)
// except where a test sets it.

std::unique_ptr<ControlRom::LookupTables> cents_tables(void)
{
  std::unique_ptr<ControlRom::LookupTables> t(new ControlRom::LookupTables{});

  for (int i = 0; i < 256; i++) {
    const int s = (int) (int8_t) i;
    t->JVTvfExpCoarse[i] = (int) std::lround(256.0 * std::exp2(256.0 * s / 1200.0));
    t->JVTvfExpFine[i] = (int) std::lround(65536.0 * (std::exp2(i / 1200.0) - 1.0));
  }
  for (int r = 0; r < 128; r++) {
    t->JVTvfDampSoft[r] = (int) std::floor(16384.0 * std::exp2(-r / 64.0));
    t->JVTvfDampHard[r] = (int) std::floor(16384.0 * std::exp2(-r / 32.0));
    t->JVTvfLimitSoft[r] = 0xffff;
    t->JVTvfLimitHard[r] = 0xffff;
    t->JVTvfBase[r] = 0x100 * (r + 1);
  }
  t->JVTvfBase[127] = 0xffff;

  const int kf[16] = { -100, -70, -50, -30, -10, 0, 10, 20, 30, 40, 50, 70,
                       100, 120, 150, 200 };
  for (int i = 0; i < 16; i++)
    t->JVTvfCutoffKF[i] = kf[i];

  for (int i = 0; i < 128; i++)
    t->envelopeTime[i] = 16 * i;             // one 16 ms tick per time step
  return t;
}


const int KF_OFF = 5;                        // 0 cents per semitone
const int KF_FULL = 12;                      // +100 cents per semitone

ControlRom::InstPartial cents_partial(int cutoff, int resonance)
{
  ControlRom::InstPartial ip = {};
  ip.TVFType = 0;
  ip.TVFBaseFlt = (int8_t) cutoff;
  ip.TVFResonance = (int8_t) resonance;
  ip.TVFCOFKeyFlwIdx = KF_OFF;
  ip.TVFEnvL1 = ip.TVFEnvL2 = ip.TVFEnvL3 = ip.TVFEnvL5 = 0;
  return ip;
}


const TvfJvLaw &cents_law(void) { return JV880_PROFILE.tvfJv; }


void run_ticks(TvfLaw &law, SVF &svf, int ticks,
               const TvfLfoInputs &lfo = still_lfos())
{
  for (int i = 0; i < ticks * cents_law().envTickPeriods; i++)
    law.update(lfo, svf);
}


// With nothing modulating it, the word is the base coefficient; at resonance
// 0 the damping is the zero-resonance rule's 0x4a48 - word/8, whose high byte
// is what reaches the filter, and the filter runs at exactly those values.
void test_cents_static(void)
{
  auto lut = cents_tables();
  ControlRom::InstPartial ip = cents_partial(31, 0);
  SVF svf(SVF::Mode::LowPass);
  CentsRatioTvfLaw law(ip, 60, 100, *lut, cents_law(), nullptr, still_lfos());

  assert(law.target_word() == 0x2000);
  assert(law.cutoff_word() == 0x2000);
  assert(law.resonance() == 0);
  assert(law.damping() == (float) 0x4600 / 0x4000);

  Block out = impulse(), want = impulse();
  law.apply_sample_set(svf, out);
  SVF ref(SVF::Mode::LowPass);
  for (int i = 0; i < 256; i++) {
    ref.set_coefficients(0x2000 / 32768.0f, (float) 0x4600 / 0x4000);
    want[i] = ref.process_sample(want[i]);
  }
  assert(same_bits(out, want));
}


// Key follow is in cents: an octave up doubles the coefficient exactly, an
// octave down halves it to within the tables' rounding, and a semitone up
// multiplies it by 2^(1/12).
void test_cents_ratio(void)
{
  auto lut = cents_tables();
  ControlRom::InstPartial ip = cents_partial(31, 0);
  ip.TVFCOFKeyFlwIdx = KF_FULL;

  CentsRatioTvfLaw up(ip, 72, 100, *lut, cents_law(), nullptr, still_lfos());
  assert(up.target_word() == 0x4000);

  CentsRatioTvfLaw down(ip, 48, 100, *lut, cents_law(), nullptr, still_lfos());
  const double half = down.target_word() / (double) 0x2000;
  assert(half > 0.49 && half < 0.51);

  // x = 100: E = 256 + (256 * 3897 >> 16) = 271, and 271 * 0x2000 >> 8.
  CentsRatioTvfLaw semi(ip, 61, 100, *lut, cents_law(), nullptr, still_lfos());
  assert(semi.target_word() == 0x21e0);
}


// The chip is told to stop at the target's high byte. A note starts it at
// zero, so a rising word settles on its high byte; a falling word is tracked
// to the full 16 bits; and a target that does not change is not sent again.
void test_cents_chip_stop(void)
{
  auto lut = cents_tables();
  ControlRom::InstPartial ip = cents_partial(48, 0);
  ip.TVFResonance = 1;                       // out of the zero-res cap
  ip.TVFCOFKeyFlwIdx = KF_FULL;
  int acc[JV_CTRL_DESTS] = {};
  SVF svf(SVF::Mode::LowPass);

  // Key 61: 271 * 0x3100 >> 8 = 0x33df.
  CentsRatioTvfLaw law(ip, 61, 100, *lut, cents_law(), acc, still_lfos());
  assert(law.target_word() == 0x33df);
  assert(law.cutoff_word() == 0x3300);

  // One cutoff step down through the controller matrix: 271 * 0x3000 >> 8.
  acc[(int) JvCtrlDest::Cutoff] = -0x100;
  law.update(still_lfos(), svf);             // half a tick: nothing yet
  assert(law.target_word() == 0x33df);
  law.update(still_lfos(), svf);
  assert(law.target_word() == 0x32d0);
  assert(law.cutoff_word() == 0x32d0);

  // Across the tick the coefficient walks from 0x3300 to 0x32d0, one line
  // over two control periods.
  SVF ref = svf;
  for (int half = 0; half < 2; half++) {
    Block out = impulse(), want = impulse();
    law.apply_sample_set(svf, out);
    for (int i = 0; i < 256; i++) {
      const float t = (256 * half + i + 1) / 512.0f;
      const float f1 = std::max(0x3300 / 32768.0f + (-0x30 / 32768.0f) * t,
                                0x3200 / 32768.0f);
      ref.set_coefficients(f1, law.damping());
      want[i] = ref.process_sample(want[i]);
    }
    assert(same_bits(out, want));
  }

  // And back up: the stop bites again.
  acc[(int) JvCtrlDest::Cutoff] = 0;
  run_ticks(law, svf, 1);
  assert(law.target_word() == 0x33df);
  assert(law.cutoff_word() == 0x3300);

  run_ticks(law, svf, 3);
  assert(law.cutoff_word() == 0x3300);
}


// Resonance slews by the profile's step per tick, starts on its target with
// no glide, and reads the SOFT or HARD rows as the tone asks; LIMIT caps the
// word at the resonance reached.
void test_cents_resonance(void)
{
  auto lut = cents_tables();
  lut->JVTvfLimitSoft[110] = 0x1000;
  int acc[JV_CTRL_DESTS] = {};
  SVF svf(SVF::Mode::LowPass);

  ControlRom::InstPartial ip = cents_partial(31, 10);
  CentsRatioTvfLaw law(ip, 60, 100, *lut, cents_law(), acc, still_lfos());
  assert(law.resonance() == 10);
  assert(law.target_word() == 0x2000);

  acc[(int) JvCtrlDest::Resonance] = 100 << 8;
  const int expect[] = { 26, 42, 58, 74, 90, 106, 110, 110 };
  for (int r : expect) {
    run_ticks(law, svf, 1);
    assert(law.resonance() == r);
    assert(law.damping() ==
           (float) (lut->JVTvfDampSoft[r] & 0xff00) / 0x4000);
  }
  assert(law.target_word() == 0x1000);

  // A note that starts with the controller already there starts on it.
  CentsRatioTvfLaw preset(ip, 60, 100, *lut, cents_law(), acc, still_lfos());
  assert(preset.resonance() == 110);

  ControlRom::InstPartial hard = cents_partial(31, 40);
  hard.TVFResoMode = 1;
  CentsRatioTvfLaw h(hard, 60, 100, *lut, cents_law(), nullptr, still_lfos());
  assert(h.damping() == (float) (lut->JVTvfDampHard[40] & 0xff00) / 0x4000);
  assert(h.damping() != (float) (lut->JVTvfDampSoft[40] & 0xff00) / 0x4000);
}


// The envelope steps once per 16 ms tick, a segment of D ms lasts D/16 ticks,
// and a time of 0 skips its segment. The release walks to L5.
void test_cents_envelope(void)
{
  auto lut = cents_tables();
  ControlRom::InstPartial ip = cents_partial(31, 0);
  ip.TVFEnvL1 = 127; ip.TVFEnvT1 = 0;        // skipped: starts at L1
  ip.TVFEnvL2 = 0;   ip.TVFEnvT2 = 10;       // 160 ms, 10 ticks
  ip.TVFEnvL3 = 64;  ip.TVFEnvT3 = 0;
  ip.TVFEnvL5 = 20;  ip.TVFEnvT5 = 0;
  SVF svf(SVF::Mode::LowPass);
  CentsRatioTvfLaw law(ip, 60, 100, *lut, cents_law(), nullptr, still_lfos());
  assert(law.get_envelope_value() == 127);
  assert(law.phase() == Phase::Attack2);

  // 5 ticks at 2^20 / 160 per tick: 32765 of 65535, half way down.
  run_ticks(law, svf, 5);
  assert(law.get_envelope_value() == 63);

  run_ticks(law, svf, 5);
  assert(law.phase() == Phase::Attack2);
  run_ticks(law, svf, 1);
  assert(law.phase() == Phase::Sustain);
  assert(law.get_envelope_value() == 64);

  law.note_off(64);
  assert(law.phase() == Phase::Release);
  run_ticks(law, svf, 1);
  assert(law.phase() == Phase::Terminated);
  assert(law.get_envelope_value() == 20);
}


// TVF-ENV Depth +63 at level 127 is 9600 cents, where the exponential tops
// out; the product saturates at 0xffff rather than wrapping in 32 bits, and at
// resonance 0 the word is then capped at 0x8000 and damped at 0x4000.
void test_cents_depth_and_saturation(void)
{
  auto lut = cents_tables();
  ControlRom::InstPartial ip = cents_partial(127, 1);
  ip.TVFEnvDepth = 63;
  ip.TVFEnvL1 = 127; ip.TVFEnvT1 = 0;
  ip.TVFEnvL2 = 127; ip.TVFEnvT2 = 0;
  ip.TVFEnvL3 = 127; ip.TVFEnvT3 = 0;
  CentsRatioTvfLaw open(ip, 60, 100, *lut, cents_law(), nullptr, still_lfos());
  assert(open.target_word() == 0xffff);

  ip.TVFBaseFlt = 0;
  ip.TVFResonance = 0;
  CentsRatioTvfLaw capped(ip, 60, 100, *lut, cents_law(), nullptr,
                          still_lfos());
  assert(capped.target_word() == 0x8000);
  assert(capped.damping() == 1.0f);
}


// The tone's own LFO depth reads the LFO's delayed and faded value; the
// controller matrix reads its raw word. Both land in cents.
void test_cents_lfo(void)
{
  auto lut = cents_tables();
  SVF svf(SVF::Mode::LowPass);

  {
    ControlRom::InstPartial ip = cents_partial(31, 0);
    ip.TVFLFO1Depth = 10;                    // 10 * 0x99 = 1530
    CentsRatioTvfLaw law(ip, 60, 100, *lut, cents_law(), nullptr,
                         still_lfos());
    TvfLfoInputs lfo = still_lfos();
    lfo.lfo1.value = 0x4000;                 // 1530 / 4 = 382 cents
    lfo.lfo1.raw = 0x7fff;                   // not read on this path
    run_ticks(law, svf, 1, lfo);
    const double ratio = law.target_word() / (double) 0x2000;
    assert(std::fabs(ratio - std::exp2(382 / 1200.0)) < 0.01);
  }
  {
    ControlRom::InstPartial ip = cents_partial(31, 0);
    int acc[JV_CTRL_DESTS] = {};
    acc[(int) JvCtrlDest::TvfLfo2] = 0x1000;
    CentsRatioTvfLaw law(ip, 60, 100, *lut, cents_law(), acc, still_lfos());
    TvfLfoInputs lfo = still_lfos();
    lfo.lfo2.value = 0;
    lfo.lfo2.raw = 0x4000;                   // 0x1000 * 0x4000 >> 16 = 1024
    run_ticks(law, svf, 1, lfo);
    const double ratio = law.target_word() / (double) 0x2000;
    assert(std::fabs(ratio - std::exp2(1024 / 1200.0)) < 0.01);
  }
}


// ---------------------------------------------------------------------------
// Which law each device gets, and that it behaves as that law.

void test_dispatch(void)
{
  FakeControls ctl;
  SVF svf(SVF::Mode::LowPass);

  auto ilut = indexed_tables();
  ControlRom::InstPartial iip = indexed_partial(60, 0x20);
  for (const DeviceProfile *d : { &SC55_PROFILE, &SC55MKII_PROFILE,
                                  &SOUND_CANVAS_DEFAULT_PROFILE }) {
    std::unique_ptr<TvfLaw> law(TvfLaw::create(*d, iip, 60, 100, *ilut, ctl,
                                               nullptr, still_lfos(), svf));
    auto *indexed = dynamic_cast<IndexedTvfLaw *>(law.get());
    assert(indexed != nullptr);
    assert(indexed->cutoff_level() == 2 * 60 * 256);
  }

  // The two Sound Canvas generations get their own cutoff laws through it.
  ctl.params[PatchParam::TVFCutoffFreq] = 0x40 + 10;
  {
    std::unique_ptr<TvfLaw> mk1(TvfLaw::create(SC55_PROFILE, iip, 60, 100,
                                               *ilut, ctl, nullptr,
                                               still_lfos(), svf));
    std::unique_ptr<TvfLaw> mk2(TvfLaw::create(SC55MKII_PROFILE, iip, 60, 100,
                                               *ilut, ctl, nullptr,
                                               still_lfos(), svf));
    assert(static_cast<IndexedTvfLaw &>(*mk1).cutoff_level() == 2 * 60 * 256);
    assert(static_cast<IndexedTvfLaw &>(*mk2).cutoff_level() == 2 * 70 * 256);
  }
  ctl.params.clear();

  auto clut = cents_tables();
  ControlRom::InstPartial cip = cents_partial(31, 0);
  {
    std::unique_ptr<TvfLaw> law(TvfLaw::create(JV880_PROFILE, cip, 60, 100,
                                               *clut, ctl, nullptr,
                                               still_lfos(), svf));
    auto *cents = dynamic_cast<CentsRatioTvfLaw *>(law.get());
    assert(cents != nullptr);
    assert(cents->cutoff_word() == 0x2000);
  }

  // Without its tables the law declines, and the filter stays disabled.
  clut->JVTvfBase[127] = 0;
  assert(TvfLaw::create(JV880_PROFILE, cip, 60, 100, *clut, ctl, nullptr,
                        still_lfos(), svf) == nullptr);
}

}  // namespace

int main()
{
  test_indexed_static_cutoff();
  test_indexed_cutoff_parameter();
  test_indexed_parameter_is_live();
  test_indexed_interpolation();
  test_indexed_ceilings();
  test_indexed_resonance();
  test_indexed_envelope_direction();
  test_indexed_ramp();

  test_cents_static();
  test_cents_ratio();
  test_cents_chip_stop();
  test_cents_resonance();
  test_cents_envelope();
  test_cents_depth_and_saturation();
  test_cents_lfo();

  test_dispatch();

  std::printf("gp_tvf_test: all passed\n");
  return 0;
}
