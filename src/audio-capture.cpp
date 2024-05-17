// SPDX-FileCopyrightText: 2021-2024 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-device-init.hpp"
#include "audio-utils.hpp"

#include <algorithm>

#ifndef USB_GADGET_MODE
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

            if ((dev->hints & kDeviceStarting) != 0
                && dev->ringbuffer->getNumReadableSamples() > bufferSize * AUDIO_BRIDGE_CAPTURE_LATENCY_BLOCKS)
            {
                DEBUGPRINT("%08u | capture | wrote enough data, removing kDeviceStarting", frame);
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
#else
static void runDeviceAudioCapture(DeviceAudio* const dev, float* buffers[], const uint32_t frame)
{
    const uint8_t hints = dev->hints;
    const uint8_t channels = dev->hwstatus.channels;
    const uint16_t bufferSize = dev->bufferSize;
    snd_pcm_sframes_t err;
    float rbuffers1[bufferSize * 2];
    float rbuffers2[bufferSize * 2];
    float* rbuffers[2] = { rbuffers1, rbuffers2 };

    if (hints & kDeviceInitializing)
    {
        // read until alsa buffers are empty
        bool started = false;
        while ((err = snd_pcm_mmap_readi(dev->pcm, dev->buffers.raw, bufferSize * 2)) > 0)
            started = true;

        if (err != -EAGAIN)
        {
            printf("%08u | capture | initial read error: %s\n", frame, snd_strerror(err));
            goto error;
        }

        if (started)
        {
            DEBUGPRINT("%08u | capture | can read data, removing kDeviceInitializing", frame);
            dev->hints &= ~kDeviceInitializing;
        }

        goto end;
    }

    err = snd_pcm_mmap_readi(dev->pcm, dev->buffers.raw, bufferSize * 2);

    if (err == 0 || err == -EAGAIN)
    {
        if (hints & kDeviceStarting)
            goto end;

        DEBUGPRINT("%08u | capture | snd_pcm_mmap_readi got no new data", frame);
        goto error;
    }

    if (err < 0)
    {
        DEBUGPRINT("%08u | capture | Read error %s, adding kDeviceInitializing", frame, snd_strerror(err));
        goto error;
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

    if (dev->framesDone >= dev->sampleRate * 2)
    {
        const double rbratio = (
                                static_cast<double>(dev->ringbuffer->getNumReadableSamples() / 16.0) /
                                static_cast<double>(dev->ringbuffer->getNumSamples() / 16.0)
                                / 0.125 + AUDIO_BRIDGE_CLOCK_FILTER_STEPS - 1
                               ) / AUDIO_BRIDGE_CLOCK_FILTER_STEPS;

        const double balratio = std::max(0.9, std::min(1.1,
            ((2.0 - rbratio) + dev->balance.ratio * (AUDIO_BRIDGE_CLOCK_FILTER_STEPS - 1)) / AUDIO_BRIDGE_CLOCK_FILTER_STEPS
        ));

        if (std::abs(dev->balance.ratio - balratio) > 0.000000002)
        {
            dev->balance.ratio = balratio;
            dev->resampler->set_rratio(balratio);
        }
    }

    dev->resampler->inp_count = err;
    dev->resampler->out_count = bufferSize * 2;
    dev->resampler->inp_data = dev->buffers.f32;
    dev->resampler->out_data = rbuffers;
    dev->resampler->process();

    if (! dev->ringbuffer->write(rbuffers, bufferSize * 2 - dev->resampler->out_count))
    {
        DEBUGPRINT("%08u | capture | failed writing data, adding kDeviceInitializing", frame);
        goto error;
    }

    if (hints & kDeviceStarting)
    {
        if (dev->ringbuffer->getNumReadableSamples() > bufferSize * AUDIO_BRIDGE_CAPTURE_LATENCY_BLOCKS)
        {
            DEBUGPRINT("%08u | capture | wrote enough data, removing kDeviceStarting", frame);
            dev->hints &= ~kDeviceStarting;
        }
    }
    else
    {
        if (! dev->ringbuffer->read(buffers, bufferSize))
        {
            DEBUGPRINT("%08u | capture | failed reading data, adding kDeviceInitializing", frame);
            deviceFailInitHints(dev);
        }
    }

    dev->framesDone += bufferSize;
    setDeviceTimings(dev, frame);
    return;

error:
    deviceFailInitHints(dev);

end:
    for (uint8_t c=0; c < dev->hwstatus.channels; ++c)
        std::memset(buffers[c], 0, sizeof(float) * bufferSize);
}
#endif // USB_GADGET_MODE
