cmake_minimum_required(VERSION 3.12...3.30)

project(
  libemusc
  VERSION 0.7.0
  DESCRIPTION "A software library for emulating Roland Sound Canvas devices"
  HOMEPAGE_URL "https://github.com/skjelten/emusc"
  LANGUAGES C CXX)

option(BUILD_SHARED_LIBS "Build libemusc as a shared library" ON)

# The legacy-fast DSP tier: the one switch under which this engine may trade
# bit-exact output for speed, and then only for an approximation already shown
# inaudible on its own evidence. It defaults ON where the CPU floor makes that
# trade worth its risk - 32-bit Windows, the Pentium III/Athlon XP baseline the
# MinGW 32-bit toolchain file targets - and OFF everywhere else, 64-bit Windows
# included. A builder can override either default.
#
# Nothing is gated on it yet: it is here ahead of its first use so the switch
# and its wiring can be reviewed apart from the first approximation that turns
# it on. An optimization that preserves the output belongs outside it, enabled
# on every platform unconditionally.
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(EMUSC_LEGACY_DSP_FAST_DEFAULT ON)
else()
  set(EMUSC_LEGACY_DSP_FAST_DEFAULT OFF)
endif()
option(EMUSC_LEGACY_DSP_FAST
  "Allow inaudible DSP approximations that do not preserve exact output"
  ${EMUSC_LEGACY_DSP_FAST_DEFAULT})

# Carried by an INTERFACE target rather than by naming the targets that need
# it: engine sources are compiled by the library and, directly, by a dozen
# tools and tests, and those two halves disagreeing about this macro would be
# an ODR violation that links quietly. src/CMakeLists.txt links this into
# every target that compiles them, so a target added later is either wired up
# or visibly not.
add_library(emusc_dsp_tier INTERFACE)
if(EMUSC_LEGACY_DSP_FAST)
  target_compile_definitions(emusc_dsp_tier INTERFACE EMUSC_LEGACY_DSP_FAST=1)
endif()
message(STATUS "libEmuSC legacy-fast DSP tier: ${EMUSC_LEGACY_DSP_FAST}")

add_subdirectory(src)
add_subdirectory(tools)

# TODO: Find a better way to handle pkg-config file generation
#       Challenge is that the correct path is first known in the install-phase
configure_file("cmake/emusc.pc.in" "emusc.pc" @ONLY)

include(GNUInstallDirs)

install(TARGETS emusc DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT lib)
install(FILES AUTHORS ChangeLog COPYING COPYING.LESSER NEWS README.md DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT lib)

install(FILES
  src/control_rom.h
  src/params.h
  src/engines/xp/device.h
  src/engines/xp/engine.h
  src/engines/xp/lfo.h
  src/engines/xp/oscillator.h
  src/engines/xp/pan.h
  src/engines/xp/pitch.h
  src/engines/xp/renderer.h
  src/engines/xp/reverb.h
  src/engines/xp/rom.h
  src/engines/xp/tva.h
  src/engines/xp/tvf.h
  src/engines/xp/wave.h
  src/synth.h
  src/wave_rom.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/emusc COMPONENT dev)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/emusc.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig" COMPONENT dev)

if(CMAKE_CURRENT_BINARY_DIR STREQUAL CMAKE_BINARY_DIR)
  include(CPack)
endif()
