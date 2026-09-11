# SPDX-FileCopyrightText: 2026 Eric Nam
# SPDX-License-Identifier: Apache-2.0
"""The panel and touch glue a generated project needs, as source it can build.

WHY THIS IS A SEPARATE MODULE FROM a3d_project.py

    `a3d_project.py` answers "will my model build and run", and it answers it
    by rendering OFF SCREEN, because a3d cannot know the board. That is still
    the right default and it is still what you get by asking for nothing.

    But "then write one function" is the step where most people stop. The
    function is short and every hard part of it is invisible: the DMA wait that
    must not return early, the byte order that turns a black background pink,
    the bus mutex two workers need, the vendor macro that does not compile in
    C++. Each of those has cost this tree a session.

    So this writes that function, for the buses a3d has actually been run on.
    The catalogue below is deliberately small: a bus that is here is one whose
    initialisation was copied from a project in this tree that has driven real
    glass, not one assembled from a datasheet.

THE PINS ARE YOURS AND THE FILE SAYS SO

    Nothing here can know your wiring. Every generated `panel.c` opens with a
    block of `#define`s and a comment saying which board they came from. That
    is honest: the alternative is a plausible-looking default that silently
    drives the wrong GPIO, which looks exactly like a dead panel.

WHY panel.c IS C AND NOT C++

    Vendor esp_lcd config MACROS use C designated initialisers in an order C++
    rejects - `ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG()` and
    `CO5300_PANEL_BUS_QSPI_CONFIG()` have both cost time in this tree, and the
    bus one is worse than it looks because `data0_io_num` is a union alias for
    `mosi_io_num`, so a macro in a perfectly sensible order is still out of
    order for the struct. Keeping this half in C sidesteps the whole class.
    `main.cpp` reaches it through one `extern "C"` header.
"""

from __future__ import annotations

# --------------------------------------------------------------------------
# The catalogue.
#
# `component` is what goes in idf_component.yml. An empty dict means the driver
# is part of esp_lcd itself and nothing is fetched - which is why ST7789 is the
# default SPI choice rather than the most common one.
# --------------------------------------------------------------------------

PANELS = {
    "offscreen": dict(
        label="None - render off screen and log the frame time",
        family="offscreen", targets=("esp32s3", "esp32p4", "esp32c6"),
        components={}, size=(240, 320), swap=False,
        source="a3d_project.py's default",
    ),
    "spi_st7789": dict(
        label="SPI - ST7789 (240x320 and friends)",
        family="spi", targets=("esp32s3", "esp32p4", "esp32c6"),
        components={}, size=(240, 320), swap=False,
        header=None, create="esp_lcd_new_panel_st7789",
        # RAMCTRL bit 3. This is the whole reason ST7789 costs no byte swap.
        little_endian=True, invert=True,
        source="examples/Waveshare_ESP32-S3-Touch-LCD-2 (hardware-verified)",
    ),
    "spi_gc9a01": dict(
        label="SPI - GC9A01 (240x240 round)",
        family="spi", targets=("esp32s3", "esp32p4", "esp32c6"),
        components={"espressif/esp_lcd_gc9a01": "^2.0.0"},
        size=(240, 240), swap=False, round=True,
        header="esp_lcd_gc9a01.h", create="esp_lcd_new_panel_gc9a01",
        little_endian=False, invert=True,
        source="esp_lcd_gc9a01 component README",
    ),
    "spi_ili9341": dict(
        label="SPI - ILI9341 (240x320)",
        family="spi", targets=("esp32s3", "esp32p4", "esp32c6"),
        components={"espressif/esp_lcd_ili9341": "^1.2.0"},
        size=(240, 320), swap=False,
        header="esp_lcd_ili9341.h", create="esp_lcd_new_panel_ili9341",
        little_endian=False, invert=False,
        source="esp_lcd_ili9341 component README",
    ),
    "qspi_sh8601": dict(
        label="QSPI - SH8601 AMOLED (368x448, 466x466)",
        family="qspi", targets=("esp32s3", "esp32p4", "esp32c6"),
        components={"espressif/esp_lcd_sh8601": "^1.0.0"},
        size=(466, 466), swap=True, round=True,
        header="esp_lcd_sh8601.h", create="esp_lcd_new_panel_sh8601",
        vendor="sh8601_vendor_config_t",
        source="examples/Waveshare_ESP32-C6-Touch-AMOLED-2.16 (hardware-verified)",
    ),
    "dsi_jd9365": dict(
        label="MIPI-DSI - JD9365 (800x1280) - ESP32-P4 only",
        family="dsi", targets=("esp32p4",),
        components={"waveshare/esp_lcd_jd9365": "~1.0.6"},
        size=(800, 1280), swap=False,
        header="esp_lcd_jd9365.h", create="esp_lcd_new_panel_jd9365",
        vendor="jd9365_vendor_config_t", lanes=2, lane_mbps=1500, dpi_mhz=80,
        timing=(20, 20, 40, 10, 4, 30),
        source="examples/Waveshare_ESP32-P4-Nano (hardware-verified)",
    ),
    "dsi_ek79007": dict(
        label="MIPI-DSI - EK79007 (1024x600) - ESP32-P4 only",
        family="dsi", targets=("esp32p4",),
        components={"espressif/esp_lcd_ek79007": "^1.0.0"},
        size=(1024, 600), swap=False,
        header="esp_lcd_ek79007.h", create="esp_lcd_new_panel_ek79007",
        vendor="ek79007_vendor_config_t", lanes=2, lane_mbps=1500, dpi_mhz=52,
        timing=(160, 10, 160, 23, 12, 12),
        source="esp_lcd_ek79007 component README",
    ),
}

