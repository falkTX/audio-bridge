// SPDX-FileCopyrightText: 2021-2025 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-device.hpp"
#include "audio-device-impl.hpp"
#include "audio-utils.hpp"

#include <ctime>
#include <cstring>
#include <memory>

//#define ALSA_PCM_NEW_HW_PARAMS_API
//#define ALSA_PCM_NEW_SW_PARAMS_API
#include <alsa/asoundlib.h>
#include <pthread.h>
// #include <semaphore.h>

// --------------------------------------------------------------------------------------------------------------------

static constexpr const unsigned kNumPeriodsToTry[] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };

static constexpr const snd_pcm_format_t kSampleFormatsToTry[] = {
    SND_PCM_FORMAT_S32,
    SND_PCM_FORMAT_S24_3LE,
    SND_PCM_FORMAT_S24,
    SND_PCM_FORMAT_S16,
};

static constexpr const unsigned kSampleRatesToTry[] = { /*48000,*/ 96000, 88200, 44100 };

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

struct AudioDevice::Impl {
    // config copy
    bool playback;
    uint16_t bufferSize;

    // hwconfig copy
    SampleFormat format;
    uint8_t numChannels;
    uint16_t periodSize;
    uint32_t fullBufferSize;

    // direct pointer
    AudioRingBuffer* ringbuffer;

    // monotonic frame counter
    uint32_t frame = 0;

    // ALSA PCM handle
    snd_pcm_t* pcm = nullptr;

    // sync primitives
    pthread_t thread;
//     sem_t sem;

    // whether audio device is closing down, triggered by `closeAudioDeviceImpl`
    bool closing = false;

    // whether audio device has been disconnected
    bool disconnected = false;
};

// --------------------------------------------------------------------------------------------------------------------

static void* _audio_device_capture_thread(void* const arg)
{
    AudioDevice::Impl* const impl = static_cast<AudioDevice::Impl*>(arg);

    while (! impl->closing)
    {
    }

    impl->disconnected = true;
    return nullptr;
}

// TODO cleanup, see what is needed
static int _xrun_recovery(snd_pcm_t *handle, int err)
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

