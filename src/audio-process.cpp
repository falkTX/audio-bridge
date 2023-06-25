// SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-process.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#define DEBUGPRINT(...) printf(__VA_ARGS__); puts("");

static constexpr const snd_pcm_format_t kFormatsToTry[] = {
    SND_PCM_FORMAT_S16_LE,
    // SND_PCM_FORMAT_U16_LE,
    // SND_PCM_FORMAT_S24_LE,
    SND_PCM_FORMAT_S24_3LE,
    // SND_PCM_FORMAT_U24_LE,
    // SND_PCM_FORMAT_U24_3LE,
    // SND_PCM_FORMAT_S32_LE,
    // SND_PCM_FORMAT_U32_LE,
    // SND_PCM_FORMAT_FLOAT_LE,
    // SND_PCM_FORMAT_S20_LE,
    // SND_PCM_FORMAT_U20_LE,
    // SND_PCM_FORMAT_S8,
    // SND_PCM_FORMAT_U8,
};

static constexpr const unsigned kPeriodsToTry[] = { 3, 2, 4 };

static const char* SND_PCM_FORMAT_STRING(const snd_pcm_format_t format)
{
    switch (format)
    {
    #define RET_STR(F) case F: return #F;
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

// TODO cleanup, see what is needed
static int xrun_recovery(snd_pcm_t *handle, int err)
{
    static int count = 0;
    // if ((count % 200) == 0)
    {
        count = 1;
        printf("stream recovery\n");
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

static /*constexpr*/ int16_t float16(const float s)
{
    return std::max<int16_t>(-32767, std::min<int16_t>(32767, static_cast<int16_t>(std::lrintf(s * 32767.f))));
}

static /*constexpr*/ int32_t float24(const float s)
{
    return std::min<int32_t>(-8388607, std::max<int32_t>(8388607, static_cast<int32_t>(std::lrintf(s * 8388607.f))));
}

static void float2s16(void* const dst, float* const* const src, const unsigned channels, const unsigned samples)
{
    int16_t* const dst2 = static_cast<int16_t*>(dst);

    for (unsigned i=0; i<samples; ++i)
        for (unsigned c=0; c<channels; ++c)
            dst2[i*channels+c] = float16(src[c][i]);
}

static void float2s24le3(int8_t* dst, float* src1, float* src2, unsigned nsamples)
{
    int32_t z;

    while (nsamples--)
    {
        z = float24(*src1);
// #if __BYTE_ORDER == __LITTLE_ENDIAN
        dst[0] = static_cast<int8_t>(z);
        dst[1] = static_cast<int8_t>(z>>8);
        dst[2] = static_cast<int8_t>(z>>16);
// #elif __BYTE_ORDER == __BIG_ENDIAN
//         dst[0] = (char)(z);
//         dst[1] = (char)(z>>8);
//         dst[2] = (char)(z>>16);
// #endif
        dst += 3;
        ++src1;

        z = float24(*src2);
// #if __BYTE_ORDER == __LITTLE_ENDIAN
        dst[0] = static_cast<int8_t>(z);
        dst[1] = static_cast<int8_t>(z>>8);
        dst[2] = static_cast<int8_t>(z>>16);
// #elif __BYTE_ORDER == __BIG_ENDIAN
//         dst[0] = (char)(z);
//         dst[1] = (char)(z>>8);
//         dst[2] = (char)(z>>16);
// #endif
        dst += 3;
        ++src2;
    }
}

DeviceAudio* initDeviceAudio(const char* const deviceID, const unsigned bufferSize, const unsigned sampleRate)
{
    int err;
    DeviceAudio dev = {};
    dev.bufferSize = bufferSize;
    dev.channels = 2;

    if ((err = snd_pcm_open(&dev.pcm, deviceID, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK)) < 0)
    {
        DEBUGPRINT("snd_pcm_open fail: %s\n", snd_strerror(err));
        return nullptr;
    }

    snd_pcm_hw_params_t* params;
    snd_pcm_hw_params_alloca(&params);

    unsigned periodsParam = 0;
    unsigned sampleRateParam = sampleRate;
    unsigned long bufferSizeParam;

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

#if 0
    for (snd_pcm_format_t format : kFormatsToTry)
    {
#else
    for (int i = 0; i < SND_PCM_FORMAT_LAST; ++i)
    {
        snd_pcm_format_t format = static_cast<snd_pcm_format_t>(i);
        if (format == SND_PCM_FORMAT_S16_LE) continue;
#endif

        if ((err = snd_pcm_hw_params_set_format(dev.pcm, params, format)) != 0)
        {
            DEBUGPRINT("snd_pcm_hw_params_set_format fail %u:%s %s", format, SND_PCM_FORMAT_STRING(format), snd_strerror(err));
            continue;
        }

        DEBUGPRINT("snd_pcm_hw_params_set_format ok %u:%s", format, SND_PCM_FORMAT_STRING(format));
        dev.format = format;
        break;
    }

    if (dev.format == SND_PCM_FORMAT_UNKNOWN)
    {
        DEBUGPRINT("snd_pcm_hw_params_set_format fail %s", snd_strerror(err));
        goto error;
    }

    if ((err = snd_pcm_hw_params_set_channels(dev.pcm, params, dev.channels)) != 0)
    {
        DEBUGPRINT("snd_pcm_hw_params_set_channels fail %s", snd_strerror(err));
        goto error;
    }

    if ((err = snd_pcm_hw_params_set_rate_near(dev.pcm, params, &sampleRateParam, 0)) != 0)
    {
        DEBUGPRINT("snd_pcm_hw_params_set_rate_near fail %s", snd_strerror(err));
        goto error;
    }

    if (sampleRateParam != sampleRate)
    {
        DEBUGPRINT("sample rate mismatch %u vs %u", sampleRateParam, sampleRate);
        goto error;
    }

    for (unsigned periods : kPeriodsToTry)
    {
        bufferSizeParam = bufferSize * periods;
        if ((err = snd_pcm_hw_params_set_buffer_size_near(dev.pcm, params, &bufferSizeParam)) != 0)
        {
            DEBUGPRINT("snd_pcm_hw_params_set_buffer_size_near fail %s", snd_strerror(err));
            continue;
        }

        bufferSizeParam /= periods;
        if ((err = snd_pcm_hw_params_set_period_size_near(dev.pcm, params, &bufferSizeParam, nullptr)) != 0)
        {
            DEBUGPRINT("snd_pcm_hw_params_set_period_size_near fail %s", snd_strerror(err));
            continue;
        }

        if (bufferSizeParam == bufferSize)
        {
            DEBUGPRINT("buffer size match %u, using %u periods", bufferSize, periods);
            periodsParam = periods;
            break;
        }

        DEBUGPRINT("buffer size mis match %lu vs %u, using %u periods", bufferSizeParam, bufferSize, periods);
    }

    if (periodsParam == 0)
    {
        DEBUGPRINT("can't find a buffer size match");
        goto error;
    }

    if ((err = snd_pcm_hw_params(dev.pcm, params)) != 0)
    {
        DEBUGPRINT("snd_pcm_hw_params fail %s", snd_strerror(err));
        goto error;
    }

    if ((err = snd_pcm_prepare(dev.pcm)) != 0)
    {
        DEBUGPRINT("snd_pcm_prepare fail %s", snd_strerror(err));
        goto error;
    }

    DEBUGPRINT("period size BEFORE %lu", bufferSizeParam);
    snd_pcm_hw_params_get_period_size(params, &bufferSizeParam, nullptr);
    DEBUGPRINT("period size AFTER %lu", bufferSizeParam);

    snd_pcm_hw_params_get_buffer_size(params, &bufferSizeParam);
    DEBUGPRINT("buffer size %lu", bufferSizeParam);

    DEBUGPRINT("periods BEFORE %u", periodsParam);
    snd_pcm_hw_params_get_periods(params, &periodsParam, nullptr);
    DEBUGPRINT("periods AFTER %u", periodsParam);

    {
        const unsigned slen = dev.format == SND_PCM_FORMAT_S24_3LE ? 3 : 2;
        dev.buffer = std::malloc(slen * dev.bufferSize * dev.channels);

        DeviceAudio* const devptr = new DeviceAudio;
        std::memcpy(devptr, &dev, sizeof(dev));

        return devptr;
    }

error:
    snd_pcm_close(dev.pcm);
    return nullptr;
}

void runDeviceAudio(DeviceAudio* const dev, float* buffers[2])
{
    // std::memset(t.b1, 0, sizeof(float)*frames);
    // std::memset(t.b2, 0, sizeof(float)*frames);

    // const snd_pcm_state_t state = snd_pcm_state(dev->pcm);
    //
    // static snd_pcm_state_t last_state = SND_PCM_STATE_PRIVATE1;
    // if (last_state != state)
    // {
    //     last_state = state;
    //     printf("alsa changed state %u\n", state);
    // }
    //
    // switch (state)
    // {
    // case SND_PCM_STATE_OPEN:
    // case SND_PCM_STATE_SETUP:
    //     break;
    // case SND_PCM_STATE_PREPARED:
    //     // printf("SND_PCM_STATE_PREPARED %d\n", snd_pcm_start(dev->pcm));
    //     break;
    // case SND_PCM_STATE_RUNNING:
    // case SND_PCM_STATE_XRUN:
    // case SND_PCM_STATE_DRAINING:
    // case SND_PCM_STATE_PAUSED:
    // case SND_PCM_STATE_SUSPENDED:
    // case SND_PCM_STATE_DISCONNECTED:
    //     break;
    // }
    //
    // int avail = snd_pcm_avail_update(handle);

    const unsigned channels = dev->channels;
    unsigned frames = dev->bufferSize;

    // this assumes SND_PCM_ACCESS_MMAP_INTERLEAVED
    switch (dev->format)
    {
    case SND_PCM_FORMAT_S16_LE:
    {
        float2s16(dev->buffer, buffers, channels, frames);

        int8_t* ptr = static_cast<int8_t*>(dev->buffer);

        static int first = 0;
        int err, tries = 0;

        while (frames > 0)
        {
            err = snd_pcm_mmap_writei(dev->pcm, ptr, frames);

            if (err == -EAGAIN)
            {
                if (++first < 3)
                {
                    DEBUGPRINT("err == -EAGAIN [FIRST %d]", first);
                    return;
                }
                else
                {
                    DEBUGPRINT("err == -EAGAIN");
                }
                continue;
            }

            if (err < 0)
            {
                if (xrun_recovery(dev->pcm, err) < 0)
                {
                    printf("Write error: %s\n", snd_strerror(err));
                    exit(EXIT_FAILURE);
                }
                break;  /* skip one period */
            }

            ptr += err * channels * 2;
            frames -= err;
        }
        break;
    }
    case SND_PCM_FORMAT_S24_3LE:
    {
        int8_t* ptr = static_cast<int8_t*>(dev->buffer);
        float2s24le3(ptr, buffers[0], buffers[1], frames);

        static int first = 0;
        int err, tries = 0;

        while (frames > 0)
        {
            err = snd_pcm_mmap_writei(dev->pcm, ptr, frames);

            if (err == -EAGAIN)
            {
                if (++first < 3)
                {
                    DEBUGPRINT("err == -EAGAIN [s24 FIRST %d]", first);
                    return;
                }
                else
                {
                    DEBUGPRINT("err == -EAGAIN");
                }
                continue;
            }

            if (err < 0)
            {
                if (xrun_recovery(dev->pcm, err) < 0)
                {
                    printf("Write error: %s\n", snd_strerror(err));
                    exit(EXIT_FAILURE);
                }
                break;  /* skip one period */
            }

            ptr += err * 2 * 3;
            frames -= err;
        }
        break;
    }
    }
}

void closeDeviceAudio(DeviceAudio* const dev)
{
    snd_pcm_close(dev->pcm);
    delete dev;
}
