// SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-device-discovery.hpp"
#include "audio-device-init.hpp"

#include <jack/jack.h>
#include <cstring>
#include <unistd.h>

#if defined(__MOD_DEVICES__) && defined(AUDIO_BRIDGE_INTERNAL_JACK_CLIENT)
# define MOD_AUDIO_USB_BRIDGE
#endif

struct ClientData;
static bool activate_capture(ClientData* d);
static bool activate_playback(ClientData* d);

struct ClientData {
    DeviceAudio* dev = nullptr;
    jack_client_t* client = nullptr;
    float** buffers = {};
    jack_port_t** ports = {};
    uint8_t channels = 0;
    bool playback = false;
    bool running = true;

   #ifdef AUDIO_BRIDGE_INTERNAL_JACK_CLIENT
    char* deviceID = nullptr;
    pthread_t thread = {};

    void runInternal()
    {
        const uint16_t bufferSize = jack_get_buffer_size(client);
        const uint32_t sampleRate = jack_get_sample_rate(client);

        while (running && dev == nullptr)
        {
            dev = initDeviceAudio(deviceID, playback, bufferSize, sampleRate);

            if (dev != nullptr)
            {
                channels = dev->hwstatus.channels;

                if (playback)
                    activate_playback(this);
                else
                    activate_capture(this);

                break;
            }

            usleep(250000); // 250ms
        }

        running = false;
    }

    static void* threadRunInternal(void* const arg)
    {
        ClientData* const d = static_cast<ClientData*>(arg);
        d->runInternal();
        return nullptr;
    }
   #else
    void runExternal(const char* const deviceID)
    {
        const uint16_t bufferSize = jack_get_buffer_size(client);
        const uint32_t sampleRate = jack_get_sample_rate(client);

        while (running)
        {
            if (dev == nullptr)
            {
                dev = initDeviceAudio(deviceID, playback, bufferSize, sampleRate);

                if (dev != nullptr)
                {
                    channels = dev->hwstatus.channels;

                    if (playback)
                        activate_playback(this);
                    else
                        activate_capture(this);
                }
            }

            usleep(250000); // 250ms
        }
    }
   #endif
};

static int jack_process(const unsigned frames, void* const arg)
{
    ClientData* const d = static_cast<ClientData*>(arg);

    for (uint8_t c = 0; c < d->channels; ++c)
        d->buffers[c] = static_cast<float*>(jack_port_get_buffer(d->ports[c], frames));

    if (d->dev != nullptr)
    {
        runDeviceAudio(d->dev, d->buffers);
    }
    else if (!d->playback)
    {
        for (uint8_t c = 0; c < d->channels; ++c)
            std::memset(d->buffers[c], 0, sizeof(float)*frames);
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
    d->client = client;
    d->playback = false;

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
    d->client = client;
    d->playback = true;

    jack_set_process_callback(client, jack_process, d);

    return d;
}

static bool activate_capture(ClientData* const d)
{
    if (d->dev == nullptr || d->dev->hwstatus.channels == 0)
        return false;

    const uint8_t channels = d->dev->hwstatus.channels;
    jack_client_t* const client = d->client;

    d->buffers = new float* [channels];
    d->ports = new jack_port_t* [channels];

    for (uint8_t c = 0; c < channels; ++c)
    {
        char name[16] = {};
        std::snprintf(name, sizeof(name)-1, "p%d", c + 1);
        d->ports[c] = jack_port_register(client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput|JackPortIsTerminal, 0);
    }

    jack_activate(client);

  #ifdef MOD_AUDIO_USB_BRIDGE
   #ifdef _MOD_DEVICE_DWARF
    jack_connect(client, "mod-usbgadget_c:p1", "system:playback_2");
    jack_connect(client, "mod-usbgadget_c:p2", "system:playback_1");
   #else
    jack_connect(client, "mod-usbgadget_c:p1", "system:playback_1");
    jack_connect(client, "mod-usbgadget_c:p2", "system:playback_2");
   #endif
    jack_connect(client, "mod-usbgadget_c:p1", "mod-peakmeter:in_3");
    jack_connect(client, "mod-usbgadget_c:p2", "mod-peakmeter:in_4");
    // jack_connect(client, "mod-usbgadget_c:p3", "mod-host:in1");
    // jack_connect(client, "mod-usbgadget_c:p4", "mod-host:in2");
  #else
    jack_connect(client, "audio-bridge-capture:p1", "audio-bridge-playback:p1");
    jack_connect(client, "audio-bridge-capture:p2", "audio-bridge-playback:p2");
  #endif

    return true;
}

static bool activate_playback(ClientData* const d)
{
    if (d->dev == nullptr || d->dev->hwstatus.channels == 0)
        return false;

    const uint8_t channels = d->dev->hwstatus.channels;
    jack_client_t* const client = d->client;

    d->buffers = new float* [channels];
    d->ports = new jack_port_t* [channels];

    for (uint8_t c = 0; c < channels; ++c)
    {
        char name[16] = {};
        std::snprintf(name, sizeof(name)-1, "p%d", c + 1);
        d->ports[c] = jack_port_register(client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput|JackPortIsTerminal, 0);
    }

    jack_activate(client);

   #ifdef MOD_AUDIO_USB_BRIDGE
    jack_connect(client, "mod-host:out1", "mod-usbgadget_p:p1");
    jack_connect(client, "mod-host:out2", "mod-usbgadget_p:p2");
    jack_connect(client, "mod-monitor:out_1", "mod-usbgadget_p:p3");
    jack_connect(client, "mod-monitor:out_2", "mod-usbgadget_p:p4");
   #else
    jack_connect(client, "PulseAudio JACK Sink:front-left", "audio-bridge-playback:p1");
    jack_connect(client, "PulseAudio JACK Sink:front-right", "audio-bridge-playback:p2");
    jack_connect(client, "audio-bridge-capture:p1", "audio-bridge-playback:p1");
    jack_connect(client, "audio-bridge-capture:p2", "audio-bridge-playback:p2");
   #endif

    return true;
}

static void close(ClientData* const d)
{
    if (d->client != nullptr)
    {
        jack_deactivate(d->client);
        jack_client_close(d->client);
    }

    closeDeviceAudio(d->dev);

    delete[] d->buffers;
    delete[] d->ports;
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
   #ifdef __MOD_DEVICES__
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

            printf("deviceID %s || %d %d\n", d->deviceID, d->playback, playback);

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

    if (d->running)
    {
        d->running = false;
        pthread_join(d->thread, nullptr);
    }

    d->client = nullptr;
    std::free(d->deviceID);
    close(d);
}
#else
int main(int argc, const char* argv[])
{
    std::vector<DeviceID> inputs, outputs;
    enumerateSoundcards(inputs, outputs);

    ClientData* d;
    const char* deviceID;

    if (argc > 2 && std::strcmp(argv[2], "capture") == 0)
    {
        deviceID = argv[1];
        d = init_capture();
    }
    else if (argc > 1)
    {
        deviceID = argv[1];
        d = init_playback();
    }
    else
    {
        deviceID = outputs[outputs.size() - 1].id.c_str();
        d = init_playback();
    }

    if (d == nullptr)
    {
    }

    d->runExternal(deviceID);
    close(d);

    cleanup();

    return 0;
}
#endif
