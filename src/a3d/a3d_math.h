// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file a3d_math.h
 * @brief Minimal column-major matrix maths. No renderer dependency.
 *
 * Column-major throughout, matching glTF, OpenGL and the
 * container's `inv_bind` layout. Element (row, col) lives at `m[col*4 + row]`.
 * Keeping one convention everywhere is the cheapest way to avoid the
 * transposed-matrix bugs that are invisible until something rotates the wrong way.
 */
#ifndef A3D_MATH_H_
#define A3D_MATH_H_

#include <stdint.h>   // uint32_t: invSqrt reads the exponent bits directly
#include <math.h>     // tanf: the only libm call here, once per camera change

namespace a3d {

/**
 * Reciprocal square root, ~0.2% error after one Newton step.
 *
 * Used to renormalize skinned normals, which is per vertex and therefore the
 * hottest float operation in the frame after the rasterizer's own. Written out
 * because `1.0f / sqrtf(x)` costs a hardware square root AND a divide: on an
 * ESP32-P4 that pair made skinning 1.52x slower on a 9,761-vertex model, 17% of
 * the frame. A normal is a direction, so a fifth of a percent of length error is
 * invisible in the diffuse term.
 */
inline float invSqrt(float x)
    {
    float half = 0.5f * x;
    uint32_t i;
    __builtin_memcpy(&i, &x, sizeof(i));
    i = 0x5F3759DFu - (i >> 1);
    __builtin_memcpy(&x, &i, sizeof(x));
    return x * (1.5f - half * x * x);
    }

/**
 * `(int)floorf(v)` and `(int)ceilf(v)` without the call.
 *
 * `floorf` and `ceilf` are out-of-line library calls on this toolchain, and in a
 * rasterizer a call costs far more than itself: every float the compiler is
 * holding has to be spilled around it and reloaded after, so one site becomes a
 * dozen loads and stores, and everything live across it is forced into
 * callee-saved registers the function then saves and restores on every entry.
 * The rasterizer had twelve such sites and two of them ran on every scanline.
 *
 * Truncation toward zero is a single instruction (`fcvt.w.s ... rtz` on RISC-V),
 * and floor and ceil are that plus one correction. The result is identical to
 * `(int)floorf(v)` wherever that expression is defined - both are undefined
 * outside int range, so this narrows nothing. `_min3`/`_max3` style callers that
 * want a float back should use these and convert, not call libm.
 *
 */
inline int iFloor(float v) { const int i = (int)v; return (v < (float)i) ? (i - 1) : i; }
inline int iCeil (float v) { const int i = (int)v; return (v > (float)i) ? (i + 1) : i; }

/**
 * `floorf(v)` without the call, exact for every finite input and for NaN.
 *
 * The int-returning pair above is enough where the result is immediately
 * truncated. The textured span setup wants the fractional part, `u - floorf(u)`,
 * and that needs a float back.
 *
 * Above 2^23 every float is already an integer, so floor is the identity there -
 * and that is also exactly where the cast to int would overflow. One test
 * therefore covers both the wide-magnitude case and the overflow, and writing it
 * as a negated conjunction sends NaN down the same branch, where returning the
 * input is what `floorf` does anyway. No undefined behaviour for any input.
 *
 */
inline float fFloor(float v)
    {
    if (!((v > -8388608.0f) && (v < 8388608.0f))) return v;
    const float f = (float)(int)v;
    return (v < f) ? (f - 1.0f) : f;
    }

/**
 * Where a point lands on screen, given the matrix a draw will use.
 *
 * `SoftBackend::projectPoint()` answers the same question but reads
 * `currentMVP()`, so it cannot be called before the tiles are drawn - and an
 * application that places overlays first has to restate the contract. Three
 * copies of it existed, and they had drifted: three different near-plane
 * epsilons, one with no depth-range check, two rounding differently.
 *
 * The `- 0.5f` is the part that gets dropped. The rasterizer puts a vertex at
 * `(ndc + 1) * half - 0.5` because pixel centres sit at `x + 0.5`, and an
 * overlay written without it lands half a pixel off the geometry it annotates.
 * On the globe in `examples/3D_Earthquakes` one pixel is about 20 km, which is
 * the width of a city, and that is exactly how far beside itself a city marker
 * sat.
 *
 * Returns false for a point at or behind the eye, or outside the depth range,
 * where there is no screen position to report. `ndcZ` is optional and receives
 * the normalized depth for callers that sort or bias by it.
 *
 */
inline bool projectToScreen(const float mvp[16], float x, float y, float z,
                            int vpW, int vpH, float* sx, float* sy,
                            float* ndcZ = nullptr)
    {
    const float cw = mvp[3]*x + mvp[7]*y + mvp[11]*z + mvp[15];
    // This guards the DIVIDE, not the rejection: as cw approaches zero the
    // reciprocal goes to infinity and the depth below leaves the range on its
    // own, so a point at or behind the eye is refused either way. Loosening
    // this epsilon therefore breaks no test - it produces an infinity first.
    if (cw <= 1e-20f) return false;
    const float iw = 1.0f / cw;
    const float nz = (mvp[2]*x + mvp[6]*y + mvp[10]*z + mvp[14]) * iw;
    if ((nz < -1.0f) || (nz > 1.0f)) return false;
    const float cx = mvp[0]*x + mvp[4]*y + mvp[8]*z  + mvp[12];
    const float cy = mvp[1]*x + mvp[5]*y + mvp[9]*z  + mvp[13];
    // The same expression the rasterizer places a vertex with. Written any other
    // way it lands half a pixel off.
    if (sx != nullptr) *sx = ((cx * iw) + 1.0f) * ((float)vpW * 0.5f) - 0.5f;
    if (sy != nullptr) *sy = ((cy * iw) + 1.0f) * ((float)vpH * 0.5f) - 0.5f;
    if (ndcZ != nullptr) *ndcZ = nz;
    return true;
    }

/** `projectToScreen` rounded to the nearest pixel, away from zero on a tie. */
inline bool projectToPixel(const float mvp[16], float x, float y, float z,
                           int vpW, int vpH, int* px, int* py,
                           float* ndcZ = nullptr)
    {
    float fx = 0.0f, fy = 0.0f;
    if (!projectToScreen(mvp, x, y, z, vpW, vpH, &fx, &fy, ndcZ)) return false;
    if (px != nullptr) *px = (int)(fx + ((fx >= 0.0f) ? 0.5f : -0.5f));
    if (py != nullptr) *py = (int)(fy + ((fy >= 0.0f) ? 0.5f : -0.5f));
    return true;
    }

inline void mat4Identity(float m[16])
    {
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    }

inline void mat4Copy(const float src[16], float dst[16])
    {
    for (int i = 0; i < 16; i++) dst[i] = src[i];
    }

/** out = a * b. Safe when `out` aliases `a` or `b`. */
inline void mat4Multiply(const float a[16], const float b[16], float out[16])
    {
    float t[16];
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
            t[c * 4 + r] = s;
            }
    for (int i = 0; i < 16; i++) out[i] = t[i];
    }

