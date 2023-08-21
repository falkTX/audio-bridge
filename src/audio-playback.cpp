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
    const uint8_t channels = dev->channels;
    const uint8_t sampleSize = getSampleSizeFromHints(hints);
    const uint16_t bufferSize = dev->bufferSize;
    const uint32_t periodTimeOver4 = ((bufferSize / 4) * 1000000) / dev->sampleRate * 1000;

    snd_pcm_sframes_t err;
    uint16_t frames;
    uint32_t frame;
    uint32_t frameCount = 0;

    float** buffers = new float*[channels];
    for (uint8_t c=0; c<channels; ++c)
        buffers[c] = new float[bufferSize * 2];

    bool again = false;
    bool pair = true;

    simd::init();

    // smooth initial volume to prevent clicks on start
    float xgain;
    ExponentialValueSmoother gain;
    gain.setSampleRate(dev->sampleRate);
    gain.setTimeConstant(3.f);
    // common setup
    gain.setTargetValue(0.f);
    gain.clearToTargetValue();
    gain.setTargetValue(1.f);

    VResampler* const resampler = new VResampler;
    resampler->setup(1.0, channels, 8);

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

    // write silence until alsa buffers are full
    std::memset(dev->buffers.raw, 0, bufferSize * channels * sampleSize);
    while ((err = snd_pcm_mmap_writei(dev->pcm, dev->buffers.raw, bufferSize)) > 0);

    if (err != -EAGAIN)
    {
        printf("%08u | playback | initial write error: %s\n", dev->frame, snd_strerror(err));
        goto end;
    }

    while (dev->channels != 0)
    {
        if (dev->ringbuffers[0].getReadableDataSize() == 0)
        {
            sem_wait(&dev->sem);
            again = true;
            continue;
        }

        frame = dev->frame;

        pair = !pair;
        for (uint8_t c=0; c<channels; ++c)
        {
            while (!dev->ringbuffers[c].readCustomData(buffers[c], sizeof(float) * bufferSize / 2))
            {
                DEBUGPRINT("%08u | failed reading data", frame);
                sched_yield();
            }
        }

        if (dev->channels == 0)
            break;

        resampler->inp_count = bufferSize / 2;
        resampler->out_count = bufferSize * 2;
        resampler->inp_data = buffers;
        resampler->out_data = dev->buffers.f32;
        resampler->process();

        frames = bufferSize * 2 - resampler->out_count;

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
                again = true;
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

                again = false;
                break;
            }

            if (pair && (again || (static_cast<uint16_t>(err) == frames && ptr == dev->buffers.raw)))
            {
                snd_pcm_uframes_t avail;
                snd_htimestamp_t tstamp;

                if (snd_pcm_htimestamp(dev->pcm, &avail, &tstamp) != 0)
                {
                    DEBUGPRINT("snd_pcm_htimestamp failed");
                    break;
                }

                uint32_t rbavail = 0;
                rbavail += dev->ringbuffers[0].getReadableDataSize() /  sizeof(float);
                rbavail += frames - err;

                if ((dev->hints & kDeviceStarting) == 0)
                    balanceDevicePlaybackSpeed(dev, resampler, avail, rbavail);

                if (dev->timestamps.alsaStartTime == 0)
                {
                    dev->timestamps.alsaStartTime = static_cast<uint64_t>(tstamp.tv_sec) * 1000000000ULL + tstamp.tv_nsec;
                    dev->timestamps.jackStartFrame = frame;
                }
                else if (dev->timestamps.alsaStartTime != 0 && frame != 0 && dev->timestamps.jackStartFrame != frame)
                {
                    const uint64_t alsadiff = static_cast<uint64_t>(tstamp.tv_sec) * 1000000000ULL + tstamp.tv_nsec - dev->timestamps.alsaStartTime;
                    const uint32_t alsaframes = alsadiff * dev->sampleRate / 1000000000ULL;
                    const uint32_t jackframes = frame - dev->timestamps.jackStartFrame;
                    dev->timestamps.ratio = ((static_cast<double>(alsaframes) / jackframes) + dev->timestamps.ratio * 511) / 512;
                    resampler->set_rratio(dev->timestamps.ratio * dev->balance.ratio);
                    if ((frameCount % dev->sampleRate) <= bufferSize || avail > 255)
                    {
                        DEBUGPRINT("%08u | playback | %.09f = %.09f * %.09f | %3u %3ld | mode: %s",
                                   frame, dev->timestamps.ratio * dev->balance.ratio, dev->timestamps.ratio, dev->balance.ratio,
                                   rbavail, avail, BalanceModeToStr(dev->balance.mode));
                    }
                }
            }

            if (static_cast<uint16_t>(err) == frames)
            {
                /**/ if (dev->hints & kDeviceInitializing)
                    dev->hints &= ~kDeviceInitializing;
                else if (dev->hints & kDeviceStarting)
                    dev->hints &= ~kDeviceStarting;
                break;
            }

            ptr += err * channels * sampleSize;
            frames -= err;

            DEBUGPRINT("%08u | playback | Incomplete write %ld of %u, %u left",
                       frame, err, bufferSize, frames);
        }

        frameCount += bufferSize / 2;
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
    const uint16_t bufferSize = dev->bufferSize;
    const uint16_t halfBufferSize = bufferSize / 2;
    const uint8_t channels = dev->channels;

    // TODO stop trying at some point

    for (uint8_t c=0; c<channels; ++c)
    {
        while (!dev->ringbuffers[c].writeCustomData(buffers[c], sizeof(float) * halfBufferSize))
            simd::yield();
    }

    for (uint8_t c=0; c<channels; ++c)
        dev->ringbuffers[c].commitWrite();

    sem_post(&dev->sem);

    for (uint8_t c=0; c<channels; ++c)
    {
        while (!dev->ringbuffers[c].writeCustomData(buffers[c] + halfBufferSize, sizeof(float) * halfBufferSize))
            simd::yield();
    }

    for (uint8_t c=0; c<channels; ++c)
        dev->ringbuffers[c].commitWrite();

    sem_post(&dev->sem);
}