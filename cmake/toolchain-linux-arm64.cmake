# Cross-compile a 64-bit ARM (aarch64) Linux build, from an x86_64 host.
#
#   cmake -B build-linux-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-linux-arm64.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# Debian/Ubuntu keep the arm64 ALSA library under the multiarch triplet
# directory (/usr/lib/aarch64-linux-gnu, from libasound2-dev:arm64 after
# `dpkg --add-architecture arm64`); CMAKE_LIBRARY_ARCHITECTURE is what makes
# find_library() look there.
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)

# Baseline ARMv8.0-A, stated here rather than inherited from the
# distribution's default, and deliberately not an -mcpu for one Cortex
# implementation: the target is "every ARMv8-A part", so -mtune=generic keeps
# scheduling unbiased and the output runs on all of them. Advanced SIMD is
# mandatory in ARMv8-A, so the compiler is free to use it; nothing outside
# ARMv8.0 is (no LSE atomics, CRC32, crypto, dot product or SVE).
#
# -moutline-atomics is gcc's own default here and is kept: the LSE
# instructions live in libgcc's helpers behind a runtime check, so the binary
# still runs on an ARMv8.0 part while using LSE on one that has it. It is
# stated explicitly so that the check below, which reads each object's
# recorded switches, sees a deliberate choice rather than a default that
# could move.
#
# Unlike ARM32, this baseline HAS a fused multiply-add and gcc contracts with
# it. That is the arithmetic this architecture already produces, so
# -ffp-contract is left alone: forcing it off would change ARM64's results
# rather than optimize them.
#
# -fno-math-errno, -fno-trapping-math and -fno-signaling-nans are the parts
# of -ffast-math that skip errno/exception bookkeeping without changing any
# computed value - verified byte-identical against a build without them
# (4 devices x 3 sample rates, run under qemu-aarch64). The value-changing
# parts of -ffast-math are deliberately left out: this project measures its
# arithmetic against the real hardware's own precision, and byte-exact
# reproducibility is how that gets verified.
#
# -frecord-gcc-switches is what makes the baseline check below able to read
# each object's own code-generation options back out of the object, which is
# the only route to them on AArch64: it has no ELF build-attribute section
# recording the FPU and ISA extensions the way ARM32 does.
set(EMUSC_ARM64_FLAGS "-march=armv8-a -mtune=generic -moutline-atomics -O3 -pipe -fno-math-errno -fno-trapping-math -fno-signaling-nans -frecord-gcc-switches")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${EMUSC_ARM64_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${EMUSC_ARM64_FLAGS}")

# The ISA/floating-point baseline this toolchain's own output has to stay
# inside, and the binutils that can read it. libemusc/src/CMakeLists.txt runs
# whatever a toolchain names here over the library's objects as part of the
# build; it names no architecture itself.
set(EMUSC_BASELINE_CHECK "${CMAKE_CURRENT_LIST_DIR}/check-baseline.sh")
set(EMUSC_BASELINE_CHECK_ARGS
    --baseline armv8-a
    --readelf aarch64-linux-gnu-readelf
    --objdump aarch64-linux-gnu-objdump)
