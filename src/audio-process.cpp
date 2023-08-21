// SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-process.hpp"

#include <cmath>
#include <cstring>

// --------------------------------------------------------------------------------------------------------------------

static constexpr const snd_pcm_format_t kFormatsToTry[] = {
    SND_PCM_FORMAT_S32,
    SND_PCM_FORMAT_S24_3LE,
    SND_PCM_FORMAT_S24,
    SND_PCM_FORMAT_S16,
};

static constexpr const unsigned kPeriodsToTry[] = { 3, 4 };

// --------------------------------------------------------------------------------------------------------------------

static const char* SND_PCM_FORMAT_STRING(const snd_pcm_format_t format)
{
    switch (format)
    {
    #define RET_STR(F) case F: return #F;
    RET_STR(SND_PCM_FORMAT_UNKNOWN)
    RET_STR(SND_PCM_FORMAT_S8)
    RET_STR(SND_PCM_FORMAT_U8)
    RET_STR(SND_PCM_FORMAT_S16_LE)
    RET_STR(SND_PCM_FORMAT_S16_BE)
    RET_STR(SND_PCM_FORMAT_U16_LE)
    RET_STR(SND_PCM_FORMAT_U16_BE)
    RET_STR(SND_PCM_FORMAT_S24_LE)
    RET_STR(SND_PCM_FORMAT_S24_BE)
    RET_STR(SND_PCM_FORMAT_U24_LE)
    RET_STR(SND_PCM_FORMAT_U24_BE)
    RET_STR(SND_PCM_FORMAT_S32_LE)
    RET_STR(SND_PCM_FORMAT_S32_BE)
    RET_STR(SND_PCM_FORMAT_U32_LE)
    RET_STR(SND_PCM_FORMAT_U32_BE)
    RET_STR(SND_PCM_FORMAT_FLOAT_LE)
    RET_STR(SND_PCM_FORMAT_FLOAT_BE)
    RET_STR(SND_PCM_FORMAT_FLOAT64_LE)
    RET_STR(SND_PCM_FORMAT_FLOAT64_BE)
    RET_STR(SND_PCM_FORMAT_IEC958_SUBFRAME_LE)
    RET_STR(SND_PCM_FORMAT_IEC958_SUBFRAME_BE)
    RET_STR(SND_PCM_FORMAT_MU_LAW)
    RET_STR(SND_PCM_FORMAT_A_LAW)
    RET_STR(SND_PCM_FORMAT_IMA_ADPCM)
    RET_STR(SND_PCM_FORMAT_MPEG)
    RET_STR(SND_PCM_FORMAT_GSM)
    RET_STR(SND_PCM_FORMAT_S20_LE)
    RET_STR(SND_PCM_FORMAT_S20_BE)
    RET_STR(SND_PCM_FORMAT_U20_LE)
    RET_STR(SND_PCM_FORMAT_U20_BE)
    RET_STR(SND_PCM_FORMAT_SPECIAL)
    RET_STR(SND_PCM_FORMAT_S24_3LE)
    RET_STR(SND_PCM_FORMAT_S24_3BE)
    RET_STR(SND_PCM_FORMAT_U24_3LE)
    RET_STR(SND_PCM_FORMAT_U24_3BE)
    RET_STR(SND_PCM_FORMAT_S20_3LE)
    RET_STR(SND_PCM_FORMAT_S20_3BE)
    RET_STR(SND_PCM_FORMAT_U20_3LE)
    RET_STR(SND_PCM_FORMAT_U20_3BE)
    RET_STR(SND_PCM_FORMAT_S18_3LE)
    RET_STR(SND_PCM_FORMAT_S18_3BE)
    RET_STR(SND_PCM_FORMAT_U18_3LE)
    RET_STR(SND_PCM_FORMAT_U18_3BE)
    RET_STR(SND_PCM_FORMAT_G723_24)
    RET_STR(SND_PCM_FORMAT_G723_24_1B)
    RET_STR(SND_PCM_FORMAT_G723_40)
    RET_STR(SND_PCM_FORMAT_G723_40_1B)
    RET_STR(SND_PCM_FORMAT_DSD_U8)
    RET_STR(SND_PCM_FORMAT_DSD_U16_LE)
    RET_STR(SND_PCM_FORMAT_DSD_U32_LE)
    RET_STR(SND_PCM_FORMAT_DSD_U16_BE)
    RET_STR(SND_PCM_FORMAT_DSD_U32_BE)
    #undef RET_STR
    }

    return "";
}

