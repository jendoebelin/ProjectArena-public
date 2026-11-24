// Desktop tests for the Project Arena gameplay simulation, run against the real baked arena collision.
// Build/run: tools/run_tests.bat
#define _CRT_SECURE_NO_WARNINGS
#include "../src/game.h"
#include "../src/avatar.h"
#include "../src/record.h"
#include "../src/training.h"
#include "../src/controls.h"
#include "../src/flight_sound.h"
#include <chrono>
#include <functional>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include <chrono>

static int g_fail = 0, g_pass = 0;
#define CHECK(c, msg) do { if (c) { ++g_pass; std::printf("  ok   %s\n", msg); } else { ++g_fail; std::printf("  FAIL %s  (%s:%d)\n", msg, __FILE__, __LINE__); } } while (0)

static std::vector<CollisionTri> loadCol(const char* path) {
    std::vector<CollisionTri> out;
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::printf("cannot open %s\n", path); std::exit(2); }
    std::vector<uint8_t> bytes;
    uint8_t buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
    out = parseCollision(bytes.data(), bytes.size());
    if (out.empty()) { std::printf("%s is not an arena.col\n", path); std::exit(2); }
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

// one step with a copy of the inputs: g.np of them (`in` holds that many: two, or four in a game for four; the game
// reads and writes back one per player, so a two-entry copy for a four-player game ran past its end)
static void stepCopy(Game& g, const PlayerInput* in) {
    PlayerInput tmp[kMaxPlayers];
    for (int p = 0; p < g.np; ++p) tmp[p] = in[p];
    g.step(1.f / 72, tmp);
}

static void run(Game& g, PlayerInput* in, float seconds, std::vector<GameEvent>* log = nullptr) {
    for (float t = 0; t < seconds; t += 1.f / 72) {
        stepCopy(g, in);
        if (log) log->insert(log->end(), g.events.begin(), g.events.end());
    }
}

static void grabBall(Game& g, PlayerInput* in, int h = 1) {
    Vec3 off = in[0].hand[h].grip.q.rotate(g.tun.launcherOffset);
    in[0].hand[h].grip.p = g.pl[0].ball.pos - off;
    in[0].hand[h].trigger = 1;
    stepCopy(g, in);
}

static void swingAndRelease(Game& g, PlayerInput* in, Vec3 dir, float speed, std::vector<GameEvent>& log, int h = 1) {
    for (int i = 0; i < 15; ++i) {
        in[0].hand[h].grip.p += dir * (speed / 72);
        stepCopy(g, in);
        log.insert(log.end(), g.events.begin(), g.events.end());
    }
    in[0].hand[h].trigger = 0;
    stepCopy(g, in);
    log.insert(log.end(), g.events.begin(), g.events.end());
}

