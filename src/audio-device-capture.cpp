// SPDX-FileCopyrightText: 2021-2025 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-device.hpp"
#include "audio-device-impl.hpp"
#include "audio-utils.hpp"

// #include <algorithm>

void runAudioDeviceCaptureThreadImpl(AudioDevice::Impl* const impl)
{
#if 0
    const uint8_t hints = dev->hints;
    const uint8_t channels = dev->hwstatus.channels;
    const uint16_t bufferSize = dev->bufferSize;

    float** buffers = new float*[channels];
    for (uint8_t c=0; c<channels; ++c)
        buffers[c] = new float[bufferSize * 2 * AUDIO_BRIDGE_CAPTURE_BLOCK_SIZE_MULT];

    simd::init();

    // smooth initial volume to prevent clicks on start
    ExponentialValueSmoother gain;
    gain.setSampleRate(dev->sampleRate);
    gain.setTimeConstant(0.5f);

    VResampler* const resampler = new VResampler;
    resampler->setup(1.0, channels, 8);

    snd_pcm_sframes_t err;
    float xgain;
    double rbRatio = 0.0;
    bool enabled = true;

    auto restart = [&dev, &resampler, &gain, &enabled]()
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

            if (err == -EPIPE)
            {
                snd_pcm_prepare(dev->pcm);
                // printf("%08u | capture | initial pipe error: %s\n", frame, snd_strerror(err));
                // started = false;
            }
            else if (err != -EAGAIN)
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
            case -EPIPE:
                DEBUGPRINT("%08u | capture | EPIPE while kDeviceStarting", frame);
                snd_pcm_prepare(dev->pcm);
                deviceTimedWait(dev);
                continue;
            default:
                printf("%08u | capture | initial read error: %s\n", frame, snd_strerror(err));
                goto end;
            }
        }

        err = snd_pcm_mmap_readi(dev->pcm, dev->buffers.raw, bufferSize * AUDIO_BRIDGE_CAPTURE_BLOCK_SIZE_MULT);

        if (dev->hwstatus.channels == 0)
            break;

        switch (err)
        {
        case -EPIPE:
            snd_pcm_prepare(dev->pcm);
            // fall-through
        case -EAGAIN:
            deviceTimedWait(dev);
            continue;
        case 0:
            // deviceTimedWait(dev);
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

            continue;
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

        if (rbRatio != dev->rbRatio)
        {
            rbRatio = dev->rbRatio;
            resampler->set_rratio(rbRatio);
        }

        resampler->inp_count = err;
        resampler->out_count = bufferSize * 2 * AUDIO_BRIDGE_CAPTURE_BLOCK_SIZE_MULT;
        resampler->inp_data = dev->buffers.f32;
        resampler->out_data = buffers;
        resampler->process();

        uint32_t frames = bufferSize * 2 * AUDIO_BRIDGE_CAPTURE_BLOCK_SIZE_MULT - resampler->out_count;

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

    delete resampler;

    for (uint8_t c=0; c<channels; ++c)
        delete[] buffers[c];
    delete[] buffers;

    dev->thread = 0;
    return nullptr;
#endif
}

#if 0
static inline
void clearCaptureBuffers(DeviceAudio* const dev, float* buffers[])
{
    const uint16_t bufferSize = dev->bufferSize;

    for (uint8_t c=0; c < dev->hwstatus.channels; ++c)
        std::memset(buffers[c], 0, sizeof(float) * bufferSize);
}
#endif

void runAudioDeviceCaptureImpl(AudioDevice::Impl* const impl, float* buffers[])
{
#if 0
    const uint16_t bufferSize = dev->bufferSize;

    sem_post(&dev->sem);

    if (dev->hints & kDeviceBuffering)
    {
        clearCaptureBuffers(dev, buffers);
        dev->framesDone = 0;
        dev->rbRatio = 1.0;
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
#endif
}
