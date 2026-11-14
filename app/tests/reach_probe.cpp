// Reach probe (owner, 2026-11-11): do the catch zone and the knuckle guard reach where players expect?
//   1. CATCHES in bot matches: every flight of a player's own ball that passes near one of their hands with the trigger
//      held, how close it came to the catch point, caught or not (the bots aim for that point; a check of the reach,
//      not of how people reach).
//   2. THE KNUCKLE BOX around the hand: its corners and the hand's own points in grip space (for the drawing).
//   3. BACKHANDS: the right hand swung outward, the back of the hand leading, across the path of a ball coming at the
//      player, at several hand speeds; for each, whether the game counted a knuckle hit, and whether the ball really
//      touched the box at any moment between two frames (checked in fine steps): a touch the game missed is a ball
//      the box jumped past between frames.
//   4. THE REACH LOG (reach_log.h, the developer log on the headset) run beside 3 and beside catches at set distances
//      from the hand, with the guards on and off: its lines must agree with what the game counted.
// Usage: reach_probe <arena.col> <out.csv prefix>   (tools/run_reach_probe.py builds, runs and draws it)
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>
#include "../src/game.h"
#include "../src/reach_log.h"

static std::vector<CollisionTri> loadArena(const char* path) {
    std::vector<uint8_t> bytes;
    if (FILE* f = std::fopen(path, "rb")) {
        uint8_t buf[4096];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) bytes.insert(bytes.end(), buf, buf + n);
        std::fclose(f);
    }
    return bytes.empty() ? std::vector<CollisionTri>{} : parseCollision(bytes.data(), bytes.size());
}

static PlayerInput standing(const Game& g, int p) {
    PlayerInput in;
    in.headTracked = true;
    const Quat face = Quat::axisAngle({0, 1, 0}, deg2rad(p == 0 ? -90.f : 90.f));
    in.head = {face, g.tun.start[p] + Vec3{0, 1.65f, 0}};
    for (int h = 0; h < 2; ++h) {
        in.hand[h].tracked = true;
        in.hand[h].grip = {face, g.tun.start[p] + Vec3{p == 0 ? 0.2f : -0.2f, 1.0f, (h == 0 ? -0.3f : 0.3f) * (p == 0 ? 1.f : -1.f)}};
    }
    return in;
}

// the ball (a sphere) against the box (centre and orientation, half sizes): touching?
static bool touches(const Pose& box, const Vec3& half, const Vec3& c, float r) {
    const Vec3 l = box.q.conj().rotate(c - box.p);
    const Vec3 q{std::max(-half.x, std::min(half.x, l.x)), std::max(-half.y, std::min(half.y, l.y)), std::max(-half.z, std::min(half.z, l.z))};
    return length(l - q) < r;
}

