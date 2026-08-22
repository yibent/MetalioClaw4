#include "lua_runtime.h"

#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "lauxlib.h"
#include "lua_runtime_internal.h"

#define HTTP_MAX_METHOD 8

static lua_runtime_http_request_callback_t s_http_request;
static void* s_http_ctx;

static int ascii_ieq(const char* a, const char* b) {
    if (!a || !b)
        return 0;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z')
            ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb)
            return 0;
    }
    return *a == *b;
}

static int is_managed_request_header(const char* key) {
    return ascii_ieq(key, "connection") || ascii_ieq(key, "content-length") ||
           ascii_ieq(key, "transfer-encoding") || ascii_ieq(key, "host");
}

static int normalize_method(const char* in, char* out, size_t out_size) {
    static const char* kAllowed[] = {"GET", "POST", "PUT", "DELETE", "HEAD", NULL};
    if (!in || !out || out_size < 2)
        return 0;
    size_t n = 0;
    for (; in[n]; ++n) {
        if (n + 1 >= out_size)
            return 0;
        char c = in[n];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        out[n] = c;
    }
    out[n] = '\0';
    for (int i = 0; kAllowed[i]; ++i) {
        if (strcmp(out, kAllowed[i]) == 0)
            return 1;
    }
    return 0;
}

static int validate_url(lua_State* state, const char* url) {
    if (!url || !url[0])
        return luaL_argerror(state, 1, "url is required");
    if (strlen(url) > LUA_RUNTIME_HTTP_MAX_URL)
        return luaL_argerror(state, 1, "url is too long");
    const char* host = NULL;
    if (strncmp(url, "https://", 8) == 0)
        host = url + 8;
    else if (strncmp(url, "http://", 7) == 0)
        host = url + 7;
    else
        return luaL_argerror(state, 1, "url must start with http:// or https://");
    if (host[0] == '\0' || host[0] == '/' || host[0] == ':')
        return luaL_argerror(state, 1, "url host is missing");
    for (const unsigned char* p = (const unsigned char*)url; *p; ++p) {
        if (*p <= 0x20 || *p == 0x7f)
            return luaL_argerror(state, 1, "url contains invalid characters");
    }
    return 0;
}

static void free_header_copies(char** keys, char** values, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        free(keys[i]);
        free(values[i]);
        keys[i] = NULL;
        values[i] = NULL;
    }
}

void lua_runtime_http_response_free(lua_runtime_http_response_t* res) {
    if (!res)
        return;
    free(res->error);
    free(res->body);
    if (res->header_keys || res->header_values) {
        for (size_t i = 0; i < res->header_count; ++i) {
            if (res->header_keys)
                free(res->header_keys[i]);
            if (res->header_values)
                free(res->header_values[i]);
        }
        free(res->header_keys);
        free(res->header_values);
    }
    memset(res, 0, sizeof(*res));
}

static const char* transport_error(esp_err_t err, const lua_runtime_http_response_t* res) {
    if (res && res->error && res->error[0])
        return res->error;
    switch (err) {
        case ESP_ERR_TIMEOUT:
            return "timeout";
        case ESP_ERR_INVALID_STATE:
            return "stopped";
        case ESP_ERR_NO_MEM:
            return "out of memory";
        case ESP_ERR_INVALID_SIZE:
            return "response too large";
        case ESP_ERR_NOT_FOUND:
            return "network is not available";
        default:
            return esp_err_to_name(err);
    }
}

static int copy_options_table(lua_State* state, int opts_index) {
    lua_newtable(state);
    if (opts_index <= 0 || lua_isnoneornil(state, opts_index))
        return lua_gettop(state);
    opts_index = lua_absindex(state, opts_index);
    int dest = lua_gettop(state);
    lua_pushnil(state);
    while (lua_next(state, opts_index) != 0) {
        lua_pushvalue(state, -2);
        lua_insert(state, -2);
        lua_settable(state, dest);
    }
    return dest;
}

