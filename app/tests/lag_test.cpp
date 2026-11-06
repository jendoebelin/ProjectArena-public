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

// one player's scripted throws, judged from what that player's own game shows
struct Thrower {
    enum { REST, REACH, SWING, RELEASED } st = REST;
    double t0 = 0, nextTry = 0;
    int frames = 0;
    Vec3 dir;
    PlayerInput in;
    std::vector<double> grab, leave;
    int missed = 0;
    long blockFrames = 0;
    void step(const Game& v, int p, double t) {
        const Ball& b = v.pl[p].ball;
        const bool playing = v.match == MatchState::InProgress;
        PlayerInput rest = standing(v, p);
        rest.buttonA = !playing;   // READY when a match waits for it
        // the left hand blocks: the other's ball coming at me, as my game draws it, met with the knuckle guard where it
        // will cross 0.5 m in front of me
        bool blocking = false;
        HandInput guard = rest.hand[0];
        {
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
                dir = normalize(v.tun.start[1 - p] + Vec3{0, 1.6f, 0} - in.hand[1].grip.p);
            } else if (t - t0 > 4.0 || !playing) { st = REST; ++missed; nextTry = t + 1.0; }
            break;
        case SWING:   // 15 steps at 9 m/s toward the other's head, then let go
            in.hand[1].grip.p += dir * (9.f / kHz);
            if (++frames >= 15) { in.hand[1].trigger = 0; st = RELEASED; t0 = t; }
            break;
        case RELEASED:   // my game shows my ball gone from my hand
            if (b.state == BallState::Flying || b.state == BallState::Returning) {
                leave.push_back(t - t0);
                st = REST; nextTry = t + 1.5;
            } else if (t - t0 > 3.0) { ++missed; st = REST; nextTry = t + 1.0; }
            else { in.hand[1].grip = rest.hand[1].grip; in.hand[1].trigger = 0; }
            break;
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

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const double secs = argc > 1 ? std::atof(argv[1]) : 90.0;
    const char* server = argc > 2 ? argv[2] : "127.0.0.1:47783";
    const bool ahead = argc > 3 ? std::atoi(argv[3]) != 0 : true;
    const std::vector<CollisionTri> arena = loadArena();
    if (arena.empty()) { std::printf("no arena.col\n"); return 1; }
    Node H("HOST", arena), C("FAR AWAY", arena);
    C.net.setLookAhead(ahead);
    Node* ns[2] = {&H, &C};
    double exUs = 0;   // the player's game: time spent in each frame's network exchange (the look-ahead with it)
    long exN = 0;
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
                if (n.net.exchange(n.th.in, in, n.game, fdt)) { n.game.step(fdt, in); n.net.sendState(n.game); }
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
    std::vector<double> speeds, pings, ownErr, wobble[2];
    std::vector<GameEvent> hostLog;   // (the host's events: the player's blocks, as the host counted them)
    Vec3 drawn[2][2];
    int drawnN[2] = {0, 0};
    float flightErr = -1;
    const double t0 = clockNow();
    double lastPrint = 0;
    while (clockNow() - t0 < secs) {
        pump(1.0 / kHz, true);
        const double t = clockNow();
        for (const GameEvent& e : H.game.events) hostLog.push_back(e);
        const Ball& hb = H.game.pl[0].ball;
        if (hb.state == BallState::Flying) {
            truth.push_back({t, hb.pos});
            speeds.push_back(length(hb.vel));
        }
        for (int q = 0; q < 2; ++q) {   // smoothness as drawn: off the path the last two frames set (cm), each ball
            const Ball& d = C.game.pl[q].ball;
            const bool fl = d.state == BallState::Flying || d.state == BallState::Returning;
            if (fl && drawnN[q] >= 2) wobble[q].push_back(length(d.pos - (drawn[q][1] + (drawn[q][1] - drawn[q][0]))) * 100.0);
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
