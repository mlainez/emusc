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

# Link-time optimization for Release builds, on every target defined below:
# the libraries, and the tools and tests that compile engine sources directly.
# Rendered output is byte-identical with and without it on the native GCC and
# MinGW 32-bit toolchains. A toolchain with EMUSC_BASELINE_CHECK set is left
# out: that check disassembles the library's objects, which under LTO hold
# only compiler IR and would pass without checking anything.
option(EMUSC_LTO "Enable link-time optimization for Release builds" ON)
get_property(EMUSC_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT EMUSC_LTO)
  message(STATUS "Link-time optimization: disabled (EMUSC_LTO=OFF)")
elseif(EMUSC_BASELINE_CHECK)
  message(STATUS "Link-time optimization: disabled (toolchain sets EMUSC_BASELINE_CHECK)")
elseif(EMUSC_MULTI_CONFIG OR CMAKE_BUILD_TYPE STREQUAL "Release")
  include(CheckIPOSupported)
  check_ipo_supported(RESULT EMUSC_IPO_SUPPORTED OUTPUT EMUSC_IPO_OUTPUT)
  if(EMUSC_IPO_SUPPORTED)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    message(STATUS "Link-time optimization: enabled for Release")
  else()
    message(STATUS "Link-time optimization: not supported by this toolchain")
  endif()
endif()

add_subdirectory(libemusc)

# emuscd/ holds the realtime MIDI daemon for both platforms it's built on:
# emuscd itself (ALSA, Linux) or emusc-winmidi (WinMM, Windows). Neither is
# built on Apple platforms - no backend exists for it yet.
if(NOT APPLE)
  add_subdirectory(emuscd)
endif()
