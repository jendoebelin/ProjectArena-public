// Project Arena avatars option tables and colour palettes, the body rig (torso, neck and two-bone IK arms
// solved from the tracked head and hands) and avatar.cfg persistence. Platform independent and unit-tested;
// drawing and the in-game builder live in avatar_draw.cpp.
// Avatar parts are separate models (tools/blender/make_avatar.py):
//   head space   origin = between the eyes (the HMD pose), +X right, +Y up, -Z forward: av_head, av_hair_N,
//                av_gear_N, av_visor_N, av_mask_N
//   torso space  origin = neck base, yaw-only (upright), -Z forward: av_torso_N (scaled by the build)
//   bone space   joint at the origin, the bone runs down -Y for its natural length, front = -Z, outside = +X
//                (right side; the left side is drawn mirrored): av_upper_N, av_fore_N, av_neck
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "game.h"

namespace avatar {

enum Channel { CH_NONE = 0, CH_PRIMARY, CH_SECONDARY, CH_GLOW, CH_SKIN, CH_VISOR, CH_HAIR, CH_TRIM, CH_COUNT };

// builder rows, in display order
enum Field { F_SUIT, F_BUILD, F_PATTERN, F_PRIMARY, F_SECONDARY, F_GLOW, F_HAIR, F_HAIRCOLOR, F_HEADGEAR, F_VISOR,
             F_VISORTINT, F_MASK, F_SKIN, F_LOWER, F_BODY, F_GAUNTLET, F_COUNT };
Field rowField(int row);                       // builder display order (BODY TYPE first)

int fieldCount(Field f);                       // number of options
const char* fieldName(Field f);
const char* optionName(Field f, int i);
uint8_t get(const AvatarDesc& d, Field f);
void set(AvatarDesc& d, Field f, int v);       // wraps into range
bool isColor(Field f);
// display colour of option i of a colour field (linear rgb); team = the wearer's team colour (for "TEAM")
void optionColor(Field f, int i, const float team[3], float out[3]);
void sanitize(AvatarDesc& d);

int presetCount();
const char* presetName(int i);
AvatarDesc preset(int i);
AvatarDesc randomLook(uint32_t seed);          // a random but coherent look (bots, the RANDOM button)

// colours for every channel, linear rgb + 1 (out[CH_NONE] = white)
void channelColors(const AvatarDesc& d, const float team[3], float out[CH_COUNT][4]);
bool neonHair(const AvatarDesc& d);            // hair glows (HAIR COLOUR = NEON)
bool hidesHair(int headgear);                  // full helmets and the hood cover the hair
const char* patternTexture(int pattern);       // "open_pat_..." or nullptr

std::string toText(const AvatarDesc& d);       // "suit=0\nbuild=1\n..." (avatar.cfg)
bool fromText(const std::string& s, AvatarDesc& d);

// ---------------------------------------------------------------- rig
// The skinned bodies (tools/build_body_parts.py, MPFB2): bind-pose joints in app space, origin = the neck base.
struct BodyBind { Vec3 eye, neckTop, shoulder[2], elbow[2], wrist[2], thumb[2]; };
const BodyBind& bodyBind(int body);
enum : int { BONE_TORSO, BONE_NECK, BONE_HEAD, BONE_UPPER_L, BONE_UPPER_R, BONE_FORE_L, BONE_FORE_R, BONE_COUNT };
struct Build { float width, depth, arm; bool curved; };   // torso x / z scale, arm thickness
Build buildOf(int build);

constexpr float kUpperArm = 0.285f, kForeArm = 0.265f, kNeck = 0.115f;   // natural bone lengths (m)
constexpr float kHeadScale = 1.0f;    // the MPFB heads are drawn at their true size

struct Bone {
    Vec3 a, b;        // joint -> joint
    Vec3 front;       // reference for the mesh's front (-Z); need not be perpendicular to the bone
    float natural;    // the mesh's authored length
};

struct Rig {
    Pose torso;                    // neck base, upright, -Z = facing
    Bone neck;
    bool arm[2] = {false, false};  // hand tracked
    Bone upper[2], fore[2];        // 0 = left, 1 = right
    Mat4 skin[BONE_COUNT];         // bind -> world for the skinned parts
};

// Skeleton and arm-attachment values the rig uses, adjustable live in developer mode (MENU > SETTINGS > TUNING; the
// app keeps them in files/tuning.cfg). The defaults are the shipped rig. Right-hand / right-side values; the left
// side is mirrored.
struct RigTuning {
    Vec3 shoulder{0, 0, 0};                     // added to both shoulders (x = outward, y = up, z = back)
    float upperLen = 1.f, foreLen = 1.f;        // bone length scales
    float armThick = 1.f;                       // arm cross-section scale (with the build's)
    Vec3 elbowPole{0.35f, -1.0f, 0.25f};        // the elbow's pull: out, down, back (torso frame)
    float elbowPull = 0.12f;                    // how far toward the pole (m)
    float reach = 0.07f;                        // the shoulder follows a hand out of reach by up to this (m)
    float reachStart = 0.97f;                   // ... once the hand is beyond this share of the arm's length
    float torsoFollow = 0.6f;                   // how much the chest turns toward the hands
    float torsoLimitDeg = 50.f;                 // never more than this from the head's heading
    Vec3 neck{0, 0, 0};                         // the neck top against the head (head frame: up, back = +y, +z)
    // where the forearm ends inside a plate gauntlet's cuff (grip space), and the cuff's own pose against the hand:
    // a rotation (pitch / yaw / roll degrees, gripEuler) about the wrist pivot, then an offset
    Vec3 forearmEnd{0.0267f, 0.0382f, 0.0876f};
    Vec3 cuffPos{0, 0, 0}, cuffRot{18, 0, 0};   // cuff pitched 18 deg about the wrist (owner's TUNING export, 2026-10-18)
};
RigTuning& rigTuning();
constexpr Vec3 kCuffPivot{0.004f, 0.005f, 0.050f};   // the wrist joint of the plate gauntlets (make_props.py, grip space)
// the cuff's pose against the hand (grip space; left = mirrored), from rigTuning()
Pose cuffPose(bool left);

// The two-bone IK elbow for shoulder s, wrist w and bone lengths l1, l2: on the circle of valid elbows, the point
// toward `pole`. Out of reach the arm points straight at the wrist.
Vec3 elbowIK(const Vec3& s, const Vec3& w, float l1, float l2, const Vec3& pole);
Rig solveRig(const PlayerInput& in, const AvatarDesc& d);
// Model matrix for a bone mesh: joint a at the origin, -Y along the bone (scaled to its length), -Z toward front;
// mirrorX for the left side (negative determinant: flip the winding when drawing); thick scales the cross-section.
Mat4 boneMatrix(const Bone& b, bool mirrorX, float thick = 1.f);

// What to draw for one avatar (hands / gauntlets are drawn by the caller): model name, world matrix, winding.
enum : uint8_t { PART_NEON = 1, PART_SKINNED = 2 };   // part glows (neon hair); skinned body part (uses the rig's skin)
struct PartDraw { char model[24]; Mat4 m; bool mirrored; uint8_t flags; };
void drawList(const Rig& r, const PlayerInput& in, const AvatarDesc& d, std::vector<PartDraw>& out, bool lod = false);

}  // namespace avatar
