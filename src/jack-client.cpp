// SPDX-FileCopyrightText: 2021-2025 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-device.hpp"
#ifdef AUDIO_BRIDGE_ALSA
#include "audio-device-discovery.hpp"
#endif

#include <cstdlib>
#include <cstring>

#include <jack/jack.h>
#include <unistd.h>

struct ClientData;
static bool activate_jack_capture(ClientData* d);
static bool activate_jack_playback(ClientData* d);

struct ClientData {
    // pointer to audio device instance, the one doing heavy lifting
    AudioDevice* dev = nullptr;

    // whether we are doing playback, or otherwise capture
    bool playback = false;

    // whether the jack ports are in place and the audio device is active
    bool active = false;

    // number of audio channels to use (from audio device to jack ports)
    uint8_t numChannels = 0;

    struct {
        jack_client_t* client = nullptr;
        float** buffers = {};
        jack_port_t** ports = {};

        // whether the jack client is active
        bool running = true;
    } jack;

   #ifdef AUDIO_BRIDGE_INTERNAL_JACK_CLIENT
    char* deviceID = nullptr;
    pthread_t thread = {};

    static void* threadRunInternal(void* const arg)
    {
        ClientData* const d = static_cast<ClientData*>(arg);
        d->runInternal();
        return nullptr;
    }

