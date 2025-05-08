// SPDX-FileCopyrightText: 2021-2025 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-device.hpp"
#include "audio-device-impl.hpp"
#include "audio-utils.hpp"

#include <memory>

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <sched.h>

#if AUDIO_BRIDGE_UDEV
#include <libudev.h>
#include <sys/time.h>
#endif

// --------------------------------------------------------------------------------------------------------------------

#ifdef _DARKGLASS_DEVICE_PABLITO
static constexpr const uint kNumPeriodsCapture = 32;
static constexpr const uint kNumPeriodsPlayback = 9;
#else
static constexpr const uint kNumPeriodsMin = 3;
static constexpr const uint kNumPeriodsMax = 12;
#endif

static constexpr const snd_pcm_format_t kSampleFormatsToTry[] = {
    SND_PCM_FORMAT_S32,
    SND_PCM_FORMAT_S24_3LE,
    SND_PCM_FORMAT_S24,
    SND_PCM_FORMAT_S16,
};

static constexpr const uint kSampleRatesToTry[] = { 48000, 44100, 96000, 88200 };

// --------------------------------------------------------------------------------------------------------------------

struct AudioDevice::Impl {
    // config copy
    bool playback;
    uint16_t bufferSize;
    uint32_t sampleRate;

    // hwconfig copy
    SampleFormat format;
    uint8_t numChannels;
    uint16_t periodSize;
    uint32_t fullBufferSize;

   #if AUDIO_BRIDGE_DEBUG
    // monotonic frame counter
    uint32_t frame = 0;
   #endif

    // ALSA PCM handle
    snd_pcm_t* pcm = nullptr;

   #if AUDIO_BRIDGE_UDEV
    // dynamic sample rate changes
    struct udev* udev = nullptr;
    struct udev_monitor* udev_mon = nullptr;
    pthread_t udev_thread;
   #endif

   #if AUDIO_BRIDGE_ASYNC
    // direct pointer
    Process* proc;

    // sync primitives
    pthread_t thread;
   #else
    // buffer for reading alsa data in RT
    uint8_t* rawBuffer;

    // buffer for converting raw buffer into float
    float** floatBuffers;
   #endif

    // whether audio device is closing down, triggered by `closeAudioDeviceImpl`
    bool closing = false;

    // whether audio device has been disconnected
    bool disconnected = false;
};

// --------------------------------------------------------------------------------------------------------------------

#if AUDIO_BRIDGE_UDEV
static void* _audio_device_udev_thread(void* const arg)
{
    AudioDevice::Impl* const impl = static_cast<AudioDevice::Impl*>(arg);

    const int fd = udev_monitor_get_fd(impl->udev_mon);

    fd_set rfds;
    FD_ZERO(&rfds);

    DEBUGPRINT("%010u | udev thread started", impl->frame);

    for (int ret; ! impl->closing;)
    {
        struct timeval tv = { 1, 0 };
        FD_SET(fd, &rfds);

        ret = select(fd + 1, &rfds, nullptr, nullptr, &tv);
        // DEBUGPRINT("%010u | udev ret %d", impl->frame, ret);

        if (ret < 0)
            break;
        if (ret == 0)
            continue;

        DEBUGPRINT("%010u | new udev event!", impl->frame);

        struct udev_device* dev;
        while ((dev = udev_monitor_receive_device(impl->udev_mon)) != nullptr)
        {
           #if AUDIO_BRIDGE_DEBUG
            struct udev_list_entry* entry = udev_device_get_properties_list_entry(dev);
            DEBUGPRINT("%010u | new udev device message %s", impl->frame, udev_list_entry_get_name(entry));
           #endif

            if (const char* const usbstate = udev_device_get_property_value(dev, "USB_STATE"))
            {
                if (std::strcmp(usbstate, "SET_SAMPLE_RATE") == 0)
                {
                    impl->closing = true;
                    udev_device_unref(dev);
                    break;
                }
                /*
                if (std::strcmp(usbstate, "SET_AUDIO_CLK") == 0)
                {
                    const char* const val = udev_device_get_property_value(dev, "PPM");
                    impl->proc->ppm = std::atoi(val);
                    DEBUGPRINT("%010u | capture | got new SET_AUDIO_CLK %s", impl->frame, val);
                }
                */
            }
            udev_device_unref(dev);
        }
    }

    DEBUGPRINT("%010u | udev thread exit", impl->frame);
    return nullptr;
}
#endif

