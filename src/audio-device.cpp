// SPDX-FileCopyrightText: 2021-2025 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-device.hpp"
#include "audio-device-impl.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

// --------------------------------------------------------------------------------------------------------------------

static inline
void clearAudioDeviceResampler(AudioDevice* const dev)
{
    const uint32_t bufferSize = dev->config.bufferSize;
    const uint8_t numChannels = dev->hwconfig.numChannels;

    for (uint8_t c = 0; c < numChannels; ++c)
        std::memset(dev->hostproc.tempBuffers[c], 0, sizeof(float) * bufferSize);

    dev->hostproc.resampler->reset();
    dev->hostproc.resampler->inp_count = bufferSize;
    dev->hostproc.resampler->out_count = bufferSize;
    dev->hostproc.resampler->inp_data = dev->hostproc.tempBuffers;
    dev->hostproc.resampler->out_data = dev->hostproc.tempBuffers;
    dev->hostproc.resampler->process();
}

static inline
void resetAudioDeviceStats(AudioDevice* const dev)
{
    dev->stats.framesDone = 0;
    dev->hostproc.leftoverResampledFrames = 0;

    // FIXME not good enough?
    dev->stats.rbRatio = 1.0;
    dev->hostproc.resampler->set_rratio(1.0);

    clearAudioDeviceResampler(dev);
}

static inline
void resetAudioDeviceRingBuffer(AudioDevice* const dev)
{
    pthread_mutex_lock(&dev->proc.ringbufferLock);
    dev->proc.ringbuffer->flush();
    pthread_mutex_unlock(&dev->proc.ringbufferLock);
}

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

    const uint32_t ringbufferSize = std::max(sampleRate, dev->hwconfig.sampleRate);
    dev->proc.ringbuffer = new AudioRingBuffer;
    dev->proc.ringbuffer->createBuffer(numChannels, ringbufferSize);

    {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
        pthread_mutex_init(&dev->proc.ringbufferLock, &attr);
        pthread_mutexattr_destroy(&attr);
    }

    dev->hostproc.resampler = new VResampler;
    dev->hostproc.resampler->setup(static_cast<double>(dev->hwconfig.sampleRate) / sampleRate, numChannels,
                                   AUDIO_BRIDGE_RESAMPLE_QUALITY);

    // TESTING
    // dev->hostproc.resampler->set_rratio(1.1);

    dev->hostproc.leftoverResampledFrames = 0;
    dev->hostproc.tempBufferSize = bufferSize * 4;
    dev->hostproc.tempBuffers = new float* [numChannels];
    dev->hostproc.tempBuffers2 = new float* [numChannels];

    for (uint8_t c = 0; c < numChannels; ++c)
        dev->hostproc.tempBuffers[c] = new float [dev->hostproc.tempBufferSize];

    dev->stats.framesDone = 0;

    dev->stats.rbFillTarget = dev->proc.numBufferingSamples / kRingBufferDataFactor;
    dev->stats.rbRatio = 1.0;

    DEBUGPRINT("Fill target is %f", dev->stats.rbFillTarget);

    clearAudioDeviceResampler(dev);
    return dev;
}

