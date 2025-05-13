// SPDX-FileCopyrightText: 2021-2025 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio-device.hpp"
#include "audio-device-discovery.hpp"

#include <lv2/core/lv2.h>
#include <lv2/core/lv2_util.h>
#include <lv2/atom/atom.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/options/options.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>
#ifndef __MOD_DEVICES__
#include <lv2/state/state.h>
#endif

#include <cmath>
#include <cstring>

static constexpr const uint8_t kMaxIO = 32;

typedef std::vector<DeviceID>::const_reverse_iterator cri;

enum {
    kWorkerLoadLastAvailableDevice = 1,
   #ifndef __MOD_DEVICES__
    kWorkerLoadDeviceWithKnownId,
   #endif
    kWorkerDestroyDevice
};

enum {
    kControlEnabled = 0,
    kControlStats,
    kControlState,
    kControlNumChannels,
    kControlNumPeriods,
    kControlPeriodSize,
    kControlFullBufferSize,
    kControlRatioActive,
    kControlRatioFiltered,
    kControlCount,
};

struct WorkerDevice {
    uint32_t r;
    AudioDevice* dev;
};

struct PluginData {
    AudioDevice* dev = nullptr;
    uint16_t bufferSize = 0;
    uint32_t sampleRate = 0;
    bool playback = false;
    bool activated = false;
    bool enabled = true;
    uint32_t numSamplesUntilWorkerIdle = 0;
   #ifndef __MOD_DEVICES__
    char* deviceID = nullptr;
   #endif

    struct Features {
        const LV2_URID_Map* const uridMap;
        const LV2_Worker_Schedule* const workerSchedule;

        Features(const LV2_Feature* const* const features)
            : uridMap(static_cast<const LV2_URID_Map*>(lv2_features_data(features, LV2_URID__map))),
              workerSchedule(static_cast<const LV2_Worker_Schedule*>(lv2_features_data(features, LV2_WORKER__schedule))) {}
    } features;

    struct Buffers {
        float* pointers[kMaxIO];
        float* dummy;
    } buffers = {};

    float* controlports[kControlCount] = {};

    struct URIs {
        const LV2_URID atom_Int;
        const LV2_URID bufsize_maxBlockLength;
       #ifndef __MOD_DEVICES__
        const LV2_URID atom_String;
        const LV2_URID deviceid;
       #endif

        URIs(const LV2_URID_Map* const uridMap)
            : atom_Int(uridMap->map(uridMap->handle, LV2_ATOM__Int)),
              bufsize_maxBlockLength(uridMap->map(uridMap->handle, LV2_BUF_SIZE__maxBlockLength))
           #ifndef __MOD_DEVICES__
            , atom_String(uridMap->map(uridMap->handle, LV2_ATOM__String))
            , deviceid(uridMap->map(uridMap->handle,"https://falktx.com/plugins/audio-bridge#deviceid"))
           #endif
        {}
    } uris;

    PluginData(const uint32_t sampleRate_, const LV2_Feature* const* const featuresPtr)
        : sampleRate(sampleRate_),
          features(featuresPtr),
          uris(features.uridMap)
    {
        // set initial options
        optionsSet(static_cast<const LV2_Options_Option*>(lv2_features_data(featuresPtr, LV2_OPTIONS__options)));
    }

    ~PluginData()
    {
        DISTRHO_SAFE_ASSERT(! activated);

        if (dev != nullptr)
            closeAudioDevice(dev);

        delete[] buffers.dummy;
    }

    void activate()
    {
        activated = true;
    }

    void deactivate()
    {
        activated = false;
    }

    void connectPort(const uint32_t index, void* const data)
    {
        switch (index)
        {
        case 0 ... 1:
            buffers.pointers[index] = static_cast<float*>(data);
            break;
        case 2 ... kControlCount + 2:
            controlports[index - 2] = static_cast<float*>(data);
            break;
        }
    }

