#include "lua_runtime.h"
#include "lua_audio_backend.h"

#include "application.h"
#include "assets/lang_config.h"
#include "audio_service.h"

#include <atomic>
#include <cstring>
#include <stdio.h>
#include <stdlib.h>
#include <string_view>

namespace {

constexpr size_t kMaxLuaSoundBytes = 512 * 1024;
std::atomic<uint32_t> s_next_handle{1};

esp_err_t PlayLuaSound(const char* source, bool loop, uint8_t volume, uint32_t* handle,
                       void* user_ctx) {
    (void)volume;
    (void)user_ctx;
    if (!source || !handle)
        return ESP_ERR_INVALID_ARG;
    std::string_view sound;
    if (strcmp(source, "builtin:success") == 0) {
        sound = Lang::Sounds::OGG_SUCCESS;
    }

    FILE* file = sound.empty() ? fopen(source, "rb") : nullptr;
    if (!sound.empty()) {
        // Built-in sounds live in flash and remain valid for the duration of playback.
    } else if (!file) {
        return ESP_ERR_NOT_FOUND;
    }
    if (file && fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    long length = file ? ftell(file) : static_cast<long>(sound.size());
    if (length <= 0 || static_cast<size_t>(length) > kMaxLuaSoundBytes) {
        if (file)
            fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }
    char* data = nullptr;
    if (file) {
        rewind(file);
        data = static_cast<char*>(malloc(static_cast<size_t>(length)));
        if (!data) {
            fclose(file);
            return ESP_ERR_NO_MEM;
        }
        size_t read = fread(data, 1, static_cast<size_t>(length), file);
        fclose(file);
        if (read != static_cast<size_t>(length)) {
            free(data);
            return ESP_FAIL;
        }
        sound = std::string_view(data, read);
    }
    uint32_t sound_handle = s_next_handle.fetch_add(1);
    if (sound_handle == 0)
        sound_handle = s_next_handle.fetch_add(1);
    bool queued = Application::GetInstance().GetAudioService().PlaySoundEffect(
        sound, sound_handle, volume, loop);
    free(data);
    if (!queued)
        return ESP_ERR_INVALID_RESPONSE;
    *handle = sound_handle;
    return ESP_OK;
}

esp_err_t StopLuaSound(uint32_t handle, void* user_ctx) {
    (void)user_ctx;
    Application::GetInstance().GetAudioService().StopSoundEffect(handle);
    return ESP_OK;
}

esp_err_t StopAllLuaSounds(void* user_ctx) {
    (void)user_ctx;
    Application::GetInstance().GetAudioService().StopAllSoundEffects();
    return ESP_OK;
}

bool LuaSoundIsPlaying(uint32_t handle, void* user_ctx) {
    (void)user_ctx;
    return Application::GetInstance().GetAudioService().IsSoundEffectPlaying(handle);
}

esp_err_t PlayLuaBytes(const void* data, size_t len, bool loop, uint8_t volume, uint32_t* handle,
                       void* user_ctx) {
    (void)user_ctx;
    if (!data || !handle || len == 0)
        return ESP_ERR_INVALID_ARG;
    if (len > kMaxLuaSoundBytes)
        return ESP_ERR_INVALID_SIZE;
    uint32_t sound_handle = s_next_handle.fetch_add(1);
    if (sound_handle == 0)
        sound_handle = s_next_handle.fetch_add(1);
    bool queued = Application::GetInstance().GetAudioService().PlayWavSoundEffect(
        data, len, sound_handle, volume, loop);
    if (!queued)
        return ESP_ERR_INVALID_RESPONSE;
    *handle = sound_handle;
    return ESP_OK;
}

}  // namespace

void RegisterLuaAudioBackend() {
    lua_runtime_set_audio_backend(PlayLuaSound, StopLuaSound, StopAllLuaSounds,
                                  LuaSoundIsPlaying, nullptr);
    lua_runtime_set_audio_bytes_backend(PlayLuaBytes, nullptr);
}
