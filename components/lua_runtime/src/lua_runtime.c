#include "lua_runtime.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lauxlib.h"
#include "lua_runtime_internal.h"
#include "lua_ui.h"
#include "lualib.h"

#define TAG "lua_runtime"
#define LUA_RUNTIME_MAX_JOBS 8
#define LUA_RUNTIME_MAX_MODULES 16
#define LUA_RUNTIME_MAX_CODE (64 * 1024)
#define LUA_RUNTIME_DEFAULT_STACK (12 * 1024)
#define LUA_RUNTIME_DEFAULT_PRIORITY 4
#define LUA_RUNTIME_RESULT_MAX_DEPTH 16
#define LUA_RUNTIME_RESULT_MAX_KEYS 256
#define LUA_RUNTIME_AUDIO_HANDLES 16
#define LUA_RUNTIME_AUDIO_CONTEXT "metalio.lua.audio.context"

typedef struct {
    bool used;
    lua_runtime_job_id_t id;
    lua_runtime_job_state_t state;
    char *name;
    char *code;
    char *path;
    char *args_json;
    char *entry;
    char *output;
    size_t output_length;
    bool output_truncated;
    char *result;
    size_t result_length;
    bool result_truncated;
    uint32_t timeout_ms;
    uint32_t capabilities;
    volatile bool stop_requested;
    TaskHandle_t task;
} runtime_job_t;

static SemaphoreHandle_t s_lock;
static runtime_job_t s_jobs[LUA_RUNTIME_MAX_JOBS];
typedef struct {
    char name[32];
    lua_CFunction open_fn;
} runtime_module_t;

static runtime_module_t s_modules[LUA_RUNTIME_MAX_MODULES];
static size_t s_module_count;
static lua_runtime_job_id_t s_next_id = 1;
static lua_runtime_job_callback_t s_callback;
static void *s_callback_ctx;
static lua_runtime_audio_play_callback_t s_audio_play;
static lua_runtime_audio_play_bytes_callback_t s_audio_play_bytes;
static lua_runtime_audio_stop_callback_t s_audio_stop;
static lua_runtime_audio_simple_callback_t s_audio_stop_all;
static lua_runtime_audio_is_playing_callback_t s_audio_is_playing;
static void *s_audio_ctx;
static bool s_initialized;
static const char kAudioContextRegistryKey;

typedef struct {
    uint32_t handles[LUA_RUNTIME_AUDIO_HANDLES];
} runtime_audio_context_t;

static void free_job(runtime_job_t *job) {
    free(job->name);
    free(job->code);
    free(job->path);
    free(job->args_json);
    free(job->entry);
    free(job->output);
    free(job->result);
    memset(job, 0, sizeof(*job));
}

void lua_runtime_set_context(lua_State *state, lua_runtime_exec_context_t *context) {
    *(lua_runtime_exec_context_t **)lua_getextraspace(state) = context;
}

lua_runtime_exec_context_t *lua_runtime_get_context(lua_State *state) {
    return *(lua_runtime_exec_context_t **)lua_getextraspace(state);
}

void lua_runtime_append_output(lua_runtime_exec_context_t *context, const char *text,
                               size_t length) {
    if (!context || !context->output || context->output_size == 0 || !text) {
        return;
    }
    size_t used = context->output_length < context->output_size ? context->output_length
                                                                : context->output_size - 1;
    size_t room = context->output_size - 1 - used;
    size_t copy = length < room ? length : room;
    if (copy) {
        memcpy(context->output + context->output_length, text, copy);
    }
    context->output_length += copy;
    context->output[context->output_length] = '\0';
    if (copy != length) {
        context->truncated = true;
    }
}

int lua_runtime_check_abort(lua_State *state) {
    lua_runtime_exec_context_t *context = lua_runtime_get_context(state);
    if (!context) {
        return 0;
    }
    if (context->stop_requested && *context->stop_requested) {
        return luaL_error(state, "stopped");
    }
    if (context->deadline_us && esp_timer_get_time() >= context->deadline_us) {
        return luaL_error(state, "timeout");
    }
    return 0;
}

static void lua_hook(lua_State *state, lua_Debug *debug) {
    (void)debug;
    lua_runtime_check_abort(state);
}

