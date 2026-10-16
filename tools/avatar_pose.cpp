// Desktop helper for tools/preview_avatar.py: prints one avatar's draw list (the game's own rig and part
// selection from app/src/avatar.cpp) for a look and a canned pose, as JSON on stdout.
// Build: tools/build_avatar_pose.bat   Usage: avatar_pose <preset N | seed:N | k=v,k=v...> <pose> <team 0|1> [yaw_deg]
//   poses: ready, guard, throw, rest
#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "../app/src/avatar.h"

static void printMat(const Mat4& m) {
    std::printf("[");
    for (int i = 0; i < 16; ++i) std::printf("%s%.6f", i ? "," : "", m.m[i]);
    std::printf("]");
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--options") {   // every builder option (colours as linear rgb), JSON
        const float team[3] = {0.1f, 0.7f, 1.0f};
        std::printf("{\"fields\":[");
        for (int f = 0; f < avatar::F_COUNT; ++f) {
            std::printf("%s{\"name\":\"%s\",\"color\":%d,\"options\":[", f ? "," : "", avatar::fieldName((avatar::Field)f),
                        avatar::isColor((avatar::Field)f) ? 1 : 0);
            for (int i = 0; i < avatar::fieldCount((avatar::Field)f); ++i) {
                float c[3];
                avatar::optionColor((avatar::Field)f, i, team, c);
                std::printf("%s{\"name\":\"%s\",\"rgb\":[%.5f,%.5f,%.5f]}", i ? "," : "", avatar::optionName((avatar::Field)f, i), c[0], c[1], c[2]);
            }
            std::printf("]}");
        }
        std::printf("],\"presets\":[");
        for (int i = 0; i < avatar::presetCount(); ++i) std::printf("%s\"%s\"", i ? "," : "", avatar::presetName(i));
        std::printf("]}\n");
        return 0;
    }
    if (argc < 4) { std::fprintf(stderr, "usage: avatar_pose <look> <pose> <team> [yaw]\n"); return 2; }
    AvatarDesc d;
    std::string look = argv[1];
    if (look.rfind("seed:", 0) == 0) d = avatar::randomLook((uint32_t)std::atoi(look.c_str() + 5));
    else if (look.find('=') != std::string::npos) {
        std::string t = look;
        for (char& c : t) if (c == ',') c = '\n';
        avatar::fromText(t, d);
    } else d = avatar::preset(std::atoi(look.c_str()));
    const std::string pose = argv[2];
    const int team = std::atoi(argv[3]);
    const float yaw = argc > 4 ? deg2rad((float)std::atof(argv[4])) : 0.f;
    // the avatar stands at the origin facing +Z (toward the preview camera), rotated by yaw
    const Quat face = Quat::axisAngle({0, 1, 0}, kPi + yaw);
    PlayerInput in;
    in.headTracked = true;
    in.head = {face, {0, 1.62f, 0}};
    struct HandPose { Vec3 p; float pitch, yaw, roll; };
    HandPose hp[2];   // in the avatar's frame: x = its right, z = its forward
    if (pose == "guard") { hp[0] = {{-0.15f, 1.40f, 0.30f}, 50, 15, 0}; hp[1] = {{0.15f, 1.40f, 0.30f}, 50, -15, 0}; }
    else if (pose == "throw") { hp[0] = {{-0.22f, 1.22f, 0.36f}, 10, 10, 0}; hp[1] = {{0.30f, 1.58f, -0.12f}, 35, -10, -20}; }
    else if (pose == "rest") { hp[0] = {{-0.24f, 0.92f, 0.12f}, -60, 0, 0}; hp[1] = {{0.24f, 0.92f, 0.12f}, -60, 0, 0}; }
    else { hp[0] = {{-0.21f, 1.08f, 0.30f}, -22, 8, 0}; hp[1] = {{0.21f, 1.08f, 0.30f}, -22, -8, 0}; }
    for (int h = 0; h < 2; ++h) {
        // avatar frame -> world: x_av = right = face(-X... ) ; build with the face rotation (avatar right = face * +X)
        Vec3 local{hp[h].p.x, hp[h].p.y, -hp[h].p.z};   // avatar frame: -Z forward
        Quat q = face * Quat::axisAngle({0, 1, 0}, deg2rad(hp[h].yaw)) * Quat::axisAngle({1, 0, 0}, deg2rad(hp[h].pitch)) *
                 Quat::axisAngle({0, 0, 1}, deg2rad(hp[h].roll));
        in.hand[h].tracked = true;
        in.hand[h].grip = {q, face.rotate({local.x, 0, local.z}) + Vec3{0, local.y, 0}};
    }
    in.avatar = d;
    avatar::Rig r = avatar::solveRig(in, d);
    std::vector<avatar::PartDraw> list;
    avatar::drawList(r, in, d, list);
    const float tc[2][3] = {{0.1f, 0.7f, 1.0f}, {1.0f, 0.4f, 0.1f}};
    float ch[avatar::CH_COUNT][4];
    avatar::channelColors(d, tc[team & 1], ch);
    std::printf("{\"parts\":[");
    for (size_t i = 0; i < list.size(); ++i) {
        std::printf("%s{\"model\":\"%s\",\"mirrored\":%d,\"neon\":%d,\"m\":", i ? "," : "", list[i].model, list[i].mirrored ? 1 : 0,
                    (list[i].flags & avatar::PART_NEON) ? 1 : 0);
        printMat(list[i].m);
        if (list[i].flags & avatar::PART_SKINNED) {   // the rig's skin matrices (bind -> world), column-major
            std::printf(",\"skin\":[");
            for (int k = 0; k < avatar::BONE_COUNT; ++k) { if (k) std::printf(","); printMat(r.skin[k]); }
            std::printf("]");
        }
        std::printf("}");
    }
    std::printf("],\"hands\":[");
    for (int h = 0; h < 2; ++h) {
        std::printf("%s{\"h\":%d,\"grip\":", h ? "," : "", h);
        printMat(Mat4::fromPose(in.hand[h].grip));
        std::printf("}");
    }
    std::printf("],\"channels\":[");
    for (int c = 0; c < avatar::CH_COUNT; ++c) std::printf("%s[%.5f,%.5f,%.5f]", c ? "," : "", ch[c][0], ch[c][1], ch[c][2]);
    const char* pat = avatar::patternTexture(d.pattern);
    std::printf("],\"gauntlet\":\"%s\"", d.gauntlet < 3 ? (d.gauntlet == 0 ? "gauntlet_0" : d.gauntlet == 1 ? "gauntlet_1" : "gauntlet_2") : "lancer");
    std::printf(",\"pattern\":\"%s\",\"team\":[%.3f,%.3f,%.3f],\"desc\":\"", pat ? pat : "", tc[team & 1][0], tc[team & 1][1], tc[team & 1][2]);
    for (int f = 0; f < avatar::F_COUNT; ++f)
        std::printf("%s%s: %s", f ? " | " : "", avatar::fieldName((avatar::Field)f), avatar::optionName((avatar::Field)f, avatar::get(d, (avatar::Field)f)));
    std::printf("\"}\n");
    return 0;
}
