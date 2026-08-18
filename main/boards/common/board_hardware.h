#ifndef BOARD_HARDWARE_H
#define BOARD_HARDWARE_H

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

// Board-owned handles shared by optional screens and USB extension support.
i2c_master_bus_handle_t board_get_i2c_bus(void);
esp_lcd_panel_io_handle_t board_get_panel_io(void);
esp_lcd_panel_handle_t board_get_panel(void);
esp_lcd_touch_handle_t board_get_touch(void);

// Recover the LCD after camera initialization when both devices share reset.
// Boards without that wiring should return ESP_OK.
esp_err_t board_recover_lcd_after_camera(void);

#ifdef __cplusplus
}
#endif

#endif  // BOARD_HARDWARE_H
