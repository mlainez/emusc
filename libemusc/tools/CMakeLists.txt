cmake_minimum_required(VERSION 3.12)

# Generate version.h from template
set(LIBEMUSC_COMMIT "unknown")
set(LIBEMUSC_REF "unknown")
set(LIBEMUSC_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(LIBEMUSC_SOURCE_SHA256 "unknown")
set(LIBEMUSC_SOURCE_FILES "unknown")

configure_file(version.h.in "${CMAKE_CURRENT_BINARY_DIR}/version.h" @ONLY)

add_executable(emusc-render
  main.cc
  smf.cc
  smf.h
  wav.h)

target_link_libraries(emusc-render PRIVATE emusc_static)
target_include_directories(emusc-render PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(emusc-render PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/../src")
target_include_directories(emusc-render PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

set_target_properties(emusc-render PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS OFF C_STANDARD 99 C_EXTENSIONS OFF)

install(TARGETS emusc-render DESTINATION bin)
