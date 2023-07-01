// SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-discovery.hpp"
#include "audio-process.hpp"

#include <jack/jack.h>
#include <cstring>
#include <unistd.h>

struct ClientData {
    DeviceAudio* dev = nullptr;
    jack_client_t* client = nullptr;
    jack_port_t* ports[2] = {};
    bool playback = false;
    bool running = true;

   #ifdef AWOOSB_INTERNAL_CLIENT
    char* deviceID = nullptr;
    pthread_t thread = {};

    void runInternal()
    {
        const uint16_t bufferSize = jack_get_buffer_size(client);
        const uint32_t sampleRate = jack_get_sample_rate(client);

        while (running && dev == nullptr)
        {
            dev = initDeviceAudio(deviceID, playback, bufferSize, sampleRate);
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
    void run(const char* const deviceID)
    {
        const uint16_t bufferSize = jack_get_buffer_size(client);
        const uint32_t sampleRate = jack_get_sample_rate(client);

        while (running)
        {
            if (dev == nullptr)
                dev = initDeviceAudio(deviceID, playback, bufferSize, sampleRate);

            sleep(1);
        }
    }
   #endif
};

static int jack_process(const unsigned frames, void* const arg)
{
    ClientData* const d = static_cast<ClientData*>(arg);
    float* buffers[2] = {
        (float*)jack_port_get_buffer(d->ports[0], frames),
        (float*)jack_port_get_buffer(d->ports[1], frames)
    };
    if (d->dev != nullptr)
    {
        runDeviceAudio(d->dev, buffers);
    }
    else if (!d->playback)
    {
        std::memset(buffers[0], 0, sizeof(float)*frames);
        std::memset(buffers[1], 0, sizeof(float)*frames);
    }
    return 0;
}

static void close(ClientData* const d);

static ClientData* init_capture(jack_client_t* client = nullptr)
{
    if (client == nullptr)
    {
       #ifdef __MOD_DEVICES__
        setenv("JACK_INTERNAL_CLIENT_SYNC", ".", 1);
       #endif
        client = jack_client_open("awoosb-capture", JackNoStartServer, nullptr);
    }

    if (client == nullptr)
        return nullptr;

    ClientData* const d = new ClientData;
    d->client = client;
    d->playback = false;
    d->ports[0] = jack_port_register(client, "USB_Audio_Capture_1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput|JackPortIsTerminal|JackPortIsPhysical, 0);
    d->ports[1] = jack_port_register(client, "USB_Audio_Capture_2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput|JackPortIsTerminal|JackPortIsPhysical, 0);

    jack_set_process_callback(client, jack_process, d);
    jack_activate(client);

  #ifdef __MOD_DEVICES__
   // #ifdef _MOD_DEVICE_DWARF
   //  jack_connect(client, "awoosb-capture:USB_Audio_Capture_1", "mod-host:in2");
   //  jack_connect(client, "awoosb-capture:USB_Audio_Capture_2", "mod-host:in1");
   // #else
   //  jack_connect(client, "awoosb-capture:USB_Audio_Capture_1", "mod-host:in1");
   //  jack_connect(client, "awoosb-capture:USB_Audio_Capture_2", "mod-host:in2");
   // #endif
  #else
    jack_connect(client, "awoosb-capture:p1", "awoosb-playback:p1");
    jack_connect(client, "awoosb-capture:p2", "awoosb-playback:p2");
  #endif

    return d;
}

static ClientData* init_playback(jack_client_t* client = nullptr)
{
    if (client == nullptr)
        client = jack_client_open("awoosb-playback", JackNoStartServer, nullptr);

    if (client == nullptr)
        return nullptr;

    ClientData* const d = new ClientData;
    d->client = client;
    d->playback = true;
    d->ports[0] = jack_port_register(client, "p1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput|JackPortIsTerminal, 0);
    d->ports[1] = jack_port_register(client, "p2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput|JackPortIsTerminal, 0);

    jack_set_process_callback(client, jack_process, d);
    jack_activate(client);

  #ifdef __MOD_DEVICES__
   // #ifdef _MOD_DEVICE_DWARF
   //  jack_connect(client, "mod-monitor:out_2", "mod-usbgadget_p:p1");
   //  jack_connect(client, "mod-monitor:out_1", "mod-usbgadget_p:p2");
   // #else
    jack_connect(client, "mod-monitor:out_1", "mod-usbgadget_p:p1");
    jack_connect(client, "mod-monitor:out_2", "mod-usbgadget_p:p2");
   // #endif
  #else
    jack_connect(client, "PulseAudio JACK Sink:front-left", "awoosb-playback:p1");
    jack_connect(client, "PulseAudio JACK Sink:front-right", "awoosb-playback:p2");
    jack_connect(client, "awoosb-capture:p1", "awoosb-playback:p1");
    jack_connect(client, "awoosb-capture:p2", "awoosb-playback:p2");
  #endif

    return d;
}

static void close(ClientData* const d)
{
    if (d->client != nullptr)
    {
        jack_deactivate(d->client);
        jack_client_close(d->client);
    }

    closeDeviceAudio(d->dev);
    delete d;
}

#ifdef AWOOSB_INTERNAL_CLIENT
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

    if (argc > 2)
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

    d->run(deviceID);
    close(d);

    cleanup();

    return 0;
}
#endif
