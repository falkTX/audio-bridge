// SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
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
    const uint16_t bufferSizeOver4 = bufferSize / 4;

    float** buffers = new float*[channels];
    for (uint8_t c=0; c<channels; ++c)
        buffers[c] = new float[bufferSize * 2];

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
            printf("%08u | capture | audio thread failed to post\n", dev->frame);
            goto end;
        }
    }

    if (dev->hwstatus.channels == 0)
        goto end;

    // read until alsa buffers are empty
    while ((err = snd_pcm_mmap_readi(dev->pcm, dev->buffers.raw, bufferSize)) > 0);

    for (uint32_t lastframe = 0; dev->hwstatus.channels != 0;)
    {
        const uint32_t frame = dev->frame;

        if (dev->hints & kDeviceInitializing)
        {
            // read until alsa buffers are empty
            bool restarted = false;
            while ((err = snd_pcm_mmap_readi(dev->pcm, dev->buffers.raw, bufferSize)) > 0)
                restarted = true;

            if (err != -EAGAIN)
            {
                printf("%08u | capture | initial read error: %s\n", frame, snd_strerror(err));
                goto end;
            }

            if (restarted)
            {
                DEBUGPRINT("%08u | capture | can read data, removing kDeviceInitializing", frame);
                dev->hints &= ~kDeviceInitializing;
                gain.setTargetValue(0.f);
                gain.clearToTargetValue();
                gain.setTargetValue(1.f);
            }
            else
            {
                deviceTimedWait(dev);
                continue;
            }
        }

        err = snd_pcm_mmap_readi(dev->pcm, dev->buffers.raw, bufferSizeOver4);

        if (dev->hwstatus.channels == 0)
            break;

        if (err == -EAGAIN)
        {
            deviceTimedWait(dev);
            continue;
        }

        if (err < 0)
        {
            deviceFailInitHints(dev);
            resampler->set_rratio(1.0);

            /*
            for (uint8_t c=0; c<channels; ++c)
                dev->ringbuffers[c].clearData();
            */

            DEBUGPRINT("%08u | capture | Read error %s\n", frame, snd_strerror(err));

            // TODO offline recovery
            if (xrun_recovery(dev->pcm, err) < 0)
            {
                printf("%08u | capture | xrun_recovery error: %s\n", frame, snd_strerror(err));
                exit(EXIT_FAILURE);
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

        resampler->inp_count = err;
        resampler->out_count = bufferSize * 2;
        resampler->inp_data = dev->buffers.f32;
        resampler->out_data = buffers;
        resampler->process();

        uint32_t frames = bufferSize * 2 - resampler->out_count;

        for (uint16_t i=0; i<frames; ++i)
        {
            xgain = gain.next();
            for (uint8_t c=0; c<channels; ++c)
                buffers[c][i] *= xgain;
        }

        while (frames != 0)
        {
            const uint32_t rbavail = std::min<uint32_t>(frames,
                                                        dev->ringbuffers[0].getWritableDataSize() / sizeof(float));

            if (rbavail == 0)
            {
                deviceTimedWait(dev);
                continue;
            }

            for (uint8_t c=0; c<channels; ++c)
            {
                while (!dev->ringbuffers[c].writeCustomData(buffers[c], sizeof(float) * rbavail))
                {
                    DEBUGPRINT("%08u | capture | failed writing data", frame);
                    sched_yield();
                }

                dev->ringbuffers[c].commitWrite();
            }

            if (rbavail != frames)
            {
                frames -= rbavail;
                DEBUGPRINT("%08u | capture | Incomplete write %u of %u", frame, rbavail, frames);
                continue;
            }

            if (lastframe == frame)
                break;

            lastframe = frame;

            snd_pcm_uframes_t avail;
            snd_htimestamp_t ts;

            if (snd_pcm_htimestamp(dev->pcm, &avail, &ts) != 0)
            {
                DEBUGPRINT("snd_pcm_htimestamp failed");
                break;
            }

            if (dev->hints & kDeviceStarting)
            {
                if (dev->ringbuffers[0].getReadableDataSize() / sizeof(float) >= bufferSize * 3)
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
                    const double availratio = availtotal > bufferSize * 3 ? 0.9999
                                            : availtotal < bufferSize * 3 ? 1.0001 : 1;
                    dev->balance.ratio = (availratio + dev->balance.ratio * 511) / 512;

                    // combined ratio for dynamic resampling
                    resampler->set_rratio(dev->timestamps.ratio * dev->balance.ratio);

                    // TESTING DEBUG REMOVE ME
                    if ((frame % dev->sampleRate) == 0 || avail > 255)
                    {
                        DEBUGPRINT("%08u | capture | %.09f = %.09f * %.09f | %3u %3ld",
                                    frame, dev->timestamps.ratio * dev->balance.ratio,
                                    dev->timestamps.ratio, dev->balance.ratio,
                                    availtotal, avail);
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

static void runDeviceAudioCapture(DeviceAudio* const dev, float* buffers[], const uint32_t frame)
{
    const uint8_t channels = dev->hwstatus.channels;
    const uint16_t bufferSize = dev->bufferSize;

    if (dev->hints & kDeviceStarting)
        goto clear;

    if (dev->ringbuffers[0].getReadableDataSize() < sizeof(float) * bufferSize)
    {
        DEBUGPRINT("%08u | capture | buffer empty, adding kDeviceInitializing", frame);
        dev->hints |= kDeviceInitializing;
        for (uint8_t c=0; c<channels; ++c)
            dev->ringbuffers[c].flush();
        goto clear;
    }

    for (uint8_t c=0; c<channels; ++c)
    {
        while (!dev->ringbuffers[c].readCustomData(buffers[c], sizeof(float) * bufferSize))
            simd::yield();
    }

    sem_post(&dev->sem);
    return;

clear:
    sem_post(&dev->sem);

    for (uint8_t c=0; c<channels; ++c)
        std::memset(buffers[c], 0, sizeof(float) * bufferSize);
}
