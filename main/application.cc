#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "ai_provider_config.h"
#include "settings.h"

#include <cstring>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>

#include <ssid_manager.h>
#include <inttypes.h>

#if CONFIG_ESP_HOSTED_ENABLED
#include "esp_hosted.h"
#endif

#ifdef HAVE_LVGL
#include "ota_screen.h"
#include "agent_ui/agent_ui_runtime.h"
#include "agent_ui/core/performance_manager.h"
#include "agent_ui/core/status_bar.h"
#endif

#define TAG "Application"


static const char* const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "audio_testing",
    "fatal_error",
    "invalid_state"
};

Application::Application() {
    event_group_ = xEventGroupCreate();

    // 打断(AEC)默认关；开机后由 ApplyInterruptPreferenceFromNvs() 按 NVS 恢复
    aec_mode_ = kAecOff;

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    CancelVoiceUiHardRelease();
    if (voice_ui_start_retry_timer_ != nullptr) {
        esp_timer_stop(voice_ui_start_retry_timer_);
        esp_timer_delete(voice_ui_start_retry_timer_);
        voice_ui_start_retry_timer_ = nullptr;
    }
    if (voice_ui_release_timer_ != nullptr) {
        esp_timer_delete(voice_ui_release_timer_);
        voice_ui_release_timer_ = nullptr;
    }
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

void Application::CheckAssetsVersion() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();


    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveMode(false);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [display](int progress, size_t speed) -> void {
            std::thread([display, progress, speed]() {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                display->SetChatMessage("system", buffer);
            }).detach();
        });

        board.SetPowerSaveMode(true);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

namespace {

bool HasCachedProtocolConfig() {
    Settings mqtt("mqtt", false);
    if (!mqtt.GetString("endpoint").empty()) {
        return true;
    }
    Settings websocket("websocket", false);
    return !websocket.GetString("url").empty();
}

bool HasOtaUpdatePartition() {
    return esp_ota_get_next_update_partition(nullptr) != nullptr;
}

#ifdef HAVE_LVGL
void SetBootTransferDemand(bool active) {
    agent_ui::PerformanceManager::Get().SetDemand(
        agent_ui::PerformanceDemand::Transfer, active);
}
#else
void SetBootTransferDemand(bool /*active*/) {}
#endif

}  // namespace

