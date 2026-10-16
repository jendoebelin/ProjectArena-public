#include "avatar.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace avatar {

namespace {

struct Swatch { const char* name; uint8_t r, g, b; };   // sRGB

const char* kSuit[] = {"GRID RUNNER", "ARENA JERSEY", "VANGUARD", "STREET JACKET", "HOODIE", "ECHO SUIT", "TANK TOP", "PILOT",
                       "CIRCUIT VEST", "TRAINING TEE"};
const char* kLower[] = {"NONE", "ATHLETIC SHORTS", "CIRCUIT SHORTS", "BIKE SHORTS"};
const char* kBody[] = {"MASCULINE", "FEMININE"};
const char* kGauntlet[] = {"STRIKER", "TITAN", "PHANTOM", "CONTROLLER"};
const char* kBuild[] = {"SLIM", "STANDARD", "BROAD", "ATHLETIC"};
const char* kPattern[] = {"NONE", "CIRCUIT", "STRIPES", "CHEVRON", "HEX", "SPLIT", "PIXEL", "BANDS"};
const char* kPatternTex[] = {nullptr, "open_pat_circuit", "open_pat_stripes", "open_pat_chevron", "open_pat_hex",
                             "open_pat_split", "open_pat_pixel", "open_pat_bands"};
const char* kHair[] = {"NONE", "BUZZ CUT", "SWEPT", "PONYTAIL", "MOHAWK", "SPIKES", "BOB", "TOP KNOT", "TOUSLED"};
const char* kHeadgear[] = {"NONE", "GRID HELMET", "RACER HELMET", "HOOD", "CREST HELMET", "HEADPHONES", "CAP"};
const char* kVisor[] = {"NONE", "BAND VISOR", "FULL SHIELD", "SHADES", "GOGGLES", "HEADSET", "HALO", "SLIT VISOR"};
const char* kMask[] = {"NONE", "MOUTH GUARD", "CYBER JAW", "BREATHER", "BANDANA", "FACEPLATE"};

const Swatch kSuitCol[] = {{"GRAPHITE", 52, 56, 64},  {"SILVER", 168, 174, 184}, {"WHITE", 226, 230, 236},
                           {"MIDNIGHT", 22, 30, 62},  {"COBALT", 30, 82, 196},   {"TEAL", 18, 150, 156},
                           {"EMERALD", 22, 150, 84},  {"LIME", 150, 210, 40},    {"GOLD", 214, 168, 52},
                           {"ORANGE", 232, 112, 24},  {"CRIMSON", 176, 22, 44},  {"MAGENTA", 196, 30, 150},
                           {"VIOLET", 104, 52, 190},  {"BLACK", 16, 16, 19},     {"BROWN", 92, 56, 36},
                           {"GREY", 92, 95, 104}};
const Swatch kGlowCol[] = {{"TEAM", 0, 0, 0},         {"WHITE", 235, 245, 255},  {"CYAN", 40, 230, 255},
                           {"BLUE", 50, 110, 255},    {"VIOLET", 150, 70, 255},  {"MAGENTA", 255, 40, 200},
                           {"RED", 255, 40, 50},      {"ORANGE", 255, 120, 20},  {"GOLD", 255, 200, 40},
                           {"LIME", 170, 255, 40},    {"GREEN", 40, 255, 120},   {"PINK", 255, 120, 190}};
const Swatch kHairCol[] = {{"BLACK", 22, 20, 20},     {"DARK BROWN", 54, 36, 26}, {"BROWN", 98, 64, 40},
                           {"AUBURN", 128, 52, 30},   {"COPPER", 184, 94, 44},    {"BLONDE", 214, 180, 110},
                           {"PLATINUM", 232, 224, 200}, {"SILVER", 170, 176, 184}, {"CRIMSON", 170, 20, 36},
                           {"COBALT", 34, 74, 200},   {"VIOLET", 110, 50, 190},  {"PINK", 240, 110, 170},
                           {"NEON", 0, 0, 0}};
const Swatch kTint[] = {{"SMOKE", 30, 34, 42},   {"MIRROR", 150, 160, 175}, {"GOLD", 200, 150, 50},
                        {"ICE", 90, 170, 230},   {"MAGENTA", 190, 40, 150}, {"AMBER", 230, 130, 30},
                        {"EMERALD", 40, 170, 100}, {"TEAM", 0, 0, 0}};
const Swatch kSkin[] = {{"PORCELAIN", 246, 222, 206}, {"FAIR", 238, 200, 176}, {"LIGHT", 226, 180, 150},
                        {"TAN", 204, 150, 112},       {"OLIVE", 176, 134, 92}, {"BROWN", 140, 94, 62},
                        {"DEEP", 100, 64, 42},        {"EBONY", 62, 40, 28},   {"CHROME", 180, 186, 196},
                        {"ICE", 150, 200, 240},       {"GHOST", 230, 236, 245}, {"OBSIDIAN", 26, 26, 32}};

template <class T, size_t N> constexpr int count(T (&)[N]) { return (int)N; }

const char* kFieldName[F_COUNT] = {"SUIT", "BUILD", "PATTERN", "MAIN COLOR", "TRIM COLOR", "GLOW", "HAIR", "HAIR COLOR",
                                   "HEADGEAR", "FACE SHIELD", "SHIELD TINT", "MASK", "SKIN", "LOWER BODY", "BODY TYPE", "GAUNTLET"};
const char* kKey[F_COUNT] = {"suit", "build", "pattern", "primary", "secondary", "glow", "hair", "haircolor",
                             "headgear", "visor", "visortint", "mask", "skin", "lower", "body", "gauntlet"};

float lin(uint8_t c) { return std::pow(c / 255.f, 2.2f); }
void swatchLin(const Swatch& s, float out[3]) { out[0] = lin(s.r); out[1] = lin(s.g); out[2] = lin(s.b); }

uint8_t* ref(AvatarDesc& d, Field f) {
    switch (f) {
    case F_SUIT: return &d.suit;           case F_BUILD: return &d.build;       case F_PATTERN: return &d.pattern;
    case F_PRIMARY: return &d.primary;     case F_SECONDARY: return &d.secondary; case F_GLOW: return &d.glow;
    case F_HAIR: return &d.hair;           case F_HAIRCOLOR: return &d.hairColor; case F_HEADGEAR: return &d.headgear;
    case F_VISOR: return &d.visor;         case F_VISORTINT: return &d.visorTint; case F_MASK: return &d.mask;
    case F_SKIN: return &d.skin;           case F_LOWER: return &d.lower;       case F_BODY: return &d.body;
    case F_GAUNTLET: return &d.gauntlet;
    default: return &d.gauntlet;
    }
}

uint32_t rng(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
int pick(uint32_t& s, int n) { return (int)(rng(s) % (uint32_t)n); }

AvatarDesc make(int suit, int build, int pattern, int primary, int secondary, int glow, int hair, int hairColor,
                int headgear, int visor, int visorTint, int mask, int skin) {
    AvatarDesc d;
    const int v[F_COUNT] = {suit, build, pattern, primary, secondary, glow, hair, hairColor, headgear, visor, visorTint, mask, skin};
    for (int f = 0; f < F_COUNT; ++f) *ref(d, (Field)f) = (uint8_t)v[f];
    return d;
}

AvatarDesc withLower(AvatarDesc d, int lower, int body = 0) { d.lower = (uint8_t)lower; d.body = (uint8_t)body; return d; }

struct Preset { const char* name; AvatarDesc d; };
const Preset kPresets[] = {
    {"GRID RUNNER", make(0, 1, 1, 13, 0, 0, 0, 0, 1, 2, 0, 0, 2)},
    {"ROOKIE", make(1, 1, 2, 4, 2, 0, 2, 2, 5, 0, 0, 1, 3)},
    {"VANGUARD", make(2, 2, 4, 1, 3, 2, 1, 0, 4, 7, 0, 2, 5)},
    {"NIGHT RIDER", make(3, 1, 0, 13, 10, 6, 3, 5, 0, 3, 2, 4, 1)},
    {"ECHO", make(5, 0, 7, 2, 13, 1, 4, 12, 0, 6, 0, 0, 6)},
    {"SKY PILOT", make(7, 1, 0, 6, 8, 8, 1, 1, 0, 4, 5, 3, 4)},
    {"GHOST", make(4, 1, 6, 0, 12, 5, 0, 0, 3, 5, 4, 0, 10)},
    {"STRIKER", make(6, 2, 3, 9, 13, 7, 5, 11, 0, 1, 3, 0, 7)},
    {"CHROME", make(0, 1, 1, 1, 13, 2, 0, 0, 0, 1, 1, 5, 8)},
    {"RACER", make(5, 1, 5, 10, 2, 0, 0, 0, 2, 2, 2, 0, 2)},
    {"CIRCUIT", withLower(make(8, 2, 0, 13, 13, 2, 8, 2, 0, 0, 0, 0, 2), 2)},
    {"TRAINER", withLower(make(9, 3, 0, 14, 4, 3, 3, 2, 0, 0, 0, 0, 3), 1, 1)},
    {"TANK AND SHORTS", withLower(make(6, 1, 0, 14, 4, 0, 0, 0, 0, 0, 0, 0, 3), 1)},
    {"TRAINING KIT", withLower(make(9, 1, 0, 13, 15, 0, 0, 0, 0, 0, 0, 0, 3), 3, 1)},
};

}  // namespace

Field rowField(int row) {
    static const Field k[F_COUNT] = {F_BODY, F_SUIT, F_BUILD, F_LOWER, F_GAUNTLET, F_PATTERN, F_PRIMARY, F_SECONDARY, F_GLOW,
                                     F_HAIR, F_HAIRCOLOR, F_HEADGEAR, F_VISOR, F_VISORTINT, F_MASK, F_SKIN};
    return row >= 0 && row < F_COUNT ? k[row] : F_SUIT;
}

int fieldCount(Field f) {
    switch (f) {
    case F_SUIT: return count(kSuit);          case F_BUILD: return count(kBuild);     case F_PATTERN: return count(kPattern);
    case F_PRIMARY: case F_SECONDARY: return count(kSuitCol);
    case F_GLOW: return count(kGlowCol);       case F_HAIR: return count(kHair);       case F_HAIRCOLOR: return count(kHairCol);
    case F_HEADGEAR: return count(kHeadgear);  case F_VISOR: return count(kVisor);     case F_VISORTINT: return count(kTint);
    case F_MASK: return count(kMask);          case F_SKIN: return count(kSkin);       case F_LOWER: return count(kLower);    case F_BODY: return count(kBody);
    case F_GAUNTLET: return count(kGauntlet);
    default: return 1;
    }
}

const char* fieldName(Field f) { return f >= 0 && f < F_COUNT ? kFieldName[f] : ""; }

const char* optionName(Field f, int i) {
    if (i < 0 || i >= fieldCount(f)) return "?";
    switch (f) {
    case F_SUIT: return kSuit[i];            case F_BUILD: return kBuild[i];        case F_PATTERN: return kPattern[i];
    case F_PRIMARY: case F_SECONDARY: return kSuitCol[i].name;
    case F_GLOW: return kGlowCol[i].name;    case F_HAIR: return kHair[i];          case F_HAIRCOLOR: return kHairCol[i].name;
    case F_HEADGEAR: return kHeadgear[i];    case F_VISOR: return kVisor[i];        case F_VISORTINT: return kTint[i].name;
    case F_MASK: return kMask[i];            case F_SKIN: return kSkin[i].name;     case F_LOWER: return kLower[i];        case F_BODY: return kBody[i];
    case F_GAUNTLET: return kGauntlet[i];
    default: return "?";
    }
}

uint8_t get(const AvatarDesc& d, Field f) { return *ref(const_cast<AvatarDesc&>(d), f); }

void set(AvatarDesc& d, Field f, int v) {
    int n = fieldCount(f);
    *ref(d, f) = (uint8_t)(((v % n) + n) % n);
}

bool isColor(Field f) {
    return f == F_PRIMARY || f == F_SECONDARY || f == F_GLOW || f == F_HAIRCOLOR || f == F_VISORTINT || f == F_SKIN;
}

void optionColor(Field f, int i, const float team[3], float out[3]) {
    out[0] = out[1] = out[2] = 0.5f;
    if (i < 0 || i >= fieldCount(f)) return;
    const Swatch* s = nullptr;
    switch (f) {
    case F_PRIMARY: case F_SECONDARY: s = &kSuitCol[i]; break;
    case F_GLOW: s = &kGlowCol[i]; break;
    case F_HAIRCOLOR: s = &kHairCol[i]; break;
    case F_VISORTINT: s = &kTint[i]; break;
    case F_SKIN: s = &kSkin[i]; break;
    default: return;
    }
    if ((f == F_GLOW && i == 0) || (f == F_VISORTINT && i == count(kTint) - 1)) { out[0] = team[0]; out[1] = team[1]; out[2] = team[2]; return; }
    if (f == F_HAIRCOLOR && i == count(kHairCol) - 1) { out[0] = team[0]; out[1] = team[1]; out[2] = team[2]; return; }   // NEON: shown in team colour
    swatchLin(*s, out);
}

void sanitize(AvatarDesc& d) {
    for (int f = 0; f < F_COUNT; ++f) {
        uint8_t* v = ref(d, (Field)f);
        if (*v >= fieldCount((Field)f)) *v = 0;
    }
}

int presetCount() { return count(kPresets); }
const char* presetName(int i) { return kPresets[((i % presetCount()) + presetCount()) % presetCount()].name; }
AvatarDesc preset(int i) { return kPresets[((i % presetCount()) + presetCount()) % presetCount()].d; }

AvatarDesc randomLook(uint32_t seed) {
    uint32_t s = seed * 2654435761u + 0x9E3779B9u;
    if (!s) s = 1;
    for (int i = 0; i < 4; ++i) rng(s);
    AvatarDesc d;
    d.suit = (uint8_t)pick(s, count(kSuit));
    d.build = (uint8_t)(pick(s, 5) == 0 ? 0 : pick(s, 4) == 0 ? 2 : 1);
    d.pattern = (uint8_t)(pick(s, 3) == 0 ? 0 : pick(s, count(kPattern)));
    d.primary = (uint8_t)pick(s, count(kSuitCol));
    // a trim colour that reads against the main colour (luminance apart)
    float p3[3], s3[3];
    swatchLin(kSuitCol[d.primary], p3);
    for (int tries = 0; tries < 12; ++tries) {
        d.secondary = (uint8_t)pick(s, count(kSuitCol));
        swatchLin(kSuitCol[d.secondary], s3);
        float lp = 0.2126f * p3[0] + 0.7152f * p3[1] + 0.0722f * p3[2], ls = 0.2126f * s3[0] + 0.7152f * s3[1] + 0.0722f * s3[2];
        if (d.secondary != d.primary && std::fabs(std::sqrt(lp) - std::sqrt(ls)) > 0.18f) break;
    }
    d.glow = (uint8_t)(pick(s, 5) < 2 ? 0 : pick(s, count(kGlowCol)));
    d.headgear = (uint8_t)(pick(s, 2) == 0 ? 0 : pick(s, count(kHeadgear)));
    d.hair = (uint8_t)pick(s, count(kHair));
    d.hairColor = (uint8_t)(pick(s, 5) == 0 ? 7 + pick(s, count(kHairCol) - 7) : pick(s, 7));
    d.visor = (uint8_t)(pick(s, 10) < 3 ? 0 : pick(s, count(kVisor)));
    d.visorTint = (uint8_t)pick(s, count(kTint));
    d.mask = (uint8_t)(pick(s, 10) < 6 ? 0 : pick(s, count(kMask)));
    d.skin = (uint8_t)(pick(s, 10) == 0 ? 8 + pick(s, 4) : pick(s, 8));
    d.lower = (uint8_t)(pick(s, 2) == 0 ? 0 : 1 + pick(s, count(kLower) - 1));
    d.body = (uint8_t)pick(s, 2);
    d.gauntlet = (uint8_t)pick(s, 3);
    return d;
}

void channelColors(const AvatarDesc& in, const float team[3], float out[CH_COUNT][4]) {
    AvatarDesc d = in;
    sanitize(d);
    for (int c = 0; c < CH_COUNT; ++c) { out[c][0] = out[c][1] = out[c][2] = out[c][3] = 1; }
    optionColor(F_PRIMARY, d.primary, team, out[CH_PRIMARY]);
    optionColor(F_SECONDARY, d.secondary, team, out[CH_SECONDARY]);
    optionColor(F_GLOW, d.glow, team, out[CH_GLOW]);
    optionColor(F_SKIN, d.skin, team, out[CH_SKIN]);
    optionColor(F_VISORTINT, d.visorTint, team, out[CH_VISOR]);
    if (neonHair(d)) for (int k = 0; k < 3; ++k) out[CH_HAIR][k] = out[CH_GLOW][k];
    else optionColor(F_HAIRCOLOR, d.hairColor, team, out[CH_HAIR]);
    if (d.visorTint == count(kTint) - 1) for (int k = 0; k < 3; ++k) out[CH_VISOR][k] *= 0.45f;   // TEAM tint: dark glass
    // trim: brushed metal leaning toward the trim colour
    const float metal[3] = {0.50f, 0.52f, 0.56f};
    for (int k = 0; k < 3; ++k) out[CH_TRIM][k] = metal[k] * 0.7f + out[CH_SECONDARY][k] * 0.3f;
    for (int k = 0; k < 3; ++k) out[CH_GLOW][k] *= 1.15f;   // glow lines are unlit: a little over white for punch
}

bool neonHair(const AvatarDesc& d) { return d.hairColor == count(kHairCol) - 1; }
bool hidesHair(int headgear) { return headgear >= 1 && headgear <= 4; }   // helmets, hood
const char* patternTexture(int pattern) { return pattern > 0 && pattern < count(kPattern) ? kPatternTex[pattern] : nullptr; }

std::string toText(const AvatarDesc& d) {
    std::string s;
    char buf[48];
    for (int f = 0; f < F_COUNT; ++f) {
        std::snprintf(buf, sizeof buf, "%s=%d\n", kKey[f], (int)get(d, (Field)f));
        s += buf;
    }
    return s;
}

bool fromText(const std::string& s, AvatarDesc& d) {
    bool any = false;
    size_t i = 0;
    while (i < s.size()) {
        size_t e = s.find('\n', i);
        if (e == std::string::npos) e = s.size();
        std::string line = s.substr(i, e - i);
        i = e + 1;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        int v = std::atoi(line.c_str() + eq + 1);
        for (int f = 0; f < F_COUNT; ++f)
            if (k == kKey[f] && v >= 0 && v < fieldCount((Field)f)) { *ref(d, (Field)f) = (uint8_t)v; any = true; }
    }
    sanitize(d);
    return any;
}

// ---------------------------------------------------------------- rig
Build buildOf(int b) {
    // CURVED uses the curved torso meshes (bust, waist) at a slightly slimmer scale
    static const Build k[4] = {{0.94f, 0.96f, 0.92f, false}, {1.f, 1.f, 1.f, false}, {1.07f, 1.05f, 1.10f, false},
                               {1.03f, 1.02f, 1.05f, false}};
    return k[b < 0 ? 0 : b > 3 ? 3 : b];
}

static Vec3 flat(Vec3 v) {
    v.y = 0;
    return length(v) > 1e-4f ? normalize(v) : Vec3{0, 0, -1};
}

Vec3 elbowIK(const Vec3& s, const Vec3& w, float l1, float l2, const Vec3& pole) {
    Vec3 d = w - s;
    float dist = length(d);
    if (dist < 1e-4f) return s + normalize(pole - s) * l1;
    Vec3 u = d / dist;
    if (dist >= l1 + l2 - 1e-4f) return s + u * (dist * l1 / (l1 + l2));   // out of reach: straight, both bones stretch
    dist = std::max(dist, std::fabs(l1 - l2) + 1e-3f);
    float a = (l1 * l1 - l2 * l2 + dist * dist) / (2 * dist);
    float h = std::sqrt(std::max(0.f, l1 * l1 - a * a));
    Vec3 c = s + u * a;
    Vec3 p = pole - c;
    p = p - u * dot(p, u);
    if (length2(p) < 1e-10f) p = std::fabs(u.y) < 0.9f ? cross(u, Vec3{0, 1, 0}) : cross(u, Vec3{1, 0, 0});
    return c + normalize(p) * h;
}

#include "avatar_bodies.inc"

const BodyBind& bodyBind(int body) { return kBodyBind[body < 0 ? 0 : body > 1 ? 1 : body]; }

static Mat4 affineInverse(const Mat4& m) {
    const float* a = m.m;   // column-major 3x3 + translation
    const float c00 = a[5] * a[10] - a[9] * a[6], c01 = a[8] * a[6] - a[4] * a[10], c02 = a[4] * a[9] - a[8] * a[5];
    const float det = a[0] * c00 + a[1] * c01 + a[2] * c02;
    const float id = std::fabs(det) > 1e-12f ? 1.f / det : 0.f;
    Mat4 r = Mat4::identity();
    r.m[0] = c00 * id;                               r.m[4] = c01 * id;                               r.m[8] = c02 * id;
    r.m[1] = (a[9] * a[2] - a[1] * a[10]) * id;      r.m[5] = (a[0] * a[10] - a[8] * a[2]) * id;      r.m[9] = (a[8] * a[1] - a[0] * a[9]) * id;
    r.m[2] = (a[1] * a[6] - a[5] * a[2]) * id;       r.m[6] = (a[4] * a[2] - a[0] * a[6]) * id;       r.m[10] = (a[0] * a[5] - a[4] * a[1]) * id;
    for (int i = 0; i < 3; ++i) r.m[12 + i] = -(r.m[i] * a[12] + r.m[4 + i] * a[13] + r.m[8 + i] * a[14]);
    return r;
}

Rig solveRig(const PlayerInput& in, const AvatarDesc& d) {
    Rig r;
    const Pose& h = in.head;
    const Build B = buildOf(d.build);
    const BodyBind& K = bodyBind(d.body);
    // the head carries the neck top as in the bind pose; the torso hangs upright below it, so nodding or tilting the
    // head never swings the body
    const Vec3 neckTop = h.transform(K.neckTop - K.eye);
    // facing: the head's heading pulled toward the hands (looking aside while throwing keeps the chest on the
    // throw), never more than 50 degrees from where the head looks
    const Vec3 hf = flat(h.q.rotate({0, 0, -1}));
    const Vec3 base0 = neckTop + Vec3{0, -K.neckTop.y, 0};
    Vec3 f = hf;
    Vec3 mid{0, 0, 0};
    int nt = 0;
    for (int k = 0; k < 2; ++k)
        if (in.hand[k].tracked) { mid += in.hand[k].grip.p; ++nt; }
    if (nt) {
        Vec3 th = mid / float(nt) - base0;
        th.y = 0;
        if (length(th) > 0.12f) f = normalize(hf + normalize(th) * 0.6f);
        const float c50 = 0.6428f, s50 = 0.7660f;
        if (dot(f, hf) < c50) {
            Vec3 perp = f - hf * dot(f, hf);
            perp = length(perp) > 1e-5f ? normalize(perp) : Vec3{-hf.z, 0, hf.x};
            f = normalize(hf * c50 + perp * s50);
        }
    }
    const Quat yaw = Quat::axisAngle({0, 1, 0}, std::atan2(-f.x, -f.z));
    const Vec3 base = neckTop - yaw.rotate(K.neckTop);
    r.torso = {yaw, base};
    const Vec3 fwd = yaw.rotate({0, 0, -1});
    const float neckLen = length(K.neckTop);
    r.neck = {neckTop, base, fwd, neckLen};
    const Bone bindNeck = {K.neckTop, {0, 0, 0}, {0, 0, -1}, neckLen};
    const Vec3 scale{B.width, 1.f, B.depth};
    r.skin[BONE_TORSO] = Mat4::fromPose(r.torso, scale);
    r.skin[BONE_HEAD] = Mat4::fromPose(h) * Mat4::fromPose({Quat{}, Vec3{0, 0, 0} - K.eye});
    r.skin[BONE_NECK] = boneMatrix(r.neck, false, 1.f) * affineInverse(boneMatrix(bindNeck, false, 1.f));
    for (int k = 0; k < 2; ++k) {
        const float L1 = length(K.elbow[k] - K.shoulder[k]), L2 = length(K.wrist[k] - K.elbow[k]);
        const Vec3 S = r.torso.transform({K.shoulder[k].x * scale.x, K.shoulder[k].y, K.shoulder[k].z * scale.z});
        const Bone bu = {K.shoulder[k], K.elbow[k], (K.wrist[k] - K.elbow[k]) + Vec3{0, 0, -0.3f}, L1};
        const Bone bf = {K.elbow[k], K.wrist[k], K.thumb[k] - K.wrist[k], L2};
        const HandInput& hi = in.hand[k];
        if (!hi.tracked) {   // arms at rest: the bind pose, rotated with the torso
            r.upper[k] = {S, r.torso.transform(K.elbow[k]), yaw.rotate(bu.front), L1};
            r.fore[k] = {r.upper[k].b, r.torso.transform(K.wrist[k]), yaw.rotate(bf.front), L2};
        } else {
            r.arm[k] = true;
            const float side = k == 1 ? 1.f : -1.f;
            // the forearm ends inside the gauntlet's bracer (tools/blender/make_props.py lancer: wrist ring at grip
            // (0.004, -0.040, 0.050), bracer to z = 0.135)
            const Vec3 W = hi.grip.transform({side * 0.004f, -0.042f, 0.105f});
            const Vec3 back = hi.grip.q.rotate(normalize(Vec3{0, -0.047f, 1.f}));
            // elbow: where a straight wrist would put it, nudged down / out / back so the IK stays stable
            const Vec3 pole = W + back * L2 + yaw.rotate({side * 0.35f, -1.0f, 0.25f}) * 0.12f;
            const Vec3 E = elbowIK(S, W, L1, L2, pole);
            r.upper[k] = {S, E, (W - E) + fwd * 0.3f, L1};   // the inner elbow faces the forearm
            r.fore[k] = {E, W, hi.grip.q.rotate({0, 1, 0}), L2};   // thumb side (controller top) is the front
        }
        const bool mir = k == 0;
        r.skin[BONE_UPPER_L + k] = boneMatrix(r.upper[k], mir, B.arm) * affineInverse(boneMatrix(bu, mir, 1.f));
        r.skin[BONE_FORE_L + k] = boneMatrix(r.fore[k], mir, B.arm) * affineInverse(boneMatrix(bf, mir, 1.f));
    }
    return r;
}

Mat4 boneMatrix(const Bone& b, bool mirrorX, float thick) {
    Vec3 axis = b.a - b.b;
    float len = length(axis);
    Vec3 y = len > 1e-5f ? axis / len : Vec3{0, 1, 0};
    Vec3 fr = b.front - y * dot(b.front, y);
    if (length2(fr) < 1e-8f) {
        fr = Vec3{0, 0, -1} - y * (-y.z);
        if (length2(fr) < 1e-8f) fr = Vec3{1, 0, 0} - y * y.x;
    }
    Vec3 z = -normalize(fr);
    Vec3 x = cross(y, z);
    if (mirrorX) x = -x;
    float s = len / b.natural;
    return Mat4::fromBasis(x * thick, y * s, z * thick, b.a);
}

void drawList(const Rig& r, const PlayerInput& in, const AvatarDesc& din, std::vector<PartDraw>& out, bool lod) {
    AvatarDesc d = din;
    sanitize(d);
    const char bk = d.body ? 'f' : 'm';
    char name[32];
    auto add = [&](const char* model, const Mat4& m, bool mirrored, uint8_t flags = 0) {
        PartDraw p;
        std::snprintf(p.model, sizeof p.model, "%s", model);
        p.m = m;
        p.mirrored = mirrored;
        p.flags = flags;
        out.push_back(p);
    };
    const Mat4 head = Mat4::fromPose(in.head, {kHeadScale, kHeadScale, kHeadScale});
    const Mat4 I = Mat4::identity();
    std::snprintf(name, sizeof name, "av_top_%d_%c", d.suit, bk);
    add(name, I, false, PART_SKINNED);
    std::snprintf(name, sizeof name, "av_bhead_%c", bk);
    add(name, I, false, PART_SKINNED);
    if (d.lower) { std::snprintf(name, sizeof name, "av_bottom_%d_%c", d.lower, bk); add(name, I, false, PART_SKINNED); }
    else { std::snprintf(name, sizeof name, "av_wfade_%c", bk); add(name, I, false, PART_SKINNED); }
    if (d.headgear) { std::snprintf(name, sizeof name, "av_gear_%d_%c", d.headgear, bk); add(name, head, false); }
    else if (lod && d.hair) { std::snprintf(name, sizeof name, "av_hair_%d_%c", d.hair, bk); add(name, head, false, neonHair(d) ? PART_NEON : 0); }
    if (lod) return;   // miniatures: the silhouette is enough
    int hair = hidesHair(d.headgear) ? 0 : d.hair;
    if (hair && d.headgear == 6) hair = 1;   // under the cap: the close-cropped layer (longer styles poke through it)
    if (hair) {
        std::snprintf(name, sizeof name, "av_hair_%d_%c", hair, bk);
        add(name, head, false, neonHair(d) ? PART_NEON : 0);
    }
    if (d.visor) { std::snprintf(name, sizeof name, "av_visor_%d_%c", d.visor, bk); add(name, head, false); }
    if (d.mask) { std::snprintf(name, sizeof name, "av_mask_%d_%c", d.mask, bk); add(name, head, false); }
}

}  // namespace avatar