TOUCH = {
    "none": dict(label="None", components={}, header=None, create=None,
                 addr=None),
    "gt911": dict(label="I2C - GT911",
                  components={"espressif/esp_lcd_touch_gt911": "^1.1.0"},
                  header="esp_lcd_touch_gt911.h",
                  create="esp_lcd_touch_new_i2c_gt911",
                  addr="ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS",
                  cmd_bits=16, param_bits=8,
                  source="examples/Waveshare_ESP32-P4-Nano (hardware-verified)"),
    "cst816s": dict(label="I2C - CST816S",
                    components={"espressif/esp_lcd_touch_cst816s": "^1.0.0"},
                    header="esp_lcd_touch_cst816s.h",
                    create="esp_lcd_touch_new_i2c_cst816s",
                    addr="ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS",
                    cmd_bits=8, param_bits=8,
                    source="esp_lcd_touch_cst816s component README"),
    "ft5x06": dict(label="I2C - FT5x06",
                   components={"espressif/esp_lcd_touch_ft5x06": "^1.0.0"},
                   header="esp_lcd_touch_ft5x06.h",
                   create="esp_lcd_touch_new_i2c_ft5x06",
                   addr="ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS",
                   cmd_bits=8, param_bits=8,
                   source="esp_lcd_touch_ft5x06 component README"),
}

#: Suggested defaults per part. Wiring, not law - every one is a #define.
PINS = {
    "esp32s3": dict(sclk=39, mosi=38, miso=40, dc=42, cs=45, rst=-1, bl=1,
                    qsclk=40, qd0=46, qd1=45, qd2=42, qd3=41, qcs=44,
                    sda=48, scl=47, tp_int=-1, tp_rst=-1),
    "esp32p4": dict(sclk=8, mosi=9, miso=-1, dc=10, cs=11, rst=-1, bl=23,
                    qsclk=8, qd0=9, qd1=10, qd2=11, qd3=12, qcs=13,
                    sda=7, scl=8, tp_int=-1, tp_rst=-1),
    "esp32c6": dict(sclk=1, mosi=2, miso=-1, dc=3, cs=4, rst=-1, bl=5,
                    qsclk=1, qd0=2, qd1=3, qd2=4, qd3=5, qcs=6,
                    sda=7, scl=6, tp_int=-1, tp_rst=-1),
}


def choices(target: str):
    """Which panels are possible on this part. DSI is not optional-by-taste."""
    return [k for k, v in PANELS.items() if target in v["targets"]]


def validate(panel: str, touch: str, target: str) -> None:
    if panel not in PANELS:
        raise ValueError("unknown panel %r; known: %s"
                         % (panel, ", ".join(sorted(PANELS))))
    if touch not in TOUCH:
        raise ValueError("unknown touch %r; known: %s"
                         % (touch, ", ".join(sorted(TOUCH))))
    if target not in PANELS[panel]["targets"]:
        raise ValueError(
            "%s has no %s: the %s is only on %s. This is a silicon fact, not a "
            "driver that is missing."
            % (target, PANELS[panel]["family"].upper(), PANELS[panel]["family"].upper(),
               " / ".join(PANELS[panel]["targets"])))
    if panel == "offscreen" and touch != "none":
        raise ValueError("touch without a panel: pick a display interface first")


