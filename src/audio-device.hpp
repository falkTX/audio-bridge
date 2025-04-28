// SPDX-FileCopyrightText: 2021-2025 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include <pthread.h>

#include "RingBuffer.hpp"
#include "ValueSmoother.hpp"

#include "zita-resampler/vresampler.h"

// --------------------------------------------------------------------------------------------------------------------

// pre-tuned values
#ifdef _DARKGLASS_DEVICE_PABLITO
#define AUDIO_BRIDGE_CAPTURE_RINGBUFFER_BLOCKS 4
#define AUDIO_BRIDGE_PLAYBACK_RINGBUFFER_BLOCKS 4
#define AUDIO_BRIDGE_DEVICE_BUFFER_SIZE 16
#endif

// pre-tuned values
#ifdef _MOD_DEVICE_DWARF
#define AUDIO_BRIDGE_CAPTURE_RINGBUFFER_BLOCKS 8
#define AUDIO_BRIDGE_PLAYBACK_RINGBUFFER_BLOCKS 8
#define AUDIO_BRIDGE_DEVICE_BUFFER_SIZE 64
#endif

// --------------------------------------------------------------------------------------------------------------------

// print debug messages for development
// NOTE messages are mixed for capture and playback, do not enable while running both
#define AUDIO_BRIDGE_DEBUG 0

// run audio in async mode, using separate thread, ringbuffer and dynamic resampling
#define AUDIO_BRIDGE_ASYNC 1

// how many seconds to wait until start trying to compensate for clock drift
#define AUDIO_BRIDGE_CLOCK_DRIFT_WAIT_DELAY_1  2 /* start ratio calculations */
#define AUDIO_BRIDGE_CLOCK_DRIFT_WAIT_DELAY_2 10 /* activate dynamic resampling */

// how many steps to use for smoothing the clock-drift compensation filter
#define AUDIO_BRIDGE_CLOCK_FILTER_STEPS_1 1024
#define AUDIO_BRIDGE_CLOCK_FILTER_STEPS_2 8192
// #define AUDIO_BRIDGE_CLOCK_FILTER_STEPS_1 128
// #define AUDIO_BRIDGE_CLOCK_FILTER_STEPS_2 1024

// how many audio buffer-size blocks to keep in the capture ringbuffer
#ifndef AUDIO_BRIDGE_CAPTURE_RINGBUFFER_BLOCKS
#define AUDIO_BRIDGE_CAPTURE_RINGBUFFER_BLOCKS 2
#endif

// priority to assign to audio capture thread
#define AUDIO_BRIDGE_CAPTURE_THREAD_PRIORITY 83

// how many audio buffer-size blocks to keep in the playback ringbuffer
#ifndef AUDIO_BRIDGE_PLAYBACK_RINGBUFFER_BLOCKS
#define AUDIO_BRIDGE_PLAYBACK_RINGBUFFER_BLOCKS 2
#endif

// priority to assign to audio capture thread
#define AUDIO_BRIDGE_PLAYBACK_THREAD_PRIORITY 82

// device buffer size to use (minimum)
#ifndef AUDIO_BRIDGE_DEVICE_BUFFER_SIZE
#define AUDIO_BRIDGE_DEVICE_BUFFER_SIZE 16
#endif

// resample quality from 8 to 96
#define AUDIO_BRIDGE_RESAMPLE_QUALITY 8

// enable smooth audio ramping when starting fresh
#define AUDIO_BRIDGE_INITIAL_LEVEL_SMOOTHING 0

// --------------------------------------------------------------------------------------------------------------------

#if AUDIO_BRIDGE_DEBUG
#define DEBUGPRINT(...) { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); }
#else
#define DEBUGPRINT(...) { }
#endif

// always enable level smoothing for LV2 plugin
#ifdef AUDIO_BRIDGE_LV2_PLUGIN
#undef AUDIO_BRIDGE_INITIAL_LEVEL_SMOOTHING
#define AUDIO_BRIDGE_INITIAL_LEVEL_SMOOTHING 1
#endif

// use udev for dynamic resampling stats
#if defined(AUDIO_BRIDGE_INTERNAL_JACK_CLIENT) && defined(_DARKGLASS_DEVICE_PABLITO)
#define AUDIO_BRIDGE_UDEV 1
#else
#define AUDIO_BRIDGE_UDEV 0
#endif

