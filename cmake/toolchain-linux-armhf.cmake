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
