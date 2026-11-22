// Long-distance play, measured (owner, 2026-11-04: a match against a player on the other side of the world; as the player who wasn't
// hosting, "I saw the ball leave my hand late and his ball arrive later than it really did").
// Two real Net + Game copies, a host (slot 0) and a player (slot 1), through a private copy of the community relay
// whose test mode delays every forwarded packet (tools/run_lag_test.py: --lag / --jitter). Both throw the same scripted
// throws (reach for the ball on its tee, swing at 9 m/s at the other's head, let go) from what their own game shows,
// and the test measures, for each side:
//   grab:     the trigger pulled at the ball -> my game shows it in my hand
//   leave:    the trigger let go             -> my game shows my ball flying
//   incoming: how far behind (ms) my game shows the other's ball compared to where the host has it at that moment
//   block:    a block made against what my game shows lands where the host has the ball this much earlier or later
// Usage: lag_test <seconds> [server host:port] [look-ahead 1/0]   (prints RESULT lines for the runner)
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <thread>
#include <vector>
#include "../src/avatar.h"
#include "../src/log.h"
#include "../src/net.h"

bool netAcquireMulticastLock() { return false; }

static double clockNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static const float kHz = 72.f;

static std::vector<CollisionTri> loadArena() {
    const char* paths[] = {"app/assets_open/arena.col", "../app/assets_open/arena.col", "../../app/assets_open/arena.col"};
    for (const char* path : paths) {
        FILE* f = std::fopen(path, "rb");
        if (!f) continue;
        std::vector<uint8_t> bytes;
        uint8_t buf[4096];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) bytes.insert(bytes.end(), buf, buf + n);
        std::fclose(f);
        return parseCollision(bytes.data(), bytes.size());
    }
    return {};
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
// (FOUR CORNERS) at corner p, facing the centre, the hands out in front
static PlayerInput standing4(const Game& g, int p) {
    PlayerInput in;
    in.headTracked = true;
    const Vec3 f = g.facing(p);
    const Quat face = Quat::axisAngle({0, 1, 0}, std::atan2(-f.x, -f.z));
    in.head = {face, g.tun.start[p] + Vec3{0, 1.65f, 0}};
    for (int h = 0; h < 2; ++h) {
        in.hand[h].tracked = true;
        in.hand[h].grip = {face, g.tun.start[p] + Vec3{0, 1.0f, 0} + f * 0.2f + face.rotate({h == 0 ? -0.3f : 0.3f, 0, 0})};
    }
    return in;
}

