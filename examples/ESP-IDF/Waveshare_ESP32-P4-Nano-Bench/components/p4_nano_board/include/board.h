// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0
//
// Derived from Waveshare's `waveshare/esp32_p4_nano` BSP (Apache-2.0): the
// pin numbers, the LDO channels, the panel and touch configuration and the
// SDMMC slot setup are theirs. See board.c for why this exists rather than
// the BSP itself.

#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

// The 10.1" 800x1280 JD9365 DSI panel this example is built for.
#define BOARD_LCD_H_RES   800
#define BOARD_LCD_V_RES   1280
#define BOARD_SD_MOUNT    "/sdcard"

/** Bring up the DSI bus and the JD9365 panel. The panel is left OFF; the
    caller turns it on, because it also wants to register DPI callbacks first. */
esp_err_t board_display_init(esp_lcd_panel_handle_t *out_panel);

/** 0..100. The backlight is an I2C register on this board, not a GPIO. */
esp_err_t board_backlight_set(int percent);

/** GT911 over the same I2C bus as the backlight. */
esp_err_t board_touch_init(esp_lcd_touch_handle_t *out_touch);

/** SDMMC slot 0, 4-bit, powered from the on-chip LDO. */
esp_err_t board_sdcard_mount(void);

#ifdef __cplusplus
}
#endif
