cmake_minimum_required(VERSION 3.12)

project(emuscd LANGUAGES CXX)

find_package(ALSA REQUIRED)

add_executable(emuscd main.cc)

target_link_libraries(emuscd PRIVATE emusc_static ${ALSA_LIBRARIES})
target_include_directories(emuscd PRIVATE ${ALSA_INCLUDE_DIRS})

set_target_properties(emuscd PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS OFF)

install(TARGETS emuscd DESTINATION bin)
