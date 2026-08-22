#include "lua_agent_screen.h"
#include "i18n.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "api_endpoints.h"
#include "board.h"
#include "config.h"
#include "home_screen/home_screen.h"
#include "lua_runtime.h"
#include "screen_util.h"
#include "settings.h"
#include "system_info.h"
#include <web_socket.h>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "LuaAgent";
constexpr int kProtocolVersion = 1;
constexpr uint32_t kDefaultTimeoutMs = 30000;
constexpr uint32_t kMaxTimeoutMs = 600000;
constexpr size_t kWsReceiveBytes = 80 * 1024;
constexpr size_t kMaxArgsJson = 16 * 1024;
constexpr int kQueueLength = 4;
constexpr uint32_t kPingIntervalMs = 20000;
constexpr uint32_t kRecvTimeoutMs = 60000;
constexpr uint32_t kUiRefreshMs = 200;
constexpr uint32_t kBackoffMinMs = 1000;
constexpr uint32_t kBackoffMaxMs = 15000;
constexpr int kWorkerStack = 32 * 1024;
constexpr int kIdMax = 64;
constexpr int kEntryMax = 32;

constexpr int32_t kPanelW = DISPLAY_WIDTH;
constexpr int32_t kPanelH = DISPLAY_HEIGHT;
constexpr int32_t kHeaderH = 90;
constexpr int32_t kBackBtnSize = 72;
constexpr int32_t kPad = 16;

constexpr uint32_t kColorBg = 0x0E1116;
constexpr uint32_t kColorCard = 0x1B2030;
constexpr uint32_t kColorText = 0xFFFFFF;
constexpr uint32_t kColorHint = 0x9AA3B2;
constexpr uint32_t kColorWaiting = 0x34D399;
constexpr uint32_t kColorConnecting = 0xFBBF24;
constexpr uint32_t kColorRunning = 0x60A5FA;
constexpr uint32_t kColorError = 0xF87171;
constexpr uint32_t kColorReconnect = 0xFB923C;

enum class ConnState {
    Idle,
    Connecting,
    Waiting,
    Running,
    Reconnecting,
    Error,
};

struct IncomingMsg {
    char* text;
};

struct UiSnapshot {
    ConnState state = ConnState::Idle;
    std::string status;
    std::string url;
    std::string job_id;
    std::string result;
    std::string output;
    std::string detail;
};

lv_obj_t* s_screen;
lv_obj_t* s_status_label;
lv_obj_t* s_url_label;
lv_obj_t* s_job_label;
lv_obj_t* s_result_label;
lv_obj_t* s_output_label;
lv_timer_t* s_ui_timer;
screen_lifecycle_cb_t s_lifecycle_cb;

std::mutex s_ws_mutex;
std::unique_ptr<WebSocket> s_ws;
std::mutex s_ui_mutex;
std::mutex s_inbound_mutex;
UiSnapshot s_snap;

std::atomic<uint32_t> s_session{0};
std::atomic<bool> s_stop{false};
std::atomic<bool> s_worker_running{false};
std::atomic<bool> s_connected{false};
std::atomic<bool> s_job_active{false};

QueueHandle_t s_msg_queue;
lua_runtime_job_id_t s_job_id;
char s_current_req_id[kIdMax];
int64_t s_job_started_us;
std::string s_url;
std::string s_token;

bool SessionAlive(uint32_t session) {
    return session == s_session.load(std::memory_order_acquire) &&
           !s_stop.load(std::memory_order_acquire);
}

void SetSnap(ConnState state, const char* status_msgid, const std::string& detail = std::string()) {
    std::lock_guard<std::mutex> lock(s_ui_mutex);
    s_snap.state = state;
    s_snap.status = I18n::T(status_msgid);
    if (!detail.empty())
        s_snap.detail = detail;
}

void SetJobFields(const char* job_id, const char* result, const char* output) {
    std::lock_guard<std::mutex> lock(s_ui_mutex);
    if (job_id)
        s_snap.job_id = job_id;
    if (result)
        s_snap.result = result;
    if (output)
        s_snap.output = output;
}

std::string Truncate(const std::string& text, size_t max_chars) {
    if (text.size() <= max_chars)
        return text;
    return text.substr(0, max_chars) + "...";
}

