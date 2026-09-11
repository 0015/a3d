// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file a3d_load_stl.h
 * @brief Read an STL straight off an SD card and draw it.
 *
 * STL is the format 3D printing runs on, and it is almost the opposite of what
 * a3d's container is. It is a TRIANGLE SOUP: no indices, no shared vertices,
 * no texture coordinates, no materials, no scene graph, no animation, no
 * units, and no agreement about where the origin is. Half the work here is
 * turning that into something the binner and the rasterizer can use; the rest
 * is doing it without a heap the size of the file.
 *
 * WHAT THIS DOES NOT DO
 *
 *   It does not build an `.a3d` container. It does not need to: TileBinner's
 *   `bin()` takes plain arrays, so a mesh loaded here goes through the same
 *   binning, the same culling and the same rasterizer as one that came out of
 *   the container - it simply skips the parts of SceneRuntime that exist for
 *   things STL does not have. If you want quantisation, textures, a node
 *   hierarchy or animation, convert the file offline with
 *   `tools/a3d_export.py` instead; that path has all of it and this one never
 *   will.
 *
 * THE THREE THINGS STL MAKES YOU DECIDE
 *
 *   1. WELDING. A soup of T triangles is 3T vertices, all unique. The
 *      container indexes with uint16, and so does the binner, so 3T must fit
 *      in 65,535 - which caps an unwelded file at 21,845 triangles. Welding
 *      shared vertices typically halves the count to about T/2 and lifts the
 *      ceiling to roughly 130,000 triangles. It is also what makes smooth
 *      normals possible at all, because a vertex has to be shared before it
 *      can average anything.
 *
 *      Welding is on by default and matches vertices EXACTLY, on the bit
 *      pattern of the three coordinates. That is not a tolerance: two facets
 *      whose shared corner differs in the last bit stay separate. In practice
 *      exporters emit the identical value for a shared corner because it came
 *      from one number in the source mesh, so this catches almost everything -
 *      and when it does not, `weldEpsilon` quantises to a grid first. The
 *      cost of a missed weld is a few extra vertices and a faint shading seam,
 *      never a hole.
 *
 *   2. NORMALS. Every STL facet carries a normal, and it is not worth reading:
 *      exporters write zeros, write them un-normalised, or write them
 *      disagreeing with the winding. The normal is recomputed from the winding
 *      here, which is the only version that cannot be wrong.
 *
 *      Vertex normals are the area-weighted average of the faces meeting at
 *      the vertex, which SMOOTHS EVERY EDGE - including the ones that should
 *      be sharp. On an organic model that is what you want; on a mechanical
 *      part, or a building, it rounds the corners off.
 *
 *      For a faceted look load with `weld = false` and draw with
 *      **Shading::Gouraud**, which reads wrong and is right: unwelded, every
 *      vertex carries its own face's normal, and three identical normals
 *      interpolate to a constant across the triangle. That is exact flat
 *      shading. `Shading::Flat` is NOT the way to get it here - on the binned
 *      path it is a constant. See drawTile().
 *
 *   3. UNITS AND ORIGIN. STL says nothing about either. A printable part is
 *      usually in millimetres and sitting on the bed, so its coordinates might
 *      be (0..180, 0..180, 0..250) - nowhere near an origin a camera would
 *      point at. `normalise` centres the model and scales it to a unit radius,
 *      and records what it did in StlInfo so the caller can get back to real
 *      millimetres.
 *
 * MEMORY
 *
 *   Bounded by the uint16 ceiling rather than by the file: positions can never
 *   exceed 65,536 vertices (786 KB), the weld table is a fixed 512 KB, and
 *   indices are 6 bytes a triangle. A 60,000-triangle model costs about
 *   1.7 MB while loading and less afterwards. The file itself is STREAMED and
 *   never held whole.
 *
 * SPEED, WHICH IS THE REAL LIMIT
 *
 *   Loading is not the problem; drawing is. An ESP32-S3 measures about
 *   12.8 us per triangle submitted, so 5,000 triangles is a 64 ms frame and
 *   20,000 is a quarter of a second. Printable STLs are routinely 100,000+.
 *   Decimate offline - `tools/a3d_export.py --max-triangles N` - and expect to view
 *   a few thousand triangles interactively, not a few hundred thousand.
 */
