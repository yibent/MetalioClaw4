#include "lua_dinosaur_screen.h"

#include <algorithm>

#include "esp_log.h"
#include "home_screen/home_screen.h"
#include "lua_runtime.h"
#include "lvgl.h"

extern const char lua_dinosaur_game[] asm("_binary_lua_dinosaur_game_start");

namespace {

constexpr const char* TAG = "LuaDinosaur";

lv_obj_t* s_launcher_screen;
lv_obj_t* s_status_label;
lv_obj_t* s_retry_button;
lv_timer_t* s_job_timer;
lv_timer_t* s_start_timer;
lua_runtime_job_id_t s_job_id;
screen_lifecycle_cb_t s_lifecycle_cb;
bool s_job_active;
bool s_return_requested;

void ReturnHome() {
    if (s_job_active) {
        lua_runtime_stop(s_job_id);
        s_return_requested = true;
        return;
    }
    if (s_job_timer != nullptr) {
        lv_timer_delete(s_job_timer);
        s_job_timer = nullptr;
    }
    if (s_start_timer != nullptr) {
        lv_timer_delete(s_start_timer);
        s_start_timer = nullptr;
    }

    lv_obj_t* old_screen = lv_screen_active();
    if (s_lifecycle_cb != nullptr) {
        s_lifecycle_cb(SCREEN_LIFECYCLE_UNLOAD);
        s_lifecycle_cb = nullptr;
    }
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_screen != nullptr && old_screen != home) {
        lv_obj_delete_async(old_screen);
    }
    s_launcher_screen = nullptr;
    s_status_label = nullptr;
    s_retry_button = nullptr;
}

void BackButtonCallback(lv_event_t* event) {
    (void)event;
    s_return_requested = true;
    if (s_job_active) {
        lua_runtime_stop(s_job_id);
        if (s_status_label != nullptr) {
            lv_label_set_text(s_status_label, "Stopping Lua game...");
        }
        return;
    }
    ReturnHome();
}

esp_err_t StartJob() {
    lua_runtime_job_config_t config = {
        .name = "lua_dinosaur",
        .code = lua_dinosaur_game,
        .path = nullptr,
        .args_json = "{}",
        .timeout_ms = 0,
        .stack_size = 16 * 1024,
        .priority = 4,
        .capabilities = 0,
    };
    esp_err_t err = lua_runtime_start(&config, &s_job_id);
    if (err == ESP_OK) {
        s_job_active = true;
        s_return_requested = false;
        if (s_status_label != nullptr) {
            lv_label_set_text(s_status_label, "Starting Lua game...");
        }
        if (s_retry_button != nullptr) {
            lv_obj_add_flag(s_retry_button, LV_OBJ_FLAG_HIDDEN);
        }
    }
    return err;
}

void RetryButtonCallback(lv_event_t* event) {
    (void)event;
    esp_err_t err = StartJob();
    if (err != ESP_OK && s_status_label != nullptr) {
        lv_label_set_text_fmt(s_status_label, "Start failed: %s", esp_err_to_name(err));
    }
}

void JobTimerCallback(lv_timer_t* timer) {
    (void)timer;
    if (!s_job_active) {
        return;
    }

    lua_runtime_job_info_t info;
    char output[256] = {};
    esp_err_t err = lua_runtime_get_job(s_job_id, &info, output, sizeof(output));
    if (err != ESP_OK || info.state < LUA_RUNTIME_JOB_DONE) {
        return;
    }

    s_job_active = false;
    if (s_return_requested || info.state == LUA_RUNTIME_JOB_DONE ||
        info.state == LUA_RUNTIME_JOB_STOPPED) {
        ReturnHome();
        return;
    }

    ESP_LOGE(TAG, "Lua game failed, state=%d, output=%s", static_cast<int>(info.state), output);
    if (s_status_label != nullptr) {
        const char* message = output[0] != '\0' ? output : "Lua game failed";
        lv_label_set_text(s_status_label, message);
    }
    if (s_retry_button != nullptr) {
        lv_obj_remove_flag(s_retry_button, LV_OBJ_FLAG_HIDDEN);
    }
}

void StartJobTimerCallback(lv_timer_t* timer) {
    (void)timer;
    s_start_timer = nullptr;
    lv_timer_delete(timer);
    if (s_launcher_screen == nullptr || s_job_active || s_return_requested)
        return;
    esp_err_t err = StartJob();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to start Lua game: %s", esp_err_to_name(err));
        if (s_status_label != nullptr)
            lv_label_set_text_fmt(s_status_label, "Start failed: %s", esp_err_to_name(err));
        if (s_retry_button != nullptr)
            lv_obj_remove_flag(s_retry_button, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t* CreateLauncherScreen() {
    lv_display_t* display = lv_display_get_default();
    const int width = display != nullptr ? lv_display_get_horizontal_resolution(display) : 720;
    const int height = display != nullptr ? lv_display_get_vertical_resolution(display) : 720;

    lv_obj_t* screen = lv_obj_create(nullptr);
    screen_mark_native_layout(screen);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xf7f7f7), LV_PART_MAIN);

    lv_obj_t* title = lv_label_create(screen);
    lv_label_set_text(title, "LUA DINO");
    lv_obj_set_style_text_color(title, lv_color_hex(0x3c4043), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -40);

    s_status_label = lv_label_create(screen);
    lv_label_set_text(s_status_label, "Preparing Lua runtime...");
    lv_obj_set_width(s_status_label, std::max(200, width - 80));
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x5f6368), LV_PART_MAIN);
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, 10);

    s_retry_button = lv_button_create(screen);
    lv_obj_set_size(s_retry_button, 120, 44);
    lv_obj_align(s_retry_button, LV_ALIGN_CENTER, 0, 80);
    lv_obj_add_event_cb(s_retry_button, RetryButtonCallback, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(s_retry_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* retry_label = lv_label_create(s_retry_button);
    lv_label_set_text(retry_label, "Retry");
    lv_obj_center(retry_label);

    lv_obj_t* back_button = lv_button_create(screen);
    lv_obj_set_size(back_button, 100, 44);
    lv_obj_set_pos(back_button, 18, std::max(18, height - 62));
    lv_obj_set_style_bg_color(back_button, lv_color_hex(0x53565a), LV_PART_MAIN);
    lv_obj_add_event_cb(back_button, BackButtonCallback, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* back_label = lv_label_create(back_button);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
    return screen;
}

}  // namespace

void LuaDinosaurApp::Launch(screen_lifecycle_cb_t lifecycle_cb) {
    if (s_job_active || s_launcher_screen != nullptr) {
        ESP_LOGW(TAG, "Lua dinosaur app is already active");
        return;
    }

    lv_obj_t* old_screen = lv_screen_active();
    s_lifecycle_cb = lifecycle_cb;
    s_return_requested = false;
    s_launcher_screen = CreateLauncherScreen();
    lv_screen_load(s_launcher_screen);
    if (s_lifecycle_cb != nullptr) {
        s_lifecycle_cb(SCREEN_LIFECYCLE_LOAD);
    }
    if (old_screen != nullptr && old_screen != s_launcher_screen) {
        lv_obj_delete_async(old_screen);
    }

    s_job_timer = lv_timer_create(JobTimerCallback, 100, nullptr);
    /* Start after the newly loaded screen has gone through at least one LVGL
     * timer cycle. This avoids racing adapter mutex creation during boot. */
    s_start_timer = lv_timer_create(StartJobTimerCallback, 100, nullptr);
    lv_timer_set_repeat_count(s_start_timer, 1);
}
