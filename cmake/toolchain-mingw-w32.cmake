# Cross-compile a 32-bit Windows build with MinGW-w64, from Linux.
#
#   cmake -B build-win32 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w32.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

set(CMAKE_C_COMPILER i686-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER i686-w64-mingw32-g++)
set(CMAKE_RC_COMPILER i686-w64-mingw32-windres)

# pentium3 is the ISA floor deliberately chosen for SSE1: MMX, SSE, FXSR and
# CMOV, nothing an AMD Athlon XP/Duron doesn't also have - unlike
# -march=athlon-xp, which would pull in 3DNow!/3DNow!A and fault on genuine
# Intel silicon. -mtune=generic (rather than -mtune=pentium3) keeps
# instruction scheduling from being biased toward one vendor's pipeline,
# since the target is "every SSE1-capable CPU", not one specific chip.
# -mfpmath=sse is required on top of -march: GCC does not switch scalar
# float/double math off x87 just because the target ISA supports SSE.
#
# -fno-math-errno, -fno-trapping-math and -fno-signaling-nans are the parts
# of -ffast-math that skip errno/exception bookkeeping without changing any
# computed value - verified byte-identical against a build without them.
# The value-changing parts of -ffast-math (reassociation, reciprocal
# approximation, flushing NaN/Inf assumptions) are deliberately left out:
# this project measures its arithmetic against the real hardware's own
# precision, and byte-exact reproducibility is how that gets verified.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=pentium3 -mtune=generic -mfpmath=sse -O3 -pipe -fno-math-errno -fno-trapping-math -fno-signaling-nans")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=pentium3 -mtune=generic -mfpmath=sse -O3 -pipe -fno-math-errno -fno-trapping-math -fno-signaling-nans")

set(CMAKE_FIND_ROOT_PATH /usr/i686-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
