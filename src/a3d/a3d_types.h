// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file a3d_types.h
 * @brief Plain, backend-agnostic types for the a3d component.
 *
 * NOTHING in this header may reference a particular renderer.
 *
 *  - Matrices are 16 floats in COLUMN-MAJOR order, the same as OpenGL.
 *  - Positions and normals are tightly packed xyz triples.
 *  - Texture coordinates are tightly packed uv pairs.
 */
#ifndef A3D_TYPES_H_
#define A3D_TYPES_H_

#include <stdint.h>

namespace a3d {

/** Component version. Bump on any breaking interface change. */
constexpr int A3D_VERSION_MAJOR = 0;
constexpr int A3D_VERSION_MINOR = 1;

/**
 * Opaque texture reference.
 *
 * The value is produced and interpreted ONLY by the backend. The asset and
 * animation layers pass it around without ever dereferencing it.
 */
struct TextureHandle
    {
    const void* id = nullptr;

    bool valid() const { return id != nullptr; }
    };


/** Lighting model requested for a draw. */
enum class Shading : uint8_t
    {
    Unlit   = 0,   ///< No lighting. Material color or texture color used directly.
    Flat    = 1,   ///< One color per face.
    Gouraud = 2    ///< Per-vertex lighting, interpolated. Requires normals.
    };


/** Texture mapping mode requested for a draw. */
enum class TextureMode : uint8_t
    {
    None        = 0,   ///< Untextured.
    Perspective = 1,   ///< Perspective-correct mapping.
    Affine      = 2    ///< Affine mapping: faster, lower quality.
    };


/**
 * Per-object surface properties.
 * Values match the Phong terms used by typical software renderers.
 */
struct Material
    {
    float color[3]        = { 1.0f, 1.0f, 1.0f }; ///< Base RGB in [0,1]; used when untextured.
    float ambient         = 0.2f;                 ///< Ambient reflection coefficient.
    float diffuse         = 0.7f;                 ///< Diffuse reflection coefficient.
    float specular        = 0.5f;                 ///< Specular reflection coefficient.
    int   specularExponent = 16;                  ///< Specular exponent. 0 disables specular.
    };


/**
 * A batch whose positions are already projected: screen x, screen y and 1/w per
 * vertex, exactly as the binner left them. Everything else is as TriangleBatch.
 *
 * The binner has to project every vertex to decide which tiles a triangle
 * touches, and then throws the result away; the rasterizer projects the same
 * vertices again, three per triangle. Benchmark C measured that redundancy at
 * 8.2x on a real mesh.
 */
struct ProjectedBatch
    {
    int             nbTriangles  = 0;

    const uint16_t* indices      = nullptr;  ///< 3 per triangle, into the arrays below
    const float*    screenX      = nullptr;  ///< one per vertex
    const float*    screenY      = nullptr;
    const float*    screenW      = nullptr;  ///< 1/w
    const uint8_t*  behind       = nullptr;  ///< non-zero: at or behind the eye

    const float*    normals      = nullptr;  ///< optional, unit length
    const uint16_t* indNormals   = nullptr;
    const float*    texcoords    = nullptr;  ///< optional
    const uint16_t* indTexcoords = nullptr;
    TextureHandle   texture      = {};
    };


/**
 * One indexed triangle list to draw.
 *
 * Index arrays hold 3 entries per triangle. `indNormals` and `indTexcoords`
 * may be null, in which case the corresponding attribute is unused.
 *
 * @warning `positions` and `normals` MUST come from IRenderBackend::allocVec3Array().
 *          See the note on that method for why.
 */
struct TriangleBatch
    {
    int             nbTriangles   = 0;

    const uint16_t* indPositions  = nullptr;  ///< Required. 3 indices per triangle.
    const float*    positions     = nullptr;  ///< Required. xyz triples. Backend-allocated.

    const uint16_t* indNormals    = nullptr;  ///< Optional. Required for Shading::Gouraud.
    const float*    normals       = nullptr;  ///< Optional. xyz triples, unit length. Backend-allocated.

    const uint16_t* indTexcoords  = nullptr;  ///< Optional. Required when textured.
    const float*    texcoords     = nullptr;  ///< Optional. uv pairs. May point into a read-only asset blob.

    TextureHandle   texture       = {};       ///< Optional. Ignored when TextureMode::None.
    };


/** Identity matrix helper, column-major. */
inline void identityMatrix(float m[16])
    {
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    }

} // namespace a3d

#endif // A3D_TYPES_H_
