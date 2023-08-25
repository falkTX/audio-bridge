// SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-device-discovery.hpp"
#include "audio-device-init.hpp"

#include <lv2/core/lv2.h>
#include <lv2/core/lv2_util.h>
#include <lv2/atom/atom.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/options/options.h>
#include <lv2/patch/patch.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#include <cmath>
#include <cstring>

static constexpr const uint8_t kMaxIO = 32;
static constexpr const uint8_t kRingBufferDataFactor = sizeof(float) * 16;

typedef std::vector<DeviceID>::const_reverse_iterator cri;

enum {
    kWorkerTryLoadingDevice = 1,
    kWorkerDestroyDevice
};

struct WorkerDevice {
    uint32_t r;
    DeviceAudio* dev;
};

struct PluginData {
    DeviceAudio* dev = nullptr;
    uint16_t bufferSize = 0;
    uint32_t sampleRate = 0;
    uint32_t maxRingBufferSize = 0;
    bool playback = false;
    bool activated = false;
   #if 1 // def __MOD_DEVICES__
    uint32_t numIdleSamples = 0;
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

    struct Ports {
        const LV2_Atom_Sequence* control;
        LV2_Atom_Sequence* notify;
        float* status[9];
    } ports = {};

    struct URIs {
        LV2_URID atom_Int;
        LV2_URID atom_Object;
        LV2_URID atom_Sequence;
        // LV2_URID atom_URID;
        LV2_URID bufsize_maxBlockLength;
        LV2_URID patch_Get;
        LV2_URID patch_Set;
        // LV2_URID patch_property;
        // LV2_URID patch_value;

