#include "lua_runtime.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lauxlib.h"
#include "lua_runtime_internal.h"

#define UART_HANDLE_METATABLE "metalio.lua.uart.handle"
#define LUA_UART_RX_BUFFER_MIN 256
#define LUA_UART_RX_BUFFER_MAX 16384
#define LUA_UART_MAX_WRITE 16384

typedef struct {
    uart_port_t port;
    bool open;
} lua_uart_handle_t;

static SemaphoreHandle_t s_uart_lock;
static bool s_uart_owned[UART_NUM_MAX];
static bool s_uart_allowed[UART_NUM_MAX];
static lua_runtime_uart_port_config_t s_uart_config[UART_NUM_MAX];

static bool uart_lock(void) {
    return s_uart_lock && xSemaphoreTake(s_uart_lock, portMAX_DELAY) == pdTRUE;
}

static void uart_unlock(void) { xSemaphoreGive(s_uart_lock); }

static lua_uart_handle_t* check_handle(lua_State* state, int index) {
    lua_uart_handle_t* handle = luaL_checkudata(state, index, UART_HANDLE_METATABLE);
    if (!handle->open)
        luaL_error(state, "UART handle is closed");
    return handle;
}

static void close_handle(lua_uart_handle_t* handle) {
    if (!handle || !handle->open)
        return;
    if (uart_lock()) {
        uart_port_t port = handle->port;
        if (port >= 0 && port < UART_NUM_MAX && s_uart_owned[port]) {
            uart_flush_input(port);
            uart_driver_delete(port);
            s_uart_owned[port] = false;
        }
        uart_unlock();
    }
    handle->open = false;
}

static int l_uart_gc(lua_State* state) {
    close_handle((lua_uart_handle_t*)luaL_checkudata(state, 1, UART_HANDLE_METATABLE));
    return 0;
}

static int l_uart_close(lua_State* state) {
    lua_uart_handle_t* handle = luaL_checkudata(state, 1, UART_HANDLE_METATABLE);
    close_handle(handle);
    return 0;
}

static int l_uart_write(lua_State* state) {
    lua_uart_handle_t* handle = check_handle(state, 1);
    size_t length = 0;
    const char* data = luaL_checklstring(state, 2, &length);
    if (length > LUA_UART_MAX_WRITE)
        return luaL_argerror(state, 2, "write is limited to 16384 bytes");
    size_t written = 0;
    while (written < length) {
        int count = uart_tx_chars(handle->port, data + written, length - written);
        if (count < 0)
            return luaL_error(state, "UART write failed");
        written += (size_t)count;
        if (written < length) {
            vTaskDelay(pdMS_TO_TICKS(1));
            lua_runtime_check_abort(state);
        }
    }
    lua_pushinteger(state, (lua_Integer)written);
    return 1;
}

static int l_uart_read(lua_State* state) {
    lua_uart_handle_t* handle = check_handle(state, 1);
    lua_Integer max_length = luaL_checkinteger(state, 2);
    lua_Integer timeout = luaL_optinteger(state, 3, 0);
    if (max_length <= 0 || max_length > LUA_UART_MAX_WRITE)
        return luaL_argerror(state, 2, "length must be between 1 and 16384");
    if (timeout < 0 || timeout > UINT32_MAX)
        return luaL_argerror(state, 3, "timeout must be between 0 and UINT32_MAX");
    char* buffer = lua_newuserdatauv(state, (size_t)max_length, 0);
    uint32_t remaining = (uint32_t)timeout;
    int received = 0;
    while (received == 0) {
        uint32_t slice = remaining > 50 ? 50 : remaining;
        received =
            uart_read_bytes(handle->port, buffer, (uint32_t)max_length, pdMS_TO_TICKS(slice));
        if (received < 0)
            return luaL_error(state, "UART read failed");
        if (received > 0 || remaining == 0)
            break;
        remaining -= slice;
        lua_runtime_check_abort(state);
    }
    lua_pushlstring(state, buffer, received > 0 ? (size_t)received : 0);
    lua_remove(state, -2);
    return 1;
}

