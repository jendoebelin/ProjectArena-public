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
             F_VISORTINT, F_MASK, F_SKIN, F_LOWER, F_BODY, F_GAUNTLET, F_FACE, F_NATION, F_COUNT };
// FACE shares AvatarDesc::body (sent raw, no spare byte): bit 0 = the body type, bits 1-2 = the face. Older builds wrap
// body % 2, so they still see the right body type (with its first face).
constexpr int kFaces = 4;
inline int bodyType(const AvatarDesc& d) { return d.body & 1; }
inline int faceOf(const AvatarDesc& d) { return ((d.body >> 1) & 3) % kFaces; }

// NATION (country presets: the team jersey in the flag's colours, that end of the court, flags on the banners). The
// country number (1.., 0 = none; app/src/nations.inc, append-only) rides in spare bits sent with the avatar: its low
// 5 bits in body bits 3-7, its high 3 bits in gauntlet bits 5-7 (GAUNTLET itself uses bits 0-4). Older builds (0.3.5
// and before) don't know it: they show such a player with FACE 1 and, for countries above 31, the first gauntlet.
struct Nation {
    const char* code;      // "USA"
    const char* name;      // "UNITED STATES"
    const char* flag;      // texture "flag_usa" (192 x 128)
    uint8_t kit[3][3];     // jersey: main, trim, accent (sRGB)
    float team[2][3];      // the glow colour for that end of the court, and a second choice (linear, brightest 1)
};
int nationCount();
const Nation* nation(int n);                   // nullptr for 0 (none) or out of range
inline int nationOf(const AvatarDesc& d) { return ((d.body >> 3) & 31) | ((d.gauntlet >> 5) << 5); }
inline int gauntletOf(const AvatarDesc& d) { return d.gauntlet & 31; }
// GAUNTLET: 0-2 the plate gauntlets (STRIKER, TITAN, PHANTOM), 3 the CONTROLLER (the lancer in a glove), 4-6 the
// grapple gauntlets (GRAPPLER, ARC CLAW, REGALIA: plate gauntlets whose fingers open on the trigger)
constexpr int kGauntletStyles = 7;
inline bool plateGauntlet(int k) { return k != 3 && k >= 0 && k < kGauntletStyles; }
inline bool grappleGauntlet(int k) { return k >= 4 && k < kGauntletStyles; }
void setNation(AvatarDesc& d, int n);
// with a country: the team kit (ARENA JERSEY, no pattern, athletic shorts, the nearest colour swatches, so builds
// that don't know countries still show a jersey in about the right colours); without: unchanged
AvatarDesc uniform(const AvatarDesc& d);
// the colours of the two ends of the court (linear, brightest channel 1): blue / orange, or the players' countries;
// two close colours (FRANCE against the blue end, or the same country twice) move the second end to its other colour
void sideColors(int nation0, int nation1, float out[2][3]);
Field rowField(int row);                       // builder display order (BODY TYPE first; COUNTRY is not a row:
constexpr int kRowCount = F_COUNT - 1;         //  the builder's PRESET > COUNTRY FLAG shows it as a sub-option)

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
const char* patternTexture(int pattern);       // "open_pat_..." or nullptr

std::string toText(const AvatarDesc& d);       // "suit=0\nbuild=1\n..." (avatar.cfg)
bool fromText(const std::string& s, AvatarDesc& d);

// ---------------------------------------------------------------- rig
// The skinned bodies (tools/build_body_parts.py, MPFB2): bind-pose joints in app space, origin = the neck base.
struct BodyBind { Vec3 eye, neckTop, shoulder[2], elbow[2], wrist[2], thumb[2]; };
const BodyBind& bodyBind(int body);
enum : int { BONE_TORSO, BONE_NECK, BONE_HEAD, BONE_UPPER_L, BONE_UPPER_R, BONE_FORE_L, BONE_FORE_R, BONE_CLAV_L, BONE_CLAV_R,
             BONE_COUNT };
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
    float reachGap[2] = {0, 0};    // THE REACH: how far each forearm ends short of its gauntlet (m; the arm never stretches)
    // THE REACH: the torso leans at the waist: below it the hips stay upright (pelvis: bone 0 there, bind -> world; the
    // shader blends it into skin[BONE_TORSO] over the waist, render.cpp); bent: it differs from the torso
    Mat4 pelvis;
    bool bent = false;
    float armScale = 1.f;          // ARM SIZING: the arms' length against the avatar's own
    // FOREARM TWIST (PR #25): the forearm bone turns with the hand (its front is the controller's top), so a turned hand
    // twisted the skin all at the elbow; the shader turns it back there, less and less toward the wrist (render.cpp).
    // 8 vec4, per forearm k (0 left, 1 right): [k] the bind elbow, 1 / (0.96 |d|^2); [2+k] d = bind elbow -> wrist, the
    // roll (rad); [4+k] the elbow (world), 1 / (0.96 length); [6+k] the forearm's axis (world). twisted: a roll to undo
    float twist[32] = {};
    float roll[2] = {0, 0};        // each forearm's turn against the upper arm's, untwisted (rad, -pi .. pi)
    bool twisted = false;
};

