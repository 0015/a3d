// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file main.cpp
 * @brief a3d on a Waveshare ESP32-C6-Touch-AMOLED-2.16.
 *
 * The board half only. Everything above the panel - loading the containers,
 * skinning, binning, rasterizing, the benchmark and the viewer - is in
 * examples/common/a3d_demo, which this project and the ESP32-S3 one both
 * register as a component. That is deliberate: a benchmark copy-pasted between
 * two projects diverges, and then a comparison table is quietly reporting two
 * different programs.
 *
 * WHAT THIS BOARD IS, AND WHY IT IS THE INTERESTING ONE
 * ----------------------------------------------------
 * ESP32-C6: ONE RISC-V core at 160 MHz, RV32IMAC, and no F extension. There is
 * no floating-point unit. a3d's rasterizer is float throughout, so every
 * multiply, add and compare in the inner loop becomes a libgcc call.
 *
 * It renders correctly. It renders slowly, and the benchmark exists to say by
 * how much rather than to leave it as an impression. No compiler flag recovers
 * this: -O2 was already on, and there is no instruction to emit.
 *
 * There is also no PSRAM on this part, so the benchmark's PSRAM placement runs
 * skip themselves and the whole working set has to fit in 512 KB of SRAM.
 */

#include "a3d_demo_app.h"

#include "display_bsp.h"
#include "i2c_bsp.h"
#include "power_bsp.h"

#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace
{

const char* TAG = "a3d_c6";

constexpr int PANEL_W = 480;
constexpr int PANEL_H = 480;

// scl, sda, i2c_port - the values this board's own firmware uses.
I2cMasterBus g_i2c(7, 8, 0);
DisplayPort* g_display = nullptr;
SemaphoreHandle_t g_transDone = nullptr;

bool onColorDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* ctx)
    {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &woken);
    return woken == pdTRUE;
    }

/**
 * Push one tile and wait for its DMA, so the caller may reuse `src` at once.
 *
 * The BSP builds the panel IO with no completion callback, which is fine for
 * LVGL - it hands over a buffer it will not touch again - and wrong here: the
 * demo owns one tile buffer per worker and overwrites it as soon as the next
 * tile is drawn. Returning while the DMA still reads it would send half of one
 * tile and half of the next. The callback is registered after construction
 * through the public API rather than by forking the vendor BSP.
 */
void sendTile(const uint16_t* src, int x, int y, int w, int h)
    {
    if (g_display == nullptr) return;

    // One transfer is in flight at a time, so the completion this reports is
    // unambiguously ours. Drain first rather than trust the count: a stale
    // token would let the next tile overwrite a buffer still being read.
    while (xSemaphoreTake(g_transDone, 0) == pdTRUE) { }

    const esp_err_t err = esp_lcd_panel_draw_bitmap(
        g_display->Get_PanelHandle(), x, y, x + w, y + h, src);
    if (err != ESP_OK)
        {
        ESP_LOGE(TAG, "draw_bitmap: %s", esp_err_to_name(err));
        return;
        }
    // 480x40 at 40 MHz on four lines is about 2.4 ms; a second is four hundred
    // times that and can only mean the bus has stopped.
    if (xSemaphoreTake(g_transDone, pdMS_TO_TICKS(1000)) != pdTRUE)
        ESP_LOGE(TAG, "tile at y=%d never completed", y);
    }

uint16_t* g_band = nullptr;
constexpr int BAND_ROWS = 8;

void fill(uint16_t colour)
    {
    if (g_band == nullptr) return;
    // The panel reads big-endian, and every other pixel this demo produces is
    // swapped on its way out; do the same here so a cleared screen and a drawn
    // one agree about what the background colour is.
    const uint16_t swapped = (uint16_t)((colour >> 8) | (colour << 8));
    for (int i = 0; i < PANEL_W * BAND_ROWS; i++) g_band[i] = swapped;
    for (int y = 0; y < PANEL_H; y += BAND_ROWS)
        {
        int rows = BAND_ROWS;
        if (y + rows > PANEL_H) rows = PANEL_H - y;
        sendTile(g_band, 0, y, PANEL_W, rows);
        }
    }

bool touchRead(int* x, int* y)
    {
    if (g_display == nullptr || !g_display->Get_TouchInitialized()) return false;
    esp_lcd_touch_handle_t tp = g_display->Get_TouchHandle();
    if (tp == nullptr) return false;
    if (esp_lcd_touch_read_data(tp) != ESP_OK) return false;

    esp_lcd_touch_point_data_t pt[1] = {};
    uint8_t count = 0;
    if (esp_lcd_touch_get_data(tp, pt, &count, 1) != ESP_OK) return false;
    if (count == 0) return false;
    if (x != nullptr) *x = pt[0].x;
    if (y != nullptr) *y = pt[0].y;
    return true;
    }

} // namespace

extern "C" void app_main(void)
    {
    // The AMOLED's rail comes from the AXP2101, so the PMIC is brought up
    // before anything tries to reset or talk to the panel.
    Custom_PmicPortInit(&g_i2c, 0x34);

    g_transDone = xSemaphoreCreateBinary();
    configASSERT(g_transDone != nullptr);

    g_display = new DisplayPort(g_i2c, PANEL_W, PANEL_H);

    esp_lcd_panel_io_callbacks_t cbs = {};
    cbs.on_color_trans_done = onColorDone;
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(
        g_display->Get_IoHandle(), &cbs, g_transDone));

    g_display->DisplayPort_TouchInit();
    g_display->Set_Backlight(100);

    g_band = (uint16_t*)heap_caps_malloc((size_t)PANEL_W * BAND_ROWS * sizeof(uint16_t),
                                         MALLOC_CAP_DMA | MALLOC_CAP_8BIT);

    a3d_demo::Display d;
    d.width     = PANEL_W;
    d.height    = PANEL_H;
    d.kind      = "sh8601_qspi";
    d.clockMHz  = 40;
    // The SH8601 has no equivalent of the ST7789's RAMCTRL endian bit, so
    // every tile is byte-swapped in software on the way out. That cost is real
    // and is reported as swap_us, separately from the blit, rather than being
    // hidden inside it.
    d.swapBytes = true;
    d.sendTile  = sendTile;
    d.fill      = (g_band != nullptr) ? fill : nullptr;
    d.touchRead = touchRead;

    a3d_demo::run(d);
    }
