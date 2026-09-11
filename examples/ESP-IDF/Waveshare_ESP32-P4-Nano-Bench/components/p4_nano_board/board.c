// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0
//
// Derived from Waveshare's `waveshare/esp32_p4_nano` BSP, which is Apache-2.0.
// Every pin number, LDO channel, panel timing and touch flag below is theirs;
// what is different is the SIZE.
//
// Why this exists instead of the BSP
// ----------------------------------
// The BSP is fine, but its idf_component.yml declares esp_lvgl_port, lvgl,
// esp_codec_dev and four panel drivers, and the component manager fetches all
// of them whether or not anything calls into them - LVGL is about 180 MB of
// source on its own. It cannot be avoided while depending on the BSP: its
// CMakeLists does not even REQUIRE lvgl, but bsp_display_start() is compiled
// unconditionally and does, so the dependency is real at build time even
// though this example never starts LVGL and plays no audio.
//
// This example uses six things from that BSP - two resolutions, the display
// bring-up, the backlight, the touch controller and the SD mount - so those
// six are here instead, and the tree drops nine managed components.
//
// This file is C on purpose. Vendor esp_lcd config macros use C designated
// initialisers in an order that C++ rejects ("designator order does not match
// declaration order"), which has cost this tree time more than once.
//
// The DPI config is filled field by field anyway, and being in C is not why.
// See the comment on dpi_cfg below: a vendor macro names the IDF struct's
// fields, so it pins the component to the IDF versions that still have them,
// and JD9365_800_1280_PANEL_60HZ_DPI_CONFIG names two that IDF 6.0 removed.
// Filling the struct here is what lets one board component serve both.

#include "board.h"

#include "driver/i2c_master.h"
#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_idf_version.h"
#include "esp_lcd_jd9365.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

static const char *TAG = "board";

// --- the board -------------------------------------------------------------
#define I2C_PORT            1
#define I2C_SDA             7
#define I2C_SCL             8

// The backlight is a register on an I2C device, not a PWM pin: there is no
// backlight GPIO on this board at all.
#define BACKLIGHT_ADDR      0x45
#define BACKLIGHT_REG       0x86

#define DSI_LANES           2
#define DSI_LANE_MBPS       1500
#define DSI_PHY_LDO_CHAN    3        // LDO_VO3 feeds VDD_MIPI_DPHY
#define DSI_PHY_LDO_MV      2500

#define SD_LDO_CHAN         4

static i2c_master_bus_handle_t s_i2c = NULL;
static sdmmc_card_t           *s_card = NULL;

static esp_err_t board_i2c_init(void)
{
    if (s_i2c != NULL) return ESP_OK;
    const i2c_master_bus_config_t cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .i2c_port   = I2C_PORT,
    };
    const esp_err_t ret = i2c_new_master_bus(&cfg, &s_i2c);
    if (ret != ESP_OK)
        ESP_LOGE(TAG, "i2c_new_master_bus(port %d, sda %d, scl %d) -> %s",
                 I2C_PORT, I2C_SDA, I2C_SCL, esp_err_to_name(ret));
    return ret;
}

esp_err_t board_backlight_set(int percent)
{
    if (percent > 100) percent = 100;
    if (percent < 0)   percent = 0;
    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "i2c");

    const uint8_t payload[2] = { BACKLIGHT_REG, (uint8_t)(255 * percent / 100) };
    const i2c_device_config_t dev_cfg = {
        .scl_speed_hz   = 100 * 1000,
        .device_address = BACKLIGHT_ADDR,
    };
    i2c_master_dev_handle_t dev = NULL;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &dev_cfg, &dev), TAG, "add");
    const esp_err_t ret = i2c_master_transmit(dev, payload, sizeof(payload), 50);
    i2c_master_bus_rm_device(dev);
    return ret;
}

esp_err_t board_display_init(esp_lcd_panel_handle_t *out_panel)
{
    ESP_RETURN_ON_FALSE(out_panel != NULL, ESP_ERR_INVALID_ARG, TAG, "null out_panel");
    *out_panel = NULL;

    // BEFORE the panel, and that ORDER IS THE WHOLE POINT. The JD9365 driver
    // depends on espressif/i2c_bus and brings port 1 up itself if it finds it
    // uninitialised - after which this bus can never be created and both the
    // backlight and the touch controller are lost, with the panel working
    // perfectly so nothing looks wrong. Created first, i2c_bus attaches to it
    // instead and says so: "I2C Bus V2 uses the externally initialized bus
    // handle". The vendor BSP did this first too, in bsp_display_brightness_init().
    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "i2c");

    // The DSI PHY has no power until this LDO is up; without it the bus below
    // fails rather than the panel merely staying dark.
    static esp_ldo_channel_handle_t phy_pwr = NULL;
    if (phy_pwr == NULL) {
        const esp_ldo_channel_config_t ldo_cfg = {
            .chan_id    = DSI_PHY_LDO_CHAN,
            .voltage_mv = DSI_PHY_LDO_MV,
        };
        ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &phy_pwr), TAG, "dphy ldo");
    }

    esp_lcd_dsi_bus_handle_t bus = NULL;
    const esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id             = 0,
        .num_data_lanes     = DSI_LANES,
        .phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = DSI_LANE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &bus), TAG, "dsi bus");

    // Commands and parameters go over DBI; pixels go over DPI below.
    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(bus, &dbi_cfg, &io), TAG, "dbi io");

    // The values are JD9365_800_1280_PANEL_60HZ_DPI_CONFIG's, written out.
    //
    // WHY NOT THE MACRO. It sets `.pixel_format` and `.flags.use_dma2d`, and
    // ESP-IDF 6.0 removed both: `pixel_format` was superseded by the
    // in/out pair in 5.3 and deleted in 6.0, and DMA2D moved from a config
    // flag to an explicit esp_lcd_dpi_panel_enable_dma2d() call. Expanding the
    // macro therefore fails to COMPILE on 6.0 with
    //   'esp_lcd_dpi_panel_config_t' has no member named 'pixel_format'
    // even though the component itself is fine: the removed names appear only
    // inside macros in its header, never in its sources. So this fills the
    // struct rather than forking the vendor component.
    //
    // On 5.5.4 this is not merely equivalent, it is IDENTICAL. That version
    // still carries `pixel_format`, but esp_lcd_panel_dpi.c computes
    // `in_color_format = COLOR_TYPE_ID(COLOR_SPACE_RGB, pixel_format)` and
    // then lets a non-zero `in_color_format` override it - and
    // LCD_COLOR_PIXEL_FORMAT_RGB565 is COLOR_PIXEL_RGB565, so both routes
    // arrive at LCD_COLOR_FMT_RGB565 (33,554,434). `out_color_format` defaults
    // to `in_color_format` there, so naming it changes nothing either.
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 80,
        .virtual_channel    = 0,
        .in_color_format    = LCD_COLOR_FMT_RGB565,
        .out_color_format   = LCD_COLOR_FMT_RGB565,
        .num_fbs            = 1,
        .video_timing = {
            .h_size            = BOARD_LCD_H_RES,
            .v_size            = BOARD_LCD_V_RES,
            .hsync_back_porch  = 20,
            .hsync_pulse_width = 20,
            .hsync_front_porch = 40,
            .vsync_back_porch  = 10,
            .vsync_pulse_width = 4,
            .vsync_front_porch = 30,
        },
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
        // 6.0 has no such flag; see the enable call after panel init below.
        .flags.use_dma2d = true,
