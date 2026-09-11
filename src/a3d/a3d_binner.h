// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file a3d_binner.h
 * @brief Assign triangles to screen tiles so each is submitted only where it lands.
 *
 * WHY THIS EXISTS
 *
 *   The naive way to render a large viewport in tiles is to re-submit the
 *   WHOLE scene for every tile. Measured on ESP32-P4: a re-submitted triangle
 *   costs 3.08 us, which is 73% of the 4.21 us a drawn one costs. Nine tiles
 *   therefore pay roughly 8 x 3.08 us per triangle in pure duplication.
 *
 *   A rasterizer processes exactly the triangles it is handed, so it is enough
 *   to work out which tiles each triangle touches and hand each tile only its
 *   own subset. This file does that and nothing else.
 *
 * CORRECTNESS RULE
 *
 *   Binning may only ever be CONSERVATIVE. A triangle assigned to too many tiles
 *   costs time; one assigned to too few loses pixels, and a renderer that
 *   silently drops geometry is worse than a slow one. Every uncertain case here
 *   therefore resolves to "all tiles":
 *     - any vertex behind the near plane (w <= 0), where screen position is
 *       meaningless
 *     - anything the caller could not transform
 *
 *   The screen mapping below must match the rasterizer's exactly: a vertex at
 *   normalized coordinate `v` lands at `L * (v + 1) / 2 - 0.5` pixels, with no
 *   y flip of its own - the flip already lives in the projection. A one-pixel
 *   margin is added on top so rounding can never lose an edge.
 */
#ifndef A3D_BINNER_H_
#define A3D_BINNER_H_

#include "a3d_math.h"

#include <new>
#include <stddef.h>    // size_t: the host build got it transitively, the P4 build did not
#include <stdint.h>

namespace a3d {

class TileBinner
    {
    public:

        TileBinner() = default;
        ~TileBinner() { release(); }

        TileBinner(const TileBinner&) = delete;
        TileBinner& operator=(const TileBinner&) = delete;

        /**
         * Place the working set somewhere other than the default heap. A scene
         * of many parts needs one binner per part, and their combined arrays
         * (5 bytes per triangle, 9 per vertex) outgrow internal RAM well before
         * the geometry itself does. Must be called before configure().
         */
        using AllocFn = void* (*)(size_t);
        using FreeFn  = void  (*)(void*);
        /** Cull back faces while binning, using the SAME convention as the
            backend that will draw them. Pass the backend's culling direction, or
            0 - the default - to bin both sides and leave culling to the draw.
            Setting this to a direction the backend disagrees with removes
            triangles the backend would have kept, so the two must match. */
        void setCulling(int direction) { _cull = direction; }
        int  culling() const { return _cull; }

        void setAllocator(AllocFn alloc, FreeFn release)
            {
            if ((alloc != nullptr) && (release != nullptr))
                { _alloc = alloc; _free = release; }
            }

        /**
         * @param viewW,viewH   full viewport in pixels
         * @param tileW,tileH   tile size; the last row/column may be partial
         * @param maxTriangles  largest mesh that will be binned
         * @param maxVertices   largest vertex count, for the transform cache
         */
        bool configure(int viewW, int viewH, int tileW, int tileH,
                       int maxTriangles, int maxVertices)
            {
            release();
            if ((viewW <= 0) || (viewH <= 0) || (tileW <= 0) || (tileH <= 0) ||
                (maxTriangles <= 0) || (maxVertices <= 0)) return false;

            _viewW = viewW; _viewH = viewH;
            _tileW = tileW; _tileH = tileH;
            _tilesX = (viewW + tileW - 1) / tileW;
            _tilesY = (viewH + tileH - 1) / tileH;
            _nbTiles = _tilesX * _tilesY;
            if ((_tilesX > 255) || (_tilesY > 255)) return false;   // uint8 tile ranges

            _maxTri = maxTriangles;
            _maxVtx = maxVertices;

            // Every member here is a trivial type, so untyped storage is
            // enough and there is nothing to construct or destroy.
            _box     = (TriBox*)_alloc(sizeof(TriBox) * (size_t)maxTriangles);
            _counts  = (uint32_t*)_alloc(sizeof(uint32_t) * (size_t)(_nbTiles + 1));
            _starts  = (uint32_t*)_alloc(sizeof(uint32_t) * (size_t)(_nbTiles + 1));
            _cursor  = (uint32_t*)_alloc(sizeof(uint32_t) * (size_t)_nbTiles);
            _sx      = (float*)_alloc(sizeof(float) * (size_t)maxVertices);
            _sy      = (float*)_alloc(sizeof(float) * (size_t)maxVertices);
            // 1/w as well, so a backend that can take pre-projected vertices
            // gets a complete one. Binning already pays for the divide; keeping
            // the result costs four bytes a vertex and saves the rasterizer from
            // doing the whole projection again, three times per triangle.
            _sw      = (float*)_alloc(sizeof(float) * (size_t)maxVertices);
            _behind  = (uint8_t*)_alloc(sizeof(uint8_t) * (size_t)maxVertices);

            if (!_box || !_counts || !_starts || !_cursor || !_sx || !_sy || !_sw || !_behind)
                { release(); return false; }
            return true;
            }

