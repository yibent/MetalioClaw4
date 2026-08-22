#include "agent_ui_runtime.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

#include <esp_random.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "agent_ui/apps/boot/boot_view.h"
#include "agent_ui/apps/ai_image_gen/ai_image_gen_view.h"
#include "agent_ui/apps/codex/codex_view.h"
#include "agent_ui/apps/display_debug/display_debug_view.h"
#include "agent_ui/apps/external_apps/external_apps_view.h"
#include "agent_ui/apps/external_apps/external_app_manager.h"
#include "agent_ui/apps/files/files_view.h"
#include "agent_ui/apps/openclaw/openclaw_view.h"
#include "agent_ui/apps/settings/settings_view.h"
#include "agent_ui/apps/translate/translate_view.h"
#include "agent_ui/apps/home/home_renderer.h"
#include "agent_ui/core/app_mcp_tools.h"
#include "agent_ui/core/idle_power.h"
#include "agent_ui/core/navigation.h"
#include "agent_ui/core/power_key.h"
#include "agent_ui/core/status_bar.h"
#include "agent_ui/core/theme.h"
#include "agent_ui/components/system_keyboard.h"
#include "touch_feed.h"
#include "ui_dispatcher.h"

namespace agent_ui {
namespace {

constexpr char kTag[] = "AgentRuntime";

constexpr std::array<const char*, 8> kGreetings = {{
    "我在，\n随时可以开始。",
    "今天想先完成\n哪件事？",
    "有什么想法，\n随时告诉我。",
    "需要我帮你\n处理什么？",
    "新的任务，\n从一句话开始。",
    "准备好了，\n我们开始吧。",
    "有问题，\n就问我吧。",
    "今天也一起做点\n有意思的事吧。",
}};

const char* RandomGreeting() {
    return kGreetings[esp_random() % kGreetings.size()];
}

}  // namespace

Runtime& Runtime::Get() {
    static Runtime instance;
    return instance;
}

void Runtime::Initialize() {
    if (initialized_) return;
    initialized_ = true;

    const char* initial_message = home_module_.state().message.empty() ? RandomGreeting() : nullptr;
    home_module_.Initialize(
        initial_message,
        [](ScreenId target) { Navigation::Get().Open(target); });

    Theme::Get().Initialize();
    Navigation::Get().Register(ScreenId::Home, CreateHomeView);
    Navigation::Get().Register(ScreenId::Codex, CodexView::Create);
    Navigation::Get().Register(ScreenId::Files, FilesView::Create);
    Navigation::Get().Register(ScreenId::Settings, SettingsView::Create);
    Navigation::Get().Register(ScreenId::OpenClaw, OpenClawView::Create);
    Navigation::Get().Register(ScreenId::AiImageGen, AiImageGenView::Create);
    Navigation::Get().Register(ScreenId::Translate, TranslateView::Create);
    Navigation::Get().Register(ScreenId::ExternalAppHost,
                               external_apps::HostView::Create);
    Navigation::Get().Register(ScreenId::DisplayDebug, DisplayDebugView::Create);
    UiDispatcher::Init();
    RegisterAppMcpTools();
    StatusBar::Get().Initialize();
    StatusBar::Get().SetAgentState(home_module_.state().agent_state);
    StatusBar::Get().SetVisible(false);
    Keyboard::Get().Initialize();
}

void Runtime::OnBoardReady(Board& board) {
    bool expected = false;
    if (!board_ready_posted_.compare_exchange_strong(expected, true)) return;

    Board* board_ptr = &board;
    if (!UiDispatcher::Post([this, board_ptr]() {
            if (power_runtime_started_) return;
            IdlePower::Get().Initialize(*board_ptr);
            PowerKey::Initialize();
            touch_feed_set_activity_callback([]() {
                IdlePower::Get().NotifyActivity();
            });
            power_runtime_started_ = true;
            ESP_LOGI(kTag, "Board ready; runtime power management started");
        })) {
        board_ready_posted_.store(false);
        ESP_LOGE(kTag, "Failed to post board-ready initialization");
    }
}

void Runtime::Start() {
    bool expected = false;
    if (!start_started_.compare_exchange_strong(expected, true)) return;
    if (xTaskCreate(StartTask, "app_install", 8192, this, 4, nullptr) != pdPASS) {
        start_started_.store(false);
        ESP_LOGE(kTag, "Failed to create external app installation task");
        Navigation::Get().Start();
    }
}

void Runtime::StartTask(void* argument) {
    static_cast<Runtime*>(argument)->RunStartTask();
    vTaskDelete(nullptr);
}

void Runtime::RunStartTask() {
    std::string external_apps_error;
    const bool refreshed = external_apps::Manager::Get().Refresh(
        &external_apps_error,
        [](const external_apps::InstallProgress& progress) {
            const uint8_t percent = progress.bytes_total == 0
                                        ? 0
                                        : static_cast<uint8_t>(std::min<uint64_t>(
                                              100,
                                              static_cast<uint64_t>(
                                                  progress.bytes_completed) *
                                                  100U /
                                                  progress.bytes_total));
            UiDispatcher::Post([
                app_name = progress.app_name,
                package_index = progress.package_index,
                package_count = progress.package_count, percent]() {
                BootView::SetInstallProgress(
                    app_name.c_str(), package_index, package_count, percent);
            });
        });

    while (!UiDispatcher::Post(
        [this, refreshed, external_apps_error]() {
            if (!refreshed) {
                ESP_LOGW(kTag, "External app discovery failed: %s",
                         external_apps_error.c_str());
            }
            BootView::SetReady();
            Navigation::Get().Start();
            start_started_.store(false);
        })) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void Runtime::SetAgentState(AgentState state) {
    // MQTT / main_event_loop 不能直接碰 LVGL，否则说话中刷字幕会把 TTS stop 堵死。
    if (!UiDispatcher::Post([this, state]() {
            if (state != AgentState::Idle) IdlePower::Get().NotifyActivity();
            home_module_.HandleEvent(home::Event::AgentStateChanged(state));
            StatusBar::Get().SetAgentState(state);
        })) {
        ESP_LOGW(kTag, "Dropped agent state update");
    }
}

void Runtime::SetConversationMessage(const char* role, const char* content) {
    std::string role_str = role != nullptr ? role : "";
    std::string content_str = content != nullptr ? content : "";
    if (!UiDispatcher::Post([this, role_str = std::move(role_str),
                             content_str = std::move(content_str)]() {
            home_module_.HandleEvent(home::Event::ConversationMessage(
                role_str.c_str(), content_str.c_str()));
        })) {
        ESP_LOGW(kTag, "Dropped conversation message");
    }
}

void Runtime::SetSystemStatus(const char* status) {
    global_state_.system_status = status != nullptr ? status : "";
}

void Runtime::PlayDizzyExpression() {
    home::Renderer::PlayDizzy();
}

lv_obj_t* Runtime::CreateHomeView() {
    return Get().home_module_.Mount();
}

}  // namespace agent_ui