#endif
    };

    jd9365_vendor_config_t vendor_cfg = {
        .mipi_config = {
            .dsi_bus    = bus,
            .dpi_config = &dpi_cfg,
            .lane_num   = DSI_LANES,
        },
    };
    const esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = -1,          // no reset line on this board
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config  = &vendor_cfg,
    };

    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9365(io, &dev_cfg, &panel), TAG, "jd9365");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "init");

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    // What `flags.use_dma2d` did before 6.0, and it is not cosmetic: without
    // it esp_lcd_panel_draw_bitmap() copies the tile into the panel's frame
    // buffer with a CPU memcpy. This example sends 32 tiles of 64,000 bytes a
    // frame, so that is 2 MB of memcpy per frame on the render core.
    //
    // It does NOT change whether on_color_trans_done fires - 6.0 invokes it on
    // the CPU-copy path too - so a board that refuses this still runs rather
    // than hanging in the wait. Which is why the error is logged, not fatal.
    if (esp_lcd_dpi_panel_enable_dma2d(panel) != ESP_OK)
        ESP_LOGW(TAG, "DMA2D unavailable; tiles will be copied by the CPU");
#endif

    ESP_LOGI(TAG, "JD9365 %dx%d, %d lanes at %d Mbps",
             BOARD_LCD_H_RES, BOARD_LCD_V_RES, DSI_LANES, DSI_LANE_MBPS);
    *out_panel = panel;
    return ESP_OK;
}

esp_err_t board_touch_init(esp_lcd_touch_handle_t *out_touch)
{
    ESP_RETURN_ON_FALSE(out_touch != NULL, ESP_ERR_INVALID_ARG, TAG, "null out_touch");
    *out_touch = NULL;
    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "i2c");

    // Filled field by field rather than through ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG():
    // the vendor macros order their designators the way C allows and C++ does
    // not, and this header is reachable from C++ translation units.
    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr            = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
        // Required by the bus-HANDLE form of esp_lcd_new_panel_io_i2c; the
        // vendor BSP passed a port number instead, which selects the v1
        // overload where this field is ignored, so its config omits it.
        .scl_speed_hz        = 400 * 1000,
        .control_phase_bytes = 1,
        .dc_bit_offset       = 0,
        .lcd_cmd_bits        = 16,
        .flags = { .disable_control_phase = 1 },
    };
    esp_lcd_panel_io_handle_t io = NULL;
    const esp_err_t ioret = esp_lcd_new_panel_io_i2c(s_i2c, &io_cfg, &io);
    if (ioret != ESP_OK)
        {
        ESP_LOGE(TAG, "touch panel io -> %s", esp_err_to_name(ioret));
        return ioret;
        }

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max        = BOARD_LCD_H_RES,
        .y_max        = BOARD_LCD_V_RES,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags  = { .swap_xy = 0, .mirror_x = 1, .mirror_y = 1 },
    };
    return esp_lcd_touch_new_i2c_gt911(io, &tp_cfg, out_touch);
}

esp_err_t board_sdcard_mount(void)
{
    const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 64 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    const sd_pwr_ctrl_ldo_config_t ldo_cfg = { .ldo_chan_id = SD_LDO_CHAN };
    sd_pwr_ctrl_handle_t pwr = NULL;
    ESP_RETURN_ON_ERROR(sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &pwr), TAG, "sd ldo");
    host.pwr_ctrl_handle = pwr;

    // Slot 0 is on the IO MUX, so the pins are implied rather than listed.
    const sdmmc_slot_config_t slot_cfg = {
        .cd    = SDMMC_SLOT_NO_CD,
        .wp    = SDMMC_SLOT_NO_WP,
        .width = 4,
        .flags = 0,
    };
    return esp_vfs_fat_sdmmc_mount(BOARD_SD_MOUNT, &host, &slot_cfg, &mount_cfg, &s_card);
}
