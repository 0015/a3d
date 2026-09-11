// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file s3_panel.h
 * @brief The ST7789 on a Waveshare ESP32-S3-Touch-LCD-2, as a tile sink.
 *
 * a3d renders a TILE at a time into a small internal-RAM buffer, so this panel
 * wrapper deliberately has no framebuffer of its own: sendTile() is the whole
 * output path. A full 240x320 framebuffer would be 153,600 bytes of the exact
 * memory the rasterizer wants, and the panel does not need one - the ST7789
 * holds the image in its own GRAM.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

namespace a3d_s3
{

/// Native panel geometry. Portrait; nothing here rotates.
inline constexpr int LCD_WIDTH  = 240;
inline constexpr int LCD_HEIGHT = 320;

class Panel
    {
    public:
        Panel() = default;

        esp_err_t begin();

        int width()  const { return LCD_WIDTH; }
        int height() const { return LCD_HEIGHT; }

        /** Backlight, 0..100 percent, LEDC on GPIO 1. */
        void setBacklight(int percent);

        /** Paint the whole panel one colour. */
        void fill(uint16_t colour);

        /**
         * Push one tile and WAIT for its DMA, so the caller may reuse `src` the
         * instant this returns.
         *
         * Blocking is not laziness. A worker owns one tile buffer and starts
         * drawing the next tile immediately; returning while the DMA still
         * reads that buffer would send half of one tile and half of the next.
         */
        void sendTile(const uint16_t* src, int x, int y, int w, int h);

        int pixelClockMHz() const;

    private:
        void* _io    = nullptr;      // esp_lcd_panel_io_handle_t
        void* _panel = nullptr;      // esp_lcd_panel_handle_t
        void* _done  = nullptr;      // SemaphoreHandle_t, one transfer in flight
        uint16_t* _band = nullptr;   // DMA scratch for fill()
        bool _started = false;
    };

} // namespace a3d_s3
