cmake_minimum_required(VERSION 3.12...3.30)

project(
  libemusc
  VERSION 0.7.0
  DESCRIPTION "A software library for emulating Roland Sound Canvas devices"
  HOMEPAGE_URL "https://github.com/skjelten/emusc"
  LANGUAGES C CXX)

option(BUILD_SHARED_LIBS "Build libemusc as a shared library" ON)

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
  src/sc88_device.h
  src/sc88_engine.h
  src/sc88_lfo.h
  src/sc88_oscillator.h
  src/sc88_pan.h
  src/sc88_pitch.h
  src/sc88_renderer.h
  src/sc88_reverb.h
  src/sc88_rom.h
  src/sc88_tva.h
  src/sc88_tvf.h
  src/sc88_wave.h
  src/synth.h
  src/wave_rom.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/emusc COMPONENT dev)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/emusc.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig" COMPONENT dev)

if(CMAKE_CURRENT_BINARY_DIR STREQUAL CMAKE_BINARY_DIR)
  include(CPack)
endif()
