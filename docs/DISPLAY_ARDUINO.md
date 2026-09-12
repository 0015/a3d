<!-- SPDX-FileCopyrightText: 2026 Eric Nam -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Getting a3d onto a panel - Arduino

This is the step most people stop at, so it is written out in full. If you are
on ESP-IDF, read [DISPLAY_ESP_IDF.md](DISPLAY_ESP_IDF.md) instead - the ideas
are the same and none of the code is.

**Arduino here means arduino-esp32.** a3d's `architectures` is `esp32`, because
every part it has been built and measured on is an ESP32 and offering it to an
UNO would only produce a compile error.

---

## What a3d asks for, and what it does not

a3d has no panel driver and will never have one. Your board has a schematic and
a3d does not. You keep using the display library you already have - Arduino_GFX,
TFT_eSPI, Adafruit_GFX, a vendor fork - and a3d asks it for one thing:

```cpp
a3d::Display panel;
panel.width    = 240;
panel.height   = 320;
panel.sendTile = sendTile;      // move a rectangle of RGB565 to the glass
```

That is the whole required interface. Everything else has a default:

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
`x` is always 0 and `w` is always the full width.

### THE ONE RULE

**`sendTile` must not return until `src` may be reused.**

a3d owns one tile buffer per worker and hands it to the next tile immediately.
A driver that queues the pointer and returns will send half of one tile and
half of the next - and it does not look like a buffer problem, it looks like a
rasterizer bug.

Most Arduino display libraries block, so this is usually satisfied by doing
nothing. It is still the first thing to check on any driver you have not used
this way before, and it is not free everywhere: some libraries have an
explicitly asynchronous DMA path (`pushImageDMA`, `writePixelsDMA` and
friends). **Do not use those.** Use the blocking call.

---

## A whole sketch

```cpp
#include <a3d.h>
#include "model_a3d.h"                 // see "The asset" below
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>

static a3d::Viewer  viewer;
static a3d::Display panel;
static a3d::FreeRtosExecutor executor(2);     // both cores draw tiles

Arduino_DataBus* bus = new Arduino_ESP32SPI(DC, CS, SCK, MOSI, MISO);
Arduino_GFX*     gfx = new Arduino_ST7789(bus, RST, 0 /*rotation*/, true);

// Blocking by contract, and it is: the ESP32 SPI path polls the transfer to
// completion before returning.
static void sendTile(void*, const uint16_t* src, int x, int y, int w, int h)
    { gfx->draw16bitRGBBitmap(x, y, (uint16_t*)src, w, h); }

static void fillPanel(void*, uint16_t colour) { gfx->fillScreen(colour); }

// The tile buffers go through the SPI DMA, so they must be internal and
// DMA-capable. See "Memory" below - this is not optional on a board with PSRAM.
static void* fastAlloc(size_t n)
    { return heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT); }
static void  fastFree(void* p) { heap_caps_free(p); }

static void* bigAlloc(size_t n)
    {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);          // so a board with PSRAM off still says why
    }
static void  bigFree(void* p) { heap_caps_free(p); }

static uint64_t nowUs(void*) { return (uint64_t)esp_timer_get_time(); }

void setup()
    {
    Serial.begin(115200);
    gfx->begin();
    gfx->fillScreen(BLACK);

    panel.width    = gfx->width();
    panel.height   = gfx->height();
    panel.name     = "st7789_spi";
    panel.sendTile = sendTile;
    panel.fill     = fillPanel;
    // panel.swapBytes = true;         // only if the panel reads big-endian

    a3d::ViewerConfig cfg;
    cfg.tileHeight = 40;               // the memory knob; see "Memory"
    cfg.background = a3d::rgb565(4, 6, 10);

    // Everything below is read by begin(), so it comes first. The executor
    // especially: it is what decides how many tile buffers get allocated.
    viewer.setExecutor(executor);
    viewer.setClock(nowUs, nullptr);
    viewer.setAllocator(fastAlloc, fastFree);
    viewer.setLargeAllocator(bigAlloc, bigFree);

    if (!viewer.begin(panel, cfg))
        { Serial.printf("a3d begin failed: %s\n", viewer.error()); return; }

    if (!viewer.openAsset(model_a3d, model_a3d_len))
        { Serial.printf("open failed: %s\n", viewer.error()); return; }

    viewer.frameModel();               // fit the camera to the model
    if (viewer.clipCount()) { viewer.selectClip(0); viewer.setPlaying(true); }
    }

void loop()
    {
    if (!viewer.ready()) { delay(500); return; }

    static uint32_t last = 0;
    const uint32_t now = millis();
    const float dtMs = last ? (float)(now - last) : 16.0f;
    last = now;

    viewer.orbit(0.012f, 0.0f);
    viewer.frame(dtMs);                // dtMs is what advances the animation
    }
```