#ifndef A3D_LOAD_STL_H_
#define A3D_LOAD_STL_H_

#include "a3d/a3d_backend.h"
#include "a3d/a3d_binner.h"
#include "a3d/a3d_types.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace a3d
{

namespace stl_detail
{

/**
 * A byte source: a file, or a block of memory.
 *
 * The two exist because the two real cases are a card and a firmware image.
 * `fmemopen` would have covered both with no code, and is not something to
 * rely on across every libc an ESP-IDF build might use, so the handful of
 * calls the parser actually makes are abstracted instead. It needs four:
 * how big, seek, read, and one character at a time for the text tokenizer.
 */
struct Reader
    {
    FILE*          f = nullptr;
    const uint8_t* mem = nullptr;
    size_t         size = 0;
    size_t         pos = 0;

    bool openFile(const char* path)
        {
        f = fopen(path, "rb");
        if (f == nullptr) return false;
        if (fseek(f, 0, SEEK_END) != 0) return false;
        const long n = ftell(f);
        if (n <= 0) return false;
        size = (size_t)n;
        rewind(f);
        pos = 0;
        return true;
        }

    void openMemory(const void* data, size_t n)
        { mem = (const uint8_t*)data; size = n; pos = 0; }

    void close() { if (f != nullptr) { fclose(f); f = nullptr; } }

    void seek(size_t off)
        {
        pos = (off > size) ? size : off;
        if (f != nullptr) fseek(f, (long)pos, SEEK_SET);
        }

    size_t read(void* dst, size_t n)
        {
        if (pos + n > size) n = size - pos;
        if (n == 0) return 0;
        if (f != nullptr) n = fread(dst, 1, n, f);
        else memcpy(dst, mem + pos, n);
        pos += n;
        return n;
        }

    /** -1 at the end. Buffered for the file case; the tokenizer is per-byte. */
    int getch()
        {
        if (pos >= size) return -1;
        pos++;
        if (f != nullptr) return fgetc(f);
        return mem[pos - 1];
        }

    /**
     * Next whitespace-separated token, or false at the end.
     *
     * This replaced fscanf("%s"), which only reads a FILE and would have left
     * the in-memory path without a text parser at all.
     */
    bool token(char* out, size_t cap)
        {
        int c;
        do { c = getch(); } while ((c == ' ') || (c == '\t') || (c == '\r') || (c == '\n'));
        if (c < 0) return false;
        size_t n = 0;
        while (c >= 0 && c != ' ' && c != '\t' && c != '\r' && c != '\n')
            {
            if (n + 1 < cap) out[n++] = (char)c;
            c = getch();
            }
        out[n] = '\0';
        return true;
        }

    bool number(float* out)
        {
        char t[64];
        if (!token(t, sizeof(t))) return false;
        char* end = nullptr;
        const float v = strtof(t, &end);
        if (end == t) return false;
        *out = v;
        return true;
        }
    };

} // namespace stl_detail

/** The uint16 index ceiling, which is what actually bounds everything here. */
inline constexpr uint32_t kStlMaxVertices = 65535u;

struct StlOptions
    {
    /**
     * Refuse a file with more triangles than this rather than spending a
     * minute reading something that will then draw at one frame every few
     * seconds. Raise it deliberately.
     */
    uint32_t maxTriangles = 80000;

    /** Share vertices between facets. See the header comment. */
    bool weld = true;

    /**
     * Grid to quantise onto before matching, in the FILE's own units, or 0 for
     * an exact bit match. Use it when a mesh arrives with corners that differ
     * in the last bits - a value around a thousandth of the smallest feature
     * is the usual choice.
     */
    float weldEpsilon = 0.0f;

    /** Centre on the origin and scale to a unit radius. */
    bool normalise = true;

    /** Drop zero-area facets. They draw nothing and cost per-triangle setup. */
    bool dropDegenerate = true;
    };

/** What the file turned out to contain, and what was done to it. */
struct StlInfo
    {
    bool     ascii         = false;
    uint32_t fileTriangles = 0;   ///< as the file presents them
    uint32_t triangles     = 0;   ///< after degenerate facets were dropped
    uint32_t degenerate    = 0;
    uint32_t vertices      = 0;   ///< after welding
    float    min[3]        = {0, 0, 0};   ///< bounds in the FILE's units
    float    max[3]        = {0, 0, 0};
    float    centre[3]     = {0, 0, 0};   ///< subtracted by normalise
    float    scale         = 1.0f;        ///< multiplied by normalise
    /** Why load() returned false. Never null; "" when it did not. */
    const char* error = "";
    };

/**
 * A triangle mesh read from an STL file.
 *
 * Owns its arrays; not copyable. The allocator is the caller's, so the mesh
 * can be placed in PSRAM on a part that has it - which is where a model of
 * any size belongs.
 */
class StlMesh
    {
    public:
        using AllocFn = void* (*)(size_t);
        using FreeFn  = void  (*)(void*);

        StlMesh() = default;
        ~StlMesh() { release(); }
        StlMesh(const StlMesh&) = delete;
        StlMesh& operator=(const StlMesh&) = delete;

        /** Read from a file - an SD card, or anything else stdio can open. */
        bool load(const char* path,
                  const StlOptions& opt = StlOptions(),
                  AllocFn alloc = nullptr,
                  FreeFn  dealloc = nullptr);

        /**
         * Read from memory, for a model embedded in the firmware with
         * EMBED_FILES. On ESP32 that pointer is memory-mapped flash, so
         * nothing is copied to read it.
         */
        bool loadMemory(const void* data, size_t size,
                        const StlOptions& opt = StlOptions(),
                        AllocFn alloc = nullptr,
                        FreeFn  dealloc = nullptr);

        void release();

        bool valid() const { return _positions != nullptr && _nbTriangles > 0; }

        const float*    positions() const { return _positions; }
        const float*    normals() const { return _normals; }
        const uint16_t* indices() const { return _indices; }
        uint32_t vertexCount() const { return _nbVertices; }
        uint32_t triangleCount() const { return _nbTriangles; }
        const StlInfo& info() const { return _info; }

        /** Bounds AFTER normalise, i.e. the coordinates actually drawn. */
        void bounds(float mn[3], float mx[3]) const
            {
            for (int i = 0; i < 3; i++) { mn[i] = _mn[i]; mx[i] = _mx[i]; }
            }

        /**
         * Bin for one frame. Same call SceneRuntime::binDrawable makes, with
         * the model matrix already folded into `mvp` by the caller.
         */
        bool bin(TileBinner& binner, const float mvp[16]) const
            {
            if (!valid()) return false;
            return binner.bin(_positions, (int)_nbVertices,
                              _indices, (int)_nbTriangles, mvp);
            }

        /**
         * Draw one tile. Mirrors SceneRuntime::drawDrawableTile, including the
         * projected fast path: the binner already transformed every vertex
         * with this frame's matrix, so re-projecting them would be doing the
         * same arithmetic twice and calling the second answer the real one.
         *
         * The caller sets the material; this sets only the shading, because
         * whether normals are attached depends on it.
         *
         * ASK FOR Shading::Gouraud, EVEN FOR A FACETED LOOK. On this path the
         * rasterizer has no world positions, so `Shading::Flat` cannot derive
         * a face normal and falls back to a CONSTANT
         * (`_ambient + _diffuse * 0.75`) - every triangle the same shade, which
         * on a model like a floor plan means no floors and no walls, just a
         * silhouette. Load with `weld = false` instead: every vertex then
         * carries its own face's normal, three identical normals interpolate
         * to a constant across the triangle, and Gouraud gives exact flat
         * shading with the edges intact.
         *
         * @return triangles submitted for this tile.
         */
        int drawTile(const TileBinner& binner, int tile,
                     uint16_t* scratch, int scratchTriangles,
                     Shading shading, IRenderBackend* be) const
            {
            if (!valid() || be == nullptr) return 0;

            const int n = binner.buildTileIndices(tile, _indices, scratch, scratchTriangles);
            if (n <= 0) return n;

            Shading use = shading;
            if ((use == Shading::Gouraud) && (_normals == nullptr)) use = Shading::Flat;
            be->setShading(use, TextureMode::None);

            if (be->supportsProjectedVertices() &&
                (binner.screenX() != nullptr) && (binner.screenW() != nullptr))
                {
                ProjectedBatch pb;
                pb.nbTriangles = n;
                pb.indices  = scratch;
                pb.screenX  = binner.screenX();
                pb.screenY  = binner.screenY();
                pb.screenW  = binner.screenW();
                pb.behind   = binner.behind();
                if ((use != Shading::Unlit) && (_normals != nullptr))
                    { pb.indNormals = scratch; pb.normals = _normals; }
                be->drawTrianglesProjected(pb);
                return n;
                }

            TriangleBatch b;
            b.nbTriangles  = n;
            b.indPositions = scratch;
            b.positions    = _positions;
            if ((use != Shading::Unlit) && (_normals != nullptr))
                { b.indNormals = scratch; b.normals = _normals; }
            be->drawTriangles(b);
            return n;
            }

    private:
        void* _alloc(size_t n)
            { return _allocFn ? _allocFn(n) : malloc(n); }
        void _free(void* p)
            { if (p == nullptr) return; if (_freeFn) _freeFn(p); else free(p); }

        bool _fail(const char* why)
            {
            _info.error = why;
            release();
            return false;
            }

        AllocFn _allocFn = nullptr;
        FreeFn  _freeFn = nullptr;

        float*    _positions = nullptr;
        float*    _normals = nullptr;
        uint16_t* _indices = nullptr;
        uint32_t* _table = nullptr;      ///< weld hash, freed after loading
        uint32_t  _tableMask = 0;

        uint32_t _nbVertices = 0;
        uint32_t _nbTriangles = 0;
        float    _mn[3] = {0, 0, 0};
        float    _mx[3] = {0, 0, 0};
        StlInfo  _info;

        // --- welding -------------------------------------------------------
        static uint32_t _hash3(const float v[3])
            {
            uint32_t h = 2166136261u;
            for (int i = 0; i < 3; i++)
                {
                uint32_t b;
                // +0.0 and -0.0 are equal and have different bit patterns, so
                // a bitwise hash puts them in different buckets and the weld
                // misses every vertex that happens to sit on an axis.
                const float f = (v[i] == 0.0f) ? 0.0f : v[i];
                memcpy(&b, &f, sizeof(b));
                h ^= b; h *= 16777619u;
                }
            return h;
            }

        /** Index of `v`, adding it if new. kStlMaxVertices + 1 when full. */
        uint32_t _intern(const float v[3])
            {
            if (_table == nullptr)              // welding off: every corner is new
                {
                if (_nbVertices > kStlMaxVertices) return kStlMaxVertices + 1;
                const uint32_t id = _nbVertices++;
                memcpy(_positions + (size_t)id * 3, v, 3 * sizeof(float));
                return id;
                }

            uint32_t slot = _hash3(v) & _tableMask;
            for (;;)
                {
                const uint32_t got = _table[slot];
                if (got == 0xFFFFFFFFu) break;
                const float* p = _positions + (size_t)got * 3;
                if ((p[0] == v[0]) && (p[1] == v[1]) && (p[2] == v[2])) return got;
                slot = (slot + 1) & _tableMask;
                }
            if (_nbVertices > kStlMaxVertices) return kStlMaxVertices + 1;
            const uint32_t id = _nbVertices++;
            memcpy(_positions + (size_t)id * 3, v, 3 * sizeof(float));
            _table[slot] = id;
            return id;
            }

        static void _quantise(float v[3], float eps)
            {
            if (eps <= 0.0f) return;
            const float inv = 1.0f / eps;
            for (int i = 0; i < 3; i++) v[i] = floorf(v[i] * inv + 0.5f) * eps;
            }

        bool _addFacet(const float t[9], const StlOptions& opt);
        bool _finish(const StlOptions& opt);
        bool _read(stl_detail::Reader& rd, const StlOptions& opt);
    };

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

inline void StlMesh::release()
    {
    _free(_positions); _positions = nullptr;
    _free(_normals);   _normals = nullptr;
    _free(_indices);   _indices = nullptr;
    _free(_table);     _table = nullptr;
    _nbVertices = 0;
    _nbTriangles = 0;
    }

/** One facet: three corners, already in file units. */
inline bool StlMesh::_addFacet(const float t[9], const StlOptions& opt)
    {
    float c[3][3];
    for (int k = 0; k < 3; k++)
        {
        for (int i = 0; i < 3; i++) c[k][i] = t[k * 3 + i];
        _quantise(c[k], opt.weldEpsilon);
        }

    if (opt.dropDegenerate)
        {
        // Zero AREA, not "two corners equal": a sliver with three distinct but
        // collinear corners is just as empty and just as expensive to set up.
        const float ux = c[1][0] - c[0][0], uy = c[1][1] - c[0][1], uz = c[1][2] - c[0][2];
        const float vx = c[2][0] - c[0][0], vy = c[2][1] - c[0][1], vz = c[2][2] - c[0][2];
        const float nx = uy * vz - uz * vy;
        const float ny = uz * vx - ux * vz;
        const float nz = ux * vy - uy * vx;
        if ((nx * nx + ny * ny + nz * nz) <= 0.0f)
            {
            _info.degenerate++;
            return true;                       // dropped, not an error
            }
        }

    uint16_t id[3];
    for (int k = 0; k < 3; k++)
        {
        const uint32_t v = _intern(c[k]);
        if (v > kStlMaxVertices) return false; // out of index space
        id[k] = (uint16_t)v;
        }

    const size_t at = (size_t)_nbTriangles * 3;
    _indices[at + 0] = id[0];
    _indices[at + 1] = id[1];
    _indices[at + 2] = id[2];
    _nbTriangles++;

    for (int k = 0; k < 3; k++)
        for (int i = 0; i < 3; i++)
            {
            const float x = c[k][i];
            if (x < _info.min[i]) _info.min[i] = x;
            if (x > _info.max[i]) _info.max[i] = x;
            }
    return true;
    }

/** Normals, normalisation and the final trim. */
inline bool StlMesh::_finish(const StlOptions& opt)
    {
    if (_nbTriangles == 0) return _fail("no triangles");

    // Centre and scale. STL says nothing about units or origin, so a printable
    // part sitting on a print bed is nowhere near where a camera would look.
    float sc = 1.0f;
    float ctr[3] = {0, 0, 0};
    if (opt.normalise)
        {
        float ext = 0.0f;
        for (int i = 0; i < 3; i++)
            {
            ctr[i] = 0.5f * (_info.min[i] + _info.max[i]);
            const float half = 0.5f * (_info.max[i] - _info.min[i]);
            if (half > ext) ext = half;
            }
        sc = (ext > 1e-20f) ? (1.0f / ext) : 1.0f;
        for (uint32_t v = 0; v < _nbVertices; v++)
            {
            float* p = _positions + (size_t)v * 3;
            for (int i = 0; i < 3; i++) p[i] = (p[i] - ctr[i]) * sc;
            }
        }
    for (int i = 0; i < 3; i++)
        {
        _info.centre[i] = ctr[i];
        _mn[i] = (_info.min[i] - ctr[i]) * sc;
        _mx[i] = (_info.max[i] - ctr[i]) * sc;
        }
    _info.scale = sc;
    _info.triangles = _nbTriangles;
    _info.vertices = _nbVertices;

    // Vertex normals: the area-weighted average of the faces at each vertex.
    // The cross product's LENGTH is twice the triangle's area, so adding the
    // un-normalised cross weights by area for free - and area weighting is
    // what stops a fan of slivers outvoting the one big face beside it.
    //
    // Built whether or not welding is on, and that is not waste. With welding
    // OFF no vertex is shared, so each one's "average" is its own face's
    // normal - and three identical normals across a triangle interpolate to a
    // constant, which is exact flat shading. That is the only way to get it on
    // the binned path; see the note on drawTile().
        {
        _normals = (float*)_alloc((size_t)_nbVertices * 3 * sizeof(float));
        if (_normals == nullptr) return _fail("out of memory for normals");
        memset(_normals, 0, (size_t)_nbVertices * 3 * sizeof(float));

        for (uint32_t t = 0; t < _nbTriangles; t++)
            {
            const uint16_t* id = _indices + (size_t)t * 3;
            const float* a = _positions + (size_t)id[0] * 3;
            const float* b = _positions + (size_t)id[1] * 3;
            const float* c = _positions + (size_t)id[2] * 3;
            const float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
            const float vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
            const float nx = uy * vz - uz * vy;
            const float ny = uz * vx - ux * vz;
            const float nz = ux * vy - uy * vx;
            for (int k = 0; k < 3; k++)
                {
                float* n = _normals + (size_t)id[k] * 3;
                n[0] += nx; n[1] += ny; n[2] += nz;
                }
            }
        for (uint32_t v = 0; v < _nbVertices; v++)
            {
            float* n = _normals + (size_t)v * 3;
            const float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len > 1e-20f) { const float k = 1.0f / len; n[0] *= k; n[1] *= k; n[2] *= k; }
            else { n[0] = 0.0f; n[1] = 1.0f; n[2] = 0.0f; }
            }
        }

    _free(_table);
    _table = nullptr;
    return true;
    }

