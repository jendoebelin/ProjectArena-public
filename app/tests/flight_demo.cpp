// BALL IN FLIGHT, to listen to (app/src/flight_sound.h): a few balls flown past a still listener, rendered to a .wav
// as the game would play them (the game sets the sound once a frame, at 72 Hz; the mixer smooths it), with each one's
// loudness every quarter second printed. Usage: flight_demo out.wav [style: 0 the first, 1 A, 2 B, 3 C; the game's]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../src/flight_sound.h"

struct Shot { const char* what; Vec3 from, vel; bool hop; float until; };

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "ball_flight_demo.wav";
    const int style = argc > 2 ? std::atoi(argv[2]) : kFlightStyle;
    const Vec3 head{0, 1.6f, 0}, right{1, 0, 0};   // (facing -Z)
    const Shot shots[] = {
        {"a ball thrown straight at you from 12 m, 11 m/s (it hits you)", {0, 1.5f, -12.f}, {0, 0.f, 11.f}, false, 12.f / 11.f},
        {"a ball passing 1.5 m to your left, 11 m/s", {-1.5f, 1.5f, -12.f}, {0, 0, 11.f}, false, 2.2f},
        {"your own ball, thrown away from you, 11 m/s", {0.3f, 1.4f, -0.5f}, {0, 0.f, -11.f}, false, 1.5f},
        {"a fireball hopping at you from 10 m, 9 m/s (it hits you)", {0.4f, 1.0f, -10.f}, {0, 0, 9.f}, true, 10.f / 9.f},
        {"a fast one, 16 m/s, from your right, at you", {9.f, 1.5f, -6.f}, {-12.f, 0, 8.f}, false, 0.75f},
    };
    const int rate = 48000, perFrame = rate / 72;
    std::vector<float> pcm;
    for (const Shot& s : shots) {
        FlightSynth syn;
        syn.style = style;
        std::vector<float> one;
        Vec3 p = s.from, v = s.vel;
        const float total = s.until + 0.6f;   // (and a little after: it goes quiet)
        std::printf("%s\n  loudness every 0.25 s:", s.what);
        double acc = 0;
        int accN = 0;
        for (float t = 0; t < total; t += 1.f / 72) {
            const bool flying = t < s.until;
            if (flying) {
                if (s.hop) {   // (a fireball: down to the floor, back up to the waist)
                    v.y -= 12.f / 72;
                    if (p.y + v.y / 72 < 0.12f && v.y < 0) v.y = std::sqrt(2.f * 12.f * 0.88f);
                }
                p += v * (1.f / 72);
                syn.want = flightParams(p, v, head, right);
            } else {
                syn.want = FlightParams{};   // (it hit you, or went out: gone)
            }
            std::vector<float> buf(perFrame * 2, 0.f);
            syn.render(buf.data(), perFrame, 1.f);
            for (int i = 0; i < perFrame; ++i) { acc += buf[2 * i] * buf[2 * i] + buf[2 * i + 1] * buf[2 * i + 1]; accN += 2; }
            if (accN >= rate / 4 * 2) { std::printf(" %.3f", std::sqrt(acc / accN)); acc = 0; accN = 0; }
            one.insert(one.end(), buf.begin(), buf.end());
        }
        std::printf("\n");
        pcm.insert(pcm.end(), one.begin(), one.end());
        pcm.insert(pcm.end(), rate * 2 / 2, 0.f);   // half a second between them
    }
    float peak = 0;
    for (float x : pcm) peak = std::max(peak, std::fabs(x));
    std::printf("peak %.3f (1.0 = full scale, before the game's volume)\n", peak);
    FILE* f = std::fopen(path, "wb");
    if (!f) return 1;
    const uint32_t n = (uint32_t)pcm.size(), bytes = n * 2;
    auto u32 = [&](uint32_t x) { std::fwrite(&x, 4, 1, f); };
    auto u16 = [&](uint16_t x) { std::fwrite(&x, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2); u32(rate); u32(rate * 4); u16(4); u16(16);
    std::fwrite("data", 1, 4, f); u32(bytes);
    for (float x : pcm) { const int16_t s = (int16_t)std::lround(clampf(x, -1.f, 1.f) * 32767.f); std::fwrite(&s, 2, 1, f); }
    std::fclose(f);
    std::printf("wrote %s (%.1f s)\n", path, n / 2.0 / rate);
    return 0;
}
