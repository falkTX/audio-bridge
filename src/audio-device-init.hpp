// SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

//#define ALSA_PCM_NEW_HW_PARAMS_API
//#define ALSA_PCM_NEW_SW_PARAMS_API
#include <alsa/asoundlib.h>
#include <jack/ringbuffer.h>
#include <pthread.h>
#include <semaphore.h>

#include "RingBuffer.hpp"
#include "ValueSmoother.hpp"

#include "zita-resampler/vresampler.h"

// --------------------------------------------------------------------------------------------------------------------

enum DeviceHints {
    kDeviceCapture = 0x1,
    kDeviceInitializing = 0x2,
    kDeviceStarting = 0x4,
    kDeviceSample16 = 0x10,
    kDeviceSample24 = 0x20,
    kDeviceSample24LE3 = 0x40,
    kDeviceSample32 = 0x80,
    kDeviceSampleHints = kDeviceSample16|kDeviceSample24|kDeviceSample24LE3|kDeviceSample32
};

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

enum BalanceMode {
    kBalanceNormal = 0,
    kBalanceSlowingDown,
    kBalanceSpeedingUp,
};

static inline
const char* BalanceModeToStr(const uint8_t mode)
{
    switch (mode)
    {
    case kBalanceNormal:
        return "kBalanceNormal";
    case kBalanceSlowingDown:
        return "kBalanceSlowingDown";
    case kBalanceSpeedingUp:
        return "kBalanceSpeedingUp";
    }

    return "";
}

// --------------------------------------------------------------------------------------------------------------------

struct DeviceAudio {
    struct Balance {
        uint8_t mode = kBalanceNormal;
        uint8_t unused[3];
        uint16_t slowingDown = 0;
        uint16_t speedingUp = 0;
        double ratio = 1.0;
    } balance;

    struct TimeStamps {
        uint64_t alsaStartTime = 0;
        uint32_t jackStartFrame = 0;
        double ratio = 1.0;
    } timestamps;

    struct HWStatus {
        uint8_t channels;
        uint8_t periods;
        uint16_t periodSize;
        uint16_t bufferSize;
    } hwstatus;

    snd_pcm_t* pcm;
    uint32_t frame;
    uint32_t sampleRate;
    uint16_t bufferSize;
    uint8_t hints;

    struct {
        int8_t* raw;
        float** f32;
    } buffers;

    pthread_t thread;
    sem_t sem;
    HeapRingBuffer* ringbuffers;
};

// --------------------------------------------------------------------------------------------------------------------

DeviceAudio* initDeviceAudio(const char* deviceID, bool playback, uint16_t bufferSize, uint32_t sampleRate);
void runDeviceAudio(DeviceAudio* dev, float* buffers[]);
void closeDeviceAudio(DeviceAudio* dev);

#define DEBUGPRINT(...) { printf(__VA_ARGS__); puts(""); }

// --------------------------------------------------------------------------------------------------------------------
