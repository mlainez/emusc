#!/usr/bin/env bash
# Inspect a build's own object files and fail if they have left the ISA or
# floating-point baseline the toolchain that built them declares.
#
#   check-baseline.sh --baseline armv7-a|armv8-a \
#                     --readelf PROG --objdump PROG OBJECT...
#
# A toolchain file names this script and its arguments in
# EMUSC_BASELINE_CHECK/EMUSC_BASELINE_CHECK_ARGS; libemusc/src/CMakeLists.txt
# runs whatever it finds there over the library's objects after linking them.
#
# Objects, not the linked binary, because -moutline-atomics puts LSE atomics
# into libgcc's own helpers, which are runtime-guarded and are statically
# linked into every executable here: a scan of the executable reports them
# and is therefore useless as a baseline check, while a scan of this
# project's objects sees only the `bl __aarch64_*` calls to them.
#
# What each check can and cannot prove:
#
# - The ARM32 attribute checks are exact. Tag_FP_arch, Tag_Advanced_SIMD_arch
#   and the Tag_ABI_FP_* triple are the same merged ELF build attributes the
#   linker uses to reject an incompatible object, so they answer "does this
#   need NEON/VFPv4" and "is this still IEEE 754 with denormals and
#   exceptions" directly. AArch64 has no equivalent attribute section.
# - The recorded-switch check is exact for switches, and covers both targets:
#   -frecord-gcc-switches stores each translation unit's own code-generation
#   options in .GCC.command.line, so a widened -march, an -mcpu, or a
#   value-changing -f option is visible in the object whether it came from
#   the toolchain file, the environment or a command line.
# - The instruction scan is a blocklist, not a proof: it names the extension
#   families a widened -march/-mcpu or a hand-written intrinsic would bring
#   in, and will not notice one that is not on the list. It also needs machine
#   code to read, so it is vacuous on a slim LTO object, whose .text is empty
#   until the link; the other two checks still hold there. A build that turns
#   LTO on needs -ffat-lto-objects for this part to mean anything.
set -uo pipefail

baseline=
readelf=readelf
objdump=objdump
objects=()

while [ $# -gt 0 ]; do
  case "$1" in
    --baseline) baseline="$2"; shift 2 ;;
    --readelf)  readelf="$2";  shift 2 ;;
    --objdump)  objdump="$2";  shift 2 ;;
    --) shift; objects+=("$@"); break ;;
    -*) echo "check-baseline.sh: unknown option $1" >&2; exit 2 ;;
    *)  objects+=("$1"); shift ;;
  esac
done

case "$baseline" in
  armv7-a|armv8-a) ;;
  *) echo "check-baseline.sh: --baseline must be armv7-a or armv8-a" >&2; exit 2 ;;
esac
if [ ${#objects[@]} -eq 0 ]; then
  echo "check-baseline.sh: no object files given" >&2; exit 2
fi

failed=0
fail() { echo "  FAIL: $*" >&2; failed=1; }

echo "check-baseline.sh: ${#objects[@]} objects against the $baseline baseline"

# ---------------------------------------------------------------------------
# 1. ARM32 only: the merged ELF build attributes.
# ---------------------------------------------------------------------------

# Both read $attrs and $o from the loop below.
want_exact() {
  got=$(printf '%s\n' "$attrs" | sed -n "s/^[[:space:]]*$1:[[:space:]]*//p")
  [ "$got" = "$2" ] || fail "$o: $1 is '${got:-absent}', baseline needs '$2'"
}
want_absent() {
  printf '%s\n' "$attrs" | grep -q "^[[:space:]]*$1:" &&
    fail "$o: $1 is set, which the ARMv7-A/VFPv3-D16 baseline does not allow"
}

if [ "$baseline" = armv7-a ]; then
  for o in "${objects[@]}"; do
    attrs=$("$readelf" -A "$o" 2>/dev/null)
    [ -n "$attrs" ] || { fail "$o: no ARM attribute section"; continue; }
    # VFPv3-D16 exactly: VFPv3 (D32) or VFPv4 means a wider -mfpu or an -mcpu
    # picked the FPU, and VFPv4 would also add the scalar fused multiply-add
    # this architecture's baseline does not have.
    want_exact Tag_CPU_arch v7
    want_exact Tag_FP_arch  VFPv3-D16
    # No NEON: the standard binary has to run on an ARMv7-A system without it.
    want_absent Tag_Advanced_SIMD_arch
    want_absent Tag_WMMX_arch
    # Extensions a particular CPU brings in; plain -march=armv7-a sets none.
    want_absent Tag_DIV_use
    want_absent Tag_MPextension_use
    want_absent Tag_Virtualization_use
    # The floating-point model itself. -ffast-math and friends drop the
    # denormal and exception tags and turn the number model to 'Finite'.
    want_exact Tag_ABI_FP_denormal     Needed
    want_exact Tag_ABI_FP_exceptions   Needed
    want_exact Tag_ABI_FP_number_model "IEEE 754"
  done
fi

# ---------------------------------------------------------------------------
# 2. Both targets: the code-generation switches each object records.
# ---------------------------------------------------------------------------
# The recorded -march is gcc's own normalized form, not the spelling it was
# given: on ARM32, -march=armv7-a with -mfpu=vfpv3-d16 is recorded as
# armv7-a+fp, the FP extension with the particular FPU left to -mfpu (and
# checked exactly by Tag_FP_arch above). Anything else in the extension list
# - +simd, +mp, +sec, a different base such as armv7ve - is a widening.
case "$baseline" in
  armv7-a) want_march='^armv7-a(\+fp)?$' ;;
  armv8-a) want_march='^armv8-a$' ;;
