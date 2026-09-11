# a3d on a Waveshare ESP32-S3-Touch-AMOLED-1.75C


> **Demo build — no benchmark.** This is the copy under `examples/ESP-IDF/`:
> it boots straight into the viewer. The measuring version, which runs the full
> sweep first and prints the rows the published tables are made from, is the
> original at `examples/Waveshare_ESP32-S3-Touch-AMOLED-1.75C`. Nothing else differs.

Two skinned, textured, animated models on a 466x466 CO5300 AMOLED. Drag to
orbit; the buttons along the bottom change model, animation clip and distance.

The viewer starts as soon as the board boots.

## Why this board is worth having

It is the **same silicon** as [`../ESP32-S3-LCD2`](../ESP32-S3-LCD2) — two
Xtensa LX7 cores at 240 MHz, an FPU, 8 MB of octal PSRAM — behind a completely
different display: 466x466 on four QSPI lanes instead of 240x320 on one.

That makes it the closest thing to a control experiment in this tree. The
benchmark's off-screen half fixes the viewport at 240x320 on every board, so
these two boards *must* report the same numbers. Measured:

| 240x320 off-screen | ESP32-S3-LCD2 | this board |
|---|---|---|
| FOX, static, 1 worker | 22.171 ms | 22.169 ms |
| FOX, animated, 2 workers | 19.528 ms | 19.505 ms |
| CESIUMMAN, static, 1 worker | 53.424 ms | 53.416 ms |
| CESIUMMAN, animated, 2 workers | 41.848 ms | 41.837 ms |

Within 0.1%. If that had *not* held, the cross-board comparison in the main
README would have been measuring panels while claiming to measure parts.

Where the two diverge is the panel, which is the point:

| On the panel, animated, 2 workers | ESP32-S3-LCD2 | this board |
|---|---|---|
| Resolution | 240x320 (76,800 px) | 466x466 (217,156 px) |
| FOX | 31.0 ms, 32.2 fps | 54.0 ms, 18.5 fps |
| CESIUMMAN | 52.0 ms, 19.2 fps | 78.0 ms, 12.8 fps |
| Blit alone | 24.3 ms | 34.4 ms |
| Byte swap alone | 0.0 ms | 11.8 ms |

2.8x the pixels for roughly 1.7x the frame time, because the QSPI bus has four
lanes and the rasterizer scales with covered pixels rather than with screen
area. Roughly a fifth of the gap is the byte swap, which is not a property of
the resolution at all - see below.

## What was removed, and why

The vendor BSP is not used. `waveshare/esp32_s3_touch_amoled_1_75c` is a fine
BSP and it brings LVGL, an ES7210 audio codec, SPIFFS, FATFS and USB along with
the panel. a3d wants a panel.

[`components/a3d_amoled175`](components/a3d_amoled175) is a ~250-line
replacement holding the CO5300 panel, the CST9217 touch controller and the
brightness command, and nothing else. The pin numbers, the initialisation
command table and the 6-pixel column offset are taken from the vendor BSP,
which is Apache-2.0; the attribution is at the top of `amoled175.cpp`.

Also deliberately **not** carried over from Waveshare's configuration:

```
CONFIG_SPIRAM_FETCH_INSTRUCTIONS
CONFIG_SPIRAM_RODATA
CONFIG_SPIRAM_XIP_FROM_PSRAM
```

Those move code *and* `.rodata` into PSRAM. a3d embeds its containers in
`.rodata` and samples their textures in place, so with those options on, the
benchmark's `place=xip` row would be measuring PSRAM while its label said
memory-mapped flash — and the board it is being compared against would not be.
Same silicon, same settings, or the comparison is not one.

## Two things about the CO5300

- **The visible area starts at column 6.** The init table sets the window and
  `esp_lcd_panel_set_gap(panel, 6, 0)` makes every later `draw_bitmap` agree
  with it. Drop either and the picture is six pixels off and wraps.
- **It reads BIG-endian, whatever the BSP says.** The vendor BSP declares
  `BSP_LCD_BIGENDIAN 0`, and taking that at face value produced a **pink**
  background instead of a black one. a3d's `0x1082` arrives as `0x8210`: red
  and blue at 52%, green at 25%, which is desaturated magenta. Equal red and
  blue with the green pulled down is the signature of a byte-swapped RGB565,
  and it is worth recognising because it looks nothing like "the colours are
  slightly off".

  So `swapBytes` is true here and the swap is done in software, costing
  11.8 ms a frame at this resolution and reported separately as `swap_us`. The
  ST7789 on the LCD-2 board can be told to read little-endian and pays nothing;
  the SH8601 on the C6 board is in the same position as this one.

## Hardware

| | |
|---|---|
| MCU | ESP32-S3, two Xtensa LX7 at 240 MHz, FPU, 8 MB octal PSRAM |
| Display | CO5300, 466x466, QSPI at 40 MHz — PCLK 38, D0-D3 on 4/5/6/7, CS 12, RST 1 |
| Touch | CST9217 on I2C0 — SDA 15, SCL 14, INT 11, RST 2. **Both axes mirrored**: the controller's origin is the opposite corner from the panel's |
| Brightness | Panel command 0x51; an AMOLED has no backlight pin |

## Round glass

The panel is a circle, so the rectangular layout the other two boards use does
not fit it: a caption in the top-left corner and a button bar along the bottom
edge are both off the glass.

`Display::round` switches the viewer to a layout computed from the circle
itself — at row *y* the glass spans `2*sqrt(R^2 - (y-R)^2)`, and every caption
and bar is inset from that rather than from a fixed margin.

- **The caption is top centre**, two lines, each fitted to the chord at its own
  row.
- **The buttons are an overlay.** A tap on empty glass brings up a centred list
  of four; a tap on a button acts; five seconds without a touch puts it away. A
  **drag** never opens it — dragging is how the model is turned, and a menu
  that appeared every time you let go would be unusable.
- **The 3D view is left alone.** It renders the full square, and the corners
  falling off the glass is what a round panel is supposed to look like.

## Build

```bash
idf.py set-target esp32s3
idf.py -p <port> flash monitor
```

Built and measured with ESP-IDF v5.5.4.
