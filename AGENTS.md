# Architecture principles for this codebase

## Devices are injected at runtime, never hardcoded

Every synthesis engine in this project (`libemusc/src/engines/gp/`,
`libemusc/src/engines/xp/`) supports more than one physical device
sharing the same core algorithm, with the differences between devices
captured as *data*, not as branches or duplicated code:

- GP: `ControlRom` holds a `const DeviceProfile *`, selected once by
  `_profile_for()` after identifying the ROM (`control_rom.h`/`.cc`,
  `device_profile.h`). SC-55, SC-55mkII and JV-880 are three rows in
  `ControlRom::KNOWN_DEVICES[]` plus their own `devices/*.cc` profile
  file - nothing else changes to add a fourth.
- XP: `struct XpDeviceProfile` (`engines/xp/devices/sc88.h`) is injected
  through `xp_rom::profile` / `xp_engine::profile`, selected once by
  `rom_init()`'s own identification table (`engines/xp/rom.cc`) the same
  way. SC-88 is one row in that table plus its own `devices/sc88.cc`
  profile file.

## The golden rule: least possible friction for a new device

Adding a device should mean: write one new `devices/<device>.cc`
profile file with that device's own facts and identification bytes, add
one row to the engine's own identification table, and nothing else.
Every change that makes this harder - a hardcoded device name outside
`devices/`, a struct or function named after a device instead of the
role it plays, a fact duplicated instead of read from the injected
profile - works against this rule and should be treated as a defect,
not a style preference.

## All common code in one place, device-specific quirks in their own

- Common (device-agnostic) logic - the actual synthesis algorithm,
  voice management, effects - lives in the engine's own generic files
  and is named after what it does, never after a device
  (`ControlRom`/`Part`/`TVA`/`SVF` in GP; the XP equivalents follow the
  same rule).
- Device-specific facts and measurements - ROM addresses, tables,
  identification signatures, per-device constants - live only in that
  device's own file under `devices/` (GP) or `engines/xp/devices/` (XP).
  A generic file may *read* a device's profile through the injection
  point above; it must never *name* a device directly.
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