def _fill(tpl: str, **kw) -> str:
    for k, v in kw.items():
        tpl = tpl.replace("@%s@" % k, str(v))
    if "@" in tpl.replace("@@", ""):
        pass          # a literal @ in prose is fine; unfilled tokens are not
    return tpl


# --------------------------------------------------------------------------
# panel.h - identical for every choice, which is the point.
#
# main.cpp is generated ONCE and never varies by bus. Everything that differs
# is behind these eight functions, so adding a bus cannot break the app.
# --------------------------------------------------------------------------

PANEL_H = '''// SPDX-License-Identifier: Apache-2.0
// Generated by tools/a3d_panel.py for: @LABEL@
//
// The same eight functions whatever the bus is, which is why main.cpp does not
// change when you change your mind about the panel.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bring up the bus, the panel and (if configured) the touch controller. */
esp_err_t a3d_panel_init(void);

int  a3d_panel_width(void);
int  a3d_panel_height(void);

/**
 * True when the panel reads RGB565 big-endian and a3d must swap every
 * halfword. Measured cost at 480x480 in this tree: 11.6 ms a frame. Look for
 * the panel register that removes it before accepting it.
 */
bool a3d_panel_swap_bytes(void);

/** True for circular glass; the viewer puts its caption top centre. */
bool a3d_panel_round(void);

/**
 * Push one tile and WAIT for it. Must not return until `src` may be reused:
 * the viewer owns one tile buffer per worker and hands it to the next tile
 * immediately, so a driver that queues the pointer and returns will send
 * whatever the next tile overwrote it with.
 */
void a3d_panel_send(const uint16_t *src, int x, int y, int w, int h);

/** Paint the whole panel one colour. Used once, before the first frame. */
void a3d_panel_fill(uint16_t colour);

/** False when no finger is down. Always false when touch is not configured. */
bool a3d_panel_touch(int *x, int *y);

/** A panel bus is not re-entrant. Two workers take turns through these. */
void a3d_panel_lock(void);
void a3d_panel_unlock(void);

#ifdef __cplusplus
}
#endif
'''

# --------------------------------------------------------------------------
# The parts every panel.c shares: the mutex, fill(), and the touch block.
# --------------------------------------------------------------------------

_COMMON_TOP = '''// SPDX-License-Identifier: Apache-2.0
// Generated by tools/a3d_panel.py for: @LABEL@
//
// THE PINS BELOW ARE NOT YOUR BOARD'S until you have checked them. They are
// @SOURCE@'s.
// Nothing in a3d can know your wiring, and a plausible default driving the
// wrong GPIO looks exactly like a dead panel.

#include "panel.h"

#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
@EXTRA_INCLUDES@
static const char *TAG = "a3d_panel";

#define PANEL_W @W@
#define PANEL_H @H@

'''

_COMMON_MUTEX = '''
// --- the bus mutex ---------------------------------------------------------
//
// Only consulted when more than one worker draws. Created unconditionally
// because a one-worker build that later raises the count would otherwise get a
// null mutex and a corrupted frame rather than a compile error.
static SemaphoreHandle_t s_bus_mutex;

void a3d_panel_lock(void)   { if (s_bus_mutex) xSemaphoreTake(s_bus_mutex, portMAX_DELAY); }
void a3d_panel_unlock(void) { if (s_bus_mutex) xSemaphoreGive(s_bus_mutex); }

int  a3d_panel_width(void)  { return PANEL_W; }
int  a3d_panel_height(void) { return PANEL_H; }
bool a3d_panel_swap_bytes(void) { return @SWAP@; }
bool a3d_panel_round(void)      { return @ROUND@; }
'''

_COMMON_FILL = '''
// --- fill ------------------------------------------------------------------
//
// A band at a time out of one DMA-capable scratch buffer, rather than a full
// framebuffer: a 240x320 one is 153,600 bytes of exactly the internal SRAM the
// rasterizer wants, and the panel is holding the image anyway.
#define FILL_BAND_ROWS 8
static uint16_t *s_band;

void a3d_panel_fill(uint16_t colour)
{
    if (!s_band) {
        s_band = heap_caps_malloc((size_t)PANEL_W * FILL_BAND_ROWS * sizeof(uint16_t),
                                  MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (!s_band) { ESP_LOGE(TAG, "no DMA memory for fill"); return; }
    }
    for (int i = 0; i < PANEL_W * FILL_BAND_ROWS; i++) s_band[i] = colour;
    for (int y = 0; y < PANEL_H; y += FILL_BAND_ROWS) {
        int rows = FILL_BAND_ROWS;
        if (y + rows > PANEL_H) rows = PANEL_H - y;
        a3d_panel_send(s_band, 0, y, PANEL_W, rows);
    }
}
'''

