// SPDX-FileCopyrightText: 2021-2025 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-device.hpp"
#include "audio-device-discovery.hpp"

#include <jack/jack.h>
#include <cstring>
#include <unistd.h>

#if defined(__MOD_DEVICES__) && defined(AUDIO_BRIDGE_INTERNAL_JACK_CLIENT)
# define MOD_AUDIO_USB_BRIDGE
#endif

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
    } jack;

   #ifdef AUDIO_BRIDGE_INTERNAL_JACK_CLIENT
    char* deviceID = nullptr;
    pthread_t thread = {};

    void runInternal()
    {
        const uint16_t bufferSize = jack_get_buffer_size(jack.client);
        const uint32_t sampleRate = jack_get_sample_rate(jack.client);

        while (running && dev == nullptr)
        {
            dev = initAudioDevice(deviceID, playback, bufferSize, sampleRate);

            if (dev != nullptr)
            {
                numChannels = dev->hwconfig.numChannels;

                if (playback)
                    activate_jack_playback(this);
                else
                    activate_jack_capture(this);

                active = true;
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
    // whether the jack client is active
    bool running = true;

    void runExternal(const char* const deviceID)
    {
        bool needsToInitialise = true;

        while (running)
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
            DEBUGPRINT("JACK bufferSize %u, sampleRate %u", bufferSize, sampleRate);

            dev = initAudioDevice(deviceID, playback, bufferSize, sampleRate);

            if (dev != nullptr)
            {
                if (needsToInitialise)
                {
                    needsToInitialise = false;
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
   #endif
};

static int jack_buffer_size(const unsigned frames, void* const arg)
{
    ClientData* const d = static_cast<ClientData*>(arg);

    DEBUGPRINT("NEW jack_buffer_size %u", frames);

    // force device to be reopened
    if (d->dev != nullptr && d->active && d->dev->config.bufferSize != frames)
        d->active = false;

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

  #ifdef MOD_AUDIO_USB_BRIDGE
  #ifdef _MOD_DEVICE_GENERIC_AARCH64
    jack_connect(client, "mod-usbgadget_c:p1", "system:playback_1");
    jack_connect(client, "mod-usbgadget_c:p2", "system:playback_2");
    jack_connect(client, "mod-usbgadget_c:p3", "system:playback_3");
    jack_connect(client, "mod-usbgadget_c:p4", "system:playback_4");
    jack_connect(client, "mod-usbgadget_c:p5", "system:playback_5");
    jack_connect(client, "mod-usbgadget_c:p6", "system:playback_6");
    jack_connect(client, "mod-usbgadget_c:p7", "system:playback_7");
    jack_connect(client, "mod-usbgadget_c:p8", "system:playback_8");
    jack_connect(client, "system:capture_5", "mod-usbgadget_p:p1");
    jack_connect(client, "system:capture_6", "mod-usbgadget_p:p2");
    jack_connect(client, "system:capture_7", "mod-usbgadget_p:p3");
    jack_connect(client, "system:capture_8", "mod-usbgadget_p:p4");
  #else
   #ifdef _MOD_DEVICE_DWARF
    jack_connect(client, "mod-usbgadget_c:p1", "system:playback_2");
    jack_connect(client, "mod-usbgadget_c:p2", "system:playback_1");
   #else
    jack_connect(client, "mod-usbgadget_c:p1", "system:playback_1");
    jack_connect(client, "mod-usbgadget_c:p2", "system:playback_2");
   #endif
    jack_connect(client, "mod-usbgadget_c:p1", "mod-peakmeter:in_3");
    jack_connect(client, "mod-usbgadget_c:p2", "mod-peakmeter:in_4");
    // optional 4x4 mode
    jack_connect(client, "mod-usbgadget_c:p3", "mod-peakmeter:in_1");
    jack_connect(client, "mod-usbgadget_c:p3", "mod-host:in1");
    jack_connect(client, "mod-usbgadget_c:p4", "mod-peakmeter:in_2");
    jack_connect(client, "mod-usbgadget_c:p4", "mod-host:in2");
  #endif
  #else
    jack_connect(client, "audio-bridge-capture:p1", "audio-bridge-playback:p1");
    jack_connect(client, "audio-bridge-capture:p2", "audio-bridge-playback:p2");
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
