// REACH LOG (developer mode only; owner, 2026-11-11: a backhand with the knuckle guards, out to the side, that has
// never hit; catches that feel just out of reach). Each time a ball passes within 25 cm of one of the local player's
// knuckle boxes (a ball someone else last touched) or of a hand's catch reach (their own ball), one CSV line: how
// close it came, where around the hand, how the hand and the ball were moving, and what happened (a hit or a catch,
// or which rule let it pass). Grip space with the left hand mirrored, so both hands read as a right hand: x to the
// back of the hand, y up, z to the wrist (-z: out past the knuckles). On a guest the host decides hits and catches;
// the positions are this headset's view of them. main.cpp writes the lines to <files>/reach_log.csv;
// app/tests/reach_probe.cpp checks the lines against simulated backhands and catches.
#pragma once
#include <cstdio>
#include <string>
#include <vector>
#include "game.h"

struct ReachPass {
    bool on = false;
    float gap = 1e9f;          // closest: the ball's surface to the box (knuckles), or past the catch reach (catches), m
    float t = 0;               // the game clock then
    Vec3 at{}, handV{}, ballV{};
    float trig = 0, heldFor = -1;
    bool guard = false, tracked = false, returning = false;
};

struct ReachLog {
    static constexpr const char* kHeader =
        "t,kind,hand,result,gap_cm,at_x_cm,at_y_cm,at_z_cm,hand_mps,hand_x,hand_y,hand_z,"
        "ball_rel_mps,ball_x,ball_y,ball_z,trigger,held_ms,guard,tracked,returning,role,rules";
    bool have = false;
    Pose prevGrip[2];
    Vec3 prevBall[kMaxPlayers];
    int prevState[kMaxPlayers] = {};
    int prevInst[kMaxPlayers] = {};
    float heldSince[2] = {-1, -1};
    ReachPass knuck[kMaxPlayers][2], cat[2];

    void reset() { *this = ReachLog{}; }

    static Vec3 mirrorH(Vec3 v, int h) { if (h == 0) v.x = -v.x; return v; }
    static float boxGap(const Pose& box, const Vec3& half, const Vec3& c, float r) {
        const Vec3 l = box.q.conj().rotate(c - box.p);
        const Vec3 q{clampf(l.x, -half.x, half.x), clampf(l.y, -half.y, half.y), clampf(l.z, -half.z, half.z)};
        return length(l - q) - r;
    }

    static std::string line(const Game& g, const char* kind, int h, const ReachPass& p, const char* result, const char* role) {
        char s[512];
        std::snprintf(s, sizeof s, "%.3f,%s,%s,%s,%.1f,%.1f,%.1f,%.1f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.0f,%d,%d,%d,%s,%s",
                      p.t, kind, h ? "right" : "left", result, p.gap * 100.f, p.at.x * 100.f, p.at.y * 100.f, p.at.z * 100.f,
                      length(p.handV), p.handV.x, p.handV.y, p.handV.z, length(p.ballV), p.ballV.x, p.ballV.y, p.ballV.z, p.trig,
                      p.heldFor * 1000.f, (int)p.guard, (int)p.tracked, (int)p.returning, role,
                      g.tun.advanced ? (g.tun.knuckleGuards() ? "advanced+guards" : "advanced")
                                     : (g.tun.knuckleGuards() ? "basic" : "basic-noguards"));
        return s;
    }

