// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file main.cpp
 * @brief The a3d benchmark on a Waveshare ESP32-P4-Nano and its 10.1" DSI panel.
 *
 * WHY THIS PROJECT EXISTS
 * -----------------------
 * The published benchmark table has an ESP32-P4 column, and its Panel cell
 * says "none (headless run)". That is accurate rather than missing: the P4
 * sweep was taken with examples/ESP32-Bench-Headless, which sends nothing to a
 * display, so the P4 is the one measured part with no row in the "On the
 * panel" table. This project fills that gap and nothing else.
 *
 * The board half is all that is here. Loading the containers, skinning,
 * binning, rasterizing, the self-checks, the timing and the viewer are in
 * examples/common/a3d_demo, which the S3 and C6 projects register too. The
 * boards therefore run the SAME OBJECT CODE above the panel, which is the only
 * reason a comparison between the columns means anything - and the reason this
 * file adds no rendering of its own, however tempting.
 *
 * WHAT IS DIFFERENT ABOUT THIS BOARD
 * ----------------------------------
 * 800x1280 over MIPI-DSI, against 240x320 on one SPI lane and 466x466 on four
 * QSPI lanes. That is 13.3x the pixels of the ST7789 board, so the `present=1`
 * rows will be nothing like the others - they are not meant to be. The
 * off-screen rows are fixed at 240x320 on every board and are where the parts
 * are compared; the panel rows measure the panel.
 *
 * Two things about DSI are not like SPI, and both are in sendTile below.
 */

#include "a3d_demo_app.h"
#include "board.h"

