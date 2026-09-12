# Changelog

All notable changes to a3d are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.9.2] - 2026-09-12

Transparency, which a3d does not have and had not been saying so. A glTF
material marked `alphaMode: BLEND` is not drawn faint by this renderer, it is
drawn solid - and the importer had been ignoring the field without a word.

### Added
- `--drop-blend` for `a3d_export.py`: leave out primitives whose glTF material
  is `alphaMode: BLEND` or `MASK`.

### Changed
- **A non-opaque material now warns on every import.** a3d writes every covered
  pixel opaquely - there is no alpha blend and no alpha test - and the importer
  had been ignoring `alphaMode` silently. The shapes authored for BLEND are the
  ones meant to be nearly invisible (a propeller blur disc, a soft shadow quad,
  a glow card), so they are large and they sit in front of the model.

  They are also in `worldBounds()`, which is the half that does not look like a
  transparency problem: on one stylized aircraft 40 triangles of 7,692 inflated
  the bounding box from 1.37 to 3.24 and pushed the framing camera 2.4x too far
  back. The vertex, triangle, material and texture counts were all correct, so
  nothing that counts could see it - the same shape as the Draco import this
  project already refuses by name.

## [0.9.1] - 2026-09-11

Panels you already have, rather than a longer catalogue of panels a3d has
driven. The catalogue in `tools/a3d_panel.py` is deliberately small - a bus is
in it only if its bring-up came from a project in this tree that has driven
real glass - and that leaves out almost every board. Every esp_lcd driver in
the component registry, every vendor fork and every BSP ends at an
`esp_lcd_panel_handle_t`, so the way in is one adapter rather than thirty
entries, and the pins stay in the code that already works on your desk.

### Added
- `a3d::EspLcdDisplay` (`src/backends/esp_lcd/a3d_display_esp_lcd.h`): an
  `a3d::Display` from an `esp_lcd_panel_handle_t` you already have. It owns the
  completion wait, the drain of stale tokens, the bus mutex two workers need,
  and optional touch. Header-only, and the optional dependencies
  (`esp_lcd_mipi_dsi.h`, `esp_lcd_touch.h`) are probed with `__has_include`, so
  a project without them compiles the feature out rather than failing.
- `--panel esp_lcd_custom` for `a3d_export.py --project`: a generated `panel.c`
  with one empty `panel_bring_up()` and everything else written. It builds and
  flashes as generated and says the function is empty, rather than refusing to
  compile until you have written the hard part. Its size comes from
  `--viewport`, because it is the one panel whose glass a3d has not been told
  about.

- `docs/DISPLAY_ESP_IDF.md` and `docs/DISPLAY_ARDUINO.md`: how to get a panel
  lit, one file per framework. Bringing up a display is the step most people
  stop at, and every hard part of it is invisible - the DMA wait that must not
  return early, the byte order that turns a black background pink, the bus
  mutex two workers need, the tile buffers that land in PSRAM on an Arduino
  board by default. Both carry a symptom table and a bring-up ladder that
  separates "it drew nothing" from "it drew and the panel did not show it".

### Fixed
- **The generated MIPI-DSI glue did not wait for its tile.** Its comment said a
  DPI `draw_bitmap` is a plain memory write, which is true only without DMA2D -
  and it enables DMA2D. With DMA2D the copy is asynchronous, the call returns
  with the 2D-DMA still reading the tile, and the next call is rejected with
  `ESP_ERR_INVALID_STATE`. It now registers `on_color_trans_done` and waits, as
  the hardware-verified example in this tree already did.
- **The generated project pinned `waveshare/esp_lcd_jd9365` to `~1.0.6`,** which
  cannot compile on ESP-IDF 6.0: 5.5 still carries
  `esp_lcd_panel_dev_config_t::color_space` as a deprecated union member and 6.0
  deleted it. The range is `>=1.0.6,<3.0.0` now, so the solver picks 1.x for
  IDF 5 and 2.x for IDF 6. Both were built to a flashable binary.
