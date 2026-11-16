// Experimental STATIC fixture exporter. Not the live VR renderer or temporal tracking path.
// Build via preview.py, using avatar_experimental.cpp and its matching bind.
#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "avatar.h"

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
    else if ((pose == "reach" || pose == "reachfit")) { hp[0] = {{-0.21f, 1.08f, 0.30f}, -22, 8, 0}; hp[1] = {{0.80f, 1.10f, 0.35f}, 0, -60, 0}; }   // far block
    else if (pose == "rest") { hp[0] = {{-0.24f, 0.92f, 0.12f}, -60, 0, 0}; hp[1] = {{0.24f, 0.92f, 0.12f}, -60, 0, 0}; }
    else { hp[0] = {{-0.21f, 1.08f, 0.30f}, -22, 8, 0}; hp[1] = {{0.21f, 1.08f, 0.30f}, -22, -8, 0}; }
    if (std::getenv("PA_LEFT_X")) hp[0].p.x=std::atof(std::getenv("PA_LEFT_X"));
    if (std::getenv("PA_LEFT_Y")) hp[0].p.y=std::atof(std::getenv("PA_LEFT_Y"));
    if (std::getenv("PA_LEFT_Z")) hp[0].p.z=std::atof(std::getenv("PA_LEFT_Z"));
    if (std::getenv("PA_TARGET_X")) hp[1].p.x=std::atof(std::getenv("PA_TARGET_X"));
    if (std::getenv("PA_TARGET_Y")) hp[1].p.y=std::atof(std::getenv("PA_TARGET_Y"));
    if (std::getenv("PA_TARGET_Z")) hp[1].p.z=std::atof(std::getenv("PA_TARGET_Z"));
    for (int h = 0; h < 2; ++h) {
        // avatar frame -> world: x_av = right = face(-X... ) ; build with the face rotation (avatar right = face * +X)
        Vec3 local{hp[h].p.x, hp[h].p.y, -hp[h].p.z};   // avatar frame: -Z forward
        Quat q = face * Quat::axisAngle({0, 1, 0}, deg2rad(hp[h].yaw)) * Quat::axisAngle({1, 0, 0}, deg2rad(hp[h].pitch)) *
                 Quat::axisAngle({0, 0, 1}, deg2rad(hp[h].roll));
        in.hand[h].tracked = true;
        in.hand[h].grip = {q, face.rotate({local.x, 0, local.z}) + Vec3{0, local.y, 0}};
        // the game's gauntlet tuning (settings.cfg pitch / fwd / up), applied as readInput does: PA_PITCH, PA_FWD, PA_UP
        const float tp = std::getenv("PA_PITCH") ? (float)std::atof(std::getenv("PA_PITCH")) : 0.f;
        const float tf = std::getenv("PA_FWD") ? (float)std::atof(std::getenv("PA_FWD")) : 0.f;
        const float tu = std::getenv("PA_UP") ? (float)std::atof(std::getenv("PA_UP")) : 0.f;
        const Quat tq = in.hand[h].grip.q * Quat::axisAngle({1, 0, 0}, deg2rad(tp));
        in.hand[h].grip = {tq, in.hand[h].grip.p + tq.rotate({0, tu, -tf})};
    }
    in.avatar = d;
    if (pose == "reachfit") {
        // A separate attainable-reach fixture, not a disguised replacement for stress reach.
        const auto& t=avatar::rigTuning(); const auto& K=avatar::bodyBind(avatar::bodyType(d));
        for(int i=0;i<16;i++) {
            auto trial=avatar::solveRig(in,d);
            const Vec3 fe{t.forearmEnd.x,t.forearmEnd.y,t.forearmEnd.z};
            const Vec3 target=in.hand[1].grip.transform(avatar::gauntletOf(in.avatar)<3 ? avatar::cuffPose(false).transform(fe) : Vec3{.004f,-.042f,.105f});
            const float natural=length(K.elbow[1]-K.shoulder[1])*t.upperLen+length(K.wrist[1]-K.elbow[1])*t.foreLen;
            const Vec3 delta=target-trial.upper[1].a;
            in.hand[1].grip.p += trial.upper[1].a+normalize(delta)*(natural*.97f)-target;
        }
    }
    avatar::Rig r = avatar::solveRig(in, d);
    std::vector<avatar::PartDraw> list;
    avatar::drawList(r, in, d, list);
    const float tc[2][3] = {{0.1f, 0.7f, 1.0f}, {1.0f, 0.4f, 0.1f}};
    float ch[avatar::CH_COUNT][4];
    avatar::channelColors(d, tc[team & 1], ch);
    std::printf("{\"targets\":[");
    for(int k=0;k<2;k++) {
        const float side=k==1?1.f:-1.f; const auto& t=avatar::rigTuning();
        const Vec3 fe{side*t.forearmEnd.x,t.forearmEnd.y,t.forearmEnd.z};
        const Vec3 w=in.hand[k].grip.transform(avatar::gauntletOf(in.avatar)<3 ? avatar::cuffPose(k==0).transform(fe) : Vec3{side*.004f,-.042f,.105f});
        std::printf("%s[%.6f,%.6f,%.6f]",k?",":"",w.x,w.y,w.z);
    }
    std::printf("],\"joints\":[");
    Vec3 pts[] = {r.neck.a,r.neck.b,r.upper[0].a,r.upper[0].b,r.fore[0].b,r.upper[1].a,r.upper[1].b,r.fore[1].b};
    for(int i=0;i<8;i++)std::printf("%s[%.6f,%.6f,%.6f]",i?",":"",pts[i].x,pts[i].y,pts[i].z);
    std::printf("],\"parts\":[");
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