A complete, real version of this against a 466x466 QSPI AMOLED is in
[examples/Arduino/Waveshare_ESP32-S3-Touch-AMOLED-1.75C](../examples/Arduino/Waveshare_ESP32-S3-Touch-AMOLED-1.75C),
with the touch controller, a caption overlay and the model switcher.

---

## Byte order, which is also the frame rate

a3d's framebuffer is native-endian RGB565 - little-endian on ESP32. Many panels
read big-endian. Getting this wrong is the single most common first symptom,
and **on Arduino the fix you pick also costs or saves a whole pass over the
frame.**

### The rule

Set `panel.swapBytes = true` when the panel reads big-endian, and then call
your library's **big-endian** entry point, so it does not swap a second time.

| library | big-endian entry point (use with `swapBytes = true`) | little-endian one |
|---|---|---|
| Arduino_GFX | `draw16bitBeRGBBitmap()` | `draw16bitRGBBitmap()` |
| TFT_eSPI | `setSwapBytes(false)` + `pushImage()` | `setSwapBytes(true)` + `pushImage()` |
| Adafruit_GFX / SPITFT | `writePixels(..., bigEndian = true)` | `drawRGBBitmap()` |

### Why it is worth caring

Measured by reading the two libraries on the AMOLED board in this tree:

- `draw16bitRGBBitmap()` reaches `Arduino_ESP32QSPI::writePixels()`, which
  byte-swaps every pixel **into a 1024-pixel staging buffer** and sends it in
  19 chunks per tile. That is an extra full pass over 434 KB every frame.
- `draw16bitBeRGBBitmap()` reaches `writeBytes()`, which hands your buffer to
  the DMA untouched.

So letting a3d swap in place - in the tile it has just rasterized, which is
therefore still in cache - and calling the `Be` entry point is strictly
cheaper than asking the driver to swap into a second buffer. It is also
exactly what the ESP-IDF build of the same demo does.

If the panel reads little-endian, leave `swapBytes` false and use the ordinary
entry point: nothing is swapped anywhere.

### How to recognise getting it wrong

**A near-black background comes out PINK.** a3d's 0x1082 byte-swapped is
0x8210 - red 52%, green 25%, blue 52%: equal red and blue with the green pulled
down, a desaturated magenta. It looks nothing like "the colours are slightly
off". Do that arithmetic on your own background colour before touching anything
else.

**And do not trust a vendor BSP's or example's declaration.** One in this tree
declares little-endian about a panel that reads big-endian. The panel settles
it, not the header.

---

## Board settings (Tools menu)

| setting | why |
|---|---|
| **PSRAM: enabled** (OPI or QSPI, as your board has) | anything past a small model needs it; without it `openAsset()` fails on a container of a few hundred KB |
| **Partition Scheme** | the model is compiled into the sketch, so pick a scheme with an app partition bigger than your model plus ~400 KB |
| **USB CDC On Boot: Enabled** | or `Serial` never appears and a failed `begin()` is silent |
| **Flash Size** | must match the board; an image built for more flash than the board has will not run |

---

## Memory, and the one Arduino default that hurts

Each slot costs `width * tileHeight * 2` bytes for pixels and the same again
for depth, with one slot per worker.

| panel | 40 rows, 2 workers, with z-buffer |
|---|---|
| 240x320 | 4 x 19 KB = 77 KB |
| 466x466 | 4 x 37 KB = 149 KB |

**arduino-esp32 sets `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` to 4096.** On a
board with PSRAM that means essentially every a3d buffer above 4 KB - tile
framebuffers included - lands in the slow memory by default. Those are written
for every pixel of every frame and read by the display DMA, which is the worst
possible place for them. There is no menu for it, so use the allocator hooks in
the sketch above:

- `setAllocator()` - tiles, z-buffers, scratch. Internal, **`MALLOC_CAP_DMA`**.
- `setLargeAllocator()` - binner lists and decoded geometry. PSRAM is right.

Both must be called **before** `begin()`. If you are short of internal RAM,
lower `cfg.tileHeight` before you move a tile buffer to PSRAM: 24 rows costs
40% less than 40 and changes nothing else.

