// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file a3d_primitives.h
 * @brief Mesh generators for shapes a scene needs but an asset file should not
 *        have to carry: spheres, cylinders, truncated cones.
 *
 * These BUILD ARRAYS; they do not draw. A generated mesh then goes through the
 * same path as a loaded one - the same binner, the same culling, the same
 * backend - so a procedural globe is tiled and binned exactly like an imported
 * model. A renderer that draws primitives itself has to re-generate them every
 * frame and cannot bin them at all.
 *
 *
 * Each generator has a `...Size()` companion that reports the vertex and
 * triangle counts for the same parameters, so a caller allocates exactly once
 * and never guesses.
 */

#ifndef A3D_PRIMITIVES_H_
#define A3D_PRIMITIVES_H_

#include <stdint.h>
#include <math.h>

namespace a3d {

/** Where a generator writes. Any pointer may be null to skip that stream. */
struct MeshSink
    {
    float*    positions = nullptr;   ///< 3 floats per vertex
    float*    normals   = nullptr;   ///< 3 floats per vertex, unit length
    float*    texcoords = nullptr;   ///< 2 floats per vertex
    uint16_t* indices   = nullptr;   ///< 3 per triangle
    };

/** Vertex and triangle counts a shape will produce. */
struct MeshSize
    {
    int vertices  = 0;
    int triangles = 0;
    };

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------

/**
 * A UV sphere of radius 1 centred on the origin.
 *
 * Rings are shared, so the seam at longitude 0 is duplicated once (`sectors+1`
 * columns) and nowhere else: a shared seam vertex would need two different `u`
 * values and would smear the texture across the whole map.
 *
 * Mapping: `x = sin(phi)cos(theta)`, `y = cos(phi)`, `z = sin(phi)sin(theta)`
 * with `u = i/sectors` and `v = 0.5cos(phi) + 0.5`, so `v` is 1 at the north
 * pole. Equirectangular textures are authored for this.
 */
inline MeshSize sphereSize(int sectors, int stacks)
    {
    if (sectors < 3) sectors = 3;
    if (stacks < 2) stacks = 2;
    MeshSize s;
    s.vertices  = (sectors + 1) * (stacks + 1);
    s.triangles = sectors * stacks * 2 - sectors * 2;   // poles contribute one each
    return s;
    }

inline MeshSize sphereMesh(int sectors, int stacks, const MeshSink& out)
    {
    if (sectors < 3) sectors = 3;
    if (stacks < 2) stacks = 2;

    const float kPi = 3.14159265358979323846f;
    int v = 0;
    for (int j = 0; j <= stacks; j++)
        {
        const float phi = kPi * (float)j / (float)stacks;
        const float cp = cosf(phi), sp = sinf(phi);
        for (int i = 0; i <= sectors; i++, v++)
            {
            const float theta = 2.0f * kPi * (float)i / (float)sectors;
            const float x = sp * cosf(theta);
            const float y = cp;
            const float z = sp * sinf(theta);
            if (out.positions != nullptr)
                { out.positions[v*3+0] = x; out.positions[v*3+1] = y; out.positions[v*3+2] = z; }
            if (out.normals != nullptr)
                { out.normals[v*3+0] = x; out.normals[v*3+1] = y; out.normals[v*3+2] = z; }
            if (out.texcoords != nullptr)
                {
                out.texcoords[v*2+0] = (float)i / (float)sectors;
                out.texcoords[v*2+1] = 0.5f * cp + 0.5f;
                }
            }
        }

    int t = 0;
    if (out.indices != nullptr)
        {
        const int row = sectors + 1;
        for (int j = 0; j < stacks; j++)
            for (int i = 0; i < sectors; i++)
                {
                const uint16_t a = (uint16_t)(j * row + i);
                const uint16_t b = (uint16_t)(a + row);
                // The pole rows collapse to a point, so one of the two triangles
                // of that quad is degenerate and is not emitted.
                if (j != 0)
                    {
                    out.indices[t*3+0] = a;
                    out.indices[t*3+1] = (uint16_t)(a + 1);
                    out.indices[t*3+2] = b;
                    t++;
                    }
                if (j != stacks - 1)
                    {
                    out.indices[t*3+0] = (uint16_t)(a + 1);
                    out.indices[t*3+1] = (uint16_t)(b + 1);
                    out.indices[t*3+2] = b;
                    t++;
                    }
                }
        }
    else t = sphereSize(sectors, stacks).triangles;

    MeshSize s;
    s.vertices = (sectors + 1) * (stacks + 1);
    s.triangles = t;
    return s;
    }

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------

/**
 * An axis-aligned box from `-half` to `+half`.
 *
 * Each face gets its own four vertices. Sharing the eight corners would be
 * smaller but would force one normal per corner, and a box lit that way has
 * rounded edges - the shape reads as a die that has been tumbled.
 */
inline MeshSize boxSize() { MeshSize s; s.vertices = 24; s.triangles = 12; return s; }

inline MeshSize boxMesh(float hx, float hy, float hz, const MeshSink& out)
    {
    static const float N[6][3] = { { 0,0,1}, { 0,0,-1}, { 1,0,0},
                                   {-1,0,0}, { 0,1, 0}, { 0,-1,0} };
    // Per face: origin corner then the two edge directions, so winding is
    // outward everywhere without a table of signs.
    static const float U[6][3] = { { 1,0,0}, {-1,0,0}, { 0,0,-1},
                                   { 0,0, 1}, { 1,0,0}, {-1,0,0} };
    static const float V[6][3] = { { 0,1,0}, { 0,1, 0}, { 0,1,0},
                                   { 0,1,0}, { 0,0,-1}, { 0,0,-1} };
    int v = 0, t = 0;
    for (int f = 0; f < 6; f++)
        {
        const float* n = N[f];
        const float* u = U[f];
        const float* w = V[f];
        const float c[3] = { n[0]*hx, n[1]*hy, n[2]*hz };
        const int base = v;
        for (int k = 0; k < 4; k++, v++)
            {
            const float su = (k == 0 || k == 3) ? -1.0f : 1.0f;
            const float sv = (k < 2) ? -1.0f : 1.0f;
            if (out.positions != nullptr)
                for (int a = 0; a < 3; a++)
                    {
                    const float ext = (a == 0) ? hx : (a == 1) ? hy : hz;
                    out.positions[v*3+a] = c[a] + su * u[a] * ext + sv * w[a] * ext;
                    }
            if (out.normals != nullptr)
                { out.normals[v*3+0] = n[0]; out.normals[v*3+1] = n[1]; out.normals[v*3+2] = n[2]; }
            if (out.texcoords != nullptr)
                {
                out.texcoords[v*2+0] = (su < 0.0f) ? 0.0f : 1.0f;
                out.texcoords[v*2+1] = (sv < 0.0f) ? 0.0f : 1.0f;
                }
            }
        if (out.indices != nullptr)
            {
            out.indices[t*3+0] = (uint16_t)(base + 0);
            out.indices[t*3+1] = (uint16_t)(base + 1);
            out.indices[t*3+2] = (uint16_t)(base + 2); t++;
            out.indices[t*3+0] = (uint16_t)(base + 0);
            out.indices[t*3+1] = (uint16_t)(base + 2);
            out.indices[t*3+2] = (uint16_t)(base + 3); t++;
            }
        else t += 2;
        }
    MeshSize s; s.vertices = v; s.triangles = t; return s;
    }

// ---------------------------------------------------------------------------
// Truncated cone
// ---------------------------------------------------------------------------

/**
 * Bottom ring of radius `rBottom` at `y = -1`, top ring of radius `rTop` at
 * `y = +1`. A cylinder is the case `rTop == rBottom`; a cone is `rTop == 0`.
 *
 * The side normal is perpendicular to the SLANT, not radial: on a cone the two
 * differ, and a radial normal lights a cone as though it were a cylinder.
 */
inline MeshSize truncatedConeSize(int sectors, bool capBottom, bool capTop)
    {
    if (sectors < 3) sectors = 3;
    MeshSize s;
    s.vertices  = (sectors + 1) * 2;                  // side, seam duplicated
    s.triangles = sectors * 2;
    if (capBottom) { s.vertices += sectors + 1; s.triangles += sectors; }
    if (capTop)    { s.vertices += sectors + 1; s.triangles += sectors; }
    return s;
    }

inline MeshSize truncatedConeMesh(int sectors, float rBottom, float rTop,
                                  bool capBottom, bool capTop, const MeshSink& out)
    {
    if (sectors < 3) sectors = 3;
    const float kPi = 3.14159265358979323846f;

    // Slant normal, shared by every side vertex: the surface is a ruled surface
    // whose slope does not change around the axis.
    const float dr = rTop - rBottom;
    const float nyLen = sqrtf(4.0f + dr * dr);
    const float nRad = 2.0f / nyLen;        // radial component
    const float nY   = -dr / nyLen;         // axial component

    int v = 0, t = 0;
    const int sideBase = v;
    for (int i = 0; i <= sectors; i++)
        {
        const float th = 2.0f * kPi * (float)i / (float)sectors;
        const float ct = cosf(th), st = sinf(th);
        for (int k = 0; k < 2; k++, v++)          // k=0 bottom, k=1 top
            {
            const float r = (k == 0) ? rBottom : rTop;
            if (out.positions != nullptr)
                {
                out.positions[v*3+0] = r * ct;
                out.positions[v*3+1] = (k == 0) ? -1.0f : 1.0f;
                out.positions[v*3+2] = r * st;
                }
            if (out.normals != nullptr)
                {
                out.normals[v*3+0] = nRad * ct;
                out.normals[v*3+1] = nY;
                out.normals[v*3+2] = nRad * st;
                }
            if (out.texcoords != nullptr)
                {
                out.texcoords[v*2+0] = (float)i / (float)sectors;
                out.texcoords[v*2+1] = (k == 0) ? 0.0f : 1.0f;
                }
            }
        }
    if (out.indices != nullptr)
        for (int i = 0; i < sectors; i++)
            {
            const uint16_t b0 = (uint16_t)(sideBase + i * 2);
            const uint16_t t0 = (uint16_t)(b0 + 1);
            const uint16_t b1 = (uint16_t)(b0 + 2);
            const uint16_t t1 = (uint16_t)(b0 + 3);
            out.indices[t*3+0] = b0; out.indices[t*3+1] = t0; out.indices[t*3+2] = b1; t++;
            out.indices[t*3+0] = b1; out.indices[t*3+1] = t0; out.indices[t*3+2] = t1; t++;
            }

    // Caps: a centre vertex plus its own ring, so the disc normal never leaks
    // into the side shading.
    struct Cap { bool on; float y; float r; float ny; };
    const Cap caps[2] = { { capBottom, -1.0f, rBottom, -1.0f },
                          { capTop,     1.0f, rTop,     1.0f } };
    for (int c = 0; c < 2; c++)
        {
        if (!caps[c].on) continue;
        const int centre = v;
        if (out.positions != nullptr)
            { out.positions[v*3+0] = 0.0f; out.positions[v*3+1] = caps[c].y; out.positions[v*3+2] = 0.0f; }
        if (out.normals != nullptr)
            { out.normals[v*3+0] = 0.0f; out.normals[v*3+1] = caps[c].ny; out.normals[v*3+2] = 0.0f; }
        if (out.texcoords != nullptr)
            { out.texcoords[v*2+0] = 0.5f; out.texcoords[v*2+1] = 0.5f; }
        v++;
        const int ring = v;
        for (int i = 0; i < sectors; i++, v++)
            {
            const float th = 2.0f * kPi * (float)i / (float)sectors;
            const float ct = cosf(th), st = sinf(th);
            if (out.positions != nullptr)
                {
                out.positions[v*3+0] = caps[c].r * ct;
                out.positions[v*3+1] = caps[c].y;
                out.positions[v*3+2] = caps[c].r * st;
                }
            if (out.normals != nullptr)
                { out.normals[v*3+0] = 0.0f; out.normals[v*3+1] = caps[c].ny; out.normals[v*3+2] = 0.0f; }
            if (out.texcoords != nullptr)
                {
                out.texcoords[v*2+0] = 0.5f + 0.5f * ct;
                out.texcoords[v*2+1] = 0.5f + 0.5f * st;
                }
            }
        if (out.indices != nullptr)
            for (int i = 0; i < sectors; i++)
                {
                const uint16_t a = (uint16_t)(ring + i);
                const uint16_t b = (uint16_t)(ring + ((i + 1) % sectors));
                // Wound so the two caps face opposite ways.
                out.indices[t*3+0] = (uint16_t)centre;
                out.indices[t*3+1] = (c == 0) ? a : b;
                out.indices[t*3+2] = (c == 0) ? b : a;
                t++;
                }
        }

    MeshSize s;
    s.vertices = v;
    s.triangles = t;
    return s;
    }

} // namespace a3d

#endif // A3D_PRIMITIVES_H_
