// The original's matrix / quaternion helpers (row-major D3D matrices, row vectors), as it computes them: its own
// small ones (TMatrix4_t, Vector3_t) and the D3DX7 functions statically linked into randy31_orig.dll.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace rnative::xm {

struct M4 {
    float m[16];
};

inline M4 Identity()                                // FUN_1002a406
{
    return M4{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
}

inline M4 Load(const void* p)
{
    M4 r;
    std::memcpy(r.m, p, sizeof(r.m));
    return r;
}

// a * b (D3DXMatrixMultiply, FUN_1006e302).
inline M4 Mul(const M4& a, const M4& b)
{
    M4 r;
    for (int i = 0; i < 4; ++i) {
        const float* row = a.m + i * 4;
        for (int j = 0; j < 4; ++j)
            r.m[i * 4 + j] = b.m[12 + j] * row[3] + b.m[8 + j] * row[2] + b.m[4 + j] * row[1] + row[0] * b.m[j];
    }
    return r;
}

// A 4x3 bone matrix (rows of three) as 4x4 (FUN_10053f05).
inline M4 From43(const float* bone)
{
    M4 r = Identity();
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 3; ++j) r.m[i * 4 + j] = bone[i * 3 + j];
    return r;
}

// The rotation of a quaternion (x, y, z, w), no translation (D3DXMatrixRotationQuaternion, FUN_1006ebed).
inline M4 FromQuaternion(const float* q)
{
    const float x2 = q[0] * 2.0f, y2 = q[1] * 2.0f, zz2 = q[2] * q[2] * 2.0f, zw2 = q[2] * 2.0f * q[3];
    const float xx = 1.0f - q[0] * x2;
    M4 r = Identity();
    r.m[0] = (1.0f - q[1] * y2) - zz2;
    r.m[1] = zw2 + q[1] * x2;
    r.m[2] = q[2] * x2 - q[3] * y2;
    r.m[4] = q[1] * x2 - zw2;
    r.m[5] = xx - zz2;
    r.m[6] = q[2] * y2 + q[3] * x2;
    r.m[8] = q[3] * y2 + q[2] * x2;
    r.m[9] = q[2] * y2 - q[3] * x2;
    r.m[10] = xx - q[1] * y2;
    return r;
}

// The rotation of a matrix as a quaternion (D3DXQuaternionRotationMatrix, FUN_1006ed0b): t is the transpose.
inline void ToQuaternion(const M4& m, float* q)
{
    float t[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) t[i * 4 + j] = m.m[j * 4 + i];
    const float trace = t[10] + (t[5] + t[0]);
    if (trace >= 0.0f) {
        float s = std::sqrt(t[15] + trace);
        q[3] = s * 0.5f;
        const float f = 0.5f / s;
        q[0] = (t[9] - t[6]) * f;
        q[1] = (t[2] - t[8]) * f;
        q[2] = (t[4] - t[1]) * f;
        return;
    }
    int i = t[0] < t[5] ? 1 : 0;
    if (t[i * 5] < t[10]) i = 2;
    if (i == 0) {
        float s = std::sqrt((t[0] - (t[5] + t[10])) + t[15]);
        q[0] = s * 0.5f;
        const float f = 0.5f / s;
        q[1] = (t[1] + t[4]) * f;
        q[2] = (t[8] + t[2]) * f;
        q[3] = (t[9] - t[6]) * f;
    } else if (i == 1) {
        float s = std::sqrt((t[5] - (t[10] + t[0])) + t[15]);
        q[1] = s * 0.5f;
        const float f = 0.5f / s;
        q[2] = (t[6] + t[9]) * f;
        q[0] = (t[1] + t[4]) * f;
        q[3] = (t[2] - t[8]) * f;
    } else {
        float s = std::sqrt((t[10] - (t[0] + t[5])) + t[15]);
        q[2] = s * 0.5f;
        const float f = 0.5f / s;
        q[0] = (t[8] + t[2]) * f;
        q[1] = (t[6] + t[9]) * f;
        q[3] = (t[4] - t[1]) * f;
    }
}

// Every row's x, y, z scaled (FUN_10017f66).
inline void ScaleColumns(M4& m, float x, float y, float z)
{
    for (int i = 0; i < 4; ++i) {
        m.m[i * 4 + 0] *= x;
        m.m[i * 4 + 1] *= y;
        m.m[i * 4 + 2] *= z;
    }
}

