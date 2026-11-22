#include "avatar.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace avatar {

namespace {

struct Swatch { const char* name; uint8_t r, g, b; };   // sRGB

const char* kSuit[] = {"GRID RUNNER", "ARENA JERSEY", "VANGUARD", "STREET JACKET", "HOODIE", "ECHO SUIT", "TANK TOP", "PILOT",
                       "CIRCUIT VEST", "TRAINING TEE", "CIRCUIT RUNNER",   // 10: modelled traces with moving pulses
                       "GRID RUNNER LIVE", "ECHO SUIT LIVE", "CIRCUIT VEST LIVE",   // 11-13: their glow lines raised, pulsing
                       "VECTOR", "NIGHTFORGE",    // 14, 15: PR #25 (their own LOWER BODY 1-4; older versions: GRID RUNNER)
                       "RONIN WRAP",              // 16: PR #27, 0.3.22 (contrib/outfit/ronin-wrap; older versions: GRID RUNNER)
                       "PELURU TUXEDO", "PELURU LAYERED DRESS"};   // 17, 18: PR #30, 0.3.22 (tools/build_peluru.py; the
                                                                   // dress its own skirt; older versions: GRID RUNNER)
const char* kLower[] = {"NONE", "ATHLETIC SHORTS", "CIRCUIT SHORTS", "BIKE SHORTS",
                        "LONG SHORTS", "TENNIS SKIRT", "KILT"};   // 4-6: PR #25 (the skirts sway; older versions: NONE)
const char* kBody[] = {"MASCULINE", "FEMININE"};
const char* kGauntlet[] = {"STRIKER", "TITAN", "PHANTOM", "CONTROLLER",
                           "GRAPPLER", "ARC CLAW", "REGALIA"};   // 4-6: the trigger opens them (older versions: STRIKER)
const char* kFace[] = {"FACE 1", "FACE 2", "FACE 3", "FACE 4"};   // tools/blender/make_body.py FACES (FACE 1 = the base)
const char* kBuild[] = {"SLIM", "STANDARD", "BROAD", "ATHLETIC"};
const char* kPattern[] = {"NONE", "CIRCUIT", "STRIPES", "CHEVRON", "HEX", "SPLIT", "PIXEL", "BANDS"};
const char* kPatternTex[] = {nullptr, "open_pat_circuit", "open_pat_stripes", "open_pat_chevron", "open_pat_hex",
                             "open_pat_split", "open_pat_pixel", "open_pat_bands"};
const char* kHair[] = {"NONE", "BUZZ CUT", "SWEPT", "PONYTAIL", "MOHAWK", "SPIKES", "BOB", "TOP KNOT", "TOUSLED",
                       "HIGH PIGTAILS", "DREADLOCKS"};   // 9, 10: their tails and locs swing (older versions: NONE)
const char* kHeadgear[] = {"NONE", "GRID HELMET", "RACER HELMET", "HOOD", "CREST HELMET", "HEADPHONES", "CAP",
                           "HORNED HELMET", "VECTOR HELMET",   // 7, 8: PR #25 (older versions: NONE)
                           "RONIN KABUTO",                     // 9: PR #27, 0.3.22 (contrib/headgear/ronin-kabuto; older: NONE)
                           "SOMBRERO"};                        // 10: PR #30, 0.3.22 (tools/build_peluru.py; older: NONE)
const char* kVisor[] = {"NONE", "BAND VISOR", "FULL SHIELD", "SHADES", "GOGGLES", "HEADSET", "HALO", "SLIT VISOR"};
const char* kMask[] = {"NONE", "MOUTH GUARD", "CYBER JAW", "BREATHER", "BANDANA", "FACEPLATE",
                       "SUGAR SKULL"};   // 6: PR #30, 0.3.22 (a mask on each face; older versions: NONE)

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
                                   "HEADGEAR", "FACE SHIELD", "SHIELD TINT", "MASK", "SKIN", "LOWER BODY", "BODY TYPE", "GAUNTLET", "FACE",
                                   "COUNTRY"};
const char* kKey[F_COUNT] = {"suit", "build", "pattern", "primary", "secondary", "glow", "hair", "haircolor",
                             "headgear", "visor", "visortint", "mask", "skin", "lower", "body", "gauntlet", "face", "nation"};

const Nation kNations[] = {
#include "nations.inc"
};

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
    default: return &d.gauntlet;   // (F_FACE and F_NATION live in spare bits: get / set handle them)
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
    // the full sets (the owner, 2026-11-19: "make the new outfits that have full sets into the presets menu ... and
    // don't forget to add night forge outfit and vector outfit"), in their makers' own colours where they gave them:
    {"RONIN", withLower(make(16, 1, 0, 4, 9, 2, 7, 0, 9, 0, 0, 4, 4), 4)},               // PR #27: wrap, kabuto, bandana
    {"PELURU TUXEDO", withLower(make(17, 1, 0, 13, 8, 11, 2, 0, 10, 0, 0, 6, 3), 4)},    // PR #30: sombrero, sugar skull
    {"PELURU DRESS", withLower(make(18, 1, 0, 13, 5, 11, 9, 0, 10, 0, 0, 6, 5), 0, 1)},
    {"VECTOR", withLower(make(14, 1, 0, 13, 1, 2, 6, 0, 8, 0, 0, 0, 1), 4, 1)},          // PR #25: its helmet, its shorts
    {"NIGHTFORGE", withLower(make(15, 3, 0, 13, 8, 2, 1, 0, 7, 0, 0, 0, 6), 4)},         // PR #25: the HORNED HELMET
};

}  // namespace

int fieldCount(Field f) {
    switch (f) {
    case F_SUIT: return count(kSuit);          case F_BUILD: return count(kBuild);     case F_PATTERN: return count(kPattern);
    case F_PRIMARY: case F_SECONDARY: return count(kSuitCol);
    case F_GLOW: return count(kGlowCol);       case F_HAIR: return count(kHair);       case F_HAIRCOLOR: return count(kHairCol);
    case F_HEADGEAR: return count(kHeadgear);  case F_VISOR: return count(kVisor);     case F_VISORTINT: return count(kTint);
    case F_MASK: return count(kMask);          case F_SKIN: return count(kSkin);       case F_LOWER: return count(kLower);    case F_BODY: return count(kBody);
    case F_GAUNTLET: return count(kGauntlet);
    case F_FACE: return kFaces;
    case F_NATION: return nationCount() + 1;
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
    case F_FACE: return kFace[i];
    case F_NATION: return i == 0 ? "NONE" : kNations[i - 1].name;
    default: return "?";
    }
}

uint8_t get(const AvatarDesc& d, Field f) {
    if (f == F_BODY) return (uint8_t)bodyType(d);
    if (f == F_FACE) return (uint8_t)faceOf(d);
    if (f == F_NATION) return (uint8_t)nationOf(d);
    if (f == F_GAUNTLET) return (uint8_t)gauntletOf(d);
    if (f == F_PATTERN) return (uint8_t)(d.pattern & ~kOwnKitBit);   // (its top bit: COUNTRY KIT)
    return *ref(const_cast<AvatarDesc&>(d), f);
}

void set(AvatarDesc& d, Field f, int v) {
    int n = fieldCount(f);
    v = ((v % n) + n) % n;
    if (f == F_BODY) d.body = (uint8_t)((d.body & ~1) | v);
    else if (f == F_FACE) d.body = (uint8_t)((d.body & ~6) | (v << 1));
    else if (f == F_NATION) setNation(d, v);
    else if (f == F_GAUNTLET) d.gauntlet = (uint8_t)((d.gauntlet & ~31) | v);
    else if (f == F_PATTERN) d.pattern = (uint8_t)((d.pattern & kOwnKitBit) | v);
    else *ref(d, f) = (uint8_t)v;
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
    const bool own = (d.pattern & kOwnKitBit) != 0;   // (COUNTRY KIT: kept with a country)
    d.pattern = (uint8_t)(d.pattern & ~kOwnKitBit);
    for (int f = 0; f < F_COUNT; ++f) {
        if (f == F_BODY || f == F_FACE || f == F_NATION || f == F_GAUNTLET) continue;
        uint8_t* v = ref(d, (Field)f);
        if (*v >= fieldCount((Field)f)) *v = 0;
    }
    int n = nationOf(d), face = (d.body >> 1) & 3;
    if (n > nationCount()) n = 0;
    if (face >= kFaces) face = 0;   // a face out of range: FACE 1
    if (gauntletOf(d) >= fieldCount(F_GAUNTLET)) d.gauntlet = 0;
    d.body = (uint8_t)(bodyType(d) | (face << 1));
    d.gauntlet = (uint8_t)gauntletOf(d);
    setNation(d, n);
    if (own && n) d.pattern = (uint8_t)(d.pattern | kOwnKitBit);
}

int nationCount() { return count(kNations); }
const Nation* nation(int n) { return n >= 1 && n <= count(kNations) ? &kNations[n - 1] : nullptr; }

void setNation(AvatarDesc& d, int n) {
    if (n < 0 || n > 255) n = 0;
    d.body = (uint8_t)((d.body & 7) | ((n & 31) << 3));
    d.gauntlet = (uint8_t)((d.gauntlet & 31) | ((n >> 5) << 5));
}

void setOwnKit(AvatarDesc& d, bool own) {
    d.pattern = (uint8_t)((d.pattern & ~kOwnKitBit) | (own && nationOf(d) ? kOwnKitBit : 0));
}

std::vector<int> nationOrder(const int* picks) {
    auto key = [](int n) {
        const char* s = kNations[n - 1].name;
        return std::strncmp(s, "THE ", 4) == 0 ? s + 4 : s;
    };
    std::vector<int> v;
    for (int n = 1; n <= nationCount(); ++n) v.push_back(n);
    std::stable_sort(v.begin(), v.end(), [&](int x, int y) {
        const int px = picks ? std::max(0, picks[x]) : 0, py = picks ? std::max(0, picks[y]) : 0;
        if (px != py) return px > py;
        return std::strcmp(key(x), key(y)) < 0;
    });
    return v;
}

