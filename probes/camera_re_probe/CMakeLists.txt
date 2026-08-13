cmake_minimum_required(VERSION 3.24)
project(camera_re_probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(camera_re_probe SHARED camera_re_probe.cpp)
target_compile_definitions(camera_re_probe PRIVATE
  _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
)

if(MSVC)
  target_compile_options(camera_re_probe PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc /O2)
endif()

set_target_properties(camera_re_probe PROPERTIES
  OUTPUT_NAME camera_re_probe
  PREFIX ""
)