static int lua_print(lua_State *state) {
    lua_runtime_exec_context_t *context = lua_runtime_get_context(state);
    int count = lua_gettop(state);
    char log_line[256] = {};
    size_t log_length = 0;
    for (int i = 1; i <= count; ++i) {
        size_t length = 0;
        const char *text = luaL_tolstring(state, i, &length);
        if (i > 1) {
            lua_runtime_append_output(context, "\t", 1);
            if (log_length + 1 < sizeof(log_line))
                log_line[log_length++] = '\t';
        }
        lua_runtime_append_output(context, text, length);
        size_t room = sizeof(log_line) - 1 - log_length;
        size_t copy = length < room ? length : room;
        if (copy) {
            memcpy(log_line + log_length, text, copy);
            log_length += copy;
        }
        lua_pop(state, 1);
    }
    lua_runtime_append_output(context, "\n", 1);
    if (context && (context->capabilities & LUA_RUNTIME_CAP_LOG_OUTPUT)) {
        ESP_LOGI(TAG, "[%s] %s%s", context->job_name ? context->job_name : "lua_job",
                 log_line, log_length == sizeof(log_line) - 1 ? " [line truncated]" : "");
    }
    return 0;
}

static int l_runtime_sleep(lua_State *state) {
    lua_Integer requested_delay = luaL_checkinteger(state, 1);
    if (requested_delay < 0 || requested_delay > UINT32_MAX) {
        return luaL_argerror(state, 1, "delay must be between 0 and UINT32_MAX");
    }
    uint32_t remaining = (uint32_t)requested_delay;
    while (remaining > 0) {
        uint32_t slice = remaining > 50 ? 50 : remaining;
        vTaskDelay(pdMS_TO_TICKS(slice));
        remaining -= slice;
        lua_runtime_check_abort(state);
    }
    return 0;
}

static int l_runtime_now_ms(lua_State *state) {
    lua_pushinteger(state, (lua_Integer)(esp_timer_get_time() / 1000));
    return 1;
}

static int l_runtime_sleep_until(lua_State *state) {
    lua_Integer deadline_ms = luaL_checkinteger(state, 1);
    while (true) {
        int64_t remaining_ms = (int64_t)deadline_ms - esp_timer_get_time() / 1000;
        if (remaining_ms <= 0)
            return 0;
        uint32_t slice = remaining_ms > 50 ? 50 : (uint32_t)remaining_ms;
        vTaskDelay(pdMS_TO_TICKS(slice));
        lua_runtime_check_abort(state);
    }
}

static int l_runtime_cancelled(lua_State *state) {
    lua_runtime_exec_context_t *context = lua_runtime_get_context(state);
    lua_pushboolean(state, context && context->stop_requested && *context->stop_requested);
    return 1;
}

static runtime_audio_context_t *get_audio_context(lua_State *state) {
    lua_rawgetp(state, LUA_REGISTRYINDEX, &kAudioContextRegistryKey);
    runtime_audio_context_t *context = lua_touserdata(state, -1);
    lua_pop(state, 1);
    if (context)
        return context;
    context = lua_newuserdatauv(state, sizeof(*context), 0);
    memset(context, 0, sizeof(*context));
    luaL_getmetatable(state, LUA_RUNTIME_AUDIO_CONTEXT);
    lua_setmetatable(state, -2);
    lua_rawsetp(state, LUA_REGISTRYINDEX, &kAudioContextRegistryKey);
    return context;
}

static int find_audio_handle(runtime_audio_context_t *context, uint32_t handle) {
    for (int i = 0; i < LUA_RUNTIME_AUDIO_HANDLES; ++i) {
        if (context->handles[i] == handle)
            return i;
    }
    return -1;
}

static int close_audio_context(lua_State *state) {
    runtime_audio_context_t *context = lua_touserdata(state, 1);
    if (!context || !s_audio_stop)
        return 0;
    for (int i = 0; i < LUA_RUNTIME_AUDIO_HANDLES; ++i) {
        if (context->handles[i]) {
            s_audio_stop(context->handles[i], s_audio_ctx);
            context->handles[i] = 0;
        }
    }
    return 0;
}

static int l_audio_play(lua_State *state) {
    const char *source = luaL_checkstring(state, 1);
    runtime_audio_context_t *context = get_audio_context(state);
    if (s_audio_is_playing) {
        for (int i = 0; i < LUA_RUNTIME_AUDIO_HANDLES; ++i) {
            if (context->handles[i] && !s_audio_is_playing(context->handles[i], s_audio_ctx))
                context->handles[i] = 0;
        }
    }
    int free_slot = find_audio_handle(context, 0);
    if (free_slot < 0)
        return luaL_error(state, "audio handle limit reached");
    bool has_options = !lua_isnoneornil(state, 2);
    if (has_options)
        luaL_checktype(state, 2, LUA_TTABLE);
    bool loop = false;
    lua_Integer volume = 100;
    if (has_options) {
        lua_getfield(state, 2, "loop");
        loop = lua_toboolean(state, -1);
        lua_pop(state, 1);
        lua_getfield(state, 2, "volume");
        volume = lua_isinteger(state, -1) ? lua_tointeger(state, -1) : 100;
        lua_pop(state, 1);
    }
    if (volume < 0 || volume > 100)
        return luaL_argerror(state, 2, "volume must be between 0 and 100");
    if (!s_audio_play)
        return luaL_error(state, "audio backend is not registered");
    uint32_t handle = 0;
    esp_err_t err = s_audio_play(source, loop, (uint8_t)volume, &handle, s_audio_ctx);
    if (err != ESP_OK)
        return luaL_error(state, "audio.play failed: %s", esp_err_to_name(err));
    context->handles[free_slot] = handle;
    lua_pushinteger(state, handle);
    return 1;
}

