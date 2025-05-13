// SPDX-FileCopyrightText: 2021-2025 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio-device-discovery.hpp"

int main()
{
    std::vector<DeviceID> inputs, outputs;
    enumerateAudioDevices(inputs, outputs);

    for (auto device : outputs)
    {
        DeviceProperties props;
        if (getAudioDeviceProperties(device.id, true, true, 48000, props))
            printf("%s | %s | ins %u/%u | outs %u/%u | min buf size %u\n",
                   device.id.c_str(), device.name.c_str(),
                   props.minChansIn, props.maxChansIn,
                   props.minChansOut, props.maxChansOut,
                   props.bufsizes[0]);
        else
            printf("%s | %s | FAIL\n", device.id.c_str(), device.name.c_str());
    }

    cleanupAudioDevices();
    return 0;
}
