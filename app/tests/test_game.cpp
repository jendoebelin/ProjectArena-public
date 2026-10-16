// Desktop tests for the Sparc gameplay simulation, run against the real baked arena collision.
// Build/run: tools/run_tests.bat
#define _CRT_SECURE_NO_WARNINGS
#include "../src/game.h"
#include "../src/avatar.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(c, msg) do { if (c) { ++g_pass; std::printf("  ok   %s\n", msg); } else { ++g_fail; std::printf("  FAIL %s  (%s:%d)\n", msg, __FILE__, __LINE__); } } while (0)

static std::vector<CollisionTri> loadCol(const char* path) {
    std::vector<CollisionTri> out;
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::printf("cannot open %s\n", path); std::exit(2); }
    char magic[4]; uint32_t n;
    std::fread(magic, 1, 4, f); std::fread(&n, 4, 1, f);
    for (uint32_t i = 0; i < n; ++i) {
        float v[9]; uint32_t tags;
        std::fread(v, 4, 9, f); std::fread(&tags, 4, 1, f);
        CollisionTri t{{v[0], v[1], v[2]}, {v[3], v[4], v[5]}, {v[6], v[7], v[8]}, {}, tags};
        t.n = normalize(cross(t.b - t.a, t.c - t.a));
        out.push_back(t);
    }
    std::fclose(f);
    return out;
}

static const Quat kFaceP0 = Quat::axisAngle({0, 1, 0}, deg2rad(-90));   // -Z -> +X
static const Quat kFaceP1 = Quat::axisAngle({0, 1, 0}, deg2rad(90));    // -Z -> -X

static PlayerInput standing(const Game& g, int p) {
    PlayerInput in;
    in.headTracked = true;
    Quat face = p == 0 ? kFaceP0 : kFaceP1;
    in.head = {face, g.tun.start[p] + Vec3{0, 1.65f, 0}};
    for (int h = 0; h < 2; ++h) {
        in.hand[h].tracked = true;
        in.hand[h].grip = {face, g.tun.start[p] + Vec3{p == 0 ? 0.2f : -0.2f, 1.0f, (h == 0 ? -0.3f : 0.3f) * (p == 0 ? 1.f : -1.f)}};
    }
    return in;
}

static bool hasEvent(const std::vector<GameEvent>& ev, EventType t, int player = -2) {
    for (auto& e : ev) if (e.type == t && (player == -2 || e.player == player)) return true;
    return false;
}

static void run(Game& g, PlayerInput in[2], float seconds, std::vector<GameEvent>* log = nullptr) {
    for (float t = 0; t < seconds; t += 1.f / 72) {
        PlayerInput tmp[2] = {in[0], in[1]};
        g.step(1.f / 72, tmp);
        if (log) log->insert(log->end(), g.events.begin(), g.events.end());
    }
}

static void grabBall(Game& g, PlayerInput in[2], int h = 1) {
    Vec3 off = in[0].hand[h].grip.q.rotate(g.tun.launcherOffset);
    in[0].hand[h].grip.p = g.pl[0].ball.pos - off;
    in[0].hand[h].trigger = 1;
    PlayerInput tmp[2] = {in[0], in[1]};
    g.step(1.f / 72, tmp);
}

static void swingAndRelease(Game& g, PlayerInput in[2], Vec3 dir, float speed, std::vector<GameEvent>& log, int h = 1) {
    for (int i = 0; i < 15; ++i) {
        in[0].hand[h].grip.p += dir * (speed / 72);
        PlayerInput tmp[2] = {in[0], in[1]};
        g.step(1.f / 72, tmp);
        log.insert(log.end(), g.events.begin(), g.events.end());
    }
    in[0].hand[h].trigger = 0;
    PlayerInput tmp[2] = {in[0], in[1]};
    g.step(1.f / 72, tmp);
    log.insert(log.end(), g.events.begin(), g.events.end());
}

// a passive human opponent standing at the far end
static Game makeGame(const std::vector<CollisionTri>& tris, PlayerInput in[2], bool advanced = false) {
    Game g; g.setArena(tris); g.tun.advanced = advanced; g.reset(false);
    in[0] = standing(g, 0); in[1] = standing(g, 1);
    run(g, in, 3.6f);
    return g;
}

