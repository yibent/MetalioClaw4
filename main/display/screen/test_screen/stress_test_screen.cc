#include "stress_test_screen.h"
#include "i18n.h"

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "display/lvgl_display/gif/lvgl_gif.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pwr_key_handler.h"
#include "screen_util.h"
#include "test_screen.h"
#include "test_ui_common.h"

#include <cerrno>
#include <sys/stat.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "esp_audio_simple_player.h"
#include "esp_audio_simple_player_advance.h"

#ifdef CONFIG_ESP_AUDIO_SIMPLE_PLAYER_RESAMPLE_EN
#include "esp_gmf_pipeline.h"
#include "esp_gmf_rate_cvt.h"
#endif

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_puhui_number_50_4);

namespace {

constexpr const char* TAG = "StressTestScreen";
constexpr const char* kFactoryTestMount = "/factory_test";
constexpr const char* kGifPath = "S:/factory_test/stress_test.gif";
constexpr const char* kGifFilePath = "/factory_test/stress_test.gif";
constexpr const char* kBgMusicUri = "file://factory_test/stress_test_music.mp3";
constexpr const char* kBgMusicFilePath = "/factory_test/stress_test_music.mp3";
constexpr size_t kFactoryTestPartitionSize = 1408 * 1024;

lv_obj_t* s_screen = nullptr;
lv_obj_t* s_setup_panel = nullptr;
lv_obj_t* s_volume_pct_lbl = nullptr;

volatile bool s_bg_music_shutdown = false;
volatile bool s_bg_music_playing = false;
TaskHandle_t s_bg_music_task = nullptr;
esp_asp_handle_t s_bg_player = nullptr;
bool s_factory_test_mounted = false;
AudioCodec* s_audio_codec = nullptr;
std::vector<int16_t> s_bgm_pcm_buf;

std::unique_ptr<LvglGif> s_gif_controller;
lv_obj_t* s_gif_image = nullptr;
bool s_playback_running = false;
bool s_system_audio_suspended = false;

bool StartStressCycle();
void StopStressCycle();

int ReadStressVolume() {
    int volume = 70;
    if (AudioCodec* codec = Board::GetInstance().GetAudioCodec()) {
        volume = codec->output_volume();
    }
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    return volume;
}

void ApplyStressVolume(int volume) {
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }

    if (s_volume_pct_lbl != nullptr) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d%%", volume);
        lv_label_set_text(s_volume_pct_lbl, buf);
    }

    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr || codec->output_volume() == volume) {
        return;
    }
    codec->SetOutputVolume(volume);
}

void OnVolumeSliderChanged(lv_event_t* e) {
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
    ApplyStressVolume(static_cast<int>(lv_slider_get_value(slider)));
}

void OnSwipeBackToMenu() {
    if (s_playback_running) {
        StopStressCycle();
    }
    TestUiNavigateTo(TestScreen::Create);
}

void OnBackBtnClicked(lv_event_t* /*e*/) { OnSwipeBackToMenu(); }

