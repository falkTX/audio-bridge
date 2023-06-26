// SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-process.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#define DEBUGPRINT(...) printf(__VA_ARGS__); puts("");

// --------------------------------------------------------------------------------------------------------------------

static constexpr const snd_pcm_format_t kFormatsToTry[] = {
    SND_PCM_FORMAT_S32,
    SND_PCM_FORMAT_S24_3LE,
    SND_PCM_FORMAT_S24,
    SND_PCM_FORMAT_S16,
};

static constexpr const unsigned kPeriodsToTry[] = { 3, 2, 4 };

// --------------------------------------------------------------------------------------------------------------------

static constexpr
uint8_t getSampleSizeFromHints(const uint8_t hints)
{
    return hints & kDeviceSample16 ? sizeof(int16_t) :
           hints & kDeviceSample24 ? sizeof(int32_t) :
           hints & kDeviceSample24LE3 ? 3 :
           hints & kDeviceSample32 ? sizeof(int32_t) :
           0;
}

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

// --------------------------------------------------------------------------------------------------------------------

// 0x7fff
static constexpr inline
int16_t float16(const float s)
{
    return s <= -1.f ? -32767 :
           s >= 1.f ? 32767 :
           std::lrintf(s * 32767.f);
}

// 0x7fffff
static constexpr inline
int32_t float24(const float s)
{
    return s <= -1.f ? -8388607 :
           s >= 1.f ? 8388607 :
           std::lrintf(s * 8388607.f);
}

// 0x7fffffff
static constexpr inline
int32_t float32(const double s)
{
    return s <= -1.f ? -2147483647 :
           s >= 1.f ? 2147483647 :
           std::lrint(s * 2147483647.f);
}

// unused, keep it might be useful later
static constexpr inline
int32_t sbit(const int8_t s, const int b)
{
    return s >= 0 ? (s << b) : -((-s) << b);
}

// --------------------------------------------------------------------------------------------------------------------

namespace float2int
{

static inline
void s16(void* const dst, float* const* const src, const uint8_t channels, const uint16_t samples)
{
    int16_t* const dstptr = static_cast<int16_t*>(dst);

    for (uint16_t i=0; i<samples; ++i)
        for (uint8_t c=0; c<channels; ++c)
            dstptr[i*channels+c] = float16(src[c][i]);
}

static inline
void s24(void* const dst, float* const* const src, const uint8_t channels, const uint16_t samples)
{
    int32_t* const dstptr = static_cast<int32_t*>(dst);

    for (uint16_t i=0; i<samples; ++i)
        for (uint8_t c=0; c<channels; ++c)
            dstptr[i*channels+c] = float24(src[c][i]);
}

static inline
void s24le3(void* const dst, float* const* const src, const uint8_t channels, const uint16_t samples)
{
    int8_t* dstptr = static_cast<int8_t*>(dst);
    int32_t z;

    for (uint16_t i=0; i<samples; ++i)
    {
        for (uint8_t c=0; c<channels; ++c)
        {
            z = float24(src[c][i]);
           #if __BYTE_ORDER == __BIG_ENDIAN
            dstptr[2] = static_cast<int8_t>(z);
            dstptr[1] = static_cast<int8_t>(z >> 8);
            dstptr[0] = static_cast<int8_t>(z >> 16);
           #else
            dstptr[0] = static_cast<int8_t>(z);
            dstptr[1] = static_cast<int8_t>(z >> 8);
            dstptr[2] = static_cast<int8_t>(z >> 16);
           #endif
            dstptr += 3;
        }
    }
}

static inline
void s32(void* const dst, float* const* const src, const uint8_t channels, const uint16_t samples)
{
    int32_t* const dstptr = static_cast<int32_t*>(dst);

    for (uint16_t i=0; i<samples; ++i)
        for (uint8_t c=0; c<channels; ++c)
            dstptr[i*channels+c] = float32(src[c][i]);
}

}

// --------------------------------------------------------------------------------------------------------------------

