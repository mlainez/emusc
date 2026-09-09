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
  sc88_pan.c
  sc88_pan.h
  sc88_renderer.c
  sc88_renderer.h
  sc88_rom.c
  sc88_rom.h
  sc88_tva.c
  sc88_tva.h
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

if(BUILD_TESTING)
  add_executable(sc88_wave_test ../tests/sc88_wave_test.c sc88_wave.c)
  target_include_directories(sc88_wave_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  set_target_properties(sc88_wave_test PROPERTIES C_STANDARD 99 C_EXTENSIONS OFF)
  add_test(NAME sc88_wave_test COMMAND sc88_wave_test)

  add_executable(sc88_rom_test ../tests/sc88_rom_test.c sc88_pan.c sc88_rom.c
                                 sc88_tva.c sc88_wave.c)
  target_include_directories(sc88_rom_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  set_target_properties(sc88_rom_test PROPERTIES C_STANDARD 99 C_EXTENSIONS OFF)
  add_test(NAME sc88_rom_test COMMAND sc88_rom_test)

  add_executable(sc88_oscillator_test ../tests/sc88_oscillator_test.c
                                       sc88_oscillator.c)
  target_include_directories(sc88_oscillator_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(sc88_oscillator_test PRIVATE m)
  set_target_properties(sc88_oscillator_test PROPERTIES C_STANDARD 99 C_EXTENSIONS OFF)
  add_test(NAME sc88_oscillator_test COMMAND sc88_oscillator_test)

  add_executable(sc88_renderer_test ../tests/sc88_renderer_test.c
                                     sc88_engine.c sc88_renderer.c
                                     sc88_oscillator.c sc88_pan.c sc88_rom.c
                                     sc88_tva.c sc88_wave.c)
  target_include_directories(sc88_renderer_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(sc88_renderer_test PRIVATE m)
  set_target_properties(sc88_renderer_test PROPERTIES C_STANDARD 99 C_EXTENSIONS OFF)
  add_test(NAME sc88_renderer_test COMMAND sc88_renderer_test)

  add_executable(sc88_engine_test ../tests/sc88_engine_test.c sc88_engine.c
                                   sc88_renderer.c sc88_oscillator.c
                                   sc88_pan.c sc88_rom.c sc88_tva.c
                                   sc88_wave.c)
  target_include_directories(sc88_engine_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(sc88_engine_test PRIVATE m)
  set_target_properties(sc88_engine_test PROPERTIES C_STANDARD 99 C_EXTENSIONS OFF)
  add_test(NAME sc88_engine_test COMMAND sc88_engine_test)

  add_executable(sc88_device_test ../tests/sc88_device_test.c sc88_device.c
                                   sc88_engine.c sc88_renderer.c
                                   sc88_oscillator.c sc88_pan.c sc88_rom.c
                                   sc88_tva.c sc88_wave.c)
  target_include_directories(sc88_device_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(sc88_device_test PRIVATE m)
  set_target_properties(sc88_device_test PROPERTIES C_STANDARD 99 C_EXTENSIONS OFF)
  add_test(NAME sc88_device_test COMMAND sc88_device_test)
endif()