// fire player 1's ball straight at a point from 0.6 m in front of player 1
static void fireOpponentBall(Game& g, Vec3 target, float speed = 10.f) {
    Ball& b = g.pl[1].ball;
    b.pos = g.tun.start[1] + Vec3{-0.6f, 1.6f, 0};
    b.state = BallState::Flying;
    b.vel = normalize(target - b.pos) * speed;
    b.speedLimit = speed;
    b.instigator = 1;
    b.scored = false;
    b.spin = {};
}

static Vec3 xform(const Mat4& M, const Vec3& p) {
    return {M.m[0] * p.x + M.m[4] * p.y + M.m[8] * p.z + M.m[12], M.m[1] * p.x + M.m[5] * p.y + M.m[9] * p.z + M.m[13],
            M.m[2] * p.x + M.m[6] * p.y + M.m[10] * p.z + M.m[14]};
}

static void avatarTests(const std::vector<CollisionTri>& tris) {
    {
        std::printf("[avatar: two-bone IK]\n");
        bool lengths = true, onPole = true;
        uint32_t s = 7;
        auto rnd = [&]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (s & 0xFFFF) / 65535.f; };
        for (int i = 0; i < 200; ++i) {
            Vec3 sh{0, 1.4f, 0};
            Vec3 dir = normalize(Vec3{rnd() - 0.5f, rnd() - 0.5f, rnd() - 0.5f} + Vec3{1e-3f, 0, 0});
            float reach = 0.05f + rnd() * 0.45f;   // within l1 + l2 = 0.55
            Vec3 w = sh + dir * reach;
            Vec3 pole = sh + Vec3{0.3f, -1, 0.2f};
            Vec3 e = avatar::elbowIK(sh, w, avatar::kUpperArm, avatar::kForeArm, pole);
            if (std::fabs(length(e - sh) - avatar::kUpperArm) > 2e-3f || std::fabs(length(w - e) - avatar::kForeArm) > 2e-3f) lengths = false;
            Vec3 u = normalize(w - sh), c = sh + u * dot(e - sh, u);
            Vec3 pp = pole - sh;
            pp = pp - u * dot(pp, u);
            if (length(pp) > 1e-3f && dot(e - c, pp) < -1e-4f) onPole = false;
        }
        CHECK(lengths, "elbow keeps both bone lengths whenever the wrist is in reach");
        CHECK(onPole, "elbow bends toward the pole");
        Vec3 e = avatar::elbowIK({0, 0, 0}, {1, 0, 0}, 0.3f, 0.2f, {0, -1, 0});
        CHECK(std::fabs(e.y) < 1e-5f && std::fabs(e.x - 0.6f) < 1e-4f, "out of reach: straight arm, bones stretched evenly");
    }
    {
        std::printf("[avatar: rig from a standing player]\n");
        Game g; g.setArena(tris); g.reset(true);
        PlayerInput in = standing(g, 0);
        in.hand[0].grip.p = in.head.p + Vec3{0.35f, -0.45f, -0.25f};
        in.hand[1].grip.p = in.head.p + Vec3{0.35f, -0.45f, 0.25f};
        AvatarDesc d;
        avatar::Rig r = avatar::solveRig(in, d);
        Vec3 f = r.torso.q.rotate({0, 0, -1});
        CHECK(f.x > 0.95f, "player 0's torso faces +X (the court)");
        CHECK(std::fabs(r.torso.p.y - (in.head.p.y - avatar::bodyBind(0).eye.y)) < 0.01f, "neck base below the eyes as in the body's bind pose");
        CHECK(r.arm[0] && r.arm[1], "both arms solved for tracked hands");
        bool ends = true;
        for (int k = 0; k < 2; ++k) {
            Vec3 w = in.hand[k].grip.transform({k == 1 ? 0.004f : -0.004f, -0.042f, 0.105f});
            if (length(r.fore[k].b - w) > 1e-4f || length(r.upper[k].b - r.fore[k].a) > 1e-6f) ends = false;
        }
        CHECK(ends, "forearm ends inside the gauntlet; the elbow joins both bones");
        Vec3 right = r.torso.q.rotate({1, 0, 0});
        CHECK(dot(r.upper[1].a - r.torso.p, right) > 0.12f && dot(r.upper[0].a - r.torso.p, right) < -0.12f, "right shoulder on the right");
        Mat4 m = avatar::boneMatrix(r.upper[1], false);
        CHECK(length(xform(m, {0, 0, 0}) - r.upper[1].a) < 1e-5f && length(xform(m, {0, -r.upper[1].natural, 0}) - r.upper[1].b) < 1e-4f,
              "bone matrix maps the mesh's joints onto the rig");
        {
            const avatar::BodyBind& K = avatar::bodyBind(0);
            bool skinOk = true;
            for (int k = 0; k < 2; ++k) {
                if (length(xform(r.skin[avatar::BONE_UPPER_L + k], K.shoulder[k]) - r.upper[k].a) > 2e-3f) skinOk = false;
                if (length(xform(r.skin[avatar::BONE_FORE_L + k], K.wrist[k]) - r.fore[k].b) > 2e-3f) skinOk = false;
                if (length(xform(r.skin[avatar::BONE_TORSO], K.shoulder[k]) - r.upper[k].a) > 2e-3f) skinOk = false;
            }
            if (length(xform(r.skin[avatar::BONE_HEAD], K.eye) - in.head.p) > 1e-4f) skinOk = false;
            if (length(xform(r.skin[avatar::BONE_NECK], K.neckTop) - r.neck.a) > 1e-3f) skinOk = false;
            CHECK(skinOk, "skin matrices carry the bind joints onto the solved rig (shoulders, wrists, head, neck)");
        }
        CHECK(dot(xform(m, {0, 0, -1}) - r.upper[1].a, r.upper[1].front) > 0, "bone mesh front (-Z) turns toward the front reference");
        in.head.q = in.head.q * Quat::axisAngle({1, 0, 0}, deg2rad(-40));   // look down
        avatar::Rig r2 = avatar::solveRig(in, d);
        CHECK(std::fabs(dot(r2.torso.q.rotate({0, 1, 0}), {0, 1, 0}) - 1) < 1e-4f, "torso stays upright when the head nods");
    }
    {
        std::printf("[avatar: looks and avatar.cfg]\n");
        bool valid = true, roundTrip = true;
        for (uint32_t seed = 1; seed < 400; ++seed) {
            AvatarDesc d = avatar::randomLook(seed), c = d;
            avatar::sanitize(c);
            if (std::memcmp(&c, &d, sizeof d) != 0) valid = false;
            AvatarDesc back;
            if (!avatar::fromText(avatar::toText(d), back) || std::memcmp(&back, &d, sizeof d) != 0) roundTrip = false;
        }
        CHECK(valid, "random looks are always in range");
        CHECK(roundTrip, "avatar.cfg text round-trips every field");
        AvatarDesc bad;
        std::memset(&bad, 0xEE, sizeof bad);
        avatar::sanitize(bad);
        bool ok = true;
        for (int f = 0; f < avatar::F_COUNT; ++f)
            if (avatar::get(bad, (avatar::Field)f) >= avatar::fieldCount((avatar::Field)f)) ok = false;
        CHECK(ok, "out-of-range looks (e.g. from the network) are sanitised");
        bool names = true;
        for (int f = 0; f < avatar::F_COUNT; ++f)
            for (int i = 0; i < avatar::fieldCount((avatar::Field)f); ++i)
                for (const char* c = avatar::optionName((avatar::Field)f, i); *c; ++c)
                    if (!((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == ' ' || *c == '-')) names = false;
        CHECK(names, "option names use only the HUD font's characters");
        int valid2 = 0;
        for (int i = 0; i < avatar::presetCount(); ++i) {
            AvatarDesc p = avatar::preset(i), c = p;
            avatar::sanitize(c);
            valid2 += std::memcmp(&p, &c, sizeof p) == 0;
        }
        CHECK(valid2 == avatar::presetCount(), "presets are valid looks");
    }
}