    void run(const uint32_t frames)
    {
        enabled = *controlports[kControlEnabled] > 0.5f;

        if (dev != nullptr)
        {
            dev->enabled = enabled;

            if (! runAudioDevice(dev, buffers.pointers, frames))
            {
                AudioDevice* const olddev = dev;
                dev = nullptr;

                const WorkerDevice r = { kWorkerDestroyDevice, olddev };
                features.workerSchedule->schedule_work(features.workerSchedule->handle, sizeof(r), &r);
            }
        }

        if (dev != nullptr)
        {
           #if AUDIO_BRIDGE_ASYNC
            *controlports[kControlState] = dev->proc.state + 1;
           #else
            *controlports[kControlState] = kDeviceRunning + 1;
           #endif
            *controlports[kControlNumChannels] = dev->hwconfig.numChannels;
            *controlports[kControlNumPeriods] = dev->hwconfig.numPeriods;
            *controlports[kControlPeriodSize] = dev->hwconfig.periodSize;
            *controlports[kControlFullBufferSize] = dev->hwconfig.fullBufferSize;

            if (*controlports[kControlStats] > 0.5f)
            {
               #if AUDIO_BRIDGE_UDEV
                const double balratio = 1.0 - static_cast<double>(dev->stats.ppm) / 1000000.0;
                *controlports[kControlRatioActive] = *controlports[kControlRatioFiltered] = balratio;
               #elif AUDIO_BRIDGE_ASYNC
                *controlports[kControlRatioActive] = clamp_ratio(
                    dev->proc.ringbuffer->getNumReadableSamples() / kRingBufferDataFactor / dev->stats.rbFillTarget);
                *controlports[kControlRatioFiltered] = dev->stats.rbRatio;
               #else
                *controlports[kControlRatioActive] = *controlports[kControlRatioFiltered] = 1.f;
               #endif
            }
            else
            {
                *controlports[kControlRatioActive] = *controlports[kControlRatioFiltered] = 0.f;
            }
        }
        else
        {
            *controlports[kControlState] = 0.f;
            *controlports[kControlNumChannels] = 0.f;
            *controlports[kControlNumPeriods] = 0.f;
            *controlports[kControlPeriodSize] = 0.f;
            *controlports[kControlFullBufferSize] = 0.f;
            *controlports[kControlRatioActive] = 0.f;
            *controlports[kControlRatioFiltered] = 0.f;

            if (!playback)
            {
                std::memset(buffers.pointers[0], 0, sizeof(float)*frames);
                std::memset(buffers.pointers[1], 0, sizeof(float)*frames);
            }

            numSamplesUntilWorkerIdle += frames;

            if (numSamplesUntilWorkerIdle >= sampleRate)
            {
                numSamplesUntilWorkerIdle = 0;
                const uint32_t r = kWorkerLoadLastAvailableDevice;
                features.workerSchedule->schedule_work(features.workerSchedule->handle, sizeof(r), &r);
            }
        }
    }

    uint32_t optionsGet(LV2_Options_Option*)
    {
        return LV2_OPTIONS_ERR_UNKNOWN;
    }

    uint32_t optionsSet(const LV2_Options_Option* const options)
    {
        for (size_t i=0; options[i].key && options[i].type; ++i)
        {
            if (options[i].key == uris.bufsize_maxBlockLength && options[i].type == uris.atom_Int)
            {
                DISTRHO_SAFE_ASSERT(! activated);
                setBufferSize(*static_cast<const int32_t*>(options[i].value));
                break;
            }
        }

        return LV2_OPTIONS_SUCCESS;
    }

    void setBufferSize(const uint32_t newBufferSize)
    {
        if (bufferSize == newBufferSize)
            return;

        bufferSize = newBufferSize;

        delete[] buffers.dummy;
        buffers.dummy = new float[newBufferSize];
        std::memset(buffers.dummy, 0, sizeof(float) * bufferSize);

        for (uint8_t i=2; i<kMaxIO; ++i)
            buffers.pointers[i] = buffers.dummy;
    }

   #ifndef __MOD_DEVICES__
    LV2_State_Status stateSave(const LV2_State_Store_Function store,
                               const LV2_State_Handle handle,
                               uint32_t, const LV2_Feature* const*)
    {
        if (dev != nullptr && dev->config.deviceID)
        {
            store(handle, uris.deviceid, dev->config.deviceID, std::strlen(dev->config.deviceID) + 1,
                  uris.atom_String, LV2_STATE_IS_POD|LV2_STATE_IS_PORTABLE);
        }
        else if (deviceID != nullptr)
        {
            store(handle, uris.deviceid, deviceID, std::strlen(deviceID) + 1,
                  uris.atom_String, LV2_STATE_IS_POD|LV2_STATE_IS_PORTABLE);
        }
        else
        {
            store(handle, uris.deviceid, "", 1,
                  uris.atom_String, LV2_STATE_IS_POD|LV2_STATE_IS_PORTABLE);
        }

        return LV2_STATE_SUCCESS;
    }

