# Model_From_Header

A real `.a3d` container - skeleton, clip, texture - rendered from a byte array
compiled into the sketch. Still off screen, so it needs no wiring.

## The asset problem is Arduino-specific

On ESP-IDF a container goes into the firmware with `EMBED_FILES` and a3d reads
it in place out of memory-mapped flash. The Arduino IDE has no equivalent, so
there are two routes:

**1. A byte array in the sketch.** What this example does.

```bash
python3 tools/a3d_export.py --check my_model.glb          # -> my_model.a3d
python3 tools/a3d_export.py --header my_model.a3d -o model_a3d.h
```

```cpp
#include "model_a3d.h"
viewer.openAsset(model_a3d, model_a3d_len);
```

The generated array is **4-byte aligned**, and that is not cosmetic: a3d casts
the base pointer straight to the file header and rejects any chunk whose offset
is not a multiple of 4. An unaligned container fails on the board, never at
compile time.

**2. A file.** `viewer.openAssetFile("/sd/model.a3d")`. a3d's stdio loader is
plain `fopen`, and the Arduino cores register VFS mounts, so the stock `SD` and
`LittleFS` libraries work - call `SD.begin()` first.

The `model_a3d.h` checked in here was generated from

```bash
python3 tools/a3d_export.py --skinned 4 --segments 8 --bones 4 -o model.a3d
python3 tools/a3d_export.py --header model.a3d -o model_a3d.h
```

## Where the memory goes is Arduino-specific too

The Arduino core sets `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` to **4096**, so on
a board with PSRAM every allocation above 4 KB lands in PSRAM by default -
including the tile framebuffers the rasterizer writes for every pixel of every
frame. `setAllocator()` puts those back in internal, DMA-capable RAM. That is
the one line in this sketch most likely to matter on your board.

## Verified

Compiled and linked for ESP32-S3 (365,343 B), ESP32-P4 (393,400 B) and
ESP32-C6 (365,206 B) on arduino-esp32 3.3.10. **Not flashed.**
