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
//     const uint16_t bufferSizeOver4 = bufferSize / 4;

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
            printf("%08u | playback | audio thread failed to post\n", dev->frame);
            goto end;
        }
    }

//     if (dev->hwstatus.channels == 0)
//         goto end;

//     // write silence until alsa buffers are full
//     std::memset(dev->buffers.raw, 0, sampleSize * bufferSize * channels * 2);
//     while ((err = snd_pcm_mmap_writei(dev->pcm, dev->buffers.raw, bufferSize)) > 0);

    while (dev->hwstatus.channels != 0)
    {
        const uint32_t frame = dev->frame;

        if (dev->hints & kDeviceInitializing)
        {
            // write silence until alsa buffers are full
            bool started = false;
            std::memset(dev->buffers.raw, 0, sampleSize * bufferSize * channels);
            while ((err = snd_pcm_mmap_writei(dev->pcm, dev->buffers.raw, bufferSize)) > 0)
                started = true;

            if (err != -EAGAIN)
            {
                printf("%08u | playback | initial write error: %s\n", frame, snd_strerror(err));
                goto end;
            }

            if (started)
            {
                DEBUGPRINT("%08u | playback | can write data, removing kDeviceInitializing", frame);
                restart();
                dev->hints &= ~kDeviceInitializing;
            }
            else
            {
                deviceTimedWait(dev);
                continue;
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

        resampler->set_rratio(dev->balance.ratio);

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

        if (dev->hwstatus.channels == 0)
            break;

        int8_t* ptr = dev->buffers.raw;

        while (frames != 0)
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

            if (snd_pcm_status(dev->pcm, status) == 0)
            {
                pthread_mutex_lock(&dev->statuslock);
                snd_pcm_status_copy(dev->status, status);
                pthread_mutex_unlock(&dev->statuslock);
            }

            if (dev->hints & kDeviceStarting)
            {
                DEBUGPRINT("%08u | playback | wrote data, removing kDeviceStarting", frame);
                dev->hints &= ~kDeviceStarting;
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

            break;
        }
    }

end:
    DEBUGPRINT("%08u | playback | audio thread closed", dev->frame);

    snd_pcm_status_free(status);

    delete resampler;

    for (uint8_t c=0; c<channels; ++c)
        delete[] buffers[c];
    delete[] buffers;

    dev->thread = 0;
    return nullptr;
}

static void setDeviceTimings(DeviceAudio* const dev, const uint32_t frame)
{
    if (dev->hints & kDeviceStarting)
        return;
    if (pthread_mutex_trylock(&dev->statuslock) != 0)
        return;

    snd_pcm_status_copy(dev->statusRT, dev->status);
    pthread_mutex_unlock(&dev->statuslock);

    dev->balance.distance = snd_pcm_status_get_delay(dev->statusRT);

    // give it 5s of processing before trying to adjust speed
    if (dev->framesDone < dev->sampleRate * 5)
        return;

    snd_timestamp_t ts;
    snd_pcm_status_get_tstamp(dev->statusRT, &ts);

    if (dev->timestamps.jackStartFrame == 0)
    {
        dev->timestamps.alsaStartTime = ts.tv_sec * 1000000ULL + ts.tv_usec;
        dev->timestamps.jackStartFrame = frame;
    }
    else
    {
        const uint64_t alsaTime = ts.tv_sec * 1000000ULL + ts.tv_usec;
        DISTRHO_SAFE_ASSERT_RETURN(alsaTime > dev->timestamps.alsaStartTime,);

        const double alsaDiff = static_cast<double>(alsaTime - dev->timestamps.alsaStartTime)
                              * dev->sampleRate
                              * 0.000001;
        const uint32_t procDiff = frame - dev->timestamps.jackStartFrame;

        const double ratio = alsaDiff / procDiff;
        dev->balance.ratio = (ratio + dev->balance.ratio * 511) / 512;
    }
}

static void runDeviceAudioPlayback(DeviceAudio* const dev, float* buffers[], const uint32_t frame)
{
    if (dev->hints & kDeviceInitializing)
    {
        dev->balance.distance = 0;
        dev->framesDone = 0;
        sem_post(&dev->sem);
        return;
    }

    setDeviceTimings(dev, frame);

    if (dev->ringbuffer->getNumWritableSamples() < dev->bufferSize)
    {
        DEBUGPRINT("%08u | playback | ringbuffer full, adding kDeviceInitializing|kDeviceStarting", frame);
        dev->hints |= kDeviceInitializing|kDeviceStarting;
        dev->ringbuffer->flush();
        return;
    }

    DISTRHO_SAFE_ASSERT_RETURN(dev->ringbuffer->write(buffers, dev->bufferSize),);

    sem_post(&dev->sem);

    dev->framesDone += dev->bufferSize;
}