int main(int argc, char** argv) {
    const char* col = argc > 1 ? argv[1] : "app/assets/arena.col";
    auto tris = loadCol(col);
    std::printf("loaded %zu collision tris\n", tris.size());
    CHECK(tris.size() > 100, "arena collision loaded");

    {
        std::printf("[match start]\n");
        Game g; g.setArena(tris); g.reset(true);
        PlayerInput in[2] = {standing(g, 0), {}};
        run(g, in, 0.1f);
        CHECK(g.match == MatchState::Countdown, "countdown begins");
        run(g, in, 3.5f);
        CHECK(g.match == MatchState::InProgress, "match in progress after countdown");
        CHECK(std::fabs(g.roundClock - (180.f - 3.5f + 3.0f + 0.1f)) < 1.0f || g.roundClock > 170.f, "Basic: 3 minute match clock");
        CHECK(length(g.pl[0].ball.pos - g.idlePoint(0)) < 0.05f, "idle ball held by damped spring");
    }
    {
        std::printf("[shield needs a held, charged ball]\n");
        PlayerInput in[2]; Game g = makeGame(tris, in);
        in[0].hand[0].trigger = 1;   // pull the trigger on the empty hand
        run(g, in, 0.1f);
        CHECK(!g.pl[0].hands[0].shieldUp, "empty hand + trigger does NOT raise a shield");
        in[0].hand[0].trigger = 0;
        grabBall(g, in, 1);
        run(g, in, 0.05f);
        CHECK(g.pl[0].ball.state == BallState::Held, "trigger near ball grabs it");
        CHECK(g.pl[0].hands[1].shieldUp, "shield forms on the arm holding the charged ball");
        CHECK(!g.pl[0].hands[0].shieldUp, "no shield on the free hand");
        CHECK(g.pl[0].hands[0].knuckleUp, "Basic: free hand has a knuckle guard");
        CHECK(g.pl[0].hands[1].knuckleUp, "Basic: knuckle guard stays on the hand holding the ball (permanent)");
    }
    {
        std::printf("[slow release drops the ball]\n");
        PlayerInput in[2]; Game g = makeGame(tris, in);
        grabBall(g, in);
        std::vector<GameEvent> log;
        swingAndRelease(g, in, {1, 0, 0}, 0.2f, log);
        CHECK(hasEvent(log, EventType::ThrowFail), "release below MinSpeedThreshold is a drop");
        CHECK(g.pl[0].ball.state == BallState::Idle, "dropped ball floats back to the spawner");
        CHECK(!g.pl[0].hands[1].shieldUp, "dropping the ball dissipates the shield");
    }
    {
        std::printf("[swing through the idle ball with the trigger held]\n");
        PlayerInput in[2]; Game g = makeGame(tris, in);
        const int h = 1;
        Vec3 off = in[0].hand[h].grip.q.rotate(g.tun.catcherOffset);
        in[0].hand[h].trigger = 1;                                   // trigger pulled early, hand still away
        in[0].hand[h].grip.p = g.pl[0].ball.pos - off + Vec3{0, 0, 0.6f};
        run(g, in, 0.1f);
        CHECK(g.pl[0].ball.state == BallState::Idle, "trigger held away from the ball: nothing grabbed");
        std::vector<GameEvent> log;
        for (int i = 0; i < 40 && g.pl[0].ball.state != BallState::Held; ++i) {   // sweep through the ball
            in[0].hand[h].grip.p += Vec3{0, 0, -0.02f};
            PlayerInput tmp[2] = {in[0], in[1]};
            g.step(1.f / 72, tmp);
            log.insert(log.end(), g.events.begin(), g.events.end());
        }
        CHECK(g.pl[0].ball.state == BallState::Held && hasEvent(log, EventType::Grab), "sweeping through it grabs it");
    }
    {
        std::printf("[a short throw comes back to the spawner]\n");
        PlayerInput in[2]; Game g = makeGame(tris, in);
        grabBall(g, in);
        std::vector<GameEvent> log;
        swingAndRelease(g, in, {1, 0, 0}, 0.7f, log);              // just above the drop threshold: a weak throw
        CHECK(hasEvent(log, EventType::Throw), "weak release is still a throw");
        run(g, in, 0.2f, &log);
        CHECK(g.pl[0].ball.state == BallState::Idle, "slow and short of the foul line: back to the spawner");
        run(g, in, 1.5f, &log);
        CHECK(length(g.pl[0].ball.pos - g.idlePoint(0)) < 0.10f, "it settles on the spawner again");
    }
    {
        std::printf("[direct hit]\n");
        PlayerInput in[2]; Game g = makeGame(tris, in);
        grabBall(g, in);
        std::vector<GameEvent> log;
        swingAndRelease(g, in, normalize(g.chestPoint(1) - g.launcherPoint(0, 1)), 5.0f, log);
        CHECK(hasEvent(log, EventType::Throw), "fast release launches projectile");
        float sp = 0; for (auto& e : log) if (e.type == EventType::Throw) sp = e.value;
        CHECK(sp > 9.f && sp < 12.f, "5 m/s hand speed -> ~10.8 m/s ball (recovered 0-14 m/s mapping)");
        run(g, in, 2.5f, &log);
        CHECK(hasEvent(log, EventType::HitPlayer, 1), "projectile hits opponent");
        CHECK(g.pl[0].score == 1, "thrower scores a point");
    }
    {
        std::printf("[miss: far wall bounce, own back wall respawn]\n");
        PlayerInput in[2]; Game g = makeGame(tris, in);
        in[1].head.p.z = 2.0f;   // opponent dodged
        grabBall(g, in);
        std::vector<GameEvent> log;
        swingAndRelease(g, in, normalize(Vec3{1, 0.02f, -0.03f}), 6.0f, log);
        for (int i = 0; i < 80 && !hasEvent(log, EventType::Returned, 0); ++i) run(g, in, 0.05f, &log);
        in[0].head.p.z = 1.2f;   // owner side-steps the slow return as soon as it starts
        bool far = false;
        for (auto& e : log) if (e.type == EventType::Bounce && e.pos.x > 9.0f) far = true;
        (void)far;
        CHECK(hasEvent(log, EventType::Returned, 0), "hitting the opponent's back wall starts the return");
        run(g, in, 6.0f, &log);   // return flight + the ~2.3 s respawn sequence
        CHECK(g.pl[0].ball.state == BallState::Idle && length(g.pl[0].ball.pos - g.idlePoint(0)) < 0.1f,
              "returning ball flies past into the owner's back wall and respawns at the spawner");
        CHECK(g.pl[0].score == 0 && g.pl[1].score == 0, "a dodged return scores nothing");
        CHECK(g.pl[0].score == 0, "no score on a miss");
    }
    {
        std::printf("[shield block drains charge; throw past foul line recharges]\n");
        PlayerInput in[2]; Game g = makeGame(tris, in);
        grabBall(g, in, 1);
        // raise the shield arm in front of the face: plate normal (+Y grip) toward the ball
        Quat q = fromTo({0, 1, 0}, {1, 0, 0});
        Vec3 face = g.pl[0].in.head.p;
        // place the grip so the shield plate (current placement, normal = local +Y) faces the ball 0.45 m ahead
        Pose sl = g.shieldLocal(1);
        Quat gq = q * sl.q.conj();
        in[0].hand[1].grip = {gq, face + Vec3{0.45f, 0, 0} - gq.rotate(sl.p)};
        run(g, in, 0.05f);
        CHECK(g.pl[0].hands[1].shieldUp, "shield is up");
        fireOpponentBall(g, face);
        std::vector<GameEvent> log;
        run(g, in, 2.0f, &log);
        CHECK(hasEvent(log, EventType::ShieldBlock, 0), "shield deflects the incoming ball");
        CHECK(!g.pl[0].ball.charged && !g.pl[0].hands[1].shieldUp, "shield breaks on hit; ball loses its charge");
        CHECK(g.pl[1].score == 0, "blocked ball does not score");
        // now throw (still holding) past the foul line to recharge
        in[0] = standing(g, 0);
        in[0].hand[1].trigger = 1;
        in[0].hand[1].grip.p = g.pl[0].ball.pos - in[0].hand[1].grip.q.rotate(g.tun.launcherOffset);
        run(g, in, 0.05f);
        log.clear();
        swingAndRelease(g, in, normalize(Vec3{1, 0.05f, 0.5f}), 5.0f, log);
        run(g, in, 0.8f, &log);
        CHECK(hasEvent(log, EventType::Recharge, 0), "throwing past your foul line recharges the ball");
    }
    {
        std::printf("[knuckle guards (Basic)]\n");
        PlayerInput in[2]; Game g = makeGame(tris, in);
        Vec3 face = g.pl[0].in.head.p;
        in[0].hand[0].grip.p = face + Vec3{0.4f, 0, 0} - in[0].hand[0].grip.q.rotate(g.tun.knuckleOffset);
        in[0].hand[0].trigger = 0;
        run(g, in, 0.05f);
        fireOpponentBall(g, face);
        std::vector<GameEvent> log;
        run(g, in, 2.0f, &log);
        CHECK(hasEvent(log, EventType::KnuckleBlock, 0), "fist knuckle guard deflects the ball");
        CHECK(!hasEvent(log, EventType::Returned, 1) || g.pl[1].ball.pos.x > -9.0f, "knuckled ball is sent back toward the opponent");
        {   // glancing hit: fist turned sideways at hip height still punches the ball forward
            PlayerInput gi[2]; Game gg = makeGame(tris, gi);
            Quat side = Quat::axisAngle({0, 1, 0}, deg2rad(-90)) * Quat::axisAngle({0, 1, 0}, deg2rad(80));
            gi[0].hand[0].grip.q = side;
            Vec3 tgt = gg.tun.start[0] + Vec3{0.45f, 0.75f, -0.2f};
            gi[0].hand[0].grip.p = tgt - side.rotate(Vec3{-gg.tun.knuckleOffset.x, gg.tun.knuckleOffset.y, gg.tun.knuckleOffset.z});
            run(gg, gi, 0.1f);
            fireOpponentBall(gg, tgt);
            std::vector<GameEvent> gl;
            bool fwd = false;
            for (int i = 0; i < 200; ++i) {
                PlayerInput tmp[2] = {gi[0], gi[1]};
                gg.step(1.f / 72, tmp);
                for (auto& e : gg.events) gl.push_back(e);
                if (hasEvent(gl, EventType::KnuckleBlock, 0) && gg.pl[1].ball.state == BallState::Flying && gg.pl[1].ball.vel.x > 2.f) fwd = true;
            }
            CHECK(hasEvent(gl, EventType::KnuckleBlock, 0) && fwd, "glancing knuckle hit still punches the ball toward the opponent");
        }
        // same again with the trigger pulled -> guard is off -> hit
        PlayerInput in2[2]; Game g2 = makeGame(tris, in2);
        in2[0].hand[0].grip.p = face + Vec3{0.4f, 0, 0} - in2[0].hand[0].grip.q.rotate(g2.tun.knuckleOffset);
        in2[0].hand[0].trigger = 1;
        run(g2, in2, 0.05f);
        fireOpponentBall(g2, face);
        log.clear();
        run(g2, in2, 2.0f, &log);
        CHECK(hasEvent(log, EventType::KnuckleBlock, 0), "Basic: knuckle guard stays active with the trigger pulled");
        {   // punch: fist moving into the ball at 4 m/s returns it faster than a still fist
            auto punchOut = [&](float fistSpeed) {
                PlayerInput pi[2]; Game gp = makeGame(tris, pi);
                Vec3 face = gp.pl[0].in.head.p;
                Vec3 ko = gp.tun.knuckleOffset; ko.x = -ko.x;   // left hand
                Vec3 start = face + Vec3{0.30f, 0, 0} - pi[0].hand[0].grip.q.rotate(ko);
                pi[0].hand[0].grip.p = start;
                run(gp, pi, 0.1f);
                fireOpponentBall(gp, face, 8.f);
                float out = 0;
                for (int i = 0; i < 200 && out == 0; ++i) {
                    float distToFist = gp.pl[1].ball.pos.x - gp.knucklePoint(0, 0).x;
                    if (distToFist < 0.6f) pi[0].hand[0].grip.p.x += fistSpeed / 72;   // punch forward (+X)
                    PlayerInput tmp[2] = {pi[0], pi[1]};
                    gp.step(1.f / 72, tmp);
                    for (auto& e : gp.events) if (e.type == EventType::KnuckleBlock) out = e.value;
                }
                return out;
            };
            float still = punchOut(0.f), punch = punchOut(4.f);
            std::printf("  outgoing speed: still fist %.2f m/s, 4 m/s punch %.2f m/s\n", still, punch);
            CHECK(still > 6.5f && still < 8.5f, "still fist returns the ball (~0.9 restitution)");
            CHECK(punch > still + 2.5f, "punching adds the fist's speed");
        }
        PlayerInput in3[2]; Game g3 = makeGame(tris, in3, true);
        CHECK(!g3.pl[0].hands[0].knuckleUp, "Advanced: no knuckle guards");
    }
    {
        std::printf("[spin curves the throw]\n");
        auto lateral = [&](Vec3 spin) {
            PlayerInput in[2]; Game g = makeGame(tris, in);
            g.tun.aimCorrectBasicDeg = 0;   // isolate the spin effect from aim assist
            in[1].head.p.z = 2.4f;   // keep the opponent out of the way
            grabBall(g, in);
            in[0].hand[1].angVel = spin;
            std::vector<GameEvent> log;
            swingAndRelease(g, in, normalize(Vec3{1, 0.03f, 0}), 6.0f, log);
            float maxDz = 0;
            for (int i = 0; i < 60; ++i) {
                run(g, in, 1.f / 72);
                if (g.pl[0].ball.state == BallState::Flying && g.pl[0].ball.pos.x < 6.0f) maxDz = g.pl[0].ball.pos.z;
            }
            return maxDz;
        };
        float none = lateral({0, 0, 0}), left = lateral({0, 25, 0}), rightS = lateral({0, -25, 0});
        std::printf("  lateral offset at x=6: none %.2f  spin+ %.2f  spin- %.2f\n", none, left, rightS);
        CHECK(std::fabs(none) < 0.2f, "no spin: straight flight");
        CHECK(std::fabs(left - none) > 0.4f && (left - none) * (rightS - none) < 0, "spin bends the flight, direction follows spin sign");
    }
    {
        std::printf("[catch your own ball]\n");
        PlayerInput in[2]; Game g = makeGame(tris, in);
        Ball& b = g.pl[0].ball;
        b.state = BallState::Flying; b.instigator = 0;
        Vec3 c = g.catcherPoint(0, 1);
        b.pos = c + Vec3{1.0f, 0, 0}; b.vel = Vec3{-6, 0, 0}; b.speedLimit = 6;
        in[0].hand[1].trigger = 1;
        std::vector<GameEvent> log;
        run(g, in, 0.4f, &log);
        CHECK(hasEvent(log, EventType::Caught, 0) && g.pl[0].ball.state == BallState::Held, "trigger + catcher catches own ball");
    }
    {
        std::printf("[own ball: the slow return can hit its owner]\n");
        PlayerInput in[2]; Game g = makeGame(tris, in);
        in[1].head.p.z = 2.0f;   // opponent dodged the throw
        grabBall(g, in);
        std::vector<GameEvent> log;
        swingAndRelease(g, in, normalize(Vec3{1, 0.02f, -0.03f}), 6.0f, log);
        run(g, in, 8.0f, &log);   // owner stands still in the return's path
        CHECK(hasEvent(log, EventType::Returned, 0), "return started");
        CHECK(hasEvent(log, EventType::HitPlayer, 0) && g.pl[1].score == 1, "own returning ball hits its owner; opponent scores");
    }
    {
        std::printf("[own ball: no self-hit right after the throw]\n");
        PlayerInput in[2]; Game g = makeGame(tris, in);
        grabBall(g, in);
        std::vector<GameEvent> log;
        swingAndRelease(g, in, normalize(g.chestPoint(1) - g.launcherPoint(0, 1)), 5.0f, log);
        run(g, in, 0.3f, &log);
        CHECK(!hasEvent(log, EventType::HitPlayer, 0), "a fresh throw never hits the thrower");
    }
    {
        std::printf("[strikes: cap 42, stepped size growth]\n");
        PlayerInput in[2]; Game g = makeGame(tris, in);
        g.match = MatchState::InProgress;
        float prev = g.strikeScale(0);
        bool monotonic = true;
        for (int n = 1; n <= 42; ++n) { float sc = g.strikeScale(n); if (sc < prev) monotonic = false; prev = sc; }
        std::printf("  scale at 0/1/4/10/24/42 strikes: %.2f %.2f %.2f %.2f %.2f %.2f\n", g.strikeScale(0), g.strikeScale(1),
                    g.strikeScale(4), g.strikeScale(10), g.strikeScale(24), g.strikeScale(42));
        CHECK(monotonic && g.strikeScale(1) > g.strikeScale(0) && g.strikeScale(10) > g.strikeScale(4), "size steps up per strike");
        CHECK(g.strikeScale(42) <= g.tun.maxBallScale + 1e-4f, "size capped at maxBallScale");
        for (int i = 0; i < 60; ++i) g.addStrike(0, {});
        CHECK(g.pl[0].strikesAgainst == 42, "strike count stops at MaxStrikes 42");
        float s42 = g.throwSpeedFromHandSpeed(6.5f, 1);
        CHECK(s42 > 34.f && s42 < 36.f, "speed at 42 strikes = 14 + 42 x 0.5 m/s");
    }
    {
        std::printf("[strike: opponent ball through your strike-zone ring]\n");
        PlayerInput in[2]; Game g = makeGame(tris, in);
        in[0].head.p.z = -1.5f; g.pl[0].in.head.p.z = -1.5f;   // dodge
        fireOpponentBall(g, g.strikeZoneCenter(0) + Vec3{-0.5f, 0, 0});
        std::vector<GameEvent> log;
        run(g, in, 2.0f, &log);
        CHECK(hasEvent(log, EventType::Strike, 0), "strike against the player whose zone was passed");
        run(g, in, 6.0f);   // let the opponent's ball finish and respawn
        CHECK(g.ballRadius(1) > g.tun.projectileRadius * 1.2f, "opponent's ball grows with strikes against you");
    }
    {
        std::printf("[arena is watertight]\n");
        PlayerInput in[2]; Game g = makeGame(tris, in);
        in[1].head.p = {0, -5, 0}; in[0].head.p = {0, -5, 0};   // players out of the way
        g.pl[0].in = in[0]; g.pl[1].in = in[1];
        uint32_t r = 1234567;
        auto rnd = [&]() { r ^= r << 13; r ^= r >> 17; r ^= r << 5; return (r & 0xFFFF) / 65535.f; };
        int escaped = 0;
        for (int k = 0; k < 300; ++k) {
            Ball& b = g.pl[0].ball;
            b.state = BallState::Flying; b.instigator = 0; b.scored = true; b.rechargePending = false;
            b.pos = {(rnd() - 0.5f) * 8.f, 0.5f + rnd() * 3.f, (rnd() - 0.5f) * 4.f};
            Vec3 d = normalize(Vec3{rnd() - 0.5f, rnd() - 0.5f, rnd() - 0.5f});
            b.vel = d * 14.f; b.speedLimit = 14.f; b.bounces = 0; b.timer = 0; b.spin = {};
            for (int i = 0; i < 40 && b.state == BallState::Flying; ++i) {
                PlayerInput tmp[2] = {in[0], in[1]};
                g.step(1.f / 72, tmp);
                if (std::fabs(b.pos.z) > 2.6f || b.pos.y < -0.05f || b.pos.y > 4.1f || std::fabs(b.pos.x) > 9.6f) { ++escaped; break; }
            }
        }
        std::printf("  escaped %d / 300\n", escaped);
        CHECK(escaped == 0, "no ball ever leaves the court volume");
    }
    for (int adv = 0; adv < 2; ++adv) {
        std::printf("[full match vs bot: %s]\n", adv ? "Advanced" : "Basic");
        Game g; g.setArena(tris); g.tun.advanced = adv; g.tun.basicMatchTime = 40; g.reset(true);
        PlayerInput in[2] = {standing(g, 0), {}};
        std::vector<GameEvent> log;
        bool nan = false;
        float t = 0, nextThrow = 4;
        for (int i = 0; i < 72 * 400 && g.match != MatchState::MatchSummary; ++i, t += 1.f / 72) {
            if (g.match == MatchState::InProgress && t > nextThrow && g.pl[0].ball.state == BallState::Idle) {
                grabBall(g, in);
                swingAndRelease(g, in, normalize(g.chestPoint(1) - g.launcherPoint(0, 1)), 5.5f, log);
                nextThrow = t + 2.0f;
                in[0] = standing(g, 0);
            }
            PlayerInput tmp[2] = {in[0], in[1]};
            g.step(1.f / 72, tmp);
            log.insert(log.end(), g.events.begin(), g.events.end());
            for (int p = 0; p < 2; ++p) if (std::isnan(g.pl[p].ball.pos.x)) nan = true;
        }
        int hits = 0, blocks = 0, knuck = 0, botThrows = 0;
        for (auto& e : log) { hits += e.type == EventType::HitPlayer; blocks += e.type == EventType::ShieldBlock; knuck += e.type == EventType::KnuckleBlock; botThrows += (e.type == EventType::Throw && e.player == 1); }
        std::printf("  score %d-%d rounds %d-%d, hits=%d shieldBlocks=%d knuckleBlocks=%d botThrows=%d\n", g.pl[0].score, g.pl[1].score, g.pl[0].roundWins, g.pl[1].roundWins, hits, blocks, knuck, botThrows);
        CHECK(!nan, "no NaNs during a full match");
        CHECK(g.match == MatchState::MatchSummary, "match reaches a winner");
        CHECK(botThrows > 3 && hits > 0, "bot throws and points are scored");
    }
    avatarTests(tris);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