_TOUCH_NONE = '''
// --- touch -----------------------------------------------------------------
// Not configured. Regenerate with a touch controller selected, or fill this in.
bool a3d_panel_touch(int *x, int *y) { (void)x; (void)y; return false; }

static esp_err_t touch_init(void) { return ESP_OK; }
'''

_TOUCH_I2C = '''
// --- touch: @TOUCH_LABEL@ ---------------------------------------------------
//
// From @TOUCH_SOURCE@.
//
// The config struct is filled FIELD BY FIELD rather than through the vendor's
// ESP_LCD_TOUCH_IO_I2C_*_CONFIG() macro. That macro orders its designators the
// way C allows and C++ does not, and forking a vendor component to fix a macro
// is the wrong trade - so it is written out here instead.
#define TOUCH_SDA_GPIO  @SDA@
#define TOUCH_SCL_GPIO  @SCL@
#define TOUCH_INT_GPIO  @TP_INT@      // -1 when the line is not wired
#define TOUCH_RST_GPIO  @TP_RST@      // -1 when reset is tied
#define TOUCH_I2C_PORT  I2C_NUM_0
#define TOUCH_I2C_HZ    (400 * 1000)

static esp_lcd_touch_handle_t s_touch;

static esp_err_t touch_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = TOUCH_SDA_GPIO,
        .scl_io_num = TOUCH_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    // Already up if the panel or a PMIC brought it up first; that is not an
    // error, and re-creating it would be.
    if (i2c_master_get_bus_handle(TOUCH_I2C_PORT, &bus) != ESP_OK || bus == NULL)
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "i2c bus");

    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr       = @TOUCH_ADDR@,
        // Required by the bus-HANDLE form of esp_lcd_new_panel_io_i2c. The
        // port-number form ignores it, which is why vendor configs omit it and
        // why leaving it out here would give a clock of zero.
        .scl_speed_hz   = TOUCH_I2C_HZ,
        .control_phase_bytes = 1,
        .dc_bit_offset  = 0,
        .lcd_cmd_bits   = @TOUCH_CMD_BITS@,
        .lcd_param_bits = @TOUCH_PARAM_BITS@,
        .flags = { .disable_control_phase = 1 },
    };
    esp_lcd_panel_io_handle_t tp_io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &tp_io), TAG, "tp io");

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = PANEL_W,
        .y_max = PANEL_H,
        .rst_gpio_num = TOUCH_RST_GPIO,
        .int_gpio_num = TOUCH_INT_GPIO,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    ESP_RETURN_ON_ERROR(@TOUCH_CREATE@(tp_io, &tp_cfg, &s_touch), TAG, "touch");
    ESP_LOGI(TAG, "touch @TOUCH_LABEL@ ready");
    return ESP_OK;
}

bool a3d_panel_touch(int *x, int *y)
{
    if (!s_touch) return false;
    esp_lcd_touch_read_data(s_touch);
    uint16_t px[1], py[1], strength[1];
    uint8_t count = 0;
    if (!esp_lcd_touch_get_coordinates(s_touch, px, py, strength, &count, 1) || count == 0)
        return false;
    if (x) *x = px[0];
    if (y) *y = py[0];
    return true;
}
'''

# --------------------------------------------------------------------------
# Per-family bodies.
# --------------------------------------------------------------------------

_OFFSCREEN_BODY = '''
// --- no panel --------------------------------------------------------------
//
// This counts pixels instead of sending them, so `idf.py flash monitor` works
// on the FIRST try and prints the real frame time for your model on your part.
// That number beats any estimate, which is why it is the default rather than a
// stub waiting for a driver you have not written yet.
volatile uint32_t g_pixels_sent;

void a3d_panel_send(const uint16_t *src, int x, int y, int w, int h)
{
    (void)src; (void)x; (void)y;
    g_pixels_sent += (uint32_t)(w * h);
}

esp_err_t a3d_panel_init(void)
{
    s_bus_mutex = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "off screen, %dx%d - nothing is sent to a display", PANEL_W, PANEL_H);
    return ESP_OK;
}
'''