static int l_audio_play_bytes(lua_State *state) {
    size_t len = 0;
    const char *data = luaL_checklstring(state, 1, &len);
    runtime_audio_context_t *context = get_audio_context(state);
    if (s_audio_is_playing) {
        for (int i = 0; i < LUA_RUNTIME_AUDIO_HANDLES; ++i) {
            if (context->handles[i] && !s_audio_is_playing(context->handles[i], s_audio_ctx))
                context->handles[i] = 0;
        }
    }
    int free_slot = find_audio_handle(context, 0);
    if (free_slot < 0)
        return luaL_error(state, "audio handle limit reached");
    bool has_options = !lua_isnoneornil(state, 2);
    if (has_options)
        luaL_checktype(state, 2, LUA_TTABLE);
    bool loop = false;
    lua_Integer volume = 100;
    if (has_options) {
        lua_getfield(state, 2, "loop");
        loop = lua_toboolean(state, -1);
        lua_pop(state, 1);
        lua_getfield(state, 2, "volume");
        volume = lua_isnumber(state, -1) ? (lua_Integer)lua_tonumber(state, -1) : 100;
        lua_pop(state, 1);
    }
    if (volume < 0 || volume > 100)
        return luaL_argerror(state, 2, "volume must be between 0 and 100");
    if (!s_audio_play_bytes)
        return luaL_error(state, "audio.play_bytes backend is not registered");
    uint32_t handle = 0;
    esp_err_t err =
        s_audio_play_bytes(data, len, loop, (uint8_t)volume, &handle, s_audio_ctx);
    if (err != ESP_OK)
        return luaL_error(state, "audio.play_bytes failed: %s", esp_err_to_name(err));
    context->handles[free_slot] = handle;
    lua_pushinteger(state, handle);
    return 1;
}

static int l_audio_stop(lua_State *state) {
    uint32_t handle = (uint32_t)luaL_checkinteger(state, 1);
    runtime_audio_context_t *context = get_audio_context(state);
    int slot = find_audio_handle(context, handle);
    if (slot < 0)
        return luaL_error(state, "audio handle is not owned by this Lua job");
    if (!s_audio_stop)
        return luaL_error(state, "audio backend is not registered");
    esp_err_t err = s_audio_stop(handle, s_audio_ctx);
    if (err != ESP_OK)
        return luaL_error(state, "audio.stop failed: %s", esp_err_to_name(err));
    context->handles[slot] = 0;
    return 0;
}

static int l_audio_stop_all(lua_State *state) {
    runtime_audio_context_t *context = get_audio_context(state);
    if (!s_audio_stop)
        return luaL_error(state, "audio backend is not registered");
    for (int i = 0; i < LUA_RUNTIME_AUDIO_HANDLES; ++i) {
        if (context->handles[i]) {
            esp_err_t err = s_audio_stop(context->handles[i], s_audio_ctx);
            if (err != ESP_OK)
                return luaL_error(state, "audio.stop_all failed: %s", esp_err_to_name(err));
            context->handles[i] = 0;
        }
    }
    return 0;
}

static int l_audio_is_playing(lua_State *state) {
    uint32_t handle = (uint32_t)luaL_checkinteger(state, 1);
    runtime_audio_context_t *context = get_audio_context(state);
    if (find_audio_handle(context, handle) < 0) {
        lua_pushboolean(state, false);
        return 1;
    }
    lua_pushboolean(state, s_audio_is_playing && s_audio_is_playing(handle, s_audio_ctx));
    return 1;
}

static int luaopen_runtime(lua_State *state) {
    static const luaL_Reg functions[] = {
        {"sleep", l_runtime_sleep},
        {"sleep_until", l_runtime_sleep_until},
        {"now_ms", l_runtime_now_ms},
        {"cancelled", l_runtime_cancelled},
        {NULL, NULL},
    };
    luaL_newlib(state, functions);
    return 1;
}

static int l_speech_removed(lua_State *state) {
    return luaL_error(state,
                      "require(\"speech\") was removed. Use require(\"alert\") and alert.show "
                      "for on-screen alerts. TTS is the workflow speech node (WebSocket speak), "
                      "not a Lua module.");
}

