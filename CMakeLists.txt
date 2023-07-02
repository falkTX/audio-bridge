# SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

cmake_minimum_required(VERSION 3.15)
project(audio-bridge)

set(CMAKE_POLICY_DEFAULT_CMP0025 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0063 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)

set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN TRUE)

set_property(GLOBAL PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

#######################################################################################################################
# Find dependencies

find_package(PkgConfig REQUIRED)

pkg_check_modules(ALSA IMPORTED_TARGET REQUIRED alsa)
pkg_check_modules(LV2 IMPORTED_TARGET REQUIRED lv2)
pkg_check_modules(JACK IMPORTED_TARGET REQUIRED jack)

#######################################################################################################################
# Setup LV2 plugin target

add_library(audio-bridge SHARED)

set_target_properties(audio-bridge
  PROPERTIES
    PREFIX ""
)

target_compile_definitions(audio-bridge
  PRIVATE
    _REENTRANT
    _POSIX_PTHREAD_SEMANTICS
)

target_include_directories(audio-bridge
  PRIVATE
    src
)

target_link_libraries(audio-bridge
  PRIVATE
    PkgConfig::ALSA
    PkgConfig::LV2
    PkgConfig::JACK
)

target_sources(audio-bridge
  PRIVATE
    src/audio-discovery.cpp
    src/audio-process.cpp
    src/lv2-plugin.cpp
    src/resampler-table.cc
    src/vresampler.cc
)

#######################################################################################################################
# Setup JACK standalone target

add_executable(jack-audio-bridge)

target_compile_definitions(jack-audio-bridge
  PRIVATE
    _REENTRANT
    _POSIX_PTHREAD_SEMANTICS
)

target_include_directories(jack-audio-bridge
  PRIVATE
    src
)

target_link_libraries(jack-audio-bridge
  PRIVATE
    PkgConfig::ALSA
    PkgConfig::JACK
)

target_sources(jack-audio-bridge
  PRIVATE
    src/audio-discovery.cpp
    src/audio-process.cpp
    src/jack-client.cpp
    src/resampler-table.cc
    src/vresampler.cc
)

#######################################################################################################################
# Setup JACK internal client target

add_library(jack-int-audio-bridge SHARED)

target_compile_definitions(jack-int-audio-bridge
  PRIVATE
    _REENTRANT
    _POSIX_PTHREAD_SEMANTICS
)

set_target_properties(jack-int-audio-bridge
  PROPERTIES
    OUTPUT_NAME "jack-audio-bridge"
    PREFIX ""
)

target_compile_definitions(jack-int-audio-bridge
  PRIVATE
    AUDIO_BRIDGE_INTERNAL_JACK_CLIENT
)

target_include_directories(jack-int-audio-bridge
  PRIVATE
    src
)

target_link_libraries(jack-int-audio-bridge
  PRIVATE
    PkgConfig::ALSA
    PkgConfig::JACK
)

target_sources(jack-int-audio-bridge
  PRIVATE
    src/audio-discovery.cpp
    src/audio-process.cpp
    src/jack-client.cpp
    src/resampler-table.cc
    src/vresampler.cc
)

#######################################################################################################################
# Setup tests

add_executable(audio-bridge-test)

target_include_directories(audio-bridge-test
  PRIVATE
    src
)

target_link_libraries(audio-bridge-test
  PRIVATE
    PkgConfig::ALSA
    PkgConfig::JACK
)

target_sources(audio-bridge-test
  PRIVATE
    src/audio-discovery.cpp
    src/tests.cpp
)

#######################################################################################################################