static int l_http_request(lua_State* state) {
    lua_runtime_exec_context_t* context = lua_runtime_get_context(state);
    if (!context || !(context->capabilities & LUA_RUNTIME_CAP_HTTP))
        return luaL_error(state, "HTTP capability is not granted to this Lua job");
    if (!s_http_request)
        return luaL_error(state, "HTTP backend is not registered");
    luaL_checktype(state, 1, LUA_TTABLE);
    lua_runtime_check_abort(state);

    char method[HTTP_MAX_METHOD] = "GET";
    lua_getfield(state, 1, "method");
    if (!lua_isnoneornil(state, -1)) {
        const char* raw = luaL_checkstring(state, -1);
        if (!normalize_method(raw, method, sizeof(method)))
            return luaL_argerror(state, 1, "method must be GET, POST, PUT, DELETE, or HEAD");
    }
    lua_pop(state, 1);

    lua_getfield(state, 1, "url");
    const char* url = luaL_checkstring(state, -1);
    validate_url(state, url);

    lua_getfield(state, 1, "timeout_ms");
    lua_Integer timeout = luaL_optinteger(state, -1, (lua_Integer)LUA_RUNTIME_HTTP_DEFAULT_TIMEOUT_MS);
    lua_pop(state, 1);
    if (timeout <= 0 || timeout > (lua_Integer)LUA_RUNTIME_HTTP_MAX_TIMEOUT_MS)
        return luaL_argerror(state, 1, "timeout_ms must be between 1 and 60000");

    lua_getfield(state, 1, "max_body");
    lua_Integer max_body = luaL_optinteger(state, -1, (lua_Integer)LUA_RUNTIME_HTTP_DEFAULT_MAX_BODY);
    lua_pop(state, 1);
    if (max_body <= 0 || max_body > (lua_Integer)LUA_RUNTIME_HTTP_HARD_MAX_BODY)
        return luaL_argerror(state, 1, "max_body must be between 1 and 262144");

    lua_getfield(state, 1, "body");
    const char* body = NULL;
    size_t body_len = 0;
    if (!lua_isnoneornil(state, -1)) {
        if (!lua_isstring(state, -1))
            return luaL_argerror(state, 1, "body must be a string");
        body = lua_tolstring(state, -1, &body_len);
        if (body_len > LUA_RUNTIME_HTTP_MAX_REQUEST_BODY)
            return luaL_argerror(state, 1, "body is limited to 65536 bytes");
    }

    char* header_keys[LUA_RUNTIME_HTTP_MAX_HEADERS] = {};
    char* header_values[LUA_RUNTIME_HTTP_MAX_HEADERS] = {};
    size_t header_count = 0;
    lua_getfield(state, 1, "headers");
    int headers_index = lua_gettop(state);
    if (!lua_isnoneornil(state, headers_index)) {
        luaL_checktype(state, headers_index, LUA_TTABLE);
        lua_pushnil(state);
        while (lua_next(state, headers_index) != 0) {
            if (!lua_isstring(state, -2) || !lua_isstring(state, -1)) {
                free_header_copies(header_keys, header_values, header_count);
                return luaL_argerror(state, 1, "headers must be string keys and values");
            }
            size_t key_len = 0;
            size_t value_len = 0;
            const char* key = lua_tolstring(state, -2, &key_len);
            const char* value = lua_tolstring(state, -1, &value_len);
            if (key_len == 0 || key_len > LUA_RUNTIME_HTTP_MAX_HEADER_SIZE ||
                value_len > LUA_RUNTIME_HTTP_MAX_HEADER_SIZE) {
                free_header_copies(header_keys, header_values, header_count);
                return luaL_argerror(state, 1, "header name or value is invalid");
            }
            if (!is_managed_request_header(key)) {
                if (header_count >= LUA_RUNTIME_HTTP_MAX_HEADERS) {
                    free_header_copies(header_keys, header_values, header_count);
                    return luaL_argerror(state, 1, "too many headers");
                }
                header_keys[header_count] = strdup(key);
                header_values[header_count] = strdup(value);
                if (!header_keys[header_count] || !header_values[header_count]) {
                    free(header_keys[header_count]);
                    free(header_values[header_count]);
                    free_header_copies(header_keys, header_values, header_count);
                    return luaL_error(state, "out of memory");
                }
                header_count++;
            }
            lua_pop(state, 1);
        }
    }

    const char* header_key_view[LUA_RUNTIME_HTTP_MAX_HEADERS];
    const char* header_value_view[LUA_RUNTIME_HTTP_MAX_HEADERS];
    for (size_t i = 0; i < header_count; ++i) {
        header_key_view[i] = header_keys[i];
        header_value_view[i] = header_values[i];
    }
    lua_runtime_http_request_t req = {
        .method = method,
        .url = url,
        .header_keys = header_count ? header_key_view : NULL,
        .header_values = header_count ? header_value_view : NULL,
        .header_count = header_count,
        .body = body,
        .body_len = body_len,
        .timeout_ms = (uint32_t)timeout,
        .max_body = (size_t)max_body,
        .stop_requested = context->stop_requested,
        .deadline_us = context->deadline_us,
    };
    lua_runtime_http_response_t res = {};
    esp_err_t err = s_http_request(&req, &res, s_http_ctx);
    free_header_copies(header_keys, header_values, header_count);
    if (err != ESP_OK) {
        char message[96];
        strlcpy(message, transport_error(err, &res), sizeof(message));
        lua_runtime_http_response_free(&res);
        if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_STATE)
            lua_runtime_check_abort(state);
        lua_pushnil(state);
        lua_pushstring(state, message);
        return 2;
    }

    lua_newtable(state);
    lua_pushinteger(state, res.status);
    lua_setfield(state, -2, "status");
    lua_pushlstring(state, res.body ? res.body : "", res.body_len);
    lua_setfield(state, -2, "body");
    lua_newtable(state);
    for (size_t i = 0; i < res.header_count; ++i) {
        if (!res.header_keys || !res.header_keys[i] || !res.header_values)
            continue;
        lua_pushstring(state, res.header_values[i] ? res.header_values[i] : "");
        lua_setfield(state, -2, res.header_keys[i]);
    }
    lua_setfield(state, -2, "headers");
    lua_runtime_http_response_free(&res);
    return 1;
}

