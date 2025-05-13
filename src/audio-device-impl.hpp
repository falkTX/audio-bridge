// SPDX-FileCopyrightText: 2021-2025 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "audio-device.hpp"

AudioDevice::Impl* initAudioDeviceImpl(const AudioDevice* dev, AudioDevice::HWConfig& hwconfig);
void closeAudioDeviceImpl(AudioDevice::Impl* impl);
bool runAudioDevicePostImpl(AudioDevice::Impl* impl, uint16_t numFrames);

bool runAudioDeviceCaptureSyncImpl(AudioDevice::Impl* impl, float* buffers[], uint16_t numFrames);
bool runAudioDevicePlaybackSyncImpl(AudioDevice::Impl* impl, float* buffers[], uint16_t numFrames);
