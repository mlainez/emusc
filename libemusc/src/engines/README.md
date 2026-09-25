# Where the code for each DSP subdomain lives

An index for a reader who knows what a Sound Canvas or JV does but not
how this tree is laid out. For each subdomain it says which engine has
code for it, which files, and whether one implementation genuinely
serves several devices or each device has its own. It describes the
code as it is, including where it falls short of `AGENTS.md`'s rules;
those places are listed there under "Known gaps".

Paths are relative to `libemusc/src/engines/`.

## The two engines, in one paragraph each

- **GP** (`gp/`) is the C++ `Part`/`Note`/`Partial` pipeline for the
  Class G chip: SC-55, SC-55mkII and JV-880. Every device runs the same
  classes; per-device data comes from `DeviceProfile`
  (`../device_profile.h`, values in `gp/devices/<device>.cc`). Where the
  JV-880's arithmetic differs in kind, the shared class carries a second
  code path selected by a `*LawKind` enum and usually an `_jv` flag.
- **XP** (`xp/`) is the C engine for the Class X chip: SC-88 and
  JV-1080. Above the voice (MIDI device, channel state, wave-ROM
  descramble, record readers, output mix) it is shared. The voice path is
  not: the SC-88 runs the firmware port in `engine.cc`/`renderer.cc` and
  the generic-named voice files, the JV-1080 injects its own through
  `XpVoiceEngineOps` (`devices/profile.h`) in `devices/jv1080_engine.cc`
  and `devices/jv1080_voice.cc`. See `xp/README.md`.
- `common/dsp_kernels.h` is the only DSP code both engines share: a
  Bessel I0 for Kaiser windows (`gp/resampler.cc`, `xp/output.cc`) and
  the Chamberlin state-variable filter step (`gp/svf.h`, `xp/tvf.cc`).
  `xp/devices/jv1080_resample.cc` carries its own copy of the Bessel I0
  rather than using this one.

## Reverb

| Engine | Files | Shared across devices? |
|---|---|---|
| GP | `gp/reverb.cc`, `gp/reverb.h`, driven by `gp/system_effects.cc` | Network shared by SC-55, SC-55mkII and JV-880; parameter loading branches |
| XP | `xp/reverb.cc`, `xp/reverb.h` | Shared by SC-88 and JV-1080 |

- **GP.** `Reverb::process_sample` is one network for all three
  devices, with no device branch in the per-sample path (the same
  TC6116AF DSP program; the hardware evidence is in `device_profile.h`,
  in the comment opening the reverb section). What differs is how the registers are *filled*: the
  JV-880 loads them from its own type records under `if (_jvRecords)`
  (`Reverb::_set_jv_character`, and branches in `_set_character`,
  `_set_reverb_time`, `_set_pre_lpf`, `_set_delay_feedback`), and its
  return gain uses `ReverbReturnLaw::JVTypeCoefficient` in `_set_level`.
  The Sound Canvas's per-type numbers are the character blocks in
  `reverb.h` and `ReverbLaw` data.
- **XP.** One `reverb.cc` implementation: the SC-88 calls it from
  `device.cc` (`reverb_init`, `reverb_process`), the JV-1080 from
  `devices/jv1080_engine.cc` (`reverb_refresh` -> `reverb_init`, and
  `reverb_process` in the render loop). The JV-1080's own parameter
  mapping is `reverb_apply_params` in `jv1080_engine.cc`. The JV-1080's
  *insert*-effect reverb types are a separate, device-local
  implementation (`insert_reverb_refresh`/`insert_reverb_process` in
  `jv1080_engine.cc`).

## Chorus

| Engine | Files | Shared across devices? |
|---|---|---|
| GP | `gp/chorus.cc`, `gp/chorus.h` | Delay line and `process_sample` shared; control update branches |
| XP | `xp/chorus.cc`, `xp/chorus.h` | Shared by SC-88 and JV-1080 |

