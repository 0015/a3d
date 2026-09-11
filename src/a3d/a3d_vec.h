// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file a3d_vec.h
 * @brief Vec3, Vec4 and Mat4 - the arithmetic an application does around a
 *        renderer, rather than inside it.
 *
 * a3d's own pipeline takes matrices as `float[16]` and never needs a class.
 * An APPLICATION does: placing a marker on a globe, building a transform for a
 * pin, projecting a point to find out where to draw its label. Making every
 * caller write that with bare arrays is how sign errors get written.
 *
 *
 * COLUMN-MAJOR, matching a3d_math.h, glTF and OpenGL: `M[col * 4 + row]`, and
 * the translation lives in `M[12..14]`. `Mat4::M` is directly usable wherever
 * a3d asks for a `const float*`.
 */

#ifndef A3D_VEC_H_
#define A3D_VEC_H_

#include <math.h>

#include "a3d/a3d_math.h"

namespace a3d {

struct Vec3
    {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    Vec3() = default;
    Vec3(float X, float Y, float Z) : x(X), y(Y), z(Z) {}

    Vec3 operator+(const Vec3& o) const { return { x + o.x, y + o.y, z + o.z }; }
    Vec3 operator-(const Vec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
    Vec3 operator*(float s) const       { return { x * s, y * s, z * s }; }
    Vec3 operator-() const              { return { -x, -y, -z }; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s)       { x *= s; y *= s; z *= s; return *this; }

    float norm2() const { return x * x + y * y + z * z; }
    float norm() const  { return sqrtf(norm2()); }

    /** Scale to unit length. A zero vector is left alone rather than turned
        into a NaN that only shows up three frames later. */
    void normalize()
        {
        const float n2 = norm2();
        if (n2 > 1e-20f) { const float inv = 1.0f / sqrtf(n2); x *= inv; y *= inv; z *= inv; }
        }
    };

struct Vec4
    {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
    Vec4() = default;
    Vec4(float X, float Y, float Z, float W) : x(X), y(Y), z(Z), w(W) {}
    Vec3 xyz() const { return { x, y, z }; }

    /** Perspective divide, in place. A w of zero is left alone: a point on the
        eye plane has no screen position, and dividing by it invents one. */
    void zdivide()
        {
        if ((w > 1e-20f) || (w < -1e-20f))
            { const float iw = 1.0f / w; x *= iw; y *= iw; z *= iw; w = 1.0f; }
        }
    };

inline float dot(const Vec3& a, const Vec3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }

inline Vec3 crossProduct(const Vec3& a, const Vec3& b)
    {
    return { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
    }

inline Vec3 normalize(Vec3 v) { v.normalize(); return v; }
struct Mat4
    {
    float M[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

    void setIdentity() { mat4Identity(M); }

    void setScale(float sx, float sy, float sz) { setScale(Vec3(sx, sy, sz)); }

    void setScale(const Vec3& s)
        {
        mat4Identity(M);
        M[0] = s.x; M[5] = s.y; M[10] = s.z;
        }

    /** PRE-multiply by a scale. */
    void multScale(const Vec3& s)
        {
        float S[16]; mat4Identity(S);
        S[0] = s.x; S[5] = s.y; S[10] = s.z;
        float out[16]; mat4Multiply(S, M, out); mat4Copy(out, M);
        }
    void multScale(float sx, float sy, float sz) { multScale(Vec3(sx, sy, sz)); }

    /** PRE-multiply: `*this = translate(t) * *this`.

        The order is the whole point and it is the opposite of what the name
        suggests to most readers. `M.setScale(); M.multRotate(); M.multTranslate()`
        builds `T * R * S`, which scales, then rotates, then moves - an object
        that spins in place and is then pushed away from the camera.

        Post-multiplying instead builds `S * R * T`: the translation is applied
        FIRST, so the object is pushed away and then rotated about the origin.
        It orbits instead of spinning, and the difference does not look like a
        matrix bug - it looks like the wrong centre of rotation. */
    void multTranslate(const Vec3& t)
        {
        float T[16]; mat4Identity(T);
        T[12] = t.x; T[13] = t.y; T[14] = t.z;
        float out[16]; mat4Multiply(T, M, out); mat4Copy(out, M);
        }

    /** PRE-multiply: `*this = rotate(angleDeg, axis) * *this`, right-handed.
        See multTranslate for why the order matters. */
    void multRotate(float angleDeg, const Vec3& axis)
        {
        Vec3 a = normalize(axis);
        const float r = angleDeg * 3.14159265358979323846f / 180.0f;
        const float c = cosf(r), s = sinf(r), ic = 1.0f - c;
        float R[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        R[0]  = c + a.x*a.x*ic;      R[1]  = a.y*a.x*ic + a.z*s;  R[2]  = a.z*a.x*ic - a.y*s;
        R[4]  = a.x*a.y*ic - a.z*s;  R[5]  = c + a.y*a.y*ic;      R[6]  = a.z*a.y*ic + a.x*s;
        R[8]  = a.x*a.z*ic + a.y*s;  R[9]  = a.y*a.z*ic - a.x*s;  R[10] = c + a.z*a.z*ic;
        float out[16]; mat4Multiply(R, M, out); mat4Copy(out, M);
        }

    void setPerspective(float fovyDeg, float aspect, float zNear, float zFar)
        { mat4Perspective(fovyDeg, aspect, zNear, zFar, M); }

    void setOrtho(float left, float right, float bottom, float top,
                  float zNear, float zFar)
        { mat4Ortho(left, right, bottom, top, zNear, zFar, M); }

    void setLookAt(const Vec3& eye, const Vec3& centre, const Vec3& up)
        {
        const float e[3] = { eye.x, eye.y, eye.z };
        const float c[3] = { centre.x, centre.y, centre.z };
        const float u[3] = { up.x, up.y, up.z };
        mat4LookAt(e, c, u, M);
        }

    /** Negate the y row: +y-up clip space becomes +y-down. */
    void invertYaxis() { mat4InvertY(M); }

    /** Transform as a DIRECTION - the translation column is not applied. */
    Vec4 mult0(const Vec3& v) const
        {
        return { M[0]*v.x + M[4]*v.y + M[8]*v.z,
                 M[1]*v.x + M[5]*v.y + M[9]*v.z,
                 M[2]*v.x + M[6]*v.y + M[10]*v.z,
                 M[3]*v.x + M[7]*v.y + M[11]*v.z };
        }

    /** Transform as a POINT. */
    Vec4 mult1(const Vec3& v) const
        {
        return { M[0]*v.x + M[4]*v.y + M[8]*v.z + M[12],
                 M[1]*v.x + M[5]*v.y + M[9]*v.z + M[13],
                 M[2]*v.x + M[6]*v.y + M[10]*v.z + M[14],
                 M[3]*v.x + M[7]*v.y + M[11]*v.z + M[15] };
        }

    Mat4 operator*(const Mat4& o) const
        { Mat4 r; mat4Multiply(M, o.M, r.M); return r; }

    Vec4 operator*(const Vec4& v) const
        {
        return { M[0]*v.x + M[4]*v.y + M[8]*v.z + M[12]*v.w,
                 M[1]*v.x + M[5]*v.y + M[9]*v.z + M[13]*v.w,
                 M[2]*v.x + M[6]*v.y + M[10]*v.z + M[14]*v.w,
                 M[3]*v.x + M[7]*v.y + M[11]*v.z + M[15]*v.w };
        }
    };

} // namespace a3d

#endif // A3D_VEC_H_
