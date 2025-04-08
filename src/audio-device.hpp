// SPDX-FileCopyrightText: 2021-2025 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>

#include "RingBuffer.hpp"
// #include "ValueSmoother.hpp"

#include "zita-resampler/vresampler.h"

// --------------------------------------------------------------------------------------------------------------------

// how many seconds to wait until start trying to compensate for clock drift
#define AUDIO_BRIDGE_CLOCK_DRIFT_WAIT_DELAY 2

// how many steps to use for smoothing the clock-drift compensation filter
#define AUDIO_BRIDGE_CLOCK_FILTER_STEPS_1 1024
#define AUDIO_BRIDGE_CLOCK_FILTER_STEPS_2 8192

// how many audio buffer-size capture blocks to store until rolling starts
// must be > 0
#define AUDIO_BRIDGE_CAPTURE_LATENCY_BLOCKS 8

// how many audio buffer-size blocks to keep in the capture ringbuffer
// #define AUDIO_BRIDGE_CAPTURE_RINGBUFFER_BLOCKS 32

// how many audio buffer-size blocks to keep in the playback ringbuffer
// #define AUDIO_BRIDGE_PLAYBACK_RINGBUFFER_BLOCKS 32

#define AUDIO_BRIDGE_DEVICE_BUFFER_SIZE 16
// #define AUDIO_BRIDGE_DEVICE_BUFFER_SIZE 128

// prefer to read in big blocks, higher latency but more stable capture
// #define AUDIO_BRIDGE_CAPTURE_BLOCK_SIZE_MULT 8

// --------------------------------------------------------------------------------------------------------------------

#define DEBUGPRINT(...) { printf(__VA_ARGS__); puts(""); }

// --------------------------------------------------------------------------------------------------------------------

enum SampleFormat {
    kSampleFormatInvalid,
    kSampleFormat16,
    kSampleFormat24,
    kSampleFormat24LE3,
    kSampleFormat32
};

enum DeviceState {
    kDeviceInitializing,
    kDeviceStarting,
    kDeviceBuffering,
    kDeviceRunning,
};

static constexpr const uint8_t kRingBufferDataFactor = 32;

static inline constexpr
uint8_t getSampleSizeFromFormat(const uint8_t format)
{
    return format == kSampleFormat16 ? sizeof(int16_t) :
           format == kSampleFormat24 ? sizeof(int32_t) :
           format == kSampleFormat24LE3 ? 3 :
           format == kSampleFormat32 ? sizeof(int32_t) :
           0;
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

    // device runtime configuration as created in `initAudioDevice`
    // does not change during the lifetime of the audio device
    struct HWConfig {
        SampleFormat format;
        uint8_t numChannels;
        uint8_t numPeriods;
        uint16_t periodSize;
        uint32_t fullBufferSize;
        uint32_t sampleRate;
    } hwconfig;

    struct Impl;
    Impl* impl;

//     uint32_t framesDone;
//     bool enabled;

    struct Buffers {
        float** f32;
    } buffers;

    AudioRingBuffer ringbuffer;
    VResampler* resampler;

    DeviceState state;
//     double rbFillTarget;
//     double rbTotalNumSamples;
//     double rbRatio = 1.0;
};

// --------------------------------------------------------------------------------------------------------------------

AudioDevice* initAudioDevice(const char* deviceID, bool playback, uint16_t bufferSize, uint32_t sampleRate);
bool runAudioDevice(AudioDevice* dev, float* buffers[], uint16_t numFrames);
void closeAudioDevice(AudioDevice* dev);

// --------------------------------------------------------------------------------------------------------------------