namespace {
int nearest(const Swatch* s, int n, int from, const uint8_t c[3]) {
    int best = from;
    float bd = 1e9f;
    for (int i = from; i < n; ++i) {
        const float dr = lin(s[i].r) - lin(c[0]), dg = lin(s[i].g) - lin(c[1]), db = lin(s[i].b) - lin(c[2]);
        const float dd = dr * dr * 0.3f + dg * dg * 0.59f + db * db * 0.11f;
        if (dd < bd) { bd = dd; best = i; }
    }
    return best;
}
// the kit's trim (ribbed collar, sleeve bands, side stripes, number panel, gloves): the flag's accent colour, or its
// second colour when the accent is the jersey's own (CANADA: red, white, red)
int kitTrim(const Nation* N) {
    int d = 0;
    for (int k = 0; k < 3; ++k) d += std::abs(int(N->kit[2][k]) - int(N->kit[0][k]));
    return d > 90 ? 2 : 1;
}
void glowOf(const uint8_t c[3], const float fallback[3], float out[3]) {   // a kit colour as a glow (brightest 1)
    float v[3] = {lin(c[0]), lin(c[1]), lin(c[2])};
    const float m = std::max(v[0], std::max(v[1], v[2]));
    for (int k = 0; k < 3; ++k) out[k] = m < 0.02f ? fallback[k] : v[k] / m;
}
}  // namespace

AvatarDesc uniform(const AvatarDesc& in) {
    AvatarDesc d = in;
    sanitize(d);
    const Nation* N = nation(nationOf(d));
    if (!N) return d;
    if (d.pattern & kOwnKitBit) {   // COUNTRY KIT = MY OUTFIT: your suit, pattern and lower body stay
        d.pattern = (uint8_t)(d.pattern & ~kOwnKitBit);
    } else {
        d.suit = 1;      // ARENA JERSEY
        d.pattern = 0;
        if (d.lower == 0 || d.lower == 2) d.lower = 1;   // ATHLETIC SHORTS (BIKE, LONG SHORTS and the skirts stay)
    }
    d.primary = (uint8_t)nearest(kSuitCol, count(kSuitCol), 0, N->kit[0]);
    d.secondary = (uint8_t)nearest(kSuitCol, count(kSuitCol), 0, N->kit[kitTrim(N)]);
    d.glow = (uint8_t)nearest(kGlowCol, count(kGlowCol), 1, N->kit[2]);
    return d;
}

AvatarDesc sentLook(const AvatarDesc& in) {
    AvatarDesc s = in;
    sanitize(s);
    AvatarDesc d = uniform(s);
    if (ownKit(s)) d.pattern = (uint8_t)(d.pattern | kOwnKitBit);
    return d;
}

void sideColors(int n0, int n1, float out[2][3]) {
    static const float def[2][3] = {{0.10f, 0.70f, 1.00f}, {1.00f, 0.40f, 0.10f}};
    const int ns[2] = {n0, n1};
    float cand[2][2][3];   // each end's first and second choice
    for (int p = 0; p < 2; ++p) {
        const Nation* N = nation(ns[p]);
        for (int k = 0; k < 3; ++k) {
            cand[p][0][k] = N ? N->team[0][k] : def[p][k];
            cand[p][1][k] = N ? N->team[1][k] : def[1 - p][k];
        }
    }
    auto dist = [](const float* a, const float* b) {
        float s = 0;
        for (int k = 0; k < 3; ++k) s += (a[k] - b[k]) * (a[k] - b[k]);
        return std::sqrt(s);
    };
    for (int k = 0; k < 3; ++k) out[0][k] = cand[0][0][k];
    const int pick = dist(cand[1][0], out[0]) >= 0.55f ? 0 : dist(cand[1][1], out[0]) > dist(cand[1][0], out[0]) ? 1 : 0;
    for (int k = 0; k < 3; ++k) out[1][k] = cand[1][pick][k];
    if (dist(out[0], out[1]) < 0.55f && dist(cand[0][1], out[1]) > dist(out[0], out[1]))   // still close: the first end moves
        for (int k = 0; k < 3; ++k) out[0][k] = cand[0][1][k];
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
    d.body = (uint8_t)(pick(s, 2) | (pick(s, kFaces) << 1));
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
    if (const Nation* N = nation(nationOf(d))) {   // the team kit in the flag's own colours
        const int t = kitTrim(N);
        for (int k = 0; k < 3; ++k) { out[CH_PRIMARY][k] = lin(N->kit[0][k]); out[CH_SECONDARY][k] = lin(N->kit[t][k]); }
        glowOf(N->kit[2], N->team[0], out[CH_GLOW]);
    }
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
const char* patternTexture(int pattern) { return pattern > 0 && pattern < count(kPattern) ? kPatternTex[pattern] : nullptr; }

std::string toText(const AvatarDesc& d) {
    std::string s;
    char buf[48];
    for (int f = 0; f < F_COUNT; ++f) {
        std::snprintf(buf, sizeof buf, "%s=%d\n", kKey[f], (int)get(d, (Field)f));
        s += buf;
    }
    if (ownKit(d)) s += "kit=1\n";   // COUNTRY KIT = MY OUTFIT (builds before 0.3.22 skip the key: the team jersey)
    return s;
}

bool fromText(const std::string& s, AvatarDesc& d) {
    bool any = false, own = false;
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
            if (k == kKey[f] && v >= 0 && v < fieldCount((Field)f)) { set(d, (Field)f, v); any = true; }
        if (k == "kit") own = v == 1;
    }
    sanitize(d);
    setOwnKit(d, own);   // (no kit line: TEAM JERSEY)
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
#include "avatar_torso.inc"

// ---------------------------------------------------------------- ELBOWS OUT OF THE BODY
// (owner, 2026-11-16, her video in the builder's mirror: hands together at the chest, the elbows sank into the torso and
// the forearms came out of the chest: the elbow was placed without the body in mind)
namespace {
// how deep a point (torso space with the build's scale taken out) is inside the torso grown by `grow` (0: outside,
// or above the chest / below the hips, where the shoulders and legs are)
float torsoDepth(const TorsoShape& T, const Vec3& p, float grow) {
    const float fi = (p.y - kTorsoY0) / kTorsoDY;   // (the slices go down)
    if (fi < -0.5f || fi > kTorsoSlices - 0.5f) return 0.f;
    const int i0 = std::max(0, std::min(kTorsoSlices - 1, int(std::floor(fi))));
    const int i1 = std::min(kTorsoSlices - 1, i0 + 1);
    const float fy = clampf(fi - float(i0), 0.f, 1.f);
    const float cz = T.cz[i0] + (T.cz[i1] - T.cz[i0]) * fy;
    const float fs = (std::atan2(p.z - cz, p.x) + kPi) / (2.f * kPi) * kTorsoSectors - 0.5f;
    const float fl = std::floor(fs);
    const int k0 = ((int(fl) % kTorsoSectors) + kTorsoSectors) % kTorsoSectors, k1 = (k0 + 1) % kTorsoSectors;
    const float fk = fs - fl;
    auto at = [&](int i) { return T.r[i][k0] + (T.r[i][k1] - T.r[i][k0]) * fk; };
    const float R = at(i0) + (at(i1) - at(i0)) * fy;
    return std::max(0.f, R + grow - std::sqrt(p.x * p.x + (p.z - cz) * (p.z - cz)));
}

float armInTorso(const TorsoShape& T, const Pose& torso, const Vec3& scale, const Vec3& S, const Vec3& E, const Vec3& W,
                 float upperR, float foreR) {
    auto local = [&](const Vec3& p) {
        const Vec3 q = torso.q.conj().rotate(p - torso.p);
        return Vec3{q.x / scale.x, q.y, q.z / scale.z};
    };
    float worst = 0.f;
    for (int i = 0; i <= 4; ++i)   // the upper arm from halfway (by the shoulder it meets the body by design) to the elbow
        worst = std::max(worst, torsoDepth(T, local(lerp(S, E, 0.5f + 0.125f * i)), upperR));
    for (int i = 1; i <= 4; ++i)   // the forearm from the elbow to the cuff's rim (its last half is inside the gauntlet's
        worst = std::max(worst, torsoDepth(T, local(lerp(E, W, 0.1375f * i)), foreR));   // cuff, on the controller)
    return worst;
}

// the elbow E0 (on the circle the two bones allow round the shoulder-to-wrist line) swung the least way round it until
// the arm is no deeper in the torso than `allow`: out to the side rather than in (going in counts half as far again),
// and only as far as it must, so it starts from where it was as soon as the arm meets the body (no jumps)
Vec3 elbowClear(const TorsoShape& T, const Pose& torso, const Vec3& scale, float side, const Vec3& S, const Vec3& W,
                const Vec3& E0, float upperR, float foreR, float allow) {
    if (armInTorso(T, torso, scale, S, E0, W, upperR, foreR) <= allow) return E0;
    const Vec3 sw = W - S;
    const float d = length(sw);
    if (d < 1e-4f) return E0;
    const Vec3 n = sw / d;
    const Vec3 C = S + n * dot(E0 - S, n);
    Vec3 u = E0 - C;
    const float rho = length(u);
    if (rho < 0.01f) return E0;   // (the arm straight: nothing to swing)
    u = u / rho;
    const Vec3 v = cross(n, u);
    auto at = [&](float phi) { return C + (u * std::cos(phi) + v * std::sin(phi)) * rho; };
    auto depth = [&](float phi) { return armInTorso(T, torso, scale, S, at(phi), W, upperR, foreR); };
    // which way round is out: the way the elbow first moves away from the torso's middle line
    const Vec3 outward = torso.q.rotate({side, 0, 0});
    const float outSign = dot(v, outward) >= 0 ? 1.f : -1.f;
    const float step = deg2rad(4.f), most = deg2rad(140.f);
    float found[2] = {-1.f, -1.f};   // [0] outward, [1] inward: the first clear angle
    float bestPhi = 0.f, bestDepth = 1e9f;
    for (int w = 0; w < 2; ++w) {
        const float sgn = w == 0 ? outSign : -outSign;
        float prev = 0.f;
        for (float a = step; a <= most + 1e-4f; a += step) {
            const float dd = depth(sgn * a);
            if (dd < bestDepth - 1e-4f) { bestDepth = dd; bestPhi = sgn * a; }
            if (dd <= allow) {   // clear: halve back toward the last step that wasn't
                float lo = prev, hi = a;
                for (int i = 0; i < 4; ++i) {
                    const float mid = (lo + hi) * 0.5f;
                    (depth(sgn * mid) <= allow ? hi : lo) = mid;
                }
                found[w] = hi;
                break;
            }
            prev = a;
        }
    }
    float phi;
    if (found[0] >= 0 && (found[1] < 0 || found[0] <= found[1] * 1.5f)) phi = outSign * found[0];
    else if (found[1] >= 0) phi = -outSign * found[1];
    else phi = bestPhi;   // (it can't clear: the hand is inside the body; as clear as it gets)
    return at(phi);
}
}  // namespace

float armInBody(const Rig& r, const AvatarDesc& d, int k) {
    const TorsoShape& T = kTorsoShape[bodyType(d)];
    const Build B = buildOf(d.build);
    const float thick = B.arm * rigTuning().armThick * rigTuning().armClearShare;
    return armInTorso(T, r.torso, {B.width, 1.f, B.depth}, r.upper[k].a, r.upper[k].b, r.fore[k].b, T.upperArm * thick,
                      T.foreArm * thick);
}

const BodyBind& bodyBind(int body) { return kBodyBind[body < 0 ? 0 : body > 1 ? 1 : body]; }

Mat4 affineInverse(const Mat4& m) {
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

RigTuning& rigTuning() { static RigTuning t; return t; }

Pose cuffPose(bool left) {
    const RigTuning& T = rigTuning();
    const float s = left ? -1.f : 1.f;
    const Quat q = gripEuler(T.cuffRot, left);
    const Vec3 piv{s * kCuffPivot.x, kCuffPivot.y, kCuffPivot.z};
    return {q, piv + Vec3{s * T.cuffPos.x, T.cuffPos.y, T.cuffPos.z} - q.rotate(piv)};
}

Pose fistPose() {
    // turned 30 degrees about the knuckle bumper (game.h Tunables::knuckleOffset, as it stands by default), so the
    // knuckle glass and the ball stay on it; then 2 cm toward the thumb and 2.5 cm back into the cuff's opening
    const Vec3 kn{0, -0.026f, -0.057f};
    const Quat q = Quat::axisAngle({1, 0, 0}, deg2rad(30.f));
    return {q, kn + Vec3{0, 0.020f, 0.025f} - q.rotate(kn)};
}

// ---------------------------------------------------------------- HANDS APART
namespace {
constexpr float kShapeVoxel = 0.003f;   // the grid's spacing (m)
constexpr int kShapePad = 3;            // its border, in points
constexpr float kPointCell = 0.010f;    // one surface point per cell this size

Vec3 xfPoint(const Mat4& m, const Vec3& v) {
    return {m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z + m.m[12], m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z + m.m[13],
            m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14]};
}
Vec3 xfDir(const Mat4& m, const Vec3& v) {
    return {m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z, m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z, m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z};
}

// the exact squared distance transform along one line (Felzenszwalb and Huttenlocher), in place: f holds 0 at the
// features and kFar elsewhere
constexpr float kFar = 1e20f;
void edtLine(float* f, int n, std::vector<float>& g, std::vector<int>& v, std::vector<float>& z) {
    int first = 0;
    while (first < n && f[first] >= kFar) ++first;
    if (first == n) return;   // (nothing near on this line yet: it stays far)
    g.assign(f, f + n);
    v.resize(n);
    z.resize(n + 1);
    int k = 0;
    v[0] = first; z[0] = -1e30f; z[1] = 1e30f;
    for (int q = first + 1; q < n; ++q) {
        if (g[q] >= kFar) continue;
        float s = ((g[q] + float(q) * q) - (g[v[k]] + float(v[k]) * v[k])) / (2.f * (q - v[k]));
        while (k > 0 && s <= z[k]) {
            --k;
            s = ((g[q] + float(q) * q) - (g[v[k]] + float(v[k]) * v[k])) / (2.f * (q - v[k]));
        }
        ++k;
        v[k] = q; z[k] = s; z[k + 1] = 1e30f;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < q) ++k;
        f[q] = float(q - v[k]) * (q - v[k]) + g[v[k]];
    }
}

// squared distance (in grid steps) from every point to the nearest feature point
std::vector<float> edt3(const std::vector<uint8_t>& feature, const int n[3]) {
    const size_t N = size_t(n[0]) * n[1] * n[2];
    std::vector<float> f(N);
    for (size_t i = 0; i < N; ++i) f[i] = feature[i] ? 0.f : kFar;
    std::vector<float> line, g, z;
    std::vector<int> v;
    const size_t stride[3] = {1, size_t(n[0]), size_t(n[0]) * n[1]};
    for (int ax = 0; ax < 3; ++ax) {
        const int a1 = (ax + 1) % 3, a2 = (ax + 2) % 3;
        line.resize(n[ax]);
        for (int j = 0; j < n[a1]; ++j)
            for (int k = 0; k < n[a2]; ++k) {
                const size_t base = j * stride[a1] + k * stride[a2];
                for (int i = 0; i < n[ax]; ++i) line[i] = f[base + i * stride[ax]];
                edtLine(line.data(), n[ax], g, v, z);
                for (int i = 0; i < n[ax]; ++i) f[base + i * stride[ax]] = line[i];
            }
    }
    return f;
}
}  // namespace

