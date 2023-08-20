// SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-process.hpp"

#include <algorithm>

static void balanceDeviceCaptureSpeed(DeviceAudio* const dev,
                                      VResampler* const resampler,
                                      const snd_pcm_sframes_t avail)
{
    const uint32_t frame = dev->frame;
    const uint16_t bufferSize = dev->bufferSize;
    const uint32_t sampleRate = dev->sampleRate;

    DeviceAudio::Balance& bal(dev->balance);

    const uint16_t kSpeedTarget = static_cast<double>(sampleRate) / bufferSize * 5;
    const uint16_t kMaxTarget = bufferSize * 4.25;
    const uint16_t kMinTarget = bufferSize * 2.75;

    if (avail > kMaxTarget)
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
            if (bal.ratio != 1.0) {
                DEBUGPRINT("%08u | capture | %.9f = %.9f * %.9f | %ld | avail > kMaxTarget | speeding up...",
                           frame, dev->timestamps.ratio * dev->balance.ratio, dev->timestamps.ratio, bal.ratio, avail);
            }
        }
        else if (++bal.speedingUp >= kSpeedTarget)
        {
            bal.speedingUp = 1;
            if (d_isNotEqual(bal.ratio, 0.995))
            {
                bal.ratio = (bal.ratio * 3 + 0.995) / 4;
                DEBUGPRINT("%08u | capture | %.9f = %.9f * %.9f | %ld | avail > kMaxTarget | speeding up even more...",
                           frame, dev->timestamps.ratio * dev->balance.ratio, dev->timestamps.ratio, bal.ratio, avail);
            }
        }
    }
    else if (avail < kMinTarget)
    {
        if (bal.slowingDown == 0)
        {
            switch (bal.mode)
            {
            case kBalanceNormal:
            case kBalanceSlowingDown:
                bal.ratio *= 1.000005;
                break;
            case kBalanceSpeedingUp:
                bal.ratio = 1.0;
                break;
            }
            bal.mode = kBalanceSlowingDown;
            bal.slowingDown = 1;
            bal.speedingUp = 0;
            resampler->set_rratio(dev->timestamps.ratio * bal.ratio);
            if (bal.ratio != 1.0) {
                DEBUGPRINT("%08u | capture | %.9f = %.9f * %.9f | %ld | avail < kMinTarget | slowing down...",
                           frame, dev->timestamps.ratio * dev->balance.ratio, dev->timestamps.ratio, bal.ratio, avail);
            }
        }
        else if (++bal.slowingDown >= kSpeedTarget)
        {
            bal.slowingDown = 1;
            if (d_isNotEqual(bal.ratio, 1.0005))
            {
                bal.ratio = (bal.ratio * 3 + 1.0005) / 4;
                DEBUGPRINT("%08u | capture | %.9f = %.9f * %.9f | %ld | avail < kMinTarget | slowing down even more...",
                           frame, dev->timestamps.ratio * dev->balance.ratio, dev->timestamps.ratio, bal.ratio, avail);
            }
        }
    }
    else
    {
        if ((bal.slowingDown != 0 && ++bal.slowingDown >= kSpeedTarget) || (bal.speedingUp != 0 && ++bal.speedingUp >= kSpeedTarget))
        {
            DEBUGPRINT("%08u | capture | %.9f = %.9f * %.9f | %ld | stopped speed compensation",
                       frame, dev->timestamps.ratio * dev->balance.ratio, dev->timestamps.ratio, bal.ratio, avail);
            bal.mode = kBalanceNormal;
            bal.slowingDown = bal.speedingUp = 0;
        }
    }
}