    LV2_State_Status stateRetrieve(const LV2_State_Retrieve_Function retrieve,
                                   const LV2_State_Handle handle,
                                   uint32_t, const LV2_Feature* const*)
    {
        size_t   size  = 0;
        uint32_t type  = 0;
        uint32_t flags = 0;
        const void* const data = retrieve(handle, uris.deviceid, &size, &type, &flags);
        DISTRHO_SAFE_ASSERT_RETURN(data != nullptr, LV2_STATE_ERR_NO_PROPERTY);
        DISTRHO_SAFE_ASSERT_RETURN(size != 0, LV2_STATE_ERR_NO_PROPERTY);
        DISTRHO_SAFE_ASSERT_RETURN(type == uris.atom_String, LV2_STATE_ERR_NO_PROPERTY);

        std::free(deviceID);
        deviceID = strdup(static_cast<const char*>(data));

        void* const msg = std::malloc(sizeof(uint32_t) + size);
        DISTRHO_SAFE_ASSERT_RETURN(msg != nullptr, LV2_STATE_ERR_NO_SPACE);

        *static_cast<uint32_t*>(msg) = kWorkerLoadDeviceWithKnownId;
        std::memcpy(static_cast<uint32_t*>(msg) + 1, data, size);

        features.workerSchedule->schedule_work(features.workerSchedule->handle, sizeof(uint32_t) + size, msg);

        std::free(msg);

        return LV2_STATE_SUCCESS;
    }
   #endif

    LV2_Worker_Status work(const LV2_Worker_Respond_Function respond,
                           const LV2_Worker_Respond_Handle handle,
                           const uint32_t size,
                           const void* const data)
    {
        DISTRHO_SAFE_ASSERT_RETURN(size >= sizeof(uint32_t), LV2_WORKER_ERR_UNKNOWN);

        const uint32_t* const udata = static_cast<const uint32_t*>(data);

        switch (*udata)
        {
        case kWorkerLoadLastAvailableDevice:
        {
            AudioDevice* devptr = nullptr;

           #ifndef __MOD_DEVICES__
            if (deviceID != nullptr)
            {
                devptr = initAudioDevice(deviceID, playback, bufferSize, sampleRate, enabled);
            }
            else
           #endif
            {
                std::vector<DeviceID> inputs, outputs;
                enumerateAudioDevices(inputs, outputs);

                const std::vector<DeviceID>& devices(playback ? outputs : inputs);

                for (cri it = devices.rbegin(); it != devices.rend() && devptr == nullptr; ++it)
                    devptr = initAudioDevice((*it).id.c_str(), playback, bufferSize, sampleRate);
            }

            if (devptr == nullptr)
                return LV2_WORKER_SUCCESS;

            respond(handle, sizeof(void*), &devptr);
            break;
        }
       #ifndef __MOD_DEVICES__
        case kWorkerLoadDeviceWithKnownId:
        {
            const char* const nextDeviceID = reinterpret_cast<const char*>(udata + 1);
            AudioDevice* const devptr = nextDeviceID[0] != '\0'
                                      ? initAudioDevice(nextDeviceID, playback, bufferSize, sampleRate)
                                      : nullptr;
            respond(handle, sizeof(void*), &devptr);
            break;
        }
       #endif
        case kWorkerDestroyDevice:
        {
            const WorkerDevice* const r = static_cast<const WorkerDevice*>(data);
            closeAudioDevice(r->dev);
            break;
        }
        }

        return LV2_WORKER_SUCCESS;
    }

    LV2_Worker_Status workResponse(const uint32_t size, const void* const data)
    {
        DISTRHO_SAFE_ASSERT_RETURN(size == sizeof(void*), LV2_WORKER_ERR_UNKNOWN);

        AudioDevice* const newdev = *static_cast<AudioDevice* const*>(data);
        AudioDevice* const olddev = dev;

        dev = newdev;
        numSamplesUntilWorkerIdle = 0;

        if (olddev == nullptr)
            return LV2_WORKER_SUCCESS;

        const WorkerDevice r = { kWorkerDestroyDevice, olddev };
        return features.workerSchedule->schedule_work(features.workerSchedule->handle, sizeof(r), &r);
    }
};

PluginData* lv2_instantiate(const double sampleRate, const LV2_Feature* const* const features)
{
    if (std::fmod(sampleRate, 1.0) != 0.0)
        return nullptr;

    PluginData* const p = new PluginData(sampleRate, features);
    if (p->bufferSize != 0)
        return p;

    delete p;
    return nullptr;
}