_SPI_BODY = '''
// --- SPI -------------------------------------------------------------------
#define PIN_SCLK   @SCLK@
#define PIN_MOSI   @MOSI@
#define PIN_MISO   @MISO@       // -1 when the panel is write-only
#define PIN_DC     @DC@
#define PIN_CS     @CS@
#define PIN_RST    @RST@        // -1 when reset is tied high
#define PIN_BL     @BL@         // -1 for no backlight control
#define SPI_HOST_ID   SPI2_HOST
#define PCLK_HZ       (@PCLK_MHZ@ * 1000 * 1000)

// The largest tile one draw_bitmap will carry, in ROWS so its relationship to
// the app's tile height is visible. 64 is comfortably past the 40 main.cpp
// asks for; the descriptor pool it buys costs a few hundred bytes.
#define MAX_TILE_ROWS 64

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t      s_done;

static bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *ev, void *ctx)
{
    (void)io; (void)ev;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &woken);
    return woken == pdTRUE;
}

void a3d_panel_send(const uint16_t *src, int x, int y, int w, int h)
{
    if (!s_panel || !src || w <= 0 || h <= 0) return;

    // Drain first rather than trusting the count. One transfer is in flight at
    // a time, so a token left over from an earlier frame would let the next
    // tile overwrite a buffer the DMA is still reading - which sends half of
    // one tile and half of the next, and looks like a rasterizer bug.
    while (xSemaphoreTake(s_done, 0) == pdTRUE) { }

    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h, src);
    if (err != ESP_OK) { ESP_LOGE(TAG, "draw_bitmap: %s", esp_err_to_name(err)); return; }

    // 240x40 at 80 MHz is about 1.9 ms. A second is four hundred times that and
    // can only mean the bus has stopped.
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(1000)) != pdTRUE)
        ESP_LOGE(TAG, "tile at y=%d never completed", y);
}

esp_err_t a3d_panel_init(void)
{
    s_bus_mutex = xSemaphoreCreateMutex();
    s_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_done, ESP_ERR_NO_MEM, TAG, "sem");

#if PIN_BL >= 0
    // Backlight OFF until there is something to look at. Raising it before the
    // first frame shows whatever the panel's GRAM held from the last boot.
    gpio_set_direction(PIN_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_BL, 0);
#endif

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = PANEL_W * MAX_TILE_ROWS * 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI_HOST_ID, &bus, SPI_DMA_CH_AUTO), TAG, "spi");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_CS,
        .dc_gpio_num = PIN_DC,
        .spi_mode = 0,
        .pclk_hz = PCLK_HZ,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_done,
        .user_ctx = s_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI_HOST_ID,
                                                 &io_cfg, &io), TAG, "panel io");

    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
@ENDIAN_FIELD@        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(@CREATE@(io, &dev, &s_panel), TAG, "panel");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, false, false), TAG, "mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, false), TAG, "swap_xy");
@INVERT_CALL@    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp on");

    a3d_panel_fill(0x0000);
#if PIN_BL >= 0
    gpio_set_level(PIN_BL, 1);
#endif
    ESP_RETURN_ON_ERROR(touch_init(), TAG, "touch");
    ESP_LOGI(TAG, "@LABEL@ %dx%d at %d MHz", PANEL_W, PANEL_H, PCLK_HZ / 1000000);
    return ESP_OK;
}
'''

