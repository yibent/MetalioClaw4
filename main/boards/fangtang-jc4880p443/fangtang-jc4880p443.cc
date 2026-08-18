#include "wifi_board.h"
#include "audio/codecs/es8311_audio_codec.h"
#include "application.h"
#include "display/lcd_display.h"
#include "button.h"
#include "board_hardware.h"
#include "config.h"
#include "led/single_led.h"

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"

#include "esp_lcd_st7701.h"
#include "esp_lcd_touch_gt911.h"

#include <wifi_manager.h>
#include <esp_log.h>
#include <driver/i2c_master.h>

#define TAG "fangtang-jc4880p443"

static i2c_master_bus_handle_t s_fangtang_i2c_bus = NULL;
static esp_lcd_panel_io_handle_t s_fangtang_panel_io = NULL;
static esp_lcd_panel_handle_t s_fangtang_panel = NULL;
static esp_lcd_touch_handle_t s_fangtang_touch = NULL;
static esp_lcd_touch_io_gt911_config_t s_fangtang_touch_config = {};

extern "C" i2c_master_bus_handle_t board_get_i2c_bus(void) {
    return s_fangtang_i2c_bus;
}

extern "C" esp_lcd_panel_io_handle_t board_get_panel_io(void) {
    return s_fangtang_panel_io;
}

extern "C" esp_lcd_panel_handle_t board_get_panel(void) {
    return s_fangtang_panel;
}

extern "C" esp_lcd_touch_handle_t board_get_touch(void) {
    return s_fangtang_touch;
}

extern "C" esp_err_t board_recover_lcd_after_camera(void) {
    // The JC4880P443 camera path does not share the ST7701 reset line.
    return ESP_OK;
}

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

