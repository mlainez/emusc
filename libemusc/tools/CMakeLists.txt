cmake_minimum_required(VERSION 3.12)

# Generate version.h from template
set(LIBEMUSC_COMMIT "unknown")
set(LIBEMUSC_REF "unknown")
set(LIBEMUSC_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(LIBEMUSC_SOURCE_SHA256 "unknown")
set(LIBEMUSC_SOURCE_FILES "unknown")

configure_file(version.h.in "${CMAKE_CURRENT_BINARY_DIR}/version.h" @ONLY)

if(WIN32)
  set(EMUSC_RENDER_AUDIO_OUT_SRC audio_out_winmm.cc)
else()
  find_package(ALSA REQUIRED)
  set(EMUSC_RENDER_AUDIO_OUT_SRC audio_out_alsa.cc)
endif()

add_executable(emusc-render
  main.cc
  smf.cc
  smf.h
  wav.h
  audio_out.h
  ${EMUSC_RENDER_AUDIO_OUT_SRC})

target_link_libraries(emusc-render PRIVATE emusc_static)
if(WIN32)
  target_link_libraries(emusc-render PRIVATE win9x_compat)
endif()
target_include_directories(emusc-render PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_include_directories(emusc-render PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/../src")
target_include_directories(emusc-render PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

if(WIN32)
  target_link_libraries(emusc-render PRIVATE winmm)
else()
  target_link_libraries(emusc-render PRIVATE ${ALSA_LIBRARIES})
  target_include_directories(emusc-render PRIVATE ${ALSA_INCLUDE_DIRS})
endif()

set_target_properties(emusc-render PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS OFF C_STANDARD 99 C_EXTENSIONS OFF)

install(TARGETS emusc-render DESTINATION bin)