// a passive human opponent standing at the far end
static Game makeGame(const std::vector<CollisionTri>& tris, PlayerInput in[2], bool advanced = false) {
    Game g; g.setArena(tris); g.tun.advanced = advanced; g.tun.meetTime = 0; g.reset(false);   // no centre meeting
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
            const Vec3 fe = avatar::rigTuning().forearmEnd;   // plate gauntlet wrist, inside the (tuned) cuff
            Vec3 w = in.hand[k].grip.transform(avatar::cuffPose(k == 0).transform({k == 1 ? fe.x : -fe.x, fe.y, fe.z}));
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
        {   // developer tuning (SETTINGS > TUNING): the rig's values change it, the defaults are the shipped rig
            avatar::RigTuning& T = avatar::rigTuning();
            const avatar::RigTuning keep = T;
            auto mx = [](Vec3 v) { v.x = -v.x; return v; };
            T.cuffRot = {10, -15, 20};
            T.cuffPos = {0.004f, 0.01f, -0.006f};
            const Pose cr = avatar::cuffPose(false), cl = avatar::cuffPose(true);
            CHECK(length(cr.transform(avatar::kCuffPivot) - (avatar::kCuffPivot + T.cuffPos)) < 1e-6f, "the cuff turns about the wrist pivot");
            const Vec3 v{0.03f, -0.02f, 0.09f};
            CHECK(length(cl.transform(mx(v)) - mx(cr.transform(v))) < 1e-6f, "the left cuff is the right one mirrored");
            avatar::Rig r3 = avatar::solveRig(in, d);
            const Vec3 w3 = in.hand[1].grip.transform(cr.transform(T.forearmEnd));
            CHECK(length(r3.fore[1].b - w3) < 1e-4f, "a moved cuff carries the forearm's end with it");
            T = keep;
            T.upperLen = 1.15f;
            in.hand[1].grip.p = in.head.p + Vec3{0.25f, -0.55f, 0.25f};   // well within reach
            avatar::Rig r4 = avatar::solveRig(in, d);
            const avatar::BodyBind& K = avatar::bodyBind(0);
            CHECK(std::fabs(length(r4.upper[1].b - r4.upper[1].a) - 1.15f * length(K.elbow[1] - K.shoulder[1])) < 2e-3f,
                  "UPPER ARM LENGTH scales the upper arm");
            T = keep;
        }
    }
    {
        std::printf("[tuning: the shield's back-of-hand pose]\n");
        Game g;
        g.tun.shieldPlacement = 1;
        const Pose s1 = g.shieldLocal(1), s0 = g.shieldLocal(0);
        // the owner's export (2026-10-19): pitch -20, yaw 19.1, roll -90.6 = yaw * pitch * roll about the grip axes
        const Quat qr = Quat::axisAngle({0, 1, 0}, deg2rad(19.1f)) * Quat::axisAngle({1, 0, 0}, deg2rad(-20)) * Quat::axisAngle({0, 0, 1}, deg2rad(-90.6f));
        auto same = [](const Quat& a, const Quat& b) { return std::fabs(std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) - 1) < 1e-5f; };
        CHECK(same(s1.q, qr) && length(s1.p - Vec3{0.076f, -0.024f, -0.122f}) < 1e-6f, "default right shield: the owner's tuned pose");
        auto mx0 = [](Vec3 v) { v.x = -v.x; return v; };
        const Vec3 u{0.1f, 0.2f, -0.3f};
        CHECK(length(s0.q.rotate(mx0(u)) - mx0(s1.q.rotate(u))) < 1e-5f && length(s0.p - mx0(s1.p)) < 1e-6f, "default left shield: its mirror image");
        g.tun.shieldBackRot = {12, -30, -75};
        const Pose t1 = g.shieldLocal(1), t0 = g.shieldLocal(0);
        auto mx = [](Vec3 v) { v.x = -v.x; return v; };
        const Vec3 v{0.1f, 0.2f, -0.3f};
        CHECK(length(t0.q.rotate(mx(v)) - mx(t1.q.rotate(v))) < 1e-5f, "a tuned shield stays mirrored between the hands");
    }
    {
        std::printf("[avatar: FACE shares the body byte]\n");
        AvatarDesc d;
        avatar::set(d, avatar::F_BODY, 1);
        avatar::set(d, avatar::F_FACE, 2);
        CHECK(avatar::get(d, avatar::F_BODY) == 1 && avatar::get(d, avatar::F_FACE) == 2, "body type and face read back separately");
        CHECK(d.body % 2 == 1, "an older build (body % 2) still sees the right body type");
        avatar::set(d, avatar::F_BODY, 0);
        CHECK(avatar::get(d, avatar::F_FACE) == 2, "changing the body type keeps the face");
        AvatarDesc c;
        CHECK(avatar::fromText(avatar::toText(d), c) && c.body == d.body, "avatar.cfg keeps the face");
        AvatarDesc bad = d;
        bad.body = 0xFF;   // from the network: a face out of range
        avatar::sanitize(bad);
        CHECK(avatar::get(bad, avatar::F_FACE) < avatar::kFaces && avatar::get(bad, avatar::F_BODY) == 1, "a bad face from the network is repaired");
    }
    {
        std::printf("[avatar: NATION in spare bits, the team kit, the ends' colours]\n");
        CHECK(avatar::nationCount() >= 31 && avatar::nationCount() <= 255 && !avatar::nation(0) && avatar::nation(avatar::nationCount()) &&
              !avatar::nation(avatar::nationCount() + 1), "the country table: 1..count, 0 = none");
        AvatarDesc d;
        avatar::set(d, avatar::F_BODY, 1);
        avatar::set(d, avatar::F_FACE, 3);
        avatar::set(d, avatar::F_GAUNTLET, 2);
        avatar::set(d, avatar::F_NATION, 7);
        CHECK(avatar::get(d, avatar::F_NATION) == 7 && avatar::get(d, avatar::F_BODY) == 1 && avatar::get(d, avatar::F_FACE) == 3 &&
              avatar::get(d, avatar::F_GAUNTLET) == 2, "country, body type, face and gauntlet read back separately");
        avatar::set(d, avatar::F_FACE, 1);
        avatar::set(d, avatar::F_GAUNTLET, 0);
        CHECK(avatar::nationOf(d) == 7, "changing the face or the gauntlet keeps the country");
        const int big = std::min(45, avatar::nationCount());
        avatar::set(d, avatar::F_NATION, big);
        CHECK(avatar::nationOf(d) == big && avatar::gauntletOf(d) == 0 && avatar::faceOf(d) == 1, "a country above 31 uses the gauntlet's spare bits");
        AvatarDesc c;
        CHECK(avatar::fromText(avatar::toText(d), c) && avatar::nationOf(c) == big, "avatar.cfg keeps the country");
        {   // the builder's COUNTRY order (0.3.23): your most chosen first, then A-Z ("THE " left out)
            const int N = avatar::nationCount();
            auto name = [](int n) { const char* s = avatar::nation(n)->name; return std::strncmp(s, "THE ", 4) == 0 ? s + 4 : s; };
            std::vector<int> o = avatar::nationOrder(nullptr);
            std::vector<bool> seen(N + 1, false);
            bool once = (int)o.size() == N, az = true;
            for (int i = 0; i < (int)o.size(); ++i) {
                if (o[i] < 1 || o[i] > N || seen[o[i]]) once = false; else seen[o[i]] = true;
                if (i && std::strcmp(name(o[i - 1]), name(o[i])) > 0) az = false;
            }
            CHECK(once && az, "COUNTRY, nothing chosen yet: every country once, A-Z");
            std::vector<int> picks(N + 1, 0);
            picks[1] = 2;                    // UNITED STATES twice
            picks[std::min(45, N)] = 5;      // another five times
            picks[2] = 2;                    // CANADA twice (a tie with the United States: A-Z between them)
            o = avatar::nationOrder(picks.data());
            CHECK((int)o.size() == N && o[0] == std::min(45, N) && o[1] == 2 && o[2] == 1, "COUNTRY: the most chosen first, equal counts A-Z");
            bool restAz = true;
            for (int i = 4; i < N; ++i) if (std::strcmp(name(o[i - 1]), name(o[i])) > 0) restAz = false;
            CHECK(restAz, "COUNTRY: the ones never chosen after them, A-Z");
            int glx = 0;
            for (int n = 1; n <= N; ++n) if (std::strcmp(avatar::nation(n)->name, "THE GALAXY") == 0) glx = n;
            if (glx) {
                o = avatar::nationOrder(nullptr);
                const int at = (int)(std::find(o.begin(), o.end(), glx) - o.begin());
                CHECK(at > 0 && at + 1 < N && std::strcmp(name(o[at - 1]), "GALAXY") < 0 && std::strcmp(name(o[at + 1]), "GALAXY") > 0,
                      "COUNTRY: THE GALAXY sorted under G");
            }
            AvatarDesc look = avatar::preset(3), back;   // MY LOOKS: a slot is avatar.cfg's lines (looks.cfg)
            avatar::setNation(look, std::min(45, N));
            avatar::setOwnKit(look, true);
            CHECK(avatar::fromText(avatar::toText(look), back) && std::memcmp(&back, &look, sizeof look) == 0, "MY LOOKS: a saved look reads back the same");
        }
        AvatarDesc s = d;
        avatar::sanitize(s);
        CHECK(avatar::nationOf(s) == big && avatar::gauntletOf(s) == 0, "sanitize (network) keeps a valid country");
        // what 0.3.5 and older make of it (their sanitize): the body type stays; FACE 1; the first gauntlet over 31
        auto oldSanitize = [](AvatarDesc o) {
            if (o.gauntlet >= 4) o.gauntlet = 0;
            o.body = (uint8_t)((o.body & 1) | ((o.body >> 1) < 4 ? (o.body & ~1) : 0));
            return o;
        };
        AvatarDesc small = d;
        avatar::set(small, avatar::F_GAUNTLET, 2);
        avatar::set(small, avatar::F_NATION, 7);
        const AvatarDesc o1 = oldSanitize(small), o2 = oldSanitize(d);
        CHECK((o1.body & 1) == 1 && o1.gauntlet == 2, "an older build: the body type and (countries 1-31) the gauntlet stay");
        CHECK((o2.body & 1) == 1 && o2.gauntlet < 4, "an older build: a country above 31 still gives a valid gauntlet");
        const AvatarDesc u = avatar::uniform(small);
        CHECK(u.suit == 1 && u.pattern == 0 && u.lower == 1 && avatar::nationOf(u) == 7, "a country wears the team kit (jersey, shorts)");
        AvatarDesc plain = avatar::preset(3);
        const AvatarDesc up = avatar::uniform(plain);
        CHECK(up.suit == plain.suit && up.pattern == plain.pattern && up.primary == plain.primary, "no country: the look is unchanged");
        // COUNTRY KIT (0.3.22): TEAM JERSEY as before; MY OUTFIT keeps your suit, pattern and lower body in the colours
        CHECK(!avatar::ownKit(small) && avatar::sentLook(small).suit == 1 && avatar::sentLook(small).pattern == 0,
              "a country's kit is the TEAM JERSEY unless chosen");
        AvatarDesc none = plain;
        avatar::setOwnKit(none, true);
        CHECK(!avatar::ownKit(none) && none.pattern == plain.pattern, "MY OUTFIT needs a country");
        AvatarDesc mine = avatar::preset(2);   // VANGUARD (its own pattern), waist up
        avatar::set(mine, avatar::F_NATION, 7);
        avatar::setOwnKit(mine, true);
        CHECK(avatar::ownKit(mine) && avatar::get(mine, avatar::F_PATTERN) == avatar::preset(2).pattern, "MY OUTFIT: the pattern row reads as before");
        avatar::set(mine, avatar::F_PATTERN, 1);
        CHECK(avatar::ownKit(mine) && avatar::get(mine, avatar::F_PATTERN) == 1, "changing the pattern keeps MY OUTFIT");
        avatar::set(mine, avatar::F_PATTERN, avatar::preset(2).pattern);
        const AvatarDesc mu = avatar::uniform(mine);
        CHECK(mu.suit == 2 && mu.pattern == avatar::preset(2).pattern && mu.lower == 0 && avatar::nationOf(mu) == 7,
              "MY OUTFIT is drawn in your own suit, pattern and lower body");
        CHECK(mu.primary == u.primary && mu.secondary == u.secondary && mu.glow == u.glow, "... in the country's colours");
        AvatarDesc mt;
        CHECK(avatar::fromText(avatar::toText(mine), mt) && avatar::ownKit(mt) && mt.suit == 2, "avatar.cfg keeps MY OUTFIT");
        std::string jersey = avatar::toText(mine);
        jersey.erase(jersey.find("kit=1\n"), 6);
        CHECK(avatar::fromText(jersey, mt) && !avatar::ownKit(mt), "an avatar.cfg without it: the TEAM JERSEY");
        AvatarDesc ms = mine;
        avatar::sanitize(ms);
        CHECK(avatar::ownKit(ms) && ms.pattern == mine.pattern, "sanitize (network) keeps MY OUTFIT");
        avatar::setNation(ms, 0);
        avatar::sanitize(ms);
        CHECK(!avatar::ownKit(ms) && ms.pattern == avatar::preset(2).pattern, "... and clears it without a country");
        const AvatarDesc sent = avatar::sentLook(mine);
        AvatarDesc back = sent;
        avatar::sanitize(back);
        CHECK(avatar::ownKit(back) && avatar::uniform(back).suit == 2, "sent: 0.3.22 shows MY OUTFIT");
        AvatarDesc older = sent;   // what 0.3.21 and older make of it: an unknown pattern (cleared), then the team jersey
        if (older.pattern >= avatar::fieldCount(avatar::F_PATTERN)) older.pattern = 0;
        const AvatarDesc ou = avatar::uniform(older);
        CHECK(sent.pattern >= avatar::fieldCount(avatar::F_PATTERN) && ou.suit == 1 && ou.pattern == 0 && avatar::nationOf(ou) == 7,
              "sent: older builds show the team jersey");
        float chm[avatar::CH_COUNT][4], chj[avatar::CH_COUNT][4];
        const float tw[3] = {1, 1, 1};
        avatar::channelColors(mine, tw, chm);
        avatar::channelColors(small, tw, chj);
        CHECK(std::fabs(chm[avatar::CH_PRIMARY][0] - chj[avatar::CH_PRIMARY][0]) < 1e-6f &&
              std::fabs(chm[avatar::CH_GLOW][1] - chj[avatar::CH_GLOW][1]) < 1e-6f, "MY OUTFIT takes the kit's exact colours");
        float ch[avatar::CH_COUNT][4];
        const float team[3] = {1, 1, 1};
        avatar::channelColors(small, team, ch);
        const avatar::Nation* N = avatar::nation(7);
        CHECK(std::fabs(ch[avatar::CH_PRIMARY][2] - std::pow(N->kit[0][2] / 255.f, 2.2f)) < 1e-4f, "the kit is in the flag's own colours");
        float sc[2][3];
        avatar::sideColors(0, 0, sc);
        CHECK(sc[0][0] == 0.1f && sc[0][2] == 1.0f && sc[1][0] == 1.0f && sc[1][2] == 0.1f, "no countries: the blue and orange ends");
        int fra = 0, usa = 0;
        for (int n = 1; n <= avatar::nationCount(); ++n) {
            if (!std::strcmp(avatar::nation(n)->code, "FRA")) fra = n;
            if (!std::strcmp(avatar::nation(n)->code, "USA")) usa = n;
        }
        avatar::sideColors(0, fra, sc);
        CHECK(sc[1][0] > sc[1][2], "FRANCE against the blue end takes its red");
        avatar::sideColors(usa, usa, sc);
        auto apart = [&]() { float s2 = 0; for (int k = 0; k < 3; ++k) s2 += (sc[0][k] - sc[1][k]) * (sc[0][k] - sc[1][k]); return std::sqrt(s2); };
        CHECK(apart() > 0.5f, "the same country at both ends: two different colours");
    }
    {
        std::printf("[avatar: hair under headgear, swinging parts]\n");
        PlayerInput in;
        in.headTracked = true;
        in.head = {Quat{}, {0, 1.6f, 0}};
        auto hairParts = [&](int hair, int headgear) {
            AvatarDesc d;
            d.hair = (uint8_t)hair;
            d.headgear = (uint8_t)headgear;
            std::vector<avatar::PartDraw> list;
            avatar::drawList(avatar::solveRig(in, d), in, d, list);
            std::string names;
            for (const avatar::PartDraw& p : list)
                if (p.flags & avatar::PART_HAIR) names += std::string(p.model) + " ";
            return names;
        };
        bool under = true;
        for (int g = 0; g < avatar::fieldCount(avatar::F_HEADGEAR); ++g)
            if (hairParts(6, g) != "av_hair_6_m ") under = false;
        CHECK(under, "the chosen hair is drawn under every headgear (the game tucks it in), the cap too");
        CHECK(hairParts(3, 2) == "av_hair_3_m av_hair_3t_m ", "a ponytail under a helmet keeps its tail");
        CHECK(hairParts(7, 0) == "av_hair_7_m av_hair_7t_m ", "the top knot's loose ends are a part of their own");
        CHECK(hairParts(9, 2) == "av_hair_9_m av_hair_9t_m ", "the high pigtails' tails are a part of their own, under a helmet too");
        CHECK(hairParts(10, 0) == "av_hair_10_m ", "the dreadlocks are one part (their locs swing by region)");
        CHECK(hairParts(0, 1).empty(), "no hair: nothing drawn under the helmet");
        bool tails = true;
        for (int face = 0; face < avatar::kFaces; ++face) {   // the BANDANA's tails: one part per body, for every face
            AvatarDesc d;
            d.mask = 4;
            d.body = (uint8_t)(face << 1);   // (masculine; the face in bits 1-2)
            std::vector<avatar::PartDraw> list;
            avatar::drawList(avatar::solveRig(in, d), in, d, list);
            int n = 0;
            for (const avatar::PartDraw& p : list)
                if (!std::strcmp(p.model, "av_mask_4t_m")) n += (p.flags & avatar::PART_HAIR) ? 1 : 100;
                else if (!std::strncmp(p.model, "av_mask_4_m", 11)) n += (p.flags & avatar::PART_HAIR) ? 10 : 100;   // (its point swings)
            tails &= n == 11;
        }
        CHECK(tails, "the bandana's tails are a swinging part of their own, and its point swings too, on every face");
        bool grapples = avatar::fieldCount(avatar::F_GAUNTLET) == avatar::kGauntletStyles;
        for (int k = 4; k < avatar::kGauntletStyles; ++k) {   // GRAPPLER, ARC CLAW, REGALIA: kept, and drawn as plate gauntlets
            AvatarDesc d;
            d.gauntlet = (uint8_t)k;
            avatar::sanitize(d);
            grapples &= avatar::gauntletOf(d) == k && avatar::plateGauntlet(k) && avatar::grappleGauntlet(k);
        }
        grapples &= !avatar::plateGauntlet(3) && !avatar::grappleGauntlet(2);   // (the CONTROLLER is no plate gauntlet)
        CHECK(grapples, "the grapple gauntlets are GAUNTLET 4-6, plate gauntlets that open");
        {   // RONIN WRAP (PR #27, 0.3.22): suit 16, its own top over the usual shorts (none of its own, unlike 14 and 15)
            AvatarDesc d;
            d.suit = 16;
            d.lower = 1;
            avatar::sanitize(d);
            std::vector<avatar::PartDraw> list;
            avatar::drawList(avatar::solveRig(in, d), in, d, list);
            bool top = false, shorts = false, own = false;
            for (const avatar::PartDraw& p : list) {
                top |= !std::strcmp(p.model, "av_top_16_m");
                shorts |= !std::strcmp(p.model, "av_bottom_1_m");
                own |= !std::strncmp(p.model, "av_sbottom_", 11);
            }
            CHECK(avatar::fieldCount(avatar::F_SUIT) > 16 && !std::strcmp(avatar::optionName(avatar::F_SUIT, 16), "RONIN WRAP") &&
                      d.suit == 16, "RONIN WRAP is suit 16 (games before 0.3.22 read it as GRID RUNNER)");
            CHECK(top && shorts && !own, "RONIN WRAP: its own top over the usual shorts");
        }
        {   // RONIN KABUTO (PR #27, 0.3.22): headgear 9, a helmet drawn on the head like the others, on both bodies
            bool drawn = true;
            for (int body = 0; body < 2; ++body) {
                AvatarDesc d;
                d.headgear = 9;
                d.body = (uint8_t)body;
                avatar::sanitize(d);
                std::vector<avatar::PartDraw> list;
                avatar::drawList(avatar::solveRig(in, d), in, d, list);
                int n = 0;
                for (const avatar::PartDraw& p : list) n += !std::strcmp(p.model, body ? "av_gear_9_f" : "av_gear_9_m");
                drawn &= d.headgear == 9 && n == 1;
            }
            CHECK(avatar::fieldCount(avatar::F_HEADGEAR) > 9 && !std::strcmp(avatar::optionName(avatar::F_HEADGEAR, 9), "RONIN KABUTO") &&
                      drawn, "RONIN KABUTO is headgear 9, drawn on both bodies (games before 0.3.22 read it as NONE)");
        }
        {   // PELURU (PR #30, 0.3.22): SUIT 17 TUXEDO and 18 LAYERED DRESS, HEADGEAR 10 SOMBRERO, MASK 6 SUGAR SKULL
            CHECK(avatar::fieldCount(avatar::F_SUIT) == 19 && !std::strcmp(avatar::optionName(avatar::F_SUIT, 17), "PELURU TUXEDO") &&
                      !std::strcmp(avatar::optionName(avatar::F_SUIT, 18), "PELURU LAYERED DRESS") &&
                      avatar::fieldCount(avatar::F_HEADGEAR) == 11 && !std::strcmp(avatar::optionName(avatar::F_HEADGEAR, 10), "SOMBRERO") &&
                      avatar::fieldCount(avatar::F_MASK) == 7 && !std::strcmp(avatar::optionName(avatar::F_MASK, 6), "SUGAR SKULL"),
                  "PELURU: suits 17 and 18, headgear 10 SOMBRERO, mask 6 SUGAR SKULL (games before 0.3.22: GRID RUNNER, NONE, NONE)");
            auto has = [](const std::vector<avatar::PartDraw>& list, const char* prefix, uint8_t flags = 0) {
                int n = 0;
                for (const avatar::PartDraw& p : list)
                    if (!std::strncmp(p.model, prefix, std::strlen(prefix)) && (p.flags & flags) == flags) ++n;
                return n;
            };
            bool dress = true, tux = true, worn = true;
            for (int body = 0; body < 2; ++body) {
                const char bk = body ? 'f' : 'm';
                char top[24], ears[24], shorts[24];
                std::snprintf(top, sizeof top, "av_top_18_%c", bk);
                std::snprintf(ears, sizeof ears, "av_top_18e_%c", bk);
                std::snprintf(shorts, sizeof shorts, "av_bottom_1_%c", bk);
                AvatarDesc d;
                d.suit = 18; d.lower = 1; d.body = (uint8_t)body;
                avatar::sanitize(d);
                std::vector<avatar::PartDraw> list;
                avatar::drawList(avatar::solveRig(in, d), in, d, list);
                // the dress: its own skirt in the LOWER BODY's place (no shorts, no waist fade), it sways, its earrings swing
                dress &= d.suit == 18 && has(list, top) == 1 && !has(list, "av_bottom_") && !has(list, "av_wfade_") &&
                         has(list, ears, avatar::PART_HAIR) == 1 && avatar::swishKind(d) == 0;
                d.suit = 17;
                list.clear();
                avatar::drawList(avatar::solveRig(in, d), in, d, list);
                std::snprintf(top, sizeof top, "av_top_17_%c", bk);
                tux &= has(list, top) == 1 && has(list, shorts) == 1 && !has(list, ears) && avatar::swishKind(d) == -1;
                for (int face = 0; face < avatar::kFaces; ++face) {   // the hat on the body, the mask fitted to each face
                    d.headgear = 10; d.mask = 6;
                    d.body = (uint8_t)(body | (face << 1));
                    list.clear();
                    avatar::drawList(avatar::solveRig(in, d), in, d, list);
                    char gear[24], mask[24];
                    std::snprintf(gear, sizeof gear, "av_gear_10_%c", bk);
                    if (face) std::snprintf(mask, sizeof mask, "av_mask_6_%c%d", bk, face);
                    else std::snprintf(mask, sizeof mask, "av_mask_6_%c", bk);
                    int m = 0;
                    for (const avatar::PartDraw& p : list) m += !std::strcmp(p.model, mask);
                    worn &= has(list, gear) == 1 && m == 1;
                }
            }
            CHECK(dress, "PELURU LAYERED DRESS: its skirt takes the LOWER BODY's place and sways; its earrings swing as hair");
            CHECK(tux, "PELURU TUXEDO: over the LOWER BODY, no earrings");
            CHECK(worn, "the SOMBRERO on both bodies, the SUGAR SKULL fitted to every face");
        }
        {   // the full sets as presets (the owner, 2026-11-19), after the fourteen there were (bots and pictures use those)
            struct Set { const char* name; int suit, headgear, mask; };
            const Set sets[] = {{"RONIN", 16, 9, 4}, {"PELURU TUXEDO", 17, 10, 6}, {"PELURU DRESS", 18, 10, 6},
                                {"VECTOR", 14, 8, 0}, {"NIGHTFORGE", 15, 7, 0}};
            bool ok = avatar::presetCount() == 19 && !std::strcmp(avatar::presetName(13), "TRAINING KIT");
            for (int i = 0; i < 5; ++i) {
                AvatarDesc d = avatar::preset(14 + i), c = d;
                avatar::sanitize(c);
                ok &= !std::strcmp(avatar::presetName(14 + i), sets[i].name) && d.suit == sets[i].suit &&
                      d.headgear == sets[i].headgear && d.mask == sets[i].mask && std::memcmp(&c, &d, sizeof d) == 0;
            }
            CHECK(ok, "the full sets are presets: RONIN, PELURU TUXEDO, PELURU DRESS, VECTOR, NIGHTFORGE (each its own pieces)");
        }
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


// ---- the practice bot: a person's limits at every level, and UNBEATABLE
static PlayerInput standingAt(const Game& g, int p, float z) {   // standing() moved sideways along the end
    PlayerInput in = standing(g, p);
    in.head.p.z += z;
    for (HandInput& h : in.hand) h.grip.p.z += z;
    return in;
}
// a strong player against the bot (player 1): takes the ball as soon as it can and throws it at 14 m/s (the fastest
// throw) at the bot's head, chest, hips and sides, curved and off the side walls, and at its strike zone, from all
// over their end. Steps of 1/hz s; `seed` moves them around their end.
// points: the bot hit by the attacker's throws; rebounds: by a ball it had just blocked itself, bouncing back into it
// (the game counts that as the attacker's point; since 2026-10-27 only after the self-hit grace, as after a throw)
struct Attack { int throws = 0, points = 0, rebounds = 0, botPoints = 0, strikes = 0, blocks = 0, knuckles = 0; double slowest = 0; };
static Attack attackBot(Game& g, int wanted, float hz, uint32_t seed, rec::Recorder* recorder = nullptr) {
    Attack a;
    auto rnd = [&]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return (seed & 0xFFFF) / 65535.f; };
    PlayerInput in[2] = {standing(g, 0), {}};
    std::vector<GameEvent> log;
    float t = 0, next = 0;
    int lastK = -1;
    auto step = [&]() {
        PlayerInput tmp[2] = {in[0], in[1]};
        const auto t0 = std::chrono::steady_clock::now();
        g.step(1.f / hz, tmp);
        a.slowest = std::max(a.slowest, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        if (recorder) recorder->frame(g, 1.f / hz);
        log.insert(log.end(), g.events.begin(), g.events.end());
        for (const GameEvent& e : g.events)   // (whose ball last touched it: the bot's own block coming back)
            if (e.type == EventType::HitPlayer && e.player == 1 && e.ball == 0 && g.pl[0].ball.instigator == 1) ++a.rebounds;
        for (const GameEvent& e : g.events) {   // (a point against UNBEATABLE: what it was doing, to find out why)
            if (e.type != EventType::HitPlayer || e.player != 1 || g.tun.botAdapt || g.tun.botLevel != kBotLevels - 1) continue;
            const PlayerState& s = g.pl[1];
            std::printf("       hit by throw type %d (ball %.1f m/s, x%.2f); its reads: %d/%d, plans %d/%d; body (%.2f %.2f %.2f)\n",
                        lastK % 7, g.pl[0].ball.speedLimit, g.pl[0].ball.scale, s.botRead[0].mode, s.botRead[1].mode, s.botRead[0].plan,
                        s.botRead[1].plan, s.botBody.x, s.botBody.y, s.botBody.z);
            const PlayerState::BotRead& r = s.botRead[0];
            std::printf("         read: noticed %.2f s, read %.2f s ago; hitT %+.2f; tried %d, again %d, act %d; stance %d\n", g.now - r.noticeT,
                        g.now - r.readT, r.hitT < 0 ? -1.f : r.hitT - g.now, (int)r.tried.size(), (int)r.again, (int)r.act, s.botStance);
        }
    };
    for (; t < 1500.f && a.throws < wanted; t += 1.f / hz) {
        in[0].buttonA = g.match == MatchState::MatchSummary;   // (a match won: the next one)
        if (g.match == MatchState::InProgress && t > next && g.pl[0].ball.state == BallState::Idle) {
            in[0] = standing(g, 0);
            grabBall(g, in);
            const int k = lastK = a.throws++;
            Vec3 target = g.chestPoint(1);
            const float sgn = (k / 7) % 2 ? -1.f : 1.f;
            switch (k % 7) {
            case 0: break;
            case 1: target.y += 0.30f; break;                  // the head
            case 2: target.y -= 0.40f; break;                  // the hips
            case 3: target.z += 0.25f * sgn; break;            // a side
            case 4: target.z += 3.0f * sgn; break;             // off the side wall
            case 5: target.z -= 1.2f * sgn; in[0].hand[1].angVel = Vec3{0, 25.f * sgn, 0}; break;   // curved
            default: target = g.strikeZoneCenter(1) + Vec3{0, 0.3f * sgn, 0.45f * sgn}; break;     // its strike zone
            }
            const Vec3 dir = normalize(target - g.launcherPoint(0, 1));
            for (int f = 0; f < 16; ++f) {   // the swing (as swingAndRelease), then let go
                if (f < 15) in[0].hand[1].grip.p += dir * (6.5f / 72); else in[0].hand[1].trigger = 0;
                step();
            }
            in[0] = standingAt(g, 0, (rnd() - 0.5f) * 1.6f);   // and somewhere else in their end
            next = t + 0.6f;
        }
        step();
    }
    for (const GameEvent& e : log) {
        if (e.type == EventType::HitPlayer) ++(e.player == 1 ? a.points : a.botPoints);
        if (e.type == EventType::Strike && e.player == 1) ++a.strikes;
        if (e.type == EventType::ShieldBlock && e.player == 1) ++a.blocks;
        if (e.type == EventType::KnuckleBlock && e.player == 1) ++a.knuckles;
    }
    a.points -= a.rebounds;
    return a;
}

static void botTests(const std::vector<CollisionTri>& tris) {
    {
        std::printf("[practice bot: a person's limits at every level]\n");
        bool reach = true, rig = true, hands = true, body = true, finite = true;
        int used[2] = {0, 0}, catches = 0, dodges = 0;
        float worstHand = 0, worstBody = 0;
        const AvatarDesc look = avatar::preset(3);
        for (int level = 0; level < kBotLevels; ++level)
            for (int adv = 0; adv < 2; ++adv) {
                Game g; g.setArena(tris); g.tun.advanced = adv; g.tun.botLevel = level; g.reset(true);
                g.pl[0].isBot = true;
                const BotSkill& sk = botSkill(level);
                PlayerInput in[2] = {{}, {}};
                Vec3 lastGrip[2][2], lastBody[2];
                for (int i = 0; i < 72 * 60; ++i) {
                    PlayerInput tmp[2] = {in[0], in[1]};
                    g.step(1.f / 72, tmp);
                    for (const GameEvent& e : g.events) {
                        if ((e.type == EventType::Grab || e.type == EventType::Throw || e.type == EventType::ShieldBlock ||
                             e.type == EventType::KnuckleBlock || e.type == EventType::Caught) && e.hand >= 0 && e.hand < 2)
                            ++used[e.hand];
                        catches += e.type == EventType::Caught;
                    }
                    for (int p = 0; p < 2; ++p) {
                        const PlayerState& s = g.pl[p];
                        for (int k = 0; k < 2; ++k) dodges += s.botRead[k].plan == BOT_DODGE;
                        for (int h = 0; h < 2; ++h) {
                            const Vec3 gp = s.in.hand[h].grip.p;
                            finite = finite && std::isfinite(gp.x) && std::isfinite(gp.y) && std::isfinite(gp.z);
                            if (length(g.botWrist(h, g.handPose(p, h)) - g.botShoulder(p, h)) > g.tun.botArm + 1e-3f) reach = false;   // (the rules' hand: drawn arms below)
                            if (i > 0) {   // its own move, plus the body carrying the arm along
                                const float lim = std::max(sk.handSpeed, sk.throwHand) + sk.bodySpeed;
                                const float v = length(gp - lastGrip[p][h]) * 72;
                                worstHand = std::max(worstHand, v / lim);
                                if (v > lim + 0.05f) hands = false;
                            }
                            lastGrip[p][h] = gp;
                        }
                        if (i > 0) {
                            const float v = length(s.botBody - lastBody[p]) * 72;
                            worstBody = std::max(worstBody, v / sk.bodySpeed);
                            if (v > sk.bodySpeed + 0.01f) body = false;
                        }
                        lastBody[p] = s.botBody;
                        if (i % 9 == 0) {   // the drawn arms: both bones keep their length (never longer than the arm)
                            const avatar::Rig r = avatar::solveRig(s.in, look);
                            const avatar::BodyBind& K = avatar::bodyBind(look.body);
                            for (int k = 0; k < 2; ++k) {
                                const float l1 = length(K.elbow[k] - K.shoulder[k]) * avatar::rigTuning().upperLen;
                                const float l2 = length(K.wrist[k] - K.elbow[k]) * avatar::rigTuning().foreLen;
                                if (length(r.upper[k].b - r.upper[k].a) > l1 * 1.002f || length(r.fore[k].b - r.fore[k].a) > l2 * 1.002f) {
                                    if (rig) {
                                        const Vec3 gs = g.botShoulder(p, k), gp = s.in.hand[k].grip.p;
                                        const Quat iq = s.in.head.q.conj();
                                        const Vec3 rs = iq.rotate(r.upper[k].a - s.in.head.p), ms = iq.rotate(gs - s.in.head.p);
                                        const Vec3 tf = r.torso.q.rotate({0, 0, -1}), hf = s.in.head.q.rotate({0, 0, -1});
                                        const Vec3 wl = s.in.hand[k].grip.q.conj().rotate(r.fore[k].b - gp);
                                        std::printf("       stretched: level %d, %s arm, %.3f m shoulder to wrist (arm %.3f); grip %.3f from the game's shoulder; rig shoulder (%.3f %.3f %.3f) game (%.3f %.3f %.3f); wrist (%.3f %.3f %.3f) in the grip; torso turned %.0f deg; plans %d/%d\n",
                                                    level, k ? "right" : "left", length(r.fore[k].b - r.upper[k].a), l1 + l2, length(gp - gs),
                                                    rs.x, rs.y, rs.z, ms.x, ms.y, ms.z, wl.x, wl.y, wl.z,
                                                    rad2deg(std::acos(clampf(dot(tf, hf), -1.f, 1.f))), s.botRead[0].plan, s.botRead[1].plan);
                                    }
                                    rig = false;
                                }
                            }
                        }
                    }
                }
            }
        std::printf("       (hands used: left %d, right %d; catches %d; dodging %d frames; fastest hand %.0f%%, body %.0f%% of its limit)\n",
                    used[0], used[1], catches, dodges, worstHand * 100, worstBody * 100);
        CHECK(finite, "the bot's hands always have a position");
        CHECK(reach, "a hand never goes beyond its arm's reach");
        CHECK(rig, "the drawn arms never stretch");
        CHECK(hands, "its hands never move faster than a person's (at its level)");
        CHECK(body, "its body never steps, leans or ducks faster than a person (at its level)");
        CHECK(used[0] > 20 && used[1] > 20, "it grabs, throws, blocks and catches with both hands");
        CHECK(catches > 5, "it catches its own ball coming back");
        CHECK(dodges > 0, "it dodges");
    }
    {
        std::printf("[practice bot: levels]\n");
        int wins = 0;
        for (int adv = 0; adv < 2; ++adv) {
            Game g; g.setArena(tris); g.tun.advanced = adv; g.reset(true);
            g.pl[0].isBot = true;
            g.pl[0].botLevel = 0; g.pl[1].botLevel = kBotLevels - 1;
            PlayerInput in[2] = {{}, {}};
            std::vector<GameEvent> log;
            run(g, in, 180.f, &log);
            int pts[2] = {0, 0};
            for (const GameEvent& e : log) if (e.type == EventType::HitPlayer) ++pts[(int)e.value & 1];
            std::printf("       %s: ROOKIE %d - %d UNBEATABLE\n", adv ? "Advanced" : "Basic", pts[0], pts[1]);
            wins += pts[1] > pts[0];
        }
        CHECK(wins == 2, "UNBEATABLE beats ROOKIE");
    }
    {   // BOT IDLE MOTION (0.3.26; the owner, 2026-11-22: idle animations, taunts when someone stalls, victory poses)
        std::printf("[bot idle motion: at ease, a taunt, a fist pump, victory poses (0.3.26)]\n");
        Game g; g.setArena(tris); g.tun.basicMatchTime = 0; g.tun.meetTime = 0; g.tun.botLevel = 0; g.reset(true);
        PlayerInput in[2] = {standing(g, 0), {}};
        for (int i = 0; i < 72 * 20 && g.match != MatchState::InProgress; ++i) stepCopy(g, in);
        g.pl[1].botStance = STANCE_RELAXED;
        auto hold = [&](float secs, auto&& each) {   // (the bot kept from throwing: only its idle motion)
            for (int i = 0; i < (int)(secs * 72); ++i) { g.pl[1].botTimer = 99.f; stepCopy(g, in); each(); }
        };
        const Vec3 face = g.botFacing(1).rotate({0, 0, -1});
        float turned = 0, faceMoved = 0;
        hold(12.f, [&]() {
            const Vec3 f = g.pl[1].in.head.q.rotate({0, 0, -1}), r = g.botFacing(1).rotate({0, 0, -1});
            turned = std::max(turned, std::acos(clampf(dot(f, r), -1.f, 1.f)));
            faceMoved = std::max(faceMoved, length(r - face));
        });
        std::printf("       at ease: its head turned up to %.0f degrees from the way it faces (that moved %.4f)\n", rad2deg(turned), faceMoved);
        CHECK(turned > deg2rad(8.f) && turned < deg2rad(60.f) && faceMoved < 1e-4f,
              "at ease, its head glances about; the way it faces, as its rules have it, stays put");
        grabBall(g, in);   // (you hold your ball and don't throw)
        float taunt = 0, out = 0, early = 0;
        int frames = 0;
        hold(9.f, [&]() {
            ++frames;
            if (frames < 72 * 3) early = std::max(early, g.pl[1].botTaunt);
            taunt = std::max(taunt, g.pl[1].botTaunt);
            for (int h = 0; h < 2; ++h) out = std::max(out, std::fabs(g.pl[1].in.hand[h].grip.p.z - g.pl[1].in.head.p.z));
        });
        std::vector<GameEvent> log;
        swingAndRelease(g, in, normalize(g.chestPoint(1) - g.launcherPoint(0, 1)), 6.5f, log);
        float after = 1;
        hold(0.4f, [&]() { after = g.pl[1].botTaunt; });
        std::printf("       you hold your ball 9 s: its taunt %.2f (none in the first 3 s: %.2f), hands out %.2f m; thrown: %.2f\n", taunt, early, out, after);
        CHECK(early < 0.01f && taunt > 0.8f && out > 0.3f, "you hold your ball without throwing: after 4 s it taunts, its hands out of its guard");
        CHECK(after < 0.1f, "... and when the ball comes it stops (back to its guard at its own speed)");
        {   // (0.3.26, the owner's test: "I also held the ball but never saw a taunt, just kept throwing balls at me"):
            // a bot left to throw as it likes taunts between its throws while you hold yours
            Game h; h.setArena(tris); h.tun.basicMatchTime = 0; h.tun.meetTime = 0; h.tun.botLevel = 0; h.reset(true);
            PlayerInput hin[2] = {standing(h, 0), {}};
            for (int i = 0; i < 72 * 20 && h.match != MatchState::InProgress; ++i) stepCopy(h, hin);
            grabBall(h, hin);
            float most = 0;
            int throws = 0;
            for (int i = 0; i < 72 * 14; ++i) {
                stepCopy(h, hin);
                most = std::max(most, h.pl[1].botTaunt);
                throws += hasEvent(h.events, EventType::Throw, 1);
            }
            std::printf("       you hold your ball 14 s, the bot free to throw: its taunt %.2f, %d throws\n", most, throws);
            CHECK(most > 0.8f && throws >= 1, "you hold your ball, it keeps attacking: it taunts between its throws");
        }
        hold(2.f, []() {});
        Vec3 rel0[2];
        for (int h = 0; h < 2; ++h) rel0[h] = g.pl[1].in.hand[h].grip.p - g.pl[1].in.head.p;
        g.pl[1].score += 1;   // (a point to it)
        float moved = 0, cheer = 0;
        hold(1.f, [&]() {
            cheer = std::max(cheer, g.pl[1].botCheer);
            for (int h = 0; h < 2; ++h) moved = std::max(moved, length(g.pl[1].in.hand[h].grip.p - g.pl[1].in.head.p - rel0[h]));
        });
        std::printf("       a point won (celebration %d): a hand moved %.2f m\n", g.pl[1].botCheerKind, moved);
        CHECK(cheer > 0.f && moved > 0.15f, "a point won: it celebrates");
        int cheerKinds = 0;   // (0.3.26, the owner's test: "a little variety"; chosen by the moment it comes)
        for (int k = 0; k < 12; ++k) {
            hold(0.37f + 0.05f * k, []() {});
            g.pl[1].score += 1;
            hold(1.f / 72, []() {});
            cheerKinds |= 1 << std::max(0, std::min(3, g.pl[1].botCheerKind));
        }
        CHECK(((cheerKinds & 1) + ((cheerKinds >> 1) & 1) + ((cheerKinds >> 2) & 1) + ((cheerKinds >> 3) & 1)) >= 3,
              "points won: its celebrations vary (a fist pump, a double pump, a point, a clap)");
        {   // (0.3.26, the owner in the asset viewer: "the point at you" pointed its fist up and back, its eyes on the
            // stands; and the double pump looked like the fist pump)
            hold(2.f, []() {});
            g.pl[1].score += 1;
            hold(1.f / 72, []() {});
            g.pl[1].botCheerKind = 2;
            float aim = -1, eyes = -1;
            hold(0.7f, [&]() {
                const PlayerState& s = g.pl[1];
                for (int h = 0; h < 2; ++h)
                    aim = std::max(aim, dot(s.in.hand[h].grip.q.rotate({0, 0, -1}), normalize(g.pl[0].in.head.p - s.in.hand[h].grip.p)));
                eyes = std::max(eyes, dot(s.in.head.q.rotate({0, 0, -1}), normalize(g.pl[0].in.head.p - s.in.head.p)));
            });
            std::printf("       a point at you: its fist %.0f degrees off you at best, its eyes %.0f\n",
                        rad2deg(std::acos(clampf(aim, -1.f, 1.f))), rad2deg(std::acos(clampf(eyes, -1.f, 1.f))));
            CHECK(aim > std::cos(deg2rad(20.f)) && eyes > std::cos(deg2rad(15.f)), "a point won, a point at you: its fist and its eyes at you");
            hold(1.f, []() {});
            g.pl[1].score += 1;
            hold(1.f / 72, []() {});
            g.pl[1].botCheerKind = 1;
            float top[2] = {-9.f, -9.f};
            hold(0.9f, [&]() {
                for (int h = 0; h < 2; ++h) top[h] = std::max(top[h], g.pl[1].in.hand[h].grip.p.y - g.pl[1].in.head.p.y);
            });
            const Ball& own = g.pl[1].ball;
            int up = 0, loose = 0;
            for (int h = 0; h < 2; ++h) {
                const bool holds = own.state == BallState::Held && own.heldHand == h;
                loose += !holds;
                up += !holds && top[h] > 0.1f;
            }
            std::printf("       a double pump: its fists up to %.2f and %.2f m over its head (%d free)\n", top[0], top[1], loose);
            CHECK(loose > 0 && up == loose, "a point won, a double pump: both free fists thrust up over its head");
        }
        g.match = MatchState::MatchSummary; g.matchWinner = 1;   // (the match won)
        g.pl[1].botWonSeen = true; g.pl[1].botWinKind = 0;   // (its stance's own pose)
        float raised = -9.f;
        int fists = 0;
        hold(1.5f, [&]() {
            raised = std::min(g.pl[1].in.hand[0].grip.p.y, g.pl[1].in.hand[1].grip.p.y) - g.pl[1].in.head.p.y;
            fists = (g.pl[1].in.hand[0].trigger > 0.5f) + (g.pl[1].in.hand[1].trigger > 0.5f);
        });
        std::printf("       the match won (RELAXED): both hands %.2f m over its head\n", raised);
        CHECK(raised > 0.15f && fists == 2, "the match won: RELAXED raises both arms, fists closed");
        g.pl[1].botStance = STANCE_KARATE;
        float bowed = 0;
        hold(1.f, [&]() { bowed = -g.pl[1].in.head.q.rotate({0, 0, -1}).y; });
        CHECK(bowed > 0.3f, "the match won: KARATE bows");
        g.pl[1].botStance = STANCE_RELAXED; g.pl[1].botWinKind = 1;   // AIR GUITAR
        float strumLo = 9.f, strumHi = -9.f, neckOut = 0;
        hold(2.f, [&]() {
            const PlayerState& s = g.pl[1];
            const float y = s.in.hand[1].grip.p.y - s.in.head.p.y;
            strumLo = std::min(strumLo, y); strumHi = std::max(strumHi, y);
            neckOut = std::fabs(s.in.hand[0].grip.p.z - s.in.head.p.z);
        });
        std::printf("       AIR GUITAR's stroke: %.2f m from top to bottom\n", strumHi - strumLo);
        CHECK(strumHi - strumLo > 0.45f && neckOut > 0.2f,
              "the match won: AIR GUITAR (the neck out to one side; a slow stroke from its shoulder past its hip)");
        {   // (0.3.26, the owner in the asset viewer: the pose alone read as something else: "manifest an actual
            // guitar"): its hologram guitar, held by the hand on its neck, the other hand at its strings; slow strokes
            Mat4 gm;
            float show = 0, offNeck = 0, nearStrings = 9.f, overHead = -9.f, lowest = 9.f;
            bool held = true;
            hold(5.6f, [&]() {   // (a whole bar: two strokes and the one from over its head)
                const PlayerState& s = g.pl[1];
                held &= g.botGuitar(1, s.in, gm, show);
                const Vec3 o{gm.m[12], gm.m[13], gm.m[14]}, neck = normalize(Vec3{gm.m[0], gm.m[1], gm.m[2]});
                const Vec3 d = s.in.hand[0].grip.p - o;
                offNeck = std::max(offNeck, length(d - neck * dot(d, neck)));
                nearStrings = std::min(nearStrings, length(s.in.hand[1].grip.p - o));
                const float y = s.in.hand[1].grip.p.y - s.in.head.p.y;
                overHead = std::max(overHead, y); lowest = std::min(lowest, y);
            });
            std::printf("       AIR GUITAR's guitar: shown %.2f, the hand on its neck %.3f m off it, the other %.2f m from its body; "
                        "the strumming hand from %.2f to %.2f m against its head\n", show, offNeck, nearStrings, lowest, overHead);
            CHECK(held && show > 0.95f && offNeck < 0.01f && nearStrings < 0.15f,
                  "AIR GUITAR: a guitar in its hands (the neck through one, the other at its strings)");
            CHECK(overHead > 0.05f && lowest < -0.55f, "AIR GUITAR: the third stroke from over its head, down past its hip");
            CHECK(!g.botGuitar(0, g.pl[0].in, gm, show), "... and none for a player");
        }
        g.pl[1].botWinKind = 2;   // RAISE THE ROOF
        float roofLo = 9.f, roofHi = -9.f;
        int open = 0;
        hold(1.2f, []() {});   // (its hands up to it first)
        {
            Mat4 gm;
            float show = 0;
            CHECK(!g.botGuitar(1, g.pl[1].in, gm, show), "another celebration: the guitar is gone");
        }
        hold(1.6f, [&]() {
            const PlayerState& s = g.pl[1];
            const float y = std::min(s.in.hand[0].grip.p.y, s.in.hand[1].grip.p.y) - s.in.head.p.y;
            roofLo = std::min(roofLo, y); roofHi = std::max(roofHi, y);
            open = 0;   // (open, but for a hand still holding its ball)
            for (int k = 0; k < 2; ++k) open += s.in.hand[k].trigger < 0.5f || (s.ball.state == BallState::Held && s.ball.heldHand == k);
        });
        std::printf("       RAISE THE ROOF: its hands from %.2f to %.2f m over its head\n", roofLo, roofHi);
        // (0.3.26, the owner in the asset viewer: held up, 10 cm at the top of its reach, it looked like the arms-up poses)
        CHECK(roofLo > -0.15f && roofHi > 0.2f && roofHi - roofLo > 0.15f && open == 2,
              "the match won: RAISE THE ROOF (open hands pushed up from its head, over and over)");
        int winKinds = 0;   // (each win chooses: by the moment it comes)
        for (int k = 0; k < 9; ++k) {
            g.match = MatchState::InProgress;
            hold(0.29f + 0.07f * k, []() {});
            g.match = MatchState::MatchSummary; g.matchWinner = 1;
            hold(1.f / 72, []() {});
            winKinds |= 1 << std::max(0, std::min(2, g.pl[1].botWinKind));
        }
        CHECK(((winKinds & 1) + ((winKinds >> 1) & 1) + ((winKinds >> 2) & 1)) >= 2, "matches won: its victory poses vary");
        g.match = MatchState::MatchSummary; g.matchWinner = 0;   // (lost: nothing to celebrate)
        g.pl[1].botStance = STANCE_RELAXED;
        hold(1.5f, [&]() { raised = std::max(g.pl[1].in.hand[0].grip.p.y, g.pl[1].in.hand[1].grip.p.y) - g.pl[1].in.head.p.y; });
        CHECK(raised < 0.f, "the match lost: no victory pose");
    }
    {
        std::printf("[practice bot: UNBEATABLE against a strong player]\n");
        struct M { const char* name; bool adv, shields; int knuckles; } modes[] = {
            {"Basic", false, true, -1}, {"Advanced", true, true, -1}, {"Basic HOTDOG", false, false, -1},
            {"Advanced HOTDOG", true, false, -1}};
        for (const M& m : modes) {
            Game g; g.setArena(tris); g.tun.advanced = m.adv; g.tun.shields = m.shields; g.tun.knuckleMode = m.knuckles;
            g.tun.basicMatchTime = 0; g.tun.meetTime = 0; g.tun.botLevel = kBotLevels - 1; g.reset(true);
            g.tun.botIdle = false;   // (BOT IDLE MOTION off: these counts come from fixed seeds; with it each level measures the same: PA_BOT_MEASURE)
            // 200 throws each from three spots and at the headset's two frame rates
            int throws = 0, points = 0, rebounds = 0, scored = 0, strikes = 0, blocks = 0, punches = 0;
            double slowest = 0;
            for (uint32_t seed : {99u, 1234u, 777u})
                for (float hz : {72.f, 90.f}) {
                    Game g2 = g;
                    const Attack a = attackBot(g2, 200, hz, seed);
                    throws += a.throws; points += a.points; rebounds += a.rebounds; scored += a.botPoints; strikes += a.strikes;
                    blocks += a.blocks; punches += a.knuckles; slowest = std::max(slowest, a.slowest);
                }
            std::printf("       %s: %d throws, %d hits (and %d of its own blocks back into it), it scored %d; %d strikes, %d shield blocks, %d punches; slowest step %.2f ms\n",
                        m.name, throws, points, rebounds, scored, strikes, blocks, punches, slowest * 1000);
            char msg[112];
            // (never hit, until it attacked with every shape, 2026-10-30: its curves and banks come back slower, so in
            // Basic it is without its ball, its shield, for longer; three strikes on it make a big fast ball)
            std::snprintf(msg, sizeof msg, "UNBEATABLE, %s: 1200 hard throws, hit 4 times at most", m.name);
            CHECK(throws == 1200 && points <= 4, msg);
            std::snprintf(msg, sizeof msg, "UNBEATABLE, %s: none of its own blocks comes back into it", m.name);
            CHECK(rebounds == 0, msg);
        }
    }
    {   // (a player's report, 2026-11-17: "on unbeatable ... it can really only be hit by its own ball, so I just
        // switched to only hitting its ball back and it was an ez win") Each time the bot's ball comes, the player's right
        // fist meets it half a metre in front, punching it back at the bot; and (or not) they throw their own ball at its
        // chest with the left hand whenever they have it
        std::printf("[practice bot: UNBEATABLE, its own ball punched back at it]\n");
        int allOwn = 0, allPunched = 0, allCaught = 0, allSlapped = 0, allCaughtBack = 0;
        for (int adv = 0; adv < 2; ++adv)
        for (int throws = 0; throws < 2; ++throws)
        for (int style = 0; style < 3; style += 2)   // straight back, or the fist turned 35 degrees (off a wall)
        for (float hs : {4.f, 8.f}) {
            Game g; g.setArena(tris); g.tun.advanced = adv; g.tun.knuckleMode = 1; g.tun.basicMatchTime = 0; g.tun.meetTime = 0;
            g.tun.botLevel = kBotLevels - 1; g.reset(true);
            PlayerInput in[2] = {standing(g, 0), {}};
            const Quat q = style == 2 ? kFaceP0 * Quat::axisAngle({0, 1, 0}, deg2rad(35.f)) : kFaceP0;
            const Vec3 kOff = q.rotate(g.tun.knuckleOffset);
            const float planeX = g.tun.start[0].x + 0.5f;
            int comes = 0, punched = 0, hitBot = 0, hitMe = 0, ownHits = 0, thrown = 0;
            bool coming = false;
            int f = -1;
            Vec3 dir{1, 0, 0};
            HandInput h0 = standing(g, 0).hand[0];
            for (float t = 0; t < 300.f; t += 1.f / 72) {
                const Ball& b = g.pl[1].ball;
                const bool now = b.state == BallState::Flying && b.instigator == 1 && b.vel.x < -1.f;
                if (now && !coming) ++comes;
                coming = now;
                in[0] = standing(g, 0);
                in[0].buttonA = g.match == MatchState::MatchSummary;
                if (now) {
                    const float tt = (planeX - b.pos.x) / b.vel.x;   // s until it reaches the plane in front
                    const Vec3 meet = b.pos + b.vel * tt;
                    // the fist waits where it will come, then punches into it over the last 0.1 s
                    const Vec3 at = Vec3{planeX - hs * std::max(0.f, tt < 0.1f ? 0.1f - tt : 0.f), meet.y, meet.z};
                    if (tt < 0.6f && std::fabs(meet.z) < 2.f && meet.y > 0.3f && meet.y < 2.4f) in[0].hand[1].grip = {q, at - kOff};
                }
                if (throws) {   // the left hand: takes its ball, swings at 6.5 m/s toward the bot's chest, lets go
                    const Ball& mine = g.pl[0].ball;
                    if (f < 0 && g.match == MatchState::InProgress && mine.state == BallState::Idle) {
                        f = 0;
                        h0 = standing(g, 0).hand[0];
                        h0.grip.p = mine.pos - h0.grip.q.rotate(g.tun.launcherOffset);
                        h0.trigger = 1;
                    } else if (f >= 0) {
                        if (f == 1) { dir = normalize(g.chestPoint(1) - g.launcherPoint(0, 0)); ++thrown; }
                        if (f >= 1 && f < 16) h0.grip.p += dir * (6.5f / 72);
                        if (f == 16) h0.trigger = 0;
                        if (++f > 60) { f = -1; h0 = standing(g, 0).hand[0]; }
                    }
                    in[0].hand[0] = h0;
                }
                PlayerInput tmp[2] = {in[0], in[1]};
                g.step(1.f / 72, tmp);
                for (const GameEvent& e : g.events) {
                    if (e.type == EventType::KnuckleBlock && e.player == 0) ++punched;
                    if (e.type == EventType::Caught && e.player == 1) {   // (the ball you punched back: you touched it last)
                        ++allCaught; allSlapped += g.pl[1].botSlap; allCaughtBack += g.pl[1].ball.instigator == 0;
                    }
                    if (e.type == EventType::HitPlayer && e.player == 1) { ++hitBot; ownHits += e.ball == 1; }
                    if (e.type == EventType::HitPlayer && e.player == 0) ++hitMe;
                }
            }
            const char* styles[] = {"straight", "", "off a wall"};
            allOwn += ownHits;
            allPunched += punched;
            std::printf("       %s, %s, %s %.0f m/s punches: its ball came %d times, punched back %d; the bot hit %d (%d by its own ball), the player hit %d\n",
                        adv ? "Advanced (knuckle guards on)" : "Basic", throws ? "throwing too" : "never throwing", styles[style], hs, comes,
                        punched, hitBot, ownHits, hitMe);
        }
        // (not reproduced, 2026-11-17: 3 in about 400 got it, all with the player throwing too; asked for a recording of
        // the match it happened in)
        std::printf("       (its own ball got it %d times in %d punched back; it caught %d of the punched-back balls; %d catches in all, %d slapped straight back)\n",
                    allOwn, allPunched, allCaughtBack, allCaught, allSlapped);
        // (before 11-17 it caught none of them: it only blocked or dodged; many punches send it off a wall or wide of it)
        CHECK(allCaughtBack * 8 >= allPunched && allSlapped * 2 >= allCaught,
              "UNBEATABLE catches its own ball sent back at it (not only a dodge: the owner, 11-17), and slaps most straight back");
        CHECK(allPunched > 300 && allOwn * 33 <= allPunched, "UNBEATABLE: its own ball punched back at it (straight, off a wall, hard or soft, throwing too or not) seldom gets it");
    }
    {   // its own ball at its back (the owner's practice, 2026-11-17: punched back past it, low or high, its ball came back
        // off the floor or the ceiling behind it into its back 2-3 s later, the player's curving throw in the air too)
        std::printf("[practice bot: UNBEATABLE, its own ball from behind]\n");
        int tries = 0, hits = 0, caught = 0, otherHits = 0;
        for (int busy = 0; busy < 2; ++busy) {
            int bh = 0, bc = 0, bt = 0;
            for (float y : {0.9f, 1.5f})                   // (where the punch sends it from, 4 m in front of it)
            for (float ty : {-0.8f, 2.9f})                 // (past it low, through its platform to the floor below; or high)
            for (float z : {-0.45f, -0.3f, 0.3f, 0.45f})   // (beside its body)
            for (float sp : {11.f, 15.f}) {
                Game g; g.setArena(tris); g.tun.knuckleMode = 1; g.tun.basicMatchTime = 0; g.tun.meetTime = 0;
                g.tun.botLevel = kBotLevels - 1; g.reset(true);
                PlayerInput in[2] = {standing(g, 0), {}};
                bool set = false;
                for (int i = 0; i < 72 * 40; ++i) {
                    Ball& mine = g.pl[1].ball;
                    if (!set && g.match == MatchState::InProgress && mine.state == BallState::Flying && mine.instigator == 1 &&
                        mine.pos.x < 3.f) {   // (it threw: its ball, as if punched back past it, comes back at its back)
                        const Vec3 head = g.pl[1].in.head.p;
                        mine.pos = Vec3{head.x - 4.f, y, z};
                        mine.vel = normalize(Vec3{head.x + 2.f, ty, z} - mine.pos) * sp;
                        mine.speedLimit = sp; mine.spin = {0, 3.f, 0}; mine.instigator = 0; mine.scored = false;
                        mine.bounces = 0; mine.timer = 1.f;
                        if (busy) {   // and the player's curving throw coming at it from the front at the same time
                            Ball& th = g.pl[0].ball;
                            th.state = BallState::Flying; th.heldHand = -1; th.instigator = 0; th.scored = false;
                            th.pos = g.launcherPoint(0, 1);
                            th.vel = normalize(g.chestPoint(1) - th.pos) * 14.5f; th.speedLimit = 14.5f;
                            th.spin = {0, 22.f, 0}; th.bounces = 0; th.timer = 0.2f;
                        }
                        set = true;
                        ++tries; ++bt;
                        for (int k = 0; k < 72 * 5; ++k) {
                            PlayerInput tmp[2] = {in[0], in[1]};
                            g.step(1.f / 72, tmp);
                            for (const GameEvent& e : g.events) {
                                if (e.type == EventType::HitPlayer && e.player == 1 && e.ball == 1) { ++hits; ++bh; std::printf("         (hit after %.2f s: from y %.1f to %.1f, z %+.2f, %.0f m/s; at %.2f %.2f %.2f, plan %d, mode %d, catch %d)\n", k / 72.f, y, ty, z, sp, e.pos.x, e.pos.y, e.pos.z, g.pl[1].botRead[1].plan, g.pl[1].botRead[1].mode, (int)g.pl[1].botRead[1].catchIt); }
                                if (e.type == EventType::HitPlayer && e.player == 1 && e.ball == 0) ++otherHits;
                                if (e.type == EventType::Caught && e.player == 1) { ++caught; ++bc; }
                            }
                        }
                        break;
                    }
                    PlayerInput tmp[2] = {in[0], in[1]};
                    g.step(1.f / 72, tmp);
                }
            }
            std::printf("       %s: its ball punched back past it %d times (low through its platform, or high): it got hit %d, caught it %d\n",
                        busy ? "the player's curving throw coming too" : "alone", bt, bh, bc);
        }
        std::printf("       (its own ball got it %d of %d; the player's throw got it %d)\n", hits, tries, otherHits);
        // (not reproduced, 2026-11-17: the game log now writes each hit's trail of bounces and the bot's read of it)
        CHECK(tries == 64 && hits * 16 <= tries, "UNBEATABLE: its own ball punched back past it, low or high, seldom comes back into it");
    }
    {   // two UNBEATABLE bots (the owner, 2026-11-17: "pit two unbeatable bots in the same arena and watch them throw their
        // own ball while punching opponent's ball back at them and similarly dodge opponent's ball while receiving their own
        // ball back from a shield deflection and slapshot it back from either hand"); shields and knuckle guards on
        std::printf("[two UNBEATABLE bots]\n");
        for (int adv = 0; adv < 2; ++adv) {
            Game g; g.setArena(tris); g.tun.advanced = adv; g.tun.knuckleMode = 1; g.tun.basicMatchTime = 0; g.tun.meetTime = 0;
            g.tun.advancedRoundTime = 0; g.tun.botLevel = kBotLevels - 1; g.resetWithBots(0x3);
            PlayerInput in[2] = {};
            int throws[2] = {}, punches[2] = {}, blocks[2] = {}, back[2] = {}, slaps[2][2] = {}, both[2] = {}, hits[2] = {}, own[2] = {};
            bool wasBoth[2] = {false, false};
            for (int i = 0; i < 72 * 600; ++i) {
                in[0].buttonA = in[1].buttonA = g.match == MatchState::MatchSummary || g.match == MatchState::RoundSummary;
                PlayerInput tmp[2] = {in[0], in[1]};
                g.step(1.f / 72, tmp);
                for (int p = 0; p < 2; ++p) {   // dodging the other's ball while its own comes back to its hand
                    const bool now = g.pl[p].botRead[0].plan == BOT_DODGE && g.pl[p].botRead[1].plan == BOT_CATCH;
                    both[p] += now && !wasBoth[p];
                    wasBoth[p] = now;
                }
                for (const GameEvent& e : g.events) {
                    if (e.player < 0 || e.player > 1) continue;
                    if (e.type == EventType::Throw) ++throws[e.player];
                    if (e.type == EventType::KnuckleBlock) ++punches[e.player];
                    if (e.type == EventType::ShieldBlock) ++blocks[e.player];
                    if (e.type == EventType::Caught && g.pl[e.player].ball.instigator != e.player) {   // (its own, sent back)
                        ++back[e.player];
                        if (g.pl[e.player].botSlap && e.hand >= 0 && e.hand < 2) ++slaps[e.player][e.hand];
                    }
                    if (e.type == EventType::HitPlayer) { ++hits[e.player]; own[e.player] += e.ball == e.player; }
                }
            }
            for (int p = 0; p < 2; ++p)
                std::printf("       %s, bot %d: %d throws, %d punches and %d shield blocks of the other's ball; its own ball sent back and caught %d"
                            " (slapped straight back: %d left hand, %d right); %d times dodging while its own ball came to its hand; hit %d (%d by its own)\n",
                            adv ? "rounds" : "timed", p, throws[p], punches[p], blocks[p], back[p], slaps[p][0], slaps[p][1], both[p], hits[p], own[p]);
            char msg[160];
            std::snprintf(msg, sizeof msg, "two UNBEATABLE bots (%s): each throws, punches and blocks the other's ball back, catches its own sent back and slaps it from either hand",
                          adv ? "rounds" : "timed");
            bool ok = true;
            for (int p = 0; p < 2; ++p)
                ok &= throws[p] > 50 && punches[p] + blocks[p] > 20 && back[p] > 5 && slaps[p][0] + slaps[p][1] > 3;
            ok &= slaps[0][0] + slaps[1][0] > 0 && slaps[0][1] + slaps[1][1] > 0;
            CHECK(ok, msg);
            std::snprintf(msg, sizeof msg, "two UNBEATABLE bots (%s): they dodge the other's ball while their own comes back to their hand", adv ? "rounds" : "timed");
            CHECK(both[0] + both[1] > 0, msg);
        }
    }
    {   // BUTTONS (controls.h; a player's report, 2026-11-17: let us change the push-to-talk button and the controls)
        std::printf("[buttons]\n");
        using namespace controls;
        const Map st = standard(), lh = leftHanded();
        CHECK(save(st) == "104542350",
              "STANDARD is the controls as they were: the left grip talks, A and X ready, B and the right stick recenter, Y peeks, "
              "the left stick shows the names");
        CHECK(save(lh) == "014245531" && sameMap(mirrored(lh), st),
              "LEFT-HANDED is STANDARD mirrored: the right grip talks, Y and the left stick recenter, B peeks, the left stick turns");
        // a grip takes only jobs that do no harm squeezed by accident, and may do nothing
        Map g = st;
        std::string gripJobs;
        for (int i = 0; i < 4; ++i) { g.job[S_RGRIP] = (uint8_t)nextJob(g, S_RGRIP, K_TOUCH); gripJobs += jobName(g.job[S_RGRIP]); gripJobs += ' '; }
        CHECK(gripJobs == "TALK PEEK NAMES NOTHING ", "a grip goes TALK, PEEK, NAMES, NOTHING (never READY or RECENTER)");
        CHECK(!canLeave(st, S_LGRIP, K_TOUCH) && nextJob(st, S_LGRIP, K_TOUCH) == J_TALK,
              "TALK's last button keeps it (give it another one first)");
        // the owner's case: TALK on Y, the left grip left empty
        Map y = st;
        for (int i = 0; i < J_COUNT && y.job[S_Y] != J_TALK; ++i) y.job[S_Y] = (uint8_t)nextJob(y, S_Y, K_TOUCH);
        for (int i = 0; i < J_COUNT && y.job[S_LGRIP] != J_NONE; ++i) y.job[S_LGRIP] = (uint8_t)nextJob(y, S_LGRIP, K_TOUCH);
        bool down[S_COUNT] = {};
        down[S_Y] = true;
        const bool yTalks = held(y, J_TALK, K_TOUCH, down);
        down[S_Y] = false; down[S_LGRIP] = true;
        CHECK(y.job[S_Y] == J_TALK && y.job[S_LGRIP] == J_NONE && yTalks && !held(y, J_TALK, K_TOUCH, down) &&
              !held(y, J_PEEK, K_TOUCH, down),
              "TALK can move to Y and the left grip do nothing (its squeeze does no job)");
        // what each job's buttons do, and the controllers that lack a button (Vive: no Y, no sticks; WMR: no Y)
        bool none[S_COUNT] = {}, x[S_COUNT] = {}, rs[S_COUNT] = {};
        x[S_X] = true; rs[S_RSTICK] = true;
        CHECK(held(st, J_READY, K_TOUCH, x) && held(st, J_RECENTER, K_TOUCH, rs) && !held(st, J_READY, K_TOUCH, none),
              "X readies up and the right stick's click recenters, as before");
        Map talkY = st;
        talkY.job[S_LGRIP] = J_NONE; talkY.job[S_Y] = J_TALK;
        Map odd{};   // TALK on the left stick, READY on Y, RECENTER on A and B
        odd.job[S_A] = J_RECENTER; odd.job[S_B] = J_RECENTER; odd.job[S_Y] = J_READY; odd.job[S_LSTICK] = J_TALK;
        const Map v1 = effective(talkY, K_VIVE), v2 = effective(odd, K_VIVE), w = effective(talkY, K_WMR);
        CHECK(v1.job[S_LGRIP] == J_TALK && w.job[S_LGRIP] == J_TALK && effective(talkY, K_TOUCH).job[S_LGRIP] == J_NONE,
              "TALK on Y with a Vive or Windows Mixed Reality controller: the left grip talks (with Touch: Y)");
        CHECK(v2.job[S_LGRIP] == J_TALK && v2.job[S_A] == J_READY && v2.job[S_B] == J_RECENTER,
              "on a Vive: READY and TALK without a button there take their usual ones, RECENTER keeps one");
        // settings.cfg: read back as saved; a map this game can't read is refused (STANDARD kept)
        Map r = st, bad = st;
        const bool round = load(r, save(lh)) && sameMap(r, lh);
        const bool refused = !load(bad, "554542350") && !load(bad, "000000000") && !load(bad, "10454235") &&
                             !load(bad, "10454235x") && !load(bad, "1045423a0") && sameMap(bad, st);
        CHECK(round && refused, "buttons= reads back as saved; READY on a grip, no TALK / READY / RECENTER, or garbage: refused");
        // pressed at random on every kind of controller: TALK, READY and RECENTER always keep a button there, a grip
        // never readies or recenters, and every map saves and reads back
        int bad1 = 0, bad2 = 0, bad3 = 0, presses = 0;
        uint32_t seed = 12345u;
        auto rnd = [&](int n) { seed = seed * 1664525u + 1013904223u; return (int)((seed >> 8) % (uint32_t)n); };
        for (int kind = K_UNKNOWN; kind <= K_WMR; ++kind) {
            Map m = st;
            for (int i = 0; i < 3000; ++i) {
                m = effective(m, kind);
                const int what = rnd(40);
                if (what == 0) m = standard();
                else if (what == 1) m = leftHanded();
                else if (what == 2) m.leftSticks = !m.leftSticks;
                else {
                    const int s = rnd(S_COUNT);
                    if (!onController(kind, s)) continue;
                    m.job[s] = (uint8_t)nextJob(m, s, kind);
                    ++presses;
                }
                const Map e = effective(m, kind);
                for (int j = 0; j < J_COUNT; ++j) bad1 += required(j) && count(e, j, kind) == 0;
                bad2 += e.job[S_LGRIP] == J_READY || e.job[S_LGRIP] == J_RECENTER || e.job[S_RGRIP] == J_READY ||
                        e.job[S_RGRIP] == J_RECENTER;
                Map back = standard();
                bad3 += !(load(back, save(m)) && sameMap(back, m));
            }
        }
        std::printf("       (%d presses on the page, five kinds of controller)\n", presses);
        CHECK(bad1 == 0 && bad2 == 0 && bad3 == 0,
              "any presses: TALK, READY and RECENTER keep a button on these controllers, no grip readies or recenters, it saves");
    }

    {   // BOT STYLES PER ROOM (a player's report, 2026-11-17: "probably need different logic for basic and advanced bots")
        std::printf("[bot styles per room]\n");
        // BASIC's brawler punches through: balls served at UNBEATABLE at 8 m/s (no shields, knuckle guards on), how fast
        // its knuckle guard sends them back; in Advanced with knuckle guards on (the duellist, no brawler) for comparison
        float meanOut[2] = {0, 0};
        int punches[2] = {0, 0};
        for (int adv = 0; adv < 2; ++adv) {
            Game g; g.setArena(tris); g.tun.advanced = true; g.tun.botPlay = adv; g.tun.shields = false; g.tun.knuckleMode = 1;   // (rounds, each style)
            g.tun.basicMatchTime = 0; g.tun.meetTime = 0; g.tun.botLevel = kBotLevels - 1; g.reset(true);
            PlayerInput in[2] = {standing(g, 0), {}};
            for (int i = 0; i < 72 * 4 && g.match != MatchState::InProgress; ++i) stepCopy(g, in);
            float sum = 0;
            for (int k = 0; k < 24; ++k) {
                for (int i = 0; i < 72 * 3 && g.pl[0].ball.state != BallState::Idle; ++i) { in[0] = standing(g, 0); stepCopy(g, in); }
                Vec3 target = g.chestPoint(1);
                const float sgn = (k / 4) % 2 ? -1.f : 1.f;
                switch (k % 4) {
                case 0: break;
                case 1: target.y += 0.30f; break;
                case 2: target.y -= 0.30f; break;
                default: target.z += 0.25f * sgn; break;
                }
                const Vec3 from = g.tun.start[0] + Vec3{0.6f, 1.3f, 0};
                g.serve(0, from, normalize(target - from), 8.f, {0, 0, 0}, false);
                for (int i = 0; i < 72 * 3; ++i) {
                    in[0] = standing(g, 0);
                    stepCopy(g, in);
                    bool done = false;
                    for (const GameEvent& e : g.events)
                        if (e.type == EventType::KnuckleBlock && e.player == 1) { sum += e.value; ++punches[adv]; done = true; }
                    if (done || g.pl[0].ball.state != BallState::Flying) break;
                }
            }
            meanOut[adv] = punches[adv] ? sum / punches[adv] : 0.f;
        }
        std::printf("       (UNBEATABLE's knuckle guard, balls at 8 m/s: Basic %d punched back at %.1f m/s on average, Advanced %d at %.1f)\n",
                    punches[0], meanOut[0], punches[1], meanOut[1]);
        CHECK(punches[0] >= 12 && punches[1] >= 12 && meanOut[0] > meanOut[1] + 1.5f,
              "BOT STYLE: the BRAWLER punches through a ball it blocks with a knuckle guard, in rounds too: it goes back faster than the DUELLIST's");
        // ADVANCED's duellist: holding its charged ball while you hold yours, it keeps its shield (as before); your ball
        // uncharged (no shield for you), it throws at once
        float firstThrow[2] = {-1, -1};
        for (int exposed = 0; exposed < 2; ++exposed) {
            Game g; g.setArena(tris); g.tun.advanced = true; g.tun.botPlay = 1; g.tun.meetTime = 0; g.tun.botLevel = kBotLevels - 1; g.reset(true);
            PlayerInput in[2] = {standing(g, 0), {}};
            for (int i = 0; i < 72 * 4 && g.match != MatchState::InProgress; ++i) stepCopy(g, in);
            grabBall(g, in, 1);
            for (int i = 0; i < 72 * 4 && g.pl[1].ball.state != BallState::Held; ++i) stepCopy(g, in);
            if (exposed) g.pl[0].ball.charged = false;
            const float t0 = g.now;
            for (int i = 0; i < 72 * 6 && firstThrow[exposed] < 0; ++i) {
                stepCopy(g, in);
                if (exposed) g.pl[0].ball.charged = false;
                for (const GameEvent& e : g.events)
                    if (e.type == EventType::Throw && e.player == 1) firstThrow[exposed] = g.now - t0;
            }
        }
        std::printf("       (ADVANCED's UNBEATABLE holding its charged ball: you with a shield, it threw after %.1f s; without one, after %.1f s)\n",
                    firstThrow[0], firstThrow[1]);
        CHECK(firstThrow[1] >= 0 && firstThrow[1] < 1.2f && (firstThrow[0] < 0 || firstThrow[0] > firstThrow[1] + 1.0f),
              "ADVANCED's duellist throws at once when you have no shield, and keeps its own while you keep yours");
    }
}


// ---- MATCH MY LEVEL, and RECORD MY MATCHES
static void matchLevelTests(const std::vector<CollisionTri>& tris) {
    {
        std::printf("[MATCH MY LEVEL: the levels between levels]\n");
        bool same = true, smooth = true;
        for (int l = 0; l < kBotLevels; ++l) {
            const BotSkill a = botSkillAt((float)l), &b = botSkill(l);
            same &= a.reaction == b.reaction && a.handSpeed == b.handSpeed && a.readsFlight == b.readsFlight && !std::strcmp(a.name, b.name);
        }
        for (float l = 0; l < kBotLevels - 1.01f; l += 0.05f)
            smooth &= botSkillAt(l + 0.05f).reaction <= botSkillAt(l).reaction && botSkillAt(l + 0.05f).handSpeed >= botSkillAt(l).handSpeed;
        CHECK(same, "at a whole level, MATCH MY LEVEL plays exactly that level");
        CHECK(smooth, "between levels it gets steadily quicker (reaction, hands)");
    }
    std::printf("[MATCH MY LEVEL: it follows the player]\n");
    {   // someone who never throws back: every point is the bot's, so it eases down to ROOKIE and stays there
        Game g; g.setArena(tris); g.tun.botAdapt = true; g.tun.botMatch = 2.f; g.tun.basicMatchTime = 0; g.reset(true);
        g.tun.selfHitScores = false;   // (its own ball coming back at it would give the other side points: not theirs to earn)
        PlayerInput in[2] = {standing(g, 0), {}};
        for (HandInput& h : in[0].hand) h.tracked = false;   // (no hands up: still fists would knock its throws back at it)
        std::vector<GameEvent> log;
        run(g, in, 400.f, &log);
        int pts = 0, own = 0;
        for (const GameEvent& e : log) { pts += e.type == EventType::HitPlayer && e.player == 0; own += e.type == EventType::HitPlayer && e.player == 1; }
        std::printf("       a player who doesn't play: the bot scored %d (hit by its own ball %d), its level %.2f\n", pts, own, g.tun.botMatch);
        CHECK(pts > 5 && g.tun.botMatch == 0.f, "against someone who doesn't play back it eases down to ROOKIE");
    }
    {   // a strong player: from ROOKIE it climbs until the points even out
        Game g; g.setArena(tris); g.tun.botAdapt = true; g.tun.botMatch = 0.f; g.tun.basicMatchTime = 0; g.tun.meetTime = 0; g.reset(true);
        const Attack a = attackBot(g, 200, 72.f, 99u);
        std::printf("       a strong player: %d points to the bot's %d, its level %.2f (%s)\n", a.points, a.botPoints, g.tun.botMatch,
                    botSkillAt(g.tun.botMatch).name);
        CHECK(a.points > 0 && g.tun.botMatch >= 0.f && g.tun.botMatch <= kBotLevels - 1.f, "a strong player scores, and its level stays in range");
        CHECK(std::abs(a.points - a.botPoints) * 5 <= a.points + a.botPoints, "the points even out (within a fifth of each other)");
        Game f; f.setArena(tris); f.tun.botLevel = 2; f.tun.basicMatchTime = 0; f.tun.meetTime = 0; f.reset(true);
        const Attack b = attackBot(f, 200, 72.f, 99u);
        std::printf("       (against PRO, set: %d points to the bot's %d)\n", b.points, b.botPoints);
    }
}

static void recordTests(const std::vector<CollisionTri>& tris) {
    std::printf("[RECORD MY MATCHES]\n");
    Game g; g.setArena(tris); g.tun.botLevel = 3; g.tun.basicMatchTime = 60; g.tun.meetTime = 0; g.reset(true);
    rec::Recorder r;
    // (in the temp folder: tools/run_tests.bat has tools/analyze_play.py check it)
    const char* tmp = std::getenv("TEMP") ? std::getenv("TEMP") : std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/data/local/tmp";
    const std::string path = std::string(tmp) + "/projectarena_sample_match.par";
    rec::Meta m;
    m.local = 0;
    m.version = "test";
    CHECK(r.begin(path, g, m), "a recording opens");
    attackBot(g, 60, 72.f, 7u, &r);
    r.end(g);
    std::FILE* f = std::fopen(path.c_str(), "rb");
    std::vector<uint8_t> bytes;
    if (f) {
        uint8_t buf[4096];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) bytes.insert(bytes.end(), buf, buf + n);
        std::fclose(f);
    }
    std::printf("       %s: %.1f MB\n", path.c_str(), bytes.size() / 1e6);
    CHECK(bytes.size() > 1000 && !std::memcmp(bytes.data(), "PAREC1", 6) && bytes[bytes.size() - 5] == 'X',
          "it starts with its name and ends with its end mark (tools/analyze_play.py reads it)");
}