#include "esp_err.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace
{

const char* TAG = "a3d_p4_bench";

esp_lcd_panel_handle_t g_panel = nullptr;
esp_lcd_touch_handle_t g_touch = nullptr;

/**
 * One DSI bus, and the workers take turns on it.
 *
 * Not optional at two workers: `esp_lcd_panel_draw_bitmap` on a DPI panel
 * rejects a second call while the first is in flight, and the two tiles would
 * be interleaved on the wire even if it did not.
 */
SemaphoreHandle_t g_busMutex = nullptr;

/**
 * A DPI `draw_bitmap` only QUEUES the 2D-DMA copy into the panel's frame
 * buffer. It returns long before the copy has happened, and the demo owns one
 * tile buffer per worker and starts overwriting it immediately - so returning
 * early sends half of one tile and half of the next.
 *
 * Every give is paired with exactly one take, both inside the bus mutex, so no
 * completion token can survive into the next blit. That pairing is the whole
 * reason this is correct; a binary semaphore taken outside the mutex would let
 * a stale token through, which is a fault this tree has already paid for on a
 * different panel.
 */
SemaphoreHandle_t g_blitDone = nullptr;

bool IRAM_ATTR onColorTransDone(esp_lcd_panel_handle_t,
                                esp_lcd_dpi_panel_event_data_t*, void*)
    {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(g_blitDone, &woken);
    return woken == pdTRUE;
    }

void sendTile(const uint16_t* src, int x, int y, int w, int h)
    {
    if (g_panel == nullptr || src == nullptr || w <= 0 || h <= 0) return;

    xSemaphoreTake(g_busMutex, portMAX_DELAY);
    if (esp_lcd_panel_draw_bitmap(g_panel, x, y, x + w, y + h, src) == ESP_OK)
        {
        // 800x40 into PSRAM over 2D-DMA is tens of microseconds. A second is
        // four orders of magnitude past that and can only mean the transfer
        // never completed, so the log line is worth more than a silent hang.
        if (xSemaphoreTake(g_blitDone, pdMS_TO_TICKS(1000)) != pdTRUE)
            ESP_LOGE(TAG, "tile at y=%d never completed", y);
        }
    xSemaphoreGive(g_busMutex);
    }

void fill(uint16_t colour)
    {
    // A band at a time out of the demo's own tile-sized buffer would need a
    // buffer this file does not own, so the fill is done in rows of one tile
    // height from a small static band in DMA-capable memory.
    static uint16_t* band = nullptr;
    constexpr int BAND_ROWS = 16;
    if (band == nullptr)
        {
        band = (uint16_t*)heap_caps_malloc(
            (size_t)BOARD_LCD_H_RES * BAND_ROWS * sizeof(uint16_t),
            MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (band == nullptr) { ESP_LOGW(TAG, "no DMA memory to clear the panel"); return; }
        }
    for (int i = 0; i < BOARD_LCD_H_RES * BAND_ROWS; i++) band[i] = colour;
    for (int y = 0; y < BOARD_LCD_V_RES; y += BAND_ROWS)
        {
        int rows = BAND_ROWS;
        if (y + rows > BOARD_LCD_V_RES) rows = BOARD_LCD_V_RES - y;
        sendTile(band, 0, y, BOARD_LCD_H_RES, rows);
        }
    }

/**
 * Touch, with both axes inverted, and up to two contacts.
 *
 * WHY THE INVERSION
 *
 *   The GT911 reports in its own frame, and on this board that frame is turned
 *   180 degrees from the one the panel is scanned in: dragging right moved the
 *   model left and dragging down moved it up. So x becomes (W-1) - x and y
 *   becomes (H-1) - y - a half turn, not a transpose, which is why both axes
 *   flip and the width and height are not exchanged.
 *
 *   It is done HERE rather than in the shared demo because it is a property of
 *   this board's wiring, and the demo is the half that must stay identical
 *   across boards.
 *
 * WHY TWO POINTS
 *
 *   Pinch. `a3d_demo::Display::touchRead2` returns the CONTACT COUNT rather
 *   than a bool, and the demo re-anchors its gesture whenever that count
 *   changes - so a second finger landing cannot be read as a huge one-finger
 *   drag. Nothing here interprets the gesture; this reports fingers.
 */
int touchRead2(int* x, int* y, int* x2, int* y2)
    {
    if (g_touch == nullptr) return 0;
    if (esp_lcd_touch_read_data(g_touch) != ESP_OK) return 0;
    esp_lcd_touch_point_data_t pt[2] = {};
    uint8_t n = 0;
    if (esp_lcd_touch_get_data(g_touch, pt, &n, 2) != ESP_OK) return 0;
    if (n == 0) return 0;

    if (x)  *x  = (BOARD_LCD_H_RES - 1) - pt[0].x;
    if (y)  *y  = (BOARD_LCD_V_RES - 1) - pt[0].y;
    if (n >= 2)
        {
        if (x2) *x2 = (BOARD_LCD_H_RES - 1) - pt[1].x;
        if (y2) *y2 = (BOARD_LCD_V_RES - 1) - pt[1].y;
        return 2;
        }
    return 1;
    }

} // namespace

extern "C" void app_main(void)
    {
    g_busMutex = xSemaphoreCreateMutex();
    g_blitDone = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(g_busMutex != nullptr && g_blitDone != nullptr ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_ERROR_CHECK(board_display_init(&g_panel));

    // Registered here rather than in the board component: the component brings
    // the panel up and says nothing about how an application blits to it, and
    // the sibling STL example registers its own callback the same way.
    esp_lcd_dpi_panel_event_callbacks_t cbs = {};
    cbs.on_color_trans_done = onColorTransDone;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(g_panel, &cbs, nullptr));

    // Backlight last, and only once there is something to put behind it -
    // raising it first shows whatever the panel held from the previous boot.
    fill(0x0000);
    ESP_ERROR_CHECK(board_backlight_set(100));

    if (board_touch_init(&g_touch) != ESP_OK)
        {
        g_touch = nullptr;
        ESP_LOGW(TAG, "no touch controller; the viewer will only spin");
        }

    a3d_demo::Display d;
    d.width     = BOARD_LCD_H_RES;
    d.height    = BOARD_LCD_V_RES;
    d.kind      = "jd9365_dsi";
    // The DPI pixel clock the board component configures. Reported, not used.
    d.clockMHz  = 80;
    // Little-endian. Unlike the CO5300 on the S3 AMOLED board, nothing here
    // needs a software byte swap - the DSI path takes a3d's native-endian
    // RGB565 straight through, so swap_us should come out at 0.0 and a PINK
    // background would mean this line is wrong.
    d.swapBytes = false;
    d.round     = false;
    d.sendTile  = sendTile;
    d.fill      = fill;
    // touchRead2 supersedes touchRead: the demo prefers it when both are set,
    // and it is the only one of the two that can carry a pinch.
    d.touchRead2 = (g_touch != nullptr) ? touchRead2 : nullptr;

    ESP_LOGI(TAG, "JD9365 %dx%d, benchmark then viewer", d.width, d.height);
    a3d_demo::run(d);
    }