/** Rotation matrix from a unit quaternion in xyzw order. */
inline void mat4FromQuat(const float q[4], float m[16])
    {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float wx = w * x, wy = w * y, wz = w * z;

    m[0]  = 1.0f - 2.0f * (yy + zz);  m[1]  = 2.0f * (xy + wz);         m[2]  = 2.0f * (xz - wy);         m[3]  = 0.0f;
    m[4]  = 2.0f * (xy - wz);         m[5]  = 1.0f - 2.0f * (xx + zz);  m[6]  = 2.0f * (yz + wx);         m[7]  = 0.0f;
    m[8]  = 2.0f * (xz + wy);         m[9]  = 2.0f * (yz - wx);         m[10] = 1.0f - 2.0f * (xx + yy);  m[11] = 0.0f;
    m[12] = 0.0f;                     m[13] = 0.0f;                     m[14] = 0.0f;                     m[15] = 1.0f;
    }

/** Compose translation * rotation * scale into a single matrix. */
inline void mat4FromTRS(const float t[3], const float r[4], const float s[3], float m[16])
    {
    mat4FromQuat(r, m);
    for (int c = 0; c < 3; c++)
        for (int row = 0; row < 3; row++)
            m[c * 4 + row] *= s[c];
    m[12] = t[0]; m[13] = t[1]; m[14] = t[2];
    }