// TENNIS SKIRT and KILT (PR #25, his swish): a skirt's swing about the hips, one avatar's own: the hem trails the way
// the body moves and lags its turns, a damped spring, bounded; it starts afresh after a jump, a stall or a new skirt.
// Stepped with the rig (stepRig); the shader moves each vertex by it, by its weight, and keeps it out of the body
// (render.cpp uSwish)
struct Swish {
    int kind = -1;                       // 0 the tennis skirt, 1 the kilt; -1 none
    bool ready = false;
    double time = 0;
    float px = 0, pz = 0, yaw = 0;       // the hips last step (world)
    float fvx = 0, fvz = 0, fturn = 0;   // their speed (m/s) and turn (rad/s), filtered
    float x = 0, z = 0, twist = 0;       // the swing, in the garment's frame (m, rad)
    float vx = 0, vz = 0, vt = 0;
};
int swishKind(const AvatarDesc& d);      // the skirt worn: 0 tennis, 1 kilt, -1 none
void stepSwish(Swish& s, int kind, float px, float pz, float yaw, double now);

// THE REACH (PR #25; ARM SIZING): one avatar's rig across frames, its own (never shared): the torso's
// smoothed turn and lean toward a hand out of reach, each shoulder's swing, and how long its arms have grown to the
// reach its hands show. Stepped once a frame (stepRig), read by every draw of that frame (solveRig).
struct RigState {
    bool started = false;
    double time = 0;              // (the caller's clock, s) when last stepped
    Vec3 torso{0, 0, 0};          // heading (world), pitch, roll
    Vec3 slide[2];                // each shoulder's swing toward its hand (m)
    float armScale = 1.f;         // 0.8 .. 1.35
    float reachPeak = 1.f;        // ARM SIZING: the longest reach the hands have shown lately (against the arm's own)
    double peakAt = 0;            // ... when a reach last came up to it
    Vec3 lastW[2];                // (each wrist last frame: a jump faster than a hand is tracking, not a reach)
    bool handSeen[2] = {false, false};
    Swish swish;                  // TENNIS SKIRT / KILT
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
    // ELBOWS OUT OF THE BODY (owner, 2026-11-16): the elbow swings out once the arm is deeper in the torso than
    // elbowAllow (m); the arm kept clear by this share of its thickness (at rest it lies against the body by design)
    float elbowAllow = 0.010f, armClearShare = 0.6f;
    float reach = 0.09f;                        // the shoulder follows a hand out of reach by up to this (m; 7 cm
                                                // until the cuff moved under the fist, 2026-11-16, its wrist 4.5 cm lower)
    float reachStart = 1.0f;                    // ... once the hand is beyond this share of the arm's length (0.97 until
                                                // 2026-11-17: the shoulder came out to meet a hand the arm still
                                                // reached, so every full reach held the elbow bent; a player's video)
    float shrug = 0.06f;                        // how far a shoulder lifts with its hand straight overhead (m)
    float torsoFollow = 0.6f;                   // how much the chest turns toward the hands
    float torsoLimitDeg = 50.f;                 // never more than this from the head's heading
    Vec3 neck{0, 0, 0};                         // the neck top against the head (head frame: up, back = +y, +z)
    // where the forearm ends inside a plate gauntlet's cuff (grip space), and the cuff's own pose against the hand:
    // a rotation (pitch / yaw / roll degrees, gripEuler) about the wrist pivot, then an offset
    Vec3 forearmEnd{0.0247f, 0.0382f, 0.0876f};
    // the cuff against the hand: the owner's TUNING export (2026-10-19: 1.6 right, 2.2 up, 2.6 back (cm), pitched 17 deg,
    // yaw and roll -1), then lined up behind the fist when it was turned in line with the forearm (owner, 2026-11-16,
    // main.cpp drawGauntlet): swung 12 degrees about its own wrist point and 3.5 cm toward the little finger's side
    Vec3 cuffPos{0.016f, -0.013f, 0.026f}, cuffRot{28.99f, -2.19f, -1.78f};
};
RigTuning& rigTuning();
constexpr Vec3 kCuffPivot{0.004f, 0.005f, 0.050f};   // the wrist joint of the plate gauntlets (make_props.py, grip space)
// the cuff's pose against the hand (grip space; left = mirrored), from rigTuning()
Pose cuffPose(bool left);
// the plate gauntlets' fist against the hand (grip space, the right hand; the left is drawn mirrored): turned in line
// with the cuff and the forearm (owner, 2026-11-16; main.cpp drawGauntlet)
Pose fistPose();

