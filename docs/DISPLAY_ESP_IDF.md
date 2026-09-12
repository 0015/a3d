<!-- SPDX-FileCopyrightText: 2026 Eric Nam -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Getting a3d onto a panel - ESP-IDF

This is the step most people stop at, so it is written out in full. If you are
on Arduino, read [DISPLAY_ARDUINO.md](DISPLAY_ARDUINO.md) instead - the ideas
are the same and none of the code is.

---

## What a3d asks for, and what it does not

a3d has no panel driver and will never have one. Your board has a schematic and
a3d does not, so a component that tried to guess would be wrong about at least
one bus. What it needs is much narrower than a driver:

```cpp
a3d::Display panel;
panel.width    = 240;
panel.height   = 320;
panel.sendTile = &sendTile;      // move a rectangle of RGB565 to the glass
```

That is the whole required interface. `src/a3d/a3d_display.h` is a plain struct
of function pointers - it names no bus, no RTOS and no vendor header - and
everything else in it has a default:

| field | required | what it is for |
|---|---|---|
| `width`, `height` | **yes** | panel size in pixels |
| `sendTile` | **yes** | move one horizontal strip of RGB565 to the panel |
| `user` | no | passed back to every callback, so you need no globals |
| `fill` | no | paint the whole panel once at startup |
| `touchRead` | no | one contact, or false |
| `swapBytes` | no | true if the panel reads RGB565 **big**-endian |
| `round` | no | circular glass; moves a caption to the top centre |
| `lockBus` / `unlockBus` | only with 2+ workers | the bus is not re-entrant |
| `name` | no | printed in logs |

a3d cuts the screen into horizontal strips and calls `sendTile` once per strip:
`x` is always 0 and `w` is always the full width, because that is the shape a
panel's address window and its DMA both prefer.

### THE ONE RULE

**`sendTile` must not return until `src` may be reused.**

a3d owns one tile buffer per worker and hands it to the next tile immediately.
A driver that queues the pointer and returns will therefore send half of one
tile and half of the next - and it does not look like a buffer problem, it
looks like a rasterizer bug, which is where the time goes.

This matters on ESP-IDF because **`esp_lcd_panel_draw_bitmap()` does not finish
with your buffer before it returns on most buses.** Read out of IDF 5.5.4:

| bus | what `draw_bitmap` does | you must |
|---|---|---|
| SPI / QSPI / I80 | `spi_device_queue_trans()`, returns with the DMA live | wait for `on_color_trans_done` |
| MIPI-DSI **with DMA2D** | starts an async copy; the *next* call is refused with `ESP_ERR_INVALID_STATE` | wait for the DPI callback |
| MIPI-DSI without DMA2D | CPU `memcpy`, complete on return | nothing |
| RGB parallel | CPU `memcpy`, complete on return | nothing |

---

## Three routes, in order of how much work they are

| route | when | you write |
|---|---|---|
| 1. `a3d::EspLcdDisplay` | you already have a working `esp_lcd` panel, from a vendor example, a BSP or an LVGL port | ~10 lines |
| 2. `a3d_export.py --project --panel ...` | your bus is in the small catalogue below | nothing; it is generated |
| 3. by hand | anything else, or you want to see all of it | ~60 lines, below |

---

## Route 1 - you already have an `esp_lcd` panel

Every `esp_lcd` driver - the thirty-odd in the component registry, every vendor
fork, every BSP, and the one for the panel that ships next year - ends at an
`esp_lcd_panel_handle_t`. So a3d ships **one adapter**, not a catalogue, and
your pins stay in the code that already works on your desk.

```cpp
#include "backends/esp_lcd/a3d_display_esp_lcd.h"
#include "viewer/a3d_viewer.h"

static a3d::EspLcdDisplay glass;
static a3d::Viewer        viewer;

extern "C" void app_main(void)
    {
    // ... your existing bring-up, unchanged, producing my_panel and my_io ...

    a3d::EspLcdConfig cfg;
    cfg.panel  = my_panel;       // esp_lcd_panel_handle_t
    cfg.io     = my_panel_io;    // esp_lcd_panel_io_handle_t - null on MIPI-DSI
    cfg.width  = 240;
    cfg.height = 320;
    // cfg.wait      = a3d::EspLcdWait::DpiCallback;   // MIPI-DSI: say so
    // cfg.swapBytes = true;                           // big-endian panel
    // cfg.round     = true;                           // circular glass
    // cfg.touch     = my_touch;                       // esp_lcd_touch_handle_t
    ESP_ERROR_CHECK(glass.begin(cfg));

    viewer.begin(glass.display());
    }
```

