// SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

//#define ALSA_PCM_NEW_HW_PARAMS_API
//#define ALSA_PCM_NEW_SW_PARAMS_API
#include <alsa/asoundlib.h>
#include <jack/ringbuffer.h>
#include <pthread.h>
#include <semaphore.h>

#include "RingBuffer.hpp"

// #ifdef WITH_GAIN
#include "ValueSmoother.hpp"
// #endif

#include "zita-resampler/vresampler.h"

#if defined(__SSE2_MATH__)
# include <xmmintrin.h>
#endif

enum DeviceHints {
    kDeviceCapture = 0x1,
    kDeviceInitializing = 0x2,
    kDeviceStarting = 0x4,
    kDeviceSample16 = 0x10,
    kDeviceSample24 = 0x20,
    kDeviceSample24LE3 = 0x40,
    kDeviceSample32 = 0x80,
    kDeviceSampleHints = kDeviceSample16|kDeviceSample24|kDeviceSample24LE3|kDeviceSample32
};

enum BalanceMode {
    kBalanceNormal = 0,
    kBalanceSlowingDown,
    kBalanceSpeedingUp,
};

static inline
const char* BalanceModeToStr(const uint8_t mode)
{
    switch (mode)
    {
    case kBalanceNormal:
        return "kBalanceNormal";
    case kBalanceSlowingDown:
        return "kBalanceSlowingDownRealFast";
    case kBalanceSpeedingUp:
        return "kBalanceSpeedingUp";
    }

    return "";
}

struct DeviceAudio {
    struct Balance {
        uint8_t mode = kBalanceNormal;
        uint8_t unused[3];
        uint16_t slowingDown = 0;
        uint16_t speedingUp = 0;
        double ratio = 1.0;
    } balance;

    struct TimeStamps {
        uint64_t alsaStartTime = 0;
        uint32_t jackStartFrame = 0;
        double ratio = 1.0;
    } timestamps;

    snd_pcm_t* pcm;
    uint32_t frame;
    uint32_t sampleRate;
    uint16_t bufferSize;
    uint8_t channels;
    uint8_t hints;

    struct {
        int8_t* raw;
        float** f32;
    } buffers;

    VResampler* resampler;

   #ifdef WITH_GAIN
    ExponentialValueSmoother gain;
   #endif

    pthread_t thread;
    sem_t sem;
//     jack_ringbuffer_t** ringbuffers;
    HeapRingBuffer* ringbuffers;
};

DeviceAudio* initDeviceAudio(const char* deviceID, bool playback, uint8_t channels, uint16_t bufferSize, uint32_t sampleRate);
void runDeviceAudio(DeviceAudio* dev, float* buffers[]);
void closeDeviceAudio(DeviceAudio* dev);

// private
static void deviceFailInitHints(DeviceAudio* dev);
static void runDeviceAudioCapture(DeviceAudio* dev, float* buffers[], uint32_t frame);
static void runDeviceAudioPlayback(DeviceAudio* dev, float* buffers[], uint32_t frame);
static void* deviceCaptureThread(void* arg);
static void* devicePlaybackThread(void* arg);

// TODO cleanup, see what is needed
static int xrun_recovery(snd_pcm_t *handle, int err);

#define DEBUGPRINT(...) { printf(__VA_ARGS__); puts(""); }

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

static inline constexpr
uint8_t getSampleSizeFromHints(const uint8_t hints)
{
    return hints & kDeviceSample16 ? sizeof(int16_t) :
           hints & kDeviceSample24 ? sizeof(int32_t) :
           hints & kDeviceSample24LE3 ? 3 :
           hints & kDeviceSample32 ? sizeof(int32_t) :
           0;
}

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
