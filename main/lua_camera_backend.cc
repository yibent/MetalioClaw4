#include "lua_camera_backend.h"

#include "board.h"
#include "camera.h"
#include "lua_runtime.h"

#include <cstring>
#include <exception>
#include <string>

#include "esp_err.h"
#include "esp_log.h"

namespace {

constexpr const char* TAG = "LuaCamera";

esp_err_t ExplainPhoto(const char* question, char* output, size_t output_size, void* user_ctx) {
    (void)user_ctx;
    if (!output || output_size == 0)
        return ESP_ERR_INVALID_ARG;
    output[0] = '\0';
    Camera* camera = Board::GetInstance().GetCamera();
    if (camera == nullptr)
        return ESP_ERR_NOT_SUPPORTED;
    try {
        if (!camera->Capture())
            return ESP_FAIL;
        const std::string result = camera->Explain(question ? question : "描述这张图片");
        strlcpy(output, result.c_str(), output_size);
        return ESP_OK;
    } catch (const std::exception& ex) {
        ESP_LOGE(TAG, "camera.explain failed: %s", ex.what());
        strlcpy(output, ex.what(), output_size);
        return ESP_FAIL;
    }
}

}  // namespace

void RegisterLuaCameraBackend() {
    if (lua_runtime_set_camera_backend(ExplainPhoto, nullptr) != ESP_OK)
        ESP_LOGW(TAG, "failed to register camera backend");
}