void Application::CheckNewVersion(Ota& ota) {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // 初始重试延迟为10秒

    auto& board = Board::GetInstance();
    SetBootTransferDemand(true);
    while (true) {
        SetDeviceState(kDeviceStateActivating);
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota.CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            // 已有协议缓存时不要把对话堵在 OTA 重试上：协议可以先起来。
            if (HasCachedProtocolConfig()) {
                ESP_LOGW(TAG,
                         "OTA check failed (err=%d), using cached protocol config",
                         static_cast<int>(err));
                SetBootTransferDemand(false);
                return;
            }
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                SetBootTransferDemand(false);
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota.GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            // Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (device_state_ == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // 每次重试后延迟时间翻倍
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // 重置重试延迟时间

        if (ota.HasNewVersion()) {
            // 单分区（无 ota_1）不能写固件。跳过升级，继续用这次响应里的
            // MQTT/WebSocket 配置，避免 OTA 界面/下载卡住后续启动。
            if (!HasOtaUpdatePartition()) {
                ESP_LOGW(TAG,
                         "New firmware %s available, skipped (no OTA update partition)",
                         ota.GetFirmwareVersion().c_str());
            } else if (UpgradeFirmware(ota)) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation (don't break, just fall through)
        }

        // No new version, mark the current version as valid
        // ota.MarkCurrentVersionValid();
        if (!ota.HasActivationCode() && !ota.HasActivationChallenge()) {
            xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
            // Exit the loop if done checking new version
            SetBootTransferDemand(false);
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota.HasActivationCode()) {
            ShowActivationCode(ota.GetActivationCode(), ota.GetActivationMessage());
        }

        // 最多试一轮激活。成功则再拉一次配置拿 MQTT；失败也继续开机，
        // 验证码留在状态栏，避免 while(true) 把协议初始化卡死。
        bool activated = false;
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota.Activate();
            if (err == ESP_OK) {
                pending_activation_code_.clear();
#ifdef HAVE_LVGL
                agent_ui::StatusBar::Get().RefreshAsync();
#endif
                xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
                activated = true;
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (device_state_ == kDeviceStateIdle) {
                break;
            }
        }
        if (activated) {
            continue;
        }
        ESP_LOGW(TAG, "Activation not finished, continuing boot");
        xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
        break;
    }
    SetBootTransferDemand(false);
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    // OTA 激活：仅缓存验证码供状态栏展示，不 Alert、不播报数字音。
    pending_activation_code_ = code;
#ifdef HAVE_LVGL
    agent_ui::StatusBar::Get().RefreshAsync();
#endif
    ESP_LOGI(TAG, "Activation code ready for status bar (no TTS): %s (%s)",
             code.c_str(), message.c_str());
}

bool Application::IsDeviceActivated() const {
    // starting / 联网 / OTA 中：尚未标记 boot_ready_
    if (!boot_ready_) {
        return false;
    }
    // 仍有激活码待绑定
    if (HasPendingActivation()) {
        return false;
    }
    // 激活流程进行中（含中途被切到其它态前的 activating）
    if (device_state_ == kDeviceStateActivating) {
        return false;
    }
    return true;
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (device_state_ == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

bool Application::ToggleChatState() {
    if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return true;
    } else if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return true;
    }

    if (!voice_ui_desired_ && !voice_ui_active_) {
        ESP_LOGW(TAG, "ToggleChatState ignored: voice UI session inactive");
        return false;
    }

    const bool in_conversation =
        voice_chat_requested_ ||
        pending_voice_ui_listen_ ||
        device_state_ == kDeviceStateConnecting ||
        device_state_ == kDeviceStateListening ||
        device_state_ == kDeviceStateSpeaking;
    if (in_conversation) {
        CancelVoiceSession();
        return true;
    }

    // 主页在 Wi-Fi / OTA / MQTT 完成前就能下滑。协议未就绪时排队，
    // 等 Start() 末尾再开通道；不要把下滑当成取消激活（否则 state 被打成
    // Idle，下一次就会误报 Protocol not initialized）。
    abort_voice_session_ = false;
    voice_chat_requested_ = true;
    if (!protocol_ || !boot_ready_ ||
        device_state_ == kDeviceStateStarting ||
        device_state_ == kDeviceStateActivating) {
        pending_voice_ui_listen_ = true;
        ESP_LOGI(TAG, "ToggleChatState queued until protocol is ready (state=%s protocol=%d boot=%d)",
                 STATE_STRINGS[device_state_], protocol_ ? 1 : 0, boot_ready_ ? 1 : 0);
        return true;
    }

    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (abort_voice_session_.load() || !voice_ui_desired_ || !protocol_) {
                pending_voice_ui_listen_ = false;
                return;
            }
            if (!voice_ui_active_) {
                ApplyVoiceUiStart();
                if (!voice_ui_active_) {
                    pending_voice_ui_listen_ = true;
                    ESP_LOGI(TAG, "ToggleChatState queued until voice UI session starts");
                    return;
                }
            }
            StartVoiceChatFromIdle();
        });
    }
    return true;
}

void Application::StartListening() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!voice_ui_active_) {
        ESP_LOGW(TAG, "StartListening ignored: voice UI session inactive");
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (abort_voice_session_.load() || !voice_ui_active_ || !protocol_) {
                return;
            }
            if (!OpenVoiceChannelOrIdle()) {
                return;
            }

            SetListeningMode(kListeningModeManualStop);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
            SetListeningMode(kListeningModeManualStop);
        });
    }
}

void Application::StopListening() {
    if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    const std::array<int, 3> valid_states = {
        kDeviceStateListening,
        kDeviceStateSpeaking,
        kDeviceStateIdle,
    };
    // If not valid, do nothing
    if (std::find(valid_states.begin(), valid_states.end(), device_state_) == valid_states.end()) {
        return;
    }

    Schedule([this]() {
        if (device_state_ == kDeviceStateListening) {
            protocol_->SendStopListening();
            SetDeviceState(kDeviceStateIdle);
        }
    });
}

