cmake_minimum_required(VERSION 3.12...3.30)

project(
  emuscd
  VERSION 0.7.0
  LANGUAGES C CXX)

include(CTest)

# Every binary this project produces should run without a compiler-specific
# runtime installed on the target machine: static-link the GCC/MinGW C++
# runtime everywhere, and on Windows also fold in libwinpthread so an .exe
# built here carries no MinGW DLLs at all (just the Windows system DLLs
# already present on any target, back to Windows 9x for the plain Win32 API
# calls this project uses).
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")
  if(WIN32)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
  endif()
endif()

add_subdirectory(libemusc)

# emuscd/ holds the realtime MIDI daemon for both platforms it's built on:
# emuscd itself (ALSA, Linux) or emusc-winmidi (WinMM, Windows). Neither is
# built on Apple platforms - no backend exists for it yet.
if(NOT APPLE)
  add_subdirectory(emuscd)
endif()
