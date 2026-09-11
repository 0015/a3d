// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file a3d_demo_app.h
 * @brief The board-independent half of the a3d demo: benchmark, then viewer.
 *
 * Two boards in this tree run the same demo, and they run the SAME OBJECT CODE
 * for everything above the panel. That is the whole point of this file. A
 * benchmark that is copy-pasted between two projects diverges within one
 * session, and then the two columns of a comparison table are measuring two
 * different programs without saying so.
 *
 * A board provides a Display: its size, how to push a tile, how to read a
 * finger. Everything else - loading the containers, binning, rasterizing,
 * timing, the self-checks, the on-screen HUD - lives here.
 */
#ifndef A3D_DEMO_APP_H_
#define A3D_DEMO_APP_H_

#include <stdint.h>

namespace a3d_demo
{

/**
 * What the board has to supply.
 *
 * `sendTile` must not return until `src` may be reused: the demo owns one tile
 * buffer per worker and overwrites it as soon as the next tile is drawn.
 */
struct Display
    {
    int width  = 0;
    int height = 0;

    /// Free-form, printed in the report; e.g. "st7789_spi", "sh8601_qspi".
    const char* kind = "none";
    /// Pixel clock in MHz, printed in the report. 0 if not meaningful.
    int clockMHz = 0;

    /**
     * True when the panel reads RGB565 big-endian and the demo must therefore
     * byte-swap each tile before sending it.
     *
     * This is a real per-frame cost and it is reported separately as swap_us,
     * not folded into the blit. An ST7789 can be told to read little-endian
     * (RAMCTRL bit 3) and pays nothing; an SH8601 cannot and pays for every
     * pixel of every frame.
     */
    bool swapBytes = false;

    /**
     * True for a circular panel.
     *
     * A rectangular margin is the wrong shape for round glass: the inset that
     * keeps a top row on the screen wastes most of the width of the middle
     * rows, and the inset that suits the middle rows runs the top and bottom
     * ones off the edge. So the layout is computed per row from the circle,
     * the caption moves to the top centre, and the button bar becomes an
     * overlay that a tap brings up - a bar along the bottom edge of a circle
     * is simply not on the glass.
     *
     * The 3D view itself is left alone: it renders the full square, and the
     * corners falling off the glass is what a round panel looks like.
     */
    bool round = false;

    void (*sendTile)(const uint16_t* src, int x, int y, int w, int h) = nullptr;
    void (*fill)(uint16_t colour) = nullptr;

    /// Optional. Return false when no finger is down. The viewer still spins.
    bool (*touchRead)(int* x, int* y) = nullptr;

    /**
     * Optional, and the only way to get pinch-zoom.
     *
     * Returns the number of contacts, 0 to 2, and fills as many points as it
     * returns. When it is null the demo falls back to `touchRead` and there is
     * simply no pinch - which is why adding this did not have to touch the
     * boards that do not implement it.
     *
     * A board that sets this does NOT also need `touchRead`; the demo prefers
     * this one when both are present.
     */
    int (*touchRead2)(int* x, int* y, int* x2, int* y2) = nullptr;
    };

/**
 * Run the benchmark and then the viewer. Never returns.
 *
 * Pass a Display with `sendTile == nullptr` for a headless build: the
 * benchmark still runs at the comparison viewport and the function halts
 * instead of entering the viewer.
 */
void run(const Display& display);

} // namespace a3d_demo

#endif // A3D_DEMO_APP_H_
