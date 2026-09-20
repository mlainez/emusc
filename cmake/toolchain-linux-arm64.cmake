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