static void* _audio_device_playback_thread(void* const arg)
{
    AudioDevice::Impl* const impl = static_cast<AudioDevice::Impl*>(arg);

    DeviceState state = kDeviceInitializing;

    const uint8_t sampleSize = getSampleSizeFromFormat(impl->format);
    const uint8_t numChannels = impl->numChannels;
    const uint16_t periodSize = impl->periodSize;
    DEBUGPRINT("_audio_device_playback_thread sampleSize %u numChannels %u periodSize %u",
               sampleSize, numChannels, periodSize);

    uint8_t* const raw = new uint8_t [periodSize * sampleSize * numChannels];

#ifdef MUSIC_TEST
    float *musicFull, *musicR;
    size_t music_size;
    size_t music_pos = 0;
    FILE* const f = fopen("/home/falktx/Source/falkTX/audio-bridge/out.raw", "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    music_size = ftell(f);
    assert(music_size % sizeof(float) == 0);
    music_size /= sizeof(float);
    fseek(f, 0, SEEK_SET);
    musicFull = new float [music_size];
    fread(musicFull, sizeof(float), music_size, f);
    fclose(f);

    music_size /= 2;

    musicR = new float [music_size];
    for (size_t i = 0; i < music_size; ++i)
    {
        musicR[i] = musicFull[i * 2 + 1];
        musicFull[i] = musicFull[i * 2];
    }
#else
    float** f32 = new float* [numChannels];
    for (uint8_t i = 0; i < numChannels; ++i)
        f32[i] = new float [periodSize];
#endif

    snd_pcm_sframes_t err;
    while (! impl->closing)
    {
        if (state == kDeviceInitializing)
        {
            // write silence until alsa buffers are full
            bool started = false;
            std::memset(raw, 0, periodSize * sampleSize * numChannels);
            while ((err = snd_pcm_mmap_writei(impl->pcm, raw, periodSize)) > 0)
                started = true;

            if (err != -EAGAIN)
            {
                printf("%08u | playback | initial write error: %s\n", impl->frame, snd_strerror(err));
                break;
            }

            if (started)
            {
                DEBUGPRINT("%08u | playback | can write data? removing kDeviceInitializing", impl->frame);
                // restart();
//                 music_pos = 0;
                state = kDeviceStarting;
            }
            else
            {
                // deviceTimedWait(dev);
                DEBUGPRINT("%08u | playback | kDeviceInitializing waiting 1 cycle", impl->frame);
                snd_pcm_wait(impl->pcm, -1);
//                 usleep(0);
//                 sched_yield();
                continue;
            }
        }

        if (state == kDeviceStarting)
        {
            // try writing a single sample to see if device is running
            // TODO replace with avail check
            std::memset(raw, 0, sampleSize * numChannels);
            err = snd_pcm_mmap_writei(impl->pcm, raw, 1);

            switch (err)
            {
            case 1:
                DEBUGPRINT("%08u | playback | wrote data, removing kDeviceStarting", impl->frame);
                state = kDeviceBuffering;
                snd_pcm_rewind(impl->pcm, 1);
                break;
            case -EAGAIN:
                // deviceTimedWait(dev);
                DEBUGPRINT("%08u | playback | kDeviceStarting waiting 1 cycle", impl->frame);
                snd_pcm_wait(impl->pcm, -1);
//                 usleep(0);
//                 sched_yield();
                continue;
            default:
                printf("%08u | playback | initial write error: %s\n", impl->frame, snd_strerror(err));
                break;
            }
        }

        if (state == kDeviceBuffering)
        {
            if (impl->ringbuffer->getNumReadableSamples() < periodSize * 256)
            {
//                 DEBUGPRINT("%08u | playback | kDeviceBuffering waiting 1 cycle because ringbuffer not ready, %u",
//                         impl->frame, impl->ringbuffer->getNumReadableSamples());

                std::memset(raw, 0, periodSize * sampleSize * numChannels);
                snd_pcm_mmap_writei(impl->pcm, raw, periodSize);
                snd_pcm_wait(impl->pcm, -1);
                continue;
            }

            DEBUGPRINT("%08u | playback | has enough ringbuffer data, removing kDeviceBuffering, %u vs %u",
                       impl->frame, impl->ringbuffer->getNumReadableSamples() , periodSize * 256);
            state = kDeviceRunning;
        }

#ifdef MUSIC_TEST
        const float* f32[2] = {
            musicFull + music_pos,
            musicR + music_pos,
        };

        if ((music_pos += periodSize) >= music_size)
            music_pos = 0;
#else
        while (!impl->ringbuffer->read(f32, periodSize))
        {
            DEBUGPRINT("%08u | playback | WARNING | failed reading data", impl->frame);
            sched_yield();

            state = kDeviceBuffering;
            std::memset(raw, 0, periodSize * sampleSize * numChannels);
            snd_pcm_mmap_writei(impl->pcm, raw, periodSize);
            snd_pcm_wait(impl->pcm, -1);
            continue;
        }

        static int counter = 0;
        if (++counter == 250)
        {
            counter = 0;
            DEBUGPRINT("%08u | playback | check %u vs %u",
                       impl->frame, impl->ringbuffer->getNumReadableSamples() , periodSize * 1024);
        }
#endif

        if (impl->closing)
            break;

#if 0
        if (enabled != dev->enabled)
        {
            enabled = dev->enabled;
            gain.setTargetValue(enabled ? 1.f : 0.f);
        }

        if (rbRatio != dev->rbRatio)
        {
            rbRatio = dev->rbRatio;
            resampler->set_rratio(rbRatio);
        }

#endif

//         for (uint16_t i = 0; i < frames; ++i)
//         {
//             xgain = gain.next();
//
//             for (uint8_t c = 0; c < numChannels; ++c)
//                 f32[c][i] *= xgain;
//         }

        switch (impl->format)
        {
        case kSampleFormat16:
            float2int::s16(raw, f32, numChannels, periodSize);
            break;
        case kSampleFormat24:
            float2int::s24(raw, f32, numChannels, periodSize);
            break;
        case kSampleFormat24LE3:
            float2int::s24le3(raw, f32, numChannels, periodSize);
            break;
        case kSampleFormat32:
            float2int::s32(raw, f32, numChannels, periodSize);
            break;
        default:
            DEBUGPRINT("unknown format");
            break;
        }

        uint8_t* ptr = raw;
        uint16_t frames = periodSize;

        while (! impl->closing && frames != 0)
        {
            err = snd_pcm_mmap_writei(impl->pcm, ptr, frames);
//             DEBUGPRINT("write %ld of %u", err, frames);

            if (err < 0)
            {
                if (err == -EAGAIN)
                {
                    // deviceTimedWait(dev);
//                     DEBUGPRINT("%08u | playback | kDeviceBuffering waiting 1 cycle", impl->frame);
                    snd_pcm_wait(impl->pcm, -1);
//                     usleep(0);
//                     sched_yield();
                    continue;
                }

                // restart();

                printf("%08u | playback | Write error: %s\n", impl->frame, snd_strerror(err));

//                 if (_xrun_recovery(impl->pcm, err) < 0)
                {
//                     printf("playback | xrun_recovery error: %s\n", snd_strerror(err));
                    goto end;
                }

                break;
            }

            // FIXME check against snd_pcm_sw_params_set_avail_min ??
            if (static_cast<uint16_t>(err) != frames)
            {
                DEBUGPRINT("%08u | playback | Incomplete write %ld of %u", impl->frame, err, frames);

                ptr += err * numChannels * sampleSize;
                frames -= err;

                // deviceTimedWait(dev);
                usleep(0);
                sched_yield();
                continue;
            }

            break;
        }

//         DEBUGPRINT("%08u | playback | DEBUG | writing done", impl->frame);
    }

end:
#ifndef MUSIC_TEST
    for (uint8_t i = 0; i < numChannels; ++i)
        delete[] f32[i];

    delete[] f32;
#endif

    delete[] raw;

    impl->disconnected = true;
    return nullptr;
}

// --------------------------------------------------------------------------------------------------------------------

AudioDevice::Impl* initAudioDeviceImpl(const AudioDevice* const dev, AudioDevice::HWConfig& hwconfig)
{
    std::unique_ptr<AudioDevice::Impl> impl = std::unique_ptr<AudioDevice::Impl>(new AudioDevice::Impl);
    impl->playback = dev->config.playback;
    impl->bufferSize = dev->config.bufferSize;
    impl->ringbuffer = const_cast<AudioRingBuffer*>(&dev->ringbuffer);

#if 0
    dev.hints = kDeviceInitializing|kDeviceStarting|kDeviceBuffering;
#endif

    int err;
    const snd_pcm_stream_t mode = dev->config.playback ? SND_PCM_STREAM_PLAYBACK : SND_PCM_STREAM_CAPTURE;

    // SND_PCM_ASYNC
    constexpr int flags = 0
                        | SND_PCM_NONBLOCK
                        | SND_PCM_NO_AUTO_CHANNELS
                        | SND_PCM_NO_AUTO_FORMAT
                        | SND_PCM_NO_AUTO_RESAMPLE
                        | SND_PCM_NO_SOFTVOL;
    if ((err = snd_pcm_open(&impl->pcm, dev->config.deviceID, mode, flags)) < 0)
    {
        DEBUGPRINT("snd_pcm_open fail %d %s\n", dev->config.playback, snd_strerror(err));
        return nullptr;
    }

    snd_pcm_hw_params_t* params;
    snd_pcm_hw_params_alloca(&params);

    snd_pcm_sw_params_t* swparams;
    snd_pcm_sw_params_alloca(&swparams);

    unsigned uintParam;
    unsigned long ulongParam;

    snd_pcm_t* const pcm = impl->pcm;

    // ----------------------------------------------------------------------------------------------------------------
    // basic init

    if ((err = snd_pcm_hw_params_any(pcm, params)) < 0)
    {
        DEBUGPRINT("snd_pcm_hw_params_any fail %s", snd_strerror(err));
        goto error;
    }

    if ((err = snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_MMAP_INTERLEAVED)) != 0)
    {
        DEBUGPRINT("snd_pcm_hw_params_set_access fail %s", snd_strerror(err));
        goto error;
    }

    if ((err = snd_pcm_hw_params_set_rate_resample(pcm, params, 0)) != 0)
    {
        DEBUGPRINT("snd_pcm_hw_params_set_rate_resample fail %s", snd_strerror(err));
        goto error;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // sample format

    hwconfig.format = kSampleFormatInvalid;

    for (snd_pcm_format_t format : kSampleFormatsToTry)
    {
        if ((err = snd_pcm_hw_params_set_format(pcm, params, format)) != 0)
        {
            DEBUGPRINT("snd_pcm_hw_params_set_format fail %u:%s %s", format, SND_PCM_FORMAT_STRING(format), snd_strerror(err));
            continue;
        }

        switch (format)
        {
        case SND_PCM_FORMAT_S16:
            hwconfig.format = kSampleFormat16;
            break;
        case SND_PCM_FORMAT_S24:
            hwconfig.format = kSampleFormat24;
            break;
        case SND_PCM_FORMAT_S24_3LE:
            hwconfig.format = kSampleFormat24LE3;
            break;
        case SND_PCM_FORMAT_S32:
            hwconfig.format = kSampleFormat32;
            break;
        default:
            DEBUGPRINT("snd_pcm_hw_params_set_format fail unimplemented format %u:%s",
                       format, SND_PCM_FORMAT_STRING(format));
            continue;
        }

        DEBUGPRINT("snd_pcm_hw_params_set_format %s", SND_PCM_FORMAT_STRING(format));
        break;
    }

    if (hwconfig.format == kSampleFormatInvalid)
    {
        DEBUGPRINT("snd_pcm_hw_params_set_format fail %s", snd_strerror(err));
        goto error;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // sample rate

    hwconfig.sampleRate = 0;

    for (unsigned rate : kSampleRatesToTry)
    {
        if ((err = snd_pcm_hw_params_set_rate(pcm, params, rate, 0)) != 0)
        {
            // DEBUGPRINT("snd_pcm_hw_params_set_rate fail %s", snd_strerror(err));
            continue;
        }

        hwconfig.sampleRate = rate;
        DEBUGPRINT("snd_pcm_hw_params_set_rate %u", rate);
        break;
    }

    if (hwconfig.sampleRate == 0)
    {
        DEBUGPRINT("snd_pcm_hw_params_set_rate fail %s", snd_strerror(err));
        goto error;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // num channels

    if (snd_pcm_hw_params_set_channels(pcm, params, 2) == 0)
    {
        hwconfig.numChannels = 2;
    }
    else if ((err = snd_pcm_hw_params_get_channels(params, &uintParam)) != 0)
    {
        DEBUGPRINT("snd_pcm_hw_params_get_channels fail %s", snd_strerror(err));
        goto error;
    }
    else
    {
        hwconfig.numChannels = uintParam;
    }

    snd_pcm_hw_params_get_channels(params, &uintParam);
    DEBUGPRINT("num channels %u | %u", uintParam, hwconfig.numChannels);
    hwconfig.numChannels = uintParam;

    // ----------------------------------------------------------------------------------------------------------------
    // num periods + period size

    uintParam = 0;
    for (unsigned periods : kNumPeriodsToTry)
    {
        ulongParam = AUDIO_BRIDGE_DEVICE_BUFFER_SIZE * periods;
        if ((err = snd_pcm_hw_params_set_buffer_size_min(pcm, params, &ulongParam)) != 0)
        {
            DEBUGPRINT("snd_pcm_hw_params_set_buffer_size_min fail %u %u %s",
                        periods, AUDIO_BRIDGE_DEVICE_BUFFER_SIZE, snd_strerror(err));
            continue;
        }

        DEBUGPRINT("snd_pcm_hw_params_set_buffer_size_min %u %lu", periods, ulongParam);
        uintParam = periods;
        break;
    }

    if (uintParam == 0)
    {
        DEBUGPRINT("can't find a buffer size match");
        goto error;
    }

    hwconfig.numPeriods = uintParam;
    hwconfig.periodSize = AUDIO_BRIDGE_DEVICE_BUFFER_SIZE;
    hwconfig.fullBufferSize = AUDIO_BRIDGE_DEVICE_BUFFER_SIZE * uintParam;

    snd_pcm_hw_params_get_periods(params, &uintParam, nullptr);
    DEBUGPRINT("num periods %u | %u", uintParam, hwconfig.numPeriods);
    hwconfig.numPeriods = uintParam;

    snd_pcm_hw_params_get_period_size(params, &ulongParam, nullptr);
    DEBUGPRINT("period size %lu | %u", ulongParam, hwconfig.periodSize);
    hwconfig.periodSize = ulongParam;

    snd_pcm_hw_params_get_buffer_size(params, &ulongParam);
    DEBUGPRINT("full buffer size %lu | %u", ulongParam, hwconfig.fullBufferSize);
    hwconfig.fullBufferSize = ulongParam;

    // ----------------------------------------------------------------------------------------------------------------

    if ((err = snd_pcm_hw_params(pcm, params)) != 0)
    {
        DEBUGPRINT("snd_pcm_hw_params fail %s", snd_strerror(err));
        goto error;
    }

    // ----------------------------------------------------------------------------------------------------------------

    if ((err = snd_pcm_sw_params_current(pcm, swparams)) != 0)
    {
        DEBUGPRINT("snd_pcm_sw_params_current fail %s", snd_strerror(err));
        goto error;
    }

    // SND_PCM_TSTAMP_NONE SND_PCM_TSTAMP_ENABLE (= SND_PCM_TSTAMP_MMAP)
    if ((err = snd_pcm_sw_params_set_tstamp_mode(pcm, swparams, SND_PCM_TSTAMP_MMAP)) != 0)
    {
        DEBUGPRINT("snd_pcm_sw_params_set_tstamp_mode fail %s", snd_strerror(err));
        goto error;
    }

    // SND_PCM_TSTAMP_TYPE_MONOTONIC
    if ((err = snd_pcm_sw_params_set_tstamp_type(pcm, swparams, SND_PCM_TSTAMP_TYPE_MONOTONIC_RAW)) != 0)
    {
        DEBUGPRINT("snd_pcm_sw_params_set_tstamp_type fail %s", snd_strerror(err));
        goto error;
    }

#if 0
    if (playback)
#endif
    {
        // unused in playback?
        if ((err = snd_pcm_sw_params_set_avail_min(pcm, swparams, hwconfig.periodSize)) != 0)
        {
            DEBUGPRINT("snd_pcm_sw_params_set_avail_min fail %s", snd_strerror(err));
            goto error;
        }

        // how many samples we need to write until audio hw starts
        if ((err = snd_pcm_sw_params_set_start_threshold(pcm, swparams, 0)) != 0)
        {
            DEBUGPRINT("snd_pcm_sw_params_set_start_threshold fail %s", snd_strerror(err));
            goto error;
        }
    }
#if 0
    else
    {
        if ((err = snd_pcm_sw_params_set_avail_min(pcm, swparams, 1)) != 0)
        {
            DEBUGPRINT("snd_pcm_sw_params_set_avail_min fail %s", snd_strerror(err));
            goto error;
        }

        if ((err = snd_pcm_sw_params_set_start_threshold(pcm, swparams, 0)) != 0)
        {
            DEBUGPRINT("snd_pcm_sw_params_set_start_threshold fail %s", snd_strerror(err));
            goto error;
        }
    }
#endif

    if ((err = snd_pcm_sw_params_set_stop_threshold(pcm, swparams, (snd_pcm_uframes_t)-1)) != 0)
    {
        DEBUGPRINT("snd_pcm_sw_params_set_stop_threshold fail %s", snd_strerror(err));
        goto error;
    }

    if ((err = snd_pcm_sw_params_set_silence_threshold(pcm, swparams, 0)) != 0)
    {
        DEBUGPRINT("snd_pcm_sw_params_set_silence_threshold fail %s", snd_strerror(err));
        goto error;
    }

    if ((err = snd_pcm_sw_params(pcm, swparams)) != 0)
    {
        DEBUGPRINT("snd_pcm_sw_params fail %s", snd_strerror(err));
        goto error;
    }

    if ((err = snd_pcm_prepare(pcm)) != 0)
    {
        DEBUGPRINT("snd_pcm_prepare fail %s", snd_strerror(err));
        goto error;
    }

//     dev.enabled = true;

    {
#if 0
        const uint16_t blocks = (playback ? AUDIO_BRIDGE_PLAYBACK_RINGBUFFER_BLOCKS
                                          : AUDIO_BRIDGE_CAPTURE_RINGBUFFER_BLOCKS);

        dev.ringbuffer = new AudioRingBuffer;
        dev.ringbuffer->createBuffer(channels, dev.bufferSize * blocks);

        dev.rbFillTarget = static_cast<double>(playback ? 1 : AUDIO_BRIDGE_CAPTURE_LATENCY_BLOCKS) / blocks;
        dev.rbTotalNumSamples = dev.bufferSize * blocks / kRingBufferDataFactor;
        dev.rbRatio = 1.0;
        printf("target is %f\n", dev.rbFillTarget);
#endif

//         sem_init(&impl->sem, 0, 0);

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM);
        pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
        sched_param sched = {};
        sched.sched_priority = dev->config.playback ? 69 : 70;
        pthread_attr_setschedparam(&attr, &sched);

        void* (*const audio_device_thread)(void*) = dev->config.playback
                                                  ? _audio_device_playback_thread
                                                  : _audio_device_capture_thread;
        if (pthread_create(&impl->thread, &attr, audio_device_thread, impl.get()) != 0)
        {
            pthread_attr_destroy(&attr);
            pthread_attr_init(&attr);
            if (pthread_create(&impl->thread, &attr, audio_device_thread, impl.get()) != 0)
            {
                pthread_attr_destroy(&attr);
                goto error;
            }
        }
        pthread_attr_destroy(&attr);
    }

    impl->format = hwconfig.format;
    impl->numChannels = hwconfig.numChannels;
    impl->periodSize = hwconfig.periodSize;
    impl->fullBufferSize = hwconfig.fullBufferSize;

    return impl.release();

error:
    snd_pcm_close(pcm);
    return nullptr;
}

void closeAudioDeviceImpl(AudioDevice::Impl* const impl)
{
    impl->closing = true;
//     sem_post(&impl->sem);
    pthread_join(impl->thread, nullptr);

//     sem_destroy(&impl->sem);
    snd_pcm_close(impl->pcm);

    delete impl;
}

bool runAudioDevicePostImpl(AudioDevice::Impl* const impl)
{
    impl->frame += impl->bufferSize;

    return !impl->disconnected;
}

// --------------------------------------------------------------------------------------------------------------------