static void preload_removed_speech(lua_State *state) {
    lua_getglobal(state, "package");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        return;
    }
    lua_getfield(state, -1, "preload");
    if (lua_istable(state, -1)) {
        lua_pushcfunction(state, l_speech_removed);
        lua_setfield(state, -2, "speech");
    }
    lua_pop(state, 2);
}

static int luaopen_audio(lua_State *state) {
    if (luaL_newmetatable(state, LUA_RUNTIME_AUDIO_CONTEXT)) {
        lua_pushcfunction(state, close_audio_context);
        lua_setfield(state, -2, "__gc");
    }
    lua_pop(state, 1);
    static const luaL_Reg functions[] = {
        {"play", l_audio_play},
        {"play_bytes", l_audio_play_bytes},
        {"stop", l_audio_stop},
        {"stop_all", l_audio_stop_all},
        {"is_playing", l_audio_is_playing},
        {NULL, NULL},
    };
    luaL_newlib(state, functions);
    return 1;
}

static void open_modules(lua_State *state) {
    luaL_openlibs(state);
    luaL_requiref(state, "ui", luaopen_ui, 1);
    lua_pop(state, 1);
    luaL_requiref(state, "runtime", luaopen_runtime, 1);
    lua_pop(state, 1);
    luaL_requiref(state, "audio", luaopen_audio, 1);
    lua_pop(state, 1);
    luaL_requiref(state, "uart", luaopen_uart, 1);
    lua_pop(state, 1);
    luaL_requiref(state, "http", luaopen_http, 1);
    lua_pop(state, 1);
    luaL_requiref(state, "camera", luaopen_camera, 1);
    lua_pop(state, 1);
    luaL_requiref(state, "alert", luaopen_alert, 1);
    lua_pop(state, 1);
    luaL_requiref(state, "device", luaopen_device, 1);
    lua_pop(state, 1);
    preload_removed_speech(state);
    for (size_t i = 0; i < s_module_count; ++i) {
        luaL_requiref(state, s_modules[i].name, s_modules[i].open_fn, 1);
        lua_pop(state, 1);
    }
}

static int l_open_modules(lua_State *state) {
    open_modules(state);
    return 0;
}

static esp_err_t push_json(lua_State *state, const cJSON *item, int depth) {
    if (depth > 32)
        return ESP_ERR_INVALID_SIZE;
    if (!item || cJSON_IsNull(item))
        lua_pushnil(state);
    else if (cJSON_IsBool(item))
        lua_pushboolean(state, cJSON_IsTrue(item));
    else if (cJSON_IsNumber(item)) {
        double value = item->valuedouble;
        lua_Integer integer = (lua_Integer)value;
        if ((double)integer == value)
            lua_pushinteger(state, integer);
        else
            lua_pushnumber(state, value);
    } else if (cJSON_IsString(item))
        lua_pushstring(state, item->valuestring);
    else if (cJSON_IsArray(item)) {
        lua_newtable(state);
        int index = 1;
        const cJSON *child = NULL;
        cJSON_ArrayForEach(child, item) {
            esp_err_t err = push_json(state, child, depth + 1);
            if (err != ESP_OK) {
                lua_pop(state, 1);
                return err;
            }
            lua_rawseti(state, -2, index++);
        }
    } else if (cJSON_IsObject(item)) {
        lua_newtable(state);
        const cJSON *child = NULL;
        cJSON_ArrayForEach(child, item) {
            esp_err_t err = push_json(state, child, depth + 1);
            if (err != ESP_OK) {
                lua_pop(state, 1);
                return err;
            }
            lua_setfield(state, -2, child->string);
        }
    } else {
        lua_pushnil(state);
    }
    return ESP_OK;
}

static esp_err_t set_args(lua_State *state, const char *json) {
    if (!json || !json[0]) {
        lua_newtable(state);
        lua_setglobal(state, "args");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return ESP_ERR_INVALID_ARG;
    esp_err_t err = push_json(state, root, 0);
    cJSON_Delete(root);
    if (err != ESP_OK)
        return err;
    lua_setglobal(state, "args");
    return ESP_OK;
}

static cJSON *lua_to_cjson(lua_State *state, int index, int depth);

static bool table_is_array(lua_State *state, int index, lua_Integer *length_out) {
    index = lua_absindex(state, index);
    lua_Integer length = (lua_Integer)lua_rawlen(state, index);
    if (length <= 0)
        return false;
    int keys = 0;
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        keys++;
        if (!lua_isinteger(state, -2)) {
            lua_pop(state, 2);
            return false;
        }
        lua_Integer key = lua_tointeger(state, -2);
        if (key < 1 || key > length) {
            lua_pop(state, 2);
            return false;
        }
        lua_pop(state, 1);
    }
    *length_out = length;
    return keys == (int)length;
}