bool runAudioDevice(AudioDevice* const dev, float* buffers[], const uint16_t numFrames)
{
    if (const DeviceReset reset = static_cast<DeviceReset>(dev->proc.reset.load()))
    {
        switch (reset)
        {
        case kDeviceResetNone:
            break;
        case kDeviceResetFull:
            resetAudioDeviceRingBuffer(dev);
            [[fallthrough]];
        case kDeviceResetStats:
            resetAudioDeviceStats(dev);
            break;
        }
        dev->proc.reset.store(kDeviceResetNone);
    }

    const DeviceState state = static_cast<DeviceState>(dev->proc.state.load());
    float** tempBuffers = dev->hostproc.tempBuffers;
    bool ok = false;

    if (dev->config.playback)
    {
        if (state == kDeviceStarted)
        {
            dev->proc.state.store(kDeviceBuffering);
        }
        else if (state >= kDeviceBuffering)
        {
            const uint32_t tempBufferSize = dev->hostproc.tempBufferSize;

            dev->hostproc.resampler->inp_count = numFrames;
            dev->hostproc.resampler->out_count = tempBufferSize;
            dev->hostproc.resampler->inp_data = buffers;
            dev->hostproc.resampler->out_data = tempBuffers;
            dev->hostproc.resampler->process();
            DISTRHO_SAFE_ASSERT(dev->hostproc.resampler->inp_count == 0);

            const uint16_t resampledFrames = tempBufferSize - dev->hostproc.resampler->out_count;

            pthread_mutex_lock(&dev->proc.ringbufferLock);
            ok = dev->proc.ringbuffer->write(tempBuffers, resampledFrames);
            pthread_mutex_unlock(&dev->proc.ringbufferLock);

            DISTRHO_SAFE_ASSERT(ok);
        }
    }
    else
    {
        const uint8_t numChannels = dev->hwconfig.numChannels;

        if (state == kDeviceStarted)
        {
            dev->proc.state.store(kDeviceBuffering);
        }
        else if (state == kDeviceRunning)
        {
            float** const tempBuffers2 = dev->hostproc.tempBuffers2;
            uint32_t leftoverFrames = dev->hostproc.leftoverResampledFrames;

            for (uint32_t offset = 0; offset != numFrames;)
            {
                const uint32_t remainingFrames = numFrames - offset;
                DISTRHO_SAFE_ASSERT(offset < numFrames);
                DISTRHO_SAFE_ASSERT(remainingFrames >= leftoverFrames);
                DISTRHO_SAFE_ASSERT(remainingFrames != 0);

                for (int retry = 0; retry < 5; ++retry)
                {
                    pthread_mutex_lock(&dev->proc.ringbufferLock);
                    ok = dev->proc.ringbuffer->read(tempBuffers, remainingFrames - leftoverFrames, leftoverFrames);
                    pthread_mutex_unlock(&dev->proc.ringbufferLock);

                    if (ok)
                        break;

                    sched_yield();
                }

                DISTRHO_SAFE_ASSERT(ok);

                if (ok)
                {
                    for (uint8_t c = 0; c < numChannels; ++c)
                        tempBuffers2[c] = buffers[c] + offset;

                    dev->hostproc.resampler->inp_count = remainingFrames;
                    dev->hostproc.resampler->out_count = remainingFrames;
                    dev->hostproc.resampler->inp_data = tempBuffers;
                    dev->hostproc.resampler->out_data = tempBuffers2;
                    dev->hostproc.resampler->process();

                    if (dev->hostproc.resampler->out_count != 0)
                    {
                        DISTRHO_SAFE_ASSERT(dev->hostproc.resampler->inp_count == 0);

                        const uint16_t resampledFrames = remainingFrames - dev->hostproc.resampler->out_count;

                        offset += resampledFrames;
                        DISTRHO_SAFE_ASSERT(offset <= numFrames);

                        leftoverFrames = 0;
                    }
                    else
                    {
                        DISTRHO_SAFE_ASSERT(dev->hostproc.resampler->out_count == 0);

                        const uint16_t resampledFrames = remainingFrames - dev->hostproc.resampler->inp_count;

                        offset += resampledFrames;
                        DISTRHO_SAFE_ASSERT(offset <= numFrames);

                        if ((leftoverFrames = dev->hostproc.resampler->inp_count) != 0)
                        {
                            /*
                            DEBUGPRINT("out_count %u, inp_count %u, offset %u, leftoverFrames %u | %f",
                                    dev->hostproc.resampler->out_count,
                                    dev->hostproc.resampler->inp_count,
                                    offset, leftoverFrames,
                                    dev->hostproc.resampler->_ratio);
                            */

                            for (uint8_t c = 0; c < numChannels; ++c)
                                std::memmove(tempBuffers[c],
                                            tempBuffers[c] + resampledFrames,
                                            sizeof(float) * leftoverFrames);
                        }
                    }
                }
                else
                {
                    leftoverFrames = 0;
                    break;
                }
            }
            dev->hostproc.leftoverResampledFrames = leftoverFrames;
        }

        if (! ok)
        {
            for (uint8_t c = 0; c < numChannels; ++c)
                std::memset(buffers[c], 0, sizeof(float) * numFrames);
        }
    }

    static bool lastok = false;
    if (lastok != ok)
    {
        DEBUGPRINT("-------------------------------------- is ok %d", ok);
        lastok = ok;
    }

    static bool print1 = true;
    static bool print2 = true;
    if (ok)
    {
        dev->stats.framesDone += numFrames;

        if (state == kDeviceRunning &&
            dev->stats.framesDone > dev->config.sampleRate * AUDIO_BRIDGE_CLOCK_DRIFT_WAIT_DELAY_1)
        {
            if (print1)
            {
                print1 = false;
                DEBUGPRINT("AUDIO_BRIDGE_CLOCK_DRIFT_WAIT_DELAY_1 reached");
            }

            const double rbratio = 2.0 - (
                /*clamp_ratio*/
                (dev->proc.ringbuffer->getNumReadableSamples() / kRingBufferDataFactor / dev->stats.rbFillTarget)
                + AUDIO_BRIDGE_CLOCK_FILTER_STEPS_1 - 1
            ) / AUDIO_BRIDGE_CLOCK_FILTER_STEPS_1;

            const double balratio = std::fmax(0.9, std::fmin(1.1,
                (rbratio + dev->stats.rbRatio * (AUDIO_BRIDGE_CLOCK_FILTER_STEPS_2 - 1)) / AUDIO_BRIDGE_CLOCK_FILTER_STEPS_2
            ));

            if (std::abs(dev->stats.rbRatio - balratio) > 0.000000002)
            {
                dev->stats.rbRatio = balratio;
                // TESTING
                // dev->stats.rbRatio = 0.999986;
                // dev->stats.rbRatio = 1.1;

                if (dev->stats.framesDone > dev->config.sampleRate * AUDIO_BRIDGE_CLOCK_DRIFT_WAIT_DELAY_2)
                {
                    if (print2)
                    {
                        print2 = false;
                        DEBUGPRINT("AUDIO_BRIDGE_CLOCK_DRIFT_WAIT_DELAY_2 reached");
                    }
                    dev->hostproc.resampler->set_rratio(dev->stats.rbRatio);
                }
            }
        }

        static int counter = 0;
        if (++counter == 2000)
        {
            counter = 0;
            DEBUGPRINT("%08u | drift check %f | %f vs %f",
                       dev->stats.framesDone,
                       dev->stats.rbRatio,
                       dev->proc.ringbuffer->getNumReadableSamples() / kRingBufferDataFactor,
                       dev->stats.rbFillTarget);
        }
    }
    else
    {
        print1 = print2 = true;

        if (state == kDeviceRunning)
        {
            dev->proc.state.store(kDeviceStarting);
            resetAudioDeviceRingBuffer(dev);
        }

        resetAudioDeviceStats(dev);
    }

    return runAudioDevicePostImpl(dev->impl, numFrames);
}

void closeAudioDevice(AudioDevice* const dev)
{
    closeAudioDeviceImpl(dev->impl);

    delete dev->proc.ringbuffer;
    pthread_mutex_destroy(&dev->proc.ringbufferLock);

    delete dev->hostproc.resampler;

    for (uint8_t c = 0; c < dev->hwconfig.numChannels; ++c)
        delete[] dev->hostproc.tempBuffers[c];

    delete[] dev->hostproc.tempBuffers;
    delete[] dev->hostproc.tempBuffers2;

    std::free(dev->config.deviceID);

    delete dev;
}

// --------------------------------------------------------------------------------------------------------------------