    // after each game step (g.events: that step's events); L the local player. New lines go to out
    void step(const Game& g, int L, float dt, const char* role, std::vector<std::string>& out) {
        if (L < 0 || L >= g.np || dt <= 0.f) { have = false; return; }
        const PlayerState& me = g.pl[L];
        bool kHit[2] = {false, false}, caught[2] = {false, false}, shield = false, hitMe = false;
        for (const GameEvent& e : g.events) {
            if (e.player != L) continue;
            if (e.type == EventType::KnuckleBlock && e.hand >= 0 && e.hand < 2) kHit[e.hand] = true;
            if (e.type == EventType::Caught && e.hand >= 0 && e.hand < 2) caught[e.hand] = true;
            if (e.type == EventType::ShieldBlock) shield = true;
            if (e.type == EventType::HitPlayer) hitMe = true;
        }
        for (int h = 0; h < 2; ++h) {   // how long each trigger has been held (catches)
            if (me.in.hand[h].trigger > 0.5f) { if (heldSince[h] < 0) heldSince[h] = g.now; }
            else heldSince[h] = -1;
        }
        if (!have) {
            for (int h = 0; h < 2; ++h) prevGrip[h] = me.in.hand[h].grip;
            for (int q = 0; q < g.np; ++q) { prevBall[q] = g.pl[q].ball.pos; prevState[q] = (int)g.pl[q].ball.state; prevInst[q] = g.pl[q].ball.instigator; }
            have = true;
            return;
        }
        const int S = 8;   // the hand and the ball between the last frame and this one, in steps
        for (int h = 0; h < 2; ++h) {
            const HandInput& hi = me.in.hand[h];
            const Pose g0 = prevGrip[h], g1 = hi.grip;
            Vec3 ko = g.tun.knuckleOffset;
            if (h == 0) ko.x = -ko.x;
            const Vec3 handV = g1.q.conj().rotate(g1.transform(ko) - g0.transform(ko)) / dt;
            for (int q = 0; q < g.np; ++q) {
                const Ball& b = g.pl[q].ball;
                const bool moving = b.state == BallState::Flying || b.state == BallState::Returning;
                const bool wasMoving = prevState[q] == (int)BallState::Flying || prevState[q] == (int)BallState::Returning;
                const Vec3 ballV = g1.q.conj().rotate((b.pos - prevBall[q]) / dt) - handV;
                // ---- knuckles: a ball someone else last touched (yours too, after they blocked it). A hit makes the
                // ball yours (instigator): the ball that became yours this frame is the one hit
                ReachPass& kp = knuck[q][h];
                const bool hitThis = kHit[h] && b.instigator == L && prevInst[q] != L;
                const bool threat = moving && wasMoving && b.instigator >= 0 && (b.instigator != L || hitThis);
                float gap = 1e9f;
                Vec3 at{};
                if (threat) {
                    for (int s = 0; s <= S; ++s) {
                        const float u = s / (float)S;
                        const Pose hp{nlerp(g0.q, g1.q, u), lerp(g0.p, g1.p, u)};
                        const Vec3 bc = lerp(prevBall[q], b.pos, u);
                        const float d = boxGap({hp.q, hp.transform(ko)}, g.tun.knuckleHalf, bc, g.ballRadius(q));
                        if (d < gap) { gap = d; at = hp.q.conj().rotate(bc - hp.p); }
                    }
                }
                if (threat && gap < 0.25f && gap < kp.gap) {
                    kp.on = true; kp.gap = gap; kp.t = g.now;
                    kp.at = mirrorH(at, h); kp.handV = mirrorH(handV, h); kp.ballV = mirrorH(ballV, h);
                    kp.trig = hi.trigger; kp.guard = me.hands[h].knuckleUp || (g.tun.knuckleGuards() && hi.tracked);
                    kp.tracked = hi.tracked; kp.returning = b.state == BallState::Returning;
                }
                if (hitThis && !kp.on) { kp.on = true; kp.gap = std::min(gap, 0.f); kp.t = g.now; kp.tracked = hi.tracked; kp.guard = true; }
                if (kp.on && (hitThis || !threat || gap > kp.gap + 0.15f)) {
                    const char* why = hitThis ? "HIT" : kHit[1 - h] ? "other-hand-hit" : shield ? "shield-took-it" : !kp.tracked ? "hand-not-tracked" :
                                      !kp.guard ? "guards-off" : kp.returning ? "returning-ball" : hitMe ? "body-hit-first" :
                                      kp.gap <= 0.f ? "touched-not-counted" : "missed";
                    out.push_back(line(g, "knuckle", h, kp, why, role));
                    kp = ReachPass{};
                }
                // ---- catches: your own ball, whoever touched it last
                if (q != L) continue;
                ReachPass& cp = cat[h];
                const float reach = g.tun.catchHalfHeight + g.ballRadius(L);
                const bool flight = (moving || caught[h]) && wasMoving;
                float cg = 1e9f;
                Vec3 ca{};
                if (flight) {
                    for (int s = 0; s <= S; ++s) {
                        const float u = s / (float)S;
                        const Pose hp{nlerp(g0.q, g1.q, u), lerp(g0.p, g1.p, u)};
                        const Vec3 bc = lerp(prevBall[q], b.pos, u);
                        const float d = length(hp.transform(g.tun.catcherOffset) - bc) - reach;
                        if (d < cg) { cg = d; ca = hp.q.conj().rotate(bc - hp.p); }
                    }
                }
                if (flight && cg < 0.25f && cg < cp.gap) {
                    cp.on = true; cp.gap = cg; cp.t = g.now;
                    cp.at = mirrorH(ca, h); cp.handV = mirrorH(handV, h); cp.ballV = mirrorH(ballV, h);
                    cp.trig = hi.trigger; cp.heldFor = heldSince[h] >= 0 ? g.now - heldSince[h] : -1.f;
                    cp.tracked = hi.tracked; cp.returning = b.state == BallState::Returning;
                }
                if (cp.on && (caught[h] || !flight || cg > cp.gap + 0.15f)) {
                    const char* why = caught[h] ? "CAUGHT" : !cp.tracked ? "hand-not-tracked" : cp.trig <= 0.5f ? "trigger-not-held" :
                                      cp.gap <= 0.f ? "in-reach-not-caught" : "missed";
                    out.push_back(line(g, "catch", h, cp, why, role));
                    cp = ReachPass{};
                }
            }
            prevGrip[h] = g1;
        }
        for (int q = 0; q < g.np; ++q) { prevBall[q] = g.pl[q].ball.pos; prevState[q] = (int)g.pl[q].ball.state; prevInst[q] = g.pl[q].ball.instigator; }
    }
};
