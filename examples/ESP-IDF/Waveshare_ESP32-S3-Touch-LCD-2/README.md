# a3d basics — three shapes, one light


> **Demo build — no benchmark.** This is the copy under `examples/ESP-IDF/`:
> it boots straight into the viewer. The measuring version, which runs the full
> sweep first and prints the rows the published tables are made from, is the
> original at `examples/Waveshare_ESP32-S3-Touch-LCD-2`. Nothing else differs.

The smallest complete a3d program. Read this one first.

No asset files, no skeletal animation, no benchmark, no frame counter. Three
primitives are generated at boot, lit by one directional light, and turned
slowly on a 240x320 panel.

Everything worth changing is in one `SETTINGS` block at the top of
[`main/main.cpp`](main/main.cpp): field of view, camera distance, light
direction, ambient / diffuse / specular, spin rate, and how round the shapes
are. Change a number, reflash, watch what moves.

For the same renderer driving loaded `.a3d` models, skeletal animation, touch
and two cores, see [`../ESP32-S3-LCD2`](../ESP32-S3-LCD2).

## Build

```bash
idf.py set-target esp32s3
idf.py -p <port> flash monitor
```

Expected on the console:

```
I (679) a3d_basic: sphere 720 tris, box 12 tris, cylinder 96 tris
I (728) a3d_basic: first frame: 22510 of 76800 pixels drawn
```

**That second line is a diagnostic, not a statistic.** A black screen is the
usual first failure, and it has two causes with the same symptom: the renderer
drew nothing, or it drew and the panel did not show it. A non-zero count rules
the first one out, and there is no overlap in where you would look next. It is
printed once, for the first frame only.

## The five things a3d needs

`main.cpp` is organised in these five sections, in this order.

| | | |
|---|---|---|
| 1 | **A target** | Where pixels go. `setTarget()` and `setOffset()`. |
| 2 | **A camera** | `setViewportSize()`, `setPerspective()`, `setLookAt()`, plus the depth buffer and the culling direction. |
| 3 | **A light** | `setLightDirection()` and the four material terms. |
| 4 | **Geometry** | `sphereMesh()`, `boxMesh()`, `truncatedConeMesh()` — the generators build arrays; they do not draw. |
| 5 | **A frame** | One `Mat4` per shape, one `TriangleBatch` per shape, then blit. |

## Why it draws in strips

A 240x320 RGB565 framebuffer is 153,600 bytes and its depth buffer is another
153,600. This board has about 270 KB of internal SRAM free by the time
`app_main` runs, so the pair does not fit — and putting them in PSRAM makes
every pixel write cross a slow bus.

So the frame is drawn as eight full-width strips of 40 rows, 19,200 bytes each,
each one rendered and pushed to the panel before the next is started. The
rasterizer clips to whatever target it is given, so drawing the whole scene
into each strip in turn produces the same picture as drawing it once into a
framebuffer that does not exist.

The subtlety: **`setViewportSize()` is the whole screen; `setTarget()` and
`setOffset()` are the strip.** The projection has to know it is making a
240x320 image even while it is filling rows 80 to 119.

This example deliberately re-submits every shape for every strip, which is the
simplest thing that works. At 828 triangles that is fine. A larger scene sorts
triangles into per-tile lists once with `a3d::TileBinner` instead — that is
what `../ESP32-S3-LCD2` does, and it is the only difference in the drawing path.

## Two things that are easy to get backwards

- **`multRotate` and `multTranslate` PRE-multiply.** `setScale()`,
  `multRotate()`, `multTranslate()` builds `T * R * S`: the shape spins about
  its own centre and is then moved into place. Doing the translate first makes
  it orbit the origin instead, which does not look like a matrix bug — it looks
  like the wrong centre of rotation.
- **`setCulling(-1)`** is the direction that matches the stored projection,
  whose y row is negated so clip space matches framebuffer rows. Both
  directions give the same silhouette on a closed shape; only the shading
  differs.

## Hardware

| | |
|---|---|
| Board | Waveshare ESP32-S3-Touch-LCD-2 |
| Display | ST7789, 240x320, SPI2 at 80 MHz — SCLK 39, MOSI 38, DC 42, CS 45 |
| Backlight | LEDC on GPIO 1 |

Touch is not used here; the scene turns on its own.

The panel is told to read RGB565 **little-endian** (`data_endian =
LCD_RGB_DATA_ENDIAN_LITTLE`, which is bit 3 of the ST7789's `RAMCTRL`), so
a3d's native output goes to the wire with no byte swap at all.
