// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0
//
// a3d on the Waveshare ESP32-S3-Touch-AMOLED-1.75C.
// Two skinned, textured, animated models on a 466x466 round AMOLED.
//
// It runs the moment the board boots: no serial monitor, no button, no
// benchmark. The first model is on the glass and animating by the end of
// setup().
//
//   drag       - orbit
//   long press - switch model (Fox <-> CesiumMan)
//
// The panel bring-up and the touch controller are taken verbatim from
// Waveshare's own Arduino demo for this board, so the pins and the init order
// are theirs and are known to work. Everything LVGL is gone: a3d renders
// RGB565 tiles and hands each one to Arduino_GFX, which is the whole of the
// integration.
//
// ---------------------------------------------------------------------------
// THE FIVE THINGS THAT ARE EASY TO GET WRONG HERE
//
// 1. THE TILE MUST BE SENT SYNCHRONOUSLY. a3d owns ONE tile buffer per worker
//    and overwrites it as soon as that worker takes the next tile, so a driver
//    that queues the pointer and returns sends whatever overwrote it.
//    Arduino_GFX's draw16bitRGBBitmap() blocks, so this is satisfied by doing
//    nothing - but it is the first thing to check on any other driver.
//
// 2. TWO WORKERS SHARE ONE QSPI BUS. The bus is not re-entrant, so a3d is
//    given a mutex through Display::lockBus. Leave it out and the two workers
//    interleave halfway through an address window; the symptom is torn bands,
//    not a crash.
//
// 3. THE CO5300 WANTS EVEN ADDRESS WINDOWS. Waveshare's LVGL port has a
//    rounder_cb that pushes every flush area out to even x1/y1. a3d's tiles are
//    x=0, w=466 and y=k*TILE_ROWS, so an EVEN TILE_ROWS makes every window even
//    for free: 466 = 11*40 + 26, and 40 and 26 are both even. Change TILE_ROWS
//    to an odd number and the last rows of the panel shear.
//
// 4. BYTE ORDER, AND IT IS ALSO THE FRAME RATE. The CO5300 reads big-endian.
//    There are two ways to give it that and they are not close in cost:
//
//      draw16bitRGBBitmap()   -> Arduino_ESP32QSPI::writePixels(), which
//                                byte-swaps every pixel into a 1024-pixel
//                                staging buffer and sends it in 19 chunks per
//                                tile. That is a whole extra pass over
//                                434 KB per frame on this panel.
//      draw16bitBeRGBBitmap() -> Arduino_ESP32QSPI::writeBytes(), which hands
//                                OUR buffer to the DMA untouched.
//
//    So a3d does the swap itself - `swapBytes = true`, one pass over the tile
//    it has just rasterized and which is therefore still in cache - and the
//    driver copies nothing. This is exactly what the ESP-IDF build of this
//    same demo does with esp_lcd_panel_draw_bitmap.
//
//    How to recognise getting this pair out of step: a near-black background
//    comes out PINK. 0x1082 byte-swapped is 0x8210, equal red and blue with
//    the green pulled down.
//
// 5. WHERE THE MEMORY GOES. The Arduino core sets
//    CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL to 4096, so by default every buffer
//    above 4 KB - the tile framebuffers included - lands in PSRAM. Those are
//    written for every pixel of every frame and belong in internal, DMA-capable
//    RAM. setAllocator() does that; setLargeAllocator() leaves the bulk
//    geometry in PSRAM, which is where it should be on this board.
// ---------------------------------------------------------------------------
// BOARD SETTINGS (Tools menu)
//   Board          : ESP32S3 Dev Module
//   PSRAM          : OPI PSRAM          <- required, the models do not fit without it
//   Flash Size     : 16MB (128Mb)
//   Partition Sch. : 16M Flash (3MB APP/9.9MB FATFS)  or any scheme with >= 1MB app
//   USB CDC On Boot: Enabled            <- or the Serial output never appears
// ---------------------------------------------------------------------------

#include <a3d.h>
// Opt-in, and that is the design: <a3d.h> brings the 5x7 that comes with the
// canvas, and each larger size is a header of its own so a program that wants
// a caption pays for nothing else. This one only needs the Font blitter.
#include <backends/soft/a3d_font.h>

#include "Arduino_GFX_Library.h"
#include "TouchDrvCSTXXX.hpp"
#include "pin_config.h"
#include <Wire.h>
#include <esp_heap_caps.h>

#include "fox_a3d.h"
#include "cesiumman_a3d.h"

// ---------------------------------------------------------------------------
// Panel and touch - Waveshare's own bring-up for this board
// ---------------------------------------------------------------------------

HWCDC USBSerial;

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS /* CS */, LCD_SCLK /* SCK */, LCD_SDIO0 /* SDIO0 */, LCD_SDIO1 /* SDIO1 */,
    LCD_SDIO2 /* SDIO2 */, LCD_SDIO3 /* SDIO3 */);

Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, LCD_RESET /* RST */, 0 /* rotation */, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

TouchDrvCST92xx touch;
static int16_t tpX[5], tpY[5];
static bool    touchOk = false;