        void release()
            {
            _free(_box);    _box = nullptr;
            _free(_counts); _counts = nullptr;
            _free(_starts); _starts = nullptr;
            _free(_cursor); _cursor = nullptr;
            _free(_sx);     _sx = nullptr;
            _free(_sy);     _sy = nullptr;
            _free(_sw);     _sw = nullptr;
            _free(_behind); _behind = nullptr;
            _free(_lists);  _lists = nullptr;
            _listCap = 0; _nbTiles = 0; _nbTri = 0;
            }

        bool ready() const { return _box != nullptr; }

        /**
         * What binning already computed, for a backend that can use it.
         *
         * The projection is done once per vertex here and then thrown away;
         * `drawTriangles()` projects three vertices per triangle, so a mesh whose
         * vertices are shared by six triangles is projected six times over.
         * Benchmark C measured that redundancy at 8.2x.
         */
        const float*   screenX() const { return _sx; }
        const float*   screenY() const { return _sy; }
        const float*   screenW() const { return _sw; }   ///< 1/w per vertex
        const uint8_t* behind()  const { return _behind; }
        int tileCount() const { return _nbTiles; }
        int tilesX() const { return _tilesX; }
        int tilesY() const { return _tilesY; }

        void tileRect(int tile, int& x, int& y, int& w, int& h) const
            {
            const int tx = tile % _tilesX;
            const int ty = tile / _tilesX;
            x = tx * _tileW;
            y = ty * _tileH;
            w = (x + _tileW <= _viewW) ? _tileW : (_viewW - x);
            h = (y + _tileH <= _viewH) ? _tileH : (_viewH - y);
            }

        /** Triangles that survived binning into `tile`. */
        uint32_t trianglesInTile(int tile) const
            {
            if ((tile < 0) || (tile >= _nbTiles) || (_lists == nullptr)) return 0;
            return _starts[tile + 1] - _starts[tile];
            }

        /** Triangles submitted across all tiles, versus `nbTriangles` submitted once. */
        uint32_t totalSubmissions() const
            { return (_lists != nullptr) ? _starts[_nbTiles] : 0; }

