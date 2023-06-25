// SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

struct DeviceID {
    std::string id;
    std::string name;
};

struct DeviceProperties {
    unsigned minChansIn = 0;
    unsigned maxChansIn = 0;
    unsigned minChansOut = 0;
    unsigned maxChansOut = 0;
    std::vector<unsigned> bufsizes;
};

bool enumerateSoundcards(std::vector<DeviceID>& inputs, std::vector<DeviceID>& outputs);

bool getDeviceProperties(const std::string& deviceID, bool checkInput, bool checkOutput, unsigned sampleRate, DeviceProperties& props);

void cleanup();
