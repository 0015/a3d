// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file a3d_display_esp_lcd.h
 * @brief An a3d::Display from the esp_lcd panel you already have working.
 *
 * WHY THIS EXISTS RATHER THAN A CATALOGUE OF PANELS
 *
 *   `tools/a3d_panel.py` generates the bring-up for buses this tree has driven
 *   real glass with, and it is deliberately small: a plausible default driving
 *   the wrong GPIO looks exactly like a dead panel. But that leaves everybody
 *   whose board is not in it, which is almost everybody.
 *
 *   The way out is not a bigger catalogue. Every esp_lcd driver - the thirty-odd
 *   in the component registry, every vendor fork, every BSP, and the one for the
 *   panel that ships next year - ends at an `esp_lcd_panel_handle_t`. Anyone
 *   with a working display has one, or has the twenty lines that make one. So
 *   a3d needs ONE adapter, not thirty entries, and the pins stay where they
 *   belong: in the code that already works on that person's desk.
 *
 * WHAT IT ACTUALLY BUYS YOU, WHICH IS THE WAIT
 *
 *   The per-panel half is the easy half. The half that is the same for every
 *   panel is the half that is subtly wrong when hand-written, because
 *   `esp_lcd_panel_draw_bitmap()` DOES NOT FINISH WITH YOUR BUFFER BEFORE IT
 *   RETURNS on most buses:
 *
 *     - SPI / QSPI / I80: the colour transfer is handed to
 *       `spi_device_queue_trans()` and the call returns with the DMA still
 *       reading `src`.
 *     - MIPI-DSI with DMA2D: the copy into the panel's frame buffer is started
 *       asynchronously, and the NEXT draw_bitmap is rejected outright with
 *       `ESP_ERR_INVALID_STATE` ("previous draw operation is not finished").
 *     - RGB parallel, and the DSI CPU-copy path: the copy IS complete on
 *       return. These are the only cases where no wait is needed.
 *
 *   `a3d::Display::sendTile` must not return until `src` may be reused: the
 *   viewer owns one tile buffer per worker and hands it to the next tile
 *   immediately. A driver that queues and returns therefore sends half of one
 *   tile and half of the next, and it does not look like a buffer problem - it
 *   looks like a rasterizer bug, which is where the time goes.
 *
 *   So this registers the bus's own completion callback, drains any token left
 *   over from an earlier frame rather than trusting a count, waits with a
 *   timeout, and owns the mutex two workers need to take turns on one bus.
 *
 * HOW TO USE IT
 *
 *   Bring your panel up however you already do - vendor example, BSP, your own
 *   code - and hand over the two handles it produced:
 *
 *       #include "backends/esp_lcd/a3d_display_esp_lcd.h"
 *
 *       a3d::EspLcdDisplay glass;
 *       a3d::EspLcdConfig  cfg;
 *       cfg.panel  = my_panel;      // esp_lcd_panel_handle_t
 *       cfg.io     = my_panel_io;   // esp_lcd_panel_io_handle_t, or null on DSI
 *       cfg.width  = 466;
 *       cfg.height = 466;
 *       ESP_ERROR_CHECK(glass.begin(cfg));
 *
 *       viewer.begin(glass.display());
 *
 *   `EspLcdWait::Auto` resolves to the IO callback when you pass an IO handle,
 *   which covers SPI, QSPI and I80, and otherwise REFUSES rather than guessing.
 *   Both remaining modes have to be named out loud - `DpiCallback` for MIPI-DSI,
 *   `Synchronous` for RGB parallel - and that is deliberate in both directions:
 *   an RGB panel genuinely needs no wait but so does a mistake, and the DSI
 *   registration cannot be offered speculatively because esp_lcd does not check
 *   that the handle it is given is a DPI panel. Naming the mode is your
 *   assertion that you looked.
 *
 * @see a3d::Display in a3d/a3d_display.h, which is the contract this fills.
 */
#ifndef A3D_DISPLAY_ESP_LCD_H_
#define A3D_DISPLAY_ESP_LCD_H_