// ---------------------------------------------------------------------------
// a3d
// ---------------------------------------------------------------------------

// Even, and it has to be: see note 3 at the top.
static const int TILE_ROWS = 40;

static a3d::Viewer  viewer;
static a3d::Display display;
static a3d::FreeRtosExecutor executor(2);      // both cores draw tiles

static SemaphoreHandle_t busLock = nullptr;

struct Model
    {
    const char*    name;
    const uint8_t* data;
    unsigned int   bytes;
    };

static const Model MODELS[] = {
    { "Fox",       fox_a3d,       fox_a3d_len       },
    { "CesiumMan", cesiumman_a3d, cesiumman_a3d_len },
};
static const int NB_MODELS = (int)(sizeof(MODELS) / sizeof(MODELS[0]));
static int currentModel = 0;

static const char* caption = nullptr;   // the model name, top centre

static bool     touching   = false;
static bool     pressFired = false;
static int      downX = 0, downY = 0, lastX = 0, lastY = 0;
static uint32_t downMs     = 0;
static int      moved      = 0;

//: Hold this long, without travelling, to switch model. Nothing switches on
//: its own: the model on screen is the one you chose, and a viewer that moved
//: on by itself while you were looking at it is a demo, not a tool.
static const uint32_t LONG_PRESS_MS = 700;

//: How far a finger may wander and still count as a press rather than a drag.
//: Dragging is how the model is turned, so a drag must never switch it.
static const int PRESS_SLOP_PX = 12;


// --- the panel seam --------------------------------------------------------

static void sendTile(void*, const uint16_t* src, int x, int y, int w, int h)
    {
    // draw16bitBeRGBBitmap, NOT draw16bitRGBBitmap, and the difference is the
    // whole of note 4 below: this one reaches Arduino_ESP32QSPI::writeBytes(),
    // which hands OUR buffer to the SPI DMA as it stands. The little-endian
    // entry point reaches writePixels(), which byte-swaps every pixel into a
    // 1024-pixel staging buffer first - a second full pass over 434 KB every
    // frame on this panel, on top of the swap a3d already did.
    //
    // Blocking by contract, and it is: the ESP32 QSPI bus polls the transfer
    // to completion before returning. See note 1.
    gfx->draw16bitBeRGBBitmap(x, y, (uint16_t*)src, w, h);
    }

static void fillPanel(void*, uint16_t colour)
    {
    gfx->fillScreen(colour);
    }

static void lockBus(void*)   { xSemaphoreTake(busLock, portMAX_DELAY); }
static void unlockBus(void*) { xSemaphoreGive(busLock); }

// --- allocators ------------------------------------------------------------

// Internal AND DMA-capable. MALLOC_CAP_DMA is not decoration here: sendTile
// hands this buffer straight to the SPI peripheral, and a tile in PSRAM or in
// non-DMA memory either fails the transfer or silently sends the wrong bytes.
static void* fastAlloc(size_t n)
    { return heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT); }
static void  fastFree(void* p) { heap_caps_free(p); }

// PSRAM: binner lists, scratch and the decoded vertex buffers. Read per
// triangle rather than per pixel, and far too large to spend internal RAM on
// with two models this size.
static void* bigAlloc(size_t n)
    {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    // Falling back rather than failing: with PSRAM set to Disabled in Tools
    // this would otherwise return null and the model would refuse to open with
    // no hint as to why. Fox alone may still fit internally; CesiumMan will not.
    return (p != nullptr) ? p : malloc(n);
    }
static void  bigFree(void* p) { heap_caps_free(p); }

static uint64_t nowUs(void*) { return (uint64_t)esp_timer_get_time(); }

// --- the caption, drawn into each tile on its way to the panel -------------

static void overlay(void*, uint16_t* tile, int tileTop, int rows, int width, int)
    {
    if (caption == nullptr) return;

    // Top centre. On round glass a corner is the one place guaranteed not to be
    // lit, so a caption cannot go in one.
    const a3d::Font& font = a3d::font5x7();
    const int scale = 2;
    const int x = (width - a3d::fontTextWidth(font, caption, scale)) / 2;

    // `y` is the BASELINE, and it is absolute-screen minus this tile's top row.
    // The overlay is called for EVERY tile, which is what lets text straddling
    // two of them come out in both; drawText clips per pixel to `rows`.
    const int y = 46 - tileTop;

    a3d::drawText(tile, width, width, rows, x, y, caption,
                  a3d::rgb565(230, 230, 240), font, scale);
    }

// ---------------------------------------------------------------------------

static bool loadModel(int index)
    {
    const Model& m = MODELS[index];
    if (!viewer.openAsset(m.data, m.bytes))
        {
        USBSerial.printf("[a3d] %s failed to open: %s\n", m.name, viewer.error());
        return false;
        }
    viewer.frameModel();
    if (viewer.clipCount() > 0)
        {
        viewer.selectClip(0);
        viewer.setPlaying(true);      // animating before the first frame is drawn
        }
    caption = m.name;
    USBSerial.printf("[a3d] %s: %lu triangles, %lu clip(s) %s\n",
                     m.name, (unsigned long)viewer.triangleCount(),
                     (unsigned long)viewer.clipCount(),
                     viewer.clipCount() ? viewer.clipName() : "");
    return true;
    }