static cJSON *lua_to_cjson(lua_State *state, int index, int depth) {
    if (depth > LUA_RUNTIME_RESULT_MAX_DEPTH)
        return cJSON_CreateNull();
    index = lua_absindex(state, index);
    switch (lua_type(state, index)) {
        case LUA_TNIL:
            return cJSON_CreateNull();
        case LUA_TBOOLEAN:
            return cJSON_CreateBool(lua_toboolean(state, index));
        case LUA_TNUMBER: {
            if (lua_isinteger(state, index))
                return cJSON_CreateNumber((double)lua_tointeger(state, index));
            double value = lua_tonumber(state, index);
            if (!isfinite(value))
                return cJSON_CreateNull();
            return cJSON_CreateNumber(value);
        }
        case LUA_TSTRING:
            return cJSON_CreateString(lua_tostring(state, index));
        case LUA_TTABLE: {
            lua_Integer length = 0;
            if (table_is_array(state, index, &length)) {
                cJSON *array = cJSON_CreateArray();
                if (!array)
                    return cJSON_CreateNull();
                lua_Integer limit = length < LUA_RUNTIME_RESULT_MAX_KEYS ? length
                                                                         : LUA_RUNTIME_RESULT_MAX_KEYS;
                for (lua_Integer i = 1; i <= limit; ++i) {
                    lua_rawgeti(state, index, i);
                    cJSON *item = lua_to_cjson(state, -1, depth + 1);
                    if (!item)
                        item = cJSON_CreateNull();
                    cJSON_AddItemToArray(array, item);
                    lua_pop(state, 1);
                }
                return array;
            }
            cJSON *object = cJSON_CreateObject();
            if (!object)
                return cJSON_CreateNull();
            int keys = 0;
            lua_pushnil(state);
            while (lua_next(state, index) != 0) {
                if (keys >= LUA_RUNTIME_RESULT_MAX_KEYS) {
                    lua_pop(state, 2);
                    break;
                }
                char key_buf[32];
                const char *key = NULL;
                if (lua_type(state, -2) == LUA_TSTRING) {
                    key = lua_tostring(state, -2);
                } else if (lua_isinteger(state, -2)) {
                    snprintf(key_buf, sizeof(key_buf), "%lld",
                             (long long)lua_tointeger(state, -2));
                    key = key_buf;
                }
                if (key) {
                    cJSON *item = lua_to_cjson(state, -1, depth + 1);
                    if (!item)
                        item = cJSON_CreateNull();
                    cJSON_AddItemToObject(object, key, item);
                    keys++;
                }
                lua_pop(state, 1);
            }
            return object;
        }
        default: {
            char buf[32];
            snprintf(buf, sizeof(buf), "<%s>", luaL_typename(state, index));
            return cJSON_CreateString(buf);
        }
    }
}

static void store_lua_result(runtime_job_t *job, lua_State *state) {
    if (!job->result)
        return;
    int nresults = lua_gettop(state);
    cJSON *root = NULL;
    if (nresults <= 0) {
        root = cJSON_CreateNull();
    } else if (nresults == 1) {
        root = lua_to_cjson(state, 1, 0);
    } else {
        root = cJSON_CreateArray();
        if (root) {
            int limit = nresults < LUA_RUNTIME_RESULT_MAX_KEYS ? nresults : LUA_RUNTIME_RESULT_MAX_KEYS;
            for (int i = 1; i <= limit; ++i) {
                cJSON *item = lua_to_cjson(state, i, 0);
                if (!item)
                    item = cJSON_CreateNull();
                cJSON_AddItemToArray(root, item);
            }
        }
    }
    if (!root)
        root = cJSON_CreateNull();
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        memcpy(job->result, "null", 5);
        job->result_length = 4;
        job->result_truncated = true;
        return;
    }
    size_t length = strlen(printed);
    if (length >= LUA_RUNTIME_RESULT_SIZE) {
        memcpy(job->result, "null", 5);
        job->result_length = 4;
        job->result_truncated = true;
    } else {
        memcpy(job->result, printed, length + 1);
        job->result_length = length;
        job->result_truncated = false;
    }
    cJSON_free(printed);
}

static int call_entry_function(lua_State *state, const char *entry) {
    lua_settop(state, 0);
    lua_getglobal(state, entry);
    if (!lua_isfunction(state, -1)) {
        lua_pop(state, 1);
        lua_pushfstring(state, "entry function not found: %s", entry);
        return LUA_ERRRUN;
    }
    lua_getglobal(state, "args");
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        lua_newtable(state);
    }
    return lua_pcall(state, 1, LUA_MULTRET, 0);
}

