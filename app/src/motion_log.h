// MOTION LOG (developer mode only; the owner, 2026-11-27, for the jump-rope victory: "astrobleem and I recorded
// ourselves jumping rope, he is much taller than me so you should get a good variance"). Their videos show what the
// avatar's arms did, not where the hands really went: this is where they went. While the avatar builder is open, the
// head and both hands every frame, as the game has them (world space, metres; the controllers' grips), to
// <files>/motion_log.csv (main.cpp motionLog). Replayed on the PC (--scene builder --motion FILE TAKE SECONDS
// [--motionout CSV]) through the game's own rig, to see and measure the arms on real motion and to make animations
// from. Each opening of the builder is a take ("# take," the version, the look); "# look," when the look changes.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "game.h"
#include "avatar.h"

namespace motionlog {

constexpr const char* kHeader =
    "t,head_tracked,hx,hy,hz,hqx,hqy,hqz,hqw,"
    "l_tracked,lx,ly,lz,lqx,lqy,lqz,lqw,l_trigger,l_squeeze,"
    "r_tracked,rx,ry,rz,rqx,rqy,rqz,rqw,r_trigger,r_squeeze";
constexpr int kFields = 29;

// avatar.cfg's lines on one line, comma separated
inline std::string lookText(const AvatarDesc& d) {
    std::string s = avatar::toText(d);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    for (char& c : s)
        if (c == '\n') c = ',';
    return s;
}

inline std::string line(double t, const PlayerInput& in) {
    char s[640];
    int n = std::snprintf(s, sizeof s, "%.4f,%d,%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.5f", t, in.headTracked ? 1 : 0, in.head.p.x,
                          in.head.p.y, in.head.p.z, in.head.q.x, in.head.q.y, in.head.q.z, in.head.q.w);
    for (int h = 0; h < 2 && n > 0 && n < (int)sizeof s; ++h) {
        const HandInput& hi = in.hand[h];
        n += std::snprintf(s + n, sizeof s - n, ",%d,%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.5f,%.2f,%.2f", hi.tracked ? 1 : 0,
                           hi.grip.p.x, hi.grip.p.y, hi.grip.p.z, hi.grip.q.x, hi.grip.q.y, hi.grip.q.z, hi.grip.q.w,
                           hi.trigger, hi.squeeze);
    }
    return s;
}

struct Frame {
    double t = 0;   // seconds since the take began
    PlayerInput in;
};
struct Take {
    std::string version;
    std::vector<Frame> frames;
};

// a file's takes, in order (the look of each frame: the last "# take," or "# look," line before it)
inline std::vector<Take> load(const char* path) {
    std::vector<Take> takes;
    FILE* f = std::fopen(path, "r");
    if (!f) return takes;
    char buf[2048];
    AvatarDesc look;
    while (std::fgets(buf, sizeof buf, f)) {
        std::string l = buf;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        const bool take = l.rfind("# take,", 0) == 0, relook = l.rfind("# look,", 0) == 0;
        if (take || relook) {
            size_t p = 7;
            while (p < l.size() && l[p] == ' ') ++p;
            if (take) {   // "# take, VERSION, k=v,k=v,..."
                const size_t q = l.find(',', p);
                Take tk;
                tk.version = l.substr(p, q == std::string::npos ? std::string::npos : q - p);
                takes.push_back(tk);
                p = q == std::string::npos ? l.size() : q + 1;
                while (p < l.size() && l[p] == ' ') ++p;
            }
            std::string t = l.substr(p);
            for (char& c : t)
                if (c == ',') c = '\n';
            look = AvatarDesc{};
            avatar::fromText(t, look);
            continue;
        }
        if (l.empty() || l[0] == '#' || l[0] == 't') continue;   // (comments; the header)
        double v[kFields];
        int n = 0;
        const char* s = l.c_str();
        while (n < kFields) {
            char* end = nullptr;
            v[n] = std::strtod(s, &end);
            if (end == s) break;
            ++n;
            s = end;
            if (*s != ',') break;
            ++s;
        }
        if (n < kFields) continue;
        if (takes.empty()) takes.push_back(Take{});
        Frame fr;
        fr.t = v[0];
        fr.in.headTracked = v[1] != 0;
        fr.in.head.p = {(float)v[2], (float)v[3], (float)v[4]};
        fr.in.head.q = Quat((float)v[5], (float)v[6], (float)v[7], (float)v[8]);
        for (int h = 0; h < 2; ++h) {
            const double* w = v + 9 + 10 * h;
            HandInput& hi = fr.in.hand[h];
            hi.tracked = w[0] != 0;
            hi.grip.p = {(float)w[1], (float)w[2], (float)w[3]};
            hi.grip.q = Quat((float)w[4], (float)w[5], (float)w[6], (float)w[7]);
            hi.trigger = (float)w[8];
            hi.squeeze = (float)w[9];
        }
        fr.in.avatar = look;
        takes.back().frames.push_back(fr);
    }
    std::fclose(f);
    return takes;
}

}  // namespace motionlog
