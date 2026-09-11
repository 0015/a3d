// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

#include "s3_panel.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace a3d_s3
{
namespace
{

const char* TAG = "a3d_panel";

// Waveshare ESP32-S3-Touch-LCD-2 wiring. These are the values the board's own
// working firmware uses; they are not guesses.
constexpr spi_host_device_t SPI_HOST_ID = SPI2_HOST;
constexpr int PIN_SCLK = 39;
constexpr int PIN_MOSI = 38;
constexpr int PIN_MISO = 40;
constexpr int PIN_DC   = 42;
constexpr int PIN_CS   = 45;
constexpr int PIN_RST  = -1;      // reset is tied high on this board
constexpr int PIN_BL   = 1;

constexpr int PCLK_HZ = 80 * 1000 * 1000;

// The largest single tile this panel will accept in one draw_bitmap. Sized in
// ROWS rather than bytes so the relationship to TILE_H is visible: 64 rows is
// well past the 40 the app uses, and the descriptor pool it buys costs a few
// hundred bytes.
constexpr int MAX_TILE_ROWS = 64;
constexpr int MAX_TRANSFER  = LCD_WIDTH * MAX_TILE_ROWS * 2;

// fill() streams this many rows at a time out of one DMA scratch band.
constexpr int BAND_ROWS = 8;

constexpr ledc_timer_t   BL_TIMER   = LEDC_TIMER_0;
constexpr ledc_mode_t    BL_MODE    = LEDC_LOW_SPEED_MODE;
constexpr ledc_channel_t BL_CHANNEL = LEDC_CHANNEL_0;
constexpr int            BL_MAX     = 1023;   // 10-bit

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

    // Backlight OFF until there is something to look at: bringing it up before
    // the first frame shows whatever the ST7789's GRAM held from the last boot.
    gpio_set_direction((gpio_num_t)PIN_BL, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_BL, 0);

    const ledc_timer_config_t timer = {
        .speed_mode      = BL_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num       = BL_TIMER,
        .freq_hz         = 10000,
        .clk_cfg         = LEDC_AUTO_CLK,
        .deconfigure     = false,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");

    // Zero-initialised and then filled: ledc_channel_config_t gained a
    // `deconfigure` member in ESP-IDF 6.0, and -Werror=missing-field-initializers
    // rejects a designated list that does not mention every one. `= {}` keeps
    // this compiling against whatever members the next release adds.
    ledc_channel_config_t chan = {};
    chan.gpio_num           = PIN_BL;
    chan.speed_mode         = BL_MODE;
    chan.channel            = BL_CHANNEL;
    chan.intr_type          = LEDC_INTR_DISABLE;
    chan.timer_sel          = BL_TIMER;
    chan.duty               = 0;
    chan.hpoint             = 0;
    chan.sleep_mode         = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD;
    chan.flags.output_invert = 0;
    ESP_RETURN_ON_ERROR(ledc_channel_config(&chan), TAG, "ledc channel");

    const spi_bus_config_t bus = {
        .mosi_io_num     = PIN_MOSI,
        .miso_io_num     = PIN_MISO,
        .sclk_io_num     = PIN_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .data4_io_num    = -1,
        .data5_io_num    = -1,
        .data6_io_num    = -1,
        .data7_io_num    = -1,
        .data_io_default_level = false,
        .max_transfer_sz = MAX_TRANSFER,
        .flags           = 0,
        .isr_cpu_id      = ESP_INTR_CPU_AFFINITY_AUTO,
        .intr_flags      = 0,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI_HOST_ID, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    _done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(_done != nullptr, ESP_ERR_NO_MEM, TAG, "done sem");

    esp_lcd_panel_io_spi_config_t io = {};
    // ESP-IDF 6.0 made these `gpio_num_t`; C++ will not convert an int to an
    // enum. The cast is correct on 5.x too, so one source builds on both.
    io.cs_gpio_num         = (gpio_num_t)PIN_CS;
    io.dc_gpio_num         = (gpio_num_t)PIN_DC;
    io.spi_mode            = 0;
    io.pclk_hz             = PCLK_HZ;
    io.trans_queue_depth   = 10;
    io.on_color_trans_done = onColorDone;
    io.user_ctx            = _done;
    io.lcd_cmd_bits        = 8;
    io.lcd_param_bits      = 8;
    esp_lcd_panel_io_handle_t ioh = nullptr;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI_HOST_ID, &io, &ioh),
        TAG, "panel io");
    _io = ioh;

    esp_lcd_panel_dev_config_t dev = {};
    dev.reset_gpio_num = (gpio_num_t)PIN_RST;
    dev.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB;
    // THE LINE THAT REMOVES A BYTE SWAP FROM EVERY FRAME.
    // a3d's framebuffer is native-endian RGB565, which on Xtensa is little
    // endian; an ST7789 defaults to big endian and every LVGL port on this
    // board therefore swaps 76,800 halfwords per frame in software. The panel
    // can be told to read little endian instead - it is bit 3 of RAMCTRL - and
    // then the rasterizer's output goes to the wire untouched.
    dev.data_endian    = LCD_RGB_DATA_ENDIAN_LITTLE;
    dev.bits_per_pixel = 16;
    esp_lcd_panel_handle_t panel = nullptr;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(ioh, &dev, &panel), TAG, "st7789");
    _panel = panel;

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(panel, false, false), TAG, "mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(panel, false), TAG, "swap_xy");
    // This board's panel is wired for inverted colour; without this every
    // frame comes out as its own negative.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel, true), TAG, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "disp on");

    _band = (uint16_t*)heap_caps_malloc((size_t)LCD_WIDTH * BAND_ROWS * sizeof(uint16_t),
                                        MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(_band != nullptr, ESP_ERR_NO_MEM, TAG, "fill band");

    _started = true;
    fill(0x0000);
    return ESP_OK;
    }

void Panel::setBacklight(int percent)
    {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    const uint32_t duty = (uint32_t)((percent * BL_MAX) / 100);
    ledc_set_duty(BL_MODE, BL_CHANNEL, duty);
    ledc_update_duty(BL_MODE, BL_CHANNEL);
    }

void Panel::fill(uint16_t colour)
    {
    if (!_started || _band == nullptr) return;
    for (int i = 0; i < LCD_WIDTH * BAND_ROWS; i++) _band[i] = colour;
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

    // One transfer is in flight at a time, so the completion the semaphore
    // reports is unambiguously this one. A stale token from an earlier frame
    // would let the next tile overwrite a buffer still being read, so drain
    // first rather than trusting the count.
    while (xSemaphoreTake((SemaphoreHandle_t)_done, 0) == pdTRUE) { }

    const esp_err_t err = esp_lcd_panel_draw_bitmap(
        (esp_lcd_panel_handle_t)_panel, x, y, x + w, y + h, src);
    if (err == ESP_OK)
        {
        // 240x40 at 80 MHz is about 1.9 ms; a second is four hundred times that
        // and can only mean the bus has stopped.
        if (xSemaphoreTake((SemaphoreHandle_t)_done, pdMS_TO_TICKS(1000)) != pdTRUE)
            ESP_LOGE(TAG, "tile at y=%d never completed", y);
        }
    else
        {
        ESP_LOGE(TAG, "draw_bitmap: %s", esp_err_to_name(err));
        }

    }

int Panel::pixelClockMHz() const { return PCLK_HZ / 1000000; }

} // namespace a3d_s3