- **GP.** `Chorus::update()` dispatches to `_update_jv()` (the JV-880's
  swept read pointer, `ChorusLawKind::JVSweptPointer`, records in
  `LookupTables::JVChorusRecords`) or `_update_sound_canvas()`;
  `process_sample` is common to both.
- **XP.** The SC-88 calls `chorus_init`/`chorus_process` from
  `device.cc`; the JV-1080 calls `chorus_init`, `chorus_set_runtime` and
  `chorus_process` from `jv1080_engine.cc` (`chorus_refresh` and the
  render loop). Per-device differences are `XpDeviceProfile` fields
  (`chorusModulator`, `chorusFeedbackTap`).

## Delay

| Engine | Files | Shared across devices? |
|---|---|---|
| GP | none separate: reverb characters 6 and 7 in `gp/reverb.cc` | As reverb |
| XP, SC-88 | `xp/delay.cc`, `xp/delay.h` | SC-88 only |
| XP, JV-1080 | `devices/jv1080_engine.cc` | JV-1080 only, separate implementation |

- **GP** has no delay effect of its own. DELAY and PANNING DELAY are
  reverb types; `Reverb::_set_delay_feedback` handles them, with a
  JV-880 branch.
- **XP.** `delay.cc` is called only from `device.cc` (the SC-88's delay
  send). The JV-1080 does not use it: its reverb-type delays and its
  STEREO-DELAY insert effect run file-local code in `jv1080_engine.cc`
  (`delay_tap`, `delay_process`, `delay_refresh`,
  `efx_stereo_delay_refresh`), which reuse `delay.cc`'s function names
  but are a different implementation.

## TVA (amplitude envelope and level)

| Engine | Files | Shared across devices? |
|---|---|---|
| GP | `gp/tva.cc`, `gp/tva.h`, `gp/envelope.cc`, `gp/velocity_curve.h` | One class; level law branches |
| XP, SC-88 | `xp/tva.cc`, `xp/tva.h`, called from `xp/engine.cc` and `xp/renderer.cc` | SC-88 only |
| XP, JV-1080 | `devices/jv1080_voice.cc` (`amp_env_*`) | JV-1080 only |

- **GP.** `TVA` serves all three devices, but the level arithmetic is
  two algorithms in one class: the Sound Canvas's log-index chain and,
  under `levelLawKind == LevelLawKind::JVCurveProduct`, the JV-880's
  curve product (`TVA::_compose_static_level`, table
  `LookupTables::JVLevel`). The envelope itself (`envelope.cc`) is
  shared, with the JV's time-sense law behind `_jvTimeSense`.
- **XP.** No shared amplitude code. The SC-88's is the firmware port in
  `tva.cc`; the JV-1080's is a measured model in `jv1080_voice.cc`
  (`amp_env_level_db`, `amp_env_units_amplitude`,
  `amp_env_attack_seconds`, `amp_env_segment_seconds`, ...).

## TVF (filter)

| Engine | Files | Shared across devices? |
|---|---|---|
| GP | `gp/tvf.cc`, `gp/tvf.h`, `gp/svf.cc`, `gp/svf.h` | Filter core shared; cutoff chain branches |
| XP, SC-88 | `xp/tvf.cc`, `xp/tvf.h`, called from `xp/engine.cc` and `xp/renderer.cc` | SC-88 only |
| XP, JV-1080 | `devices/jv1080_voice.cc` (`tvf_*`, `set_svf_gains`) | JV-1080 only |

- **GP.** The `SVF` filter core is shared. The cutoff/envelope chain is
  two algorithms in one class: `TVF` sets `_jv` when
  `tvfLawKind == TvfLawKind::JVCentsRatio` and then early-returns into
  the JV-880 chain (`_jv_init`, `_jv_iterate`, `_jv_next_phase`,
  `_jv_apply_sample_set`) from `apply_sample_set`, the control-period
  update and `_init_new_phase`, with its own `_jv*` state in `tvf.h`.
  Constants for that chain are `TvfJvLaw` in `device_profile.h`.
