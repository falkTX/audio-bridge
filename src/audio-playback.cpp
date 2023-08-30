// SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-device-init.hpp"
#include "audio-utils.hpp"

static void* devicePlaybackThread(void* const  arg)
{
    DeviceAudio* const dev = static_cast<DeviceAudio*>(arg);

    const uint8_t hints = dev->hints;
    const uint8_t channels = dev->hwstatus.channels;
    const uint8_t sampleSize = getSampleSizeFromHints(hints);
    const uint16_t bufferSize = dev->bufferSize;
    const uint16_t bufferSizeOver4 = bufferSize / 4;

    float** buffers = new float*[channels];
    for (uint8_t c=0; c<channels; ++c)
        buffers[c] = new float[bufferSize];

    simd::init();

    // smooth initial volume to prevent clicks on start
    ExponentialValueSmoother gain;
    gain.setSampleRate(dev->sampleRate);
    gain.setTimeConstant(3.f);

    VResampler* const resampler = new VResampler;
    resampler->setup(1.0, channels, 8);

    snd_pcm_sframes_t err;
    float xgain;

    // wait for audio thread to post
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 15;

        if (sem_timedwait(&dev->sem, &ts) != 0)
        {
            printf("%08u | playback | audio thread failed to post\n", dev->frame);
            goto end;
        }
    }

    if (dev->hwstatus.channels == 0)
        goto end;

    // write silence until alsa buffers are full
    std::memset(dev->buffers.raw, 0, bufferSize * channels * sampleSize);
    while ((err = snd_pcm_mmap_writei(dev->pcm, dev->buffers.raw, bufferSize)) > 0);

    for (uint8_t loopCount = 1; dev->hwstatus.channels != 0; ++loopCount)
    {
        const uint32_t frame = dev->frame;

        if (dev->hints & kDeviceInitializing)
        {
            // write silence until alsa buffers are full
            bool restarted = false;
            std::memset(dev->buffers.raw, 0, bufferSize * channels * sampleSize);
            while ((err = snd_pcm_mmap_writei(dev->pcm, dev->buffers.raw, bufferSize)) > 0)
                restarted = true;

            if (err != -EAGAIN)
            {
                printf("%08u | playback | initial write error: %s\n", frame, snd_strerror(err));
                goto end;
            }

            if (restarted)
            {
                DEBUGPRINT("%08u | playback | can write data, removing kDeviceInitializing", frame);
                deviceFailInitHints(dev);
                dev->hints &= ~kDeviceInitializing;
                resampler->set_rratio(1.0);
                gain.setTargetValue(0.f);
                gain.clearToTargetValue();
                gain.setTargetValue(1.f);
                loopCount = 1;
            }
            else
            {
                loopCount = 0;
                deviceTimedWait(dev);
                continue;
            }
        }

        if (loopCount == 5)
            loopCount = 1;

       #if 0
        // NOTE trick for when not using RT
        // sem_timedwait makes the scheduler put us at the end of the queue and we end up missing the audio timing
        while (dev->ringbuffer->getReadableDataSize() == 0 && (dev->hints & kDeviceInitializing) == 0)
            sched_yield();
       #endif

        if (dev->ringbuffer->getNumReadableSamples() < bufferSizeOver4)
        {
            if (loopCount != 1)
            {
                DEBUGPRINT("%08u | playback | WARNING | long wait outside loopCount 1, %u", frame, loopCount);
            }

            --loopCount;
            deviceTimedWait(dev);
            continue;
        }

        while (!dev->ringbuffer->read(buffers, bufferSizeOver4))
        {
            DEBUGPRINT("%08u | playback | failed reading data", frame);
            sched_yield();
        }

        if (dev->hwstatus.channels == 0)
            break;

        resampler->inp_count = bufferSizeOver4;
        resampler->out_count = bufferSize;
        resampler->inp_data = buffers;
        resampler->out_data = dev->buffers.f32;
        resampler->process();

        uint16_t frames = bufferSize - resampler->out_count;

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

        while (frames != 0)
        {
            err = snd_pcm_mmap_writei(dev->pcm, ptr, frames);
            // DEBUGPRINT("write %d of %u", err, frames);

            if (err == -EAGAIN)
            {
                deviceTimedWait(dev);
                continue;
            }

            if (err < 0)
            {
                deviceFailInitHints(dev);
                resampler->set_rratio(1.0);
                gain.setTargetValue(0.f);
                gain.clearToTargetValue();
                gain.setTargetValue(1.f);
                loopCount = 0;

                printf("%08u | playback | Write error: %s\n", frame, snd_strerror(err));

                if (xrun_recovery(dev->pcm, err) < 0)
                {
                    printf("playback | xrun_recovery error: %s\n", snd_strerror(err));
                    goto end;
                }

                break;
            }

            // FIXME check against snd_pcm_sw_params_set_avail_min ??
            if (static_cast<uint16_t>(err) != frames)
            {
                ptr += err * channels * sampleSize;
                frames -= err;

                DEBUGPRINT("%08u | playback | Incomplete write %ld of %u, %u left",
                           frame, err, bufferSize, frames);

                deviceTimedWait(dev);
                continue;
            }

            if (loopCount != 4 || ptr != dev->buffers.raw)
                break;

            const snd_pcm_sframes_t avail = snd_pcm_avail_update(dev->pcm);

            if (avail < 0)
            {
                DEBUGPRINT("snd_pcm_avail failed");
                break;
            }

            if (dev->hints & kDeviceStarting)
            {
                dev->hints &= ~kDeviceStarting;
            }
            else
            {
                // hw ratio
                const double availratio = avail > bufferSize / 4 ? 1.0001 : 1;
                dev->timestamps.ratio = (availratio + dev->timestamps.ratio * 511) / 512;

                // sw ratio
                const uint32_t rbavail = dev->ringbuffer->getNumReadableSamples();
                const double rbavailratio = rbavail >= bufferSize ? 0.9999 : 1;
                dev->balance.ratio = (rbavailratio + dev->balance.ratio * 511) / 512;

                // combined ratio for dynamic resampling
                resampler->set_rratio(dev->timestamps.ratio * dev->balance.ratio);

                // TESTING DEBUG REMOVE ME
                if (avail > 255 || (d_isNotEqual(rbavailratio, 1.0) && (frame % dev->sampleRate) == 0))
                {
                    DEBUGPRINT("%08u | playback | %.09f = %.09f * %.09f | %3u %3ld",
                                frame, dev->timestamps.ratio * dev->balance.ratio,
                                dev->timestamps.ratio, dev->balance.ratio,
                                rbavail, avail);
                }
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
}

static void runDeviceAudioPlayback(DeviceAudio* const dev, float* buffers[], const uint32_t frame)
{
    if (dev->hints & kDeviceInitializing)
    {
        sem_post(&dev->sem);
        return;
    }

    if (dev->ringbuffer->getNumWritableSamples() < dev->bufferSize)
    {
        DEBUGPRINT("%08u | playback | buffer full, adding kDeviceInitializing", frame);
        dev->hints |= kDeviceInitializing|kDeviceStarting;
        dev->ringbuffer->flush();
        return;
    }

    while (!dev->ringbuffer->write(buffers, dev->bufferSize))
    {
        sem_post(&dev->sem);
        sched_yield();
        sched_yield();
    }

    sem_post(&dev->sem);
}
