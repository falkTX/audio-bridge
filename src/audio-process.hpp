// SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

//#define ALSA_PCM_NEW_HW_PARAMS_API
//#define ALSA_PCM_NEW_SW_PARAMS_API
#include <alsa/asoundlib.h>

struct DeviceAudio {
    snd_pcm_t* pcm;
    void* buffer;
    uint16_t bufferSize;
    uint8_t channels;
    uint8_t unused;
    snd_pcm_format_t format = SND_PCM_FORMAT_UNKNOWN;
};

DeviceAudio* initDeviceAudio(const char* deviceID, unsigned bufferSize, unsigned sampleRate);
void runDeviceAudio(DeviceAudio*, float* buffers[2]);
void closeDeviceAudio(DeviceAudio*);