#if !defined(ESP_PLATFORM)
#error "a3d_display_esp_lcd.h is ESP-IDF only; it needs esp_lcd."
#endif

#include <stdint.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "a3d/a3d_display.h"

// MIPI-DSI is a P4 thing and the header is not on every target's include path.
// Probing for it rather than testing SOC_MIPI_DSI_SUPPORTED keeps this header
// compiling on a part that has no DSI at all.
#if defined(__has_include)
#  if __has_include("esp_lcd_mipi_dsi.h")
#    include "esp_lcd_mipi_dsi.h"
#    define A3D_ESP_LCD_HAS_DSI 1
#  endif
#endif
#if !defined(A3D_ESP_LCD_HAS_DSI)
#  define A3D_ESP_LCD_HAS_DSI 0
#endif

// Touch is optional and lives in a managed component. If `esp_lcd_touch` is not
// in your REQUIRES its header is not on the path, this compiles without it, and
// `EspLcdConfig::touch` simply does not exist. Define A3D_ESP_LCD_NO_TOUCH to
// leave it out even when the component IS present.
#if !defined(A3D_ESP_LCD_NO_TOUCH) && defined(__has_include)
#  if __has_include("esp_lcd_touch.h")
#    include "esp_lcd_touch.h"
#    define A3D_ESP_LCD_HAS_TOUCH 1
#  endif
#endif
#if !defined(A3D_ESP_LCD_HAS_TOUCH)
#  define A3D_ESP_LCD_HAS_TOUCH 0
#endif

namespace a3d {

/** How this bus says it has finished reading the buffer you handed it. */
enum class EspLcdWait
    {
    /**
     * Decide from the handles given: an `io` handle means IoCallback, a DSI
     * panel with no `io` means DpiCallback, and neither means `begin()` fails
     * rather than guessing that no wait is needed.
     */
    Auto,
    /** SPI, QSPI and I80: `esp_lcd_panel_io_register_event_callbacks`. */
    IoCallback,
    /** MIPI-DSI: `esp_lcd_dpi_panel_register_event_callbacks`. */
    DpiCallback,
    /**
     * `draw_bitmap` has finished with the buffer when it returns, so there is
     * nothing to wait for. True of RGB parallel panels and of a DSI panel built
     * WITHOUT DMA2D, where the copy is a CPU memcpy. Say it deliberately: it is
     * also what a missed callback looks like.
     */
    Synchronous,
    };

/** The handles and facts a3d needs. Everything here is yours already. */
struct EspLcdConfig
    {
    /** Required. Whatever your driver or BSP returned. */
    esp_lcd_panel_handle_t panel = nullptr;

    /**
     * The panel IO the panel was built on. Required for IoCallback, which is
     * SPI, QSPI and I80. A DSI panel does not have one in the sense that
     * matters here - its pixels do not go through the DBI IO - so leave it null.
     */
    esp_lcd_panel_io_handle_t io = nullptr;

    /** Panel size in pixels. a3d cannot ask the driver, so it asks you. */
    int width  = 0;
    int height = 0;

    EspLcdWait wait = EspLcdWait::Auto;

    /**
     * True when the panel reads RGB565 big-endian. Costs a pass over every tile
     * - 11.6 ms a frame at 480x480 on a measured board - so look for the panel
     * register that removes it first. An ST7789 has one (RAMCTRL bit 3, which
     * esp_lcd exposes as `data_endian = LCD_RGB_DATA_ENDIAN_LITTLE`); a CO5300
     * does not.
     *
     * How to recognise getting this wrong: a near-black background comes out
     * PINK, not "slightly off". And do not trust a vendor BSP's declaration -
     * one in this tree says little-endian about a panel that reads big-endian.
     */
    bool swapBytes = false;

    /** True for circular glass; the viewer puts its caption top centre. */
    bool round = false;

    /**
     * How long to wait for one tile before deciding the bus has stopped. A
     * 240x40 tile at 80 MHz is about 1.9 ms; the default is four hundred times
     * that, so it fires only for a bus that is never coming back.
     */
    uint32_t timeoutMs = 1000;

