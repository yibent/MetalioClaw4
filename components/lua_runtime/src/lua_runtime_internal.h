#pragma once

#include "lua.h"
#include "lua_runtime.h"

typedef struct {
    const char* job_name;
    char* output;
    size_t output_size;
    size_t output_length;
    bool truncated;
    int64_t deadline_us;
    volatile bool* stop_requested;
    uint32_t capabilities;
} lua_runtime_exec_context_t;

void lua_runtime_set_context(lua_State* state, lua_runtime_exec_context_t* context);
lua_runtime_exec_context_t* lua_runtime_get_context(lua_State* state);
void lua_runtime_append_output(lua_runtime_exec_context_t* context, const char* text,
                               size_t length);
int lua_runtime_check_abort(lua_State* state);
int luaopen_uart(lua_State* state);
esp_err_t lua_uart_init(void);
void lua_uart_deinit(void);
int luaopen_http(lua_State* state);
esp_err_t lua_http_init(void);
void lua_http_deinit(void);
int luaopen_camera(lua_State* state);
int luaopen_speech(lua_State* state);
int luaopen_device(lua_State* state);