---

## `-O2`, which is worth more than it sounds

The Arduino core compiles sketches at `-Os` and offers no menu for it. a3d is
header-only, so the **whole rasterizer is compiled as part of your sketch** and
gets whatever the sketch gets - and on one measured board the rasterizer is
45 ms of a 54 ms frame.

Put a file called `build_opt.h` next to your `.ino` containing one line:

```
-O2
```

The core appends it after its own flags, so it wins. Measured on
arduino-esp32 3.3.10: flash 393,400 -> 424,342 bytes on a P4. That is what
`-O2` looks like.

**Arduino is measurably the slow path**, and a3d's published benchmark tables
are ESP-IDF's for that reason. If you want the numbers in those tables, use
ESP-IDF.

---

## The asset - Arduino has no `EMBED_FILES`

Two routes, and the first needs a tool:

```bash
python3 tools/a3d_export.py --header model.a3d -o model_a3d.h
```

```cpp
#include "model_a3d.h"
viewer.openAsset(model_a3d, model_a3d_len);       // byte array
viewer.openAssetFile("/sd/model.a3d");            // or a file: SD.begin() first
```

**The generated array is `__attribute__((aligned(4)))` and that is
load-bearing.** a3d casts the base pointer straight to the file header and
rejects any chunk offset that is not a multiple of 4. `EMBED_FILES` gives that
alignment for free; a plain `const uint8_t[]` does not have to, and an
unaligned container fails **on the board**, never at compile time. Do not
hand-roll it with `xxd -i`.

The file route needs nothing new: `openAssetFile()` is plain `fopen`, and the
Arduino cores register VFS mounts, so the stock `SD` and `LittleFS` libraries
work.

---

## Two workers share one bus

A panel bus is not re-entrant. With `FreeRtosExecutor(2)`, two `sendTile` calls
can overlap and interleave halfway through an address window. The symptom is
torn bands, not a crash.

```cpp
static SemaphoreHandle_t busLock;                 // xSemaphoreCreateMutex() in setup()

static void lockBus(void*)   { xSemaphoreTake(busLock, portMAX_DELAY); }
static void unlockBus(void*) { xSemaphoreGive(busLock); }

panel.lockBus   = lockBus;
panel.unlockBus = unlockBus;
```

Leave the executor out entirely and a3d renders every tile inline on one core,
allocates one set of buffers instead of two, and needs no mutex at all. Single
core is a supported target, not a fallback.

---

## Tile height, and the panels that want even windows

`cfg.tileHeight` is rows per tile. Two constraints beyond memory:

1. **Some controllers want even address windows.** A CO5300 does - vendor LVGL
   ports carry a `rounder_cb` that pushes every flush area out to even
   coordinates. a3d's tiles are `x = 0`, `w = full width`, `y = k * tileHeight`,
   so an **even `tileHeight` makes every window even for free**. An odd one
   shears the last rows of the panel.
2. **A partial last tile is normal.** 466 = 11x40 + 26; a3d clamps the last
   strip itself. Your `sendTile` must honour the `h` it is given rather than
   assuming a constant.

---

## Touch

a3d never reads touch by itself - input is yours, and an application with a
menu needs the same events. Feed it whatever your touch library gives you:

```cpp
if (n > 0)                                   // finger down
    {
    if (!touching) { touching = true; lastX = px; lastY = py; }
    else           { viewer.drag(px - lastX, py - lastY); lastX = px; lastY = py; }
    }
else touching = false;
```

`viewer.drag(dx, dy)` orbits, `viewer.zoom(ratio)` moves in and out,
`viewer.setOrbit(yaw, pitch, distance)` places the camera outright.

**Touch reset and panel reset are sometimes the same GPIO** - they are on the
AMOLED board in this tree, where resetting touch also resets the panel, so
touch has to be brought up **first** or the screen stays dark. Check the
schematic before blaming the display.

**And a long press must be distinguished from a drag.** Dragging is how the
model is turned; a gesture that fires whenever a finger lifts makes the viewer
unusable. Gate it on the finger not having travelled.

---

## Bring it up in stages

Each stage separates two faults that have the same symptom.

1. **Render off screen first.**
   [examples/Arduino/Model_From_Header](../examples/Arduino/Model_From_Header)
   uploads and runs on a board with nothing attached, and proves a3d, the
   model, PSRAM and the partition scheme before the panel is involved at all.