static esp_err_t execute_job(runtime_job_t *job) {
    char *source = job->code;
    size_t source_length = source ? strlen(source) : 0;
    if (!source && job->path) {
        struct stat st;
        if (stat(job->path, &st) != 0)
            return ESP_ERR_NOT_FOUND;
        if (st.st_size <= 0 || st.st_size > LUA_RUNTIME_MAX_CODE)
            return ESP_ERR_INVALID_SIZE;
        FILE *file = fopen(job->path, "rb");
        if (!file)
            return ESP_ERR_NOT_FOUND;
        source = malloc((size_t)st.st_size + 1);
        if (!source) {
            fclose(file);
            return ESP_ERR_NO_MEM;
        }
        source_length = fread(source, 1, (size_t)st.st_size, file);
        bool read_complete = source_length == (size_t)st.st_size && !ferror(file);
        fclose(file);
        if (!read_complete) {
            free(source);
            return ESP_FAIL;
        }
        source[source_length] = '\0';
    }
    if (!source || source_length == 0 || source_length > LUA_RUNTIME_MAX_CODE) {
        if (source != job->code)
            free(source);
        return ESP_ERR_INVALID_SIZE;
    }
    lua_State *state = luaL_newstate();
    if (!state) {
        if (source != job->code)
            free(source);
        return ESP_ERR_NO_MEM;
    }
    lua_runtime_exec_context_t context = {
        .job_name = job->name,
        .output = job->output,
        .output_size = LUA_RUNTIME_OUTPUT_SIZE,
        .deadline_us = job->timeout_ms ? esp_timer_get_time() + (int64_t)job->timeout_ms * 1000 : 0,
        .stop_requested = &job->stop_requested,
        .capabilities = job->capabilities,
    };
    lua_runtime_set_context(state, &context);
    lua_pushcfunction(state, l_open_modules);
    int result = lua_pcall(state, 0, 0, 0);
    if (result != LUA_OK) {
        const char *message = lua_tostring(state, -1);
        lua_runtime_append_output(&context, "ERROR: module initialization failed: ", 37);
        lua_runtime_append_output(&context, message ? message : "unknown error",
                                  message ? strlen(message) : 13);
        lua_runtime_append_output(&context, "\n", 1);
        job->output_length = context.output_length;
        job->output_truncated = context.truncated;
        lua_close(state);
        if (source != job->code)
            free(source);
        return ESP_FAIL;
    }
    lua_pushlightuserdata(state, &context);
    lua_pushcclosure(state, lua_print, 1);
    lua_setglobal(state, "print");
    esp_err_t args_result = set_args(state, job->args_json);
    if (args_result != ESP_OK) {
        lua_runtime_append_output(&context, "ERROR: invalid args_json\n", 25);
        job->output_length = context.output_length;
        job->output_truncated = context.truncated;
        lua_close(state);
        if (source != job->code)
            free(source);
        return args_result;
    }
    lua_sethook(state, lua_hook, LUA_MASKCOUNT, 1000);
    result = luaL_loadbuffer(state, source, source_length, job->name ? job->name : "lua_job");
    if (result == LUA_OK)
        result = lua_pcall(state, 0, LUA_MULTRET, 0);
    if (result == LUA_OK && job->entry && job->entry[0])
        result = call_entry_function(state, job->entry);
    if (result != LUA_OK) {
        const char *message = lua_tostring(state, -1);
        lua_runtime_append_output(&context, "ERROR: ", 7);
        lua_runtime_append_output(&context, message ? message : "unknown error",
                                  message ? strlen(message) : 13);
        lua_runtime_append_output(&context, "\n", 1);
    } else if (job->entry && job->entry[0]) {
        store_lua_result(job, state);
    }
    job->output_length = context.output_length;
    job->output_truncated = context.truncated;
    lua_close(state);
    if (source != job->code)
        free(source);
    if (job->stop_requested)
        return ESP_ERR_INVALID_STATE;
    if (result != LUA_OK)
        return context.deadline_us && esp_timer_get_time() >= context.deadline_us ? ESP_ERR_TIMEOUT
                                                                                  : ESP_FAIL;
    return ESP_OK;
}

