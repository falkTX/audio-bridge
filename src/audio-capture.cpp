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

    snd_pcm_status_t* status;
    snd_pcm_status_malloc(&status);
    std::memset(status, 0, snd_pcm_status_sizeof());

    auto restart = [&dev, &resampler, &gain]()
    {
        deviceFailInitHints(dev);
        resampler->set_rratio(1.0);
        gain.setTargetValue(0.f);
        gain.clearToTargetValue();
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
                DEBUGPRINT("%08u | capture | can read data, removing kDeviceInitializing", frame);
                restart();
                dev->hints &= ~kDeviceInitializing;
            }
            else
            {
                deviceTimedWait(dev);
                continue;
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

        if (snd_pcm_status(dev->pcm, status) == 0)
        {
            pthread_mutex_lock(&dev->statuslock);
            snd_pcm_status_copy(dev->status, status);
            pthread_mutex_unlock(&dev->statuslock);
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

        resampler->set_rratio(dev->balance.ratio);

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

            if ((dev->hints & kDeviceStarting) != 0 && dev->ringbuffer->getNumReadableSamples() > bufferSize * 2)
            {
                DEBUGPRINT("%08u | capture | wrote bufferSize * 2 amount of data, removing kDeviceStarting", frame);
                dev->hints &= ~kDeviceStarting;
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

    snd_pcm_status_free(status);

    delete resampler;

    for (uint8_t c=0; c<channels; ++c)
        delete[] buffers[c];
    delete[] buffers;

    dev->thread = 0;
    return nullptr;
}

static void runDeviceAudioCapture(DeviceAudio* const dev, float* buffers[], const uint32_t frame)
{
    const uint16_t bufferSize = dev->bufferSize;

    for (uint8_t c=0; c < dev->hwstatus.channels; ++c)
        std::memset(buffers[c], 0, sizeof(float) * bufferSize);

    if (dev->hints & kDeviceStarting)
    {
        dev->balance.distance = 0;
        dev->framesDone = 0;
        sem_post(&dev->sem);
        return;
    }

    if (dev->ringbuffer->getNumReadableSamples() < bufferSize)
    {
        DEBUGPRINT("%08u | capture | buffer empty, adding kDeviceInitializing", frame);
        dev->hints |= kDeviceInitializing|kDeviceStarting;
        dev->ringbuffer->flush();
        return;
    }

    DISTRHO_SAFE_ASSERT_RETURN(dev->ringbuffer->read(buffers, bufferSize),);

    sem_post(&dev->sem);

    dev->framesDone += bufferSize;
    setDeviceTimings(dev, frame);
}