2. **`gfx->fillScreen(RED)`** - that proves the bus, the pins, the reset line
   and the backlight. No a3d yet.
3. **One tile by hand:** fill a `width * 8` buffer with one colour and call
   your `sendTile` at y=0, then y=100. Wrong position is rotation or an offset;
   wrong colour is byte order; nothing at all is the address window.
4. **Then `viewer.begin()`** - check the return value and print
   `viewer.error()`. A refused `begin()` is silent otherwise.
5. **Count ink if the screen is still blank.** Set `cfg.countPixels = true` and
   read `viewer.stats().litPixels`. "It drew nothing" and "it drew and the
   panel did not show it" look identical on the glass and have no overlap in
   where you would look.

---

## Symptoms

| what you see | what it usually is |
|---|---|
| nothing on Serial at all | **USB CDC On Boot** is disabled |
| black screen, no errors | backlight off; or `begin()` failed and nobody printed `error()` |
| **pink or magenta background** | byte order. Pair `swapBytes` with the matching entry point |
| colours inverted | your driver's `invert` argument - some boards need it on, some off |
| red and blue swapped | the driver's BGR flag, not a3d |
| image shifted a few pixels | a column/row offset your `Arduino_GFX` constructor takes as `col_offset1` / `row_offset1` |
| the bottom rows shear | odd `tileHeight` on a controller that wants even windows |
| torn bands with 2 workers | no bus mutex |
| garbage that changes every frame | `sendTile` returning early - THE ONE RULE. Are you on a DMA entry point? |
| `openAsset` returns false | PSRAM disabled, or the byte array is not 4-byte aligned |
| boots, then reboots | app partition too small, or no PSRAM for the model |
| runs but slowly | `-Os` (add `build_opt.h`), tiles in PSRAM (add `setAllocator`), or a swapping driver entry point |
| ESP-IDF version of the same thing is faster | it is. See `-O2` above; the tables are ESP-IDF's |

---

## Other display libraries

Only Arduino_GFX has been used with a3d in this tree. The two below are written
from their published APIs and **nothing here has built or run them** - the
shape is what matters, and the shape is always the same: one blocking call that
pushes RGB565, plus the byte-order pairing.

```cpp
// TFT_eSPI - UNVERIFIED HERE
static void sendTile(void*, const uint16_t* src, int x, int y, int w, int h)
    {
    tft.startWrite();
    tft.setAddrWindow(x, y, w, h);
    tft.pushPixels((uint16_t*)src, w * h);     // NOT pushPixelsDMA
    tft.endWrite();
    }
// tft.setSwapBytes(false) together with a3d's panel.swapBytes = true,
// or setSwapBytes(true) with a3d's left false. Exactly one of them swaps.
```

```cpp
// Adafruit_GFX / Adafruit_SPITFT - UNVERIFIED HERE
static void sendTile(void*, const uint16_t* src, int x, int y, int w, int h)
    {
    tft.startWrite();
    tft.setAddrWindow(x, y, w, h);
    tft.writePixels((uint16_t*)src, (uint32_t)w * h, true /*block*/, false /*bigEndian*/);
    tft.endWrite();
    }
```

Whatever the library, the four questions are the same: **does the call block,
who swaps the bytes, is there a mutex if two workers share the bus, and is the
tile buffer DMA-capable.**

---

## Checklist

- [ ] `width`, `height` and `sendTile` set; `viewer.begin()`'s return value checked
- [ ] `sendTile` uses a **blocking** entry point, never a DMA one
- [ ] `swapBytes` paired with the matching entry point - exactly one swap happens
- [ ] executor, clock and both allocators set **before** `begin()`
- [ ] `setAllocator` gives internal, DMA-capable memory
- [ ] `lockBus`/`unlockBus` set if more than one worker
- [ ] `tileHeight` even if the controller wants even windows
- [ ] `build_opt.h` with `-O2` beside the sketch
- [ ] PSRAM enabled and the partition scheme big enough
- [ ] the model header is 4-byte aligned (use `--header`, not `xxd`)

---

## What is verified, and what is not

The sketches in `examples/Arduino/` were compiled **and linked** with
arduino-cli 1.2.0 on arduino-esp32 3.2.0 and 3.3.10, for ESP32-S3, P4 and C6.
**None of them has been flashed.** The panel bring-up in the board sketch is
taken verbatim from the vendor's own Arduino demo for that board, so the pins
and the init order are theirs and are known to work; a3d's half above it is
not proven on glass by this tree.
