// SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-process.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(__SSE2_MATH__)
# include <xmmintrin.h>
#endif

#define DEBUGPRINT(...) printf(__VA_ARGS__); puts("");

// --------------------------------------------------------------------------------------------------------------------

#if 0
typedef struct {
    float* buf;
    size_t len;
} jack_ringbuffer_float_data_t;

void jack_ringbuffer_get_float_write_vector(const jack_ringbuffer_t* const rb, jack_ringbuffer_float_data_t vec[2])
{
    jack_ringbuffer_data_t* const vecptr = static_cast<jack_ringbuffer_data_t*>(static_cast<void*>(vec));
    jack_ringbuffer_get_write_vector(rb, vecptr);
}
#endif

// --------------------------------------------------------------------------------------------------------------------

static constexpr const snd_pcm_format_t kFormatsToTry[] = {
    SND_PCM_FORMAT_S32,
    SND_PCM_FORMAT_S24_3LE,
    SND_PCM_FORMAT_S24,
    SND_PCM_FORMAT_S16,
};

static constexpr const unsigned kPeriodsToTry[] = { 3, 4 };

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
void s16(float* const* const dst, void* const src, const uint16_t offset, const uint8_t channels, const uint16_t samples)
{
    int16_t* const srcptr = static_cast<int16_t*>(src);

    for (uint16_t i=0; i<samples; ++i)
        for (uint8_t c=0; c<channels; ++c)
            dst[c][i+offset] = static_cast<float>(srcptr[i*channels+c]) * (1.f / 32767.f);
}

static inline
void s24(float* const* const dst, void* const src, const uint16_t offset, const uint8_t channels, const uint16_t samples)
{
    int32_t* const srcptr = static_cast<int32_t*>(src);

    for (uint16_t i=0; i<samples; ++i)
        for (uint8_t c=0; c<channels; ++c)
            dst[c][i+offset] = static_cast<float>(srcptr[i*channels+c]) * (1.f / 8388607.f);
}

static inline
void s24le3(float* const* const dst, void* const src, const uint16_t offset, const uint8_t channels, const uint16_t samples)
{
    uint8_t* srcptr = static_cast<uint8_t*>(src);
    int32_t z;

    for (uint16_t i=0; i<samples; ++i)
    {
        for (uint8_t c=0; c<channels; ++c)
        {
           #if __BYTE_ORDER == __BIG_ENDIAN
            z = (static_cast<int32_t>(srcptr[0]) << 16)
              + (static_cast<int32_t>(srcptr[1]) << 8)
              +  static_cast<int32_t>(srcptr[2]);

            if (srcptr[0] & 0x80)
                z |= 0xff000000;
           #else
            z = (static_cast<int32_t>(srcptr[2]) << 16)
              + (static_cast<int32_t>(srcptr[1]) << 8)
              +  static_cast<int32_t>(srcptr[0]);

            if (srcptr[2] & 0x80)
                z |= 0xff000000;
           #endif

            dst[c][i+offset] = z <= -8388607 ? -1.f
                             : z >= 8388607 ? 1.f
                             : static_cast<float>(z) * (1.f / 8388607.f);

            srcptr += 3;
        }
    }
}

static inline
void s32(float* const* const dst, void* const src, const uint16_t offset, const uint8_t channels, const uint16_t samples)
{
    int32_t* const srcptr = static_cast<int32_t*>(src);

    for (uint16_t i=0; i<samples; ++i)
        for (uint8_t c=0; c<channels; ++c)
            dst[c][i+offset] = static_cast<double>(srcptr[i*channels+c]) * (1.0 / 2147483647.0);
}

}

// --------------------------------------------------------------------------------------------------------------------

static inline
void simd_yield()
{
   #if defined(__SSE2_MATH__)
    _mm_pause();
   #elif defined(__aarch64__) || (defined(__arm__) && !defined(__SOFTFP__))
    __asm__ __volatile__("isb");
   #else
    sched_yield();
   #endif
}

// --------------------------------------------------------------------------------------------------------------------

static void* deviceCaptureThread(void* arg);

