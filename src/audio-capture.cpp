// SPDX-FileCopyrightText: 2021-2024 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-device-init.hpp"
#include "audio-utils.hpp"

#include <algorithm>

static void* deviceCaptureThread(void* const  arg)
{
    DeviceAudio* const dev = static_cast<DeviceAudio*>(arg);

    const uint8_t hints = dev->hints;
    const uint8_t channels = dev->hwstatus.channels;
    const uint16_t bufferSize = dev->bufferSize;

    float** buffers = new float*[channels];

    simd::init();

    // smooth initial volume to prevent clicks on start
    ExponentialValueSmoother gain;
    gain.setSampleRate(dev->sampleRate);
    gain.setTimeConstant(0.5f);

   #ifdef WITH_RESAMPLER
    VResampler* const resampler = new VResampler;
    resampler->setup(1.0, channels, 8);
    double rbRatio = 0.0;

    for (uint8_t c=0; c<channels; ++c)
        buffers[c] = new float[bufferSize * 2];
   #else
    for (uint8_t c=0; c<channels; ++c)
        buffers[c] = dev->buffers.f32[c];
   #endif

    snd_pcm_sframes_t err;
    float xgain;
    bool enabled = true;

    auto restart = [&dev, &gain, &enabled]()
    {
        deviceFailInitHints(dev);
        gain.setTargetValue(0.f);
        gain.clearToTargetValue();
        if (enabled)
            gain.setTargetValue(1.f);
    };

    // wait for audio thread to post
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 15;

        if (sem_timedwait(&dev->sem, &ts) != 0)
        {
            printf("%08u | capture | audio thread failed to post\n", dev->frame);
            goto end;
        }
    }

    while (dev->hwstatus.channels != 0)
    {
        const uint32_t frame = dev->frame;

        if (dev->hints & kDeviceInitializing)
        {
            // read until alsa buffers are empty
            bool started = false;
            while ((err = snd_pcm_mmap_readi(dev->pcm, dev->buffers.raw, bufferSize * 2)) > 0)
                started = true;

            if (err != -EAGAIN)
            {
                printf("%08u | capture | initial read error: %s\n", frame, snd_strerror(err));
                goto end;
            }

            if (started)
            {
                DEBUGPRINT("%08u | capture | can read data? removing kDeviceInitializing", frame);
                restart();
                dev->hints &= ~kDeviceInitializing;
            }
            else
            {
                deviceTimedWait(dev);
                continue;
            }
        }

        if (dev->hints & kDeviceStarting)
        {
            // try reading a single sample to see if device is running
            err = snd_pcm_mmap_readi(dev->pcm, dev->buffers.raw, 1);

            switch (err)
            {
            case 1:
                DEBUGPRINT("%08u | capture | can read data, removing kDeviceStarting", frame);
                dev->hints &= ~kDeviceStarting;
                snd_pcm_rewind(dev->pcm, 1);
                break;
            case -EAGAIN:
                deviceTimedWait(dev);
                continue;
            default:
                printf("%08u | capture | initial read error: %s\n", frame, snd_strerror(err));
                goto end;
            }
        }

        err = snd_pcm_mmap_readi(dev->pcm, dev->buffers.raw, bufferSize);

        if (dev->hwstatus.channels == 0)
            break;

        if (err == 0 || err == -EAGAIN)
        {
            deviceTimedWait(dev);
            continue;
        }

        if (err < 0)
        {
            restart();

            /*
            for (uint8_t c=0; c<channels; ++c)
                dev->ringbuffer->clearData();
            */

            DEBUGPRINT("%08u | capture | Read error %s", frame, snd_strerror(err));

            // TODO offline recovery
            if (xrun_recovery(dev->pcm, err) < 0)
            {
                printf("%08u | capture | xrun_recovery error: %s\n", frame, snd_strerror(err));
                goto end;
            }

            break;
        }

        switch (hints & kDeviceSampleHints)
        {
        case kDeviceSample16:
            int2float::s16(dev->buffers.f32, dev->buffers.raw, channels, err);
            break;
        case kDeviceSample24:
            int2float::s24(dev->buffers.f32, dev->buffers.raw, channels, err);
            break;
        case kDeviceSample24LE3:
            int2float::s24le3(dev->buffers.f32, dev->buffers.raw, channels, err);
            break;
        case kDeviceSample32:
            int2float::s32(dev->buffers.f32, dev->buffers.raw, channels, err);
            break;
        }

        if (enabled != dev->enabled)
        {
            enabled = dev->enabled;
            gain.setTargetValue(enabled ? 1.f : 0.f);
        }

       #ifdef WITH_RESAMPLER
        if (rbRatio != dev->rbRatio)
        {
            rbRatio = dev->rbRatio;
            resampler->set_rratio(rbRatio);
        }

        resampler->inp_count = err;
        resampler->out_count = bufferSize * 2;
        resampler->inp_data = dev->buffers.f32;
        resampler->out_data = buffers;
        resampler->process();

        uint32_t frames = bufferSize * 2 - resampler->out_count;
       #else
        uint32_t frames = err;
       #endif

        for (uint16_t i=0; i<frames; ++i)
        {
            xgain = gain.next();
            for (uint8_t c=0; c<channels; ++c)
                buffers[c][i] *= xgain;
        }

        while (dev->hwstatus.channels != 0 && frames != 0)
        {
            const uint32_t rbavail = std::min<uint32_t>(frames, dev->ringbuffer->getNumWritableSamples());

            if (rbavail == 0)
            {
                deviceTimedWait(dev);
                continue;
            }

            while (!dev->ringbuffer->write(buffers, rbavail))
            {
                DEBUGPRINT("%08u | capture | failed writing data", frame);
                sched_yield();
            }

            if ((dev->hints & kDeviceBuffering) != 0
                && dev->ringbuffer->getNumReadableSamples() > bufferSize * AUDIO_BRIDGE_CAPTURE_LATENCY_BLOCKS)
            {
                DEBUGPRINT("%08u | capture | wrote enough data, removing kDeviceBuffering", frame);
                dev->hints &= ~kDeviceBuffering;
            }

            if (rbavail != frames)
            {
                DEBUGPRINT("%08u | capture | Incomplete write %u of %u", frame, rbavail, frames);

                frames -= rbavail;

                deviceTimedWait(dev);
                continue;
            }

            break;
        }
    }

