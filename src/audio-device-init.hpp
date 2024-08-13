// SPDX-FileCopyrightText: 2021-2024 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

//#define ALSA_PCM_NEW_HW_PARAMS_API
//#define ALSA_PCM_NEW_SW_PARAMS_API
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <semaphore.h>

#include "RingBuffer.hpp"
#include "ValueSmoother.hpp"

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
#define AUDIO_BRIDGE_CAPTURE_RINGBUFFER_BLOCKS 32

// prefer to read in big blocks, higher latency but more stable capture
#define AUDIO_BRIDGE_CAPTURE_BLOCK_SIZE_MULT 8

// how many audio buffer-size blocks to keep in the playback ringbuffer
#define AUDIO_BRIDGE_PLAYBACK_RINGBUFFER_BLOCKS 8

// --------------------------------------------------------------------------------------------------------------------

enum DeviceHints {
    kDeviceCapture = 0x1,
    kDeviceInitializing = 0x2,
    kDeviceStarting = 0x4,
    kDeviceBuffering = 0x8,
    kDeviceSample16 = 0x10,
    kDeviceSample24 = 0x20,
    kDeviceSample24LE3 = 0x40,
    kDeviceSample32 = 0x80,
    kDeviceSampleHints = kDeviceSample16|kDeviceSample24|kDeviceSample24LE3|kDeviceSample32
};

static constexpr const uint8_t kRingBufferDataFactor = 32;

static inline constexpr
uint8_t getSampleSizeFromHints(const uint8_t hints)
{
    return hints & kDeviceSample16 ? sizeof(int16_t) :
           hints & kDeviceSample24 ? sizeof(int32_t) :
           hints & kDeviceSample24LE3 ? 3 :
           hints & kDeviceSample32 ? sizeof(int32_t) :
           0;
}

// --------------------------------------------------------------------------------------------------------------------

struct DeviceAudio {
    struct HWStatus {
        uint32_t channels;
        uint32_t periods;
        uint32_t periodSize;
        uint32_t fullBufferSize;
    } hwstatus;

    char* deviceID;

    snd_pcm_t* pcm;
    uint32_t frame;
    uint32_t framesDone;
    uint32_t sampleRate;
    uint32_t bufferSize;
    uint32_t hints;
    bool enabled;

    struct {
        int8_t* raw;
        float** f32;
    } buffers;

    pthread_t thread;
    sem_t sem;

    AudioRingBuffer* ringbuffer;
    double rbFillTarget;
    double rbTotalNumSamples;
    double rbRatio = 1.0;
};

// --------------------------------------------------------------------------------------------------------------------

DeviceAudio* initDeviceAudio(const char* deviceID, bool playback, uint16_t bufferSize, uint32_t sampleRate);
bool runDeviceAudio(DeviceAudio* dev, float* buffers[]);
void closeDeviceAudio(DeviceAudio* dev);

#define DEBUGPRINT(...) { printf(__VA_ARGS__); puts(""); }

// --------------------------------------------------------------------------------------------------------------------