inline bool StlMesh::load(const char* path, const StlOptions& opt,
                          AllocFn alloc, FreeFn dealloc)
    {
    release();
    _allocFn = alloc; _freeFn = dealloc;
    _info = StlInfo();
    for (int i = 0; i < 3; i++) { _info.min[i] = 1e30f; _info.max[i] = -1e30f; }

    if (path == nullptr) return _fail("no path");

    stl_detail::Reader rd;
    if (!rd.openFile(path)) { rd.close(); return _fail("cannot open"); }
    const bool ok = _read(rd, opt);
    rd.close();
    return ok;
    }

inline bool StlMesh::loadMemory(const void* data, size_t size, const StlOptions& opt,
                                AllocFn alloc, FreeFn dealloc)
    {
    release();
    _allocFn = alloc; _freeFn = dealloc;
    _info = StlInfo();
    for (int i = 0; i < 3; i++) { _info.min[i] = 1e30f; _info.max[i] = -1e30f; }

    if (data == nullptr || size == 0) return _fail("empty buffer");

    stl_detail::Reader rd;
    rd.openMemory(data, size);
    return _read(rd, opt);
    }

inline bool StlMesh::_read(stl_detail::Reader& rd, const StlOptions& opt)
    {
    // ---- which flavour ----------------------------------------------------
    // NOT "does it start with the word solid": plenty of binary writers put
    // that in the 80-byte header, and a file that lies about being ASCII is
    // parsed as text until it hits a byte that is not a number. The size is
    // the honest test - a binary STL is exactly 84 + 50 per triangle, and no
    // ASCII file lands on that by accident.
    unsigned char head[84];
    rd.seek(0);
    const size_t got = rd.read(head, sizeof(head));
    uint32_t claimed = 0;
    bool binary = false;
    if (got == sizeof(head))
        {
        memcpy(&claimed, head + 80, 4);
        binary = ((uint64_t)84u + 50ull * claimed) == (uint64_t)rd.size;
        }
    _info.ascii = !binary;

    uint32_t upperTris = 0;
    if (binary)
        {
        upperTris = claimed;
        }
    else
        {
        // ASCII carries no count, so it is read twice: once to count "vertex"
        // tokens, once to keep them. Two passes over a text STL is slow and
        // the format is why - a binary file of the same model is a fifth the
        // size and needs one pass.
        rd.seek(0);
        char tok[64];
        uint32_t verts = 0;
        while (rd.token(tok, sizeof(tok)))
            if (strcmp(tok, "vertex") == 0) verts++;
        upperTris = verts / 3;
        }

    if (upperTris == 0) return _fail("no facets");
    if (upperTris > opt.maxTriangles) return _fail("too many triangles");

    // ---- storage ----------------------------------------------------------
    // Bounded by the uint16 ceiling, not by the file: welding can never
    // produce more than 65,536 distinct vertices, so the position buffer has a
    // fixed maximum however large the model is.
    uint64_t maxVerts = (uint64_t)upperTris * 3u;
    if (opt.weld && (maxVerts > (uint64_t)kStlMaxVertices + 1u))
        maxVerts = (uint64_t)kStlMaxVertices + 1u;
    if (!opt.weld && (maxVerts > (uint64_t)kStlMaxVertices + 1u))
        return _fail("unwelded file needs more than 65535 vertices");

    _positions = (float*)_alloc((size_t)maxVerts * 3 * sizeof(float));
    _indices   = (uint16_t*)_alloc((size_t)upperTris * 3 * sizeof(uint16_t));
    if (_positions == nullptr || _indices == nullptr)
        return _fail("out of memory for the mesh");

    if (opt.weld)
        {
        uint32_t cap = 1024;
        while ((uint64_t)cap < maxVerts * 2ull) cap <<= 1;
        _table = (uint32_t*)_alloc((size_t)cap * sizeof(uint32_t));
        if (_table == nullptr) return _fail("out of memory for the weld table");
        memset(_table, 0xFF, (size_t)cap * sizeof(uint32_t));
        _tableMask = cap - 1;
        }

    // ---- read -------------------------------------------------------------
    bool ok = true;
    if (binary)
        {
        _info.fileTriangles = claimed;
        rd.seek(84);
        // A facet is 50 bytes, so its floats are unaligned three times out of
        // four. They are memcpy'd out rather than cast, which is free on this
        // compiler and defined everywhere.
        unsigned char rec[50];
        for (uint32_t i = 0; i < claimed && ok; i++)
            {
            if (rd.read(rec, sizeof(rec)) != sizeof(rec)) { ok = false; break; }
            float t[9];
            memcpy(t, rec + 12, sizeof(t));   // skip the facet normal: recomputed
            ok = _addFacet(t, opt);
            }
        }
    else
        {
        rd.seek(0);
        char tok[64];
        float t[9];
        int have = 0;
        uint32_t facets = 0;
        while (ok && rd.token(tok, sizeof(tok)))
            {
            if (strcmp(tok, "vertex") != 0) continue;
            if (!rd.number(&t[have * 3]) || !rd.number(&t[have * 3 + 1]) ||
                !rd.number(&t[have * 3 + 2])) { ok = false; break; }
            if (++have == 3)
                {
                have = 0;
                facets++;
                ok = _addFacet(t, opt);
                }
            }
        _info.fileTriangles = facets;
        }

    if (!ok)
        return _fail(_nbVertices > kStlMaxVertices ? "more than 65535 vertices after welding"
                                                   : "truncated or malformed file");
    return _finish(opt);
    }

} // namespace a3d

#endif // A3D_LOAD_STL_H_
