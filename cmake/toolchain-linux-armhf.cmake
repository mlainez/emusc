# Cross-compile a 32-bit ARM (armhf/EABI) Linux build, from an x86_64 host.
#
#   cmake -B build-linux-arm32 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-linux-armhf.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)

# Debian/Ubuntu keep the armhf ALSA library under the multiarch triplet
# directory (/usr/lib/arm-linux-gnueabihf, from libasound2-dev:armhf after
# `dpkg --add-architecture armhf`); CMAKE_LIBRARY_ARCHITECTURE is what makes
# find_library() look there.
set(CMAKE_LIBRARY_ARCHITECTURE arm-linux-gnueabihf)

# ARMv7-A hard-float with VFPv3-D16 is the ISA floor for this build, stated
# here rather than inherited from whatever the distribution's armhf gcc
# happens to default to. VFPv3-D16 is the 16-double-register form every
# ARMv7-A VFP implementation has, and it carries no fused multiply-add: this
# architecture's arithmetic therefore never contracts a multiply and an add
# into one rounding, which VFPv4 (-mfpu=vfpv4, or any -mcpu that selects it)
# would change. No NEON, because the standard binary has to run on an
# ARMv7-A system that has none. -mtune=generic-armv7-a rather than a
# -mtune/-mcpu for one implementation keeps scheduling unbiased across the
# ARMv7-A parts this targets.
#
# -fno-math-errno, -fno-trapping-math and -fno-signaling-nans are the parts
# of -ffast-math that skip errno/exception bookkeeping without changing any
# computed value - verified byte-identical against a build without them
# (4 devices x 3 sample rates, run under qemu-arm). The value-changing parts
# of -ffast-math are deliberately left out: this project measures its
# arithmetic against the real hardware's own precision, and byte-exact
# reproducibility is how that gets verified.
#
# -frecord-gcc-switches is what makes the baseline check below able to read
# each object's own code-generation options back out of the object.
set(EMUSC_ARMHF_FLAGS "-march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard -mtune=generic-armv7-a -O3 -pipe -fno-math-errno -fno-trapping-math -fno-signaling-nans -frecord-gcc-switches")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${EMUSC_ARMHF_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${EMUSC_ARMHF_FLAGS}")

# The ISA/floating-point baseline this toolchain's own output has to stay
# inside, and the binutils that can read it. libemusc/src/CMakeLists.txt runs
# whatever a toolchain names here over the library's objects as part of the
# build; it names no architecture itself.
set(EMUSC_BASELINE_CHECK "${CMAKE_CURRENT_LIST_DIR}/check-baseline.sh")
set(EMUSC_BASELINE_CHECK_ARGS
    --baseline armv7-a
    --readelf arm-linux-gnueabihf-readelf
    --objdump arm-linux-gnueabihf-objdump)
