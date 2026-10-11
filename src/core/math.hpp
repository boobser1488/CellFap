#pragma once
#include <cmath>
#include <cstdint>

constexpr float kPi = 3.14159265358979f;
constexpr float kTau = 6.28318530717959f;

struct Vec2 {
    float x = 0, y = 0;
    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}
    constexpr Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    constexpr Vec2 operator*(float s) const { return {x * s, y * s}; }
    constexpr Vec2 operator/(float s) const { return {x / s, y / s}; }
    constexpr Vec2 operator-() const { return {-x, -y}; }
    Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(Vec2 o) { x -= o.x; y -= o.y; return *this; }
    Vec2& operator*=(float s) { x *= s; y *= s; return *this; }
};

inline float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline float cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
inline float length(Vec2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }
inline float length2(Vec2 a) { return a.x * a.x + a.y * a.y; }
inline Vec2 perp(Vec2 a) { return {-a.y, a.x}; }
inline Vec2 fromAngle(float a) { return {std::cos(a), std::sin(a)}; }
inline Vec2 normalize(Vec2 a) {
    float l = length(a);
    return l > 1e-12f ? a / l : Vec2{1, 0};
}
inline float wrapAngle(float a) {
    a = std::fmod(a + kPi, kTau);
    if (a < 0) a += kTau;
    return a - kPi;
}
template <class T>
inline T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float mixf(float a, float b, float t) { return a + (b - a) * t; }
inline float deg2rad(float d) { return d * (kPi / 180.0f); }
inline float rad2deg(float r) { return r * (180.0f / kPi); }

// Small fast PRNG (xorshift64*), one per thread / per world.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed = 0x9E3779B97F4A7C15ull) : s(seed ? seed : 1) {}
    uint64_t next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 2685821657736338717ull;
    }
    float uniform() { return (float)(next() >> 40) * (1.0f / 16777216.0f); }
    float range(float a, float b) { return a + (b - a) * uniform(); }
    int irange(int a, int b) { return a + (int)(next() % (uint64_t)(b - a + 1)); }  // inclusive
    bool chance(float p) { return uniform() < p; }
    float normal() {
        float u1 = std::fmax(uniform(), 1e-7f), u2 = uniform();
        return std::sqrt(-2.0f * std::log(u1)) * std::cos(kTau * u2);
    }
};

struct Color {
    float r = 1, g = 1, b = 1;
};
inline uint32_t packColor(Color c, float a = 1.0f) {
    auto q = [](float v) { return (uint32_t)(clampv(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return q(c.r) | (q(c.g) << 8) | (q(c.b) << 16) | (q(a) << 24);
}
inline Color hsv(float h, float s, float v) {
    h = h - std::floor(h);
    float i = std::floor(h * 6.0f), f = h * 6.0f - i;
    float p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    switch ((int)i % 6) {
        case 0: return {v, t, p};
        case 1: return {q, v, p};
        case 2: return {p, v, t};
        case 3: return {p, q, v};
        case 4: return {t, p, v};
        default: return {v, p, q};
    }
}