DeviceAudio* initDeviceAudio(const char* const deviceID,
                             const bool playback,
                             const uint8_t channels,
                             const uint16_t bufferSize,
                             const uint32_t sampleRate)
{
    int err;
    DeviceAudio dev = {};
    dev.sampleRate = sampleRate;
    dev.bufferSize = bufferSize;
    dev.channels = channels;
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

    unsigned periodsParam = 0;
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

    if ((err = snd_pcm_hw_params_set_channels(dev.pcm, params, channels)) != 0)
    {
        DEBUGPRINT("snd_pcm_hw_params_set_channels fail %s", snd_strerror(err));
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

    for (unsigned periods : kPeriodsToTry)
    {
        if ((err = snd_pcm_hw_params_set_period_size(dev.pcm, params, bufferSize, 0)) != 0)
        {
            DEBUGPRINT("snd_pcm_hw_params_set_period_size fail %u %u %s", periods, bufferSize, snd_strerror(err));
            continue;
        }

        // if ((err = snd_pcm_hw_params_set_period_size(dev.pcm, params, bufferSize, 1)) != 0)
        // {
        //     DEBUGPRINT("snd_pcm_hw_params_set_period_size fail %u %u %s", periods, bufferSize, snd_strerror(err));
        //     continue;
        // }

        if ((err = snd_pcm_hw_params_set_periods(dev.pcm, params, periods, 0)) != 0)
        {
            DEBUGPRINT("snd_pcm_hw_params_set_periods fail %u %u %s", periods, bufferSize, snd_strerror(err));
            continue;
        }

        // if ((err = snd_pcm_hw_params_set_periods(dev.pcm, params, periods, 1)) != 0)
        // {
        //     DEBUGPRINT("snd_pcm_hw_params_set_periods fail %u %u %s", periods, bufferSize, snd_strerror(err));
        //     continue;
        // }

        // if ((err = snd_pcm_hw_params_set_buffer_size(dev.pcm, params, bufferSize * periods)) != 0)
        // {
        //     DEBUGPRINT("snd_pcm_hw_params_set_buffer_size fail %u %u %s", periods, bufferSize, snd_strerror(err));
        //     continue;
        // }

        periodsParam = periods;
        // DEBUGPRINT("buffer size match %u, using %u periods", bufferSize, periodsParam);
        break;
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
        err = snd_pcm_sw_params_set_avail_min(dev.pcm, swparams, bufferSize * (periodsParam - 1));
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

//     if ((err = snd_pcm_pause(dev.pcm)) != 0)
//     {
//         DEBUGPRINT("snd_pcm_prepare fail %s", snd_strerror(err));
//         goto error;
//     }

    // if ((err = snd_spcm_init(dev.pcm, sampleRate, channels,
    //     SND_PCM_FORMAT_S32, SND_PCM_SUBFORMAT_STD, SND_SPCM_LATENCY_REALTIME, SND_PCM_ACCESS_MMAP_INTERLEAVED,
    //     SND_SPCM_XRUN_IGNORE)) != 0)
    // {
    //     DEBUGPRINT("snd_spcm_init fail %s", snd_strerror(err));
    //     goto error;
    // }

    snd_pcm_hw_params_get_period_size(params, &bufferSizeParam, nullptr);
    DEBUGPRINT("period size %lu", bufferSizeParam);

    snd_pcm_hw_params_get_buffer_size(params, &bufferSizeParam);
    DEBUGPRINT("buffer size %lu", bufferSizeParam);

    snd_pcm_hw_params_get_periods(params, &periodsParam, nullptr);
    DEBUGPRINT("num periods %u", periodsParam);

    {
        const size_t rawbufferlen = getSampleSizeFromHints(dev.hints) * dev.bufferSize * channels * 2;
        dev.buffers.raw = new int8_t[rawbufferlen];
        dev.buffers.f32 = new float*[channels];
        dev.ringbuffers = new HeapRingBuffer[channels];

        for (uint8_t c=0; c<channels; ++c)
        {
            dev.buffers.f32[c] = new float[dev.bufferSize * 2];
            dev.ringbuffers[c].createBuffer(sizeof(float) * dev.bufferSize * 5);
        }

        sem_init(&dev.sem, 0, 0);

        dev.resampler = new VResampler;

       #if 0 // defined(__MOD_DEVICES__) && defined(_MOD_DEVICE_DWARF) && defined(AUDIO_BRIDGE_INTERNAL_JACK_CLIENT)
        if (!playback)
        {
            dev.resampler->setup(0.99997392, channels, 8);
        }
        else
       #endif
        {
            dev.resampler->setup(1.0, channels, 8);
        }

        DeviceAudio* const devptr = new DeviceAudio;
        std::memcpy(devptr, &dev, sizeof(dev));

       #ifdef WITH_GAIN
        devptr->gain.setSampleRate(sampleRate);
        devptr->gain.setTargetValue(0.f);
        devptr->gain.setTimeConstant(3.f);
        devptr->gain.clearToTargetValue();
        devptr->gain.setTargetValue(1.f);
       #endif

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
        sched_param sched = {};
        sched.sched_priority = playback ? 69 : 70;
        pthread_attr_setschedparam(&attr, &sched);
        pthread_create(&devptr->thread, &attr, deviceCaptureThread, devptr);
        pthread_attr_destroy(&attr);

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
    dev->balance.slowingDown = dev->balance.slowingDownRealFast = dev->balance.speedingUp = dev->balance.speedingUpRealFast = 0;
    dev->balance.ratio = 1.0;
    dev->timestamps.alsaStartTime = dev->timestamps.jackStartFrame = 0;
    dev->timestamps.ratio = 1.0;
    dev->resampler->set_rratio(1.0);
}

static void balanceDeviceCaptureSpeed(DeviceAudio* const dev, const snd_pcm_sframes_t avail)
{
    const uint32_t frame = dev->frame;
    const uint16_t bufferSize = dev->bufferSize;
    const uint32_t sampleRate = dev->sampleRate;
    VResampler* const resampler = dev->resampler;

    DeviceAudio::Balance& bal(dev->balance);

    const uint16_t kSpeedTarget = sampleRate / bufferSize * 5;
    const uint16_t kMaxTargetRFL = bufferSize * 7;
    const uint16_t kMaxTargetRF = bufferSize * 6;
    const uint16_t kMaxTargetN = bufferSize * 4.25;
    const uint16_t kMinTargetRFL = bufferSize * 2;
    const uint16_t kMinTargetRF = bufferSize * 2.1666;
    const uint16_t kMinTargetN = bufferSize * 2.75;

#if 0
    if (avail >= kMaxTargetRF)
    {
        if (bal.speedingUp == 1)
        {
            // DEBUGPRINT("%08u | capture | %.9f | avail >= kMaxTargetRF %ld | %ld | ignored last frame", frame, bal.ratio, avail, avail - bufferSize);
        }
        else if (bal.speedingUpRealFast == 0)
        {
            if (avail >= kMaxTargetRFL)
            {
                bal.ratio = 0.99995;
            }
            else
            {
                switch (bal.mode)
                {
                case kBalanceNormal:
                case kBalanceSpeedingUpRealFast:
                    bal.ratio *= 0.99995;
                    break;
                case kBalanceSpeedingUp:
                    bal.ratio = std::max(0.99995, (bal.ratio * 3 + 0.99995) / 4);
                    break;
                case kBalanceSlowingDown:
                case kBalanceSlowingDownRealFast:
                    bal.ratio = 1.0;
                    break;
                }
            }
            bal.mode = kBalanceSpeedingUpRealFast;
            if (bal.speedingUp != 0)
                bal.speedingUpRealFast = bal.speedingUp;
            else
                bal.speedingUpRealFast = bal.speedingUp = 1;
            bal.slowingDown = bal.slowingDownRealFast = 0;
            resampler->set_rratio(dev->timestamps.ratio * bal.ratio);
//             if (bal.ratio != 1.0) {
                DEBUGPRINT("%08u | capture | %.9f = %.9f * %.9f | %ld | avail >= kMaxTargetRF | speeding up real fast...",
                           frame, dev->timestamps.ratio * dev->balance.ratio, dev->timestamps.ratio, bal.ratio, avail);
//             }
        }
        else if (++bal.speedingUp == 0 || ++bal.speedingUpRealFast == 0)
        {
            bal.speedingUp = bal.speedingUpRealFast = 1;
        }
    }
    else
#endif
        if (avail > kMaxTargetN)
    {
        /*if (bal.speedingUpRealFast != 0)
        {
            bal.mode = kBalanceSpeedingUp;
            bal.ratio = std::min(1.00005, (bal.ratio * 3 + 1.00005) / 4);
            bal.speedingUp = bal.speedingUpRealFast;
            bal.speedingUpRealFast = bal.slowingDown = bal.slowingDownRealFast = 0;
            resampler->set_rratio(dev->timestamps.ratio * bal.ratio);
            DEBUGPRINT("%08u | capture | %.9f = %.9f * %.9f | %ld | avail > kMaxTargetN | keep speed up but not as much...",
                       frame, dev->timestamps.ratio * dev->balance.ratio, dev->timestamps.ratio, bal.ratio, avail);
        }
        else*/ if (bal.speedingUp == 0)
        {
            switch (bal.mode)
            {
            case kBalanceNormal:
            case kBalanceSpeedingUp:
                bal.ratio *= 0.999995;
                break;
            case kBalanceSpeedingUpRealFast:
                bal.ratio = std::min(1.00005, (bal.ratio * 3 + 1.00005) / 4);
                break;
            case kBalanceSlowingDown:
                bal.ratio = 1.0;
                break;
            case kBalanceSlowingDownRealFast:
                bal.ratio = (bal.ratio * 3 + 0.99995) / 4;
                break;
            }
            bal.mode = kBalanceSpeedingUp;
            bal.speedingUp = bal.speedingUpRealFast != 0 ? bal.speedingUpRealFast : 1;
            bal.speedingUpRealFast = bal.slowingDown = bal.slowingDownRealFast = 0;
            resampler->set_rratio(dev->timestamps.ratio * bal.ratio);
            if (bal.ratio != 1.0) {
                DEBUGPRINT("%08u | capture | %.9f = %.9f * %.9f | %ld | avail > kMaxTargetN | speeding up...",
                           frame, dev->timestamps.ratio * dev->balance.ratio, dev->timestamps.ratio, bal.ratio, avail);
            }
        }
        else if (++bal.speedingUp >= kSpeedTarget)
        {
            bal.speedingUp = 1;
            if (d_isNotEqual(bal.ratio, 0.995))
            {
                bal.ratio = (bal.ratio * 3 + 0.995) / 4;
                DEBUGPRINT("%08u | capture | %.9f = %.9f * %.9f | %ld | avail > kMaxTargetN | speeding up even more...",
                           frame, dev->timestamps.ratio * dev->balance.ratio, dev->timestamps.ratio, bal.ratio, avail);
            }
        }

        // while (avail - bufferSize * 2 > 0) {
        //     const snd_pcm_uframes_t size = avail - bufferSize * 2 > bufferSize ? bufferSize : avail - bufferSize * 2;
        //     err = snd_pcm_mmap_readi(dev->pcm, dev->buffer, size);
        //     DEBUGPRINT("%08u | avail > bufferSize*2 %ld || err %ld", frame, avail, err);
        //     if (err < 0)
        //         break;
        //     avail -= err;
        // }
        //
        // avail = snd_pcm_avail(dev->pcm);
    }
#if 0
    else if (avail <= kMinTargetRF)
    {
        if (bal.slowingDownRealFast == 0)
        {
            if (avail <= kMinTargetRFL)
            {
                bal.ratio = 1.00005;
            }
            else
            {
                switch (bal.mode)
                {
                case kBalanceNormal:
                case kBalanceSlowingDownRealFast:
                    bal.ratio *= 1.00005;
                    break;
                case kBalanceSlowingDown:
                    bal.ratio = std::min(1.00005, (bal.ratio * 3 + 1.00005) / 4);
                    break;
                case kBalanceSpeedingUp:
                case kBalanceSpeedingUpRealFast:
                    bal.ratio = 1.0;
                    break;
                }
            }
            bal.mode = kBalanceSlowingDownRealFast;
            if (bal.slowingDown != 0)
                bal.slowingDownRealFast = bal.slowingDown;
            else
                bal.slowingDownRealFast = bal.slowingDown = 1;
            bal.speedingUp = bal.speedingUpRealFast = 0;
            resampler->set_rratio(dev->timestamps.ratio * bal.ratio);
//             if (bal.ratio != 1.0) {
                DEBUGPRINT("%08u | capture | %.9f = %.9f * %.9f | %ld | avail <= kMinTargetRF | slowing down real fast...",
                           frame, dev->timestamps.ratio * dev->balance.ratio, dev->timestamps.ratio, bal.ratio, avail);
//             }
        }
        else if (++bal.slowingDown == 0 || ++bal.slowingDownRealFast == 0)
        {
            bal.slowingDown = bal.slowingDownRealFast = 1;
        }
    }
#endif
    else if (avail < kMinTargetN)
    {
        /*if (bal.slowingDownRealFast != 0)
        {
            bal.mode = kBalanceSlowingDown;
            bal.ratio = std::max(0.99995, (bal.ratio * 3 + 0.99995) / 4);
            bal.slowingDown = bal.slowingDownRealFast;
            bal.slowingDownRealFast = bal.speedingUp = bal.speedingUpRealFast = 0;
            resampler->set_rratio(dev->timestamps.ratio * bal.ratio);
            DEBUGPRINT("%08u | capture | %.9f = %.9f * %.9f | %ld | avail < kMinTargetN | keep speed down but not as much...",
                       frame, dev->timestamps.ratio * dev->balance.ratio, dev->timestamps.ratio, bal.ratio, avail);
        }
        else */if (bal.slowingDown == 0)
        {
            switch (bal.mode)
            {
            case kBalanceNormal:
            case kBalanceSlowingDown:
                bal.ratio *= 1.000005;
                break;
            case kBalanceSlowingDownRealFast:
                bal.ratio = std::max(0.99995, (bal.ratio * 3 + 0.99995) / 4);
                break;
            case kBalanceSpeedingUp:
                bal.ratio = 1.0;
                break;
            case kBalanceSpeedingUpRealFast:
                bal.ratio = (bal.ratio * 3 + 1.00005) / 4;
                break;
            }
            bal.mode = kBalanceSlowingDown;
            bal.slowingDown = bal.slowingDownRealFast != 0 ? bal.slowingDownRealFast : 1;
            bal.slowingDownRealFast = bal.speedingUp = bal.speedingUpRealFast = 0;
            resampler->set_rratio(dev->timestamps.ratio * bal.ratio);
            if (bal.ratio != 1.0) {
                DEBUGPRINT("%08u | capture | %.9f = %.9f * %.9f | %ld | avail < kMinTargetN | slowing down...",
                           frame, dev->timestamps.ratio * dev->balance.ratio, dev->timestamps.ratio, bal.ratio, avail);
            }
        }
        else if (++bal.slowingDown >= kSpeedTarget)
        {
            bal.slowingDown = 1;
            if (d_isNotEqual(bal.ratio, 1.0005))
            {
                bal.ratio = (bal.ratio * 3 + 1.0005) / 4;
                DEBUGPRINT("%08u | capture | %.9f = %.9f * %.9f | %ld | avail < kMinTargetN | slowing down even more...",
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
            bal.slowingDown = bal.slowingDownRealFast = bal.speedingUp = bal.speedingUpRealFast = 0;
        }
    }
}

static void balanceDevicePlaybackSpeed(DeviceAudio* const dev, const snd_pcm_sframes_t avail)
{
    const uint32_t frame = dev->frame;
    const uint16_t bufferSize = dev->bufferSize;
    const uint32_t sampleRate = dev->sampleRate;
    VResampler* const resampler = dev->resampler;

    DeviceAudio::Balance& bal(dev->balance);

    const uint16_t kSpeedTarget = static_cast<double>(sampleRate) / bufferSize * 8;
    const uint16_t kMaxTargetRFL = bufferSize * 2.333;
    const uint16_t kMaxTargetRF = bufferSize * 2.25;
    const uint16_t kMaxTargetN = bufferSize * 2;
    const uint16_t kMinTargetRFL = bufferSize * 1;
    const uint16_t kMinTargetRF = bufferSize * 1.05;
    const uint16_t kMinTargetN = bufferSize * 1.25;

    if (avail >= kMaxTargetRF)
    {
        if (bal.slowingDownRealFast == 0)
        {
            if (avail >= kMaxTargetRFL)
            {
                bal.ratio = 1.0005;
            }
            else
            {
                switch (bal.mode)
                {
                case kBalanceNormal:
                case kBalanceSlowingDownRealFast:
                    bal.ratio *= 1.0005;
                    break;
                case kBalanceSlowingDown:
                    bal.ratio = (bal.ratio * 3 + 1.0005) / 4;
                    break;
                case kBalanceSpeedingUp:
                case kBalanceSpeedingUpRealFast:
                    bal.ratio = 1.0;
                    break;
                }
            }
            bal.mode = kBalanceSlowingDownRealFast;
            bal.slowingDownRealFast = 1;
            bal.slowingDown = bal.speedingUp = bal.speedingUpRealFast = 0;
            resampler->set_rratio(dev->timestamps.ratio * bal.ratio);
            if (bal.ratio != 1.0) {
                DEBUGPRINT("%08u | playback | avail >= kMaxTargetRF %ld | %ld | %.9f | slowing down real fast...", frame, avail, avail - bufferSize, bal.ratio);
            }
        }
        else if (++bal.slowingDown == 0 || ++bal.slowingDownRealFast == 0)
        {
            bal.slowingDown = bal.slowingDownRealFast = 1;
        }
    }
    else if (avail > kMaxTargetN)
    {
        if (bal.slowingDown == 0)
        {
            switch (bal.mode)
            {
            case kBalanceNormal:
            case kBalanceSlowingDown:
                bal.ratio *= 1.000005;
                break;
            case kBalanceSlowingDownRealFast:
                bal.ratio = (bal.ratio * 3 + 1.000005) / 4;
                break;
            case kBalanceSpeedingUp:
                bal.ratio = 1.0;
                break;
            case kBalanceSpeedingUpRealFast:
                bal.ratio = (bal.ratio * 3 + 1.0005) / 4;
                break;
            }
            bal.mode = kBalanceSlowingDown;
            bal.slowingDown = 1;
            bal.slowingDownRealFast = bal.speedingUp = bal.speedingUpRealFast = 0;
            resampler->set_rratio(dev->timestamps.ratio * bal.ratio);
            if (bal.ratio != 1.0) {
                DEBUGPRINT("%08u | playback | avail > kMaxTargetN %ld | %ld | %.9f | slowing down...", frame, avail, avail - bufferSize, bal.ratio);
            }
        }
        else if (++bal.slowingDown == 0)
        {
            bal.slowingDown = 1;
            bal.slowingDownRealFast = 0;
        }
    }
    else if (avail <= kMinTargetRF)
    {
        if (bal.speedingUpRealFast == 0)
        {
            if (avail <= kMinTargetRFL)
            {
                bal.ratio = 0.9995;
            }
            else
            {
                switch (bal.mode)
                {
                case kBalanceNormal:
                case kBalanceSpeedingUpRealFast:
                    bal.ratio *= 0.9995;
                    break;
                case kBalanceSpeedingUp:
                    bal.ratio = (bal.ratio * 3 + 0.9995) / 4;
                    break;
                case kBalanceSlowingDown:
                case kBalanceSlowingDownRealFast:
                    bal.ratio = 1.0;
                    break;
                }
            }
            bal.mode = kBalanceSpeedingUpRealFast;
            bal.speedingUpRealFast = 1;
            bal.speedingUp = bal.slowingDown = bal.slowingDownRealFast = 0;
            resampler->set_rratio(dev->timestamps.ratio * bal.ratio);
            if (bal.ratio != 1.0) {
                DEBUGPRINT("%08u | playback | avail <= kMinTargetRF %ld | %ld | %.9f | speeding up real fast...", frame, avail, avail - bufferSize, bal.ratio);
            }
        }
        else if (++bal.speedingUp == 0)
        {
            bal.speedingUp = bal.speedingUpRealFast = 1;
        }
    }
    else if (avail < kMinTargetN)
    {
        if (bal.speedingUp == 0)
        {
            switch (bal.mode)
            {
            case kBalanceNormal:
            case kBalanceSpeedingUp:
                bal.ratio *= 0.999995;
                break;
            case kBalanceSpeedingUpRealFast:
                bal.ratio = (bal.ratio * 3 + 0.999995) / 4;
                break;
            case kBalanceSlowingDown:
                bal.ratio = 1.0;
                break;
            case kBalanceSlowingDownRealFast:
                bal.ratio = (bal.ratio * 3 + 0.9995) / 4;
                break;
            }
            bal.mode = kBalanceSpeedingUp;
            bal.speedingUp = 1;
            bal.speedingUpRealFast = bal.slowingDown = bal.slowingDownRealFast = 0;
            resampler->set_rratio(dev->timestamps.ratio * bal.ratio);
            if (bal.ratio != 1.0) {
                DEBUGPRINT("%08u | playback | avail < kMinTargetN %ld | %ld | %.9f | speeding up...", frame, avail, avail - bufferSize, bal.ratio);
            }
        }
        else if (++bal.speedingUp == 0)
        {
            bal.speedingUp = 1;
            bal.speedingUpRealFast = 0;
        }
    }
    else
    {
        if ((bal.slowingDown != 0 && ++bal.slowingDown == kSpeedTarget) || (bal.speedingUp != 0 && ++bal.speedingUp == kSpeedTarget))
        {
            DEBUGPRINT("%08u | playback | stopped speed compensation | %.9f", frame, bal.ratio);
            bal.mode = kBalanceNormal;
            bal.slowingDown = bal.slowingDownRealFast = bal.speedingUp = bal.speedingUpRealFast = 0;
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
//             sem_timedwait(&dev->sem, &ts);
            sem_wait(&dev->sem);
            again = true;
            continue;
        }

        frame = dev->frame;

        if (frames < 0)
        {
            deviceFailInitHints(dev);

            for (uint8_t c=0; c<channels; ++c)
//                 jack_ringbuffer_reset(dev->ringbuffers[c]);
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
//             const char* data = static_cast<const char*>(static_cast<const void*>(buffers[c]));

            while (!dev->ringbuffers[c].writeCustomData(buffers[c], sizeof(float) * written))
//             for (uint32_t wrtn = 0; wrtn < written;)
            {
//                 wrtn = jack_ringbuffer_write(dev->ringbuffers[c], data, (sizeof(float) * written) - wrtn);

//                 if (wrtn == sizeof(float) * written)
//                     break;

//                 data += wrtn;

                // DEBUGPRINT("jack buffer full");
                sched_yield();
                full = true;
            }

            dev->ringbuffers[c].commitWrite();
        }

//         if ((frame % dev->sampleRate) == 0)
        if (again)
        {
            snd_pcm_sframes_t savail = 0;
            savail += dev->ringbuffers[0].getReadableDataSize() /  sizeof(float);
//             savail += jack_ringbuffer_read_space(dev->ringbuffers[0]) / sizeof(float);
            savail += frames;
            savail -= bufferSize;

            if ((dev->hints & kDeviceStarting) == 0)
                balanceDeviceCaptureSpeed(dev, savail);

//             dev->timestamps.ratio = 0.999997143;
//             resampler->set_rratio(dev->timestamps.ratio * dev->balance.ratio);

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
            dev->balance.speedingUpRealFast = dev->balance.slowingDown = dev->balance.slowingDownRealFast = 0;
            resampler->set_rratio(dev->timestamps.ratio * dev->balance.ratio);
            DEBUGPRINT("%08u | capture | %.9f = %.9f * %.9f | speeding up real fast...", frame, dev->timestamps.ratio * dev->balance.ratio, dev->timestamps.ratio, dev->balance.ratio);

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
    const uint32_t sampleRate = dev->sampleRate;
    const uint16_t bufferSize = dev->bufferSize;
    const uint8_t channels = dev->channels;

    const HeapRingBuffer& rb(dev->ringbuffers[0]);
    uint32_t avail = rb.getReadableDataSize() / sizeof(float);

#if 1
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
            int counter = 0;
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
                dev->balance.slowingDown = dev->balance.slowingDownRealFast = 0;
                dev->balance.speedingUpRealFast = dev->balance.speedingUp = 0;

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
#else
    const uint8_t sampleSize = getSampleSizeFromHints(hints);
    VResampler* const resampler = dev->resampler;

    uint16_t retries = 0;
    size_t rbwrite;

    snd_pcm_sframes_t err;

   #if 0
    jack_ringbuffer_float_data_t vec[channels][2];
    float* vecbuffers[channels];
   #endif

    if (frame == 0)
    {
        DEBUGPRINT("%08u | capture | frame == 0 | %ld", frame, avail);
        snd_pcm_rewind(dev->pcm, bufferSize * 2);
        avail = snd_pcm_avail(dev->pcm);
    }

    if (!(hints & kDeviceStarting))
    {
        balanceDeviceCaptureSpeed(dev, avail);
    }

   #if 0
    if (avail == 0)
    {
        DEBUGPRINT("%08u | avail == 0", frame);
        snd_pcm_rewind(dev->pcm, bufferSize);

        avail = snd_pcm_avail_update(dev->pcm);
        dev->hints |= kDeviceStarting;

        DEBUGPRINT("%08u | avail < bufferSize --> %ld", frame, avail);

        if ((hints & kDeviceStarting) == 0)
        {
            if (jack_ringbuffer_read_space(dev->ringbuffer[0]) < bufferSize * sizeof(float))
            {
                DEBUGPRINT("%08u | capture going too fast, failed to compensate! removing starting flag", frame);

                dev->hints &= ~kDeviceStarting;
            }
            else
            {
                DEBUGPRINT("%08u | capture going too fast but we are still ok!", frame);

                for (uint8_t c=0; c<channels; ++c)
                    jack_ringbuffer_write_advance(dev->ringbuffer[c], sizeof(float)*bufferSize);
            }
        }
    }
   #endif

    int lasterr = 0;
    const uint16_t extraBufferSize = bufferSize / 4;

    for (uint16_t frames = bufferSize; frames != 0;)
    {
        err = snd_pcm_mmap_readi(dev->pcm, dev->buffers.raw, frames + extraBufferSize);
        // DEBUGPRINT("%08u | read %ld of %u", frame, err, frames + extraBufferSize);

        if (err == -EAGAIN)
        {
            if (hints & kDeviceStarting)
            {
                for (uint8_t c=0; c<channels; ++c)
                    std::memset(buffers[c], 0, sizeof(float)*bufferSize);
                return;
            }

            if (++retries < 1000)
                continue;

            {
                DEBUGPRINT("%08u | capture | read err == -EAGAIN %u retries %ld avail", frame, retries, avail);
                dev->hints |= kDeviceStarting;
            }

           #if 0
            for (uint8_t c=0; c<channels; ++c)
                jack_ringbuffer_reset(dev->ringbuffer[c]);
           #endif

            return;
        }

        if (err < 0)
        {
            deviceFailInitHints(dev);

            for (uint8_t c=0; c<channels; ++c)
                std::memset(buffers[c], 0, sizeof(float)*bufferSize);

            DEBUGPRINT("%08u | capture | Error read %s\n", frame, snd_strerror(err));

           #if 0
            for (uint8_t c=0; c<channels; ++c)
                jack_ringbuffer_reset(dev->ringbuffer[c]);
           #endif

            // TODO offline recovery
            if (xrun_recovery(dev->pcm, err) < 0)
            {
                printf("%08u | capture | Read error: %s\n", frame, snd_strerror(err));
                exit(EXIT_FAILURE);
            }

            return;  /* skip one period */
        }

        if (static_cast<uint16_t>(err) < frames + extraBufferSize)
        {
            // DEBUGPRINT("%08u | capture | Incomplete read %ld, %ld avail", frame, err, avail);

            if ((dev->hints & kDeviceInitializing) == 0x0 && lasterr == err && ++retries == 1000)
            {
                DEBUGPRINT("%08u | capture | Incomplete read %ld, %ld avail, adding kDeviceInitializing", frame, err, avail);
                deviceFailInitHints(dev);
                break;
            }

            if (dev->balance.mode != kBalanceSlowingDownRealFast || dev->balance.ratio > 1.5)
                dev->balance.ratio = 1.5;
            else
                dev->balance.ratio *= 1.5;
            dev->balance.mode = kBalanceSlowingDownRealFast;
            dev->balance.slowingDown = dev->balance.slowingDownRealFast = 1;
            dev->balance.speedingUpRealFast = dev->balance.speedingUp = 0;
            resampler->set_rratio(dev->timestamps.ratio * dev->balance.ratio);
            DEBUGPRINT("%08u | capture | %.9f | slowing down real fast...", frame, dev->balance.ratio);
        }

        // if (err != avail) {
        // }
        // frames = 0;
        if (dev->hints & kDeviceInitializing)
        {
            DEBUGPRINT("%08u | capture | Complete read >= %u, err %ld, %ld avail, removing kDeviceInitializing", frame, bufferSize + extraBufferSize, err, avail);
            dev->hints &= ~kDeviceInitializing;
        }
        else if (dev->hints & kDeviceStarting)
        {
            DEBUGPRINT("%08u | capture | Complete read >= %u, err %ld, %ld avail, removing kDeviceStarting", frame, bufferSize + extraBufferSize, err, avail);
            dev->hints &= ~kDeviceStarting;
        }

        const uint16_t offset = 0;
        switch (hints & kDeviceSampleHints)
        {
        case kDeviceSample16:
            int2float::s16(dev->buffers.f32, dev->buffers.raw, offset, channels, err);
            break;
        case kDeviceSample24:
            int2float::s24(dev->buffers.f32, dev->buffers.raw, offset, channels, err);
            break;
        case kDeviceSample24LE3:
            int2float::s24le3(dev->buffers.f32, dev->buffers.raw, offset, channels, err);
            break;
        case kDeviceSample32:
            int2float::s32(dev->buffers.f32, dev->buffers.raw, offset, channels, err);
            break;
        }

        resampler->inp_count = err;
        resampler->out_count = frames;
        resampler->inp_data = dev->buffers.f32;
        resampler->out_data = buffers;
        resampler->process();

        if (resampler->inp_count == 0)
        {
            // printf("%08u | E1 resampler->out_count == %u | resampler->inp_count == %u | err %ld | avail %ld\n",
                    // frame, resampler->out_count, resampler->inp_count, err, avail);
            // err = bufferSize;
        }
        else if (resampler->inp_count != 0)
        {
            // printf("%08u | E2 resampler->out_count == %u | resampler->inp_count == %u | err %ld | avail %ld\n",
                    // frame, resampler->out_count, resampler->inp_count, err, avail);
            // err = bufferSize;
        }
        else if (resampler->out_count != 0)
        {
            printf("%08u | capture | E3 resampler->out_count == %u | resampler->inp_count == %u | err %ld | avail %ld\n",
                    frame, resampler->out_count, resampler->inp_count, err, avail);
            exit(EXIT_FAILURE);
        }

        retries = 0;
        lasterr = err;

        if (const unsigned inp_count = resampler->inp_count)
            snd_pcm_rewind(dev->pcm, inp_count);

        frames = resampler->out_count;
    }
#endif
}

static void runDeviceAudioPlayback(DeviceAudio* const dev, float* buffers[], const uint32_t frame, snd_pcm_uframes_t avail)
{
    const uint16_t bufferSize = dev->bufferSize;
    const uint8_t channels = dev->channels;
    const uint8_t hints = dev->hints;
    const uint8_t sampleSize = getSampleSizeFromHints(hints);
    VResampler* const resampler = dev->resampler;

    uint16_t retries = 0;

    if (frame == 0)
    {
        DEBUGPRINT("%08u | playback | frame == 0 | %ld", frame, avail);
        snd_pcm_forward(dev->pcm, bufferSize);
        avail = snd_pcm_avail(dev->pcm);
    }

    if (!(hints & kDeviceStarting))
    {
        balanceDevicePlaybackSpeed(dev, avail);
    }

    resampler->inp_count = bufferSize;
    resampler->out_count = bufferSize * 2;
    resampler->inp_data = buffers;
    resampler->out_data = dev->buffers.f32;
    resampler->process();

    if (resampler->inp_count != 0)
    {
        printf("%08u | playback | E1 resampler->out_count == %u | resampler->inp_count == %u | avail %ld\n",
               frame, resampler->out_count, resampler->inp_count, avail);
        exit(EXIT_FAILURE);
    }
    else if (resampler->out_count == 0)
    {
        printf("%08u | playback | E2 resampler->out_count == %u | resampler->inp_count == %u | avail %ld\n",
               frame, resampler->out_count, resampler->inp_count, avail);
        exit(EXIT_FAILURE);
    }
    else if (resampler->out_count != 128)
    {
        // printf("%08u | E2 resampler->out_count == %u | resampler->inp_count == %u | avail %ld\n",
                // frame, resampler->out_count, resampler->inp_count, avail);
    }

    uint16_t frames = bufferSize * 2 - resampler->out_count;

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

    int8_t* ptr = dev->buffers.raw;
    int err;

    while (frames != 0)
    {
        err = snd_pcm_mmap_writei(dev->pcm, ptr, frames);
        // DEBUGPRINT("write %d of %u", err, frames);

        if (err == -EAGAIN)
        {
            if (hints & kDeviceStarting)
                return;

            if (++retries < 1000)
                continue;

            {
                DEBUGPRINT("%08u | playback | write err == -EAGAIN %u retries", frame, retries);
                dev->hints |= kDeviceStarting;
            }

            return;
        }

        if (err < 0)
        {
            printf("%08u | playback | Write error: %s\n", frame, snd_strerror(err));
            deviceFailInitHints(dev);

            if (xrun_recovery(dev->pcm, err) < 0)
            {
                printf("playback | xrun_recovery error: %s\n", snd_strerror(err));
                exit(EXIT_FAILURE);
            }
            break;  /* skip one period */
        }

        if (static_cast<uint16_t>(err) == frames)
        {
            if (dev->hints & kDeviceInitializing)
                dev->hints &= ~kDeviceInitializing;
            else
                dev->hints &= ~kDeviceStarting;

            if (retries || frame == 0) {
                DEBUGPRINT("%08u | playback | Complete write %u, %u retries", frame, frames, retries);
            }

            if (retries != 0)
            {
                if (dev->balance.mode != kBalanceSpeedingUpRealFast || dev->balance.ratio > 0.9995)
                    dev->balance.ratio = 0.9995;
                else
                    dev->balance.ratio *= 0.999995;
                dev->balance.mode = kBalanceSpeedingUpRealFast;
                dev->balance.speedingUpRealFast = dev->balance.speedingUp = 1;
                dev->balance.slowingDown = dev->balance.slowingDownRealFast = 0;
                resampler->set_rratio(dev->timestamps.ratio * dev->balance.ratio);
                DEBUGPRINT("%08u | playback | %.9f | speeding up real fast...", frame, dev->balance.ratio);
            }
            break;
        }

        ptr += err * channels * sampleSize;
        frames -= err;

        DEBUGPRINT("%08u | playback | Incomplete write %d of %u, %u left, %u retries", frame, err, bufferSize, frames, retries);
    }
}

void runDeviceAudio(DeviceAudio* const dev, float* buffers[])
{
    const uint32_t frame = dev->frame;
    dev->frame += dev->bufferSize;

    if (dev->hints & kDeviceCapture)
        runDeviceAudioCapture(dev, buffers, frame);
//     else
//         runDeviceAudioPlayback(dev, buffers, frame);
}

void closeDeviceAudio(DeviceAudio* const dev)
{
    const uint8_t channels = dev->channels;
    dev->channels = 0;
    sem_post(&dev->sem);
    pthread_join(dev->thread, nullptr);

    sem_destroy(&dev->sem);

    for (uint8_t c=0; c<channels; ++c)
        delete[] dev->buffers.f32[c];
    delete[] dev->buffers.f32;
    delete[] dev->buffers.raw;
    snd_pcm_close(dev->pcm);
    delete dev;
}

// --------------------------------------------------------------------------------------------------------------------
