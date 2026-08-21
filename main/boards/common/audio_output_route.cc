#include "audio_output_route.h"

namespace {
AudioOutputTarget s_target = AudioOutputTarget::LocalSpeaker;
AudioOutputVolumeChangeHandler s_volume_handler = nullptr;
void* s_volume_context = nullptr;
AudioOutputCodecChangeHandler s_codec_handler = nullptr;
void* s_codec_context = nullptr;
}  // namespace

AudioOutputTarget AudioOutput_GetTarget() { return s_target; }

void AudioOutput_SetTarget(AudioOutputTarget target, bool /*persist*/) {
    s_target = target;
}

void AudioOutput_SetCodecEnabled(bool enabled) {
    if (s_codec_handler != nullptr) {
        s_codec_handler(enabled, s_target, s_codec_context);
    }
}

void AudioOutput_SetStandby(bool /*standby*/) {}

void AudioOutput_SetVolumeChangeHandler(AudioOutputVolumeChangeHandler handler,
                                        void* context) {
    s_volume_handler = handler;
    s_volume_context = context;
}

void AudioOutput_SetCodecChangeHandler(AudioOutputCodecChangeHandler handler,
                                       void* context) {
    s_codec_handler = handler;
    s_codec_context = context;
}

void AudioOutput_NotifyVolumeChanged(int volume) {
    if (s_volume_handler != nullptr) {
        s_volume_handler(volume, s_volume_context);
    }
}
