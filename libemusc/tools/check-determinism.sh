#!/usr/bin/env bash
# Render the same MIDI file twice with identical arguments and prove the two
# WAVs are byte-identical. Prints both hashes; exits non-zero on mismatch.
#
#   tools/emusc-render/check-determinism.sh <romset> <rate> <gm|gs> <input.mid> [outdir]
#
# Uses tools/emusc-render/build/emusc-render unless EMUSC_RENDER is set.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
bin="${EMUSC_RENDER:-$here/build/emusc-render}"
romset="${1:?romset}"; rate="${2:?rate}"; reset="${3:?gm|gs}"; in="${4:?input.mid}"
outdir="${5:-$here/build/determinism}"
mkdir -p "$outdir"
a="$outdir/a.wav"; b="$outdir/b.wav"
rm -f "$a" "$b"
"$bin" --romset "$romset" --rate "$rate" --reset "$reset" "$in" "$a" 2>/dev/null
"$bin" --romset "$romset" --rate "$rate" --reset "$reset" "$in" "$b" 2>/dev/null
ha=$(sha256sum "$a" | cut -d' ' -f1); hb=$(sha256sum "$b" | cut -d' ' -f1)
echo "run 1: $ha"
echo "run 2: $hb"
if [ "$ha" = "$hb" ] && cmp -s "$a" "$b"; then
  echo "IDENTICAL ($(stat -c %s "$a") bytes) romset=$romset rate=$rate reset=$reset input=$(sha256sum "$in" | cut -d' ' -f1)"
else
  echo "MISMATCH" >&2; exit 1
fi
