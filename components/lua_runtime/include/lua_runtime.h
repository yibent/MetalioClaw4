#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t lua_runtime_job_id_t;

#define LUA_RUNTIME_CAP_UART (1u << 0)
/* Grants the built-in UI event injector to an explicitly marked self-test job. */
#define LUA_RUNTIME_CAP_UI_TEST (1u << 1)
/* Mirrors Lua print() calls to ESP_LOG in addition to captured job output. */
#define LUA_RUNTIME_CAP_LOG_OUTPUT (1u << 2)
/* Grants the managed HTTP client. Jobs cannot open raw sockets. */
#define LUA_RUNTIME_CAP_HTTP (1u << 3)
/* Grants the camera.capture / camera.explain module. */
#define LUA_RUNTIME_CAP_CAMERA (1u << 4)

#define LUA_RUNTIME_HTTP_DEFAULT_TIMEOUT_MS 15000u
#define LUA_RUNTIME_HTTP_MAX_TIMEOUT_MS 60000u
#define LUA_RUNTIME_HTTP_DEFAULT_MAX_BODY (64u * 1024u)
#define LUA_RUNTIME_HTTP_HARD_MAX_BODY (256u * 1024u)
#define LUA_RUNTIME_HTTP_MAX_REQUEST_BODY (64u * 1024u)
#define LUA_RUNTIME_HTTP_MAX_HEADERS 32u
#define LUA_RUNTIME_HTTP_MAX_HEADER_SIZE 256u
#define LUA_RUNTIME_HTTP_MAX_URL 2048u
#define LUA_RUNTIME_HTTP_MAX_CONCURRENT 2u
#define LUA_RUNTIME_OUTPUT_SIZE (4u * 1024u)
#define LUA_RUNTIME_RESULT_SIZE (8u * 1024u)

typedef enum {
    LUA_RUNTIME_JOB_QUEUED = 0,
    LUA_RUNTIME_JOB_RUNNING,
    LUA_RUNTIME_JOB_DONE,
    LUA_RUNTIME_JOB_FAILED,
    LUA_RUNTIME_JOB_TIMEOUT,
    LUA_RUNTIME_JOB_STOPPED,
} lua_runtime_job_state_t;

typedef struct {
    const char* name;
    const char* code;
    const char* path;
    const char* args_json;
    uint32_t timeout_ms;
    uint32_t stack_size;
    int priority;
    uint32_t capabilities;
    /* If non-NULL, after the chunk runs the runtime calls this global function
     * with the `args` table and stores its return value(s) as JSON. */
    const char* entry;
} lua_runtime_job_config_t;

typedef struct {
    lua_runtime_job_id_t id;
    lua_runtime_job_state_t state;
    size_t output_length;
    bool output_truncated;
    size_t result_length;
    bool result_truncated;
} lua_runtime_job_info_t;

typedef struct {
    int port;
    int tx_pin;
    int rx_pin;
    uint32_t max_baud_rate;
} lua_runtime_uart_port_config_t;

typedef void (*lua_runtime_job_callback_t)(const lua_runtime_job_info_t* info, const char* output,
                                           void* user_ctx);

typedef esp_err_t (*lua_runtime_audio_play_callback_t)(const char* source, bool loop,
                                                       uint8_t volume, uint32_t* handle,
                                                       void* user_ctx);
typedef esp_err_t (*lua_runtime_audio_stop_callback_t)(uint32_t handle, void* user_ctx);
typedef esp_err_t (*lua_runtime_audio_simple_callback_t)(void* user_ctx);
typedef bool (*lua_runtime_audio_is_playing_callback_t)(uint32_t handle, void* user_ctx);
typedef bool (*lua_runtime_ui_lock_callback_t)(int timeout_ms, void* user_ctx);
typedef void (*lua_runtime_ui_unlock_callback_t)(void* user_ctx);

typedef struct {
    const char* method;
    const char* url;
    const char** header_keys;
    const char** header_values;
    size_t header_count;
    const char* body;
    size_t body_len;
    uint32_t timeout_ms;
    size_t max_body;
    volatile bool* stop_requested;
    int64_t deadline_us;
} lua_runtime_http_request_t;

typedef struct {
    int status;
    int last_error;
    char* error;
    char* body;
    size_t body_len;
    char** header_keys;
    char** header_values;
    size_t header_count;
} lua_runtime_http_response_t;

typedef esp_err_t (*lua_runtime_http_request_callback_t)(const lua_runtime_http_request_t* req,
                                                         lua_runtime_http_response_t* res,
                                                         void* user_ctx);

typedef esp_err_t (*lua_runtime_camera_explain_callback_t)(const char* question, char* output,
                                                           size_t output_size, void* user_ctx);
typedef esp_err_t (*lua_runtime_speech_say_callback_t)(const char* text, void* user_ctx);
typedef esp_err_t (*lua_runtime_device_set_int_callback_t)(int value, void* user_ctx);
typedef esp_err_t (*lua_runtime_device_vibrate_callback_t)(uint32_t duration_ms, void* user_ctx);
typedef esp_err_t (*lua_runtime_device_notify_callback_t)(const char* text, void* user_ctx);

esp_err_t lua_runtime_init(void);
esp_err_t lua_runtime_deinit(void);
esp_err_t lua_runtime_register_module(const char* name, lua_CFunction open_fn);
esp_err_t lua_runtime_run(const lua_runtime_job_config_t* config, char* output, size_t output_size);
esp_err_t lua_runtime_start(const lua_runtime_job_config_t* config, lua_runtime_job_id_t* job_id);
esp_err_t lua_runtime_stop(lua_runtime_job_id_t job_id);
esp_err_t lua_runtime_get_job(lua_runtime_job_id_t job_id, lua_runtime_job_info_t* info,
                              char* output, size_t output_size);
esp_err_t lua_runtime_get_job_result(lua_runtime_job_id_t job_id, char* result, size_t result_size);
esp_err_t lua_runtime_set_callback(lua_runtime_job_callback_t callback, void* user_ctx);
esp_err_t lua_runtime_set_audio_backend(lua_runtime_audio_play_callback_t play,
                                        lua_runtime_audio_stop_callback_t stop,
                                        lua_runtime_audio_simple_callback_t stop_all,
                                        lua_runtime_audio_is_playing_callback_t is_playing,
                                        void* user_ctx);
esp_err_t lua_runtime_set_ui_backend(lua_runtime_ui_lock_callback_t lock,
                                     lua_runtime_ui_unlock_callback_t unlock, void* user_ctx);
esp_err_t lua_runtime_set_http_backend(lua_runtime_http_request_callback_t request, void* user_ctx);
void lua_runtime_http_response_free(lua_runtime_http_response_t* res);
esp_err_t lua_runtime_set_camera_backend(lua_runtime_camera_explain_callback_t explain,
                                         void* user_ctx);
esp_err_t lua_runtime_set_speech_backend(lua_runtime_speech_say_callback_t say, void* user_ctx);
esp_err_t lua_runtime_set_device_backend(lua_runtime_device_set_int_callback_t set_brightness,
                                         lua_runtime_device_set_int_callback_t set_volume,
                                         lua_runtime_device_vibrate_callback_t vibrate,
                                         lua_runtime_device_notify_callback_t notify,
                                         void* user_ctx);
esp_err_t lua_runtime_register_uart_port(const lua_runtime_uart_port_config_t* config);

#ifdef __cplusplus
}
#endif
