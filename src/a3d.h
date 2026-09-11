// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file a3d.h
 * @brief One include that brings up the renderer. The Arduino entry point.
 *
 * WHY THIS FILE EXISTS, AND WHY IT IS AT THE ROOT OF src/
 *
 *   Not for convenience. Arduino's library resolver indexes only the headers
 *   sitting DIRECTLY in a library's `src/`, so a library whose every header
 *   lives in a subdirectory is invisible to it: a sketch that says
 *   `#include <viewer/a3d_viewer.h>` fails with "No such file or directory"
 *   before any include path is consulted, because no library was ever matched
 *   and `src/` was never added to the path. Measured on arduino-cli 1.2.0 with
 *   arduino-esp32 3.2.0, both ways round.
 *
 *   So one header has to be here. Its NAME does not matter to the resolver -
 *   that was measured too - but `a3d.h` is what somebody will type.
 *
 *   ESP-IDF and plain CMake do not need it. They get `src/` as an include root
 *   and `#include "viewer/a3d_viewer.h"` keeps working exactly as before; this
 *   header is an addition, not a replacement, and no existing include changed.
 *
 * WHAT IS DELIBERATELY NOT HERE
 *
 *   The loaders. `loaders/a3d_load_esp_partition.h` and `loaders/a3d_load_sd.h`
 *   name ESP-IDF components that a caller has to declare on the ESP-IDF side
 *   (`REQUIRES sdmmc fatfs esp_driver_sdmmc` for the SD one), and pulling them
 *   in unasked would make every user of a 3D library pay for FATFS. Include the
 *   one you want.
 *
 *   The large fonts. `backends/soft/a3d_font_{8x14,12x21,20x35}.h` are 1.3, 4.0
 *   and 10.0 KB of .rodata each and the whole point of splitting them per size
 *   is that a program which wants a caption pays for none of the others. The
 *   5x7 that arrives with the canvas is 665 bytes and is dropped by
 *   --gc-sections when nothing draws text.
 */
#ifndef A3D_H_
#define A3D_H_

#include "a3d/a3d_runtime.h"        // container, scene, skinning
#include "a3d/a3d_primitives.h"     // sphere, box, cone
#include "viewer/a3d_viewer.h"      // a model on a screen in four calls
#include "backends/soft/a3d_canvas.h"  // 2D overlay + the 5x7 font

#if defined(ESP_PLATFORM)
// Multi-core tile drawing. On Arduino this is the same FreeRTOS the core is
// already built on, so there is nothing extra to declare.
#include "backends/freertos/a3d_freertos_executor.h"
#endif

#endif // A3D_H_
