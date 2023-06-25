// SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-discovery.hpp"

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

    cleanup();

    return 0;
}
