// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file a3d_canvas.h
 * @brief 2D drawing over a 16-bit framebuffer: the overlay a 3D scene needs.
 *
 * A renderer that can only draw triangles cannot label anything. Readouts,
 * panels, markers and text are 2D work, and they are the difference between a
 * demo and an instrument.
 *
 *
 * A Canvas is a VIEW, not an owner: it holds a pointer, a size and a stride, so
 * it can address one tile of a larger framebuffer without a copy. Every
 * primitive clips against the view, so a caller may pass tile-local coordinates
 * that fall outside it and get the right answer.
 *
 * Colours are RGB565. Opacity, where a primitive takes it, is 0..1 and blends
 * in 5-6-5 directly - no intermediate 8-bit expansion, which would cost three
 * multiplies and give back the same five bits.
 */

#ifndef A3D_CANVAS_H_
#define A3D_CANVAS_H_

#include <stdint.h>
#include <stddef.h>

#include "backends/soft/a3d_soft_text.h"

namespace a3d {

/** Pack 8-bit RGB into 5-6-5. */
inline constexpr uint16_t rgb565(int r, int g, int b)
    {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3));
    }

/** Pack components that are ALREADY in 5-6-5 range: r and b are 0..31, g is
    0..63. Palettes written by eye are usually authored this way, and rounding
    them up to 8 bits only to shift them back down loses the low bit. */
inline constexpr uint16_t rgb565Raw(int r, int g, int b)
    {
    return (uint16_t)(((r & 31) << 11) | ((g & 63) << 5) | (b & 31));
    }

// A few names the eye reaches for.
inline constexpr uint16_t kBlack = 0x0000;
inline constexpr uint16_t kWhite = 0xFFFF;
inline constexpr uint16_t kGray  = rgb565Raw(15, 31, 15);