// one player's scripted throws, judged from what that player's own game shows
struct Thrower {
    enum { REST, REACH, SWING, RELEASED, HOLD } st = REST;
    double t0 = 0, nextTry = 0;
    int frames = 0;
    Vec3 dir;
    PlayerInput in;
    std::vector<double> grab, leave;
    int missed = 0;
    long blockFrames = 0;
    // catch mode (tools/run_catch_test.py): throws go past the other player, nobody blocks, and each of my returning
    // balls is snatched where my own game shows it once it's within snatchAt of my head (the hand onto it, the trigger
    // pulled), as a player grabs at a ball reaching them
    bool catching = false;
    float snatchAt = 0.9f;
    // shield mode (tools/run_shield_test.py; a player's report, 11-11: "a shield held low didn't deflect"): the player
    // holds their charged ball low, the shield out to the side, and swings it into the other's ball where their own
    // game shows it, `react` seconds before it arrives (a late block); the other throws low, at the belly
    bool shielding = false, throwLow = false;
    bool slapping = false;   // (PA_GUARD=knuckle: no ball held; the right fist's knuckle guard swung in late instead)
    float react = 0.15f;
    int incoming = 0;
    bool inIncoming = false;
    int returns = 0;
    bool inReturn = false;
    int target = -1;   // (FOUR CORNERS: the corner I throw at, the next one round; two players: the other end)
    uint8_t firesAt = 0;   // (FIREBALL BLITZ: my fireballs already flying when I let go)
    void step(const Game& v, int p, double t) {
        const Ball& b = v.pl[p].ball;
        const bool playing = v.match == MatchState::InProgress;
        const bool four = v.np == 4 && target >= 0;
        PlayerInput rest = four ? standing4(v, p) : standing(v, p);
        rest.buttonA = !playing;   // READY when a match waits for it
        // the left hand blocks: the other's ball coming at me, as my game draws it, met with the knuckle guard where it
        // will cross 0.5 m in front of me
        bool blocking = false;
        HandInput guard = rest.hand[0];
        const bool oct = v.octagon();   // (FIREBALL BLITZ: every throw a fireball, v.fire)
        if (four) {   // (four players: whichever ball comes at me first, across the plane 0.5 m in front of me)
            const Vec3 f = v.facing(p), plane = v.tun.start[p] + f * 0.5f;
            float soonest = 0.6f;
            for (int i = 0; i < (oct ? v.np * Game::kMaxFire : v.np); ++i) {
                const Ball& o = oct ? v.fire[i / Game::kMaxFire][i % Game::kMaxFire] : v.pl[i].ball;
                const float in = dot(o.vel, f);
                if ((oct ? o.instigator == p : i == p) || !(o.state == BallState::Flying || o.state == BallState::Returning) || in > -1.f) continue;
                const float tt = dot(plane - o.pos, f) / in;
                const Vec3 meet = o.pos + o.vel * tt, side = meet - v.tun.start[p];
                if (tt <= 0 || tt >= soonest || length(Vec3{side.x, 0, side.z} - f * dot(side, f)) > 1.0f) continue;
                soonest = tt;
                guard.grip.p = meet - (v.knucklePoint(p, 0) - v.pl[p].in.hand[0].grip.p);
                blocking = true;
            }
        } else {
            const Ball& o = v.pl[1 - p].ball;
            const float myX = v.tun.start[p].x, toward = myX < 0 ? -1.f : 1.f;   // (the way a ball comes at me)
            const float planeX = myX - toward * 0.5f;
            if ((o.state == BallState::Flying || o.state == BallState::Returning) && o.vel.x * toward > 1.f) {
                const float tt = (planeX - o.pos.x) / o.vel.x;
                if (tt > 0 && tt < 0.6f) {
                    const Vec3 meet = o.pos + o.vel * tt;
                    guard.grip.p = meet - (v.knucklePoint(p, 0) - v.pl[p].in.hand[0].grip.p);
                    blocking = true;
                }
            }
        }
        if (catching) { blocking = false; guard = rest.hand[0]; }
        if (slapping) {   // the right fist out to the side, swung into the other's ball, late, as my game shows it
            in = rest;
            const float myX = v.tun.start[p].x, toward = myX < 0 ? -1.f : 1.f;
            const Ball& o = v.pl[1 - p].ball;
            const Vec3 kOff = v.knucklePoint(p, 1) - v.pl[p].in.hand[1].grip.p;
            Vec3 at = v.tun.start[p] + Vec3{-toward * 0.5f, 1.0f, 0.55f};
            bool coming = false;
            if (o.state == BallState::Flying && o.instigator == 1 - p && o.vel.x * toward > 1.f) {
                const float tt = (myX - toward * 0.5f - o.pos.x) / o.vel.x;
                if (tt > 0 && tt < 1.5f) coming = true;
                if (tt > 0 && tt < react) at = o.pos + o.vel * tt;
            }
            if (coming && !inIncoming) { inIncoming = true; ++incoming; }
            if (!coming) inIncoming = false;
            in.hand[1].grip.p = at - kOff;
            in.hand[0] = rest.hand[0];
            return;
        }
        if (blocking) ++blockFrames;
        switch (st) {
        case REST:
            in = rest;
            if (playing && b.state == BallState::Idle && t >= nextTry) { st = REACH; t0 = t; }
            break;
        case REACH:   // the hand on the ball, trigger pulled, until my game says I hold it
            in.hand[1].grip.p = b.pos - in.hand[1].grip.q.rotate(v.tun.launcherOffset);
            in.hand[1].trigger = 1;
            if (b.state == BallState::Held && b.heldHand == 1) {
                grab.push_back(t - t0);
                st = SWING; frames = 0;
                int at = four ? target : 1 - p;
                if (oct)   // (FIREBALL BLITZ: the next one round still in)
                    for (int i = 1; i < v.np; ++i) if (v.alive((p + i) % v.np)) { at = (p + i) % v.np; break; }
                dir = normalize(v.tun.start[at] + Vec3{0, throwLow ? 1.0f : 1.6f, catching ? 1.3f : 0.f} - in.hand[1].grip.p);
                if (shielding) st = HOLD;   // (keep it: the shield is up while a charged ball is held)
            } else if (t - t0 > 4.0 || !playing) { st = REST; ++missed; nextTry = t + 1.0; }
            break;
        case HOLD: {   // the charged ball held low; the shield swung into the other's ball, late
            const float myX = v.tun.start[p].x, toward = myX < 0 ? -1.f : 1.f;
            const Pose sl = v.shieldLocal(1);
            const Quat gq = fromTo({0, 1, 0}, {-toward, 0, 0}) * sl.q.conj();   // the plate facing the ball
            Vec3 at = v.tun.start[p] + Vec3{-toward * 0.45f, 0.95f, 0.55f};    // low, out to the side
            const Ball& o = v.pl[1 - p].ball;
            bool coming = false;
            if (o.state == BallState::Flying && o.instigator == 1 - p && o.vel.x * toward > 1.f) {
                const float tt = (myX - toward * 0.45f - o.pos.x) / o.vel.x;
                if (tt > 0 && tt < 1.5f) coming = true;
                if (tt > 0 && tt < react) at = o.pos + o.vel * tt;   // the plate's middle onto the ball's path
            }
            if (coming && !inIncoming) { inIncoming = true; ++incoming; }
            if (!coming) inIncoming = false;
            in.hand[1].grip = {gq, at - gq.rotate(sl.p)};
            in.hand[1].trigger = 1;
            if (b.state != BallState::Held) { st = REST; nextTry = t + 0.5; }
            else if (!b.charged && !coming) { st = SWING; frames = 0; }   // (blocked: throw it to charge it again)
            break;
        }
        case SWING:   // 12 steps speeding up from 3 to 9 m/s toward the other's head (about 1 m, as an arm reaches;
                      // 2026-11-11: it was 1.9 m at a steady 9 m/s, a reach no arm has, and its shield swept the
                      // court), then let go
            in.hand[1].grip.p += dir * ((3.f + 6.f * frames / 11.f) / kHz);
            if (++frames >= 12) {
                in.hand[1].trigger = 0; st = RELEASED; t0 = t;
                firesAt = 0;
                for (int k = 0; k < Game::kMaxFire; ++k) firesAt |= v.fire[p][k].state == BallState::Flying ? (uint8_t)(1u << k) : 0;
            }
            break;
        case RELEASED: {   // my game shows my ball gone from my hand (FIREBALL BLITZ: a fireball of mine that wasn't there)
            bool newFire = false;
            for (int k = 0; k < Game::kMaxFire && oct; ++k) newFire |= v.fire[p][k].state == BallState::Flying && !((firesAt >> k) & 1);
            if (b.state == BallState::Flying || b.state == BallState::Returning || newFire) {
                leave.push_back(t - t0);
                st = REST; nextTry = t + 1.5;
            } else if (t - t0 > 3.0) { ++missed; st = REST; nextTry = t + 1.0; }
            else { in.hand[1].grip = rest.hand[1].grip; in.hand[1].trigger = 0; }
            break;
        }
        }
        if (catching && st == REST) {
            const bool back = b.state == BallState::Returning && b.instigator == p;
            if (!back) inReturn = false;
            else {
                const float d = length(b.pos - v.pl[p].in.head.p);
                if (!inReturn && d < snatchAt) { inReturn = true; ++returns; }
                if (inReturn) {
                    in.hand[1].grip.p = b.pos - in.hand[1].grip.q.rotate(v.tun.catcherOffset);
                    in.hand[1].trigger = 1;
                }
            }
        }
        in.hand[0] = guard;
    }
};