        URIs(const LV2_URID_Map* const uridMap)
            : atom_Int(uridMap->map(uridMap->handle, LV2_ATOM__Int)),
              atom_Object(uridMap->map(uridMap->handle, LV2_ATOM__Object)),
              atom_Sequence(uridMap->map(uridMap->handle, LV2_ATOM__Sequence)),
              bufsize_maxBlockLength(uridMap->map(uridMap->handle, LV2_BUF_SIZE__maxBlockLength)),
              patch_Get(uridMap->map(uridMap->handle, LV2_PATCH__Get)),
              patch_Set(uridMap->map(uridMap->handle, LV2_PATCH__Set)) {}
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
        if (dev != nullptr)
            closeDeviceAudio(dev);

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
        case 0:
        case 1:
            buffers.pointers[index] = static_cast<float*>(data);
            break;
        case 2:
            ports.control = static_cast<const LV2_Atom_Sequence*>(data);
            break;
        case 3:
            ports.notify = static_cast<LV2_Atom_Sequence*>(data);
            break;
        case 4:
        case 5:
        case 6:
        case 7:
        case 8:
        case 9:
        case 10:
        case 11:
        case 12:
            ports.status[index - 4] = static_cast<float*>(data);
            break;
        }
    }

    void run(const uint32_t frames)
    {
        // prepare notify/output port
        if (LV2_Atom_Sequence* const notify = ports.notify)
        {
            // const uint32_t capacity = notify->atom.size;

            notify->atom.size = sizeof(LV2_Atom_Sequence_Body);
            notify->atom.type = uris.atom_Sequence;
            notify->body.unit = 0;
            notify->body.pad  = 0;
        }

#if 0
        // handle control/input events
        if (const LV2_Atom_Sequence* const control = ports.control)
        {
            struct Atom_Event {
                int64_t frame;
                union {
                    LV2_Atom_Object object;
                    uint32_t size;
                };
            };
            union Atom_Iter {
                const void* voidptr;
                const uint8_t* u8ptr;
                const Atom_Event* event;
            } iter;
            iter.voidptr = &control->body + 1;

            for (const Atom_Iter end = { iter.u8ptr + ports.control->atom.size - sizeof(LV2_Atom_Sequence_Body) };
                 iter.u8ptr < end.u8ptr;
                 iter.u8ptr += sizeof(LV2_Atom_Event) + ((iter.event->size + 7u) & ~7u))
            {
                const Atom_Event* const event = iter.event;

                if (event->object.atom.type != uris.atom_Object)
                    continue;

                if (event->object.body.otype == uris.patch_Get)
                {
                }
                else if (event->object.body.otype == uris.patch_Set)
                {
                }
            }
        }
#endif

        if (dev != nullptr && ! runDeviceAudio(dev, buffers.pointers))
        {
            DeviceAudio* const olddev = dev;
            dev = nullptr;

            const WorkerDevice r = { kWorkerDestroyDevice, olddev };
            features.workerSchedule->schedule_work(features.workerSchedule->handle, sizeof(r), &r);
        }

        if (dev != nullptr)
        {
            const uint8_t hints = dev->hints;
            *ports.status[0] = hints & kDeviceInitializing ? 1.f : hints & kDeviceStarting ? 2.f : 3.f;
            *ports.status[1] = dev->hwstatus.channels;
            *ports.status[2] = dev->hwstatus.periods;
            *ports.status[3] = dev->hwstatus.periodSize;
            *ports.status[4] = dev->hwstatus.bufferSize;
            *ports.status[5] = dev->timestamps.ratio;
            *ports.status[6] = dev->balance.ratio;
            *ports.status[7] = dev->timestamps.ratio * dev->balance.ratio;
            *ports.status[8] = static_cast<float>(dev->ringbuffers[0].getReadableDataSize() / kRingBufferDataFactor)
                             / static_cast<float>(maxRingBufferSize);
        }
        else
        {
            *ports.status[0] = *ports.status[1] = *ports.status[2] = *ports.status[3] = 0.f;
            *ports.status[4] = *ports.status[5] = *ports.status[6] = *ports.status[7] = *ports.status[8] = 0.f;

            if (!playback)
            {
                std::memset(buffers.pointers[0], 0, sizeof(float)*frames);
                std::memset(buffers.pointers[1], 0, sizeof(float)*frames);
            }

           #if 1 // def __MOD_DEVICES__
            numIdleSamples += frames;

            if (numIdleSamples >= sampleRate)
            {
                numIdleSamples = 0;
                const uint32_t r = kWorkerTryLoadingDevice;
                features.workerSchedule->schedule_work(features.workerSchedule->handle, sizeof(r), &r);
            }
           #endif
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
                if (activated)
                {
                    // TODO stop audio until pending worker
                }
                else
                {
                    setBufferSize(*static_cast<const int32_t*>(options[i].value));
                }
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

   #if 0 // ndef __MOD_DEVICES__
    LV2_State_Status stateSave(const LV2_State_Store_Function store,
                               const LV2_State_Handle handle,
                               const uint32_t flags,
                               const LV2_Feature* const* const features)
    {
        return LV2_STATE_ERR_UNKNOWN;
    }

    LV2_State_Status stateRetrieve(const LV2_State_Retrieve_Function retrieve,
                                   const LV2_State_Handle handle,
                                   const uint32_t flags,
                                   const LV2_Feature* const* const features)
    {
        return LV2_STATE_ERR_UNKNOWN;
    }
   #endif

    LV2_Worker_Status work(const LV2_Worker_Respond_Function respond,
                           const LV2_Worker_Respond_Handle handle,
                           const uint32_t size,
                           const void* const data)
    {
        DISTRHO_SAFE_ASSERT_RETURN(size >= sizeof(uint32_t), LV2_WORKER_ERR_UNKNOWN);
        DISTRHO_SAFE_ASSERT_RETURN(bufferSize != 0, LV2_WORKER_ERR_UNKNOWN);

        const uint32_t r = *static_cast<const uint32_t*>(data);

        switch (r)
        {
        case kWorkerTryLoadingDevice:
        {
            std::vector<DeviceID> inputs, outputs;
            enumerateSoundcards(inputs, outputs);

            const std::vector<DeviceID>& devices(playback ? outputs : inputs);

            if (devices.size() == 0)
                return LV2_WORKER_SUCCESS;

            DeviceAudio* devptr = nullptr;

            for (cri it = devices.rbegin(); it != devices.rend() && devptr == nullptr; ++it)
                devptr = initDeviceAudio((*it).id.c_str(), playback, bufferSize, sampleRate);

            if (devptr == nullptr)
                return LV2_WORKER_SUCCESS;

            printf("TESTING %u %u | %u %u %p | %s %s\n",
                   bufferSize, sampleRate, devptr->hwstatus.channels, devptr->hwstatus.periods, devptr,
                   devices[devices.size() - 1].id.c_str(),
                   devices[devices.size() - 1].name.c_str());

            respond(handle, sizeof(devptr), &devptr);
            break;
        }
        case kWorkerDestroyDevice:
        {
            DeviceAudio* const olddev = static_cast<const WorkerDevice*>(data)->dev;
            closeDeviceAudio(olddev);
            break;
        }
        }

        return LV2_WORKER_SUCCESS;
    }

    LV2_Worker_Status workResponse(const uint32_t size, const void* const data)
    {
        DISTRHO_SAFE_ASSERT_RETURN(size == sizeof(DeviceAudio*), LV2_WORKER_ERR_UNKNOWN);
        DISTRHO_SAFE_ASSERT_RETURN(bufferSize != 0, LV2_WORKER_ERR_UNKNOWN);

        DeviceAudio* const newdev = *static_cast<DeviceAudio* const*>(data);
        DeviceAudio* const olddev = dev;

        dev = newdev;
        maxRingBufferSize = newdev->ringbuffers[0].getSize() / kRingBufferDataFactor;

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
    return p;
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

#if 0 // ndef __MOD_DEVICES__
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

   #if 0 // ndef __MOD_DEVICES__
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