- **XP.** No shared filter code. The SC-88's register-level port is
  `tvf.cc` (on `common/dsp_kernels.h`'s filter step); the JV-1080's
  behavioural model is in `jv1080_voice.cc` (`tvf_cutoff_hz`,
  `tvf_natural_hz`, `tvf_q`, `tvf_bypassed`, `set_svf_gains`), file-local
  functions that share the `tvf_` prefix but not the implementation.

## Oscillator and wave reading

| Engine | Files | Shared across devices? |
|---|---|---|
| GP | `gp/wave_oscillator.cc`, `gp/partial.cc`, `gp/pitch.cc`, `gp/wave_generator.cc` (LFO); wave ROM in `../wave_rom.cc` | Sample reading shared, no device branch; pitch envelope and LFO branch |
| XP, shared | `xp/wave.cc` (descramble), `xp/wave_cache.cc`, `xp/packed_rom.cc` | SC-88 and JV-1080 |
| XP, SC-88 | `xp/oscillator.cc`, `xp/pitch.cc`, `xp/lfo.cc`, via `xp/renderer.cc`/`xp/engine.cc` | SC-88 only |
| XP, JV-1080 | `devices/jv1080_voice.cc` (`wave_tap`, `pitch_env_*`, `lfo_*`), `devices/jv1080_resample.cc` | JV-1080 only |

- **GP.** `WaveOscillator` reads and interpolates samples the same way
  for every device and contains no JV branch. `Pitch` runs a separate
  JV-880 pitch envelope under `if (_jv)`, and the LFO
  (`gp/wave_generator.cc`, class `WaveGenerator`) early-returns into
  `_jv_update()` for the JV-880. `gp/resampler.cc` is not part of the
  voice: it is `Synth`'s output-rate converter, shared by every GP
  device.
- **XP.** Shared: `wave_descramble_chip` (`wave.cc`, called once from
  `device.cc` for both boards, parameterised by `XpDeviceProfile`),
  `wave_cache.cc` (decoded PCM, used by `renderer.cc` and
  `jv1080_voice.cc`) and the packed-record reader `packed_rom.cc`
  (`wave_element_open`). Per device: the SC-88's oscillator, pitch and
  LFO ports versus the JV-1080's file-local `wave_tap`,
  `pitch_env_*` and `lfo_*` in `jv1080_voice.cc`, plus its own output
  resampler `jv1080_resample.cc`.

## JV-1080 insert effects (EFX)

| Effect | File | Called from |
|---|---|---|
| Program loader, routing | `xp/efx.cc`, `xp/efx.h` | `devices/jv1080_engine.cc` |
| Overdrive / distortion | `xp/drive.cc`, `xp/drive.h` | `devices/jv1080_engine.cc` only |
| Compressor / limiter | `xp/dynamics.cc`, `xp/dynamics.h` | `devices/jv1080_engine.cc` only |
| Phaser | `xp/phaser.cc`, `xp/phaser.h` | `devices/jv1080_engine.cc` only |
| Rotary | `xp/rotary.cc`, `xp/rotary.h` | `devices/jv1080_engine.cc` only |
| Spectrum | `xp/spectrum.cc`, `xp/spectrum.h` | `devices/jv1080_engine.cc` only |
| Enhancer | `xp/enhancer.cc`, `xp/enhancer.h` | `devices/jv1080_engine.cc` only |
| Stereo EQ (and the output shelves) | `xp/stereo_eq.cc`, `xp/stereo_eq.h` | `devices/jv1080_engine.cc` only |

These files are generic XP code - they read the ROM through
`XpDeviceProfile` and name no device in their logic - but today the
JV-1080 is their only caller: the SC-88 has no insert effect. The
JV-1080 configures each through an `efx_*_refresh` function and runs the
selected one in its render loop, both in `jv1080_engine.cc`. Insert
types without a file of their own (the delay and reverb types) are
file-local to `jv1080_engine.cc`, as noted under Delay and Reverb. GP
has no insert effects.

`xp/eq.cc` is not one of these: it is the SC-88's output EQ, called only
from `device.cc`.
