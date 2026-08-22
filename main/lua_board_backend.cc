#include "lua_board_backend.h"

#include "application.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "backlight.h"
#include "board.h"
#include "display.h"
#include "lua_runtime.h"

#include <algorithm>
#include <string>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr const char* TAG = "LuaBoard";
constexpr gpio_num_t kVibrateGpio = GPIO_NUM_22;
constexpr ledc_mode_t kLedcMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kLedcTimer = LEDC_TIMER_1;
constexpr ledc_channel_t kLedcChannel = LEDC_CHANNEL_1;
constexpr ledc_timer_bit_t kLedcDutyRes = LEDC_TIMER_10_BIT;
constexpr uint32_t kLedcFreqHz = 5000;
constexpr uint32_t kLedcDutyMax = (1U << kLedcDutyRes) - 1U;
constexpr int kOnDutyPct = 60;

bool s_vibrate_ready = false;

bool EnsureVibratePwm() {
    if (s_vibrate_ready)
        return true;

    ledc_timer_config_t timer_cfg = {};
    timer_cfg.speed_mode = kLedcMode;
    timer_cfg.timer_num = kLedcTimer;
    timer_cfg.duty_resolution = kLedcDutyRes;
    timer_cfg.freq_hz = kLedcFreqHz;
    timer_cfg.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&timer_cfg) != ESP_OK)
        return false;

    ledc_channel_config_t ch_cfg = {};
    ch_cfg.gpio_num = kVibrateGpio;
    ch_cfg.speed_mode = kLedcMode;
    ch_cfg.channel = kLedcChannel;
    ch_cfg.intr_type = LEDC_INTR_DISABLE;
    ch_cfg.timer_sel = kLedcTimer;
    ch_cfg.duty = 0;
    ch_cfg.hpoint = 0;
    if (ledc_channel_config(&ch_cfg) != ESP_OK)
        return false;

    s_vibrate_ready = true;
    return true;
}

void SetVibrateDutyPct(int pct) {
    if (!s_vibrate_ready)
        return;
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    const uint32_t duty = (static_cast<uint32_t>(pct) * kLedcDutyMax) / 100U;
    ledc_set_duty(kLedcMode, kLedcChannel, duty);
    ledc_update_duty(kLedcMode, kLedcChannel);
}

esp_err_t ShowAlert(const char* text, void* user_ctx) {
    (void)user_ctx;
    std::string message = text ? text : "";
    Application::GetInstance().Schedule([message]() {
        Application::GetInstance().Alert("播报", message.c_str(), "happy", Lang::Sounds::OGG_POPUP);
    });
    return ESP_OK;
}

esp_err_t SetBrightness(int value, void* user_ctx) {
    (void)user_ctx;
    int clamped = std::clamp(value, 0, 100);
    Application::GetInstance().Schedule([clamped]() {
        auto* backlight = Board::GetInstance().GetBacklight();
        if (backlight)
            backlight->SetBrightness(static_cast<uint8_t>(clamped), true);
    });
    return ESP_OK;
}

esp_err_t SetVolume(int value, void* user_ctx) {
    (void)user_ctx;
    int clamped = std::clamp(value, 0, 100);
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (!codec)
        return ESP_ERR_NOT_SUPPORTED;
    codec->SetOutputVolume(clamped);
    return ESP_OK;
}

esp_err_t Vibrate(uint32_t duration_ms, void* user_ctx) {
    (void)user_ctx;
    if (!EnsureVibratePwm())
        return ESP_FAIL;
    SetVibrateDutyPct(kOnDutyPct);
    uint32_t remaining = duration_ms;
    while (remaining > 0) {
        uint32_t slice = remaining > 50 ? 50 : remaining;
        vTaskDelay(pdMS_TO_TICKS(slice));
        remaining -= slice;
    }
    SetVibrateDutyPct(0);
    return ESP_OK;
}

esp_err_t Notify(const char* text, void* user_ctx) {
    (void)user_ctx;
    std::string message = text ? text : "";
    Application::GetInstance().Schedule([message]() {
        auto* display = Board::GetInstance().GetDisplay();
        if (display)
            display->ShowNotification(message.c_str(), 2500);
    });
    return ESP_OK;
}

}  // namespace

void RegisterLuaBoardBackend() {
    if (lua_runtime_set_alert_backend(ShowAlert, nullptr) != ESP_OK)
        ESP_LOGW(TAG, "failed to register alert backend");
    if (lua_runtime_set_device_backend(SetBrightness, SetVolume, Vibrate, Notify, nullptr) !=
        ESP_OK)
        ESP_LOGW(TAG, "failed to register device backend");
}
