// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file amoled175.h
 * @brief The CO5300 AMOLED and CST9217 touch on a Waveshare
 *        ESP32-S3-Touch-AMOLED-1.75C, as a tile sink.
 *
 * This is a deliberately small replacement for the vendor BSP. That BSP is a
 * fine thing and this project uses none of it: it brings LVGL, an ES7210 audio
 * codec, SPIFFS, FATFS and USB along with the panel, and a3d wants a panel.
 * The pin numbers, the CO5300 initialisation table and the 6-pixel column
 * offset below are taken from it - it is Apache-2.0, and the attribution is in
 * amoled175.cpp.
 *
 * a3d renders a TILE at a time into a small internal-RAM buffer, so there is
 * no framebuffer here: sendTile() is the whole output path. A 466x466 RGB565
 * framebuffer would be 434,312 bytes, which is more internal SRAM than this
 * part has.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

namespace a3d_amoled
{

/// Native panel geometry. Square; nothing here rotates.
inline constexpr int LCD_WIDTH  = 466;
inline constexpr int LCD_HEIGHT = 466;

class Panel
    {
    public:
        Panel() = default;

        esp_err_t begin();

        int width()  const { return LCD_WIDTH; }
        int height() const { return LCD_HEIGHT; }

        /** 0..100 percent. An AMOLED has no backlight; this is a panel command. */
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

/** @return false if the controller did not answer; the app stays usable. */
bool touchBegin();

/** @return true while a finger is down, with `x`,`y` in panel coordinates. */
bool touchRead(int* x, int* y);

} // namespace a3d_amoled
