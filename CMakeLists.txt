cmake_minimum_required(VERSION 3.16)

cmake_policy(SET CMP0091 NEW)

project(
  mojie
  VERSION 1.0.0
  DESCRIPTION "Automatic text-to-image replacement plugin for AviUtl2"
  LANGUAGES CXX
)

set(MOJIE_VERSION_SUFFIX "a1")
set(MOJIE_VERSION "${PROJECT_VERSION}${MOJIE_VERSION_SUFFIX}")

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/version.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/mojie_version.hpp"
  @ONLY
)

include(FetchContent)
include(CTest)

FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
)
FetchContent_MakeAvailable(nlohmann_json)

if(NOT WIN32)
  message(FATAL_ERROR "mojie is an AviUtl2 plugin and can only be built for Windows.")
endif()

set(
  MOJIE_AVIUTL2_SDK_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/.deps/aviutl2_sdk"
  CACHE PATH
  "Path to the AviUtl2 SDK mirror checkout"
)

set(MOJIE_PLUGIN_HEADER "${MOJIE_AVIUTL2_SDK_DIR}/include/aviutl2_sdk/plugin2.h")
if(NOT EXISTS "${MOJIE_PLUGIN_HEADER}")
  message(FATAL_ERROR
    "AviUtl2 SDK was not found at ${MOJIE_AVIUTL2_SDK_DIR}. "
    "Run scripts/setup.ps1 first or set MOJIE_AVIUTL2_SDK_DIR."
  )
endif()

add_library(mojie_core STATIC
  src/config.cpp
  src/replacement.cpp
  src/width_normalization.cpp
)

target_compile_features(mojie_core PUBLIC cxx_std_17)
target_compile_definitions(mojie_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(mojie_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(mojie_core PUBLIC
  nlohmann_json::nlohmann_json
  ole32
  uuid
  windowscodecs
)

add_library(mojie SHARED
  src/app_state.cpp
  src/aviutl_plugin.cpp
  src/config_ui.cpp
  src/plugin.cpp
)

target_compile_features(mojie PRIVATE cxx_std_17)
target_compile_definitions(mojie PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(mojie PRIVATE
  "${MOJIE_AVIUTL2_SDK_DIR}/include"
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_link_libraries(mojie PRIVATE mojie_core user32 gdi32 ole32 shell32 uuid windowscodecs comctl32)

if(MSVC)
  target_compile_options(mojie_core PRIVATE /utf-8 /W4 /permissive- /EHsc)
  target_compile_options(mojie PRIVATE /utf-8 /W4 /permissive- /EHsc)
else()
  message(FATAL_ERROR "The supported toolchain is MSVC (Visual Studio 2019 or newer).")
endif()

set_target_properties(mojie PROPERTIES
  PREFIX ""
  OUTPUT_NAME "mojie"
  SUFFIX ".aux2"
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

set_target_properties(mojie_core PROPERTIES
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

if(BUILD_TESTING)
  add_executable(mojie_replacement_tests tests/replacement_tests.cpp)
  target_link_libraries(mojie_replacement_tests PRIVATE mojie_core)
  target_compile_features(mojie_replacement_tests PRIVATE cxx_std_17)
  if(MSVC)
    target_compile_options(mojie_replacement_tests PRIVATE /utf-8 /W4 /permissive- /EHsc)
  endif()
  set_target_properties(mojie_replacement_tests PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
  )
  add_test(NAME replacement COMMAND mojie_replacement_tests)

  add_executable(mojie_config_tests tests/config_tests.cpp)
  target_link_libraries(mojie_config_tests PRIVATE mojie_core)
  target_compile_features(mojie_config_tests PRIVATE cxx_std_17)
  target_compile_definitions(mojie_config_tests PRIVATE
    MOJIE_TESTIMAGE_DIR=L"${CMAKE_CURRENT_SOURCE_DIR}/testimage"
  )
  if(MSVC)
    target_compile_options(mojie_config_tests PRIVATE /utf-8 /W4 /permissive- /EHsc)
  endif()
  set_target_properties(mojie_config_tests PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
  )
  add_test(NAME config COMMAND mojie_config_tests)

  add_executable(mojie_width_normalization_tests tests/width_normalization_tests.cpp)
  target_link_libraries(mojie_width_normalization_tests PRIVATE mojie_core)
  target_compile_features(mojie_width_normalization_tests PRIVATE cxx_std_17)
  if(MSVC)
    target_compile_options(mojie_width_normalization_tests PRIVATE /utf-8 /W4 /permissive- /EHsc)
  endif()
  set_target_properties(mojie_width_normalization_tests PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
  )
  add_test(NAME width_normalization COMMAND mojie_width_normalization_tests)
endif()

foreach(configuration Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${configuration}" configuration_upper)
  set_target_properties(mojie PROPERTIES
    "RUNTIME_OUTPUT_DIRECTORY_${configuration_upper}" "${CMAKE_BINARY_DIR}/bin/${configuration}"
  )
endforeach()
