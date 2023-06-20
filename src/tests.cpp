// SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-discovery.hpp"

// TESTING
#include <jack/jack.h>
#include <unistd.h>
static int proc(unsigned frames, void* arg)
{
    Test* const t = static_cast<Test*>(arg);
    t->b1 = (float*)jack_port_get_buffer(t->p1, 0);
    t->b2 = (float*)jack_port_get_buffer(t->p2, 0);
    runAudio(*t, frames);
    return 0;
}

int main(int argc, const char* argv[])
{
    std::vector<DeviceID> inputs, outputs;
    enumerateSoundcards(inputs, outputs);

    for (auto device : outputs)
    {
        DeviceProperties props;
        if (getDeviceProperties(device.id, true, true, 48000, props))
            printf("%s | %s | ins %u/%u | outs %u/%u | min buf size %u\n",
                   device.id.c_str(), device.name.c_str(),
                   props.minChansIn, props.maxChansIn,
                   props.minChansOut, props.maxChansOut,
                   props.bufsizes[0]);
        else
            printf("%s | %s | FAIL\n", device.id.c_str(), device.name.c_str());
    }

    // TESTING
    if (jack_client_t* const c = jack_client_open("audio-test", JackNoStartServer, nullptr))
    {
        Test t;
        if (!initAudio(t, argc > 1 ? argv[1] : outputs[outputs.size() - 1].id.c_str(),
                       jack_get_buffer_size(c), jack_get_sample_rate(c)))
            return 1;

        t.p1 = jack_port_register(c, "p1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
        t.p2 = jack_port_register(c, "p2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
        jack_set_process_callback(c, proc, &t);
        jack_activate(c);

        while (true) sleep(1);

        jack_client_close(c);
    }

    cleanup();

    return 0;
}