What the adapter owns is the half that is the same for every panel and subtly
wrong when hand-written: it registers the bus's completion callback, **drains
any token left over from an earlier frame** rather than trusting a count, waits
with a timeout, and holds the mutex two workers need to take turns on one bus.

### Wait modes, and why `Auto` sometimes refuses

| mode | bus |
|---|---|
| `Auto` | resolves to `IoCallback` when you pass an `io` handle; otherwise **fails** rather than guessing |
| `IoCallback` | SPI, QSPI, I80 |
| `DpiCallback` | MIPI-DSI |
| `Synchronous` | RGB parallel, and MIPI-DSI built without DMA2D |

`Auto` will not reach for the DSI callback on its own, and that is deliberate:
`esp_lcd_dpi_panel_register_event_callbacks()` is a bare `__containerof` with
**no check that the handle it was given is a DPI panel**. Calling it on an SPI
or RGB panel writes through a pointer into the wrong struct - memory
corruption, no error, no crash at the call site. Naming the mode is your
assertion that you looked.

`Synchronous` has to be named out loud for the opposite reason: an RGB panel
genuinely needs no wait, but so does a missed callback.

**One caveat if you also run LVGL on this panel.** Registering the IO callback
overwrites any callback already on that IO, and `esp_lcd` logs a warning saying
so. a3d and LVGL cannot both own the completion; pick one owner of the panel.

---

## Route 2 - let the project be generated

```bash
python3 tools/a3d_export.py --list-panels
python3 tools/a3d_export.py model.glb --project myapp \
        --target esp32s3 --panel spi_st7789 --touch gt911
cd myapp && idf.py set-target esp32s3 && idf.py -p /dev/ttyUSB0 flash monitor
```

You get a buildable project: a partition sized for the model, the `sdkconfig`
your part needs, the `EMBED_FILES` symbol spelled the way the linker will spell
it, `main/panel.c` with the bring-up, and `main.cpp` that names no bus.

The catalogue is deliberately small. A bus is in it only because a project in
this tree has driven real glass with it:

| `--panel` | bus | native size | where the init came from |
|---|---|---|---|
| `spi_st7789` | SPI | 240x320 | hardware-verified in this tree |
| `spi_gc9a01` | SPI | 240x240 round | the component's README |
| `spi_ili9341` | SPI | 240x320 | the component's README |
| `qspi_sh8601` | QSPI AMOLED | 466x466 | hardware-verified in this tree |
| `dsi_jd9365` | MIPI-DSI, P4 only | 800x1280 | hardware-verified in this tree |
| `dsi_ek79007` | MIPI-DSI, P4 only | 1024x600 | the component's README |
| `esp_lcd_custom` | yours | `--viewport` | you fill in `panel_bring_up()` |
| `offscreen` | none | 240x320 | renders to memory and logs the frame rate |

`--touch` takes `gt911`, `cst816s`, `ft5x06` or `none`.

**The pins are a real board's and every generated file says whose.** Nothing in
a generator can know your wiring, and a plausible default driving the wrong
GPIO looks exactly like a dead panel. Open `main/panel.c` and change the
`#define` block at the top before you flash.

`main/panel.c` is C on purpose: vendor `esp_lcd` config macros use C designated
initialisers in an order C++ rejects. `main.cpp` reaches it through one
`extern "C"` header.

---

## Route 3 - writing it by hand

This is a complete, working SPI panel, adapted from
[examples/ESP-IDF/Waveshare_ESP32-S3-Touch-LCD-2](../examples/ESP-IDF/Waveshare_ESP32-S3-Touch-LCD-2),
which has driven real glass. Every part of it is here because leaving it out
breaks something.

### 1. The completion callback

