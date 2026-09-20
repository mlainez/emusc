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

# This toolchain's own default target is pentiumpro, which is stricter than
# it looks: -msse/-msse2 are off, but CMOV/FCOMI are still on, and those are
# real Pentium Pro+ instructions a plain Pentium or 486 doesn't have -
# confirmed 1948 cmov and 218 fcomi in an actual build before this was added.
# -march=i486 is the lowest target GCC exposes for x86 that still assumes a
# hardware FPU (486SX has none; going below this means software floating
# point, which this DSP-heavy codebase is in no position to run in real
# time). It's the practical floor: any 486DX or later, with an FPU.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=i486")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=i486")

set(CMAKE_FIND_ROOT_PATH /usr/i686-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