// --------------------------------------------------------------------------------------------------------------------

// TODO cleanup, see what is needed
static int xrun_recovery(snd_pcm_t *handle, int err)
{
    // static int count = 0;
    // if ((count % 200) == 0)
    {
        // count = 1;
        printf("stream recovery: %s\n", snd_strerror(err));
    }

    if (err == -EPIPE)
    {
        /* under-run */
        err = snd_pcm_prepare(handle);
        if (err < 0)
            printf("Can't recovery from underrun, prepare failed: %s\n", snd_strerror(err));
        return 0;
    }
    else if (err == -ESTRPIPE)
    {
        while ((err = snd_pcm_resume(handle)) == -EAGAIN)
            sleep(1);   /* wait until the suspend flag is released */

        if (err < 0)
        {
            err = snd_pcm_prepare(handle);
            if (err < 0)
                printf("Can't recovery from suspend, prepare failed: %s\n", snd_strerror(err));
        }

        return 0;
    }

    return err;
}

static void deviceFailInitHints(DeviceAudio* const dev)
{
    dev->hints |= kDeviceInitializing|kDeviceStarting;
    dev->balance.mode = kBalanceNormal;
    dev->balance.slowingDown = dev->balance.speedingUp = 0;
    dev->balance.ratio = 1.0;
    dev->timestamps.alsaStartTime = dev->timestamps.jackStartFrame = 0;
    dev->timestamps.ratio = 1.0;
}

// --------------------------------------------------------------------------------------------------------------------

