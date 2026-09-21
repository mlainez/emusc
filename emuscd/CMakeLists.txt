cmake_minimum_required(VERSION 3.12)

project(emuscd LANGUAGES CXX)

if(WIN32)
  add_executable(emusc-winmidi main_winmidi.cc)
  target_link_libraries(emusc-winmidi PRIVATE emusc_static win9x_compat winmm)
  set_target_properties(emusc-winmidi PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS OFF)
  install(TARGETS emusc-winmidi DESTINATION bin)
else()
  find_package(ALSA REQUIRED)

  add_executable(emuscd main.cc)
  target_link_libraries(emuscd PRIVATE emusc_static ${ALSA_LIBRARIES})
  target_include_directories(emuscd PRIVATE ${ALSA_INCLUDE_DIRS})
  set_target_properties(emuscd PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS OFF)
  install(TARGETS emuscd DESTINATION bin)
endif()
