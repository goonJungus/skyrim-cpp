// math.hpp - minimal row-vector math for the host renderer.
//
// Deliberately matches the Direct3D 9 convention that the rest of the engine
// assumes: row-vector matrices (v * M), translation in the last row, and
// left-handed view/projection. Keeping the convention explicit here means the
// renderer can hand matrices straight to SetTransform without transposes.
//
// Header-only and standard library only so it compiles anywhere.

#pragma once

#include <cmath>

namespace skyrim::host {

constexpr float kPi = 3.14159265358979323846f;

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }

inline Vec3 normalize(const Vec3& v) {
    const float len = length(v);
    return len > 1e-6f ? v * (1.0f / len) : Vec3{0.0f, 0.0f, 0.0f};
}

// Row-major, row-vector: element (row, col) is m[row * 4 + col].
struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

inline Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            r.m[i * 4 + j] = a.m[i * 4 + 0] * b.m[0 * 4 + j] +
                             a.m[i * 4 + 1] * b.m[1 * 4 + j] +
                             a.m[i * 4 + 2] * b.m[2 * 4 + j] +
                             a.m[i * 4 + 3] * b.m[3 * 4 + j];
        }
    }
    return r;
}

inline Mat4 translation(float x, float y, float z) {
    Mat4 r;
    r.m[12] = x; r.m[13] = y; r.m[14] = z;
    return r;
}

inline Mat4 scaling(float s) {
    Mat4 r;
    r.m[0] = s; r.m[5] = s; r.m[10] = s;
    return r;
}

inline Mat4 rotationX(float a) {
    Mat4 r;
    const float c = std::cos(a), s = std::sin(a);
    r.m[5] = c; r.m[6] = s; r.m[9] = -s; r.m[10] = c;
    return r;
}

inline Mat4 rotationY(float a) {
    Mat4 r;
    const float c = std::cos(a), s = std::sin(a);
    r.m[0] = c; r.m[2] = -s; r.m[8] = s; r.m[10] = c;
    return r;
}

inline Mat4 rotationZ(float a) {
    Mat4 r;
    const float c = std::cos(a), s = std::sin(a);
    r.m[0] = c; r.m[1] = s; r.m[4] = -s; r.m[5] = c;
    return r;
}

// Yaw (about Z, since the world is Z-up), then pitch (about Y), then roll
// (about X) -- applied scale -> roll -> pitch -> yaw -> translate.
inline Mat4 instanceMatrix(float x, float y, float z,
                           float rotX, float rotY, float rotZ, float s) {
    Mat4 m = scaling(s);
    m = multiply(m, rotationX(rotX));
    m = multiply(m, rotationY(rotY));
    m = multiply(m, rotationZ(rotZ));
    m = multiply(m, translation(x, y, z));
    return m;
}

// Left-handed look-at, matching D3DXMatrixLookAtLH.
inline Mat4 lookAtLH(const Vec3& eye, const Vec3& at, const Vec3& up) {
    const Vec3 zaxis = normalize(at - eye);
    const Vec3 xaxis = normalize(cross(up, zaxis));
    const Vec3 yaxis = cross(zaxis, xaxis);
    Mat4 r;
    r.m[0] = xaxis.x; r.m[1] = yaxis.x; r.m[2] = zaxis.x; r.m[3] = 0.0f;
    r.m[4] = xaxis.y; r.m[5] = yaxis.y; r.m[6] = zaxis.y; r.m[7] = 0.0f;
    r.m[8] = xaxis.z; r.m[9] = yaxis.z; r.m[10] = zaxis.z; r.m[11] = 0.0f;
    r.m[12] = -dot(xaxis, eye);
    r.m[13] = -dot(yaxis, eye);
    r.m[14] = -dot(zaxis, eye);
    r.m[15] = 1.0f;
    return r;
}

// Left-handed perspective projection, matching D3DXMatrixPerspectiveFovLH.
inline Mat4 perspectiveFovLH(float fovY, float aspect, float zn, float zf) {
    const float h = 1.0f / std::tan(fovY * 0.5f);
    const float w = h / aspect;
    Mat4 r;
    r.m[0] = w; r.m[5] = h;
    r.m[10] = zf / (zf - zn);
    r.m[11] = 1.0f;
    r.m[14] = -zn * zf / (zf - zn);
    r.m[15] = 0.0f;
    return r;
}

inline Vec3 transformPoint(const Mat4& m, const Vec3& v) {
    return {v.x * m.m[0] + v.y * m.m[4] + v.z * m.m[8] + m.m[12],
            v.x * m.m[1] + v.y * m.m[5] + v.z * m.m[9] + m.m[13],
            v.x * m.m[2] + v.y * m.m[6] + v.z * m.m[10] + m.m[14]};
}

struct Aabb {
    Vec3 mn{1e30f, 1e30f, 1e30f};
    Vec3 mx{-1e30f, -1e30f, -1e30f};
    bool valid() const { return mn.x <= mx.x; }
    void add(const Vec3& p) {
        if (p.x < mn.x) mn.x = p.x;
        if (p.y < mn.y) mn.y = p.y;
        if (p.z < mn.z) mn.z = p.z;
        if (p.x > mx.x) mx.x = p.x;
        if (p.y > mx.y) mx.y = p.y;
        if (p.z > mx.z) mx.z = p.z;
    }
};

}  // namespace skyrim::host