```cpp
static SemaphoreHandle_t s_done;

// IRAM_ATTR: it runs from an ISR.
static bool IRAM_ATTR onColorDone(esp_lcd_panel_io_handle_t,
                                  esp_lcd_panel_io_event_data_t*, void* ctx)
    {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &woken);
    return woken == pdTRUE;
    }
```

### 2. Bus, IO and panel

```cpp
static esp_lcd_panel_handle_t s_panel;

esp_err_t panelBegin(void)
    {
    const spi_bus_config_t bus = {
        .mosi_io_num     = PIN_MOSI,
        .miso_io_num     = PIN_MISO,
        .sclk_io_num     = PIN_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        // The largest single tile this bus will accept. Sized in ROWS so the
        // relationship to tileHeight is visible: 64 is well past the 40 the
        // app uses, and the descriptor pool costs a few hundred bytes.
        .max_transfer_sz = LCD_WIDTH * 64 * 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi");

    s_done = xSemaphoreCreateBinary();

    // Zero-initialised and then filled field by field. Do NOT use the vendor's
    // config macro from C++ - see "ESP-IDF 6.0" below for both reasons.
    esp_lcd_panel_io_spi_config_t io = {};
    io.cs_gpio_num         = (gpio_num_t)PIN_CS;    // the cast is 6.0; correct on 5.x too
    io.dc_gpio_num         = (gpio_num_t)PIN_DC;
    io.spi_mode            = 0;
    io.pclk_hz             = 80 * 1000 * 1000;
    io.trans_queue_depth   = 10;
    io.on_color_trans_done = onColorDone;           // <- THE RULE lives here
    io.user_ctx            = s_done;
    io.lcd_cmd_bits        = 8;
    io.lcd_param_bits      = 8;
    esp_lcd_panel_io_handle_t ioh = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &ioh), TAG, "io");

    esp_lcd_panel_dev_config_t dev = {};
    dev.reset_gpio_num = (gpio_num_t)PIN_RST;       // -1 if reset is tied high
    dev.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB; // BGR on some boards
    dev.data_endian    = LCD_RGB_DATA_ENDIAN_LITTLE;// see "Byte order" below
    dev.bits_per_pixel = 16;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(ioh, &dev, &s_panel), TAG, "st7789");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    esp_lcd_panel_mirror(s_panel, false, false);
    esp_lcd_panel_swap_xy(s_panel, false);
    esp_lcd_panel_invert_color(s_panel, true);      // this board is wired inverted
    esp_lcd_panel_disp_on_off(s_panel, true);
    return ESP_OK;
    }
```

### 3. `sendTile`, which is the part that has to be right

```cpp
static void sendTile(void*, const uint16_t* src, int x, int y, int w, int h)
    {
    // Drain rather than trust a count. A token left over from an earlier frame
    // - a failed draw, a timeout that then completed - would satisfy THIS
    // tile's wait immediately and let the next tile overwrite a buffer the DMA
    // is still reading.
    while (xSemaphoreTake(s_done, 0) == pdTRUE) { }

    // Note the end coordinates: draw_bitmap takes x2/y2 EXCLUSIVE.
    if (esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h, src) != ESP_OK)
        return;

    // 240x40 at 80 MHz is about 1.9 ms. A second is four hundred times that
    // and can only mean the bus has stopped.
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(1000)) != pdTRUE)
        ESP_LOGE(TAG, "tile at y=%d never completed", y);
    }
```

### 4. Hand it to a3d

```cpp
static a3d::Display panel;
panel.width    = LCD_WIDTH;
panel.height   = LCD_HEIGHT;
panel.name     = "st7789_spi";
panel.sendTile = &sendTile;

static a3d::FreeRtosExecutor exec(2);   // BEFORE begin(): it sizes the buffers
static a3d::Viewer viewer;
viewer.setExecutor(exec);
if (!viewer.begin(panel)) ESP_LOGE(TAG, "%s", viewer.error());
```

### The MIPI-DSI difference

Two things change on DSI, and both have bitten this tree:

