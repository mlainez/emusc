cmake_minimum_required(VERSION 3.12...3.30)

project(
  emuscd
  VERSION 0.7.0
  LANGUAGES C CXX)

include(CTest)

add_subdirectory(libemusc)

# emuscd daemon requires ALSA (Linux only)
if(UNIX AND NOT APPLE)
  add_subdirectory(emuscd)
endif()
