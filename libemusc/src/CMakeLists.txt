cmake_minimum_required(VERSION 3.8...3.30)

configure_file(config.h.in config.h)

add_library(emusc
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
  sc88_device.c
  sc88_device.h
  sc88_engine.c
  sc88_engine.h
  sc88_oscillator.c
  sc88_oscillator.h
  sc88_lfo.c
  sc88_lfo.h
  sc88_pan.c
  sc88_pan.h
  sc88_pitch.c
  sc88_pitch.h
  sc88_chorus.c
  sc88_chorus.h
  sc88_delay.c
  sc88_delay.h
  sc88_eq.c
  sc88_eq.h
  sc88_renderer.c
  sc88_renderer.h
  sc88_reverb.c
  sc88_reverb.h
  sc88_rom.c
  sc88_rom.h
  sc88_tva.c
  sc88_tva.h
  sc88_tvf.c
  sc88_tvf.h
  sc88_wave.c
  sc88_wave.h
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

target_compile_features(emusc PUBLIC cxx_std_17)
target_compile_features(emusc PRIVATE c_std_99)
target_include_directories(emusc PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
set_target_properties(emusc PROPERTIES CXX_EXTENSIONS OFF VERSION ${CMAKE_PROJECT_VERSION} SOVERSION ${CMAKE_PROJECT_VERSION_MAJOR})

# SC-88 specific debug/analysis tools have been removed.
# Use the multi-device emusc-render renderer in libemusc/tools/ instead.
# Individual tools (sc88_render, sc88_tvf_probe, sc88_dump_samples) can be built
# separately from their source files if needed for specialized analysis.

if(BUILD_TESTING)
  # Every test here is assertion-driven: the assertions are the test, not a
  # decoration on it. NDEBUG therefore compiles the bodies away and leaves a
  # suite that passes while testing nothing - and two of the eight went
  # further, freeing an engine that the stripped assertions never initialised.
  # -UNDEBUG is applied last, so it survives whatever the build type asks for.
  set(SC88_VOICE_SOURCES
    sc88_engine.c sc88_lfo.c sc88_oscillator.c sc88_pan.c sc88_pitch.c
    sc88_renderer.c sc88_reverb.c
    sc88_rom.c sc88_tva.c sc88_tvf.c sc88_wave.c)

  function(sc88_test name)
    add_executable(${name} ../tests/${name}.c ${ARGN})
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT MSVC)
      target_link_libraries(${name} PRIVATE m)
    endif()
    if(MSVC)
      target_compile_options(${name} PRIVATE /UNDEBUG)
    else()
      target_compile_options(${name} PRIVATE -UNDEBUG)
    endif()
    set_target_properties(${name} PROPERTIES C_STANDARD 99 C_EXTENSIONS OFF)
    add_test(NAME ${name} COMMAND ${name})
  endfunction()

  sc88_test(sc88_wave_test sc88_wave.c)
  sc88_test(sc88_oscillator_test sc88_oscillator.c)
  sc88_test(sc88_reverb_test sc88_reverb.c sc88_rom.c sc88_wave.c)
  sc88_test(sc88_chorus_test sc88_chorus.c sc88_reverb.c sc88_rom.c sc88_wave.c)
  sc88_test(sc88_delay_test sc88_delay.c sc88_reverb.c sc88_rom.c sc88_wave.c)
  sc88_test(sc88_eq_test sc88_eq.c sc88_rom.c sc88_wave.c)
  # This one checks the held ROM's own coefficient records, so it needs
  # the ROM; without it the test reports skipped rather than passing
  # while checking nothing.
  set_property(TEST sc88_eq_test PROPERTY SKIP_RETURN_CODE 77)
  if(DEFINED ENV{SC88_CONTROL_ROM})
    set_property(TEST sc88_eq_test PROPERTY
      COMMAND $<TARGET_FILE:sc88_eq_test> $ENV{SC88_CONTROL_ROM})
  endif()
  sc88_test(sc88_drum_test sc88_rom.c sc88_wave.c)
  sc88_test(sc88_lfo_test sc88_lfo.c sc88_rom.c sc88_wave.c)
  sc88_test(sc88_pitch_test sc88_pitch.c sc88_rom.c sc88_wave.c)
  sc88_test(sc88_tvf_test sc88_rom.c sc88_tvf.c sc88_wave.c)
  # The four that build a voice share one source list, so a new module is
  # wired in once instead of four times.
  sc88_test(sc88_rom_test ${SC88_VOICE_SOURCES})
  sc88_test(sc88_renderer_test ${SC88_VOICE_SOURCES})
  sc88_test(sc88_engine_test ${SC88_VOICE_SOURCES})
  sc88_test(sc88_device_test sc88_device.c sc88_chorus.c sc88_delay.c sc88_eq.c ${SC88_VOICE_SOURCES})
endif()
