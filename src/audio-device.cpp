// SPDX-FileCopyrightText: 2021-2025 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-device.hpp"
#include "audio-device-impl.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#if ! AUDIO_BRIDGE_DEBUG
#undef DISTRHO_SAFE_ASSERT
#define DISTRHO_SAFE_ASSERT(x)
#endif

// --------------------------------------------------------------------------------------------------------------------

#if AUDIO_BRIDGE_ASYNC

// reset resampler filters and process 1 empty buffer as to reduce initial latency
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

// reset audio device stats and resampling ratio
// triggered from kDeviceResetStats and kDeviceResetFull
static inline
void resetAudioDeviceStats(AudioDevice* const dev)
{
    dev->stats.framesDone = 0;
    dev->hostproc.leftoverResampledFrames = 0;

   #if AUDIO_BRIDGE_INITIAL_LEVEL_SMOOTHING
    if (dev->hostproc.gainEnabled)
    {
        dev->hostproc.gainEnabled = false;
        dev->hostproc.gain.setTargetValue(0.f);
        dev->hostproc.gain.clearToTargetValue();
    }
   #endif

   #if ! AUDIO_BRIDGE_UDEV
    if (dev->stats.rbRatio != 1.0)
    {
        DEBUGPRINT("resetAudioDeviceStats to initial 1.0 ratio");
        dev->stats.rbRatio = 1.0;
        dev->hostproc.resampler->set_rratio(1.0);
        clearAudioDeviceResampler(dev);
    }
   #endif
}

// reset/empty audio device ringbuffer
// triggered from kDeviceResetFull
static inline
void resetAudioDeviceRingBuffer(AudioDevice* const dev)
{
    pthread_mutex_lock(&dev->proc.ringbufferLock);
    dev->proc.ringbuffer->flush();
    pthread_mutex_unlock(&dev->proc.ringbufferLock);
}

#endif // AUDIO_BRIDGE_ASYNC

// --------------------------------------------------------------------------------------------------------------------

AudioDevice* initAudioDevice(const char* const deviceID,
                             const uint16_t bufferSize,
                             const uint32_t sampleRate,
                             const bool playback,
                             const bool enabled [[maybe_unused]])
{
    AudioDevice* const dev = new AudioDevice;
    dev->config.deviceID = strdup(deviceID);
    dev->config.playback = playback;
    dev->config.bufferSize = bufferSize;
    dev->config.sampleRate = sampleRate;

  #if AUDIO_BRIDGE_ASYNC
    dev->proc.state = kDeviceInitializing;
   #if AUDIO_BRIDGE_UDEV
    dev->proc.ppm = 0;
   #endif
  #endif
   #if AUDIO_BRIDGE_INITIAL_LEVEL_SMOOTHING
    dev->enabled = enabled;
   #endif

    if ((dev->impl = initAudioDeviceImpl(dev, dev->hwconfig)) == nullptr)
    {
        delete dev;
        return nullptr;
    }

  #if AUDIO_BRIDGE_ASYNC
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

   #if AUDIO_BRIDGE_INITIAL_LEVEL_SMOOTHING
    dev->hostproc.gain.setSampleRate(sampleRate);
    dev->hostproc.gain.setTimeConstant(0.5f);
    dev->hostproc.gainEnabled = true; // initial value to force flush to zero
   #endif

    dev->hostproc.resampler = new VResampler;
    dev->hostproc.resampler->setup(playback ? static_cast<double>(dev->hwconfig.sampleRate) / sampleRate
                                            : static_cast<double>(sampleRate) / dev->hwconfig.sampleRate,
                                   numChannels,
                                   AUDIO_BRIDGE_RESAMPLE_QUALITY);

    dev->hostproc.leftoverResampledFrames = 0;
    dev->hostproc.tempBufferSize = bufferSize * 4;
    dev->hostproc.tempBuffers = new float* [numChannels];
    dev->hostproc.tempBuffers2 = new float* [numChannels];

    for (uint8_t c = 0; c < numChannels; ++c)
        dev->hostproc.tempBuffers[c] = new float [dev->hostproc.tempBufferSize];

    dev->stats.framesDone = 0;
   #if AUDIO_BRIDGE_UDEV
    dev->stats.ppm = 0;
   #elif AUDIO_BRIDGE_ASYNC
    dev->stats.rbFillTarget = dev->proc.numBufferingSamples / kRingBufferDataFactor;
    dev->stats.rbRatio = 1.0;
   #endif

    clearAudioDeviceResampler(dev);
  #endif

    return dev;
}