static int l_uart_available(lua_State* state) {
    lua_uart_handle_t* handle = check_handle(state, 1);
    size_t available = 0;
    esp_err_t err = uart_get_buffered_data_len(handle->port, &available);
    if (err != ESP_OK)
        return luaL_error(state, "UART status failed: %s", esp_err_to_name(err));
    lua_pushinteger(state, (lua_Integer)available);
    return 1;
}

static int l_uart_flush(lua_State* state) {
    lua_uart_handle_t* handle = check_handle(state, 1);
    esp_err_t err = uart_flush_input(handle->port);
    if (err != ESP_OK)
        return luaL_error(state, "UART flush failed: %s", esp_err_to_name(err));
    return 0;
}

static int l_uart_poll_event(lua_State* state) {
    lua_uart_handle_t* handle = check_handle(state, 1);
    lua_Integer timeout = luaL_optinteger(state, 2, 0);
    if (timeout < 0 || timeout > UINT32_MAX)
        return luaL_argerror(state, 2, "timeout must be between 0 and UINT32_MAX");
    uint32_t remaining = (uint32_t)timeout;
    size_t available = 0;
    while (true) {
        esp_err_t err = uart_get_buffered_data_len(handle->port, &available);
        if (err != ESP_OK)
            return luaL_error(state, "UART status failed: %s", esp_err_to_name(err));
        if (available > 0) {
            lua_createtable(state, 0, 2);
            lua_pushstring(state, "data");
            lua_setfield(state, -2, "type");
            lua_pushinteger(state, (lua_Integer)available);
            lua_setfield(state, -2, "available");
            return 1;
        }
        if (remaining == 0) {
            lua_pushnil(state);
            return 1;
        }
        uint32_t slice = remaining > 50 ? 50 : remaining;
        vTaskDelay(pdMS_TO_TICKS(slice));
        remaining -= slice;
        lua_runtime_check_abort(state);
    }
}

