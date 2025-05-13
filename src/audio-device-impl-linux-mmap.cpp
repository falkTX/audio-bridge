// SPDX-FileCopyrightText: 2021-2025 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio-device.hpp"
#include "audio-device-impl.hpp"
#include "audio-utils.hpp"

#include <memory>

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

// extra PPM average count
#define NUM_PPMS (48000 / 32)

// weight to give to extra PPM from userspace
#define PPM_FACTOR 8

// minimum/maximum possible value to use as extra PPM
#define PPM_LIMIT 100

// --------------------------------------------------------------------------------------------------------------------

// data shared with kernel
struct uac_mmap_data {
    uint8_t active_kernel;
    uint8_t active_userspace;
    uint8_t data_size; // same as format
    uint8_t num_channels;
    uint32_t sample_rate;
    uint32_t buffer_size;
    uint32_t bufpos_kernel;
    uint32_t bufpos_userspace;
    int32_t extra_ppm;
    uint8_t buffer[];
};

struct AudioDevice::Impl {
    // config copy
    bool playback;
    uint16_t sampleRate;

   #if AUDIO_BRIDGE_DEBUG
    // monotonic frame counter
    uint32_t frame = 0;
   #endif

    // direct kernel memory access
    int fd;
    uac_mmap_data* mdata;

    // buffer for read/write ringbuffer data into
    uint8_t* rawBuffer;

    // whether audio processing has been called at least once
    uint8_t started = 0;

    // whether audio device has been disconnected
    bool disconnected = false;

    // extra ppm to give to kernel
    struct {
        int64_t sum = 0;
        int32_t idx = 0;
        int32_t ppms[NUM_PPMS] = {};

        void reset(const int32_t d)
        {
            for (int i = 0; i < NUM_PPMS; ++i)
                ppms[i] = d;

            idx = 0;
            sum = static_cast<int64_t>(d) * NUM_PPMS;
        }
    } distance;
};

// --------------------------------------------------------------------------------------------------------------------

