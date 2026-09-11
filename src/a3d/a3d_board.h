// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file a3d_board.h
 * @brief Target board detection and per-board defaults.
 *
 * Detect the target, then
 * supply defaults that the application may override from the build system.
 *
 * Targets, and what has actually been done on each
 *
 *   - ESP32-P4 : primary. Built, run and MEASURED on hardware; every number in
 *                the project's measurement archive comes from one.
 *   - ESP32-S3 : COMPILES for the target. Never run on an S3, so nothing here
 *                is known about its performance and nothing is claimed. Treat
 *                the defaults below as untested starting points.
 *   - Host CPU : development and unit tests. Repeatedly shown NOT to predict
 *                the P4 - see the measurement archive.
 */
#ifndef A3D_BOARD_H_
#define A3D_BOARD_H_

#include <stdint.h>

/**
 * Marks the rasterizer's inner function so a target can place it where
 * instruction fetches are cheap.
 *
 * Off by default, and MEASURED SLOWER the one time it was tried: on an
 * ESP32-P4 the globe example ran 66.17 ms a frame with the rasterizer in IRAM
 * against 61.3 ms with it left in flash. The L2 cache was doing a better job
 * than moving the code out from under it. It also spends internal RAM - the
 * same RAM the tile framebuffers want, and those must win.
 *
 * The knob stays because the answer is a property of the target and the working
 * set, not of the idea. Opt in with `-DA3D_RASTERIZER_IN_IRAM=1`, then MEASURE.
 * Do not assume it helps.
 *
 */
#ifndef A3D_HOT
#  if defined(ESP_PLATFORM) && defined(A3D_RASTERIZER_IN_IRAM) && (A3D_RASTERIZER_IN_IRAM)
#    include "esp_attr.h"
#    define A3D_HOT IRAM_ATTR
#  else
#    define A3D_HOT
#  endif
#endif

#if defined(ESP_PLATFORM)
// The CONFIG_IDF_TARGET_* macros this file switches on come from sdkconfig.h.
// Without this include their visibility depends on what happened to be included
// FIRST by the translation unit, and when they are missing the detection below
// falls through to "unknown" - which quietly caps the worker count at 1 and
// makes every executor single-threaded.
#include "sdkconfig.h"
#endif

// ---------------------------------------------------------------------------
// Board detection
// ---------------------------------------------------------------------------

#if defined(CONFIG_IDF_TARGET_ESP32P4) || defined(ESP32P4)

    #define A3D_BOARD_ESP32P4         1
    #define A3D_BOARD_NAME            "ESP32-P4"
    #define A3D_CONFIG_MAX_WORKERS    2      ///< dual RISC-V core
    #define A3D_CONFIG_HAS_FPU        1

#elif defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ESP32S3)

    #define A3D_BOARD_ESP32S3         1
    #define A3D_BOARD_NAME            "ESP32-S3"
    #define A3D_CONFIG_MAX_WORKERS    2      ///< dual Xtensa LX7 core
    #define A3D_CONFIG_HAS_FPU        1

#elif defined(CONFIG_IDF_TARGET_ESP32C6) || defined(ESP32C6)

    #define A3D_BOARD_ESP32C6         1
    #define A3D_BOARD_NAME            "ESP32-C6"
    #define A3D_CONFIG_MAX_WORKERS    1      ///< one RISC-V core
    // RV32IMAC. There is no F extension, so every float in the rasterizer is a
    // libgcc call. a3d runs correctly here and is an order of magnitude slower
    // than it is on a part with an FPU; that is a property of the silicon, not
    // of a build setting, and no compiler flag recovers it.
    #define A3D_CONFIG_HAS_FPU        0

#elif defined(_WIN32) || defined(__linux__) || defined(__APPLE__) || defined(__unix__)

    #define A3D_BOARD_HOST            1
    #define A3D_BOARD_NAME            "host"
    #define A3D_CONFIG_MAX_WORKERS    4      ///< development machine; tests override this
    #define A3D_CONFIG_HAS_FPU        1

#else

    #define A3D_BOARD_UNKNOWN         1
    #define A3D_BOARD_NAME            "unknown"
    #define A3D_CONFIG_MAX_WORKERS    1      ///< conservative: assume single core
    // Assumed, not detected. A part without one still renders; it renders
    // slowly, and BoardInfo::hasFpu will not have warned anybody.
    #define A3D_CONFIG_HAS_FPU        1

#endif


// ---------------------------------------------------------------------------
// User-overridable settings. Define these before including a3d headers, or
// pass them from the build system, to override the board defaults.
// ---------------------------------------------------------------------------

/**
 * @def A3D_MAX_WORKERS
 * Upper bound on worker count. An executor may still be constructed with fewer.
 */
#ifndef A3D_MAX_WORKERS
    #define A3D_MAX_WORKERS A3D_CONFIG_MAX_WORKERS
#endif

/**
 * @def A3D_PARALLEL_MIN_CHUNK
 * Below this many items per worker, parallelFor() runs serially. Splitting a
 * tiny workload costs more in synchronization than it saves.
 */
#ifndef A3D_PARALLEL_MIN_CHUNK
    #define A3D_PARALLEL_MIN_CHUNK 256
#endif


namespace a3d {

/** Compile-time board facts, queryable from portable code. */
struct BoardInfo
    {
    static constexpr const char* name       = A3D_BOARD_NAME;
    static constexpr int         maxWorkers = A3D_MAX_WORKERS;
    static constexpr bool        hasFpu     = (A3D_CONFIG_HAS_FPU != 0);
    };

} // namespace a3d

#endif // A3D_BOARD_H_
