// Minimal vector / quaternion / matrix math for the Sparc Quest port.
// App space: meters, right-handed, +Y up (see tools/build_open_assets.py).
#pragma once
#include <cmath>
#include <algorithm>

struct Vec2 { float x = 0, y = 0; };

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
};
inline Vec3 operator*(float s, const Vec3& v) { return v * s; }
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }
inline float length2(const Vec3& v) { return dot(v, v); }
inline Vec3 normalize(const Vec3& v) { float l = length(v); return l > 1e-8f ? v / l : Vec3{0, 0, 0}; }
inline Vec3 lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }
inline float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
constexpr float kPi = 3.14159265358979f;
inline float deg2rad(float d) { return d * kPi / 180.f; }
inline float rad2deg(float r) { return r * 180.f / kPi; }

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
    Quat() = default;
    constexpr Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    static Quat axisAngle(const Vec3& axis, float rad) {
        Vec3 a = normalize(axis);
        float s = std::sin(rad * 0.5f);
        return {a.x * s, a.y * s, a.z * s, std::cos(rad * 0.5f)};
    }
    Quat operator*(const Quat& b) const {
        return {w * b.x + x * b.w + y * b.z - z * b.y,
                w * b.y - x * b.z + y * b.w + z * b.x,
                w * b.z + x * b.y - y * b.x + z * b.w,
                w * b.w - x * b.x - y * b.y - z * b.z};
    }
    Quat conj() const { return {-x, -y, -z, w}; }
    Vec3 rotate(const Vec3& v) const {
        Vec3 u{x, y, z};
        Vec3 t = cross(u, v) * 2.f;
        return v + t * w + cross(u, t);
    }
};
// A grip-space rotation from pitch (about X), yaw (Y) and roll (Z) in degrees, applied yaw * pitch * roll. left: its
// mirror image across grip X (left-hand parts are the right hand's mirrored: yaw and roll change sign).
inline Quat gripEuler(const Vec3& deg, bool left) {
    const float s = left ? -1.f : 1.f;
    return Quat::axisAngle({0, 1, 0}, deg2rad(s * deg.y)) * Quat::axisAngle({1, 0, 0}, deg2rad(deg.x)) *
           Quat::axisAngle({0, 0, 1}, deg2rad(s * deg.z));
}
inline Quat nlerp(const Quat& a, Quat b, float t) {
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) b = {-b.x, -b.y, -b.z, -b.w};
    Quat r{lerpf(a.x, b.x, t), lerpf(a.y, b.y, t), lerpf(a.z, b.z, t), lerpf(a.w, b.w, t)};
    float l = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
    return {r.x / l, r.y / l, r.z / l, r.w / l};
}

// Shortest-arc rotation taking unit vector a to unit vector b.
inline Quat fromTo(const Vec3& a, const Vec3& b) {
    Vec3 c = cross(a, b);
    float d = dot(a, b);
    if (d < -0.99999f) {
        Vec3 axis = std::fabs(a.x) < 0.9f ? cross(a, {1, 0, 0}) : cross(a, {0, 1, 0});
        return Quat::axisAngle(axis, kPi);
    }
    float s = std::sqrt((1 + d) * 2);
    return {c.x / s, c.y / s, c.z / s, s * 0.5f};
}

struct Pose {
    Quat q;
    Vec3 p;
    Vec3 transform(const Vec3& v) const { return q.rotate(v) + p; }
    Pose operator*(const Pose& child) const { return {q * child.q, transform(child.p)}; }
};

// Column-major 4x4 (OpenGL convention).
struct Mat4 {
    float m[16];
    static Mat4 identity() { Mat4 r{}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1; return r; }
    Mat4 operator*(const Mat4& b) const {
        Mat4 r{};
        for (int c = 0; c < 4; ++c)
            for (int rr = 0; rr < 4; ++rr) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += m[k * 4 + rr] * b.m[c * 4 + k];
                r.m[c * 4 + rr] = s;
            }
        return r;
    }
    static Mat4 fromPose(const Pose& p, const Vec3& scale = {1, 1, 1}) {
        const Quat& q = p.q;
        float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z, wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        Mat4 r{};
        r.m[0] = (1 - 2 * (yy + zz)) * scale.x; r.m[1] = 2 * (xy + wz) * scale.x;       r.m[2] = 2 * (xz - wy) * scale.x;
        r.m[4] = 2 * (xy - wz) * scale.y;       r.m[5] = (1 - 2 * (xx + zz)) * scale.y; r.m[6] = 2 * (yz + wx) * scale.y;
        r.m[8] = 2 * (xz + wy) * scale.z;       r.m[9] = 2 * (yz - wx) * scale.z;       r.m[10] = (1 - 2 * (xx + yy)) * scale.z;
        r.m[12] = p.p.x; r.m[13] = p.p.y; r.m[14] = p.p.z; r.m[15] = 1;
        return r;
    }
    // Columns x, y, z (any lengths / handedness) and origin o.
    static Mat4 fromBasis(const Vec3& x, const Vec3& y, const Vec3& z, const Vec3& o) {
        Mat4 r{};
        r.m[0] = x.x; r.m[1] = x.y; r.m[2] = x.z;
        r.m[4] = y.x; r.m[5] = y.y; r.m[6] = y.z;
        r.m[8] = z.x; r.m[9] = z.y; r.m[10] = z.z;
        r.m[12] = o.x; r.m[13] = o.y; r.m[14] = o.z; r.m[15] = 1;
        return r;
    }
    // Rigid inverse (rotation + translation only).
    static Mat4 viewFromPose(const Pose& eye) {
        Pose inv{eye.q.conj(), eye.q.conj().rotate(-eye.p)};
        return fromPose(inv);
    }
    // OpenXR asymmetric-FOV projection (angles in radians), reversed-handedness as GL expects.
    static Mat4 projection(float l, float r, float u, float d, float n, float f) {
        float tl = std::tan(l), tr = std::tan(r), tu = std::tan(u), td = std::tan(d);
        float w = tr - tl, h = tu - td;
        Mat4 p{};
        p.m[0] = 2 / w; p.m[8] = (tr + tl) / w;
        p.m[5] = 2 / h; p.m[9] = (tu + td) / h;
        p.m[10] = -(f + n) / (f - n); p.m[14] = -2 * f * n / (f - n);
        p.m[11] = -1;
        return p;
    }
};
