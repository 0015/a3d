// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file a3d_display.h
 * @brief Everything a3d needs from a panel, and nothing else.
 *
 * WHY THIS IS FIVE FIELDS AND NOT A DRIVER
 *
 *   Every panel is wired differently and initialised differently, and that is
 *   the application's problem - it has the board schematic and a3d does not.
 *   What a3d needs is narrower than a driver: a size, and one function that
 *   moves a rectangle of RGB565 to the glass. A component that asked for more
 *   than that would have to know about SPI, QSPI, DSI, DMA and vendor BSPs,
 *   and would then be wrong about at least one of them.
 *
 *   So this is a plain struct of function pointers. It names no rasterizer, no
 *   RTOS and no vendor header, which is why it can live in the core.
 *
 * THE CONTRACT ON sendTile
 *
 *   It must not return until `src` may be reused. The viewer owns ONE tile
 *   buffer per worker and overwrites it as soon as that worker is handed the
 *   next tile, so a driver that queues the pointer and returns immediately
 *   will send whatever the next tile happened to overwrite it with. If the
 *   driver is asynchronous, wait for its completion inside sendTile.
 *
 * OFF-SCREEN RENDERING IS THE SAME SEAM
 *
 *   To render into memory rather than to a panel - a screenshot, a host test,
 *   a benchmark with no display attached - supply a sendTile that memcpys each
 *   tile into a full-size framebuffer of your own. There is deliberately no
 *   second code path for it: a headless build that does not go through the
 *   same dispatch is not measuring the same program.
 *
 * @see a3d::Viewer in viewer/a3d_viewer.h, which is what consumes this.
 */
#ifndef A3D_DISPLAY_H_
#define A3D_DISPLAY_H_

#include <stdint.h>

namespace a3d {

/**
 * A panel, as a3d sees it.
 *
 * `user` is passed back to every callback. It exists because the alternative
 * is a file-scope global per application, which is what every example in this
 * tree had to write before this struct existed.
 */
struct Display
    {
    /** Panel size in pixels. Both must be positive. */
    int width  = 0;
    int height = 0;

    /** Free-form, printed in logs; e.g. "st7789_spi", "co5300_qspi". */
    const char* name = "display";

    /** Passed back to sendTile, lockBus, unlockBus and touchRead. */
    void* user = nullptr;

    /**
     * Move one tile to the panel. `x` is always 0 and `w` always the full
     * width: the viewer cuts the screen into horizontal strips, because that
     * is the shape a panel's own address window and DMA both prefer.
     *
     * Must not return until `src` may be reused. See the file comment.
     */
    void (*sendTile)(void* user, const uint16_t* src,
                     int x, int y, int w, int h) = nullptr;

    /**
     * Optional. Fill the whole panel with one colour, used once at startup so
     * the screen is not showing the previous app while the first frame builds.
     * When null the viewer simply does not clear.
     */
    void (*fill)(void* user, uint16_t colour) = nullptr;

    /**
     * Optional. Return false when no finger is down.
     *
     * The viewer never calls this by itself - input is the application's, and
     * an application that wants a spin-on-idle or a menu needs to see the same
     * events. It is here so that a board description is ONE object rather than
     * a display plus a touch controller that have to be passed around together.
     */
    bool (*touchRead)(void* user, int* x, int* y) = nullptr;

    /**
     * True when the panel reads RGB565 big-endian, so the viewer must swap
     * every halfword of every tile before sending it.
     *
     * This is a real per-frame cost - 11.6 ms at 480x480 on a measured board -
     * so look for the panel setting that removes it before accepting it. An
     * ST7789 can be told to read little-endian (RAMCTRL bit 3) and then pays
     * nothing. And do not trust a vendor BSP's declaration: one in this tree
     * says little-endian about a panel that reads big-endian.
     *
     * How to recognise getting this wrong: a near-black background comes out
     * PINK. 0x1082 byte-swapped is 0x8210, which is equal red and blue with
     * the green pulled down - a desaturated magenta, and nothing like
     * "the colours are slightly off".
     */
    bool swapBytes = false;

    /**
     * True for circular glass. The viewer does not inset the 3D view for it -
     * rendering the full square and letting the corners fall off the glass is
     * what a round panel is supposed to look like - but an overlay callback
     * can ask, and a caption belongs top centre rather than in a corner.
     */
    bool round = false;

    /**
     * Optional, and only consulted when more than one worker is drawing.
     *
     * A panel bus is not re-entrant, so with two workers the two sendTile
     * calls have to take turns. The viewer has no portable mutex to offer -
     * that is an RTOS question - so it takes yours. Leave both null and make
     * sendTile itself thread-safe, or run on one worker where the question
     * does not arise.
     */
    void (*lockBus)(void* user)   = nullptr;
    void (*unlockBus)(void* user) = nullptr;

    bool valid() const
        { return (width > 0) && (height > 0) && (sendTile != nullptr); }
    };

} // namespace a3d

#endif // A3D_DISPLAY_H_