// TODO cleanup, see what is needed
static int _xrun_recovery(snd_pcm_t *handle, bool& closing, int err)
{
    DEBUGPRINT("stream recovery: %s", snd_strerror(err));

    if (err == -EPIPE)
    {
        /* under-run */
        err = snd_pcm_prepare(handle);
        if (err < 0)
            DEBUGPRINT("Can't recovery from underrun, prepare failed: %s", snd_strerror(err));
        return 0;
    }
    else if (err == -ESTRPIPE)
    {
        while ((err = snd_pcm_resume(handle)) == -EAGAIN && ! closing)
            sleep(1);   /* wait until the suspend flag is released */

        if (closing)
            return 0;

        if (err < 0)
        {
            err = snd_pcm_prepare(handle);
            if (err < 0)
                DEBUGPRINT("Can't recovery from suspend, prepare failed: %s", snd_strerror(err));
        }

        return 0;
    }

    return err;
}

#if AUDIO_BRIDGE_ASYNC
static void* _audio_device_capture_thread(void* const arg)
{
    AudioDevice::Impl* const impl = static_cast<AudioDevice::Impl*>(arg);

    const uint8_t sampleSize = getSampleSizeFromFormat(impl->format);
    const uint8_t numChannels = impl->numChannels;
    const uint16_t periodSize = impl->periodSize;
    const uint32_t sampleRate = impl->sampleRate;
    const uint32_t numBufferingSamples = impl->proc->numBufferingSamples;
    DEBUGPRINT("_audio_device_capture_thread sampleSize %u numChannels %u periodSize %u",
               sampleSize, numChannels, periodSize);

    uint8_t* const raw = new uint8_t [periodSize * sampleSize * numChannels];

    float** convBuffers = new float* [numChannels];
    for (uint8_t i = 0; i < numChannels; ++i)
        convBuffers[i] = new float [periodSize];

    uint32_t numAttemptsWaitingForStart = 0;

    simd::init();

    bool ok;
    snd_pcm_sframes_t err;
    while (! impl->closing)
    {
        DeviceState state = static_cast<DeviceState>(impl->proc->state.load());

        if (state == kDeviceInitializing)
        {
            // read until alsa buffers are empty
            bool started = false;
            while ((err = snd_pcm_mmap_readi(impl->pcm, raw, periodSize)) > 0)
                started = true;

            numAttemptsWaitingForStart = 0;

            if (err == -EPIPE)
            {
                DEBUGPRINT("%010u | capture | initial pipe error: %s", impl->frame, snd_strerror(err));
                snd_pcm_prepare(impl->pcm);
                sched_yield();
                snd_pcm_wait(impl->pcm, -1);
                continue;
            }
            if (err != -EAGAIN)
            {
                DEBUGPRINT("%010u | capture | initial read error: %s", impl->frame, snd_strerror(err));
                break;
            }

            if (started)
            {
                DEBUGPRINT("%010u | capture | can read data? kDeviceInitializing -> kDeviceStarting", impl->frame);
                state = kDeviceStarting;
                impl->proc->state.store(kDeviceStarting);
                impl->proc->reset.store(kDeviceResetFull);
            }
            else
            {
                DEBUGPRINT("%010u | capture | kDeviceInitializing waiting 1 cycle", impl->frame);
                sched_yield();
                snd_pcm_wait(impl->pcm, -1);
                continue;
            }
        }

        if (state == kDeviceStarting)
        {
            // check if device is running
            err = snd_pcm_avail(impl->pcm);

            if (err > 0)
            {
                DEBUGPRINT("%010u | capture | device is running, kDeviceStarting -> kDeviceStarted", impl->frame);
                state = kDeviceStarted;
                impl->proc->state.store(kDeviceStarted);
            }
            else
            {
                switch (err)
                {
                case 0:
                    if (++numAttemptsWaitingForStart >= sampleRate / periodSize)
                    {
                        DEBUGPRINT("%010u | capture | kDeviceStarting took more than 1 second, closing", impl->frame);
                        impl->closing = true;
                        continue;
                    }
                    DEBUGPRINT("%010u | capture | kDeviceStarting waiting 1 cycle", impl->frame);
                    sched_yield();
                    snd_pcm_wait(impl->pcm, -1);
                    continue;
                case -EPIPE:
                    DEBUGPRINT("%010u | capture | EPIPE while kDeviceStarting", impl->frame);
                    snd_pcm_prepare(impl->pcm);
                    sched_yield();
                    snd_pcm_wait(impl->pcm, -1);
                    continue;
                default:
                    impl->closing = true;
                    DEBUGPRINT("%010u | capture | starting read error: %s", impl->frame, snd_strerror(err));
                    break;
                }

                if (impl->closing)
                    break;
            }
        }

        numAttemptsWaitingForStart = 0;
        err = snd_pcm_mmap_readi(impl->pcm, raw, periodSize);

        if (impl->closing)
            break;

        switch (err)
        {
        case -EPIPE:
            snd_pcm_prepare(impl->pcm);
            // fall-through
        case -EAGAIN:
        case 0:
            sched_yield();
            snd_pcm_wait(impl->pcm, -1);
            continue;
        }

        if (err < 0)
        {
            impl->proc->state.store(kDeviceStarting);
            impl->proc->reset.store(kDeviceResetFull);

            DEBUGPRINT("%010u | capture | Read error %s", impl->frame, snd_strerror(err));

            // TODO offline recovery
            if (_xrun_recovery(impl->pcm, impl->closing, err) < 0)
            {
                DEBUGPRINT("%010u | capture | xrun_recovery error: %s", impl->frame, snd_strerror(err));
                impl->closing = true;
                break;
            }

            continue;
        }

        if (state == kDeviceStarted)
        {
            // wait for host side to be ready
            sched_yield();
            snd_pcm_wait(impl->pcm, -1);
            continue;
        }

        switch (impl->format)
        {
        case kSampleFormat16:
            int2float::s16(convBuffers, raw, numChannels, err);
            break;
        case kSampleFormat24:
            int2float::s24(convBuffers, raw, numChannels, err);
            break;
        case kSampleFormat24LE3:
            int2float::s24le3(convBuffers, raw, numChannels, err);
            break;
        case kSampleFormat32:
            int2float::s32(convBuffers, raw, numChannels, err);
            break;
        default:
            DEBUGPRINT("unknown format");
            break;
        }

        pthread_mutex_lock(&impl->proc->ringbufferLock);
        ok = impl->proc->ringbuffer->write(convBuffers, err);
        pthread_mutex_unlock(&impl->proc->ringbufferLock);

       #if AUDIO_BRIDGE_DEBUG
        static int counter = 0;
        if (++counter == 256)
        {
            counter = 0;
            DEBUGPRINT("%010u | capture | check %u vs %u",
                       impl->frame, impl->proc->ringbuffer->getNumReadableSamples(), numBufferingSamples);
        }
       #endif

        if (ok)
        {
            if (state == kDeviceBuffering && impl->proc->ringbuffer->getNumReadableSamples() >= numBufferingSamples)
                impl->proc->state.store(kDeviceRunning);
        }
        else
        {
            DEBUGPRINT("%010u | capture | failed writing data, ... -> kDeviceStarting", impl->frame);

            impl->proc->state.store(kDeviceStarting);
            impl->proc->reset.store(kDeviceResetFull);
            sched_yield();
            snd_pcm_wait(impl->pcm, -1);
        }
    }

    impl->disconnected = true;
    return nullptr;
}