        /**
         * Bin one indexed triangle list.
         *
         * @param positions   xyz triples in model space, `nbVertices` of them
         * @param indices     3 per triangle
         * @param mvp         projection * view * model, column-major
         * @returns false if the binner was not configured for this size
         */
        bool bin(const float* positions, int nbVertices,
                 const uint16_t* indices, int nbTriangles, const float mvp[16])
            {
            if (!ready() || (nbTriangles > _maxTri) || (nbVertices > _maxVtx)) return false;
            if ((positions == nullptr) || (indices == nullptr)) return false;
            _nbTri = nbTriangles;

            // --- transform every vertex ONCE ------------------------------
            // The vertex cache a per-triangle draw call cannot have: shared
            // vertices are projected a single time instead of once per triangle
            // that uses them.
            // Everything the loop needs is hoisted into locals first.
            //
            // `_behind` is a uint8_t*, and a write through a character type may
            // alias ANY object as far as the compiler is concerned. Storing to
            // it inside the loop therefore forces every member - the matrix, the
            // viewport size, the output pointers - to be reloaded on the next
            // iteration. Measured on ESP32-P4 the pass cost 0.425 us/vertex,
            // against 0.53 us/vertex for the skinning kernel doing roughly five
            // times the arithmetic.
            const float m0 = mvp[0], m4 = mvp[4], m8  = mvp[8],  m12 = mvp[12];
            const float m1 = mvp[1], m5 = mvp[5], m9  = mvp[9],  m13 = mvp[13];
            const float m3 = mvp[3], m7 = mvp[7], m11 = mvp[11], m15 = mvp[15];
            const float halfW = (float)_viewW * 0.5f;
            const float halfH = (float)_viewH * 0.5f;
            float* const sxOut = _sx;
            float* const syOut = _sy;
            float* const swOut = _sw;
            uint8_t* const behindOut = _behind;
            const float* p = positions;

            for (int v = 0; v < nbVertices; v++, p += 3)
                {
                const float x = p[0], y = p[1], z = p[2];
                const float cw = m3 * x + m7 * y + m11 * z + m15;
                if (cw <= 1e-6f)
                    {
                    behindOut[v] = 1;
                    sxOut[v] = 0.0f;
                    syOut[v] = 0.0f;
                    swOut[v] = 0.0f;
                    continue;
                    }
                const float cx = m0 * x + m4 * y + m8  * z + m12;
                const float cy = m1 * x + m5 * y + m9  * z + m13;
                const float inv = 1.0f / cw;
                behindOut[v] = 0;
                sxOut[v] = ((cx * inv) + 1.0f) * halfW - 0.5f;
                syOut[v] = ((cy * inv) + 1.0f) * halfH - 0.5f;
                swOut[v] = inv;
                }

            // --- per-triangle tile range, then count ----------------------
            for (int t = 0; t <= _nbTiles; t++) _counts[t] = 0;

            for (int t = 0; t < nbTriangles; t++)
                {
                const uint16_t i0 = indices[t * 3 + 0];
                const uint16_t i1 = indices[t * 3 + 1];
                const uint16_t i2 = indices[t * 3 + 2];
                TriBox& b = _box[t];

                if ((i0 >= nbVertices) || (i1 >= nbVertices) || (i2 >= nbVertices))
                    { b.skip = 1; continue; }

                if (_behind[i0] || _behind[i1] || _behind[i2])
                    {
                    // Screen position is meaningless for a vertex behind the
                    // eye, so the only safe answer is every tile.
                    b.skip = 0; b.x0 = 0; b.y0 = 0;
                    b.x1 = (uint8_t)(_tilesX - 1); b.y1 = (uint8_t)(_tilesY - 1);
                    }
                else
                    {
                    // A back face is culled HERE, once, instead of inside the
                    // rasterizer once for every tile the triangle touches - and
                    // before it is written into any tile list at all, so roughly
                    // half a closed mesh never costs a bin append. The screen
                    // positions this needs were computed a few lines ago.
                    //
                    // Off by default: turning it on by default would silently
                    // change what every existing test draws.
                    if (_cull != 0)
                        {
                        const float ax = _sx[i1] - _sx[i0], ay = _sy[i1] - _sy[i0];
                        const float bx = _sx[i2] - _sx[i0], by = _sy[i2] - _sy[i0];
                        const float ar = ax * by - ay * bx;
                        if ((_cull > 0) ? (ar <= 0.0f) : (ar >= 0.0f))
                            { b.skip = 1; continue; }
                        }

                    const uint16_t corner[3] = { i0, i1, i2 };
                    float mnx = _sx[i0], mxx = _sx[i0];
                    float mny = _sy[i0], mxy = _sy[i0];
                    for (int k = 1; k < 3; k++)
                        {
                        const float x = _sx[corner[k]];
                        const float y = _sy[corner[k]];
                        if (x < mnx) mnx = x;
                        if (x > mxx) mxx = x;
                        if (y < mny) mny = y;
                        if (y > mxy) mxy = y;
                        }

                    // One pixel of margin: rounding must never lose an edge.
                    int px0 = (int)floorf(mnx) - 1;
                    int py0 = (int)floorf(mny) - 1;
                    int px1 = (int)ceilf(mxx) + 1;
                    int py1 = (int)ceilf(mxy) + 1;

                    if ((px1 < 0) || (py1 < 0) || (px0 >= _viewW) || (py0 >= _viewH))
                        { b.skip = 1; continue; }       // wholly off-screen: free culling

                    if (px0 < 0) px0 = 0;
                    if (py0 < 0) py0 = 0;
                    if (px1 > _viewW - 1) px1 = _viewW - 1;
                    if (py1 > _viewH - 1) py1 = _viewH - 1;

                    b.skip = 0;
                    b.x0 = (uint8_t)(px0 / _tileW);
                    b.y0 = (uint8_t)(py0 / _tileH);
                    b.x1 = (uint8_t)(px1 / _tileW);
                    b.y1 = (uint8_t)(py1 / _tileH);
                    }

                for (int ty = b.y0; ty <= b.y1; ty++)
                    for (int tx = b.x0; tx <= b.x1; tx++)
                        _counts[ty * _tilesX + tx]++;
                }

            // --- prefix sum into a compact CSR layout ---------------------
            // One flat list instead of a per-tile array: memory scales with the
            // number of (triangle, tile) memberships, not tiles x triangles.
            uint32_t total = 0;
            for (int t = 0; t < _nbTiles; t++)
                { _starts[t] = total; total += _counts[t]; _cursor[t] = _starts[t]; }
            _starts[_nbTiles] = total;

            if (total > _listCap)
                {
                _free(_lists);
                _lists = (uint16_t*)_alloc(sizeof(uint16_t) * (size_t)total);
                if (_lists == nullptr) { _listCap = 0; return false; }
                _listCap = total;
                }

            for (int t = 0; t < nbTriangles; t++)
                {
                const TriBox& b = _box[t];
                if (b.skip) continue;
                for (int ty = b.y0; ty <= b.y1; ty++)
                    for (int tx = b.x0; tx <= b.x1; tx++)
                        _lists[_cursor[ty * _tilesX + tx]++] = (uint16_t)t;
                }
            return true;
            }

