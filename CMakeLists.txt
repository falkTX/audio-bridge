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

set(AUDIO_BRIDGE_ASYNC TRUE CACHE BOOL "Use async approach (default)")
set(AUDIO_BRIDGE_LINUX_MMAP FALSE CACHE BOOL "Use custom Linux mmap buffer instead of ALSA")
set(AUDIO_BRIDGE_UDEV FALSE CACHE BOOL "Enable libudev support")

if(NOT AUDIO_BRIDGE_LINUX_MMAP)
set(AUDIO_BRIDGE_ALSA TRUE)
endif()

#######################################################################################################################
# Find dependencies

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(JACK IMPORTED_TARGET REQUIRED jack)
pkg_check_modules(LV2 IMPORTED_TARGET REQUIRED lv2)

if(AUDIO_BRIDGE_ALSA)
pkg_check_modules(ALSA IMPORTED_TARGET REQUIRED alsa)
endif()

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
      $<$<BOOL:${AUDIO_BRIDGE_ASYNC}>:AUDIO_BRIDGE_ASYNC>
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
      $<$<BOOL:${AUDIO_BRIDGE_ALSA}>:PkgConfig::ALSA>
      $<$<BOOL:${AUDIO_BRIDGE_UDEV}>:PkgConfig::UDEV>
  )
endfunction()

#######################################################################################################################
# Setup JACK standalone target

add_executable(jack-audio-bridge)

set_common_target_properties(jack-audio-bridge)

target_compile_definitions(jack-audio-bridge
  PRIVATE
    AUDIO_BRIDGE_EXTERNAL_JACK_CLIENT
)

target_link_libraries(jack-audio-bridge
  PRIVATE
    PkgConfig::JACK
)

target_sources(jack-audio-bridge
  PRIVATE
    src/audio-device.cpp
    src/jack-client.cpp
    $<$<BOOL:${AUDIO_BRIDGE_ALSA}>:src/audio-device-discovery.cpp>
    $<$<BOOL:${AUDIO_BRIDGE_ALSA}>:src/audio-device-impl-alsa.cpp>
    $<$<BOOL:${AUDIO_BRIDGE_ALSA}>:src/resampler-table.cc>
    $<$<BOOL:${AUDIO_BRIDGE_ALSA}>:src/vresampler.cc>
    $<$<BOOL:${AUDIO_BRIDGE_LINUX_MMAP}>:src/audio-device-impl-linux-mmap.cpp>
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

target_link_options(jack-int-audio-bridge
  PRIVATE
    -Wl,-no-undefined
)

target_sources(jack-int-audio-bridge
  PRIVATE
    src/audio-device.cpp
    src/jack-client.cpp
    $<$<BOOL:${AUDIO_BRIDGE_ALSA}>:src/audio-device-discovery.cpp>
    $<$<BOOL:${AUDIO_BRIDGE_ALSA}>:src/audio-device-impl-alsa.cpp>
    $<$<BOOL:${AUDIO_BRIDGE_ALSA}>:src/resampler-table.cc>
    $<$<BOOL:${AUDIO_BRIDGE_ALSA}>:src/vresampler.cc>
    $<$<BOOL:${AUDIO_BRIDGE_LINUX_MMAP}>:src/audio-device-impl-linux-mmap.cpp>
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
    AUDIO_BRIDGE_LV2_PLUGIN
)

target_link_options(lv2-audio-bridge
  PRIVATE
    -Wl,-no-undefined
)

target_link_libraries(lv2-audio-bridge
  PRIVATE
    PkgConfig::LV2
    PkgConfig::JACK
)

target_sources(lv2-audio-bridge
  PRIVATE
    src/audio-device.cpp
    src/lv2-plugin.cpp
    $<$<BOOL:${AUDIO_BRIDGE_ALSA}>:src/audio-device-discovery.cpp>
    $<$<BOOL:${AUDIO_BRIDGE_ALSA}>:src/audio-device-impl-alsa.cpp>
    $<$<BOOL:${AUDIO_BRIDGE_ALSA}>:src/resampler-table.cc>
    $<$<BOOL:${AUDIO_BRIDGE_ALSA}>:src/vresampler.cc>
    $<$<BOOL:${AUDIO_BRIDGE_LINUX_MMAP}>:src/audio-device-impl-linux-mmap.cpp>
)

#######################################################################################################################
# Setup tests

if(AUDIO_BRIDGE_ALSA)

add_executable(audio-bridge-test)

set_common_target_properties(audio-bridge-test)

target_sources(audio-bridge-test
  PRIVATE
    src/audio-device-discovery.cpp
    src/tests.cpp
)

endif()

#######################################################################################################################