// ---- the bot's stances and its fist bump
static void stanceTests(const std::vector<CollisionTri>& tris) {
    {
        std::printf("[practice bot: stances]\n");
        bool reach = true, rig = true, held = true;
        const AvatarDesc look = avatar::preset(3);
        for (int st = 0; st < kBotStances; ++st) {
            Game g; g.setArena(tris); g.tun.botLevel = 3; g.reset(true);
            g.pl[1].botStance = st;
            PlayerInput in[2] = {standing(g, 0), {}};
            in[0].head.p.z += 3.f;   // (out of the way: nothing happens, the bot just stands)
            run(g, in, 2.f);
            const PlayerState& s = g.pl[1];
            for (int h = 0; h < 2; ++h) {
                if (length(g.botWrist(h, g.handPose(1, h)) - g.botShoulder(1, h)) > g.tun.botArm + 1e-3f) reach = false;
                if (length(s.botHandVel[h]) > 0.5f) held = false;
            }
            const avatar::Rig r = avatar::solveRig(s.in, look);
            const avatar::BodyBind& K = avatar::bodyBind(look.body);
            for (int k = 0; k < 2; ++k) {
                const float l1 = length(K.elbow[k] - K.shoulder[k]) * avatar::rigTuning().upperLen;
                const float l2 = length(K.wrist[k] - K.elbow[k]) * avatar::rigTuning().foreLen;
                if (length(r.upper[k].b - r.upper[k].a) > l1 * 1.002f || length(r.fore[k].b - r.fore[k].a) > l2 * 1.002f) rig = false;
            }
            std::printf("       %-8s hands at (%.2f %.2f %.2f) and (%.2f %.2f %.2f) from its head\n", botStanceName(st),
                        s.in.hand[0].grip.p.x - s.in.head.p.x, s.in.hand[0].grip.p.y - s.in.head.p.y, s.in.hand[0].grip.p.z - s.in.head.p.z,
                        s.in.hand[1].grip.p.x - s.in.head.p.x, s.in.hand[1].grip.p.y - s.in.head.p.y, s.in.hand[1].grip.p.z - s.in.head.p.z);
        }
        CHECK(reach && rig, "every stance is within the arm's reach, and the drawn arms never stretch");
        CHECK(held, "and it holds it (the hands settled)");
        int open = 0, picks = 0;
        for (int i = 0; i < 40; ++i) {   // beginners hold no guard
            Game g; g.setArena(tris); g.tun.botLevel = 0; g.rngSeed((i + 1) * 2654435761u); g.reset(true);
            PlayerInput in[2] = {standing(g, 0), {}};
            run(g, in, 0.1f);
            const int st = g.pl[1].botStance;
            open += st == STANCE_KITTEN || st == STANCE_RELAXED || st == STANCE_MONKEY;
            ++picks;
        }
        CHECK(open == picks, "ROOKIE only takes an open stance (no guard)");
        bool all[kBotStances] = {};
        for (int i = 0; i < 80; ++i) {
            Game g; g.setArena(tris); g.tun.botLevel = 4; g.rngSeed((i + 7) * 2246822519u); g.reset(true);
            PlayerInput in[2] = {standing(g, 0), {}};
            run(g, in, 0.1f);
            all[g.pl[1].botStance] = true;
        }
        bool every = true;
        for (bool b : all) every &= b;
        CHECK(every, "from PRO up, every stance comes up (picked at random each match)");
    }
    {
        std::printf("[practice bot: FIST BUMP FIRST]\n");
        Game g; g.setArena(tris); g.tun.botMeet = true; g.reset(true);
        g.pl[1].botStance = STANCE_KARATE;
        PlayerInput in[2] = {standing(g, 0), {}};
        run(g, in, 0.1f);
        CHECK(g.match == MatchState::Meet, "with FIST BUMP FIRST on, the bot meets you in the centre");
        // the person at their side of the centre line, the right fist held out to the middle, closed
        in[0].head.p = {-g.tun.meetSpot, 1.65f, 0};
        in[0].hand[1].grip = {kFaceP0, {-0.12f, 1.30f, 0.02f}};
        in[0].hand[1].trigger = 1;
        in[0].hand[0].grip.p = {-0.55f, 1.0f, -0.25f};
        std::vector<GameEvent> log;
        float bowed = 0;
        for (int i = 0; i < 72 * 4 && !hasEvent(log, EventType::FistBump); ++i) {
            PlayerInput tmp[2] = {in[0], in[1]};
            g.step(1.f / 72, tmp);
            log.insert(log.end(), g.events.begin(), g.events.end());
        }
        CHECK(hasEvent(log, EventType::FistBump), "it brings its fist to yours: a fist bump");
        CHECK(std::fabs(g.pl[1].in.head.p.x - g.tun.meetSpot) < 0.3f, "(it came to its side of the centre for it)");
        for (int i = 0; i < 36; ++i) {
            PlayerInput tmp[2] = {in[0], in[1]};
            g.step(1.f / 72, tmp);
            bowed = std::max(bowed, -g.pl[1].in.head.q.rotate({0, 0, -1}).y);
        }
        CHECK(bowed > 0.3f, "KARATE bows at the fist bump");
        run(g, in, 1.f);
        CHECK(g.match == MatchState::Countdown || g.match == MatchState::InProgress, "then the match starts");
        Game h; h.setArena(tris); h.reset(true);
        PlayerInput in2[2] = {standing(h, 0), {}};
        run(h, in2, 0.1f);
        CHECK(h.match == MatchState::Countdown, "with it off (as before), straight to the countdown");
    }
}

// TRAINING: every lesson can be passed. The throwing lessons by a perfect thrower (the coach's planner, from where a
// person stands), STRIKE ZONE and CATCH by a scripted player, the defense lessons by the coach-bot in your place; and
// the coach's demonstrations take their targets.
static void trainingTests(const std::vector<CollisionTri>& tris) {
    using namespace training;
    std::printf("[TRAINING: lessons]\n");
    const float dt = 1.f / 72;
    auto person = [&](const Game& g) { return standing(g, 0); };   // at your end, facing the far wall, hands down
    // play lesson i: `act` drives the player each step (it may serve the ball); until done or `limit` seconds
    auto play = [&](int i, float limit, const std::function<void(Game&, Trainer&, PlayerInput&)>& act, bool botPlayer = false) {
        Game g; g.setArena(tris); g.tun.botLevel = 2;
        Trainer tr;
        tr.begin(g, i, tris);
        PlayerInput in[kMaxPlayers];
        for (int k = 0; k < 72; ++k) { in[0] = person(g); tr.step(g, dt, in); }   // the card a moment
        tr.start(g);
        if (botPlayer) {   // the coach's own skills in your place: it holds its ball and meets the throws
            g.pl[0].isBot = true; g.pl[0].botLevel = kBotLevels - 1;
            g.pl[0].coach = PlayerState::Coach{}; g.pl[0].coach.on = true;
            g.pl[0].coach.stance = STANCE_KITTEN;
        }
        int cueN[32] = {0};
        for (float t = 0; t < limit && tr.phase != Phase::Done; t += dt) {
            in[0] = person(g);
            if (tr.phase == Phase::Play) act(g, tr, in[0]);
            tr.step(g, dt, in);
            for (const CueEvent& c : tr.cues) ++cueN[(int)c.cue];
        }
        if (botPlayer)
            std::printf("       (blocked %d, punched %d, dodged %d, hit %d, let through %d, body %d)\n", cueN[(int)Cue::Blocked],
                        cueN[(int)Cue::Punched], cueN[(int)Cue::Dodged], cueN[(int)Cue::Hit], cueN[(int)Cue::Dropped], cueN[(int)Cue::BodyHit]);
        return tr;
    };
    {   // CUSTOM TRAINING (0.3.22): a lesson with MY OPTIONS plays your room options; STANDARD the rules' own; out of
        // training, your rules as they were
        auto lessonRules = [&](bool mine, Tunables& during, Tunables& after) {
            Game g; g.setArena(tris);
            g.tun.aimAssist = 3; g.tun.shieldSize = 2; g.tun.maiden = true; g.tun.strikeZone = 2; g.tun.strikeSpeed = 1;
            g.tun.ballGrowth = 1; g.tun.wallsLively = true; g.tun.wallGrip = true; g.tun.bankAssist = 2;
            g.tun.foulShift = 2; g.tun.basicMatchTime = 300.f;   // (not the lessons': the foul line and the clock stay theirs)
            Trainer tr;
            tr.custom = mine;
            tr.begin(g, lessonById(21), tris);   // (BLOCKING 1: SHIELD)
            during = g.tun;
            tr.stop(g);
            after = g.tun;
        };
        Tunables d0, a0, d1, a1;
        lessonRules(false, d0, a0);
        lessonRules(true, d1, a1);
        const Tunables std_;
        const bool standard = d0.aimAssistNow() == 2 && d0.shieldSizeNow() == 0 && !d0.maidenNow() && d0.strikeZoneNow() == 1 &&
                              d0.strikeSpeedNow() == 0 && d0.ballGrowthNow() == 3 && d0.bouncesStandard();
        const bool mine = d1.aimAssistNow() == 3 && d1.shieldSizeNow() == 2 && d1.maidenNow() && d1.strikeZoneNow() == 2 &&
                          d1.strikeSpeedNow() == 1 && d1.ballGrowthNow() == 1 && d1.wallsLivelyNow() && d1.wallGripNow() &&
                          d1.bankAssistNow() == 2;
        const bool lessonsOwn = d1.foulShiftNow() == 0 && d1.basicMatchTime == 0.f && d1.shields && d1.knuckleMode == 0;
        const bool back = a0.aimAssist == 3 && a1.aimAssist == 3 && a1.wallGrip && a1.foulShift == 2 && a1.basicMatchTime == 300.f;
        CHECK(standard && mine && lessonsOwn && back,
              "CUSTOM TRAINING: STANDARD plays the rules' options; MY OPTIONS your own (aim assist, shield size, MAIDEN, strike zone and "
              "speed, ball growth, BOUNCES); the lesson's shields, foul line and clock stay its own; afterwards your rules as they were");
    }
    const Vec3 hand{-6.6f, 1.35f, 0.1f};   // where a person's throw leaves
    int planned = 0, plannedOk = 0;
    for (int i = 0; i < (int)lessons().size(); ++i) {
        const Lesson& L = lessons()[i];
        if (L.kind != THROW) continue;
        {   // every target, by itself, can be had from where you stand (aim assist on, as you throw)
            Game g; g.setArena(tris);
            Trainer tr; tr.begin(g, i, tris);
            std::vector<Target> T = tr.targets;
            for (int k = 0; k < (int)T.size(); ++k) {
                for (int j = 0; j < (int)T.size(); ++j) T[j].live = j == k;
                const Shot s = planShot(g, 0, hand, L, T, k, 1.f + 0.7f * k, true);
                ++planned; plannedOk += s.ok;
                if (!s.ok) std::printf("       %s: target %d not reached\n", L.name, k);
            }
        }
        // a perfect thrower: the planned throw at the first target up, each time the ball is ready
        Trainer r = play(i, 120.f, [&](Game& gg, Trainer& t, PlayerInput&) {
            if (gg.pl[0].ball.state != BallState::Idle) return;
            int k = -1;
            for (int j = 0; j < (int)t.targets.size() && k < 0; ++j) if (t.targets[j].live && !t.targets[j].down) k = j;
            if (k < 0) return;
            const Shot s = planShot(gg, 0, hand, L, t.targets, k, t.clock, true);
            gg.serve(0, hand, s.dir, s.speed, s.spin, true);
        });
        std::printf("       %-16s %d / %d in %.1f s (%d throws)\n", L.name, r.successes, (int)r.targets.size(), r.clock, r.attempts);
        CHECK(r.phase == Phase::Done && r.result.passed && r.result.attempts == (int)L.targets.size(),
              (std::string(L.name) + ": a perfect thrower clears it, a throw a target").c_str());
    }
    CHECK(planned > 0 && plannedOk == planned, "every throwing lesson's targets can be reached (planned throws)");
    for (int i = 0; i < (int)lessons().size(); ++i) {   // a person's throws, by the hands (grab, swing back, sweep, let go) count
        const Lesson& L = lessons()[i];
        if (L.kind == DEFEND) continue;
        Thrower th;
        Trainer r = play(i, 150.f, [&](Game& g, Trainer& t, PlayerInput& in) { in = th.step(g, 0, L, t.targets, t.clock, dt); });
        std::printf("       by hand: %-16s %d / %d in %d tries, %.1f s\n", L.name, r.successes, L.goal(), r.attempts, r.clock);
        CHECK(r.phase == Phase::Done && r.result.passed, (std::string(L.name) + ": thrown by hand (the trigger let go mid-swing), it is passed").c_str());
    }
    {   // STRIKE ZONE: past the coach's body, through its ring
        const int i = lessonById(7);
        int n = 0;
        Trainer r = play(i, 120.f, [&](Game& g, Trainer&, PlayerInput&) {
            if (g.pl[0].ball.state != BallState::Idle) return;
            const Vec3 at = g.strikeZoneCenter(1) + Vec3{0, 0.02f, (n++ & 1) ? 0.45f : -0.45f};
            g.serve(0, hand, at - hand, 11.5f, {}, true);
        });
        std::printf("       %-16s %d strikes in %d throws, %.1f s\n", lessons()[i].name, r.successes, r.attempts, r.clock);
        CHECK(r.result.passed && r.result.attempts <= 6, "STRIKE ZONE: throws past the coach earn strikes; passed");
    }
    {   // CATCH: at the far wall, then a hand (trigger held) where the ball comes back
        const int i = lessonById(8);
        Trainer r = play(i, 120.f, [&](Game& g, Trainer&, PlayerInput& in) {
            Ball& b = g.pl[0].ball;
            if (b.state == BallState::Idle) g.serve(0, hand, Vec3{9.5f, 1.7f, 0.3f} - hand, 10.f, {}, true);
            if (b.state == BallState::Returning && length(b.pos - in.head.p) < 1.2f) {
                in.hand[1].trigger = 1;
                in.hand[1].grip.p = b.pos - in.hand[1].grip.q.rotate(g.tun.catcherOffset);
            }
        });
        std::printf("       %-16s %d / %d caught\n", lessons()[i].name, r.successes, r.attempts);
        CHECK(r.result.passed && r.successes == r.attempts, "CATCH: each ball back from the far wall is caught; passed");
    }
    for (int i = 0; i < (int)lessons().size(); ++i) {   // the defense lessons: the far end's bot skills in your place
        const Lesson& L = lessons()[i];
        if (L.kind != DEFEND) continue;
        Trainer r = play(i, 150.f, [&](Game&, Trainer&, PlayerInput&) {}, true);
        std::printf("       %-16s %d / %d (pass %d), %.1f s\n", L.name, r.successes, r.attempts, L.goal(), r.clock);
        if (L.guard == G_SENDBACK) CHECK(r.phase == Phase::Done && r.successes > 0, "SEND IT BACK: blocks take targets");
        else if (L.guard == G_SHIELD || L.guard == G_EITHER)   // (the bot keeps its ball in its right hand: the throws to its left it lets by)
            CHECK(r.phase == Phase::Done && r.attempts == L.budget && r.successes >= L.budget / 3,
                  (std::string(L.name) + ": the throws all come; a shield held where they come blocks them").c_str());
        else CHECK(r.phase == Phase::Done && r.result.passed && r.attempts == L.budget,
                   (std::string(L.name) + ": the coach throws them all; met well, it is passed").c_str());
    }
    {   // bests: a better run replaces them; the next lesson opens once one is passed; saved and read back
        Bests b;
        const int b0 = lessonById(21), d0 = lessonById(41);
        CHECK(b.unlocked(0) && !b.unlocked(1) && b.unlocked(b0) && !b.unlocked(b0 + 1) && b.unlocked(d0), "only each track's first lesson is open at first");
        Result r; r.lesson = 0; r.passed = false; r.successes = 3; r.attempts = 15; r.time = 30;
        CHECK(!b.add(r) && !b.unlocked(1), "a run not passed counts for nothing");
        r.passed = true; r.successes = 5; r.attempts = 9; r.time = 40;
        CHECK(b.add(r) && b.unlocked(1) && !b.unlocked(2), "passed: the next lesson opens");
        Result faster = r; faster.time = 25; faster.attempts = 12;
        CHECK(b.add(faster) && b.b[1].time == 25 && b.b[1].attempts == 9, "a faster run keeps the time; the accuracy stays the better one's");
        Bests c; c.load(b.save());
        CHECK(c.b[1].passed && c.b[1].time == 25 && c.b[1].successes == 5 && c.total() == 555, "bests saved and read back");
        c.load("1 1 -5 9 3\n300 1 1 1 1\n");
        CHECK(c.b[1].time == 25, "nonsense lines are ignored");
    }
}