namespace int2float
{

static inline
void s16(float* const* const dst, void* const src, const uint8_t channels, const uint16_t samples)
{
    int16_t* const srcptr = static_cast<int16_t*>(src);

    for (uint16_t i=0; i<samples; ++i)
        for (uint8_t c=0; c<channels; ++c)
            dst[c][i] = static_cast<float>(srcptr[i*channels+c]) * (1.f / 32767.f);
}

static inline
void s24(float* const* const dst, void* const src, const uint8_t channels, const uint16_t samples)
{
    int32_t* const srcptr = static_cast<int32_t*>(src);

    for (uint16_t i=0; i<samples; ++i)
        for (uint8_t c=0; c<channels; ++c)
            dst[c][i] = static_cast<float>(srcptr[i*channels+c]) * (1.f / 8388607.f);
}

static inline
void s24le3(float* const* const dst, void* const src, const uint8_t channels, const uint16_t samples)
{
    uint8_t* srcptr = static_cast<uint8_t*>(src);
    int32_t z;

    for (uint16_t i=0; i<samples; ++i)
    {
        for (uint8_t c=0; c<channels; ++c)
        {
           #if __BYTE_ORDER == __BIG_ENDIAN
            z = (static_cast<int32_t>(srcptr[2]) << 16)
              + (static_cast<int32_t>(srcptr[1]) << 8)
              +  static_cast<int32_t>(srcptr[0]);

            if (srcptr[2] & 0x80)
                z |= 0xff000000;
           #else
            z = (static_cast<int32_t>(srcptr[0]) << 16)
              + (static_cast<int32_t>(srcptr[1]) << 8)
              +  static_cast<int32_t>(srcptr[2]);

            if (srcptr[0] & 0x80)
                z |= 0xff000000;
           #endif

            dst[c][i] = z <= -8388607 ? -1.f
                      : z >= 8388607 ? 1.f
                      : static_cast<float>(z) * (1.f / 8388607.f);

            srcptr += 3;
        }
    }
}

static inline
void s32(float* const* const dst, void* const src, const uint8_t channels, const uint16_t samples)
{
    int32_t* const srcptr = static_cast<int32_t*>(src);

    for (uint16_t i=0; i<samples; ++i)
        for (uint8_t c=0; c<channels; ++c)
            dst[c][i] = static_cast<double>(srcptr[i*channels+c]) * (1.0 / 2147483647.0);
}

}

// --------------------------------------------------------------------------------------------------------------------

