// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file a3d_soft_backend.h
 * @brief The rasterizer, implementing IRenderBackend.
 *
 * What it does
 * ------------
 * Flat and Gouraud shading, perspective-correct nearest-neighbour texturing,
 * a z-buffer, back-face culling and near-plane clipping. Roughly 1,000 lines,
 * and it draws every fixture in this project.
 *
 * The one thing it can do that a general interface cannot
 * -------------------------------------------------------
 * The binner already projects every vertex to screen space to decide which
 * tiles a triangle touches, and then throws the result away. A draw call that
 * takes model space has to project the same vertices again, three per triangle.
 * This backend can be handed what binning already computed - see
 * drawTrianglesProjected() - and on the P4 that is worth 1.13x with a
 * pixel-identical result, because the binner projected with the very matrix the
 * draw would have used.
 *
 * Where it is exact and where it is not
 * -------------------------------------
 * The projected path has no clip space left to clip in, so a triangle with a
 * vertex at or behind the eye is skipped there; the binner flags those and the
 * caller keeps the unprojected path for them. Flat shading is also approximate
 * on that path - there are no world positions to take a face normal from -
 * while Gouraud is exact, and Gouraud is what a textured scene uses.
 *
 * ---------------------------
 * None of these is checked at runtime, and none of them announces itself when
 * it is violated: this backend degrades silently by design, because the
 * alternative is a branch in a loop that runs per pixel. That makes the list
 * below the only warning there is. Read it before you conclude the renderer is
 * broken.
 *
 *  - **Framebuffer is RGB565, always.** setTarget() takes `uint16_t*` and there
 *    is no other pixel type. A panel that wants RGB888 needs a conversion in
 *    the present path, not a setting here.
 *
 *  - **Textures must be powers of two in BOTH dimensions, and RGB565**
 *    (`fmt::TEXEL_RGB565`, format 0). Wrapping is by mask, hence the first;
 *    the sampler reads `uint16_t`, hence the second. createTexture() returns an
 *    INVALID handle for anything else, SceneRuntime::bind() stores that handle
 *    and still returns true, and the model then draws untextured with nothing
 *    said anywhere. `docs/ASSET_FORMAT.md` declares TEXEL_RGB24 and TEXEL_RGB32
 *    as legal container formats; this backend does not implement them. The
 *    exporter already emits RGB565 at power-of-two sizes, so an asset that came
 *    through `tools/a3d_export.py` is safe - a hand-built texture is not.
 *
 *  - **The z-buffer holds 1/w, so 0 is FAR.** clearZBuffer() memsets to 0 for
 *    that reason. Clearing it to 0xFFFF by habit - which is what a depth buffer
 *    usually wants - makes every fragment fail the test and the screen stays
 *    empty.
 *
 *  - **65,535 vertices per batch.** TriangleBatch indices are `uint16_t`. The
 *    exporter enforces this and says so; a batch you fill in yourself is not
 *    checked anywhere.
 *
 *  - **setViewProjection() and setPerspective()/setLookAt() are EXCLUSIVE.**
 *    They write the same `_vp`, and the camera pair recomposes it from `_proj`
 *    and `_view` - so a setLookAt() after a setViewProjection() discards the
 *    matrix you supplied, silently. Pick one of the two ways to aim the camera
 *    and stay with it.
 *
 *  - **A missing attribute silently downgrades the draw.** Shading::Gouraud
 *    with null `normals`, or a TextureMode with a null `texcoords` or an
 *    invalid texture handle, draws without that feature rather than refusing.
 *    supports() will NOT catch this: it reports what was compiled in, not what
 *    this batch carries, and it returns true in both cases above.
 */
#ifndef A3D_SOFT_BACKEND_H_
#define A3D_SOFT_BACKEND_H_

#include "a3d/a3d_backend.h"
#include "a3d/a3d_board.h"
#include "a3d/a3d_math.h"

#include <math.h>
#include <new>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace a3d {

/**
 * @tparam color_t  16-bit framebuffer pixel. Only RGB565 is implemented.
 */
