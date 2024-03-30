# SPDX-FileCopyrightText: 2021-2024 Filipe Coelho <falktx@falktx.com>
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
find_package(Threads REQUIRED)

pkg_check_modules(ALSA IMPORTED_TARGET REQUIRED alsa)
pkg_check_modules(LV2 IMPORTED_TARGET REQUIRED lv2)
pkg_check_modules(JACK IMPORTED_TARGET REQUIRED jack)

#######################################################################################################################
# Utilities

function(set_common_target_properties TARGET)
  target_compile_definitions(${TARGET}
    PRIVATE
      _REENTRANT
      _POSIX_PTHREAD_SEMANTICS
  )

  target_compile_options(${TARGET}
    PRIVATE
      -Wall
      -Wextra
      -Wshadow
  )

  target_include_directories(${TARGET}
    PRIVATE
      src
  )

  target_link_libraries(${TARGET}
    PRIVATE
      ${CMAKE_THREAD_LIBS_INIT}
      PkgConfig::ALSA
  )
endfunction()

#######################################################################################################################
# Setup LV2 plugin target

add_library(audio-bridge SHARED)

set_common_target_properties(audio-bridge)

configure_file(res/manifest.ttl audio-bridge.lv2/manifest.ttl COPYONLY)
configure_file(res/audio-bridge.ttl audio-bridge.lv2/audio-bridge.ttl COPYONLY)

set_target_properties(audio-bridge
  PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY audio-bridge.lv2
)

target_link_libraries(audio-bridge
  PRIVATE
    PkgConfig::LV2
    PkgConfig::JACK
)

target_sources(audio-bridge
  PRIVATE
    src/audio-device-discovery.cpp
    src/audio-device-init.cpp
    src/lv2-plugin.cpp
    src/resampler-table.cc
    src/vresampler.cc
)

#######################################################################################################################
# Setup JACK standalone target

add_executable(jack-audio-bridge)

set_common_target_properties(jack-audio-bridge)

target_link_libraries(jack-audio-bridge
  PRIVATE
    PkgConfig::JACK
)

target_sources(jack-audio-bridge
  PRIVATE
    src/audio-device-discovery.cpp
    src/audio-device-init.cpp
    src/jack-client.cpp
    src/resampler-table.cc
    src/vresampler.cc
)

#######################################################################################################################
# Setup JACK internal client target

add_library(jack-int-audio-bridge SHARED)

set_common_target_properties(jack-int-audio-bridge)

set_target_properties(jack-int-audio-bridge
  PROPERTIES
    OUTPUT_NAME "jack-audio-bridge"
    PREFIX ""
)

target_compile_definitions(jack-int-audio-bridge
  PRIVATE
    AUDIO_BRIDGE_INTERNAL_JACK_CLIENT
)

target_link_libraries(jack-int-audio-bridge
  PRIVATE
    PkgConfig::JACK
)

target_sources(jack-int-audio-bridge
  PRIVATE
    src/audio-device-discovery.cpp
    src/audio-device-init.cpp
    src/jack-client.cpp
    src/resampler-table.cc
    src/vresampler.cc
)

#######################################################################################################################
# Setup tests

add_executable(audio-bridge-test)

set_common_target_properties(audio-bridge-test)

target_sources(audio-bridge-test
  PRIVATE
    src/audio-device-discovery.cpp
    src/tests.cpp
)

#######################################################################################################################
