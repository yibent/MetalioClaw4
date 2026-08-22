idf_component_register(
    SRCS "src/lua_runtime.c" "src/lua_ui.c" "src/lua_uart.c" "src/lua_http.c" "src/lua_camera.c" "src/lua_board.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_timer lvgl georgik__lua json driver
)
