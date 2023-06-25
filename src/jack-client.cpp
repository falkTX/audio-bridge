// SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-discovery.hpp"
#include "audio-process.hpp"

#include <jack/jack.h>
#include <unistd.h>

struct ClientData {
    DeviceAudio* dev;
    jack_port_t* ports[2];
};

static int jack_process(const unsigned frames, void* const arg)
{
    ClientData* const c = static_cast<ClientData*>(arg);
    float* buffers[2] = {
        (float*)jack_port_get_buffer(c->ports[0], frames),
        (float*)jack_port_get_buffer(c->ports[1], frames)
    };
    runDeviceAudio(c->dev, buffers);
    return 0;
}

int main(int argc, const char* argv[])
{
    std::vector<DeviceID> inputs, outputs;
    enumerateSoundcards(inputs, outputs);

    // TESTING
    if (jack_client_t* const c = jack_client_open("audio-test", JackNoStartServer, nullptr))
    {
        ClientData d;
        if ((d.dev = initDeviceAudio(argc > 1 ? argv[1] : outputs[outputs.size() - 1].id.c_str(),
                                     jack_get_buffer_size(c),
                                     jack_get_sample_rate(c))) == nullptr)
            goto end;

        d.ports[0] = jack_port_register(c, "p1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
        d.ports[1] = jack_port_register(c, "p2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
        jack_set_process_callback(c, jack_process, &d);
        jack_activate(c);
        jack_connect(c, "mod-monitor:out_1", "audio-test:p1");
        jack_connect(c, "mod-monitor:out_2", "audio-test:p2");

        while (true) sleep(1);

        jack_deactivate(c);
        closeDeviceAudio(d.dev);

    end:
        jack_client_close(c);
    }

    cleanup();

    return 0;
}
