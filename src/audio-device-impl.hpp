// SPDX-FileCopyrightText: 2021-2025 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "audio-device.hpp"

AudioDevice::Impl* initAudioDeviceImpl(const AudioDevice* dev, AudioDevice::HWConfig& hwconfig);
void closeAudioDeviceImpl(AudioDevice::Impl* impl);
void runAudioDeviceCaptureImpl(AudioDevice::Impl* impl, float* buffers[]);
void runAudioDeviceCaptureThreadImpl(AudioDevice::Impl* impl);
void runAudioDevicePlaybackImpl(AudioDevice::Impl* impl, float* buffers[]);
void runAudioDevicePlaybackThreadImpl(AudioDevice::Impl* impl);
bool runAudioDevicePostImpl(AudioDevice::Impl* impl);
