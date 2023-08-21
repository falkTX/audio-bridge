// SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-discovery.hpp"
#include "audio-process.hpp"

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

struct PluginData {
    DeviceAudio* dev = nullptr;
    uint16_t bufferSize = 0;
    uint32_t sampleRate = 0;
    bool playback = false;
    bool activated = false;

    struct Features {
        const LV2_URID_Map* uridMap;
        const LV2_Worker_Schedule* workerSchedule;

        Features(const LV2_Feature* const* const features)
            : uridMap(static_cast<const LV2_URID_Map*>(lv2_features_data(features, LV2_URID__map))),
              workerSchedule(static_cast<const LV2_Worker_Schedule*>(lv2_features_data(features, LV2_WORKER__schedule))) {}
    } features;

    struct Buffers {
        float** pointers;
        float* dummy;
    } buffers = {};

    struct Ports {
        float* audio[2];
        const LV2_Atom_Sequence* control;
        LV2_Atom_Sequence* notify;
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

    PluginData(const uint32_t sampleRate, const LV2_Feature* const* const featuresPtr)
        : sampleRate(sampleRate),
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

        if (buffers.pointers != ports.audio)
            delete[] buffers.pointers;
    }

    // TESTING
    void testing()
    {
        if (bufferSize == 0)
            return;

        std::vector<DeviceID> inputs, outputs;
        enumerateSoundcards(inputs, outputs);

        dev = initDeviceAudio(playback ? outputs[outputs.size() - 1].id.c_str()
                                       : inputs[inputs.size() - 1].id.c_str(),
                              playback, bufferSize, sampleRate);

        if (dev != nullptr)
        {
            printf("TESTING %u %u | %u %u %p\n", bufferSize, sampleRate, dev->channels, dev->periods, dev);

            if (dev->channels <= 2)
            {
                buffers.pointers = ports.audio;
                buffers.dummy = nullptr;
            }
            else
            {
                buffers.pointers = new float*[dev->channels];
                buffers.dummy = new float[bufferSize];
                std::memset(buffers.dummy, 0, sizeof(float) * bufferSize);

                buffers.pointers[0] = ports.audio[0];
                buffers.pointers[1] = ports.audio[1];

                for (uint8_t c=2; c<dev->channels; ++c)
                    buffers.pointers[c] = buffers.dummy;
            }
        }
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
            ports.audio[index] = static_cast<float*>(data);
            if (buffers.pointers != ports.audio)
                buffers.pointers[index] = ports.audio[index];
            break;
        case 2:
            ports.control = static_cast<const LV2_Atom_Sequence*>(data);
            break;
        case 3:
            ports.notify = static_cast<LV2_Atom_Sequence*>(data);
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

        if (dev != nullptr)
        {
            runDeviceAudio(dev, ports.audio);
        }
        else if (!playback)
        {
            std::memset(ports.audio[0], 0, sizeof(float)*frames);
            std::memset(ports.audio[1], 0, sizeof(float)*frames);
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
                    bufferSize = *(const int32_t*)options[i].value;
                }
                break;
            }
        }

        return LV2_OPTIONS_SUCCESS;
    }

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

    LV2_Worker_Status work(const LV2_Worker_Respond_Function respond,
                           const LV2_Worker_Respond_Handle handle,
                           const uint32_t size,
                           const void* const data)
    {
        return LV2_WORKER_ERR_UNKNOWN;
    }

    LV2_Worker_Status workResponse(const uint32_t size, const void* const data)
    {
        return LV2_WORKER_ERR_UNKNOWN;
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
        p->testing();
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
        p->testing();
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
    static const LV2_Options_Interface options = {
        lv2_options_get, lv2_options_set
    };
    static const LV2_State_Interface state = {
        lv2_state_save, lv2_state_retrieve
    };
    static const LV2_Worker_Interface worker = {
        lv2_work, lv2_work_response, nullptr
    };

    if (std::strcmp(uri, LV2_OPTIONS__interface) == 0)
        return &options;
    if (std::strcmp(uri, LV2_STATE__interface) == 0)
        return &state;
    if (std::strcmp(uri, LV2_WORKER__interface) == 0)
        return &worker;

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