    /** Free-form, printed in logs. */
    const char* name = "esp_lcd";

#if A3D_ESP_LCD_HAS_TOUCH
    /** Optional. When set, Display::touchRead reads this controller. */
    esp_lcd_touch_handle_t touch = nullptr;
#endif
    };

namespace detail {

/**
 * Both completion callbacks run from an ISR and both must live in IRAM - the
 * DPI one is CHECKED for it (`esp_ptr_in_iram`) and registration fails
 * otherwise, which is a better error than the cache miss it would otherwise be.
 */
inline bool IRAM_ATTR espLcdGive(void* ctx)
    {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(static_cast<SemaphoreHandle_t>(ctx), &woken);
    return woken == pdTRUE;
    }

inline bool IRAM_ATTR espLcdIoDone(esp_lcd_panel_io_handle_t,
                                   esp_lcd_panel_io_event_data_t*, void* ctx)
    { return espLcdGive(ctx); }

#if A3D_ESP_LCD_HAS_DSI
inline bool IRAM_ATTR espLcdDpiDone(esp_lcd_panel_handle_t,
                                    esp_lcd_dpi_panel_event_data_t*, void* ctx)
    { return espLcdGive(ctx); }
#endif

} // namespace detail

/**
 * Wraps an esp_lcd panel as an a3d::Display, and owns the two things that make
 * the wrapping correct rather than merely present: the completion semaphore and
 * the bus mutex.
 *
 * Hold it for as long as the viewer does. It is not copyable, because the
 * callbacks it registers carry its address.
 */
class EspLcdDisplay
    {
public:
    EspLcdDisplay() = default;
    ~EspLcdDisplay() { end(); }

    EspLcdDisplay(const EspLcdDisplay&)            = delete;
    EspLcdDisplay& operator=(const EspLcdDisplay&) = delete;