void Application::Start() {
    auto& board = Board::GetInstance();
#ifdef HAVE_LVGL
    agent_ui::Runtime::Get().OnBoardReady(board);
#endif
    SetDeviceState(kDeviceStateStarting);

    /* Setup the display */
    auto display = board.GetDisplay();

    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    /* Setup the audio service */
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // OTA URL：NVS(wifi/ota_url) 为空则写入默认地址，非空则沿用 NVS
    {
        static constexpr const char* kDefaultOtaUrl = "https://api.tenclass.net/xiaozhi/ota/";
        Settings settings("wifi", true);
        std::string ota_url = settings.GetString("ota_url");
        if (ota_url.empty()) {
            ota_url = kDefaultOtaUrl;
            settings.SetString("ota_url", ota_url);
            ESP_LOGI(TAG, "OTA URL empty in NVS, wrote default: %s", ota_url.c_str());
        } else {
            ESP_LOGI(TAG, "OTA URL from NVS: %s", ota_url.c_str());
        }
    }

    // Start the main event loop task with priority 3
    xTaskCreate([](void* arg) {
        ((Application*)arg)->MainEventLoop();
        vTaskDelete(NULL);
    }, "main_event_loop", 2048 * 4, this, 3, &main_event_loop_task_handle_);

    /* Start the clock timer to update the status bar */
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    //直接校验OTA
    Ota ota;
    ota.MarkCurrentVersionValid();

// #if CONFIG_ESP_HOSTED_ENABLED
//     /* Boot-time probe: C5 ESP-Hosted slave (WiFi coprocessor) present? */
//     ESP_LOGI(TAG, "C5 hosted check: connecting to slave...");
//     if (esp_hosted_connect_to_slave() == ESP_OK) {
//         esp_hosted_coprocessor_fwver_t fwver{};
//         uint32_t chip_id = 0;
//         char target_name[32] = {0};
//         if (esp_hosted_get_coprocessor_fwversion(&fwver) == ESP_OK) {
//             ESP_LOGI(TAG,
//                      "C5 hosted check: OK — FW %" PRIu32 ".%" PRIu32 ".%" PRIu32
//                      " (rev=%" PRId32 ")",
//                      fwver.major1, fwver.minor1, fwver.patch1, fwver.revision);
//         } else {
//             ESP_LOGW(TAG, "C5 hosted check: transport up, but fwversion RPC failed");
//         }
//         if (esp_hosted_get_cp_info(&chip_id, target_name, sizeof(target_name)) == ESP_OK) {
//             ESP_LOGI(TAG, "C5 hosted check: chip_id=0x%" PRIx32 " target=%s",
//                      chip_id, target_name[0] ? target_name : "(n/a)");
//         }
//     } else {
//         ESP_LOGE(TAG,
//                  "C5 hosted check: FAIL — slave not reachable "
//                  "(no hosted FW / SDIO / reset?)");
//         PlaySound(Lang::Sounds::OGG_ERR_REG);
//     }
// #endif

    /* Wait for the network to be ready */
    SetBootTransferDemand(true);
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);

   
    // Check for new assets version
    // CheckAssetsVersion();

    // 拉取 MQTT/WebSocket 配置与激活。单分区没有 OTA 槽时 CheckNewVersion
    // 会跳过固件升级，不挡住后续协议启动。
    CheckNewVersion(ota);
    //加载唤醒词模型
    GetAudioService().SetModelsList(esp_srmodel_init("model"));
    GetAudioService().EnableWakeWordDetection(false);
    // 模型就绪后再同步打断(AEC)偏好，避免处理器尚未初始化
    ApplyInterruptPreferenceFromNvs();

    // Initialize the protocol
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    // Add MCP common tools before initializing the protocol
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    if (ota.HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota.HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (device_state_ == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveMode(false);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveMode(true);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening) {
                        SetDeviceState(kDeviceStateSpeaking);
                    }
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (device_state_ == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    bool protocol_started = protocol_->Start();

    SystemInfo::PrintHeapStats();
    // 启动流水线完成（联网/OTA/激活/协议）后才标记就绪；勿把 starting 或中途 Idle 当已激活
    boot_ready_ = true;
    SetBootTransferDemand(false);
    SetDeviceState(kDeviceStateIdle);
    // 唤醒词由主页语音会话接管。启动流水线结束时若 Home 已 desired，
    // 再同步一次，并兑现开机期间排队的下滑聊天。
    if (voice_ui_desired_) {
        Schedule([this]() {
            SyncVoiceUiSession();
            FlushPendingVoiceUiListen();
        });
    } else {
        audio_service_.EnableWakeWordDetection(false);
        pending_voice_ui_listen_ = false;
    }

    has_server_time_ = false;
    if (protocol_started) {
        std::string message = std::string(Lang::Strings::VERSION) + ota.GetCurrentVersion();
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
        // Play the success sound to indicate the device is ready
        // audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    }
}

// Add a async task to MainLoop
void Application::Schedule(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

// The Main Event Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainEventLoop() {
    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, MAIN_EVENT_SCHEDULE |
            MAIN_EVENT_SEND_AUDIO |
            MAIN_EVENT_WAKE_WORD_DETECTED |
            MAIN_EVENT_VAD_CHANGE |
            MAIN_EVENT_CLOCK_TICK |
            MAIN_EVENT_ERROR, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            if (voice_ui_active_) {
                OnWakeWordDetected();
            }
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (device_state_ == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
        
            // Print the debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
                // SystemInfo::PrintTaskList();
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

void Application::OnWakeWordDetected() {
    if (!voice_ui_active_ || !protocol_) {
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!OpenVoiceChannelOrIdle()) {
            if (voice_ui_active_) {
                audio_service_.EnableWakeWordDetection(true);
            }
            return;
        }

        auto wake_word = audio_service_.GetLastWakeWord();
        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected("Hi 钛灵");
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        // Play the pop up sound to indicate the wake word is detected
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
#endif
    } else if (device_state_ == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
    } else if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

void Application::SetDeviceState(DeviceState state) {
    if (device_state_ == state) {
        return;
    }
    
    clock_ticks_ = 0;
    auto previous_state = device_state_;
    device_state_ = state;
    ESP_LOGI(TAG, "STATE: %s", STATE_STRINGS[device_state_]);

    // Send the state change event
    DeviceStateEventManager::GetInstance().PostStateChangeEvent(previous_state, state);

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            voice_chat_requested_ = false;
            abort_voice_session_ = false;
            pending_voice_ui_listen_ = false;
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
            audio_service_.EnableVoiceProcessing(false);
            // 离开主页只软停。Idle 里硬 destroy 会在回设置/主页时重建 AFE，
            // ESP32-P4 上 INTERNAL 回落到 LP SRAM，PIE 卷积 Store access fault。
            if (voice_ui_active_) {
                audio_service_.EnableWakeWordDetection(true);
            } else {
                audio_service_.EnableWakeWordDetection(false);
            }
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (!audio_service_.IsAudioProcessorRunning()) {
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
                audio_service_.EnableWakeWordDetection(false);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(
                    voice_ui_active_ && audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // 重启前关背光，避免过渡花屏/蓝屏；不写 NVS，下次启动仍按原亮度恢复。
    if (Backlight* bl = Board::GetInstance().GetBacklight()) {
        bl->SetBrightness(0, false);
    }
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    // 等待背光渐暗（SetBrightness 约 5ms/级）后再重启
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(Ota& /*ota*/, const std::string& /*url*/) {
    // 启动路径不会走到这里（无 OTA 槽时 CheckNewVersion 已跳过）。
    // 手动触发时也直接拒绝，避免下载固件后无法落盘而卡住。
    ESP_LOGW(TAG, "Firmware upgrade refused (no OTA update partition)");
    Alert(Lang::Strings::ERROR, "OTA 已禁用", "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
    return false;
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!voice_ui_active_ || !protocol_) {
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!OpenVoiceChannelOrIdle()) {
            if (voice_ui_active_) {
                audio_service_.EnableWakeWordDetection(true);
            }
            return;
        }

        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_USE_AFE_WAKE_WORD || CONFIG_USE_CUSTOM_WAKE_WORD
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        // Play the pop up sound to indicate the wake word is detected
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
#endif
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (device_state_ != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    if (protocol_ == nullptr) {
        return;
    }

    // Make sure you are using main thread to send MCP message
    if (xTaskGetCurrentTaskHandle() == main_event_loop_task_handle_) {
        protocol_->SendMcpMessage(payload);
    } else {
        Schedule([this, payload = std::move(payload)]() {
            protocol_->SendMcpMessage(payload);
        });
    }
}

void Application::SetAecMode(AecMode mode) {
    if (mode != kAecOff && mode != kAecOnDeviceSide) {
        ESP_LOGW(TAG, "unsupported AecMode %d, fallback Off", static_cast<int>(mode));
        mode = kAecOff;
    }

    aec_mode_ = mode;
    {
        // NVS 键最长 15 字符；interrupt 记忆「是否开启打断」
        Settings settings("audio", true);
        settings.SetBool("interrupt", aec_mode_ != kAecOff);
    }

    // 同步写入偏好，避免 Schedule 前已按 Realtime 进聆听却尚未 EnableDeviceAec
    audio_service_.EnableDeviceAec(aec_mode_ == kAecOnDeviceSide);

    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        if (aec_mode_ == kAecOnDeviceSide) {
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
        } else {
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::ApplyInterruptPreferenceFromNvs() {
    Settings settings("audio");
    // 无记录时默认关打断，避免上电即全双工
    const bool interrupt_on = settings.GetBool("interrupt", false);
    aec_mode_ = interrupt_on ? kAecOnDeviceSide : kAecOff;
    audio_service_.EnableDeviceAec(interrupt_on);
    ESP_LOGI(TAG, "interrupt preference: %s (device AEC)", interrupt_on ? "on" : "off");
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::SoftStopVoiceAudioPaths() {
    audio_service_.EnableWakeWordDetection(false);
    audio_service_.EnableVoiceProcessing(false);
}

void Application::TearDownVoiceAudioPaths(bool release_wake_word) {
    SoftStopVoiceAudioPaths();
    if (release_wake_word) {
        audio_service_.ReleaseWakeWordEngine();
    }
}

void Application::CancelVoiceUiHardRelease() {
    if (voice_ui_release_timer_ != nullptr) {
        esp_timer_stop(voice_ui_release_timer_);
    }
}

void Application::ScheduleVoiceUiHardRelease(uint32_t epoch) {
    CancelVoiceUiHardRelease();
    voice_ui_pending_release_epoch_ = epoch;

    if (voice_ui_release_timer_ == nullptr) {
        esp_timer_create_args_t args = {
            .callback =
                [](void* arg) {
                    auto* app = static_cast<Application*>(arg);
                    const uint32_t epoch = app->voice_ui_pending_release_epoch_;
                    app->Schedule([app, epoch]() {
                        if (app->voice_ui_desired_ || app->voice_ui_epoch_ != epoch) {
                            return;
                        }
                        ESP_LOGI(TAG, "voice UI delayed hard release (epoch=%" PRIu32 ")", epoch);
                        app->ApplyVoiceUiStop();
                    });
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "voice_ui_rel",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&args, &voice_ui_release_timer_);
    }

    // 停 Feed 后短暂保留 AFE：快速再进语音页可复用，并让旧屏内存先释放。
    constexpr uint64_t kReleaseDelayUs = 1200 * 1000;
    esp_timer_start_once(voice_ui_release_timer_, kReleaseDelayUs);
}

void Application::ScheduleVoiceUiStartRetry(uint32_t epoch) {
    voice_ui_pending_retry_epoch_ = epoch;
    if (voice_ui_start_retry_timer_ == nullptr) {
        esp_timer_create_args_t args = {
            .callback =
                [](void* arg) {
                    auto* app = static_cast<Application*>(arg);
                    const uint32_t epoch = app->voice_ui_pending_retry_epoch_;
                    app->Schedule([app, epoch]() {
                        if (!app->voice_ui_desired_ || app->voice_ui_epoch_ != epoch) {
                            return;
                        }
                        app->ApplyVoiceUiStart();
                    });
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "voice_ui_retry",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&args, &voice_ui_start_retry_timer_);
    } else {
        esp_timer_stop(voice_ui_start_retry_timer_);
    }
    esp_timer_start_once(voice_ui_start_retry_timer_, 200 * 1000);
}

bool Application::HeapOkForWakeWordCreate() const {
    // ESP32-P4 PIE 卷积只能安全访问 HP SRAM / PSRAM。MALLOC_CAP_INTERNAL 含
    // LP SRAM（约 32KB @ 0x50108000），总空闲够用时仍可能把 scratch 分到 LP，
    // 随后 dl_esp32p4_sr_atrous_conv1d 触发 Load access fault。
    const size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
    const size_t largest_dma =
        heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    constexpr size_t kMinFreeDma = 48 * 1024;
    constexpr size_t kMinLargestDma = 16 * 1024;
    constexpr size_t kMinPsram = 256 * 1024;
    if (free_dma < kMinFreeDma || largest_dma < kMinLargestDma ||
        free_psram < kMinPsram) {
        ESP_LOGW(TAG,
                 "Defer wake word init (dma_free=%uKB dma_largest=%uKB psram=%uKB)",
                 (unsigned)(free_dma / 1024), (unsigned)(largest_dma / 1024),
                 (unsigned)(free_psram / 1024));
        return false;
    }
    return true;
}

bool Application::TryEnableWakeWordForVoiceUi() {
    Settings wake_pref(std::string(ai_provider_config::kNamespace), false);
    if (wake_pref.GetInt("wake", 1) == 0) {
        audio_service_.EnableWakeWordDetection(false);
        return true;
    }

    if (audio_service_.IsWakeWordEngineReady()) {
        audio_service_.EnableWakeWordDetection(true);
        return audio_service_.IsWakeWordEngineReady();
    }

    if (voice_ui_engine_not_before_us_ != 0 &&
        esp_timer_get_time() < voice_ui_engine_not_before_us_) {
        ESP_LOGD(TAG, "Defer wake word init until outgoing UI is released");
        return false;
    }

    if (!HeapOkForWakeWordCreate()) {
        return false;
    }

    audio_service_.EnableWakeWordDetection(true);
    if (!audio_service_.IsWakeWordEngineReady()) {
        ESP_LOGW(TAG, "Wake word init failed, will retry");
        return false;
    }
    return true;
}

void Application::NotifyUiTransitionFinished() {
    voice_ui_engine_not_before_us_ = 0;
    if (!voice_ui_desired_) {
        return;
    }
    Schedule([this]() { SyncVoiceUiSession(); });
}

void Application::RequestVoiceEngineHardRelease() {
    if (voice_ui_desired_) {
        return;
    }
    ScheduleVoiceUiHardRelease(voice_ui_epoch_);
}

void Application::SetVoiceUiDesired(bool desired) {
    if (voice_ui_desired_ == desired) {
        ESP_LOGD(TAG, "voice UI desired unchanged (%d)", desired ? 1 : 0);
        return;
    }
    voice_ui_desired_ = desired;
    ++voice_ui_epoch_;
    const uint32_t epoch = voice_ui_epoch_;
    ESP_LOGI(TAG, "voice UI desired -> %d (epoch=%" PRIu32 ")", desired ? 1 : 0, epoch);

    if (!desired) {
        // 立刻软停。设置等轻量页不硬 destroy，回主页复用同一份 AFE。
        pending_voice_ui_listen_ = false;
        voice_ui_active_ = false;
        voice_ui_engine_not_before_us_ = 0;
        SoftStopVoiceAudioPaths();
        if (voice_ui_start_retry_timer_ != nullptr) {
            esp_timer_stop(voice_ui_start_retry_timer_);
        }
        Schedule([this]() { SyncVoiceUiSession(); });
        return;
    }

    CancelVoiceUiHardRelease();
    if (!audio_service_.IsWakeWordEngineReady()) {
        // 仅在引擎已被硬释放后重建。等旧屏 delete，避免双屏峰值抢 HP SRAM。
        constexpr int64_t kUiReleaseFallbackUs = 400 * 1000;
        voice_ui_engine_not_before_us_ = esp_timer_get_time() + kUiReleaseFallbackUs;
    }
    Schedule([this]() { SyncVoiceUiSession(); });
}

void Application::ParkVoiceUiProtocol() {
    if (device_state_ == kDeviceStateListening && protocol_) {
        protocol_->CloseAudioChannel();
    } else if (device_state_ == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    } else if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }

    if (device_state_ == kDeviceStateListening ||
        device_state_ == kDeviceStateSpeaking ||
        device_state_ == kDeviceStateConnecting) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::SyncVoiceUiSession() {
    if (voice_ui_desired_) {
        ApplyVoiceUiStart();
        // Start 中途又 leave：禁止留下唤醒词
        if (!voice_ui_desired_) {
            SoftStopVoiceAudioPaths();
            voice_ui_active_ = false;
            ScheduleVoiceUiHardRelease(voice_ui_epoch_);
        }
    } else {
        // 硬释放交给延迟定时器；这里只停协议，避免抵消 debounce
        voice_ui_active_ = false;
        SoftStopVoiceAudioPaths();
        ParkVoiceUiProtocol();
    }
}

void Application::ApplyVoiceUiStart() {
    if (!voice_ui_desired_) {
        SoftStopVoiceAudioPaths();
        voice_ui_active_ = false;
        ESP_LOGI(TAG, "ApplyVoiceUiStart skipped (desired=0)");
        return;
    }

    const uint32_t epoch = voice_ui_epoch_;

    if (voice_ui_active_) {
        if (device_state_ == kDeviceStateIdle) {
            audio_service_.EnableVoiceProcessing(false);
            if (!TryEnableWakeWordForVoiceUi()) {
                ScheduleVoiceUiStartRetry(epoch);
                FlushPendingVoiceUiListen();
                return;
            }
        }
        ESP_LOGI(TAG, "voice UI already active");
        FlushPendingVoiceUiListen();
        return;
    }

    ESP_LOGI(TAG, "ApplyVoiceUiStart");
    voice_ui_active_ = true;

    if (device_state_ == kDeviceStateIdle) {
        audio_service_.EnableVoiceProcessing(false);
        if (!TryEnableWakeWordForVoiceUi()) {
            voice_ui_active_ = false;
            ScheduleVoiceUiStartRetry(epoch);
            return;
        }
        if (!voice_ui_desired_) {
            SoftStopVoiceAudioPaths();
            voice_ui_active_ = false;
            return;
        }
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
    } else if (device_state_ != kDeviceStateActivating &&
               device_state_ != kDeviceStateStarting &&
               device_state_ != kDeviceStateWifiConfiguring &&
               device_state_ != kDeviceStateAudioTesting &&
               device_state_ != kDeviceStateUpgrading) {
        SetDeviceState(kDeviceStateIdle);
    }
    FlushPendingVoiceUiListen();
}

void Application::StartVoiceChatFromIdle() {
    if (abort_voice_session_.load() || !voice_ui_active_ || !voice_ui_desired_ ||
        !protocol_) {
        return;
    }
    if (device_state_ != kDeviceStateIdle) {
        return;
    }
    if (!OpenVoiceChannelOrIdle()) {
        return;
    }
    SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
}

void Application::FlushPendingVoiceUiListen() {
    if (!pending_voice_ui_listen_ || !voice_ui_active_ || !voice_ui_desired_) {
        return;
    }
    if (abort_voice_session_.load() || device_state_ != kDeviceStateIdle) {
        return;
    }
    pending_voice_ui_listen_ = false;
    StartVoiceChatFromIdle();
}

void Application::ApplyVoiceUiStop() {
    voice_ui_active_ = false;
    TearDownVoiceAudioPaths(true);
    ParkVoiceUiProtocol();
    SoftStopVoiceAudioPaths();
    ESP_LOGI(TAG, "ApplyVoiceUiStop: AFE destroyed, protocol parked");
}

void Application::CancelVoiceSession() {
    ESP_LOGI(TAG, "Cancel voice session (state=%s)", STATE_STRINGS[device_state_]);
    pending_voice_ui_listen_ = false;
    voice_chat_requested_ = false;
    abort_voice_session_ = true;
    if (protocol_) {
        protocol_->CancelOpenAudioChannel();
    }
    Schedule([this]() { EndVoiceSessionToIdle(); });
}

void Application::EndVoiceSessionToIdle() {
    pending_voice_ui_listen_ = false;
    voice_chat_requested_ = false;

    if (device_state_ == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (device_state_ == kDeviceStateListening && protocol_) {
        protocol_->SendStopListening();
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    audio_service_.ResetDecoder();
    if (device_state_ == kDeviceStateConnecting ||
        device_state_ == kDeviceStateListening ||
        device_state_ == kDeviceStateSpeaking) {
        SetDeviceState(kDeviceStateIdle);
    } else {
        abort_voice_session_ = false;
    }
}

bool Application::OpenVoiceChannelOrIdle() {
    if (abort_voice_session_.load()) {
        EndVoiceSessionToIdle();
        return false;
    }
    voice_chat_requested_ = true;
    if (protocol_->IsAudioChannelOpened()) {
        return true;
    }
    SetDeviceState(kDeviceStateConnecting);
    const bool opened =
        protocol_->OpenAudioChannel() && !abort_voice_session_.load();
    if (!opened) {
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        if (device_state_ == kDeviceStateConnecting) {
            SetDeviceState(kDeviceStateIdle);
        }
        return false;
    }
    return true;
}

void Application::ForceReturnToIdle() {
    CancelVoiceSession();
}

void Application::SetLowPowerStandby(bool enabled) {
    low_power_standby_.store(enabled);
}