static void* _audio_device_playback_thread(void* const arg)
{
    AudioDevice::Impl* const impl = static_cast<AudioDevice::Impl*>(arg);

    const uint8_t sampleSize = getSampleSizeFromFormat(impl->format);
    const uint8_t numChannels = impl->numChannels;
    const uint16_t periodSize = impl->periodSize;
    const uint32_t numBufferingSamples = impl->proc->numBufferingSamples;
    DEBUGPRINT("_audio_device_playback_thread sampleSize %u numChannels %u periodSize %u",
               sampleSize, numChannels, periodSize);

    uint8_t* const raw = new uint8_t [periodSize * sampleSize * numChannels];
    uint8_t* const zeros = new uint8_t [periodSize * sampleSize * numChannels];
    std::memset(zeros, 0, periodSize * sampleSize * numChannels);

    float** convBuffers = new float* [numChannels];
    for (uint8_t i = 0; i < numChannels; ++i)
        convBuffers[i] = new float [periodSize];

    simd::init();

    bool ok;
    snd_pcm_sframes_t err;
    while (! impl->closing)
    {
        DeviceState state = static_cast<DeviceState>(impl->proc->state.load());

        if (state == kDeviceInitializing)
        {
            // write silence until alsa buffers are full
            bool started = false;
            while ((err = snd_pcm_mmap_writei(impl->pcm, zeros, periodSize)) > 0)
                started = true;

            if (err != -EAGAIN)
            {
                DEBUGPRINT("%010u | playback | initial write error: %s", impl->frame, snd_strerror(err));
                break;
            }

            if (started)
            {
                DEBUGPRINT("%010u | playback | can write data? kDeviceInitializing -> kDeviceStarting", impl->frame);
                state = kDeviceStarting;
                impl->proc->state.store(kDeviceStarting);
                impl->proc->reset.store(kDeviceResetFull);
            }
            else
            {
                DEBUGPRINT("%010u | playback | kDeviceInitializing waiting 1 cycle", impl->frame);
                sched_yield();
                snd_pcm_wait(impl->pcm, -1);
                continue;
            }
        }

        if (state == kDeviceStarting)
        {
            // check if device is running
            err = snd_pcm_avail(impl->pcm);

            if (err > 0)
            {
                DEBUGPRINT("%010u | playback | device is running, kDeviceStarting -> kDeviceStarted", impl->frame);
                state = kDeviceStarted;
                impl->proc->state.store(kDeviceStarted);
            }
            else
            {
                switch (err)
                {
                case 0:
                    DEBUGPRINT("%010u | playback | kDeviceStarting waiting 1 cycle", impl->frame);
                    sched_yield();
                    snd_pcm_wait(impl->pcm, -1);
                    continue;
                default:
                    DEBUGPRINT("%010u | playback | initial write error: %s", impl->frame, snd_strerror(err));
                    break;
                }
            }
        }

        if (state == kDeviceStarted)
        {
            // wait for host side to be ready
            snd_pcm_mmap_writei(impl->pcm, zeros, periodSize);
            sched_yield();
            snd_pcm_wait(impl->pcm, -1);
            continue;
        }

        if (state == kDeviceBuffering)
        {
            if (impl->proc->ringbuffer->getNumReadableSamples() < numBufferingSamples)
            {
               #if AUDIO_BRIDGE_DEBUG && 0
                DEBUGPRINT("%010u | playback | kDeviceBuffering waiting 1 cycle because ringbuffer not ready, %u",
                           impl->frame, impl->proc->ringbuffer->getNumReadableSamples());
               #endif

                snd_pcm_mmap_writei(impl->pcm, zeros, periodSize);
                sched_yield();
                snd_pcm_wait(impl->pcm, -1);
                continue;
            }

            DEBUGPRINT("%010u | playback | has enough ringbuffer data, kDeviceBuffering -> kDeviceRunning", impl->frame);
            impl->proc->state.store(kDeviceRunning);
        }

        pthread_mutex_lock(&impl->proc->ringbufferLock);
        ok = impl->proc->ringbuffer->read(convBuffers, periodSize);
        pthread_mutex_unlock(&impl->proc->ringbufferLock);

        if (! ok)
        {
           #if AUDIO_BRIDGE_DEBUG
            static int counter = 0;
            if (++counter == 50)
            {
                counter = 0;
                DEBUGPRINT("%010u | playback | WARNING | failed reading data", impl->frame);
            }
           #endif

            impl->proc->state.store(kDeviceBuffering);
            impl->proc->reset.store(kDeviceResetStats);
            snd_pcm_mmap_writei(impl->pcm, zeros, periodSize);
            sched_yield();
            snd_pcm_wait(impl->pcm, -1);
            continue;
        }

       #if AUDIO_BRIDGE_DEBUG
        static int counter = 0;
        if (++counter == 250)
        {
            counter = 0;
            DEBUGPRINT("%010u | playback | check %u vs %u",
                       impl->frame, impl->proc->ringbuffer->getNumReadableSamples() , numBufferingSamples);
        }
       #endif

        if (impl->closing)
            break;

        switch (impl->format)
        {
        case kSampleFormat16:
            float2int::s16(raw, convBuffers, numChannels, periodSize);
            break;
        case kSampleFormat24:
            float2int::s24(raw, convBuffers, numChannels, periodSize);
            break;
        case kSampleFormat24LE3:
            float2int::s24le3(raw, convBuffers, numChannels, periodSize);
            break;
        case kSampleFormat32:
            float2int::s32(raw, convBuffers, numChannels, periodSize);
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

            if (err < 0)
            {
                if (err == -EAGAIN)
                {
                   #if AUDIO_BRIDGE_DEBUG && 0
                    DEBUGPRINT("%010u | playback | kDeviceBuffering waiting 1 cycle", impl->frame);
                   #endif
                    sched_yield();
                    snd_pcm_wait(impl->pcm, -1);
                    continue;
                }

                impl->proc->state.store(kDeviceStarting);
                impl->proc->reset.store(kDeviceResetFull);

                DEBUGPRINT("%010u | playback | Write error: %s", impl->frame, snd_strerror(err));

                if (_xrun_recovery(impl->pcm, impl->closing, err) < 0)
                {
                    DEBUGPRINT("playback | xrun recovery error: %s", snd_strerror(err));
                    impl->closing = true;
                }

                break;
            }

            // FIXME does this ever happen now ??
            if (static_cast<uint16_t>(err) != frames)
            {
                DEBUGPRINT("%010u | playback | Incomplete write %ld of %u", impl->frame, err, frames);

                ptr += err * numChannels * sampleSize;
                frames -= err;

                sched_yield();
                usleep(0);
                continue;
            }

            break;
        }
    }

    impl->disconnected = true;
    delete[] raw;
    delete[] zeros;

    return nullptr;
}
#endif // AUDIO_BRIDGE_ASYNC