static const st7701_lcd_init_cmd_t lcd_cmd[] = {
    {0xFF, (uint8_t []){0x77,0x01,0x00,0x00,0x13},5,0},
    {0xEF, (uint8_t []){0x08}, 1, 0},
    {0xFF, (uint8_t []){0x77,0x01,0x00,0x00,0x10},5,0},
    {0xC0, (uint8_t []){0x63, 0x00}, 2, 0},
    {0xC1, (uint8_t []){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t []){0x10, 0x08}, 2, 0},
    {0xCC, (uint8_t []){0x10}, 1, 0},

    {0xB0, (uint8_t []){0x80, 0x09, 0x53, 0x0C, 0xD0, 0x07, 0x0C, 0x09, 0x09, 0x28, 0x06, 0xD4, 0x13, 0x69, 0x2B, 0x71}, 16, 0},
    {0xB1, (uint8_t []){0x80, 0x94, 0x5A, 0x10, 0xD3, 0x06, 0x0A, 0x08, 0x08, 0x25, 0x03, 0xD3, 0x12, 0x66, 0x6A, 0x0D}, 16, 0},
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},

    {0xB0, (uint8_t []){0x5D}, 1, 0},
    {0xB1, (uint8_t []){0x58}, 1, 0},
    {0xB2, (uint8_t []){0x87}, 1, 0},
    {0xB3, (uint8_t []){0x80}, 1, 0},
    {0xB5, (uint8_t []){0x4E}, 1, 0},
    {0xB7, (uint8_t []){0x85}, 1, 0},
    {0xB8, (uint8_t []){0x21}, 1, 0},
    {0xB9, (uint8_t []){0x10, 0x1F}, 2, 0},
    {0xBB, (uint8_t []){0x03}, 1,0},
    {0xBC, (uint8_t []){0x00}, 1,0},
    
    {0xC1, (uint8_t []){0x78}, 1, 0},
    {0xC2, (uint8_t []){0x78}, 1, 0},
    {0xD0, (uint8_t []){0x88}, 1, 0},

    {0xE0, (uint8_t []){0x00, 0x3A, 0x02}, 3, 0},
    {0xE1, (uint8_t []){0x04, 0xA0, 0x00, 0xA0, 0x05,0xA0, 0x00, 0xA0, 0x00, 0x40, 0x40}, 11, 0},
    {0xE2, (uint8_t []){0x30, 0x00, 0x40, 0x40, 0x32, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00}, 13, 0},
    {0xE3, (uint8_t []){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE4, (uint8_t []){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t []){0x09, 0x2E, 0xA0, 0xA0, 0x0B, 0x30, 0xA0, 0xA0, 0x05, 0x2A, 0xA0, 0xA0, 0x07, 0x2C, 0xA0, 0xA0}, 16, 0},
    {0xE6, (uint8_t []){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE7, (uint8_t []){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t []){0x08, 0x2D, 0xA0, 0xA0, 0x0A, 0x2F, 0xA0, 0xA0, 0x04, 0x29, 0xA0, 0xA0, 0x06, 0x2B, 0xA0, 0xA0}, 16, 0},

    {0xEB, (uint8_t []){0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00}, 7, 0},
    {0xEC, (uint8_t []){0x08, 0x01}, 2, 0},

    {0xED, (uint8_t []){0xB0, 0x2B, 0x98, 0xA4, 0x56, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF7, 0x65, 0x4A, 0x89, 0xB2, 0x0B}, 16, 0},
    {0xEF, (uint8_t []){0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6, 0},
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},

    // {0x3A, (uint8_t []){0x66}, 1, 0},
    {0x11, (uint8_t []){0x00}, 1, 120},
    {0x29, (uint8_t []){0x00}, 1, 20},

};

class jc4880p443 : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    Button boot_button_;
    MipiLcdDisplay* display__;

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
        s_fangtang_i2c_bus = codec_i2c_bus_;
    }

    bool InitializeGT911() {
        ESP_LOGI(TAG, "Initialize GT911");
        const uint8_t addresses[] = {
            ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
            ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP,
        };

        for (uint8_t dev_addr : addresses) {
            esp_lcd_panel_io_i2c_config_t tp_io_config = {};
            tp_io_config.dev_addr = dev_addr;
            tp_io_config.control_phase_bytes = 1;
            tp_io_config.dc_bit_offset = 0;
            tp_io_config.lcd_cmd_bits = 16;
            tp_io_config.flags.disable_control_phase = 1;
            tp_io_config.scl_speed_hz = 100000;

            // GT911 latches its I2C address from INT while reset is asserted. The
            // driver needs this value through driver_data to perform that sequence.
            s_fangtang_touch_config.dev_addr = dev_addr;
            const esp_lcd_touch_config_t tp_cfg = {
                .x_max = LCD_H_RES,
                .y_max = LCD_V_RES,
                .rst_gpio_num = LCD_TOUCH_RST,
                .int_gpio_num = LCD_TOUCH_INT,
                .levels = {
                    .reset = 0,
                    .interrupt = 0,
                },
                .flags = {
                    .swap_xy = LCD_TOUCH_SWAP_XY,
                    .mirror_x = LCD_TOUCH_MIRROR_X,
                    .mirror_y = LCD_TOUCH_MIRROR_Y,
                },
                .driver_data = &s_fangtang_touch_config,
            };

            esp_lcd_panel_io_handle_t tp_io_handle = NULL;
            esp_err_t err = esp_lcd_new_panel_io_i2c(codec_i2c_bus_, &tp_io_config, &tp_io_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "GT911 I2C IO creation failed for 0x%02X: 0x%x", dev_addr, err);
                continue;
            }

            err = esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &s_fangtang_touch);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "GT911 initialized at I2C address 0x%02X", dev_addr);
                return true;
            }

            ESP_LOGW(TAG, "GT911 initialization failed at I2C address 0x%02X: 0x%x", dev_addr, err);
            esp_lcd_panel_io_del(tp_io_handle);
            s_fangtang_touch = NULL;
        }

        ESP_LOGE(TAG, "GT911 unavailable; continuing without touch input");
        return false;
    }

    static esp_err_t bsp_enable_dsi_phy_power(void) {
#if MIPI_DSI_PHY_PWR_LDO_CHAN > 0
        // Turn on the power for MIPI DSI PHY, so it can go from "No Power" state to "Shutdown" state
        static esp_ldo_channel_handle_t phy_pwr_chan = NULL;
        esp_ldo_channel_config_t ldo_cfg = {
            .chan_id = MIPI_DSI_PHY_PWR_LDO_CHAN,
            .voltage_mv = MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
        };
        ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &phy_pwr_chan));
        ESP_LOGI(TAG, "MIPI DSI PHY Powered on");
