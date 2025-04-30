// SPDX-FileCopyrightText: 2021-2025 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-device.hpp"
#include "audio-device-impl.hpp"
#include "audio-utils.hpp"

#include <memory>

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

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
    uint8_t buffer[];
};

struct AudioDevice::Impl {
    // config copy
    bool playback;
    uint16_t sampleRate;

    // direct pointer
    Process* proc;

   #if AUDIO_BRIDGE_DEBUG
    // monotonic frame counter
    uint32_t frame = 0;
   #endif

    // direct kernel memory access
    int fd;
    uac_mmap_data* mdata;

    // buffer for reading alsa data in RT
    uint8_t* rawBuffer;

    // whether audio device is closing down, triggered by `closeAudioDeviceImpl`
    bool closing = false;

    // whether audio device has been disconnected
    bool disconnected = false;
};

// --------------------------------------------------------------------------------------------------------------------

AudioDevice::Impl* initAudioDeviceImpl(const AudioDevice* const dev, AudioDevice::HWConfig& hwconfig)
{
    std::unique_ptr<AudioDevice::Impl> impl = std::unique_ptr<AudioDevice::Impl>(new AudioDevice::Impl);
    impl->playback = dev->config.playback;
    impl->sampleRate = dev->config.sampleRate;
    impl->proc = &dev->proc;

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
    impl->closing = true;

    delete[] impl->rawBuffer;

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

bool runAudioDeviceCaptureSyncImpl(AudioDevice::Impl* const impl, float* buffers[], const uint16_t numFrames)
{
    uac_mmap_data* const mdata = impl->mdata;

    if (mdata->active_kernel == 0 || mdata->sample_rate != impl->sampleRate)
    {
        impl->closing = true;
        return false;
    }

    DeviceState state = static_cast<DeviceState>(impl->proc->state.load());

    const uint32_t numChannels = mdata->num_channels;
    const uint32_t sampleSize = mdata->data_size;
    const uint32_t bufferSize = mdata->buffer_size;

    if (state == kDeviceInitializing)
    {
        if (mdata->active_kernel != 0)
        {
            DEBUGPRINT("%08u | capture | kernel is ready kDeviceInitializing -> kDeviceStarting", impl->frame);
            state = kDeviceStarting;
            impl->proc->state.store(kDeviceStarting);
        }
        else
        {
            // DEBUGPRINT("%08u | capture | kernel is not ready, waiting 1 cycle", impl->frame);
            return false;
        }
    }

    if (state == kDeviceStarting)
    {
        DEBUGPRINT("%08u | capture | kernel is ready kDeviceStarting -> kDeviceBuffering", impl->frame);
        mdata->active_userspace = 2;
        state = kDeviceBuffering;
        impl->proc->state.store(kDeviceBuffering);
        return false;
    }

    const uint32_t numFramesBytes = numFrames * numChannels * sampleSize;
    uint32_t bufpos_kernel = mdata->bufpos_kernel;

    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    uint32_t bufpos_userspace = mdata->bufpos_userspace;

//     if (state == kDeviceBuffering)
    {
        uint32_t distance = (bufpos_kernel - bufpos_userspace) % bufferSize;

        if (distance < numFramesBytes)
        {
            DEBUGPRINT("%08u | capture | out of data | %u", impl->frame, distance / sampleSize / numChannels);
            bufpos_userspace = (bufpos_kernel - numFramesBytes * AUDIO_BRIDGE_CAPTURE_RINGBUFFER_BLOCKS) % bufferSize;

            if (const uint32_t rest = bufpos_userspace % (numChannels * sampleSize))
                bufpos_userspace -= rest;

            if ((bufpos_kernel % (numChannels * sampleSize)) != 0)
            {
                DEBUGPRINT("%08u | capture | bufpos_kernel1 %u (numChannels * sampleSize) %u",
                           impl->frame, bufpos_userspace, numChannels * sampleSize);
                abort();
            }
            if ((bufpos_userspace % (numChannels * sampleSize)) != 0)
            {
                DEBUGPRINT("%08u | capture | bufpos_userspace1 %u (numChannels * sampleSize) %u",
                           impl->frame, bufpos_userspace, numChannels * sampleSize);
                abort();
            }
        }

        while (distance > numFramesBytes * AUDIO_BRIDGE_CAPTURE_RINGBUFFER_BLOCKS + numFramesBytes * 4)
        {
            DEBUGPRINT("%08u | capture | too much data | %u", impl->frame, distance / sampleSize / numChannels);
            // skip ahead one audio cycle
            bufpos_userspace = (bufpos_userspace + numFramesBytes) % bufferSize;
            distance -= numFramesBytes;

            if ((bufpos_userspace % numChannels * sampleSize) != 0)
            {
                DEBUGPRINT("%08u | capture | bufpos_userspace2 %u numChannels * sampleSize %u",
                           impl->frame, bufpos_userspace, numChannels * sampleSize);
                abort();
            }
        }

        if (state == kDeviceBuffering)
        {
            DEBUGPRINT("%08u | capture | kernel has enough samples kDeviceBuffering -> kDeviceRunning | %u", impl->frame, distance / sampleSize / numChannels);
            state = kDeviceRunning;
            impl->proc->state.store(kDeviceRunning);
        }
    }

    uint32_t pending = bufferSize - bufpos_userspace;

    if (pending < numFramesBytes)
    {
        std::memcpy(impl->rawBuffer, mdata->buffer + bufpos_userspace, pending);
        std::memcpy(impl->rawBuffer + pending, mdata->buffer, numFramesBytes - pending);
    }
    else
    {
        std::memcpy(impl->rawBuffer, mdata->buffer + bufpos_userspace, numFramesBytes);
    }

    __atomic_thread_fence(__ATOMIC_RELEASE);
    mdata->bufpos_userspace = (bufpos_userspace + numFramesBytes) % bufferSize;

    if ((mdata->bufpos_userspace % numChannels * sampleSize) != 0)
    {
        DEBUGPRINT("%08u | capture | bufpos_userspace3 %u numChannels * sampleSize %u",
                    impl->frame, mdata->bufpos_userspace, numChannels * sampleSize);
        abort();
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

    if (mdata->active_kernel == 0 || mdata->sample_rate != impl->sampleRate)
    {
        impl->closing = true;
        return false;
    }

    DeviceState state = static_cast<DeviceState>(impl->proc->state.load());

    if (state == kDeviceInitializing)
    {
        if (mdata->active_kernel != 0)
        {
            DEBUGPRINT("%08u | capture | kernel is ready kDeviceInitializing -> kDeviceBuffering", impl->frame);
            mdata->active_userspace = 2;
            state = kDeviceBuffering;
            impl->proc->state.store(kDeviceBuffering);
        }
        else
        {
            // DEBUGPRINT("%08u | capture | kernel is not ready, waiting 1 cycle", impl->frame);
            return false;
        }
    }

    const uint32_t numChannels = mdata->num_channels;
    const uint32_t sampleSize = mdata->data_size;
    const uint32_t bufferSize = mdata->buffer_size;

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

    const uint32_t numFramesBytes = numFrames * numChannels * sampleSize;
    uint32_t bufpos_kernel = mdata->bufpos_kernel;

    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    uint32_t bufpos_userspace = mdata->bufpos_userspace;

//     if (state == kDeviceBuffering)
    {
        uint32_t distance = (bufpos_userspace - bufpos_kernel) % bufferSize;

        if (distance < numFramesBytes)
        {
            DEBUGPRINT("%08u | playback | out of data | %u", impl->frame, distance / sampleSize / numChannels);
            bufpos_userspace = (bufpos_kernel + numFramesBytes * AUDIO_BRIDGE_CAPTURE_RINGBUFFER_BLOCKS) % bufferSize;

            if ((bufpos_kernel % (numChannels * sampleSize)) != 0)
            {
                DEBUGPRINT("%08u | playback | bufpos_kernel1 %u (numChannels * sampleSize) %u",
                           impl->frame, bufpos_userspace, numChannels * sampleSize);
                abort();
            }
            if ((bufpos_userspace % (numChannels * sampleSize)) != 0)
            {
                DEBUGPRINT("%08u | playback | bufpos_userspace1 %u (numChannels * sampleSize) %u",
                           impl->frame, bufpos_userspace, numChannels * sampleSize);
                abort();
            }
        }

        while (distance > numFramesBytes * AUDIO_BRIDGE_CAPTURE_RINGBUFFER_BLOCKS + numFramesBytes * 4)
        {
            DEBUGPRINT("%08u | playback | too much data | %u", impl->frame, distance / sampleSize / numChannels);
            // skip ahead one audio cycle
            bufpos_userspace = (bufpos_userspace - numFramesBytes) % bufferSize;
            distance -= numFramesBytes;

            if (const uint32_t rest = bufpos_userspace % (numChannels * sampleSize))
                bufpos_userspace -= rest;

            if ((bufpos_userspace % numChannels * sampleSize) != 0)
            {
                DEBUGPRINT("%08u | capture | bufpos_userspace2 %u numChannels * sampleSize %u",
                           impl->frame, bufpos_userspace, numChannels * sampleSize);
                abort();
            }
        }

        if (state == kDeviceBuffering)
        {
            DEBUGPRINT("%08u | playback | wrote enough samples kDeviceBuffering -> kDeviceRunning | %u", impl->frame, distance / sampleSize / numChannels);
            state = kDeviceRunning;
            impl->proc->state.store(kDeviceRunning);
        }
    }

    uint32_t pending = bufferSize - bufpos_userspace;

    if (pending < numFramesBytes)
    {
        std::memcpy(mdata->buffer + bufpos_userspace, impl->rawBuffer, pending);
        std::memcpy(mdata->buffer, impl->rawBuffer + pending, numFramesBytes - pending);
    }
    else
    {
        std::memcpy(mdata->buffer + bufpos_userspace, impl->rawBuffer, numFramesBytes);
    }

    __atomic_thread_fence(__ATOMIC_RELEASE);
    mdata->bufpos_userspace = (bufpos_userspace + numFramesBytes) % bufferSize;

    if ((mdata->bufpos_userspace % numChannels * sampleSize) != 0)
    {
        DEBUGPRINT("%08u | capture | bufpos_userspace2 %u numChannels * sampleSize %u",
                    impl->frame, mdata->bufpos_userspace, numChannels * sampleSize);
        abort();
    }

    return true;
}

// --------------------------------------------------------------------------------------------------------------------