void buildHandShape(HandShape& s, const uint8_t* verts, int stride, int nv, const uint32_t* idx, int ni, const Mat4& toFrame,
                    int sides) {
    s = HandShape();
    if (nv <= 0 || ni < 3) return;
    std::vector<Vec3> P(nv);
    Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    for (int i = 0; i < nv; ++i) {
        float p[3];
        std::memcpy(p, verts + size_t(i) * stride, 12);
        P[i] = xfPoint(toFrame, {p[0], p[1], p[2]});
        lo = {std::min(lo.x, P[i].x), std::min(lo.y, P[i].y), std::min(lo.z, P[i].z)};
        hi = {std::max(hi.x, P[i].x), std::max(hi.y, P[i].y), std::max(hi.z, P[i].z)};
    }
    const float h = kShapeVoxel;
    s.h = h;
    s.lo = lo - Vec3{h, h, h} * float(kShapePad);
    const Vec3 span = hi - lo;
    s.n[0] = int(std::ceil(span.x / h)) + 2 * kShapePad + 1;
    s.n[1] = int(std::ceil(span.y / h)) + 2 * kShapePad + 1;
    s.n[2] = int(std::ceil(span.z / h)) + 2 * kShapePad + 1;
    if (s.n[0] * s.n[1] * s.n[2] > 400000) { s = HandShape(); return; }   // (not a hand)
    const int* n = s.n;
    const size_t N = size_t(n[0]) * n[1] * n[2];
    auto at = [&](int x, int y, int z) { return (size_t(z) * n[1] + y) * n[0] + x; };
    // the surface: each triangle sampled half a step apart, its grid points marked; one surface point kept for
    // each centimetre cell (the first one sampled there)
    std::vector<uint8_t> surf(N, 0);
    std::vector<Vec3> samples;
    const int cells[3] = {int(span.x / kPointCell) + 1, int(span.y / kPointCell) + 1, int(span.z / kPointCell) + 1};
    std::vector<uint8_t> cellTaken(size_t(cells[0]) * cells[1] * cells[2], 0);
    for (int t = 0; t + 2 < ni; t += 3) {
        if (idx[t] >= uint32_t(nv) || idx[t + 1] >= uint32_t(nv) || idx[t + 2] >= uint32_t(nv)) continue;
        const Vec3 A = P[idx[t]], B = P[idx[t + 1]], C = P[idx[t + 2]];
        const float e = std::max(length(B - A), std::max(length(C - B), length(A - C)));
        const int m = std::max(1, std::min(200, int(std::ceil(e / (h * 0.5f)))));
        for (int i = 0; i <= m; ++i)
            for (int j = 0; i + j <= m; ++j) {
                const float u = float(i) / m, w = float(j) / m;
                const Vec3 p = A * u + B * w + C * (1.f - u - w);
                const int gx = int(std::floor((p.x - s.lo.x) / h + 0.5f)), gy = int(std::floor((p.y - s.lo.y) / h + 0.5f)),
                          gz = int(std::floor((p.z - s.lo.z) / h + 0.5f));
                if (gx < 0 || gy < 0 || gz < 0 || gx >= n[0] || gy >= n[1] || gz >= n[2]) continue;
                surf[at(gx, gy, gz)] = 1;
                const int cx = std::min(cells[0] - 1, std::max(0, int((p.x - lo.x) / kPointCell)));
                const int cy = std::min(cells[1] - 1, std::max(0, int((p.y - lo.y) / kPointCell)));
                const int cz = std::min(cells[2] - 1, std::max(0, int((p.z - lo.z) / kPointCell)));
                uint8_t& taken = cellTaken[(size_t(cz) * cells[1] + cy) * cells[0] + cx];
                if (!taken) { taken = 1; samples.push_back(p); }
            }
    }
    // inside: the surface on at least `sides` of the six directions along the axes
    std::vector<uint8_t> count(N, 0);
    for (int ax = 0; ax < 3; ++ax) {
        const int a1 = (ax + 1) % 3, a2 = (ax + 2) % 3;
        for (int j = 0; j < n[a1]; ++j)
            for (int k = 0; k < n[a2]; ++k) {
                int c[3];
                c[a1] = j; c[a2] = k;
                bool seen = false;
                for (int i = 0; i < n[ax]; ++i) { c[ax] = i; const size_t q = at(c[0], c[1], c[2]); seen |= surf[q] != 0; count[q] += seen; }
                seen = false;
                for (int i = n[ax] - 1; i >= 0; --i) { c[ax] = i; const size_t q = at(c[0], c[1], c[2]); seen |= surf[q] != 0; count[q] += seen; }
            }
    }
    std::vector<uint8_t> solid(N), empty(N);
    for (size_t q = 0; q < N; ++q) { solid[q] = surf[q] || count[q] >= sides; empty[q] = !solid[q]; }
    const std::vector<float> toSolid = edt3(solid, n), toEmpty = edt3(empty, n);
    s.d.resize(N);
    for (size_t q = 0; q < N; ++q) {   // (zero through the outermost solid points: the surface runs through them)
        const float mm = solid[q] ? -(float(std::sqrt(toEmpty[q])) - 1.f) * h * 1000.f : float(std::sqrt(toSolid[q])) * h * 1000.f;
        s.d[q] = int8_t(std::max(-120.f, std::min(120.f, std::round(mm))));
    }
    // the surface points on its outside (not those within it, such as the dark grip inside the fist)
    for (const Vec3& p : samples)
        if (s.dist(p) > -h * 1.5f) s.pts.push_back(p);
    if (s.pts.empty()) { s = HandShape(); return; }
    Vec3 plo{1e9f, 1e9f, 1e9f}, phi{-1e9f, -1e9f, -1e9f};
    for (const Vec3& p : s.pts) {
        plo = {std::min(plo.x, p.x), std::min(plo.y, p.y), std::min(plo.z, p.z)};
        phi = {std::max(phi.x, p.x), std::max(phi.y, p.y), std::max(phi.z, p.z)};
    }
    s.c = (plo + phi) * 0.5f;
    for (const Vec3& p : s.pts) s.r = std::max(s.r, length(p - s.c));
    s.r += h;
}