AudioDevice::Impl* initAudioDeviceImpl(const AudioDevice* const dev, AudioDevice::HWConfig& hwconfig)
{
    std::unique_ptr<AudioDevice::Impl> impl = std::unique_ptr<AudioDevice::Impl>(new AudioDevice::Impl);
    impl->playback = dev->config.playback;
    impl->sampleRate = dev->config.sampleRate;

    // ----------------------------------------------------------------------------------------------------------------

    int fd = open(dev->config.playback ? "/proc/uac2p" : "/proc/uac2c", O_RDWR | O_SYNC);
    if (fd < 0)
    {
       #if AUDIO_BRIDGE_DEBUG >= 2
        DEBUGPRINT("failed to open uac proc file");
       #endif
        return nullptr;
    }

    uac_mmap_data fdata;
    if (read(fd, &fdata, sizeof(fdata)) != sizeof(fdata))
    {
        DEBUGPRINT("failed to read uac proc file");
        close(fd);
        return nullptr;
    }

    if (fdata.active_kernel == 0)
    {
        close(fd);
        return nullptr;
    }

   #ifdef _DARKGLASS_DEVICE_PABLITO
    if (fdata.num_channels != (dev->config.playback ? 3 : 9))
    {
        DEBUGPRINT("wrong number of channels! %u", fdata.num_channels);
        close(fd);
        return nullptr;
    }
    if (fdata.data_size != 4)
    {
        DEBUGPRINT("wrong data size! %u", fdata.data_size);
        close(fd);
        return nullptr;
    }
   #endif

    if ((fdata.buffer_size % (fdata.num_channels * fdata.data_size)) != 0)
    {
        DEBUGPRINT("wrong buffer size! %u | %u", fdata.buffer_size, fdata.num_channels * fdata.data_size);
        close(fd);
        return nullptr;
    }

    const size_t mmap_size = sizeof(uac_mmap_data) + fdata.buffer_size;

    uac_mmap_data* mdata = static_cast<uac_mmap_data*>(mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (mdata == NULL || mdata == MAP_FAILED)
    {
        DEBUGPRINT("failed to mmap uac proc file");
        close(fd);
        return nullptr;
    }

    // ----------------------------------------------------------------------------------------------------------------

    hwconfig.format = getSampleFormatFromSize(fdata.data_size);
    hwconfig.numChannels = fdata.num_channels;
    hwconfig.numPeriods = 1;
    hwconfig.periodSize = hwconfig.fullBufferSize = fdata.buffer_size / fdata.num_channels / fdata.data_size;
    hwconfig.sampleRate = fdata.sample_rate;

    // ----------------------------------------------------------------------------------------------------------------

    impl->fd = fd;
    impl->mdata = mdata;
    impl->rawBuffer = new uint8_t [std::max<int>(mdata->buffer_size,
                                                 fdata.num_channels * fdata.data_size * dev->config.bufferSize)];

    mdata->active_userspace = 1;
    mdata->bufpos_userspace = 0;
    mdata->bufpos_kernel = 0;

    return impl.release();
}

void closeAudioDeviceImpl(AudioDevice::Impl* const impl)
{
    delete[] impl->rawBuffer;

    impl->mdata->active_userspace = 0;
    impl->mdata->extra_ppm = 0;

    const size_t mmap_size = sizeof(uac_mmap_data) + impl->mdata->buffer_size;
    munmap(impl->mdata, mmap_size);

    close(impl->fd);

    delete impl;
}

bool runAudioDevicePostImpl(AudioDevice::Impl* const impl, const uint16_t numFrames [[maybe_unused]])
{
   #if AUDIO_BRIDGE_DEBUG
    impl->frame += numFrames;
   #endif
    return !impl->disconnected;
}

// --------------------------------------------------------------------------------------------------------------------

static constexpr inline int32_t positive_modulo(int32_t i, int32_t n) {
    return (i % n + n) % n;
}

bool runAudioDeviceCaptureSyncImpl(AudioDevice::Impl* const impl, float* buffers[], const uint16_t numFrames)
{
    uac_mmap_data* const mdata = impl->mdata;

    if (mdata->active_kernel == 0)
    {
        DEBUGPRINT("%010u | capture | kernel is not ready, closing", impl->frame);
        impl->disconnected = true;
        return false;
    }
    if (mdata->sample_rate != impl->sampleRate)
    {
        DEBUGPRINT("%010u | capture | sample rate changed %u -> %u, closing", impl->frame, impl->sampleRate, mdata->sample_rate);
        impl->disconnected = true;
        return false;
    }

    static constexpr const int32_t kHalfRingBufferBlocks = AUDIO_BRIDGE_CAPTURE_RINGBUFFER_BLOCKS / 2;
    const uint32_t numChannels = mdata->num_channels;
    const uint32_t sampleSize = mdata->data_size;
    const uint32_t bufferSize = mdata->buffer_size;
    const int32_t numFramesBytes = numFrames * numChannels * sampleSize;
    int32_t bufpos_kernel, bufpos_userspace;
    int32_t distance, pending;

    if (impl->started == 0)
    {
        impl->started = 1;
        mdata->extra_ppm = 0;
        mdata->active_userspace = 2;

        bufpos_kernel = __atomic_load_n(&mdata->bufpos_kernel, __ATOMIC_ACQUIRE);
        bufpos_userspace = positive_modulo(bufpos_kernel - numFramesBytes * (kHalfRingBufferBlocks - 1), bufferSize);
        __atomic_store_n(&mdata->bufpos_userspace, bufpos_userspace, __ATOMIC_RELEASE);

        distance = positive_modulo(bufpos_kernel - bufpos_userspace, bufferSize) / (numChannels * sampleSize);
        impl->distance.reset(distance);
        DEBUGPRINT("%010u | capture | kernel is ready, starting distance %d", impl->frame, distance);
        return false;
    }

    bufpos_kernel = __atomic_load_n(&mdata->bufpos_kernel, __ATOMIC_ACQUIRE);
    bufpos_userspace = mdata->bufpos_userspace;
    distance = positive_modulo(bufpos_kernel - bufpos_userspace, bufferSize);

    if (distance < numFramesBytes)
    {
        DEBUGPRINT("%010u | capture | out of data | %d", impl->frame, distance / sampleSize / numChannels);
        distance = numFramesBytes * kHalfRingBufferBlocks;
        bufpos_userspace = positive_modulo(bufpos_kernel - distance, bufferSize);

        mdata->extra_ppm = 0;
        impl->distance.reset(distance / (numChannels * sampleSize));
    }
    else if (distance > numFramesBytes * AUDIO_BRIDGE_CAPTURE_RINGBUFFER_BLOCKS)
    {
        DEBUGPRINT("%010u | capture | too much data | %d", impl->frame, distance / sampleSize / numChannels);
        distance = numFramesBytes * kHalfRingBufferBlocks;
        bufpos_userspace = positive_modulo(bufpos_kernel - distance, bufferSize);

        mdata->extra_ppm = 0;
        impl->distance.reset(distance / (numChannels * sampleSize));
    }

    pending = bufferSize - bufpos_userspace;

    if (pending < numFramesBytes)
    {
        std::memcpy(impl->rawBuffer, mdata->buffer + bufpos_userspace, pending);
        std::memcpy(impl->rawBuffer + pending, mdata->buffer, numFramesBytes - pending);
    }
    else
    {
        std::memcpy(impl->rawBuffer, mdata->buffer + bufpos_userspace, numFramesBytes);
    }

    bufpos_userspace = (bufpos_userspace + numFramesBytes) % bufferSize;
    __atomic_store_n(&mdata->bufpos_userspace, bufpos_userspace, __ATOMIC_RELEASE);

    {
        distance /= numChannels * sampleSize;

        const int32_t idx = impl->distance.idx++ % NUM_PPMS;
        impl->distance.sum = impl->distance.sum - impl->distance.ppms[idx] + distance;
        impl->distance.ppms[idx] = distance;

        const int32_t ppm = std::max<double>(-PPM_LIMIT, std::min<double>(PPM_LIMIT,
            static_cast<double>(numFrames * kHalfRingBufferBlocks + numFrames / 2 - distance) / numFrames * PPM_FACTOR));
        mdata->extra_ppm = (mdata->extra_ppm * 3 + ppm) / 4;
    }

    switch (mdata->data_size)
    {
    case 2:
        int2float::s16(buffers, impl->rawBuffer, numChannels, numFrames);
        break;
    case 3:
        int2float::s24le3(buffers, impl->rawBuffer, numChannels, numFrames);
        break;
    case 4:
        int2float::s32(buffers, impl->rawBuffer, numChannels, numFrames);
        break;
    default:
        DEBUGPRINT("unknown data size");
        return false;
    }

   #if 0
    static int count = 0;
    if (++count == impl->sampleRate / numFrames / 2 || impl->started == 1)
    {
        impl->started = 2;
        count = 0;
        fprintf(stderr, "%010u | capture | kernel is running, distance %d / %d, extra_ppm %d $ \n",
                impl->frame, distance, numFrames * kHalfRingBufferBlocks, mdata->extra_ppm);
    }
   #endif

    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool runAudioDevicePlaybackSyncImpl(AudioDevice::Impl* impl, float* buffers[], uint16_t numFrames)
{
    uac_mmap_data* const mdata = impl->mdata;

    if (mdata->active_kernel == 0)
    {
        DEBUGPRINT("%010u | playback | kernel is not ready, closing", impl->frame);
        impl->disconnected = true;
        return false;
    }
    if (mdata->sample_rate != impl->sampleRate)
    {
        DEBUGPRINT("%010u | playback | sample rate changed, closing", impl->frame);
        impl->disconnected = true;
        return false;
    }

    static constexpr const int32_t kHalfRingBufferBlocks = AUDIO_BRIDGE_PLAYBACK_RINGBUFFER_BLOCKS / 2;
    const uint32_t numChannels = mdata->num_channels;
    const uint32_t sampleSize = mdata->data_size;
    const uint32_t bufferSize = mdata->buffer_size;
    const int32_t numFramesBytes = numFrames * numChannels * sampleSize;
    int32_t bufpos_kernel, bufpos_userspace;
    int32_t distance, pending;

    if (impl->started == 0)
    {
        impl->started = 1;
        mdata->extra_ppm = 0;
        mdata->active_userspace = 2;

        bufpos_kernel = __atomic_load_n(&mdata->bufpos_kernel, __ATOMIC_ACQUIRE);
        bufpos_userspace = (bufpos_kernel + numFramesBytes * (kHalfRingBufferBlocks + 1)) % bufferSize;
        __atomic_store_n(&mdata->bufpos_userspace, bufpos_userspace, __ATOMIC_RELEASE);

        distance = positive_modulo(bufpos_userspace - bufpos_kernel, bufferSize) / (numChannels * sampleSize);
        impl->distance.reset(distance);
        DEBUGPRINT("%010u | playback | kernel is ready, starting distance %d", impl->frame, distance);
        return false;
    }

    switch (mdata->data_size)
    {
    case 2:
        float2int::s16(impl->rawBuffer, buffers, numChannels, numFrames);
        break;
    case 3:
        float2int::s24le3(impl->rawBuffer, buffers, numChannels, numFrames);
        break;
    case 4:
        float2int::s32(impl->rawBuffer, buffers, numChannels, numFrames);
        break;
    default:
        DEBUGPRINT("unknown data size");
        return false;
    }

    bufpos_kernel = __atomic_load_n(&mdata->bufpos_kernel, __ATOMIC_ACQUIRE);
    bufpos_userspace = mdata->bufpos_userspace;
    distance = positive_modulo(bufpos_userspace - bufpos_kernel, bufferSize);

    if (distance < numFramesBytes)
    {
        DEBUGPRINT("%010u | playback | out of data | %d", impl->frame, distance / sampleSize / numChannels);
        distance = numFramesBytes * kHalfRingBufferBlocks;
        bufpos_userspace = (bufpos_kernel + distance) % bufferSize;

        mdata->extra_ppm = 0;
        impl->distance.reset(distance / (numChannels * sampleSize));
    }
    else if (distance > numFramesBytes * AUDIO_BRIDGE_PLAYBACK_RINGBUFFER_BLOCKS)
    {
        DEBUGPRINT("%010u | playback | too much data | %d", impl->frame, distance / sampleSize / numChannels);
        distance = numFramesBytes * kHalfRingBufferBlocks;
        bufpos_userspace = (bufpos_kernel + distance) % bufferSize;

        mdata->extra_ppm = 0;
        impl->distance.reset(distance / (numChannels * sampleSize));
    }

    pending = bufferSize - bufpos_userspace;

    if (pending < numFramesBytes)
    {
        std::memcpy(mdata->buffer + bufpos_userspace, impl->rawBuffer, pending);
        std::memcpy(mdata->buffer, impl->rawBuffer + pending, numFramesBytes - pending);
    }
    else
    {
        std::memcpy(mdata->buffer + bufpos_userspace, impl->rawBuffer, numFramesBytes);
    }

    bufpos_userspace = (bufpos_userspace + numFramesBytes) % bufferSize;
    __atomic_store_n(&mdata->bufpos_userspace, bufpos_userspace, __ATOMIC_RELEASE);

    {
        distance /= numChannels * sampleSize;

        const int32_t idx = impl->distance.idx++ % NUM_PPMS;
        impl->distance.sum = impl->distance.sum - impl->distance.ppms[idx] + distance;
        impl->distance.ppms[idx] = distance;

        const int32_t ppm = std::max<double>(-PPM_LIMIT, std::min<double>(PPM_LIMIT,
            static_cast<double>(distance - numFrames * kHalfRingBufferBlocks + numFrames / 2) / numFrames * PPM_FACTOR));
        mdata->extra_ppm = (mdata->extra_ppm * 3 + ppm) / 4;
    }

   #if 0
    static int count = 0;
    if (++count == impl->sampleRate / numFrames / 2 || impl->started == 1)
    {
        impl->started = 2;
        count = 0;
        fprintf(stderr, "%010u | playback | kernel is running, distance %d / %d, extra_ppm %d $ \n",
                impl->frame, distance, numFrames * kHalfRingBufferBlocks, mdata->extra_ppm);
    }
   #endif

    return true;
}

// --------------------------------------------------------------------------------------------------------------------