class Canvas
    {
    public:
        Canvas() = default;
        Canvas(uint16_t* pixels, int w, int h, int stride)
            { set(pixels, w, h, stride); }

        /** Point the canvas at a framebuffer, or at one tile of one.
            `stride` is the full framebuffer width in pixels. */
        /** Stride defaults to the width: the common case is a whole buffer. */
        void set(uint16_t* pixels, int w, int h) { set(pixels, w, h, w); }

        void set(uint16_t* pixels, int w, int h, int stride)
            {
            _px = pixels; _w = w; _h = h; _stride = (stride > 0) ? stride : w;
            }

        int width() const { return _w; }
        int height() const { return _h; }
        uint16_t* pixels() const { return _px; }

        // ------------------------------------------------------------------
        // ------------------------------------------------------------------

        void pixel(int x, int y, uint16_t c)
            {
            if ((_px == nullptr) || (x < 0) || (y < 0) || (x >= _w) || (y >= _h)) return;
            _px[(size_t)y * _stride + x] = c;
            }

        void pixel(int x, int y, uint16_t c, float opacity)
            {
            if ((_px == nullptr) || (x < 0) || (y < 0) || (x >= _w) || (y >= _h)) return;
            if (opacity >= 1.0f) { _px[(size_t)y * _stride + x] = c; return; }
            if (opacity <= 0.0f) return;
            uint16_t* p = _px + (size_t)y * _stride + x;
            *p = _blend(*p, c, (int)(opacity * 256.0f));
            }

        /** The pixel already there, or 0 outside the view. */
        uint16_t read(int x, int y) const
            {
            if ((_px == nullptr) || (x < 0) || (y < 0) || (x >= _w) || (y >= _h)) return 0;
            return _px[(size_t)y * _stride + x];
            }

        // ------------------------------------------------------------------
        // ------------------------------------------------------------------

        void fill(uint16_t c)
            {
            if (_px == nullptr) return;
            for (int y = 0; y < _h; y++)
                {
                uint16_t* row = _px + (size_t)y * _stride;
                for (int x = 0; x < _w; x++) row[x] = c;
                }
            }

        void hline(int x, int y, int len, uint16_t c)
            {
            if ((_px == nullptr) || (y < 0) || (y >= _h) || (len <= 0)) return;
            int x0 = x, x1 = x + len - 1;
            if (x0 < 0) x0 = 0;
            if (x1 > _w - 1) x1 = _w - 1;
            if (x0 > x1) return;
            uint16_t* row = _px + (size_t)y * _stride;
            for (int i = x0; i <= x1; i++) row[i] = c;
            }

        void vline(int x, int y, int len, uint16_t c)
            {
            if ((_px == nullptr) || (x < 0) || (x >= _w) || (len <= 0)) return;
            int y0 = y, y1 = y + len - 1;
            if (y0 < 0) y0 = 0;
            if (y1 > _h - 1) y1 = _h - 1;
            for (int i = y0; i <= y1; i++) _px[(size_t)i * _stride + x] = c;
            }

        /** Bresenham. Horizontal and vertical runs go to the fast paths, which
            is what a wireframe overlay is mostly made of. */
        void line(int x0, int y0, int x1, int y1, uint16_t c)
            {
            if (y0 == y1) { hline((x0 < x1) ? x0 : x1, y0, (x1 > x0 ? x1 - x0 : x0 - x1) + 1, c); return; }
            if (x0 == x1) { vline(x0, (y0 < y1) ? y0 : y1, (y1 > y0 ? y1 - y0 : y0 - y1) + 1, c); return; }

            int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
            int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
            const int sx = (x0 < x1) ? 1 : -1;
            const int sy = (y0 < y1) ? 1 : -1;
            dy = -dy;
            int err = dx + dy;
            for (;;)
                {
                pixel(x0, y0, c);
                if ((x0 == x1) && (y0 == y1)) break;
                const int e2 = err * 2;
                if (e2 >= dy) { err += dy; x0 += sx; }
                if (e2 <= dx) { err += dx; y0 += sy; }
                }
            }

        // ------------------------------------------------------------------
        // ------------------------------------------------------------------

        void hline(int x, int y, int len, uint16_t c, float opacity)
            {
            if (opacity >= 1.0f) { hline(x, y, len, c); return; }
            for (int i = 0; i < len; i++) pixel(x + i, y, c, opacity);
            }

        void vline(int x, int y, int len, uint16_t c, float opacity)
            {
            if (opacity >= 1.0f) { vline(x, y, len, c); return; }
            for (int i = 0; i < len; i++) pixel(x, y + i, c, opacity);
            }

        void line(int x0, int y0, int x1, int y1, uint16_t c, float opacity)
            {
            if (opacity >= 1.0f) { line(x0, y0, x1, y1, c); return; }
            if (opacity <= 0.0f) return;
            int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
            int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
            const int sx = (x0 < x1) ? 1 : -1;
            const int sy = (y0 < y1) ? 1 : -1;
            dy = -dy;
            int err = dx + dy;
            for (;;)
                {
                pixel(x0, y0, c, opacity);
                if ((x0 == x1) && (y0 == y1)) break;
                const int e2 = err * 2;
                if (e2 >= dy) { err += dy; x0 += sx; }
                if (e2 <= dx) { err += dx; y0 += sy; }
                }
            }

        void circle(int cx, int cy, int r, uint16_t c, float opacity)
            {
            if (opacity >= 1.0f) { circle(cx, cy, r, c); return; }
            if ((opacity <= 0.0f) || (r < 0)) return;
            int x = r, y = 0, err = 1 - r;
            while (x >= y)
                {
                pixel(cx + x, cy + y, c, opacity); pixel(cx + y, cy + x, c, opacity);
                pixel(cx - y, cy + x, c, opacity); pixel(cx - x, cy + y, c, opacity);
                pixel(cx - x, cy - y, c, opacity); pixel(cx - y, cy - x, c, opacity);
                pixel(cx + y, cy - x, c, opacity); pixel(cx + x, cy - y, c, opacity);
                y++;
                if (err < 0) err += 2 * y + 1;
                else { x--; err += 2 * (y - x) + 1; }
                }
            }

        void rect(int x, int y, int w, int h, uint16_t c, float opacity)
            {
            if ((w <= 0) || (h <= 0)) return;
            hline(x, y, w, c, opacity);
            hline(x, y + h - 1, w, c, opacity);
            vline(x, y, h, c, opacity);
            vline(x + w - 1, y, h, c, opacity);
            }

        void rect(int x, int y, int w, int h, uint16_t c)
            {
            if ((w <= 0) || (h <= 0)) return;
            hline(x, y, w, c);
            hline(x, y + h - 1, w, c);
            vline(x, y, h, c);
            vline(x + w - 1, y, h, c);
            }

        void fillRect(int x, int y, int w, int h, uint16_t c)
            {
            if ((_px == nullptr) || (w <= 0) || (h <= 0)) return;
            int x0 = x, y0 = y, x1 = x + w - 1, y1 = y + h - 1;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > _w - 1) x1 = _w - 1;
            if (y1 > _h - 1) y1 = _h - 1;
            if ((x0 > x1) || (y0 > y1)) return;
            for (int yy = y0; yy <= y1; yy++)
                {
                uint16_t* row = _px + (size_t)yy * _stride;
                for (int xx = x0; xx <= x1; xx++) row[xx] = c;
                }
            }

        void fillRect(int x, int y, int w, int h, uint16_t c, float opacity)
            {
            if (opacity >= 1.0f) { fillRect(x, y, w, h, c); return; }
            if ((opacity <= 0.0f) || (_px == nullptr) || (w <= 0) || (h <= 0)) return;
            const int a = (int)(opacity * 256.0f);
            int x0 = x, y0 = y, x1 = x + w - 1, y1 = y + h - 1;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > _w - 1) x1 = _w - 1;
            if (y1 > _h - 1) y1 = _h - 1;
            if ((x0 > x1) || (y0 > y1)) return;
            for (int yy = y0; yy <= y1; yy++)
                {
                uint16_t* row = _px + (size_t)yy * _stride;
                for (int xx = x0; xx <= x1; xx++) row[xx] = _blend(row[xx], c, a);
                }
            }

        // ------------------------------------------------------------------
        // ------------------------------------------------------------------

        void circle(int cx, int cy, int r, uint16_t c)
            {
            if (r < 0) return;
            int x = r, y = 0, err = 1 - r;
            while (x >= y)
                {
                pixel(cx + x, cy + y, c); pixel(cx + y, cy + x, c);
                pixel(cx - y, cy + x, c); pixel(cx - x, cy + y, c);
                pixel(cx - x, cy - y, c); pixel(cx - y, cy - x, c);
                pixel(cx + y, cy - x, c); pixel(cx + x, cy - y, c);
                y++;
                if (err < 0) err += 2 * y + 1;
                else { x--; err += 2 * (y - x) + 1; }
                }
            }

        void fillCircle(int cx, int cy, int r, uint16_t c)
            {
            if (r < 0) return;
            for (int dy = -r; dy <= r; dy++)
                {
                const int span = _isqrt(r * r - dy * dy);
                hline(cx - span, cy + dy, span * 2 + 1, c);
                }
            }

        void fillCircle(int cx, int cy, int r, uint16_t c, float opacity)
            {
            if (opacity >= 1.0f) { fillCircle(cx, cy, r, c); return; }
            if ((opacity <= 0.0f) || (r < 0)) return;
            const int a = (int)(opacity * 256.0f);
            for (int dy = -r; dy <= r; dy++)
                {
                const int yy = cy + dy;
                if ((yy < 0) || (yy >= _h)) continue;
                const int span = _isqrt(r * r - dy * dy);
                int x0 = cx - span, x1 = cx + span;
                if (x0 < 0) x0 = 0;
                if (x1 > _w - 1) x1 = _w - 1;
                uint16_t* row = _px + (size_t)yy * _stride;
                for (int xx = x0; xx <= x1; xx++) row[xx] = _blend(row[xx], c, a);
                }
            }

        // ------------------------------------------------------------------
        // Rounded rectangles
        // ------------------------------------------------------------------

        void roundRect(int x, int y, int w, int h, int r, uint16_t c, float opacity = 1.0f)
            {
            if ((w <= 0) || (h <= 0)) return;
            const int m = ((w < h) ? w : h) / 2;
            if (r > m) r = m;
            if (r < 1) { rect(x, y, w, h, c, opacity); return; }
            hline(x + r, y, w - 2 * r, c, opacity);
            hline(x + r, y + h - 1, w - 2 * r, c, opacity);
            vline(x, y + r, h - 2 * r, c, opacity);
            vline(x + w - 1, y + r, h - 2 * r, c, opacity);
            _corners(x, y, w, h, r, c, opacity, false);
            }

        void fillRoundRect(int x, int y, int w, int h, int r, uint16_t c,
                           float opacity = 1.0f)
            {
            if ((w <= 0) || (h <= 0)) return;
            const int m = ((w < h) ? w : h) / 2;
            if (r > m) r = m;
            if (r < 1) { fillRect(x, y, w, h, c, opacity); return; }
            fillRect(x, y + r, w, h - 2 * r, c, opacity);
            _corners(x, y, w, h, r, c, opacity, true);
            }

        // ------------------------------------------------------------------
        // ------------------------------------------------------------------

        /**
         * Copy a 16-bit image into the canvas at `x, y`.
         *
         * The clip is worked out ONCE, before the loop, rather than per pixel.
         * Writing this by hand as a nested loop over pixel() is the obvious
         * thing and it pays a bounds test on every texel of every icon, every
         * frame - which is exactly the shape of cost that does not show up in
         * a profile as one line.
         *
         *
         * `srcStride` is the source image's full width; 0 means "same as sw".
         */
        void blit(int x, int y, const uint16_t* src, int sw, int sh,
                  int srcStride = 0, float opacity = 1.0f)
            {
            _blit(x, y, src, sw, sh, srcStride, opacity, false, 0);
            }

        /** Same, but pixels equal to `transparent` are left alone - the usual
            way a sprite carries its own cutout. */
        void blitMasked(int x, int y, const uint16_t* src, int sw, int sh,
                        uint16_t transparent, int srcStride = 0, float opacity = 1.0f)
            {
            _blit(x, y, src, sw, sh, srcStride, opacity, true, transparent);
            }

        // ------------------------------------------------------------------
        // ------------------------------------------------------------------

        /** Flat-filled, scanline between sorted edges. This is 2D: no depth, no
            interpolation, no relation to the rasterizer. */
        void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c)
            {
            if (y0 > y1) { _swap(x0, x1); _swap(y0, y1); }
            if (y1 > y2) { _swap(x1, x2); _swap(y1, y2); }
            if (y0 > y1) { _swap(x0, x1); _swap(y0, y1); }
            if (y2 == y0)
                {
                int lo = x0, hi = x0;
                if (x1 < lo) lo = x1;
                if (x2 < lo) lo = x2;
                if (x1 > hi) hi = x1;
                if (x2 > hi) hi = x2;
                hline(lo, y0, hi - lo + 1, c);
                return;
                }
            for (int y = y0; y <= y2; y++)
                {
                const bool upper = (y < y1);
                const int ya = upper ? y0 : y1, yb = upper ? y1 : y2;
                const int xa = upper ? x0 : x1, xb = upper ? x1 : x2;
                int sa = (yb != ya) ? (xa + (xb - xa) * (y - ya) / (yb - ya)) : xa;
                int sb = x0 + (x2 - x0) * (y - y0) / (y2 - y0);
                if (sa > sb) _swap(sa, sb);
                hline(sa, y, sb - sa + 1, c);
                }
            }

        void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2,
                          uint16_t c, float opacity)
            {
            if (opacity >= 1.0f) { fillTriangle(x0, y0, x1, y1, x2, y2, c); return; }
            if (opacity <= 0.0f) return;
            if (y0 > y1) { _swap(x0, x1); _swap(y0, y1); }
            if (y1 > y2) { _swap(x1, x2); _swap(y1, y2); }
            if (y0 > y1) { _swap(x0, x1); _swap(y0, y1); }
            if (y2 == y0) return;
            for (int y = y0; y <= y2; y++)
                {
                const bool upper = (y < y1);
                const int ya = upper ? y0 : y1, yb = upper ? y1 : y2;
                const int xa = upper ? x0 : x1, xb = upper ? x1 : x2;
                int sa = (yb != ya) ? (xa + (xb - xa) * (y - ya) / (yb - ya)) : xa;
                int sb = x0 + (x2 - x0) * (y - y0) / (y2 - y0);
                if (sa > sb) _swap(sa, sb);
                hline(sa, y, sb - sa + 1, c, opacity);
                }
            }

        void triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c)
            {
            line(x0, y0, x1, y1, c);
            line(x1, y1, x2, y2, c);
            line(x2, y2, x0, y0, c);
            }

        // ------------------------------------------------------------------
        // ------------------------------------------------------------------

        /** `y` is the BASELINE. `scale` repeats each glyph pixel, so 2 gives a
            heading twice the height of body text.

            Drawn through pixel(), not through a separate blitter, so clipping
            and opacity are the same code that every other primitive uses -
            there is no second place for an off-by-one to live. */
        void text(int x, int y, const char* s, uint16_t c,
                  float opacity = 1.0f, int scale = 1)
            {
            if ((s == nullptr) || (scale < 1)) return;
            const int top = y - (kFontH * scale - 1);
            for (; *s != '\0'; s++, x += kFontAdvance * scale)
                {
                unsigned ch = (unsigned char)*s;
                if ((ch < 32u) || (ch > 126u)) ch = (unsigned)'?';
                const uint8_t* col = kFont5x7 + (ch - 32u) * kFontW;
                for (int cx = 0; cx < kFontW; cx++)
                    {
                    const uint8_t bits = col[cx];
                    for (int cy = 0; cy < kFontH; cy++)
                        {
                        if (((bits >> cy) & 1u) == 0u) continue;
                        for (int ry = 0; ry < scale; ry++)
                            for (int rx = 0; rx < scale; rx++)
                                pixel(x + cx * scale + rx, top + cy * scale + ry, c, opacity);
                        }
                    }
                }
            }

        /** Same, centred horizontally on `cx`. */
        void textCentered(int cx, int y, const char* s, uint16_t c,
                          float opacity = 1.0f, int scale = 1)
            {
            text(cx - textWidth(s, scale) / 2, y, s, c, opacity, scale);
            }

        /** Width in pixels the string will occupy. */
        static int textWidth(const char* s, int scale = 1)
            {
            int n = 0;
            if (s != nullptr) for (; *s != '\0'; s++) n++;
            if (scale < 1) scale = 1;
            return (n > 0) ? ((n * kFontAdvance - 1) * scale) : 0;
            }

    private:
        static void _swap(int& a, int& b) { const int t = a; a = b; b = t; }

        /// The four rounded corners, outline or filled.
        void _corners(int x, int y, int w, int h, int r, uint16_t c,
                      float opacity, bool filled)
            {
            const int lx = x + r, rx = x + w - 1 - r;
            const int ty = y + r, by = y + h - 1 - r;
            int cx = r, cy = 0, err = 1 - r;
            while (cx >= cy)
                {
                if (filled)
                    {
                    hline(lx - cx, ty - cy, (rx - lx) + 2 * cx + 1, c, opacity);
                    hline(lx - cy, ty - cx, (rx - lx) + 2 * cy + 1, c, opacity);
                    hline(lx - cx, by + cy, (rx - lx) + 2 * cx + 1, c, opacity);
                    hline(lx - cy, by + cx, (rx - lx) + 2 * cy + 1, c, opacity);
                    }
                else
                    {
                    pixel(rx + cx, by + cy, c, opacity); pixel(rx + cy, by + cx, c, opacity);
                    pixel(lx - cy, by + cx, c, opacity); pixel(lx - cx, by + cy, c, opacity);
                    pixel(lx - cx, ty - cy, c, opacity); pixel(lx - cy, ty - cx, c, opacity);
                    pixel(rx + cy, ty - cx, c, opacity); pixel(rx + cx, ty - cy, c, opacity);
                    }
                cy++;
                if (err < 0) err += 2 * cy + 1;
                else { cx--; err += 2 * (cy - cx) + 1; }
                }
            }

        void _blit(int x, int y, const uint16_t* src, int sw, int sh,
                   int srcStride, float opacity, bool masked, uint16_t key)
            {
            if ((_px == nullptr) || (src == nullptr) || (sw <= 0) || (sh <= 0)) return;
            if (opacity <= 0.0f) return;
            if (srcStride <= 0) srcStride = sw;

            // Clip once: how much to skip on the source, and how much survives.
            int sx0 = 0, sy0 = 0;
            int dx0 = x, dy0 = y;
            if (dx0 < 0) { sx0 = -dx0; dx0 = 0; }
            if (dy0 < 0) { sy0 = -dy0; dy0 = 0; }
            int cw = sw - sx0, chh = sh - sy0;
            if (dx0 + cw > _w) cw = _w - dx0;
            if (dy0 + chh > _h) chh = _h - dy0;
            if ((cw <= 0) || (chh <= 0)) return;

            const int a = (int)(opacity * 256.0f);
            const bool blend = (opacity < 1.0f);
            for (int r = 0; r < chh; r++)
                {
                const uint16_t* srow = src + (size_t)(sy0 + r) * srcStride + sx0;
                uint16_t* drow = _px + (size_t)(dy0 + r) * _stride + dx0;
                for (int i = 0; i < cw; i++)
                    {
                    const uint16_t v = srow[i];
                    if (masked && (v == key)) continue;
                    drow[i] = blend ? _blend(drow[i], v, a) : v;
                    }
                }
            }

        /** Integer square root, for circle spans. No float, no libm. */
        static int _isqrt(int v)
            {
            if (v <= 0) return 0;
            int r = 0, bit = 1 << 15;
            while (bit > v) bit >>= 2;
            int rem = v;
            while (bit != 0)
                {
                if (rem >= r + bit) { rem -= r + bit; r = (r >> 1) + bit; }
                else r >>= 1;
                bit >>= 2;
                }
            return r;
            }

        /** Blend in 5-6-5. `a` is 0..256. */
        static uint16_t _blend(uint16_t dst, uint16_t src, int a)
            {
            if (a >= 256) return src;
            if (a <= 0) return dst;
            const int ia = 256 - a;
            const int r = (((src >> 11) & 31) * a + ((dst >> 11) & 31) * ia) >> 8;
            const int g = (((src >> 5) & 63) * a + ((dst >> 5) & 63) * ia) >> 8;
            const int b = (( src       & 31) * a + ( dst       & 31) * ia) >> 8;
            return (uint16_t)((r << 11) | (g << 5) | b);
            }

        uint16_t* _px = nullptr;
        int _w = 0, _h = 0, _stride = 0;
    };

} // namespace a3d

#endif // A3D_CANVAS_H_