float HandShape::dist(const Vec3& p) const {
    if (d.empty()) return 1.f;
    const float g[3] = {(p.x - lo.x) / h, (p.y - lo.y) / h, (p.z - lo.z) / h};
    float off2 = 0, f[3];
    int i0[3];
    for (int k = 0; k < 3; ++k) {
        const float c = std::max(0.f, std::min(float(n[k] - 1), g[k]));
        off2 += (g[k] - c) * (g[k] - c);
        i0[k] = std::min(n[k] - 2, int(c));
        f[k] = c - i0[k];
    }
    auto v = [&](int dx, int dy, int dz) { return float(d[(size_t(i0[2] + dz) * n[1] + (i0[1] + dy)) * n[0] + (i0[0] + dx)]); };
    const float x00 = v(0, 0, 0) + (v(1, 0, 0) - v(0, 0, 0)) * f[0], x10 = v(0, 1, 0) + (v(1, 1, 0) - v(0, 1, 0)) * f[0];
    const float x01 = v(0, 0, 1) + (v(1, 0, 1) - v(0, 0, 1)) * f[0], x11 = v(0, 1, 1) + (v(1, 1, 1) - v(0, 1, 1)) * f[0];
    const float y0 = x00 + (x10 - x00) * f[1], y1 = x01 + (x11 - x01) * f[1];
    return (y0 + (y1 - y0) * f[2]) * 0.001f + std::sqrt(off2) * h;
}

Vec3 HandShape::normal(const Vec3& p) const {
    const float e = h * 0.75f;
    const Vec3 g{dist(p + Vec3{e, 0, 0}) - dist(p - Vec3{e, 0, 0}), dist(p + Vec3{0, e, 0}) - dist(p - Vec3{0, e, 0}),
                 dist(p + Vec3{0, 0, e}) - dist(p - Vec3{0, 0, e})};
    const float l = length(g);
    if (l > 1e-6f) return g / l;
    const Vec3 o = p - c;   // (flat there: straight out from the middle)
    return length(o) > 1e-6f ? normalize(o) : Vec3{0, 1, 0};
}

namespace {
// how deep any surface point of either hand is inside the other, with hand b moved S against hand a; `vote` (when
// given) adds up each one's way out for hand b, by its depth; it stops early once one is deeper than `enough`
float apartDepth(const HandPart* a, int na, const HandPart* b, int nb, const Vec3& S, Vec3* vote, float enough) {
    float deepest = 0;
    for (int i = 0; i < na; ++i)
        for (int j = 0; j < nb; ++j) {
            const HandPart& A = a[i];
            const HandPart& B = b[j];
            if (!A.shape || !B.shape || !A.shape->valid() || !B.shape->valid()) continue;
            const Vec3 ca = xfPoint(A.place, A.shape->c), cb = xfPoint(B.place, B.shape->c) + S;
            if (length(ca - cb) > A.shape->r + B.shape->r) continue;
            for (const Vec3& p : A.shape->pts) {   // a's surface inside b: b goes back, the way out of b reversed
                const Vec3 w = xfPoint(A.place, p);
                if (length(w - cb) > B.shape->r) continue;
                const Vec3 q = xfPoint(B.inv, w - S);
                const float d = B.shape->dist(q);
                if (d >= 0) continue;
                deepest = std::max(deepest, -d);
                if (vote) *vote = *vote + xfDir(B.place, B.shape->normal(q)) * d;
                else if (deepest > enough) return deepest;
            }
            for (const Vec3& p : B.shape->pts) {   // b's surface inside a: b goes on, the way out of a
                const Vec3 w = xfPoint(B.place, p) + S;
                if (length(w - ca) > A.shape->r) continue;
                const Vec3 q = xfPoint(A.inv, w);
                const float d = A.shape->dist(q);
                if (d >= 0) continue;
                deepest = std::max(deepest, -d);
                if (vote) *vote = *vote + xfDir(A.place, A.shape->normal(q)) * -d;
                else if (deepest > enough) return deepest;
            }
        }
    return deepest;
}

constexpr float kApartTouch = 0.001f;   // deeper than this, they cross (m)
constexpr float kApartStep = 0.004f;

// the least distance along D from which, out to maxShift, the hands stay apart (stepped, then halved down); more
// than maxShift when they don't come apart within it
float apartAlong(const HandPart* a, int na, const HandPart* b, int nb, const Vec3& D, float maxShift) {
    float last = 0;   // the furthest step at which they still cross (0: only where they are)
    for (float t = kApartStep; t < maxShift + kApartStep * 0.5f; t += kApartStep)
        if (apartDepth(a, na, b, nb, D * t, nullptr, kApartTouch) > kApartTouch) last = t;
    if (last >= maxShift - kApartStep * 0.5f) return maxShift * 2.f;
    float lo = last, hi = std::min(maxShift, last + kApartStep);
    for (int i = 0; i < 3; ++i) {
        const float mid = (lo + hi) * 0.5f;
        (apartDepth(a, na, b, nb, D * mid, nullptr, kApartTouch) > kApartTouch ? lo : hi) = mid;
    }
    return hi;
}
}  // namespace

namespace {
// the closest points of segments p0-p1 and q0-q1
void closestOnSegments(const Vec3& p0, const Vec3& p1, const Vec3& q0, const Vec3& q1, Vec3& cp, Vec3& cq) {
    const Vec3 d1 = p1 - p0, d2 = q1 - q0, r = p0 - q0;
    const float a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
    float s = 0, t = 0;
    if (a < 1e-10f && e < 1e-10f) { cp = p0; cq = q0; return; }
    if (a < 1e-10f) {
        t = clampf(f / e, 0.f, 1.f);
    } else {
        const float c = dot(d1, r);
        if (e < 1e-10f) {
            s = clampf(-c / a, 0.f, 1.f);
        } else {
            const float b = dot(d1, d2), den = a * e - b * b;
            s = den > 1e-10f ? clampf((b * f - c * e) / den, 0.f, 1.f) : 0.5f;   // (parallel: their middles)
            t = (b * s + f) / e;
            if (t < 0) { t = 0; s = clampf(-c / a, 0.f, 1.f); }
            else if (t > 1) { t = 1; s = clampf((b - c) / a, 0.f, 1.f); }
        }
    }
    cp = p0 + d1 * s;
    cq = q0 + d2 * t;
}
}  // namespace

Vec3 handsApartShift(const HandPart* a, int na, const HandPart* b, int nb, float maxShift) {
    // The way apart: each hand's middle line (from its fist's middle to its cuff's), and the way from the nearest point
    // of one to the other's: across when the hands are side by side, over or under when the wrists cross (the way
    // they already are). Along it, the least distance from which they stay apart. (A way picked from the points inside
    // went back and forth: sliding apart, hands can first cross deeper, and more than half a fist across, the way out
    // of it points back; choosing between two ways flipped crossed wrists over and under.)
    if (na <= 0 || nb <= 0 || !a[0].shape || !b[0].shape || apartDepth(a, na, b, nb, {0, 0, 0}, nullptr, kApartTouch) <= kApartTouch)
        return {0, 0, 0};
    auto line = [](const HandPart* h, int n, Vec3& p0, Vec3& p1) {
        p0 = xfPoint(h[0].place, h[0].shape->c);
        p1 = n > 1 && h[1].shape && h[1].shape->valid() ? xfPoint(h[1].place, h[1].shape->c) : p0;
    };
    Vec3 a0, a1, b0, b1, ca, cb;
    line(a, na, a0, a1);
    line(b, nb, b0, b1);
    closestOnSegments(a0, a1, b0, b1, ca, cb);
    Vec3 D = cb - ca;
    if (length(D) < 0.001f) {   // (the middle lines meet: across both, the way from one fist to the other)
        D = cross(a1 - a0, b1 - b0);
        if (dot(D, b0 - a0) < 0) D = D * -1.f;
        if (length(D) < 1e-6f) D = b0 - a0;
        if (length(D) < 1e-6f) D = {1, 0, 0};
    }
    D = normalize(D);
    return D * std::min(maxShift, apartAlong(a, na, b, nb, D, maxShift));
}

float handsApartDepth(const HandPart* a, int na, const HandPart* b, int nb, const Vec3& S) {
    return apartDepth(a, na, b, nb, S, nullptr, 1e9f);
}

bool handShapeFromModel(HandShape& s, const uint8_t* data, size_t size, const Mat4& toFrame, int sides) {
    s = HandShape();
    if (size < 24 || std::memcmp(data, "SMDL", 4) != 0) return false;
    uint32_t hdr[5];
    std::memcpy(hdr, data + 4, sizeof hdr);
    const size_t vsz = hdr[0] >= 3 ? 52 : 44;
    if (size < 24 + size_t(hdr[1]) * vsz + size_t(hdr[2]) * 4) return false;
    std::vector<uint32_t> idx(hdr[2]);
    std::memcpy(idx.data(), data + 24 + size_t(hdr[1]) * vsz, idx.size() * 4);
    buildHandShape(s, data + 24, int(vsz), int(hdr[1]), idx.data(), int(hdr[2]), toFrame, sides);
    return s.valid();
}

Mat4 ueLocalToGrip(Vec3 locCm, Vec3 pyrDeg, Vec3 scale) {
    float p = deg2rad(pyrDeg.x), y = deg2rad(pyrDeg.y), r = deg2rad(pyrDeg.z);
    float sp = std::sin(p), cp = std::cos(p), sy = std::sin(y), cy = std::cos(y), sr = std::sin(r), cr = std::cos(r);
    float R[3][3] = {{cp * cy, cp * sy, sp}, {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp}, {-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp}};
    Mat4 T = Mat4::identity();
    for (int c = 0; c < 3; ++c) for (int rr = 0; rr < 3; ++rr) T.m[c * 4 + rr] = R[c][rr] * (&scale.x)[c];
    T.m[12] = locCm.x; T.m[13] = locCm.y; T.m[14] = locCm.z;
    Mat4 S{};   // gltf (x,y,z) m -> UE (x,z,y) cm
    S.m[0] = 100; S.m[6] = 100; S.m[9] = 100; S.m[15] = 1;
    Mat4 C{};   // UE cm -> grip (y, z, -x) m
    C.m[2] = -0.01f; C.m[4] = 0.01f; C.m[9] = 0.01f; C.m[15] = 1;
    return C * T * S;
}

