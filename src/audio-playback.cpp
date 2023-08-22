// SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-process.hpp"
#include "audio-utils.hpp"

#include <algorithm>

static void balanceDevicePlaybackSpeed(DeviceAudio* const dev,
                                       VResampler* const resampler,
                                       const snd_pcm_sframes_t avail,
                                       const uint32_t rbavail)
{
    const uint32_t frame = dev->frame;
    const uint16_t bufferSize = dev->bufferSize;

    // recalibrate every 8s
    const uint16_t kSpeedTarget = static_cast<double>(dev->sampleRate) / bufferSize * 8;

    DeviceAudio::Balance& bal(dev->balance);

    if (avail == 0 || rbavail > bufferSize)
    {
        if (bal.speedingUp == 0)
        {
            switch (bal.mode)
            {
            case kBalanceNormal:
            case kBalanceSpeedingUp:
                bal.ratio *= 0.999995;
                break;
            case kBalanceSlowingDown:
                bal.ratio = 1.0;
                break;
            }
            bal.mode = kBalanceSpeedingUp;
            bal.speedingUp = 1;
            bal.slowingDown = 0;
            resampler->set_rratio(dev->timestamps.ratio * bal.ratio);
//             if (bal.ratio != 1.0) {
                DEBUGPRINT("%08u | playback | %.9f = %.9f * %.9f | %3u %3ld | avail == 0 || rbavail > bufferSize | speeding up...",
                           frame, dev->timestamps.ratio * dev->balance.ratio, dev->timestamps.ratio, bal.ratio, rbavail, avail);
//             }
        }
        else if (++bal.speedingUp >= kSpeedTarget)
        {
            bal.speedingUp = 1;
            if (d_isNotEqual(bal.ratio, 0.9995))
            {
                bal.ratio = (bal.ratio * 3 + 0.9995) / 4;
                DEBUGPRINT("%08u | playback | %.9f = %.9f * %.9f | %3u %3ld | avail == 0 || rbavail > bufferSize | speeding up even more...",
                           frame, dev->timestamps.ratio * dev->balance.ratio, dev->timestamps.ratio, bal.ratio, rbavail, avail);
            }
        }
    }
    else if (avail > bufferSize)
    {
        if (bal.slowingDown == 0)
        {
            switch (bal.mode)
            {
            case kBalanceNormal:
            case kBalanceSlowingDown:
                bal.ratio *= 1.00005;
                break;
            case kBalanceSpeedingUp:
                bal.ratio = 1.0;
                break;
            }
            bal.mode = kBalanceSlowingDown;
            bal.slowingDown = 1;
            bal.speedingUp = 0;
            resampler->set_rratio(dev->timestamps.ratio * bal.ratio);
//             if (bal.ratio != 1.0) {
                DEBUGPRINT("%08u | playback | %.9f = %.9f * %.9f | %3u %3ld | avail > bufferSize | slowing down...",
                           frame, dev->timestamps.ratio * dev->balance.ratio, dev->timestamps.ratio, bal.ratio, rbavail, avail);
//             }
        }
        else if (++bal.slowingDown == kSpeedTarget)
        {
            bal.slowingDown = 1;
            if (d_isNotEqual(bal.ratio, 1.005))
            {
                bal.ratio = (bal.ratio * 3 + 1.005) / 4;
                DEBUGPRINT("%08u | playback | %.9f = %.9f * %.9f | %3u %3ld | avail > bufferSize | slowing down even more...",
                           frame, dev->timestamps.ratio * dev->balance.ratio, dev->timestamps.ratio, bal.ratio, rbavail, avail);
            }
        }
    }
    else
    {
        if ((bal.slowingDown != 0 && ++bal.slowingDown == kSpeedTarget) || (bal.speedingUp != 0 && ++bal.speedingUp == kSpeedTarget))
        {
            DEBUGPRINT("%08u | playback | %.9f = %.9f * %.9f | %3u %3ld | stopped speed compensation",
                       frame, dev->timestamps.ratio * dev->balance.ratio, dev->timestamps.ratio, bal.ratio, rbavail, avail);
            bal.mode = kBalanceNormal;
            bal.slowingDown = bal.speedingUp = 0;
        }
    }
}

static void* devicePlaybackThread(void* const  arg)
{
    DeviceAudio* const dev = static_cast<DeviceAudio*>(arg);

    const uint8_t hints = dev->hints;
    const uint8_t channels = dev->hwstatus.channels;
    const uint8_t sampleSize = getSampleSizeFromHints(hints);
    const uint16_t bufferSize = dev->bufferSize;
    const uint16_t bufferSizeOver4 = dev->bufferSize / 4;
    const uint32_t periodTimeOver4 = (std::max(1, bufferSize / 4) * 1000000) / dev->sampleRate * 1000;

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

        if (dev->ringbuffers[0].getReadableDataSize() == 0)
        {
            sem_wait(&dev->sem);
            loopCount = 0;
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
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += periodTimeOver4;
                if (ts.tv_nsec >= 1000000000ULL)
                {
                    ts.tv_sec += 1;
                    ts.tv_nsec -= 1000000000ULL;
                }
                sem_timedwait(&dev->sem, &ts);
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
                        const uint32_t totalavail = avail + dev->ringbuffers[0].getReadableDataSize() / sizeof(float);
                        const double availratio = totalavail > bufferSizeOver4 ? 1.001
                                                : totalavail < bufferSizeOver4 ? 0.999 : 1;
                        dev->balance.ratio = (availratio + dev->balance.ratio * 511) / 512;

                        // combined ratio for dynamic resampling
                        resampler->set_rratio(dev->timestamps.ratio * dev->balance.ratio);

                        // TESTING DEBUG REMOVE ME
                        if ((frame % dev->sampleRate) == 0 || avail > 255)
                        {
                            DEBUGPRINT("%08u | playback | %.09f = %.09f * %.09f | %3u %3ld",
                                       frame, dev->timestamps.ratio * dev->balance.ratio,
                                       dev->timestamps.ratio, dev->balance.ratio,
                                       totalavail, avail);
                        }
                    }
                }
            }

            break;
        }
    }

end:
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

    if (dev->ringbuffers[0].getWritableDataSize() < sizeof(float) * dev->bufferSize ||
        dev->ringbuffers[channels - 1].getWritableDataSize() < sizeof(float) * dev->bufferSize)
    {
        DEBUGPRINT("%08u | playback | buffer full, adding kDeviceInitializing", frame);
        dev->hints |= kDeviceInitializing;
        for (uint8_t c=0; c<channels; ++c)
            dev->ringbuffers[c].flush();
        return;
    }

    const uint16_t bufferSizeOver4 = dev->bufferSize / 4;
    const uint16_t rbWriteSize = bufferSizeOver4 * sizeof(float);

    for (uint8_t q=0; q<4; ++q)
    {
        for (uint8_t c=0; c<channels; ++c)
        {
            while (!dev->ringbuffers[c].writeCustomData(buffers[c] + bufferSizeOver4 * q, rbWriteSize))
            {
                DEBUGPRINT("%08u | failed writing data", frame);
                simd::yield();
            }

            dev->ringbuffers[c].commitWrite();
        }

        sem_post(&dev->sem);
    }
}