static int l_uart_open(lua_State* state) {
    lua_runtime_exec_context_t* context = lua_runtime_get_context(state);
    if (!context || !(context->capabilities & LUA_RUNTIME_CAP_UART))
        return luaL_error(state, "UART capability is not granted to this Lua job");
    luaL_checktype(state, 1, LUA_TTABLE);
    lua_getfield(state, 1, "port");
    int port = (int)luaL_checkinteger(state, -1);
    lua_pop(state, 1);
    int baud = 115200;
    int rx_buffer = 2048;
    int data_bits = 8;
    int stop_bits = 1;
    uart_parity_t parity = UART_PARITY_DISABLE;
    lua_getfield(state, 1, "baud_rate");
    if (lua_isinteger(state, -1))
        baud = (int)lua_tointeger(state, -1);
    lua_pop(state, 1);
    lua_getfield(state, 1, "rx_buffer");
    if (lua_isinteger(state, -1))
        rx_buffer = (int)lua_tointeger(state, -1);
    lua_pop(state, 1);
    lua_getfield(state, 1, "data_bits");
    if (lua_isinteger(state, -1))
        data_bits = (int)lua_tointeger(state, -1);
    lua_pop(state, 1);
    lua_getfield(state, 1, "stop_bits");
    if (lua_isinteger(state, -1))
        stop_bits = (int)lua_tointeger(state, -1);
    lua_pop(state, 1);
    lua_getfield(state, 1, "parity");
    if (!lua_isnil(state, -1)) {
        const char* value = luaL_checkstring(state, -1);
        if (strcmp(value, "none") == 0)
            parity = UART_PARITY_DISABLE;
        else if (strcmp(value, "even") == 0)
            parity = UART_PARITY_EVEN;
        else if (strcmp(value, "odd") == 0)
            parity = UART_PARITY_ODD;
        else {
            lua_pop(state, 1);
            return luaL_argerror(state, 1, "parity must be none, even, or odd");
        }
    }
    lua_pop(state, 1);
    if (port < 0 || port >= UART_NUM_MAX)
        return luaL_argerror(state, 1, "invalid UART port");
    if (baud < 300 || baud > 4000000)
        return luaL_argerror(state, 1, "invalid baud_rate");
    if (rx_buffer < LUA_UART_RX_BUFFER_MIN || rx_buffer > LUA_UART_RX_BUFFER_MAX)
        return luaL_argerror(state, 1, "rx_buffer must be between 256 and 16384");
    if (data_bits < 5 || data_bits > 8)
        return luaL_argerror(state, 1, "data_bits must be between 5 and 8");
    if (stop_bits != 1 && stop_bits != 2)
        return luaL_argerror(state, 1, "stop_bits must be 1 or 2");
    lua_uart_handle_t* handle = lua_newuserdatauv(state, sizeof(*handle), 0);
    handle->port = port;
    handle->open = false;
    luaL_getmetatable(state, UART_HANDLE_METATABLE);
    lua_setmetatable(state, -2);
    if (uart_lock()) {
        if (!s_uart_allowed[port]) {
            uart_unlock();
            return luaL_error(state, "UART port is not allowed by board policy");
        }
        if (baud > (int)s_uart_config[port].max_baud_rate) {
            uart_unlock();
            return luaL_argerror(state, 1, "baud_rate exceeds board policy");
        }
        if (s_uart_owned[port]) {
            uart_unlock();
            return luaL_error(state, "UART port is already in use");
        }
        if (uart_is_driver_installed(port)) {
            uart_unlock();
            return luaL_error(state, "UART port is already used by the system");
        }
        uart_config_t config = {
            .baud_rate = baud,
            .data_bits = (uart_word_length_t)(data_bits - 5),
            .parity = parity,
            .stop_bits = stop_bits == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        esp_err_t err = uart_param_config(port, &config);
        if (err == ESP_OK)
            err = uart_set_pin(port, s_uart_config[port].tx_pin, s_uart_config[port].rx_pin,
                               UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (err == ESP_OK)
            err = uart_driver_install(port, rx_buffer, 0, 0, NULL, 0);
        if (err != ESP_OK) {
            uart_unlock();
            return luaL_error(state, "UART open failed: %s", esp_err_to_name(err));
        }
        s_uart_owned[port] = true;
        handle->open = true;
        uart_unlock();
    } else
        return luaL_error(state, "UART lock is unavailable");
    return 1;
}

int luaopen_uart(lua_State* state) {
    if (luaL_newmetatable(state, UART_HANDLE_METATABLE)) {
        lua_pushcfunction(state, l_uart_gc);
        lua_setfield(state, -2, "__gc");
        lua_newtable(state);
        static const luaL_Reg methods[] = {
            {"write", l_uart_write},
            {"read", l_uart_read},
            {"available", l_uart_available},
            {"poll_event", l_uart_poll_event},
            {"flush", l_uart_flush},
            {"close", l_uart_close},
            {NULL, NULL},
        };
        luaL_setfuncs(state, methods, 0);
        lua_setfield(state, -2, "__index");
    }
    lua_pop(state, 1);
    static const luaL_Reg functions[] = {{"open", l_uart_open}, {NULL, NULL}};
    luaL_newlib(state, functions);
    return 1;
}

esp_err_t lua_uart_init(void) {
    if (!s_uart_lock)
        s_uart_lock = xSemaphoreCreateMutex();
    return s_uart_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t lua_runtime_register_uart_port(const lua_runtime_uart_port_config_t* config) {
    if (!config || config->port < 0 || config->port >= UART_NUM_MAX ||
        !GPIO_IS_VALID_OUTPUT_GPIO(config->tx_pin) || !GPIO_IS_VALID_GPIO(config->rx_pin) ||
        config->tx_pin == config->rx_pin || config->max_baud_rate < 300 ||
        config->max_baud_rate > 4000000)
        return ESP_ERR_INVALID_ARG;
    if (!s_uart_lock)
        return ESP_ERR_INVALID_STATE;
    if (!uart_lock())
        return ESP_ERR_INVALID_STATE;
    if (s_uart_owned[config->port]) {
        uart_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_uart_config[config->port] = *config;
    s_uart_allowed[config->port] = true;
    uart_unlock();
    return ESP_OK;
}

void lua_uart_deinit(void) {
    if (!s_uart_lock)
        return;
    for (int port = 0; port < UART_NUM_MAX; ++port) {
        if (s_uart_owned[port]) {
            uart_driver_delete(port);
            s_uart_owned[port] = false;
        }
        s_uart_allowed[port] = false;
    }
    vSemaphoreDelete(s_uart_lock);
    s_uart_lock = NULL;
}