// --------------------------------------------------------------------------------------------------------------------

#if AUDIO_BRIDGE_DEBUG < 2
static void _snd_lib_error_silence(const char*, int, const char*, int, const char*, ...) {}
#endif

AudioDevice::Impl* initAudioDeviceImpl(const AudioDevice* const dev, AudioDevice::HWConfig& hwconfig)
{
    std::unique_ptr<AudioDevice::Impl> impl = std::unique_ptr<AudioDevice::Impl>(new AudioDevice::Impl);
    impl->playback = dev->config.playback;
    impl->bufferSize = dev->config.bufferSize;
    impl->sampleRate = dev->config.sampleRate;
   #if AUDIO_BRIDGE_ASYNC
    impl->proc = &dev->proc;
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

   #if AUDIO_BRIDGE_DEBUG < 2
    // silence warnings when opening PCM
    snd_lib_error_set_handler(_snd_lib_error_silence);
   #endif

    err = snd_pcm_open(&impl->pcm, dev->config.deviceID, mode, flags);

   #if AUDIO_BRIDGE_DEBUG < 2
    snd_lib_error_set_handler(nullptr);
   #endif

    if (err < 0)
    {
       #if AUDIO_BRIDGE_DEBUG >= 2
        DEBUGPRINT("snd_pcm_open fail %d %s\n", dev->config.playback, snd_strerror(err));
       #endif
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
            DEBUGPRINT("snd_pcm_hw_params_set_format fail %u:%s %s", format, snd_pcm_format_name(format), snd_strerror(err));
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
                       format, snd_pcm_format_name(format));
            continue;
        }

        DEBUGPRINT("snd_pcm_hw_params_set_format %s", snd_pcm_format_name(format));
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
            DEBUGPRINT("snd_pcm_hw_params_set_rate %u fail %s", rate, snd_strerror(err));
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
    DEBUGPRINT("num channels req: %u, got: %u", hwconfig.numChannels, uintParam);
    hwconfig.numChannels = uintParam;

    // ----------------------------------------------------------------------------------------------------------------
    // num periods + period size

    uintParam = 0;

