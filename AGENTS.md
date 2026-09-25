# Architecture principles for this codebase

## Devices are injected at runtime, never hardcoded

Every synthesis engine in this project (`libemusc/src/engines/gp/`,
`libemusc/src/engines/xp/`) supports more than one physical device
sharing the same core algorithm, with the differences between devices
captured as *data*, not as branches or duplicated code. Both engines
split this into two layers:

- **Identification** - deciding which device a ROM is, before either
  engine's own code runs - lives in `libemusc/src/devices/<device>.cc`.
  Each device exports one `identify()` function and a fully-populated
  `ControlRom::DeviceEntry` (model, generation, profile pointer, the
  function itself). `src/devices/registry.cc` is the *only* file that
  lists every device together; `control_rom.h`/`.cc` name no device,
  ROM offset, byte pattern or threshold anywhere - only the generic
  `DeviceEntry` vocabulary type and one dispatch loop that calls
  whichever `identify()` each `DeviceEntry` supplies.
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

Adding a device means exactly:
1. `src/devices/<device>.cc` - an `identify()` function plus an
   exported `DeviceEntry`, building on a shared identification
   mechanism if one already fits (see below) or a bespoke one if the
   device needs it (a fixed ROM banner vs. JV-880's structural probe
   are the two precedents).
2. `engines/<engine>/devices/<device>.cc` (+`.h` if the engine needs its
   own profile type) - the device's real synthesis parameters.
3. One line in `src/devices/registry.cc`'s device list.
4. One line in `libemusc/src/CMakeLists.txt`.

Nothing outside those four touches changes. Every change that makes
this harder - a hardcoded device name outside `devices/`, a struct or
function named after a device instead of the role it plays, a fact
duplicated instead of read from the injected profile, identification
logic that only works for one device baked into shared dispatch code -
works against this rule and should be treated as a defect, not a style
preference.

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

## Naming reflects this

A struct, class, function or file that could serve a second device of
the same family must not be named after the first device it happened to
be written for. If a name change would make the answer to "does this
file mention a device by name" go from yes to no with no loss of
information, that is the rename to make.

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