```cpp
// 1. The DPI callback, not the IO one. There is no IO handle that carries pixels.
esp_lcd_dpi_panel_event_callbacks_t cbs = {};
cbs.on_color_trans_done = onDpiDone;        // also IRAM_ATTR, and it is CHECKED:
                                            // registration fails if it is not
esp_lcd_dpi_panel_register_event_callbacks(panel, &cbs, s_done);

// 2. On ESP-IDF 6.0, flags.use_dma2d is gone and this call replaces it.
//    Skip it and draw_bitmap copies every tile with the CPU - 2 MB a frame on
//    an 800x1280 panel. It does not hang: 6.0 still fires on_color_trans_done
//    on the CPU-copy path.
esp_lcd_dpi_panel_enable_dma2d(panel);
```

---

## Byte order - the pink-screen problem

a3d's framebuffer is native-endian RGB565. Some panels read big-endian. There
are two ways to reconcile that and they are not close in cost.

**Look for the panel register first.** An ST7789 can be told to read
little-endian - it is RAMCTRL bit 3, exposed as
`dev.data_endian = LCD_RGB_DATA_ENDIAN_LITTLE` - and then a3d's output goes to
the wire untouched. Every LVGL port for that board byte-swaps 76,800 halfwords
a frame in software for want of that one line.

**If the panel has no such bit** (a CO5300 and an SH8601 do not), set
`panel.swapBytes = true` and a3d swaps the tile it has just rasterized, in
place, while it is still in cache. That is a real cost - **11.6 ms a frame at
480x480 on a measured board** - which is why it is worth a look at the
datasheet first.

**How to recognise getting it wrong: a near-black background comes out PINK.**
a3d's 0x1082 byte-swapped is 0x8210 - red 52%, green 25%, blue 52%, equal red
and blue with the green pulled down, a desaturated magenta. It looks nothing
like "the colours are slightly off". Do that arithmetic on your background
colour before touching anything else.

**Do not trust a vendor BSP's declaration.** One BSP in this tree sets
`BSP_LCD_BIGENDIAN 0` about a panel that reads big-endian. The BSP is
presumably right about what its own LVGL adapter does, which is not the same
question.

---

## Two workers share one bus

A panel bus is not re-entrant. With more than one worker drawing tiles, two
`sendTile` calls have to take turns or they interleave halfway through an
address window. The symptom is torn bands, not a crash.

```cpp
static SemaphoreHandle_t s_busLock;                 // xSemaphoreCreateMutex()

static void lockBus(void*)   { xSemaphoreTake(s_busLock, portMAX_DELAY); }
static void unlockBus(void*) { xSemaphoreGive(s_busLock); }

panel.lockBus   = &lockBus;
panel.unlockBus = &unlockBus;
```

`a3d::EspLcdDisplay` does this for you. Create the mutex even on one worker:
raising the worker count later would otherwise give you a torn frame rather
than a compile error.

**Single core is a supported target, not a fallback.** Leave the executor out
entirely and a3d renders every tile inline, allocates one set of buffers
instead of two, and needs no mutex at all.

---

## Memory - where the tile buffers must live

Each slot costs `width * tileHeight * 2` bytes for pixels, and the same again
for depth. There is one slot per worker.

| panel | 40 rows, 2 workers, with z-buffer |
|---|---|
| 240x320 | 4 x 19 KB = 77 KB |
| 466x466 | 4 x 37 KB = 149 KB |
| 800x1280 | 4 x 64 KB = 256 KB |

Two things about that memory:

1. **It must be DMA-capable and it should be internal.** `sendTile` hands it
   straight to the peripheral. A tile in PSRAM is written for every pixel of
   every frame and read by the DMA, which is the worst place for it - one
   measurement in this tree took a frame rate to 1 fps by moving a framebuffer
   to PSRAM.
2. **On a part with PSRAM, ESP-IDF may put it there without telling you.** Any
   allocation above `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` (16 KB by default)
   goes to PSRAM. A 19 KB tile buffer is above that line.

```cpp
static void* fastAlloc(size_t n)
    { return heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT); }
static void  fastFree(void* p) { heap_caps_free(p); }

static void* bigAlloc(size_t n)  { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
static void  bigFree(void* p)    { heap_caps_free(p); }

viewer.setAllocator(fastAlloc, fastFree);           // tiles: internal + DMA
viewer.setLargeAllocator(bigAlloc, bigFree);        // geometry: PSRAM is right
```