#ifdef _DARKGLASS_DEVICE_PABLITO
    if (dev->config.playback)
    {
        ulongParam = 0;
        if ((err = snd_pcm_hw_params_set_buffer_size_min(pcm, params, &ulongParam)) != 0)
        {
            DEBUGPRINT("snd_pcm_hw_params_set_buffer_size_min fail %u %u %s",
                       0, AUDIO_BRIDGE_DEVICE_BUFFER_SIZE, snd_strerror(err));
            goto error;
        }
        ulongParam = AUDIO_BRIDGE_DEVICE_BUFFER_SIZE * kNumPeriodsPlayback;
        if ((err = snd_pcm_hw_params_set_buffer_size_max(pcm, params, &ulongParam)) != 0)
        {
            DEBUGPRINT("snd_pcm_hw_params_set_buffer_size_max fail %u %u %s",
                       kNumPeriodsPlayback, AUDIO_BRIDGE_DEVICE_BUFFER_SIZE, snd_strerror(err));
            goto error;
        }
        uintParam = kNumPeriodsPlayback;
    }
    else
    {
        ulongParam = AUDIO_BRIDGE_DEVICE_BUFFER_SIZE * kNumPeriodsCapture;
        if ((err = snd_pcm_hw_params_set_buffer_size_min(pcm, params, &ulongParam)) != 0)
        {
            DEBUGPRINT("snd_pcm_hw_params_set_buffer_size_min fail %u %u %s",
                       0, AUDIO_BRIDGE_DEVICE_BUFFER_SIZE, snd_strerror(err));
            goto error;
        }
        uintParam = kNumPeriodsCapture;
    }