static Pose lerpPose(const Pose& a, const Pose& b, float u) { return {nlerp(a.q, b.q, u), lerp(a.p, b.p, u)}; }

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: reach_probe arena.col out_prefix\n"); return 2; }
    const std::vector<CollisionTri> arena = loadArena(argv[1]);
    if (arena.empty()) { std::printf("no arena\n"); return 1; }
    const std::string out = argv[2];
    const float dt = 1.f / 90.f;

    // ---- 1. catches in bot matches
    {
        FILE* f = std::fopen((out + "_catches.csv").c_str(), "w");
        std::fprintf(f, "level,player,caught,closest_cm,reach_cm\n");
        int flights = 0, caughtN = 0;
        for (int level = 0; level < kBotLevels; ++level) {
            Game g;
            g.setArena(arena);
            g.tun.botLevel = level;
            g.tun.meetTime = 0;
            g.resetWithBots(3u);
            PlayerInput in[kMaxPlayers];
            // per player: the closest the own ball came to a catch point with that hand's trigger held, this flight
            float closest[2] = {1e9f, 1e9f};
            bool armed[2] = {false, false};
            for (int i = 0; i < (int)(90 * 60 * 6); ++i) {   // six minutes of play a level
                g.step(dt, in);
                for (int p = 0; p < 2; ++p) {
                    const Ball& b = g.pl[p].ball;
                    const bool flying = b.state == BallState::Flying || b.state == BallState::Returning;
                    bool caughtNow = false;
                    for (const GameEvent& e : g.events) caughtNow |= e.type == EventType::Caught && e.player == p;
                    if (flying) {
                        for (int h = 0; h < 2; ++h) {
                            if (g.pl[p].in.hand[h].trigger <= 0.5f) continue;
                            const float d = length(g.catcherPoint(p, h) - b.pos);
                            if (d < 0.9f) { armed[p] = true; closest[p] = std::min(closest[p], d); }
                        }
                    }
                    if ((caughtNow || !flying) && armed[p]) {
                        const float reach = g.tun.catchHalfHeight + g.ballRadius(p);
                        std::fprintf(f, "%d,%d,%d,%.1f,%.1f\n", level, p, caughtNow ? 1 : 0, closest[p] * 100.f, reach * 100.f);
                        ++flights; caughtN += caughtNow;
                        armed[p] = false; closest[p] = 1e9f;
                    }
                }
            }
        }
        std::fclose(f);
        std::printf("catches: %d flights with the trigger held near the ball, %d caught\n", flights, caughtN);
    }

    // ---- 2. the knuckle box and the hand's points, in grip space (the right hand)
    {
        Game g;
        FILE* f = std::fopen((out + "_hand.csv").c_str(), "w");
        std::fprintf(f, "name,x,y,z\n");
        const Vec3 k = g.tun.knuckleOffset, hk = g.tun.knuckleHalf;
        std::fprintf(f, "box_min,%.4f,%.4f,%.4f\nbox_max,%.4f,%.4f,%.4f\n", k.x - hk.x, k.y - hk.y, k.z - hk.z, k.x + hk.x, k.y + hk.y, k.z + hk.z);
        std::fprintf(f, "grip,0,0,0\n");
        std::fprintf(f, "catcher,%.4f,%.4f,%.4f\n", g.tun.catcherOffset.x, g.tun.catcherOffset.y, g.tun.catcherOffset.z);
        std::fprintf(f, "launcher,%.4f,%.4f,%.4f\n", g.tun.launcherOffset.x, g.tun.launcherOffset.y, g.tun.launcherOffset.z);
        std::fprintf(f, "hold,%.4f,%.4f,%.4f\n", g.tun.holdOffset.x, g.tun.holdOffset.y, g.tun.holdOffset.z);
        std::fprintf(f, "fist_back,%.4f,%.4f,%.4f\nfist_front,%.4f,%.4f,%.4f\n", g.tun.fistBack.x, g.tun.fistBack.y, g.tun.fistBack.z,
                     g.tun.fistFront.x, g.tun.fistFront.y, g.tun.fistFront.z);
        std::fprintf(f, "ball_r,%.4f,0,0\n", g.tun.projectileRadius);
        std::fclose(f);
    }

    // ---- 3. backhands
    {
        FILE* f = std::fopen((out + "_backhand.csv").c_str(), "w");
        std::fprintf(f, "speed,offset_ms,height_cm,counted,touched,contact_x,contact_y,contact_z,log_result,log_gap_cm\n");
        int agree = 0, disagree = 0, noLine = 0, guardsOffRight = 0, guardsOffN = 0;
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> U(-1.f, 1.f);
        const float speeds[] = {0.f, 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 8.f};
        for (float speed : speeds) {
            for (int trial = 0; trial < 160 + 20; ++trial) {   // (the last 20: the guards off, for the log's reason)
                const bool guardsOff = trial >= 160;
                Game g;
                g.setArena(arena);
                g.tun.meetTime = 0;
                if (guardsOff) g.tun.knuckleMode = 0;
                g.reset(false);
                PlayerInput in[2] = {standing(g, 0), standing(g, 1)};
                for (int i = 0; i < 330; ++i) { PlayerInput t[2] = {in[0], in[1]}; g.step(dt, t); }   // into the match
                ReachLog log;
                std::vector<std::string> lines;
                // the ball comes straight at player 0's right side, 55 cm out, at chest to waist height
                const float height = 1.05f + 0.25f * U(rng);
                const float side = 0.55f;
                const Vec3 startP = g.tun.start[0] + Vec3{0.25f, 0, 0};   // (the hand's x: in front of the body)
                Ball& b = g.pl[1].ball;
                b.state = BallState::Flying; b.instigator = 1; b.scored = false; b.spin = {};
                b.pos = Vec3{startP.x + 6.f, height, side};
                b.vel = {-12.f, 0.f, 0.f}; b.speedLimit = 12.f;
                const float arrive = 6.f / 12.f;                      // s until it crosses the hand's x
                // the right hand: fist forward, swung outward (+z, the back of the hand leading) through z = side at the
                // moment the ball arrives, give or take up to 60 ms; a still hand (speed 0) waits right there
                const float offset = 0.06f * U(rng);
                const Quat face = Quat::axisAngle({0, 1, 0}, deg2rad(-90.f));
                const float z0 = side - speed * (arrive + offset);
                Pose prev{};
                bool counted = false, touched = false;
                Vec3 contact{};
                for (int i = 0; i < (int)(arrive * 90) + 30; ++i) {
                    const float t = (i + 1) * dt;
                    const float z = speed > 0 ? std::min(side + 0.6f, z0 + speed * t) : side;
                    in[0].hand[1].grip = {face, Vec3{startP.x, height, z}};
                    in[0].hand[1].trigger = 0;
                    const Vec3 ballBefore = b.pos;
                    const Pose handBefore = i ? prev : in[0].hand[1].grip;
                    PlayerInput tmp[2] = {in[0], in[1]};
                    g.step(dt, tmp);
                    for (const GameEvent& e : g.events) counted |= (e.type == EventType::KnuckleBlock && e.player == 0 && e.hand == 1);
                    log.step(g, 0, dt, "probe", lines);
                    // the truth: the ball and the box between the frames, in 24 steps
                    for (int s = 0; s <= 24 && !touched; ++s) {
                        const float u = s / 24.f;
                        const Pose hp = lerpPose(handBefore, in[0].hand[1].grip, u);
                        Vec3 o = g.tun.knuckleOffset;
                        const Pose box{hp.q, hp.transform(o)};
                        const Vec3 bc = lerp(ballBefore, ballBefore + Vec3{-12.f * dt, 0, 0}, u);
                        if (touches(box, g.tun.knuckleHalf, bc, g.tun.projectileRadius)) { touched = true; contact = hp.q.conj().rotate(bc - hp.p); }
                    }
                    prev = in[0].hand[1].grip;
                    if (counted && touched) break;
                    if (b.pos.x < startP.x - 1.f || b.state != BallState::Flying) break;
                }
                // the log's line for the right hand (field 4 the result, field 5 the gap)
                std::string res = "none", gapS = "";
                for (const std::string& s : lines) {
                    std::vector<std::string> c;
                    size_t i = 0;
                    for (size_t j; (j = s.find(',', i)) != std::string::npos; i = j + 1) c.push_back(s.substr(i, j - i));
                    if (c.size() > 4 && c[1] == "knuckle" && c[2] == "right") { res = c[3]; gapS = c[4]; }
                }
                if (guardsOff) {
                    if (touched) { ++guardsOffN; guardsOffRight += res == "guards-off"; }
                } else if (res == "none") {
                    noLine += touched;   // (a ball that never came within 25 cm writes nothing: right)
                } else {
                    const bool ok = counted ? res == "HIT" : (res == "missed" && !touched) || (res == "touched-not-counted" && touched) ||
                                                             (res == "missed" && touched && std::atof(gapS.c_str()) < 1.0f);
                    (ok ? agree : disagree)++;
                    if (!ok) std::printf("  log disagrees: speed %.0f counted %d touched %d log %s gap %s\n", speed, counted, touched, res.c_str(), gapS.c_str());
                }
                if (!guardsOff)
                    std::fprintf(f, "%.0f,%.0f,%.0f,%d,%d,%.3f,%.3f,%.3f,%s,%s\n", speed, offset * 1000.f, height * 100.f, counted ? 1 : 0, touched ? 1 : 0,
                                 contact.x, contact.y, contact.z, res.c_str(), gapS.c_str());
            }
        }
        std::fclose(f);
        std::printf("backhands written\n");
        std::printf("REACH LOG vs the game (backhands): %d lines agree, %d disagree; %d touches with no line; guards off: %d of %d touches logged as guards-off\n",
                    agree, disagree, noLine, guardsOffRight, guardsOffN);
    }

    // ---- 4. the reach log on catches: the player's own ball (blocked back by the other player) passing the right
    // hand's catch point at set distances, the trigger held or not
    {
        int agree = 0, disagree = 0, rows = 0;
        for (int held = 0; held < 2; ++held)
            for (int cm = 0; cm <= 60; cm += 4) {
                Game g;
                g.setArena(arena);
                g.tun.meetTime = 0;
                g.reset(false);
                PlayerInput in[2] = {standing(g, 0), standing(g, 1)};
                for (int i = 0; i < 330; ++i) { PlayerInput t2[2] = {in[0], in[1]}; g.step(dt, t2); }
                const Quat face = Quat::axisAngle({0, 1, 0}, deg2rad(-90.f));
                const Vec3 gp = g.tun.start[0] + Vec3{0.25f, 1.1f, 0.5f};
                in[0].hand[1].grip = {face, gp};
                in[0].hand[1].trigger = held ? 1.f : 0.f;
                const Vec3 cpt = in[0].hand[1].grip.transform(g.tun.catcherOffset);
                Ball& b = g.pl[0].ball;
                b.state = BallState::Flying; b.instigator = 1; b.scored = false; b.spin = {};
                b.pos = Vec3{cpt.x + 6.f, cpt.y, cpt.z + cm / 100.f};
                b.vel = {-12.f, 0.f, 0.f}; b.speedLimit = 12.f;
                ReachLog log;
                std::vector<std::string> lines;
                bool caughtE = false;
                for (int i = 0; i < 90; ++i) {
                    PlayerInput t2[2] = {in[0], in[1]};
                    g.step(dt, t2);
                    for (const GameEvent& e : g.events) caughtE |= e.type == EventType::Caught && e.player == 0 && e.hand == 1;
                    log.step(g, 0, dt, "probe", lines);
                    if (caughtE || b.pos.x < cpt.x - 1.5f) break;
                }
                std::string res = "none";
                for (const std::string& s : lines) {
                    const size_t k = s.find(",catch,right,");
                    if (k != std::string::npos) { res = s.substr(k + 13); res = res.substr(0, res.find(',')); }
                }
                const float reachCm = (g.tun.catchHalfHeight + g.ballRadius(0)) * 100.f;
                const bool nearby = cm < reachCm + 25.f;
                const bool ok = caughtE ? res == "CAUGHT" : !nearby ? res == "none" :
                                held ? (res == "missed" || res == "in-reach-not-caught") : res == "trigger-not-held";
                (ok ? agree : disagree)++;
                rows += res != "none";
                if (!ok || cm % 12 == 0)
                    std::printf("  catch %2d cm from the catch point, trigger %s: caught %d, log %s%s\n", cm, held ? "held" : "off ",
                                (int)caughtE, res.c_str(), ok ? "" : "   <- disagrees");
            }
        std::printf("REACH LOG vs the game (catches): %d agree, %d disagree (%d lines)\n", agree, disagree, rows);
    }
    return 0;
}