_QSPI_BODY = '''
// --- QSPI ------------------------------------------------------------------
//
// Four data lanes and NO dc line: the command carries it, which is why
// lcd_cmd_bits is 32 here and 8 on a 3-wire SPI panel.
#define PIN_SCLK   @QSCLK@
#define PIN_D0     @QD0@
#define PIN_D1     @QD1@
#define PIN_D2     @QD2@
#define PIN_D3     @QD3@
#define PIN_CS     @QCS@
#define PIN_RST    @RST@
#define SPI_HOST_ID   SPI2_HOST
#define PCLK_HZ       (@PCLK_MHZ@ * 1000 * 1000)
#define MAX_TILE_ROWS 64

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t      s_done;

static bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *ev, void *ctx)
{
    (void)io; (void)ev;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &woken);
    return woken == pdTRUE;
}

void a3d_panel_send(const uint16_t *src, int x, int y, int w, int h)
{
    if (!s_panel || !src || w <= 0 || h <= 0) return;
    while (xSemaphoreTake(s_done, 0) == pdTRUE) { }
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h, src);
    if (err != ESP_OK) { ESP_LOGE(TAG, "draw_bitmap: %s", esp_err_to_name(err)); return; }
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(1000)) != pdTRUE)
        ESP_LOGE(TAG, "tile at y=%d never completed", y);
}

esp_err_t a3d_panel_init(void)
{
    s_bus_mutex = xSemaphoreCreateMutex();
    s_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_done, ESP_ERR_NO_MEM, TAG, "sem");

    // Field by field, NOT through the vendor's *_PANEL_BUS_QSPI_CONFIG() macro.
    // `data0_io_num` is a union alias for `mosi_io_num`, which is declared
    // before `sclk_io_num`, so a macro written in a perfectly sensible order is
    // still out of designator order for the struct and C++ rejects it. This
    // file is C, but the same shape has cost this tree time twice.
    spi_bus_config_t bus = {0};
    bus.sclk_io_num  = PIN_SCLK;
    bus.data0_io_num = PIN_D0;
    bus.data1_io_num = PIN_D1;
    bus.data2_io_num = PIN_D2;
    bus.data3_io_num = PIN_D3;
    bus.max_transfer_sz = PANEL_W * MAX_TILE_ROWS * 2;
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI_HOST_ID, &bus, SPI_DMA_CH_AUTO), TAG, "spi");

    esp_lcd_panel_io_spi_config_t io_cfg = {0};
    io_cfg.cs_gpio_num = PIN_CS;
    io_cfg.dc_gpio_num = -1;
    io_cfg.spi_mode = 0;
    io_cfg.pclk_hz = PCLK_HZ;
    io_cfg.trans_queue_depth = 10;
    io_cfg.on_color_trans_done = on_color_done;
    io_cfg.user_ctx = s_done;
    io_cfg.lcd_cmd_bits = 32;
    io_cfg.lcd_param_bits = 8;
    io_cfg.flags.quad_mode = true;
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI_HOST_ID,
                                                 &io_cfg, &io), TAG, "panel io");

    // init_cmds left NULL: the component falls back to its own default table.
    // If your board came with a vendor init sequence, put it here - an AMOLED
    // that lights up but shows the wrong gamma or the wrong window is usually
    // this and not the renderer.
    @VENDOR@ vendor = {0};
    vendor.init_cmds = NULL;
    vendor.init_cmds_size = 0;
    vendor.flags.use_qspi_interface = 1;

    esp_lcd_panel_dev_config_t dev = {0};
    dev.reset_gpio_num = PIN_RST;
    dev.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    dev.bits_per_pixel = 16;
    dev.vendor_config = &vendor;
    ESP_RETURN_ON_ERROR(@CREATE@(io, &dev, &s_panel), TAG, "panel");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp on");

    a3d_panel_fill(0x0000);
    ESP_RETURN_ON_ERROR(touch_init(), TAG, "touch");
    ESP_LOGI(TAG, "@LABEL@ %dx%d at %d MHz", PANEL_W, PANEL_H, PCLK_HZ / 1000000);
    return ESP_OK;
}
'''

