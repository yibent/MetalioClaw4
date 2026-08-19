#include "lua_runtime.h"
#include "lua_audio_backend.h"

#include "application.h"
#include "audio_service.h"

#include <atomic>
#include <stdio.h>
#include <stdlib.h>

namespace {

constexpr size_t kMaxLuaSoundBytes = 256 * 1024;
std::atomic<uint32_t> s_next_handle{1};

esp_err_t PlayLuaSound(const char* source, bool loop, uint8_t volume, uint32_t* handle,
                       void* user_ctx) {
    (void)volume;
    (void)user_ctx;
    if (!source || !handle)
        return ESP_ERR_INVALID_ARG;
    FILE* file = fopen(source, "rb");
    if (!file)
        return ESP_ERR_NOT_FOUND;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    long length = ftell(file);
    if (length <= 0 || (size_t)length > kMaxLuaSoundBytes) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }
    rewind(file);
    auto* data = static_cast<char*>(malloc((size_t)length));
    if (!data) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    size_t read = fread(data, 1, (size_t)length, file);
    fclose(file);
    if (read != (size_t)length) {
        free(data);
        return ESP_FAIL;
    }
    uint32_t sound_handle = s_next_handle.fetch_add(1);
    if (sound_handle == 0)
        sound_handle = s_next_handle.fetch_add(1);
    bool queued = Application::GetInstance().GetAudioService().PlaySoundEffect(
        std::string_view(data, read), sound_handle, volume, loop);
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

}  // namespace

void RegisterLuaAudioBackend() {
    lua_runtime_set_audio_backend(PlayLuaSound, StopLuaSound, StopAllLuaSounds,
                                  LuaSoundIsPlaying, nullptr);
}
