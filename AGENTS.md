# Architecture principles for this codebase

## Devices are injected at runtime, never hardcoded

Every synthesis engine in this project (`libemusc/src/engines/gp/`,
`libemusc/src/engines/xp/`) supports more than one physical device
sharing the same core algorithm. The goal is that differences between
devices are captured as *data* or as an injected *strategy* (see "Data
for constants, strategies for laws" below), not as branches or
duplicated code. That goal is not met everywhere yet; "Known gaps"
below lists where the code falls short of it today. Both engines split
device knowledge into two layers:

- **Identification** - deciding which device a ROM is, before either
  engine's own code runs - lives in `libemusc/src/devices/<device>.cc`.
  Each device exports one `identify()` function and a fully-populated
  `ControlRom::DeviceEntry` (model, generation, profile pointer, the
  function itself). `src/devices/registry.cc` is the one file that
  lists every device's `DeviceEntry` together, and `control_rom.cc`'s
  identification is one generic dispatch loop over those entries: no
  ROM offset, byte pattern or threshold lives in `control_rom.h`/`.cc`.
  `control_rom.h` does still name devices in its `SynthGen` and
  `SynthModel` enums and in `uses_xp_engine()` (see "Known gaps").
- **Synthesis parameters** - the real per-device data an engine's own
  generic code reads - live in `engines/<engine>/devices/<device>.cc`
  (`engines/gp/devices/sc55.cc`, `engines/xp/devices/sc88.h`/`.cc`,
  etc). GP's is `DeviceProfile`, injected into `ControlRom::profile()`.
  XP's is `XpDeviceProfile`, injected through `xp_rom::profile` /
  `xp_engine::profile` and selected by XP's *own*, separate
  identification (`engines/xp/rom.cc`'s `kKnownProfiles[]`/`matches()`).

XP's identification is deliberately independent of `src/devices/`'s:
`ControlRom`'s own identification (via `src/devices/sc88.cc`'s
`SC88_SIGNATURE`) exists only so `Synth` knows to hand an SC-88 ROM to
the XP engine at all (`ControlRom::uses_xp_engine()`); once there, XP
re-identifies the same bytes itself, entirely inside `engines/xp/`, to
pick its own `XpDeviceProfile`. Neither calls into the other.

## The golden rule: least possible friction for a new device

Adding a device should mean exactly:
1. `src/devices/<device>.cc` - an `identify()` function plus an
   exported `DeviceEntry`, building on a shared identification
   mechanism if one already fits (see below) or a bespoke one if the
   device needs it (a fixed ROM banner vs. JV-880's structural probe
   are the two precedents).
2. `engines/<engine>/devices/<device>.cc` (+`.h` if the engine needs its
   own profile type) - the device's real synthesis parameters.
3. One line in `src/devices/registry.cc`'s device list.
4. One line in `libemusc/src/CMakeLists.txt`.
5. For an XP-engine device: one entry in `engines/xp/rom.cc`'s
   `kKnownProfiles[]`, XP's own, second device list, which picks the
   `XpDeviceProfile` for a ROM (see "Devices are injected" above). It is
   a real touch point that `src/devices/registry.cc` does not cover:
   skip it and `ControlRom` identifies the device while the XP engine
   rejects the same ROM.

That is the target. Adding a device *today* touches more than those
five, and the extra touch points are known gaps, not part of the rule.
Adding the JV-1080 (commits `5e509a5`, `f9c424e`, `d724931`,
`2bac644`) also had to edit:
- `control_rom.h`'s `SynthGen` and `SynthModel` enums, and
  `ControlRom::uses_xp_engine()` for an XP-family device;
- `wave_rom.h`/`.cc`, which tested for one device by name and now asks
  `uses_xp_engine()` instead - a one-off cost of the second XP device;
- `engines/xp/devices/profile.h` and `engines/xp/device.cc`, to add the
  injection point (`XpVoiceEngineOps`) the device needed - likewise a
  one-off cost of the first device with its own voice path;
- the frontends' device lists and ROM-file conventions:
  `emuscd/common.h`'s `SUPPORTED_DEVICES` and `resolve_device_roms()`,
  `emuscd/main.cc`'s help text, and `libemusc/tools/main.cc`'s
  `--device` presets.

Nothing outside the five touches above should change. Every change
that makes this harder - a hardcoded device name outside `devices/`, a
struct or function named after a device instead of the role it plays,
a fact duplicated instead of read from the injected profile,
identification logic that only works for one device baked into shared
dispatch code - works against this rule and should be treated as a
defect, not a style preference.

## All common code in one place, device-specific quirks in their own

- Common (device-agnostic) logic lives in the engine's own generic
  files and is named after what it does, never after a device
  (`ControlRom`/`Part`/`TVA`/`SVF` in GP; the XP equivalents follow the
  same rule).
- Device-specific facts and measurements live only in that device's own
  file, at the layer they belong to: `src/devices/<device>.cc` for
  identification, `engines/<engine>/devices/<device>.cc` for synthesis
  parameters. A generic file may *read* a device's profile through the
  injection point above; it must never *name* a device directly.
- **"Common" is scoped to who actually shares it, not to the nearest
  folder.** Three distinct scopes exist; put shared code in whichever
  one matches what genuinely shares it:
  - `engines/common/` - shared between the GP and XP engine *families*
    (`dsp_kernels.h`'s Bessel I0 and state-variable-filter core, used
    by both).
  - `engines/xp/common/` - shared across the whole XP chip family, but
    not device-specific and not shared with GP (`constants.h`'s native
    sample rate, control-tick period, pitch-register format).
  - `src/devices/common/` - shared only by the identification devices
    that use the same *mechanism* (`rom_signature.h`'s signature-byte
    matching, used by SC-55/SC-55mkII/SC-88's `identify()` but not by
    JV-880's structural probe, which shares nothing with it).
- The one narrow, documented exception is a value that must be known at
  compile time to size a fixed array or that only a hot per-sample path
  with no profile/rom parameter in its call chain can reach - GP's
  `DeviceProfile::MAX_PARTIALS` and a handful of XP constants documented
  in `engines/xp/devices/sc88.h` are the precedent. Even then, the
  *value* still comes from measurement per device; only its *type*
  (compile-time vs. injected) is the exception, and the reason must be
  written down next to it, the same way the existing exceptions are.

## Data for constants, strategies for laws

Devices sharing a chip differ in one of two ways, and each has its own
injection mechanism:

- **They differ in what the parameters mean** - a constant table, a
  threshold, a scale factor, a record offset, fed into the same
  arithmetic. That is data: a field of the device's profile
  (`DeviceProfile`, `XpDeviceProfile`), read by generic code. This is
  the mechanism most of the codebase already uses.
- **They differ in what algorithm runs** - a fundamentally different
  formula or state machine, not different inputs to the same one. That
  is a strategy: an object or function table the device's profile
  supplies and the generic code calls through without asking which
  device it is. `XpVoiceEngineOps` (`engines/xp/devices/profile.h`),
  injected through `XpDeviceProfile::voiceEngine` and called by
  `engines/xp/device.cc`, is the precedent.

What a law difference must not become is an `if` on a kind enum inside
an otherwise shared class, with a parallel set of members that only one
branch uses. An enum that selects between two algorithms is a strategy
pointer spelled the expensive way: every shared method grows an early
return, the class carries state for every device at once, and a third
device means editing every branch. A kind enum is acceptable only where
it selects between a handful of lines inside one function; once a
branch owns its own state or its own methods, it is a strategy and is
written as one.

## Naming reflects this

A struct, class, function or file that could serve a second device of
the same family must not be named after the first device it happened to
be written for. If a name change would make the answer to "does this
file mention a device by name" go from yes to no with no loss of
information, that is the rename to make.

A law or strategy is named by its *mechanism*, never by the device that
first needed it: `CentsRatioTvfLaw`, `IndexedTvfLaw`,
`CurveProductLevel`, `SweptPointerChorus` - not `JVCentsRatio`,
`_jv_init` or `JVLevel`. The mechanism name stays true when a second
device uses the same law; the device name becomes a lie.

Provenance tags (see "Provenance annotations") are exempt: naming the
devices the *evidence* came from is documentation, not a name.

## Known gaps

The rules above describe the target. The code does not meet them
everywhere today, and these are the known, tracked places where it does
not. They are defects to fix, not precedents to copy; new code follows
the rules, not these.

- **GP's TVF branches on the device's law inside the shared class.**
  `TVF` (`engines/gp/tvf.cc`) picks the JV-880 cutoff chain at
  `tvf.cc:111` (`tvfLawKind == TvfLawKind::JVCentsRatio`) and then
  early-returns into it with `if (_jv)` at `tvf.cc:150`
  (`apply_sample_set`), `tvf.cc:222` (the control-period update) and
  `tvf.cc:659` (`_init_new_phase`). `tvf.h:134-159` carries the second
  algorithm's own state as twenty parallel `_jv*` members plus four
  `_jv_*` methods. Both chains share only the `SVF` filter core
  (`svf.cc`). This is a law difference implemented as a branch and is
  the first candidate for a strategy.
- **GP's TVA level law branches the same way.**
  `tva.cc:334` and `tva.cc:940` test
  `levelLawKind == LevelLawKind::JVCurveProduct` and run a different
  level arithmetic (`_compose_static_level`) instead of the Sound
  Canvas's; `control_rom.cc:2428` branches on the same enum when
  loading tables.
- **The same pattern, smaller, elsewhere in GP:** `Chorus::update()`
  dispatches to `_update_jv()` or `_update_sound_canvas()`
  (`chorus.cc:73`, selected by `ChorusLawKind::JVSweptPointer` at
  `chorus.cc:51`); `WaveGenerator::update()` (the LFO) early-returns
  into `_jv_update()` (`wave_generator.cc:127`); `Pitch` runs a `_jv_*`
  envelope (`if (_jv)` at `pitch.cc:800`, `Pitch::_jv_init` at
  `pitch.cc:940`); `Reverb` loads its registers
  from the JV-880's type records under `if (_jvRecords)`
  (`reverb.cc:370`, `411`, `543`, `582`), though its per-sample network
  (`Reverb::process_sample`) is one implementation for all three GP
  devices.
- **Shared GP code names a device generation directly:** `part.cc:479`,
  `552`, `631`, `950` and `note.cc:157` compare `generation()` against
  `ControlRom::SynthGen::JV880`.
- **Device names in law and table names**, against "Naming reflects
  this": `TvfLawKind::JVCentsRatio`, `LevelLawKind::JVCurveProduct`,
  `ChorusLawKind::JVSweptPointer`, `ReverbReturnLaw::JVTypeCoefficient`,
  `struct TvfJvLaw` (`device_profile.h`), `TVF::_jv_init` and the other
  `_jv*` members, and the `RomLookup`/`LookupTables` entries `JVLevel`,
  `JVTvfExpCoarse` and their siblings (`device_profile.h:468` onward,
  `control_rom.h`). In XP, `XP_TONE_MAP_SC55`/`XP_TONE_MAP_SC88`
  (`engines/xp/rom.h:59-60`, used in `engine.cc`, `renderer.cc`,
  `device.cc` and `rom.cc`) name devices. These two are a softer case:
  Roland's own name for CC32 values 1 and 2 is the "SC-55 map" and the
  "SC-88 map", so a rename should keep that protocol meaning visible
  rather than drop it.
- **`control_rom.h` names devices.** The `SynthGen` and `SynthModel`
  enums list every device, and `uses_xp_engine()` compares against
  `SynthGen::SC88` and `SynthGen::JV1080` by name, so adding an XP
  device edits it (see the golden rule's extra touch points).
- **Two device lists.** `src/devices/registry.cc` and
  `engines/xp/rom.cc`'s `kKnownProfiles[]` both enumerate devices; see
  golden-rule step 5.
- **XP's firmware-port voice path is the default, not a strategy.** A
  profile whose `voiceEngine` is null gets `engine.cc`/`renderer.cc`
  and the generic-named `tva.cc`, `tvf.cc`, `lfo.cc`, `pitch.cc` and
  `oscillator.cc`, which only the SC-88 uses; the JV-1080's voice path
  is `XpVoiceEngineOps` in `devices/jv1080_engine.cc` and
  `devices/jv1080_voice.cc`. Likewise `delay.cc` serves only the SC-88:
  `devices/jv1080_engine.cc` has its own delay (`delay_tap`,
  `delay_process`, `delay_refresh` at `jv1080_engine.cc:1467-1502`,
  file-local and reusing the shared file's function names).
- **One duplicated kernel.** `engines/xp/devices/jv1080_resample.cc`
  defines its own `bessel_i0` beside `engines/common/dsp_kernels.h`'s,
  with a different series cut-off.

## Provenance annotations

Much of this codebase's behaviour rests on evidence of very different
strength - a table read straight out of a firmware dump, a law fitted to
recordings of one owner's unit, a formula borrowed from someone else's
project. A comment that states such a law says which kind of evidence
it rests on, in one machine-checkable tag on its own comment line:

```
// @provenance class=MEASURED devices=JV-1080 ref=M-018
/* @provenance class=FW-EXACT devices=SC-88 ref=0x78802
   ...prose explaining the law and the evidence... */
```

- `class` (required) is exactly one of this closed set:
  - `FW-EXACT` - read from, or reproducing, a dumped firmware or ROM's
    own code or data; nothing is inferred beyond what the bytes say.
  - `DOCUMENTED` - taken from a published first-party document (a
    service manual's schematic or parts list, an owner's manual), not
    recovered from code or audio.
  - `MEASURED` - read off recordings of the real device or a
    chip-level reference, with no free parameters chosen to make the
    engine match.
  - `FITTED` - parameters chosen so that *this engine's* output lands on
    the measurement; they carry the engine's own remaining error.
  - `CREDITED` - taken from a third party's work (another emulator,
    published research) and not yet independently confirmed here.
  - `UNVERIFIED` - believed, but with no evidence yet, or with evidence
    that has not been checked against the device.
- `devices` (required) is a comma-separated list, no spaces, of the
  devices the evidence was gathered on or the law applies to, spelled as
  the model name (`SC-55`, `SC-55mkII`, `JV-880`, `SC-88`, `JV-1080`). A
  law measured on one device and assumed for another lists only the one
  it was measured on; the assumption belongs in the prose.
- `ref` (optional) points at the evidence, in whatever scheme already
  names it: a measurement id (`M-018`), a provenance-log entry
  (`P-0390`), a research-corpus decision (`D-27`), a taint-register
  entry (`T-008`), a ROM address (`ROM1:0x617F`, `0x78802`), a capture
  name. Several refs are comma-separated, no spaces. Leave it out rather
  than write a placeholder such as `P-xxxx`.

**Where `ref=` IDs are tracked, and how far to trust the lookup.** The
ledgers behind the IDs are not in this repository. They live in two
sibling projects, `../scdb` and `../emusc-match` relative to this
repo's root. These are separate git repositories, not submodules, so a
given checkout may not have them at all:

- `scdb/devices/<device>/` (`jv880`, `jv1080`, `sc55`, `sc55mk2`,
  `sc88`, `scc1`) keeps one research tree per device. Measurement ids
  are in `11_validation/measurements.md` and belong to that device
  (`M-018` in `devices/jv1080/` and `M-065` in `devices/jv880/` are
  separate ledgers). Decision/divergence ids are in
  `12_implementation/implementation_divergences.md` (`D-27`, `D-65` in
  `devices/jv880/`). Paths such as `08_effects/dsp_program.md` in
  comments are relative to a device's tree.
- `emusc-match/PROVENANCE.md` holds `P-NNNN` entries,
  `emusc-match/TAINT-REGISTER.md` holds `T-NNN` entries, and
  `emusc-match/backlog/tasks/` holds `TASK-NNN` entries.

The link between these ledgers and this repo's comments is partial,
and cross-checking them is still in progress. Neither ledger is a
complete 1:1 index of the IDs cited here. Some ledger entries are
cited nowhere in this repo. Some IDs cited here have no confirmed
matching entry yet, and a few cite a number whose ledger entry is
about something else: comments here cite `P-0390` for the JV-880's
TVF, while `emusc-match/PROVENANCE.md`'s `P-0390` is about the MT-32
bank. A lookup that finds nothing, or finds a different subject, means
the ref is not yet confirmed. It does not mean the ref is wrong. Treat
it as an open question for whoever does the cross-checking, and never
delete or rewrite a ref on the strength of a failed lookup alone.

One tag states one claim. A comment whose parts rest on different
evidence carries one tag per class (the analog output stage in
`engines/gp/devices/jv880.cc` is `MEASURED` in its shape and `FITTED` in
its numbers). The tag supplements the prose, it does not replace it: the
reasoning, the numbers and their residuals stay in the comment.

**Provenance tags are exempt from "never name a device".** A `devices=`
list is documentation of where evidence came from, not logic: no code
reads it, so it creates none of the coupling the naming rule exists to
prevent, and it may appear in any file, generic or not.

`libemusc/tools/lint-provenance.py` checks every tag (a `class` outside
the set, a missing or malformed `devices`, an unknown key, a placeholder
`ref`) and lists every comment that still states its evidence class in
free text (`FW-EXACT`, `MEASURED`, `` `FIT` ``, `[DOCUMENT]`, ...)
without a tag, as needing migration. `--summary` prints the per-file
count of those; `--strict` makes them fail the run. New and edited
comments use the tag.

## License headers

A new file's header depends on where its content actually came from,
not on which neighboring file it was templated from:

- **LGPL-2.1+, `Copyright (C) 2022-2026 Håkon Skjelten`**: content
  derived from, or relocated from, the original upstream libEmuSC
  codebase - the core GP engine, `control_rom.*`, `device_profile.h`,
  SC-55/SC-55mkII's own signature data. Relocating existing logic to a
  new file (`engines/common/dsp_kernels.h`,
  `src/devices/common/rom_signature.h`) keeps this header: the code's
  substance hasn't changed, only its address.
- **`SPDX-License-Identifier: CC0-1.0` plus the AI-generated-code
  disclosure**: content with no upstream lineage at all, produced by
  AI-assisted reverse-engineering or fresh design as part of this
  fork's own work. The entire XP engine uses the terse one-line form,
  since every file in that tree qualifies; a genuinely new file living
  alongside upstream code instead - JV-880-specific GP files,
  `src/devices/registry.cc` - uses the fuller disclosure paragraph,
  since the surrounding directory is otherwise Håkon's. See
  `engines/gp/devices/jv880.cc` for the exact wording of both forms.
- When in doubt: does any of this file's logic exist only because
  someone reverse-engineered or designed it as part of this fork, with
  no prior version in upstream libEmuSC? CC0. Is it moving or lightly
  adapting something that was already there? Keep the original
  attribution. Never copy a neighboring file's copyright line onto
  content that person didn't write.

## Git commit policy

These rules apply to every commit in this repository, including merges,
regardless of which agent or human is making it.

**Never do these:**
- Do not add a `Signed-off-by:` trailer. These are private/fork trees,
  not upstream submissions - no Developer Certificate of Origin is
  needed. If an imported patch already carries one, strip it before
  committing.
- Do not add a `Co-Authored-By:` / `Co-developed-by:` trailer, or any
  other co-authorship attribution. An AI assistant is never a co-author.
- Do not link to or identify an AI/chat session in a commit message or
  code comment: no transcript URLs, no session or task IDs, no "see the
  conversation where...".

**Do add an AI-assistance disclosure** (kernel.org convention,
<https://www.kernel.org/doc/html/latest/process/coding-assistants.html>)
when an AI assistant - any of them, not just one vendor's - materially
helped produce a commit. A single trailer, which is disclosure, not
co-authorship, naming the actual agent and model/version that helped:

```
Assisted-by: <agent-name>:<model-or-version>
```

For example `Assisted-by: Claude:claude-opus-4-8` or
`Assisted-by: Copilot:gpt-4`, whichever agent actually did the work -
always the real one, never a placeholder. Append any specialized
analysis tool actually used (e.g. `sparse`, `coccinelle`); do not list
ordinary tools like git/gcc/make/editors. Trivial/mechanical commits
made without AI help need no trailer.

**Language:** commit messages, code comments, branch names and PR text
are written in English, always, regardless of what language the
originating conversation or issue used.

**Keep commit messages concise:** optimize signal-to-noise; noise is a
defect.
- Subject line: imperative mood, at most 72 characters, no trailing
  period.
- Add a body only when the *why* isn't obvious from the diff - many
  commits need no body at all.
- Never restate the diff in prose, and never narrate the process
  ("first tried X, then Y", "after review", "as discussed") - the
  result is the commit, the journey is not.
- One logical change per commit.

**Code comments state present facts:** a comment describes the code as
it is at the moment it's written, not the story of how it got there. No
"previously this did X" / "changed from Y" / "was a workaround for Z" -
git history holds that. A comment that has gone stale is deleted, not
amended.
