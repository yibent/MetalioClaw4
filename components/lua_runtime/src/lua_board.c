#include "lua_runtime.h"

#include "esp_err.h"
#include "lauxlib.h"
#include "lua_runtime_internal.h"

static lua_runtime_speech_say_callback_t s_say;
static lua_runtime_device_set_int_callback_t s_set_brightness;
static lua_runtime_device_set_int_callback_t s_set_volume;
static lua_runtime_device_vibrate_callback_t s_vibrate;
static lua_runtime_device_notify_callback_t s_notify;
static void* s_speech_ctx;
static void* s_device_ctx;

esp_err_t lua_runtime_set_speech_backend(lua_runtime_speech_say_callback_t say, void* user_ctx) {
    if (!say)
        return ESP_ERR_INVALID_ARG;
    s_say = say;
    s_speech_ctx = user_ctx;
    return ESP_OK;
}

esp_err_t lua_runtime_set_device_backend(lua_runtime_device_set_int_callback_t set_brightness,
                                         lua_runtime_device_set_int_callback_t set_volume,
                                         lua_runtime_device_vibrate_callback_t vibrate,
                                         lua_runtime_device_notify_callback_t notify,
                                         void* user_ctx) {
    if (!set_brightness || !set_volume || !vibrate || !notify)
        return ESP_ERR_INVALID_ARG;
    s_set_brightness = set_brightness;
    s_set_volume = set_volume;
    s_vibrate = vibrate;
    s_notify = notify;
    s_device_ctx = user_ctx;
    return ESP_OK;
}

static int l_say(lua_State* state) {
    lua_runtime_check_abort(state);
    const char* text = luaL_optstring(state, 1, "");
    if (!s_say)
        return luaL_error(state, "speech backend is not registered");
    esp_err_t err = s_say(text, s_speech_ctx);
    if (err != ESP_OK)
        return luaL_error(state, "speech.say failed: %s", esp_err_to_name(err));
    return 0;
}

static int l_set_brightness(lua_State* state) {
    lua_runtime_check_abort(state);
    int value = (int)luaL_checkinteger(state, 1);
    if (!s_set_brightness)
        return luaL_error(state, "device backend is not registered");
    esp_err_t err = s_set_brightness(value, s_device_ctx);
    if (err != ESP_OK)
        return luaL_error(state, "device.set_brightness failed: %s", esp_err_to_name(err));
    return 0;
}

static int l_set_volume(lua_State* state) {
    lua_runtime_check_abort(state);
    int value = (int)luaL_checkinteger(state, 1);
    if (!s_set_volume)
        return luaL_error(state, "device backend is not registered");
    esp_err_t err = s_set_volume(value, s_device_ctx);
    if (err != ESP_OK)
        return luaL_error(state, "device.set_volume failed: %s", esp_err_to_name(err));
    return 0;
}

static int l_vibrate(lua_State* state) {
    lua_runtime_check_abort(state);
    lua_Integer ms = luaL_optinteger(state, 1, 300);
    if (ms < 0)
        ms = 0;
    if (ms > 5000)
        ms = 5000;
    if (!s_vibrate)
        return luaL_error(state, "device backend is not registered");
    esp_err_t err = s_vibrate((uint32_t)ms, s_device_ctx);
    if (err != ESP_OK)
        return luaL_error(state, "device.vibrate failed: %s", esp_err_to_name(err));
    return 0;
}

static int l_notify(lua_State* state) {
    lua_runtime_check_abort(state);
    const char* text = luaL_optstring(state, 1, "");
    if (!s_notify)
        return luaL_error(state, "device backend is not registered");
    esp_err_t err = s_notify(text, s_device_ctx);
    if (err != ESP_OK)
        return luaL_error(state, "device.notify failed: %s", esp_err_to_name(err));
    return 0;
}

int luaopen_speech(lua_State* state) {
    static const luaL_Reg functions[] = {
        {"say", l_say},
        {NULL, NULL},
    };
    luaL_newlib(state, functions);
    return 1;
}

int luaopen_device(lua_State* state) {
    static const luaL_Reg functions[] = {
        {"set_brightness", l_set_brightness},
        {"set_volume", l_set_volume},
        {"vibrate", l_vibrate},
        {"notify", l_notify},
        {NULL, NULL},
    };
    luaL_newlib(state, functions);
    return 1;
}
