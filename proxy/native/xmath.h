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

}  // namespace rnative::xm