    /**
     * Register the completion callback and build the Display.
     *
     * Returns ESP_ERR_INVALID_ARG for a missing handle or size,
     * ESP_ERR_NOT_SUPPORTED when Auto can find no completion signal - see
     * EspLcdWait::Synchronous for what to do about that - and whatever the
     * registration itself returned otherwise. `error()` says which in words.
     */
    esp_err_t begin(const EspLcdConfig& cfg)
        {
        end();
        _cfg = cfg;
        _err = nullptr;

        if (!_cfg.panel)                           return _fail(ESP_ERR_INVALID_ARG, "no panel handle");
        if (_cfg.width <= 0 || _cfg.height <= 0)   return _fail(ESP_ERR_INVALID_ARG, "panel size must be positive");

        EspLcdWait mode = _cfg.wait;
        if (mode == EspLcdWait::Auto)
            {
            // Auto resolves to IoCallback or to nothing, and never GUESSES the
            // DPI one. esp_lcd_dpi_panel_register_event_callbacks() does not
            // check that the handle it was given is a DPI panel - it is a bare
            // __containerof - so calling it on an RGB or SPI panel writes
            // through a pointer into the wrong struct. That is memory
            // corruption with no error and no crash at the call, which is a
            // worse outcome than making you name the mode.
            if (_cfg.io) mode = EspLcdWait::IoCallback;
            else return _fail(ESP_ERR_NOT_SUPPORTED,
                              "no panel IO handle, so there is no completion "
                              "callback to take: pass the IO handle, or name the "
                              "mode - EspLcdWait::DpiCallback for MIPI-DSI, "
                              "EspLcdWait::Synchronous for RGB parallel and for a "
                              "DSI panel built without DMA2D");
            }

        // The mutex exists even for one worker. A build that later raises the
        // worker count would otherwise get a null mutex and a torn frame rather
        // than a compile error.
        _mutex = xSemaphoreCreateMutex();
        if (!_mutex) return _fail(ESP_ERR_NO_MEM, "no memory for the bus mutex");

        if (mode != EspLcdWait::Synchronous)
            {
            _sem = xSemaphoreCreateBinary();
            if (!_sem) return _fail(ESP_ERR_NO_MEM, "no memory for the completion semaphore");
            }

        if (mode == EspLcdWait::IoCallback)
            {
            if (!_cfg.io) return _fail(ESP_ERR_INVALID_ARG,
                                       "EspLcdWait::IoCallback needs the panel IO handle");
            // Registering here OVERWRITES any callback already on this IO, and
            // esp_lcd logs a warning saying so. That matters if you also drive
            // this panel from LVGL: a3d and LVGL cannot both own the completion.
            esp_lcd_panel_io_callbacks_t cbs = {};
            cbs.on_color_trans_done = detail::espLcdIoDone;
            const esp_err_t e = esp_lcd_panel_io_register_event_callbacks(_cfg.io, &cbs, _sem);
            if (e != ESP_OK) return _fail(e, "esp_lcd_panel_io_register_event_callbacks failed");
            }
#if A3D_ESP_LCD_HAS_DSI
        else if (mode == EspLcdWait::DpiCallback)
            {
            esp_lcd_dpi_panel_event_callbacks_t cbs = {};
            cbs.on_color_trans_done = detail::espLcdDpiDone;
            const esp_err_t e = esp_lcd_dpi_panel_register_event_callbacks(_cfg.panel, &cbs, _sem);
            if (e != ESP_OK) return _fail(e,
                "esp_lcd_dpi_panel_register_event_callbacks failed - if this panel "
                "is not a DSI one, name the wait mode rather than leaving it Auto");
            }
#endif

        _mode = mode;
        _disp = Display{};
        _disp.width     = _cfg.width;
        _disp.height    = _cfg.height;
        _disp.name      = _cfg.name;
        _disp.user      = this;
        _disp.sendTile  = &_sendThunk;
        _disp.fill      = &_fillThunk;
        _disp.lockBus   = &_lockThunk;
        _disp.unlockBus = &_unlockThunk;
        _disp.swapBytes = _cfg.swapBytes;
        _disp.round     = _cfg.round;
#if A3D_ESP_LCD_HAS_TOUCH
        if (_cfg.touch) _disp.touchRead = &_touchThunk;
#endif

        ESP_LOGI("a3d_esp_lcd", "%s %dx%d, %s%s", _cfg.name, _cfg.width, _cfg.height,
                 _modeName(_mode), _cfg.swapBytes ? ", byte-swapped" : "");
        return ESP_OK;
        }

    /** Release the semaphore, the mutex and the fill band. Safe to call twice. */
    void end()
        {
        if (_band)  { heap_caps_free(_band); _band = nullptr; }
        if (_sem)   { vSemaphoreDelete(_sem);   _sem = nullptr; }
        if (_mutex) { vSemaphoreDelete(_mutex); _mutex = nullptr; }
        _disp = Display{};
        }

    /** The struct to hand to a3d::Viewer, or to any code taking an a3d::Display. */
    Display&       display()       { return _disp; }
    const Display& display() const { return _disp; }

    /** Why begin() refused, or null. Never cleared by a later success. */
    const char* error() const { return _err; }

    bool valid() const { return _disp.valid(); }

private:
    static const char* _modeName(EspLcdWait m)
        {
        switch (m)
            {
            case EspLcdWait::IoCallback:  return "waiting on the panel IO callback";
            case EspLcdWait::DpiCallback: return "waiting on the DPI callback";
            case EspLcdWait::Synchronous: return "no wait (caller says draw_bitmap is synchronous)";
            default:                      return "unresolved";
            }
        }

    esp_err_t _fail(esp_err_t e, const char* why)
        {
        _err = why;
        ESP_LOGE("a3d_esp_lcd", "%s", why);
        end();
        return e;
        }