// ---------------------------------------------------------------- THE REACH
// (ProjectArena-public PR #25 and issue #24, taken into the game 2026-11-17 with the owner's go: his
// reaching, plus ARM SIZING): a hand out of reach swings its shoulder forward about the collarbone's root, the
// collarbone's skin with it (in #24 the shoulder slid toward the hand while its skin stayed), turns and leans the
// torso toward it about the neck (the head and the neck's length stay), and the arm never stretches: what is left is a
// gap between the forearm and the gauntlet, which ARM SIZING closes by lengthening a player's arms to the reach their
// hands show. With a RigState (one per avatar, stepped once a frame) the torso and shoulders move smoothly; without
// one, the rig is the posture the moment asks for.
namespace {
// the torso leans at most 25 degrees (pitch and roll together) about the neck, bending at the waist: the hips stay
// upright under it (Rig::pelvis; leant whole, the hips and thighs swung out sideways: owner, 2026-11-17, the pictures,
// "I don't think that was a correct fix ... why can't we have a lean that is natural")
constexpr float kMaxTilt = 0.4363323f;
constexpr float kWaistY = -0.38f;            // the bend's middle (bind space; the shader blends from -0.50 to -0.26)
constexpr float kMaxProtract = 0.5235988f;   // a collarbone swings forward at most 30 degrees about its root
constexpr float kRigTau = 0.12f;             // the torso's and shoulders' smoothing (s)
constexpr float kTorsoSpeed = 2.0943951f;    // ... and their limits: 120 degrees a second
constexpr float kSlideSpeed = 0.4f;          // ... 40 cm a second
constexpr float kArmSizing = 1.5f;           // ARM SIZING: the arms lengthen toward a longer reach over this (s)
constexpr float kMaxArmScale = 1.35f;        // ... at most 35% longer (the feminine avatar's arms are 47 cm: a
                                             // tall player's reach is 1.3 times that)
// ... and they shorten toward the longest reach the hands have shown lately (the owner, 2026-11-17, her mirror videos:
// arms straight in life, the avatar's elbows bent, and nothing ever shortened them): that reach remembered, fading
// kPeakFall a second once kPeakHold has passed without a reach that long, while a hand is out kPeakOut of the arm's
// length or more (a full reach in play renews it; hands by the chest in a menu don't shorten them), the arms toward it
// over kArmShrink, never under kMinArmScale. (Held 20 s, then 0.5% a second until 2026-11-17: a player's and the
// owner's videos in the mirror, arms fully extended in life and the avatar's elbows bent, long forearms on the
// masculine body in the short-sleeved team kit; one long reach kept the arms long for minutes)
constexpr float kMinArmScale = 0.8f;
constexpr float kPeakHold = 5.f;
constexpr float kPeakFall = 0.03f;
constexpr float kPeakOut = 0.85f;
constexpr float kStraightFrom = 0.96f, kStraightAt = 0.985f;   // FULL REACH, STRAIGHT (solveWith)
constexpr float kUpperShare = 1.3f;   // the upper arm takes this much of ARM SIZING's change (its share of the length), the forearm the rest
constexpr float kArmShrink = 6.f;
constexpr float kTouch = 0.001f;             // an arm that comes within this of its hand reaches it (m)
float smooth01(float x) { x = clampf(x, 0.f, 1.f); return x * x * (3.f - 2.f * x); }

float wrapAngle(float a) {
    while (a > kPi) a -= 2.f * kPi;
    while (a < -kPi) a += 2.f * kPi;
    return a;
}

struct RigFrame {
    const RigTuning* T;
    const BodyBind* K;
    Build B;
    Vec3 scale, neckTop;
    float headHeading = 0, restOffset = 0;   // the head's heading; the torso's at rest, against it (toward the hands)
    bool tracked[2];
    Vec3 W[2];   // where each forearm reaches for: inside the gauntlet's cuff
    float natural[2];   // each arm's length (tuned, not sized)
};

RigFrame frameOf(const PlayerInput& in, const AvatarDesc& d) {
    RigFrame F;
    F.T = &rigTuning();
    F.K = &bodyBind(bodyType(d));
    F.B = buildOf(d.build);
    const RigTuning& T = *F.T;
    const BodyBind& K = *F.K;
    const Pose& h = in.head;
    F.scale = {F.B.width, 1.f, F.B.depth};
    // the head carries the neck top as in the bind pose; the torso hangs below it, so nodding or tilting the head never
    // swings the body
    F.neckTop = h.transform(K.neckTop - K.eye + T.neck);
    // at rest: the head's heading pulled toward the hands (looking aside while throwing keeps the chest on the throw),
    // never more than torsoLimitDeg from where the head looks
    const Vec3 hf = flat(h.q.rotate({0, 0, -1}));
    const Vec3 base0 = F.neckTop + Vec3{0, -K.neckTop.y, 0};
    Vec3 f = hf;
    Vec3 mid{0, 0, 0};
    int nt = 0;
    for (int k = 0; k < 2; ++k)
        if (in.hand[k].tracked) { mid += in.hand[k].grip.p; ++nt; }
    if (nt) {
        Vec3 th = mid / float(nt) - base0;
        th.y = 0;
        if (length(th) > 0.12f) f = normalize(hf + normalize(th) * T.torsoFollow);
        const float c50 = std::cos(deg2rad(T.torsoLimitDeg)), s50 = std::sin(deg2rad(T.torsoLimitDeg));
        if (dot(f, hf) < c50) {
            Vec3 perp = f - hf * dot(f, hf);
            perp = length(perp) > 1e-5f ? normalize(perp) : Vec3{-hf.z, 0, hf.x};
            f = normalize(hf * c50 + perp * s50);
        }
    }
    F.headHeading = std::atan2(-hf.x, -hf.z);
    F.restOffset = wrapAngle(std::atan2(-f.x, -f.z) - F.headHeading);
    for (int k = 0; k < 2; ++k) {
        const float side = k == 1 ? 1.f : -1.f;
        F.tracked[k] = in.hand[k].tracked;
        // the forearm ends inside the gauntlet's bracer or cuff (RigTuning forearmEnd, carried with the cuff when the
        // tuning tool moves it; the classic lancer's wrist ring, tools/blender/make_props.py)
        const Vec3 fe{side * T.forearmEnd.x, T.forearmEnd.y, T.forearmEnd.z};
        F.W[k] = in.hand[k].grip.transform(plateGauntlet(gauntletOf(in.avatar)) ? cuffPose(k == 0).transform(fe) : Vec3{side * 0.004f, -0.042f, 0.105f});
        F.natural[k] = length(K.elbow[k] - K.shoulder[k]) * T.upperLen + length(K.wrist[k] - K.elbow[k]) * T.foreLen;
    }
    return F;
}

// the torso turned by a (x: heading against the head's, y: pitch, z: roll), about the neck
Quat torsoRot(const RigFrame& F, const Vec3& a) {
    return Quat::axisAngle({0, 1, 0}, F.headHeading + a.x) * Quat::axisAngle({0, 0, 1}, a.z) * Quat::axisAngle({1, 0, 0}, a.y);
}
Pose torsoAt(const RigFrame& F, const Vec3& a) {
    const Quat q = torsoRot(F, a);
    return {q, F.neckTop - q.rotate(F.K->neckTop)};
}
Vec3 constrain(const RigFrame& F, Vec3 a) {
    const float lim = deg2rad(F.T->torsoLimitDeg);
    a.x = clampf(a.x, -lim, lim);
    const float tilt = std::sqrt(a.y * a.y + a.z * a.z);
    if (tilt > kMaxTilt) { a.y *= kMaxTilt / tilt; a.z *= kMaxTilt / tilt; }
    return a;
}

// the shortest swing of the shoulder about the collarbone's root toward `wanted`, at most kMaxProtract (a swing,
// never the collarbone sliding)
Quat protraction(const Vec3& root, const Vec3& shoulder, const Vec3& wanted) {
    const Vec3 a = shoulder - root, b = wanted - root;
    if (length2(a) < 1e-8f || length2(b) < 1e-8f) return Quat{};
    const Vec3 na = normalize(a), nb = normalize(b);
    const float angle = std::acos(clampf(dot(na, nb), -1.f, 1.f));
    if (angle < 1e-6f) return Quat{};
    Vec3 axis = cross(na, nb);
    if (length2(axis) < 1e-10f) axis = cross(na, Vec3{0, 1, 0});
    return Quat::axisAngle(normalize(axis), std::min(angle, kMaxProtract));
}

struct Shoulder {
    Vec3 S;        // where the arm hangs from
    Vec3 S0;       // ... before the collarbone's forward swing
    Vec3 wanted;   // the swing the hand asks for (m, toward it)
    Mat4 clav;     // the collarbone's skin (bind -> world)
};

// arm k's shoulder on torso `torso`: lifted with a raised hand (a real one shrugs as the arm goes overhead: the
// collarbone turned up about a point near the neck, its skin with it, build_body_parts.py), then swung forward
// about the collarbone's root by `slide` (or as the hand asks, when null); `total`: the arm's length
Shoulder shoulderOf(const RigFrame& F, int k, const Pose& torso, const Mat4& torsoSkin, float total, const Vec3* slide) {
    const RigTuning& T = *F.T;
    const BodyBind& K = *F.K;
    const float side = k == 1 ? 1.f : -1.f;
    Shoulder o;
    const Vec3 rest = torso.transform({K.shoulder[k].x * F.scale.x + side * T.shoulder.x, K.shoulder[k].y + T.shoulder.y,
                                       K.shoulder[k].z * F.scale.z + T.shoulder.z});
    // e: 0 with the hand level or below, 1 straight up (the torso's up)
    const float e = F.tracked[k] ? clampf(dot(normalize(F.W[k] - rest), torso.q.rotate({0, 1, 0})) * 1.4f, 0.f, 1.f) : 0.f;
    // (the collarbone's root: near the neck; the new bodies bring their own, plan item 22)
    const Vec3 piv{side * 0.05f, K.shoulder[k].y, K.shoulder[k].z};
    const float ang = side * std::asin(clampf(T.shrug * e / (std::fabs(K.shoulder[k].x) - 0.05f), 0.f, 0.8f));
    const Quat cq = Quat::axisAngle({0, 0, 1}, ang);
    const Vec3 Kr = piv + cq.rotate(K.shoulder[k] - piv);
    o.S0 = torso.transform({Kr.x * F.scale.x + side * T.shoulder.x, Kr.y + T.shoulder.y, Kr.z * F.scale.z + T.shoulder.z});
    // a hand beyond reachStart of the arm's length draws the shoulder after it (a real shoulder blade slides), up to
    // T.reach: now a swing about the collarbone's root. As far as brings the hand in: a swing about the root doesn't
    // carry the shoulder straight at the hand, so it swings on by what is still left, within T.reach (started early, at
    // 97% of the arm, it covered that, but held every full reach's elbow bent: a player's video, 2026-11-17)
    const Vec3 root = torso.transform({piv.x * F.scale.x, piv.y, piv.z * F.scale.z});
    o.wanted = {0, 0, 0};
    if (F.tracked[k]) {
        const Vec3 SW = F.W[k] - o.S0;
        const float over = length(SW) - total * T.reachStart;
        if (over > 0) {   // (half a millimetre inside: the forearm then ends on the gauntlet exactly)
            const Vec3 dir = normalize(SW);
            const float inside = total * T.reachStart - 5e-4f;
            float amt = std::min(over + 5e-4f, T.reach);
            for (int it = 0; it < 4 && amt < T.reach; ++it) {
                const Vec3 Sx = root + protraction(root, o.S0, o.S0 + dir * amt).rotate(o.S0 - root);
                const float left = length(F.W[k] - Sx) - inside;
                if (left <= 0.f) break;
                amt = std::min(T.reach, amt + left);
            }
            o.wanted = dir * amt;
        }
    }
    const Quat pr = protraction(root, o.S0, o.S0 + (slide ? *slide : o.wanted));
    o.S = root + pr.rotate(o.S0 - root);
    o.clav = Mat4::fromPose({pr, root - pr.rotate(root)}) * torsoSkin * Mat4::fromPose({cq, piv - cq.rotate(piv)});
    return o;
}

// how far arm k falls short of its hand with the torso at a (0: it reaches)
float shortfall(const RigFrame& F, int k, const Vec3& a, float armScale, const Vec3* slide) {
    if (!F.tracked[k]) return 0.f;
    const Pose torso = torsoAt(F, a);
    const float total = F.natural[k] * armScale;
    const Shoulder s = shoulderOf(F, k, torso, Mat4::fromPose(torso, F.scale), total, slide);
    return std::max(0.f, length(F.W[k] - s.S) - total + 2e-4f);
}

// the torso the moment asks for: at rest, unless a hand is out of reach; then turned and leant the least that brings
// the hands in (both hands take part; an arm that reaches at rest is kept reaching), by a deterministic descent
Vec3 targetPosture(const RigFrame& F, float armScale) {
    Vec3 a = constrain(F, {F.restOffset, 0, 0});
    const float base[2] = {shortfall(F, 0, a, armScale, nullptr), shortfall(F, 1, a, armScale, nullptr)};
    if (base[0] <= kTouch && base[1] <= kTouch) return a;
    const Vec3 rest = a;
    auto cost = [&](const Vec3& p) {
        float c = 0.f;
        for (int k = 0; k < 2; ++k) {
            const float r = shortfall(F, k, p, armScale, nullptr);
            if (base[k] <= kTouch && r > kTouch) return 1000.f + r * r;   // (don't pull a reaching arm away)
            c += r * r;
        }
        return c + 0.00001f * length2(p - rest);
    };
    float best = cost(a);
    for (int it = 0; it < 100; ++it) {
        Vec3 g{};
        const float eps = 0.0002f;
        for (int ax = 0; ax < 3; ++ax) {
            Vec3 plus = a, minus = a;
            (ax == 0 ? plus.x : ax == 1 ? plus.y : plus.z) += eps;
            (ax == 0 ? minus.x : ax == 1 ? minus.y : minus.z) -= eps;
            (ax == 0 ? g.x : ax == 1 ? g.y : g.z) = (cost(constrain(F, plus)) - cost(constrain(F, minus))) / (2.f * eps);
        }
        const float g2 = length2(g);
        if (g2 < 1e-14f) break;
        Vec3 step = g * -std::min(best / g2, 0.05f / std::sqrt(g2));
        bool better = false;
        for (int b = 0; b < 14; ++b) {
            const Vec3 trial = constrain(F, a + step);
            const float v = cost(trial);
            if (v < best - 1e-12f) { a = trial; best = v; better = true; break; }
            step = step * 0.5f;
        }
        if (!better) break;
    }
    return a;
}

// the rig for torso a, the shoulders' swings (null: as the hands ask) and the arms' length
Rig solveWith(const RigFrame& F, const PlayerInput& in, const AvatarDesc& d, const Vec3& a, const Vec3* slides, float armScale) {
    Rig r;
    const RigTuning& T = *F.T;
    const BodyBind& K = *F.K;
    const Build& B = F.B;
    const Quat q = torsoRot(F, a);
    r.torso = {q, F.neckTop - q.rotate(K.neckTop)};
    const Vec3 fwd = q.rotate({0, 0, -1});
    const float neckLen = length(K.neckTop);
    r.neck = {F.neckTop, r.torso.p, fwd, neckLen};
    const Bone bindNeck = {K.neckTop, {0, 0, 0}, {0, 0, -1}, neckLen};
    r.skin[BONE_TORSO] = Mat4::fromPose(r.torso, F.scale);
    r.skin[BONE_HEAD] = Mat4::fromPose(in.head) * Mat4::fromPose({Quat{}, Vec3{0, 0, 0} - K.eye});
    r.skin[BONE_NECK] = boneMatrix(r.neck, false, 1.f) * affineInverse(boneMatrix(bindNeck, false, 1.f));
    r.armScale = armScale;
    {   // the hips: the torso upright (its heading, no lean), hinged to it at the waist
        const TorsoShape& TS = kTorsoShape[bodyType(d)];
        const float wi = clampf((kWaistY - kTorsoY0) / kTorsoDY, 0.f, float(kTorsoSlices - 1));
        const int i0 = int(wi), i1 = std::min(kTorsoSlices - 1, i0 + 1);
        const Vec3 waist{0, kWaistY, TS.cz[i0] + (TS.cz[i1] - TS.cz[i0]) * (wi - float(i0))};
        const Vec3 waistScaled{0, waist.y, waist.z * F.scale.z};
        const Quat up = Quat::axisAngle({0, 1, 0}, F.headHeading + a.x);
        r.pelvis = Mat4::fromPose({up, r.torso.transform(waistScaled) - up.rotate(waistScaled)}, F.scale);
        r.bent = std::fabs(a.y) + std::fabs(a.z) > 1e-4f;
    }
    for (int k = 0; k < 2; ++k) {
        const float side = k == 1 ? 1.f : -1.f;
        // natural (bind) lengths for the meshes; the IK uses the tuned and sized ones (the meshes stretch to match)
        const float L1 = length(K.elbow[k] - K.shoulder[k]), L2 = length(K.wrist[k] - K.elbow[k]);
        // (ARM SIZING's change mostly above the elbow, the reach the same: these avatars' forearms are already as long as
        // their upper arms, a real one about 4/5; lengthened evenly a long arm's forearm read as Popeye's, the owner's
        // video of the masculine body in the short-sleeved team kit, 2026-11-17)
        const float L1u = L1 * T.upperLen, L2f = L2 * T.foreLen, g = armScale - 1.f;
        const float L1t = L1u * (1.f + g * kUpperShare), L2t = L2f * (1.f + g * (1.f - (kUpperShare - 1.f) * L1u / L2f));
        const Bone bu = {K.shoulder[k], K.elbow[k], (K.wrist[k] - K.elbow[k]) + Vec3{0, 0, -0.3f}, L1};
        const Bone bf = {K.elbow[k], K.wrist[k], K.thumb[k] - K.wrist[k], L2};
        const HandInput& hi = in.hand[k];
        r.skin[BONE_CLAV_L + k] = r.skin[BONE_TORSO];   // (the collarbone as bound, unless an arm moves it, below)
        if (!hi.tracked) {   // arms at rest: the bind pose, turned with the torso
            const Vec3 S = r.torso.transform({K.shoulder[k].x * F.scale.x + side * T.shoulder.x, K.shoulder[k].y + T.shoulder.y,
                                              K.shoulder[k].z * F.scale.z + T.shoulder.z});
            r.upper[k] = {S, r.torso.transform(K.elbow[k]), q.rotate(bu.front), L1};
            r.fore[k] = {r.upper[k].b, r.torso.transform(K.wrist[k]), q.rotate(bf.front), L2};
        } else {
            r.arm[k] = true;
            const Shoulder sh = shoulderOf(F, k, r.torso, r.skin[BONE_TORSO], L1t + L2t, slides ? &slides[k] : nullptr);
            r.skin[BONE_CLAV_L + k] = sh.clav;
            const Vec3 S = sh.S, W = F.W[k];
            const Vec3 back = hi.grip.q.rotate(normalize(Vec3{0, -0.047f, 1.f}));
            // elbow: where a straight wrist would put it, nudged down / out / back so the IK stays stable; a hand raised
            // above the shoulder turns it out to the side instead of hanging down (owner's report 2026-10-27: arms
            // raised to the ceiling folded the shoulders in)
            const float e = clampf(dot(normalize(W - sh.S0), q.rotate({0, 1, 0})) * 1.4f, 0.f, 1.f);
            const Vec3 hang = q.rotate({side * T.elbowPole.x, T.elbowPole.y, T.elbowPole.z});
            const Vec3 out = q.rotate({side * 1.0f, 0.15f, 0.35f});
            const Vec3 pole = W + back * (L2t * (1.f - 0.6f * e)) + normalize(lerp(hang, out, e)) * (T.elbowPull + 0.25f * e);
            // never stretched: the forearm ends on the arm's reach; what is left is a gap to the gauntlet (ARM SIZING)
            const Vec3 SW = W - S;
            const float dist = length(SW);
            const Vec3 dir = dist > 1e-5f ? SW / dist : q.rotate({side, 0, 0});
            // FULL REACH, STRAIGHT (a player's video and the owner's, 2026-11-17: arms fully extended in life, the
            // avatar's elbows bent): a hand within kStraightAt of the arm's reach is a full reach, the arm drawn straight,
            // its bones that much shorter (from kStraightFrom, more and more so). An arm sized even 1% long was bent
            // 11 degrees there, and 3% short of its reach a real arm is bent 28: tracking can't tell those apart
            float Lu = L1t, Lf = L2t;
            {
                const float ext = dist / (L1t + L2t);
                if (ext > kStraightFrom && ext < 1.f) {
                    const float f1 = std::min(1.f, (dist + 2.5e-4f) / (L1t + L2t));
                    const float f = 1.f + (f1 - 1.f) * smooth01((ext - kStraightFrom) / (kStraightAt - kStraightFrom));
                    Lu *= f;
                    Lf *= f;
                }
            }
            const Vec3 Wd = S + dir * clampf(dist, std::fabs(Lu - Lf) + 1e-3f, Lu + Lf - 2e-4f);
            r.reachGap[k] = length(W - Wd);
            Vec3 E = elbowIK(S, Wd, Lu, Lf, pole);
            // ELBOWS OUT OF THE BODY (owner, 2026-11-16): swung out round the shoulder-to-wrist line, only as far as
            // keeps the upper arm and forearm out of the torso (hands together at the chest put them inside it)
            const TorsoShape& TS = kTorsoShape[bodyType(d)];
            const float thick = B.arm * T.armThick * T.armClearShare;
            E = elbowClear(TS, r.torso, F.scale, side, S, Wd, E, TS.upperArm * thick, TS.foreArm * thick, T.elbowAllow);
            r.upper[k] = {S, E, (Wd - E) + fwd * 0.3f, L1};   // the inner elbow faces the forearm
            r.fore[k] = {E, Wd, hi.grip.q.rotate({0, 1, 0}), L2};   // thumb side (controller top) is the front
        }
        const bool mir = k == 0;
        r.skin[BONE_UPPER_L + k] = boneMatrix(r.upper[k], mir, B.arm * T.armThick) * affineInverse(boneMatrix(bu, mir, 1.f));
        r.skin[BONE_FORE_L + k] = boneMatrix(r.fore[k], mir, B.arm * T.armThick) * affineInverse(boneMatrix(bf, mir, 1.f));
        // FOREARM TWIST: the forearm as the upper arm carries it (its bind direction and front, turned the least way onto
        // where the forearm points) is the untwisted one; the roll is how far the hand's turns it from that, about the
        // forearm. More than 135 degrees from the hand held thumbs up it fades out (a wrist can't turn that far; at 180 the
        // turn's side is a guess: no snap from one side to the other)
        const Mat4& Mu = r.skin[BONE_UPPER_L + k];
        const Mat4& Mf = r.skin[BONE_FORE_L + k];
        const Vec3 dv = K.wrist[k] - K.elbow[k];
        const Vec3 bfront = bf.front - dv * (dot(bf.front, dv) / std::max(1e-8f, dot(dv, dv)));
        const Vec3 Ew = xfPoint(Mf, K.elbow[k]), dw = xfDir(Mf, dv);
        const float Lw = std::max(1e-4f, length(dw));
        const Vec3 ax = dw / Lw;
        const Vec3 a1 = xfDir(Mu, dv);
        Vec3 f0 = fromTo(normalize(a1), ax).rotate(xfDir(Mu, bfront)), fa = xfDir(Mf, bfront);
        f0 = f0 - ax * dot(f0, ax);
        fa = fa - ax * dot(fa, ax);
        float roll = 0.f;
        if (length2(f0) > 1e-10f && length2(fa) > 1e-10f) roll = std::atan2(dot(cross(f0, fa), ax), dot(f0, fa));
        // (about the hand held thumbs up, where the forearm is turned some 60 degrees out from the bind's: the turns a
        // wrist makes, about 90 degrees either way, stay clear of the fade)
        const float neutral = side * 1.0471976f, rel = wrapAngle(roll - neutral);
        roll = neutral + rel;
        r.roll[k] = roll;
        const float fade = 1.f - smooth01((std::fabs(rel) - 2.356194f) / (kPi - 2.356194f));
        float* t = r.twist;
        const float v[4][4] = {{K.elbow[k].x, K.elbow[k].y, K.elbow[k].z, 1.f / (0.96f * std::max(1e-8f, dot(dv, dv)))},
                               {dv.x, dv.y, dv.z, roll * fade},
                               {Ew.x, Ew.y, Ew.z, 1.f / (0.96f * Lw)},
                               {ax.x, ax.y, ax.z, 0.f}};
        for (int j = 0; j < 4; ++j) std::memcpy(t + (2 * j + k) * 4, v[j], sizeof v[j]);
        if (std::fabs(roll * fade) > 1e-4f) r.twisted = true;
    }
    return r;
}
}  // namespace