/** Transform a point (w = 1). */
inline void mat4TransformPoint(const float m[16], const float p[3], float out[3])
    {
    const float x = p[0], y = p[1], z = p[2];
    out[0] = m[0] * x + m[4] * y + m[8]  * z + m[12];
    out[1] = m[1] * x + m[5] * y + m[9]  * z + m[13];
    out[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
    }

/** Transform a direction (w = 0): translation is ignored. */
inline void mat4TransformDirection(const float m[16], const float v[3], float out[3])
    {
    const float x = v[0], y = v[1], z = v[2];
    out[0] = m[0] * x + m[4] * y + m[8]  * z;
    out[1] = m[1] * x + m[5] * y + m[9]  * z;
    out[2] = m[2] * x + m[6] * y + m[10] * z;
    }

/** Quaternion product, xyzw: `out = a * b`, i.e. apply `b` in `a`'s local frame.

    `out` may alias `a` or `b`.

    The header had `mat4FromQuat` and `quatNlerp` but no product, and composing a
    small delta onto a node's authored rotation is the ordinary thing an
    application does with `setNodeRotation` - the Pokemon demo's synthesized idle
    needed exactly this and carried a private copy for a while. Blending two poses
    was covered; turning one pose slightly was not.
    */
inline void quatMul(const float a[4], const float b[4], float out[4])
    {
    // Read every input before writing, so out may alias either operand.
    const float ax = a[0], ay = a[1], az = a[2], aw = a[3];
    const float bx = b[0], by = b[1], bz = b[2], bw = b[3];
    out[0] = aw * bx + ax * bw + ay * bz - az * by;
    out[1] = aw * by - ax * bz + ay * bw + az * bx;
    out[2] = aw * bz + ax * by - ay * bx + az * bw;
    out[3] = aw * bw - ax * bx - ay * by - az * bz;
    }


/** Normalized linear interpolation of quaternions, taking the short path. */
inline void quatNlerp(const float a[4], const float b[4], float t, float out[4])
    {
    float dot = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
    const float sign = (dot < 0.0f) ? -1.0f : 1.0f;
    float len2 = 0.0f;
    for (int i = 0; i < 4; i++)
        {
        out[i] = a[i] * (1.0f - t) + b[i] * sign * t;
        len2 += out[i] * out[i];
        }
    if (len2 > 0.0f)
        {
        // No <cmath> dependency: two Newton steps on the inverse square root are
        // plenty for a value already close to 1.
        float inv = 1.0f;
        for (int i = 0; i < 3; i++) inv = inv * (1.5f - 0.5f * len2 * inv * inv);
        for (int i = 0; i < 4; i++) out[i] *= inv;
        }
    else
        {
        out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f;
        }
    }

/** Perspective projection, the gluPerspective matrix, column-major.

    Standard OpenGL convention: +y is UP in clip space. A rasterizer that draws
    into a framebuffer whose rows run downward needs that axis flipped, but the
    flip is a property of the screen it draws to, not of the projection, so it
    is applied by the backend and not baked in here.

    @param fovyDeg  vertical field of view, in degrees.
    @param aspect   width / height.
    @param zNear    near plane distance, positive.
    @param zFar     far plane distance, greater than zNear. */
inline void mat4Perspective(float fovyDeg, float aspect, float zNear, float zFar,
                            float out[16])
    {
    const float f = 1.0f / tanf(fovyDeg * 0.5f * 3.14159265358979323846f / 180.0f);
    for (int i = 0; i < 16; i++) out[i] = 0.0f;
    out[0]  = f / aspect;
    out[5]  = f;
    out[10] = (zFar + zNear) / (zNear - zFar);
    out[11] = -1.0f;
    out[14] = (2.0f * zFar * zNear) / (zNear - zFar);
    }

/** Orthographic projection, the glOrtho matrix, column-major.

    No perspective divide happens for an orthographic camera - w stays 1 - so a
    rasterizer that interpolates 1/w still works, it just interpolates a
    constant. */
inline void mat4Ortho(float left, float right, float bottom, float top,
                      float zNear, float zFar, float out[16])
    {
    for (int i = 0; i < 16; i++) out[i] = 0.0f;
    out[0]  =  2.0f / (right - left);
    out[5]  =  2.0f / (top - bottom);
    out[10] = -2.0f / (zFar - zNear);
    out[12] = -(right + left) / (right - left);
    out[13] = -(top + bottom) / (top - bottom);
    out[14] = -(zFar + zNear) / (zFar - zNear);
    out[15] =  1.0f;
    }

/** View matrix for a camera at `eye` looking at `center`, column-major.

    `up` is orthonormalised against the view direction first. Taking the cross
    product of two vectors that are merely unit-length - which is what the
    Khronos gluLookAt page does - leaves a result that is not unit-length, and
    the camera visibly shears when it looks steeply up or down. */
inline void mat4LookAt(const float eye[3], const float center[3], const float up[3],
                       float out[16])
    {
    // Exact reciprocal square roots, not invSqrt(): this runs once per camera
    // change, and invSqrt's 0.2% is a per-vertex bargain, not a per-frame one.
    // Approximating here tilts the whole camera basis and moves every pixel.
    float f[3] = { center[0] - eye[0], center[1] - eye[1], center[2] - eye[2] };
    float n = 1.0f / sqrtf(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
    f[0] *= n; f[1] *= n; f[2] *= n;

    float u[3] = { up[0], up[1], up[2] };
    n = 1.0f / sqrtf(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]);
    u[0] *= n; u[1] *= n; u[2] *= n;
    const float d = u[0]*f[0] + u[1]*f[1] + u[2]*f[2];
    u[0] -= d * f[0]; u[1] -= d * f[1]; u[2] -= d * f[2];
    n = 1.0f / sqrtf(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]);
    u[0] *= n; u[1] *= n; u[2] *= n;

    const float s[3] = { f[1]*u[2] - f[2]*u[1],
                         f[2]*u[0] - f[0]*u[2],
                         f[0]*u[1] - f[1]*u[0] };
    const float v[3] = { s[1]*f[2] - s[2]*f[1],
                         s[2]*f[0] - s[0]*f[2],
                         s[0]*f[1] - s[1]*f[0] };

    out[0] = s[0];  out[4] = s[1];  out[8]  = s[2];
    out[1] = v[0];  out[5] = v[1];  out[9]  = v[2];
    out[2] = -f[0]; out[6] = -f[1]; out[10] = -f[2];
    out[3] = 0.0f;  out[7] = 0.0f;  out[11] = 0.0f;
    out[12] = -(s[0]*eye[0] + s[1]*eye[1] + s[2]*eye[2]);
    out[13] = -(v[0]*eye[0] + v[1]*eye[1] + v[2]*eye[2]);
    out[14] =   f[0]*eye[0] + f[1]*eye[1] + f[2]*eye[2];
    out[15] = 1.0f;
    }

/** Negates the y row, turning a clip space with +y up into one with +y down. */
inline void mat4InvertY(float m[16])
    {
    m[1] = -m[1]; m[5] = -m[5]; m[9] = -m[9]; m[13] = -m[13];
    }

} // namespace a3d

#endif // A3D_MATH_H_
