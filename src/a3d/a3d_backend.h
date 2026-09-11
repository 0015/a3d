// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file a3d_backend.h
 * @brief Type-erased render backend interface.
 *
 * This is the seam that keeps everything above it independent of whatever
 * actually draws the pixels.
 *
 * Design rules enforced here
 *   1. No renderer type appears in this file.
 *   2. This interface is NOT a template.
 *   3. Cost is one virtual call PER DRAW CALL,
 *
 * Scope boundary
 *   This interface covers PER-OBJECT drawing only. Global render setup
 *   (viewport, camera, projection, z-buffer, lights) stays with the
 *   application, which configures the concrete renderer directly.
 */
#ifndef A3D_BACKEND_H_
#define A3D_BACKEND_H_

#include "a3d_types.h"

namespace a3d {

class IRenderBackend
    {
    public:

        virtual ~IRenderBackend() = default;

        // ------------------------------------------------------------------
        // Capability queries
        //
        // A backend may be compiled with a restricted shader set. Callers
        // should query and degrade gracefully rather than draw nothing.
        // ------------------------------------------------------------------

        virtual bool supports(Shading shading) const = 0;

        virtual bool supports(TextureMode mode) const = 0;


        // ------------------------------------------------------------------
        // Per-object state
        // ------------------------------------------------------------------

        /** @param m16 16 floats, COLUMN-MAJOR. */
        virtual void setModelMatrix(const float m16[16]) = 0;

        virtual void setMaterial(const Material& material) = 0;

        /**
         * Select lighting model and texture mapping mode for subsequent draws.
         * Ignored silently by a backend that does not support the combination;
         * call supports() first.
         */
        virtual void setShading(Shading shading, TextureMode textureMode) = 0;


        // ------------------------------------------------------------------
        // Geometry submission
        // ------------------------------------------------------------------

        virtual void drawTriangles(const TriangleBatch& batch) = 0;

        // ------------------------------------------------------------------
        // Optional: triangles whose vertices are already in screen space.
        //
        // The binner has to project every vertex to decide which tiles a
        // triangle touches, and then throws the result away; the rasterizer
        // projects the same vertices again, three per triangle. A backend that
        // can consume the binner's output skips the second pass entirely.
        //
        // Optional because a backend that owns its own transform cannot express
        // it: such a draw call takes model space by definition. Defaulting to
        // "no" is what keeps those backends valid.
        //
        //
        // ------------------------------------------------------------------

        virtual bool supportsProjectedVertices() const { return false; }
        virtual void drawTrianglesProjected(const ProjectedBatch&) {}


        // ------------------------------------------------------------------
        // Backend-owned vertex storage
        //
        // WHY THIS EXISTS
        //   A backend may want its vertices as objects of its own type rather
        //   than as a bare float array. If that type is not standard-layout,
        //   casting a plain `float*` to it works on every real compiler but is
        //   formally UB. Letting the BACKEND allocate means the memory genuinely
        //   holds objects of that type, and the animation layer merely writes
        //   floats into them - which is well defined.
        //
        //
        //   The rasterizer that ships here reads plain float arrays and needs
        //   none of this, but the rule stays so a backend that does is possible.
        // ------------------------------------------------------------------

        /**
         * Allocate storage for `nbVec3` xyz triples, laid out so the backend
         * can consume it directly. Returns null on failure.
         * Caller writes 3 * nbVec3 floats. Free with freeVec3Array().
         */
        virtual float* allocVec3Array(int nbVec3) = 0;

        virtual void freeVec3Array(float* p) = 0;

        /**
         * Same contract for uv pairs. A two-float type is usually standard-layout
         * so a plain cast would often be defensible, but routing both through the
         * backend keeps ONE rule for callers instead of a per-type exception.
         */
        virtual float* allocVec2Array(int nbVec2) = 0;

        virtual void freeVec2Array(float* p) = 0;


        // ------------------------------------------------------------------
        // Texture registration
        // ------------------------------------------------------------------

        /**
         * Wrap a backend-native texture object into an opaque handle.
         *
         * The asset layer obtains native textures from the backend-specific
         * loader path; this method exists so the rest of the component never
         * needs the native type.
         */
        virtual TextureHandle wrapNativeTexture(const void* nativeTexture) = 0;

        /**
         * Build a texture that reads its pixels straight out of `pixels`.
         *
         * The container keeps texture blobs in their native pixel format, so
         * this is the zero-copy path: on flash-resident assets the pixels are
         * never brought into RAM at all. The backend must NOT copy them, and the
         * caller must keep the image alive for as long as the texture is used.
         *
         * @param format  a3d::fmt::TexelFormat value.
         * @returns an invalid handle when the format is unsupported.
         */
        virtual TextureHandle createTexture(int width, int height, int format,
                                            const void* pixels) = 0;

        virtual void destroyTexture(TextureHandle handle) = 0;


        // ------------------------------------------------------------------
        // Transform introspection
        // ------------------------------------------------------------------

        /**
         * The composed projection * view * model matrix the backend will apply
         * to the next draw, column-major.
         *
         * Tile binning has to project vertices to decide which tiles a triangle
         * touches, and it must arrive at EXACTLY the screen positions the
         * renderer will. Asking the backend for its own matrix removes any
         * chance of the two drifting apart - a mismatch would not crash, it
         * would silently drop triangles from tiles they actually cover.
         */
        virtual void currentMVP(float out[16]) const = 0;
    };

} // namespace a3d

#endif // A3D_BACKEND_H_