Rig solveRig(const PlayerInput& in, const AvatarDesc& d, const RigState* st) {
    const RigFrame F = frameOf(in, d);
    if (st && st->started) {
        const Vec3 a = constrain(F, {wrapAngle(st->torso.x - F.headHeading), st->torso.y, st->torso.z});
        return solveWith(F, in, d, a, st->slide, st->armScale);
    }
    const float scale = st ? st->armScale : 1.f;
    return solveWith(F, in, d, targetPosture(F, scale), nullptr, scale);
}

// ---------------------------------------------------------------- TENNIS SKIRT and KILT
// (PR #25: his swish.mjs, the inspector's) The hem's target is where drag and inertia carry it, against the hips'
// filtered speed and its change, in the garment's frame, at most maxOffset; the twist lags the hips' turn. A damped
// spring follows them (240 steps a second), never past the bounds. A jump (30 cm or 1 rad in a step), a stall
// (0.12 s), a step back in time or another skirt: afresh, at rest.
namespace {
struct SwishStyle { float omega, damping, maxOffset, maxTwist, drag, inertia, turnLag; };
const SwishStyle kSwish[2] = {{11.f, 0.61f, 0.065f, 0.23f, 0.025f, 0.007f, 0.10f},    // the tennis skirt
                              {8.f, 0.72f, 0.05f, 0.17f, 0.020f, 0.006f, 0.12f}};      // the kilt
}  // namespace

