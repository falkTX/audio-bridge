// SPDX-FileCopyrightText: 2021-2025 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-device.hpp"
#include "audio-device-impl.hpp"

void runAudioDevicePlaybackThreadImpl(AudioDevice::Impl* const impl)
{
#if 0
    const uint8_t hints = dev->hints;
    const uint8_t channels = dev->hwstatus.channels;
    const uint8_t sampleSize = getSampleSizeFromHints(hints);
    const uint16_t bufferSize = dev->bufferSize;

    float** buffers = new float*[channels];
    for (uint8_t c=0; c<channels; ++c)
        buffers[c] = new float[bufferSize];

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
#endif

#if 0
    // wait for audio thread to post
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 15;

        if (sem_timedwait(&impl->sem, &ts) != 0)
        {
            printf("%08u | audio thread failed to post\n", impl->frame);
            goto end;
        }
    }

    while (dev->hwstatus.channels != 0)
    {
        const uint32_t frame = dev->frame;

        if (dev->hints & kDeviceInitializing)
        {
            // write silence until alsa buffers are full
            bool started = false;
            std::memset(dev->buffers.raw, 0, sampleSize * bufferSize * channels * 2);
            while ((err = snd_pcm_mmap_writei(dev->pcm, dev->buffers.raw, bufferSize * 2)) > 0)
                started = true;

            if (err != -EAGAIN)
            {
                printf("%08u | playback | initial write error: %s\n", frame, snd_strerror(err));
                goto end;
            }

            if (started)
            {
                DEBUGPRINT("%08u | playback | can write data? removing kDeviceInitializing", frame);
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
            // try writing a single sample to see if device is running
            std::memset(dev->buffers.raw, 0, sampleSize * channels);
            err = snd_pcm_mmap_writei(dev->pcm, dev->buffers.raw, 1);

            switch (err)
            {
            case 1:
                DEBUGPRINT("%08u | playback | wrote data, removing kDeviceStarting", frame);
                dev->hints &= ~kDeviceStarting;
                snd_pcm_rewind(dev->pcm, 1);
                break;
            case -EAGAIN:
                deviceTimedWait(dev);
                continue;
            default:
                printf("%08u | playback | initial write error: %s\n", frame, snd_strerror(err));
                goto end;
            }
        }

        if (dev->ringbuffer->getNumReadableSamples() < bufferSize)
        {
            deviceTimedWait(dev);
            continue;
        }

        while (!dev->ringbuffer->read(buffers, bufferSize))
        {
            DEBUGPRINT("%08u | playback | WARNING | failed reading data", frame);
            sched_yield();
        }

        if (dev->hwstatus.channels == 0)
            break;

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

        resampler->inp_count = bufferSize;
        resampler->out_count = bufferSize * 2;
        resampler->inp_data = buffers;
        resampler->out_data = dev->buffers.f32;
        resampler->process();

        uint16_t frames = bufferSize * 2 - resampler->out_count;

        for (uint16_t i=0; i<frames; ++i)
        {
            xgain = gain.next();
            for (uint8_t c=0; c<channels; ++c)
                dev->buffers.f32[c][i] *= xgain;
        }

        switch (hints & kDeviceSampleHints)
        {
        case kDeviceSample16:
            float2int::s16(dev->buffers.raw, dev->buffers.f32, channels, frames);
            break;
        case kDeviceSample24:
            float2int::s24(dev->buffers.raw, dev->buffers.f32, channels, frames);
            break;
        case kDeviceSample24LE3:
            float2int::s24le3(dev->buffers.raw, dev->buffers.f32, channels, frames);
            break;
        case kDeviceSample32:
            float2int::s32(dev->buffers.raw, dev->buffers.f32, channels, frames);
            break;
        default:
            DEBUGPRINT("unknown format");
            break;
        }

        int8_t* ptr = dev->buffers.raw;

        while (dev->hwstatus.channels != 0 && frames != 0)
        {
            err = snd_pcm_mmap_writei(dev->pcm, ptr, frames);
            // DEBUGPRINT("write %d of %u", err, frames);

            if (err < 0)
            {
                if (err == -EAGAIN)
                {
                    deviceTimedWait(dev);
                    continue;
                }

                restart();

                printf("%08u | playback | Write error: %s\n", frame, snd_strerror(err));

                if (xrun_recovery(dev->pcm, err) < 0)
                {
                    printf("playback | xrun_recovery error: %s\n", snd_strerror(err));
                    goto end;
                }

                break;
            }

            if (dev->hints & kDeviceBuffering)
            {
                DEBUGPRINT("%08u | playback | wrote data, removing kDeviceBuffering", frame);
                dev->hints &= ~kDeviceBuffering;
            }

            // FIXME check against snd_pcm_sw_params_set_avail_min ??
            if (static_cast<uint16_t>(err) != frames)
            {
                DEBUGPRINT("%08u | playback | Incomplete write %ld of %u", frame, err, frames);

                ptr += err * channels * sampleSize;
                frames -= err;

                deviceTimedWait(dev);
                continue;
            }

            break;
        }
    }

end:
    DEBUGPRINT("%08u | playback | audio thread closed", dev->frame);

    delete resampler;

    for (uint8_t c=0; c<channels; ++c)
        delete[] buffers[c];
    delete[] buffers;

    dev->thread = 0;
    return nullptr;
#endif
}

void runAudioDevicePlaybackImpl(AudioDevice::Impl* const impl, float* buffers[])
{
#if 0
#endif
}
