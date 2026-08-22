#include "lua_http_backend.h"

#include "board.h"
#include "http.h"
#include "lua_runtime.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <strings.h>
#include <string>

namespace {

constexpr const char* TAG = "LuaHttp";
constexpr int kConnectId = 2;
constexpr int kMaxRedirects = 3;
constexpr uint32_t kWaitSliceMs = 200;
constexpr const char* kUserAgent = "MetalioClaw-Lua/1.0";
constexpr const char* kCaptureHeaders[] = {
    "content-type", "content-length", "location",     "content-encoding", "cache-control",
    "etag",         "date",           "retry-after", "www-authenticate",
};

SemaphoreHandle_t s_slots;

bool Aborted(const lua_runtime_http_request_t* req) {
    return req && req->stop_requested && *req->stop_requested;
}

uint32_t RemainingMs(const lua_runtime_http_request_t* req) {
    if (!req || Aborted(req))
        return 0;
    uint32_t left = req->timeout_ms;
    if (req->deadline_us) {
        int64_t now = esp_timer_get_time();
        if (now >= req->deadline_us)
            return 0;
        uint32_t job_left = static_cast<uint32_t>((req->deadline_us - now) / 1000);
        if (job_left < left)
            left = job_left;
    }
    return left;
}

char* DupCStr(const std::string& value) {
    char* copy = static_cast<char*>(malloc(value.size() + 1));
    if (!copy)
        return nullptr;
    memcpy(copy, value.c_str(), value.size() + 1);
    return copy;
}

void SetError(lua_runtime_http_response_t* res, const char* message, int last_error = 0) {
    if (!res)
        return;
    lua_runtime_http_response_free(res);
    res->status = -1;
    res->last_error = last_error;
    res->error = DupCStr(message ? message : "request failed");
}

std::string CanonicalHeader(const std::string& key) {
    std::string out = key;
    bool cap = true;
    for (char& c : out) {
        if (c == '-') {
            cap = true;
            continue;
        }
        if (cap) {
            c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
            cap = false;
        } else {
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        }
    }
    return out;
}

std::string AsciiLower(std::string value) {
    for (char& c : value)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return value;
}

bool HasRequestHeader(const lua_runtime_http_request_t* req, const char* name) {
    if (!req || !name || !req->header_keys)
        return false;
    for (size_t i = 0; i < req->header_count; ++i) {
        if (req->header_keys[i] && strcasecmp(req->header_keys[i], name) == 0)
            return true;
    }
    return false;
}

std::string LookupHeader(Http* http, const char* name) {
    if (!http || !name)
        return {};
    std::string value = http->GetResponseHeader(name);
    if (!value.empty())
        return value;
    value = http->GetResponseHeader(CanonicalHeader(name));
    if (!value.empty())
        return value;
    return http->GetResponseHeader(AsciiLower(name));
}

bool IsHttpUrl(const std::string& url) {
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

bool UrlHostOk(const std::string& url) {
    const char* host = nullptr;
    if (url.rfind("https://", 0) == 0)
        host = url.c_str() + 8;
    else if (url.rfind("http://", 0) == 0)
        host = url.c_str() + 7;
    else
        return false;
    return host[0] != '\0' && host[0] != '/' && host[0] != ':';
}

bool ResolveRedirect(const std::string& current, const std::string& location, std::string& out) {
    if (location.empty() || location.size() > LUA_RUNTIME_HTTP_MAX_URL)
        return false;
    if (IsHttpUrl(location)) {
        out = location;
        return UrlHostOk(out);
    }
    auto scheme_end = current.find("://");
    if (scheme_end == std::string::npos)
        return false;
    auto path_start = current.find('/', scheme_end + 3);
    std::string origin = path_start == std::string::npos ? current : current.substr(0, path_start);
    if (location.rfind("//", 0) == 0) {
        out = current.substr(0, scheme_end + 1) + location;
        return IsHttpUrl(out) && UrlHostOk(out) && out.size() <= LUA_RUNTIME_HTTP_MAX_URL;
    }
    if (location[0] == '/') {
        out = origin + location;
        return out.size() <= LUA_RUNTIME_HTTP_MAX_URL;
    }
    return false;
}

bool IsRedirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

std::string UrlForLog(const std::string& url) {
    std::string out = url;
    auto query = out.find('?');
    if (query != std::string::npos)
        out.resize(query);
    if (out.size() > 96)
        out.resize(96);
    return out;
}

esp_err_t CaptureHeaders(Http* http, lua_runtime_http_response_t* res) {
    const size_t count = sizeof(kCaptureHeaders) / sizeof(kCaptureHeaders[0]);
    res->header_keys = static_cast<char**>(calloc(count, sizeof(char*)));
    res->header_values = static_cast<char**>(calloc(count, sizeof(char*)));
    if (!res->header_keys || !res->header_values)
        return ESP_ERR_NO_MEM;
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        std::string value = LookupHeader(http, kCaptureHeaders[i]);
        if (value.empty())
            continue;
        res->header_keys[used] = DupCStr(kCaptureHeaders[i]);
        res->header_values[used] = DupCStr(value);
        if (!res->header_keys[used] || !res->header_values[used]) {
            res->header_count = used + 1;
            return ESP_ERR_NO_MEM;
        }
        used++;
        res->header_count = used;
    }
    return ESP_OK;
}

esp_err_t WaitStatus(Http* http, const lua_runtime_http_request_t* req, int* status) {
    while (true) {
        if (Aborted(req))
            return ESP_ERR_INVALID_STATE;
        uint32_t left = RemainingMs(req);
        if (left == 0)
            return ESP_ERR_TIMEOUT;
        http->SetTimeout(static_cast<int>(std::min(left, kWaitSliceMs)));
        int code = http->GetStatusCode();
        if (code >= 100 && code <= 599) {
            *status = code;
            return ESP_OK;
        }
        if (Aborted(req))
            return ESP_ERR_INVALID_STATE;
        if (RemainingMs(req) == 0)
            return ESP_ERR_TIMEOUT;
        int last_error = http->GetLastError();
        if (last_error != 0) {
            *status = -1;
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t ReadBody(Http* http, const lua_runtime_http_request_t* req, const char* method,
                   int status, std::string* body) {
    if (strcmp(method, "HEAD") == 0 || status == 204 || status == 304)
        return ESP_OK;
    size_t known = http->GetBodyLength();
    if (known > req->max_body)
        return ESP_ERR_INVALID_SIZE;
    char chunk[1024];
    int fail_streak = 0;
    while (true) {
        if (Aborted(req))
            return ESP_ERR_INVALID_STATE;
        uint32_t left = RemainingMs(req);
        if (left == 0)
            return ESP_ERR_TIMEOUT;
        http->SetTimeout(static_cast<int>(std::min(left, kWaitSliceMs)));
        int n = http->Read(chunk, sizeof(chunk));
        if (n < 0) {
            if (Aborted(req))
                return ESP_ERR_INVALID_STATE;
            if (RemainingMs(req) == 0)
                return ESP_ERR_TIMEOUT;
            fail_streak++;
            if (fail_streak >= 3 && http->GetLastError() != 0)
                return ESP_FAIL;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        fail_streak = 0;
        if (n == 0)
            break;
        if (body->size() + static_cast<size_t>(n) > req->max_body)
            return ESP_ERR_INVALID_SIZE;
        body->append(chunk, static_cast<size_t>(n));
    }
    return ESP_OK;
}

bool AcquireSlot(const lua_runtime_http_request_t* req) {
    if (!s_slots)
        return false;
    while (true) {
        if (Aborted(req) || RemainingMs(req) == 0)
            return false;
        if (xSemaphoreTake(s_slots, pdMS_TO_TICKS(50)) == pdTRUE)
            return true;
    }
}

esp_err_t LuaHttpRequest(const lua_runtime_http_request_t* req, lua_runtime_http_response_t* res,
                         void* user_ctx) {
    (void)user_ctx;
    if (!req || !res || !req->method || !req->url)
        return ESP_ERR_INVALID_ARG;
    memset(res, 0, sizeof(*res));

    if (Aborted(req)) {
        SetError(res, "stopped");
        return ESP_ERR_INVALID_STATE;
    }
    if (RemainingMs(req) == 0) {
        SetError(res, "timeout");
        return ESP_ERR_TIMEOUT;
    }
    if (!AcquireSlot(req)) {
        if (Aborted(req)) {
            SetError(res, "stopped");
            return ESP_ERR_INVALID_STATE;
        }
        SetError(res, "timeout");
        return ESP_ERR_TIMEOUT;
    }

    struct SlotGuard {
        ~SlotGuard() {
            if (s_slots)
                xSemaphoreGive(s_slots);
        }
    } slot_guard;

    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        SetError(res, "network is not available");
        return ESP_ERR_NOT_FOUND;
    }

    std::string method = req->method;
    std::string url = req->url;
    std::string body = (req->body && req->body_len) ? std::string(req->body, req->body_len) : std::string();
    int redirects = 0;

    while (true) {
        if (Aborted(req)) {
            SetError(res, "stopped");
            return ESP_ERR_INVALID_STATE;
        }
        uint32_t left = RemainingMs(req);
        if (left == 0) {
            SetError(res, "timeout");
            return ESP_ERR_TIMEOUT;
        }

        auto http = network->CreateHttp(kConnectId);
        if (!http) {
            SetError(res, "network is not available");
            return ESP_ERR_NOT_FOUND;
        }

        http->SetKeepAlive(false);
        http->SetTimeout(static_cast<int>(left));
        http->SetHeader("Connection", "close");
        if (!HasRequestHeader(req, "user-agent"))
            http->SetHeader("User-Agent", kUserAgent);
        for (size_t i = 0; i < req->header_count; ++i) {
            if (req->header_keys && req->header_keys[i] && req->header_values && req->header_values[i])
                http->SetHeader(req->header_keys[i], req->header_values[i]);
        }
        if (!body.empty() && method != "GET" && method != "HEAD")
            http->SetContent(std::string(body));

        ESP_LOGI(TAG, "%s %s", method.c_str(), UrlForLog(url).c_str());
        if (!http->Open(method, url)) {
            SetError(res, "connect failed", http->GetLastError());
            return ESP_FAIL;
        }

        int status = -1;
        esp_err_t wait_err = WaitStatus(http.get(), req, &status);
        if (wait_err != ESP_OK) {
            http->Close();
            if (wait_err == ESP_ERR_TIMEOUT)
                SetError(res, "timeout");
            else if (wait_err == ESP_ERR_INVALID_STATE)
                SetError(res, "stopped");
            else
                SetError(res, "connect failed", http->GetLastError());
            return wait_err == ESP_FAIL ? ESP_FAIL : wait_err;
        }

        if (IsRedirect(status) && redirects < kMaxRedirects) {
            std::string next;
            if (!ResolveRedirect(url, LookupHeader(http.get(), "location"), next)) {
                http->Close();
                SetError(res, "invalid redirect");
                return ESP_FAIL;
            }
            if (status == 301 || status == 302 || status == 303) {
                method = "GET";
                body.clear();
            }
            http->Close();
            url = std::move(next);
            redirects++;
            continue;
        }
        if (IsRedirect(status) && redirects >= kMaxRedirects) {
            http->Close();
            SetError(res, "redirect limit exceeded");
            return ESP_FAIL;
        }

        esp_err_t header_err = CaptureHeaders(http.get(), res);
        if (header_err != ESP_OK) {
            http->Close();
            SetError(res, "out of memory");
            return header_err;
        }

        std::string response_body;
        esp_err_t read_err = ReadBody(http.get(), req, method.c_str(), status, &response_body);
        http->Close();
        if (read_err != ESP_OK) {
            if (read_err == ESP_ERR_TIMEOUT)
                SetError(res, "timeout");
            else if (read_err == ESP_ERR_INVALID_STATE)
                SetError(res, "stopped");
            else if (read_err == ESP_ERR_INVALID_SIZE)
                SetError(res, "response too large");
            else
                SetError(res, "read failed", http->GetLastError());
            return read_err;
        }

        if (!response_body.empty()) {
            res->body = DupCStr(response_body);
            if (!res->body) {
                SetError(res, "out of memory");
                return ESP_ERR_NO_MEM;
            }
            res->body_len = response_body.size();
        }
        res->status = status;
        ESP_LOGI(TAG, "%s %s -> %d (%u bytes)", method.c_str(), UrlForLog(url).c_str(), status,
                 static_cast<unsigned>(res->body_len));
        return ESP_OK;
    }
}

}  // namespace

void RegisterLuaHttpBackend() {
    if (!s_slots) {
        s_slots = xSemaphoreCreateCounting(LUA_RUNTIME_HTTP_MAX_CONCURRENT,
                                           LUA_RUNTIME_HTTP_MAX_CONCURRENT);
        if (!s_slots)
            ESP_LOGE(TAG, "failed to create HTTP slot semaphore");
    }
    if (lua_runtime_set_http_backend(LuaHttpRequest, nullptr) != ESP_OK)
        ESP_LOGE(TAG, "failed to register HTTP backend");
}
