// SPDX-FileCopyrightText: 2021-2025 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: GPL-3.0-or-later

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
#ifdef AUDIO_BRIDGE_ALSA
// bluetooth audio (through bluez-alsa)
#define AUDIO_BRIDGE_CAPTURE_RINGBUFFER_BLOCKS 16
#define AUDIO_BRIDGE_PLAYBACK_RINGBUFFER_BLOCKS 16
#define AUDIO_BRIDGE_CLOCK_DRIFT_WAIT_DELAY_1 1
#define AUDIO_BRIDGE_CLOCK_DRIFT_WAIT_DELAY_2 5
#define AUDIO_BRIDGE_CLOCK_FILTER_STEPS_1 64
#define AUDIO_BRIDGE_CLOCK_FILTER_STEPS_2 512
#else
// usb audio (through custom mmap buffer)
#define AUDIO_BRIDGE_CAPTURE_RINGBUFFER_BLOCKS 8
#define AUDIO_BRIDGE_PLAYBACK_RINGBUFFER_BLOCKS 8
#define AUDIO_BRIDGE_LEVEL_SMOOTHING 1
#endif
#define AUDIO_BRIDGE_DEVICE_BUFFER_SIZE 16
#define AUDIO_BRIDGE_UDEV 0
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
#ifndef AUDIO_BRIDGE_DEBUG
#define AUDIO_BRIDGE_DEBUG 1
#endif

// how many seconds to wait until start trying to compensate for clock drift
#ifndef AUDIO_BRIDGE_CLOCK_DRIFT_WAIT_DELAY_1
#define AUDIO_BRIDGE_CLOCK_DRIFT_WAIT_DELAY_1  2 /* start ratio calculations */
#endif
#ifndef AUDIO_BRIDGE_CLOCK_DRIFT_WAIT_DELAY_2
#define AUDIO_BRIDGE_CLOCK_DRIFT_WAIT_DELAY_2 10 /* activate dynamic resampling */
#endif

// how many steps to use for smoothing the clock-drift compensation filter
#ifndef AUDIO_BRIDGE_CLOCK_FILTER_STEPS_1
#define AUDIO_BRIDGE_CLOCK_FILTER_STEPS_1 1024
#endif
#ifndef AUDIO_BRIDGE_CLOCK_FILTER_STEPS_2
#define AUDIO_BRIDGE_CLOCK_FILTER_STEPS_2 8192
#endif

// how many audio buffer-size blocks to keep in the capture ringbuffer
#ifndef AUDIO_BRIDGE_CAPTURE_RINGBUFFER_BLOCKS
#define AUDIO_BRIDGE_CAPTURE_RINGBUFFER_BLOCKS 4
#endif

// priority to assign to audio capture thread (ALSA only)
#ifndef AUDIO_BRIDGE_CAPTURE_THREAD_PRIORITY
#define AUDIO_BRIDGE_CAPTURE_THREAD_PRIORITY 71
#endif

// how many audio buffer-size blocks to keep in the playback ringbuffer
#ifndef AUDIO_BRIDGE_PLAYBACK_RINGBUFFER_BLOCKS
#define AUDIO_BRIDGE_PLAYBACK_RINGBUFFER_BLOCKS 4
#endif

// priority to assign to audio capture thread (ALSA only)
#ifndef AUDIO_BRIDGE_PLAYBACK_THREAD_PRIORITY
#define AUDIO_BRIDGE_PLAYBACK_THREAD_PRIORITY 70
#endif

// device buffer size to use (minimum)
#ifndef AUDIO_BRIDGE_DEVICE_BUFFER_SIZE
#define AUDIO_BRIDGE_DEVICE_BUFFER_SIZE 16
#endif

// resample quality from 8 to 96
#ifndef AUDIO_BRIDGE_RESAMPLE_QUALITY
#define AUDIO_BRIDGE_RESAMPLE_QUALITY 8
#endif

// use udev for dynamic resampling stats
#ifndef AUDIO_BRIDGE_UDEV
#define AUDIO_BRIDGE_UDEV 0
#endif