static int l_http_get(lua_State* state) {
    const char* url = luaL_checkstring(state, 1);
    if (!lua_isnoneornil(state, 2))
        luaL_checktype(state, 2, LUA_TTABLE);
    copy_options_table(state, lua_isnoneornil(state, 2) ? 0 : 2);
    lua_pushstring(state, "GET");
    lua_setfield(state, -2, "method");
    lua_pushstring(state, url);
    lua_setfield(state, -2, "url");
    lua_replace(state, 1);
    lua_settop(state, 1);
    return l_http_request(state);
}

static int l_http_post(lua_State* state) {
    const char* url = luaL_checkstring(state, 1);
    int body_index = 0;
    int opts_index = 0;
    if (!lua_isnoneornil(state, 2)) {
        if (lua_isstring(state, 2))
            body_index = 2;
        else if (lua_istable(state, 2))
            opts_index = 2;
        else
            return luaL_argerror(state, 2, "body must be a string");
    }
    if (!lua_isnoneornil(state, 3)) {
        if (body_index == 0)
            return luaL_argerror(state, 3, "unexpected argument");
        luaL_checktype(state, 3, LUA_TTABLE);
        opts_index = 3;
    }
    copy_options_table(state, opts_index);
    lua_pushstring(state, "POST");
    lua_setfield(state, -2, "method");
    lua_pushstring(state, url);
    lua_setfield(state, -2, "url");
    if (body_index) {
        lua_pushvalue(state, body_index);
        lua_setfield(state, -2, "body");
    }
    lua_replace(state, 1);
    lua_settop(state, 1);
    return l_http_request(state);
}

int luaopen_http(lua_State* state) {
    static const luaL_Reg functions[] = {
        {"request", l_http_request},
        {"get", l_http_get},
        {"post", l_http_post},
        {NULL, NULL},
    };
    luaL_newlib(state, functions);
    return 1;
}

esp_err_t lua_http_init(void) {
    s_http_request = NULL;
    s_http_ctx = NULL;
    return ESP_OK;
}

void lua_http_deinit(void) {
    s_http_request = NULL;
    s_http_ctx = NULL;
}

esp_err_t lua_runtime_set_http_backend(lua_runtime_http_request_callback_t request, void* user_ctx) {
    if (!request)
        return ESP_ERR_INVALID_ARG;
    s_http_request = request;
    s_http_ctx = user_ctx;
    return ESP_OK;
}
