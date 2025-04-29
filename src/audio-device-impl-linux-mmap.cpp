// SPDX-FileCopyrightText: 2021-2025 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-device.hpp"
#include "audio-device-impl.hpp"
#include "audio-utils.hpp"

#include <memory>

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
// #include <sched.h>

#define PAGE_SIZE 1024

#define USABLE_KERNEL_BUFFER_SIZE (PAGE_SIZE - sizeof(int) * 4)

// --------------------------------------------------------------------------------------------------------------------

struct uac2_mmap_data {
    int magic1;
    int magic2;
    int active_kernel;
    int active_userspace;
    char buffer[];
};

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
    Process* proc;

   #if AUDIO_BRIDGE_DEBUG
    // monotonic frame counter
    uint32_t frame = 0;
   #endif

    int fd;
    uac2_mmap_data* mdata;
    uint32_t offset = 0;

    // buffer for reading alsa data in RT
    uint8_t* rawBuffer;

    // buffer for converting raw buffer into float
    float** floatBuffers;

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
    impl->bufferSize = dev->config.bufferSize;
    impl->proc = &dev->proc;

    // known config
    hwconfig.format = kSampleFormat32;
    hwconfig.numChannels = 9;
    hwconfig.numPeriods = 4;
    hwconfig.periodSize = 64;
    hwconfig.fullBufferSize = 4 * 64;
    hwconfig.sampleRate = 48000;

    // ----------------------------------------------------------------------------------------------------------------

    int fd = open("/proc/uac2", O_RDWR | O_SYNC);
    if (fd < 0)
    {
        DEBUGPRINT("failed to open /proc/uac2");
        return nullptr;
    }

    uac2_mmap_data* mdata = static_cast<uac2_mmap_data*>(mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (mdata == NULL || mdata == MAP_FAILED)
    {
        DEBUGPRINT("failed to mmap /proc/uac2");
        close(fd);
        return nullptr;
    }

    // ----------------------------------------------------------------------------------------------------------------

    impl->format = hwconfig.format;
    impl->numChannels = hwconfig.numChannels;
    impl->periodSize = hwconfig.periodSize;
    impl->fullBufferSize = hwconfig.fullBufferSize;

    impl->fd = fd;
    impl->mdata = mdata;

    // ----------------------------------------------------------------------------------------------------------------

    {
        const uint8_t sampleSize = getSampleSizeFromFormat(impl->format);
        const uint8_t numChannels = impl->numChannels;
        const uint16_t periodSize = impl->periodSize;

        impl->rawBuffer = new uint8_t [periodSize * sampleSize * numChannels];
        impl->floatBuffers = new float* [numChannels];
        for (uint8_t c = 0; c < numChannels; ++c)
            impl->floatBuffers[c] = new float [periodSize];
    }

    impl->mdata->active_userspace = 1;

    return impl.release();
}

void closeAudioDeviceImpl(AudioDevice::Impl* const impl)
{
    impl->closing = true;

    for (uint8_t c = 0; c < impl->numChannels; ++c)
            delete[] impl->floatBuffers[c];
    delete[] impl->floatBuffers;
    delete[] impl->rawBuffer;

    munmap(impl->mdata, PAGE_SIZE);
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

bool runAudioDeviceCaptureSyncImpl(AudioDevice::Impl* const impl, const uint16_t numFrames)
{
    DeviceState state = static_cast<DeviceState>(impl->proc->state.load());

    const uint32_t numChannels = impl->numChannels;
    const uint32_t sampleSize = getSampleSizeFromFormat(impl->format);

    if (state == kDeviceInitializing)
    {
        if (impl->mdata->active_kernel != 0)
        {
            DEBUGPRINT("%08u | capture | kernel is ready kDeviceInitializing -> kDeviceStarting", impl->frame);
            state = kDeviceStarting;
            impl->proc->state.store(kDeviceStarting);
            impl->proc->reset.store(kDeviceResetFull);
        }
        else
        {
            // DEBUGPRINT("%08u | capture | kernel is not ready, waiting 1 cycle", impl->frame);
            return false;
        }
    }

    if (state == kDeviceStarting)
    {
        // TODO
        state = kDeviceStarted;
        impl->proc->state.store(kDeviceStarted);
    }

    const uint32_t numFramesBytes = numFrames * sampleSize * numChannels;
    uint32_t pending = 4000 - impl->offset;

    if (pending < numFramesBytes)
    {
        std::memcpy(impl->rawBuffer, impl->mdata->buffer + impl->offset, pending);
        std::memcpy(impl->rawBuffer + pending, impl->mdata->buffer, numFramesBytes - pending);
    }
    else
    {
        std::memcpy(impl->rawBuffer, impl->mdata->buffer + impl->offset, numFramesBytes);
    }

    impl->offset = (impl->offset + numFramesBytes) % 4000;

    switch (impl->format)
    {
    case kSampleFormat16:
        int2float::s16(impl->floatBuffers, impl->rawBuffer, numChannels, numFrames);
        break;
    case kSampleFormat24:
        int2float::s24(impl->floatBuffers, impl->rawBuffer, numChannels, numFrames);
        break;
    case kSampleFormat24LE3:
        int2float::s24le3(impl->floatBuffers, impl->rawBuffer, numChannels, numFrames);
        break;
    case kSampleFormat32:
        int2float::s32(impl->floatBuffers, impl->rawBuffer, numChannels, numFrames);
        break;
    default:
        DEBUGPRINT("unknown format");
        break;
    }

    if (impl->proc->ringbuffer->write(impl->floatBuffers, numFrames))
    {
        switch (state)
        {
        case kDeviceStarted ... kDeviceRunning - 1:
            impl->proc->state.store(state + 1);
            break;
        default:
            break;
        }
    }
    else
    {
        impl->proc->state.store(kDeviceInitializing);
        impl->proc->reset.store(kDeviceResetFull);
        DEBUGPRINT("%08u | capture | ringbuffer full", impl->frame);
        return false;
    }

    return true;
}
