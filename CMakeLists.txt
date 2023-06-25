# SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

cmake_minimum_required(VERSION 3.15)
project(lv2-awoosb)

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

add_library(lv2-awoosb MODULE)

set_target_properties(lv2-awoosb
  PROPERTIES
    PREFIX ""
)

target_include_directories(lv2-awoosb
  PRIVATE
    src
)

target_link_libraries(lv2-awoosb
  PRIVATE
    PkgConfig::ALSA
    PkgConfig::LV2
)

target_sources(lv2-awoosb
  PRIVATE
    src/audio-discovery.cpp
    src/audio-process.cpp
    src/lv2-plugin.cpp
)

#######################################################################################################################
# Setup JACK standalone target

add_executable(jack-awoosb)

target_include_directories(jack-awoosb
  PRIVATE
    src
)

target_link_libraries(jack-awoosb
  PRIVATE
    PkgConfig::ALSA
    PkgConfig::JACK
)

target_sources(jack-awoosb
  PRIVATE
    src/audio-discovery.cpp
    src/audio-process.cpp
    src/jack-client.cpp
)

#######################################################################################################################
# Setup tests

add_executable(awoosb-test)

target_include_directories(awoosb-test
  PRIVATE
    src
)

target_link_libraries(awoosb-test
  PRIVATE
    PkgConfig::ALSA
)

target_sources(awoosb-test
  PRIVATE
    src/audio-discovery.cpp
    src/audio-process.cpp
    src/tests.cpp
)

#######################################################################################################################
