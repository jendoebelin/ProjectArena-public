#pragma once
// SMOOTH VIEWS WHILE WAITING (0.3.26). Someone in a room's waiting area, or a spectator, gets the host's snapshots 30
// times a second, not every frame, and their game drew each one as it came and held it until the next: the players
// moved in steps, and the ping's swings showed (the owner, 2026-11-23, watching a FIREBALL BLITZ room as its fifth
// person). This draws the room about one update behind instead: each frame the players' heads and hands and the balls
// are blended from what was on screen when the latest snapshot came to that snapshot, over the snapshots' usual
// spacing. A point that moved further than kSnap between two snapshots (a player put on another spot, a ball back to
// its spawner) goes straight there, and after kGap without one, everything does. Only what is drawn changes: the
// viewer's game isn't simulated, the network is as before, and players in the arena don't use it (their BALL
// LOOK-AHEAD). Events (hits, knockouts, points) still play as they arrive: at most one update before the picture.
#include "game.h"

struct ViewSmooth {
    static constexpr float kSnap = 1.5f;     // metres between two snapshots: a jump, not movement
    static constexpr double kGap = 0.25;     // seconds without a snapshot: straight to the next one
    struct Pts {
        Pose head, hand[2];
        Vec3 ball;
        bool ballOn = false;
        Vec3 fire[Game::kMaxFire];
        bool fireOn[Game::kMaxFire] = {};
    };
    Pts from[kMaxPlayers], to[kMaxPlayers];
    bool have = false;
    int np = 0;
    double at = 0;          // when the latest snapshot came (our clock)
    float dur = 1.f / 30;   // the snapshots' spacing, smoothed: how long each blend takes

    void reset() { have = false; }

    static Pts take(const Game& g, int p) {
        Pts v;
        const PlayerState& s = g.pl[p];
        v.head = s.in.head;
        v.hand[0] = s.in.hand[0].grip;
        v.hand[1] = s.in.hand[1].grip;
        v.ball = s.ball.pos;
        v.ballOn = s.ball.state != BallState::Dead;
        for (int k = 0; k < Game::kMaxFire; ++k) {
            v.fire[k] = g.fire[p][k].pos;
            v.fireOn[k] = g.fire[p][k].state == BallState::Flying;
        }
        return v;
    }
    static void put(Game& g, int p, const Pts& v) {
        PlayerState& s = g.pl[p];
        s.in.head = v.head;
        s.in.hand[0].grip = v.hand[0];
        s.in.hand[1].grip = v.hand[1];
        if (s.ball.state != BallState::Dead) s.ball.pos = v.ball;
        for (int k = 0; k < Game::kMaxFire; ++k)
            if (g.fire[p][k].state == BallState::Flying) g.fire[p][k].pos = v.fire[k];
    }
    static Pose mix(const Pose& a, const Pose& b, float t) { return {nlerp(a.q, b.q, t), lerp(a.p, b.p, t)}; }
    float alpha(double now) const {
        const double t = (now - at) / dur;
        return t <= 0 ? 0.f : t >= 1 ? 1.f : (float)t;
    }
    Pts blend(int p, float t) const {
        Pts v = to[p];
        v.head = mix(from[p].head, to[p].head, t);
        for (int h = 0; h < 2; ++h) v.hand[h] = mix(from[p].hand[h], to[p].hand[h], t);
        v.ball = lerp(from[p].ball, to[p].ball, t);
        for (int k = 0; k < Game::kMaxFire; ++k) v.fire[k] = lerp(from[p].fire[k], to[p].fire[k], t);
        return v;
    }
    static bool within(const Vec3& a, const Vec3& b) { return length(b - a) <= kSnap; }

    // a new snapshot has just been read into g: blend from what is on screen now to it
    void arrived(const Game& g, double now) {
        const double since = now - at;
        if (!have || g.np != np || since > kGap) {
            np = g.np;
            for (int p = 0; p < np; ++p) from[p] = to[p] = take(g, p);
            have = true;
            at = now;
            dur = 1.f / 30;
            return;
        }
        const float a = alpha(now);   // (where the picture is now: before the spacing changes, or it would jump)
        // (the spacing, smoothed: 30 a second from 0.3.26 hosts, 22 to 24 from older ones; a late one moves it a little)
        const float s = (float)since < 1.f / 90 ? 1.f / 90 : (float)since > 0.1f ? 0.1f : (float)since;
        dur += (s - dur) * 0.25f;
        for (int p = 0; p < np; ++p) {
            const Pts shown = blend(p, a), next = take(g, p);
            Pts f = shown;
            if (!within(f.head.p, next.head.p)) f.head = next.head;
            for (int h = 0; h < 2; ++h) if (!within(f.hand[h].p, next.hand[h].p)) f.hand[h] = next.hand[h];
            if (!(to[p].ballOn && next.ballOn && within(f.ball, next.ball))) f.ball = next.ball;
            for (int k = 0; k < Game::kMaxFire; ++k)
                if (!(to[p].fireOn[k] && next.fireOn[k] && within(f.fire[k], next.fire[k]))) f.fire[k] = next.fire[k];
            f.ballOn = next.ballOn;
            for (int k = 0; k < Game::kMaxFire; ++k) f.fireOn[k] = next.fireOn[k];
            from[p] = f;
            to[p] = next;
        }
        at = now;
    }
    // each frame: what to draw
    void apply(Game& g, double now) const {
        if (!have || g.np != np) return;
        const float a = alpha(now);
        for (int p = 0; p < np; ++p) put(g, p, blend(p, a));
    }
};
