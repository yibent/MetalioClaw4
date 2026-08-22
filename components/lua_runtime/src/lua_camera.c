#include "lua_runtime.h"

#include <string.h>

#include "esp_err.h"
#include "lauxlib.h"
#include "lua_runtime_internal.h"

static lua_runtime_camera_explain_callback_t s_explain;
static void* s_ctx;

esp_err_t lua_runtime_set_camera_backend(lua_runtime_camera_explain_callback_t explain,
                                         void* user_ctx) {
    if (!explain)
        return ESP_ERR_INVALID_ARG;
    s_explain = explain;
    s_ctx = user_ctx;
    return ESP_OK;
}

static int l_explain(lua_State* state) {
    lua_runtime_check_abort(state);
    lua_runtime_exec_context_t* context = lua_runtime_get_context(state);
    if (!context || !(context->capabilities & LUA_RUNTIME_CAP_CAMERA))
        return luaL_error(state, "camera capability is not granted");
    if (!s_explain)
        return luaL_error(state, "camera backend is not registered");
    const char* question = luaL_optstring(state, 1, "描述这张图片");
    char output[LUA_RUNTIME_OUTPUT_SIZE];
    output[0] = '\0';
    esp_err_t err = s_explain(question, output, sizeof(output), s_ctx);
    if (err != ESP_OK) {
        lua_pushnil(state);
        lua_pushstring(state, output[0] ? output : esp_err_to_name(err));
        return 2;
    }
    lua_pushstring(state, output);
    return 1;
}

static int l_capture(lua_State* state) {
    return l_explain(state);
}

int luaopen_camera(lua_State* state) {
    static const luaL_Reg functions[] = {
        {"explain", l_explain},
        {"capture", l_capture},
        {NULL, NULL},
    };
    luaL_newlib(state, functions);
    return 1;
}