Both must be set **before** `begin()`. If you are short of internal RAM, lower
`ViewerConfig::tileHeight` before you move a tile buffer to PSRAM - 24 rows
costs 40% less than 40 and changes nothing else.

**A free-memory delta is not an allocation size.** Measuring
`heap_caps_get_free_size()` before and after `begin()` lied for a whole session
in this tree, because a previous model's freed blocks were reused and because
the big arrays had gone to PSRAM. Print both heaps.

---

## Size, rotation, offsets

**a3d renders at the panel's size and does not rotate.** Orientation belongs to
the panel driver, where it costs nothing:

```cpp
esp_lcd_panel_swap_xy(panel, true);         // landscape from a portrait controller
esp_lcd_panel_mirror(panel, true, false);
esp_lcd_panel_set_gap(panel, 6, 0);         // the CO5300's 6-pixel column offset
```

`set_gap` is the one people miss. If your image is correct but shifted by a few
pixels with a stripe down one edge, the controller's GRAM is bigger than the
glass and every `draw_bitmap` needs the offset. Set it once, after init, and
every later draw agrees with the init table.

To render smaller than the panel - a viewport in a corner, or to buy frame rate
on a big panel - set `ViewerConfig::width` and `height`. a3d will then only
send those rows; nothing else is affected.

If your glass is portrait but you want the model in landscape, do **not**
rotate the framebuffer. Drive the panel in its native orientation and rotate
the camera instead; a 90-degree roll folded into the view-projection costs
nothing per pixel.

---

## Touch

a3d never calls `touchRead` by itself - input is the application's, and an
application that wants a menu needs to see the same events. The field exists so
that a board description is one object.

```cpp
static bool touchRead(void*, int* x, int* y)
    {
    esp_lcd_touch_read_data(s_tp);
    uint16_t px[1], py[1], strength[1];
    uint8_t  count = 0;
    if (!esp_lcd_touch_get_coordinates(s_tp, px, py, strength, &count, 1) || count == 0)
        return false;
    *x = px[0]; *y = py[0];
    return true;
    }
panel.touchRead = &touchRead;

// in your loop:
int x, y;
if (touchRead(nullptr, &x, &y) && wasDown) viewer.drag(x - lastX, y - lastY);
```

`esp_lcd_touch_get_coordinates()` is deprecated from `esp_lcd_touch` 1.2.0
in favour of `esp_lcd_touch_get_data()` and is due to go at 2.0.0. Use whichever
your resolved version has; a3d's own adapter keeps the older one because
`get_data()` does not exist before 1.2.0 and a header cannot pin the version
its host project resolved.

**Touch reset and panel reset are sometimes the same GPIO.** On more than one
board, toggling one resets the other, so the order of the two bring-ups
matters and getting it backwards leaves the screen dark. Check the schematic
before you blame the panel.

---

## Bring it up in stages

Each stage separates two faults that have the same symptom. Skipping them is
how an afternoon disappears.

1. **`--panel offscreen` first, or any project that renders to memory.** It
   proves a3d, the model and the toolchain work before the panel is in the
   picture at all. If the frame rate logs, the 3D half is fine.
2. **`fill(0xF800)`** - a red screen. That proves the bus, the pins, the reset
   line and the backlight. Nothing about a3d is involved yet.
3. **One tile, by hand:** fill a `width * 8` buffer with a colour and
   `sendTile` it at y=0, then at y=100. Wrong position means a gap or a
   swap_xy problem; wrong colour means byte order; nothing at all means the
   window coordinates are off (remember x2/y2 are exclusive).
4. **Then `viewer.begin()`.** Check its return value and print `viewer.error()`
   - a refused `begin()` is silent otherwise.
5. **Count ink if the screen is still blank.** Set
   `ViewerConfig::countPixels = true` and read `viewer.stats().litPixels`. A
   correct render that the panel does not show and a render that drew nothing
   look identical on the glass and have no overlap in where you would look.

---

## Symptoms

