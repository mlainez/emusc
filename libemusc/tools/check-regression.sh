#!/usr/bin/env bash
# Render the same MIDI file through two emusc-render builds - typically
# before and after a change - and prove the two WAVs are identical, or
# report that they aren't. check-determinism.sh answers "is this one
# build internally repeatable"; this answers the different question a
# refactor actually needs: "did this change the output at all".
#
#   tools/check-regression.sh <before-render> <after-render> <device> <rom-dir> <rate> <gm|gs> <input.mid> [outdir]
#
# <before-render>/<after-render> are paths to two emusc-render binaries,
# e.g. from two `git worktree`s built on either side of a commit:
#
#   git worktree add /tmp/before <commit-before-the-change>
#   cmake -S /tmp/before -B /tmp/before/build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
#   cmake --build /tmp/before/build --target emusc-render --parallel
#   cmake --build build --target emusc-render --parallel
#   tools/check-regression.sh /tmp/before/build/libemusc/tools/emusc-render \
#                              build/libemusc/tools/emusc-render \
#                              sc88 /path/to/sc88-roms 44100 gs song.mid
#
# --seed is fixed at 1 so a run-to-run non-determinism (a real bug, but a
# different one) isn't mistaken for a regression from the change itself.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
before="${1:?path to the "before" emusc-render}"
after="${2:?path to the "after" emusc-render}"
device="${3:?device}"; romdir="${4:?rom-dir}"; rate="${5:?rate}"; reset="${6:?gm|gs}"; in="${7:?input.mid}"
outdir="${8:-$here/build/regression}"
mkdir -p "$outdir"
a="$outdir/before.wav"; b="$outdir/after.wav"
rm -f "$a" "$b"
"$before" --device "$device" --rom-dir "$romdir" --rate "$rate" --reset "$reset" --seed 1 "$in" "$a"
"$after"  --device "$device" --rom-dir "$romdir" --rate "$rate" --reset "$reset" --seed 1 "$in" "$b"
ha=$(sha256sum "$a" | cut -d' ' -f1); hb=$(sha256sum "$b" | cut -d' ' -f1)
echo "before: $ha"
echo "after:  $hb"
if [ "$ha" = "$hb" ] && cmp -s "$a" "$b"; then
  echo "IDENTICAL ($(stat -c %s "$a") bytes) device=$device rate=$rate reset=$reset input=$(sha256sum "$in" | cut -d' ' -f1)"
else
  echo "DIFFERS" >&2
  cmp "$a" "$b" || true
  exit 1
fi
