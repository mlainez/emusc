cmake_minimum_required(VERSION 3.8...3.30)

configure_file(config.h.in config.h)

set(EMUSC_SOURCES
  analog_stage.cc
  analog_stage.h
  chorus.cc
  chorus.h
  control_rom.cc
  devices/jv880.cc
  devices/sc55.cc
  devices/sc55mkii.cc
  devices/sc88.cc
  devices/sound_canvas_default.cc
  control_rom.h
  envelope.cc
  envelope.h
  note.cc
  note.h
  params.h
  part.cc
  part.h
  partial.cc
  partial.h
  pitch.cc
  pitch.h
  resampler.cc
  resampler.h
  reverb.cc
  reverb.h
  engines/xp/device.cc
  engines/xp/device.h
  engines/xp/engine.cc
  engines/xp/engine.h
  engines/xp/oscillator.cc
  engines/xp/oscillator.h
  engines/xp/output.cc
  engines/xp/output.h
  engines/xp/lfo.cc
  engines/xp/lfo.h
  engines/xp/pan.cc
  engines/xp/pan.h
  engines/xp/pitch.cc
  engines/xp/pitch.h
  engines/xp/chorus.cc
  engines/xp/chorus.h
  engines/xp/delay.cc
  engines/xp/delay.h
  engines/xp/eq.cc
  engines/xp/eq.h
  engines/xp/renderer.cc
  engines/xp/renderer.h
  engines/xp/reverb.cc
  engines/xp/reverb.h
  engines/xp/rom.cc
  engines/xp/rom.h
  engines/xp/tva.cc
  engines/xp/tva.h
  engines/xp/tvf.cc
  engines/xp/tvf.h
  engines/xp/wave.cc
  engines/xp/wave.h
  settings.cc
  settings.h
  slew_calc.h
  svf.cc
  svf.h
  synth.cc
  synth.h
  system_effects.cc
  system_effects.h
  tva.cc
  tva.h
  tvf.cc
  tvf.h
  wave_generator.cc
  wave_generator.h
  wave_oscillator.cc
  wave_oscillator.h
  wave_rom.cc
  wave_rom.h)

# A plain OBJECT library, not folded into EMUSC_SOURCES: a static archive
# only pulls in a member when something already needs one of its symbols at
# the point the archive is scanned, and nothing does until libstdc++'s own
# guard.o is linked in afterwards - by which point ld has already moved past
# emusc_static.a and won't come back for it. Object libraries are always
# passed to the final link as loose .o files instead, sidestepping that.
#
# Only emusc-render and emusc-winmidi (see their own CMakeLists.txt) link
# this in: emusc/emusc_static's own sources need none of it, confirmed by
# checking the built libemusc.dll's own import table directly.
add_library(win9x_compat OBJECT win9x_compat.cc)
set_target_properties(win9x_compat PROPERTIES CXX_EXTENSIONS OFF)

add_library(emusc SHARED ${EMUSC_SOURCES})
add_library(emusc_static STATIC ${EMUSC_SOURCES})

target_compile_features(emusc PUBLIC cxx_std_17)
target_compile_features(emusc PRIVATE c_std_99)
target_compile_features(emusc_static PUBLIC cxx_std_17)
target_compile_features(emusc_static PRIVATE c_std_99)