esac

# Value-changing floating-point options. -fno-math-errno, -fno-trapping-math
# and -fno-signaling-nans are deliberately absent: those three skip errno and
# exception bookkeeping without changing a computed value, which is why the
# toolchains set them. -ffp-contract=off is here not because it is unsafe but
# because it changes results - ARM64's baseline contracts and ARM32's cannot,
# and both behaviours are the ones being preserved.
unsafe_fp='-Ofast|-ffast-math|-funsafe-math-optimizations|-fassociative-math|-freciprocal-math|-ffinite-math-only|-fno-signed-zeros|-fsingle-precision-constant|-fcx-limited-range|-ffp-contract=off|-mfpmath='

for o in "${objects[@]}"; do
  line=$("$readelf" -p .GCC.command.line "$o" 2>/dev/null |
           sed -n 's/^[[:space:]]*\[[[:space:]]*[0-9a-f]*\][[:space:]]*//p')
  if [ -z "$line" ]; then
    fail "$o: no .GCC.command.line section (is -frecord-gcc-switches set?)"
    continue
  fi
  # The whole -march value, not a substring of it: -march=armv8-a+crypto and
  # -march=armv7ve+simd both contain the baseline's own name.
  got_march=$(printf '%s\n' "$line" | tr ' ' '\n' | sed -n 's/^-march=//p' | tail -1)
  printf '%s\n' "$got_march" | grep -qE "$want_march" ||
    fail "$o: -march is '${got_march:-absent}', outside the $baseline baseline: $line"
  printf '%s\n' "$line" | tr ' ' '\n' | grep -q -e '^-mcpu=' &&
    fail "$o: built with an -mcpu=, which ties the output to one CPU: $line"
  printf '%s\n' "$line" | tr ' ' '\n' | grep -q -e '^-O3$' ||
    fail "$o: built without -O3: $line"
  hit=$(printf '%s\n' "$line" | tr ' ' '\n' | grep -o -E -e "$unsafe_fp" |
          sort -u | tr '\n' ' ')
  [ -z "$hit" ] ||
    fail "$o: built with floating-point options that change computed values: $hit"
done

# ---------------------------------------------------------------------------
# 3. Both targets: instructions from outside the baseline.
# ---------------------------------------------------------------------------
case "$baseline" in
  # VFPv4's scalar fused multiply-add, which VFPv3-D16 does not have and
  # which would give ARM32 a contraction its baseline does not perform.
  armv7-a) banned='^vf(ma|ms|nma|nms)[a-z]*(\.|$)' ;;
  # Families outside ARMv8.0-A, or optional within it: LSE atomics (v8.1),
  # RDMA (v8.1), CRC32, crypto/PMULL, dot product (v8.2/8.4), JS conversion
  # and RCpc loads (v8.3), pointer authentication (v8.3), BTI and the
  # restricted FRINTs (v8.5), MTE tagging (v8.5), BFloat16 (v8.6) and SVE.
  armv8-a) banned='^(cas[ablph]*|ldadd[ablh]*|ldclr[ablh]*|ldeor[ablh]*|ldset[ablh]*|ldsmax[ablh]*|ldsmin[ablh]*|ldumax[ablh]*|ldumin[ablh]*|swp[ablh]*|stadd[lbh]*|stclr[lbh]*|steor[lbh]*|stset[lbh]*|stsmax[lbh]*|stsmin[lbh]*|stumax[lbh]*|stumin[lbh]*|sqrdml[as]h|crc32c?[bhwx]|aes[edm]c|aesimc|sha1[chmp]|sha1su[01]|sha1h|sha256h2?|sha256su[01]|sha512[a-z0-9]*|sm[34][a-z0-9]*|pmull2?|[su]dot|fjcvtzs|ldapr[bh]?|ldapur[a-z]*|stlur[a-z]*|pac[idg][a-z0-9]*|aut[id][a-z0-9]*|xpac[a-z]*|reta[ab]|bra[ab][az]?|bti|frint(32|64)[xz]|irg|st2?g|ldg|subp|bf(dot|mmla|cvt[a-z0-9]*|mlal[a-z0-9]*)|ptrue[a-z0-9]*|whilel[teo][a-z0-9]*|cnt[bdhw])$' ;;
esac

for o in "${objects[@]}"; do
  hit=$("$objdump" -d "$o" 2>/dev/null |
          sed -n 's/^[[:space:]]*[0-9a-f]\+:\t[^\t]*\t\([a-z][a-z0-9._]*\).*/\1/p' |
          grep -E "$banned" | sort -u | tr '\n' ' ')
  [ -z "$hit" ] ||
    fail "$o: instructions from outside the $baseline baseline: $hit"
done

if [ "$failed" -ne 0 ]; then
  echo "check-baseline.sh: baseline violated" >&2
  exit 1
fi
echo "check-baseline.sh: within the $baseline baseline"