// ensure we don't use clock-drift filters for udev approach
#if AUDIO_BRIDGE_UDEV
#undef AUDIO_BRIDGE_CLOCK_DRIFT_WAIT_DELAY_1
#undef AUDIO_BRIDGE_CLOCK_DRIFT_WAIT_DELAY_2
#undef AUDIO_BRIDGE_CLOCK_FILTER_STEPS_1
#undef AUDIO_BRIDGE_CLOCK_FILTER_STEPS_2
#endif

// --------------------------------------------------------------------------------------------------------------------

enum SampleFormat {
    kSampleFormatInvalid = 0,
    kSampleFormat16,
    kSampleFormat24,
    kSampleFormat24LE3,
    kSampleFormat32
};

enum DeviceReset {
    kDeviceResetNone = 0,
   #if AUDIO_BRIDGE_ASYNC
    kDeviceResetStats,
   #endif
    kDeviceResetFull,
};

enum DeviceState {
    kDeviceInitializing = 0,
    kDeviceStarting,
    kDeviceStarted,
    kDeviceBuffering,
   #if ! AUDIO_BRIDGE_ASYNC
    kDeviceBufferings = kDeviceBuffering + 16,
   #endif
    kDeviceRunning,
};

static constexpr const double kRingBufferDataFactor = 32;

static inline constexpr
uint8_t getSampleSizeFromFormat(const uint8_t format)
{
    return format == kSampleFormat16 ? sizeof(int16_t) :
           format == kSampleFormat24 ? sizeof(int32_t) :
           format == kSampleFormat24LE3 ? 3 :
           format == kSampleFormat32 ? sizeof(int32_t) :
           0;
}

static inline constexpr
double clamp_ratio(const double ratio)
{
    return std::fmax(0.0, std::fmin(4.0, ratio));
}

// --------------------------------------------------------------------------------------------------------------------

struct AudioDevice {
    // device configuration as passed in `initAudioDevice`
    // does not change during the lifetime of the audio device
    struct Config {
        char* deviceID;
        bool playback;
        uint16_t bufferSize;
        uint32_t sampleRate;
    } config;

    // device runtime configuration as created in `initAudioDeviceImpl`
    // does not change during the lifetime of the audio device
    struct HWConfig {
        SampleFormat format;
        uint8_t numChannels;
        uint8_t numPeriods;
        uint16_t periodSize;
        uint32_t fullBufferSize;
        uint32_t sampleRate;
    } hwconfig;

    // shared process data between host code and device implementations
    mutable struct Process {
        AudioRingBuffer* ringbuffer;
       #if AUDIO_BRIDGE_ASYNC
        pthread_mutex_t ringbufferLock;
       #endif
        std::atomic<int> reset = { kDeviceResetNone };
        std::atomic<int> state = { kDeviceInitializing };
       #if AUDIO_BRIDGE_ASYNC
        uint32_t numBufferingSamples;
       #endif
       #if AUDIO_BRIDGE_UDEV
        int ppm;
       #endif
    } proc;

   #if AUDIO_BRIDGE_ASYNC
    // host process data
    struct {
        VResampler* resampler;
        uint32_t leftoverResampledFrames;
        uint32_t tempBufferSize;
        float** tempBuffers;
        float** tempBuffers2;
       #if AUDIO_BRIDGE_INITIAL_LEVEL_SMOOTHING
        ExponentialValueSmoother gain;
        bool gainEnabled;
       #endif
    } hostproc;
   #endif

    // statistics for clock drift and dynamic resampling
    struct Stats {
        uint32_t framesDone;
       #if AUDIO_BRIDGE_UDEV
        int ppm;
       #elif AUDIO_BRIDGE_ASYNC
        double rbFillTarget;
        double rbRatio;
       #endif
    } stats;

    // private device-specific implementation (ALSA, etc)
    struct Impl;
    Impl* impl;

   #if AUDIO_BRIDGE_INITIAL_LEVEL_SMOOTHING
    // lv2 enabled control, for on/off (enable/bypass) control
    bool enabled;
   #endif
};

// --------------------------------------------------------------------------------------------------------------------

AudioDevice* initAudioDevice(const char* deviceID,
                             uint16_t bufferSize,
                             uint32_t sampleRate,
                             bool playback,
                             bool enabled = true);

bool runAudioDevice(AudioDevice* dev, float* buffers[], uint16_t numFrames);

void closeAudioDevice(AudioDevice* dev);

// --------------------------------------------------------------------------------------------------------------------