        /**
         * Expand a tile's triangle list into index triplets ready for
         * `drawTriangles()`.
         *
         * @returns the triangle count written, or -1 if `out` is too small.
         */
        int buildTileIndices(int tile, const uint16_t* srcIndices,
                             uint16_t* out, int maxTriangles) const
            {
            if ((tile < 0) || (tile >= _nbTiles) || (_lists == nullptr)) return 0;
            const uint32_t begin = _starts[tile];
            const uint32_t end = _starts[tile + 1];
            const int n = (int)(end - begin);
            if (n > maxTriangles) return -1;
            for (int k = 0; k < n; k++)
                {
                const uint16_t t = _lists[begin + k];
                out[k * 3 + 0] = srcIndices[t * 3 + 0];
                out[k * 3 + 1] = srcIndices[t * 3 + 1];
                out[k * 3 + 2] = srcIndices[t * 3 + 2];
                }
            return n;
            }

        /** Largest per-tile triangle count, for sizing the scratch index buffer. */
        uint32_t maxTrianglesInAnyTile() const
            {
            uint32_t m = 0;
            for (int t = 0; t < _nbTiles; t++)
                {
                const uint32_t n = trianglesInTile(t);
                if (n > m) m = n;
                }
            return m;
            }

    private:

        struct TriBox { uint8_t x0, y0, x1, y1; uint8_t skip; };

        static float floorf(float v) { const float f = (float)(long)v; return (v < f) ? (f - 1.0f) : f; }
        static float ceilf(float v)  { const float f = (float)(long)v; return (v > f) ? (f + 1.0f) : f; }

        int       _cull = 0;   ///< 0 none, +1 keep positive area, -1 keep negative
        int       _viewW = 0, _viewH = 0, _tileW = 0, _tileH = 0;
        int       _tilesX = 0, _tilesY = 0, _nbTiles = 0;
        int       _maxTri = 0, _maxVtx = 0, _nbTri = 0;

        static void* _defaultAlloc(size_t n) { return ::operator new(n, std::nothrow); }
        static void  _defaultFree(void* p)    { ::operator delete(p, std::nothrow); }

        AllocFn   _alloc = &_defaultAlloc;
        FreeFn    _free  = &_defaultFree;

        TriBox*   _box = nullptr;
        uint32_t* _counts = nullptr;
        uint32_t* _starts = nullptr;
        uint32_t* _cursor = nullptr;
        float*    _sx = nullptr;
        float*    _sy = nullptr;
        float*    _sw = nullptr;
        uint8_t*  _behind = nullptr;
        uint16_t* _lists = nullptr;
        uint32_t  _listCap = 0;
    };

} // namespace a3d

#endif // A3D_BINNER_H_