int swishKind(const AvatarDesc& d) {   // (PELURU LAYERED DRESS: its skirt sways as the tennis skirt does)
    return d.suit == kSuitDress ? 0 : d.lower == 5 ? 0 : d.lower == 6 ? 1 : -1;
}

void stepSwish(Swish& s, int kind, float px, float pz, float yaw, double now) {
    auto afresh = [&] {
        s = Swish{};
        s.kind = kind;
        s.ready = kind >= 0;
        s.time = now; s.px = px; s.pz = pz; s.yaw = yaw;
    };
    if (kind < 0) { if (s.kind >= 0) s = Swish{}; return; }
    if (!s.ready || s.kind != kind) { afresh(); return; }
    const double dtd = now - s.time;
    if (dtd == 0) return;   // (another draw in the same frame)
    if (dtd < 0 || dtd > 0.12) { afresh(); return; }
    const float dt = (float)dtd;
    const float dx = px - s.px, dz = pz - s.pz, dyaw = wrapAngle(yaw - s.yaw);
    if (std::hypot(dx, dz) > 0.3f || std::fabs(dyaw) > 1.f) { afresh(); return; }
    const SwishStyle& c = kSwish[kind];
    const float alpha = 1.f - std::exp(-dt / 0.045f), ovx = s.fvx, ovz = s.fvz;
    s.fvx += (clampf(dx / dt, -4.f, 4.f) - s.fvx) * alpha;
    s.fvz += (clampf(dz / dt, -4.f, 4.f) - s.fvz) * alpha;
    s.fturn += (clampf(dyaw / dt, -5.f, 5.f) - s.fturn) * alpha;
    const float ax = clampf((s.fvx - ovx) / dt, -12.f, 12.f), az = clampf((s.fvz - ovz) / dt, -12.f, 12.f);
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    float tx = -(cy * s.fvx - sy * s.fvz) * c.drag - (cy * ax - sy * az) * c.inertia;
    float tz = -(sy * s.fvx + cy * s.fvz) * c.drag - (sy * ax + cy * az) * c.inertia;
    const float tr = std::hypot(tx, tz);
    if (tr > c.maxOffset) { tx *= c.maxOffset / tr; tz *= c.maxOffset / tr; }
    const float tt = clampf(-s.fturn * c.turnLag, -c.maxTwist, c.maxTwist);
    const int n = std::max(1, (int)std::ceil(dt * 240.f));
    const float h = dt / (float)n, k = c.omega * c.omega, dd = 2.f * c.damping * c.omega;
    for (int i = 0; i < n; ++i) {
        s.vx += (k * (tx - s.x) - dd * s.vx) * h;
        s.vz += (k * (tz - s.z) - dd * s.vz) * h;
        s.vt += (k * (tt - s.twist) - dd * s.vt) * h;
        s.x += s.vx * h; s.z += s.vz * h; s.twist += s.vt * h;
        const float rr = std::hypot(s.x, s.z);
        if (rr > c.maxOffset) {
            s.x *= c.maxOffset / rr; s.z *= c.maxOffset / rr;
            const float out = (s.vx * s.x + s.vz * s.z) / std::max(rr, 1e-9f);
            if (out > 0) { s.vx -= out * s.x / c.maxOffset; s.vz -= out * s.z / c.maxOffset; }
        }
        if (std::fabs(s.twist) > c.maxTwist) {
            s.twist = clampf(s.twist, -c.maxTwist, c.maxTwist);
            if (s.twist * s.vt > 0) s.vt = 0;
        }
    }
    s.time = now; s.px = px; s.pz = pz; s.yaw = yaw;
}