    void _send(const uint16_t* src, int x, int y, int w, int h)
        {
        if (!_cfg.panel || !src || w <= 0 || h <= 0) return;

        // Drain rather than trust a count. A token left over from an earlier
        // frame - a failed draw, a timeout that then completed - would satisfy
        // this tile's wait immediately and let the next tile overwrite a buffer
        // the DMA is still reading.
        if (_sem) while (xSemaphoreTake(_sem, 0) == pdTRUE) { }

        const esp_err_t e = esp_lcd_panel_draw_bitmap(_cfg.panel, x, y, x + w, y + h, src);
        if (e != ESP_OK)
            {
            ESP_LOGE("a3d_esp_lcd", "draw_bitmap at y=%d: %s", y, esp_err_to_name(e));
            return;
            }
        if (_sem && xSemaphoreTake(_sem, pdMS_TO_TICKS(_cfg.timeoutMs)) != pdTRUE)
            ESP_LOGE("a3d_esp_lcd", "tile at y=%d never completed", y);
        }

    /**
     * A band at a time out of one DMA-capable buffer rather than a full-screen
     * one: at 800x1280 a framebuffer is 2 MB, and the panel is holding the
     * image anyway.
     */
    void _fill(uint16_t colour)
        {
        const int rowsPerBand = 8;
        if (!_band)
            {
            _band = static_cast<uint16_t*>(
                heap_caps_malloc((size_t)_cfg.width * rowsPerBand * sizeof(uint16_t),
                                 MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
            if (!_band) { ESP_LOGE("a3d_esp_lcd", "no DMA memory for fill"); return; }
            }
        for (int i = 0; i < _cfg.width * rowsPerBand; i++) _band[i] = colour;
        for (int y = 0; y < _cfg.height; y += rowsPerBand)
            {
            int rows = rowsPerBand;
            if (y + rows > _cfg.height) rows = _cfg.height - y;
            // Through lock/unlock, because fill is a tile sender like any other
            // and may run while a worker is mid-frame.
            _lock();
            _send(_band, 0, y, _cfg.width, rows);
            _unlock();
            }
        }

    void _lock()   { if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY); }
    void _unlock() { if (_mutex) xSemaphoreGive(_mutex); }

    static EspLcdDisplay* _self(void* user) { return static_cast<EspLcdDisplay*>(user); }

    static void _sendThunk(void* user, const uint16_t* src, int x, int y, int w, int h)
        { _self(user)->_send(src, x, y, w, h); }
    static void _fillThunk(void* user, uint16_t colour)
        { _self(user)->_fill(colour); }
    static void _lockThunk(void* user)   { _self(user)->_lock(); }
    static void _unlockThunk(void* user) { _self(user)->_unlock(); }

#if A3D_ESP_LCD_HAS_TOUCH
    static bool _touchThunk(void* user, int* x, int* y)
        {
        esp_lcd_touch_handle_t tp = _self(user)->_cfg.touch;
        if (!tp) return false;
        esp_lcd_touch_read_data(tp);
        uint16_t px[1], py[1], strength[1];
        uint8_t count = 0;
        // esp_lcd_touch 1.2.0 deprecated this in favour of esp_lcd_touch_get_data()
        // and will remove it at 2.0.0. It is kept because get_data() does not
        // exist before 1.2.0 - checked against 1.1.2 - and a header-only adapter
        // cannot pin the version its host project resolved. Silenced locally so
        // it does not become noise in somebody else's build; it has to change
        // before esp_lcd_touch 2.0.0.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        const bool got = esp_lcd_touch_get_coordinates(tp, px, py, strength, &count, 1);
#pragma GCC diagnostic pop
        if (!got || count == 0) return false;
        if (x) *x = px[0];
        if (y) *y = py[0];
        return true;
        }
#endif

    EspLcdConfig      _cfg{};
    Display           _disp{};
    EspLcdWait        _mode  = EspLcdWait::Auto;
    SemaphoreHandle_t _sem   = nullptr;
    SemaphoreHandle_t _mutex = nullptr;
    uint16_t*         _band  = nullptr;
    const char*       _err   = nullptr;
    };

} // namespace a3d

#endif // A3D_DISPLAY_ESP_LCD_H_