bool runAudioDevice(AudioDevice* const dev, float* buffers[], const uint16_t numFrames)
{
    bool ok = false;
    const uint8_t numChannels = dev->hwconfig.numChannels;

#if AUDIO_BRIDGE_ASYNC
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
   #if AUDIO_BRIDGE_INITIAL_LEVEL_SMOOTHING
    float gain;
   #endif

    if (dev->config.playback)
    {
        if (state == kDeviceStarted)
        {
            DEBUGPRINT("%010u | playback | host is running, kDeviceStarted -> kDeviceBuffering", dev->stats.framesDone);
            dev->proc.state.store(kDeviceBuffering);
        }
        else if (state >= kDeviceBuffering)
        {
           #if AUDIO_BRIDGE_INITIAL_LEVEL_SMOOTHING
            if (dev->hostproc.gainEnabled != dev->enabled)
            {
                dev->hostproc.gainEnabled = dev->enabled;
                dev->hostproc.gain.setTargetValue(dev->enabled ? 1.f : 0.f);
            }
           #endif

            const uint32_t tempBufferSize = dev->hostproc.tempBufferSize;

            dev->hostproc.resampler->inp_count = numFrames;
            dev->hostproc.resampler->out_count = tempBufferSize;
            dev->hostproc.resampler->inp_data = buffers;
            dev->hostproc.resampler->out_data = tempBuffers;
            dev->hostproc.resampler->process();
            DISTRHO_SAFE_ASSERT(dev->hostproc.resampler->inp_count == 0);

            const uint16_t resampledFrames = tempBufferSize - dev->hostproc.resampler->out_count;

           #if AUDIO_BRIDGE_INITIAL_LEVEL_SMOOTHING
            for (uint16_t i = 0; i < resampledFrames; ++i)
            {
                gain = dev->hostproc.gain.next();

                for (uint8_t c = 0; c < numChannels; ++c)
                    tempBuffers[c][i] *= gain;
            }
           #endif

            pthread_mutex_lock(&dev->proc.ringbufferLock);
            ok = dev->proc.ringbuffer->write(tempBuffers, resampledFrames);
            pthread_mutex_unlock(&dev->proc.ringbufferLock);

            DISTRHO_SAFE_ASSERT(ok);
        }
    }
    else
    {
        if (state == kDeviceStarted)
        {
            DEBUGPRINT("%010u | capture | host is running, kDeviceStarted -> kDeviceBuffering", dev->stats.framesDone);
            dev->proc.state.store(kDeviceBuffering);
        }
        else if (state == kDeviceRunning)
        {
           #if AUDIO_BRIDGE_INITIAL_LEVEL_SMOOTHING
            if (dev->hostproc.gainEnabled != dev->enabled)
            {
                dev->hostproc.gainEnabled = dev->enabled;
                dev->hostproc.gain.setTargetValue(dev->enabled ? 1.f : 0.f);
            }
           #endif

            float** const buffers2 = dev->hostproc.tempBuffers2;
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
                        buffers2[c] = buffers[c] + offset;

                    dev->hostproc.resampler->inp_count = remainingFrames;
                    dev->hostproc.resampler->out_count = remainingFrames;
                    dev->hostproc.resampler->inp_data = tempBuffers;
                    dev->hostproc.resampler->out_data = buffers2;
                    dev->hostproc.resampler->process();

                    if (dev->hostproc.resampler->out_count != 0)
                    {
                        DISTRHO_SAFE_ASSERT(dev->hostproc.resampler->inp_count == 0);

                        const uint16_t resampledFrames = remainingFrames - dev->hostproc.resampler->out_count;

                        offset += resampledFrames;
                        DISTRHO_SAFE_ASSERT(offset <= numFrames);

                       #if AUDIO_BRIDGE_INITIAL_LEVEL_SMOOTHING
                        for (uint16_t i = 0; i < resampledFrames; ++i)
                        {
                            gain = dev->hostproc.gain.next();

                            for (uint8_t c = 0; c < numChannels; ++c)
                                buffers2[c][i] *= gain;
                        }
                       #endif

                        leftoverFrames = 0;
                    }
                    else
                    {
                        const uint16_t resampledFrames = remainingFrames - dev->hostproc.resampler->inp_count;

                        if ((leftoverFrames = dev->hostproc.resampler->inp_count) != 0)
                        {
                            for (uint8_t c = 0; c < numChannels; ++c)
                                std::memmove(tempBuffers[c],
                                             tempBuffers[c] + resampledFrames,
                                             sizeof(float) * leftoverFrames);
                        }

                       #if AUDIO_BRIDGE_INITIAL_LEVEL_SMOOTHING
                        for (uint16_t i = 0; i < resampledFrames; ++i)
                        {
                            gain = dev->hostproc.gain.next();

                            for (uint8_t c = 0; c < numChannels; ++c)
                                buffers2[c][i] *= gain;
                        }
                       #endif

                       #if AUDIO_BRIDGE_DEBUG && 0
                        DEBUGPRINT("%010u | out_count %u, inp_count %u, offset %u, leftoverFrames %u",
                                   dev->stats.framesDone,
                                   dev->hostproc.resampler->out_count,
                                   dev->hostproc.resampler->inp_count,
                                   offset, leftoverFrames);
                       #endif
                        break;
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
#else // AUDIO_BRIDGE_ASYNC
    if (dev->config.playback)
    {
        ok = runAudioDevicePlaybackSyncImpl(dev->impl, buffers, numFrames);
    }
    else
    {
        ok = runAudioDeviceCaptureSyncImpl(dev->impl, buffers, numFrames);

        if (! ok)
        {
            for (uint8_t c = 0; c < numChannels; ++c)
                std::memset(buffers[c], 0, sizeof(float) * numFrames);
        }
    }
#endif // AUDIO_BRIDGE_ASYNC

  #if AUDIO_BRIDGE_DEBUG
    static bool lastok_c = false;
    static bool lastok_p = false;
    if (dev->config.playback)
    {
        if (lastok_p != ok)
        {
            DEBUGPRINT("\n%010u | playback | -------------------------------------- is ok %d", dev->stats.framesDone, ok);
            lastok_p = ok;
        }
    }
    else
    {
        if (lastok_c != ok)
        {
            DEBUGPRINT("\n%010u | capture | -------------------------------------- is ok %d", dev->stats.framesDone, ok);
            lastok_c = ok;
        }
    }

   #if AUDIO_BRIDGE_ASYNC
    static bool print1 = true;
    static bool print2 = true;
   #endif
  #endif

   #if AUDIO_BRIDGE_ASYNC
    if (ok)
    {
        dev->stats.framesDone += numFrames;

      #if AUDIO_BRIDGE_UDEV
        if (dev->stats.ppm != dev->proc.ppm)
        {
            dev->stats.ppm = dev->proc.ppm;
            const double balratio = dev->config.playback
                                  ? 1.0 + static_cast<double>(dev->stats.ppm) / 1000000.0
                                  : 1.0 - static_cast<double>(dev->stats.ppm) / 1000000.0;
            dev->hostproc.resampler->set_rratio(balratio);
            DEBUGPRINT("%010u | drift check %.8f | %u",
                       dev->stats.framesDone,
                       balratio,
                       dev->proc.ringbuffer->getNumReadableSamples());
        }
      #else
        if (state == kDeviceRunning &&
            dev->stats.framesDone > dev->config.sampleRate * AUDIO_BRIDGE_CLOCK_DRIFT_WAIT_DELAY_1)
        {
           #if AUDIO_BRIDGE_DEBUG
            if (print1)
            {
                print1 = false;
                DEBUGPRINT("AUDIO_BRIDGE_CLOCK_DRIFT_WAIT_DELAY_1 reached");
            }
           #endif

            const double rbratio = 2.0 - (
                /* NOTE should we use clamp_ratio here? */
                clamp_ratio(dev->proc.ringbuffer->getNumReadableSamples() / kRingBufferDataFactor / dev->stats.rbFillTarget)
                + AUDIO_BRIDGE_CLOCK_FILTER_STEPS_1 - 1
            ) / AUDIO_BRIDGE_CLOCK_FILTER_STEPS_1;

            const double balratio = std::fmax(0.9, std::fmin(1.1,
                (rbratio + dev->stats.rbRatio * (AUDIO_BRIDGE_CLOCK_FILTER_STEPS_2 - 1)) / AUDIO_BRIDGE_CLOCK_FILTER_STEPS_2
            ));

            if (std::abs(dev->stats.rbRatio - balratio) > 0.000000002)
            {
                dev->stats.rbRatio = balratio;

                if (dev->stats.framesDone > dev->config.sampleRate * AUDIO_BRIDGE_CLOCK_DRIFT_WAIT_DELAY_2)
                {
                   #if AUDIO_BRIDGE_DEBUG
                    if (print2)
                    {
                        print2 = false;
                        DEBUGPRINT("AUDIO_BRIDGE_CLOCK_DRIFT_WAIT_DELAY_2 reached");
                    }
                   #endif
                    dev->hostproc.resampler->set_rratio(dev->stats.rbRatio);
                }
            }
        }
       #if AUDIO_BRIDGE_DEBUG
        static int counter = 0;
        if (++counter == 2000)
        {
            counter = 0;
            DEBUGPRINT("%010u | drift check %f | %f vs %f",
                       dev->stats.framesDone,
                       dev->stats.rbRatio,
                       dev->proc.ringbuffer->getNumReadableSamples() / kRingBufferDataFactor,
                       dev->stats.rbFillTarget);
        }
       #endif
      #endif
    }
    else
    {
       #if AUDIO_BRIDGE_DEBUG
        print1 = print2 = true;
       #endif

        if (dev->config.playback ? state >= kDeviceBuffering : state == kDeviceRunning)
        {
            dev->proc.state.store(kDeviceStarting);
            resetAudioDeviceRingBuffer(dev);
        }

        resetAudioDeviceStats(dev);
    }
   #else // AUDIO_BRIDGE_ASYNC
    if (ok)
        dev->stats.framesDone += numFrames;
   #endif // AUDIO_BRIDGE_ASYNC

    return runAudioDevicePostImpl(dev->impl, numFrames);
}

void closeAudioDevice(AudioDevice* const dev)
{
    closeAudioDeviceImpl(dev->impl);

   #if AUDIO_BRIDGE_ASYNC
    delete dev->proc.ringbuffer;

    delete dev->hostproc.resampler;
    pthread_mutex_destroy(&dev->proc.ringbufferLock);

    for (uint8_t c = 0; c < dev->hwconfig.numChannels; ++c)
        delete[] dev->hostproc.tempBuffers[c];

    delete[] dev->hostproc.tempBuffers;
    delete[] dev->hostproc.tempBuffers2;
   #endif

    std::free(dev->config.deviceID);

    delete dev;
}

// --------------------------------------------------------------------------------------------------------------------
