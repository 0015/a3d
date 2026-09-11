// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0
//
// The pin assignments, the CO5300 initialisation command table and the
// 6-pixel column offset in this file are taken from Waveshare's
// `esp32_s3_touch_amoled_1_75c` board support package, which is distributed
// under the Apache License 2.0. They are facts about the board, and getting
// them from the vendor is the only way to have them right.

#include "amoled175.h"

#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_cst9217.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace a3d_amoled
{
namespace
{

const char* TAG = "a3d_amoled";

// --- board wiring, from the vendor BSP -------------------------------------
constexpr spi_host_device_t SPI_HOST_ID = SPI2_HOST;
constexpr int PIN_PCLK  = 38;
constexpr int PIN_D0    = 4;
constexpr int PIN_D1    = 5;
constexpr int PIN_D2    = 6;
constexpr int PIN_D3    = 7;
constexpr int PIN_CS    = 12;
constexpr int PIN_RST   = 1;

// ESP-IDF 6.0 changed `cs_gpio_num`, `dc_gpio_num` and `reset_gpio_num` from
// `int` to `gpio_num_t`. C accepts int -> enum; C++ does not, so every
// assignment needs the cast. The cast is correct on 5.x too (enum -> int is a
// standard conversion), which is why this file builds on both.
//
// The QSPI pixel clock, named once: begin() configures the bus with it and
// pixelClockMHz() reports it.
constexpr unsigned PCLK_HZ = 40u * 1000u * 1000u;

constexpr int PIN_I2C_SCL = 14;
constexpr int PIN_I2C_SDA = 15;
constexpr int PIN_TP_RST  = 2;
constexpr int PIN_TP_INT  = 11;

// The visible area starts at column 6 of the controller's memory. The init
// table sets the window and set_gap() makes every later draw_bitmap agree with
// it; drop either and the picture is six pixels off and wraps.
constexpr int X_GAP = 0x06;

// The largest tile this panel will accept in one draw_bitmap, in ROWS, so the
// relationship to the demo's TILE_H is visible. 48 is comfortably past the 40
// the demo uses.
constexpr int MAX_TILE_ROWS = 48;
constexpr int MAX_TRANSFER  = LCD_WIDTH * MAX_TILE_ROWS * 2;

// fill() streams this many rows at a time out of one DMA scratch band.
constexpr int BAND_ROWS = 8;

const co5300_lcd_init_cmd_t kInitCmds[] = {
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},

    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},   // 16 bits per pixel
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},   // brightness, overwritten below
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},    // columns 6..471
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 600},  // rows 0..465
    {0x11, NULL, 0, 600},              // sleep out
    {0x29, NULL, 0, 0},                // display on
};

esp_lcd_touch_handle_t g_tp = nullptr;
i2c_master_bus_handle_t g_i2c = nullptr;

bool IRAM_ATTR onColorDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* ctx)
    {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &woken);
    return woken == pdTRUE;
    }

} // namespace