void setup()
    {
    // Started but never waited on: the demo must not depend on somebody
    // opening a serial monitor.
    USBSerial.begin(115200);
    USBSerial.println("\n[a3d] Waveshare ESP32-S3-Touch-AMOLED-1.75C");

    // --- touch, exactly as Waveshare brings it up --------------------------
    //
    // TOUCH FIRST, AND THAT ORDER IS NOT ARBITRARY: on this board TP_RST and
    // LCD_RESET are the SAME GPIO (2). Toggling it to reset the touch
    // controller also resets the panel, so the panel has to be brought up
    // afterwards. Swap these two blocks and the screen stays dark.
    Wire.begin(IIC_SDA, IIC_SCL);
    pinMode(TP_RST, OUTPUT);
    digitalWrite(TP_RST, LOW);
    delay(30);
    digitalWrite(TP_RST, HIGH);
    delay(50);

    touch.setPins(TP_RST, TP_INT);
    touchOk = touch.begin(Wire, 0x5A, IIC_SDA, IIC_SCL);
    if (!touchOk)
        USBSerial.println("[a3d] touch is not online; the demo still runs, "
                          "it just cannot be tapped or dragged");
    else
        {
        touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
        touch.setMirrorXY(true, true);
        }

    // --- panel -------------------------------------------------------------
    gfx->begin();
    gfx->fillScreen(RGB565_BLACK);
    gfx->setBrightness(200);

    busLock = xSemaphoreCreateMutex();

    // --- a3d ---------------------------------------------------------------
    display.width     = gfx->width();
    display.height    = gfx->height();
    display.name      = "co5300_qspi";
    display.sendTile  = sendTile;
    display.fill      = fillPanel;
    display.lockBus   = lockBus;
    display.unlockBus = unlockBus;
    display.round     = true;      // 466x466 circular glass
    display.swapBytes = true;      // see note 4

    a3d::ViewerConfig cfg;
    cfg.tileHeight = TILE_ROWS;
    cfg.background = a3d::rgb565(4, 6, 10);

    // Everything below is read by begin(), so it comes first. The executor
    // especially: it is what decides how many tile buffers get allocated.
    viewer.setExecutor(executor);
    viewer.setClock(nowUs, nullptr);
    viewer.setAllocator(fastAlloc, fastFree);
    viewer.setLargeAllocator(bigAlloc, bigFree);
    viewer.setOverlay(overlay, nullptr);

    if (!viewer.begin(display, cfg))
        {
        USBSerial.printf("[a3d] begin failed: %s\n", viewer.error());
        gfx->setCursor(20, 220);
        gfx->setTextColor(RGB565_RED);
        gfx->println("a3d begin() failed");
        return;
        }

    USBSerial.printf("[a3d] %dx%d, %d rows a tile, internal free %u, psram free %u\n",
                     display.width, display.height, TILE_ROWS,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    loadModel(currentModel);
    }

// --- input: a long press switches the model, a drag orbits -----------------

static void nextModel()
    {
    currentModel = (currentModel + 1) % NB_MODELS;
    loadModel(currentModel);
    }

static void readTouch()
    {
    if (!touchOk) return;
    const uint8_t n = touch.getPoint(tpX, tpY, 1);
    const uint32_t now = millis();

    if (n > 0)
        {
        const int px = tpX[0], py = tpY[0];
        if (!touching)
            {
            touching = true; pressFired = false;
            downX = lastX = px; downY = lastY = py;
            downMs = now; moved = 0;
            }
        else
            {
            viewer.drag(px - lastX, py - lastY);
            moved = max(moved, max(abs(px - downX), abs(py - downY)));
            lastX = px; lastY = py;
            }

        // Fires WHILE the finger is still down, so the model changes under it
        // and there is feedback without having to let go and wonder. Once per
        // press - pressFired is what stops it repeating every frame - and
        // never once the finger has travelled, because by then it is a drag.
        if (!pressFired && (moved < PRESS_SLOP_PX) && ((now - downMs) >= LONG_PRESS_MS))
            {
            pressFired = true;
            nextModel();
            }
        }
    else
        {
        touching = false;
        }
    }

void loop()
    {
    if (!viewer.ready()) { delay(500); return; }

    readTouch();

    // Turning from the very first frame, so the board looks alive the instant
    // it boots rather than after somebody touches it.
    if (!touching)
        viewer.orbit(0.012f, 0.0f);

    static uint64_t lastUs = 0;
    const uint64_t t0 = nowUs(nullptr);
    const float dtMs = (lastUs == 0) ? 16.0f : (float)(t0 - lastUs) / 1000.0f;
    lastUs = t0;

    // dtMs is what advances the clip, so the animation runs at real speed
    // whatever the frame rate happens to be.
    viewer.frame(dtMs);
    }