void stepRig(RigState& st, const PlayerInput& in, const AvatarDesc& d, double now) {
    if (st.started && now - st.time < 0.0005) return;   // (stepped already this frame: the other draws use it)
    const RigFrame F = frameOf(in, d);
    const float dt = st.started ? clampf(float(now - st.time), 0.f, 0.1f) : 0.f;
    const Vec3 target = targetPosture(F, st.armScale);
    Vec3 posture = target;
    if (st.started) {
        // toward what the moment asks, smoothly (frame-rate independent), never faster than kTorsoSpeed
        const Vec3 prev = constrain(F, {wrapAngle(st.torso.x - F.headHeading), st.torso.y, st.torso.z});
        Vec3 delta = (target - prev) * (1.f - std::exp(-dt / kRigTau));
        const float dl = length(delta), maxStep = kTorsoSpeed * dt;
        if (dl > maxStep && dl > 0) delta = delta * (maxStep / dl);
        posture = constrain(F, prev + delta);
        // a hand that the target reaches but the smoothed torso leaves short: as much further toward the target as
        // it takes (contact first), then the whole step bounded again (never a snap)
        auto worst = [&](const Vec3& p) { return std::max(shortfall(F, 0, p, st.armScale, nullptr), shortfall(F, 1, p, st.armScale, nullptr)); };
        if (worst(posture) > kTouch && worst(target) <= kTouch) {
            float lo = 0, hi = 1;
            for (int i = 0; i < 16; ++i) { const float m = (lo + hi) * 0.5f; (worst(lerp(posture, target, m)) <= kTouch ? hi : lo) = m; }
            posture = lerp(posture, target, hi);
        }
        const Quat oldQ = torsoRot(F, prev);
        auto turned = [&](const Vec3& p) {
            const Quat qq = torsoRot(F, p);
            const float c = std::fabs(oldQ.x * qq.x + oldQ.y * qq.y + oldQ.z * qq.z + oldQ.w * qq.w);
            return 2.f * std::acos(clampf(c, 0.f, 1.f));
        };
        if (turned(posture) > maxStep) {
            const Vec3 want = posture;
            float lo = 0, hi = 1;
            for (int i = 0; i < 18; ++i) { const float m = (lo + hi) * 0.5f; (turned(lerp(prev, want, m)) <= maxStep ? lo : hi) = m; }
            posture = lerp(prev, want, lo);
        }
    }
    st.torso = {F.headHeading + posture.x, posture.y, posture.z};
    // each shoulder's swing, the same way (40 cm a second at most, contact first)
    const Pose torso = torsoAt(F, posture);
    stepSwish(st.swish, swishKind(d), torso.p.x, torso.p.z, st.torso.x, now);   // (TENNIS SKIRT / KILT)
    const Mat4 torsoSkin = Mat4::fromPose(torso, F.scale);
    for (int k = 0; k < 2; ++k) {
        const float total = F.natural[k] * st.armScale;
        const Shoulder want = shoulderOf(F, k, torso, torsoSkin, total, nullptr);
        if (!st.started || !F.tracked[k]) { st.slide[k] = want.wanted; continue; }
        auto gap = [&](const Vec3& s) {
            const Shoulder o = shoulderOf(F, k, torso, torsoSkin, total, &s);
            return std::max(0.f, length(F.W[k] - o.S) - total + 2e-4f);
        };
        const float maxStep = kSlideSpeed * dt;
        Vec3 s = st.slide[k] + (want.wanted - st.slide[k]) * (1.f - std::exp(-dt / kRigTau));
        if (length(s - st.slide[k]) > maxStep) s = st.slide[k] + normalize(s - st.slide[k]) * maxStep;
        if (gap(s) > kTouch && gap(want.wanted) <= kTouch) {
            float lo = 0, hi = 1;
            for (int i = 0; i < 16; ++i) { const float m = (lo + hi) * 0.5f; (gap(lerp(s, want.wanted, m)) <= kTouch ? hi : lo) = m; }
            s = lerp(s, want.wanted, hi);
        }
        if (length(s - st.slide[k]) > maxStep) s = st.slide[k] + normalize(s - st.slide[k]) * maxStep;
        if (length(s) > F.T->reach) s = normalize(s) * F.T->reach;
        st.slide[k] = s;
    }
    // ARM SIZING: how far each hand gets from its shoulder on the torso at rest (no turn or lean toward the hand), with
    // the shoulder's own forward swing as the hand asks (a real shoulder blade's, up to T.reach): the player's arm, not
    // what the body does to reach. A reach longer than the arms lengthens them toward it over kArmSizing (a jump faster
    // than a hand moves is tracking, not a reach). So the arms grow only past what the shoulder covers, and the lean is
    // for reaches past those. (From the leant, smoothed shoulder, a quick punch looked longer than it was and longer arms
    // leant less: they grew to the cap; from a shoulder that never swings, every full reach grew them by its swing)
    const Pose restTorso = torsoAt(F, constrain(F, {F.restOffset, 0, 0}));
    const Mat4 restSkin = Mat4::fromPose(restTorso, F.scale);
    float peakNow = 0.f;   // (this frame's longest reach, either hand)
    for (int k = 0; k < 2; ++k) {
        if (!F.tracked[k]) { st.handSeen[k] = false; continue; }
        const Shoulder sh = shoulderOf(F, k, restTorso, restSkin, F.natural[k] * st.armScale, nullptr);
        const float ratio = length(F.W[k] - sh.S) / std::max(0.1f, F.natural[k]);
        const bool jumped = st.handSeen[k] && length(F.W[k] - st.lastW[k]) > 8.f * std::max(dt, 1.f / 90.f);
        // (a hand across the body is no measure of the arm: the real shoulder turns and reaches with it, so from the
        // shoulder at rest it looks further than the arm is long: pointing at a menu across the body grew the arms)
        Vec3 out = sh.S - restTorso.p;
        out.y = 0.f;
        const bool ownSide = length(out) < 1e-4f || dot(F.W[k] - restTorso.p, normalize(out)) > -0.05f;
        if (st.started && dt > 0 && !jumped && ownSide && ratio > st.armScale && ratio < 1.6f)
            st.armScale = std::min(kMaxArmScale, st.armScale + (ratio - st.armScale) * (1.f - std::exp(-dt / kArmSizing)));
        if (!jumped && ownSide && ratio < 1.6f) peakNow = std::max(peakNow, ratio);
        st.lastW[k] = F.W[k];
        st.handSeen[k] = true;
    }
    if (!st.started) {
        st.reachPeak = st.armScale;   // (the arms' own length: nothing shortens them before reaches are seen)
        st.peakAt = now;
    } else if (dt > 0) {
        // (a reach as long as the one remembered: held again. Within 2% until 2026-11-17: the fade stopped 2% above the
        // reach the hands showed, and an arm 2% too long is bent 24 degrees at its full reach)
        if (peakNow >= st.reachPeak - 0.001f) st.peakAt = now;
        // (fading only while a hand is out at arm's length, reaching shorter than remembered: hands by the chest in a
        // menu don't shorten them)
        const float fall = now - st.peakAt > kPeakHold && peakNow >= kPeakOut ? kPeakFall * dt : 0.f;
        st.reachPeak = std::max(peakNow, st.reachPeak - fall);
        const float toward = std::max(kMinArmScale, st.reachPeak);
        if (st.armScale > toward) st.armScale += (toward - st.armScale) * (1.f - std::exp(-dt / kArmShrink));
    }
    st.started = true;
    st.time = now;
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
    AvatarDesc d = uniform(din);   // (sanitized; with a country, the team kit)
    const char bk = bodyType(d) ? 'f' : 'm';
    const int face = faceOf(d);
    char fk[4] = {bk, 0, 0, 0};   // the face's own parts (head, face shields, masks): m / m1 / m2 / m3
    if (face) fk[1] = char('0' + face);
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
    std::snprintf(name, sizeof name, "av_bhead_%s", fk);
    add(name, I, false, PART_SKINNED);
    if (d.suit == kSuitDress) {}   // PELURU LAYERED DRESS: its skirt in the LOWER BODY's place (nothing under it)
    else if (d.lower >= 1 && d.lower <= 4 && (d.suit == 14 || d.suit == 15))   // VECTOR and NIGHTFORGE: their own shorts
        { std::snprintf(name, sizeof name, "av_sbottom_%d_%d_%c", d.suit, d.lower, bk); add(name, I, false, PART_SKINNED); }
    else if (d.lower) { std::snprintf(name, sizeof name, "av_bottom_%d_%c", d.lower, bk); add(name, I, false, PART_SKINNED); }
    else { std::snprintf(name, sizeof name, "av_wfade_%c", bk); add(name, I, false, PART_SKINNED); }
    if (d.headgear) { std::snprintf(name, sizeof name, "av_gear_%d_%c", d.headgear, bk); add(name, head, false); }
    else if (lod && d.hair) { std::snprintf(name, sizeof name, "av_hair_%d_%c", d.hair, bk); add(name, head, false, neonHair(d) ? PART_NEON : 0); }
    if (lod) return;   // miniatures: the silhouette is enough
    if (d.suit >= 10 && d.suit <= 13) {   // CIRCUIT RUNNER and the LIVE suits: the pulses along their raised traces (scrolled by the game)
        std::snprintf(name, sizeof name, "av_tpulse_%d_%c", d.suit, bk);
        add(name, I, false, PART_SKINNED | PART_PULSE);
    }
    // hair under every headgear: the game keeps it inside the helmet, hood or cap, so what shows is what would (a
    // ponytail from under a helmet's rim, a fringe in a hood)
    if (d.hair) {
        const uint8_t hf = uint8_t(PART_HAIR | (neonHair(d) ? PART_NEON : 0));
        std::snprintf(name, sizeof name, "av_hair_%d_%c", d.hair, bk);
        add(name, head, false, hf);
        if (d.hair == 3 || d.hair == 7 || d.hair == 9) {   // PONYTAIL's tail, TOP KNOT's ends, HIGH PIGTAILS: parts of their own (they swing)
            std::snprintf(name, sizeof name, "av_hair_%dt_%c", d.hair, bk);
            add(name, head, false, hf);
        }
    }
    // face shields and masks are fitted to each face (the game falls back to the base head's if one is missing)
    if (d.visor) { std::snprintf(name, sizeof name, "av_visor_%d_%s", d.visor, fk); add(name, head, false); }
    // (the BANDANA's point below the chin swings, its cloth drawn as hair is: av_mask_4_*.swing)
    if (d.mask) { std::snprintf(name, sizeof name, "av_mask_%d_%s", d.mask, fk); add(name, head, false, d.mask == 4 ? PART_HAIR : 0); }
    if (d.mask == 4) {   // BANDANA: its two tails, a part of their own (one per body, every face): they swing as hair does
        std::snprintf(name, sizeof name, "av_mask_4t_%c", bk);
        add(name, head, false, PART_HAIR);
    }
    if (d.suit == kSuitDress) {   // PELURU LAYERED DRESS: its hoop earrings, which swing as hair does
        std::snprintf(name, sizeof name, "av_top_%de_%c", kSuitDress, bk);
        add(name, head, false, PART_HAIR);
    }
}

}  // namespace avatar
