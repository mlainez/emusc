cmake_minimum_required(VERSION 3.12)

add_executable(emusc-render
  main.cc
  smf.cc
  smf.h
  wav.h
  ../src/sc88_render.c
  ../src/sc88_renderer.h)

target_link_libraries(emusc-render PRIVATE emusc_static)
target_include_directories(emusc-render PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(emusc-render PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/../src")

set_target_properties(emusc-render PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS OFF C_STANDARD 99 C_EXTENSIONS OFF)

install(TARGETS emusc-render DESTINATION bin)
