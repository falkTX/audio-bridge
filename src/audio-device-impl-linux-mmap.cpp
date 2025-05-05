// SPDX-FileCopyrightText: 2021-2025 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-device.hpp"
#include "audio-device-impl.hpp"
#include "audio-utils.hpp"

#include <memory>

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#define NUM_PPMS 32

// --------------------------------------------------------------------------------------------------------------------

// data shared with kernel
struct uac_mmap_data {
    uint8_t active_kernel;
    uint8_t active_userspace;
    uint8_t data_size; // same as format
    uint8_t num_channels;
    uint16_t sample_rate;
    uint16_t buffer_size;
    uint16_t bufpos_kernel;
    uint16_t bufpos_userspace;
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
    bool started = false;

    // whether audio device has been disconnected
    bool disconnected = false;

    // extra ppm to give to kernel
    struct {
        int32_t ppms[NUM_PPMS] = {};
        int64_t sum = 0;
        int32_t idx = 0;
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
        DEBUGPRINT("failed to open uac proc file");
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
        DEBUGPRINT("%08u | capture | kernel is not ready, closing", impl->frame);
        impl->disconnected = true;
        return false;
    }
    if (mdata->sample_rate != impl->sampleRate)
    {
        DEBUGPRINT("%08u | capture | sample rate changed, closing", impl->frame);
        impl->disconnected = true;
        return false;
    }

    const uint32_t numChannels = mdata->num_channels;
    const uint32_t sampleSize = mdata->data_size;
    const uint32_t bufferSize = mdata->buffer_size;
    const int32_t numFramesBytes = numFrames * numChannels * sampleSize;
    uint32_t bufpos_kernel, bufpos_userspace;
    int32_t distance, pending;

    bufpos_kernel = __atomic_load_n(&mdata->bufpos_kernel, __ATOMIC_ACQUIRE);

    if (! impl->started)
    {
        DEBUGPRINT("%08u | capture | kernel is ready, starting", impl->frame);
        impl->started = true;
        mdata->active_userspace = 2;

        bufpos_userspace = positive_modulo(bufpos_kernel - numFramesBytes * AUDIO_BRIDGE_CAPTURE_RINGBUFFER_BLOCKS, bufferSize);
        __atomic_store_n(&mdata->bufpos_userspace, bufpos_userspace, __ATOMIC_RELEASE);

        for (int i = 0; i < NUM_PPMS; ++i)
            impl->distance.ppms[i] = numFramesBytes * AUDIO_BRIDGE_CAPTURE_RINGBUFFER_BLOCKS;

        return false;
    }

    bufpos_userspace = mdata->bufpos_userspace;
    distance = positive_modulo(bufpos_kernel - bufpos_userspace, bufferSize);

    // fprintf(stderr, "\r%08u | capture | kernel is running, distance %d, sum %ld, extra_ppm %d $ ",
    //         impl->frame, distance / sampleSize / numChannels, impl->distance.sum, mdata->extra_ppm);

    if (distance < numFramesBytes)
    {
        DEBUGPRINT("%08u | capture | out of data | %d", impl->frame, distance / sampleSize / numChannels);
        bufpos_userspace = positive_modulo(bufpos_kernel - numFramesBytes * AUDIO_BRIDGE_CAPTURE_RINGBUFFER_BLOCKS, bufferSize);
    }

    while (distance > numFramesBytes * AUDIO_BRIDGE_CAPTURE_RINGBUFFER_BLOCKS * 2)
    {
        DEBUGPRINT("%08u | capture | too much data | %d", impl->frame, distance / sampleSize / numChannels);
        // skip ahead one audio cycle
        bufpos_userspace = (bufpos_userspace + numFramesBytes) % bufferSize;
        distance -= numFramesBytes;
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
        const int32_t idx = impl->distance.idx++ % NUM_PPMS;
        impl->distance.sum = impl->distance.sum - impl->distance.ppms[idx] + distance;
        impl->distance.ppms[idx] = distance;

        mdata->extra_ppm = static_cast<double>(numFramesBytes * AUDIO_BRIDGE_CAPTURE_RINGBUFFER_BLOCKS - distance) * 8 / numFramesBytes;
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

    return true;
}

// --------------------------------------------------------------------------------------------------------------------

bool runAudioDevicePlaybackSyncImpl(AudioDevice::Impl* impl, float* buffers[], uint16_t numFrames)
{
    uac_mmap_data* const mdata = impl->mdata;

    if (mdata->active_kernel == 0)
    {
        DEBUGPRINT("%08u | playback | kernel is not ready, closing", impl->frame);
        impl->disconnected = true;
        return false;
    }
    if (mdata->sample_rate != impl->sampleRate)
    {
        DEBUGPRINT("%08u | playback | sample rate changed, closing", impl->frame);
        impl->disconnected = true;
        return false;
    }

    const uint32_t numChannels = mdata->num_channels;
    const uint32_t sampleSize = mdata->data_size;
    const uint32_t bufferSize = mdata->buffer_size;
    const int32_t numFramesBytes = numFrames * numChannels * sampleSize;
    uint32_t bufpos_kernel, bufpos_userspace;
    int32_t distance, pending;

    if (! impl->started)
    {
        DEBUGPRINT("%08u | playback | kernel is ready, starting", impl->frame);
        impl->started = true;
        mdata->active_userspace = 2;

        bufpos_kernel = __atomic_load_n(&mdata->bufpos_kernel, __ATOMIC_ACQUIRE);
        bufpos_userspace = (bufpos_kernel + numFramesBytes * AUDIO_BRIDGE_PLAYBACK_RINGBUFFER_BLOCKS) % bufferSize;
        __atomic_store_n(&mdata->bufpos_userspace, bufpos_userspace, __ATOMIC_RELEASE);

        for (int i = 0; i < NUM_PPMS; ++i)
            impl->distance.ppms[i] = numFramesBytes * AUDIO_BRIDGE_PLAYBACK_RINGBUFFER_BLOCKS;

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
        DEBUGPRINT("%08u | playback | out of data | %d", impl->frame, distance / sampleSize / numChannels);
        bufpos_userspace = (bufpos_kernel + numFramesBytes * AUDIO_BRIDGE_PLAYBACK_RINGBUFFER_BLOCKS) % bufferSize;
    }

    while (distance > numFramesBytes * AUDIO_BRIDGE_PLAYBACK_RINGBUFFER_BLOCKS * 2)
    {
        DEBUGPRINT("%08u | playback | too much data | %d", impl->frame, distance / sampleSize / numChannels);
        // skip ahead one audio cycle
        bufpos_userspace = positive_modulo(bufpos_userspace - numFramesBytes, bufferSize);
        distance -= numFramesBytes;
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
        const int32_t idx = impl->distance.idx++ % NUM_PPMS;
        impl->distance.sum = impl->distance.sum - impl->distance.ppms[idx] + distance;
        impl->distance.ppms[idx] = distance;

        mdata->extra_ppm = static_cast<double>(distance - numFramesBytes * AUDIO_BRIDGE_PLAYBACK_RINGBUFFER_BLOCKS) * 8 / numFramesBytes;
    }

    // fprintf(stderr, "\r%08u | playback | kernel is running, distance %d, sum %ld, extra_ppm %d $ ",
    //         impl->frame, distance / sampleSize / numChannels, impl->distance.sum, mdata->extra_ppm);

    return true;
}

// --------------------------------------------------------------------------------------------------------------------
