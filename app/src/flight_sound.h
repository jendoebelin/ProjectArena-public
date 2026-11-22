// BALL IN FLIGHT (0.3.24; the owner, 2026-11-20: "it's hard to tell audibly when a ball is coming at you that is
// going to hurt you ... it's not increasing sound as it gets closer"; a player on Discord the same day: "so you can
// hear it coming ... in 4 square it would help you when your looking elsewhere"). Each ball or fireball that can hurt
// you is heard as it flies: a whoosh (noise through a band that opens as it nears) over a low hum whose pitch rises as
// it closes in fast. Louder the nearer it is and the straighter it comes at you; across or away, quieter; on your
// left or right as it is to your head. Made here, sample by sample: no sound file.
// flightParams turns a ball (where, how fast) and your head into how it sounds; FlightSynth plays it (the mixer: a few
// of them, smoothed between the game's frames). tools/flight_demo.cpp renders it to a .wav to listen to.
#pragma once
#include <cmath>
#include <cstdint>
#include "math3d.h"

struct FlightParams {
    float gain = 0;    // 0 silent .. 1 (a ball at you, a metre away)
    float pan = 0;     // -1 your left .. 1 your right
    float bright = 0;  // 0 far .. 1 near: the whoosh opens up
    float pitch = 1;   // 1 .. 1.6: the hum rises as it closes in fast
};

// a ball at `pos` flying at `vel`, heard from `head` (`right`: the head's right, flat)
inline FlightParams flightParams(const Vec3& pos, const Vec3& vel, const Vec3& head, const Vec3& right) {
    FlightParams f;
    const Vec3 d = head - pos;
    const float dist = length(d);
    if (dist < 1e-3f) return f;
    const float closing = dot(vel, d) / dist;   // m/s toward you (less than 0: going away)
    // nearer, louder: all of it within a metre, about a third at 6 m, a sixth at 12 m
    const float prox = std::min(1.f, 2.2f / (dist + 1.2f));
    // at you, all of it; the further off its line passes from your head, the less (a third at 2 m off); going away,
    // it fades out over a few metres a second
    const float sp = length(vel);
    float aim;
    if (sp < 0.5f) aim = 0.2f;
    else if (closing <= 0.f) aim = 0.2f * clampf(1.f + closing / 3.f, 0.f, 1.f);
    else {
        const float tc = dot(d, vel) / (sp * sp);   // seconds to its nearest
        const float miss = length(d - vel * tc);     // how far from your head it passes then
        aim = 0.3f + 0.7f * clampf(1.f - (miss - 0.4f) / 1.6f, 0.f, 1.f);
    }
    f.gain = prox * aim;
    f.bright = clampf(1.f - dist / 8.f, 0.f, 1.f);
    f.pitch = 1.f + 0.6f * clampf(closing / 15.f, 0.f, 1.f);
    f.pan = clampf(dot(pos - head, right) / dist, -1.f, 1.f) * 0.85f;
    return f;
}

// how it sounds (the owner, 2026-11-20, of the first: "close but not quite as subtle or blended in as I wanted"): the
// whoosh's band (lowHz far .. lowHz + spanHz near), its width (damp: higher, broader and softer) and level; a tone's
// level (hum, pitch rising as it closes in); a low rumble's (noise under 120 Hz, rising as it nears); all of it (level)
struct FlightStyle { float lowHz, spanHz, damp, whoosh, hum, rumble, level; };
static constexpr FlightStyle kFlightStyles[4] = {
    {500.f, 2700.f, 0.9f, 0.70f, 0.28f, 0.f, 1.f},     // 0: the first (11-20)
    {500.f, 2700.f, 0.9f, 0.70f, 0.06f, 0.f, 0.55f},   // A: the first, quieter, the hum all but gone
    {250.f, 1300.f, 1.4f, 0.80f, 0.f, 0.f, 0.77f},     // B: a soft rush of air: darker, broader, no tone
    {250.f, 1300.f, 1.4f, 0.72f, 0.f, 5.f, 0.55f},     // C: B with a low rumble under it as it nears
};
static constexpr int kFlightStyle = 2;   // (the one the game plays)

struct FlightSynth {
    FlightParams want;   // (set by the game, read by the mixer: under the mixer's lock)
    int style = kFlightStyle;
    void render(float* out, int frames, float level) {   // added into 48 kHz stereo
        const FlightStyle& st = kFlightStyles[style < 0 || style > 3 ? kFlightStyle : style];
        const float tl = want.gain * level * st.level * std::sqrt(0.5f * (1.f - want.pan));
        const float tr = want.gain * level * st.level * std::sqrt(0.5f * (1.f + want.pan));
        if (tl < 1e-4f && tr < 1e-4f && gL < 1e-4f && gR < 1e-4f) { gL = gR = 0; lp = bp = lo = lo2 = 0; return; }
        const float kPi2 = 6.2831853f;
        const float fcWant = 2.f * std::sin(0.5f * kPi2 * (st.lowHz + st.spanHz * want.bright) / 48000.f);
        const float hzWant = 120.f * want.pitch;
        const float fLow = 2.f * std::sin(0.5f * kPi2 * 120.f / 48000.f), rumbleWant = st.rumble * want.bright;
        for (int i = 0; i < frames; ++i) {
            gL += (tl - gL) * kGlide; gR += (tr - gR) * kGlide;   // (smoothed: the game sets it once a frame)
            fc += (fcWant - fc) * kGlide; hz += (hzWant - hz) * kGlide; rum += (rumbleWant - rum) * kGlide;
            rng = rng * 1664525u + 1013904223u;
            const float n = (float)(int32_t)rng * (1.f / 2147483648.f);
            const float hp = n - lp - st.damp * bp;   // a band-pass (state-variable filter): the whoosh
            bp += fc * hp;
            lp += fc * bp;
            float s = bp * st.whoosh;
            if (st.hum > 0.f) {
                phase += hz * (kPi2 / 48000.f);
                if (phase > kPi2) phase -= kPi2;
                s += (std::sin(phase) * 0.75f + std::sin(2.f * phase) * 0.2f) * st.hum;
            }
            if (st.rumble > 0.f) {   // (the same noise, twice through a low-pass under 120 Hz)
                lo += fLow * (n - lo);
                lo2 += fLow * (lo - lo2);
                s += lo2 * rum;
            }
            out[2 * i] += s * gL;
            out[2 * i + 1] += s * gR;
        }
    }
    static constexpr float kGlide = 0.0007f;   // per sample: about 30 ms to follow a change
    float gL = 0, gR = 0, fc = 0.07f, hz = 120.f, rum = 0, lp = 0, bp = 0, lo = 0, lo2 = 0, phase = 0;
    uint32_t rng = 0x9e3779b9u;
};