// enable smooth audio ramping when starting fresh
#ifndef AUDIO_BRIDGE_LEVEL_SMOOTHING
#define AUDIO_BRIDGE_LEVEL_SMOOTHING 0
#endif

// --------------------------------------------------------------------------------------------------------------------

#if AUDIO_BRIDGE_DEBUG
#define DEBUGPRINT(...) { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); }
#else
#define DEBUGPRINT(...) { }
#endif

// always enable level smoothing for LV2 plugin
#ifdef AUDIO_BRIDGE_LV2_PLUGIN
#undef AUDIO_BRIDGE_LEVEL_SMOOTHING
#define AUDIO_BRIDGE_LEVEL_SMOOTHING 1
#endif

// us async mode for alsa, sync mode for linux-mmap
#ifdef AUDIO_BRIDGE_ALSA
#define AUDIO_BRIDGE_ASYNC 1
#else
#define AUDIO_BRIDGE_ASYNC 0
#endif

// ensure we don't use clock-drift filters for sync or udev approach
#if AUDIO_BRIDGE_UDEV || ! AUDIO_BRIDGE_ASYNC
#undef AUDIO_BRIDGE_CLOCK_DRIFT_WAIT_DELAY_1
#undef AUDIO_BRIDGE_CLOCK_DRIFT_WAIT_DELAY_2
#undef AUDIO_BRIDGE_CLOCK_FILTER_STEPS_1
#undef AUDIO_BRIDGE_CLOCK_FILTER_STEPS_2
#endif

// ensure we don't use separate threads for sync mode
#if ! AUDIO_BRIDGE_ASYNC
#undef AUDIO_BRIDGE_CAPTURE_THREAD_PRIORITY
#undef AUDIO_BRIDGE_PLAYBACK_THREAD_PRIORITY
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
   #if AUDIO_BRIDGE_ASYNC
    kDeviceInitializing = 0,
    kDeviceStarting,
    kDeviceStarted,
    kDeviceBuffering,
   #endif
    kDeviceRunning,
};

static constexpr const double kRingBufferDataFactor = 32;

static inline constexpr
uint8_t getSampleSizeFromFormat(const SampleFormat format)
{
    return format == kSampleFormat16 ? sizeof(int16_t) :
           format == kSampleFormat24 ? sizeof(int32_t) :
           format == kSampleFormat24LE3 ? 3 :
           format == kSampleFormat32 ? sizeof(int32_t) :
           0;
}

static inline constexpr
SampleFormat getSampleFormatFromSize(const uint8_t size)
{
    return size == 2 ? kSampleFormat16 :
           size == 3 ? kSampleFormat24LE3 :
           size == 4 ? kSampleFormat32 :
           kSampleFormatInvalid;
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
       #if AUDIO_BRIDGE_ASYNC
        std::atomic<int> state = { kDeviceInitializing };
        AudioRingBuffer* ringbuffer;
        pthread_mutex_t ringbufferLock;
        std::atomic<int> reset = { kDeviceResetNone };
        uint32_t numBufferingSamples;
       #endif
       #if AUDIO_BRIDGE_UDEV
        int ppm;
       #endif
       #if AUDIO_BRIDGE_LEVEL_SMOOTHING
        bool enabled;
        float volume;
       #endif
    } proc;

    // host process data
    struct {
       #if AUDIO_BRIDGE_ASYNC
        VResampler* resampler;
        uint32_t leftoverResampledFrames;
        uint32_t tempBufferSize;
        float** tempBuffers;
        float** tempBuffers2;
       #endif
       #if AUDIO_BRIDGE_LEVEL_SMOOTHING
        ExponentialValueSmoother gain;
        bool gainEnabled;
       #endif
    } hostproc;

    // statistics for clock drift and dynamic resampling
    struct Stats {
        uint32_t framesDone;
       #if AUDIO_BRIDGE_UDEV
        int ppm;
       #elif AUDIO_BRIDGE_ASYNC
        double rbFillTarget;
        double rbRatio;
        uint32_t lastChangeFrame;
       #endif
    } stats;

    // private device-specific implementation (ALSA, etc)
    struct Impl;
    Impl* impl;
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
