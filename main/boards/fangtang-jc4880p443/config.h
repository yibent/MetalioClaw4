#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_13
#define AUDIO_I2S_GPIO_WS GPIO_NUM_10
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_12
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_48
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_9

#define AUDIO_CODEC_PA_PIN       GPIO_NUM_11
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_7
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_8
#define I2C_SDA_PIN              AUDIO_CODEC_I2C_SDA_PIN
#define I2C_SCL_PIN              AUDIO_CODEC_I2C_SCL_PIN
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

#define BUILTIN_LED_GPIO        GPIO_NUM_26
#define BOOT_BUTTON_GPIO        GPIO_NUM_35
#define POWER_BUTTON_PIN        GPIO_NUM_NC 

#define MIPI_DPI_PX_FORMAT         (LCD_COLOR_PIXEL_FORMAT_RGB565)
#define DISPLAY_SWAP_XY false
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true
#define BACKLIGHT_INVERT false

#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0

#define DISPLAY_WIDTH 480
#define DISPLAY_HEIGHT 800
#define DISPLAY_H_RES DISPLAY_WIDTH
#define DISPLAY_V_RES DISPLAY_HEIGHT

#define LCD_H_RES                  (480)
#define LCD_V_RES                  (800)
#define LCD_BIT_PER_PIXEL          (16)
#define PIN_NUM_LCD_RST            GPIO_NUM_5
#define PIN_NUM_BK_LIGHT           GPIO_NUM_23   // set to -1 if not used
#define LCD_BK_LIGHT_ON_LEVEL      (1)
#define LCD_BK_LIGHT_OFF_LEVEL     (!LCD_BK_LIGHT_ON_LEVEL)

#define DELAY_TIME_MS                      (3000)
#define LCD_MIPI_DSI_LANE_NUM          (2)    // 2 data lanes
#define BSP_LCD_COLOR_SPACE         (ESP_LCD_COLOR_SPACE_RGB)

#define MIPI_DSI_PHY_PWR_LDO_CHAN          (3)
#define MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV    (2500)

#define LCD_TOUCH_RST       GPIO_NUM_22
#define LCD_TOUCH_INT       GPIO_NUM_21
// No vibration motor on this board. GPIO 22 is GT911 reset, not a motor pin.
#define HAPTIC_MOTOR_GPIO   GPIO_NUM_NC
#define LCD_TOUCH_SWAP_XY   false
// LVGL ROTATION_180 already remaps pointer coords; keep GT911 in panel space.
#define LCD_TOUCH_MIRROR_X  false
#define LCD_TOUCH_MIRROR_Y  false

// SDMMC (SD card) pins for ESP32-P4. Uses SDMMC slot 0 with 4-bit bus.
// Verify these GPIO assignments against the actual Fangtang schematic.
#define SDMMC_CLK_PIN  GPIO_NUM_43
#define SDMMC_CMD_PIN  GPIO_NUM_44
#define SDMMC_D0_PIN   GPIO_NUM_39
#define SDMMC_D1_PIN   GPIO_NUM_40
#define SDMMC_D2_PIN   GPIO_NUM_41
#define SDMMC_D3_PIN   GPIO_NUM_42
#define SDMMC_LDO_CHAN_ID     4    // On-chip LDO channel for SDMMC PHY power

// USB OTG FS PHY0（与 USB Serial/JTAG 共用）：启用虚拟 U 盘时切到 OTG MSC
#define USB_OTG_DM_PIN  GPIO_NUM_24
#define USB_OTG_DP_PIN  GPIO_NUM_25

// Battery voltage sense: GPIO53 = ESP32-P4 ADC2_CH4, 68k upper / 100k lower.
// Charge-status pin is not wired to P4.
#define BATTERY_ADC_GPIO           GPIO_NUM_53
#define BATTERY_ADC_UNIT           ADC_UNIT_2
#define BATTERY_ADC_CHANNEL        ADC_CHANNEL_4
#define BATTERY_UPPER_RESISTOR     68000.0f
#define BATTERY_LOWER_RESISTOR     100000.0f

#endif // _BOARD_CONFIG_H_