    void runInternal()
    {
        bool needsToInitialise = numChannels != 0;
   #else
    void runExternal(const char* const deviceID)
    {
        bool needsToInitialise = true;
   #endif

        if (! needsToInitialise)
        {
            if (playback)
                activate_jack_playback(this);
            else
                activate_jack_capture(this);
        }

        while (jack.running)
        {
            if (dev != nullptr)
            {
                if (! active)
                {
                    closeAudioDevice(dev);
                    dev = nullptr;
                }

                usleep(250000); // 250ms
                continue;
            }

            const uint16_t bufferSize = jack_get_buffer_size(jack.client);
            const uint32_t sampleRate = jack_get_sample_rate(jack.client);
            // DEBUGPRINT("JACK %s, bufferSize %u, sampleRate %u",
            //            jack_get_client_name(jack.client), bufferSize, sampleRate);

            dev = initAudioDevice(deviceID, bufferSize, sampleRate, playback);

            if (dev != nullptr)
            {
                if (needsToInitialise)
                {
                    needsToInitialise = false;
                    if (numChannels != 0) {
                        DISTRHO_SAFE_ASSERT_UINT2_RETURN(numChannels == dev->hwconfig.numChannels,
                                                         numChannels,
                                                         dev->hwconfig.numChannels,);
                    }
                    numChannels = dev->hwconfig.numChannels;

                    if (playback)
                        activate_jack_playback(this);
                    else
                        activate_jack_capture(this);
                }

                active = true;
            }
            else
            {
                usleep(250000); // 250ms
                continue;
            }
        }
    }
};

static int jack_buffer_size(const unsigned frames, void* const arg)
{
    ClientData* const d = static_cast<ClientData*>(arg);

    // force device to be reopened
    if (d->dev != nullptr && d->active && d->dev->config.bufferSize != frames)
    {
        DEBUGPRINT("NEW jack_buffer_size %u", frames);
        d->active = false;
    }

    return 0;
}

static int jack_process(const unsigned frames, void* const arg)
{
    ClientData* const d = static_cast<ClientData*>(arg);

    for (uint8_t c = 0; c < d->numChannels; ++c)
        d->jack.buffers[c] = static_cast<float*>(jack_port_get_buffer(d->jack.ports[c], frames));

    if (d->dev != nullptr && d->active)
    {
        if (runAudioDevice(d->dev, d->jack.buffers, frames))
            return 0;

        d->active = false;
    }

    if (!d->playback)
    {
        for (uint8_t c = 0; c < d->numChannels; ++c)
            std::memset(d->jack.buffers[c], 0, sizeof(float)*frames);
    }

    return 0;
}

static ClientData* init_capture(jack_client_t* client = nullptr)
{
    if (client == nullptr)
    {
       #ifdef __MOD_DEVICES__
        setenv("JACK_INTERNAL_CLIENT_SYNC", ".", 1);
       #endif
        client = jack_client_open("audio-bridge-capture", JackNoStartServer, nullptr);
    }

    if (client == nullptr)
        return nullptr;

    ClientData* const d = new ClientData;
    d->jack.client = client;
    d->playback = false;
  #ifdef AUDIO_BRIDGE_INTERNAL_JACK_CLIENT
   #if defined(_MOD_DEVICE_DUO) || defined(_MOD_DEVICE_DUOX) || defined(_MOD_DEVICE_DWARF)
    d->numChannels = access("/data/enable-usb-audio-4x4", F_OK) == 0 ? 4 : 2;
   #elif defined(_DARKGLASS_DEVICE_PABLITO) && defined(AUDIO_BRIDGE_ALSA)
    d->numChannels = 2; // bluetooth audio
   #elif defined(_DARKGLASS_DEVICE_PABLITO)
    d->numChannels = 9; // usb audio
   #endif
  #endif

    jack_set_buffer_size_callback(client, jack_buffer_size, d);
    jack_set_process_callback(client, jack_process, d);

    return d;
}

static ClientData* init_playback(jack_client_t* client = nullptr)
{
    if (client == nullptr)
        client = jack_client_open("audio-bridge-playback", JackNoStartServer, nullptr);

    if (client == nullptr)
        return nullptr;

    ClientData* const d = new ClientData;
    d->jack.client = client;
    d->playback = true;
  #ifdef AUDIO_BRIDGE_INTERNAL_JACK_CLIENT
   #if defined(_MOD_DEVICE_DUO) || defined(_MOD_DEVICE_DUOX) || defined(_MOD_DEVICE_DWARF)
    d->numChannels = 4;
   #elif defined(_DARKGLASS_DEVICE_PABLITO)
    d->numChannels = 3; // usb audio
   #endif
  #endif

    jack_set_process_callback(client, jack_process, d);

    return d;
}

static bool activate_jack_capture(ClientData* const d)
{
    if (d->dev == nullptr || d->numChannels == 0)
        return false;

    const uint8_t channels = d->numChannels;
    jack_client_t* const client = d->jack.client;

    d->jack.buffers = new float* [channels];
    d->jack.ports = new jack_port_t* [channels];

    for (uint8_t c = 0; c < channels; ++c)
    {
        char name[16] = {};
        std::snprintf(name, sizeof(name)-1, "p%d", c + 1);
        d->jack.ports[c] = jack_port_register(client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput|JackPortIsTerminal, 0);
    }

    jack_activate(client);

  #ifdef AUDIO_BRIDGE_INTERNAL_JACK_CLIENT
   #if defined(_MOD_DEVICE_DUO) || defined(_MOD_DEVICE_DUOX) || defined(_MOD_DEVICE_DWARF)
    static constexpr const char* kPlaybackPorts[2] = {
       #ifdef _MOD_DEVICE_DWARF
        "system:playback_2",
        "system:playback_1",
       #else
        "system:playback_1",
        "system:playback_2",
       #endif
    };
    jack_connect(client, "mod-usbgadget_c:p1", kPlaybackPorts[0]);
    jack_connect(client, "mod-usbgadget_c:p1", "mod-peakmeter:in_3");
    jack_connect(client, "mod-usbgadget_c:p2", kPlaybackPorts[1]);
    jack_connect(client, "mod-usbgadget_c:p2", "mod-peakmeter:in_4");
    if (d->numChannels == 4) // optional 4x4 mode
    {
        jack_connect(client, "mod-usbgadget_c:p3", "mod-peakmeter:in_1");
        jack_connect(client, "mod-usbgadget_c:p3", "mod-host:in1");
        jack_connect(client, "mod-usbgadget_c:p4", "mod-peakmeter:in_2");
        jack_connect(client, "mod-usbgadget_c:p4", "mod-host:in2");
    }
   #elif defined(_DARKGLASS_DEVICE_PABLITO) && defined(AUDIO_BRIDGE_ALSA)
    // bluetooth audio
    if (jack_port_by_name(client, "effect_9992:inUSBL") != nullptr)
    {
        jack_connect(client, "bluetooth-capture:p1", "effect_9992:inUSBL");
        jack_connect(client, "bluetooth-capture:p2", "effect_9992:inUSBR");
    }
   #elif defined(_DARKGLASS_DEVICE_PABLITO)
    // usb audio
    jack_connect(client, "usbgadget-capture:p3", "system:playback_1");
    jack_connect(client, "usbgadget-capture:p4", "system:playback_2");
    jack_connect(client, "usbgadget-capture:p5", "system:playback_3");
    jack_connect(client, "usbgadget-capture:p6", "system:playback_4");
    jack_connect(client, "usbgadget-capture:p7", "system:playback_7");
    jack_connect(client, "usbgadget-capture:p8", "system:playback_8");
    jack_connect(client, "usbgadget-capture:p9", "anagram-input:usbin");
    if (jack_port_by_name(client, "effect_9992:inUSBL") != nullptr)
    {
        jack_connect(client, "usbgadget-capture:p1", "effect_9992:inUSBL");
        jack_connect(client, "usbgadget-capture:p2", "effect_9992:inUSBR");
    }
   #endif
  #endif

    return true;
}

static bool activate_jack_playback(ClientData* const d)
{
    if (d->dev == nullptr || d->numChannels == 0)
        return false;

    const uint8_t channels = d->numChannels;
    jack_client_t* const client = d->jack.client;

    d->jack.buffers = new float* [channels];
    d->jack.ports = new jack_port_t* [channels];

    for (uint8_t c = 0; c < channels; ++c)
    {
        char name[16] = {};
        std::snprintf(name, sizeof(name)-1, "p%d", c + 1);
        d->jack.ports[c] = jack_port_register(client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput|JackPortIsTerminal, 0);
    }

    jack_activate(client);

  #ifdef AUDIO_BRIDGE_INTERNAL_JACK_CLIENT
   #if defined(_MOD_DEVICE_DUO) || defined(_MOD_DEVICE_DUOX) || defined(_MOD_DEVICE_DWARF)
    jack_connect(client, "mod-host:out1", "mod-usbgadget_p:p1");
    jack_connect(client, "mod-host:out2", "mod-usbgadget_p:p2");
    jack_connect(client, "mod-monitor:out_1", "mod-usbgadget_p:p3");
    jack_connect(client, "mod-monitor:out_2", "mod-usbgadget_p:p4");
   #elif defined(_DARKGLASS_DEVICE_PABLITO)
    jack_connect(client, "mod-monitor:out_9", "usbgadget-playback:p1");
    jack_connect(client, "mod-monitor:out_10", "usbgadget-playback:p2");
    jack_connect(client, "anagram-input:usbout", "usbgadget-playback:p3");
   #endif
  #endif

    return true;
}

static void close(ClientData* const d)
{
    if (d->jack.client != nullptr)
    {
        jack_deactivate(d->jack.client);
        jack_client_close(d->jack.client);
    }

    if (d->dev != nullptr)
        closeAudioDevice(d->dev);

    delete[] d->jack.buffers;
    delete[] d->jack.ports;
    delete d;
}

#ifdef AUDIO_BRIDGE_INTERNAL_JACK_CLIENT
extern "C"
JACK_LIB_EXPORT
int jack_initialize(jack_client_t* client, const char* load_init);

extern "C"
JACK_LIB_EXPORT
void jack_finish(void* arg);

int jack_initialize(jack_client_t* const client, const char* const load_init)
{
   #if defined(_MOD_DEVICE_DUO) || defined(_MOD_DEVICE_DUOX) || defined(_MOD_DEVICE_DWARF)
    if (access("/data/enable-usb-multi-gadget", F_OK) != 0 ||
        access("/data/enable-usb-audio-gadget", F_OK) != 0)
        return 1;
   #endif

    if (const char* const ctype = std::strrchr(load_init, ' '))
    {
        const bool playback = std::strcmp(ctype + 1, "playback") == 0;

        if (ClientData* const d = playback ? init_playback(client) : init_capture(client))
        {
            const size_t devlen = ctype - load_init;
            d->deviceID = static_cast<char*>(std::malloc(devlen + 1));
            std::memcpy(d->deviceID, load_init, devlen);
            d->deviceID[devlen] = '\0';

            DEBUGPRINT("deviceID: %s, playback: %d", d->deviceID, d->playback);

            if (pthread_create(&d->thread, nullptr, ClientData::threadRunInternal, d) == 0)
                return 0;

            jack_finish(d);
        }
    }

    return 1;
}

void jack_finish(void* const arg)
{
    ClientData* const d = static_cast<ClientData*>(arg);

    if (d->jack.running)
    {
        d->jack.running = false;
        pthread_join(d->thread, nullptr);
    }

    d->jack.client = nullptr;
    std::free(d->deviceID);
    close(d);
}
#else
int main(int argc, const char* argv[])
{
    ClientData* d;
    const char* deviceID;

    if (argc > 2 && std::strcmp(argv[2], "capture") == 0)
    {
        deviceID = argv[1];
        d = init_capture();
        DEBUGPRINT("capture %p", d);
    }
    else if (argc > 1)
    {
        deviceID = argv[1];
        d = init_playback();
        DEBUGPRINT("playback %p", d);
    }
    else
    {
        std::vector<DeviceID> inputs, outputs;
        enumerateAudioDevices(inputs, outputs);

        deviceID = outputs[outputs.size() - 1].id.c_str();
        d = init_playback();
    }

    if (d == nullptr)
    {
    }

    d->runExternal(deviceID);

    close(d);
    cleanupAudioDevices();

    return 0;
}
#endif