_DSI_BODY = '''
// --- MIPI-DSI --------------------------------------------------------------
//
// Commands go over DBI, pixels over DPI. The panel keeps its own framebuffer
// in PSRAM and draw_bitmap copies into it, so a tile here is a memory write
// rather than a bus transfer - which is why there is no completion semaphore.
#define DSI_LANES        @LANES@
#define DSI_LANE_MBPS    @LANE_MBPS@
#define DSI_DPI_MHZ      @DPI_MHZ@
#define DSI_PHY_LDO_CHAN 3
#define DSI_PHY_LDO_MV   2500
#define PIN_RST          @RST@

static esp_lcd_panel_handle_t s_panel;

void a3d_panel_send(const uint16_t *src, int x, int y, int w, int h)
{
    if (!s_panel || !src || w <= 0 || h <= 0) return;
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h, src);
    if (err != ESP_OK) ESP_LOGE(TAG, "draw_bitmap: %s", esp_err_to_name(err));
}

esp_err_t a3d_panel_init(void)
{
    s_bus_mutex = xSemaphoreCreateMutex();

    // The DSI PHY has no power until this LDO is up. Without it the BUS
    // creation fails rather than the panel merely staying dark, which is a
    // more useful error than it sounds.
    static esp_ldo_channel_handle_t phy_pwr;
    if (!phy_pwr) {
        esp_ldo_channel_config_t ldo = { .chan_id = DSI_PHY_LDO_CHAN,
                                         .voltage_mv = DSI_PHY_LDO_MV };
        ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo, &phy_pwr), TAG, "dphy ldo");
    }

    esp_lcd_dsi_bus_handle_t dsi = NULL;
    esp_lcd_dsi_bus_config_t bus = {
        .bus_id = 0,
        .num_data_lanes = DSI_LANES,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = DSI_LANE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus, &dsi), TAG, "dsi bus");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_dbi_io_config_t dbi = { .virtual_channel = 0,
                                    .lcd_cmd_bits = 8, .lcd_param_bits = 8 };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi, &dbi, &io), TAG, "dbi io");

    // Written out rather than taken from the vendor's *_DPI_CONFIG() macro.
    // That macro sets `.pixel_format` and `.flags.use_dma2d`, and ESP-IDF 6.0
    // deleted both, so expanding it fails to COMPILE on 6.0 even though the
    // component itself is fine. On 5.5 this is identical, not merely
    // equivalent: in_color_format overrides the value pixel_format computes,
    // and both arrive at LCD_COLOR_FMT_RGB565.
    esp_lcd_dpi_panel_config_t dpi = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = DSI_DPI_MHZ,
        .virtual_channel = 0,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = PANEL_W,
            .v_size = PANEL_H,
            .hsync_back_porch = @HBP@,
            .hsync_pulse_width = @HPW@,
            .hsync_front_porch = @HFP@,
            .vsync_back_porch = @VBP@,
            .vsync_pulse_width = @VPW@,
            .vsync_front_porch = @VFP@,
        },
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
        .flags.use_dma2d = true,
#endif
    };

    @VENDOR@ vendor = {
        .mipi_config = { .dsi_bus = dsi, .dpi_config = &dpi, .lane_num = DSI_LANES },
    };
    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(@CREATE@(io, &dev, &s_panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    // What flags.use_dma2d did before 6.0, and it is not cosmetic: without it
    // draw_bitmap copies every tile with a CPU memcpy on the render core.
    // Logged rather than fatal - 6.0 still fires on_color_trans_done on the
    // CPU-copy path, so a board that refuses this runs slowly instead of
    // hanging.
    if (esp_lcd_dpi_panel_enable_dma2d(s_panel) != ESP_OK)
        ESP_LOGW(TAG, "DMA2D unavailable; tiles will be copied by the CPU");
#endif

    a3d_panel_fill(0x0000);
    ESP_RETURN_ON_ERROR(touch_init(), TAG, "touch");
    ESP_LOGI(TAG, "@LABEL@ %dx%d, %d lanes at %d Mbps",
             PANEL_W, PANEL_H, DSI_LANES, DSI_LANE_MBPS);
    return ESP_OK;
}
'''

_FAMILY_INCLUDES = {
    "offscreen": "",
    "spi": '#include "driver/gpio.h"\n#include "driver/spi_master.h"\n',
    "qspi": '#include "driver/gpio.h"\n#include "driver/spi_master.h"\n',
    "dsi": ('#include "esp_lcd_mipi_dsi.h"\n'
            '#include "esp_ldo_regulator.h"\n'
            '#include "esp_idf_version.h"\n'),
}

_FAMILY_BODY = {
    "offscreen": _OFFSCREEN_BODY,
    "spi": _SPI_BODY,
    "qspi": _QSPI_BODY,
    "dsi": _DSI_BODY,
}


