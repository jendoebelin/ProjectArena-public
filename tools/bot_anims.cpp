// The asset viewer's ANIMATIONS (0.3.26; the owner, 2026-11-24: "are the idle animations, taunts, and victory poses
// available in the asset viewer, if not, please add them. Also add the swish animations to the asset viewer"): the
// practice bot's own motion (Game::stepBot: at ease, its taunts, its celebrations, its victory poses) played by the
// game, its head and hands taken each step into an avatar's frame (at the origin, facing +Z, as tools/avatar_pose.cpp
// stands it), and the game's rig stepped through them for each body and build (avatar.cpp stepRig, solveRig,
// drawList), with the TENNIS SKIRT's and the KILT's swing (stepSwish) as the rig's hips carry them. Two more for the
// swing on its own: side to side, and turning.
// Build: tools/build_bot_anims.bat   Usage: bot_anims <arena.col> <out dir>   writes <out>/anims_<body><build>.json
// (one file a body and build, every animation; the viewer fetches the one it needs) and <out>/anims_index.json
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "../app/src/avatar.h"
#include "../app/src/game.h"

namespace {
constexpr int kHz = 72, kEvery = 6;   // the game's step; a frame kept every 6 (12 a second; the viewer blends between)

struct Anim {
    const char* id;
    const char* group;
    const char* name;
    float lead, secs;                 // seconds played before the first frame kept (the rig and the motion settled), kept
    std::vector<PlayerInput> steps;   // the head and hands each step, in the avatar's frame
    std::vector<Mat4> prop;           // AIR GUITAR's guitar each step, in the avatar's frame (Game::botGuitar), and how
    std::vector<float> show;          // far it has come to its hands (0: none)
};
std::vector<Mat4> g_prop;    // (the last playBot's)
std::vector<float> g_show;

std::vector<CollisionTri> loadCol(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); std::exit(2); }
    std::vector<uint8_t> bytes;
    uint8_t buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
    return parseCollision(bytes.data(), bytes.size());
}

// you, standing at your end with your hands ready (the bot's opponent)
PlayerInput standing(const Game& g) {
    PlayerInput in;
    in.headTracked = true;
    const Quat face = Quat::axisAngle({0, 1, 0}, deg2rad(-90.f));   // (player 0 faces +X)
    in.head = {face, g.tun.start[0] + Vec3{0, 1.65f, 0}};
    for (int h = 0; h < 2; ++h) {
        in.hand[h].tracked = true;
        in.hand[h].grip = {face, g.tun.start[0] + Vec3{0.25f, 1.05f, h ? 0.28f : -0.28f}};
    }
    return in;
}

// the bot (player 1) as an avatar at the origin facing +Z: its place and facing taken out
PlayerInput toAvatar(const Game& g, const PlayerInput& b) {
    const Vec3 stand = g.tun.start[1];
    const Quat botQ = Quat::axisAngle({0, 1, 0}, deg2rad(90.f)), face = Quat::axisAngle({0, 1, 0}, kPi);
    const Quat T = face * botQ.conj();
    PlayerInput o = b;
    auto move = [&](Pose& x) { Vec3 d = x.p - stand; d.y = x.p.y; x = {T * x.q, T.rotate({d.x, 0, d.z}) + Vec3{0, d.y, 0}}; };
    move(o.head);
    for (int h = 0; h < 2; ++h) move(o.hand[h].grip);
    o.headTracked = true;
    o.hand[0].tracked = o.hand[1].tracked = true;
    return o;
}