| what you see | what it usually is |
|---|---|
| black screen, no errors | backlight never turned on; or `begin()` failed and nobody printed `error()` |
| **pink or magenta background** | byte order. See above; do the arithmetic on your background colour |
| colours inverted | `esp_lcd_panel_invert_color()` - some boards need it on, some off |
| red and blue swapped | `rgb_ele_order`: `LCD_RGB_ELEMENT_ORDER_BGR` |
| image shifted a few pixels, stripe at one edge | `esp_lcd_panel_set_gap()` |
| torn bands, worse with two workers | no bus mutex, or `sendTile` returning before the DMA is done |
| the bottom rows shear or repeat | the panel wants even address windows; use an even `tileHeight` |
| garbage that changes every frame | `sendTile` returning early - THE ONE RULE |
| "previous draw operation is not finished" | MIPI-DSI with DMA2D and no wait |
| `tile at y=... never completed` | the callback was never registered, or something else owns it (LVGL) |
| correct geometry, invisible on a reflective panel | not a bug: 1px line art reads as blank at ~3% ink. Needs thicker strokes |
| works on one core, breaks on two | the mutex, or per-worker state you made global |

---

## ESP-IDF 6.0

The library is version-agnostic; board glue usually is not. Four changes broke
every shipped example in this tree and **none of them was a3d**. All four are
C++-only - the same C compiled:

| 6.0 changed | symptom | fix that is correct on 5.x too |
|---|---|---|
| `cs_gpio_num`/`dc_gpio_num`/`reset_gpio_num` are `gpio_num_t` | `invalid conversion from 'int'` | cast |
| `-Werror=missing-field-initializers` is on | a partial designated-initialiser list is an error, **including inside vendor config macros** | `= {}` then assign fields |
| the umbrella `driver` component stopped requiring the split ones | `driver/ledc.h: No such file` | add `esp_driver_ledc` etc. to `REQUIRES` |
| `ledc_channel_config_t` gained `deconfigure` | missing-initializer again | `= {}` then assign |

Two more, for MIPI-DSI specifically: `esp_lcd_dpi_panel_config_t::pixel_format`
became `in_color_format`/`out_color_format`, and `flags.use_dma2d` became
`esp_lcd_dpi_panel_enable_dma2d()`. Filling the DPI config field by field is
identical on 5.5.4 and works on both.

**Vendor `esp_lcd` config macros do not compile in C++, and it keeps
happening.** They use designated initialisers in an order C++ rejects - and
`data0_io_num` is a union alias for `mosi_io_num`, declared before
`sclk_io_num`, so a macro in a perfectly sensible order is still out of order
for the struct. Fill the struct field by field with the same values; do not
fork the vendor component for it.

**Pin your vendor driver per IDF major, and pin it exactly.** `~1.0.6` of one
panel driver fails to compile on 6.0 because 5.5.4 still carried a field 6.0
deleted; `"*"` silently resolved to a different minor version between two
builds of an unchanged repository. A range per IDF is right; an unbounded one
never is, and a caret is still a range.

**A grep for the symptom is not a survey of the incompatibility.** Collect
every error from a full build before you start editing - fixing one at a time
turned a one-pass job into four rounds here.

---

## Checklist

- [ ] `width`, `height` and `sendTile` set; `viewer.begin()`'s return value checked
- [ ] `sendTile` waits for completion - callback registered, token drained first, timeout
- [ ] executor constructed **before** `viewer.begin()`
- [ ] `lockBus`/`unlockBus` set if more than one worker
- [ ] byte order settled: panel register if it has one, else `swapBytes = true`
- [ ] tile buffers internal and DMA-capable (`setAllocator`)
- [ ] `max_transfer_sz` at least `width * tileHeight * 2`
- [ ] gap/mirror/swap_xy set on the panel, not in a3d
- [ ] backlight on after the first frame, not before

---

## What is verified, and what is not

The adapter in `src/backends/esp_lcd/` was built for the S3 (IO-callback path),
the P4 (DPI path) and with all three wait modes instantiated, on ESP-IDF 5.5.4
and 6.0.2, and both ISR callbacks were confirmed at IRAM addresses in the
linked ELF. The SPI, QSPI and DSI bring-up quoted here is copied from projects
in this tree that have driven real glass and been measured on it.

**Nothing in the generated panel glue has been flashed.** A build proves the
API calls and the struct fields. It says nothing about whether any pin is
yours.
