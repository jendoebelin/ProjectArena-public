// RECORD MY MATCHES: see record.h for what is kept and the file's layout.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS   // (fopen, strncpy: plain C, on every platform)
#endif
#include "record.h"
#include <algorithm>
#include <cstring>
#include <vector>
#ifdef _WIN32
#include <io.h>
#else
#include <dirent.h>
#endif

namespace rec {

namespace {
template <typename T> void put(std::FILE* f, T v) { std::fwrite(&v, sizeof v, 1, f); }
void putVec(std::FILE* f, const Vec3& v) { put(f, v.x); put(f, v.y); put(f, v.z); }
void putPose(std::FILE* f, const Pose& p) { putVec(f, p.p); put(f, p.q.x); put(f, p.q.y); put(f, p.q.z); put(f, p.q.w); }
}  // namespace

bool Recorder::begin(const std::string& path, const Game& g, const Meta& m) {
    close();
    fp_ = std::fopen(path.c_str(), "wb");
    if (!fp_) return false;
    path_ = path;
    local_ = m.local & 1;
    keepOpp_ = g.pl[1 - local_].isBot;   // (a person opponent's motion is never kept)
    seconds_ = 0;
    std::fwrite("PAREC1", 1, 6, fp_);
    put<uint8_t>(fp_, (uint8_t)local_);
    put<uint8_t>(fp_, keepOpp_ ? 1 : 0);
    const int lv = g.pl[1 - local_].botLevel >= 0 ? g.pl[1 - local_].botLevel : g.tun.botLevel;
    put<uint8_t>(fp_, (uint8_t)lv);
    put<uint8_t>(fp_, g.tun.botAdapt ? 1 : 0);
    put<float>(fp_, g.tun.botMatch);
    put<uint8_t>(fp_, g.tun.advanced ? 1 : 0);
    put<uint8_t>(fp_, g.tun.shields ? 1 : 0);
    put<int8_t>(fp_, (int8_t)g.tun.knuckleMode);
    put<uint8_t>(fp_, (uint8_t)std::max(0, std::min(255, m.lesson)));
    put<float>(fp_, g.tun.basicMatchTime);
    put<float>(fp_, g.tun.advancedRoundTime);
    char ver[16] = {};
    std::strncpy(ver, m.version ? m.version : "", sizeof ver - 1);
    std::fwrite(ver, 1, sizeof ver, fp_);
    return true;
}

void Recorder::frame(const Game& g, float dt) {
    if (!fp_) return;
    put<uint8_t>(fp_, 'F');
    put<float>(fp_, g.now);
    put<float>(fp_, dt);
    put<uint8_t>(fp_, (uint8_t)g.match);
    put<uint8_t>(fp_, (uint8_t)std::min(255, std::max(0, g.pl[0].score)));
    put<uint8_t>(fp_, (uint8_t)std::min(255, std::max(0, g.pl[1].score)));
    for (int p = 0; p < 2; ++p) {
        const bool keep = p == local_ || keepOpp_;
        put<uint8_t>(fp_, keep ? 1 : 0);
        if (!keep) continue;
        const PlayerInput& in = g.pl[p].in;
        putPose(fp_, in.head);
        for (const HandInput& h : in.hand) {
            putPose(fp_, h.grip);
            put<float>(fp_, h.trigger);
            put<float>(fp_, h.squeeze);
            put<uint8_t>(fp_, h.tracked ? 1 : 0);
        }
    }
    for (int p = 0; p < 2; ++p) {
        const Ball& b = g.pl[p].ball;
        put<uint8_t>(fp_, (uint8_t)b.state);
        put<int8_t>(fp_, (int8_t)b.instigator);
        putVec(fp_, b.pos);
        putVec(fp_, b.vel);
        putVec(fp_, b.spin);
        put<float>(fp_, b.scale);
        put<uint8_t>(fp_, b.charged ? 1 : 0);
        put<int8_t>(fp_, (int8_t)b.heldHand);
    }
    for (const GameEvent& e : g.events) {
        put<uint8_t>(fp_, 'E');
        put<uint8_t>(fp_, (uint8_t)e.type);
        put<int8_t>(fp_, (int8_t)e.player);
        put<int8_t>(fp_, (int8_t)e.hand);
        put<int8_t>(fp_, (int8_t)e.ball);
        putVec(fp_, e.pos);
        put<float>(fp_, e.value);
    }
    if ((seconds_ += dt) > kMaxSeconds) end(g);
}

void Recorder::end(const Game& g) {
    if (!fp_) return;
    put<uint8_t>(fp_, 'X');
    put<float>(fp_, g.now);
    close();
}

void Recorder::close() {
    if (fp_) std::fclose(fp_);
    fp_ = nullptr;
}

bool Replay::load(const uint8_t* d, size_t n) {
    frames.clear();
    size_t at = 0;
    bool ok = true;
    auto take = [&](void* out, size_t k) { if (at + k > n) { ok = false; std::memset(out, 0, k); return; } std::memcpy(out, d + at, k); at += k; };
    auto u8 = [&]() { uint8_t v; take(&v, 1); return v; };
    auto i8 = [&]() { int8_t v; take(&v, 1); return v; };
    auto f32 = [&]() { float v; take(&v, 4); return v; };
    auto vec = [&]() { Vec3 v; v.x = f32(); v.y = f32(); v.z = f32(); return v; };
    auto pose = [&]() { Pose p; p.p = vec(); p.q.x = f32(); p.q.y = f32(); p.q.z = f32(); p.q.w = f32(); return p; };
    if (n < 6 || std::memcmp(d, "PAREC1", 6) != 0) return false;
    at = 6;
    local = u8() & 1;
    u8(); u8(); u8(); f32(); u8(); u8(); i8();
    lesson = u8();
    f32(); f32();
    char ver[16];
    take(ver, 16);
    while (ok && at < n) {
        const uint8_t tag = u8();
        if (tag == 'F') {
            Frame f;
            f.t = f32(); f32();
            f.match = (MatchState)u8(); u8(); u8();
            for (int p = 0; p < 2; ++p) {
                f.kept[p] = u8() != 0;
                if (!f.kept[p]) continue;
                PlayerInput& in = f.in[p];
                in.headTracked = true;
                in.head = pose();
                for (HandInput& hi : in.hand) { hi.grip = pose(); hi.trigger = f32(); hi.squeeze = f32(); hi.tracked = u8() != 0; }
            }
            for (int p = 0; p < 2; ++p) {
                BallAt& b = f.ball[p];
                b.state = (BallState)u8(); b.instigator = i8();
                b.pos = vec(); b.vel = vec(); b.spin = vec();
                b.scale = f32(); b.charged = u8() != 0; b.heldHand = i8();
            }
            if (ok) frames.push_back(f);
        } else if (tag == 'E') {
            GameEvent e{};
            e.type = (EventType)u8(); e.player = i8(); e.hand = i8(); e.ball = i8(); e.pos = vec(); e.value = f32();
            if (ok && !frames.empty()) frames.back().events.push_back(e);
        } else {
            break;   // 'X' (the end), or something this build doesn't know
        }
    }
    return !frames.empty();
}

void prune(const std::string& dir, int keep) {
    std::vector<std::string> names;
#ifdef _WIN32
    _finddata_t fd;
    intptr_t h = _findfirst((dir + "/match_*.par").c_str(), &fd);
    if (h != -1) {
        do names.push_back(fd.name); while (_findnext(h, &fd) == 0);
        _findclose(h);
    }
#else
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d))
            if (std::strncmp(e->d_name, "match_", 6) == 0 && std::strstr(e->d_name, ".par")) names.push_back(e->d_name);
        closedir(d);
    }
#endif
    std::sort(names.begin(), names.end());
    for (int i = 0; i + keep < (int)names.size(); ++i) std::remove((dir + "/" + names[i]).c_str());
}

}  // namespace rec