#else
    for (uint periods = kNumPeriodsMin; periods <= kNumPeriodsMax; ++periods)
    {
        if (dev->config.playback)
        {
            ulongParam = AUDIO_BRIDGE_DEVICE_BUFFER_SIZE * periods * 16;
            if ((err = snd_pcm_hw_params_set_buffer_size_max(pcm, params, &ulongParam)) != 0)
            {
                DEBUGPRINT("snd_pcm_hw_params_set_buffer_size_max fail %u %u %s",
                           periods, AUDIO_BRIDGE_DEVICE_BUFFER_SIZE, snd_strerror(err));
                continue;
            }
        }
        else
        {
            ulongParam = AUDIO_BRIDGE_DEVICE_BUFFER_SIZE * periods;
            if ((err = snd_pcm_hw_params_set_buffer_size_minmax(pcm, params, &ulongParam, &ulongParam)) != 0)
            {
                DEBUGPRINT("snd_pcm_hw_params_set_buffer_size_minmax fail %u %u %s",
                        periods, AUDIO_BRIDGE_DEVICE_BUFFER_SIZE, snd_strerror(err));
                continue;
            }
        }

        DEBUGPRINT("snd_pcm_hw_params_set_buffer_size_min/max %u %lu", periods, ulongParam);
        uintParam = periods;
        break;
    }
