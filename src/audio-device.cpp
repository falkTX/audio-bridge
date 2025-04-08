// SPDX-FileCopyrightText: 2021-2025 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-device.hpp"
#include "audio-device-impl.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

// --------------------------------------------------------------------------------------------------------------------

AudioDevice* initAudioDevice(const char* const deviceID,
                             const bool playback,
                             const uint16_t bufferSize,
                             const uint32_t sampleRate)
{
    AudioDevice* const dev = new AudioDevice;
    dev->config.deviceID = strdup(deviceID);
    dev->config.playback = playback;
    dev->config.bufferSize = bufferSize;
    dev->config.sampleRate = sampleRate;

    if ((dev->impl = initAudioDeviceImpl(dev, dev->hwconfig)) == nullptr)
    {
        delete dev;
        return nullptr;
    }

    const uint8_t numChannels = dev->hwconfig.numChannels;

    dev->resampler = new VResampler;
    dev->resampler->setup(static_cast<double>(dev->hwconfig.sampleRate) / sampleRate, numChannels, 8);

//     const uint32_t numBlocks = (playback ? AUDIO_BRIDGE_PLAYBACK_RINGBUFFER_BLOCKS
//                                          : AUDIO_BRIDGE_CAPTURE_RINGBUFFER_BLOCKS);

    dev->ringbuffer.createBuffer(dev->hwconfig.numChannels, std::max(sampleRate, dev->hwconfig.sampleRate) * 2);

    dev->buffers.f32 = new float* [numChannels];

    for (uint8_t c = 0; c < numChannels; ++c)
    {
        dev->buffers.f32[c] = new float[bufferSize * 4];
        std::memset(dev->buffers.f32[c], 0, sizeof(float) * bufferSize * 4);
    }

    return dev;
}

bool runAudioDevice(AudioDevice* const dev, float* buffers[], const uint16_t numFrames)
{
    const uint16_t bufferSize = dev->config.bufferSize;

    if (dev->config.playback)
    {
        // runAudioDevicePlaybackImpl(dev->impl, buffers);

        // sem_post(&dev->sem);

//         if (dev->state <= kDeviceStarting)
//         {
//             dev->framesDone = 0;
//             dev->rbRatio = 1.0;
//             return;
//         }
//
//         if (dev->ringbuffer->getNumWritableSamples() < bufferSize)
//         {
//             DEBUGPRINT("%08u | playback | ringbuffer full, adding kDeviceInitializing|kDeviceStarting|kDeviceBuffering", frame);
//             deviceFailInitHints(dev);
//             return;
//         }

        dev->resampler->inp_count = numFrames;
        dev->resampler->out_count = bufferSize * 4;
        dev->resampler->inp_data = buffers;
        dev->resampler->out_data = dev->buffers.f32;
        dev->resampler->process();
        DISTRHO_SAFE_ASSERT(dev->resampler->inp_count == 0);

        const uint16_t resampledFrames = bufferSize * 4 - dev->resampler->out_count;

        const bool ok = dev->ringbuffer.write(dev->buffers.f32, resampledFrames);
        DISTRHO_SAFE_ASSERT(ok);

//         dev->framesDone += bufferSize;
//         setDeviceTimings(dev);
    }
    else
    {
        runAudioDeviceCaptureImpl(dev->impl, buffers);
    }

    return runAudioDevicePostImpl(dev->impl);
}

void closeAudioDevice(AudioDevice* const dev)
{
    closeAudioDeviceImpl(dev->impl);

    delete dev->resampler;
    dev->ringbuffer.deleteBuffer();

    for (uint8_t c = 0; c < dev->hwconfig.numChannels; ++c)
        delete[] dev->buffers.f32[c];

    delete[] dev->buffers.f32;

    std::free(dev->config.deviceID);

    delete dev;
}

// --------------------------------------------------------------------------------------------------------------------