#endif

        return ESP_OK;
    }

    void InitializeLCD() {
        ESP_ERROR_CHECK(bsp_enable_dsi_phy_power());
        esp_lcd_panel_io_handle_t io = NULL;
        esp_lcd_panel_handle_t disp_panel = NULL;

        esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
        esp_lcd_dsi_bus_config_t bus_config = {
            .bus_id = 0,
            .num_data_lanes = LCD_MIPI_DSI_LANE_NUM,
            .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
            .lane_bit_rate_mbps = 500,
        };
        ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus));

        ESP_LOGI(TAG, "Install MIPI DSI LCD control panel");
        // we use DBI interface to send LCD commands and parameters
        esp_lcd_dbi_io_config_t dbi_config = {
            .virtual_channel = 0,
            .lcd_cmd_bits = 8,   // according to the LCD spec
            .lcd_param_bits = 8, // according to the LCD spec
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &io));
        s_fangtang_panel_io = io;

        // esp_lcd_dpi_panel_config_t dpi_config = JD9165_1024_600_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);

        esp_lcd_dpi_panel_config_t dpi_config ={
            .virtual_channel = 0, 
            .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,  
            .dpi_clock_freq_mhz = 34,                                             
            .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,                    
            .num_fbs = 2,                                 
            .video_timing = {                             
                .h_size = 480,                            
                .v_size = 800, 
                .hsync_pulse_width = 12,                            
                .hsync_back_porch = 42,                              
                .hsync_front_porch = 42,   
                .vsync_pulse_width = 2,                
                .vsync_back_porch = 8,                                          
                .vsync_front_porch = 166,                  
            },                                            
            .flags={
                .use_dma2d = true,
            }                      
        };

        st7701_vendor_config_t vendor_config = {
            .init_cmds = lcd_cmd,
            .init_cmds_size = sizeof(lcd_cmd) / sizeof(st7701_lcd_init_cmd_t),
            .mipi_config = {
                .dsi_bus = mipi_dsi_bus,
                .dpi_config = &dpi_config,
            },
            .flags = {
                .use_mipi_interface = 1,
            }
        };

        const esp_lcd_panel_dev_config_t lcd_dev_config = {
            .reset_gpio_num = PIN_NUM_LCD_RST,
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = 16,
            .vendor_config = &vendor_config,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7701(io, &lcd_dev_config, &disp_panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(disp_panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(disp_panel));
        s_fangtang_panel = disp_panel;

        display__ = new MipiLcdDisplay(io,disp_panel,LCD_H_RES,LCD_V_RES,
                 DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y,
                 DISPLAY_SWAP_XY, LV_COLOR_FORMAT_RGB565);
        return;
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiManager::GetInstance().IsConnected()) {
                EnterWifiConfigMode();
            }
        });

    }

    // 物联网初始化，添加对 AI 可见设备
    // void InitializeIot() {
    //     auto& thing_manager = iot::ThingManager::GetInstance();
    //     thing_manager.AddThing(iot::CreateThing("Speaker"));
    // }

public:
    jc4880p443() : boot_button_(BOOT_BUTTON_GPIO) {

        InitializeCodecI2c();
        // InitializeIot();
        InitializeLCD();
        if (InitializeGT911() && !display__->AddTouch(s_fangtang_touch)) {
            ESP_LOGE(TAG, "Failed to register GT911 with LVGL");
        }
        InitializeButtons();
        GetBacklight()->RestoreBrightness();
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(codec_i2c_bus_, I2C_NUM_1, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }
    
    virtual Display* GetDisplay() override {
        
        return display__;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(PIN_NUM_BK_LIGHT, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

};

DECLARE_BOARD(jc4880p443);