#endif

    if (uintParam == 0)
    {
        DEBUGPRINT("can't find a buffer size match");
        goto error;
    }

    hwconfig.numPeriods = uintParam;
    hwconfig.periodSize = AUDIO_BRIDGE_DEVICE_BUFFER_SIZE;
    hwconfig.fullBufferSize = AUDIO_BRIDGE_DEVICE_BUFFER_SIZE * uintParam;

    snd_pcm_hw_params_get_periods(params, &uintParam, nullptr);
    DEBUGPRINT("num periods req: %u, got: %u", hwconfig.numPeriods, uintParam);
    hwconfig.numPeriods = uintParam;

    snd_pcm_hw_params_get_period_size(params, &ulongParam, nullptr);
    DEBUGPRINT("period size req: %u, got: %lu", hwconfig.periodSize, ulongParam);
    hwconfig.periodSize = ulongParam;

    snd_pcm_hw_params_get_buffer_size(params, &ulongParam);
    DEBUGPRINT("full buffer size %u, got: %lu", hwconfig.fullBufferSize, ulongParam);
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
    if ((err = snd_pcm_sw_params_set_tstamp_mode(pcm, swparams, SND_PCM_TSTAMP_NONE)) != 0)
    {
        DEBUGPRINT("snd_pcm_sw_params_set_tstamp_mode fail %s", snd_strerror(err));
        goto error;
    }

    /*
    // SND_PCM_TSTAMP_TYPE_MONOTONIC
    if ((err = snd_pcm_sw_params_set_tstamp_type(pcm, swparams, SND_PCM_TSTAMP_TYPE_MONOTONIC_RAW)) != 0)
    {
        DEBUGPRINT("snd_pcm_sw_params_set_tstamp_type fail %s", snd_strerror(err));
        goto error;
    }
    */

    // how many samples we need to until snd_pcm_wait completes
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

    impl->format = hwconfig.format;
    impl->numChannels = hwconfig.numChannels;
    impl->periodSize = hwconfig.periodSize;
    impl->fullBufferSize = hwconfig.fullBufferSize;

   #if AUDIO_BRIDGE_ASYNC
    dev->proc.numBufferingSamples = std::max<uint32_t>(dev->config.bufferSize, dev->hwconfig.fullBufferSize);
    dev->proc.numBufferingSamples *= dev->config.playback ? AUDIO_BRIDGE_PLAYBACK_RINGBUFFER_BLOCKS
                                                          : AUDIO_BRIDGE_CAPTURE_RINGBUFFER_BLOCKS;
   #endif

   #if AUDIO_BRIDGE_UDEV
    if (struct udev* const udev = udev_new())
    {
        if (struct udev_monitor* const udev_mon = udev_monitor_new_from_netlink(udev, "kernel"))
        {
            udev_monitor_filter_add_match_subsystem_devtype(udev_mon, "u_audio", nullptr);
            udev_monitor_enable_receiving(udev_mon);

            impl->udev = udev;
            impl->udev_mon = udev_mon;

            if (pthread_create(&impl->udev_thread, nullptr, _audio_device_udev_thread, impl.get()) != 0)
            {
                udev_monitor_unref(impl->udev_mon);
                udev_unref(udev);
                goto error;
            }
        }
        else
        {
            udev_unref(udev);
        }
    }
   #endif

    {
       #if AUDIO_BRIDGE_ASYNC
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM);
        pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
        sched_param sched = {};
        sched.sched_priority = dev->config.playback ? AUDIO_BRIDGE_PLAYBACK_THREAD_PRIORITY
                                                    : AUDIO_BRIDGE_CAPTURE_THREAD_PRIORITY;
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
       #else
        const uint8_t sampleSize = getSampleSizeFromFormat(impl->format);
        const uint8_t numChannels = impl->numChannels;
        const uint16_t periodSize = impl->periodSize;

        impl->rawBuffer = new uint8_t [periodSize * sampleSize * numChannels];
        impl->floatBuffers = new float* [numChannels];
        for (uint8_t c = 0; c < numChannels; ++c)
            impl->floatBuffers[c] = new float [periodSize];
       #endif
    }

    return impl.release();

error:
    snd_pcm_close(pcm);
    return nullptr;
}

void closeAudioDeviceImpl(AudioDevice::Impl* const impl)
{
    impl->closing = true;

   #if AUDIO_BRIDGE_ASYNC
    pthread_join(impl->thread, nullptr);
   #else
    for (uint8_t c = 0; c < impl->numChannels; ++c)
            delete[] impl->floatBuffers[c];
    delete[] impl->floatBuffers;
    delete[] impl->rawBuffer;
   #endif

   #if AUDIO_BRIDGE_UDEV
    if (impl->udev != nullptr)
    {
        pthread_join(impl->udev_thread, nullptr);

        if (impl->udev_mon != nullptr)
            udev_monitor_unref(impl->udev_mon);

        udev_unref(impl->udev);
    }
   #endif

    snd_pcm_close(impl->pcm);

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

bool runAudioDeviceCaptureSyncImpl(AudioDevice::Impl*, float* [], uint16_t)
{
    return false;
}

bool runAudioDevicePlaybackSyncImpl(AudioDevice::Impl*, float* [], uint16_t)
{
    return false;
}


// --------------------------------------------------------------------------------------------------------------------
