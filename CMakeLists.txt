cmake_minimum_required(VERSION 3.24)
project(fh6_radio_rework VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FH6R_PORTABLE_TESTS_ONLY "Build only platform-neutral tests" OFF)

if(FH6R_PORTABLE_TESTS_ONLY)
  enable_testing()
  find_package(Threads REQUIRED)
  add_executable(audio_ring_test tests/audio_ring_test.cpp src/audio_ring.cpp)
  target_include_directories(audio_ring_test PRIVATE include)
  target_link_libraries(audio_ring_test PRIVATE Threads::Threads)
  add_test(NAME audio_ring COMMAND audio_ring_test)

  add_executable(dsp_contract_test tests/dsp_contract_test.cpp)
  target_include_directories(dsp_contract_test PRIVATE include)
  add_test(NAME dsp_contract COMMAND dsp_contract_test)

  add_executable(drift_control_test tests/drift_control_test.cpp)
  target_include_directories(drift_control_test PRIVATE include)
  add_test(NAME drift_control COMMAND drift_control_test)
  return()
endif()

if(NOT WIN32)
  message(FATAL_ERROR "FH6 Radio Rework targets Windows x64. Use -DFH6R_PORTABLE_TESTS_ONLY=ON for portable tests.")
endif()

add_library(version SHARED
  src/proxy/dll_main.cpp
  src/bridge.cpp
  src/log.cpp
  src/config.cpp
  src/audio_ring.cpp
  src/audio_state_probe.cpp
  src/memory_diff.cpp
  src/wasapi_capture.cpp
  src/http/http_server.cpp
  src/fmod/pe_image.cpp
  src/fmod/sig_scanner.cpp
  src/fmod/sig_scout.cpp
  src/fmod/studio_probe.cpp
  src/fmod/studio_param_hook.cpp
  src/fmod/radio_discovery.cpp
  src/fmod/dsp_bridge.cpp
  src/fmod/controller.cpp
)

target_include_directories(version PRIVATE include)
target_compile_definitions(version PRIVATE
  _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
)

if(MSVC)
  target_compile_options(version PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc /MP)
  target_compile_options(version PRIVATE "$<$<CONFIG:Release>:/O2;/GL;/Gy;/Gw>")
  target_link_options(version PRIVATE "$<$<CONFIG:Release>:/LTCG;/OPT:REF;/OPT:ICF>")
else()
  target_sources(version PRIVATE src/proxy/version.def)
endif()

target_link_libraries(version PRIVATE
  ole32 uuid ksuser avrt ws2_32
)

set_target_properties(version PROPERTIES OUTPUT_NAME version PREFIX "")
