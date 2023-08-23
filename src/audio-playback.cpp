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
    const uint16_t bufferSizeOver4 = dev->bufferSize / 4;

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

        if (loopCount == 5)
            loopCount = 1;

        if (dev->hints & kDeviceInitializing)
        {
            // write silence until alsa buffers are full
            bool restarted = false;
            std::memset(dev->buffers.raw, 0, bufferSize * channels * sampleSize);
            while ((err = snd_pcm_mmap_writei(dev->pcm, dev->buffers.raw, bufferSize)) > 0)
                restarted = true;

            if (restarted)
            {
                DEBUGPRINT("%08u | playback | can write data, removing kDeviceInitializing", frame);
                dev->hints &= ~kDeviceInitializing;
                gain.setTargetValue(0.f);
                gain.clearToTargetValue();
                gain.setTargetValue(1.f);
            }

            if (err != -EAGAIN)
            {
                printf("%08u | playback | initial write error: %s\n", frame, snd_strerror(err));
                goto end;
            }
        }

       #if 0
        // NOTE trick for when not using RT
        // sem_timedwait makes the scheduler put us at the end of the queue and we end up missing the audio timing
        while (dev->ringbuffers[0].getReadableDataSize() == 0 && (dev->hints & kDeviceInitializing) == 0)
            sched_yield();
       #endif

        if (dev->ringbuffers[0].getReadableDataSize() == 0)
        {
            if (loopCount != 1)
            {
                DEBUGPRINT("%08u | playback | WARNING | long wait outside loopCount 1, %u", frame, loopCount);
            }

            --loopCount;
            deviceTimedWait(dev);
            continue;
        }

        for (uint8_t c=0; c<channels; ++c)
        {
            while (!dev->ringbuffers[c].readCustomData(buffers[c], sizeof(float) * bufferSizeOver4))
            {
                DEBUGPRINT("%08u | failed reading data", frame);
                sched_yield();
            }
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

                // common setup
                gain.setTargetValue(0.f);
                gain.clearToTargetValue();
                gain.setTargetValue(1.f);

                printf("%08u | playback | Write error: %s\n", frame, snd_strerror(err));

                if (xrun_recovery(dev->pcm, err) < 0)
                {
                    printf("playback | xrun_recovery error: %s\n", snd_strerror(err));
                    exit(EXIT_FAILURE);
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
                continue;
            }

            if (loopCount == 4 && ptr == dev->buffers.raw)
            {
                snd_pcm_uframes_t avail;
                snd_htimestamp_t ts;

                if (snd_pcm_htimestamp(dev->pcm, &avail, &ts) != 0)
                {
                    DEBUGPRINT("snd_pcm_htimestamp failed");
                    break;
                }

                if (dev->hints & kDeviceStarting)
                {
                    dev->hints &= ~kDeviceStarting;
                }
                else
                {
                    if (dev->timestamps.alsaStartTime == 0)
                    {
                        dev->timestamps.alsaStartTime = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
                        dev->timestamps.jackStartFrame = frame;
                    }
                    else
                    {
                        // hw ratio
                        const uint64_t alsadiff = ts.tv_sec * 1000000000ULL + ts.tv_nsec - dev->timestamps.alsaStartTime;
                        const double alsaframes = static_cast<double>(alsadiff * dev->sampleRate / 1000000000ULL);
                        const double jackframes = static_cast<double>(frame - dev->timestamps.jackStartFrame);
                        dev->timestamps.ratio = ((jackframes / alsaframes) + dev->timestamps.ratio * 511) / 512;

                        // sw ratio
                        const uint32_t availtotal = avail + dev->ringbuffers[0].getReadableDataSize() / sizeof(float);
                        const double availratio = availtotal > bufferSizeOver4 ? 1.0001
                                                : availtotal < bufferSizeOver4 ? 0.9999 : 1;
                        dev->balance.ratio = (availratio + dev->balance.ratio * 511) / 512;

                        // combined ratio for dynamic resampling
                        resampler->set_rratio(dev->timestamps.ratio * dev->balance.ratio);

                        // TESTING DEBUG REMOVE ME
                        if ((frame % dev->sampleRate) == 0 || avail > 255)
                        {
                            DEBUGPRINT("%08u | playback | %.09f = %.09f * %.09f | %3u %3ld",
                                       frame, dev->timestamps.ratio * dev->balance.ratio,
                                       dev->timestamps.ratio, dev->balance.ratio,
                                       availtotal, avail);
                        }
                    }
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

    return nullptr;
}

static void runDeviceAudioPlayback(DeviceAudio* const dev, float* buffers[], const uint32_t frame)
{
    if (dev->hints & kDeviceInitializing)
    {
        sem_post(&dev->sem);
        return;
    }

    const uint8_t channels = dev->hwstatus.channels;
    const uint16_t bufferSize = dev->bufferSize;

    if (dev->ringbuffers[0].getWritableDataSize() < sizeof(float) * bufferSize)
    {
        DEBUGPRINT("%08u | playback | buffer full, adding kDeviceInitializing", frame);
        dev->hints |= kDeviceInitializing;
        for (uint8_t c=0; c<channels; ++c)
            dev->ringbuffers[c].flush();
        return;
    }

    const uint16_t bufferSizeOver4 = bufferSize / 4;
    const uint16_t rbWriteSize = bufferSizeOver4 * sizeof(float);

    for (uint8_t q=0; q<4; ++q)
    {
        for (int8_t c = channels; --c >= 0;)
        {
            while (!dev->ringbuffers[c].writeCustomData(buffers[c] + bufferSizeOver4 * q, rbWriteSize))
                simd::yield();

            dev->ringbuffers[c].commitWrite();
        }

        sem_post(&dev->sem);
    }
}