std::string GetAgentUrl() {
    Settings settings("lua_agent", false);
    std::string url = settings.GetString("url");
    if (!url.empty())
        return url;
#ifdef CONFIG_LUA_AGENT_WS_URL
    if (CONFIG_LUA_AGENT_WS_URL[0] != '\0')
        return CONFIG_LUA_AGENT_WS_URL;
#endif
    return api::LuaAgentWsUrl();
}

std::string GetAgentToken() {
    Settings settings("lua_agent", false);
    return settings.GetString("token");
}

void CloseWebSocket() {
    std::lock_guard<std::mutex> lock(s_ws_mutex);
    if (s_ws) {
        s_ws->Close();
        s_ws.reset();
    }
    s_connected.store(false, std::memory_order_release);
}

bool SendJson(cJSON* root) {
    if (!root)
        return false;
    char* printed = cJSON_PrintUnformatted(root);
    if (!printed)
        return false;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(s_ws_mutex);
        if (s_ws && s_ws->IsConnected())
            ok = s_ws->Send(printed);
    }
    cJSON_free(printed);
    return ok;
}

void SendError(const char* id, const char* code, const char* message) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", kProtocolVersion);
    cJSON_AddStringToObject(root, "type", "error");
    if (id && id[0])
        cJSON_AddStringToObject(root, "id", id);
    cJSON* error = cJSON_CreateObject();
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message ? message : "");
    cJSON_AddItemToObject(root, "error", error);
    SendJson(root);
    cJSON_Delete(root);
}

