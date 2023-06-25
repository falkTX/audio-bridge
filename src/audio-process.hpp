// SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

//#define ALSA_PCM_NEW_HW_PARAMS_API
//#define ALSA_PCM_NEW_SW_PARAMS_API
#include <alsa/asoundlib.h>

struct DeviceAudio {
    snd_pcm_t* pcm;
    unsigned bufferSize;
    unsigned channels;
    snd_pcm_format_t format = SND_PCM_FORMAT_UNKNOWN;
    void* buffer;
};

DeviceAudio* initDeviceAudio(const char* deviceID, unsigned bufferSize, unsigned sampleRate);
void runDeviceAudio(DeviceAudio*, float* buffers[2]);
void closeDeviceAudio(DeviceAudio*);