DeviceAudio* initDeviceAudio(const char* const deviceID,
                             const bool playback,
                             const uint16_t bufferSize,
                             const uint32_t sampleRate)
{
    int err;
    DeviceAudio dev = {};
    dev.sampleRate = sampleRate;
    dev.bufferSize = bufferSize;
    dev.hints = kDeviceInitializing|kDeviceStarting|(playback ? 0 : kDeviceCapture);

    const snd_pcm_stream_t mode = playback ? SND_PCM_STREAM_PLAYBACK : SND_PCM_STREAM_CAPTURE;

    // SND_PCM_ASYNC
    // SND_PCM_NONBLOCK
    const int flags = SND_PCM_NONBLOCK | SND_PCM_NO_AUTO_RESAMPLE | SND_PCM_NO_AUTO_CHANNELS | SND_PCM_NO_AUTO_FORMAT | SND_PCM_NO_SOFTVOL;
    if ((err = snd_pcm_open(&dev.pcm, deviceID, mode, flags)) < 0)
    {
        DEBUGPRINT("snd_pcm_open fail %d %s\n", playback, snd_strerror(err));
        return nullptr;
    }

    snd_pcm_hw_params_t* params;
    snd_pcm_hw_params_alloca(&params);

    snd_pcm_sw_params_t* swparams;
    snd_pcm_sw_params_alloca(&swparams);

    unsigned uintParam;
    unsigned long ulongParam;

    if ((err = snd_pcm_hw_params_any(dev.pcm, params)) < 0)
    {
        DEBUGPRINT("snd_pcm_hw_params_any fail %s", snd_strerror(err));
        goto error;
    }

    if ((err = snd_pcm_hw_params_set_rate_resample(dev.pcm, params, 0)) != 0)
    {
        DEBUGPRINT("snd_pcm_hw_params_set_rate_resample fail %s", snd_strerror(err));
        goto error;
    }

    if ((err = snd_pcm_hw_params_set_access(dev.pcm, params, SND_PCM_ACCESS_MMAP_INTERLEAVED)) != 0)
    {
        DEBUGPRINT("snd_pcm_hw_params_set_access fail %s", snd_strerror(err));
        goto error;
    }

    for (snd_pcm_format_t format : kFormatsToTry)
    {
        if ((err = snd_pcm_hw_params_set_format(dev.pcm, params, format)) != 0)
        {
            // DEBUGPRINT("snd_pcm_hw_params_set_format fail %u:%s %s", format, SND_PCM_FORMAT_STRING(format), snd_strerror(err));
            continue;
        }

        switch (format)
        {
        case SND_PCM_FORMAT_S16:
            dev.hints |= kDeviceSample16;
            break;
        case SND_PCM_FORMAT_S24:
            dev.hints |= kDeviceSample24;
            break;
        case SND_PCM_FORMAT_S24_3LE:
            dev.hints |= kDeviceSample24LE3;
            break;
        case SND_PCM_FORMAT_S32:
            dev.hints |= kDeviceSample32;
            break;
        default:
            DEBUGPRINT("snd_pcm_hw_params_set_format fail unimplemented format %u:%s", format, SND_PCM_FORMAT_STRING(format));
            continue;
        }

        DEBUGPRINT("snd_pcm_hw_params_set_format %s", SND_PCM_FORMAT_STRING(format));
        break;
    }

    if ((dev.hints & kDeviceSampleHints) == 0)
    {
        DEBUGPRINT("snd_pcm_hw_params_set_format fail %s", snd_strerror(err));
        goto error;
    }

    if ((err = snd_pcm_hw_params_set_rate(dev.pcm, params, sampleRate, 0)) != 0)
    {
        DEBUGPRINT("snd_pcm_hw_params_set_rate fail %s", snd_strerror(err));
        goto error;
    }

    // if ((err = snd_pcm_hw_params_set_rate(dev.pcm, params, sampleRate, 1)) != 0)
    // {
    //     DEBUGPRINT("snd_pcm_hw_params_set_rate fail %s", snd_strerror(err));
    //     goto error;
    // }

    uintParam = 0;
    for (unsigned periods : kPeriodsToTry)
    {
        if ((err = snd_pcm_hw_params_set_period_size(dev.pcm, params, bufferSize, 0)) != 0)
        {
            DEBUGPRINT("snd_pcm_hw_params_set_period_size fail %u %u %s", periods, bufferSize, snd_strerror(err));
            continue;
        }

        if ((err = snd_pcm_hw_params_set_periods(dev.pcm, params, periods, 0)) != 0)
        {
            DEBUGPRINT("snd_pcm_hw_params_set_periods fail %u %u %s", periods, bufferSize, snd_strerror(err));
            continue;
        }

        uintParam = periods;
        break;
    }

    if (uintParam == 0)
    {
        for (unsigned periods : kPeriodsToTry)
        {
            ulongParam = bufferSize * periods;
            if ((err = snd_pcm_hw_params_set_period_size_max(dev.pcm, params, &ulongParam, nullptr)) != 0)
            {
                DEBUGPRINT("snd_pcm_hw_params_set_period_size_max fail %u %u %s", periods, bufferSize, snd_strerror(err));
                continue;
            }

            uintParam = periods;
            break;
        }

        if (uintParam == 0)
        {
            DEBUGPRINT("can't find a buffer size match");
            goto error;
        }
    }

    dev.periods = uintParam;

    if (snd_pcm_hw_params_set_channels(dev.pcm, params, 2) == 0)
    {
        dev.channels = 2;
    }
    else if ((err = snd_pcm_hw_params_get_channels(params, &uintParam)) != 0)
    {
        DEBUGPRINT("snd_pcm_hw_params_get_channels fail %s", snd_strerror(err));
        goto error;
    }
    else
    {
        dev.channels = uintParam;
    }

    if ((err = snd_pcm_hw_params(dev.pcm, params)) != 0)
    {
        DEBUGPRINT("snd_pcm_hw_params fail %s", snd_strerror(err));
        goto error;
    }

    if ((err = snd_pcm_sw_params_current(dev.pcm, swparams)) != 0)
    {
        DEBUGPRINT("snd_pcm_sw_params_current fail %s", snd_strerror(err));
        goto error;
    }

    // SND_PCM_TSTAMP_NONE SND_PCM_TSTAMP_ENABLE (= SND_PCM_TSTAMP_MMAP)
    if ((err = snd_pcm_sw_params_set_tstamp_mode(dev.pcm, swparams, SND_PCM_TSTAMP_MMAP)) != 0)
    {
        DEBUGPRINT("snd_pcm_sw_params_set_tstamp_mode fail %s", snd_strerror(err));
        goto error;
    }

    // SND_PCM_TSTAMP_TYPE_MONOTONIC
    if ((err = snd_pcm_sw_params_set_tstamp_type(dev.pcm, swparams, SND_PCM_TSTAMP_TYPE_MONOTONIC_RAW)) != 0)
    {
        DEBUGPRINT("snd_pcm_sw_params_set_tstamp_type fail %s", snd_strerror(err));
        goto error;
    }

    if ((err = snd_pcm_sw_params_set_start_threshold(dev.pcm, swparams, 0)) != 0)
    {
        DEBUGPRINT("snd_pcm_sw_params_set_start_threshold fail %s", snd_strerror(err));
        goto error;
    }

    if ((err = snd_pcm_sw_params_set_stop_threshold(dev.pcm, swparams, (snd_pcm_uframes_t)-1)) != 0)
    {
        DEBUGPRINT("snd_pcm_sw_params_set_stop_threshold fail %s", snd_strerror(err));
        goto error;
    }

    if (playback)
        err = snd_pcm_sw_params_set_avail_min(dev.pcm, swparams, bufferSize * (dev.periods - 1));
    else
        err = snd_pcm_sw_params_set_avail_min(dev.pcm, swparams, 1);

    if (err != 0)
    {
        DEBUGPRINT("snd_pcm_sw_params_set_avail_min fail %s", snd_strerror(err));
        goto error;
    }

    if ((err = snd_pcm_sw_params_set_silence_threshold(dev.pcm, swparams, 0)) != 0)
    {
        DEBUGPRINT("snd_pcm_sw_params_set_silence_threshold fail %s", snd_strerror(err));
        goto error;
    }

    if ((err = snd_pcm_sw_params(dev.pcm, swparams)) != 0)
    {
        DEBUGPRINT("snd_pcm_sw_params fail %s", snd_strerror(err));
        goto error;
    }

    if ((err = snd_pcm_prepare(dev.pcm)) != 0)
    {
        DEBUGPRINT("snd_pcm_prepare fail %s", snd_strerror(err));
        goto error;
    }

    snd_pcm_hw_params_get_channels(params, &uintParam);
    DEBUGPRINT("num channels %u | %u", uintParam, dev.channels);

    snd_pcm_hw_params_get_periods(params, &uintParam, nullptr);
    DEBUGPRINT("num periods %u | %u", uintParam, dev.periods);

    snd_pcm_hw_params_get_period_size(params, &ulongParam, nullptr);
    DEBUGPRINT("period size %lu | %u", ulongParam, dev.bufferSize);

    snd_pcm_hw_params_get_buffer_size(params, &ulongParam);
    DEBUGPRINT("buffer size %lu | %u", ulongParam, dev.bufferSize * dev.periods);

    {
        const size_t rawbufferlen = getSampleSizeFromHints(dev.hints) * dev.bufferSize * dev.channels * 2;
        dev.buffers.raw = new int8_t[rawbufferlen];
        dev.buffers.f32 = new float*[dev.channels];
        dev.ringbuffers = new HeapRingBuffer[dev.channels];

        for (uint8_t c=0; c<dev.channels; ++c)
        {
            dev.buffers.f32[c] = new float[dev.bufferSize * 2];
            dev.ringbuffers[c].createBuffer(sizeof(float) * dev.bufferSize * 5);
        }

        sem_init(&dev.sem, 0, 0);

        DeviceAudio* const devptr = new DeviceAudio;
        std::memcpy(devptr, &dev, sizeof(dev));

        void* (*threadCall)(void*) = playback ? devicePlaybackThread : deviceCaptureThread;

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
        sched_param sched = {};
        sched.sched_priority = playback ? 69 : 70;
        pthread_attr_setschedparam(&attr, &sched);
        if (pthread_create(&devptr->thread, &attr, threadCall, devptr) != 0)
        {
            pthread_attr_destroy(&attr);
            pthread_attr_init(&attr);
            if (pthread_create(&devptr->thread, &attr, threadCall, devptr) != 0)
            {
                pthread_attr_destroy(&attr);
                goto error;
            }
        }
        pthread_attr_destroy(&attr);

        return devptr;
    }

error:
    snd_pcm_close(dev.pcm);
    return nullptr;
}

void runDeviceAudio(DeviceAudio* const dev, float* buffers[])
{
    const uint32_t frame = dev->frame;
    dev->frame += dev->bufferSize;

    if (dev->hints & kDeviceCapture)
        runDeviceAudioCapture(dev, buffers, frame);
    else
        runDeviceAudioPlayback(dev, buffers, frame);
}

void closeDeviceAudio(DeviceAudio* const dev)
{
    const uint8_t channels = dev->channels;
    dev->channels = 0;
    sem_post(&dev->sem);
    pthread_join(dev->thread, nullptr);

    sem_destroy(&dev->sem);
    snd_pcm_close(dev->pcm);

    for (uint8_t c=0; c<channels; ++c)
        delete[] dev->buffers.f32[c];
    delete[] dev->buffers.f32;
    delete[] dev->buffers.raw;

    delete dev;
}

// --------------------------------------------------------------------------------------------------------------------

#include "audio-capture.cpp"
#include "audio-playback.cpp"

// --------------------------------------------------------------------------------------------------------------------