target_include_directories(emusc PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(emusc_static PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")

set_target_properties(emusc PROPERTIES CXX_EXTENSIONS OFF VERSION ${CMAKE_PROJECT_VERSION} SOVERSION ${CMAKE_PROJECT_VERSION_MAJOR})
set_target_properties(emusc_static PROPERTIES CXX_EXTENSIONS OFF VERSION ${CMAKE_PROJECT_VERSION})

# SC-88 specific debug/analysis tools. Distinct from the multi-device
# emusc-render in libemusc/tools/: that one renders through EmuSC::Synth's
# public API, these link the sc88_* sources directly, because the values they
# report are internal to the engine and no measurement of the rendered audio
# can separate them.
add_executable(sc88_tvf_probe ../tools/sc88_tvf_probe.cc
  engines/xp/rom.cc engines/xp/tvf.cc engines/xp/tva.cc engines/xp/wave.cc)
target_include_directories(sc88_tvf_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(NOT MSVC)
  target_link_libraries(sc88_tvf_probe PRIVATE m)
endif()

add_executable(sc88_dump_samples ../tools/sc88_dump_samples.cc
  engines/xp/oscillator.cc engines/xp/rom.cc engines/xp/wave.cc)
target_include_directories(sc88_dump_samples PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(NOT MSVC)
  target_link_libraries(sc88_dump_samples PRIVATE m)
endif()

if(BUILD_TESTING)
  # Every test here is assertion-driven: the assertions are the test, not a
  # decoration on it. NDEBUG therefore compiles the bodies away and leaves a
  # suite that passes while testing nothing - and two of the eight went
  # further, freeing an engine that the stripped assertions never initialised.
  # -UNDEBUG is applied last, so it survives whatever the build type asks for.
  set(SC88_VOICE_SOURCES
    engines/xp/engine.cc engines/xp/lfo.cc engines/xp/oscillator.cc
    engines/xp/pan.cc engines/xp/pitch.cc
    engines/xp/renderer.cc engines/xp/reverb.cc
    engines/xp/rom.cc engines/xp/tva.cc engines/xp/tvf.cc
    engines/xp/wave.cc)

  function(sc88_test name)
    add_executable(${name} ../tests/${name}.cc ${ARGN})
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT MSVC)
      target_link_libraries(${name} PRIVATE m)
    endif()
    if(MSVC)
      target_compile_options(${name} PRIVATE /UNDEBUG)
    else()
      target_compile_options(${name} PRIVATE -UNDEBUG)
    endif()
    add_test(NAME ${name} COMMAND ${name})
  endfunction()

  sc88_test(sc88_wave_test engines/xp/wave.cc)
  sc88_test(sc88_oscillator_test engines/xp/oscillator.cc)
  sc88_test(sc88_pan_test engines/xp/pan.cc)
  sc88_test(sc88_reverb_test engines/xp/reverb.cc engines/xp/rom.cc engines/xp/wave.cc)
  sc88_test(sc88_chorus_test engines/xp/chorus.cc engines/xp/reverb.cc engines/xp/rom.cc engines/xp/wave.cc)
  sc88_test(sc88_delay_test engines/xp/delay.cc engines/xp/reverb.cc engines/xp/rom.cc engines/xp/wave.cc)
  sc88_test(sc88_eq_test engines/xp/eq.cc engines/xp/rom.cc engines/xp/wave.cc)
  sc88_test(sc88_output_test engines/xp/output.cc)
  # This one checks the held ROM's own coefficient records, so it needs
  # the ROM; without it the test reports skipped rather than passing
  # while checking nothing. The path is read from SC88_CONTROL_ROM by the
  # test itself, at run time, so ctest's own environment carries it
  # whatever the tree was configured with.
  set_property(TEST sc88_eq_test PROPERTY SKIP_RETURN_CODE 77)
  sc88_test(sc88_drum_test engines/xp/rom.cc engines/xp/wave.cc)
  sc88_test(sc88_lfo_test engines/xp/lfo.cc engines/xp/rom.cc engines/xp/wave.cc)
  sc88_test(sc88_pitch_test engines/xp/pitch.cc engines/xp/rom.cc engines/xp/wave.cc)
  sc88_test(sc88_tvf_test engines/xp/rom.cc engines/xp/tvf.cc engines/xp/tva.cc engines/xp/wave.cc)
  sc88_test(sc88_tva_test engines/xp/tva.cc engines/xp/rom.cc engines/xp/wave.cc)
  # The interpolation word's identity is a property of the SC-88's own
  # curve and rate tables, so the sweep needs the ROM; without it the test
  # reports skipped rather than passing while checking only the layout.
  # The path is read from SC88_CONTROL_ROM by the test itself, at run time.
  set_property(TEST sc88_tva_test PROPERTY SKIP_RETURN_CODE 77)
  # The four that build a voice share one source list, so a new module is
  # wired in once instead of four times.
  sc88_test(sc88_rom_test ${SC88_VOICE_SOURCES})
  # Each also sweeps the held ROM(s) end to end (every melodic tone for
  # sc88_rom_test; a real note-on and render for sc88_renderer_test and
  # device_test), gated on the same environment variables as
  # sc88_tva_test above; without them the sweep is skipped and only the
  # synthetic fixtures run.
  set_property(TEST sc88_rom_test PROPERTY SKIP_RETURN_CODE 77)
  sc88_test(sc88_renderer_test ${SC88_VOICE_SOURCES})
  set_property(TEST sc88_renderer_test PROPERTY SKIP_RETURN_CODE 77)
  sc88_test(sc88_engine_test ${SC88_VOICE_SOURCES})

  # EmuSC::Xp::Device and EmuSC::Xp::ChannelState (engines/xp/device.h) never
  # had a plain C compat struct or extern "C" shim layer - synth.cc was their
  # only caller outside this test, and it has always held them as an opaque
  # pointer - so device_test needs its own source list rather than
  # SC88_VOICE_SOURCES plus sc88_test()'s single shared-name convention.
  add_executable(device_test ../tests/device_test.cc engines/xp/device.cc
    engines/xp/chorus.cc engines/xp/delay.cc engines/xp/eq.cc
    engines/xp/output.cc ${SC88_VOICE_SOURCES})
  target_include_directories(device_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  if(NOT MSVC)
    target_link_libraries(device_test PRIVATE m)
  endif()
  if(MSVC)
    target_compile_options(device_test PRIVATE /UNDEBUG)
  else()
    target_compile_options(device_test PRIVATE -UNDEBUG)
  endif()
  add_test(NAME device_test COMMAND device_test)
  set_property(TEST device_test PROPERTY SKIP_RETURN_CODE 77)

  # Synth::reset() is reached through the public C++ API, so this one links the
  # library rather than the engine sources. ControlRom and WaveRom read files
  # and have no public constructor that takes bytes, so it needs the SC-88's
  # own ROM images and reports skipped without them. The paths are read from
  # the environment by the test itself, at run time, so ctest's own environment
  # carries them whatever the tree was configured with.
  add_executable(synth_reset_test ../tests/synth_reset_test.cc)
  target_include_directories(synth_reset_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(synth_reset_test PRIVATE emusc_static)
  if(MSVC)
    target_compile_options(synth_reset_test PRIVATE /UNDEBUG)
  else()
    target_compile_options(synth_reset_test PRIVATE -UNDEBUG)
  endif()
  set_target_properties(synth_reset_test PROPERTIES CXX_EXTENSIONS OFF)
  add_test(NAME synth_reset_test COMMAND synth_reset_test)
  set_property(TEST synth_reset_test PROPERTY SKIP_RETURN_CODE 77)
endif()
