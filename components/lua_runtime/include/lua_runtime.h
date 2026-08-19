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
} lua_runtime_job_config_t;

typedef struct {
    lua_runtime_job_id_t id;
    lua_runtime_job_state_t state;
    size_t output_length;
    bool output_truncated;
} lua_runtime_job_info_t;

typedef void (*lua_runtime_job_callback_t)(const lua_runtime_job_info_t* info, const char* output,
                                           void* user_ctx);

typedef esp_err_t (*lua_runtime_audio_play_callback_t)(const char* source, bool loop,
                                                       uint8_t volume, uint32_t* handle,
                                                       void* user_ctx);
typedef esp_err_t (*lua_runtime_audio_stop_callback_t)(uint32_t handle, void* user_ctx);
typedef esp_err_t (*lua_runtime_audio_simple_callback_t)(void* user_ctx);
typedef bool (*lua_runtime_audio_is_playing_callback_t)(uint32_t handle, void* user_ctx);

esp_err_t lua_runtime_init(void);
esp_err_t lua_runtime_deinit(void);
esp_err_t lua_runtime_register_module(const char* name, lua_CFunction open_fn);
esp_err_t lua_runtime_run(const lua_runtime_job_config_t* config, char* output, size_t output_size);
esp_err_t lua_runtime_start(const lua_runtime_job_config_t* config, lua_runtime_job_id_t* job_id);
esp_err_t lua_runtime_stop(lua_runtime_job_id_t job_id);
esp_err_t lua_runtime_get_job(lua_runtime_job_id_t job_id, lua_runtime_job_info_t* info,
                              char* output, size_t output_size);
esp_err_t lua_runtime_set_callback(lua_runtime_job_callback_t callback, void* user_ctx);
esp_err_t lua_runtime_set_audio_backend(lua_runtime_audio_play_callback_t play,
                                        lua_runtime_audio_stop_callback_t stop,
                                        lua_runtime_audio_simple_callback_t stop_all,
                                        lua_runtime_audio_is_playing_callback_t is_playing,
                                        void* user_ctx);

#ifdef __cplusplus
}
#endif