// HANDS APART (owner, 2026-11-16, her video: with the controllers held together, the two gauntlets crossed into each
// other; armour is bigger than a hand round a controller): each part of a drawn gauntlet (the fist, the cuff) as a
// grid of its signed distance (inside negative) with points on its surface, so the two hands can be drawn just apart.
// Drawing only: the game keeps the tracked hands.
struct HandShape {
    Vec3 lo{0, 0, 0};                  // the grid's first point (the part's own frame)
    float h = 0;                       // the grid's spacing (m)
    int n[3] = {0, 0, 0};
    std::vector<int8_t> d;             // the signed distance at each point (mm, clamped)
    std::vector<Vec3> pts;             // points on its outside, about a centimetre apart
    Vec3 c{0, 0, 0};                   // a sphere round the points
    float r = 0;
    bool valid() const { return !pts.empty(); }
    float dist(const Vec3& p) const;   // (m) trilinear; off the grid, its edge value plus the way off
    Vec3 normal(const Vec3& p) const;  // the way out (unit)
};
// from a model's vertex positions (`stride` bytes apart, three floats first) and triangles, placed by toFrame. A point
// is inside when the surface lies on at least `sides` of its six axis directions (5 for a fist, whose plates have
// gaps between them; 4 for a tube open at both ends, such as a cuff)
void buildHandShape(HandShape& s, const uint8_t* verts, int stride, int nv, const uint32_t* idx, int ni, const Mat4& toFrame,
                    int sides);
// one part of a drawn hand: its shape, where it is drawn (place may mirror) and the way back
struct HandPart { const HandShape* shape = nullptr; Mat4 place, inv; };
Mat4 affineInverse(const Mat4& m);
// how far to move hand b (hand a moving the other way: half each) so that no surface point of either is inside the
// other, at most maxShift; zero when they don't meet
Vec3 handsApartShift(const HandPart* a, int na, const HandPart* b, int nb, float maxShift);
// the deepest any surface point of either hand is inside the other (m), with hand b moved S against hand a
float handsApartDepth(const HandPart* a, int na, const HandPart* b, int nb, const Vec3& S);
// buildHandShape from a whole model file (.mdl); false when it isn't one
bool handShapeFromModel(HandShape& s, const uint8_t* data, size_t size, const Mat4& toFrame, int sides);
// a hand prop's model space -> the grip (gltf metres -> UE centimetres -> the UE motion controller's transform -> the
// OpenXR grip: see tools/frames.py); main.cpp draws the gauntlets with ueLocalToGrip({1, -0.5, 0}, {5, 0, 0}, {1, 1, 1})
Mat4 ueLocalToGrip(Vec3 locCm, Vec3 pyrDeg, Vec3 scale);

// The two-bone IK elbow for shoulder s, wrist w and bone lengths l1, l2: on the circle of valid elbows, the point
// toward `pole`. Out of reach the arm points straight at the wrist.
Vec3 elbowIK(const Vec3& s, const Vec3& w, float l1, float l2, const Vec3& pole);
// the rig: with st, as it stands this frame (stepRig); without, the posture the moment asks for, arms unsized
Rig solveRig(const PlayerInput& in, const AvatarDesc& d, const RigState* st = nullptr);
// THE REACH: one step of an avatar's rig state, to `now` (s, the caller's clock; again within the same frame: nothing)
void stepRig(RigState& st, const PlayerInput& in, const AvatarDesc& d, double now);
// ELBOWS OUT OF THE BODY: how deep arm k of a solved rig is inside its torso (m, beyond RigTuning armClearShare of the
// arm's thickness; 0: clear). The torso's shape: app/src/avatar_torso.inc (tools/measure_torso.py)
float armInBody(const Rig& r, const AvatarDesc& d, int k);
// Model matrix for a bone mesh: joint a at the origin, -Y along the bone (scaled to its length), -Z toward front;
// mirrorX for the left side (negative determinant: flip the winding when drawing); thick scales the cross-section.
Mat4 boneMatrix(const Bone& b, bool mirrorX, float thick = 1.f);

// What to draw for one avatar (hands / gauntlets are drawn by the caller): model name, world matrix, winding.
// part glows (neon hair); skinned body part (uses the rig's skin); its texture scrolls along v (CIRCUIT RUNNER pulses)
// PART_HAIR: hair (head space). The game swings the styles that move (their spring chains: av_hair_*.swing) and tucks
// hair inside the headgear and face shield worn (av_gear_* / av_visor_*.tuck)
enum : uint8_t { PART_NEON = 1, PART_SKINNED = 2, PART_PULSE = 4, PART_HAIR = 8 };
struct PartDraw { char model[24]; Mat4 m; bool mirrored; uint8_t flags; };
void drawList(const Rig& r, const PlayerInput& in, const AvatarDesc& d, std::vector<PartDraw>& out, bool lod = false);

}  // namespace avatar