static void job_task(void *arg) {
    runtime_job_t *job = arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    job->state = LUA_RUNTIME_JOB_RUNNING;
    xSemaphoreGive(s_lock);
    esp_err_t result = execute_job(job);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (job->stop_requested)
        job->state = LUA_RUNTIME_JOB_STOPPED;
    else if (result == ESP_OK)
        job->state = LUA_RUNTIME_JOB_DONE;
    else if (result == ESP_ERR_TIMEOUT)
        job->state = LUA_RUNTIME_JOB_TIMEOUT;
    else
        job->state = LUA_RUNTIME_JOB_FAILED;
    lua_runtime_job_info_t info = {.id = job->id,
                                   .state = job->state,
                                   .output_length = job->output_length,
                                   .output_truncated = job->output_truncated,
                                   .result_length = job->result_length,
                                   .result_truncated = job->result_truncated};
    lua_runtime_job_callback_t callback = s_callback;
    void *callback_ctx = s_callback_ctx;
    xSemaphoreGive(s_lock);
    if (callback)
        callback(&info, job->output, callback_ctx);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    job->task = NULL;
    xSemaphoreGive(s_lock);
    vTaskDelete(NULL);
}

esp_err_t lua_runtime_init(void) {
    if (s_initialized)
        return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock)
        return ESP_ERR_NO_MEM;
    memset(s_jobs, 0, sizeof(s_jobs));
    memset(s_modules, 0, sizeof(s_modules));
    s_module_count = 0;
    s_callback = NULL;
    s_callback_ctx = NULL;
    s_audio_play = NULL;
    s_audio_play_bytes = NULL;
    s_audio_stop = NULL;
    s_audio_stop_all = NULL;
    s_audio_is_playing = NULL;
    s_audio_ctx = NULL;
    esp_err_t uart_result = lua_uart_init();
    if (uart_result != ESP_OK) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return uart_result;
    }
    esp_err_t http_result = lua_http_init();
    if (http_result != ESP_OK) {
        lua_uart_deinit();
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return http_result;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "independent Lua runtime initialized");
    return ESP_OK;
}

esp_err_t lua_runtime_deinit(void) {
    if (!s_initialized)
        return ESP_OK;
    for (size_t i = 0; i < LUA_RUNTIME_MAX_JOBS; ++i) {
        if (s_jobs[i].used && s_jobs[i].task)
            s_jobs[i].stop_requested = true;
    }
    for (int attempt = 0; attempt < 300; ++attempt) {
        bool running = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (size_t i = 0; i < LUA_RUNTIME_MAX_JOBS; ++i) {
            if (s_jobs[i].used && s_jobs[i].task) {
                running = true;
                break;
            }
        }
        xSemaphoreGive(s_lock);
        if (!running)
            break;
        vTaskDelay(pdMS_TO_TICKS(10));
        if (attempt == 299)
            return ESP_ERR_TIMEOUT;
    }
    for (size_t i = 0; i < LUA_RUNTIME_MAX_JOBS; ++i)
        free_job(&s_jobs[i]);
    lua_http_deinit();
    lua_uart_deinit();
    vSemaphoreDelete(s_lock);
    s_lock = NULL;
    s_initialized = false;
    return ESP_OK;
}

