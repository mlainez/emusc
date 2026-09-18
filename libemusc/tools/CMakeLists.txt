# emusc-render: headless SMF -> WAV renderer on libEmuSC.
#
# Builds libEmuSC statically from the emusc/ submodule (libemusc only; the Qt
# client under emusc/emusc is never configured) and links this tool against it.
#
#   cmake -S tools/emusc-render -B tools/emusc-render/build -DCMAKE_BUILD_TYPE=Release
#   cmake --build tools/emusc-render/build
#
# The libEmuSC commit is read from git's own metadata files at configure time
# (no git executable is invoked) and compiled into --version.

cmake_minimum_required(VERSION 3.16...3.30)

project(emusc-render VERSION 0.1.0 LANGUAGES CXX)

set(EMUSC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../emusc" CACHE PATH
    "Path to the EmuSC checkout (the submodule containing libemusc/)")
get_filename_component(EMUSC_ROOT "${EMUSC_ROOT}" ABSOLUTE)

if(NOT EXISTS "${EMUSC_ROOT}/libemusc/CMakeLists.txt")
  message(FATAL_ERROR "libEmuSC not found at ${EMUSC_ROOT}/libemusc "
                      "(is the emusc submodule checked out?)")
endif()

# ---- Which libEmuSC commit is this? -----------------------------------------
# Resolve <root>/.git (a file 'gitdir: ...' for submodules, or a directory),
# then HEAD, then the ref it points to (loose ref or packed-refs).
set(LIBEMUSC_COMMIT "unknown")
set(LIBEMUSC_REF "unknown")
set(_gitdir "${EMUSC_ROOT}/.git")
if(EXISTS "${_gitdir}" AND NOT IS_DIRECTORY "${_gitdir}")
  file(READ "${_gitdir}" _gitdir_content)
  string(REGEX REPLACE "^gitdir:[ \t]*([^\r\n]*).*" "\\1" _gitdir_rel "${_gitdir_content}")
  if(IS_ABSOLUTE "${_gitdir_rel}")
    set(_gitdir "${_gitdir_rel}")
  else()
    get_filename_component(_gitdir "${EMUSC_ROOT}/${_gitdir_rel}" ABSOLUTE)
  endif()
endif()
if(EXISTS "${_gitdir}/HEAD")
  file(READ "${_gitdir}/HEAD" _head)
  string(STRIP "${_head}" _head)
  if(_head MATCHES "^ref: (.*)$")
    set(LIBEMUSC_REF "${CMAKE_MATCH_1}")
    if(EXISTS "${_gitdir}/${LIBEMUSC_REF}")
      file(READ "${_gitdir}/${LIBEMUSC_REF}" LIBEMUSC_COMMIT)
      string(STRIP "${LIBEMUSC_COMMIT}" LIBEMUSC_COMMIT)
    elseif(EXISTS "${_gitdir}/packed-refs")
      file(STRINGS "${_gitdir}/packed-refs" _packed REGEX "^[0-9a-f]+ ${LIBEMUSC_REF}$")
      if(_packed)
        list(GET _packed 0 _line)
        string(REGEX REPLACE " .*" "" LIBEMUSC_COMMIT "${_line}")
      endif()
    endif()
  else()
    set(LIBEMUSC_REF "detached")
    set(LIBEMUSC_COMMIT "${_head}")
  endif()
endif()
# Re-configure when the submodule's HEAD moves, so --version never reports a
# commit the binary does not contain (the files below are read above at
# configure time only; without this the identity goes stale after a submodule
# commit and any identity record made from it would be wrong).
foreach(_dep "${_gitdir}/HEAD" "${_gitdir}/${LIBEMUSC_REF}" "${_gitdir}/packed-refs")
  if(EXISTS "${_dep}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_dep}")
  endif()
endforeach()

message(STATUS "libEmuSC source: ${EMUSC_ROOT} at ${LIBEMUSC_COMMIT} (${LIBEMUSC_REF})")
set(LIBEMUSC_SOURCE_DIR "${EMUSC_ROOT}")
# version.h is regenerated at BUILD time so the source hash tracks edits
# without a reconfigure -- see source-id.cmake for why the commit alone is not
# enough.
set(LIBEMUSC_SOURCE_SHA256 "")
set(LIBEMUSC_SOURCE_FILES 0)
configure_file(version.h.in version.h @ONLY)
add_custom_target(emusc-render-source-id ALL
  COMMAND ${CMAKE_COMMAND}
          -DEMUSC_SRC_DIR=${EMUSC_ROOT}/libemusc/src
          -DVERSION_IN=${CMAKE_CURRENT_SOURCE_DIR}/version.h.in
          -DVERSION_OUT=${CMAKE_CURRENT_BINARY_DIR}/version.h
          -DPROJECT_VERSION=${PROJECT_VERSION}
          -DLIBEMUSC_COMMIT=${LIBEMUSC_COMMIT}
          -DLIBEMUSC_REF=${LIBEMUSC_REF}
          -DLIBEMUSC_SOURCE_DIR=${LIBEMUSC_SOURCE_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/source-id.cmake
  COMMENT "Hashing the libEmuSC sources for build identity"
  VERBATIM)

# ---- libEmuSC, static, no client ---------------------------------------------
# libemusc/CMakeLists.txt uses CMAKE_PROJECT_VERSION (the top-level project's)
# for its VERSION/SOVERSION properties; our project() above provides one so the
# subdirectory configures cleanly. Only libemusc/ is added; the Qt client is
# not part of this build at all.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "libEmuSC is linked statically into emusc-render" FORCE)
add_subdirectory("${EMUSC_ROOT}/libemusc" libemusc EXCLUDE_FROM_ALL)

# Keep floating point strictly IEEE in both the library and the tool: no
# fast-math, no FMA contraction. Bit-reproducibility of the WAV depends on it.
target_compile_options(emusc PRIVATE -ffp-contract=off -fno-fast-math)

# ---- the tool -----------------------------------------------------------------
add_executable(emusc-render main.cc smf.cc smf.h wav.h)
add_dependencies(emusc-render emusc-render-source-id)
target_compile_features(emusc-render PRIVATE cxx_std_17)
set_target_properties(emusc-render PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(emusc-render PRIVATE -ffp-contract=off -fno-fast-math -Wall -Wextra)
target_include_directories(emusc-render PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}"          # version.h
  "${EMUSC_ROOT}/libemusc/src")          # synth.h, control_rom.h, wave_rom.h
target_link_libraries(emusc-render PRIVATE emusc)