end:
    DEBUGPRINT("%08u | capture | audio thread closed", dev->frame);

   #ifdef WITH_RESAMPLER
    delete resampler;

    for (uint8_t c=0; c<channels; ++c)
        delete[] buffers[c];
   #endif

    delete[] buffers;

    dev->thread = 0;
    return nullptr;
}

static inline
void clearCaptureBuffers(DeviceAudio* const dev, float* buffers[])
{
    const uint16_t bufferSize = dev->bufferSize;

    for (uint8_t c=0; c < dev->hwstatus.channels; ++c)
        std::memset(buffers[c], 0, sizeof(float) * bufferSize);
}

static void runDeviceAudioCapture(DeviceAudio* const dev, float* buffers[], const uint32_t frame)
{
    const uint16_t bufferSize = dev->bufferSize;

    sem_post(&dev->sem);

    if (dev->hints & kDeviceBuffering)
    {
        clearCaptureBuffers(dev, buffers);
        dev->framesDone = 0;
       #ifdef WITH_RESAMPLER
        dev->rbRatio = 1.0;
       #endif
        return;
    }

    if (dev->ringbuffer->getNumReadableSamples() < bufferSize)
    {
        DEBUGPRINT("%08u | capture | buffer empty, adding kDeviceInitializing|kDeviceStarting|kDeviceBuffering", frame);
        clearCaptureBuffers(dev, buffers);
        deviceFailInitHints(dev);
        return;
    }

    DISTRHO_SAFE_ASSERT_RETURN(dev->ringbuffer->read(buffers, bufferSize), clearCaptureBuffers(dev, buffers));

    dev->framesDone += bufferSize;
    setDeviceTimings(dev);
}