LV2_Handle lv2_instantiate_capture(const LV2_Descriptor*,
                                   const double sampleRate,
                                   const char*,
                                   const LV2_Feature* const* const features)
{
    if (PluginData* const p = lv2_instantiate(sampleRate, features))
    {
        p->playback = false;
        return p;
    }
    return nullptr;
}

LV2_Handle lv2_instantiate_playback(const LV2_Descriptor*,
                                    const double sampleRate,
                                    const char*,
                                    const LV2_Feature* const* const features)
{
    if (PluginData* const p = lv2_instantiate(sampleRate, features))
    {
        p->playback = true;
        return p;
    }
    return nullptr;
}

void lv2_connect_port(const LV2_Handle handle, const uint32_t index, void* const data)
{
    static_cast<PluginData*>(handle)->connectPort(index, data);
}

void lv2_activate(const LV2_Handle handle)
{
    static_cast<PluginData*>(handle)->activate();
}

void lv2_run(const LV2_Handle handle, const uint32_t frames)
{
    static_cast<PluginData*>(handle)->run(frames);
}

void lv2_deactivate(const LV2_Handle handle)
{
    static_cast<PluginData*>(handle)->deactivate();
}

void lv2_cleanup(const LV2_Handle handle)
{
    delete static_cast<PluginData*>(handle);
}

uint32_t lv2_options_get(const LV2_Handle handle, LV2_Options_Option* const options)
{
    return static_cast<PluginData*>(handle)->optionsGet(options);
}

uint32_t lv2_options_set(const LV2_Handle handle, const LV2_Options_Option* const options)
{
    return static_cast<PluginData*>(handle)->optionsSet(options);
}

#ifndef __MOD_DEVICES__
LV2_State_Status lv2_state_save(const LV2_Handle handle,
                                const LV2_State_Store_Function store,
                                const LV2_State_Handle shandle,
                                const uint32_t flags,
                                const LV2_Feature* const* const features)
{
    return static_cast<PluginData*>(handle)->stateSave(store, shandle, flags, features);
}

LV2_State_Status lv2_state_retrieve(const LV2_Handle handle,
                                    const LV2_State_Retrieve_Function retrieve,
                                    const LV2_State_Handle shandle,
                                    const uint32_t flags,
                                    const LV2_Feature* const* const features)
{
    return static_cast<PluginData*>(handle)->stateRetrieve(retrieve, shandle, flags, features);
}
#endif

LV2_Worker_Status lv2_work(const LV2_Handle handle,
                           const LV2_Worker_Respond_Function respond,
                           const LV2_Worker_Respond_Handle whandle,
                           const uint32_t size,
                           const void* const data)
{
    return static_cast<PluginData*>(handle)->work(respond, whandle, size, data);
}

LV2_Worker_Status lv2_work_response(const LV2_Handle handle, const uint32_t size, const void* const data)
{
    return static_cast<PluginData*>(handle)->workResponse(size, data);
}

const void* lv2_extension_data(const char* const uri)
{
    if (std::strcmp(uri, LV2_OPTIONS__interface) == 0)
    {
        static const LV2_Options_Interface options = {
            lv2_options_get, lv2_options_set
        };
        return &options;
    }

   #ifndef __MOD_DEVICES__
    if (std::strcmp(uri, LV2_STATE__interface) == 0)
    {
        static const LV2_State_Interface state = {
            lv2_state_save, lv2_state_retrieve
        };
        return &state;
    }
   #endif

    if (std::strcmp(uri, LV2_WORKER__interface) == 0)
    {
        static const LV2_Worker_Interface worker = {
            lv2_work, lv2_work_response, nullptr
        };
        return &worker;
    }

    return nullptr;
}

LV2_SYMBOL_EXPORT
const LV2_Descriptor* lv2_descriptor(const uint32_t index)
{
    static const LV2_Descriptor descriptor_capture = {
        "https://falktx.com/plugins/audio-bridge#capture",
        lv2_instantiate_capture,
        lv2_connect_port,
        lv2_activate,
        lv2_run,
        lv2_deactivate,
        lv2_cleanup,
        lv2_extension_data
    };
    static const LV2_Descriptor descriptor_playback = {
        "https://falktx.com/plugins/audio-bridge#playback",
        lv2_instantiate_playback,
        lv2_connect_port,
        lv2_activate,
        lv2_run,
        lv2_deactivate,
        lv2_cleanup,
        lv2_extension_data
    };

    switch (index)
    {
    case 0:
        return &descriptor_capture;
    case 1:
        return &descriptor_playback;
    default:
        return nullptr;
    }
}