esp_err_t Panel::begin()
    {
    if (_started) return ESP_OK;

    _done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(_done != nullptr, ESP_ERR_NO_MEM, TAG, "done sem");

    ESP_LOGI(TAG, "QSPI bus");
    // CO5300_PANEL_BUS_QSPI_CONFIG() is written with C designated initialisers
    // in an order C++ rejects: data0_io_num is a union alias for mosi_io_num,
    // which is declared before sclk_io_num. Same struct, same values, filled in
    // field by field so it compiles.
    spi_bus_config_t bus = {};
    bus.sclk_io_num  = PIN_PCLK;
    bus.data0_io_num = PIN_D0;
    bus.data1_io_num = PIN_D1;
    bus.data2_io_num = PIN_D2;
    bus.data3_io_num = PIN_D3;
    bus.data4_io_num = -1;
    bus.data5_io_num = -1;
    bus.data6_io_num = -1;
    bus.data7_io_num = -1;
    bus.max_transfer_sz = MAX_TRANSFER;
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI_HOST_ID, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    // Field by field rather than CO5300_PANEL_IO_QSPI_CONFIG(), and NOT because
    // the vendor is wrong. That macro is C designated initialisers: on ESP-IDF
    // 6.0 it assigns `-1` to a `dc_gpio_num` that is now `gpio_num_t`, and it
    // leaves later members unset, which -Werror=missing-field-initializers
    // rejects. Both are C++-only failures inside a vendor header this tree must
    // not fork. The same values, written out, compile on 5.x and 6.x alike.
    //
    // The completion callback goes in here rather than being registered
    // afterwards: without it, draw_bitmap returns while the DMA is still
    // reading the tile buffer.
    esp_lcd_panel_io_spi_config_t io = {};
    io.cs_gpio_num         = (gpio_num_t)PIN_CS;
    io.dc_gpio_num         = GPIO_NUM_NC;
    io.spi_mode            = 0;
    io.pclk_hz             = PCLK_HZ;
    io.on_color_trans_done = onColorDone;
    io.user_ctx            = _done;
    io.lcd_cmd_bits        = 32;
    io.lcd_param_bits      = 8;
    io.flags.quad_mode     = true;
    io.trans_queue_depth   = 10;
    esp_lcd_panel_io_handle_t ioh = nullptr;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI_HOST_ID, &io, &ioh),
        TAG, "panel io");
    _io = ioh;

    co5300_vendor_config_t vendor = {};
    vendor.init_cmds = kInitCmds;
    vendor.init_cmds_size = sizeof(kInitCmds) / sizeof(kInitCmds[0]);
    vendor.flags.use_qspi_interface = 1;

    esp_lcd_panel_dev_config_t dev = {};
    dev.reset_gpio_num = (gpio_num_t)PIN_RST;
    dev.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB;
    dev.bits_per_pixel = 16;
    dev.vendor_config  = &vendor;

    esp_lcd_panel_handle_t panel = nullptr;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(ioh, &dev, &panel), TAG, "co5300");
    _panel = panel;

    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(panel, X_GAP, 0), TAG, "gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "disp on");

    _band = (uint16_t*)heap_caps_malloc((size_t)LCD_WIDTH * BAND_ROWS * sizeof(uint16_t),
                                        MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(_band != nullptr, ESP_ERR_NO_MEM, TAG, "fill band");

    _started = true;
    // Dark before the first frame: the panel still holds whatever was in its
    // memory at the last boot, and on an AMOLED that is genuinely visible.
    setBacklight(0);
    fill(0x0000);
    return ESP_OK;
    }

void Panel::setBacklight(int percent)
    {
    if (!_started && _io == nullptr) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    // There is no backlight pin on an AMOLED. This is the controller's own
    // display-brightness register, 0x51, wrapped in the QSPI command framing
    // the CO5300 expects.
    uint32_t cmd = 0x51;
    cmd &= 0xFF;
    cmd <<= 8;
    cmd |= 0x02 << 24;
    uint8_t value = (uint8_t)((percent * 255) / 100);
    esp_lcd_panel_io_tx_param((esp_lcd_panel_io_handle_t)_io, cmd, &value, 1);
    }

void Panel::fill(uint16_t colour)
    {
    if (!_started || _band == nullptr) return;
    // The panel reads big-endian and the demo swaps every tile on its way out,
    // so a cleared screen has to be swapped too - otherwise the background
    // colour changes the moment the first real frame lands on top of it.
    const uint16_t swapped = (uint16_t)((colour >> 8) | (colour << 8));
    for (int i = 0; i < LCD_WIDTH * BAND_ROWS; i++) _band[i] = swapped;
    for (int y = 0; y < LCD_HEIGHT; y += BAND_ROWS)
        {
        int rows = BAND_ROWS;
        if (y + rows > LCD_HEIGHT) rows = LCD_HEIGHT - y;
        sendTile(_band, 0, y, LCD_WIDTH, rows);
        }
    }