// the bot's motion: `setup` once a match is on, `each` before every step (the bot kept from throwing, its own ball
// away so both hands are free, whatever the scene needs)
template <class Setup, class Each>
std::vector<PlayerInput> playBot(const std::vector<CollisionTri>& tris, int stance, float seconds, Setup setup, Each each) {
    Game g;
    g.setArena(tris);
    g.tun.basicMatchTime = 0; g.tun.meetTime = 0; g.tun.botLevel = 0;
    g.reset(true);
    PlayerInput in[2] = {standing(g), {}};
    for (int i = 0; i < kHz * 20 && g.match != MatchState::InProgress; ++i) { PlayerInput t[2] = {in[0], in[1]}; g.step(1.f / kHz, t); }
    g.pl[1].botStance = stance;
    setup(g, in);
    std::vector<PlayerInput> out;
    g_prop.clear();
    g_show.clear();
    const Vec3 stand = g.tun.start[1];
    const Quat toAv = Quat::axisAngle({0, 1, 0}, kPi) * Quat::axisAngle({0, 1, 0}, deg2rad(90.f)).conj();   // (toAvatar's)
    for (int i = 0; i < (int)(seconds * kHz); ++i) {
        g.pl[1].botTimer = 99.f;
        Ball& own = g.pl[1].ball;
        own.state = BallState::Dead; own.heldHand = -1; own.timer = 0;
        each(g, in, i);
        PlayerInput t[2] = {in[0], in[1]};
        g.step(1.f / kHz, t);
        out.push_back(toAvatar(g, g.pl[1].in));
        Mat4 m = Mat4::identity();
        float show = 0;
        if (!g.botGuitar(1, g.pl[1].in, m, show)) show = 0;
        g_prop.push_back(Mat4::fromPose({toAv, toAv.rotate(Vec3{-stand.x, 0, -stand.z})}) * m);
        g_show.push_back(show);
    }
    return out;
}

// the swing on its own: the avatar moved by hand (its head and hands, ready, carried with it)
std::vector<PlayerInput> scripted(float seconds, bool turn) {
    std::vector<PlayerInput> out;
    const Quat face = Quat::axisAngle({0, 1, 0}, kPi);
    for (int i = 0; i < (int)(seconds * kHz); ++i) {
        const float t = i / (float)kHz;
        const float side = turn ? 0.f : 0.22f * std::sin(t * 2.f * kPi * 0.7f);
        const float yaw = turn ? 0.65f * std::sin(t * 2.f * kPi * 0.55f) : 0.f;
        const Quat q = face * Quat::axisAngle({0, 1, 0}, yaw);
        PlayerInput in;
        in.headTracked = true;
        in.head = {q, Vec3{side, 1.62f, 0}};
        const Vec3 hp[2] = {{-0.21f, 1.08f, 0.30f}, {0.21f, 1.08f, 0.30f}};
        for (int h = 0; h < 2; ++h) {
            const Vec3 l{hp[h].x, 0, -hp[h].z};   // (avatar frame: -Z forward)
            in.hand[h].tracked = true;
            in.hand[h].grip = {q * Quat::axisAngle({0, 1, 0}, deg2rad(h ? -8.f : 8.f)) * Quat::axisAngle({1, 0, 0}, deg2rad(-22.f)),
                               Vec3{side, 0, 0} + q.rotate(l) + Vec3{0, hp[h].y, 0}};
        }
        out.push_back(in);
    }
    return out;
}

