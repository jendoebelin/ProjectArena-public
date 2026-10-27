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
        CHECK(hairParts(0, 1).empty(), "no hair: nothing drawn under the helmet");
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
        in[1].hand[1].trigger = 1;   // one player holds the trigger: enough
        std::vector<GameEvent> log;
        run(g, in, 0.05f, &log);
        CHECK(hasEvent(log, EventType::FistBump) && g.match == MatchState::Meet, "fist bump (one trigger held): confirmed first (buzz, sound)");
        log.clear();
        run(g, in, g.tun.bumpPause + 0.05f, &log);
        int bumps = 0;
        for (const GameEvent& e : log) bumps += e.type == EventType::FistBump;
        CHECK(g.match == MatchState::Countdown && bumps == 0, "... then the countdown, once (holding the fists together does not repeat it)");
        bool named = true;
        for (int t = 0; t <= (int)EventType::FistBump; ++t) named &= std::strcmp(eventName((EventType)t), "?") != 0;
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
        {   // real matches: two lively bots for ten minutes; every event behind a point names its ball
            Game g; g.setArena(tris); g.reset(true);
            g.pl[0].isBot = true;
            g.tun.botBlockChance = 0.5f; g.tun.botHoldMin = 0.4f; g.tun.botHoldMax = 1.0f; g.tun.botAimError = 0.1f;
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
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
