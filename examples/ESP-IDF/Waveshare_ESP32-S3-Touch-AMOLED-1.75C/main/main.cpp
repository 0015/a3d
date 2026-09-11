// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file main.cpp
 * @brief a3d on a Waveshare ESP32-S3-Touch-AMOLED-1.75C.
 *
 * The board half only. Everything above the panel - loading the containers,
 * skinning, binning, rasterizing, the benchmark and the viewer - is in
 * examples/common/a3d_demo, which this project, the ESP32-S3-LCD2 one and the
 * ESP32-C6 one all register as a component. The three boards therefore run the
 * same object code above the panel, which is the only reason a comparison
 * between them means anything.
 *
 * WHAT IS DIFFERENT ABOUT THIS BOARD
 * ----------------------------------
 * Same silicon as ESP32-S3-LCD2 - two Xtensa LX7 cores at 240 MHz, an FPU,
 * octal PSRAM - behind a very different display: a 466x466 CO5300 AMOLED on a
 * four-lane QSPI bus instead of a 240x320 ST7789 on one lane.
 *
 * That is 2.8x the pixels. The off-screen half of the benchmark fixes the
 * viewport at 240x320 on every board and so should report the same numbers as
 * ESP32-S3-LCD2; the `present=1` rows run at 466x466 and will not. Any gap in
 * the first set is a property of THIS BUILD, not of the panel, and worth
 * chasing - it is the closest thing to a control experiment this tree has.
 *
 * The vendor BSP is not used. It brings LVGL, an audio codec, SPIFFS, FATFS
 * and USB along with the panel; components/a3d_amoled175 is the panel and the
 * touch controller and nothing else, built from the same pin numbers and the
 * same CO5300 command table.
 */

#include "a3d_demo_app.h"
#include "amoled175.h"

#include "esp_err.h"
#include "esp_log.h"

namespace
{

a3d_amoled::Panel g_panel;

void sendTile(const uint16_t* src, int x, int y, int w, int h)
    { g_panel.sendTile(src, x, y, w, h); }

void fill(uint16_t colour) { g_panel.fill(colour); }

} // namespace

extern "C" void app_main(void)
    {
    ESP_ERROR_CHECK(g_panel.begin());
    g_panel.setBacklight(100);

    const bool touchOk = a3d_amoled::touchBegin();
    if (!touchOk) ESP_LOGW("a3d_amoled", "no touch controller; the viewer will only spin");

    a3d_demo::Display d;
    d.width     = a3d_amoled::LCD_WIDTH;
    d.height    = a3d_amoled::LCD_HEIGHT;
    d.kind      = "co5300_qspi";
    d.clockMHz  = g_panel.pixelClockMHz();
    // MEASURED, not read off the BSP. The vendor's BSP_LCD_BIGENDIAN is 0, and
    // taking that at face value produced a PINK background instead of a black
    // one: a3d's 0x1082 arrives as 0x8210, which is R=52% G=25% B=52% -
    // desaturated magenta. Equal red and blue with the green pulled down is
    // the signature of a byte-swapped RGB565, and it is worth knowing because
    // it looks nothing like "the colours are a bit off".
    //
    // So the CO5300 here reads big-endian and the swap is done in software,
    // reported separately as swap_us. The ST7789 on the LCD-2 board can be
    // told to read little-endian and pays nothing.
    d.swapBytes = true;
    // Round glass. The caption moves to the top centre and the button bar
    // becomes a tap-to-open overlay, because a bar along the bottom edge of a
    // circle is not on the panel at all.
    d.round     = true;
    d.sendTile  = sendTile;
    d.fill      = fill;
    d.touchRead = touchOk ? a3d_amoled::touchRead : nullptr;

    a3d_demo::run(d);
    }