def panel_c(panel: str, touch: str, target: str, width: int, height: int,
            pclk_mhz: int = 80) -> str:
    """The whole of panel.c for one choice."""
    p = PANELS[panel]
    t = TOUCH[touch]
    fam = p["family"]
    pins = dict(PINS[target])

    includes = _FAMILY_INCLUDES[fam]
    if p.get("header"):
        includes += '#include "%s"\n' % p["header"]
    if t.get("header"):
        includes += ('#include "driver/i2c_master.h"\n'
                     '#include "esp_lcd_touch.h"\n'
                     '#include "%s"\n' % t["header"])

    out = _fill(_COMMON_TOP, LABEL=p["label"], SOURCE=p["source"],
                EXTRA_INCLUDES=includes, W=width, H=height)
    out += _fill(_COMMON_MUTEX,
                 SWAP="true" if p.get("swap") else "false",
                 ROUND="true" if p.get("round") else "false")

    if touch == "none":
        out += _TOUCH_NONE
    else:
        out += _fill(_TOUCH_I2C, TOUCH_LABEL=t["label"], TOUCH_SOURCE=t["source"],
                     TOUCH_ADDR=t["addr"], TOUCH_CREATE=t["create"],
                     TOUCH_CMD_BITS=t["cmd_bits"], TOUCH_PARAM_BITS=t["param_bits"],
                     SDA=pins["sda"], SCL=pins["scl"],
                     TP_INT=pins["tp_int"], TP_RST=pins["tp_rst"])

    if fam != "offscreen":
        out += _COMMON_FILL
    else:
        out += ("\nvoid a3d_panel_fill(uint16_t colour) { (void)colour; }\n")

    body = _FAMILY_BODY[fam]
    kw = dict(LABEL=p["label"], CREATE=p.get("create", ""),
              PCLK_MHZ=pclk_mhz, VENDOR=p.get("vendor", ""))
    kw.update({k.upper(): v for k, v in pins.items()})
    if fam == "spi":
        kw["ENDIAN_FIELD"] = (
            "        // THE LINE THAT REMOVES A BYTE SWAP FROM EVERY FRAME.\n"
            "        // a3d's framebuffer is native-endian RGB565; this panel\n"
            "        // defaults to big endian, and every port that does not set\n"
            "        // this swaps every halfword of every frame in software.\n"
            "        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,\n"
            if p.get("little_endian") else
            "        // This controller has no endian bit, so a3d swaps in\n"
            "        // software - see a3d_panel_swap_bytes() above.\n")
        kw["INVERT_CALL"] = (
            "    // Many modules of this type are wired for inverted colour.\n"
            "    // If every frame comes out as its own negative, flip this.\n"
            "    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, \"invert\");\n"
            if p.get("invert") else "")
    if fam == "dsi":
        hbp, hpw, hfp, vbp, vpw, vfp = p["timing"]
        kw.update(LANES=p["lanes"], LANE_MBPS=p["lane_mbps"], DPI_MHZ=p["dpi_mhz"],
                  HBP=hbp, HPW=hpw, HFP=hfp, VBP=vbp, VPW=vpw, VFP=vfp)
    out += _fill(body, **kw)
    return out


def panel_h(panel: str) -> str:
    return _fill(PANEL_H, LABEL=PANELS[panel]["label"])


def components(panel: str, touch: str) -> dict:
    """Everything idf_component.yml must fetch for this pair."""
    d = dict(PANELS[panel]["components"])
    d.update(TOUCH[touch]["components"])
    # esp_lcd_touch is a dependency of each driver, but naming it makes the
    # manifest readable and pins one version rather than whichever the first
    # driver happened to pull.
    if touch != "none":
        d["espressif/esp_lcd_touch"] = "^1.1.0"
    return d


def sdkconfig_extra(panel: str, target: str) -> list:
    """Config lines this bus needs that a default project does not have."""
    p = PANELS[panel]
    if p["family"] == "dsi":
        return [
            "",
            "# The DSI panel keeps its framebuffer in PSRAM, so PSRAM is on and",
            "# runs at 200 MHz.",
            "#",
            "# THE TWO CACHE LINES ARE A PERFORMANCE CHOICE, NOT A REQUIREMENT,",
            "# and they are worth keeping. IDF defaults the P4 to a 128 KB L2",
            "# with 64-byte lines; these double both. Nothing in esp_psram or",
            "# esp_lcd depends on them - a build in this tree runs 200 MHz PSRAM",
            "# on the defaults - so this was measured rather than assumed:",
            "# reverting to the IDF default on one board, one variable, made",
            "# every row of the a3d benchmark SLOWER, frame time by 1.0% to",
            "# 28.5% (mean 11.7%) against a 0.19% run-to-run spread.",
            "#",
            "# They cost internal SRAM you might want back on a small model, so",
            "# they are worth re-measuring for your scene - but the default is",
            "# not the fast choice on this part.",
            "CONFIG_SPIRAM_SPEED_200M=y",
            "CONFIG_CACHE_L2_CACHE_256KB=y",
            "CONFIG_CACHE_L2_CACHE_LINE_128B=y",
        ]
    if p["family"] == "qspi":
        return [
            "",
            "# A QSPI AMOLED at 466x466 is 434,312 bytes a frame on the wire.",
            "# Nothing here needs raising for a3d, which sends a tile at a time.",
        ]
    return []