void Panel::sendTile(const uint16_t* src, int x, int y, int w, int h)
    {
    if (!_started || src == nullptr || w <= 0 || h <= 0) return;

    // One transfer is in flight at a time, so the completion this reports is
    // unambiguously ours. Drain first rather than trust the count: a stale
    // token would let the next tile overwrite a buffer still being read.
    while (xSemaphoreTake((SemaphoreHandle_t)_done, 0) == pdTRUE) { }

    const esp_err_t err = esp_lcd_panel_draw_bitmap(
        (esp_lcd_panel_handle_t)_panel, x, y, x + w, y + h, src);
    if (err != ESP_OK)
        {
        ESP_LOGE(TAG, "draw_bitmap: %s", esp_err_to_name(err));
        return;
        }
    // 466x40 at 40 MHz on four lines is about 1.9 ms; a second is far past any
    // explanation but a stopped bus.
    if (xSemaphoreTake((SemaphoreHandle_t)_done, pdMS_TO_TICKS(1000)) != pdTRUE)
        ESP_LOGE(TAG, "tile at y=%d never completed", y);
    }

int Panel::pixelClockMHz() const
    {
    // One constant, used by begin() as well, so this cannot drift from what the
    // bus was actually configured with.
    return (int)(PCLK_HZ / 1000000);
    }

bool touchBegin()
    {
    i2c_master_bus_config_t bus = {};
    bus.i2c_port = I2C_NUM_0;
    bus.sda_io_num = (gpio_num_t)PIN_I2C_SDA;
    bus.scl_io_num = (gpio_num_t)PIN_I2C_SCL;
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&bus, &g_i2c);
    if (err != ESP_OK) { ESP_LOGE(TAG, "i2c bus: %s", esp_err_to_name(err)); return false; }

    // Field by field rather than ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG(). The
    // vendor macro is a C designated-initialiser list that stops short of the
    // struct's later members, and ESP-IDF 6.0 compiles C++ with
    // -Werror=missing-field-initializers, which rejects that outright inside a
    // header this tree must not fork. Same values, written out; `= {}` zeroes
    // the rest, which is what the partial list meant.
    esp_lcd_panel_io_i2c_config_t ioCfg = {};
    ioCfg.dev_addr                     = ESP_LCD_TOUCH_IO_I2C_CST9217_ADDRESS;
    ioCfg.control_phase_bytes          = 1;
    ioCfg.dc_bit_offset                = 0;
    ioCfg.lcd_cmd_bits                 = 8;
    ioCfg.flags.disable_control_phase  = 1;
    ioCfg.scl_speed_hz                 = 400000;
    esp_lcd_panel_io_handle_t io = nullptr;
    err = esp_lcd_new_panel_io_i2c(g_i2c, &ioCfg, &io);
    if (err != ESP_OK) { ESP_LOGE(TAG, "touch io: %s", esp_err_to_name(err)); return false; }

    esp_lcd_touch_config_t cfg = {};
    cfg.x_max = LCD_WIDTH;
    cfg.y_max = LCD_HEIGHT;
    cfg.rst_gpio_num = (gpio_num_t)PIN_TP_RST;
    cfg.int_gpio_num = (gpio_num_t)PIN_TP_INT;
    cfg.levels.reset = 0;
    cfg.levels.interrupt = 0;
    // Measured on the board: dragging right moved the model left and dragging
    // down moved it up, on both axes at once. The controller's origin is the
    // opposite corner from the panel's.
    cfg.flags.mirror_x = 1;
    cfg.flags.mirror_y = 1;

    err = esp_lcd_touch_new_i2c_cst9217(io, &cfg, &g_tp);
    if (err != ESP_OK)
        {
        ESP_LOGE(TAG, "cst9217: %s", esp_err_to_name(err));
        g_tp = nullptr;
        return false;
        }
    return true;
    }

bool touchRead(int* x, int* y)
    {
    if (g_tp == nullptr) return false;
    if (esp_lcd_touch_read_data(g_tp) != ESP_OK) return false;

    esp_lcd_touch_point_data_t pt[1] = {};
    uint8_t count = 0;
    if (esp_lcd_touch_get_data(g_tp, pt, &count, 1) != ESP_OK) return false;
    if (count == 0) return false;
    if (x != nullptr) *x = pt[0].x;
    if (y != nullptr) *y = pt[0].y;
    return true;
    }

} // namespace a3d_amoled
