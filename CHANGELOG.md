# Changelog

All notable changes to a3d are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.9.0] - 2026-09-11

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
