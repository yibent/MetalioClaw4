#pragma once

#include <cstdint>

enum class AudioOutputTarget : uint8_t {
    LocalSpeaker = 0,
    BluetoothSpeaker = 1,
};

using AudioOutputVolumeChangeHandler = void (*)(int volume, void* context);
using AudioOutputCodecChangeHandler = void (*)(bool enabled,
                                                AudioOutputTarget target,
                                                void* context);

AudioOutputTarget AudioOutput_GetTarget();
void AudioOutput_SetTarget(AudioOutputTarget target, bool persist = true);
void AudioOutput_SetCodecEnabled(bool enabled);
void AudioOutput_SetStandby(bool standby);
void AudioOutput_SetVolumeChangeHandler(AudioOutputVolumeChangeHandler handler,
                                        void* context);
void AudioOutput_SetCodecChangeHandler(AudioOutputCodecChangeHandler handler,
                                       void* context);
void AudioOutput_NotifyVolumeChanged(int volume);
