# Waveshare ESP32-S3-Touch-AMOLED-1.75C — Arduino

Two skinned, textured, animated models on the 466x466 round AMOLED. It runs
from the moment the board boots: no serial monitor, no button, no benchmark.

- **drag** — orbit
- **long press** (0.7 s, without moving) — switch model (Fox ↔ CesiumMan)

Nothing switches on its own. The model on screen is the one you chose.

## Board settings

| Tools menu | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| PSRAM | **OPI PSRAM** — required; the two models do not fit without it |
| Flash Size | 16MB (128Mb) |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS), or any scheme with ≥ 1MB app |
| USB CDC On Boot | Enabled |

Libraries: **GFX Library for Arduino**, **SensorLib**, and the board's
`pin_config.h`. The panel bring-up and the touch init are taken verbatim from
Waveshare's own Arduino demo for this board, so the pins and the order are
theirs. Everything LVGL is gone — a3d renders RGB565 tiles and hands each one
to Arduino_GFX, which is the whole integration.

## The models

`fox_a3d.h` and `cesiumman_a3d.h` are the same two containers the rest of this
project measures, byte for byte, turned into C arrays because Arduino has no
`EMBED_FILES`:

```bash
python3 tools/a3d_export.py --header fox.a3d       -o fox_a3d.h
python3 tools/a3d_export.py --header cesiumman.a3d -o cesiumman_a3d.h
```

They are 187,492 and 238,468 bytes of `.rodata`, read in place out of
memory-mapped flash. To trade quality for flash, re-import the source `.glb`
with a smaller texture — `--max-texture 128` removes about 98 KB from each.

Both models are CC-BY 4.0 from Khronos; see `examples/common/assets/ATTRIBUTION.md`.

## The five things that are easy to get wrong

1. **The tile must be sent synchronously.** a3d owns one tile buffer per worker
   and overwrites it as soon as that worker takes the next tile.
   `draw16bitRGBBitmap()` blocks, so this is satisfied by doing nothing — but
   it is the first thing to check on any other driver.
2. **Two workers share one QSPI bus.** The bus is not re-entrant, so a3d is
   given a mutex through `Display::lockBus`. Leave it out and the symptom is
   torn bands, not a crash.
3. **The CO5300 wants even address windows.** Waveshare's LVGL port has a
   `rounder_cb` that forces this. a3d's tiles are `x=0, w=466, y=k*TILE_ROWS`,
   so an *even* `TILE_ROWS` makes every window even for free: 466 = 11×40 + 26,
   and both are even. An odd `TILE_ROWS` shears the last rows.
4. **Byte order, and it is also the frame rate.** The CO5300 reads big-endian,
   and the two ways to give it that are not close in cost:

   | call | reaches | what it does |
   |---|---|---|
   | `draw16bitRGBBitmap()` | `Arduino_ESP32QSPI::writePixels()` | byte-swaps every pixel into a 1024-pixel staging buffer, 19 chunks per tile |
   | `draw16bitBeRGBBitmap()` | `Arduino_ESP32QSPI::writeBytes()` | hands our buffer to the DMA untouched |

   So a3d does the swap itself (`swapBytes = true`, one pass over the tile it
   has just rasterized and which is therefore still in cache) and the driver
   copies nothing. That removes a whole extra pass over 434 KB per frame, and
   it is exactly what the ESP-IDF build of this same demo does with
   `esp_lcd_panel_draw_bitmap`. If the background comes out **pink**, this pair
   is out of step.
5. **Where the memory goes.** The Arduino core sets
   `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` to 4096, so by default every buffer
   above 4 KB — the tile framebuffers included — lands in PSRAM. Those are
   written per pixel per frame and belong in internal, DMA-capable RAM;
   `setAllocator()` does that and `setLargeAllocator()` leaves the bulk
   geometry in PSRAM, which is where it should be here.

## Why `build_opt.h` is here

It contains one line, `-O2`. The Arduino core compiles sketches at `-Os` and
offers no menu for it; the ESP-IDF build of this demo uses
`CONFIG_COMPILER_OPTIMIZATION_PERF`, which is `-O2`. a3d is header-only, so the
whole rasterizer is compiled as part of this sketch and gets whatever the
sketch gets — and on the ESP-IDF measurement of this board the rasterizer is
**45 ms of a 54 ms frame**, so it is the term that matters most.

`build_opt.h` is appended after the core's own flags, so `-O2` wins. Delete the
file to go back to `-Os`.

## What ESP-IDF gets on this board, for comparison

From `examples/common/bench/`, 466x466, animated, two workers:

| model | frame | rasterizer | swap | blit |
|---|---|---|---|---|
| Fox | 53.97 ms (18.5 fps) | 44.96 ms | 11.77 ms | 34.35 ms |
| CesiumMan | 77.97 ms (12.8 fps) | 79.19 ms | 11.49 ms | 31.25 ms |

The rasterizer and blit figures are summed across both workers, which is why
they exceed the frame time. Two things worth reading off it: **the byte swap
alone is 11.5 ms a frame**, which is why it must not be paid twice; and the
panel is a hard floor — 466x466x2 bytes at 40 MHz over four lines is about
22 ms of bus time per frame no matter what draws it.

## Verified

Compiled and linked on arduino-esp32 **3.3.10** with the board settings above:
957,990 bytes of flash (30% of a 3 MB app partition), 26,588 bytes of static
RAM. The rendering changes above are read off the two libraries' sources and
have **not been measured on the board** — if you flash it and it is still slow,
say so and the next thing to look at is whether `writeBytes` is reaching DMA.