esp_err_t lua_runtime_register_module(const char *name, lua_CFunction open_fn) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!name || !name[0] || !open_fn) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < LUA_RUNTIME_MAX_JOBS; ++i) {
        if (s_jobs[i].used && s_jobs[i].state < LUA_RUNTIME_JOB_DONE) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (s_module_count >= LUA_RUNTIME_MAX_MODULES) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    if (strlcpy(s_modules[s_module_count].name, name, sizeof(s_modules[s_module_count].name)) >=
        sizeof(s_modules[s_module_count].name)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_SIZE;
    }
    s_modules[s_module_count++].open_fn = open_fn;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static runtime_job_t *new_job(const lua_runtime_job_config_t *config) {
    for (size_t i = 0; i < LUA_RUNTIME_MAX_JOBS; ++i)
        if (!s_jobs[i].used ||
            (s_jobs[i].task == NULL && s_jobs[i].state >= LUA_RUNTIME_JOB_DONE)) {
            runtime_job_t *job = &s_jobs[i];
            free_job(job);
            job->used = true;
            job->id = s_next_id++;
            job->state = LUA_RUNTIME_JOB_QUEUED;
            job->timeout_ms = config->timeout_ms;
            job->capabilities = config->capabilities;
            job->name = strdup(config->name ? config->name : "lua_job");
            job->code = config->code ? strdup(config->code) : NULL;
            job->path = config->path ? strdup(config->path) : NULL;
            job->args_json = config->args_json ? strdup(config->args_json) : NULL;
            job->entry = config->entry ? strdup(config->entry) : NULL;
            job->output = calloc(1, LUA_RUNTIME_OUTPUT_SIZE);
            job->result = calloc(1, LUA_RUNTIME_RESULT_SIZE);
            if (!job->name || (config->code && !job->code) || (config->path && !job->path) ||
                (config->args_json && !job->args_json) || (config->entry && !job->entry) ||
                !job->output || !job->result) {
                free_job(job);
                return NULL;
            }
            return job;
        }
    return NULL;
}

esp_err_t lua_runtime_start(const lua_runtime_job_config_t *config, lua_runtime_job_id_t *job_id) {
    if (!s_initialized)
        return ESP_ERR_INVALID_STATE;
    if (!config || !job_id || (!config->code && !config->path))
        return ESP_ERR_INVALID_ARG;
    if (config->code && strlen(config->code) > LUA_RUNTIME_MAX_CODE)
        return ESP_ERR_INVALID_SIZE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    runtime_job_t *job = new_job(config);
    if (job)
        *job_id = job->id;
    xSemaphoreGive(s_lock);
    if (!job)
        return ESP_ERR_NO_MEM;
    uint32_t stack = config->stack_size ? config->stack_size : LUA_RUNTIME_DEFAULT_STACK;
    int priority = config->priority ? config->priority : LUA_RUNTIME_DEFAULT_PRIORITY;
    if (xTaskCreate(job_task, "lua_job", stack, job, priority, &job->task) != pdPASS) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        free_job(job);
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t lua_runtime_run(const lua_runtime_job_config_t *config, char *output,
                          size_t output_size) {
    lua_runtime_job_id_t id;
    esp_err_t err = lua_runtime_start(config, &id);
    if (err != ESP_OK)
        return err;
    while (true) {
        lua_runtime_job_info_t info;
        err = lua_runtime_get_job(id, &info, output, output_size);
        if (err != ESP_OK)
            return err;
        if (info.state >= LUA_RUNTIME_JOB_DONE) {
            if (info.state == LUA_RUNTIME_JOB_DONE)
                return ESP_OK;
            if (info.state == LUA_RUNTIME_JOB_TIMEOUT)
                return ESP_ERR_TIMEOUT;
            if (info.state == LUA_RUNTIME_JOB_STOPPED)
                return ESP_ERR_INVALID_STATE;
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t lua_runtime_stop(lua_runtime_job_id_t job_id) {
    if (!s_initialized)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < LUA_RUNTIME_MAX_JOBS; ++i) {
        if (s_jobs[i].used && s_jobs[i].id == job_id) {
            s_jobs[i].stop_requested = true;
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t lua_runtime_get_job(lua_runtime_job_id_t job_id, lua_runtime_job_info_t *info,
                              char *output, size_t output_size) {
    if (!s_initialized)
        return ESP_ERR_INVALID_STATE;
    if (!info)
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < LUA_RUNTIME_MAX_JOBS; ++i) {
        if (s_jobs[i].used && s_jobs[i].id == job_id) {
            *info = (lua_runtime_job_info_t){
                .id = s_jobs[i].id,
                .state = s_jobs[i].state,
                .output_length = s_jobs[i].output_length,
                .output_truncated = s_jobs[i].output_truncated,
                .result_length = s_jobs[i].result_length,
                .result_truncated = s_jobs[i].result_truncated,
            };
            if (output && output_size) {
                strlcpy(output, s_jobs[i].output ? s_jobs[i].output : "", output_size);
            }
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t lua_runtime_get_job_result(lua_runtime_job_id_t job_id, char *result, size_t result_size) {
    if (!s_initialized)
        return ESP_ERR_INVALID_STATE;
    if (!result || result_size == 0)
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < LUA_RUNTIME_MAX_JOBS; ++i) {
        if (s_jobs[i].used && s_jobs[i].id == job_id) {
            strlcpy(result, s_jobs[i].result ? s_jobs[i].result : "", result_size);
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t lua_runtime_set_callback(lua_runtime_job_callback_t callback, void *user_ctx) {
    if (!s_initialized)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_callback = callback;
    s_callback_ctx = user_ctx;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t lua_runtime_set_audio_backend(lua_runtime_audio_play_callback_t play,
                                        lua_runtime_audio_stop_callback_t stop,
                                        lua_runtime_audio_simple_callback_t stop_all,
                                        lua_runtime_audio_is_playing_callback_t is_playing,
                                        void *user_ctx) {
    if (!s_initialized)
        return ESP_ERR_INVALID_STATE;
    if (!play || !stop || !stop_all)
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_audio_play = play;
    s_audio_stop = stop;
    s_audio_stop_all = stop_all;
    s_audio_is_playing = is_playing;
    s_audio_ctx = user_ctx;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t lua_runtime_set_audio_bytes_backend(lua_runtime_audio_play_bytes_callback_t play_bytes,
                                              void *user_ctx) {
    if (!s_initialized)
        return ESP_ERR_INVALID_STATE;
    if (!play_bytes)
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_audio_play_bytes = play_bytes;
    if (user_ctx)
        s_audio_ctx = user_ctx;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
