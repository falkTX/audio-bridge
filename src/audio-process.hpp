// SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

//#define ALSA_PCM_NEW_HW_PARAMS_API
//#define ALSA_PCM_NEW_SW_PARAMS_API
#include <alsa/asoundlib.h>
#include <jack/ringbuffer.h>

#include "zita-resampler/vresampler.h"

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

struct DeviceAudio {
    snd_pcm_t* pcm;
    uint32_t frame;
    uint16_t bufferSize;
    uint8_t channels;
    uint8_t hints;
    int8_t* buffer;
    VResampler* resampler[2];
    float* buffer2[2];
};

DeviceAudio* initDeviceAudio(const char* deviceID, bool playback, uint16_t bufferSize, uint32_t sampleRate);
void getDeviceAudio(DeviceAudio* dev, float* buffers[2]);
void runDeviceAudio(DeviceAudio* dev, float* buffers[2]);
void closeDeviceAudio(DeviceAudio* dev);
