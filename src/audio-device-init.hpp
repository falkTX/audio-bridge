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

#define USB_GADGET_MODE

// --------------------------------------------------------------------------------------------------------------------

// how many seconds to wait until start trying to compensate for clock drift
#define AUDIO_BRIDGE_CLOCK_DRIFT_WAIT_DELAY 30

// how many steps to use for smoothing the clock-drift compensation filter
#define AUDIO_BRIDGE_CLOCK_FILTER_STEPS 8192

// how many audio buffer-size capture blocks to store until rolling starts
#define AUDIO_BRIDGE_CAPTURE_LATENCY_BLOCKS 3

// how many audio buffer-size blocks to keep in the capture ringbuffer
#define AUDIO_BRIDGE_CAPTURE_RINGBUFFER_BLOCKS 32

// how many audio buffer-size blocks to keep in the playback ringbuffer
#define AUDIO_BRIDGE_PLAYBACK_RINGBUFFER_BLOCKS 4

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

struct DeviceAudio {
    struct Balance {
        double ratio = 1.0;
        int32_t distance = 0;
    } balance;

    struct TimeStamps {
        uint64_t alsaStartTime = 0;
        uint32_t jackStartFrame = 0;
    } timestamps;

    struct HWStatus {
        uint8_t channels;
        uint8_t periods;
        uint16_t periodSize;
        uint16_t fullBufferSize;
    } hwstatus;

    char* deviceID;

    snd_pcm_status_t* statusRT;
   #ifndef USB_GADGET_MODE
    snd_pcm_status_t* status;
    pthread_mutex_t statuslock;
   #endif

    snd_pcm_t* pcm;
    uint32_t frame;
    uint32_t framesDone;
    uint32_t sampleRate;
    uint16_t bufferSize;
    uint8_t hints;

    struct {
        int8_t* raw;
        float** f32;
    } buffers;

   #ifdef USB_GADGET_MODE
    VResampler* resampler;
   #else
    pthread_t thread;
    sem_t sem;
   #endif

    AudioRingBuffer* ringbuffer;
};

// --------------------------------------------------------------------------------------------------------------------

DeviceAudio* initDeviceAudio(const char* deviceID, bool playback, uint16_t bufferSize, uint32_t sampleRate);
bool runDeviceAudio(DeviceAudio* dev, float* buffers[]);
void closeDeviceAudio(DeviceAudio* dev);

#define DEBUGPRINT(...) { printf(__VA_ARGS__); puts(""); }

// --------------------------------------------------------------------------------------------------------------------