struct Node {
    Net net;
    Game game;
    Thrower th;
    double lastFrame = 0;
    explicit Node(const char* name, const std::vector<CollisionTri>& arena) {
        net.init();
        net.setAppVersion("0.3.15");
        net.setName(name);
        game.setArena(arena);
        game.reset(true);
    }
};

static double median(std::vector<double> v) {
    if (v.empty()) return -1;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
static double pct(std::vector<double> v, double q) {
    if (v.empty()) return -1;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, (size_t)(q * v.size()))];
}

static bool flying(const Ball& b) { return b.state == BallState::Flying || b.state == BallState::Returning; }

// FOUR CORNERS (tools/run_lag_test.py --four): the host and three players, all far from it (the relay delays everyone),
// in a free-for-all; each throws at the next corner round and blocks what comes at them, from what their own game
// shows. Measured as the two-player run is, from the first player's game (FAR ONE); grab and let-go times from all three.
// FIREBALL BLITZ (--blitz): the same in the octagon, at the next one round still in; the fireballs measured, by slot
static int main4(double secs, const char* server, bool ahead, const std::vector<CollisionTri>& arena, bool blitz) {
    Node H("HOST", arena), A("FAR ONE", arena), B("FAR TWO", arena), D("FAR THREE", arena);
    Node* ns[4] = {&H, &A, &B, &D};
    for (int i = 1; i < 4; ++i) ns[i]->net.setLookAhead(ahead);
    H.game.tun.experimental = true; H.game.tun.expRoom = blitz ? ROOM_BLITZ : ROOM_CORNERS; H.game.tun.players = 4;
    H.game.tun.teamMode = TEAMS_FFA;
    auto arenaOf = [](const Game& g) {   // (the room's arena, as the game picks it: main.cpp's arenaFor)
        const Tunables& t = g.tun;
        if (t.experimental && t.expRoom == ROOM_BLITZ) return (int)ARENA_OCTAGON;
        return !t.experimental || t.expRoom != ROOM_CORNERS ? (int)ARENA_CLASSIC : t.players == 4 ? (int)ARENA_FOURCORNERS : (int)ARENA_CORNERS;
    };
    // what is tracked: the four balls, or (FIREBALL BLITZ) the twelve fireball slots, three a player; whose it is
    const int nTrack = blitz ? 4 * Game::kMaxFire : 4;
    auto ballOf = [&](const Game& g, int i) -> const Ball& { return blitz ? g.fire[i / Game::kMaxFire][i % Game::kMaxFire] : g.pl[i].ball; };
    auto ownerOf = [&](int i) { return blitz ? i / Game::kMaxFire : i; };
    std::vector<GameEvent> hostLog;
    double exUs = 0;
    long exN = 0;
    auto pump = [&](double seconds, bool play) {
        const double end = clockNow() + seconds;
        while (clockNow() < end) {
            for (int i = 0; i < 4; ++i) {
                Node& n = *ns[i];
                n.game.useArena(arenaOf(n.game), arena);
                const int s = n.net.localSlot();
                if (play && s >= 0) n.th.step(n.game, s, clockNow());
                else n.th.in = n.game.np == 4 && s >= 0 ? standing4(n.game, s) : standing(n.game, 0);
                PlayerInput in[kMaxPlayers];
                const double e0 = clockNow();
                const float fdt = n.lastFrame > 0 ? (float)std::min(0.05, e0 - n.lastFrame) : 1.f / kHz;
                n.lastFrame = e0;
                if (n.net.exchange(n.th.in, in, n.game, fdt)) {
                    n.game.step(fdt, in); n.net.sendState(n.game);
                    if (i == 0 && play) hostLog.insert(hostLog.end(), n.game.events.begin(), n.game.events.end());
                }
                if (i == 1 && play) { exUs += (clockNow() - e0) * 1e6; ++exN; }
            }
            std::this_thread::sleep_for(std::chrono::microseconds((int)(1e6 / kHz)));
        }
    };
    for (Node* n : ns) n->net.goOnline(server);
    pump(1.5, false);
    for (Node* n : ns)
        if (!n->net.onlineReady()) { std::printf("not connected to the relay at %s\n", server); return 1; }
    H.net.host();
    pump(0.5, false);
    H.net.queue(true);
    uint32_t hip = 0;
    for (int k = 0; k < 60 && !hip; ++k) {
        pump(0.1, false);
        for (const LanGame& lg : A.net.games()) if (std::strcmp(lg.name, "HOST") == 0) hip = lg.ip;
    }
    if (!hip) { std::printf("no beacon from the host\n"); return 1; }
    for (int i = 1; i < 4; ++i) ns[i]->net.join(hip);
    pump(2.0, false);
    for (int i = 1; i < 4; ++i) ns[i]->net.queue(true);
    bool all = false;
    for (int k = 0; k < 80 && !all; ++k) {
        pump(0.1, false);
        all = true;
        for (Node* n : ns) all &= n->net.localSlot() >= 0 && n->game.np == 4;
    }
    if (!all) { std::printf("not everyone got a corner\n"); return 1; }
    for (Node* n : ns) n->th.target = (n->net.localSlot() + 1) % 4;
    const int me = A.net.localSlot();
    std::printf("in the match (%s, free-for-all; FAR ONE in place %d); playing %.0f s\n", blitz ? "FIREBALL BLITZ" : "FOUR CORNERS", me, secs);

    struct Seen { double t; Vec3 p; bool atMe; };   // (atMe: within 2.5 m of FAR ONE's place then, where blocks are made)
    std::vector<Seen> real[4 * Game::kMaxFire], drawnAt[4 * Game::kMaxFire];
    std::vector<double> speeds, pings, ownErr, wobble[2];   // (wobble: 0 the others' balls, 1 my own, as I see them)
    Vec3 drawn[4 * Game::kMaxFire][2];
    int drawnN[4 * Game::kMaxFire] = {};
    float flightErr = -1;
    int knockouts = 0, matches = 0;
    const double t0 = clockNow();
    double lastPrint = 0;
    while (clockNow() - t0 < secs) {
        pump(1.0 / kHz, true);
        const double t = clockNow();
        const Vec3 myPlace = H.game.tun.start[me];
        for (int q = 0; q < nTrack; ++q) {
            const Ball& hq = ballOf(H.game, q);
            const Ball& d = ballOf(A.game, q);
            const Vec3 off = d.pos - myPlace;
            if (hq.state == BallState::Flying) speeds.push_back(length(hq.vel));
            if (flying(hq)) real[q].push_back({t, hq.pos, false});
            const bool fl = flying(d);
            if (fl) drawnAt[q].push_back({t, d.pos, length(Vec3{off.x, 0, off.z}) < 2.5f});
            const Vec3 m0 = drawn[q][1] - drawn[q][0], m1 = d.pos - drawn[q][1];
            const bool turned = drawnN[q] >= 2 && dot(m0, m1) < 0.9f * length(m0) * length(m1);
            if (fl && drawnN[q] >= 2 && !turned) {
                const double w = length(d.pos - (drawn[q][1] + (drawn[q][1] - drawn[q][0]))) * 100.0;
                wobble[ownerOf(q) == me].push_back(w);
                if (w > 25.0 && std::getenv("PA_JUMPLOG")) {   // (each jump: where, the host's ball then, what just happened)
                    std::printf("JUMP %.2f s track %d (%s): %.0f cm; drawn at %.2f %.2f %.2f, the host's %s at %.2f %.2f %.2f; bounces %d/%d; host events:",
                                t - t0, q, ownerOf(q) == me ? "mine" : "theirs", w, d.pos.x, d.pos.y, d.pos.z, flying(hq) ? "flying" : "not flying",
                                hq.pos.x, hq.pos.y, hq.pos.z, d.bounces, hq.bounces);
                    for (const GameEvent& e : H.game.events) std::printf(" %d/%d", (int)e.type, e.player);
                    std::printf("\n");
                }
            }
            if (fl) { drawn[q][0] = drawn[q][1]; drawn[q][1] = d.pos; ++drawnN[q]; } else drawnN[q] = 0;
        }
        float ownNow = -1;   // (my own throw's correction still gliding, the biggest in each flight: cm)
        for (int q = 0; q < nTrack; ++q) {
            if (ownerOf(q) != me || !flying(ballOf(A.game, q))) continue;
            const Vec3 e = blitz ? A.game.ahead.fireErr[q / Game::kMaxFire][q % Game::kMaxFire] : A.game.ahead.err[me];
            ownNow = std::max(ownNow, length(e) * 100.f);
        }
        if (ownNow >= 0) flightErr = std::max(flightErr, ownNow);
        else if (flightErr >= 0) { ownErr.push_back(flightErr); flightErr = -1; }
        for (const GameEvent& e : H.game.events) { knockouts += e.type == EventType::Knockout; matches += e.type == EventType::MatchEnd; }
        if (A.net.pingMs() > 0 && t - lastPrint > 2.0) pings.push_back(A.net.pingMs());
        if (t - lastPrint > 10.0) {
            lastPrint = t;
            std::printf("  %3.0f s: match %d, points %d %d %d %d, throws %zu %zu %zu %zu, ping %.0f ms\n", t - t0, (int)H.game.match,
                        H.game.pl[0].score, H.game.pl[1].score, H.game.pl[2].score, H.game.pl[3].score, H.th.leave.size(),
                        A.th.leave.size(), B.th.leave.size(), D.th.leave.size(), A.net.pingMs());
        }
    }
    if (blitz) std::printf("RESULT knockouts %d\nRESULT matches %d\n", knockouts, matches);
    // the others' balls as FAR ONE sees them: how late (+) or early (-) against the host's, and a block made against
    // one near FAR ONE's corner: how far off it lands (the hands reach the host half a round trip later)
    std::vector<double> incoming, blockErr;
    const double half = median(pings) / 2000.0;
    for (int q = 0; q < nTrack; ++q) {
        if (ownerOf(q) == me) continue;
        size_t lo = 0;
        for (const Seen& s : drawnAt[q]) {
            while (lo < real[q].size() && real[q][lo].t < s.t - 0.7) ++lo;
            float best = 1e9f;
            double at = 0;
            for (size_t i = lo; i < real[q].size() && real[q][i].t < s.t + 0.7; ++i) {
                const float d = length(real[q][i].p - s.p);
                if (d < best) { best = d; at = real[q][i].t; }
            }
            if (best > 0.15f) continue;
            incoming.push_back((s.t - at) * 1000.0);
            if (s.atMe) blockErr.push_back(std::fabs(at - (s.t + half)) * 1000.0);
        }
    }
    auto ms = [](const std::vector<double>& v) { std::vector<double> o; for (double x : v) o.push_back(x * 1000.0); return o; };
    std::vector<double> grab, leave;
    for (int i = 1; i < 4; ++i) {
        for (double g : ns[i]->th.grab) grab.push_back(g);
        for (double l : ns[i]->th.leave) leave.push_back(l);
    }
    std::printf("RESULT block_error_ms %.0f\n", median(blockErr));
    std::printf("RESULT ping_ms %.0f\n", median(pings));
    std::printf("RESULT host_grab_ms %.0f\n", median(ms(H.th.grab)));
    std::printf("RESULT host_leave_ms %.0f\n", median(ms(H.th.leave)));
    std::printf("RESULT player_grab_ms %.0f\n", median(ms(grab)));
    std::printf("RESULT player_leave_ms %.0f\n", median(ms(leave)));
    std::printf("RESULT player_leave_p90_ms %.0f\n", pct(ms(leave), 0.9));
    std::printf("RESULT incoming_late_ms %.0f\n", median(incoming));
    std::printf("RESULT incoming_late_p90_ms %.0f\n", pct(incoming, 0.9));
    std::printf("RESULT ball_speed %.1f\n", median(speeds));
    std::printf("RESULT throws %zu %zu %zu %zu\n", H.th.leave.size(), A.th.leave.size(), B.th.leave.size(), D.th.leave.size());
    std::printf("RESULT missed %d %d %d %d\n", H.th.missed, A.th.missed, B.th.missed, D.th.missed);
    {   // what the host counted for the three players: their blocks, and the balls that hit them
        int blocks = 0, hits = 0;
        const int hostSlot = H.net.localSlot();
        for (const GameEvent& e : hostLog) {
            const bool guest = e.player >= 0 && e.player != hostSlot;
            if ((e.type == EventType::KnuckleBlock || e.type == EventType::ShieldBlock) && guest) ++blocks;
            if (e.type == EventType::HitPlayer && guest) ++hits;
        }
        std::printf("RESULT player_blocks %d\n", blocks);
        std::printf("RESULT player_hit %d\n", hits);
    }
    std::printf("RESULT own_glide_cm %.0f\n", median(ownErr));
    std::printf("RESULT own_glide_p90_cm %.0f\n", pct(ownErr, 0.9));
    std::printf("RESULT exchange_us %.0f\n", exN ? exUs / exN : -1.0);
    for (int mine = 0; mine < 2; ++mine) {   // off the ball's real path (cm): the others' balls, then my own
        std::vector<double> off;
        for (int q = 0; q < nTrack; ++q) {
            if ((ownerOf(q) == me) != (mine == 1)) continue;
            for (const Seen& s : drawnAt[q]) {
                // (FIREBALL BLITZ: against any of that player's fireballs: the host may put a throw in another slot
                // than the one it was drawn in, when one of theirs went out meanwhile)
                float best = 1e9f;
                for (int r = 0; r < nTrack; ++r) {
                    if (blitz ? ownerOf(r) != ownerOf(q) : r != q) continue;
                    const std::vector<Seen>& rp = real[r];
                    const size_t lo = std::lower_bound(rp.begin(), rp.end(), s.t - 0.8, [](const Seen& x, double t) { return x.t < t; }) - rp.begin();
                    for (size_t i = lo; i + 1 < rp.size() && rp[i].t < s.t + 0.8; ++i) {
                        const Vec3 a = rp[i].p, b = rp[i + 1].p, ab = b - a;
                        if (rp[i + 1].t - rp[i].t > 0.1 || length(ab) > 1.f) continue;
                        const float u = clampf(dot(s.p - a, ab) / std::max(1e-6f, dot(ab, ab)), 0.f, 1.f);
                        best = std::min(best, length(s.p - (a + ab * u)));
                    }
                }
                if (best < 1e8f) off.push_back(best * 100.0);
            }
        }
        std::printf("RESULT offpath%d_p99_cm %.0f\n", mine, pct(off, 0.99));
        std::printf("RESULT offpath%d_max_cm %.0f\n", mine, off.empty() ? -1.0 : *std::max_element(off.begin(), off.end()));
        int big = 0;
        for (double w : wobble[mine]) big += w > 25.0;
        std::printf("RESULT wobble%d_p99_cm %.0f\n", mine, pct(wobble[mine], 0.99));
        std::printf("RESULT jumps%d %d\n", mine, big);
    }
    for (Node* n : ns) { n->net.goOffline(); n->net.shutdown(); }
    return 0;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const double secs = argc > 1 ? std::atof(argv[1]) : 90.0;
    const char* server = argc > 2 ? argv[2] : "127.0.0.1:47783";
    const bool ahead = argc > 3 ? std::atoi(argv[3]) != 0 : true;
    const bool catchMode = argc > 4 && std::atoi(argv[4]) == 1;   // (tools/run_catch_test.py)
    const bool shieldMode = argc > 4 && std::atoi(argv[4]) == 2;  // (tools/run_shield_test.py)
    const char* lc = std::getenv("PA_LATE_CATCH");                  // 0: the host without CATCHES ONLINE
    const std::vector<CollisionTri> arena = loadArena();
    if (arena.empty()) { std::printf("no arena.col\n"); return 1; }
    if (argc > 4 && (std::atoi(argv[4]) == 3 || std::atoi(argv[4]) == 4))   // (FOUR CORNERS: --four; FIREBALL BLITZ: --blitz)
        return main4(secs, server, ahead, arena, std::atoi(argv[4]) == 4);
    Node H("HOST", arena), C("FAR AWAY", arena);
    C.net.setLookAhead(ahead);
    H.th.catching = C.th.catching = catchMode;
    C.th.shielding = H.th.throwLow = shieldMode;   // (the player shields, the host throws low at them)
    if (const char* rt = std::getenv("PA_REACT")) C.th.react = (float)std::atof(rt);   // (s before the ball arrives)
    const char* pg = std::getenv("PA_GUARD");
    const bool slapMode = shieldMode && pg && std::strcmp(pg, "knuckle") == 0;
    C.th.slapping = slapMode;
    if (shieldMode) H.game.tun.knuckleMode = C.game.tun.knuckleMode = slapMode ? 1 : 0;   // (the shield alone; or the guard alone)
    if (const char* sa = std::getenv("PA_SNATCH_AT")) H.th.snatchAt = C.th.snatchAt = (float)std::atof(sa);   // (m from the head)
    H.game.lateCatch = !(lc && lc[0] == '0');
    if (const char* lb = std::getenv("PA_LATE_BLOCK")) H.game.lateBlock = lb[0] != '0';   // 0: the host without BLOCKS ONLINE
    Node* ns[2] = {&H, &C};
    std::vector<GameEvent> hostLog;   // (the host's events, every step: the player's blocks and catches, as the host counted them)
    double exUs = 0;   // the player's game: time spent in each frame's network exchange (the look-ahead with it)
    long exN = 0;
    // the player's throws, launched twice: at once in their own game (BALL LOOK-AHEAD) and by the host when the let-go
    // arrives. Each pair compared: where it starts, how fast, which way, and when
    struct Launch { double t; Vec3 p, v; };
    std::vector<Launch> localThrows, hostThrows;
    bool wasMine = false;
    auto pump = [&](double seconds, bool play) {
        const double end = clockNow() + seconds;
        while (clockNow() < end) {
            for (int i = 0; i < 2; ++i) {
                Node& n = *ns[i];
                if (play) n.th.step(n.game, i, clockNow());
                else n.th.in = standing(n.game, i);
                PlayerInput in[kMaxPlayers];
                const double e0 = clockNow();
                // (each game steps by the real time since its last frame, as the game does: a host's clock keeps time)
                const float fdt = n.lastFrame > 0 ? (float)std::min(0.05, e0 - n.lastFrame) : 1.f / kHz;
                n.lastFrame = e0;
                if (n.net.exchange(n.th.in, in, n.game, fdt)) {
                    n.game.step(fdt, in); n.net.sendState(n.game);
                    if (i == 0 && play) hostLog.insert(hostLog.end(), n.game.events.begin(), n.game.events.end());
                    if (i == 0 && play)
                        for (const GameEvent& e : n.game.events)
                            if (e.type == EventType::Throw && e.player == 1)
                                hostThrows.push_back({e0, e.pos, n.game.pl[1].ball.vel});   // (where it was launched: the step has moved it on since)
                }
                if (i == 1 && play) {
                    if (n.game.ahead.mine && !wasMine) localThrows.push_back({e0, n.game.ahead.mineBall.pos, n.game.ahead.mineBall.vel});
                    wasMine = n.game.ahead.mine;
                }
                if (i == 1 && play) { exUs += (clockNow() - e0) * 1e6; ++exN; }
            }
            std::this_thread::sleep_for(std::chrono::microseconds((int)(1e6 / kHz)));
        }
    };
    H.net.goOnline(server); C.net.goOnline(server);
    pump(1.5, false);
    if (!H.net.onlineReady() || !C.net.onlineReady()) { std::printf("not connected to the relay at %s\n", server); return 1; }
    H.net.host();
    pump(0.5, false);
    H.net.queue(true);
    for (int k = 0; k < 60 && C.net.games().empty(); ++k) pump(0.1, false);
    if (C.net.games().empty()) { std::printf("no beacon from the host\n"); return 1; }
    C.net.join(C.net.games()[0].ip);
    pump(2.0, false);
    C.net.queue(true);
    for (int k = 0; k < 50 && C.net.localSlot() != 1; ++k) pump(0.1, false);
    if (C.net.localSlot() != 1) { std::printf("the player never got a place in the match\n"); return 1; }
    std::printf("in the match; playing %.0f s\n", secs);

    // the host's true ball positions (its game is the match), and where the player's game shows that ball: afterwards,
    // each shown position is matched to the moment the host had the ball there (BALL LOOK-AHEAD shows it ahead)
    struct Seen { double t; Vec3 p; };
    std::vector<Seen> truth, shown;
    // each ball's real flight (the host's game) and where the player's game draws it, flying: how far off that path it
    // is drawn (owner, 2026-11-10: BALL LOOK-AHEAD wobbled with no lag; the drawn ball swung across its path after each
    // bounce and block)
    std::vector<Seen> real[2], drawnAt[2];
    std::vector<double> speeds, pings, ownErr, wobble[2];
    Vec3 drawn[2][2];
    int drawnN[2] = {0, 0};
    float flightErr = -1;
    const double t0 = clockNow();
    double lastPrint = 0;
    while (clockNow() - t0 < secs) {
        pump(1.0 / kHz, true);
        const double t = clockNow();
        const Ball& hb = H.game.pl[0].ball;
        if (hb.state == BallState::Flying) {
            truth.push_back({t, hb.pos});
            speeds.push_back(length(hb.vel));
        }
        for (int q = 0; q < 2; ++q) {   // smoothness as drawn: off the path the last two frames set (cm), each ball
            const Ball& d = C.game.pl[q].ball;
            const bool fl = d.state == BallState::Flying || d.state == BallState::Returning;
            const Ball& hq = H.game.pl[q].ball;
            if (hq.state == BallState::Flying || hq.state == BallState::Returning) real[q].push_back({t, hq.pos});
            if (fl) drawnAt[q].push_back({t, d.pos});
            // (a frame where the drawn ball turned, a bounce or a block, is the ball's own path: not counted)
            const Vec3 m0 = drawn[q][1] - drawn[q][0], m1 = d.pos - drawn[q][1];
            const bool turned = drawnN[q] >= 2 && dot(m0, m1) < 0.9f * length(m0) * length(m1);
            if (fl && drawnN[q] >= 2 && !turned) wobble[q].push_back(length(d.pos - (drawn[q][1] + (drawn[q][1] - drawn[q][0]))) * 100.0);
            if (fl) { drawn[q][0] = drawn[q][1]; drawn[q][1] = d.pos; ++drawnN[q]; } else drawnN[q] = 0;
        }
        const Ball& cb = C.game.pl[0].ball;   // the host's ball, as the far-away player's game shows it now
        if (cb.state == BallState::Flying) shown.push_back({t, cb.pos});
        // the player's own throw as their game draws it: the biggest correction still gliding in each flight (cm)
        const Ball& own = C.game.pl[1].ball;
        if (own.state == BallState::Flying || own.state == BallState::Returning) flightErr = std::max(flightErr, length(C.game.ahead.err[1]) * 100.f);
        else if (flightErr >= 0) { ownErr.push_back(flightErr); flightErr = -1; }
        if ((own.state == BallState::Flying || own.state == BallState::Returning) && flightErr < 0) flightErr = 0;
        if (C.net.pingMs() > 0 && t - lastPrint > 2.0) { pings.push_back(C.net.pingMs()); }
        if (t - lastPrint > 10.0) {
            lastPrint = t;
            std::printf("  %3.0f s: match %d, score %d - %d, throws host %zu / player %zu, ping %.0f ms\n", t - t0, (int)H.game.match,
                        H.game.pl[0].score, H.game.pl[1].score, H.th.leave.size(), C.th.leave.size(), C.net.pingMs());
        }
    }
    // how late (+) or early (-) the player's game shows the host's ball: the time it is shown minus the time the host had
    // it there (within the same flight: the host's samples within 0.7 s either side, the nearest within 15 cm)
    std::vector<double> incoming, blockErr;
    const double half = median(pings) / 2000.0;   // the player's hands reach the host about half a round trip later
    size_t lo = 0;
    for (const Seen& s : shown) {
        while (lo < truth.size() && truth[lo].t < s.t - 0.7) ++lo;
        float best = 1e9f;
        double at = 0;
        for (size_t i = lo; i < truth.size() && truth[i].t < s.t + 0.7; ++i) {
            const float d = length(truth[i].p - s.p);
            if (d < best) { best = d; at = truth[i].t; }
        }
        if (best > 0.15f) continue;
        incoming.push_back((s.t - at) * 1000.0);
        // a block made against it (near the player's end, where blocks are made) lands where the host has the ball when
        // the hands' move arrives: off by this much
        if (std::fabs(s.p.x - H.game.tun.start[1].x) < 2.5f) blockErr.push_back(std::fabs(at - (s.t + half)) * 1000.0);
    }
    std::printf("RESULT block_error_ms %.0f\n", median(blockErr));
    auto ms = [](const std::vector<double>& v) { std::vector<double> o; for (double x : v) o.push_back(x * 1000.0); return o; };
    std::printf("RESULT ping_ms %.0f\n", median(pings));
    std::printf("RESULT host_grab_ms %.0f\n", median(ms(H.th.grab)));
    std::printf("RESULT host_leave_ms %.0f\n", median(ms(H.th.leave)));
    std::printf("RESULT player_grab_ms %.0f\n", median(ms(C.th.grab)));
    std::printf("RESULT player_leave_ms %.0f\n", median(ms(C.th.leave)));
    std::printf("RESULT player_leave_p90_ms %.0f\n", pct(ms(C.th.leave), 0.9));
    std::printf("RESULT incoming_late_ms %.0f\n", median(incoming));
    std::printf("RESULT incoming_late_p90_ms %.0f\n", pct(incoming, 0.9));
    std::printf("RESULT ball_speed %.1f\n", median(speeds));
    std::printf("RESULT throws %zu %zu\n", H.th.leave.size(), C.th.leave.size());
    std::printf("RESULT missed %d %d\n", H.th.missed, C.th.missed);
    {
        int hb = 0;
        for (const GameEvent& e : hostLog) hb += (e.type == EventType::KnuckleBlock || e.type == EventType::ShieldBlock) && e.player == 1;
        std::printf("RESULT player_blocks %d\n", hb);
    }
    std::printf("RESULT own_glide_cm %.0f\n", median(ownErr));
    std::printf("RESULT own_glide_p90_cm %.0f\n", pct(ownErr, 0.9));
    std::printf("RESULT exchange_us %.0f\n", exN ? exUs / exN : -1.0);
    if (shieldMode) {   // the host's balls at the player, and what the host counted: blocked by the shield, or a hit
        int blocks = 0, hits = 0;
        for (const GameEvent& e : hostLog) {
            if ((e.type == EventType::ShieldBlock || e.type == EventType::KnuckleBlock) && e.player == 1) ++blocks;
            if (e.type == EventType::HitPlayer && e.player == 1) ++hits;
        }
        std::printf("RESULT shield_incoming %d\n", C.th.incoming);
        std::printf("RESULT shield_blocks %d\n", blocks);
        std::printf("RESULT shield_hits %d\n", hits);
    }
    if (catchMode) {   // the snatches each side made at its own returning ball, and what the host made of them
        int caught[2] = {0, 0}, selfHit[2] = {0, 0};
        for (const GameEvent& e : hostLog) {
            if (e.type == EventType::Caught && e.player >= 0 && e.player < 2) ++caught[e.player];
            if (e.type == EventType::HitPlayer && e.player >= 0 && e.player < 2) ++selfHit[e.player];
        }
        std::printf("RESULT catch_host %d/%d\n", caught[0], H.th.returns);
        std::printf("RESULT catch_player %d/%d\n", caught[1], C.th.returns);
        std::printf("RESULT selfhit_player %d\n", selfHit[1]);
        std::printf("RESULT late_catch %s\n", H.game.lateCatch ? "on" : "off");
    }
    {   // each local throw with the host's next throw for the player (within 1 s)
        std::vector<double> dPos, dSpeed, dDeg, dT, dAt;
        size_t k = 0;
        for (const Launch& l : localThrows) {
            while (k < hostThrows.size() && hostThrows[k].t < l.t - 0.05) ++k;
            if (k >= hostThrows.size() || hostThrows[k].t > l.t + 1.0) continue;
            const Launch& h = hostThrows[k++];
            const double dt = h.t - l.t;
            dT.push_back(dt * 1000.0);
            dSpeed.push_back(length(h.v) - length(l.v));
            dDeg.push_back(rad2deg(std::acos(clampf(dot(normalize(h.v), normalize(l.v)), -1.f, 1.f))));
            dPos.push_back(length(h.p - l.p) * 100.0);
            // where each has the ball 0.5 s into its own flight (straight on, no gravity: the start, the speed and
            // the aim; the host's launch comes a round trip later, which BALL LOOK-AHEAD's lead already allows for)
            const Vec3 a = l.p + l.v * 0.5f, b = h.p + h.v * 0.5f;
            dAt.push_back(length(a - b) * 100.0);
        }
        std::printf("RESULT own_pairs %zu\n", dT.size());
        std::printf("RESULT own_host_later_ms %.0f\n", median(dT));
        std::printf("RESULT own_start_cm %.0f\n", median(dPos));
        std::printf("RESULT own_speed_ms %.2f\n", median(dSpeed));
        std::printf("RESULT own_dir_deg %.1f\n", median(dDeg));
        std::printf("RESULT own_apart_cm %.0f\n", median(dAt));
    }
    for (int q = 0; q < 2; ++q) {   // off the ball's real path (cm): the nearest point of the host's flight within 0.8 s
        std::vector<double> off;
        size_t lo = 0;
        for (const Seen& s : drawnAt[q]) {
            while (lo < real[q].size() && real[q][lo].t < s.t - 0.8) ++lo;
            float best = 1e9f;
            for (size_t i = lo; i + 1 < real[q].size() && real[q][i].t < s.t + 0.8; ++i) {
                const Vec3 a = real[q][i].p, b = real[q][i + 1].p, ab = b - a;
                if (real[q][i + 1].t - real[q][i].t > 0.1 || length(ab) > 1.f) continue;   // (not the same flight)
                const float u = clampf(dot(s.p - a, ab) / std::max(1e-6f, dot(ab, ab)), 0.f, 1.f);
                best = std::min(best, length(s.p - (a + ab * u)));
            }
            if (best < 1e8f) off.push_back(best * 100.0);
        }
        std::printf("RESULT offpath%d_p99_cm %.0f\n", q, pct(off, 0.99));
        std::printf("RESULT offpath%d_max_cm %.0f\n", q, off.empty() ? -1.0 : *std::max_element(off.begin(), off.end()));
    }
    for (int q = 0; q < 2; ++q) {
        int big = 0;
        for (double w : wobble[q]) big += w > 25.0;
        std::printf("RESULT wobble%d_p99_cm %.0f\n", q, pct(wobble[q], 0.99));
        std::printf("RESULT jumps%d %d\n", q, big);
    }
    C.net.goOffline(); H.net.goOffline();
    C.net.shutdown(); H.net.shutdown();
    return 0;
}
