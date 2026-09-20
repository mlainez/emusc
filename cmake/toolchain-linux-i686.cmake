# Cross-compile a 32-bit x86 Linux build with multilib, from an x86_64 host.
#
#   cmake -B build-linux-x86 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-linux-i686.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#
# There is no separate "i686-linux-gnu" cross-compiler package: Debian/Ubuntu
# treat 32-bit x86 as a multilib target of the native amd64 toolchain
# (gcc-multilib/g++-multilib), selected with -m32 on the same gcc/g++.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR i686)

set(CMAKE_C_COMPILER gcc)
set(CMAKE_CXX_COMPILER g++)
set(CMAKE_C_FLAGS_INIT "-m32")
set(CMAKE_CXX_FLAGS_INIT "-m32")

# Debian/Ubuntu keep the i386 ALSA library under the multiarch triplet
# directory (/usr/lib/i386-linux-gnu, from libasound2-dev:i386 after `dpkg
# --add-architecture i386`); CMAKE_LIBRARY_ARCHITECTURE is what makes
# find_library() look there instead of the native amd64 lib dir.
set(CMAKE_LIBRARY_ARCHITECTURE i386-linux-gnu)