cJSON* BuildHelloDevice() {
    cJSON* device = cJSON_CreateObject();
    auto& board = Board::GetInstance();
    const auto* app_desc = esp_app_get_description();
    cJSON_AddStringToObject(device, "uuid", board.GetUuid().c_str());
    cJSON_AddStringToObject(device, "mac", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(device, "board", BOARD_NAME);
    cJSON_AddStringToObject(device, "chip", SystemInfo::GetChipModelName().c_str());
    cJSON_AddStringToObject(device, "firmware", app_desc->version);
    cJSON_AddStringToObject(device, "idf", app_desc->idf_ver);
    cJSON_AddStringToObject(device, "language", I18n::GetLocaleCode());
    cJSON_AddNumberToObject(device, "flash_size", (double)SystemInfo::GetFlashSize());
    cJSON_AddNumberToObject(device, "heap_free", (double)SystemInfo::GetFreeHeapSize());

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    cJSON_AddNumberToObject(device, "cores", chip_info.cores);

    int battery = 0;
    bool charging = false;
    bool discharging = false;
    if (board.GetBatteryLevel(battery, charging, discharging)) {
        cJSON* bat = cJSON_CreateObject();
        cJSON_AddNumberToObject(bat, "level", battery);
        cJSON_AddBoolToObject(bat, "charging", charging);
        cJSON_AddItemToObject(device, "battery", bat);
    }

    cJSON* lua = cJSON_CreateObject();
    cJSON_AddNumberToObject(lua, "max_code_bytes", 64 * 1024);
    cJSON_AddNumberToObject(lua, "max_output_bytes", 4 * 1024);
    cJSON_AddNumberToObject(lua, "max_result_bytes", (double)LUA_RUNTIME_RESULT_SIZE);
    cJSON* caps = cJSON_CreateArray();
    cJSON_AddItemToArray(caps, cJSON_CreateString("lua"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("ui"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("audio"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("http"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("speech"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("device"));
    if (board.GetCamera() != nullptr)
        cJSON_AddItemToArray(caps, cJSON_CreateString("camera"));
    cJSON_AddItemToObject(lua, "capabilities", caps);
    cJSON_AddItemToObject(device, "lua", lua);
    return device;
}

std::string BootId() {
    static std::string id;
    if (!id.empty())
        return id;
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3f) | 0x80);
    char text[37];
    snprintf(text, sizeof(text),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0],
             bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8],
             bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    id = text;
    return id;
}

bool SendHello() {
    char id[32];
    snprintf(id, sizeof(id), "h-%08lx", (unsigned long)esp_random());
    auto& board = Board::GetInstance();
    const auto* app_desc = esp_app_get_description();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", kProtocolVersion);
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddStringToObject(root, "id", id);
    cJSON_AddStringToObject(root, "protocol", "lua-agent");
    cJSON_AddNumberToObject(root, "ts_ms", (double)(esp_timer_get_time() / 1000));
    cJSON_AddStringToObject(root, "ts", "1970-01-01T00:00:00.000Z");
    cJSON* device = BuildHelloDevice();
    cJSON_AddItemToObject(root, "device", device);

    cJSON* data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "protocol", "lua-agent");
    cJSON_AddStringToObject(data, "device_id", board.GetUuid().c_str());
    cJSON_AddStringToObject(data, "boot_id", BootId().c_str());
    cJSON_AddStringToObject(data, "firmware_version", app_desc->version);
    cJSON_AddStringToObject(data, "lua_runtime", "claw4");
    cJSON* caps = cJSON_CreateArray();
    cJSON_AddItemToArray(caps, cJSON_CreateString("lua"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("ui"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("audio"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("http"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("speech"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("device"));
    if (board.GetCamera() != nullptr)
        cJSON_AddItemToArray(caps, cJSON_CreateString("camera"));
    cJSON_AddItemToObject(data, "capabilities", caps);
    cJSON* limits = cJSON_CreateObject();
    cJSON_AddNumberToObject(limits, "max_script_bytes", 64 * 1024);
    cJSON_AddNumberToObject(limits, "max_params_bytes", 16 * 1024);
    cJSON_AddNumberToObject(limits, "max_chunk_bytes", 64 * 1024);
    cJSON_AddNumberToObject(limits, "max_message_bytes", (double)kWsReceiveBytes);
    cJSON_AddNumberToObject(limits, "max_log_bytes", 1024);
    cJSON_AddItemToObject(data, "limits", limits);
    cJSON* runtime = cJSON_CreateObject();
    cJSON_AddStringToObject(runtime, "execution_model", "main_once");
    cJSON_AddStringToObject(runtime, "api_version", "claw4.v1");
    cJSON_AddStringToObject(runtime, "transfer_storage", "ram");
    cJSON_AddNumberToObject(runtime, "max_run_timeout_ms", 60000);
    cJSON_AddItemToObject(data, "runtime", runtime);
    cJSON_AddItemToObject(root, "data", data);

    std::string system_json = board.GetSystemInfoJson();
    cJSON* system = cJSON_Parse(system_json.c_str());
    if (system)
        cJSON_AddItemToObject(root, "system", system);
    bool ok = SendJson(root);
    cJSON_Delete(root);
    return ok;
}

void SendPong(const char* id, cJSON* ts) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", kProtocolVersion);
    cJSON_AddStringToObject(root, "type", "pong");
    if (id && id[0])
        cJSON_AddStringToObject(root, "id", id);
    if (cJSON_IsNumber(ts))
        cJSON_AddNumberToObject(root, "ts_ms", ts->valuedouble);
    else
        cJSON_AddNumberToObject(root, "ts_ms", (double)(esp_timer_get_time() / 1000));
    SendJson(root);
    cJSON_Delete(root);
}

void SendPing() {
    char id[32];
    snprintf(id, sizeof(id), "p-%08lx", (unsigned long)esp_random());
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", kProtocolVersion);
    cJSON_AddStringToObject(root, "type", "ping");
    cJSON_AddStringToObject(root, "id", id);
    cJSON_AddNumberToObject(root, "ts_ms", (double)(esp_timer_get_time() / 1000));
    SendJson(root);
    cJSON_Delete(root);
    std::lock_guard<std::mutex> lock(s_ws_mutex);
    if (s_ws && s_ws->IsConnected())
        s_ws->Ping();
}

uint32_t ParseCapabilities(const cJSON* caps) {
    if (!cJSON_IsArray(caps))
        return LUA_RUNTIME_CAP_HTTP | LUA_RUNTIME_CAP_LOG_OUTPUT;
    uint32_t value = LUA_RUNTIME_CAP_LOG_OUTPUT;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, caps) {
        if (!cJSON_IsString(item) || !item->valuestring)
            continue;
        if (strcmp(item->valuestring, "http") == 0)
            value |= LUA_RUNTIME_CAP_HTTP;
        else if (strcmp(item->valuestring, "uart") == 0)
            value |= LUA_RUNTIME_CAP_UART;
        else if (strcmp(item->valuestring, "log") == 0)
            value |= LUA_RUNTIME_CAP_LOG_OUTPUT;
        else if (strcmp(item->valuestring, "camera") == 0)
            value |= LUA_RUNTIME_CAP_CAMERA;
    }
    return value;
}

void SendJobResult(const char* id, lua_runtime_job_state_t state, const char* output,
                   bool output_truncated, const char* value_json, bool value_truncated,
                   uint32_t duration_ms) {
    const char* status = "failed";
    const char* code = "lua_error";
    bool ok = false;
    switch (state) {
        case LUA_RUNTIME_JOB_DONE:
            status = "done";
            ok = true;
            code = nullptr;
            break;
        case LUA_RUNTIME_JOB_TIMEOUT:
            status = "timeout";
            code = "timeout";
            break;
        case LUA_RUNTIME_JOB_STOPPED:
            status = "cancelled";
            code = "cancelled";
            break;
        default:
            status = "failed";
            if (output && strstr(output, "entry function not found"))
                code = "no_entry";
            else
                code = "lua_error";
            break;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", kProtocolVersion);
    cJSON_AddStringToObject(root, "type", "result");
    cJSON_AddStringToObject(root, "id", id);
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "status", status);
    if (!ok && code) {
        cJSON* error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "code", code);
        const char* message = output && output[0] ? output : code;
        cJSON_AddStringToObject(error, "message", message);
        cJSON_AddItemToObject(root, "error", error);
    }
    cJSON* value = nullptr;
    if (value_json && value_json[0])
        value = cJSON_Parse(value_json);
    if (value)
        cJSON_AddItemToObject(root, "value", value);
    else
        cJSON_AddNullToObject(root, "value");
    cJSON_AddBoolToObject(root, "value_truncated", value_truncated);
    cJSON_AddStringToObject(root, "output", output ? output : "");
    cJSON_AddBoolToObject(root, "output_truncated", output_truncated);
    cJSON_AddNumberToObject(root, "duration_ms", duration_ms);
    SendJson(root);
    cJSON_Delete(root);
}

void SendImmediateResult(const char* id, const char* status, const char* code,
                         const char* message) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", kProtocolVersion);
    cJSON_AddStringToObject(root, "type", "result");
    cJSON_AddStringToObject(root, "id", id ? id : "");
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "status", status);
    cJSON* error = cJSON_CreateObject();
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message ? message : code);
    cJSON_AddItemToObject(root, "error", error);
    cJSON_AddNullToObject(root, "value");
    cJSON_AddBoolToObject(root, "value_truncated", false);
    cJSON_AddStringToObject(root, "output", "");
    cJSON_AddBoolToObject(root, "output_truncated", false);
    cJSON_AddNumberToObject(root, "duration_ms", 0);
    SendJson(root);
    cJSON_Delete(root);
}

void FinishJobIfDone() {
    if (!s_job_active.load(std::memory_order_acquire))
        return;
    lua_runtime_job_info_t info = {};
    std::unique_ptr<char[]> output(new (std::nothrow) char[LUA_RUNTIME_OUTPUT_SIZE]());
    if (!output)
        return;
    if (lua_runtime_get_job(s_job_id, &info, output.get(), LUA_RUNTIME_OUTPUT_SIZE) != ESP_OK)
        return;
    if (info.state < LUA_RUNTIME_JOB_DONE)
        return;

    std::unique_ptr<char[]> result(new (std::nothrow) char[LUA_RUNTIME_RESULT_SIZE]());
    if (!result)
        return;
    lua_runtime_get_job_result(s_job_id, result.get(), LUA_RUNTIME_RESULT_SIZE);
    const uint32_t duration_ms =
        (uint32_t)((esp_timer_get_time() - s_job_started_us) / 1000);
    SendJobResult(s_current_req_id, info.state, output.get(), info.output_truncated, result.get(),
                  info.result_truncated, duration_ms);
    SetJobFields(s_current_req_id, result[0] ? result.get() : "null", output.get());
    s_job_active.store(false, std::memory_order_release);
    s_current_req_id[0] = '\0';
    if (!s_stop.load(std::memory_order_acquire) &&
        s_connected.load(std::memory_order_acquire)) {
        SetSnap(ConnState::Waiting, "已连接，等待任务");
    }
}

void HandleRun(cJSON* root) {
    const cJSON* id_item = cJSON_GetObjectItem(root, "id");
    const char* id = cJSON_IsString(id_item) ? id_item->valuestring : "";
    if (!id[0]) {
        SendError("", "invalid", "run requires id");
        return;
    }
    if (s_job_active.load(std::memory_order_acquire)) {
        SendImmediateResult(id, "rejected", "busy", "a job is already running");
        return;
    }
    const cJSON* script_item = cJSON_GetObjectItem(root, "script");
    if (!cJSON_IsString(script_item) || !script_item->valuestring ||
        script_item->valuestring[0] == '\0') {
        SendImmediateResult(id, "rejected", "invalid", "run requires script");
        return;
    }
    const size_t script_len = strlen(script_item->valuestring);
    if (script_len > 64 * 1024) {
        SendImmediateResult(id, "rejected", "too_large", "script exceeds 64KiB");
        return;
    }

    const cJSON* entry_item = cJSON_GetObjectItem(root, "entry");
    const char* entry = "main";
    if (cJSON_IsString(entry_item) && entry_item->valuestring && entry_item->valuestring[0])
        entry = entry_item->valuestring;
    if (strlen(entry) >= kEntryMax) {
        SendImmediateResult(id, "rejected", "invalid", "entry name too long");
        return;
    }

    uint32_t timeout_ms = kDefaultTimeoutMs;
    const cJSON* timeout_item = cJSON_GetObjectItem(root, "timeout_ms");
    if (cJSON_IsNumber(timeout_item)) {
        if (timeout_item->valuedouble < 0) {
            SendImmediateResult(id, "rejected", "invalid", "timeout_ms must be >= 0");
            return;
        }
        timeout_ms = (uint32_t)timeout_item->valuedouble;
        if (timeout_ms > kMaxTimeoutMs)
            timeout_ms = kMaxTimeoutMs;
    }

    char* args_json = nullptr;
    const cJSON* args_item = cJSON_GetObjectItem(root, "args");
    if (args_item) {
        args_json = cJSON_PrintUnformatted(args_item);
        if (!args_json || strlen(args_json) > kMaxArgsJson) {
            cJSON_free(args_json);
            SendImmediateResult(id, "rejected", "too_large", "args exceeds 16KiB");
            return;
        }
    }

    lua_runtime_job_config_t config = {
        .name = "lua_agent",
        .code = script_item->valuestring,
        .path = nullptr,
        .args_json = args_json ? args_json : "{}",
        .timeout_ms = timeout_ms,
        .stack_size = 16 * 1024,
        .priority = 4,
        .capabilities = ParseCapabilities(cJSON_GetObjectItem(root, "capabilities")),
        .entry = entry,
    };
    lua_runtime_job_id_t job_id = 0;
    const esp_err_t err = lua_runtime_start(&config, &job_id);
    cJSON_free(args_json);
    if (err != ESP_OK) {
        const char* code = "busy";
        if (err == ESP_ERR_INVALID_SIZE)
            code = "too_large";
        else if (err == ESP_ERR_INVALID_ARG)
            code = "invalid";
        SendImmediateResult(id, "rejected", code, esp_err_to_name(err));
        return;
    }
    strlcpy(s_current_req_id, id, sizeof(s_current_req_id));
    s_job_id = job_id;
    s_job_started_us = esp_timer_get_time();
    s_job_active.store(true, std::memory_order_release);
    SetJobFields(id, "", "");
    SetSnap(ConnState::Running, "正在运行脚本", id);
    ESP_LOGI(TAG, "run id=%s bytes=%u timeout_ms=%u", id, (unsigned)script_len, timeout_ms);
}

void HandleCancel(cJSON* root) {
    const cJSON* id_item = cJSON_GetObjectItem(root, "id");
    const char* id = cJSON_IsString(id_item) ? id_item->valuestring : "";
    if (!id[0]) {
        SendError("", "invalid", "cancel requires id");
        return;
    }
    if (!s_job_active.load(std::memory_order_acquire) ||
        strcmp(s_current_req_id, id) != 0) {
        SendError(id, "not_found", "no matching running job");
        return;
    }
    lua_runtime_stop(s_job_id);
}

void HandleMessage(char* text) {
    cJSON* root = cJSON_Parse(text);
    if (!root) {
        SendError("", "invalid", "JSON parse failed");
        return;
    }
    const cJSON* type_item = cJSON_GetObjectItem(root, "type");
    const char* type = cJSON_IsString(type_item) ? type_item->valuestring : "";
    const cJSON* id_item = cJSON_GetObjectItem(root, "id");
    const char* id = cJSON_IsString(id_item) ? id_item->valuestring : "";

    if (strcmp(type, "ping") == 0) {
        SendPong(id, cJSON_GetObjectItem(root, "ts_ms"));
    } else if (strcmp(type, "pong") == 0 || strcmp(type, "hello_ok") == 0 ||
               strcmp(type, "hello.welcome") == 0 || strcmp(type, "error") == 0) {
        ESP_LOGI(TAG, "recv type=%s id=%s", type, id);
    } else if (strcmp(type, "run") == 0) {
        HandleRun(root);
    } else if (strcmp(type, "cancel") == 0) {
        HandleCancel(root);
    } else {
        SendError(id, "invalid", "unknown type");
    }
    cJSON_Delete(root);
}

void DrainQueue() {
    IncomingMsg msg;
    while (xQueueReceive(s_msg_queue, &msg, 0) == pdTRUE) {
        if (msg.text) {
            HandleMessage(msg.text);
            free(msg.text);
        }
    }
}

bool ConnectWebSocket(uint32_t session) {
    auto network = Board::GetInstance().GetNetwork();
    if (!network)
        return false;
    auto ws = network->CreateWebSocket(1);
    if (!ws)
        return false;
    ws->SetReceiveBufferSize(kWsReceiveBytes);
    ws->SetHeader("Protocol-Version", "1");
    ws->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    ws->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    if (!s_token.empty()) {
        std::string auth = s_token;
        if (auth.find(' ') == std::string::npos)
            auth = "Bearer " + auth;
        ws->SetHeader("Authorization", auth.c_str());
    }

    ws->OnData([session](const char* data, size_t len, bool binary) {
        if (binary)
            return;
        IncomingMsg msg = {};
        msg.text = static_cast<char*>(malloc(len + 1));
        if (!msg.text)
            return;
        memcpy(msg.text, data, len);
        msg.text[len] = '\0';
        std::lock_guard<std::mutex> lock(s_inbound_mutex);
        if (!SessionAlive(session) || !s_msg_queue ||
            xQueueSend(s_msg_queue, &msg, 0) != pdTRUE) {
            free(msg.text);
        }
    });
    ws->OnDisconnected([session]() {
        ESP_LOGI(TAG, "websocket disconnected");
        if (SessionAlive(session))
            s_connected.store(false, std::memory_order_release);
    });
    ws->OnError([session](int err) {
        ESP_LOGE(TAG, "websocket error=%d", err);
        if (SessionAlive(session))
            s_connected.store(false, std::memory_order_release);
    });

    ESP_LOGI(TAG, "connecting %s", s_url.c_str());
    if (!ws->Connect(s_url.c_str())) {
        ESP_LOGE(TAG, "connect failed err=%d", ws->GetLastError());
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(s_ws_mutex);
        s_ws = std::move(ws);
    }
    s_connected.store(true, std::memory_order_release);
    if (!SendHello()) {
        CloseWebSocket();
        return false;
    }
    return true;
}

void InterruptibleDelay(uint32_t ms) {
    while (ms > 0 && !s_stop.load(std::memory_order_acquire)) {
        const uint32_t slice = ms > 50 ? 50 : ms;
        vTaskDelay(pdMS_TO_TICKS(slice));
        ms -= slice;
    }
}

void AgentTask(void* arg) {
    const uint32_t session = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    s_worker_running.store(true, std::memory_order_release);
    uint32_t backoff_ms = kBackoffMinMs;
    int64_t last_rx_us = esp_timer_get_time();
    int64_t last_ping_us = 0;

    while (!s_stop.load(std::memory_order_acquire) && SessionAlive(session)) {
        if (!s_connected.load(std::memory_order_acquire)) {
            if (s_job_active.load(std::memory_order_acquire)) {
                lua_runtime_stop(s_job_id);
                FinishJobIfDone();
            }
            CloseWebSocket();
            SetSnap(ConnState::Connecting, "正在连接服务器", s_url);
            if (ConnectWebSocket(session)) {
                SetSnap(ConnState::Waiting, "已连接，等待任务", s_url);
                backoff_ms = kBackoffMinMs;
                last_rx_us = esp_timer_get_time();
                last_ping_us = last_rx_us;
            } else {
                SetSnap(ConnState::Reconnecting, "连接断开，正在重连", s_url);
                InterruptibleDelay(backoff_ms);
                backoff_ms = backoff_ms * 2;
                if (backoff_ms > kBackoffMaxMs)
                    backoff_ms = kBackoffMaxMs;
            }
            continue;
        }

        IncomingMsg msg = {};
        if (xQueueReceive(s_msg_queue, &msg, pdMS_TO_TICKS(50)) == pdTRUE) {
            last_rx_us = esp_timer_get_time();
            if (msg.text) {
                HandleMessage(msg.text);
                free(msg.text);
            }
        }
        DrainQueue();
        FinishJobIfDone();

        const int64_t now = esp_timer_get_time();
        if ((now - last_ping_us) / 1000 >= kPingIntervalMs) {
            SendPing();
            last_ping_us = now;
        }
        if ((now - last_rx_us) / 1000 >= kRecvTimeoutMs) {
            ESP_LOGW(TAG, "recv timeout, reconnecting");
            s_connected.store(false, std::memory_order_release);
        }
    }

    if (s_job_active.load(std::memory_order_acquire))
        lua_runtime_stop(s_job_id);
    for (int i = 0; i < 80 && s_job_active.load(std::memory_order_acquire); ++i) {
        FinishJobIfDone();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    DrainQueue();
    CloseWebSocket();
    s_worker_running.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

void StopWorker() {
    s_stop.store(true, std::memory_order_release);
    s_session.fetch_add(1, std::memory_order_acq_rel);
    if (s_job_active.load(std::memory_order_acquire))
        lua_runtime_stop(s_job_id);
    CloseWebSocket();
    for (int i = 0; i < 250 && s_worker_running.load(std::memory_order_acquire); ++i)
        vTaskDelay(pdMS_TO_TICKS(10));
    {
        std::lock_guard<std::mutex> lock(s_inbound_mutex);
        if (s_msg_queue) {
            IncomingMsg msg;
            while (xQueueReceive(s_msg_queue, &msg, 0) == pdTRUE)
                free(msg.text);
            vQueueDelete(s_msg_queue);
            s_msg_queue = nullptr;
        }
    }
}

uint32_t StatusColor(ConnState state) {
    switch (state) {
        case ConnState::Waiting:
            return kColorWaiting;
        case ConnState::Connecting:
            return kColorConnecting;
        case ConnState::Running:
            return kColorRunning;
        case ConnState::Error:
            return kColorError;
        case ConnState::Reconnecting:
            return kColorReconnect;
        default:
            return kColorHint;
    }
}

void UiTimerCallback(lv_timer_t* timer) {
    (void)timer;
    UiSnapshot snap;
    {
        std::lock_guard<std::mutex> lock(s_ui_mutex);
        snap = s_snap;
    }
    if (s_status_label) {
        lv_label_set_text(s_status_label, snap.status.c_str());
        lv_obj_set_style_text_color(s_status_label, lv_color_hex(StatusColor(snap.state)),
                                    LV_PART_MAIN);
    }
    if (s_url_label) {
        const std::string line = snap.detail.empty() ? snap.url : snap.detail;
        lv_label_set_text(s_url_label, Truncate(line, 80).c_str());
    }
    if (s_job_label) {
        if (snap.job_id.empty())
            lv_label_set_text(s_job_label, "-");
        else
            lv_label_set_text(s_job_label, snap.job_id.c_str());
    }
    if (s_result_label)
        lv_label_set_text(s_result_label,
                          snap.result.empty() ? "-" : Truncate(snap.result, 400).c_str());
    if (s_output_label)
        lv_label_set_text(s_output_label,
                          snap.output.empty() ? "-" : Truncate(snap.output, 400).c_str());
}

void ReturnHome() {
    if (s_ui_timer) {
        lv_timer_delete(s_ui_timer);
        s_ui_timer = nullptr;
    }
    StopWorker();
    s_status_label = nullptr;
    s_url_label = nullptr;
    s_job_label = nullptr;
    s_result_label = nullptr;
    s_output_label = nullptr;
    s_screen = nullptr;

    lv_obj_t* old_scr = lv_screen_active();
    if (s_lifecycle_cb != nullptr) {
        s_lifecycle_cb(SCREEN_LIFECYCLE_UNLOAD);
        s_lifecycle_cb = nullptr;
    }
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home)
        lv_obj_delete_async(old_scr);
}

void OnSwipeBack() { ReturnHome(); }

void OnBackClicked(lv_event_t* /*e*/) { OnSwipeBack(); }

lv_style_selector_t Sel(lv_part_t part, lv_state_t state) {
    return static_cast<lv_style_selector_t>(part | state);
}

lv_obj_t* MakeCard(lv_obj_t* parent, const char* title) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* label = lv_label_create(card);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_color(label, lv_color_hex(kColorHint), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &font_puhui_20_4, LV_PART_MAIN);
    return card;
}

lv_obj_t* MakeValue(lv_obj_t* card) {
    lv_obj_t* value = lv_label_create(card);
    lv_obj_set_width(value, LV_PCT(100));
    lv_label_set_long_mode(value, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(value, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(value, &font_puhui_20_4, LV_PART_MAIN);
    lv_label_set_text(value, "-");
    return value;
}

void BuildHeader(lv_obj_t* parent) {
    lv_obj_t* header = lv_obj_create(parent);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelW, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* back = lv_button_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF), Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(back, LV_OPA_20, Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_add_event_cb(back, OnBackClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, I18n::T("远程脚本"));
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 16, 0);
}

void BuildBody(lv_obj_t* parent) {
    lv_obj_t* list = lv_obj_create(parent);
    screen_strip_obj_chrome(list);
    lv_obj_set_size(list, kPanelW - 2 * kPad, kPanelH - kHeaderH - kPad);
    lv_obj_set_pos(list, kPad, kHeaderH);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(list, kPad, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    lv_obj_t* hint = lv_label_create(list);
    lv_label_set_text(hint, I18n::T("打开后自动连接，等待服务端下发 Lua 脚本"));
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorHint), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);

    lv_obj_t* status_card = MakeCard(list, I18n::T("正在连接服务器"));
    s_status_label = lv_obj_get_child(status_card, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(kColorConnecting), LV_PART_MAIN);
    s_url_label = MakeValue(status_card);

    lv_obj_t* job_card = MakeCard(list, "ID");
    s_job_label = MakeValue(job_card);

    lv_obj_t* result_card = MakeCard(list, I18n::T("最近返回值"));
    s_result_label = MakeValue(result_card);

    lv_obj_t* output_card = MakeCard(list, I18n::T("脚本输出"));
    s_output_label = MakeValue(output_card);
}

bool StartWorker() {
    s_stop.store(false, std::memory_order_release);
    s_job_active.store(false, std::memory_order_release);
    s_connected.store(false, std::memory_order_release);
    s_current_req_id[0] = '\0';
    s_url = GetAgentUrl();
    s_token = GetAgentToken();
    {
        std::lock_guard<std::mutex> lock(s_ui_mutex);
        s_snap = UiSnapshot{};
        s_snap.state = ConnState::Connecting;
        s_snap.status = I18n::T("正在连接服务器");
        s_snap.url = s_url;
        s_snap.detail = s_url;
    }
    s_msg_queue = xQueueCreate(kQueueLength, sizeof(IncomingMsg));
    if (!s_msg_queue)
        return false;
    const uint32_t session = s_session.fetch_add(1, std::memory_order_acq_rel) + 1;
    s_session.store(session, std::memory_order_release);
    if (xTaskCreate(AgentTask, "lua_agent", kWorkerStack,
                    reinterpret_cast<void*>(static_cast<uintptr_t>(session)), 5, nullptr) !=
        pdPASS) {
        vQueueDelete(s_msg_queue);
        s_msg_queue = nullptr;
        return false;
    }
    return true;
}

}  // namespace

void LuaAgentApp::Launch(screen_lifecycle_cb_t lifecycle_cb) {
    if (s_screen != nullptr || s_worker_running.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "Lua agent app is already active");
        return;
    }

    lv_obj_t* old_scr = lv_screen_active();
    s_lifecycle_cb = lifecycle_cb;
    s_screen = lv_obj_create(nullptr);
    screen_strip_obj_chrome(s_screen);
    lv_obj_set_size(s_screen, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    BuildHeader(s_screen);
    BuildBody(s_screen);
    screen_mark_native_layout(s_screen);
    screen_attach_swipe_back(s_screen, OnSwipeBack);
    lv_screen_load(s_screen);
    if (s_lifecycle_cb != nullptr)
        s_lifecycle_cb(SCREEN_LIFECYCLE_LOAD);
    if (old_scr != nullptr && old_scr != s_screen)
        lv_obj_delete_async(old_scr);

    if (!StartWorker()) {
        SetSnap(ConnState::Error, "连接失败", "worker start failed");
    }
    s_ui_timer = lv_timer_create(UiTimerCallback, kUiRefreshMs, nullptr);
}

void LuaAgentApp::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: lua_agent");
    } else {
        ESP_LOGI(TAG, "unload: lua_agent");
    }
}
