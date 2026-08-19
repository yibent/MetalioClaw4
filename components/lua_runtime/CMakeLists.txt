idf_component_register(
    SRCS "src/lua_runtime.c" "src/lua_ui.c" "src/lua_uart.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_timer esp_lvgl_adapter lvgl georgik__lua json driver
)