// ---- FIREBALL BLITZ (owner, 2026-10-30): an octagon for up to four, fireballs hopping along the floor, health, the ring
// closing in, the last one standing
static Game octGame(const std::vector<CollisionTri>& tris, uint32_t bots, int health = 5, int balls = 1) {
    Game g; g.setArena(tris);
    g.tun.experimental = true; g.tun.expRoom = ROOM_BLITZ; g.tun.players = 4; g.tun.meetTime = 0;
    g.tun.health = health; g.tun.fireBalls = balls; g.tun.basicMatchTime = 0;
    g.useArena(ARENA_OCTAGON, tris);
    g.resetWithBots(bots);
    return g;
}
static void octagonTests(const std::vector<CollisionTri>& tris) {
    std::printf("[FIREBALL BLITZ]\n");
    {
        Game g = octGame(tris, 0);
        bool ring = true;
        for (int p = 0; p < 4; ++p) {
            ring &= std::fabs(length(g.tun.start[p]) - 6.f) < 1e-3f && dot(g.facing(p), normalize(Vec3{0, 0, 0} - g.tun.start[p])) > 0.999f;
            ring &= length(g.tun.start[p] - Vec3{-7.f, 0, 0}) > 0.5f || p != 0;
        }
        CHECK(ring && std::fabs(g.tun.start[0].x + 6.f) < 1e-3f && std::fabs(g.tun.start[2].z + 6.f) < 1e-3f && std::fabs(g.ringApothem - 7.5f) < 1e-3f,
              "four places evenly round a 6 m ring, each facing the centre, the walls 1.5 m behind");
        g.layoutRing(0x3);
        CHECK(std::fabs(g.tun.start[0].x + 3.5f) < 1e-3f && std::fabs(g.tun.start[1].x - 3.5f) < 1e-3f && std::fabs(g.ringApothem - 5.f) < 1e-3f,
              "two left: face to face 3.5 m from the centre, the walls closed in");
        g.layoutRing(0xB);
        bool three = true;
        for (int p : {0, 1, 3}) three &= std::fabs(length(g.tun.start[p]) - 4.8f) < 1e-3f;
        CHECK(three && std::fabs(dot(normalize(g.tun.start[0]), normalize(g.tun.start[1])) + 0.5f) < 1e-3f, "three: 4.8 m, a third of the way round each");
        Game c = octGame(tris, 0);
        c.useArena(ARENA_CLASSIC, tris);
        CHECK(length(c.tun.start[0] - Vec3{-7.f, 0, 0}) < 1e-4f && length(c.tun.spawner[1] - Vec3{6.48f, 0.02f, 0}) < 1e-4f,
              "leaving the octagon puts every place back where it always was");
    }
    {   // a fireball: down to the floor, back up to knee height each hop, its speed along the floor kept, gone at its 4th bounce
        Game g = octGame(tris, 0);
        PlayerInput in[4] = {standing(g, 0), {}, {}, {}};
        g.serve(0, g.tun.start[0] + Vec3{0.6f, 1.3f, 0.3f}, normalize(Vec3{1, 0, 0.35f}), 9.f, {}, false);
        CHECK(g.pl[0].ball.state != BallState::Flying && g.fireCount(0) == 1, "a throw becomes a fireball (the tee keeps no flying ball)");
        float apex = 0, lastY = 9, minFlat = 99, maxFlat = 0;
        int bounces = 0, fizzles = 0;
        bool rising = false;
        for (int i = 0; i < 72 * 4 && g.fireCount(0); ++i) {
            const Ball& f = g.fire[0][0];
            g.step(1.f / 72, in);
            for (const GameEvent& e : g.events) { bounces += e.type == EventType::Bounce; fizzles += e.type == EventType::Fizzle; }
            if (f.state != BallState::Flying) break;
            const float flat = length(Vec3{f.vel.x, 0, f.vel.z});
            minFlat = std::min(minFlat, flat); maxFlat = std::max(maxFlat, flat);
            if (bounces >= 1) { if (f.pos.y > lastY) rising = true; else if (rising) { apex = std::max(apex, lastY); rising = false; } }
            lastY = f.pos.y;
        }
        char msg[128];
        std::snprintf(msg, sizeof msg, "it hops to the waist (%.2f m) at the same speed along the floor (%.2f to %.2f m/s)", apex, minFlat, maxFlat);
        CHECK(apex > 0.9f && apex < 1.06f && maxFlat - minFlat < 0.05f, msg);
        CHECK(bounces == 3 && fizzles == 1 && g.fireCount(0) == 0, "three hops, and it fizzles out at the fourth bounce");
    }
    {   // a body: the legs count; your own fireball never hurts you
        Game g = octGame(tris, 0);
        const Vec3 head = g.pl[1].in.head.p;
        Ball b; b.scale = 1.f;
        b.pos = head + Vec3{-3.f, -1.25f, 0}; b.vel = {8.f, 0, 0}; b.speedLimit = 8.f;
        CHECK(g.fireHitTime(b, head, 0.3f) < 0.f, "a fireball at knee height doesn't count (waist up only)");
        b.pos = head + Vec3{-3.f, -0.6f, 0}; b.vel = {8.f, 2.f, 0};
        CHECK(g.fireHitTime(b, head, 1.f) >= 0.f, "one at the body does");
        Ball seat; seat.scale = 1.f;   // (across behind, at seat height: the butt counts)
        seat.pos = head + Vec3{0.2f, -1.03f, -2.f}; seat.vel = {0, 1.5f, 8.f}; seat.speedLimit = 8.f;   // (level as it passes behind)
        CHECK(g.fireHitTime(seat, head, 0.4f) >= 0.f, "one across your seat counts (the hips and butt are in)");
        PlayerInput in[4] = {standing(g, 0), standing(g, 1), standing(g, 2), standing(g, 3)};
        for (int i = 0; i < 72 * 5 && g.match != MatchState::InProgress; ++i) g.step(1.f / 72, in);
        in[0] = standing(g, 0);
        g.serve(0, g.pl[0].in.head.p + Vec3{0, 3.f, 0}, Vec3{0, -1, 0}, 6.f, {}, false);   // (straight down onto its own head)
        std::vector<GameEvent> log;
        run(g, in, 1.f, &log);
        CHECK(g.match == MatchState::InProgress && g.pl[0].health == 5 && !hasEvent(log, EventType::HitPlayer), "your own fireball never hurts you");
    }
    {   // the ways out (owner, 2026-11-02: "couldn't get away from fireballs"): a fist held low punches it away; a jump
        // lets it pass under; standing still, it hits
        auto shot = [&](int how) {   // 0 stand, 1 a fist low in its way, 2 a step to the side as it comes
            Game g = octGame(tris, 0);
            PlayerInput in[4] = {standing(g, 0), standing(g, 1), {}, {}};
            for (int i = 0; i < 72 * 5 && g.match != MatchState::InProgress; ++i) { in[0] = standing(g, 0); in[1] = standing(g, 1); g.step(1.f / 72, in); }
            in[1] = standing(g, 1);
            for (int i = 0; i < 72 * 3; ++i) { PlayerInput tmp[4] = {in[0], in[1], {}, {}}; g.step(1.f / 72, tmp); }   // (its usual height)
            const Vec3 head = g.pl[1].in.head.p;
            if (how == 1) {   // the right fist at the waist, a little in front, toward the thrower
                in[1].hand[1].grip.p = head + Vec3{-0.45f, -0.75f, 0};
                in[1].hand[1].grip.q = Quat::axisAngle({0, 1, 0}, deg2rad(90)) * Quat::axisAngle({1, 0, 0}, deg2rad(-60));
            }
            const Vec3 from = head + Vec3{-5.f, -0.5f, 0};
            Vec3 dir{1, 0, 0};
            for (float deg = -30; deg <= 20; deg += 2) {   // (the throw that meets the body)
                Ball b; b.scale = 1.f; b.pos = from;
                const Vec3 d{std::cos(deg2rad(deg)), std::sin(deg2rad(deg)), 0};
                b.vel = d * 8.f; b.speedLimit = 8.f * std::cos(deg2rad(deg));
                if (g.fireHitTime(b, head, 2.f) >= 0) { dir = d; break; }
            }
            g.serve(0, from, dir, 8.f, {}, false);
            std::vector<GameEvent> log;
            for (int i = 0; i < 72 * 2; ++i) {
                PlayerInput tmp[4] = {in[0], in[1], {}, {}};
                if (how == 2 && i / 72.f > 0.3f) {   // a step half a metre to the side as it comes
                    tmp[1].head.p.z += 0.5f;
                    for (HandInput& h : tmp[1].hand) h.grip.p.z += 0.5f;
                }
                g.step(1.f / 72, tmp);
                log.insert(log.end(), g.events.begin(), g.events.end());
            }
            return std::make_pair(hasEvent(log, EventType::HitPlayer, 1), hasEvent(log, EventType::KnuckleBlock, 1));
        };
        const auto stand = shot(0), punch = shot(1), step = shot(2);
        CHECK(stand.first, "standing still in its way: hit");
        CHECK(!punch.first && punch.second, "a knuckle guard held in its way punches it away");
        CHECK(!step.first, "a step to the side as it comes: it goes past");
    }
    {   // how many in the room: one ball, the tee waits; RAPID FIRE, up to three, the tee refilling at once
        for (int balls : {1, 4}) {
            Game g = octGame(tris, 0, 5, balls);
            PlayerInput in[4] = {standing(g, 0), {}, {}, {}};
            int most = 0, served = 0;
            float firstAt = -1, refill = -1;   // (the first throw, and how long until the tee had another)
            for (int i = 0; i < 72 * 6; ++i) {
                const Ball& t = g.pl[0].ball;
                if (t.state == BallState::Idle) {
                    if (served == 1 && refill < 0) refill = g.now - firstAt;
                    if (served == 0) firstAt = g.now;
                    g.serve(0, g.idlePoint(0), normalize(Vec3{1, 0.4f, (served % 3 - 1) * 0.3f}), 7.f, {}, false);
                    ++served;
                }
                g.step(1.f / 72, in);
                most = std::max(most, g.fireCount(0));
            }
            char msg[128];
            if (balls == 1) {
                std::snprintf(msg, sizeof msg, "ONE BALL: one in the room at a time; the tee waits until it fizzles (%.1f s)", refill);
                CHECK(most == 1 && refill > 1.f, msg);
            } else {
                std::snprintf(msg, sizeof msg, "RAPID FIRE: up to three in the room, the tee refilled in %.2f s (%d thrown in 6 s)", refill, served);
                CHECK(most == 3 && refill >= 0 && refill < 0.4f && served >= 5, msg);
            }
        }
    }
    {   // a match: you (standing still) and three bots; knockouts, the ring closing in, the last one standing
        Game g = octGame(tris, 0xE, 3);
        g.tun.botLevel = 3;
        PlayerInput in[4];
        int knockouts = 0, closes = 0, hits = 0;
        bool shrank = false, ended = false;
        for (int i = 0; i < 72 * 400 && !ended; ++i) {
            in[0] = standing(g, 0);
            g.step(1.f / 72, in);
            for (const GameEvent& e : g.events) {
                knockouts += e.type == EventType::Knockout; closes += e.type == EventType::RingClose; hits += e.type == EventType::HitPlayer;
                ended |= e.type == EventType::MatchEnd;
            }
            int alive = 0;
            for (int p = 0; p < 4; ++p) alive += g.alive(p);
            if (alive == 2 && g.ringCloseAt < 0 && g.match == MatchState::InProgress) {
                bool two = true;
                for (int p = 0; p < 4; ++p) if (g.alive(p)) two &= std::fabs(length(g.tun.start[p]) - 3.5f) < 1e-3f;
                shrank |= two && std::fabs(g.ringApothem - 5.f) < 1e-3f;
            }
        }
        char msg[128];
        std::snprintf(msg, sizeof msg, "a match plays out: %d hits, %d knocked out, the ring closed %d times, the last one standing wins (%s)",
                      hits, knockouts, closes, g.matchWinner >= 0 ? "player" : "none");
        CHECK(ended && knockouts == 3 && closes == 2 && g.matchWinner >= 0 && g.alive(g.matchWinner) && hits >= 9, msg);
        CHECK(shrank, "with two left, they stand 3.5 m out, the walls closed in");
        CHECK(g.pl[0].health == 0 && g.pl[0].ball.state == BallState::Dead, "you, standing still, are knocked out (and your tee stays empty)");
    }
    {   // the best bots' fireballs: straight, curved and off the walls (owner, 2026-11-02: "only straight balls")
        Game g = octGame(tris, 0xE, 100);
        g.tun.botLevel = kBotLevels - 1;
        g.tun.botIdle = false;   // (BOT IDLE MOTION off: these counts come from fixed seeds; with it each level measures the same: PA_BOT_MEASURE)
        PlayerInput in[4];
        int thrown = 0, curved = 0, banked = 0;
        for (int i = 0; i < 72 * 60; ++i) {
            in[0] = standing(g, 0);
            g.step(1.f / 72, in);
            for (const GameEvent& e : g.events) {
                if (e.type != EventType::Throw || e.player == 0) continue;
                for (const Ball& f : g.fire[e.player]) {
                    if (f.state != BallState::Flying || f.timer > 0.03f) continue;   // (the one just thrown)
                    ++thrown;
                    if (length(f.spin) > 3.f) ++curved;
                    else {   // aimed well off the line to everyone: off a wall
                        float bestDot = -2.f;
                        for (int q = 0; q < 4; ++q) {
                            if (q == e.player || !g.alive(q)) continue;
                            const Vec3 to = g.pl[q].in.head.p - f.pos;
                            bestDot = std::max(bestDot, dot(normalize(Vec3{to.x, 0, to.z}), normalize(Vec3{f.vel.x, 0, f.vel.z})));
                        }
                        banked += bestDot < 0.93f;
                    }
                }
            }
        }
        char msg[128];
        std::snprintf(msg, sizeof msg, "UNBEATABLE's fireballs: %d thrown, %d curved, %d off a wall, the rest straight", thrown, curved, banked);
        CHECK(thrown > 20 && curved * 10 >= thrown && banked * 8 >= thrown && (thrown - curved - banked) * 3 >= thrown, msg);
    }
    {   // a knockout: a ceasefire until the room has closed in (owner, 2026-11-02: no cheap shots while everyone moves)
        Game g = octGame(tris, 0, 1);
        PlayerInput in[4] = {standing(g, 0), standing(g, 1), standing(g, 2), standing(g, 3)};
        for (int i = 0; i < 72 * 5 && g.match != MatchState::InProgress; ++i) g.step(1.f / 72, in);
        auto at = [&](int from, int to) {   // a fireball from one at another, the throw that meets them
            const Vec3 head = g.pl[to].in.head.p, src = g.pl[from].in.head.p + Vec3{0, -0.5f, 0};
            Vec3 flat = normalize(Vec3{head.x - src.x, 0, head.z - src.z}), dir = flat;
            for (float deg = -30; deg <= 20; deg += 2) {
                Ball b; b.scale = 1.f; b.pos = src;
                const Vec3 d = flat * std::cos(deg2rad(deg)) + Vec3{0, std::sin(deg2rad(deg)), 0};
                b.vel = d * 9.f; b.speedLimit = 9.f * std::cos(deg2rad(deg));
                if (g.fireHitTime(b, head, 2.f) >= 0) { dir = d; break; }
            }
            g.parkBall(from);
            g.serve(from, src, dir, 9.f, {}, false);
        };
        at(1, 0);   // (player 0 knocked out)
        std::vector<GameEvent> log;
        for (int i = 0; i < 72 * 2 && g.alive(0); ++i) { g.step(1.f / 72, in); log.insert(log.end(), g.events.begin(), g.events.end()); }
        const bool closing = !g.alive(0) && g.ringCloseAt >= 0;
        at(2, 3);   // a shot during the warning
        log.clear();
        for (int i = 0; i < 72 * 1; ++i) { g.step(1.f / 72, in); log.insert(log.end(), g.events.begin(), g.events.end()); }
        CHECK(closing && g.alive(3) && !hasEvent(log, EventType::HitPlayer), "a knockout: nobody can be hit until the room has closed in");
    }
    {   // at time: the most health wins; a tie at the top goes to sudden death, one hit each
        Game g = octGame(tris, 0, 5);
        g.tun.basicMatchTime = 5.f;
        PlayerInput in[4] = {standing(g, 0), standing(g, 1), standing(g, 2), standing(g, 3)};
        for (int i = 0; i < 72 * 5 && g.match != MatchState::InProgress; ++i) g.step(1.f / 72, in);
        g.pl[2].health = 2; g.pl[3].health = 4;
        std::vector<GameEvent> log;
        run(g, in, 6.f, &log);
        CHECK(g.overtime && g.alive(0) && g.alive(1) && !g.alive(2) && !g.alive(3) && g.pl[0].health == 1 && g.pl[1].health == 1 &&
              hasEvent(log, EventType::Overtime), "at time the most health stays in; tied at the top: sudden death, one hit each");
    }
    {   // FIREBALL BLITZ in teams (0.3.24; the owner, 2026-11-20: "I wanted to select 3 against 1 where I was the 1"): no
        // friendly fire, THE ONE three times the HEALTH, a team wins when the other is all out; with an older player in
        // the room, everyone for themselves
        auto teamGame = [&](int mode, int version) {
            Game g = octGame(tris, 0, 5);
            g.tun.teamMode = mode; g.tun.theOne = 2; g.tun.matchVersion = version;
            PlayerInput in[4] = {standing(g, 0), standing(g, 1), standing(g, 2), standing(g, 3)};
            for (int i = 0; i < 72 * 5 && g.match != MatchState::InProgress; ++i) g.step(1.f / 72, in);
            return g;
        };
        auto shoot = [&](Game& g, int from, int to) {   // a fireball from one at another, the throw that meets them; hit?
            const Vec3 head = g.pl[to].in.head.p, src = g.pl[from].in.head.p + Vec3{0, -0.5f, 0};
            Vec3 flat = normalize(Vec3{head.x - src.x, 0, head.z - src.z}), dir = flat;
            for (float deg = -30; deg <= 20; deg += 2) {
                Ball b; b.scale = 1.f; b.pos = src;
                const Vec3 d = flat * std::cos(deg2rad(deg)) + Vec3{0, std::sin(deg2rad(deg)), 0};
                b.vel = d * 9.f; b.speedLimit = 9.f * std::cos(deg2rad(deg));
                if (g.fireHitTime(b, head, 2.f) >= 0) { dir = d; break; }
            }
            g.parkBall(from);
            g.serve(from, src, dir, 9.f, {}, false);
            PlayerInput in[4] = {standing(g, 0), standing(g, 1), standing(g, 2), standing(g, 3)};
            std::vector<GameEvent> log;
            run(g, in, 2.f, &log);
            return hasEvent(log, EventType::HitPlayer, to);
        };
        {
            Game g = teamGame(TEAMS_2V2, Tunables::kVerAll);
            const bool mates = g.teamsNow() == TEAMS_2V2 && g.team(0) == g.team(1) && g.team(2) == g.team(3) && g.team(0) != g.team(2);
            const bool through = !shoot(g, 0, 1) && g.pl[1].health == 5;
            const bool rival = shoot(g, 0, 2) && g.pl[2].health == 4;
            CHECK(mates && through && rival, "FIREBALL BLITZ 2 v 2: partners across the ring; a teammate's fireball passes, a rival's hits");
        }
        {
            Game g = teamGame(TEAMS_3V1, Tunables::kVerAll);
            CHECK(g.pl[2].health == 15 && g.maxHealth(2) == 15 && g.pl[0].health == 5 && g.pl[1].health == 5 && g.pl[3].health == 5,
                  "FIREBALL BLITZ 3 v 1: THE ONE starts with three times the HEALTH (15), the three with 5");
        }
        {
            Game g = teamGame(TEAMS_2V2, Tunables::kVerAll);
            PlayerInput in[4] = {standing(g, 0), standing(g, 1), standing(g, 2), standing(g, 3)};
            g.pl[1].health = 0; g.pl[3].health = 0;   // (one out on each team)
            run(g, in, 0.5f);
            const bool goesOn = g.match == MatchState::InProgress;
            g.pl[2].health = 0;   // (and the other of that team: it is all out)
            std::vector<GameEvent> log;
            run(g, in, 0.5f, &log);
            CHECK(goesOn && g.match == MatchState::MatchSummary && g.matchWinner == g.team(0) && hasEvent(log, EventType::MatchEnd),
                  "FIREBALL BLITZ in teams: one out on each team, it goes on; a team all out, the other team wins");
        }
        {
            Game g = teamGame(TEAMS_2V2, Tunables::kVerAll);
            g.tun.basicMatchTime = 5.f; g.roundClock = 0.5f;
            g.pl[0].health = 5; g.pl[1].health = 1; g.pl[2].health = 3; g.pl[3].health = 4;   // (6 against 7)
            PlayerInput in[4] = {standing(g, 0), standing(g, 1), standing(g, 2), standing(g, 3)};
            run(g, in, 1.5f);
            CHECK(!g.alive(0) && !g.alive(1) && g.alive(2) && g.alive(3) && g.match == MatchState::MatchSummary && g.matchWinner == g.team(2),
                  "FIREBALL BLITZ in teams, at time: the team with the most health left between them wins");
        }
        {
            Game g = teamGame(TEAMS_3V1, Tunables::kVerBlitzTeams - 1);
            const bool ffa = g.teamsNow() == TEAMS_FFA && g.team(1) == 1 && g.pl[2].health == 5;
            CHECK(ffa && shoot(g, 0, 1), "FIREBALL BLITZ with a player before 0.3.24: everyone for themselves (THE ONE as everyone)");
        }
        {
            Game g = octGame(tris, 0, 5);   // (FOUR CORNERS' teams as before: the same setting, its own room)
            Game c; c.tun.experimental = true; c.tun.expRoom = ROOM_CORNERS; c.tun.players = 4; c.tun.teamMode = TEAMS_3V1;
            c.tun.matchVersion = Tunables::kVerBlitzTeams - 1;
            c.resetWithBots(0);
            CHECK(g.teamsNow() == TEAMS_FFA && c.teamsNow() == TEAMS_3V1 && c.team(c.tun.theOne) == 1,
                  "FIREBALL BLITZ is everyone for themselves unless TEAMS says otherwise; FOUR CORNERS' 3 v 1 as before");
        }
        {   // (review 2026-11-21) a team nobody plays in: everyone for themselves, and the match goes on (it had ended the
            // moment it began, all who played being on one team)
            auto start = [&](int mode, int one, int here) {   // here: the corners with a player (bits)
                Game g = octGame(tris, 0, 5);
                g.tun.teamMode = mode; g.tun.theOne = one; g.tun.matchVersion = Tunables::kVerAll;
                PlayerInput in[4];
                for (int p = 0; p < 4; ++p) if ((here >> p) & 1) in[p] = standing(g, p);
                for (int i = 0; i < 72 * 8 && g.match != MatchState::InProgress; ++i) g.step(1.f / 72, in);
                run(g, in, 1.f);
                return g;
            };
            const Game two = start(TEAMS_2V2, 0, 0x3);    // (corners 0 and 1: both on one team)
            const Game empty = start(TEAMS_3V1, 3, 0x7);  // (THE ONE's corner empty)
            const Game three = start(TEAMS_3V1, 0, 0x7);  // (THE ONE and two of the three)
            const bool waiting = octGame(tris, 0, 5).teamsNow() == TEAMS_FFA;   // (TEAMS off: as before)
            Game w = octGame(tris, 0, 5); w.tun.teamMode = TEAMS_2V2;           // (before the countdown: everyone counts)
            CHECK(two.match == MatchState::InProgress && two.teamsNow() == TEAMS_FFA && !two.friendly(0, 1) &&
                  empty.match == MatchState::InProgress && empty.teamsNow() == TEAMS_FFA && empty.pl[0].health == 5 &&
                  three.match == MatchState::InProgress && three.teamsNow() == TEAMS_3V1 && three.pl[0].health == 15 &&
                  waiting && w.teamsNow() == TEAMS_2V2,
                  "FIREBALL BLITZ in teams with a team nobody plays in: everyone for themselves, and the match goes on");
        }
    }
    {   // the bots dodge, by their level: fireballs at a bot that is standing its ground
        int hit[2] = {0, 0};
        for (int k = 0; k < 2; ++k) {
            Game g = octGame(tris, 0x2, 100);
            g.tun.botLevel = k ? kBotLevels - 1 : 0;
            g.layoutRing(0x3);
            PlayerInput in[4] = {standing(g, 0), {}, {}, {}};
            for (int i = 0; i < 72 * 5 && g.match != MatchState::InProgress; ++i) { in[0] = standing(g, 0); g.step(1.f / 72, in); }
            for (int shot = 0; shot < 20; ++shot) {
                in[0] = standing(g, 0);
                // aimed at the bot as it stands: the pitch that reaches it
                const Vec3 from = g.tun.start[0] + Vec3{0.5f, 1.3f, 0};
                Vec3 flat = normalize(Vec3{g.pl[1].in.head.p.x - from.x, 0, g.pl[1].in.head.p.z - from.z});
                Vec3 best = flat;
                for (float deg = -30; deg <= 10; deg += 2) {
                    Ball b; b.scale = 1.f; b.pos = from;
                    const Vec3 d = flat * std::cos(deg2rad(deg)) + Vec3{0, std::sin(deg2rad(deg)), 0};
                    b.vel = d * 11.f; b.speedLimit = std::max(5.f, 11.f * std::cos(deg2rad(deg)));
                    if (g.fireHitTime(b, g.pl[1].in.head.p, 2.f) >= 0) { best = d; break; }
                }
                if (g.pl[0].ball.state != BallState::Dead) g.parkBall(0);
                g.serve(0, from, best, 11.f, {}, false);
                for (int i = 0; i < 72 * 2; ++i) {
                    in[0] = standing(g, 0);
                    g.step(1.f / 72, in);
                    for (const GameEvent& e : g.events) hit[k] += e.type == EventType::HitPlayer && e.player == 1;
                }
            }
        }
        char msg[96];
        std::snprintf(msg, sizeof msg, "the bots dodge by their level: 20 fireballs, ROOKIE hit %d times, UNBEATABLE %d", hit[0], hit[1]);
        CHECK(hit[1] <= 4 && hit[0] >= hit[1] + 6, msg);
    }
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);   // (unbuffered: a crash shows where it stopped)
    const char* col = argc > 1 ? argv[1] : "app/assets/arena.col";
    auto tris = loadCol(col);
    std::printf("loaded %zu collision tris\n", tris.size());
    CHECK(tris.size() > 100, "arena collision loaded");
    if (const char* m = std::getenv("PA_BOT_MEASURE")) {   // BOT IDLE MOTION (0.3.26): each level hit by a strong
        // player's throws, with its idle motion and without, over many seeds (PA_BOT_MEASURE=seeds; nothing else runs)
        const int seeds = std::max(1, std::atoi(m));
        std::printf("[measure: each level against a strong player, %d seeds x 72 and 90 Hz x 200 throws]\n", seeds);
        for (int adv = 0; adv < 2; ++adv)
            for (int level = 0; level < kBotLevels; ++level) {
                int hits[2] = {0, 0}, throws[2] = {0, 0};
                for (int idle = 0; idle < 2; ++idle) {
                    Game g; g.setArena(tris); g.tun.advanced = adv; g.tun.basicMatchTime = 0; g.tun.meetTime = 0;
                    g.tun.botLevel = level; g.tun.botIdle = idle == 1; g.reset(true);
                    for (int k = 0; k < seeds; ++k)
                        for (float hz : {72.f, 90.f}) {
                            Game g2 = g;
                            g2.rngSeed(0x9E3779B9u * (uint32_t)(k + 1) + 17u * (uint32_t)level);
                            const Attack a = attackBot(g2, 200, hz, 1000u + 7919u * (uint32_t)k);
                            hits[idle] += a.points; throws[idle] += a.throws;
                        }
                }
                std::printf("       %s level %d: without %d hits in %d throws (%.2f%%), with %d in %d (%.2f%%)\n",
                            adv ? "Advanced" : "Basic", level, hits[0], throws[0], 100.0 * hits[0] / std::max(1, throws[0]),
                            hits[1], throws[1], 100.0 * hits[1] / std::max(1, throws[1]));
            }
        return 0;
    }

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
        std::printf("[fist bump start (two people)]\n");
        Game g; g.setArena(tris); g.reset(false);
        PlayerInput in[2] = {standing(g, 0), standing(g, 1)};
        run(g, in, 0.1f);
        CHECK(g.match == MatchState::Meet, "two people meet in the centre before the match");
        // the fronts of the two visible fists 2 cm apart, as players bump in the headset (the grips are ~15 cm apart;
        // the first version measured the grips, the second a point 13.7 cm out, past the real knuckles)
        auto placeFist = [&](PlayerInput& pi, const Vec3& at) {
            pi.hand[1].grip.p = at - pi.hand[1].grip.q.rotate(g.tun.fistFront);
        };
        in[0].hand[1].trigger = 0.3f; placeFist(in[0], {-0.01f, 1.2f, 0.f});
        in[1].hand[1].trigger = 0.3f; placeFist(in[1], {0.01f, 1.2f, 0.f});
        CHECK(length(in[0].hand[1].grip.p - in[1].hand[1].grip.p) > 0.12f, "(test setup: fists front to front, grips 15 cm apart)");
        run(g, in, 0.2f);
        CHECK(g.match == MatchState::Meet, "fists together, neither trigger pulled: no bump");
        // the meeting point (owner, 2026-10-28): one offers, the other meets it there
        placeFist(in[0], {-0.30f, 1.2f, 0.f});
        in[1].hand[1].trigger = 1;   // orange offers: a closed fist held in its ring
        run(g, in, 0.3f);
        CHECK(g.meetLit(1, 1) && !g.meetLit(0, 1) && g.match == MatchState::Meet, "a closed fist in its ring lights it; nothing yet");
        std::vector<GameEvent> log;
        placeFist(in[0], {-0.01f, 1.2f, 0.f});   // blue meets it (trigger half pulled: one trigger held is enough)
        run(g, in, 0.05f, &log);
        CHECK(hasEvent(log, EventType::FistBump) && g.match == MatchState::Meet, "fist bump (one trigger held): confirmed first (buzz, sound)");
        log.clear();
        run(g, in, g.tun.bumpPause + 0.05f, &log);
        int bumps = 0;
        for (const GameEvent& e : log) bumps += e.type == EventType::FistBump;
        CHECK(g.match == MatchState::Countdown && bumps == 0, "... then the countdown, once (holding the fists together does not repeat it)");
        bool named = true;
        for (int t = 0; t <= (int)EventType::LeadChange; ++t) named &= std::strcmp(eventName((EventType)t), "?") != 0;
        CHECK(named, "every event has a name (logs and sounds read it: a missing one crashed both headsets)");
        Game g2; g2.setArena(tris); g2.reset(false);
        PlayerInput in2[2] = {standing(g2, 0), standing(g2, 1)};
        run(g2, in2, 0.1f);
        run(g2, in2, g2.tun.meetTime + 0.1f);
        CHECK(g2.match == MatchState::Countdown, "no bump: the countdown starts by itself");
        Game g3; g3.setArena(tris); g3.reset(true);
        PlayerInput in3[2] = {standing(g3, 0), {}};
        run(g3, in3, 0.1f);
        CHECK(g3.match == MatchState::Countdown, "against the bot: no meeting");
        {   // nobody can force the start: away from the meeting point, or a fist slammed in before either was offered
            Game g4; g4.setArena(tris); g4.reset(false);
            PlayerInput in4[2] = {standing(g4, 0), standing(g4, 1)};
            run(g4, in4, 0.1f);
            auto place = [&](PlayerInput& pi, const Vec3& at) { pi.hand[1].grip.p = at - pi.hand[1].grip.q.rotate(g4.tun.fistFront); };
            in4[0].hand[1].trigger = in4[1].hand[1].trigger = 1;
            place(in4[0], {-0.01f, 1.2f, 1.2f}); place(in4[1], {0.01f, 1.2f, 1.2f});   // fists together, off to the side
            std::vector<GameEvent> log4;
            run(g4, in4, 1.0f, &log4);
            CHECK(!hasEvent(log4, EventType::FistBump), "fists together away from the meeting point: no bump");
            in4[0].hand[1].trigger = in4[1].hand[1].trigger = 0;
            place(in4[0], {-0.30f, 1.2f, 0.f}); place(in4[1], {0.01f, 1.2f, 0.f});   // orange's fist in its ring, not closed
            run(g4, in4, 0.3f, &log4);
            in4[0].hand[1].trigger = 1;
            place(in4[0], {-0.01f, 1.2f, 0.f});   // blue slams a closed fist into it at once
            log4.clear();
            run(g4, in4, 0.1f, &log4);
            CHECK(!hasEvent(log4, EventType::FistBump), "a fist slammed into one that was not offered: no bump");
        }
        {   // the bot: meets a fist you offer; offers its own when you don't; never reaches for a fist elsewhere
            auto meetBot = [&](int mode, float& took) {
                Game g; g.setArena(tris); g.tun.botMeet = true; g.tun.botLevel = 2; g.reset(true);
                PlayerInput pin[2] = {standing(g, 0), {}};
                Quat fq = kFaceP0;
                auto place = [&](const Vec3& at) { pin[0].hand[1].grip = {fq, at - fq.rotate(g.tun.fistFront)}; };
                took = -1;
                bool bumped = false;
                for (int i = 0; i < 72 * 8 && !bumped; ++i) {
                    const float t = i / 72.f;
                    pin[0].head.p = {-g.tun.meetSpot, 1.65f, 0};   // standing at its spot in the centre
                    if (mode == 0) { pin[0].hand[1].trigger = 1; place(g.meetRing(0) + Vec3{0.03f, 0, 0}); }   // offer
                    else if (mode == 1) {   // wait, then bring the fist to the bot's offered one
                        const Vec3 bf = g.pl[1].in.hand[0].grip.transform(g.tun.fistFront), bf1 = g.pl[1].in.hand[1].grip.transform(g.tun.fistFront);
                        const bool lit0 = g.meetLit(1, 0), lit1 = g.meetLit(1, 1);
                        pin[0].hand[1].trigger = 1;
                        if (lit0 || lit1) place((lit1 ? bf1 : bf) + Vec3{-0.02f, 0, 0}); else place({-0.6f, 1.0f, 0.3f});
                    } else { pin[0].hand[1].trigger = 1; place({-0.05f, 1.2f, 1.0f}); }   // a fist held out, off to the side
                    PlayerInput tmp[2] = {pin[0], pin[1]};
                    g.step(1.f / 72, tmp);
                    if (g.match == MatchState::Meet || hasEvent(g.events, EventType::FistBump)) took = t;
                    bumped = hasEvent(g.events, EventType::FistBump);
                }
                return bumped;
            };
            float took;
            CHECK(meetBot(0, took) && took < 2.5f, "the bot meets a fist you offer in your ring");
            CHECK(meetBot(1, took), "it offers its own fist when you don't, and you can meet it");
            CHECK(!meetBot(2, took), "it never reaches for a fist away from the meeting point");
            {   // its fist held out, waiting: its body holds still (a sway folded the arm into the torso, 2026-10-29)
                Game g; g.setArena(tris); g.tun.botMeet = true; g.tun.botLevel = 2; g.tun.meetTime = 30.f; g.reset(true);
                PlayerInput pin[2] = {standing(g, 0), {}};
                pin[0].head.p = {-g.tun.meetSpot, 1.65f, 0};
                float lo = 9.f, hi = -9.f;
                for (int i = 0; i < 72 * 8; ++i) {
                    PlayerInput tmp[2] = {pin[0], pin[1]};
                    g.step(1.f / 72, tmp);
                    if (i > 72 * 4) { lo = std::min(lo, g.pl[1].in.head.p.z); hi = std::max(hi, g.pl[1].in.head.p.z); }
                }
                CHECK(g.meetLit(1, 0) || g.meetLit(1, 1), "(it is offering its fist)");
                CHECK(hi - lo < 0.01f, "while it offers, its body does not sway");
            }
        }
    }
    {
        std::printf("[EXPERIMENTAL arena CORNERS: the diagonal room, floating platforms]\n");
        auto expGame = [&](PlayerInput in[2]) {
            Game g; g.setArena(tris); g.tun.meetTime = 0; g.useArena(ARENA_CORNERS, tris); g.reset(false);
            in[0] = standing(g, 0); in[1] = standing(g, 1);
            run(g, in, 3.6f);
            return g;
        };
        auto fire = [&](Game& g, PlayerInput in[2], Vec3 from, Vec3 vel, float seconds, std::vector<GameEvent>& log, float* lowest = nullptr) {
            Ball& b = g.pl[0].ball;
            b.state = BallState::Flying; b.instigator = 0; b.rechargePending = true; b.scored = false; b.spin = {};
            b.pos = from; b.vel = vel; b.speedLimit = length(vel); b.timer = 0; b.progX = from.x; b.progT = 0; b.bounces = 0;
            b.fouled = b.homing = false; b.heldHand = -1;
            for (int i = 0; i < int(72 * seconds); ++i) {
                PlayerInput tmp[2] = {in[0], in[1]};
                g.step(1.f / 72, tmp);
                log.insert(log.end(), g.events.begin(), g.events.end());
                if (lowest) *lowest = std::min(*lowest, b.pos.y);
                if (b.state != BallState::Flying && b.state != BallState::Returning) break;
            }
        };
        {
            PlayerInput in[2]; Game g = expGame(in);
            CHECK(g.arenaKind == 1 && g.tun.foulLineX > 5.f && g.tun.outLow < exparena::kFloor, "the EXPERIMENTAL arena: its collision, foul line and bounds");
            std::vector<GameEvent> log;
            const Vec3 from = g.pl[0].ball.pos;
            fire(g, in, from, normalize(g.chestPoint(1) - from) * 14.f, 3.f, log);
            CHECK(hasEvent(log, EventType::HitPlayer, 1), "a throw straight at the other player hits them, corner to corner");
        }
        {
            PlayerInput in[2]; Game g = expGame(in);
            std::vector<GameEvent> log;
            float lowest = 9.f;
            fire(g, in, {-4.0f, 0.8f, 0.f}, normalize(Vec3{1.f, -0.9f, 0.1f}) * 12.f, 2.5f, log, &lowest);
            bool up = false;
            for (const GameEvent& e : log) up |= e.type == EventType::Bounce && e.pos.y < exparena::kFloor + 0.3f;
            CHECK(up && lowest < exparena::kFloor + 0.5f && !hasEvent(log, EventType::SpawnRise),
                  "past the platforms the ball drops to the floor below and bounces back up (no respawn)");
        }
        {
            PlayerInput in[2]; Game g = expGame(in);
            std::vector<GameEvent> log;
            fire(g, in, g.pl[0].ball.pos, normalize(Vec3{0.3f, -1.f, 0.4f}) * 6.f, 3.f, log);
            CHECK(hasEvent(log, EventType::Foul, 0) && g.pl[0].ball.state == BallState::Idle, "your throw onto your own platform: a foul, back to your start");
        }
        {
            PlayerInput in[2]; Game g = expGame(in);
            std::vector<GameEvent> log;
            fire(g, in, {-5.5f, 1.4f, 0.f}, normalize(Vec3{1.f, 0.05f, 0.06f}) * 14.f, 3.f, log);   // over their shoulder into their corner
            bool fromTheirCorner = false;
            for (const GameEvent& e : log) fromTheirCorner |= e.type == EventType::Returned && e.player == 0 && e.pos.x > exparena::kBackX;
            CHECK(fromTheirCorner || hasEvent(log, EventType::HitPlayer, 1), "the corner behind the other player is their back wall (the ball returns)");
        }
        {   // many random throws: every one ends (hit, return, foul, home), nothing escapes or goes on for ever
            PlayerInput in[2]; Game g = expGame(in);
            uint32_t seed = 777;
            auto rnd = [&]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return (seed & 0xFFFFFF) / float(0xFFFFFF); };
            int ended = 0, escaped = 0, n = 600;
            for (int k = 0; k < n; ++k) {
                g.match = MatchState::InProgress; g.roundClock = 1e6f;
                g.pl[0].score = g.pl[1].score = 0; g.pl[0].strikesAgainst = g.pl[1].strikesAgainst = 0;
                const float yaw = (rnd() - 0.5f) * 3.f, pitch = (rnd() - 0.5f) * 1.8f;
                const Vec3 d{std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw)};
                std::vector<GameEvent> log;
                Ball& b = g.pl[0].ball;
                fire(g, in, g.idlePoint(0), d * (2.f + rnd() * 17.f), 25.f, log);   // (a block restarts its 10 s)
                const bool fine = std::isfinite(b.pos.x) && std::fabs(b.pos.x) < g.tun.outX && std::fabs(b.pos.z) < g.tun.outZ;
                ended += b.state != BallState::Flying && b.state != BallState::Returning;
                escaped += !fine;
            }
            std::printf("       %d random throws: %d ended within 25 s, %d left the room\n", n, ended, escaped);
            CHECK(ended == n && escaped == 0, "every throw ends, and none leaves the room");
        }
        {   // the bot plays there: two bots, a minute and a half
            Game g; g.setArena(tris); g.useArena(ARENA_CORNERS, tris); g.reset(true);
            g.pl[0].isBot = true;
            PlayerInput in[2] = {standing(g, 0), {}};
            std::vector<GameEvent> log;
            run(g, in, 90.f, &log);
            int throws[2] = {0, 0}, nan = 0, pts = 0;
            for (const GameEvent& e : log) { if (e.type == EventType::Throw && e.player >= 0) ++throws[e.player]; pts += e.type == EventType::HitPlayer; }
            for (int p = 0; p < 2; ++p) nan += !std::isfinite(g.pl[p].ball.pos.x) || !std::isfinite(g.pl[p].in.head.p.x);
            std::printf("       bots in the EXPERIMENTAL arena: %d and %d throws, %d points\n", throws[0], throws[1], pts);
            CHECK(throws[0] > 3 && throws[1] > 3 && nan == 0 && pts > 0, "the bot plays in the EXPERIMENTAL arena (throws, scores)");
        }
        {
            PlayerInput in[2]; Game g = expGame(in);
            g.useArena(ARENA_CLASSIC, tris);
            CHECK(g.arenaKind == 0 && std::fabs(g.tun.foulLineX - 4.73f) < 1e-4f && g.tun.outZ < 5.f, "and back to the classic court");
        }
    }
    {
        std::printf("[EXPERIMENTAL arena DIAMOND: the classic court rolled 45 degrees, floating platforms]\n");
        auto rolled = [&](PlayerInput in[2]) {
            Game g; g.setArena(tris); g.tun.meetTime = 0; g.useArena(ARENA_DIAMOND, tris); g.reset(false);
            in[0] = standing(g, 0); in[1] = standing(g, 1);
            run(g, in, 3.6f);
            return g;
        };
        auto fire = [&](Game& g, PlayerInput in[2], Vec3 from, Vec3 vel, float seconds, std::vector<GameEvent>& log) {
            Ball& b = g.pl[0].ball;
            b.state = BallState::Flying; b.instigator = 0; b.rechargePending = true; b.scored = false; b.spin = {};
            b.pos = from; b.vel = vel; b.speedLimit = length(vel); b.timer = 0; b.progX = from.x; b.progT = 0; b.bounces = 0;
            b.fouled = b.homing = false; b.heldHand = -1;
            for (int i = 0; i < int(72 * seconds); ++i) {
                PlayerInput tmp[2] = {in[0], in[1]};
                g.step(1.f / 72, tmp);
                log.insert(log.end(), g.events.begin(), g.events.end());
                if (b.state != BallState::Flying && b.state != BallState::Returning) break;
            }
        };
        {
            PlayerInput in[2]; Game g = rolled(in);
            CHECK(g.arenaKind == ARENA_DIAMOND && std::fabs(g.tun.foulLineX - 4.73f) < 1e-4f && g.tun.outLow < -1.5f,
                  "the DIAMOND arena: its collision, the classic foul line, its bounds");
            std::vector<GameEvent> log;
            const Vec3 from = g.pl[0].ball.pos;
            fire(g, in, from, normalize(g.chestPoint(1) - from) * 14.f, 3.f, log);
            CHECK(hasEvent(log, EventType::HitPlayer, 1), "a throw straight at the other player hits them");
        }
        {
            PlayerInput in[2]; Game g = rolled(in);
            std::vector<GameEvent> log;
            fire(g, in, {-3.0f, 0.6f, 0.f}, normalize(Vec3{1.f, -1.2f, 0.3f}) * 11.f, 2.5f, log);   // down past the platforms
            bool low = false;
            for (const GameEvent& e : log) low |= e.type == EventType::Bounce && e.pos.y < -0.6f;
            CHECK(low && !hasEvent(log, EventType::SpawnRise) && !hasEvent(log, EventType::Foul), "the V below the platforms: the ball bounces back up");
        }
        {
            PlayerInput in[2]; Game g = rolled(in);
            std::vector<GameEvent> log;
            fire(g, in, g.pl[0].ball.pos, normalize(Vec3{0.3f, -1.f, 0.4f}) * 6.f, 3.f, log);
            bool offPlatform = false;
            for (const GameEvent& e : log)
                offPlatform |= e.type == EventType::Bounce && std::fabs(e.pos.y) < 0.25f && length(Vec3{e.pos.x + 7.f, 0, e.pos.z}) < exparena::kPlatformR;
            CHECK(!offPlatform, "the ball goes through the platforms (it can come up at you from below)");
            Game gs; gs.setArena(tris); gs.tun.meetTime = 0; gs.tun.solidPlatforms = true; gs.useArena(ARENA_DIAMOND, tris); gs.reset(false);
            PlayerInput ins[2] = {standing(gs, 0), standing(gs, 1)};
            run(gs, ins, 3.6f);
            std::vector<GameEvent> logs;
            fire(gs, ins, gs.pl[0].ball.pos, normalize(Vec3{0.3f, -1.f, 0.4f}) * 6.f, 3.f, logs);
            bool bounced = false;
            for (const GameEvent& e : logs) bounced |= e.type == EventType::Bounce && std::fabs(e.pos.y) < 0.25f;
            CHECK(bounced && !hasEvent(logs, EventType::Foul), "solid platforms (an option): the ball bounces off them, no foul");
            PlayerInput in2[2]; Game g2 = rolled(in2);
            std::vector<GameEvent> log2;
            fire(g2, in2, g2.pl[0].ball.pos, normalize(Vec3{0.2f, 0.9f, 1.f}) * 8.f, 3.f, log2);   // up into a slanted wall of your end
            CHECK(!hasEvent(log2, EventType::Foul), "no foul zone in DIAMOND: a throw into your own end's walls just bounces");
        }
        {
            PlayerInput in[2]; Game g = rolled(in);
            std::vector<GameEvent> log;
            fire(g, in, {-4.0f, 2.2f, 0.f}, normalize(Vec3{1.f, 0.02f, 0.03f}) * 15.f, 3.f, log);   // over their head into their end
            bool back = false;
            for (const GameEvent& e : log) back |= e.type == EventType::Returned && e.player == 0 && e.pos.x > 9.f;
            CHECK(back || hasEvent(log, EventType::HitPlayer, 1), "the far end is their back wall (the ball returns)");
        }
        {
            PlayerInput in[2]; Game g = rolled(in);
            uint32_t seed = 4242;
            auto rnd = [&]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return (seed & 0xFFFFFF) / float(0xFFFFFF); };
            int ended = 0, escaped = 0, n = 600;
            for (int k = 0; k < n; ++k) {
                g.match = MatchState::InProgress; g.roundClock = 1e6f;
                g.pl[0].score = g.pl[1].score = 0; g.pl[0].strikesAgainst = g.pl[1].strikesAgainst = 0;
                const float yaw = (rnd() - 0.5f) * 3.f, pitch = (rnd() - 0.5f) * 1.8f;
                const Vec3 d{std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw)};
                std::vector<GameEvent> log;
                Ball& b = g.pl[0].ball;
                fire(g, in, g.idlePoint(0), d * (2.f + rnd() * 17.f), 25.f, log);
                ended += b.state != BallState::Flying && b.state != BallState::Returning;
                escaped += !std::isfinite(b.pos.x) || std::fabs(b.pos.x) > g.tun.outX || std::fabs(b.pos.z) > g.tun.outZ;
            }
            std::printf("       %d random throws: %d ended within 25 s, %d left the room\n", n, ended, escaped);
            CHECK(ended == n && escaped == 0, "every throw ends, and none leaves the room");
        }
        {
            Game g; g.setArena(tris); g.useArena(ARENA_DIAMOND, tris); g.reset(true);
            g.pl[0].isBot = true;
            PlayerInput in[2] = {standing(g, 0), {}};
            std::vector<GameEvent> log;
            run(g, in, 90.f, &log);
            int throws[2] = {0, 0}, nan = 0, pts = 0;
            for (const GameEvent& e : log) { if (e.type == EventType::Throw && e.player >= 0) ++throws[e.player]; pts += e.type == EventType::HitPlayer; }
            for (int p = 0; p < 2; ++p) nan += !std::isfinite(g.pl[p].ball.pos.x) || !std::isfinite(g.pl[p].in.head.p.x);
            std::printf("       bots in the DIAMOND arena: %d and %d throws, %d points\n", throws[0], throws[1], pts);
            CHECK(throws[0] > 3 && throws[1] > 3 && nan == 0 && pts > 0, "the bot plays in the DIAMOND arena (throws, scores)");
        }
    }
    {
        std::printf("[FOUR CORNERS: four players, teams]\n");
        // a person standing at corner p, facing the centre
        auto stand4 = [&](const Game& g, int p) {
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
        };
        auto make4 = [&](int mode, uint32_t bots, PlayerInput in[4]) {
            Game g; g.setArena(tris); g.tun.players = 4; g.tun.teamMode = mode; g.tun.meetTime = 0;
            g.useArena(ARENA_FOURCORNERS, tris); g.resetWithBots(bots);
            for (int p = 0; p < 4; ++p) in[p] = stand4(g, p);
            for (int i = 0; i < int(72 * 3.6f); ++i) { PlayerInput tmp[4] = {in[0], in[1], in[2], in[3]}; g.step(1.f / 72, tmp); }
            return g;
        };
        auto fire = [&](Game& g, PlayerInput in[4], int owner, Vec3 from, Vec3 vel, float seconds, std::vector<GameEvent>& log) {
            Ball& b = g.pl[owner].ball;
            b.state = BallState::Flying; b.instigator = owner; b.rechargePending = true; b.scored = false; b.spin = {};
            b.pos = from; b.vel = vel; b.speedLimit = length(vel); b.timer = 0; b.progX = from.x; b.progZ = from.z; b.progT = 0;
            b.bounces = 0; b.fouled = b.homing = false; b.heldHand = -1;
            for (int i = 0; i < int(72 * seconds); ++i) {
                PlayerInput tmp[4] = {in[0], in[1], in[2], in[3]};
                g.step(1.f / 72, tmp);
                log.insert(log.end(), g.events.begin(), g.events.end());
                if (b.state != BallState::Flying && b.state != BallState::Returning) break;
            }
        };
        {
            PlayerInput in[4]; Game g = make4(TEAMS_FFA, 0, in);
            CHECK(g.np == 4 && g.match == MatchState::InProgress && g.arenaKind == ARENA_FOURCORNERS, "four players in FOUR CORNERS: the match starts (no fist bump)");
            bool facesIn = true;
            for (int p = 0; p < 4; ++p) facesIn &= dot(g.facing(p), Vec3{0, 0, 0} - g.tun.start[p]) > 6.9f;
            CHECK(facesIn && g.teamCount() == 4, "each faces the centre; free-for-all: four teams");
        }
        {   // 2 v 2: partners in opposite corners
            PlayerInput in[4]; Game g = make4(TEAMS_2V2, 0, in);
            CHECK(g.team(0) == g.team(1) && g.team(2) == g.team(3) && g.team(0) != g.team(2), "2 v 2: partners in opposite corners");
            std::vector<GameEvent> log;
            const Vec3 from = g.tun.start[0] + Vec3{1.6f, 1.3f, 0};
            fire(g, in, 0, from, normalize(g.chestPoint(1) - from) * 12.f, 2.f, log);
            CHECK(!hasEvent(log, EventType::HitPlayer), "a ball thrown at your partner goes through them");
            PlayerInput in2[4]; Game g2 = make4(TEAMS_2V2, 0, in2);
            std::vector<GameEvent> log2;
            const Vec3 from2 = g2.tun.start[0] + Vec3{1.6f, 1.3f, 0};
            fire(g2, in2, 0, from2, normalize(g2.chestPoint(2) - from2) * 12.f, 2.f, log2);
            CHECK(hasEvent(log2, EventType::HitPlayer, 2) && g2.teamScore(0) == 1 && g2.teamScore(1) == 0, "a hit on a rival scores for your team");
        }
        {   // 3 v 1: THE ONE's hits count double
            PlayerInput in[4]; Game g = make4(TEAMS_3V1, 0, in);
            CHECK(g.team(0) != g.team(1) && g.team(1) == g.team(2) && g.team(2) == g.team(3), "3 v 1: THE ONE (slot 0) against the other three");
            std::vector<GameEvent> log;
            const Vec3 from = g.tun.start[0] + Vec3{1.6f, 1.3f, 0};
            fire(g, in, 0, from, normalize(g.chestPoint(3) - from) * 12.f, 2.f, log);
            CHECK(hasEvent(log, EventType::HitPlayer, 3) && g.pl[0].score == 2, "THE ONE's hit counts two");
        }
        {   // the clock: the most points wins; a tie, sudden death
            PlayerInput in[4]; Game g = make4(TEAMS_FFA, 0, in);
            g.pl[2].score = 3; g.pl[0].score = 1; g.roundClock = 0.01f;
            for (int i = 0; i < 3; ++i) { PlayerInput tmp[4] = {in[0], in[1], in[2], in[3]}; g.step(1.f / 72, tmp); }
            CHECK(g.match == MatchState::MatchSummary && g.matchWinner == 2, "at time, the player with the most points wins");
            PlayerInput in2[4]; Game g2 = make4(TEAMS_FFA, 0, in2);
            g2.pl[1].score = 2; g2.pl[3].score = 2; g2.roundClock = 0.01f;
            for (int i = 0; i < 3; ++i) { PlayerInput tmp[4] = {in2[0], in2[1], in2[2], in2[3]}; g2.step(1.f / 72, tmp); }
            CHECK(g2.match == MatchState::InProgress && g2.overtime, "a tie at the top: sudden death");
        }
        {   // a new leader on its own: the lead-change cue (not on a tie, not on the deciding point)
            auto leads = [](const std::vector<GameEvent>& log, float& to) {
                int n = 0;
                for (const GameEvent& e : log) if (e.type == EventType::LeadChange) { ++n; to = e.value; }
                return n;
            };
            PlayerInput in[4]; Game g = make4(TEAMS_FFA, 0, in);
            std::vector<GameEvent> log;
            const Vec3 from = g.tun.start[0] + Vec3{1.6f, 1.3f, 0};
            fire(g, in, 0, from, normalize(g.chestPoint(2) - from) * 12.f, 2.f, log);
            float to = -1;
            CHECK(hasEvent(log, EventType::HitPlayer, 2) && leads(log, to) == 1 && to == 0.f && g.leader() == 0, "the first point: a new leader, the cue");
            PlayerInput in2[4]; Game g2 = make4(TEAMS_FFA, 0, in2);
            g2.pl[3].score = 1;
            std::vector<GameEvent> log2;
            fire(g2, in2, 0, from, normalize(g2.chestPoint(2) - from) * 12.f, 2.f, log2);
            CHECK(hasEvent(log2, EventType::HitPlayer, 2) && leads(log2, to) == 0 && g2.leader() == -1, "level at the top: no cue");
            PlayerInput in3[4]; Game g3 = make4(TEAMS_3V1, 0, in3);
            g3.pl[2].score = 1;
            std::vector<GameEvent> log3;
            fire(g3, in3, 0, from, normalize(g3.chestPoint(3) - from) * 12.f, 2.f, log3);
            CHECK(leads(log3, to) == 1 && to == float(g3.team(0)) && g3.leader() == g3.team(0), "THE ONE's double overtakes the three: the cue");
            PlayerInput in4[4]; Game g4 = make4(TEAMS_2V2, 0, in4);
            g4.overtime = true;
            std::vector<GameEvent> log4;
            const Vec3 from4 = g4.tun.start[1] + Vec3{-1.6f, 1.3f, 0};
            fire(g4, in4, 1, from4, normalize(g4.chestPoint(2) - from4) * 12.f, 2.f, log4);
            CHECK(g4.match == MatchState::MatchSummary && g4.matchWinner == 0 && hasEvent(log4, EventType::MatchEnd, 0) && leads(log4, to) == 0,
                  "sudden death: the match ends (the event names the winning team), no lead cue");
        }
        {   // every corner is its player's back wall; past your platform's edge a throw recharges
            PlayerInput in[4]; Game g = make4(TEAMS_FFA, 0, in);
            std::vector<GameEvent> log;
            const Vec3 from{-2.f, 3.0f, -3.f};
            fire(g, in, 0, from, normalize(Vec3{1.6f, 3.2f, -9.2f} - from) * 13.f, 2.f, log);
            bool back = false;
            for (const GameEvent& e : log) back |= e.type == EventType::Returned && e.player == 0 && e.pos.z < -7.f;
            CHECK(back, "a ball into another player's corner comes back to its thrower");
        }
        {   // BOUNCES IN FOUR CORNERS (0.3.26): the walls between the corners (z = x + 9.5 between corners 0 and 3's)
            auto wallHit = [&](bool lively, bool grip, int version, float& speed, float& spin) {
                PlayerInput in[4]; Game g = make4(TEAMS_FFA, 0, in);
                g.tun.experimental = true; g.tun.expRoom = ROOM_CORNERS;
                g.tun.wallsLively = lively; g.tun.wallGrip = grip; g.tun.matchVersion = version;
                const Vec3 n{-0.70710678f, 0, 0.70710678f}, along{0.70710678f, 0, 0.70710678f};   // (out of the room; along the wall)
                const Vec3 from = Vec3{-4.75f, 1.5f, 4.75f} - n * 1.f - along * 1.7f;
                const float a = deg2rad(30.f);
                Ball& b = g.pl[0].ball;
                b.state = BallState::Flying; b.instigator = 0; b.rechargePending = false; b.scored = false; b.spin = {};
                b.pos = from; b.vel = (along * std::cos(a) + n * std::sin(a)) * 10.f; b.speedLimit = 10.f; b.timer = 0;
                b.progX = from.x; b.progZ = from.z; b.progT = 0; b.bounces = 0; b.fouled = b.homing = false; b.heldHand = -1;
                speed = spin = -1.f;
                for (int i = 0; i < 72; ++i) {
                    PlayerInput tmp[4] = {in[0], in[1], in[2], in[3]};
                    g.step(1.f / 72, tmp);
                    if (hasEvent(g.events, EventType::Bounce, 0)) { speed = g.pl[0].ball.speedLimit; spin = length(g.pl[0].ball.spin); break; }
                }
            };
            float s0, w0, s1, w1, s2, w2, s3, w3;
            wallHit(false, false, Tunables::kVerAll, s0, w0);
            wallHit(true, false, Tunables::kVerAll, s1, w1);
            wallHit(false, true, Tunables::kVerAll, s2, w2);
            wallHit(true, true, 3025, s3, w3);
            std::printf("       (FOUR CORNERS, a 30 degree bank at 10 m/s: STANDARD %.2f m/s; LIVELY %.2f; GRIP %.2f spinning %.1f rad/s; "
                        "a 0.3.25 player in it %.2f)\n", s0, s1, s2, w2, s3);
            CHECK(std::fabs(s0 - 9.f) < 0.02f && std::fabs(s1 - 9.926f) < 0.03f && std::fabs(s2 - 8.93f) < 0.05f && w2 > 20.f,
                  "BOUNCES IN FOUR CORNERS: its walls between the corners: LIVELY keeps 9.93 m/s of 10, WALL GRIP grips and spins it");
            CHECK(std::fabs(s3 - 9.f) < 0.02f && w3 < 0.01f, "... a player on 0.3.25 in the match: STANDARD");
            // BANK ASSIST: player 0's bank off that wall toward player 3, half a metre wide of their chest
            auto banked4 = [&](int level, int version, int mode, int theOne, float& closest, float& turnDeg) {
                PlayerInput in[4]; Game g = make4(mode, 0, in);
                g.tun.experimental = true; g.tun.expRoom = ROOM_CORNERS; g.tun.aimAssist = 0; g.tun.theOne = theOne;
                g.tun.bankAssist = level; g.tun.matchVersion = version;
                const Vec3 chest = g.chestPoint(3), wide = chest + Vec3{0.5f, 0, 0};
                const float c = 9.5f - g.ballRadius(0) * 1.41421356f;   // (the wall, for the ball's centre)
                const Vec3 mirror{wide.z - c, wide.y, wide.x + c};      // (seen in the wall z = x + c)
                const Vec3 from = g.tun.start[0] + Vec3{0.6f, 1.3f, 0};
                g.serve(0, from, normalize(mirror - from), 10.f, {}, true);
                closest = 1e9f; turnDeg = 0.f;
                bool bounced = false;
                for (int i = 0; i < 72 * 3; ++i) {
                    PlayerInput tmp[4] = {in[0], in[1], in[2], in[3]};
                    g.step(1.f / 72, tmp);
                    const Ball& b = g.pl[0].ball;
                    if (!bounced && hasEvent(g.events, EventType::Bounce, 0)) { bounced = true; turnDeg = rad2deg(length(b.bankTurn)); }
                    if (b.state != BallState::Flying) break;
                    if (bounced) closest = std::min(closest, length(b.pos - chest));
                }
            };
            float c0, t0, c3, t3, cOld, tOld, cMate, tMate;
            banked4(0, Tunables::kVerAll, TEAMS_FFA, 0, c0, t0);
            banked4(3, Tunables::kVerAll, TEAMS_FFA, 0, c3, t3);
            banked4(3, 3025, TEAMS_FFA, 0, cOld, tOld);
            banked4(3, Tunables::kVerAll, TEAMS_3V1, 1, cMate, tMate);   // (THE ONE player 1: 0 and 3 on the same team)
            std::printf("       (FOUR CORNERS, a bank 0.5 m wide of a rival's chest: OFF passes %.2f m from it; HIGH turns it %.2f deg and "
                        "passes %.2f m; a teammate's: %.2f deg)\n", c0, t3, c3, tMate);
            // (a shorter leg than the SQUARE's, about 5 m: the 2.4 degrees at most move it less)
            CHECK(t0 == 0.f && t3 > 0.5f && t3 <= 2.4f + 1e-3f && c3 < c0 - 0.05f,
                  "BANK ASSIST IN FOUR CORNERS: a bank's last leg turned toward the rival it passes nearest, and passes nearer");
            CHECK(tOld == 0.f && tMate == 0.f, "... none with a player on 0.3.25 in the match, and never toward a teammate");
            PlayerInput in2[4]; Game g2 = make4(TEAMS_FFA, 0, in2);
            std::vector<GameEvent> log2;
            g2.pl[2].ball.charged = false;
            fire(g2, in2, 2, g2.idlePoint(2), Vec3{0.3f, 0.1f, 1.f} * 10.f, 0.4f, log2);
            CHECK(!g2.pl[2].ball.rechargePending && g2.pl[2].ball.charged, "a corner player's throw past the platform's edge recharges");
            PlayerInput in3[4]; Game g3 = make4(TEAMS_FFA, 0, in3);
            in3[2].head.p.x += 1.6f;   // standing aside: the ring behind them is open
            for (int i = 0; i < 4; ++i) { PlayerInput tmp[4] = {in3[0], in3[1], in3[2], in3[3]}; g3.step(1.f / 72, tmp); }
            std::vector<GameEvent> log3;
            fire(g3, in3, 0, {0, 1.28f, -3.f}, Vec3{0, 0, -12.f}, 0.6f, log3);
            CHECK(hasEvent(log3, EventType::Strike, 2) && g3.pl[0].strikesFor == 1, "through a corner player's strike ring: a strike, held by the thrower");
        }
        for (int mode = 0; mode < 3; ++mode) {   // four bots, a match each way
            Game g; g.setArena(tris); g.tun.players = 4; g.tun.teamMode = mode; g.tun.basicMatchTime = 0; g.tun.botLevel = 1;
            g.tun.botIdle = false;   // (BOT IDLE MOTION off: these counts come from fixed seeds; with it each level measures the same: PA_BOT_MEASURE)
            g.useArena(ARENA_FOURCORNERS, tris); g.resetWithBots(0xF);
            PlayerInput in[4];
            std::vector<GameEvent> log;
            for (int i = 0; i < 72 * 120; ++i) {
                PlayerInput tmp[4] = {in[0], in[1], in[2], in[3]};
                g.step(1.f / 72, tmp);
                log.insert(log.end(), g.events.begin(), g.events.end());
            }
            int throws[4] = {0, 0, 0, 0}, hits[4] = {0, 0, 0, 0}, nan = 0;
            for (const GameEvent& e : log) {
                if (e.type == EventType::Throw && e.player >= 0) ++throws[e.player];
                if (e.type == EventType::HitPlayer && e.value >= 0) ++hits[(int)e.value];
            }
            for (int p = 0; p < 4; ++p) nan += !std::isfinite(g.pl[p].ball.pos.x) || !std::isfinite(g.pl[p].in.head.p.x);
            int scorers = 0, sum = 0;
            for (int p = 0; p < 4; ++p) { scorers += hits[p] > 0; sum += g.pl[p].score; }
            static const char* names[3] = {"FREE-FOR-ALL", "2 V 2", "3 V 1"};
            std::printf("       %s, four AMATEUR bots, 2 min: throws %d %d %d %d, hits %d %d %d %d, team scores %d / %d\n", names[mode], throws[0],
                        throws[1], throws[2], throws[3], hits[0], hits[1], hits[2], hits[3], g.teamScore(0), g.teamScore(1));
            char msg[96];
            std::snprintf(msg, sizeof msg, "%s: four bots all throw and score, nothing breaks", names[mode]);
            CHECK(throws[0] > 4 && throws[1] > 4 && throws[2] > 4 && throws[3] > 4 && scorers >= 2 && nan == 0 && sum > 0, msg);
        }
    }
    {
        std::printf("[lobby: two bots play each other]\n");
        Game g; g.setArena(tris); g.reset(true);
        g.pl[0].isBot = true;
        PlayerInput in[2] = {standing(g, 0), {}};
        std::vector<GameEvent> log;
        run(g, in, 90.f, &log);
        int throws[2] = {0, 0}, nan = 0;
        for (const GameEvent& e : log) if (e.type == EventType::Throw && e.player >= 0) ++throws[e.player];
        for (int p = 0; p < 2; ++p) nan += !std::isfinite(g.pl[p].ball.pos.x) || !std::isfinite(g.pl[p].in.head.p.x);
        CHECK(throws[0] > 3 && throws[1] > 3 && nan == 0, "both bots throw (the lobby's match runs by itself)");
        CHECK(g.match != MatchState::Meet, "bots never wait for a fist bump");
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
        std::printf("[the rising ball can be taken off the spawner]\n");
        PlayerInput in[2]; Game g = makeGame(tris, in);
        const int h = 1;
        g.respawnBall(0);
        const Vec3 off = in[0].hand[h].grip.q.rotate(g.tun.catcherOffset);
        // still under the floor (the rings): a hand where it will come up takes nothing
        in[0].hand[h].trigger = 1;
        run(g, in, 0.3f);
        in[0].hand[h].grip.p = g.pl[0].ball.pos - off;
        run(g, in, 1.f / 72);
        CHECK(g.pl[0].ball.state == BallState::Spawning, "under the floor: not grabbable yet");
        // up and rising: the hand on it with the trigger held takes it, before it reaches the top
        std::vector<GameEvent> log;
        while (g.pl[0].ball.state == BallState::Spawning && g.pl[0].ball.timer > g.tun.spawnRiseTime * 0.5f) {
            in[0].hand[h].grip.p = g.pl[0].ball.pos - off;
            PlayerInput tmp[2] = {in[0], in[1]};
            g.step(1.f / 72, tmp);
            log.insert(log.end(), g.events.begin(), g.events.end());
        }
        CHECK(g.pl[0].ball.state == BallState::Held && hasEvent(log, EventType::Grab), "grabbed while it rises");
        CHECK(!hasEvent(log, EventType::Spawn), "... before it reached the top");
        CHECK(g.pl[0].hands[h].shieldUp, "and it's charged: the shield is up");
    }
    {
        std::printf("[CATCHES ONLINE: a catch is judged as the player saw the ball]\n");
        bool held[3] = {};
        for (int pass = 0; pass < 3; ++pass) {   // 0: seen on time, 1: seen 0.12 s late, 2: late but the rule off
            PlayerInput in[2]; Game g = makeGame(tris, in);
            g.lateCatch = pass != 2;
            Ball& b = g.pl[0].ball;
            const Vec3 head = in[0].head.p;
            b.state = BallState::Returning; b.instigator = 0;
            b.pos = head + Vec3{3.f, -0.3f, 0.f}; b.vel = {-10.f, 0.f, 0.f}; b.speedLimit = 10.f;
            in[0].hand[0].trigger = in[0].hand[1].trigger = 0;
            Vec3 then{};
            for (int i = 0; i < 11; ++i) {   // 11 steps of 1/72 s: about 0.15 s
                PlayerInput tmp[2] = {in[0], in[1]};
                g.step(1.f / 72, tmp);
                if (i == 2) then = b.pos;      // where it was about 0.12 s before the last step
            }
            const int h = 1;
            const Vec3 off = in[0].hand[h].grip.q.rotate(g.tun.catcherOffset);
            in[0].hand[h].grip.p = then - off;   // the hand where the ball was then, the trigger pulled
            in[0].hand[h].trigger = 1;
            g.seenLate[0] = pass == 0 ? 0.f : 0.12f;
            PlayerInput tmp[2] = {in[0], in[1]};
            g.step(1.f / 72, tmp);
            held[pass] = b.state == BallState::Held;
        }
        CHECK(!held[0], "seen on time: a hand where the ball was 0.12 s ago doesn't catch it");
        CHECK(held[1], "seen 0.12 s late (the round trip): the same hand catches it");
        CHECK(!held[2], "... and not with the rule off");
    }
    {
        std::printf("[CATCHES ONLINE: your own ball's hit waits for your catch]\n");
        // 0: seen on time (the hit at once); 1: 0.1 s late, no catch (the hit after the wait); 2: 0.1 s late, caught
        bool hitAtOnce[3] = {}, waited[3] = {}, hitLater[3] = {}, held[3] = {};
        for (int pass = 0; pass < 3; ++pass) {
            PlayerInput in[2]; Game g = makeGame(tris, in);
            Ball& b = g.pl[0].ball;
            const Vec3 head = in[0].head.p;
            b.state = BallState::Returning; b.instigator = 0; b.timer = 1.f;
            b.pos = head + Vec3{1.5f, 0.f, 0.f}; b.vel = {-10.f, 0.f, 0.f}; b.speedLimit = 10.f;
            in[0].hand[0].trigger = in[0].hand[1].trigger = 0;
            g.seenLate[0] = pass == 0 ? 0.f : 0.1f;
            std::vector<GameEvent> log;
            int arrived = -1;
            for (int i = 0; i < 72 && b.state != BallState::Dead && b.state != BallState::Held; ++i) {
                if (pass == 2 && b.hitWaitAt >= 0.f) {   // the catch, as the player made it, arriving during the wait
                    const Vec3 off = in[0].hand[1].grip.q.rotate(g.tun.catcherOffset);
                    in[0].hand[1].grip.p = b.pos - off;
                    in[0].hand[1].trigger = 1;
                }
                PlayerInput tmp[2] = {in[0], in[1]};
                g.step(1.f / 72, tmp);
                log.insert(log.end(), g.events.begin(), g.events.end());
                if (arrived < 0 && (b.hitWaitAt >= 0.f || hasEvent(log, EventType::HitPlayer))) arrived = i;
                if (b.hitWaitAt >= 0.f) waited[pass] = true;
                if (arrived >= 0 && i == arrived && hasEvent(log, EventType::HitPlayer)) hitAtOnce[pass] = true;
            }
            hitLater[pass] = !hitAtOnce[pass] && hasEvent(log, EventType::HitPlayer);
            held[pass] = b.state == BallState::Held;
        }
        CHECK(hitAtOnce[0] && !waited[0], "seen on time: your own ball hits you at once, as before");
        CHECK(waited[1] && hitLater[1] && !held[1], "seen late, no catch: it waits at you, then it's the point");
        CHECK(waited[2] && held[2] && !hitLater[2], "seen late, caught during the wait: the catch wins, no point");
    }
    {
        std::printf("[your own ball is yours to catch, whoever touched it last]\n");
        PlayerInput in[2]; Game g = makeGame(tris, in);
        Ball& b = g.pl[0].ball;
        b.state = BallState::Flying; b.instigator = 1;   // (the other player blocked it last)
        b.pos = in[0].head.p + Vec3{0.6f, -0.4f, 0.2f}; b.vel = {-1.0f, 0.f, 0.f}; b.speedLimit = 1.0f;
        const int h = 1;
        const Vec3 off = in[0].hand[h].grip.q.rotate(g.tun.catcherOffset);
        in[0].hand[h].grip.p = b.pos - off;
        in[0].hand[h].trigger = 1;
        std::vector<GameEvent> log;
        run(g, in, 1.f / 72, &log);
        CHECK(b.state == BallState::Held && hasEvent(log, EventType::Caught), "caught, though the other player touched it last");
    }
    {
        std::printf("[STRIKE SPEED: a host's option for faster strikes]\n");
        Tunables t;
        CHECK(t.strikeSpeed == 0 && t.perStrike() == 0.5f, "regulation by default: 0.5 m/s a strike, the original's");
        CHECK(t.statsMode() == 0, "... and a regulation match counts as standard");
        t.strikeSpeed = 1; const float fast = t.perStrike();
        t.strikeSpeed = 2; const float faster = t.perStrike();
        CHECK(fast == 1.0f && faster == 1.5f, "FAST 1.0 m/s, FASTER 1.5 m/s a strike");
        CHECK(t.statsMode() == 2, "a match on FASTER counts as custom, not standard");
        t.experimental = true; t.expRoom = ROOM_BLITZ;
        CHECK(t.perStrike() == 0.5f, "four-player rooms stay at regulation");
        PlayerInput in[2]; Game g = makeGame(tris, in);
        g.pl[1].strikesAgainst = 3;   // (three strikes held by player 0: strikesHeld)
        const float reg = g.throwSpeedFromHandSpeed(99.f, 0);
        g.tun.strikeSpeed = 2;
        const float fs = g.throwSpeedFromHandSpeed(99.f, 0);
        std::printf("    a full-power throw with %d strikes: %.2f m/s regulation, %.2f m/s FASTER\n", g.strikesHeld(0), reg, fs);
        CHECK(g.strikesHeld(0) == 0 || fs > reg, "a full-power throw with strikes is faster on FASTER");
    }
    {   // ROOMS AND THEIR OPTIONS (0.3.21; the owner's design, 2026-11-17): MAIDEN, AIM ASSIST, STRIKE ZONE, BALL GROWTH
        std::printf("[rooms and their options]\n");
        Tunables t;
        CHECK(!t.maidenNow() && t.aimAssistNow() == 2 && t.strikeZoneNow() == 1 && t.ballGrowthNow() == 3 && t.statsMode() == 0 &&
                  std::fabs(t.aimAssistDeg() - 3.f) < 1e-4f && std::fabs(t.strikeRadius() - 0.7f) < 1e-4f && t.growthShare() == 1.f,
              "as the rules say by default: Basic's MEDIUM aim assist (3 deg), a 70 cm ring, the ball's full growth; standard");
        t.advanced = true;
        CHECK(t.aimAssistNow() == 1 && std::fabs(t.aimAssistDeg() - 1.5f) < 1e-4f && t.statsMode() == 0, "Advanced: LOW (1.5 deg)");
        // a throw 8 degrees off the opponent's chest, aim assist on: how far it turns toward them
        float turned[4] = {};
        for (int s = 0; s < 4; ++s) {
            PlayerInput in[2]; Game g = makeGame(tris, in);
            g.tun.aimAssist = s;
            const Vec3 from = g.tun.start[0] + Vec3{0.6f, 1.3f, 0};
            const Vec3 d0 = normalize(g.chestPoint(1) - from);
            const Vec3 dir = Quat::axisAngle({0, 1, 0}, 8.f * 3.14159265f / 180.f).rotate(d0);
            g.serve(0, from, dir, 10.f, {0, 0, 0}, true);
            turned[s] = std::acos(std::max(-1.f, std::min(1.f, dot(normalize(g.pl[0].ball.vel), dir)))) * 57.2957795f;
        }
        std::printf("       (a throw 8 deg off, turned: OFF %.2f, LOW %.2f, MEDIUM %.2f, HIGH %.2f deg)\n", turned[0], turned[1], turned[2], turned[3]);
        CHECK(turned[0] < 0.01f && std::fabs(turned[1] - 1.5f) < 0.1f && std::fabs(turned[2] - 3.f) < 0.1f && std::fabs(turned[3] - 4.5f) < 0.1f,
              "AIM ASSIST: a throw is turned OFF 0, LOW 1.5, MEDIUM 3, HIGH 4.5 deg toward its target");
        // a ball through the ring 0.6 m and 0.8 m from its centre: a strike or not, by the ring's size
        auto strikeAt = [&](int zone, float off) {
            PlayerInput in[2]; Game g = makeGame(tris, in);
            g.tun.strikeZone = zone;
            in[0].head.p.z = -1.5f; g.pl[0].in.head.p.z = -1.5f;   // (out of the way)
            Ball& b = g.pl[1].ball;
            const Vec3 at = g.strikeZoneCenter(0) + Vec3{0, 0, off};
            fireOpponentBall(g, at + normalize(at - (g.tun.start[1] + Vec3{-0.6f, 1.6f, 0})) * 0.5f);
            std::vector<GameEvent> log;
            run(g, in, 2.0f, &log);
            (void)b;
            return hasEvent(log, EventType::Strike, 0);
        };
        const bool s06 = strikeAt(0, 0.6f), r06 = strikeAt(1, 0.6f), r08 = strikeAt(1, 0.8f), l08 = strikeAt(2, 0.8f);
        CHECK(!s06 && r06 && !r08 && l08, "STRIKE ZONE: 60 cm out a strike in REGULATION, not SMALL (50 cm); 80 cm one in LARGE (90 cm), not REGULATION");
        // BALL GROWTH: the ball at 4 strikes (regulation: twice its size)
        float grown[4] = {};
        for (int k = 0; k < 4; ++k) { Game g; g.tun.ballGrowth = k; grown[k] = g.strikeScale(4); }
        CHECK(grown[0] == 1.f && std::fabs(grown[1] - 1.25f) < 1e-4f && std::fabs(grown[2] - 1.5f) < 1e-4f && std::fabs(grown[3] - 2.f) < 1e-4f,
              "BALL GROWTH at 4 strikes: NONE 1x, QUARTER 1.25x, HALF 1.5x, REGULATION 2x");
        // MAIDEN: an uncharged ball still raises the shield, and a block never breaks it
        {
            PlayerInput in[2]; Game g = makeGame(tris, in);
            g.tun.maiden = true;
            grabBall(g, in, 1);
            g.pl[0].ball.charged = false;
            const Quat q = fromTo({0, 1, 0}, {1, 0, 0});
            const Vec3 face = g.pl[0].in.head.p;
            const Pose sl = g.shieldLocal(1);
            const Quat gq = q * sl.q.conj();
            in[0].hand[1].grip = {gq, face + Vec3{0.45f, 0, 0} - gq.rotate(sl.p)};
            run(g, in, 0.05f);
            const bool upUncharged = g.pl[0].hands[1].shieldUp;
            g.pl[0].ball.charged = true;
            fireOpponentBall(g, face);
            std::vector<GameEvent> log;
            run(g, in, 2.0f, &log);
            CHECK(upUncharged && hasEvent(log, EventType::ShieldBlock, 0) && !hasEvent(log, EventType::ShieldBreak, 0) &&
                      g.pl[0].ball.charged && g.pl[0].hands[1].shieldUp && g.pl[1].score == 0,
                  "MAIDEN: the shield is up with an uncharged ball, blocks, and stays up (no break, the charge kept)");
        }
        // FAIR MATCHES and the rooms: an older player in the match, or a four-player room, plays them as the rules say
        Tunables f;
        f.maiden = true; f.aimAssist = 3; f.strikeZone = 2; f.ballGrowth = 1;
        CHECK(f.maidenNow() && f.aimAssistNow() == 3 && f.strikeZoneNow() == 2 && f.ballGrowthNow() == 1 && f.statsMode() == 2,
              "every option plays when every player's game has them (0.3.21), and the match counts as custom");
        f.matchVersion = 3020;   // (the competition build)
        CHECK(!f.maidenNow() && f.aimAssistNow() == 2 && f.strikeZoneNow() == 1 && f.ballGrowthNow() == 3 && f.statsMode() == 0 &&
                  f.maiden && f.aimAssist == 3,
              "... a player on 0.3.20 (the competition build): all as the rules say, the match standard, the host's choices kept");
        {   // the Meta competition build (0.3.20, frozen 11-15): the FOUL LINE plays; the reach, knuckles and room options as before
            Tunables c; c.matchVersion = 3020; c.foulShift = 2; c.maiden = true; c.advanced = true; c.advancedRoundsToWin = 3;
            const Vec3 k = c.knuckleOffsetNow(), ko = c.knuckleOffsetOld;
            CHECK(c.foulMove() == 1.0f && !c.fistReachNow() && k.x == ko.x && k.y == ko.y && k.z == ko.z && !c.maidenNow() &&
                      c.roundsToWinNow() == 2,
                  "a player on 0.3.20 (the competition build): FOUL LINE plays; the old reach and knuckles, no room options, BEST OF 3");
        }
        f.matchVersion = Tunables::kVerAll; f.experimental = true; f.expRoom = ROOM_DIAMOND;
        const bool diamond = f.maidenNow() && f.strikeZoneNow() == 2;
        f.expRoom = ROOM_CORNERS;
        const bool corners = !f.maidenNow() && f.aimAssistNow() == 2 && f.strikeZoneNow() == 1 && f.ballGrowthNow() == 3;
        f.expRoom = ROOM_BLITZ;
        const bool blitz = !f.maidenNow() && f.strikeZoneNow() == 1;
        CHECK(diamond && corners && blitz, "DIAMOND has them; FOUR CORNERS and FIREBALL BLITZ keep their own for now");
        f = Tunables(); f.maiden = true; f.shields = false;
        CHECK(!f.maidenNow() && f.statsMode() == 1, "MAIDEN needs shields: with SHIELDS OFF it's HOTDOG");
        {   // THE BALL IN THE LEFT PALM (a player's report, 11-19): the left hand's ball on its palm side, as the right's is
            PlayerInput in[2]; Game g = makeGame(tris, in);
            const Vec3 r = g.holdLocal(1), l = g.holdLocal(0);
            const bool mirrored = r.x < 0 && l.x == -r.x && l.y == r.y && l.z == r.z;
            g.tun.matchVersion = 3021;
            const bool older = g.holdLocal(0).x == r.x && g.holdLocal(1).x == r.x;
            g.tun.matchVersion = Tunables::kVerAll; g.tun.palmHold = false;
            const bool guest = g.holdLocal(0).x == r.x;
            CHECK(mirrored && older && guest, "the left hand's ball in its palm (grip +X) as the right's (-X); with a player on "
                                              "0.3.21, or a host that doesn't say so: where it was");
        }
        {   // SHIELD SIZE (the owner, 11-17): a ball 35 cm out from the plate's middle: past a regulation shield, blocked by +50%
            auto blocks = [&](int size) {
                PlayerInput in[2]; Game g = makeGame(tris, in);
                g.tun.shieldSize = size;
                grabBall(g, in, 1);
                const Quat q = fromTo({0, 1, 0}, {1, 0, 0});
                const Vec3 face = g.pl[0].in.head.p;
                const Pose sl = g.shieldLocal(1);
                const Quat gq = q * sl.q.conj();
                in[0].hand[1].grip = {gq, face + Vec3{0.45f, 0, 0} - gq.rotate(sl.p)};
                run(g, in, 0.05f);
                const Pose sp = g.shieldPose(0, 1);
                const Vec3 side = normalize(sp.q.rotate({1, 0, 0}));
                fireOpponentBall(g, sp.p + side * 0.35f);
                std::vector<GameEvent> log;
                run(g, in, 2.0f, &log);
                return hasEvent(log, EventType::ShieldBlock, 0);
            };
            Tunables c; c.shieldSize = 4; c.matchVersion = 3020;
            CHECK(!blocks(0) && blocks(2) && c.shieldSizeNow() == 0 && std::fabs(c.shieldScale() - 1.f) < 1e-6f,
                  "SHIELD SIZE: a ball 35 cm out from the middle passes a regulation shield, a +50% one blocks it; a 0.3.20 player: regulation");
            // BALL FEEL (the owner, 11-17): the owner's pick the standard ball; only how it looks: never custom in the stats
            Tunables f;
            const int std0 = f.statsMode();
            bool same = f.ballFeel == (BALL_TRUE | BALL_SPIN | BALL_WEIGHT);
            for (int bits = 0; bits <= BALL_FEEL_ALL; ++bits) { f.ballFeel = bits; same &= f.statsMode() == std0; }
            CHECK(same, "BALL FEEL: the standard ball is glow core, true size, visible spin, heavy; any ball feel leaves the stats as they are");
        }
        {   // THROW AND PUNCH FEEL (0.3.25; a player's report, the owner's options): THROW POWER and KNUCKLE RETURNS
            PlayerInput in[2]; Game g = makeGame(tris, in);
            g.pl[1].isBot = true;
            const float reg2 = g.throwSpeedFromHandSpeed(2.f, 0), reg5 = g.throwSpeedFromHandSpeed(5.f, 0);
            g.tun.throwPower = 1;
            const float nat2 = g.throwSpeedFromHandSpeed(2.f, 0), nat5 = g.throwSpeedFromHandSpeed(5.f, 0);
            const float natFull = g.throwSpeedFromHandSpeed(g.tun.maxInputSpeed, 0), bot2 = g.throwSpeedFromHandSpeed(2.f, 1);
            g.tun.matchVersion = 3024;
            const float old2 = g.throwSpeedFromHandSpeed(2.f, 0);
            std::printf("       (a hand at 2 m/s throws %.1f m/s REGULATION, %.1f NATURAL; at 5 m/s %.1f and %.1f; a full throw NATURAL %.1f)\n",
                        reg2, nat2, reg5, nat5, natFull);
            CHECK(std::fabs(reg2 - 4.31f) < 0.02f && std::fabs(nat2 - 2.12f) < 0.03f && std::fabs(nat5 - 9.2f) < 0.05f &&
                  std::fabs(natFull - g.tun.projectileMaxSpeed) < 1e-3f,
                  "THROW POWER: NATURAL: a light throw slow (2 m/s hand: 2.1 m/s, not 4.3), a full throw as fast as ever");
            CHECK(std::fabs(bot2 - reg2) < 1e-4f && std::fabs(old2 - reg2) < 1e-4f,
                  "THROW POWER: a bot throws as ever; with a 0.3.24 player in the match, REGULATION");
            // a knuckle guard facing +x, a ball coming in at 10 m/s: still, and punched out at 3 m/s
            auto ret = [&](int mode, float fist, float speedIn) {
                Game k = g;
                k.tun.matchVersion = Tunables::kVerAll;
                k.tun.knuckleReturns = mode;
                Ball b; b.vel = {-speedIn, 0, 0};
                return k.deflectVelocity(b, {1, 0, 0}, {fist, 0, 0}, true, 0.f).x;
            };
            const float regStill = ret(0, 0, 10), punStill = ret(1, 0, 10), regHit = ret(0, 3, 10), punHit = ret(1, 3, 10);
            const float punSlow = ret(1, 3, 5), regSlow = ret(0, 3, 5);
            std::printf("       (a ball at 10 m/s on a knuckle guard: still REGULATION %.2f, PUNCH %.2f; punched at 3 m/s %.2f and %.2f; "
                        "a 5 m/s ball punched %.2f and %.2f)\n", regStill, punStill, regHit, punHit, regSlow, punSlow);
            CHECK(std::fabs(regStill - 9.f) < 1e-3f && std::fabs(regHit - 14.7f) < 1e-3f,
                  "KNUCKLE RETURNS: REGULATION as before: 90% of the incoming speed back, the fist's added");
            CHECK(std::fabs(punStill - 4.5f) < 1e-3f && std::fabs(punHit - 11.85f) < 1e-3f && std::fabs(punSlow - 9.6f) < 1e-3f &&
                      punHit - punStill > regHit - regStill,
                  "KNUCKLE RETURNS: PUNCH: a still fist drops it back softly (4.5 m/s of 10), a punch counts for more");
            Ball sb; sb.vel = {-10, 0, 0};
            Game k = g; k.tun.matchVersion = Tunables::kVerAll; k.tun.knuckleReturns = 1;
            const float shield = k.deflectVelocity(sb, {1, 0, 0}, {0, 0, 0}, false, 0.f).x;
            CHECK(std::fabs(shield - 9.f) < 1e-3f, "KNUCKLE RETURNS: PUNCH leaves a shield's block as it is");
            {   // FULL EFFORT (0.3.26: "at least 50% more effort"): top speed only at half as hard a throw or punch again
                Game h = g;
                h.tun.matchVersion = Tunables::kVerAll; h.tun.throwPower = 2;
                const float full2 = h.throwSpeedFromHandSpeed(2.f, 0), full5 = h.throwSpeedFromHandSpeed(5.f, 0);
                const float fullOld = h.throwSpeedFromHandSpeed(h.tun.maxInputSpeed, 0);
                const float fullTop = h.throwSpeedFromHandSpeed(h.tun.maxInputSpeed * 1.5f, 0);
                h.tun.matchVersion = 3025;
                const float with25 = h.throwSpeedFromHandSpeed(5.f, 0);
                std::printf("       (FULL EFFORT: a hand at 2 m/s throws %.1f m/s, at 5 m/s %.1f; NATURAL's full throw (6.5 m/s) %.1f; "
                            "top speed at 9.75 m/s: %.1f)\n", full2, full5, fullOld, fullTop);
                CHECK(full2 < nat2 && full5 < nat5 && fullOld < 0.6f * h.tun.projectileMaxSpeed &&
                          std::fabs(fullTop - h.tun.projectileMaxSpeed) < 1e-3f && std::fabs(with25 - nat5) < 1e-4f,
                      "THROW POWER: FULL EFFORT: top speed only from a 50% faster throw; with a 0.3.25 player in the match, NATURAL");
                const float fullStill = ret(2, 0, 10), fullHit = ret(2, 3, 10), fullHard = ret(2, 4.5f, 10);
                std::printf("       (FULL EFFORT on a knuckle guard, a ball at 10 m/s: still %.2f, punched at 3 m/s %.2f, at 4.5 m/s %.2f)\n",
                            fullStill, fullHit, fullHard);
                CHECK(std::fabs(fullStill - 3.f) < 1e-3f && fullHit < punHit - 2.f && std::fabs(fullHard - punHit) < 0.01f,
                      "KNUCKLE RETURNS: FULL EFFORT: a still fist 3 m/s of 10 back, and a punch half as hard again for PUNCH's return");
                Tunables x; x.throwPower = 2; x.knuckleReturns = 2; x.matchVersion = 3025;
                Tunables y = x; y.matchVersion = 3024;
                Tunables z = x; z.matchVersion = Tunables::kVerAll;
                CHECK(x.naturalThrowNow() && !x.fullThrowNow() && x.punchReturnsNow() && !x.fullPunchNow() && y.feelStandard() &&
                          z.fullThrowNow() && z.fullPunchNow() && z.statsMode() == 2,
                      "FULL EFFORT FAIR MATCHES: a 0.3.25 player: NATURAL and PUNCH; older: REGULATION; a custom match in the stats");
            }
            // FAIR MATCHES, the rooms, the stats
            Tunables f; f.throwPower = 1;
            const int natMode = f.statsMode();
            f.throwPower = 0; f.knuckleReturns = 1;
            const int punMode = f.statsMode();
            Tunables c4; c4.experimental = true; c4.expRoom = ROOM_CORNERS; c4.throwPower = 1; c4.knuckleReturns = 1;
            Tunables bz = c4; bz.expRoom = ROOM_BLITZ;
            Tunables dm = c4; dm.expRoom = ROOM_DIAMOND;
            Tunables old = f; old.matchVersion = 3024;
            CHECK(natMode == 2 && punMode == 2 && Tunables().statsMode() == 0 && Tunables().feelStandard(),
                  "THROW POWER and KNUCKLE RETURNS: either makes the match custom in the stats; REGULATION by default");
            CHECK(c4.naturalThrowNow() && c4.punchReturnsNow() && dm.naturalThrowNow() && !bz.naturalThrowNow() &&
                      !bz.punchReturnsNow() && !old.punchReturnsNow(),
                  "THROW POWER and KNUCKLE RETURNS: the SQUARE, DIAMOND and FOUR CORNERS, not FIREBALL BLITZ; a 0.3.24 player in the match: REGULATION");
        }
        {   // BOUNCES (0.3.22; pull request #26's ideas, as the host's options): a ball served at 10 m/s, 30 degrees into the
            // +z side wall, no spin; its speed and spin after that bounce
            auto bank = [&](bool lively, bool grip, int version, float& speed, float& spin, float& outDeg) {
                PlayerInput in[2]; Game g = makeGame(tris, in);
                g.tun.wallsLively = lively; g.tun.wallGrip = grip; g.tun.matchVersion = version;
                const float a = deg2rad(30.f);
                g.serve(0, g.tun.start[0] + Vec3{3.f, 1.3f, 0}, Vec3{std::cos(a), 0, std::sin(a)}, 10.f, {}, false);
                speed = spin = outDeg = -1.f;
                for (int i = 0; i < 72 * 2; ++i) {
                    stepCopy(g, in);
                    const Ball& b = g.pl[0].ball;
                    if (hasEvent(g.events, EventType::Bounce, 0)) {
                        speed = b.speedLimit; spin = length(b.spin);
                        outDeg = rad2deg(std::atan2(-b.vel.z, b.vel.x));
                        break;
                    }
                }
            };
            float s0, w0, d0, s1, w1, d1, s2, w2, d2, s3, w3, d3;
            bank(false, false, Tunables::kVerAll, s0, w0, d0);
            bank(true, false, Tunables::kVerAll, s1, w1, d1);
            bank(false, true, Tunables::kVerAll, s2, w2, d2);
            bank(true, true, 3021, s3, w3, d3);
            std::printf("       (a 30 degree bank at 10 m/s: STANDARD %.2f m/s out at %.1f deg; LIVELY %.2f at %.1f; GRIP %.2f at %.1f, "
                        "spinning %.1f rad/s)\n", s0, d0, s1, d1, s2, d2, w2);
            CHECK(std::fabs(s0 - 9.f) < 0.02f && std::fabs(d0 - 30.f) < 0.5f && w0 < 0.01f,
                  "BOUNCES STANDARD: a side wall mirrors the ball and keeps 90% of its speed, as before");
            // (LIVELY: the 5 m/s into the wall cut to 97%, the 8.66 along it kept: sqrt(8.66^2 + 4.85^2) = 9.93)
            CHECK(std::fabs(s1 - 9.926f) < 0.02f && d1 > 29.f && d1 < 30.f,
                  "WALLS: LIVELY: only the speed into the side wall is cut (97%): 9.93 m/s of 10 kept");
            // (GRIP: the normal impulse 1.9 x 5 = 9.5; the grip needed (2/7 of 8.66, 2.47) is more than the friction allows
            // (0.10 x 9.5 = 0.95): 8.66 - 0.95 along, 4.5 out: 8.93 m/s; spin 0.95 x 5 / (2 x 0.1) = 23.8 rad/s)
            CHECK(std::fabs(s2 - 8.93f) < 0.03f && std::fabs(w2 - 23.75f) < 0.6f && d2 > 30.f,
                  "WALL GRIP: the wall grips the ball as a solid sphere: 8.93 m/s kept, spinning 23.8 rad/s, a little steeper out");
            CHECK(std::fabs(s3 - 9.f) < 0.02f && w3 < 0.01f, "BOUNCES FAIR MATCHES: a player on 0.3.21 in the match: STANDARD, for both");
            // BANK ASSIST: a bank set to cross the other player's plane half a metre wide of their chest
            auto banked = [&](int level, int version, float& closest, float& turnDeg) {
                PlayerInput in[2]; Game g = makeGame(tris, in);
                g.tun.aimAssist = 0;   // (AIM ASSIST off: the throw goes where it's sent)
                g.tun.bankAssist = level; g.tun.matchVersion = version;
                const Vec3 from = g.tun.start[0] + Vec3{0.6f, 1.3f, 0}, chest = g.chestPoint(1);
                const float wall = 2.5f - g.ballRadius(0);
                const Vec3 mirror{chest.x, chest.y, 2.f * wall - (chest.z + 0.5f)};   // (the far end, seen in the wall)
                g.serve(0, from, normalize(mirror - from), 10.f, {}, true);
                closest = 1e9f; turnDeg = 0.f;
                bool bounced = false;
                for (int i = 0; i < 72 * 3; ++i) {
                    stepCopy(g, in);
                    const Ball& b = g.pl[0].ball;
                    if (!bounced && hasEvent(g.events, EventType::Bounce, 0)) { bounced = true; turnDeg = rad2deg(length(b.bankTurn)); }
                    if (b.state != BallState::Flying) break;
                    if (bounced) closest = std::min(closest, length(b.pos - chest));
                }
            };
            float c0, t0, c3, t3, cOld, tOld;
            banked(0, Tunables::kVerAll, c0, t0);
            banked(3, Tunables::kVerAll, c3, t3);
            banked(3, 3021, cOld, tOld);
            std::printf("       (a bank 0.5 m wide of the chest: OFF passes %.2f m from it; HIGH turns it %.2f deg and passes %.2f m)\n", c0, t3, c3);
            CHECK(t0 == 0.f && t3 > 0.5f && t3 <= 2.4f + 1e-3f && c3 < c0 - 0.1f,
                  "BANK ASSIST HIGH: the leg after the bounce is turned toward the other player (at most 2.4 degrees) and passes nearer");
            CHECK(tOld == 0.f && std::fabs(cOld - c0) < 1e-3f, "BANK ASSIST FAIR MATCHES: a player on 0.3.21 in the match: none");
            Tunables f;
            f.wallGrip = true;
            const int gripMode = f.statsMode();
            f.wallGrip = false; f.bankAssist = 1;
            const int bankMode = f.statsMode();
            f.experimental = true; f.expRoom = ROOM_CORNERS; f.wallsLively = true; f.wallGrip = true;
            const bool corners = !f.bouncesStandard();   // (BOUNCES IN FOUR CORNERS, 0.3.26)
            Tunables fOld = f; fOld.matchVersion = 3025;
            Tunables fBlitz = f; fBlitz.expRoom = ROOM_BLITZ;
            CHECK(gripMode == 2 && bankMode == 2 && corners && fOld.bouncesStandard() && fBlitz.bouncesStandard() &&
                      Tunables().statsMode() == 0,
                  "BOUNCES: any of them makes the match custom; FOUR CORNERS has them (0.3.26: STANDARD with an older player), FIREBALL BLITZ not");
            // (what BANK ASSIST costs a bot reading your banks: its reads and tryouts fly the turn too; the PC's time a step)
            double avg[2] = {0, 0}, worst[2] = {0, 0};
            for (int k = 0; k < 2; ++k) {
                Game g; g.setArena(tris); g.tun.meetTime = 0; g.tun.basicMatchTime = 0; g.tun.botLevel = kBotLevels - 1;
                g.tun.bankAssist = k ? 3 : 0; g.tun.aimAssist = 0; g.tun.wallGrip = k == 1;
                g.reset(true);
                PlayerInput in[2] = {standing(g, 0), {}};
                for (int i = 0; i < 72 * 4 && g.match != MatchState::InProgress; ++i) stepCopy(g, in);
                int steps = 0;
                double total = 0;
                for (int n = 0; n < 16; ++n) {
                    const Vec3 from = g.tun.start[0] + Vec3{0.6f, 1.3f, 0}, chest = g.chestPoint(1);
                    const float side = n % 2 ? -1.f : 1.f, wall = 2.5f - g.ballRadius(0);
                    const Vec3 mirror{chest.x, chest.y, side * (2.f * wall - 0.5f) - chest.z};
                    g.serve(0, from, normalize(mirror - from), 11.f, {}, true);
                    for (int i = 0; i < 72 * 3 && g.pl[0].ball.state == BallState::Flying; ++i) {
                        in[0] = standing(g, 0);
                        const auto t0 = std::chrono::steady_clock::now();
                        stepCopy(g, in);
                        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                        total += ms; ++steps; worst[k] = std::max(worst[k], ms);
                    }
                    for (int i = 0; i < 72 * 2; ++i) { in[0] = standing(g, 0); stepCopy(g, in); }
                }
                avg[k] = steps ? total / steps : 0;
            }
            std::printf("       (UNBEATABLE reading 16 banks, a step on this PC: STANDARD %.3f ms (at most %.2f); WALL GRIP and BANK ASSIST HIGH "
                        "%.3f ms (at most %.2f))\n", avg[0], worst[0], avg[1], worst[1]);
        }
        {   // ROUNDS: BEST OF 5 is three rounds won (5 s rounds here: the one ahead at time takes each)
            PlayerInput in[2];
            Game g; g.setArena(tris); g.tun.advanced = true; g.tun.meetTime = 0; g.tun.advancedRoundTime = 5.f;
            g.tun.advancedRoundsToWin = 3;
            g.reset(false);
            in[0] = standing(g, 0); in[1] = standing(g, 1);
            run(g, in, 3.6f);
            const bool custom = g.tun.statsMode() == 2;
            int rounds = 0;
            for (int r = 0; r < 6 && g.match != MatchState::MatchSummary; ++r) {
                if (g.match == MatchState::InProgress) { g.pl[0].score = 3; ++rounds; }
                run(g, in, 6.f);
                if (g.match != MatchState::MatchSummary) run(g, in, g.tun.roundSummaryTime + g.tun.countdown + 0.5f);
            }
            Tunables old; old.advanced = true; old.advancedRoundsToWin = 3; old.matchVersion = 3020;
            Tunables d; d.experimental = true; d.expRoom = ROOM_DIAMOND; d.advanced = true; d.advancedRoundsToWin = 9;
            const bool diamondRounds = d.roundsToWinNow() == 5 && d.aimAssistNow() == 1 && !d.knuckleGuards() && d.statsMode() == 2;
            std::printf("       (BEST OF 5: the match ended after %d rounds, %d won)\n", rounds, g.pl[0].roundWins);
            CHECK(custom && g.match == MatchState::MatchSummary && g.pl[0].roundWins == 3 && rounds == 3 && old.roundsToWinNow() == 2,
                  "ROUNDS: BEST OF 5 ends at the third round won (a custom match); with a player on 0.3.20, BEST OF 3");
            CHECK(diamondRounds, "DIAMOND plays rounds too (its rules' LOW aim assist, no knuckle guards); at most BEST OF 9");
        }
    }
    {
        std::printf("[FOUL LINE: a host's option to move the line, 0.5 m a step]\n");
        Tunables t;
        CHECK(t.foulShift == 0 && t.foulMove() == 0.f && t.statsMode() == 0, "where the arena has it by default, and standard");
        t.foulShift = 2;
        CHECK(t.foulMove() == 1.0f && t.statsMode() == 2, "LONGER 2: 1 m toward the centre; the match counts as custom");
        t.foulShift = -9;
        CHECK(t.foulMove() == -1.0f, "at most 2 steps either way");
        t.experimental = true; t.expRoom = ROOM_CORNERS;
        CHECK(t.foulMove() == 0.f, "the four-player rooms keep their own");
        t.expRoom = ROOM_DIAMOND;
        CHECK(t.foulMove() == 0.f, "DIAMOND too (its foul zone is the platform)");
        {   // FAIR MATCHES: an option plays only when every player's game has it
            Tunables f;
            f.foulShift = 2; f.strikeSpeed = 2;
            f.matchVersion = 3020;
            CHECK(f.foulMove() == 1.0f && f.perStrike() == 1.5f, "FAIR MATCHES: every player on 0.3.20: both options play");
            f.matchVersion = 3019;
            CHECK(f.foulMove() == 0.f && f.perStrike() == 1.5f, "... a player on 0.3.19: the foul line at regulation, faster strikes still");
            f.matchVersion = 0;
            CHECK(f.foulMove() == 0.f && f.perStrike() == 0.5f && f.statsMode() == 0,
                  "... a game that doesn't say its version: both at regulation, and the match counts as standard");
            CHECK(f.foulShift == 2 && f.strikeSpeed == 2, "... the host's choice itself kept");
        }
        // your own ball landing on your own end's floor at x = -4.3 (just short of the arena's line at 4.75)
        auto landsAt = [&](int shift, float x, bool& fouled, bool& bounced) {
            PlayerInput in[2]; Game g = makeGame(tris, in);
            g.tun.foulShift = shift;
            Ball& b = g.pl[0].ball;
            b.state = BallState::Flying; b.instigator = 0; b.rechargePending = false; b.scored = false; b.spin = {};
            b.pos = {x + 1.2f, 1.0f, 0.5f}; b.speedLimit = 7.f; b.vel = normalize(Vec3{x, 0.f, 0.5f} - b.pos) * 7.f;
            b.progX = b.pos.x; b.progT = 0; b.timer = 0;
            std::vector<GameEvent> log;
            fouled = bounced = false;
            for (int i = 0; i < 72 && !fouled && !bounced; ++i) {
                PlayerInput tmp[2] = {in[0], in[1]};
                g.step(1.f / 72, tmp);
                log.insert(log.end(), g.events.begin(), g.events.end());
                fouled = hasEvent(log, EventType::Foul, 0);
                bounced = hasEvent(log, EventType::Bounce, 0);
            }
        };
        bool f, bo;
        landsAt(0, -4.3f, f, bo);
        CHECK(!f && bo, "regulation: landing 4.3 m from the centre, short of the line, just bounces");
        landsAt(2, -4.3f, f, bo);
        CHECK(f, "LONGER 2 (the line at 3.73 m): the same throw is a foul");
        landsAt(1, -4.6f, f, bo);   // (the ball meets the floor ~0.15 m before where it is aimed)
        CHECK(f, "LONGER 1 (4.23 m): landing 4.6 m from the centre is a foul");
        landsAt(0, -5.3f, f, bo);
        CHECK(f, "regulation: landing 5.3 m from the centre is a foul");
        landsAt(-2, -5.3f, f, bo);
        CHECK(!f && bo, "SHORTER 2 (the line at 5.73 m): the same throw just bounces");
        landsAt(-2, -6.2f, f, bo);
        CHECK(f, "SHORTER 2: past the moved line it is still a foul");
        // the recharge: a throw crossing 4.3 m recharges on regulation, not on LONGER 2 until it crosses 3.73 m
        PlayerInput in[2]; Game g = makeGame(tris, in);
        g.tun.foulShift = 2;
        CHECK(std::fabs(g.foulLine() - 3.73f) < 1e-4f, "the game's line follows the option");
        g.tun.foulShift = -1;
        CHECK(std::fabs(g.foulLine() - 5.23f) < 1e-4f, "... either way");
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
        std::printf("[warm-up throws while waiting for an opponent]\n");
        PlayerInput in[2]; Game g = makeGame(tris, in);
        in[1].headTracked = false;   // nobody at the other end yet
        g.reset(false);
        run(g, in, 2.0f);
        CHECK(g.match == MatchState::WaitingForPlayers, "alone: still waiting for an opponent");
        grabBall(g, in);
        std::vector<GameEvent> log;
        swingAndRelease(g, in, {1, 0, 0}, 5.0f, log);
        CHECK(hasEvent(log, EventType::Throw), "a fast release throws while waiting (warm-up)");
        run(g, in, 4.0f, &log);
        CHECK(g.pl[0].score == 0 && g.pl[1].score == 0 && g.pl[1].strikesAgainst == 0, "warm-up throws score nothing");
        CHECK(g.pl[0].ball.state == BallState::Idle || g.pl[0].ball.state == BallState::Held, "the ball comes back to its spawner");
        in[1].headTracked = true;
        run(g, in, 0.2f);
        CHECK(g.match != MatchState::WaitingForPlayers, "an opponent arrives: the match starts as before");
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
    {   // a player's report (11-11: "a shield held low in front didn't deflect the ball; always got hit"): the right hand
        // holding its charged ball low in front (fist at 1.0 m, 0.30 m ahead), the forearm from level to pointing well
        // down, the wrist rolled every 30 degrees; a ball at the plate's middle along the court. Offline every one
        // blocks; online a late block failed for the player who isn't hosting (tools/run_shield_test.py)
        std::printf("[a shield held low in front blocks]\n");
        int blocks = 0, hits = 0, misses = 0;
        for (int pitch = 0; pitch <= 60; pitch += 30)
        for (int roll = -90; roll <= 90; roll += 30) {
            PlayerInput in[2]; Game g = makeGame(tris, in);
            grabBall(g, in, 1);
            const Quat q = kFaceP0 * Quat::axisAngle({1, 0, 0}, deg2rad(-float(pitch))) * Quat::axisAngle({0, 0, 1}, deg2rad(float(roll)));
            in[0].hand[1].grip = {q, g.tun.start[0] + Vec3{0.30f, 1.0f, 0.08f}};
            run(g, in, 0.05f);
            const Pose s = g.shieldPose(0, 1);
            const Vec3 n = s.q.rotate({0, 1, 0});
            Vec3 aim = s.p;
            fireOpponentBall(g, aim);
            std::vector<GameEvent> log;
            run(g, in, 2.0f, &log);
            const bool blocked = hasEvent(log, EventType::ShieldBlock, 0), hit = g.pl[1].score > 0;
            blocks += blocked; hits += !blocked && hit; misses += !blocked && !hit;
            const Vec3 ax = g.pl[0].in.head.p - Vec3{0, g.tun.torsoTop, 0};
            if (!blocked)
                std::printf("  pitch %2d roll %4d: plate at %.2f m (%.2f m in front of the body's axis), face-on %.2f -> %s\n",
                            pitch, roll, s.p.y, s.p.x - ax.x, std::fabs(n.x), hit ? "HIT" : "missed");
        }
        CHECK(blocks == 21 && hits == 0 && misses == 0, "held low, the forearm level to well down, the wrist rolled every way: it blocks");
    }
    {   // BLOCKS ONLINE: player 0 as a player who isn't hosting (a 200 ms round trip): the ball reaches them with their
        // shield out to the side; their block, as they made it then, arrives during the wait
        std::printf("[BLOCKS ONLINE: a block that was in time on the player's screen counts]\n");
        for (int mode = 0; mode < 4; ++mode) {   // 0 the block arrives, 1 none, 2 the rule off, 3 a 400 ms round trip (past the cap)
            PlayerInput in[2]; Game g = makeGame(tris, in);
            grabBall(g, in, 1);
            g.tun.knuckleMode = 0;   // (the shield alone)
            g.lateBlock = mode != 2;
            const Pose sl = g.shieldLocal(1);
            const Quat gq = fromTo({0, 1, 0}, {1, 0, 0}) * sl.q.conj();   // the plate facing the ball
            const Vec3 chest = g.pl[0].in.head.p - Vec3{0, 0.45f, 0};
            in[0].hand[1].grip = {gq, chest + Vec3{0.45f, 0, 0.9f} - gq.rotate(sl.p)};   // out to the side
            run(g, in, 0.05f);
            g.seenLate[0] = mode == 3 ? 0.4f : 0.2f;
            fireOpponentBall(g, chest);
            std::vector<GameEvent> log;
            float t = 0;
            for (; t < 2.f && g.pl[1].ball.blockWaitAt < 0.f && !hasEvent(log, EventType::HitPlayer, 0); t += 1.f / 72) {
                stepCopy(g, in); log.insert(log.end(), g.events.begin(), g.events.end());
                g.seenLate[0] = mode == 3 ? 0.4f : 0.2f;   // (the host sets it each frame)
            }
            const bool waited = g.pl[1].ball.blockWaitAt >= 0.f;
            const Vec3 dir = normalize(g.pl[1].ball.blockWaitVel), at = g.pl[1].ball.pos;
            const float rtt = mode == 3 ? 0.4f : 0.2f, before = 0.45f / length(g.pl[1].ball.blockWaitVel);
            for (float u = 0; u < 0.6f; u += 1.f / 72) {
                // their shield onto the ball's path 0.45 m before it reached them, as they did then: it arrives here
                // their round trip after the ball was there
                if ((mode == 0 || mode == 3) && u >= rtt - before) in[0].hand[1].grip = {gq, at - dir * 0.45f - gq.rotate(sl.p)};
                stepCopy(g, in); log.insert(log.end(), g.events.begin(), g.events.end());
                g.seenLate[0] = mode == 3 ? 0.4f : 0.2f;
            }
            const bool blocked = hasEvent(log, EventType::ShieldBlock, 0), hit = hasEvent(log, EventType::HitPlayer, 0);
            if (mode == 0) CHECK(waited && blocked && !hit && g.pl[1].ball.vel.x > 0.f, "a block arriving within the round trip counts; the ball goes back");
            if (mode == 1) CHECK(waited && !blocked && hit, "no block: the hit counts after the wait");
            if (mode == 2) CHECK(!waited && hit, "the rule off: the hit at once, as before");
            if (mode == 3) CHECK(waited && !blocked && hit, "a round trip past lateBlockMax (0.25 s): a block that late does not count");
        }
    }
    {   // THE REACH ROUND THE FIST (two players' report, 2026-11-17: picking a ball up "too small", "hard to tell where
        // the hitbox is"): the ball on its spawner, the right fist put beside it, behind it or in front of it, the trigger
        // pulled; as before (a match with an older game: FAIR MATCHES) and now
        std::printf("[the reach round the fist]\n");
        struct Place { const char* name; Vec3 d; bool before, now; };   // the ball from the fist (m: forward, right, up)
        const Place places[] = {{"30 cm beside the fist", {0, 0.32f, 0}, false, true},
                                {"25 cm behind the fist", {-0.25f, 0, 0}, false, true},
                                {"15 cm under the fist", {0, 0, -0.15f}, true, true},
                                {"50 cm in front", {0.50f, 0, 0}, true, true},
                                {"60 cm in front", {0.60f, 0, 0}, false, false},
                                {"45 cm beside the fist", {0, 0.45f, 0}, false, false}};
        bool right = true, cueRight = true;
        for (const Place& pl : places)
            for (int now = 0; now < 2; ++now) {
                PlayerInput in[2]; Game g = makeGame(tris, in);
                g.tun.matchVersion = now ? Tunables::kVerAll : Tunables::kVerReach - 1;
                const Quat q = kFaceP0;
                const Vec3 fwd = q.rotate({0, 0, -1}), side = q.rotate({1, 0, 0}), up = {0, 1, 0};
                const Vec3 ball = g.pl[0].ball.pos;
                const Vec3 fist = ball - (fwd * pl.d.x + side * pl.d.y + up * pl.d.z);
                in[0].hand[1].grip = {q, fist - q.rotate(g.tun.fistReachOffset)};
                in[0].hand[1].trigger = 0;
                stepCopy(g, in);
                const bool cue = g.ballInReach(0, 1);
                in[0].hand[1].trigger = 1;
                std::vector<GameEvent> log;
                run(g, in, 0.05f, &log);
                const bool took = hasEvent(log, EventType::Grab, 0);
                const bool want = now ? pl.now : pl.before;
                if (took != want) {
                    right = false;
                    std::printf("  %s, %s: %s\n", pl.name, now ? "now" : "before", took ? "taken" : "not taken");
                }
                if (cue != took) cueRight = false;
            }
        CHECK(right, "beside and behind the fist a ball is taken now (not before); in front as far as before, and no further");
        CHECK(cueRight, "the ball shows it is in reach exactly when the trigger would take it");
    }
    {   // a player's report (11-11: "slap shots work from the side but hardly on a ball coming straight on"): a ball
        // straight at the chest; the right fist sweeps across its path 0.5 m in front, through the ball's middle at the
        // moment it gets there (every one a real touch), at several hand speeds and wrist turns. Offline every one is
        // punched back; online a late one failed for the player who isn't hosting until BLOCKS ONLINE
        // (PA_GUARD=knuckle tools/run_shield_test.py)
        std::printf("[slap shots at a ball coming straight on]\n");
        int punchedBack = 0, total = 0;
        const char* turns[] = {"fist forward", "knuckles up", "fist sideways", "backhand"};
        for (int turn = 0; turn < 4; ++turn)
        for (float hs : {1.f, 3.f, 5.f, 8.f})
        for (float bs : {10.f, 14.f}) {
            PlayerInput in[2]; Game g = makeGame(tris, in);
            g.tun.knuckleMode = 1;
            Quat q = kFaceP0;
            if (turn == 1) q = q * Quat::axisAngle({0, 0, 1}, deg2rad(90.f));
            if (turn == 2) q = q * Quat::axisAngle({0, 1, 0}, deg2rad(90.f));    // the fist pointing across, along the sweep
            if (turn == 3) q = q * Quat::axisAngle({0, 1, 0}, deg2rad(-90.f));   // the back of the hand leading
            const Vec3 chest = g.pl[0].in.head.p - Vec3{0, 0.45f, 0};
            fireOpponentBall(g, chest, bs);
            const Vec3 kOff = q.rotate(g.tun.knuckleOffset);
            const float planeX = g.tun.start[0].x + 0.5f;
            std::vector<GameEvent> log;
            for (float t = 0; t < 2.f; t += 1.f / 72) {
                const Ball& b = g.pl[1].ball;
                const float tt = b.vel.x < -1e-3f ? (planeX - b.pos.x) / b.vel.x : 0.f;   // s until it reaches the plane
                const Vec3 meet = b.pos + b.vel * tt;
                // the guard's middle sweeps along +Z through the meeting point, at hs m/s
                const Vec3 at = Vec3{planeX, meet.y, meet.z} + Vec3{0, 0, -hs * tt};
                in[0].hand[1].grip = {q, at - kOff};
                stepCopy(g, in); log.insert(log.end(), g.events.begin(), g.events.end());
                if (hasEvent(log, EventType::KnuckleBlock, 0) || hasEvent(log, EventType::HitPlayer, 0)) break;
            }
            const bool punched = hasEvent(log, EventType::KnuckleBlock, 0);
            ++total;
            if (punched && g.pl[1].ball.vel.x > 0) ++punchedBack;
            else std::printf("  %-13s hand %.0f m/s, ball %2.0f m/s: %s\n", turns[turn], hs, bs,
                             punched ? "punched, not back" : hasEvent(log, EventType::HitPlayer, 0) ? "went through: hit" : "went through: missed");
        }
        CHECK(punchedBack == total, "every wrist turn, hands at 1 to 8 m/s, balls at 10 and 14 m/s: punched back");
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
    {
        std::printf("[MATCH OPTIONS: hotdog mode, knuckle guards, free play, timed Advanced rounds]\n");
        auto setup = [&](PlayerInput in[2], bool adv, bool shields, int knuckles, float matchT, float roundT) {
            Game g; g.setArena(tris); g.tun.advanced = adv; g.tun.meetTime = 0;
            g.tun.shields = shields; g.tun.knuckleMode = knuckles; g.tun.basicMatchTime = matchT; g.tun.advancedRoundTime = roundT;
            g.reset(false);
            in[0] = standing(g, 0); in[1] = standing(g, 1);
            run(g, in, 3.6f);
            return g;
        };
        {   // HOTDOG: holding a charged ball raises no shield; the ball that would have been blocked scores
            PlayerInput in[2]; Game g = setup(in, false, false, 0, 180, 0);
            grabBall(g, in, 1);
            Quat q = fromTo({0, 1, 0}, {1, 0, 0});
            Vec3 face = g.pl[0].in.head.p;
            Pose sl = g.shieldLocal(1);
            Quat gq = q * sl.q.conj();
            in[0].hand[1].grip = {gq, face + Vec3{0.45f, 0, 0} - gq.rotate(sl.p)};
            run(g, in, 0.05f);
            CHECK(!g.pl[0].hands[1].shieldUp && g.pl[0].ball.charged, "hotdog mode: no shield, even holding a charged ball");
            fireOpponentBall(g, face);
            std::vector<GameEvent> log;
            run(g, in, 2.0f, &log);
            CHECK(!hasEvent(log, EventType::ShieldBlock) && g.pl[1].score == 1, "hotdog mode: the ball goes through and scores");
        }
        {
            PlayerInput in[2];
            Game b = setup(in, false, true, -1, 180, 0);
            CHECK(b.pl[0].hands[0].knuckleUp, "Basic: knuckle guards on as the rules say");
            Game b0 = setup(in, false, true, 0, 180, 0);
            CHECK(!b0.pl[0].hands[0].knuckleUp, "Basic with knuckle guards off: none");
            Game a1 = setup(in, true, true, 1, 0, 0);
            CHECK(a1.pl[0].hands[0].knuckleUp, "Advanced with knuckle guards on: there");
            Game a0 = setup(in, true, true, -1, 0, 0);
            CHECK(!a0.pl[0].hands[0].knuckleUp, "Advanced: no knuckle guards as the rules say");
        }
        {   // FREE PLAY: no clock, no winner
            PlayerInput in[2]; Game g = setup(in, false, true, -1, 0, 0);
            std::vector<GameEvent> log;
            run(g, in, 240.f, &log);
            CHECK(g.match == MatchState::InProgress && !hasEvent(log, EventType::MatchEnd) && !hasEvent(log, EventType::Overtime),
                  "free play: still playing after 4 minutes, no match end, no overtime");
        }
        {   // Advanced, 1 minute rounds: at time the one ahead wins the round
            PlayerInput in[2]; Game g = setup(in, true, true, -1, 180, 60);
            CHECK(g.match == MatchState::InProgress && g.roundClock > 50.f && g.roundClock <= 60.f, "timed round: the clock runs from 1:00");
            g.pl[1].score = 2;
            std::vector<GameEvent> log;
            run(g, in, 61.f, &log);
            CHECK(hasEvent(log, EventType::RoundEnd, 1) && g.pl[1].roundWins == 1, "timed round: at time the one ahead (2-0) wins it");
            run(g, in, g.tun.roundSummaryTime + g.tun.countdown + 0.5f);
            CHECK(g.match == MatchState::InProgress && g.round == 2 && g.roundClock > 55.f && !g.overtime, "the next round starts its own 1:00");
        }
        {   // Advanced, timed, tied at time: sudden death, the next point wins the round
            PlayerInput in[2]; Game g = setup(in, true, true, -1, 180, 60);
            std::vector<GameEvent> log;
            run(g, in, 61.f, &log);
            CHECK(g.overtime && hasEvent(log, EventType::Overtime) && g.match == MatchState::InProgress, "timed round tied 0-0 at time: sudden death");
            log.clear();
            fireOpponentBall(g, g.pl[0].in.head.p - Vec3{0, 0.3f, 0});
            run(g, in, 2.0f, &log);
            CHECK(hasEvent(log, EventType::RoundEnd, 1) && g.pl[1].roundWins == 1 && g.pl[1].score == 1,
                  "sudden death: one point (1-0, short of 4) wins the round");
        }
        {
            Tunables t;
            CHECK(t.statsMode() == 0, "stats mode: the rules as they come are standard");
            t.shields = false; t.basicMatchTime = 300;
            CHECK(t.statsMode() == 1, "stats mode: any match without shields is HOTDOG");
            t.shields = true;
            CHECK(t.statsMode() == 2, "stats mode: other options are custom");
            t.advanced = true; t.basicMatchTime = 180; t.advancedRoundTime = 0;
            CHECK(t.statsMode() == 0 && !t.freePlay() && !t.timed(), "Advanced without a round time is standard and untimed");
            t.knuckleMode = 0;   // "off" chosen, then Advanced (which has none): still the rules as they come
            CHECK(t.statsMode() == 0, "knuckle guards count by what is in play, not by how they were chosen");
        }
    }
    {
        std::printf("[foul ball: stays in the arena (owner, 2026-10-21)]\n");
        {   // a low ball landing just short of the far foul line: it bounces on as off the floor (2026-10-28: the zone's
            // edge sent it nearly straight up, then it bounced between floor and ceiling)
            PlayerInput in[2]; Game g0 = makeGame(tris, in);
            int upright = 0;
            for (int k = 0; k < 40 * 5; ++k) {   // landing spots 1 cm apart across the line, for every ball size
                Game g = g0;
                Ball& b = g.pl[0].ball;
                g.pl[1].strikesAgainst = k / 40;
                b.scale = 1.f + g.tun.scaleIncreasePerStrike * (k / 40);
                b.state = BallState::Flying; b.instigator = 0; b.rechargePending = false; b.scored = false; b.spin = {};
                b.pos = {4.0f + 0.01f * (k % 40), 0.6f, -1.6f};
                b.speedLimit = 7.f; b.vel = normalize(Vec3{6.5f, -3.7f, 2.0f}) * 7.f;
                const float vx = b.vel.x;
                bool bounced = false;
                for (int i = 0; i < 72 && !bounced; ++i) {
                    PlayerInput tmp[2] = {in[0], in[1]};
                    g.step(1.f / 72, tmp);
                    bounced = hasEvent(g.events, EventType::Bounce, 0);
                }
                if (!bounced || b.vel.x < 0.8f * vx) ++upright;
            }
            CHECK(upright == 0, "a ball landing at the foul line bounces on along the court, not straight up");
        }
        {   // your throw landing in the OTHER player's foul zone is no foul: it bounces on as off any floor (owner, 2026-10-28)
            PlayerInput in[2]; Game g = makeGame(tris, in);
            Ball& b = g.pl[0].ball;
            b.state = BallState::Flying; b.instigator = 0; b.rechargePending = false; b.scored = false; b.spin = {};
            b.pos = {4.6f, 1.0f, 1.0f}; b.speedLimit = 9.f; b.vel = normalize(Vec3{6.2f, 0.f, 1.2f} - b.pos) * 9.f;
            b.progX = b.pos.x; b.progT = 0; b.timer = 0;
            std::vector<GameEvent> log;
            bool bounced = false;
            for (int i = 0; i < 36 && !bounced; ++i) {
                PlayerInput tmp[2] = {in[0], in[1]};
                g.step(1.f / 72, tmp);
                log.insert(log.end(), g.events.begin(), g.events.end());
                bounced = hasEvent(g.events, EventType::Bounce, 0);
            }
            CHECK(bounced && !hasEvent(log, EventType::Foul) && b.state == BallState::Flying && b.vel.y > 0 && !b.homing,
                  "your throw landing in their foul zone just bounces (no foul)");
        }
        {   // your ball blocked or punched into THEIR own foul zone: their strike, and it comes back at you as from their
            // back wall, live (owner, 2026-10-29)
            PlayerInput in[2]; Game g = makeGame(tris, in);
            Ball& b = g.pl[0].ball;
            b.state = BallState::Flying; b.instigator = 1; b.rechargePending = false; b.scored = false; b.spin = {};   // (their touch)
            b.pos = {5.6f, 0.9f, 0.3f}; b.speedLimit = 8.f; b.vel = normalize(Vec3{0.4f, -1.f, 0.f}) * 8.f;
            b.progX = b.pos.x; b.progT = 0; b.timer = 0;
            std::vector<GameEvent> log;
            for (int i = 0; i < 72 * 4 && !hasEvent(log, EventType::HitPlayer); ++i) {
                PlayerInput tmp[2] = {in[0], in[1]};
                g.step(1.f / 72, tmp);
                log.insert(log.end(), g.events.begin(), g.events.end());
            }
            bool strikeThem = false, hitMe = false;
            for (const GameEvent& e : log) {
                strikeThem |= e.type == EventType::Strike && e.player == 1;
                hitMe |= e.type == EventType::HitPlayer && e.player == 0;
            }
            CHECK(strikeThem && hasEvent(log, EventType::Returned, 0), "their block into their own foul zone's floor: their strike, and it comes back");
            CHECK(hitMe, "... live: standing still, its owner is hit by it");
            CHECK(!hasEvent(log, EventType::Foul), "... and no FOUL shown for it (it was never their throw)");
        }
        {   // ... but off their own end's side wall it is a wall: it bounces on (owner, 2026-10-30)
            PlayerInput in[2]; Game g = makeGame(tris, in);
            Ball& b = g.pl[0].ball;
            b.state = BallState::Flying; b.instigator = 1; b.rechargePending = false; b.scored = false; b.spin = {};   // (their touch)
            b.pos = {6.5f, 1.5f, 1.6f}; b.speedLimit = 8.f; b.vel = normalize(Vec3{0.3f, 0.f, 1.f}) * 8.f;
            b.progX = b.pos.x; b.progT = 0; b.timer = 0;
            std::vector<GameEvent> log;
            for (int i = 0; i < 36; ++i) {
                PlayerInput tmp[2] = {in[0], in[1]};
                g.step(1.f / 72, tmp);
                log.insert(log.end(), g.events.begin(), g.events.end());
            }
            bool strikeThem = false, bounced = false;
            for (const GameEvent& e : log) { strikeThem |= e.type == EventType::Strike && e.player == 1; bounced |= e.type == EventType::Bounce; }
            CHECK(bounced && !strikeThem && !hasEvent(log, EventType::Foul) && b.instigator == 1,
                  "their block off their own end's side wall: a plain bounce (no strike, no foul)");
        }
        {   // past the foul line, then back down into its own foul zone: bounce, wall, home; harmless
            PlayerInput in[2]; Game g = makeGame(tris, in);
            Ball& b = g.pl[0].ball;
            b.state = BallState::Flying; b.instigator = 0; b.rechargePending = false; b.scored = false; b.spin = {};
            b.pos = {-3.6f, 1.3f, 0.4f};
            b.speedLimit = 8.f; b.vel = normalize(Vec3{-5.6f, 0.f, 1.2f} - b.pos) * 8.f;
            std::vector<GameEvent> log;
            bool dissolved = false, homeBound = false;
            for (int i = 0; i < 72 * 8 && b.state != BallState::Idle; ++i) {
                PlayerInput tmp[2] = {in[0], in[1]};
                g.step(1.f / 72, tmp);
                log.insert(log.end(), g.events.begin(), g.events.end());
                dissolved |= b.state == BallState::Spawning || b.state == BallState::Dead;
                homeBound |= b.homing;
            }
            CHECK(hasEvent(log, EventType::Foul, 0), "a ball landing in its owner's foul zone is a foul");
            CHECK(!dissolved && !hasEvent(log, EventType::SpawnRise), "it no longer dissolves or rises from under the floor");
            CHECK(homeBound && b.state == BallState::Idle && length(b.pos - g.idlePoint(0)) < 0.6f,
                  "it bounces off the wall it was heading for and flies home to its launch spot");
            CHECK(g.pl[0].score == 0 && g.pl[1].score == 0 && !hasEvent(log, EventType::HitPlayer), "nobody scores from a fouled ball");
        }
        {   // never crossed the foul line: springs straight back to its float position
            PlayerInput in[2]; Game g = makeGame(tris, in);
            Ball& b = g.pl[0].ball;
            b.state = BallState::Flying; b.instigator = 0; b.rechargePending = true; b.scored = false; b.spin = {};
            b.pos = {-5.4f, 0.9f, 0.6f}; b.speedLimit = 8.f; b.vel = normalize(Vec3{-5.2f, 0.f, 0.8f} - b.pos) * 8.f;
            std::vector<GameEvent> log;
            bool dissolved = false;
            float home = -1;
            for (int i = 0; i < 72 * 3; ++i) {
                PlayerInput tmp[2] = {in[0], in[1]};
                g.step(1.f / 72, tmp);
                log.insert(log.end(), g.events.begin(), g.events.end());
                dissolved |= b.state == BallState::Spawning || b.state == BallState::Dead;
                if (home < 0 && b.state == BallState::Idle) home = i / 72.f;
            }
            std::printf("  (back at its float position after %.2f s)\n", home);
            CHECK(!dissolved && b.state == BallState::Idle && length(b.pos - g.idlePoint(0)) < 0.2f,
                  "a ball that never crossed the foul line bounces and is drawn back to its float position (no dissolving)");
            CHECK(home > 0 && home < 1.0f, "and it is back within a second (the magnetic pull)");
        }
        {   // stuck balls (2026-10-28): no headway along the court, or slowed to a crawl, go home harmless
            auto fly = [&](Vec3 pos, Vec3 vel, float seconds, std::vector<GameEvent>& log, bool& home, float& at) {
                PlayerInput in[2]; Game g = makeGame(tris, in);
                Ball& b = g.pl[0].ball;
                b.state = BallState::Flying; b.instigator = 0; b.rechargePending = false; b.scored = false; b.spin = {};
                b.pos = pos; b.vel = vel; b.speedLimit = length(vel); b.timer = 0; b.progX = pos.x; b.progT = 0;
                home = false; at = -1;
                for (int i = 0; i < int(72 * seconds); ++i) {
                    PlayerInput tmp[2] = {in[0], in[1]};
                    g.step(1.f / 72, tmp);
                    log.insert(log.end(), g.events.begin(), g.events.end());
                    if (b.homing && at < 0) at = i / 72.f;
                    home |= b.state == BallState::Idle;
                }
            };
            std::vector<GameEvent> log;
            bool home; float at;
            fly({1.0f, 1.8f, 0.3f}, {0.2f, 9.f, 0.1f}, 6.f, log, home, at);   // between floor and ceiling, all but upright
            CHECK(home && at > 1.8f && at < 2.6f && !hasEvent(log, EventType::HitPlayer),
                  "a ball bouncing up and down without getting anywhere goes home after 2 s, harmless");
            log.clear();
            fly({-2.0f, 1.5f, 0.f}, {1.7f, 0.f, 0.3f}, 4.f, log, home, at);   // a crawl past the foul line
            CHECK(home && at > 0.9f && at < 1.2f, "a ball crawling under 2 m/s goes home (after a second)");
            log.clear();
            fly({-2.0f, 1.5f, 0.f}, {12.f, 0.5f, 3.f}, 1.2f, log, home, at);   // a real throw: untouched
            CHECK(at < 0, "a throw on its way down the court is left alone");
        }
        {   // a fouled ball keeps its own speed to the wall, not a jump to 12 m/s (2026-10-28)
            PlayerInput in[2]; Game g = makeGame(tris, in);
            Ball& b = g.pl[1].ball;   // orange's ball, knocked back into orange's own zone
            b.state = BallState::Flying; b.instigator = 1; b.rechargePending = false; b.scored = false; b.spin = {};
            b.pos = {5.4f, 0.8f, 0.3f}; b.speedLimit = 6.f; b.vel = normalize(Vec3{0.6f, -1.f, 0.f}) * 6.f;
            b.progX = b.pos.x; b.progT = 0; b.timer = 0;
            float speedAfter = -1;
            for (int i = 0; i < 72 && speedAfter < 0; ++i) {
                PlayerInput tmp[2] = {in[0], in[1]};
                g.step(1.f / 72, tmp);
                if (hasEvent(g.events, EventType::Foul)) speedAfter = b.speedLimit;
            }
            CHECK(speedAfter > 5.9f && speedAfter < 6.1f, "a fouled ball keeps its own speed");
        }
        {   // a weak throw that never reaches the foul line: drawn home by the same pull
            PlayerInput in[2]; Game g = makeGame(tris, in);
            Ball& b = g.pl[0].ball;
            b.state = BallState::Flying; b.instigator = 0; b.rechargePending = true; b.scored = false; b.spin = {};
            b.pos = {-5.6f, 1.4f, 0.2f}; b.speedLimit = 2.f; b.vel = Vec3{-0.4f, 0.3f, 0.f};
            float home = -1;
            for (int i = 0; i < 72 * 3 && home < 0; ++i) {
                PlayerInput tmp[2] = {in[0], in[1]};
                g.step(1.f / 72, tmp);
                if (b.state == BallState::Idle) home = i / 72.f;
            }
            CHECK(home > 0 && home < 1.0f, "a weak throw is drawn back home within a second");
        }
    }
    {
        std::printf("[match statistics: throw accuracy is literal, returns count apart (GitHub #14)]\n");
        auto ev = [](EventType t, int p, float v, int ball) { GameEvent e{t, p, -1, {}, v}; e.ball = ball; return e; };
        // what the community server accepts (server/projectarena_relay.py, 0.3.9)
        auto valid = [](const MatchTally& s) {
            return s.onTarget <= s.throws && s.returnHits <= s.hits && s.returnHits <= s.blocks &&
                   s.hits - s.returnHits <= s.onTarget && s.blocks <= s.faced;
        };
        {   // the issue's case: 0 throws, 1 blocks it back, the return hits 0
            StatCounter c;
            c.add(ev(EventType::Throw, 0, 9.f, -1));
            c.add(ev(EventType::ShieldBlock, 1, 0, 0));
            c.add(ev(EventType::HitPlayer, 0, 1.f, 0));
            const MatchTally &a = c.s[0], &b = c.s[1];
            CHECK(a.throws == 1 && a.onTarget == 1 && a.hits == 0 && a.faced == 1, "the thrower: one throw, on target (it was blocked)");
            CHECK(b.throws == 0 && b.onTarget == 0 && b.hits == 1 && b.returnHits == 1 && b.blocks == 1,
                  "the defender: a point from a return, no throw on target");
            CHECK(valid(a) && valid(b), "the server accepts both players' numbers");
        }
        {   // one throw that strikes and then hits: on target once
            StatCounter c;
            c.add(ev(EventType::Throw, 0, 9.f, -1));
            c.add(ev(EventType::Strike, 1, 1.f, 0));
            c.add(ev(EventType::HitPlayer, 1, 0.f, 0));
            CHECK(c.s[0].onTarget == 1 && c.s[0].hits == 1 && c.s[0].strikes == 1 && c.s[0].returnHits == 0 && c.s[1].faced == 1,
                  "a throw that strikes and then hits is on target once");
        }
        {   // your own ball hits you: the other player's point, but nobody's shot
            StatCounter c;
            c.add(ev(EventType::Throw, 0, 9.f, -1));
            c.add(ev(EventType::HitPlayer, 0, 1.f, 0));
            CHECK(c.s[1].hits == 0 && c.s[1].onTarget == 0 && c.s[0].onTarget == 0, "an own goal is nobody's hit or throw on target");
        }
        {   // the shot clock's automatic strike belongs to no ball
            StatCounter c;
            c.add(ev(EventType::Throw, 0, 9.f, -1));
            c.add(ev(EventType::Strike, 1, 1.f, -1));
            CHECK(c.s[0].strikes == 1 && c.s[0].onTarget == 0, "an automatic strike counts as a strike, not a throw on target");
        }
        {   // a returned ball blocked back again: the first thrower's throw on target once, then it's a return again
            StatCounter c;
            c.add(ev(EventType::Throw, 0, 9.f, -1));
            c.add(ev(EventType::KnuckleBlock, 1, 9.f, 0));
            c.add(ev(EventType::ShieldBlock, 0, 0, 0));
            c.add(ev(EventType::HitPlayer, 1, 0.f, 0));
            CHECK(c.s[0].onTarget == 1 && c.s[0].hits == 1 && c.s[0].returnHits == 1 && c.s[1].onTarget == 0 && valid(c.s[0]) &&
                  valid(c.s[1]), "a rally of blocks: each throw on target once; the last return scores");
        }
        {   // four players (FOUR CORNERS, 0.3.23, for The Crowd): the one attacking is whoever threw or returned that ball
            StatCounter c;
            c.np = 4;
            c.add(ev(EventType::Throw, 2, 11.f, -1));     // 2 throws its ball
            c.add(ev(EventType::ShieldBlock, 3, 0, 2));   // 3 blocks it: 2's throw on target, now 3's return
            c.add(ev(EventType::HitPlayer, 0, 3.f, 2));   // the return hits 0: 3 scores
            c.add(ev(EventType::Throw, 1, 8.f, -1));      // 1 throws, and strikes 2
            c.add(ev(EventType::Strike, 2, 1.f, 1));
            c.add(ev(EventType::Caught, 1, 0, -1));
            CHECK(c.s[2].throws == 1 && c.s[2].onTarget == 1 && c.s[2].hits == 0 && c.s[2].topSpeed == 11.f,
                  "four players: a blocked throw is on target once, for its thrower");
            CHECK(c.s[3].blocks == 1 && c.s[3].faced == 1 && c.s[3].hits == 1 && c.s[3].returnHits == 1 && c.s[0].faced == 1,
                  "... the blocker's return scores, as theirs, against the one it hit");
            CHECK(c.s[1].strikes == 1 && c.s[1].onTarget == 1 && c.s[2].faced == 1 && c.s[1].catches == 1 && c.s[3].strikes == 0 &&
                  c.s[0].strikes == 0, "... a strike goes to whoever threw that ball (not to player 1 - p)");
            bool ok = true;
            for (int p = 0; p < 4; ++p) ok = ok && valid(c.s[p]);
            CHECK(ok, "... and the server's checks hold for all four");
        }
        {   // real matches: two lively bots for ten minutes; every event behind a point names its ball
            Game g; g.setArena(tris); g.reset(true);
            g.pl[0].isBot = true;
            PlayerInput in[2] = {standing(g, 0), {}};
            std::vector<GameEvent> log;
            run(g, in, 600.f, &log);
            StatCounter c;
            int untagged = 0, points[2] = {0, 0};
            bool ok = true;
            for (const GameEvent& e : log) {
                if (e.type == EventType::RoundStart && (int)e.value <= 1) c = StatCounter{};
                if ((e.type == EventType::HitPlayer || e.type == EventType::ShieldBlock || e.type == EventType::KnuckleBlock) && e.ball < 0)
                    ++untagged;
                if (e.type == EventType::HitPlayer) ++points[(int)e.value & 1];
                c.add(e);
                ok = ok && valid(c.s[0]) && valid(c.s[1]);
            }
            int returns = 0, blocks = 0;
            for (const GameEvent& e : log) blocks += e.type == EventType::ShieldBlock || e.type == EventType::KnuckleBlock;
            for (int p = 0; p < 2; ++p) returns += c.s[p].returnHits;
            std::printf("       (bots: %d + %d points, %d blocks, %d events)\n", points[0], points[1], blocks, (int)log.size());
            CHECK(points[0] + points[1] > 4 && blocks > 2, "(test setup: the bots score and block)");
            CHECK(untagged == 0, "every hit and block names the ball behind it");
            CHECK(ok, "the counts stay acceptable to the server all through ten minutes of bot matches");
        }
    }
    avatarTests(tris);
    botTests(tris);
    matchLevelTests(tris);
    stanceTests(tris);
    recordTests(tris);
    trainingTests(tris);
    octagonTests(tris);
    {   // BALL IN FLIGHT (0.3.24; the owner, 2026-11-20: "it's not increasing sound as it gets closer"): how a ball sounds
        std::printf("[BALL IN FLIGHT: hear it coming]\n");
        const Vec3 head{0, 1.6f, 0}, right{1, 0, 0};   // (facing -Z)
        const FlightParams far = flightParams({0, 1.5f, -12}, {0, 0, 11}, head, right), mid = flightParams({0, 1.5f, -6}, {0, 0, 11}, head, right),
                           close = flightParams({0, 1.5f, -1.5f}, {0, 0, 11}, head, right);
        CHECK(far.gain > 0.1f && mid.gain > far.gain * 1.5f && close.gain > mid.gain * 1.5f && close.bright > mid.bright && mid.bright > far.bright,
              "a ball coming at you: louder and brighter the nearer it is (heard from across the court)");
        const FlightParams past = flightParams({-2.f, 1.5f, -6}, {0, 0, 11}, head, right);
        CHECK(past.gain < mid.gain * 0.6f && past.pan < -0.2f && mid.pan > -0.05f && mid.pan < 0.05f,
              "one that will pass 2 m to your left: quieter than one at you, and on your left");
        const FlightParams away = flightParams({0, 1.5f, -6}, {0, 0, -11}, head, right), slow = flightParams({0, 1.5f, -6}, {0, 0, 4}, head, right);
        CHECK(away.gain < 0.01f && slow.pitch < mid.pitch && mid.pitch > 1.3f,
              "going away: silent; the hum rises with how fast it comes at you");
        FlightSynth syn;
        syn.want = close;
        std::vector<float> buf(48000 / 10 * 2, 0.f);
        syn.render(buf.data(), 48000 / 10, 1.f);
        float peak = 0, rms = 0;
        bool finite = true;
        for (float x : buf) { peak = std::max(peak, std::fabs(x)); rms += x * x; finite &= std::isfinite(x); }
        rms = std::sqrt(rms / buf.size());
        FlightSynth off;
        std::vector<float> quiet(48000 / 10 * 2, 0.f);
        off.render(quiet.data(), 48000 / 10, 0.f);
        float q = 0;
        for (float x : quiet) q = std::max(q, std::fabs(x));
        char msg[128];
        std::snprintf(msg, sizeof msg, "the sound: heard, never past full scale (peak %.2f, loudness %.3f); BALL SOUND off: silent", peak, rms);
        CHECK(finite && rms > 0.02f && peak < 0.95f && q == 0.f, msg);
    }
    {   // NEW BOT (0.3.23; the owner, 2026-11-20: "hosts may not want it to change so we need a push to change button"):
        // the bot's stance comes from botLook: the same every match until a new one; beginners an open one
        std::printf("[NEW BOT: a stance until a new one]\n");
        auto stanceAfter = [&](uint32_t look, int level, int matches) {
            Game g; g.setArena(tris); g.tun.botLevel = level; g.botLook = look; g.reset(true);
            std::vector<int> seen;
            for (int m = 0; m < matches; ++m) {
                g.resetWithBots(2);   // (a new match: the stance is picked again)
                PlayerInput in[2] = {standing(g, 0), {}};
                run(g, in, 0.3f);
                seen.push_back(g.pl[1].botStance);
            }
            return seen;
        };
        const std::vector<int> kept = stanceAfter(0xC0FFEEu, 3, 4);
        bool same = kept[0] >= 0;
        for (int s : kept) same &= s == kept[0];
        CHECK(same, "the bot keeps its stance match after match (the same botLook)");
        bool open = true;
        int kinds = 0;
        unsigned seenMask = 0;
        for (uint32_t look = 1; look <= 40; ++look) {
            const int s = stanceAfter(look * 7919u, 3, 1)[0];
            if (s >= 0 && s < kBotStances && !((seenMask >> s) & 1u)) { seenMask |= 1u << s; ++kinds; }
            const int r = stanceAfter(look * 7919u, 0, 1)[0];   // (ROOKIE: no guard)
            open &= r == STANCE_KITTEN || r == STANCE_RELAXED || r == STANCE_MONKEY;
        }
        char msg[112];
        std::snprintf(msg, sizeof msg, "NEW BOT's other looks bring other stances (%d of %d over 40), a ROOKIE only open ones", kinds, kBotStances);
        CHECK(kinds >= 5 && open, msg);
        Game four; four.botLook = 0xC0FFEEu;
        bool differ = false;
        for (int p = 1; p < 4; ++p) differ |= four.botStanceFor(p, 3.f) != four.botStanceFor(0, 3.f);
        CHECK(differ && four.botStanceFor(1, 3.f) == four.botStanceFor(1, 3.f), "four-player bots: each corner a stance of its own");
    }
    {   // the slap shot (owner, 2026-10-29): the stronger bots send a caught ball straight back; the beginners never do
        std::printf("[bots: the slap shot]\n");
        int slaps[2] = {0, 0}, catches[2] = {0, 0}, shaped = 0;   // (shaped: UNBEATABLE's slaps curved or off a wall)
        for (int k = 0; k < 2; ++k) {
            Game g; g.setArena(tris); g.tun.botLevel = k ? kBotLevels - 1 : 0; g.tun.basicMatchTime = 0; g.tun.meetTime = 0;
            g.resetWithBots(3u);
            PlayerInput in[2];
            float caughtAt[2] = {-9, -9};
            for (int i = 0; i < 72 * 180; ++i) {
                PlayerInput tmp[2] = {in[0], in[1]};
                g.step(1.f / 72, tmp);
                for (const GameEvent& e : g.events) {
                    if (e.player < 0 || e.player > 1) continue;
                    if (e.type == EventType::Caught) { caughtAt[e.player] = g.now; ++catches[k]; }
                    if (e.type == EventType::Throw && g.now - caughtAt[e.player] < 0.6f) {
                        ++slaps[k];
                        const Ball& b = g.pl[e.player].ball;
                        const Vec3 to = normalize(g.chestPoint(1 - e.player) - b.pos), v = normalize(b.vel);
                        if (k && (length(b.spin) > 3.f || dot(normalize(Vec3{to.x, 0, to.z}), normalize(Vec3{v.x, 0, v.z})) < 0.96f)) ++shaped;
                    }
                }
            }
        }
        std::printf("       ROOKIE: %d catches, %d slap shots; UNBEATABLE: %d catches, %d slap shots (%d curved or off a wall)\n", catches[0],
                    slaps[0], catches[1], slaps[1], shaped);
        // (a shape only when its trial flight would score; against itself, with the knuckle guards on the fist (2026-11-17),
        // it catches and slaps more, the shaped ones as many as before (2), the rest straight: 1 in 5 became 1 in 11)
        CHECK(shaped * 12 >= slaps[1] && shaped >= 2, "UNBEATABLE's slaps curve and bank too (planned while its ball comes back)");
        CHECK(slaps[0] == 0 && catches[1] > 4 && slaps[1] * 3 >= catches[1], "UNBEATABLE often sends its caught ball straight back; ROOKIE never");
    }
    {   // the attack plan (testers, 2026-10-30): CHAMPION and UNBEATABLE go for strikes, left and right of the ring in turn,
        // one throw in seven at the player; holding 3 strikes (a big, fast ball), nearly every throw at the player
        std::printf("[bots: the attack plan]\n");
        // (and every shape of throw, testers 2026-10-30: curved, off the walls, curved off the walls, off the floor)
        int shapes[5] = {0, 0, 0, 0, 0}, landedCurved = 0, landedBounced = 0, landedBoth = 0;
        for (int adv = 0; adv < 2; ++adv) {
            Game g; g.setArena(tris); g.tun.advanced = adv; g.tun.botPlay = adv; g.tun.basicMatchTime = 0; g.tun.meetTime = 0;
            g.tun.botLevel = kBotLevels - 1; g.reset(true);   // (timed with the BRAWLER, rounds with the DUELLIST)
            g.tun.botIdle = false;   // (BOT IDLE MOTION off: these counts come from fixed seeds; with it each level measures the same: PA_BOT_MEASURE)
            PlayerInput in[2] = {standing(g, 0), {}};
            bool thrownCurved = false;
            int throwsCold = 0, atCold = 0, throwsHot = 0, atHot = 0, strikes = 0, onSide = 0, sides[2] = {0, 0}, hits = 0;
            for (int i = 0; i < 72 * 240; ++i) {
                in[0].buttonA = g.match == MatchState::MatchSummary || g.match == MatchState::RoundSummary;
                const int held = g.strikesHeld(1);
                PlayerInput tmp[2] = {in[0], in[1]};
                g.step(1.f / 72, tmp);
                for (const GameEvent& e : g.events) {
                    if (e.type == EventType::Throw && e.player == 1) {
                        ++shapes[std::max(0, std::min(4, g.pl[1].botStyle))];
                        thrownCurved = length(g.pl[1].ball.spin) > 3.f;   // (as it leaves the hand: spin fades in flight)
                        if (held >= 3) { ++throwsHot; atHot += g.pl[1].botIntent == 1; }
                        else { ++throwsCold; atCold += g.pl[1].botIntent == 1; }
                    }
                    if (e.type == EventType::Strike && e.player == 0 && e.ball == 1) {   // (not the shot clock's)
                        const float side = e.pos.z - g.strikeZoneCenter(0).z;
                        onSide += side * g.pl[1].botStrikeSide < 0;   // (the side it meant: flipped for the next one as it threw)
                        sides[side > 0] += 1;
                        ++strikes;
                    }
                    hits += e.type == EventType::HitPlayer && e.player == 0;
                    if ((e.type == EventType::HitPlayer && e.player == 0) || (e.type == EventType::Strike && e.player == 0 && e.ball == 1)) {
                        const Ball& b = g.pl[1].ball;
                        const bool curved = thrownCurved, bounced = b.bounces > 0;
                        landedCurved += curved; landedBounced += bounced; landedBoth += curved && bounced;
                    }
                }
            }
            const char* mode = adv ? "Advanced" : "Basic";
            std::printf("       %s: %d throws under 3 strikes (%d at the player), %d at 3 (%d at the player); %d strikes (%d / %d each side, %d where it meant), %d hits\n",
                        mode, throwsCold, atCold, throwsHot, atHot, strikes, sides[0], sides[1], onSide, hits);
            char msg[112];
            if (!adv) {
                std::snprintf(msg, sizeof msg, "%s: under 3 strikes it goes for strikes, about one throw in seven at the player", mode);
                CHECK(throwsCold > 20 && atCold * 20 >= throwsCold && atCold * 4 <= throwsCold && strikes * 2 >= throwsCold - atCold, msg);
                std::snprintf(msg, sizeof msg, "%s: its strikes go left and right of the ring in turn", mode);
                CHECK(strikes > 10 && onSide * 10 >= strikes * 9 && sides[0] * 3 >= strikes && sides[1] * 3 >= strikes, msg);
                std::snprintf(msg, sizeof msg, "%s: holding 3 strikes, nearly every throw is at the player, and hits", mode);
                CHECK(throwsHot > 5 && atHot * 10 >= throwsHot * 7 && hits > 5, msg);
            } else {   // (BOT STYLES, 2026-11-17: ADVANCED's duellist, first to 4 points, goes at you more, and at once when
                       // you have no shield; its strikes still alternate)
                std::snprintf(msg, sizeof msg, "%s (the duellist): under 3 strikes a quarter to three quarters of its throws at the player", mode);
                CHECK(throwsCold > 20 && atCold * 4 >= throwsCold && atCold * 4 <= throwsCold * 3, msg);
                std::snprintf(msg, sizeof msg, "%s: its strikes go left and right of the ring in turn", mode);
                CHECK(strikes > 6 && onSide * 10 >= strikes * 9 && sides[0] * 4 >= strikes && sides[1] * 4 >= strikes, msg);
                std::snprintf(msg, sizeof msg, "%s: holding 3 strikes, nearly every throw is at the player, and it hits", mode);
                CHECK(atHot * 10 >= throwsHot * 7 && hits > 10, msg);
            }
        }
        std::printf("       shapes thrown: %d straight, %d curved, %d off a wall, %d curved off a wall, %d off the floor; "
                    "points and strikes: %d curved, %d off a wall or the floor, %d both\n",
                    shapes[0], shapes[1], shapes[2], shapes[3], shapes[4], landedCurved, landedBounced, landedBoth);
        CHECK(shapes[0] > 0 && shapes[1] > 0 && shapes[2] > 0 && shapes[3] > 0 && shapes[4] > 0, "it throws every shape");
        CHECK(landedCurved > 5 && landedBounced > 5 && landedBoth > 0, "and they land: curves, banks, curves off the walls");
    }
    std::printf("[HOST HANDOFF: a match taken over goes on from where it was (0.3.13)]\n");
    {
        Game g;
        g.tun.advanced = false;
        g.resetWithBots(3u);
        PlayerInput in[kMaxPlayers];
        for (int i = 0; i < 90 * 20 && g.match != MatchState::InProgress; ++i) g.step(1.f / 90.f, in);
        g.pl[0].score = 5; g.pl[1].score = 4;
        g.serve(0, g.tun.start[0] + Vec3{0, 1.3f, 0}, Vec3{0, 0, 1}, 8.f, {}, false);
        const float clock = g.roundClock;
        const bool flying = g.pl[0].ball.state == BallState::Flying;
        g.resumeAfterHandoff();
        CHECK(flying && g.pl[0].ball.state == BallState::Spawning, "a ball in the air goes back to its thrower");
        CHECK(g.match == MatchState::Countdown && g.pl[0].score == 5 && g.pl[1].score == 4 && g.roundClock == clock,
              "a match being played counts down again, the score and the clock as they were");
        for (int i = 0; i < 90 * 6 && g.match == MatchState::Countdown; ++i) g.step(1.f / 90.f, in);
        CHECK(g.match == MatchState::InProgress && g.pl[0].score == 5 && g.roundClock <= clock && g.roundClock > clock - 1.f,
              "then it plays on: nothing reset");
    }
    std::printf("[READY UP: a room for four starts when every person in it pressed A (0.3.14)]\n");
    for (int room = 1; room <= 2; ++room) {   // FOUR CORNERS and FIREBALL BLITZ: two people, two bots
        Game g;
        g.tun.experimental = true; g.tun.expRoom = room; g.tun.players = 4;
        if (room == ROOM_BLITZ) g.useArena(ARENA_OCTAGON, {});
        g.tun.readyUp = true;
        g.resetWithBots(0xCu);
        PlayerInput in[kMaxPlayers];
        auto run = [&](float secs, bool a0, bool a1) {
            for (int i = 0; i < (int)(secs * 90); ++i) {
                for (int p = 0; p < 2; ++p) {
                    in[p] = PlayerInput(); in[p].headTracked = true;
                    in[p].head = {Quat{}, g.tun.start[p] + Vec3{0, 1.6f, 0}};
                    in[p].buttonA = p == 0 ? a0 : a1;
                }
                g.step(1.f / 90.f, in);
            }
        };
        run(3.f, false, false);
        const bool waited = g.match == MatchState::WaitingForPlayers;
        run(0.2f, true, false);
        run(3.f, false, false);
        const bool waitedForOne = g.match == MatchState::WaitingForPlayers && g.pl[0].ready && !g.pl[1].ready;
        run(0.2f, false, true);
        run(0.3f, false, false);
        char msg[112];
        std::snprintf(msg, sizeof msg, "%s: nothing starts until everyone is READY, then the countdown", room == 1 ? "FOUR CORNERS" : "FIREBALL BLITZ");
        CHECK(waited && waitedForOne && g.match == MatchState::Countdown, msg);
    }
    {   // ELBOWS OUT OF THE BODY (owner, 2026-11-16, her video in the builder's mirror: hands together at the chest, the
        // elbows sank into the torso): the elbow swings out round the shoulder-to-wrist line only as far as it must
        std::printf("[elbows out of the body]\n");
        struct HandPose { Vec3 p; float pitch, yaw; };
        auto make = [](int body, const HandPose& l, const HandPose& r, AvatarDesc& d) {   // facing +Z, as tools/avatar_pose.cpp
            d = avatar::preset(0);
            d.body = (uint8_t)body;
            const Quat face = Quat::axisAngle({0, 1, 0}, kPi);
            PlayerInput in;
            in.headTracked = true;
            in.head = {face, {0, 1.62f, 0}};
            const HandPose hp[2] = {l, r};
            for (int h = 0; h < 2; ++h) {
                const Quat q = face * Quat::axisAngle({0, 1, 0}, deg2rad(hp[h].yaw)) * Quat::axisAngle({1, 0, 0}, deg2rad(hp[h].pitch));
                in.hand[h].tracked = true;
                in.hand[h].grip = {q, face.rotate({hp[h].p.x, 0, -hp[h].p.z}) + Vec3{0, hp[h].p.y, 0}};
            }
            in.avatar = d;
            return in;
        };
        avatar::RigTuning& T = avatar::rigTuning();
        const float allow = T.elbowAllow;
        auto solve = [&](const PlayerInput& in, const AvatarDesc& d, bool swing) {
            T.elbowAllow = swing ? allow : 1e9f;
            const avatar::Rig r = avatar::solveRig(in, d);
            T.elbowAllow = allow;
            return r;
        };
        struct Named { const char* name; HandPose l, r; bool deep = true; };
        const Named usual[] = {{"rest", {{-0.24f, 0.92f, 0.12f}, -60, 0}, {{0.24f, 0.92f, 0.12f}, -60, 0}},
                               {"ready", {{-0.21f, 1.08f, 0.30f}, -22, 8}, {{0.21f, 1.08f, 0.30f}, -22, -8}},
                               {"throw", {{-0.22f, 1.22f, 0.36f}, 10, 10}, {{0.30f, 1.58f, -0.12f}, 35, -10}}};
        // (the far reach turns the torso 40 degrees toward that hand: the other arm, still in front, would cross it)
        const Named inside[] = {{"the far reach", {{-0.21f, 1.08f, 0.30f}, -22, 8}, {{0.80f, 1.10f, 0.35f}, 0, -60}, false},
                                {"at the chest", {{-0.05f, 1.27f, 0.24f}, 40, 30}, {{0.05f, 1.27f, 0.24f}, 40, -30}},
                                {"at the belly", {{-0.06f, 1.05f, 0.20f}, 0, 30}, {{0.06f, 1.05f, 0.20f}, 0, -30}},
                                {"at the chin", {{-0.05f, 1.45f, 0.22f}, 60, 30}, {{0.05f, 1.45f, 0.22f}, 60, -30}}};
        bool kept = true, cleared = true, wereIn = true, lengths = true;
        for (int body = 0; body < 2; ++body) {
            for (const Named& p : usual) {   // (an arm already clear of the body keeps its elbow; one in it, out)
                AvatarDesc d;
                const PlayerInput in = make(body, p.l, p.r, d);
                const avatar::Rig a = solve(in, d, false), b = solve(in, d, true);
                for (int k = 0; k < 2; ++k) {
                    const float moved = length(a.upper[k].b - b.upper[k].b), before = avatar::armInBody(a, d, k), after = avatar::armInBody(b, d, k);
                    if (moved > 0.001f)
                        std::printf("       %s, body %d: %.0f mm in, the elbow moved %.0f mm, then %.0f mm in\n", p.name, body, before * 1000, moved * 1000, after * 1000);
                    if ((before <= allow && moved > 0.001f) || after > 0.020f) kept = false;
                }
            }
            for (const Named& p : inside) {
                AvatarDesc d;
                const PlayerInput in = make(body, p.l, p.r, d);
                const avatar::Rig a = solve(in, d, false), b = solve(in, d, true);
                for (int k = 0; k < 2; ++k) {
                    const float before = avatar::armInBody(a, d, k), after = avatar::armInBody(b, d, k);
                    if (p.deep) wereIn &= before > 0.04f;
                    if (after > 0.030f) { cleared = false; std::printf("       %s, body %d: %.0f mm in (%.0f before)\n", p.name, body, after * 1000, before * 1000); }
                    lengths &= std::fabs(length(b.upper[k].b - b.upper[k].a) - length(a.upper[k].b - a.upper[k].a)) < 1e-3f &&
                               std::fabs(length(b.fore[k].b - b.upper[k].b) - length(a.fore[k].b - a.upper[k].b)) < 1e-3f;
                }
            }
        }
        // (the PR #25 masculine body is broad, its arms thick: standing and ready, its arms were in it)
        CHECK(kept, "standing, ready and throwing, an elbow stays where it was unless its arm was in the body, then out of it");
        CHECK(wereIn, "(test setup: hands at the chest, the belly and the chin put the arms 4 cm or more into the body)");
        // (within 1 cm, but for the hands at the belly: the straight forearm across its curve, the PR #25 masculine body's
        // 2.6 cm in; issue #24's far reach, past what an avatar's arm reaches even sized: the torso turned 40 degrees, the
        // other arm in front of it)
        CHECK(cleared, "there, the elbows swing out until the arms clear the body (to within 3 cm, from 7 to 14 cm in)");
        CHECK(lengths, "the upper arm and forearm keep their lengths");
        // the hands brought in from ready to the chest and down to the belly, 3 mm a step: the elbows never jump
        float jump = 0;
        for (int body = 0; body < 2; ++body) {
            Vec3 prev[2];
            const int steps = 120;
            for (int i = 0; i <= steps; ++i) {
                const float t = float(i) / steps;
                const Vec3 a{0.21f, 1.08f, 0.30f}, b{0.05f, 1.27f, 0.24f}, c{0.06f, 1.05f, 0.20f};
                const Vec3 p = t < 0.5f ? lerp(a, b, t * 2) : lerp(b, c, t * 2 - 1);
                const float pitch = t < 0.5f ? -22 + 62 * t * 2 : 40 - 40 * (t * 2 - 1), yaw = t < 0.5f ? -8 - 22 * t * 2 : -30;
                AvatarDesc d;
                const PlayerInput in = make(body, {{-p.x, p.y, p.z}, pitch, -yaw}, {p, pitch, yaw}, d);
                const avatar::Rig r = solve(in, d, true);
                for (int k = 0; k < 2; ++k) {
                    if (i) jump = std::max(jump, length(r.upper[k].b - prev[k]));
                    prev[k] = r.upper[k].b;
                }
            }
        }
        std::printf("       (hands brought in to the chest and the belly, 3 mm a step: an elbow moved at most %.0f mm a step)\n", jump * 1000);
        CHECK(jump < 0.03f, "brought in a few millimetres at a time, the elbows never jump");
    }
    {   // THE REACH (astrobleem's PR #25 and issue #24; ARM SIZING, the owner's go 2026-11-17)
        std::printf("[the reach]\n");
        struct HandPose { Vec3 p; float pitch, yaw; };
        auto make = [](int body, const HandPose& l, const HandPose& r, AvatarDesc& d) {   // facing +Z, as tools/avatar_pose.cpp
            d = avatar::preset(0);
            d.body = (uint8_t)body;
            const Quat face = Quat::axisAngle({0, 1, 0}, kPi);
            PlayerInput in;
            in.headTracked = true;
            in.head = {face, {0, 1.62f, 0}};
            const HandPose hp[2] = {l, r};
            for (int h = 0; h < 2; ++h) {
                const Quat q = face * Quat::axisAngle({0, 1, 0}, deg2rad(hp[h].yaw)) * Quat::axisAngle({1, 0, 0}, deg2rad(hp[h].pitch));
                in.hand[h].tracked = true;
                in.hand[h].grip = {q, face.rotate({hp[h].p.x, 0, -hp[h].p.z}) + Vec3{0, hp[h].p.y, 0}};
            }
            in.avatar = d;
            return in;
        };
        auto xf = [](const Mat4& m, const Vec3& v) {
            return Vec3{m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z + m.m[12], m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z + m.m[13],
                        m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14]};
        };
        const HandPose ready[2] = {{{-0.21f, 1.08f, 0.30f}, -22, 8}, {{0.21f, 1.08f, 0.30f}, -22, -8}};
        const HandPose far = {{0.80f, 1.10f, 0.35f}, 0, -60};    // the reach fixture of issue #24
        const HandPose overhead = {{0.25f, 2.15f, 0.10f}, 80, 0};
        const avatar::RigTuning& T = avatar::rigTuning();
        bool clavFollows = true, neverStretched = true, leans = true;
        for (int body = 0; body < 2; ++body)
            for (const HandPose& rp : {far, overhead, ready[1]}) {
                AvatarDesc d;
                const PlayerInput in = make(body, ready[0], rp, d);
                const avatar::Rig r = avatar::solveRig(in, d);
                const avatar::BodyBind& K = avatar::bodyBind(body);
                for (int k = 0; k < 2; ++k) {
                    // #24: the collarbone's skin carries the shoulder the arm hangs from (it slid away from it)
                    if (length(xf(r.skin[avatar::BONE_CLAV_L + k], K.shoulder[k]) - r.upper[k].a) > 0.001f + length(T.shoulder)) clavFollows = false;
                    const float L1 = length(K.elbow[k] - K.shoulder[k]) * T.upperLen, L2 = length(K.wrist[k] - K.elbow[k]) * T.foreLen;
                    if (length(r.upper[k].b - r.upper[k].a) > L1 * 1.0005f || length(r.fore[k].b - r.fore[k].a) > L2 * 1.0005f) neverStretched = false;
                }
                if (&rp == &far || rp.p.x == far.p.x) {
                    // the torso turned and leant toward the far hand, so less is left over than the shoulder alone leaves
                    const Vec3 up = r.torso.q.rotate({0, 1, 0}), front = r.torso.q.rotate({0, 0, -1});
                    const float turned = rad2deg(std::acos(clampf(dot(front, Vec3{0, 0, 1}), -1.f, 1.f)));
                    const float leant = rad2deg(std::acos(clampf(up.y, -1.f, 1.f)));
                    std::printf("       (body %d, the far reach: the torso turned %.0f deg, leant %.0f; the forearm %.0f mm short)\n", body, turned, leant, r.reachGap[1] * 1000);
                    // ... bending at the waist: the hips stay upright under the leant chest
                    const Vec3 hipsUp = normalize(Vec3{r.pelvis.m[4], r.pelvis.m[5], r.pelvis.m[6]});
                    std::printf("       (body %d: the hips lean %.1f deg)\n", body, rad2deg(std::acos(clampf(hipsUp.y, -1.f, 1.f))));
                    leans &= turned + leant > 5.f && leant > 5.f && r.bent && hipsUp.y > 0.9995f;
                }
            }
        CHECK(clavFollows, "the collarbone's skin moves with the shoulder (issue #24: it stayed while the shoulder slid)");
        CHECK(neverStretched, "no arm is ever longer than its bones, however far the hand");
        CHECK(leans, "a hand out of reach turns and leans the chest toward it, bending at the waist: the hips stay upright");
        // a state stepped at 90 frames a second: the torso never turns faster than 120 degrees a second
        {
            AvatarDesc d;
            avatar::RigState st;
            double t = 0;
            float fastest = 0, gapEnd = 1;
            Quat prev;
            for (int i = 0; i < 180; ++i, t += 1.0 / 90) {
                const PlayerInput in = make(0, ready[0], i < 30 ? ready[1] : far, d);
                avatar::stepRig(st, in, d, t);
                const avatar::Rig r = avatar::solveRig(in, d, &st);
                if (i) {
                    const float c = std::fabs(prev.x * r.torso.q.x + prev.y * r.torso.q.y + prev.z * r.torso.q.z + prev.w * r.torso.q.w);
                    fastest = std::max(fastest, rad2deg(2.f * std::acos(clampf(c, 0.f, 1.f))) * 90.f);
                }
                prev = r.torso.q;
                gapEnd = r.reachGap[1];
            }
            AvatarDesc d2;
            const avatar::Rig target = avatar::solveRig(make(0, ready[0], far, d2), d2);
            std::printf("       (a hand thrown out to the far reach: the torso at most %.0f deg/s; after 1.5 s %.0f mm short, the moment's posture %.0f)\n",
                        fastest, gapEnd * 1000, target.reachGap[1] * 1000);
            CHECK(fastest <= 121.f, "the torso follows smoothly, never faster than 120 degrees a second");
        }
        // ARM SIZING: reaching again and again a little past the arm closes the gap; within reach, the arms stay
        {
            // (the right elbow's bend at the reach: 0 a straight arm; a player's video, 2026-11-17: arms fully extended in
            // life, the avatar's elbows bent)
            float lastBend = 0;
            auto bendOf = [](const avatar::Rig& r) {
                const Vec3 a = normalize(r.upper[1].a - r.upper[1].b), b = normalize(r.fore[1].b - r.fore[1].a);
                return 180.f - rad2deg(std::acos(clampf(dot(a, b), -1.f, 1.f)));
            };
            auto play = [&](int body, const HandPose& out, int glitchAt, float& gapStart, float& gapEnd) {
                AvatarDesc d;
                avatar::RigState st;
                double t = 0;
                gapStart = -1;
                for (int i = 0; i < 90 * 40; ++i, t += 1.0 / 90) {   // 40 s: a punch out and back every second
                    const bool outNow = (i % 90) >= 40 && (i % 90) < 60;
                    PlayerInput in = make(body, ready[0], outNow ? out : ready[1], d);
                    if (i == glitchAt) in.hand[1].grip.p = in.hand[1].grip.p + Vec3{2.f, 0, 0};   // (tracking: one frame 2 m off)
                    avatar::stepRig(st, in, d, t);
                    const avatar::Rig r = avatar::solveRig(in, d, &st);
                    if (outNow && (i % 90) == 59) { if (gapStart < 0) gapStart = r.reachGap[1]; gapEnd = r.reachGap[1]; lastBend = bendOf(r); }
                }
                return st.armScale;
            };
            // a long-armed player's full reaches, past each body's arm and what its shoulder's swing covers (about 1.12
            // and 1.16 the avatar's arms: real arms are 5 to 20% longer than these avatars')
            const HandPose pastM = {{0.50f, 1.30f, 0.60f}, 0, -40}, pastF = {{0.45f, 1.30f, 0.50f}, 0, -40};
            float g0, g1, f0, f1, w0, w1, s0, s1;
            const float mScale = play(0, pastM, -1, g0, g1);
            const float mBend = lastBend;
            const float fScale = play(1, pastF, -1, f0, f1);
            const float fBend = lastBend;
            const float glitchScale = play(0, ready[1], 900, s0, s1);
            const float farScale = play(1, far, -1, w0, w1);
            std::printf("       (reaching past the arm, 40 punches: masculine x%.2f, short %.0f mm at first, %.0f at the end; feminine x%.2f, %.0f -> %.0f mm)\n",
                        mScale, g0 * 1000, g1 * 1000, fScale, f0 * 1000, f1 * 1000);
            std::printf("       (the far reach, feminine: x%.2f, %.0f -> %.0f mm short; within reach with a 2 m glitch: x%.3f)\n", farScale, w0 * 1000,
                        w1 * 1000, glitchScale);
            CHECK(g1 < 0.005f && f1 < 0.005f, "ARM SIZING: reaching past the arm again and again, the arms lengthen until the forearm meets the gauntlet (both bodies)");
            std::printf("       (the elbow at those full reaches: masculine bent %.0f deg, feminine %.0f)\n", mBend, fBend);
            CHECK(mBend < 8.f && fBend < 8.f, "ARM SIZING: a full reach is a straight arm, not bent at the elbow (both bodies)");
            // (the PR #25 bodies' arms are shorter than the MPFB bodies': the masculine 49 cm, x1.27 for this reach)
            CHECK(mScale > 1.05f && mScale < 1.3f && fScale > 1.05f && fScale < 1.3f, "ARM SIZING: to the player's reach, not to the limit");
            CHECK(farScale <= 1.35f, "ARM SIZING: never more than 35% longer");
            CHECK(glitchScale <= 1.f && glitchScale >= 0.8f, "ARM SIZING: within reach the arms never lengthen (a tracking jump is no reach), at most 20% shorter");
            // the owner's mirror (2026-11-17): arms that grew too long (or bodies whose arms are longer than the player's)
            // shorten to the reach the hands show, so a full reach is a straight arm; and a hand across the body is no reach
            auto play2 = [&](int body, float startScale, const HandPose& out, float seconds, float& gapEnd) {
                AvatarDesc d;
                avatar::RigState st;
                double t = 0;
                bool first = true;
                for (int i = 0; i < (int)(90 * seconds); ++i, t += 1.0 / 90) {   // a reach out and back every second
                    const bool outNow = (i % 90) >= 40 && (i % 90) < 60;
                    PlayerInput in = make(body, ready[0], outNow ? out : ready[1], d);
                    avatar::stepRig(st, in, d, t);
                    if (first) { st.armScale = startScale; st.reachPeak = startScale; first = false; }
                    const avatar::Rig r = avatar::solveRig(in, d, &st);
                    if (outNow && (i % 90) == 59) gapEnd = r.reachGap[1];
                }
                const float scale = st.armScale;
                for (int i = 0; i < 90; ++i, t += 1.0 / 90) {   // (then the reach held a second, as in a mirror: the elbow)
                    const PlayerInput in = make(body, ready[0], out, d);
                    avatar::stepRig(st, in, d, t);
                    lastBend = bendOf(avatar::solveRig(in, d, &st));
                }
                return scale;
            };
            const HandPose nearM = {{0.30f, 1.30f, 0.40f}, 0, -30}, across = {{-0.45f, 1.25f, 0.45f}, 0, 40};
            float n1 = 0, n2 = 0, a1 = 0, u1 = 0, u2 = 0;
            const float shortM = play2(0, 1.25f, nearM, 90.f, n1);
            const float nBendM = lastBend;
            const float shortF = play2(1, 1.25f, nearM, 90.f, n2);
            const float nBendF = lastBend;
            const float acrossScale = play2(0, 1.f, across, 40.f, a1);
            // both arms straight up, as in the player's video: a hand 60 cm over the shoulder, again and again
            const HandPose up = {{0.24f, 2.05f, 0.04f}, 80, 0};
            play2(0, 1.f, up, 40.f, u1);
            const float upBendM = lastBend;
            play2(1, 1.f, up, 40.f, u2);
            const float upBendF = lastBend;
            std::printf("       (arms at x1.25, a shorter full reach for 90 s: masculine x%.2f, feminine x%.2f, short %.0f / %.0f mm at the reach, "
                        "the elbow bent %.0f / %.0f deg; reaching across the body for 40 s: x%.2f; straight up: bent %.0f / %.0f deg, %.0f / %.0f mm short)\n",
                        shortM, shortF, n1 * 1000, n2 * 1000, nBendM, nBendF, acrossScale, upBendM, upBendF, u1 * 1000, u2 * 1000);
            CHECK(shortM < 1.15f && shortF < 1.15f && shortM >= 0.8f && shortF >= 0.8f && n1 < 0.005f && n2 < 0.005f,
                  "ARM SIZING: arms longer than the reach shown shorten toward it (a full reach is a straight arm; it still meets the gauntlet)");
            CHECK(nBendM < 8.f && nBendF < 8.f && upBendM < 8.f && upBendF < 8.f,
                  "ARM SIZING: arms that shortened to the reach are straight at it; both arms straight up are straight (the elbows not bent)");
            CHECK(acrossScale <= 1.f, "ARM SIZING: a hand reaching across the body never lengthens the arms");
        }
        // FOREARM TWIST (PR #25): the forearm bone turns with the hand; the shader turns its skin back toward the elbow.
        // Here the shader's sum for one forearm influence (render.cpp), on rings round the forearm near each end
        std::printf("[forearm twist]\n");
        auto wrapAngle = [](float a) { while (a > kPi) a -= 2 * kPi; while (a < -kPi) a += 2 * kPi; return a; };
        auto twisted = [&](const avatar::Rig& r, int k, const Vec3& bp, bool on) {
            const float* t = r.twist;
            auto v4 = [&](int i) { return Vec3{t[i * 4], t[i * 4 + 1], t[i * 4 + 2]}; };
            const Vec3 p = xf(r.skin[avatar::BONE_FORE_L + k], bp);
            if (!on) return p;
            const float s = clampf(dot(bp - v4(k), v4(2 + k)) * t[k * 4 + 3], 0.f, 1.f);
            const float th = -t[(2 + k) * 4 + 3] * (1.f - s * s * (3.f - 2.f * s));
            const Vec3 E = v4(4 + k);
            return E + Quat::axisAngle(v4(6 + k), th).rotate(p - E);
        };
        // the right hand held out in front at chest height, turned about its own forward axis by `roll` degrees
        auto held = [&](int body, float roll, AvatarDesc& d) {
            PlayerInput in = make(body, ready[0], {{0.21f, 1.22f, 0.42f}, 0, 0}, d);
            in.hand[1].grip.q = in.hand[1].grip.q * Quat::axisAngle({0, 0, 1}, deg2rad(roll));
            return in;
        };
        // a ring's turn about the forearm, against the shoulder seen from the elbow
        auto ringTurn = [&](const avatar::Rig& r, int body, float at, bool on) {
            const avatar::BodyBind& K = avatar::bodyBind(body);
            const Vec3 a = K.elbow[1], b = K.wrist[1], ax0 = normalize(b - a);
            const Vec3 side = normalize(cross(ax0, Vec3{0, 1, 0}));
            const Vec3 bp = a + (b - a) * at + side * 0.04f;
            const Vec3 E = r.fore[1].a, ax = normalize(r.fore[1].b - r.fore[1].a);
            Vec3 u = twisted(r, 1, bp, on) - E, ref = r.upper[1].a - E;
            u = u - ax * dot(u, ax);
            ref = ref - ax * dot(ref, ax);
            return std::atan2(dot(cross(ref, u), ax), dot(ref, u));
        };
        bool restNone = true, follows = true, elbowStays = true, wristTurns = true, smooth = true;
        float worstElbow = 0, worstJump = 0, worstWrist = 0;
        for (int body = 0; body < 2; ++body) {
            AvatarDesc d;
            PlayerInput rest = make(body, ready[0], ready[1], d);
            rest.hand[0].tracked = rest.hand[1].tracked = false;
            const avatar::Rig r0 = avatar::solveRig(rest, d);
            if (r0.twisted || std::fabs(r0.roll[0]) > 1e-4f || std::fabs(r0.roll[1]) > 1e-4f) restNone = false;
            for (float base : {-60.f, 0.f, 40.f}) {
                const avatar::Rig ra = avatar::solveRig(held(body, base, d), d), rb = avatar::solveRig(held(body, base + 50.f, d), d);
                const float dRoll = rad2deg(wrapAngle(rb.roll[1] - ra.roll[1]));
                // the roll is the hand's turn about the forearm (the grip's z points back along it: its +50 is the forearm's -50)
                if (dRoll > -40.f || dRoll < -60.f) follows = false;
                // near the elbow (4% of the way) the skin keeps its turn whatever the hand does; untwisted it turned with it
                const float tOn = rad2deg(wrapAngle(ringTurn(rb, body, 0.04f, true) - ringTurn(ra, body, 0.04f, true)));
                const float tOff = rad2deg(wrapAngle(ringTurn(rb, body, 0.04f, false) - ringTurn(ra, body, 0.04f, false)));
                worstElbow = std::max(worstElbow, std::fabs(tOn));
                if (std::fabs(tOn) > 6.f || std::fabs(tOff) < 40.f) elbowStays = false;
                // at the wrist the skin turns with the hand, as the bone does
                const float wOn = rad2deg(wrapAngle(ringTurn(rb, body, 1.f, true) - ringTurn(ra, body, 1.f, true)));
                if (std::fabs(wOn - dRoll) > 6.f) wristTurns = false;
            }
            // the hand turned all the way round, 2 degrees a step: what the shader undoes follows the turn within a wrist's
            // reach (120 degrees either way from thumbs up), and never jumps (past 135 it fades out: at 180 the turn's side
            // is a guess)
            float last = 0;
            for (int i = 0; i <= 360; i += 2) {
                const avatar::Rig r = avatar::solveRig(held(body, -180.f + (float)i, d), d);
                const float undo = rad2deg(r.twist[(2 + 1) * 4 + 3]);
                if (i > 0) worstJump = std::max(worstJump, std::fabs(undo - last));
                if (i > 0 && std::fabs(-180.f + (float)i) <= 120.f) worstWrist = std::max(worstWrist, std::fabs(undo - last));
                last = undo;
            }
        }
        smooth = worstJump < 15.f && worstWrist < 3.f;
        std::printf("       (the skin near the elbow turned at most %.1f deg with the hand turned 50; the undone turn moves at most %.1f deg a 2 deg step within a wrist's turn, %.1f past it)\n",
                    worstElbow, worstWrist, worstJump);
        CHECK(restNone, "FOREARM TWIST: arms at rest are untwisted");
        CHECK(follows, "FOREARM TWIST: the roll follows the hand's turn about the forearm");
        CHECK(elbowStays, "FOREARM TWIST: near the elbow the skin keeps its turn when the hand turns (untwisted, it turned with the hand)");
        CHECK(wristTurns, "FOREARM TWIST: at the wrist the skin turns with the hand");
        CHECK(smooth, "FOREARM TWIST: turning the hand all the way round, the twist never jumps");
        // TENNIS SKIRT and KILT (PR #25, his swish): the swing, stepped at 90 frames a second
        std::printf("[skirts]\n");
        {
            AvatarDesc d = avatar::preset(0);
            d.lower = 5;
            bool kinds = avatar::swishKind(d) == 0;
            d.lower = 6;
            kinds &= avatar::swishKind(d) == 1;
            d.lower = 4;
            kinds &= avatar::swishKind(d) == -1;
            CHECK(kinds, "SKIRTS: the tennis skirt and the kilt sway, nothing else does");
            const float maxOff[2] = {0.065f, 0.05f}, maxTw[2] = {0.23f, 0.17f};
            bool trails = true, bounded = true, settles = true, lags = true, fresh = true;
            float most = 0, after = 1;
            for (int kind = 0; kind < 2; ++kind) {
                avatar::Swish s;
                double t = 0;
                float x = 0, yaw = 0;
                avatar::stepSwish(s, kind, x, 0, yaw, t);
                // a sidestep: 1 m/s along +x for half a second, facing -z (yaw 0): the hem trails toward -x
                for (int i = 0; i < 45; ++i) {
                    t += 1.0 / 90; x += 1.f / 90;
                    avatar::stepSwish(s, kind, x, 0, yaw, t);
                    if (std::hypot(s.x, s.z) > maxOff[kind] + 1e-5f || !std::isfinite(s.x)) bounded = false;
                    most = std::max(most, std::hypot(s.x, s.z));
                }
                if (s.x > -0.01f) trails = false;
                // stopped: back to rest within 1.5 s
                for (int i = 0; i < 135; ++i) { t += 1.0 / 90; avatar::stepSwish(s, kind, x, 0, yaw, t); }
                after = std::max(after == 1 ? 0.f : after, std::hypot(s.x, s.z));
                if (std::hypot(s.x, s.z) > 0.002f) settles = false;
                // a quick turn to the left (yaw growing): the skirt lags, turned the other way, never past its limit
                float least = 0;
                for (int i = 0; i < 30; ++i) {
                    t += 1.0 / 90; yaw += 3.f / 90;
                    avatar::stepSwish(s, kind, x, 0, yaw, t);
                    least = std::min(least, s.twist);
                    if (std::fabs(s.twist) > maxTw[kind] + 1e-5f) bounded = false;
                }
                if (least > -0.05f) lags = false;
                // a jump (a teleport, a respawn): afresh, at rest
                t += 1.0 / 90;
                avatar::stepSwish(s, kind, x + 2.f, 0, yaw, t);
                if (s.x != 0 || s.z != 0 || s.twist != 0) fresh = false;
            }
            std::printf("       (a sidestep swings the hem out %.0f mm at most; stopped, %.1f mm left after 1.5 s)\n", most * 1000, after * 1000);
            CHECK(trails, "SKIRTS: stepping sideways, the hem trails the way it came");
            CHECK(bounded, "SKIRTS: never further than the skirt's limits (out, twist)");
            CHECK(settles, "SKIRTS: stopped, the skirt settles back to rest");
            CHECK(lags, "SKIRTS: turning, the skirt lags the turn");
            CHECK(fresh, "SKIRTS: after a jump, at rest again");
        }
    }
    {   // HANDS APART (owner, 2026-11-16, her video: with the controllers held together the gauntlets crossed): the
        // shapes of the real gauntlet models, and the two drawn hands moved apart until they only touch
        std::printf("[hands apart]\n");
        std::string dir = col;
        dir = dir.substr(0, dir.find_last_of("/\\") + 1);
        auto readFile = [](const std::string& path) {
            std::vector<uint8_t> out;
            if (FILE* f = std::fopen(path.c_str(), "rb")) {
                uint8_t buf[4096];
                for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) out.insert(out.end(), buf, buf + n);
                std::fclose(f);
            }
            return out;
        };
        const Mat4 lancer = avatar::ueLocalToGrip({1, -0.5f, 0}, {5, 0, 0}, {1, 1, 1});
        avatar::HandShape fist, cuff;
        const std::vector<uint8_t> fm = readFile(dir + "gauntlet_0_hand.mdl"), cm = readFile(dir + "gauntlet_0_cuff.mdl");
        const auto t0 = std::chrono::steady_clock::now();
        const bool built = !fm.empty() && !cm.empty() && avatar::handShapeFromModel(fist, fm.data(), fm.size(), lancer, 5) &&
                           avatar::handShapeFromModel(cuff, cm.data(), cm.size(), lancer, 4);
        const double buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("       (the STRIKER fist and cuff: %zu and %zu points, built in %.0f ms)\n", fist.pts.size(), cuff.pts.size(), buildMs);
        CHECK(built, "the gauntlet's fist and cuff become shapes");
        if (built) {   // KNUCKLES ON THE FIST (two players' report, 2026-11-17): the bumper's front face at the drawn knuckles
            const Pose fp = avatar::fistPose();
            float front = 1e9f;
            for (const Vec3& p : fist.pts) front = std::min(front, fp.transform(p).z);
            const Tunables tn;
            const float boxFront = tn.knuckleOffset.z - tn.knuckleHalf.z, oldFront = tn.knuckleOffsetOld.z - tn.knuckleHalf.z;
            std::printf("       (the drawn fist's front %.1f cm out from the grip; the knuckle bumper's front %.1f cm, before %.1f)\n",
                        -front * 100, -boxFront * 100, -oldFront * 100);
            CHECK(std::fabs(boxFront - front) < 0.005f, "KNUCKLES ON THE FIST: the knuckle bumper's front face is at the drawn knuckles (within 5 mm)");
        }
        if (built) {
            std::vector<float> on;
            for (const Vec3& p : fist.pts) on.push_back(std::fabs(fist.dist(p)));
            std::sort(on.begin(), on.end());
            std::printf("       (its surface points from its surface: half within %.1f mm, 90%% within %.1f; 30 cm off: %.0f mm out)\n",
                        on[on.size() / 2] * 1000, on[on.size() * 9 / 10] * 1000, fist.dist(fist.c + Vec3{0.3f, 0, 0}) * 1000);
            CHECK(on[on.size() / 2] < 0.002f && fist.dist(fist.c + Vec3{0.3f, 0, 0}) > 0.2f, "its surface points are on its surface; far away is outside");
            // the drawn parts of both hands, as main.cpp handsApart places them (the left mirrored)
            auto parts = [&](const Pose grip[2], avatar::HandPart P[2][2]) {
                for (int h = 0; h < 2; ++h) {
                    Mat4 mirror = Mat4::identity();
                    if (h == 0) mirror.m[0] = -1;
                    const Mat4 gm = Mat4::fromPose(grip[h]) * mirror;
                    P[h][0].shape = &fist; P[h][0].place = gm * Mat4::fromPose(avatar::fistPose());
                    P[h][1].shape = &cuff; P[h][1].place = gm * Mat4::fromPose(avatar::cuffPose(false));
                    for (avatar::HandPart& p : P[h]) p.inv = avatar::affineInverse(p.place);
                }
            };
            // the picture tool's poses (main.cpp --handstouch): 0 the tops together, 1 the palms, 2 the knuckles, 3 crossed
            auto pose = [](int k, float gap, Pose grip[2]) {
                Quat qr, ql;
                Vec3 dr{gap, 0, 0}, dl{-gap, 0, 0};
                if (k == 0) { qr = Quat::axisAngle({0, 0, 1}, kPi * 0.25f); ql = Quat::axisAngle({0, 0, 1}, -kPi * 0.25f); }
                if (k == 2) { qr = Quat::axisAngle({0, 1, 0}, kPi * 0.5f); ql = Quat::axisAngle({0, 1, 0}, -kPi * 0.5f); }
                if (k == 3) { qr = Quat::axisAngle({0, 1, 0}, kPi * 0.20f); ql = Quat::axisAngle({0, 1, 0}, -kPi * 0.20f); dr = {-gap, 0.035f, 0}; dl = {gap, -0.005f, 0}; }
                grip[1] = {qr, Vec3{0, 1.2f, -0.4f} + dr};
                grip[0] = {ql, Vec3{0, 1.2f, -0.4f} + dl};
            };
            bool cleared = true, small = true, sideways = true, overlapped = false;
            double worstMs = 0;
            for (int k = 0; k < 4; ++k)
                for (float gap = 0.015f; gap < 0.071f; gap += 0.005f) {
                    Pose grip[2];
                    pose(k, gap, grip);
                    avatar::HandPart P[2][2];
                    parts(grip, P);
                    const float before = avatar::handsApartDepth(P[0], 2, P[1], 2, {0, 0, 0});
                    const auto s0 = std::chrono::steady_clock::now();
                    const Vec3 S = avatar::handsApartShift(P[0], 2, P[1], 2, 0.06f);
                    worstMs = std::max(worstMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count());
                    const float after = avatar::handsApartDepth(P[0], 2, P[1], 2, S);
                    overlapped |= before > 0.01f;
                    if (length(S) < 0.0595f && after > 0.0025f) {
                        cleared = false;
                        std::printf("       pose %d, %.0f mm out: %.1f mm deep after %.1f mm apart (%.1f before)\n", k, gap * 1000, after * 1000, length(S) * 1000, before * 1000);
                    }
                    small &= length(S) <= 0.0601f && (before > 0 || length(S) == 0);
                    if (k < 3 && length(S) > 0.004f && std::fabs(S.x) < 0.7f * length(S)) {
                        sideways = false;
                        std::printf("       pose %d, %.0f mm out: moved (%.1f %.1f %.1f) mm\n", k, gap * 1000, S.x * 1000, S.y * 1000, S.z * 1000);
                    }
                }
            std::printf("       (the slowest: %.2f ms)\n", worstMs);
            CHECK(overlapped, "(test setup: the poses' gauntlets cross)");
            CHECK(cleared, "moved apart, the gauntlets no longer cross (under 2.5 mm), unless the 3 cm each way ran out");
            CHECK(small, "they move only when they cross, and at most 3 cm each");
            CHECK(sideways, "side by side, they move apart sideways (not forward or up)");
            {   // drawing them 30 cm apart: nothing; sliding them together: no jumps
                Pose grip[2];
                pose(1, 0.15f, grip);
                avatar::HandPart P[2][2];
                parts(grip, P);
                CHECK(length(avatar::handsApartShift(P[0], 2, P[1], 2, 0.06f)) == 0.f, "hands apart are left where they are");
                float worst = 0;
                for (int k : {0, 1, 3}) {   // the tops, the palms, the wrists crossed
                    float jump = 0;
                    Vec3 prev{0, 0, 0};
                    for (float gap = 0.07f; gap > 0.025f; gap -= 0.001f) {
                        pose(k, gap, grip);
                        parts(grip, P);
                        const Vec3 S = avatar::handsApartShift(P[0], 2, P[1], 2, 0.06f);
                        if (gap < 0.0695f) jump = std::max(jump, length(S - prev));
                        prev = S;
                    }
                    std::printf("       (pose %d slid together 1 mm at a time: the biggest step %.1f mm)\n", k, jump * 1000);
                    worst = std::max(worst, jump);
                }
                CHECK(worst < 0.006f, "slid together a millimetre at a time, the drawn hands never jump");
            }
        }
    }
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