class SoftBackend : public IRenderBackend
    {
    public:

        /**
         * A texture is the asset's own pixels plus its dimensions. Nothing is
         * copied: the container is mapped and the pixels are read where they
         * lie, which is why textures cost no RAM.
         *
         * Wrapping is by mask, so both dimensions must be powers of two - which
         * the exporter already guarantees (TEXFLAG_POW2).
         */
        struct Texture
            {
            // A handle this backend did not create must be REFUSED, not read.
            //
            // An image type from another backend can easily begin with a pixel
            // pointer followed by two ints, which is also how this struct begins
            // - so a foreign handle read as a Texture sampled it as if it were
            // one and produced a picture that was MOSTLY right. The face came
            // out black and the ears the wrong colour; nothing failed, nothing
            // crashed, and nothing said so.
            //
            uint32_t magic = kMagic;
            const uint16_t* px = nullptr;
            int w = 0, h = 0, wmask = 0, hmask = 0, wshift = 0;

            // Optional smaller copies of the same image. Level 0 is the fields
            // above; a level that was never registered is null and the sampler
            // falls back to the largest one below it.
            static constexpr int kMaxLevels = 8;
            const uint16_t* lv[kMaxLevels] = { nullptr };
            int lvW[kMaxLevels] = { 0 };
            int lvWmask[kMaxLevels] = { 0 };
            int lvHmask[kMaxLevels] = { 0 };
            int lvShift[kMaxLevels] = { 0 };
            int levels = 1;
            };

        static constexpr uint32_t kMagic = 0xA3D50F70u;

        // Cost attribution, compiled in only for a benchmark. Each point stops
        // or skips part of the per-triangle path, so the difference between two
        // of them prices one stage. The pictures are wrong on purpose; only the
        // times mean anything. Off, these expand to nothing - a runtime flag in
        // the vertex loop measured a 5.6% "gain" that was the flag itself.
#ifdef A3D_BENCH_PROBES
    public:
        void setProbe(int v) { _probe = v; }
    private:
        int _probe = 0;
        #define A3D_PROBE_STOP(n) do { if (_probe == (n)) return; } while (0)
        #define A3D_PROBE_SKIP(n) if (_probe == (n)) continue
        #define A3D_PROBE_IS(n)   (_probe == (n))
    public:   // restore the access this block interrupted
#else
        #define A3D_PROBE_STOP(n) do { } while (0)
        #define A3D_PROBE_SKIP(n) do { } while (0)
        #define A3D_PROBE_IS(n)   false
#endif

        // ------------------------------------------------------------------
        // Surface: a small image that is a window onto a larger viewport, which
        // is what the tile loop needs.
        // ------------------------------------------------------------------

        void setViewportSize(int w, int h) { _vpW = w; _vpH = h; }

        /**
         * The pixels to draw into, and how far apart their rows are.
         *
         * If the viewport has not been set yet, this sets it to the target's
         * own size. A full-screen target IS the viewport, which is what every
         * caller outside the tile loop wants, and leaving the two unrelated
         * made the first program a new reader writes draw NOTHING: `_vpW` and
         * `_vpH` start at 0, so every vertex projects into a screen of width
         * zero, and it does that silently, with no diagnostic and an exit code
         * of 0. Every example and test in this tree calls setViewportSize()
         * because they were all written from the inside; the omission is only
         * reachable from outside.
         *
         * A tile renderer is unaffected: it sets the viewport to the whole
         * screen FIRST and then points setTarget() at each tile, so the
         * viewport is already non-zero and nothing is seeded.
         *
         */
        void setTarget(uint16_t* pixels, int w, int h, int stride)
            {
            _fb = pixels; _w = w; _h = h; _stride = (stride > 0) ? stride : w;
            if ((_vpW <= 0) || (_vpH <= 0)) { _vpW = w; _vpH = h; }
            }

        /** Where this target's top-left sits in the viewport. */
        void setOffset(int x, int y) { _ox = x; _oy = y; }

        /** The z-buffer must cover the target, not the viewport. */
        void setZBuffer(uint16_t* z) { _zb = z; }

        void clearZBuffer()
            { if (_zb != nullptr) memset(_zb, 0, (size_t)_w * _h * sizeof(uint16_t)); }

        /** Composed outside, exactly as the binner receives it. */
        void setViewProjection(const float vp[16]) { mat4Copy(vp, _vp); _mvpDirty = true; }

        /** Camera, without a renderer to borrow one from.

            The stored matrix has its y row negated, because this rasterizer
            writes into rows that run DOWN the screen while clip space has +y
            going up. Keeping the flip in the matrix rather than in the pixel
            loop costs nothing per frame and is what the culling direction and
            the tile binner were both measured against. */
        void setPerspective(float fovyDeg, float aspect, float zNear, float zFar)
            {
            mat4Perspective(fovyDeg, aspect, zNear, zFar, _proj);
            mat4InvertY(_proj);
            _near = (zNear > 1e-5f) ? zNear : 1e-5f;
            _composeVP();
            }

        /** Orthographic camera. Same y flip as the perspective one, for the same
            reason, so culling and binning do not change meaning with the
            projection. */
        void setOrtho(float left, float right, float bottom, float top,
                      float zNear, float zFar)
            {
            mat4Ortho(left, right, bottom, top, zNear, zFar, _proj);
            mat4InvertY(_proj);
            _near = (zNear > 1e-5f) ? zNear : 1e-5f;
            _composeVP();
            }

        void setLookAt(const float eye[3], const float center[3], const float up[3])
            {
            mat4LookAt(eye, center, up, _view);
            _composeVP();
            }

        /** Where a model-space point lands on screen, using the SAME matrices
            this backend is about to draw with.

            Without this an application keeps its own copy of the projection and
            has to remember to change both - and the two drift silently, because
            a label drawn a few pixels off still looks like a label.
            Returns false for a point behind the eye or outside the depth range,
            where there is no screen position to report. */
        bool projectPoint(float x, float y, float z, int* sx, int* sy) const
            {
            // Forwarded rather than restated. This function and the stateless
            // helper answered the same question in two places for as long as
            // both existed, which is the arrangement that lets them drift - and
            // the three copies in applications had already drifted from each
            // other by an epsilon, a missing depth check and a rounding rule.
            float mvp[16];
            currentMVP(mvp);
            return projectToPixel(mvp, x, y, z, _vpW, _vpH, sx, sy);
            }

        /** The view matrix on its own - lighting and normal transforms want it
            separately from the projection. */
        const float* viewMatrix() const { return _view; }
        const float* projectionMatrix() const { return _proj; }

        /**
         * The near distance the projection was built with.
         *
         * Clipping against the EYE instead leaves geometry between the eye and
         * the near plane in the picture, which does not belong there - measured
         * as a 4.2% silhouette difference at close range that SURVIVED adding a
         * clipper, because the clipper was cutting at the wrong plane.
         */
        void setNearPlane(float n) { _near = (n > 1e-5f) ? n : 1e-5f; }

        /**
         * Which screen-space winding faces the camera: +1, -1, or 0 for none.
         *
         * Not a constant, because the sign depends on the projection: the stored
         * one has its y row negated so that clip space matches framebuffer rows,
         * and that flips screen-space winding. Guessing produced an empty image
         * that still took 0.7 ms, which looks like a slow renderer rather than a
         * culled one.
         */
        void setCulling(int direction) { _cull = direction; }

        void setLightDirection(float x, float y, float z)
            {
            const float n = sqrtf(x * x + y * y + z * z);
            const float inv = (n > 1e-8f) ? (1.0f / n) : 1.0f;
            _lx = x * inv; _ly = y * inv; _lz = z * inv;
            _lightToModel();
            }

        // ------------------------------------------------------------------
        // IRenderBackend
        // ------------------------------------------------------------------

        bool supports(Shading s) const override
            { return (s == Shading::Unlit) || (s == Shading::Flat) ||
                     (s == Shading::Gouraud); }

        bool supports(TextureMode m) const override
            { return (m == TextureMode::None) || (m == TextureMode::Perspective); }

        void setModelMatrix(const float m16[16]) override
            {
            mat4Copy(m16, _model);
            _mvpDirty = true;
            // Normals need the model rotation without its scale. Rows divided by
            // their own length, exactly as the skinning kernel does it, and for
            // the same reason: a normal that arrives short darkens the surface
            // in proportion and nothing reports it.
            for (int r = 0; r < 3; r++)
                {
                const float x = m16[0 * 4 + r], y = m16[1 * 4 + r], z = m16[2 * 4 + r];
                const float l2 = x * x + y * y + z * z;
                const float inv = (l2 > 1e-20f) ? invSqrt(l2) : 1.0f;
                _nrm[r * 3 + 0] = x * inv;
                _nrm[r * 3 + 1] = y * inv;
                _nrm[r * 3 + 2] = z * inv;
                }
            _lightToModel();
            }

        void setMaterial(const Material& m) override
            {
            _r = m.color[0]; _g = m.color[1]; _b = m.color[2];
            _ambient = m.ambient; _diffuse = m.diffuse;
            _specular = m.specular;
            _specExp = (m.specularExponent > 0) ? m.specularExponent : 0;
            }

        void setShading(Shading s, TextureMode m) override { _shading = s; _texMode = m; }

        // Changing one term of a material without restating the other four.
        // Every one of these is what a caller would otherwise write by copying
        // the current Material, editing a field and setting it back.
        void setMaterialColor(float r, float g, float b) { _r = r; _g = g; _b = b; }
        void setMaterialAmbient(float v)  { _ambient = v; }
        void setMaterialDiffuse(float v)  { _diffuse = v; }
        void setMaterialSpecular(float v) { _specular = v; }
        void setMaterialSpecularExponent(int e) { _specExp = (e > 0) ? e : 0; }


        /** Bilinear texture filtering. Off by default: it reads four texels where
            nearest reads one, and at one texel per pixel it buys nothing.
            Worth turning on where the texture is magnified. */
        void setTextureFilter(bool bilinear) { _bilinear = bilinear; }
        bool textureFilter() const { return _bilinear; }

        /** Push mip selection toward smaller levels, in whole levels.
         *
         * Textbook selection asks which level puts about one texel on a pixel.
         * That is the right question when the texture is in fast memory and the
         * wrong one when it is not.
         *
         * Measured, ESP32-P4, the globe example at steady state with a 4 MB map
         * in PSRAM filling a 720 px panel:
         *     unbiased        89.55 ms a frame
         *     biased one down 73.82 ms a frame     (18% faster)
         *
         * Default is 0 so the renderer does the standard thing; an application
         * whose texture does not fit in cache should measure and set 1.
         *
         *
         * Two earlier numbers in this file were wrong: one measured warm-up, the
         * other measured a globe that a matrix bug was drawing at 1/3 the size.
         */
        void setMipBias(int levels) { _mipBias = levels; }
        int mipBias() const { return _mipBias; }

        /** Whole-draw transparency, 0..1. Below 1 the triangles blend with what
            is already in the target instead of replacing it.

            Selected at the triangle, not at the pixel: an opacity test inside
            the per-pixel loop costs the opaque case too, and the opaque case is
            almost every triangle.

            Depth still gates the pixel, so a translucent surface hides what is
            behind it in the DEPTH sense; it does not sort. Draw back to front
            and turn depth writes off for the translucent pass if that matters. */
        void setOpacity(float o)
            { _opacity = (o < 0.0f) ? 0.0f : ((o > 1.0f) ? 1.0f : o); }
        float opacity() const { return _opacity; }

        void currentMVP(float out[16]) const override
            {
            // The binner projects with whatever this returns, so it has to be
            // the matrix the rasterizer below actually uses - composed the same
            // way, in the same order.
            mat4Multiply(_vp, _model, out);
            }

        // ------------------------------------------------------------------
        // Where this backend's OWN allocations come from
        //
        // Everything below asks for memory with plain `operator new` unless
        // this is set, and on a part with PSRAM the IDF heap sends any
        // allocation above CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (16 KB by
        // default) to the slow memory. The arrays that go through here are the
        // DECODED VERTEX BUFFERS a skinned mesh rewrites every frame, so that
        // default silently puts the hottest write in the build in PSRAM and
        // nothing says so. CesiumMan's 3,273 vertices are 105 KB.
        //
        // `TileBinner::setAllocator` has the same two arguments in the same
        // order, deliberately: an application that decides where its geometry
        // lives should be able to say it the same way twice.
        //
        // Set it BEFORE SceneRuntime::bind(), which is what allocates. Both
        // pointers must be non-null or the pair is ignored; the free function
        // is never called with null.
        // ------------------------------------------------------------------
        using AllocFn = void* (*)(size_t);
        using FreeFn  = void  (*)(void*);

        void setAllocator(AllocFn alloc, FreeFn release)
            {
            if ((alloc != nullptr) && (release != nullptr))
                { _allocFn = alloc; _freeFn = release; }
            }

        float* allocVec3Array(int n) override
            { return (n > 0) ? (float*)_alloc((size_t)n * 3 * sizeof(float)) : nullptr; }
        void freeVec3Array(float* p) override { _free(p); }

        float* allocVec2Array(int n) override
            { return (n > 0) ? (float*)_alloc((size_t)n * 2 * sizeof(float)) : nullptr; }
        void freeVec2Array(float* p) override { _free(p); }

        // Textures are stage 2. Refusing the handle rather than ignoring it
        // means a scene that needs one downgrades through supports() instead of
        // rendering untextured and looking like a bug.
        TextureHandle wrapNativeTexture(const void* t) override
            {
            // Only a Texture this backend made. Anything else is another
            // backend's object and must not be dereferenced.
            TextureHandle h;
            if ((t != nullptr) && (((const Texture*)t)->magic == kMagic)) h.id = t;
            return h;
            }

        /** Null unless the handle really is one of ours. */
        static Texture* _texture(TextureHandle h)
            {
            Texture* t = (Texture*)const_cast<void*>(h.id);
            return ((t != nullptr) && (t->magic == kMagic)) ? t : nullptr;
            }

        TextureHandle createTexture(int w, int h, int format, const void* pixels) override
            {
            TextureHandle out;
            if ((pixels == nullptr) || (w <= 0) || (h <= 0) || (format != 0)) return out;
            if (((w & (w - 1)) != 0) || ((h & (h - 1)) != 0)) return out;  // wrap is by mask
            Texture* t = (Texture*)_alloc(sizeof(Texture));
            if (t == nullptr) return out;
            t->magic = kMagic;
            t->px = (const uint16_t*)pixels;
            t->w = w; t->h = h; t->wmask = w - 1; t->hmask = h - 1;
            int sh = 0; while ((1 << sh) < w) sh++;
            t->wshift = sh;
            t->lv[0] = t->px; t->lvW[0] = w;
            t->lvWmask[0] = t->wmask; t->lvHmask[0] = t->hmask; t->lvShift[0] = sh;
            t->levels = 1;
            out.id = t;
            return out;
            }

        /**
         * Register a smaller copy of an existing texture as mip level `level`.
         *
         * The rasterizer then picks a level per TRIANGLE from how much texture
         * it covers per pixel. Without this a magnified map is sampled at one
         * texel per pixel and every pixel misses cache into the full-size
         * image - which is the shape of the problem, not the detail: the detail
         * is finer than the panel resolves and is paid for anyway.
         *
         *
         * Both dimensions must be powers of two, as for level 0.
         */
        bool setMipLevel(TextureHandle h, int level, int w, int hgt, const void* pixels)
            {
            Texture* t = _texture(h);
            if ((t == nullptr) || (pixels == nullptr)) return false;
            if ((level < 1) || (level >= Texture::kMaxLevels)) return false;
            if ((w <= 0) || (hgt <= 0)) return false;
            if (((w & (w - 1)) != 0) || ((hgt & (hgt - 1)) != 0)) return false;
            t->lv[level] = (const uint16_t*)pixels;
            t->lvW[level] = w;
            t->lvWmask[level] = w - 1;
            t->lvHmask[level] = hgt - 1;
            int sh = 0; while ((1 << sh) < w) sh++;
            t->lvShift[level] = sh;
            if (level + 1 > t->levels) t->levels = level + 1;
            return true;
            }

        void destroyTexture(TextureHandle h) override
            {
            Texture* t = _texture(h);
            if (t != nullptr) { t->magic = 0; _free(t); }
            }

        void drawTriangles(const TriangleBatch& b) override
            {
            if ((_fb == nullptr) || (b.nbTriangles <= 0) ||
                (b.indPositions == nullptr) || (b.positions == nullptr)) return;
            if (_mvpDirty) { mat4Multiply(_vp, _model, _mvp); _mvpDirty = false; }

            // Which inner loop runs is decided ONCE, here, and becomes a
            // template argument. A runtime test inside a loop that runs per
            // pixel measured slower than doing the work unconditionally - the
            // branch defeats the scheduling.
            const bool g = (_shading == Shading::Gouraud) &&
                           (b.normals != nullptr) && (b.indNormals != nullptr);
            const bool t = (_texMode != TextureMode::None) && b.texture.valid() &&
                           (b.texcoords != nullptr) && (b.indTexcoords != nullptr);
            // No depth buffer is a property of the whole draw, so it is decided
            // here rather than tested at every pixel. A scene that culls its own
            // back faces - a convex globe, a terrain seen from above - has no
            // use for a depth test and should not pay for one 300,000 times.
            if (_zb == nullptr)
                {
                if (g && t)       _draw<true,  true , true>(b);
                else if (g)       _draw<true,  false, true>(b);
                else if (t)       _draw<false, true , true>(b);
                else              _draw<false, false, true>(b);
                }
            else
                {
                if (g && t)       _draw<true,  true , false>(b);
                else if (g)       _draw<true,  false, false>(b);
                else if (t)       _draw<false, true , false>(b);
                else              _draw<false, false, false>(b);
                }
            }

        /**
         * A line segment in MODEL space, occluded by whatever is already in the
         * depth buffer.
         *
         * This is not a 2D line with projected endpoints. That is what an
         * application writes when the renderer will not do it, and it cannot be
         * hidden by geometry - so the caller ends up deciding visibility by
         * hand, which only works for a convex shape and silently draws through
         * everything else.
         *
         *
         * `depthBias` nudges the segment toward the viewer in depth units, for
         * a wireframe drawn ON the surface it describes: without it the line and
         * the face it lies on fight over every pixel.
         */
        void drawLine3D(const float a[3], const float b[3], uint16_t color,
                        float depthBias = 0.0f)
            {
            if (_fb == nullptr) return;
            if (_mvpDirty) { mat4Multiply(_vp, _model, _mvp); _mvpDirty = false; }
            const float* M = _mvp;

            float cx[2], cy[2], cw[2];
            const float* P[2] = { a, b };
            for (int k = 0; k < 2; k++)
                {
                cx[k] = M[0]*P[k][0] + M[4]*P[k][1] + M[8] *P[k][2] + M[12];
                cy[k] = M[1]*P[k][0] + M[5]*P[k][1] + M[9] *P[k][2] + M[13];
                cw[k] = M[3]*P[k][0] + M[7]*P[k][1] + M[11]*P[k][2] + M[15];
                }

            // Near-plane clip of the SEGMENT, not rejection of it. Dropping a
            // line because one end is behind the eye makes it blink out as the
            // camera passes it, which reads as a bug in the data.
            const bool in0 = (cw[0] > _near), in1 = (cw[1] > _near);
            if (!in0 && !in1) return;
            if (!in0 || !in1)
                {
                const int out = in0 ? 1 : 0, keep = in0 ? 0 : 1;
                const float t = (cw[keep] - _near) / (cw[keep] - cw[out]);
                cx[out] = cx[keep] + (cx[out] - cx[keep]) * t;
                cy[out] = cy[keep] + (cy[out] - cy[keep]) * t;
                cw[out] = _near;
                }

            const float halfW = (float)_vpW * 0.5f;
            const float halfH = (float)_vpH * 0.5f;
            float sx[2], sy[2], iw[2];
            for (int k = 0; k < 2; k++)
                {
                iw[k] = 1.0f / cw[k];
                sx[k] = ((cx[k] * iw[k]) + 1.0f) * halfW - 0.5f;
                sy[k] = ((cy[k] * iw[k]) + 1.0f) * halfH - 0.5f;
                }

            int x0 = (int)(sx[0] + 0.5f), y0 = (int)(sy[0] + 0.5f);
            const int x1 = (int)(sx[1] + 0.5f), y1 = (int)(sy[1] + 0.5f);
            int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
            int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
            const int stepx = (x0 < x1) ? 1 : -1;
            const int stepy = (y0 < y1) ? 1 : -1;
            const int steps = (dx > dy) ? dx : dy;

            // 1/w is linear in SCREEN space, so stepping it along the pixels is
            // exact rather than an approximation.
            const float iw0 = iw[0];
            const float diw = (steps > 0) ? ((iw[1] - iw[0]) / (float)steps) : 0.0f;

            dy = -dy;
            int err = dx + dy;
            for (int i = 0; ; i++)
                {
                const int px = x0 - _ox, py = y0 - _oy;
                if ((px >= 0) && (py >= 0) && (px < _w) && (py < _h))
                    {
                    const uint16_t zi = _packDepth(iw0 + diw * (float)i + depthBias);
                    uint16_t* zp = (_zb != nullptr)
                                 ? (_zb + (size_t)py * _w + px) : nullptr;
                    if ((zp == nullptr) || (zi > *zp))
                        {
                        _fb[(size_t)py * _stride + px] = color;
                        if (zp != nullptr) *zp = zi;
                        }
                    }
                if ((x0 == x1) && (y0 == y1)) break;
                if (i > (_vpW + _vpH) * 2) break;   // a clipped segment cannot be longer
                const int e2 = err * 2;
                if (e2 >= dy) { err += dy; x0 += stepx; }
                if (e2 <= dx) { err += dx; y0 += stepy; }
                }
            }

        /** A batch of segments: `indices` holds two vertex indices per segment. */
        void drawLines3D(int nbSegments, const uint16_t* indices,
                         const float* positions, uint16_t color,
                         float depthBias = 0.0f)
            {
            if ((indices == nullptr) || (positions == nullptr)) return;
            for (int i = 0; i < nbSegments; i++)
                drawLine3D(positions + (size_t)indices[i*2+0] * 3,
                           positions + (size_t)indices[i*2+1] * 3, color, depthBias);
            }

        bool supportsProjectedVertices() const override { return true; }

        /**
         * Triangles whose vertices the binner already projected.
         *
         * No matrix, no perspective divide, no per-triangle transform at all -
         * the three vertices are looked up and rasterized. That is the one thing
         * owning the rasterizer buys that no backend interface can offer, and it
         * attacks the geometry half of the frame directly.
         *
         * A triangle with a vertex at or behind the eye is SKIPPED here: screen
         * space has no clip space left in it, so there is nothing to clip
         * against. The caller keeps the unprojected path for those - and the
         * binner already flags them, so this is a decision made on data, not a
         * hope that it never happens.
         */
        void drawTrianglesProjected(const ProjectedBatch& b) override
            {
            if ((_fb == nullptr) || (b.nbTriangles <= 0) || (b.indices == nullptr) ||
                (b.screenX == nullptr) || (b.screenY == nullptr) ||
                (b.screenW == nullptr)) return;

            const bool g = (_shading == Shading::Gouraud) &&
                           (b.normals != nullptr) && (b.indNormals != nullptr);
            const bool t = (_texMode != TextureMode::None) && b.texture.valid() &&
                           (b.texcoords != nullptr) && (b.indTexcoords != nullptr);
            if (_zb == nullptr)
                {
                if (g && t)       _drawP<true,  true , true>(b);
                else if (g)       _drawP<true,  false, true>(b);
                else if (t)       _drawP<false, true , true>(b);
                else              _drawP<false, false, true>(b);
                }
            else
                {
                if (g && t)       _drawP<true,  true , false>(b);
                else if (g)       _drawP<true,  false, false>(b);
                else if (t)       _drawP<false, true , false>(b);
                else              _drawP<false, false, false>(b);
                }
            }

    private:

        template <bool GOURAUD, bool TEXTURED, bool NOZ>
        void _drawP(const ProjectedBatch& b)
            {
            const Texture* tex = TEXTURED ? _texture(b.texture) : nullptr;
            if (TEXTURED && ((tex == nullptr) || (tex->px == nullptr)))
                { _drawP<GOURAUD, false, NOZ>(b); return; }

            for (int t = 0; t < b.nbTriangles; t++)
                {
                const uint16_t i0 = b.indices[t * 3 + 0];
                const uint16_t i1 = b.indices[t * 3 + 1];
                const uint16_t i2 = b.indices[t * 3 + 2];
                if (b.behind != nullptr &&
                    (b.behind[i0] | b.behind[i1] | b.behind[i2])) continue;

                SV q[3];
                const uint16_t idx[3] = { i0, i1, i2 };
                for (int k = 0; k < 3; k++)
                    {
                    const uint16_t i = idx[k];
                    q[k].x  = b.screenX[i];
                    q[k].y  = b.screenY[i];
                    q[k].iw = b.screenW[i];
                    q[k].li = 1.0f;
                    q[k].uw = 0.0f;
                    q[k].vw = 0.0f;
                    if (GOURAUD)
                        {
                        const uint16_t ni = b.indNormals[t * 3 + k];
                        const float* n = b.normals + (size_t)ni * 3;
                        q[k].li = _lit(n[0], n[1], n[2]);
                        }
                    if (TEXTURED)
                        {
                        const uint16_t ti = b.indTexcoords[t * 3 + k];
                        q[k].uw = b.texcoords[(size_t)ti * 2 + 0] * q[k].iw;
                        q[k].vw = b.texcoords[(size_t)ti * 2 + 1] * q[k].iw;
                        }
                    }

                const float uvA = TEXTURED
                    ? _uvArea(b.texcoords, b.indTexcoords, t, tex) : 0.0f;

                float flatCol = 1.0f;
                if (!GOURAUD && (_shading != Shading::Unlit))
                    {
                    // No world positions here, so the face normal comes from the
                    // screen-space winding: a face turned away from the light
                    // cannot be told apart this way, and flat shading on this
                    // path is therefore an approximation. Gouraud is exact.
                    flatCol = _ambient + _diffuse * 0.75f;
                    if (flatCol > 1.0f) flatCol = 1.0f;
                    }

                _rast<GOURAUD, TEXTURED, NOZ>(q[0], q[1], q[2], flatCol, tex, uvA);
                }
            }

        /** One vertex after projection, carrying whatever the shader needs. */
        struct SV
            {
            float x, y;      // screen
            float iw;        // 1/w, linear in screen space
            float li;        // light intensity, Gouraud only
            float uw, vw;    // u/w, v/w - linear in screen space, Perspective only
            };

        /** One vertex before the divide, so the near plane can be clipped. */
        struct CV
            {
            float cx, cy, cw;
            float li, u, v;
            };

        static CV _lerpCV(const CV& a, const CV& b, float s)
            {
            CV o;
            o.cx = a.cx + (b.cx - a.cx) * s;
            o.cy = a.cy + (b.cy - a.cy) * s;
            o.cw = a.cw + (b.cw - a.cw) * s;
            o.li = a.li + (b.li - a.li) * s;
            o.u  = a.u  + (b.u  - a.u ) * s;
            o.v  = a.v  + (b.v  - a.v ) * s;
            return o;
            }

        /** Light a world-space normal. */
        /** Carries the light into the space the normals are already in.

            Lighting wants -(N*n) . L, and (N*n) . L is exactly n . (N^T * L),
            so the 3x3 can be applied to the ONE light vector when the matrix
            changes instead of to every normal of every vertex of every
            triangle. The two are equal, not approximately equal: nothing here
            renormalises N*n, which is what would have broken the identity.
            Three vertices a triangle, six triangles a vertex, and the same
            triangle again in every tile it touches - it was the same nine
            multiplies and six adds every time. */
        void _lightToModel()
            {
            _lm[0] = _nrm[0]*_lx + _nrm[3]*_ly + _nrm[6]*_lz;
            _lm[1] = _nrm[1]*_lx + _nrm[4]*_ly + _nrm[7]*_lz;
            _lm[2] = _nrm[2]*_lx + _nrm[5]*_ly + _nrm[8]*_lz;

            // The half vector gets the same treatment as the light: computed
            // once here, so the per-vertex cost of a specular highlight is one
            // dot product and a handful of squarings.
            //
            // The view direction is taken as the camera's forward axis rather
            // than as eye-minus-vertex. That is the distant-viewer
            // approximation, and it is what makes the half vector a constant
            // per model matrix instead of a per-vertex normalize.
            const float vx = _view[2], vy = _view[6], vz = _view[10];
            float hx = vx - _lx, hy = vy - _ly, hz = vz - _lz;
            const float l2 = hx*hx + hy*hy + hz*hz;
            if (l2 > 1e-12f)
                {
                const float inv = 1.0f / sqrtf(l2);
                hx *= inv; hy *= inv; hz *= inv;
                }
            _hm[0] = _nrm[0]*hx + _nrm[3]*hy + _nrm[6]*hz;
            _hm[1] = _nrm[1]*hx + _nrm[4]*hy + _nrm[7]*hz;
            _hm[2] = _nrm[2]*hx + _nrm[5]*hy + _nrm[8]*hz;
            }

        /** `x` raised to a non-negative integer power, by squaring.
            An exponent of 16 costs four multiplies; `powf` would cost a call. */
        static float _ipow(float x, int e)
            {
            float r = 1.0f;
            while (e > 0)
                {
                if (e & 1) r *= x;
                x *= x;
                e >>= 1;
                }
            return r;
            }

        float _lit(float nx, float ny, float nz) const
            {
            float d = -(nx * _lm[0] + ny * _lm[1] + nz * _lm[2]);
            if (d < 0.0f) d = 0.0f;
            float l = _ambient + _diffuse * d;
            if ((_specular > 0.0f) && (_specExp > 0) && (d > 0.0f))
                {
                // Only where the surface faces the light: a highlight on a face
                // turned away from it is a highlight coming through the object.
                const float sdot = nx * _hm[0] + ny * _hm[1] + nz * _hm[2];
                if (sdot > 0.0f) l += _specular * _ipow(sdot, _specExp);
                }
            return (l > 1.0f) ? 1.0f : l;
            }

        template <bool GOURAUD, bool TEXTURED, bool NOZ>
        void _draw(const TriangleBatch& b)
            {
            const float* M = _mvp;
            const float halfW = (float)_vpW * 0.5f;
            const float halfH = (float)_vpH * 0.5f;
            const Texture* tex = TEXTURED ? _texture(b.texture) : nullptr;
            if (TEXTURED && ((tex == nullptr) || (tex->px == nullptr)))
                {
                // Draw it untextured rather than not at all. A drawable that
                // vanishes is harder to notice than one that is the wrong
                // colour, and neither is a reason to lose the geometry.
                _draw<GOURAUD, false, NOZ>(b);
                return;
                }

            for (int t = 0; t < b.nbTriangles; t++)
                {
                CV cv[3];
                float flatCol = 1.0f;
                for (int k = 0; k < 3; k++)
                    {
                    const uint16_t i = b.indPositions[t * 3 + k];
                    const float* p = b.positions + (size_t)i * 3;
                    cv[k].cx = M[0]*p[0] + M[4]*p[1] + M[8] *p[2] + M[12];
                    cv[k].cy = M[1]*p[0] + M[5]*p[1] + M[9] *p[2] + M[13];
                    cv[k].cw = M[3]*p[0] + M[7]*p[1] + M[11]*p[2] + M[15];
                    cv[k].li = 1.0f;
                    cv[k].u = cv[k].v = 0.0f;
                    if (GOURAUD)
                        {
                        const uint16_t ni = b.indNormals[t * 3 + k];
                        const float* n = b.normals + (size_t)ni * 3;
                        cv[k].li = _lit(n[0], n[1], n[2]);
                        }
                    if (TEXTURED)
                        {
                        const uint16_t ti = b.indTexcoords[t * 3 + k];
                        cv[k].u = b.texcoords[(size_t)ti * 2 + 0];
                        cv[k].v = b.texcoords[(size_t)ti * 2 + 1];
                        }
                    }

                if (!GOURAUD && (_shading != Shading::Unlit) && A3D_PROBE_IS(3))
                    { flatCol = 0.8f; }
                else if (!GOURAUD && (_shading != Shading::Unlit))
                    {
                    // Flat: the face normal, from the positions this triangle
                    // was built from - before clipping, which does not change it.
                    const uint16_t i0 = b.indPositions[t*3+0];
                    const uint16_t i1 = b.indPositions[t*3+1];
                    const uint16_t i2 = b.indPositions[t*3+2];
                    const float* p0 = b.positions + (size_t)i0 * 3;
                    const float* p1 = b.positions + (size_t)i1 * 3;
                    const float* p2 = b.positions + (size_t)i2 * 3;
                    const float ux = p1[0]-p0[0], uy = p1[1]-p0[1], uz = p1[2]-p0[2];
                    const float vx = p2[0]-p0[0], vy = p2[1]-p0[1], vz = p2[2]-p0[2];
                    float nx = uy*vz - uz*vy, ny = uz*vx - ux*vz, nz = ux*vy - uy*vx;
                    const float n2 = nx*nx + ny*ny + nz*nz;
                    if (n2 > 1e-20f)
                        { const float s = invSqrt(n2); flatCol = _lit(nx*s, ny*s, nz*s); }
                    }

                const float uvA = TEXTURED
                    ? _uvArea(b.texcoords, b.indTexcoords, t, tex) : 0.0f;

                // Almost every triangle is entirely in front of the near plane,
                // and for those the clipper is pure overhead: six-float vertex
                // records, a Sutherland-Hodgman loop and a copy, per triangle.
                // Adding it cost 20% on the P4 even in the flat path, where it
                // never had anything to cut.
                if ((cv[0].cw > _near) && (cv[1].cw > _near) && (cv[2].cw > _near))
                    {
                    SV q[3];
                    for (int k = 0; k < 3; k++)
                        {
                        const float inv = 1.0f / cv[k].cw;
                        q[k].x = ((cv[k].cx * inv) + 1.0f) * halfW - 0.5f;
                        q[k].y = ((cv[k].cy * inv) + 1.0f) * halfH - 0.5f;
                        q[k].iw = inv;
                        q[k].li = cv[k].li;
                        q[k].uw = cv[k].u * inv;
                        q[k].vw = cv[k].v * inv;
                        }
                    A3D_PROBE_SKIP(4);
                    _rast<GOURAUD, TEXTURED, NOZ>(q[0], q[1], q[2], flatCol, tex, uvA);
                    continue;
                    }

                // --- near plane -------------------------------------------
                // Sutherland-Hodgman against cw > eps. A triangle crossing it
                // becomes a quad, which is two triangles - stage 1 DROPPED these
                // and the bench showed it as a 4% silhouette difference at close
                // range. Dropping is cheaper and wrong.
                const float kNear = _near;
                CV poly[4];
                int np = 0;
                for (int k = 0; k < 3; k++)
                    {
                    const CV& A = cv[k];
                    const CV& B = cv[(k + 1) % 3];
                    const bool ain = A.cw > kNear, bin = B.cw > kNear;
                    if (ain) poly[np++] = A;
                    if (ain != bin)
                        {
                        const float s = (kNear - A.cw) / (B.cw - A.cw);
                        poly[np++] = _lerpCV(A, B, s);
                        }
                    if (np >= 4) break;
                    }
                if (np < 3) continue;

                SV sv[4];
                for (int k = 0; k < np; k++)
                    {
                    const float inv = 1.0f / poly[k].cw;
                    sv[k].x = ((poly[k].cx * inv) + 1.0f) * halfW - 0.5f;
                    sv[k].y = ((poly[k].cy * inv) + 1.0f) * halfH - 0.5f;
                    sv[k].iw = inv;
                    sv[k].li = poly[k].li;
                    sv[k].uw = poly[k].u * inv;
                    sv[k].vw = poly[k].v * inv;
                    }

                for (int f = 2; f < np; f++)
                    _rast<GOURAUD, TEXTURED, NOZ>(sv[0], sv[f - 1], sv[f], flatCol, tex, uvA);
                }
            }

        /** Picks the opacity path. One place instead of six call sites that
            each spelled the same choice out twice. */
        template <bool GOURAUD, bool TEXTURED, bool NOZ>
        inline void _rast(const SV& a, const SV& b, const SV& c,
                          float flatCol, const Texture* tex, float uvA)
            {
            if (_opacity < 1.0f) _rasterize<GOURAUD, TEXTURED, true,  NOZ>(a, b, c, flatCol, tex, uvA);
            else                 _rasterize<GOURAUD, TEXTURED, false, NOZ>(a, b, c, flatCol, tex, uvA);
            }

        template <bool GOURAUD, bool TEXTURED, bool BLEND, bool NOZ>
        A3D_HOT void _rasterize(const SV& v0, const SV& v1, const SV& v2,
                        float flatCol, const Texture* tex, float uvArea = 0.0f)
            {
            const float sx[3] = { v0.x, v1.x, v2.x };
            const float sy[3] = { v0.y, v1.y, v2.y };
            const float sw[3] = { v0.iw, v1.iw, v2.iw };

            const float ax = sx[1] - sx[0], ay = sy[1] - sy[0];
            const float bx = sx[2] - sx[0], by = sy[2] - sy[0];
            float area = ax * by - ay * bx;
            if (_cull > 0) { if (area <= 0.0f) return; }
            else if (_cull < 0) { if (area >= 0.0f) return; }
            else if (area == 0.0f) return;
            const float flip = (area < 0.0f) ? -1.0f : 1.0f;
            area *= flip;
            const float invArea = 1.0f / area;

            // A pixel belongs to the box only if its CENTRE can be in the
            // triangle, and centres sit at x + 0.5. So the first candidate
            // column is the smallest x with x + 0.5 >= minX, and the last is
            // the largest with x + 0.5 <= maxX - which is ceil(min - 0.5)
            // and floor(max - 0.5), not floor(min) and ceil(max).
            // floor/ceil of the raw extents can be two rows too tall: a
            // triangle spanning y 10.6 to 12.4 covers only row 11, and the
            // loose form iterates rows 10 through 13 to discover that. The
            // horizontal-edge clamp below already uses this exact expression.
            int x0 = iCeil (_min3(sx[0], sx[1], sx[2]) - 0.5f);
            int x1 = iFloor(_max3(sx[0], sx[1], sx[2]) - 0.5f);
            int y0 = iCeil (_min3(sy[0], sy[1], sy[2]) - 0.5f);
            int y1 = iFloor(_max3(sy[0], sy[1], sy[2]) - 0.5f);
            if (x0 < _ox) x0 = _ox;
            if (y0 < _oy) y0 = _oy;
            if (x1 > _ox + _w - 1) x1 = _ox + _w - 1;
            if (y1 > _oy + _h - 1) y1 = _oy + _h - 1;
            if ((x0 > x1) || (y0 > y1)) return;
            A3D_PROBE_STOP(1);

            const float e0dx = flip * (sy[0] - sy[1]), e0dy = flip * (sx[1] - sx[0]);
            const float e1dx = flip * (sy[1] - sy[2]), e1dy = flip * (sx[2] - sx[1]);
            const float e2dx = flip * (sy[2] - sy[0]), e2dy = flip * (sx[0] - sx[2]);
                {
                // A horizontal edge constrains y and nothing else, so it can be
                // folded into the row range once instead of tested on every row.
                // The row loop then has no edge that varies with y alone, which
                // is what lets the three side tests below become branch-free
                // and lets e0row..e2row stop advancing per row entirely.
                //
                // e_k = (py - sy_k) * ekdy when ekdx is zero, and e_k >= 0 is
                // the half-plane the triangle lives in, so with py = y + 0.5:
                //     ekdy > 0  ->  y >= sy_k - 0.5
                //     ekdy < 0  ->  y <= sy_k - 0.5
                // Written out three times rather than looped over two stack
                // arrays: this runs for every triangle to fire for almost none,
                // so what it costs when it does nothing is what it costs.
                #define A3D_YCLAMP(edx, edy, syk)                                    \
                    if (((edx) <= 1e-12f) && ((edx) >= -1e-12f))                     \
                        {                                                            \
                        if ((edy) > 0.0f)                                            \
                            { const int yy = iCeil ((syk) - 0.5f); if (yy > y0) y0 = yy; } \
                        else if ((edy) < 0.0f)                                       \
                            { const int yy = iFloor((syk) - 0.5f); if (yy < y1) y1 = yy; } \
                        }
                A3D_YCLAMP(e0dx, e0dy, sy[0])
                A3D_YCLAMP(e1dx, e1dy, sy[1])
                A3D_YCLAMP(e2dx, e2dy, sy[2])
                #undef A3D_YCLAMP
                if (y0 > y1) return;
                }

            const float px0 = (float)x0 + 0.5f, py0 = (float)y0 + 0.5f;
            float e0row = (px0 - sx[0]) * e0dx + (py0 - sy[0]) * e0dy;
            float e1row = (px0 - sx[1]) * e1dx + (py0 - sy[1]) * e1dy;
            float e2row = (px0 - sx[2]) * e2dx + (py0 - sy[2]) * e2dy;

            // A triangle has at most two edges pushing each bound: the three
            // dk terms are (sy0-sy1), (sy1-sy2), (sy2-sy0), which sum to zero,
            // so they cannot all share a sign. Two slots a side is enough, and
            // the row loop then folds four values rather than six.
            // An unused slot holds the bounding box's own edge rather than an
            // infinity. The integer clamp below already pins the span to
            // [x0, x1], so seeding the slots with that same bound makes the box
            // fall out of the reduction: two compares a row instead of four, and
            // two fewer values live across the loop in a function that spends a
            // quarter of its instructions spilling.
            const float x0f = (float)x0, x1f = (float)x1;
            float lo0 = x0f, lo1 = x0f, dlo0 = 0.0f, dlo1 = 0.0f;
            float hi0 = x1f, hi1 = x1f, dhi0 = 0.0f, dhi1 = 0.0f;
            {
            const float dk[3] = { e0dx, e1dx, e2dx };
            const float ek[3] = { e0row, e1row, e2row };
            const float ey[3] = { e0dy, e1dy, e2dy };
            int nl = 0, nh = 0;
            for (int k = 0; k < 3; k++)
                {
                if ((dk[k] > 1e-12f) || (dk[k] < -1e-12f))
                    {
                    const float inv = 1.0f / dk[k];
                    const float xv  = (float)x0 - ek[k] * inv;
                    const float dv  = -ey[k] * inv;
                    if (dk[k] > 0.0f)
                        {
                        if (nl == 0) { lo0 = xv; dlo0 = dv; } else { lo1 = xv; dlo1 = dv; }
                        nl++;
                        }
                    else
                        {
                        if (nh == 0) { hi0 = xv; dhi0 = dv; } else { hi1 = xv; dhi1 = dv; }
                        nh++;
                        }
                    }
                }
            }

            // 1/area folded into the nine edge terms once, instead of into each
            // of the four gradients three times over. The span slopes above are
            // a ratio of two edge terms and so are untouched by the scaling; the
            // sign tests survive it because the area is positive by here.
            // z = w * _zScale exactly, so a textured triangle carries
            // one plane where it used to carry two - and the pixel loop trades
            // an add for a multiply to get z back, freeing three registers in a
            // function that was spending a quarter of its instructions spilling.
            const bool zFromW = TEXTURED;
            float zBase = 0, dzdx = 0, dzdy = 0;
            float wBase = 0, dwdx = 0, dwdy = 0;
            float lBase = 0, dldx = 0, dldy = 0;
            float uBase = 0, dudx = 0, dudy = 0, vBase = 0, dvdx = 0, dvdy = 0;
            const float ts = zFromW ? _zScale : 1.0f;

            // The plane through the three vertices, solved directly. An attribute is affine in screen space, so
            //     [ax ay][dadx]   [a1-a0]
            //     [bx by][dady] = [a2-a0]
            // and the 2x2 inverse is the signed area already in hand. The
            // nine scaled edge terms disappear: they were a preamble paid in
            // full whether the triangle carried four attributes or one, and
            // a flat untextured triangle carries one.
            const float sInv = flip * invArea;   // 1 / signed area
            const float rx = px0 - sx[0], ry = py0 - sy[0];

            #define A3D_GRAD(a0, a1, a2, base, gx, gy)                          \
                do {                                                            \
                    const float p0 = (a0);                                       \
                    const float d1 = (a1) - p0, d2 = (a2) - p0;                  \
                    gx = (d1 * by - d2 * ay) * sInv;                             \
                    gy = (d2 * ax - d1 * bx) * sInv;                             \
                    base = p0 + gx * rx + gy * ry;                               \
                } while (0)

            if (zFromW)
                {
                A3D_GRAD(sw[0]*_zScale, sw[1]*_zScale, sw[2]*_zScale, wBase, dwdx, dwdy);
                }
            else
                {
                A3D_GRAD(sw[0]*_zScale, sw[1]*_zScale, sw[2]*_zScale, zBase, dzdx, dzdy);
                if (TEXTURED) A3D_GRAD(sw[0], sw[1], sw[2], wBase, dwdx, dwdy);
                }
            if (GOURAUD) A3D_GRAD(v0.li, v1.li, v2.li, lBase, dldx, dldy);
            if (TEXTURED)
                {
                A3D_GRAD(v0.uw*ts, v1.uw*ts, v2.uw*ts, uBase, dudx, dudy);
                A3D_GRAD(v0.vw*ts, v1.vw*ts, v2.vw*ts, vBase, dvdx, dvdy);
                }
            #undef A3D_GRAD
            // One level for the whole triangle. Choosing per pixel would need
            // the UV derivatives at every pixel to buy a difference no panel
            // shows on a triangle this size.
            // The UV area arrives already computed, from where the RAW texture
            // coordinates were still to hand. Recovering them here would mean
            // dividing u/w back by 1/w three times a triangle, and that measured
            // at 4.5 ms of a 60 ms frame on the globe example.
            MipView mip;
            if (TEXTURED) mip = _pickLevel(tex, uvArea, area);

            const uint16_t flatRGB = _pack(_r * flatCol, _g * flatCol, _b * flatCol);
            // The material colour, packed ONCE. The untextured Gouraud path used
            // to call _pack per pixel: three float multiplies, six clamps and
            // three float-to-int conversions, to produce a colour that only
            // differs by a scalar. Measured on the globe example at 0.28 us a
            // pixel against 0.062 for the paths that were already integer.
            const uint16_t baseRGB = _pack(_r, _g, _b);
            const int blendA = BLEND ? (int)(_opacity * 256.0f) : 256;
            constexpr bool noZ = NOZ;

            A3D_PROBE_STOP(2);

            // Row starts, stepped rather than multiplied out. The two integer
            // multiplies this replaces were the only two left in the function.
            uint16_t* fbRow = _fb + (size_t)(y0 - _oy) * (size_t)_stride;
            uint16_t* zbRow = noZ ? nullptr : (_zb + (size_t)(y0 - _oy) * (size_t)_w);

            for (int y = y0; y <= y1; y++)
                {
                const float lo = fmaxf(lo0, lo1);
                const float hi = fminf(hi0, hi1);
                    {
                    int xs = iCeil(lo), xe = iFloor(hi);
                    if (xs < x0) xs = x0;
                    if (xe > x1) xe = x1;
                    if (!A3D_PROBE_IS(5))
                    if (xs <= xe)
                        {
                        // The span is written here rather than called.
                        //
                        // As a function it took fourteen scalar arguments, once
                        // per scanline. On x86 that is free; on the P4 there are
                        // eight argument registers and the rest go through the
                        // stack, every row. Inlining it by hand recovered what
                        // the clipper and the shader templates had cost.
                        const int nSkip = xs - x0;
                        float z  = zFromW ? 0.0f : (zBase + dzdx * (float)nSkip);
                        float w  = TEXTURED ? (wBase + dwdx * (float)nSkip) : 0.0f;
                        float li = GOURAUD  ? (lBase + dldx * (float)nSkip) : 0.0f;
                        // The shade too: a scalar that was converted to an
                        // integer once per pixel to modulate an already-integer
                        // colour.
                        int32_t lifx = GOURAUD ? (int32_t)(li * 65536.0f) : 0;
                        const int32_t dlifx = GOURAUD ? (int32_t)(dldx * 65536.0f) : 0;
                        float uw = TEXTURED ? (uBase + dudx * (float)nSkip) : 0.0f;
                        float vw = TEXTURED ? (vBase + dvdx * (float)nSkip) : 0.0f;

                        uint16_t* row = fbRow + (xs - _ox);
                        uint16_t* zp  = noZ ? nullptr : (zbRow + (xs - _ox));

                        float u = 0.0f, v = 0.0f, du = 0.0f, dv = 0.0f;
                        int32_t ufx = 0, vfx = 0, dufx = 0, dvfx = 0;
                        if (TEXTURED)
                            {
                            const float iw = (w > 1e-20f) ? (1.0f / w) : 0.0f;
                            u = uw * iw; v = vw * iw;
                            }

                        constexpr int SEG = 16;
                        int x = xs;
                        while (x <= xe)
                            {
                            int seg = xe - x + 1;
                            if (seg > SEG) seg = SEG;
                            if (TEXTURED)
                                {
                                const float fseg = (float)seg;
                                const float wEnd  = w  + dwdx * fseg;
                                const float uwEnd = uw + dudx * fseg;
                                const float vwEnd = vw + dvdx * fseg;
                                const float iwEnd = (wEnd > 1e-20f) ? (1.0f / wEnd) : 0.0f;
                                const float uEnd = uwEnd * iwEnd;
                                const float vEnd = vwEnd * iwEnd;
                                const float inv = 1.0f / fseg;
                                du = (uEnd - u) * inv;
                                dv = (vEnd - v) * inv;

                                // u/w, v/w and w are read HERE and nowhere else
                                // when there is no depth buffer, so they advance
                                // a segment at a time instead of a pixel at a
                                // time. They MUST advance: leaving them frozen
                                // makes every segment recompute the same
                                // endpoint, and the texture is right for the
                                // first sixteen pixels of a span and wrong after.
                                if (NOZ) { w = wEnd; uw = uwEnd; vw = vwEnd; }

                                // Into texel units once, here, instead of on
                                // every pixel. Wrapped into [0,1) first so the
                                // 16.16 integer part cannot run out of range.
                                const float uw0 = u - fFloor(u);
                                const float vw0 = v - fFloor(v);
                                ufx  = (int32_t)(uw0 * mip.w * 65536.0f);
                                vfx  = (int32_t)(vw0 * mip.h * 65536.0f);
                                dufx = (int32_t)(du * mip.w * 65536.0f);
                                dvfx = (int32_t)(dv * mip.h * 65536.0f);
                                }
                            for (int k = 0; k < seg; k++)
                                {
                                // Packing the depth is inside the NOZ guard, not
                                // relying on the optimiser to see that nothing
                                // reads it: a clamp and a float-to-int on every
                                // pixel of every triangle is not something to
                                // leave to dead-code elimination.
                                uint16_t zi = 0;
                                if (!NOZ)
                                    {
                                    const float zv = zFromW ? w : z;
                                    zi = (zv <= 0.0f) ? 0
                                       : ((zv >= 65535.0f) ? (uint16_t)65535 : (uint16_t)zv);
                                    }
                                if (NOZ || (zi > *zp))
                                    {
                                    uint16_t c;
                                    if (TEXTURED)
                                        {
                                        c = _bilinear ? _sampleBilinearFx(mip, ufx, vfx)
                                                      : _sampleNearestFx(mip, ufx, vfx);
                                        if (GOURAUD) c = _modulate(c, lifx >> 8);
                                        }
                                    else if (GOURAUD) c = _modulate(baseRGB, lifx >> 8);
                                    else              c = flatRGB;
                                    *row = BLEND ? _blendPix(*row, c, blendA) : c;
                                    if (!noZ) *zp = zi;
                                    }
                                row++;
                                if (!noZ) zp++;
                                if (!zFromW) z += dzdx;
                                if (TEXTURED)
                                    {
                                    ufx += dufx; vfx += dvfx;
                                    u += du; v += dv;
                                    if (!NOZ) { w += dwdx; uw += dudx; vw += dvdx; }
                                    }
                                if (GOURAUD) lifx += dlifx;
                                }
                            x += seg;
                            }
                        }
                    }
                lo0 += dlo0; lo1 += dlo1; hi0 += dhi0; hi1 += dhi1;
                fbRow += _stride;
                if (!noZ) zbRow += _w;
                if (!zFromW) zBase += dzdy;
                if (GOURAUD) lBase += dldy;
                if (TEXTURED) { wBase += dwdy; uBase += dudy; vBase += dvdy; }
                }
            }

        /** Shade a packed pixel by a 0..256 integer factor.

            The float version cost six conversions a pixel: each of the three
            channels was widened to float, multiplied, and narrowed back. The
            shade is one number, so it is converted once at the call site and
            the channels stay integers. Same arithmetic, six conversions become
            one - and this runs on every lit pixel of every textured triangle. */
        /** Blend two packed pixels. `a` is 0..256. Same arithmetic as the 2D
            canvas, for the same reason: five bits stay five bits. */
        static uint16_t _blendPix(uint16_t dst, uint16_t src, int a)
            {
            if (a >= 256) return src;
            const int ia = 256 - a;
            const int r = (((src >> 11) & 31) * a + ((dst >> 11) & 31) * ia) >> 8;
            const int g = (((src >> 5) & 63) * a + ((dst >> 5) & 63) * ia) >> 8;
            const int b = (( src       & 31) * a + ( dst       & 31) * ia) >> 8;
            return (uint16_t)((r << 11) | (g << 5) | b);
            }

        /** Texel area of a triangle, from the RAW texture coordinates.
            Computed where they are still raw; recovering them after the
            perspective divide costs three divisions a triangle. */
        static float _uvArea(const float* uv, const uint16_t* ind, int t,
                             const Texture* tex)
            {
            if ((tex == nullptr) || (tex->levels <= 1)) return 0.0f;
            const float* a = uv + (size_t)ind[t * 3 + 0] * 2;
            const float* b = uv + (size_t)ind[t * 3 + 1] * 2;
            const float* c = uv + (size_t)ind[t * 3 + 2] * 2;
            float cr = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
            if (cr < 0.0f) cr = -cr;
            return cr * (float)tex->w * (float)tex->h;
            }

        /// One mip level, flattened so the samplers never branch on which.
        struct MipView
            {
            const uint16_t* px = nullptr;
            float w = 0.0f, h = 0.0f;
            int wmask = 0, hmask = 0, shift = 0;
            };

        /** The level whose texels come closest to one per pixel.

            Both areas are already to hand: the screen area was computed for the
            culling test, and the UV area is the same cross product on the other
            side of the mapping. The level is then the one where the ratio has
            been divided down to about one, which is a handful of compares - no
            logarithm, and none needed when the ratio is already small. */
        MipView _pickLevel(const Texture* t, float uvArea, float screenArea) const
            {
            int lod = _mipBias;
            if (lod < 0) lod = 0;
            if ((t->levels > 1) && (screenArea > 1e-9f))
                {
                float r = uvArea / screenArea;
                while ((lod + 1 < t->levels) && (t->lv[lod + 1] != nullptr) && (r >= 4.0f))
                    { r *= 0.25f; lod++; }
                }
            while ((lod > 0) && (t->lv[lod] == nullptr)) lod--;
            MipView m;
            m.px = t->lv[lod];
            m.wmask = t->lvWmask[lod];
            m.hmask = t->lvHmask[lod];
            m.shift = t->lvShift[lod];
            m.w = (float)(m.wmask + 1);
            m.h = (float)(m.hmask + 1);
            return m;
            }

        static uint16_t _sampleNearest(const MipView& t, float u, float v)
            {
            const int tu = ((int)(u * t.w)) & t.wmask;
            const int tv = ((int)(v * t.h)) & t.hmask;
            return t.px[((size_t)tv << t.shift) + tu];
            }

        /** Nearest sample from 16.16 texel coordinates.

            The float form multiplied by the texture size and converted to int
            on EVERY pixel: two multiplies and two float-to-int conversions to
            move one texel sideways. Stepping in fixed point does the multiply
            once a segment and leaves a shift and a mask per pixel. */
        static uint16_t _sampleNearestFx(const MipView& t, int32_t ufx, int32_t vfx)
            {
            const int tu = (ufx >> 16) & t.wmask;
            const int tv = (vfx >> 16) & t.hmask;
            return t.px[((size_t)tv << t.shift) + tu];
            }

        /** Bilinear from the same coordinates: the fractional bits ARE the
            weights, so they cost nothing to obtain. */
        // GCC leaves this out of line, and it is called once per PIXEL: the
        // MipView fields it reads are then reloaded on every sample instead of
        // being hoisted out of the span. Forced inline so the span keeps them
        // in registers.
        __attribute__((always_inline))
        static inline uint16_t _sampleBilinearFx(const MipView& t, int32_t ufx, int32_t vfx)
            {
            const int32_t uh = ufx - 32768, vh = vfx - 32768;   // half-texel offset
            const int x0 = (uh >> 16) & t.wmask, x1 = ((uh >> 16) + 1) & t.wmask;
            const int y0 = (vh >> 16) & t.hmask, y1 = ((vh >> 16) + 1) & t.hmask;
            const int wx = (uh >> 12) & 15, wy = (vh >> 12) & 15;
            const uint16_t* rowA = t.px + ((size_t)y0 << t.shift);
            const uint16_t* rowB = t.px + ((size_t)y1 << t.shift);
            const uint16_t p00 = rowA[x0], p10 = rowA[x1];
            const uint16_t p01 = rowB[x0], p11 = rowB[x1];
            const int ix = 16 - wx, iy = 16 - wy;
            const int w00 = ix * iy, w10 = wx * iy, w01 = ix * wy, w11 = wx * wy;
            const int r = (((p00 >> 11) & 31) * w00 + ((p10 >> 11) & 31) * w10
                         + ((p01 >> 11) & 31) * w01 + ((p11 >> 11) & 31) * w11) >> 8;
            const int g = (((p00 >> 5) & 63) * w00 + ((p10 >> 5) & 63) * w10
                         + ((p01 >> 5) & 63) * w01 + ((p11 >> 5) & 63) * w11) >> 8;
            const int b = ((( p00 & 31)) * w00 + (( p10 & 31)) * w10
                         + (( p01 & 31)) * w01 + (( p11 & 31)) * w11) >> 8;
            return (uint16_t)((r << 11) | (g << 5) | b);
            }

        /** Four texels, weighted by where the sample fell between them.

            Blended in 5-6-5 with 4-bit weights: widening to 8 bits per channel
            costs three more multiplies per texel and gives back the same five
            bits. The neighbours are taken with the wrap mask, so a sample on the
            seam of a sphere reads across it instead of clamping to an edge that
            is not there. */
        static uint16_t _sampleBilinear(const MipView& t, float u, float v)
            {
            const float fx = u * t.w - 0.5f;
            const float fy = v * t.h - 0.5f;
            const int x0 = iFloor(fx), y0 = iFloor(fy);
            const int wx = (int)((fx - (float)x0) * 16.0f);
            const int wy = (int)((fy - (float)y0) * 16.0f);
            const int xa = x0 & t.wmask, xb = (x0 + 1) & t.wmask;
            const int ya = y0 & t.hmask, yb = (y0 + 1) & t.hmask;
            const uint16_t* rowA = t.px + ((size_t)ya << t.shift);
            const uint16_t* rowB = t.px + ((size_t)yb << t.shift);
            const uint16_t p00 = rowA[xa], p10 = rowA[xb];
            const uint16_t p01 = rowB[xa], p11 = rowB[xb];
            const int ix = 16 - wx, iy = 16 - wy;
            const int w00 = ix * iy, w10 = wx * iy, w01 = ix * wy, w11 = wx * wy;
            const int r = (((p00 >> 11) & 31) * w00 + ((p10 >> 11) & 31) * w10
                         + ((p01 >> 11) & 31) * w01 + ((p11 >> 11) & 31) * w11) >> 8;
            const int g = (((p00 >> 5) & 63) * w00 + ((p10 >> 5) & 63) * w10
                         + ((p01 >> 5) & 63) * w01 + ((p11 >> 5) & 63) * w11) >> 8;
            const int b = ((( p00 & 31)) * w00 + (( p10 & 31)) * w10
                         + (( p01 & 31)) * w01 + (( p11 & 31)) * w11) >> 8;
            return (uint16_t)((r << 11) | (g << 5) | b);
            }

        static uint16_t _modulate(uint16_t c, int s)
            {
            if (s >= 256) return c;
            if (s <= 0) return 0;
            const int r = (((c >> 11) & 31) * s) >> 8;
            const int g = (((c >> 5)  & 63) * s) >> 8;
            const int b = (( c        & 31) * s) >> 8;
            return (uint16_t)((r << 11) | (g << 5) | b);
            }

        static uint16_t _pack(float r, float g, float b)
            {
            // One statement per line. Two `if`s sharing a line is what the
            // compiler calls misleading indentation, and it gets fixed here
            // rather than suppressed with a flag.
            if (r > 1.0f) r = 1.0f;
            if (r < 0.0f) r = 0.0f;
            if (g > 1.0f) g = 1.0f;
            if (g < 0.0f) g = 0.0f;
            if (b > 1.0f) b = 1.0f;
            if (b < 0.0f) b = 0.0f;
            return (uint16_t)(((int)(r * 31.0f + 0.5f) << 11) |
                              ((int)(g * 63.0f + 0.5f) << 5)  |
                               (int)(b * 31.0f + 0.5f));
            }


        static float _min3(float a, float b, float c)
            { const float m = (a < b) ? a : b; return (m < c) ? m : c; }
        static float _max3(float a, float b, float c)
            { const float m = (a > b) ? a : b; return (m > c) ? m : c; }

        /** 1/w into 16 bits. `_zScale` is set from the depth range in use. */
        uint16_t _packDepth(float iw) const
            {
            float v = iw * _zScale;
            if (v <= 0.0f) return 0;
            if (v >= 65535.0f) return 65535;
            return (uint16_t)v;
            }

        /**
         * Flat shading from the face normal in WORLD space.
         *
         * Taken from the world positions, not the supplied vertex normals: this
         * stage has no per-vertex interpolation, so a face normal is the only
         * thing a flat shade can honestly use. Gouraud is stage 2.
         */
        uint16_t _shadeFace(const float wx[3], const float wy[3], const float wz[3]) const
            {
            float lit = 1.0f;
            if (_shading != Shading::Unlit)
                {
                const float ux = wx[1]-wx[0], uy = wy[1]-wy[0], uz = wz[1]-wz[0];
                const float vx = wx[2]-wx[0], vy = wy[2]-wy[0], vz = wz[2]-wz[0];
                float nx = uy*vz - uz*vy, ny = uz*vx - ux*vz, nz = ux*vy - uy*vx;
                const float n = sqrtf(nx*nx + ny*ny + nz*nz);
                if (n > 1e-12f)
                    {
                    const float inv = 1.0f / n;
                    float d = -(nx*inv*_lx + ny*inv*_ly + nz*inv*_lz);
                    if (d < 0.0f) d = 0.0f;
                    lit = _ambient + _diffuse * d;
                    }
                }
            float r = _r * lit, g = _g * lit, b = _b * lit;
            if (r > 1.0f) r = 1.0f;
            if (g > 1.0f) g = 1.0f;
            if (b > 1.0f) b = 1.0f;
            const int ri = (int)(r * 31.0f + 0.5f);
            const int gi = (int)(g * 63.0f + 0.5f);
            const int bi = (int)(b * 31.0f + 0.5f);
            return (uint16_t)((ri << 11) | (gi << 5) | bi);
            }

        static void* _defaultAlloc(size_t n) { return ::operator new(n, std::nothrow); }
        static void  _defaultFree(void* p)   { ::operator delete(p, std::nothrow); }

        void* _alloc(size_t n) { return _allocFn(n); }
        void  _free(void* p)   { if (p != nullptr) _freeFn(p); }

        AllocFn _allocFn = &_defaultAlloc;
        FreeFn  _freeFn  = &_defaultFree;

        uint16_t* _fb = nullptr;
        uint16_t* _zb = nullptr;
        int _w = 0, _h = 0, _stride = 0, _ox = 0, _oy = 0;
        int _vpW = 0, _vpH = 0;

        float _vp[16]    = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        float _model[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        float _mvp[16]   = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        bool  _mvpDirty  = true;

        float _r = 1.0f, _g = 1.0f, _b = 1.0f;
        float _ambient = 0.2f, _diffuse = 0.7f;
        float _lx = 0.0f, _ly = 0.0f, _lz = -1.0f;
        float _view[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        float _proj[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        void _composeVP()
            {
            mat4Multiply(_proj, _view, _vp);
            _mvpDirty = true;
            // The half vector is built from the camera's forward axis, so it is
            // stale the moment the camera moves.
            _lightToModel();
            }
        float _lm[3] = { 0.0f, 0.0f, 0.0f };  ///< light carried into normal space
        float _hm[3] = { 0.0f, 0.0f, 0.0f };  ///< half vector, same space
        float _specular = 0.0f;
        int   _specExp = 0;
        float _zScale = 65535.0f;
        bool  _bilinear = false;
        int   _mipBias = 0;
        float _opacity = 1.0f;
        float _near = 1e-4f;
        // -1, measured: the stored projection has its y row negated so clip
        // space matches framebuffer rows, and that flips screen-space winding.
        // Both directions give the same silhouette on a closed model - only the
        // shading tells them apart (1.31% of pixels off by more than a step,
        // against 15.29% the other way), which is why this was measured rather
        // than reasoned about.
        int     _cull = -1;
        Shading _shading = Shading::Flat;
        TextureMode _texMode = TextureMode::None;
        float _nrm[9] = { 1,0,0, 0,1,0, 0,0,1 };
    };

} // namespace a3d

#endif // A3D_SOFT_BACKEND_H_