- The generated project README advertised `--resolution`, which is not a flag.
  It is `--viewport`.

## [0.9.0] - 2026-09-10

First public release.

0.9 rather than 1.0 because the container format is at v0.1 and is still
allowed to change: a `MRPH` chunk is reserved but unimplemented, and the
`uint16` vertex index is a format-level ceiling that a future version may
raise. Everything documented in the README as verified has been run on
hardware.

### Added
- Binary asset container (`.a3d`): offsets rather than pointers, so an image
  is usable in place from memory-mapped flash with no fixup pass.
- Scene runtime: node hierarchy, world transforms, materials, textures.
- Skeletal animation: clip playback, quantized channels, GPU-free skinning
  parallelised across an executor.
- Tile binner: per-tile triangle lists, so a scene renders into a small
  internal-RAM tile rather than a full framebuffer.
- Software rasterizer (`SoftBackend`): RGB565, Gouraud and flat shading,
  perspective-correct texturing with mipmaps, nearest and bilinear filtering,
  specular, transparency, depth-aware 3D lines, and a no-depth fast path.
- 2D canvas and bitmap text: a hand-authored 5x7 font, plus generated 8x14,
  12x21 and 20x35 sizes in a header each, and one blitter that reads all of
  them. `tools/gen_font.py` produces more from any monospace TTF whose licence
  allows it; see `docs/FONTS.md`.
- Executors: serial, `std::thread`, and FreeRTOS.
- Loaders: stdio, ESP-IDF partition, SD/SDMMC, and STL (binary and ASCII, from
  a file or from memory, welded and fed through the same binner and rasterizer
  as a container mesh).
- Python exporter: glTF/GLB to `.a3d`, with mesh decimation and animation
  key reduction.
- **Arduino library support, from the same files as the ESP-IDF component.**
  Everything lives under one `src/` root because Arduino adds exactly
  `<lib>/src` to the include path and will not add a second; ESP-IDF is told
  `INCLUDE_DIRS "src"` and every `#include "a3d/..."`, `"viewer/..."`,
  `"backends/..."` and `"loaders/..."` resolves from it. `#include <a3d.h>`
  brings up the renderer in one line - it is at the root of `src/` because
  Arduino's resolver indexes only the headers directly there, and a library
  whose every header is one level down is never matched at all.
- Two Arduino sketches under `examples/Arduino/`, which the IDE shows as one
  submenu. `Waveshare_ESP32-S3-Touch-AMOLED-1.75C` puts Fox and CesiumMan on a
  466x466 round AMOLED and runs from boot; `Model_From_Header` renders off
  screen and needs no wiring at all. Both compiled and linked on arduino-esp32
  3.3.10. **Neither has been flashed.**
- `tools/a3d_export.py --header FILE.a3d` writes a container as a C byte array,
  which is the only way onto an Arduino board short of a file system. The array
  is 4-byte aligned, and that is required rather than cosmetic: a3d casts the
  base pointer to the file header, so an unaligned container is refused on the
  board and never at compile time.
- `a3d::Viewer` (`src/viewer/a3d_viewer.h`): a model on a screen in four calls -
  `begin()`, an `open*()`, `frame()`. It owns the tile buffers, the binners,
  the orbit camera and the animation player, and it exists to make the orderings
  that are silent when wrong impossible to get wrong.
- `a3d_view`: renders a container to a PNG contact sheet on the host, so an
  asset can be looked at before it is ever flashed.
- `a3d_export.py --project DIR` writes a buildable ESP-IDF project around a
  model, and `--panel` / `--touch` generate the display glue for the buses this
  project has driven glass with.
- Board detection for ESP32-P4, ESP32-S3 and ESP32-C6, including whether the
  part has a floating-point unit.

### Known limitations
See the README. The short version: textures must be power-of-two RGB565, a
single mesh cannot exceed 65,535 vertices, there is no top-left fill rule, and
a part without an FPU (ESP32-C6) is roughly an order of magnitude slower.