void OnStartStressClicked(lv_event_t* /*e*/) {
    if (s_playback_running || s_setup_panel == nullptr) {
        return;
    }

    lv_obj_add_flag(s_setup_panel, LV_OBJ_FLAG_HIDDEN);
    if (!StartStressCycle()) {
        lv_obj_clear_flag(s_setup_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

void BuildSetupPanel(lv_obj_t* scr) {
    s_setup_panel = lv_obj_create(scr);
    screen_strip_obj_chrome(s_setup_panel);
    lv_obj_set_size(s_setup_panel, kTestPanelW, kTestPanelH);
    lv_obj_set_pos(s_setup_panel, 0, 0);
    lv_obj_set_style_bg_color(s_setup_panel, lv_color_hex(kTestColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_setup_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(s_setup_panel, LV_OBJ_FLAG_SCROLLABLE);

    TestUiCreateHeader(s_setup_panel, I18n::T("压力测试"), OnBackBtnClicked);

    const int initial_volume = ReadStressVolume();

    lv_obj_t* card = lv_obj_create(s_setup_panel);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, kTestPanelW - 2 * kTestSideMargin, 220);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, kTestHeaderH + 24);
    lv_obj_set_style_bg_color(card, lv_color_hex(kTestColorCardBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 24, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* pct = lv_label_create(card);
    s_volume_pct_lbl = pct;
    lv_obj_set_width(pct, LV_PCT(100));
    lv_label_set_long_mode(pct, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(pct, lv_color_hex(0x60A5FA), LV_PART_MAIN);
    lv_obj_set_style_text_font(pct, &font_puhui_number_50_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(pct, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(pct, LV_ALIGN_CENTER, 0, -20);
    ApplyStressVolume(initial_volume);

    lv_obj_t* hint = lv_label_create(card);
    lv_label_set_text(hint, I18n::T("背景音乐音量"));
    lv_obj_set_style_text_color(hint, lv_color_hex(kTestColorTextDim), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);

    lv_obj_t* slider_row = lv_obj_create(s_setup_panel);
    lv_obj_remove_style_all(slider_row);
    lv_obj_set_size(slider_row, kTestPanelW - 2 * kTestSideMargin, 52);
    lv_obj_align(slider_row, LV_ALIGN_TOP_MID, 0, kTestHeaderH + 24 + 220 + 20);
    lv_obj_set_style_bg_opa(slider_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(slider_row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(slider_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* slider = lv_slider_create(slider_row);
    lv_obj_set_width(slider, LV_PCT(100));
    lv_obj_set_height(slider, 28);
    lv_obj_align(slider, LV_ALIGN_CENTER, 0, 0);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, initial_volume, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(kTestColorMuted), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x3B82F6), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_add_flag(slider, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_pad_hor(slider, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(slider, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(slider, 4, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, 10, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 10, LV_PART_INDICATOR);
    lv_obj_add_event_cb(slider, OnVolumeSliderChanged, LV_EVENT_VALUE_CHANGED, nullptr);
    screen_swipe_back_ignore(slider, true);

    lv_obj_t* range = lv_label_create(s_setup_panel);
    lv_label_set_text(range, "0% ~ 100%");
    lv_obj_set_style_text_color(range, lv_color_hex(kTestColorTextDim), LV_PART_MAIN);
    lv_obj_set_style_text_font(range, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(range, LV_ALIGN_TOP_MID, 0, kTestHeaderH + 24 + 220 + 20 + 56);

    lv_obj_t* start = lv_button_create(s_setup_panel);
    lv_obj_set_size(start, 320, 72);
    lv_obj_align(start, LV_ALIGN_BOTTOM_MID, 0, -48);
    lv_obj_set_style_radius(start, 20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(start, lv_color_hex(0x3B82F6), LV_PART_MAIN);
    lv_obj_add_event_cb(start, OnStartStressClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(start, true);

    lv_obj_t* start_lbl = lv_label_create(start);
    lv_label_set_text(start_lbl, I18n::T("开始压力测试"));
    lv_obj_set_style_text_color(start_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(start_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_center(start_lbl);

    lv_obj_t* foot = lv_label_create(s_setup_panel);
    lv_label_set_text(foot, I18n::T("循环播放 GIF 动画和自定义音乐"));
    lv_obj_set_width(foot, kTestPanelW - 2 * kTestSideMargin);
    lv_label_set_long_mode(foot, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(foot, lv_color_hex(kTestColorTextDim), LV_PART_MAIN);
    lv_obj_set_style_text_font(foot, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(foot, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -132);
}

extern "C" int BgMusicEventCallback(esp_asp_event_pkt_t* /*event*/, void* /*ctx*/) {
    // Required so run_to_end receives STOPPED/FINISHED via wait_event.
    return 0;
}

extern "C" int BgMusicOutCallback(uint8_t* data, int data_size, void* ctx) {
    auto* codec = static_cast<AudioCodec*>(ctx);
    if (codec == nullptr || data == nullptr || data_size <= 0) {
        return 0;
    }

    const int samples = data_size / static_cast<int>(sizeof(int16_t));
    if (samples <= 0) {
        return 0;
    }

    const auto* pcm_data = reinterpret_cast<const int16_t*>(data);
    s_bgm_pcm_buf.resize(static_cast<size_t>(samples));
    std::memcpy(s_bgm_pcm_buf.data(), pcm_data, static_cast<size_t>(data_size));
    codec->OutputData(s_bgm_pcm_buf);
    return 0;
}

extern "C" int BgMusicPrevCallback(esp_asp_handle_t* handle, void* ctx) {
#ifdef CONFIG_ESP_AUDIO_SIMPLE_PLAYER_RESAMPLE_EN
    const esp_asp_handle_t player = reinterpret_cast<esp_asp_handle_t>(handle);
    if (player == nullptr) {
        return 0;
    }

    auto* codec = static_cast<AudioCodec*>(ctx);
    if (codec == nullptr) {
        return 0;
    }

    esp_gmf_pipeline_handle_t pipe = nullptr;
    esp_gmf_element_handle_t rate_el = nullptr;
    if (esp_audio_simple_player_get_pipeline(player, &pipe) != ESP_GMF_ERR_OK || pipe == nullptr) {
        return 0;
    }
    if (esp_gmf_pipeline_get_el_by_name(pipe, "aud_rate_cvt", &rate_el) != ESP_GMF_ERR_OK ||
        rate_el == nullptr) {
        return 0;
    }

    esp_gmf_rate_cvt_set_dest_rate(rate_el, codec->output_sample_rate());
#endif
    return 0;
}

bool MountFactoryTestPartition() {
    if (s_factory_test_mounted) {
        return true;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = kFactoryTestMount,
        .partition_label = "factory_test",
        .max_files = 2,
        .format_if_mount_failed = false,
    };

    const esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount factory_test spiffs failed: %s", esp_err_to_name(err));
        return false;
    }

    s_factory_test_mounted = true;
    ESP_LOGI(TAG, "factory_test spiffs mounted at %s", kFactoryTestMount);
    return true;
}

bool ValidateStressAsset(const char* name, const char* path) {
    struct stat file_info = {};
    // ESP-IDF's SPIFFS VFS implements stat/open but not access().
    if (stat(path, &file_info) != 0) {
        const int stat_errno = errno;
        ESP_LOGE(TAG, "%s unavailable: %s (errno=%d: %s)", name, path, stat_errno,
                 std::strerror(stat_errno));
        return false;
    }
    if (!S_ISREG(file_info.st_mode)) {
        ESP_LOGE(TAG, "%s is not a regular file: %s", name, path);
        return false;
    }

    ESP_LOGI(TAG, "%s ready: %s (%lu bytes)", name, path,
             static_cast<unsigned long>(file_info.st_size));
    return true;
}

bool ValidateStressAssets() {
    bool ready = true;
    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "factory_test");
    if (partition == nullptr) {
        ESP_LOGE(TAG, "factory_test partition not found in partition table");
        ready = false;
    } else {
        ESP_LOGI(TAG, "factory_test partition: offset=0x%08lx, size=%lu bytes",
                 static_cast<unsigned long>(partition->address),
                 static_cast<unsigned long>(partition->size));
        if (partition->size < kFactoryTestPartitionSize) {
            ESP_LOGE(TAG,
                     "factory_test partition is too small (%lu < %lu bytes); "
                     "flash the updated partition table and factory_test image",
                     static_cast<unsigned long>(partition->size),
                     static_cast<unsigned long>(kFactoryTestPartitionSize));
            ready = false;
        }
    }

    size_t total = 0;
    size_t used = 0;
    const esp_err_t info_err = esp_spiffs_info("factory_test", &total, &used);
    if (info_err == ESP_OK) {
        ESP_LOGI(TAG, "factory_test SPIFFS: used=%lu, total=%lu bytes",
                 static_cast<unsigned long>(used), static_cast<unsigned long>(total));
    } else {
        ESP_LOGW(TAG, "read factory_test SPIFFS info failed: %s",
                 esp_err_to_name(info_err));
    }

    const bool gif_ready = ValidateStressAsset("stress GIF", kGifFilePath);
    const bool music_ready = ValidateStressAsset("stress music", kBgMusicFilePath);
    ready = ready && gif_ready && music_ready;
    if (!ready) {
        ESP_LOGE(TAG,
                 "stress-test assets are unavailable; run a full 'idf.py flash' "
                 "instead of app-only flashing");
    }
    return ready;
}

void UnmountFactoryTestPartition() {
    if (!s_factory_test_mounted) {
        return;
    }

    esp_vfs_spiffs_unregister("factory_test");
    s_factory_test_mounted = false;
}

void BgMusicTask(void* /*arg*/) {
    esp_asp_cfg_t cfg = {
        .in = {},
        .out =
            {
                .cb = BgMusicOutCallback,
                .user_ctx = s_audio_codec,
            },
        .task_prio = 5,
        .task_stack = 8 * 1024,
        .prev = BgMusicPrevCallback,
        .prev_ctx = s_audio_codec,
    };

    if (esp_audio_simple_player_new(&cfg, &s_bg_player) != ESP_GMF_ERR_OK ||
        s_bg_player == nullptr) {
        ESP_LOGE(TAG, "create bg music player failed");
        s_bg_music_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    esp_audio_simple_player_set_event(s_bg_player, BgMusicEventCallback, nullptr);

    if (s_audio_codec != nullptr) {
        s_audio_codec->EnableOutput(true);
    }

    while (!s_bg_music_shutdown) {
        if (!s_bg_music_playing) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        const esp_gmf_err_t err =
            esp_audio_simple_player_run_to_end(s_bg_player, kBgMusicUri, nullptr);
        if (err != ESP_GMF_ERR_OK) {
            ESP_LOGW(TAG, "bg music play ended/failed: 0x%x", err);
            if (!s_bg_music_playing || s_bg_music_shutdown) {
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    esp_audio_simple_player_stop(s_bg_player);
    esp_audio_simple_player_destroy(s_bg_player);
    s_bg_player = nullptr;
    s_bg_music_task = nullptr;
    vTaskDelete(nullptr);
}

void WaitBgMusicTaskStopped() {
    for (int i = 0; i < 100 && s_bg_music_task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void ShutdownBgMusicSession() {
    s_bg_music_playing = false;
    s_bg_music_shutdown = true;

    if (s_bg_player != nullptr) {
        esp_audio_simple_player_stop(s_bg_player);
    }

    WaitBgMusicTaskStopped();
    s_audio_codec = nullptr;
}

bool InitBgMusicSession() {
    ShutdownBgMusicSession();

    s_audio_codec = Board::GetInstance().GetAudioCodec();
    if (s_audio_codec == nullptr) {
        ESP_LOGE(TAG, "no audio codec for stress music");
        return false;
    }

    s_bg_music_shutdown = false;
    s_bg_music_playing = false;

    const BaseType_t ok =
        xTaskCreate(BgMusicTask, "stress_bgm", 8192, nullptr, 5, &s_bg_music_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create bg music task failed");
        s_audio_codec = nullptr;
        s_bg_music_task = nullptr;
        return false;
    }
    return true;
}

void StartBgMusic() {
    if (s_bg_music_task == nullptr) {
        return;
    }
    s_bg_music_playing = true;
}

void StopGif() {
    if (s_gif_image != nullptr) {
        lv_image_set_src(s_gif_image, nullptr);
        lv_obj_delete(s_gif_image);
        s_gif_image = nullptr;
    }
    s_gif_controller.reset();
}

bool StartGif() {
    s_gif_controller = std::make_unique<LvglGif>(kGifPath);
    if (!s_gif_controller->IsLoaded()) {
        ESP_LOGE(TAG, "failed to load stress GIF: %s", kGifPath);
        s_gif_controller.reset();
        return false;
    }
    s_gif_controller->SetLoopCount(0);

    s_gif_image = lv_image_create(s_screen);
    lv_obj_set_size(s_gif_image, s_gif_controller->width(), s_gif_controller->height());
    lv_image_set_src(s_gif_image, s_gif_controller->image_dsc());
    const uint32_t scale_x =
        static_cast<uint32_t>(kTestPanelW) * 256 / std::max<uint16_t>(1, s_gif_controller->width());
    const uint32_t scale_y = static_cast<uint32_t>(kTestPanelH) * 256 /
                             std::max<uint16_t>(1, s_gif_controller->height());
    lv_image_set_scale(s_gif_image, std::min(scale_x, scale_y));
    lv_obj_center(s_gif_image);
    screen_make_input_passive(s_gif_image);
    s_gif_controller->SetFrameCallback([]() {
        if (s_gif_image != nullptr) {
            lv_obj_invalidate(s_gif_image);
        }
    });
    s_gif_controller->Start();
    return true;
}

void StopStressCycle() {
    s_playback_running = false;
    StopGif();
    ShutdownBgMusicSession();
    UnmountFactoryTestPartition();
    Application::GetInstance().GetAudioService().SetExternalPlaybackActive(false);
    if (s_system_audio_suspended) {
        auto& app = Application::GetInstance();
        app.SetActivationSuspended(false);
        app.RestoreSystemAudioAfterStressTest();
        s_system_audio_suspended = false;
    }
}

bool StartStressCycle() {
    StopStressCycle();
    if (!MountFactoryTestPartition()) {
        return false;
    }
    if (!ValidateStressAssets()) {
        UnmountFactoryTestPartition();
        return false;
    }

    auto& app = Application::GetInstance();
    app.SetActivationSuspended(true);
    app.StopSystemAudioForStressTest();
    app.GetAudioService().SetExternalPlaybackActive(true);
    s_system_audio_suspended = true;

    if (!StartGif()) {
        StopStressCycle();
        return false;
    }

    if (!InitBgMusicSession()) {
        StopStressCycle();
        return false;
    }
    s_playback_running = true;
    StartBgMusic();
    ESP_LOGI(TAG, "stress GIF and music playback started");
    return true;
}

void OnScreenUnloaded(lv_event_t* /*e*/) {
    StopStressCycle();
    s_screen = nullptr;
    s_setup_panel = nullptr;
    s_volume_pct_lbl = nullptr;
}

void stress_test_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("stress_test", event);
}

}  // namespace

lv_obj_t* StressTestScreen::Create() {
    ESP_LOGI(TAG, "create stress test screen");

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_screen = scr;
    screen_strip_obj_chrome(scr);
    // Keep setup controls and GIF playback in the panel's native coordinates.
    lv_obj_set_size(scr, kTestPanelW, kTestPanelH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kTestColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    BuildSetupPanel(scr);

    screen_mark_native_layout(scr);
    screen_attach_lifecycle(scr, stress_test_lifecycle_cb);
    screen_attach_swipe_back(scr, OnSwipeBackToMenu);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    return scr;
}