// A point through a matrix (FUN_10015601).
inline void Transform(const float* p, const float* m, float* out)
{
    const float x = p[0], y = p[1], z = p[2];
    out[0] = m[8] * z + m[0] * x + m[4] * y + m[12];
    out[1] = m[9] * z + m[5] * y + m[1] * x + m[13];
    out[2] = m[10] * z + m[6] * y + m[2] * x + m[14];
}

// ---- Randy's own vector / quaternion helpers (Vector3_t, Quaternion_t), in its order of operations ----

inline float LengthSquared(const float* v) { return (v[1] * v[1] + v[0] * v[0]) + v[2] * v[2]; }   // FUN_10016851

// FUN_10018833: v scaled to length `length`.
inline void Normalize(float* v, float length = 1.0f)
{
    const float len = std::sqrt(LengthSquared(v));
    const float f = (1.0f / len) * length;           // FUN_10017f2b (1 / length), then times `length`
    v[0] *= f, v[1] *= f, v[2] *= f;
}

inline void Cross(const float* a, const float* b, float* out)   // FUN_1002a358
{
    const float x = b[2] * a[1] - b[1] * a[2];
    const float y = a[2] * b[0] - a[0] * b[2];
    const float z = b[1] * a[0] - b[0] * a[1];
    out[0] = x, out[1] = y, out[2] = z;
}

// FUN_10044b5d: the shortest rotation taking direction `a` to `b` (both unit length).
inline void RotateTo(const float* a, const float* b, float* q)
{
    float h[3] = {b[0] + a[0], b[1] + a[1], b[2] + a[2]};   // the half-way direction
    float lenSq = LengthSquared(h);
    if (lenSq < 1e-12f) {                            // opposite: any perpendicular
        h[0] = a[2], h[1] = 0.0f, h[2] = -a[0];
        lenSq = h[0] * h[0] + h[2] * h[2];
    }
    const float f = 1.0f / std::sqrt(lenSq);
    h[0] *= f, h[1] *= f, h[2] *= f;
    q[0] = a[1] * h[2] - a[2] * h[1];
    q[1] = a[2] * h[0] - a[0] * h[2];
    q[2] = a[0] * h[1] - a[1] * h[0];
    q[3] = (a[1] * h[1] + a[0] * h[0]) + a[2] * h[2];
}

// FUN_1002ce4b: a rotation of `angle` radians around `axis` (angles outside [0, 2 pi) wrapped).
inline void AxisAngle(const float* axis, float angle, float* q)
{
    float half;
    if (angle < 0.0f || double(angle) >= 6.283185307179586) {
        const double turns = double(float(double(angle) / 6.2831854820251465));
        const float whole = float(std::floor(turns));
        half = float(double(float(turns - double(whole))) * 3.1415927410125732);
    } else {
        half = float(double(angle) * 0.5);
    }
    const float s = float(std::sin(double(half)));
    q[0] = axis[0] * s, q[1] = axis[1] * s, q[2] = axis[2] * s;
    q[3] = float(std::cos(double(half)));
}

// FUN_1002a43c: v rotated by the (unit) quaternion q.
inline void RotateVector(float* v, const float* q)
{
    float t1[3], t2[3];
    Cross(q, v, t1);
    Cross(q, t1, t2);
    const float w2 = q[3] * 2.0f;
    v[0] = v[0] + t1[0] * w2 + t2[0] * 2.0f;
    v[1] = v[1] + t1[1] * w2 + t2[1] * 2.0f;
    v[2] = w2 * t1[2] + v[2] + 2.0f * t2[2];
}

// FUN_100170cf: a = a * b (quaternions).
inline void QuatMul(float* a, const float* b)
{
    const float x = a[0], y = a[1], z = a[2], w = a[3];
    a[3] = w * b[3] - (b[2] * z + b[0] * x + y * b[1]);
    a[0] = x * b[3] + ((b[1] * z + w * b[0]) - b[2] * y);
    a[1] = y * b[3] + (b[2] * x + (w * b[1] - b[0] * z));
    a[2] = (b[3] * z + b[2] * w) + (y * b[0] - x * b[1]);
}

}  // namespace rnative::xm
