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
                             const uint32_t sampleRate,
                             const bool enabled)
{
    AudioDevice* const dev = new AudioDevice;
    dev->config.deviceID = strdup(deviceID);
    dev->config.playback = playback;
    dev->config.bufferSize = bufferSize;
    dev->config.sampleRate = sampleRate;

    dev->enabled = enabled;
    dev->proc.state = kDeviceInitializing;

    if ((dev->impl = initAudioDeviceImpl(dev, dev->hwconfig)) == nullptr)
    {
        delete dev;
        return nullptr;
    }

    const uint8_t numChannels = dev->hwconfig.numChannels;

    dev->proc.resampler = new VResampler;
    dev->proc.resampler->setup(static_cast<double>(dev->hwconfig.sampleRate) / sampleRate, numChannels, 8);

//     const uint32_t numBlocks = (playback ? AUDIO_BRIDGE_PLAYBACK_RINGBUFFER_BLOCKS
//                                          : AUDIO_BRIDGE_CAPTURE_RINGBUFFER_BLOCKS);

    dev->proc.ringbuffer = new AudioRingBuffer;
    dev->proc.ringbuffer->createBuffer(dev->hwconfig.numChannels, std::max(sampleRate, dev->hwconfig.sampleRate) * 2);

    dev->proc.buffers = new float* [numChannels];

    for (uint8_t c = 0; c < numChannels; ++c)
    {
        dev->proc.buffers[c] = new float[bufferSize * 4];
        std::memset(dev->proc.buffers[c], 0, sizeof(float) * bufferSize * 4);
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

        if (dev->proc.state == kDeviceRunning)
        {
            dev->proc.resampler->inp_count = numFrames;
            dev->proc.resampler->out_count = bufferSize * 4;
            dev->proc.resampler->inp_data = buffers;
            dev->proc.resampler->out_data = dev->proc.buffers;
            dev->proc.resampler->process();
            DISTRHO_SAFE_ASSERT(dev->proc.resampler->inp_count == 0);

            const uint16_t resampledFrames = bufferSize * 4 - dev->proc.resampler->out_count;

            const bool ok = dev->proc.ringbuffer->write(dev->proc.buffers, resampledFrames);
            DISTRHO_SAFE_ASSERT(ok);

//         dev->framesDone += bufferSize;
//         setDeviceTimings(dev);
        }
    }
    else
    {
//         runAudioDeviceCaptureImpl(dev->impl, buffers);

        bool ok;
        if (dev->proc.state == kDeviceRunning)
        {
            const uint16_t neededFrames = numFrames;
            ok = dev->proc.ringbuffer->read(dev->proc.buffers, neededFrames);
            // DISTRHO_SAFE_ASSERT(ok);

            if (ok)
            {
                dev->proc.resampler->inp_count = neededFrames;
                dev->proc.resampler->out_count = numFrames;
                dev->proc.resampler->inp_data = dev->proc.buffers;
                dev->proc.resampler->out_data = buffers;
                dev->proc.resampler->process();
                DISTRHO_SAFE_ASSERT(dev->proc.resampler->inp_count == 0);
                DISTRHO_SAFE_ASSERT(dev->proc.resampler->out_count == 0);
            }
            else
            {
                dev->proc.state = kDeviceBuffering;
            }
        }
        else
        {
            ok = false;
        }

        static bool lastok = false;
        if (lastok != ok)
        {
            DEBUGPRINT("-------------------------------------- is ok %d", ok);
            lastok = ok;
        }

        if (!ok)
        {
            for (uint8_t c = 0; c < dev->hwconfig.numChannels; ++c)
                std::memset(buffers[c], 0, sizeof(float) * numFrames);
        }
    }

    return runAudioDevicePostImpl(dev->impl);
}

void closeAudioDevice(AudioDevice* const dev)
{
    closeAudioDeviceImpl(dev->impl);

    delete dev->proc.resampler;
    delete dev->proc.ringbuffer;

    for (uint8_t c = 0; c < dev->hwconfig.numChannels; ++c)
        delete[] dev->proc.buffers[c];

    delete[] dev->proc.buffers;

    std::free(dev->config.deviceID);

    delete dev;
}

// --------------------------------------------------------------------------------------------------------------------