void put34(std::string& s, const Mat4& m) {   // the matrix's top three rows (column-major, 12 numbers)
    char b[32];
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 3; ++r) {
            std::snprintf(b, sizeof b, "%s%.4f", s.back() == '[' ? "" : ",", m.m[c * 4 + r]);
            s += b;
        }
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: bot_anims <arena.col> <out dir>\n"); return 2; }
    const std::vector<CollisionTri> tris = loadCol(argv[1]);
    const std::string outDir = argv[2];
    std::vector<Anim> anims;
    auto none = [](Game&, PlayerInput*) {};
    // AT EASE
    anims.push_back({"idle", "THE BOT AT EASE", "Breathing, its hands drifting, its weight shifting, glancing about", 2.f, 6.f,
                     playBot(tris, STANCE_RELAXED, 8.f, none, [](Game&, PlayerInput*, int) {})});
    // TAUNTS: you hold your ball; the stall held where each gesture is (Game::stepBot: a beckon from 4 s, the others later)
    auto taunt = [&](const char* id, const char* name, int stance, float stall) {
        anims.push_back({id, "TAUNTS (YOU HOLD YOUR BALL)", name, 2.f, 3.f,
                         playBot(tris, stance, 5.f,
                                 [](Game& g, PlayerInput* in) {
                                     g.pl[0].ball.state = BallState::Held; g.pl[0].ball.heldHand = 1;
                                     in[0].hand[1].trigger = 1;
                                 },
                                 [stall](Game& g, PlayerInput*, int) { g.pl[1].botStall = stall; })});
    };
    taunt("taunt_beckon", "Come on: a beckon with both hands", STANCE_RELAXED, 5.f);
    taunt("taunt_wide", "Here I am: arms spread wide", STANCE_RELAXED, 14.f);
    taunt("taunt_shrug", "A shrug", STANCE_RELAXED, 16.5f);
    taunt("taunt_chest", "Pounding its chest", STANCE_RELAXED, 10.5f);
    taunt("taunt_gloves", "BOXER: its gloves tapped together", STANCE_BOXER, 10.5f);
    taunt("taunt_monkey", "MONKEY: bouncing, its arms shaken out", STANCE_MONKEY, 10.5f);
    // A POINT WON: each celebration from the moment it comes
    auto cheer = [&](const char* id, const char* name, int kind) {
        anims.push_back({id, "A POINT WON", name, 1.f, 1.4f,
                         playBot(tris, STANCE_RELAXED, 2.4f, none, [kind](Game& g, PlayerInput*, int i) {
                             if (i == kHz) g.pl[1].score += 1;               // (the point, a second in)
                             if (i == kHz + 1) g.pl[1].botCheerKind = kind;  // (this celebration)
                         })});
    };
    cheer("cheer_pump", "A fist pump", 0);
    cheer("cheer_double", "A double pump: both fists", 1);
    cheer("cheer_point", "A point at you", 2);
    cheer("cheer_clap", "A clap", 3);
    // THE MATCH WON
    auto win = [&](const char* id, const char* name, int stance, int kind, float secs = 3.f) {
        anims.push_back({id, "THE MATCH WON", name, 1.f, secs,
                         playBot(tris, stance, secs + 1.f,
                                 [kind](Game& g, PlayerInput*) {
                                     g.match = MatchState::MatchSummary; g.matchWinner = 1;
                                     g.pl[1].botWonSeen = true; g.pl[1].botWinKind = kind;
                                 },
                                 [](Game&, PlayerInput*, int) {})});
        anims.back().prop = g_prop;
        anims.back().show = g_show;
    };
    win("win_kitten", "KITTEN: a big wave", STANCE_KITTEN, 0);
    win("win_relaxed", "RELAXED: both arms raised", STANCE_RELAXED, 0);
    win("win_boxer", "BOXER: flexing", STANCE_BOXER, 0);
    win("win_karate", "KARATE: a bow, a fist in its palm", STANCE_KARATE, 0);
    win("win_mantis", "MANTIS: both arms raised", STANCE_MANTIS, 0);
    win("win_monkey", "MONKEY: jumping, its arms up", STANCE_MONKEY, 0);
    win("win_guitar", "AIR GUITAR: slow, big strokes", STANCE_RELAXED, 1, 5.4f);   // (a bar: two strokes and one from over its head)
    win("win_roof", "RAISE THE ROOF: pushed up from its head", STANCE_RELAXED, 2);
    // THE SKIRTS' SWING on its own
    anims.push_back({"swish_side", "THE SWISH (TENNIS SKIRT, KILT, DRESS)", "Side to side", 1.f, 4.3f, scripted(5.3f, false)});
    anims.push_back({"swish_turn", "THE SWISH (TENNIS SKIRT, KILT, DRESS)", "Turning", 1.f, 3.7f, scripted(4.7f, true)});

    {   // the index: each animation's name, group, frames a second and length
        std::string s = "{\"fps\":" + std::to_string(kHz / kEvery) + ",\"anims\":[";
        for (size_t k = 0; k < anims.size(); ++k) {
            char b[512];
            std::snprintf(b, sizeof b, "%s{\"id\":\"%s\",\"group\":\"%s\",\"name\":\"%s\",\"frames\":%d}", k ? "," : "", anims[k].id,
                          anims[k].group, anims[k].name, (int)(anims[k].secs * kHz) / kEvery);
            s += b;
        }
        s += "]}\n";
        FILE* f = std::fopen((outDir + "/anims_index.json").c_str(), "wb");
        if (!f) { std::fprintf(stderr, "cannot write %s\n", outDir.c_str()); return 2; }
        std::fputs(s.c_str(), f);
        std::fclose(f);
    }
    for (int body = 0; body < 2; ++body)
        for (int build = 0; build < 4; ++build) {
            AvatarDesc d;
            std::string t = "body=" + std::to_string(body) + "\nbuild=" + std::to_string(build) +
                            "\nsuit=0\nlower=0\nhair=0\nheadgear=0\nvisor=1\nmask=0\ngauntlet=0";
            avatar::fromText(t, d);
            std::string s = "{";
            for (size_t k = 0; k < anims.size(); ++k) {
                const Anim& A = anims[k];
                avatar::RigState st;
                avatar::Swish sw[2];
                // THE WRISTS (the owner, 2026-11-24: "check all the animations for correct wrist movements"): over the
                // frames kept, each hand's worst bend off its forearm (the cuff's line against the forearm's), the
                // forearm's worst turn against the upper arm, and how far the arm ends short of the hand
                float bend[2] = {0, 0}, bendSum[2] = {0, 0}, roll[2] = {0, 0}, gap[2] = {0, 0};
                int kept = 0;
                s += (k ? ",\"" : "\"") + std::string(A.id) + "\":[";
                const int first = (int)(A.lead * kHz), last = first + (int)(A.secs * kHz);
                bool any = false;
                for (int i = 0; i < (int)A.steps.size() && i < last; ++i) {
                    PlayerInput in = A.steps[i];
                    in.avatar = d;
                    const double now = i / (double)kHz;
                    avatar::stepRig(st, in, d, now);
                    const avatar::Rig r = avatar::solveRig(in, d, &st);
                    for (int kind = 0; kind < 2; ++kind) avatar::stepSwish(sw[kind], kind, r.torso.p.x, r.torso.p.z, st.torso.x, now);
                    if (i < first || (i - first) % kEvery) continue;
                    ++kept;
                    for (int h = 0; h < 2; ++h) {
                        const Vec3 fore = normalize(r.fore[h].b - r.fore[h].a);
                        const Vec3 line = normalize(Vec3{0, 0.047f, -1.f});   // (the hand's own line: solveRig's `back`)
                        const float a = rad2deg(std::acos(clampf(dot(fore, in.hand[h].grip.q.rotate(line)), -1.f, 1.f)));
                        bend[h] = std::max(bend[h], a); bendSum[h] += a;
                        if (std::getenv("PA_WRIST_DEBUG") && body == 0 && build == 1 && !std::strcmp(A.id, std::getenv("PA_WRIST_DEBUG")) && kept % 4 == 1) {
                            const Vec3 S = r.upper[h].a, E = r.fore[h].a, W = r.fore[h].b, G = in.hand[h].grip.p, L = in.hand[h].grip.q.rotate(line);
                            std::printf("  %s f%d h%d bend %3.0f head %.2f %.2f %.2f | S %.2f %.2f %.2f E %.2f %.2f %.2f W %.2f %.2f %.2f G %.2f %.2f %.2f line %.2f %.2f %.2f fore %.2f %.2f %.2f torso yaw %.0f\n",
                                        A.id, kept, h, a, in.head.p.x, in.head.p.y, in.head.p.z, S.x, S.y, S.z, E.x, E.y, E.z, W.x, W.y, W.z, G.x, G.y, G.z, L.x, L.y, L.z, fore.x, fore.y, fore.z, rad2deg(st.torso.x));
                        }
                        roll[h] = std::max(roll[h], std::fabs(rad2deg(r.roll[h])));
                        gap[h] = std::max(gap[h], r.reachGap[h] * 100.f);
                    }
                    std::vector<avatar::PartDraw> list;
                    avatar::drawList(r, in, d, list);
                    Mat4 head = Mat4::fromPose(in.head);
                    for (const avatar::PartDraw& p : list) if (!std::strncmp(p.model, "av_visor_", 9)) head = p.m;
                    s += any ? ",{\"s\":[" : "{\"s\":[";
                    any = true;
                    for (int b = 0; b < avatar::BONE_COUNT; ++b) put34(s, r.skin[b]);
                    s += "],\"h\":[";
                    put34(s, head);
                    s += "],\"g\":[";   // each hand: the grip, the fist, the cuff (the viewer adds its gauntlet frames)
                    for (int h = 0; h < 2; ++h) {
                        const Mat4 gm = Mat4::fromPose(in.hand[h].grip) * Mat4::fromPose(Pose{}, {h == 0 ? -1.f : 1.f, 1, 1});
                        put34(s, Mat4::fromPose(in.hand[h].grip));
                        put34(s, gm * Mat4::fromPose(avatar::fistPose()));
                        put34(s, gm * Mat4::fromPose(avatar::cuffPose(false)));
                    }
                    s += "]";
                    if (i < (int)A.show.size() && A.show[i] > 0.02f) {   // AIR GUITAR's guitar, and how far it has come
                        s += ",\"x\":[";
                        put34(s, A.prop[i]);
                        char xb[32];
                        std::snprintf(xb, sizeof xb, "],\"xs\":%.3f", A.show[i]);
                        s += xb;
                    }
                    if (r.bent) { s += ",\"p\":["; put34(s, r.pelvis); s += "]"; }
                    if (r.twisted) {
                        s += ",\"t\":[";
                        char b[24];
                        for (int j = 0; j < 32; ++j) { std::snprintf(b, sizeof b, "%s%.4f", j ? "," : "", r.twist[j]); s += b; }
                        s += "]";
                    }
                    char b[160];   // the TENNIS SKIRT's swing, then the KILT's (x, z, twist)
                    std::snprintf(b, sizeof b, ",\"w\":[%.4f,%.4f,%.4f,%.4f,%.4f,%.4f]}", sw[0].x, sw[0].z, sw[0].twist, sw[1].x, sw[1].z, sw[1].twist);
                    s += b;
                }
                s += "]";
                if (body == 0 && build == 1 && kept)
                    std::printf("wrists %-13s L bend %3.0f (mean %3.0f) roll %3.0f gap %4.1f cm | R bend %3.0f (mean %3.0f) roll %3.0f gap %4.1f cm%s\n",
                                A.id, bend[0], bendSum[0] / kept, roll[0], gap[0], bend[1], bendSum[1] / kept, roll[1], gap[1],
                                std::max(bend[0], bend[1]) > 60.f || std::max(roll[0], roll[1]) > 150.f || std::max(gap[0], gap[1]) > 3.f ? "   <<<" : "");
            }
            s += "}\n";
            const std::string path = outDir + "/anims_" + std::to_string(body) + std::to_string(build) + ".json";
            FILE* f = std::fopen(path.c_str(), "wb");
            if (!f) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return 2; }
            std::fputs(s.c_str(), f);
            std::fclose(f);
            std::printf("%s: %zu bytes\n", path.c_str(), s.size());
        }
    return 0;
}