static void* deviceCaptureThread(void* const  arg)
{
    DeviceAudio* const dev = static_cast<DeviceAudio*>(arg);

    int err;
    snd_pcm_sframes_t frames;
    uint32_t frame, written;
    uint32_t frameCount = 0;

    const uint8_t hints = dev->hints;
    const uint8_t channels = dev->channels;
    const uint16_t bufferSize = dev->bufferSize;
    const uint16_t extraBufferSize = bufferSize / 2;
    const uint32_t periodTimeOver4 = ((bufferSize / 4) * 1000000) / dev->sampleRate * 1000;

    float** buffers = new float*[channels];
    for (uint8_t c=0; c<channels; ++c)
        buffers[c] = new float[bufferSize * 2];

    bool again = false, full;
    snd_pcm_uframes_t avail = 0;
    snd_htimestamp_t tstamp = {};

    // disable denormals and enable flush to zero
    {
       #if defined(__SSE2_MATH__)
        _mm_setcsr(_mm_getcsr() | 0x8040);
       #elif defined(__aarch64__)
        uint64_t flags;
        __asm__ __volatile__("mrs %0, fpcr" : "=r" (flags));
        __asm__ __volatile__("msr fpcr, %0" :: "r" (flags | 0x1000000));
       #elif defined(__arm__) && !defined(__SOFTFP__)
        uint32_t flags;
        __asm__ __volatile__("vmrs %0, fpscr" : "=r" (flags));
        __asm__ __volatile__("vmsr fpscr, %0" :: "r" (flags | 0x1000000));
       #endif
    }

    VResampler* const resampler = new VResampler;
    resampler->setup(1.0, channels, 8);

    struct timespec ts = {};
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 15;

    if (sem_timedwait(&dev->sem, &ts) != 0)
    {
        return nullptr;
    }

    while (dev->channels != 0)
    {
        frames = snd_pcm_mmap_readi(dev->pcm, dev->buffers.raw, bufferSize + extraBufferSize);

        if (dev->channels == 0)
            break;

        if (frames == -EAGAIN)
        {
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += periodTimeOver4;
            if (ts.tv_nsec >= 1000000000ULL)
            {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000ULL;
            }
            sem_timedwait(&dev->sem, &ts);
//             sem_wait(&dev->sem);
            again = true;
            continue;
        }

        frame = dev->frame;

        if (frames < 0)
        {
            deviceFailInitHints(dev);
            resampler->set_rratio(1.0);

            for (uint8_t c=0; c<channels; ++c)
                dev->ringbuffers[c].clearData();

            DEBUGPRINT("%08u | capture | Error read %s\n", frame, snd_strerror(frames));

            // TODO offline recovery
            if (xrun_recovery(dev->pcm, frames) < 0)
            {
                printf("%08u | capture | Read error: %s\n", frame, snd_strerror(frames));
                exit(EXIT_FAILURE);
            }

            again = false;
            continue;
        }

        if (again)
        {
            if (snd_pcm_htimestamp(dev->pcm, &avail, &tstamp) != 0)
            {
                DEBUGPRINT("snd_pcm_htimestamp failed");
                break;
            }
        }

        const uint16_t offset = 0;
        switch (hints & kDeviceSampleHints)
        {
        case kDeviceSample16:
            int2float::s16(dev->buffers.f32, dev->buffers.raw, offset, channels, frames);
            break;
        case kDeviceSample24:
            int2float::s24(dev->buffers.f32, dev->buffers.raw, offset, channels, frames);
            break;
        case kDeviceSample24LE3:
            int2float::s24le3(dev->buffers.f32, dev->buffers.raw, offset, channels, frames);
            break;
        case kDeviceSample32:
            int2float::s32(dev->buffers.f32, dev->buffers.raw, offset, channels, frames);
            break;
        }

       #ifdef WITH_GAIN
        float gain;
        for (uint16_t i=0; i<bufferSize; ++i)
        {
            gain = dev->gain.next();
            for (uint8_t c=0; c<channels; ++c)
                buffers[c][i] *= gain;
        }
       #endif

        resampler->inp_count = frames;
        resampler->out_count = bufferSize * 2;
        resampler->inp_data = dev->buffers.f32;
        resampler->out_data = buffers;
        resampler->process();

        written = bufferSize * 2 - resampler->out_count;

        full = false;
        for (uint8_t c=0; c<channels; ++c)
        {
            while (!dev->ringbuffers[c].writeCustomData(buffers[c], sizeof(float) * written))
            {
                sched_yield();
                full = true;
            }

            dev->ringbuffers[c].commitWrite();
        }

        if (again)
        {
            snd_pcm_sframes_t savail = 0;
            savail += dev->ringbuffers[0].getReadableDataSize() /  sizeof(float);
            savail += frames;
            savail -= bufferSize;

            if ((dev->hints & kDeviceStarting) == 0)
                balanceDeviceCaptureSpeed(dev, resampler, savail);

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
                if ((frameCount % dev->sampleRate) == 0)
                {
                    DEBUGPRINT("%08u | %s | %.09f = %.09f * %.09f | %ld avail | mode: %s",
                            frame, dev->hints & kDeviceCapture ? "capture" : "playback",
                            dev->timestamps.ratio * dev->balance.ratio, dev->timestamps.ratio, dev->balance.ratio, savail, BalanceModeToStr(dev->balance.mode));
                }
            }
        }

        again = full;
        frameCount += frames;

        if (full)
        {
            dev->balance.mode = kBalanceSpeedingUp;
            dev->balance.ratio = std::min(0.995, (dev->balance.ratio * 3 + 0.995) / 4);
            dev->balance.speedingUp = 1;
            dev->balance.slowingDown = 0;
            resampler->set_rratio(dev->timestamps.ratio * dev->balance.ratio);
            DEBUGPRINT("%08u | capture | %.9f = %.9f * %.9f | speeding up real fast...",
                       frame, dev->timestamps.ratio * dev->balance.ratio, dev->timestamps.ratio, dev->balance.ratio);

            sem_wait(&dev->sem);
        }
    }

    for (uint8_t c=0; c<channels; ++c)
        delete[] buffers[c];
    delete[] buffers;

    return nullptr;
}

