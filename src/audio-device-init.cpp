// SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-device-init.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

// --------------------------------------------------------------------------------------------------------------------

// private
static void deviceFailInitHints(DeviceAudio* dev);
static void deviceTimedWait(DeviceAudio* dev);
static void runDeviceAudioCapture(DeviceAudio* dev, float* buffers[], uint32_t frame);
static void runDeviceAudioPlayback(DeviceAudio* dev, float* buffers[], uint32_t frame);
static void* deviceCaptureThread(void* arg);
static void* devicePlaybackThread(void* arg);

// TODO cleanup, see what is needed
static int xrun_recovery(snd_pcm_t *handle, int err);

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
    dev->balance.ratio = 1.0;
    dev->timestamps.alsaStartTime = dev->timestamps.jackStartFrame = 0;
    dev->timestamps.ratio = 1.0;
    dev->framesDone = 0;
}

static void deviceTimedWait(DeviceAudio* const dev)
{
    if (sem_trywait(&dev->sem) == 0)
        return;

    const uint32_t periodTimeOver4 = (std::max(1, dev->bufferSize / 1) * 1000000) / dev->sampleRate * 1000;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += periodTimeOver4;
    if (ts.tv_nsec >= 1000000000LL)
    {
        ++ts.tv_sec;
        ts.tv_nsec -= 1000000000LL;
    }
    sem_timedwait(&dev->sem, &ts);
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
            if ((err = snd_pcm_hw_params_set_buffer_size_max(dev.pcm, params, &ulongParam)) != 0)
            {
                DEBUGPRINT("snd_pcm_hw_params_set_buffer_size_max fail %u %u %s", periods, bufferSize, snd_strerror(err));
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

    dev.hwstatus.periods = uintParam;

    if (snd_pcm_hw_params_set_channels(dev.pcm, params, 2) == 0)
    {
        dev.hwstatus.channels = 2;
    }
    else if ((err = snd_pcm_hw_params_get_channels(params, &uintParam)) != 0)
    {
        DEBUGPRINT("snd_pcm_hw_params_get_channels fail %s", snd_strerror(err));
        goto error;
    }
    else
    {
        dev.hwstatus.channels = uintParam;
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

    if (playback)
    {
        // unused in playback?
        if ((err = snd_pcm_sw_params_set_avail_min(dev.pcm, swparams, 0)) != 0)
        {
            DEBUGPRINT("snd_pcm_sw_params_set_avail_min fail %s", snd_strerror(err));
            goto error;
        }

        // how many samples we need to write until audio hw starts
        if ((err = snd_pcm_sw_params_set_start_threshold(dev.pcm, swparams, bufferSize)) != 0)
        {
            DEBUGPRINT("snd_pcm_sw_params_set_start_threshold fail %s", snd_strerror(err));
            goto error;
        }
    }
    else
    {
        if ((err = snd_pcm_sw_params_set_avail_min(dev.pcm, swparams, 1)) != 0)
        {
            DEBUGPRINT("snd_pcm_sw_params_set_avail_min fail %s", snd_strerror(err));
            goto error;
        }

        if ((err = snd_pcm_sw_params_set_start_threshold(dev.pcm, swparams, 0)) != 0)
        {
            DEBUGPRINT("snd_pcm_sw_params_set_start_threshold fail %s", snd_strerror(err));
            goto error;
        }
    }

    if ((err = snd_pcm_sw_params_set_stop_threshold(dev.pcm, swparams, (snd_pcm_uframes_t)-1)) != 0)
    {
        DEBUGPRINT("snd_pcm_sw_params_set_stop_threshold fail %s", snd_strerror(err));
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
    DEBUGPRINT("num channels %u | %u", uintParam, dev.hwstatus.channels);
    dev.hwstatus.channels = uintParam;

    snd_pcm_hw_params_get_periods(params, &uintParam, nullptr);
    DEBUGPRINT("num periods %u | %u", uintParam, dev.hwstatus.periods);
    dev.hwstatus.periods = uintParam;

    snd_pcm_hw_params_get_period_size(params, &ulongParam, nullptr);
    DEBUGPRINT("period size %lu | %u", ulongParam, dev.bufferSize);
    dev.hwstatus.periodSize = ulongParam;

    snd_pcm_hw_params_get_buffer_size(params, &ulongParam);
    DEBUGPRINT("buffer size %lu | %u", ulongParam, dev.bufferSize * dev.hwstatus.periods);
    dev.hwstatus.bufferSize = ulongParam;

    {
        const uint8_t channels = dev.hwstatus.channels;
        const size_t rawbufferlen = getSampleSizeFromHints(dev.hints) * dev.bufferSize * channels * 2;

        dev.buffers.raw = new int8_t[rawbufferlen];
        dev.buffers.f32 = new float*[channels];

        for (uint8_t c=0; c<channels; ++c)
            dev.buffers.f32[c] = new float[dev.bufferSize * 2];

        dev.ringbuffer = new AudioRingBuffer;
        dev.ringbuffer->createBuffer(channels, dev.bufferSize * 32);

        sem_init(&dev.sem, 0, 0);

        snd_pcm_status_malloc(&dev.status);
        snd_pcm_status_malloc(&dev.statusRT);
        std::memset(dev.status, 0, snd_pcm_status_sizeof());

        pthread_mutexattr_t mutexattr;
        pthread_mutexattr_init(&mutexattr);
        pthread_mutexattr_setprotocol(&mutexattr, PTHREAD_PRIO_INHERIT);
        pthread_mutex_init(&dev.statuslock, &mutexattr);
        pthread_mutexattr_destroy(&mutexattr);

        DeviceAudio* const devptr = new DeviceAudio;
        std::memcpy(devptr, &dev, sizeof(dev));

        void* (*threadCall)(void*) = playback ? devicePlaybackThread : deviceCaptureThread;

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM);
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

bool runDeviceAudio(DeviceAudio* const dev, float* buffers[])
{
    const uint32_t frame = dev->frame;

    if (dev->hints & kDeviceCapture)
        runDeviceAudioCapture(dev, buffers, frame);
    else
        runDeviceAudioPlayback(dev, buffers, frame);

    dev->frame += dev->bufferSize;

    return dev->thread != 0;
}

void closeDeviceAudio(DeviceAudio* const dev)
{
    const uint8_t channels = dev->hwstatus.channels;

    if (dev->thread != 0)
    {
        dev->hwstatus.channels = 0;
        sem_post(&dev->sem);
        pthread_join(dev->thread, nullptr);
    }

    sem_destroy(&dev->sem);
    pthread_mutex_destroy(&dev->statuslock);
    snd_pcm_status_free(dev->status);
    snd_pcm_status_free(dev->statusRT);
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
