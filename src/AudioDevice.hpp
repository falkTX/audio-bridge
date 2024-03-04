// SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#if defined(__APPLE__)
typedef void* d_audio_device;
#else
//#define ALSA_PCM_NEW_HW_PARAMS_API
//#define ALSA_PCM_NEW_SW_PARAMS_API
#include <alsa/asoundlib.h>
typedef snd_pcm_t* d_audio_device;
#endif

static inline
void audio_device_close(d_audio_device* const dev)
{
   #if defined(__APPLE__)
   #else
    return snd_pcm_close(dev->pcm) == 0;
   #endif
}