static void runDeviceAudioCapture(DeviceAudio* const dev, float* buffers[], const uint32_t frame)
{
    const uint16_t bufferSize = dev->bufferSize;
    const uint8_t channels = dev->channels;

    const HeapRingBuffer& rb(dev->ringbuffers[0]);
    uint32_t avail = rb.getReadableDataSize() / sizeof(float);

    if (dev->hints & kDeviceInitializing)
    {
        if (avail >= bufferSize * 2)
        {
            dev->hints &= ~kDeviceInitializing;
            DEBUGPRINT("%08u | capture | %u >= bufferSize * 2, removing kDeviceInitializing", frame, avail);
        }
    }

    if (dev->hints & kDeviceInitializing)
    {
        for (uint8_t c=0; c<channels; ++c)
            std::memset(buffers[c], 0, sizeof(float) * bufferSize);
    }
    else if (avail >= bufferSize)
    {
        if ((dev->hints & kDeviceStarting) != 0)
        {
            dev->hints &= ~kDeviceStarting;
            DEBUGPRINT("%08u | capture | %u >= bufferSize, removing kDeviceStarting", frame, avail);
        }

        for (uint8_t c=0; c<channels; ++c)
        {
            while (!dev->ringbuffers[c].readCustomData(buffers[c], sizeof(float) * bufferSize))
                simd_yield();
        }
    }
    else
    {
        // oh no! lets see if the reader side can quickly get a few more samples
        sem_post(&dev->sem);

        dev->hints |= kDeviceStarting;
        DEBUGPRINT("%08u | capture | WARNING %u < bufferSize, adding kDeviceStarting", frame, avail);

        if (avail == 0)
        {
            sched_yield();
            avail = rb.getReadableDataSize() / sizeof(float);

            if (avail == 0)
            {
                dev->hints |= kDeviceInitializing;
                DEBUGPRINT("%08u | capture | ERROR no audio available, adding kDeviceInitializing", frame);

                dev->balance.mode = kBalanceNormal;
                dev->balance.ratio = 1.0;
                dev->balance.slowingDown = dev->balance.speedingUp = 0;

                for (uint8_t c=0; c<channels; ++c)
                    std::memset(buffers[c], 0, sizeof(float) * bufferSize);

                return;
            }
        }

        // write what we can already
        for (uint8_t c=0; c<channels; ++c)
        {
            while (!dev->ringbuffers[c].readCustomData(buffers[c], sizeof(float) * avail))
                simd_yield();
        }

        // keep going while fetching the rest
        for (uint16_t offset = avail, retries = 0; offset < bufferSize; offset += avail)
        {
            sem_post(&dev->sem);

            sched_yield();
            avail = std::min<uint32_t>(bufferSize - offset, rb.getReadableDataSize() / sizeof(float));

            if (avail == 0)
            {
                if (++retries == 1000)
                {
                    dev->hints |= kDeviceInitializing;
                    DEBUGPRINT("%08u | capture | ERROR no more audio available, adding kDeviceInitializing", frame);
                    for (uint8_t c=0; c<channels; ++c)
                        std::memset(buffers[c] + offset, 0, sizeof(float) * (bufferSize - offset));
                    break;
                }
                continue;
            }

            retries = 0;
            for (uint8_t c=0; c<channels; ++c)
            {
                while (!dev->ringbuffers[c].readCustomData(buffers[c] + offset, sizeof(float) * avail))
                    simd_yield();
            }
        }
    }

    sem_post(&dev->sem);
}