DeviceAudio* initDeviceAudio(const char* const deviceID,
                             const bool playback,
                             const uint16_t bufferSize,
                             const uint32_t sampleRate)
{
    int err;
    DeviceAudio dev = {};
    dev.bufferSize = bufferSize;
    dev.channels = 2;
    dev.hints = playback ? kDeviceStarting : kDeviceStarting|kDeviceCapture;

    const snd_pcm_stream_t mode = playback ? SND_PCM_STREAM_PLAYBACK : SND_PCM_STREAM_CAPTURE;

    // SND_PCM_ASYNC
    // SND_PCM_NONBLOCK
    if ((err = snd_pcm_open(&dev.pcm, deviceID, mode, SND_PCM_NONBLOCK)) < 0)
    {
        DEBUGPRINT("snd_pcm_open fail %d %s\n", playback, snd_strerror(err));
        return nullptr;
    }

    snd_pcm_hw_params_t* params;
    snd_pcm_hw_params_alloca(&params);

    snd_pcm_sw_params_t* swparams;
    snd_pcm_sw_params_alloca(&swparams);

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

    for (snd_pcm_format_t format : kFormatsToTry)
    {
        if ((err = snd_pcm_hw_params_set_format(dev.pcm, params, format)) != 0)
        {
            DEBUGPRINT("snd_pcm_hw_params_set_format fail %u:%s %s", format, SND_PCM_FORMAT_STRING(format), snd_strerror(err));
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

        DEBUGPRINT("snd_pcm_hw_params_set_format ok %u:%s", format, SND_PCM_FORMAT_STRING(format));
        break;
    }

    if ((dev.hints & kDeviceSampleHints) == 0)
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
        bufferSizeParam = bufferSize;
        if ((err = snd_pcm_hw_params_set_period_size_min(dev.pcm, params, &bufferSizeParam, nullptr)) != 0)
        {
            DEBUGPRINT("snd_pcm_hw_params_set_period_size_min fail %u %u %lu %s", periods, bufferSize, bufferSizeParam, snd_strerror(err));
            continue;
        }
        bufferSizeParam = bufferSize;
        if ((err = snd_pcm_hw_params_set_period_size_max(dev.pcm, params, &bufferSizeParam, nullptr)) != 0)
        {
            DEBUGPRINT("snd_pcm_hw_params_set_period_size_max fail %u %u %lu %s", periods, bufferSize, bufferSizeParam, snd_strerror(err));
            continue;
        }
        bufferSizeParam = bufferSize;
        if ((err = snd_pcm_hw_params_set_period_size_near(dev.pcm, params, &bufferSizeParam, nullptr)) != 0)
        {
            DEBUGPRINT("snd_pcm_hw_params_set_period_size_near fail %u %u %lu %s", periods, bufferSize, bufferSizeParam, snd_strerror(err));
            continue;
        }

        bufferSizeParam = bufferSize * periods;
        if ((err = snd_pcm_hw_params_set_buffer_size_min(dev.pcm, params, &bufferSizeParam)) != 0)
        {
            DEBUGPRINT("snd_pcm_hw_params_set_buffer_size_min fail %u %u %lu %s", periods, bufferSize, bufferSizeParam, snd_strerror(err));
            continue;
        }
        bufferSizeParam = bufferSize * periods;
        if ((err = snd_pcm_hw_params_set_buffer_size_max(dev.pcm, params, &bufferSizeParam)) != 0)
        {
            DEBUGPRINT("snd_pcm_hw_params_set_buffer_size_max fail %u %u %lu %s", periods, bufferSize, bufferSizeParam, snd_strerror(err));
            continue;
        }
        bufferSizeParam = bufferSize * periods;
        if ((err = snd_pcm_hw_params_set_buffer_size_near(dev.pcm, params, &bufferSizeParam)) != 0)
        {
            DEBUGPRINT("snd_pcm_hw_params_set_buffer_size_near fail %u %u %lu %s", periods, bufferSize, bufferSizeParam, snd_strerror(err));
            continue;
        }

        // bufferSizeParam /= periods;
        // if (bufferSizeParam == bufferSize)
        // {
        periodsParam = bufferSizeParam / bufferSize;
        DEBUGPRINT("buffer size match %u, using %u periods", bufferSize, periodsParam);
        break;
        // }

        // DEBUGPRINT("buffer size mismatch %lu vs %u, using %u periods", bufferSizeParam, bufferSize, periods);
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

    if ((err = snd_pcm_sw_params_current(dev.pcm, swparams)) != 0)
    {
        DEBUGPRINT("snd_pcm_sw_params_current fail %s", snd_strerror(err));
        goto error;
    }

    if ((err = snd_pcm_sw_params_set_start_threshold(dev.pcm, swparams, bufferSize * periodsParam)) != 0)
    {
        DEBUGPRINT("snd_pcm_sw_params_set_start_threshold fail %s", snd_strerror(err));
        goto error;
    }

    if ((err = snd_pcm_sw_params_set_stop_threshold(dev.pcm, swparams, -1)) != 0)
    {
        DEBUGPRINT("snd_pcm_sw_params_set_stop_threshold fail %s", snd_strerror(err));
        goto error;
    }

    if ((err = snd_pcm_sw_params_set_avail_min(dev.pcm, swparams, bufferSize)) != 0)
    {
        DEBUGPRINT("snd_pcm_sw_params_set_avail_min fail %s", snd_strerror(err));
        goto error;
    }

    if ((err = snd_pcm_sw_params(dev.pcm, swparams)) != 0)
    {
        DEBUGPRINT("snd_pcm_sw_params fail %s", snd_strerror(err));
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

    dev.buffer = std::malloc(getSampleSizeFromHints(dev.hints) * dev.bufferSize * dev.channels);

    if (playback)
    {
        DeviceAudio* const devptr = new DeviceAudio;
        std::memcpy(devptr, &dev, sizeof(dev));

        return devptr;
    }
    else
    {
        CaptureDeviceAudio* const devptr = new CaptureDeviceAudio;
        std::memcpy(devptr, &dev, sizeof(dev));

        devptr->readOffset = 0;

        return devptr;
    }

error:
    snd_pcm_close(dev.pcm);
    return nullptr;
}

// --------------------------------------------------------------------------------------------------------------------

// TODO cleanup, see what is needed
static int xrun_recovery(snd_pcm_t *handle, int err)
{
    static int count = 0;
    // if ((count % 200) == 0)
    {
        count = 1;
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

void runDeviceAudio(DeviceAudio* const dev, float* buffers[2])
{
    const uint16_t bufferSize = dev->bufferSize;
    const uint8_t channels = dev->channels;
    const uint8_t hints = dev->hints;
    const uint8_t sampleSize = getSampleSizeFromHints(hints);
    uint16_t frames = bufferSize;
    uint8_t retries = 0;

    // this assumes SND_PCM_ACCESS_MMAP_INTERLEAVED
    if (hints & kDeviceCapture)
    {
        CaptureDeviceAudio* const cdev = static_cast<CaptureDeviceAudio*>(dev);
        int8_t* ptr = static_cast<int8_t*>(dev->buffer);
        int err;

        if (const uint16_t prevOffset = cdev->readOffset)
        {
            frames -= prevOffset;
            ptr += prevOffset * channels * sampleSize;
            cdev->readOffset = 0;
        }

        while (frames != 0)
        {
            err = snd_pcm_mmap_readi(dev->pcm, ptr, frames);
            // DEBUGPRINT("read %d of %u", err, frames);

            if (err == -EAGAIN)
            {
                if ((hints & kDeviceStarting) || ++retries > 10)
                {
                    cdev->readOffset = bufferSize - frames;
                    // DEBUGPRINT("err == -EAGAIN [kDeviceStarting]");

                    for (uint8_t c=0; c<channels; ++c)
                        std::memset(buffers[c], 0, sizeof(float)*bufferSize);

                    return;
                }
                else
                {
                    // DEBUGPRINT("err == -EAGAIN");
                }
                continue;
            }

            if (err < 0)
            {
                dev->hints |= kDeviceStarting;
                if (xrun_recovery(dev->pcm, err) < 0)
                {
                    printf("Read error: %s\n", snd_strerror(err));
                    exit(EXIT_FAILURE);
                }
                break;  /* skip one period */
            }

            if (static_cast<uint16_t>(err) == frames)
            {
                // DEBUGPRINT("Complete read %u", frames);
                dev->hints &= ~kDeviceStarting;
                frames = 0;
                break;
            }

            ptr += err * channels * sampleSize;
            frames -= err;

            DEBUGPRINT("Incomplete read %d of %u, %u left", err, bufferSize, frames);
        }

        switch (hints & kDeviceSampleHints)
        {
        case kDeviceSample16:
            int2float::s16(buffers, dev->buffer, channels, bufferSize);
            break;
        case kDeviceSample24:
            int2float::s24(buffers, dev->buffer, channels, bufferSize);
            break;
        case kDeviceSample24LE3:
            int2float::s24le3(buffers, dev->buffer, channels, bufferSize);
            break;
        case kDeviceSample32:
            int2float::s32(buffers, dev->buffer, channels, bufferSize);
            break;
        default:
            DEBUGPRINT("unknown format");
            break;
        }
    }
    else
    {
        switch (hints & kDeviceSampleHints)
        {
        case kDeviceSample16:
            float2int::s16(dev->buffer, buffers, channels, frames);
            break;
        case kDeviceSample24:
            float2int::s24(dev->buffer, buffers, channels, frames);
            break;
        case kDeviceSample24LE3:
            float2int::s24le3(dev->buffer, buffers, channels, frames);
            break;
        case kDeviceSample32:
            float2int::s32(dev->buffer, buffers, channels, frames);
            break;
        default:
            DEBUGPRINT("unknown format");
            break;
        }

        int8_t* ptr = static_cast<int8_t*>(dev->buffer);
        int err;

        while (frames > 0)
        {
            err = snd_pcm_mmap_writei(dev->pcm, ptr, frames);
            // DEBUGPRINT("write %d of %u", err, frames);

            if (err == -EAGAIN)
            {
                if ((hints & kDeviceStarting) || ++retries > 10)
                {
                    // DEBUGPRINT("err == -EAGAIN [kDeviceStarting]");
                    return;
                }
                else
                {
                    // DEBUGPRINT("err == -EAGAIN");
                }
                continue;
            }

            if (err < 0)
            {
                dev->hints |= kDeviceStarting;
                if (xrun_recovery(dev->pcm, err) < 0)
                {
                    printf("Write error: %s\n", snd_strerror(err));
                    exit(EXIT_FAILURE);
                }
                break;  /* skip one period */
            }

            if (static_cast<uint16_t>(err) == frames)
            {
                dev->hints &= ~kDeviceStarting;
                // DEBUGPRINT("Complete write %u", frames);
                break;
            }

            ptr += err * channels * sampleSize;
            frames -= err;

            DEBUGPRINT("Incomplete write %d of %u, %u left", err, dev->bufferSize, frames);
        }
    }
}

void closeDeviceAudio(DeviceAudio* const dev)
{
    std::free(dev->buffer);
    delete dev;
}

// --------------------------------------------------------------------------------------------------------------------
