# SPDX-FileCopyrightText: 2021-2025 Filipe Coelho <falktx@falktx.com>
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
# Build options

set(AUDIO_BRIDGE_UDEV FALSE CACHE BOOL "Enable libudev support")

#######################################################################################################################
# Find dependencies

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(ALSA IMPORTED_TARGET REQUIRED alsa)
pkg_check_modules(JACK IMPORTED_TARGET REQUIRED jack)
pkg_check_modules(LV2 IMPORTED_TARGET REQUIRED lv2)

if(AUDIO_BRIDGE_UDEV)
pkg_check_modules(UDEV IMPORTED_TARGET REQUIRED libudev)
endif()

#######################################################################################################################
# Utilities

function(set_common_target_properties TARGET)
  target_compile_definitions(${TARGET}
    PRIVATE
      _REENTRANT
      _POSIX_PTHREAD_SEMANTICS
      $<$<BOOL:${AUDIO_BRIDGE_UDEV}>:AUDIO_BRIDGE_UDEV>
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
      $<$<BOOL:${AUDIO_BRIDGE_UDEV}>:PkgConfig::UDEV>
  )
endfunction()

#######################################################################################################################
# Setup JACK internal client target (ALSA)

add_library(jack-int-audio-bridge-alsa SHARED)

set_common_target_properties(jack-int-audio-bridge-alsa)

set_target_properties(jack-int-audio-bridge-alsa
  PROPERTIES
    OUTPUT_NAME "jack-audio-bridge-alsa"
    PREFIX ""
)

target_compile_definitions(jack-int-audio-bridge-alsa
  PRIVATE
    AUDIO_BRIDGE_ALSA
    AUDIO_BRIDGE_INTERNAL_JACK_CLIENT
)

target_link_libraries(jack-int-audio-bridge-alsa
  PRIVATE
    PkgConfig::ALSA
    PkgConfig::JACK
)

target_link_options(jack-int-audio-bridge-alsa
  PRIVATE
    -Wl,-no-undefined
)

target_sources(jack-int-audio-bridge-alsa
  PRIVATE
    src/audio-device.cpp
    src/audio-device-discovery.cpp
    src/audio-device-impl-alsa.cpp
    src/jack-client.cpp
    src/resampler-table.cc
    src/vresampler.cc
)

#######################################################################################################################
# Setup JACK internal client target (Linux MMAP)

add_library(jack-int-audio-bridge-mmap SHARED)

set_common_target_properties(jack-int-audio-bridge-mmap)

set_target_properties(jack-int-audio-bridge-mmap
  PROPERTIES
    OUTPUT_NAME "jack-audio-bridge-mmap"
    PREFIX ""
)

target_compile_definitions(jack-int-audio-bridge-mmap
  PRIVATE
    AUDIO_BRIDGE_INTERNAL_JACK_CLIENT
    AUDIO_BRIDGE_LINUX_MMAP
)

target_link_libraries(jack-int-audio-bridge-mmap
  PRIVATE
    PkgConfig::JACK
)

target_link_options(jack-int-audio-bridge-mmap
  PRIVATE
    -Wl,-no-undefined
)

target_sources(jack-int-audio-bridge-mmap
  PRIVATE
    src/audio-device.cpp
    src/audio-device-impl-linux-mmap.cpp
    src/jack-client.cpp
)

#######################################################################################################################
# Setup JACK standalone target

add_executable(jack-audio-bridge)

set_common_target_properties(jack-audio-bridge)

target_compile_definitions(jack-audio-bridge
  PRIVATE
    AUDIO_BRIDGE_ALSA
    AUDIO_BRIDGE_EXTERNAL_JACK_CLIENT
)

target_link_libraries(jack-audio-bridge
  PRIVATE
    PkgConfig::ALSA
    PkgConfig::JACK
)

target_sources(jack-audio-bridge
  PRIVATE
    src/audio-device.cpp
    src/audio-device-discovery.cpp
    src/audio-device-impl-alsa.cpp
    src/jack-client.cpp
    src/resampler-table.cc
    src/vresampler.cc
)

#######################################################################################################################
# Setup LV2 plugin target

add_library(lv2-audio-bridge SHARED)

set_common_target_properties(lv2-audio-bridge)

configure_file(res/manifest.ttl audio-bridge.lv2/manifest.ttl COPYONLY)
configure_file(res/audio-bridge.ttl audio-bridge.lv2/audio-bridge.ttl COPYONLY)
configure_file(res/modgui/box.png audio-bridge.lv2/modgui/box.png COPYONLY)
configure_file(res/modgui/footswitch.png audio-bridge.lv2/modgui/footswitch.png COPYONLY)
configure_file(res/modgui/icon.html audio-bridge.lv2/modgui/icon.html COPYONLY)
configure_file(res/modgui/screenshot-capture.png audio-bridge.lv2/modgui/screenshot-capture.png COPYONLY)
configure_file(res/modgui/screenshot-playback.png audio-bridge.lv2/modgui/screenshot-playback.png COPYONLY)
configure_file(res/modgui/script.js audio-bridge.lv2/modgui/script.js COPYONLY)
configure_file(res/modgui/stylesheet.css audio-bridge.lv2/modgui/stylesheet.css COPYONLY)
configure_file(res/modgui/thumbnail-capture.png audio-bridge.lv2/modgui/thumbnail-capture.png COPYONLY)
configure_file(res/modgui/thumbnail-playback.png audio-bridge.lv2/modgui/thumbnail-playback.png COPYONLY)

set_target_properties(lv2-audio-bridge
  PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY audio-bridge.lv2
    OUTPUT_NAME audio-bridge
    PREFIX ""
)

target_compile_definitions(lv2-audio-bridge
  PRIVATE
    AUDIO_BRIDGE_ALSA
    AUDIO_BRIDGE_LV2_PLUGIN
)

target_link_options(lv2-audio-bridge
  PRIVATE
    -Wl,-no-undefined
)

target_link_libraries(lv2-audio-bridge
  PRIVATE
    PkgConfig::ALSA
    PkgConfig::JACK
    PkgConfig::LV2
)

target_sources(lv2-audio-bridge
  PRIVATE
    src/audio-device.cpp
    src/audio-device-discovery.cpp
    src/audio-device-impl-alsa.cpp
    src/lv2-plugin.cpp
    src/resampler-table.cc
    src/vresampler.cc
)

#######################################################################################################################
# Setup tests

add_executable(audio-bridge-test)

set_common_target_properties(audio-bridge-test)

target_link_libraries(audio-bridge-test
  PRIVATE
    PkgConfig::ALSA
)

target_sources(audio-bridge-test
  PRIVATE
    src/audio-device-discovery.cpp
    src/tests.cpp
)

#######################################################################################################################
