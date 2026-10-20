#include "render.h"
#include "log.h"
#include "platform.h"
#include <cstring>
#include <vector>

// ------------------------------------------------------------------ shaders
static const char* kVS = R"(#version 300 es
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNrm;
layout(location=2) in vec2 aUV0;
layout(location=3) in vec2 aUV1;
layout(location=4) in vec4 aCol;
layout(location=5) in vec4 aBone;
layout(location=6) in vec4 aWeight;
uniform mat4 uMVP;
uniform mat4 uModel;
uniform int uSkinned;
uniform mat4 uBones[7];
uniform int uUV;
uniform vec4 uUVRect;
uniform int uTuck;          // hair under headgear: kept inside it (uTuckTex: per direction from uTuckC, the radius)
uniform vec3 uTuckC;
uniform sampler2D uTuckTex;
out vec3 vN;
out vec3 vW;
out vec2 vUV;
out vec4 vCol;
void main() {
    vec4 p = vec4(aPos, 1.0);
    vec3 nrm = aNrm;
    float pin = 0.0;
    if (uTuck == 1) {   // pulled in (smoothly, from 6 mm short of the radius) and, where the gear covers it, held still
        vec3 d = aPos - uTuckC;
        float r = max(length(d), 1e-4);
        vec2 tc = vec2(atan(d.x, d.z) * 0.1591549 + 0.5, asin(clamp(d.y / r, -1.0, 1.0)) * 0.3183099 + 0.5);
        float lim = 0.05 + 0.5 * textureLod(uTuckTex, tc, 0.0).r;
        float x = r - (lim - 0.006);
        if (x > 0.0) p.xyz = uTuckC + d * ((lim - 0.006 + 0.006 * (1.0 - exp(-x / 0.006))) / r);
        pin = clamp((0.45 - lim) * 4.0, 0.0, 1.0);
    }
    if (uSkinned == 1) {   // linear blend skinning over the avatar's 7 bones
        mat4 S = uBones[int(aBone.x)] * aWeight.x + uBones[int(aBone.y)] * aWeight.y +
                 uBones[int(aBone.z)] * aWeight.z + uBones[int(aBone.w)] * aWeight.w;
        if (uTuck == 1) S = S * (1.0 - pin) + mat4(1.0) * pin;   // (hair rigs: bone 0 is the head)
        p = S * p;
        nrm = mat3(S) * aNrm;
    }
    gl_Position = uMVP * p;
    vW = (uModel * p).xyz;
    vN = mat3(uModel) * nrm;
    vUV = (uUV == 1 ? aUV1 : aUV0) * uUVRect.xy + uUVRect.zw;
    vCol = aCol;
}
)";

// Shader kinds approximate the original UE4 material families on mobile:
//  LIT        textured/tinted lit surface (props, avatars, tools)
//  COURTMASK  court walls: the "brawlCourt_00_lightmap" texture is a channel-packed mask
//             (G = baked ambient light, R/B = per-team glow regions), see the developers' research notes
//  ADDITIVE / TRANSLUCENT / UNLIT for VFX, glass and HUD
static const char* kFS = R"(#version 300 es
precision mediump float;
in vec3 vN;
in vec3 vW;
in vec2 vUV;
in vec4 vCol;
uniform int uShader;
uniform vec4 uTint;
uniform vec4 uEmissive;
uniform int uHasDiffuse;
uniform int uHasMask;
uniform sampler2D uDiffuse;
uniform sampler2D uMask;
uniform vec4 uTeam0;
uniform vec4 uTeam1;
uniform vec3 uEye;
uniform vec3 uLightDir;
uniform int uVColFade;
uniform vec2 uUVOff;
uniform float uErode;
uniform vec4 uRim;
uniform int uPattern;
uniform sampler2D uPatternTex;
uniform vec4 uPatCol;
uniform vec4 uPatGlow;
uniform int uDesign;
uniform vec4 uDesCol;
uniform vec4 uDesGlow;
uniform int uChar;        // avatar parts: character lighting (wrapped key light + a fill from the viewer)
uniform int uCloth;       // garment fabric: knit texture and sheen instead of a glossy highlight
uniform sampler2D uFabric;
out vec4 oColor;
// Stand-in for reflections of the arena: dark below, the bright band of stadium light slats around the horizon,
// cool ceiling glow above.
vec3 arenaEnv(vec3 r) {
    // dark arena with discrete bright light columns around the horizon (the slat galleries) and a dim ceiling
    float band = exp(-pow((r.y - 0.10) / 0.09, 2.0));
    float cols = pow(0.5 + 0.5 * sin(atan(r.z, r.x) * 26.0), 6.0);
    vec3 c = mix(vec3(0.004, 0.006, 0.012), vec3(0.04, 0.055, 0.08), smoothstep(-0.2, 1.0, r.y));
    float sides = smoothstep(0.35, 0.85, abs(r.z));   // the galleries run along the long sides (app z)
    return c + vec3(0.42, 0.47, 0.55) * band * sides * (0.15 + 0.85 * cols);
}
void main() {
    vec3 n = normalize(vN);
    if (!gl_FrontFacing) n = -n;
    vec4 base = uTint;
    if (uHasDiffuse == 1) base *= texture(uDiffuse, vUV);
    if (uShader == 5) {
        vec3 v = normalize(uEye - vW);
        vec3 r = reflect(-v, n);
        float fres = pow(1.0 - max(dot(n, v), 0.0), 5.0);
        vec3 F = uEmissive.rgb + (vec3(1.0) - uEmissive.rgb) * fres * 0.6;
        oColor = vec4(base.rgb + arenaEnv(r) * F * uEmissive.a, base.a);
        return;
    }
    if (uShader == 1) {
        // court layers: albedo baked (sqrt-encoded) into vCol; G = baked light, R/B = team glow; emissive.a = metallic
        vec4 m;
        if (uHasMask == 1) m = texture(uMask, vUV);
        else {   // open assets: baked AO in vertex alpha, team glow from the end zones (|x| 4.6 .. 9.5 m, inside the court)
            float inside = step(abs(vW.z), 2.7) * step(abs(vW.x), 9.7) * step(vW.y, 4.2);
            inside *= abs(vW.x) > 9.4 ? 0.35 : 1.0;   // back walls stay dark, only a hint of team colour
            m = vec4(clamp((-vW.x - 4.6) / 4.9, 0.0, 1.0) * inside, vCol.a, clamp((vW.x - 4.6) / 4.9, 0.0, 1.0) * inside, 1.0);
        }
        vec3 alb = base.rgb * vCol.rgb * vCol.rgb;
        float metal = clamp(uEmissive.a, 0.0, 1.0);
        vec3 v = normalize(uEye - vW);
        vec3 r = reflect(-v, n);
        // cheap stand-in for the dark stadium reflection: deep navy below, cool steel blue toward the ceiling lights
        vec3 env = mix(vec3(0.010, 0.014, 0.026), vec3(0.16, 0.24, 0.34), smoothstep(-0.2, 0.9, r.y));
        float fres = 0.04 + 0.96 * pow(1.0 - max(dot(n, v), 0.0), 5.0);
        vec3 spec = env * mix(vec3(0.04 + fres * 0.3), alb * 1.1 + 0.03, metal);
        vec3 diff = alb * (1.0 - 0.85 * metal) * (0.05 + 0.85 * m.g * m.g);
        vec3 glow = (uTeam0.rgb * m.r * m.r * uTeam0.a + uTeam1.rgb * m.b * m.b * uTeam1.a) * 0.30;
        oColor = vec4(diff + spec + glow + uEmissive.rgb, base.a);
    } else if (uShader == 2) {
        // additive; uErode > 0 dissolves the texture (FadeSubtractMask): only texels brighter than uErode survive
        vec4 tx = uHasDiffuse == 1 ? texture(uDiffuse, vUV + uUVOff) : vec4(1.0);
        float k = 1.0;
        if (uErode > 0.0) {
            float lum = max(tx.r, max(tx.g, tx.b));
            k = clamp((lum - uErode) / max(1e-3, 1.0 - uErode), 0.0, 1.0) / max(lum, 1e-3);
        }
        oColor = vec4(uTint.rgb * tx.rgb * (k * uTint.a) + uEmissive.rgb, 1.0);
    } else if (uShader == 3) {
        float f = uVColFade == 1 ? vCol.a : 1.0;
        if (uVColFade == 1) f *= sin(3.14159 * clamp(vUV.x, 0.0, 1.0));   // ribbons: feathered edges across the width
        oColor = vec4((base.rgb + uEmissive.rgb) * f, base.a * f);
    } else {
        vec3 alb = base.rgb, glowAdd = vec3(0.0);
        if (uDesign == 1) {   // garment design (mask texture): R trim colour, G glowing lines, B white accents
            vec4 dm = texture(uMask, vUV);
            alb = mix(alb, uDesCol.rgb, dm.r);
            alb = mix(alb, vec3(0.80), dm.b);
            glowAdd += uDesGlow.rgb * dm.g;
        }
        alb *= vCol.r;   // baked ambient occlusion (1 on models without it)
        if (uPattern == 1) {   // avatar suit pattern: R = secondary colour regions, G = glowing lines
            vec4 pt = texture(uPatternTex, vUV);
            alb = mix(alb, uPatCol.rgb, pt.r);
            glowAdd += uPatGlow.rgb * pt.g;
        }
        float hemi = 0.5 + 0.5 * n.y;
        float ndl = max(dot(n, -uLightDir), 0.0);
        vec3 v = normalize(uEye - vW);
        vec3 h = normalize(v - uLightDir);
        float nv = max(dot(n, v), 0.0);
        float matte = uEmissive.a < 0.0 ? -uEmissive.a : 0.0;   // negative gloss: matte (hair, cloth)
        float spec = pow(max(dot(n, h), 0.0), 32.0) * 0.25 * (1.0 - matte);
        vec3 c = alb * (0.10 + 0.18 * hemi + 0.80 * ndl) + spec + uEmissive.rgb + glowAdd;
        if (uChar == 1) {
            // the arena's key light is nearly overhead, which leaves the front of a player flat: wrap it round the
            // body and add a soft fill from the viewer so chests, shoulders and folds read in 3D
            float wrap = clamp((dot(n, -uLightDir) + 0.5) / 1.5, 0.0, 1.0);
            float fill = max(dot(n, v), 0.0);
            float diff = 0.10 + 0.12 * hemi + 0.42 * wrap + 0.36 * fill;
            if (uCloth == 1) {
                float fab = texture(uFabric, vUV * 28.0).r;
                alb *= 0.82 + 0.36 * fab;
                float sheen = pow(1.0 - nv, 3.0);
                c = alb * (diff + 0.45 * sheen) + vec3(0.025) * sheen + uEmissive.rgb + glowAdd;
            } else {
                c = alb * diff + spec + uEmissive.rgb + glowAdd;
            }
        }
        if (uEmissive.a > 0.0 && uCloth == 0) {   // gloss: the arena's lights reflected in visors / polished armour
            float fres = 0.04 + 0.96 * pow(1.0 - nv, 5.0);
            c += arenaEnv(reflect(-v, n)) * (0.25 + fres) * (uEmissive.a * 1.5) + pow(max(dot(n, h), 0.0), 96.0) * uEmissive.a * 0.8;
        }
        c += uRim.rgb * (uRim.a * pow(1.0 - nv, 2.5) * (1.0 - 0.75 * matte));   // team-colour rim light (avatars)
        oColor = vec4(c, base.a);
    }
}
)";

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[2048];
        glGetShaderInfoLog(s, sizeof buf, nullptr, buf);
        LOGE("shader compile failed: %s", buf);
    }
    return s;
}

GLuint Renderer::tuckTexture(int w, int h, const uint8_t* v) {
    std::vector<uint8_t> px((size_t)w * h * 4);
    for (int i = 0; i < w * h; ++i) px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = px[i * 4 + 3] = v[i];
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);          // round the head
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);   // straight down .. straight up
    return t;
}

// ------------------------------------------------------------------ asset helpers
GLuint Renderer::loadTexture(const std::string& rel, bool srgb) {
    if (rel.empty()) return 0;
    std::string key = rel + (srgb ? "#s" : "#l");
    auto it = texCache_.find(key);
    if (it != texCache_.end()) return it->second;
    std::string file = "tex/";
    for (char c : rel) file += (c == '/' ? '_' : c);
    file += ".tex";
    std::vector<uint8_t> d;
    GLuint t = 0;
    if (assetRead(file, d) && d.size() > 16 && std::memcmp(d.data(), "STEX", 4) == 0) {
        uint32_t w, h;
        std::memcpy(&w, d.data() + 4, 4);
        std::memcpy(&h, d.data() + 8, 4);
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, d.data() + 16);
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        LOGI("texture %s %ux%u", file.c_str(), w, h);
    } else {
        LOGW("missing texture %s", file.c_str());
    }
    texCache_[key] = t;
    return t;
}

bool Renderer::loadModel(const char* name, Model& m) {
    std::vector<uint8_t> d;
    std::string path = std::string(name) + ".mdl";
    if (!assetRead(path, d) || d.size() < 24 || std::memcmp(d.data(), "SMDL", 4) != 0) {
        LOGE("failed to load model %s", path.c_str());
        return false;
    }
    uint32_t hdr[5];
    std::memcpy(hdr, d.data() + 4, sizeof hdr);
    uint32_t nv = hdr[1], ni = hdr[2], ns = hdr[3], nm = hdr[4];
    const uint8_t* p = d.data() + 24;
    m.skinned = hdr[0] >= 3;
    const size_t vsz = m.skinned ? 52 : 44;
    const uint8_t* verts = p; p += nv * vsz;
    const uint8_t* idx = p; p += ni * 4;
    m.sections.resize(ns);
    std::memcpy(m.sections.data(), p, ns * 12); p += ns * 12;
    m.mats.resize(nm);
    for (uint32_t i = 0; i < nm; ++i) {
        Material& mt = m.mats[i];
        std::memcpy(&mt.shader, p, 4); p += 4;
        std::memcpy(mt.tint, p, 16); p += 16;
        std::memcpy(mt.emissive, p, 16); p += 16;
        std::memcpy(&mt.uv, p, 4); p += 4;
        std::memcpy(&mt.flags, p, 4); p += 4;
        char dn[65] = {}, mn[65] = {};
        std::memcpy(dn, p, 64); p += 64;
        std::memcpy(mn, p, 64); p += 64;
        mt.diffuse = loadTexture(dn, true);
        mt.mask = loadTexture(mn, false);
    }
    m.bmin = {1e9f, 1e9f, 1e9f}; m.bmax = {-1e9f, -1e9f, -1e9f};
    for (uint32_t i = 0; i < nv; ++i) {
        float pos[3]; std::memcpy(pos, verts + i * vsz, 12);
        m.bmin = {std::min(m.bmin.x, pos[0]), std::min(m.bmin.y, pos[1]), std::min(m.bmin.z, pos[2])};
        m.bmax = {std::max(m.bmax.x, pos[0]), std::max(m.bmax.y, pos[1]), std::max(m.bmax.z, pos[2])};
    }
    glGenVertexArrays(1, &m.vao);
    glBindVertexArray(m.vao);
    glGenBuffers(1, &m.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, nv * vsz, verts, GL_STATIC_DRAW);
    glGenBuffers(1, &m.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, ni * 4, idx, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, vsz, (void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, vsz, (void*)12);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, vsz, (void*)24);
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, vsz, (void*)32);
    glEnableVertexAttribArray(4); glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, vsz, (void*)40);
    if (m.skinned) {
        glEnableVertexAttribArray(5); glVertexAttribPointer(5, 4, GL_UNSIGNED_BYTE, GL_FALSE, vsz, (void*)44);
        glEnableVertexAttribArray(6); glVertexAttribPointer(6, 4, GL_UNSIGNED_BYTE, GL_TRUE, vsz, (void*)48);
    }
    glBindVertexArray(0);
    LOGI("model %s: %u verts %u tris %u sections %u mats", name, nv, ni / 3, ns, nm);
    return true;
}

// Blending writes the image's alpha too, as premultiplied coverage, so PASSTHROUGH can show the room where nothing is
// drawn: translucent surfaces add their coverage, additive light adds colour and leaves the coverage alone (glows
// brighten the room behind them). With an opaque background the colours are exactly as before.
static void blendAlpha() { glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); }
static void blendAdditive() { glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE); }

void Renderer::buildPrimitive(Model& m, const std::vector<float>& v, const std::vector<uint32_t>& idx) {
    // v: pos3 nrm3 uv2 per vertex -> expand to the 44-byte layout
    size_t nv = v.size() / 8;
    std::vector<uint8_t> buf(nv * 44, 0);
    for (size_t i = 0; i < nv; ++i) {
        std::memcpy(&buf[i * 44], &v[i * 8], 32);
        std::memcpy(&buf[i * 44 + 32], &v[i * 8 + 6], 8);
        std::memset(&buf[i * 44 + 40], 255, 4);
    }
    glGenVertexArrays(1, &m.vao);
    glBindVertexArray(m.vao);
    glGenBuffers(1, &m.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, buf.size(), buf.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &m.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * 4, idx.data(), GL_STATIC_DRAW);
    const GLsizei s = 44;
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, s, (void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, s, (void*)12);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, s, (void*)24);
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, s, (void*)32);
    glEnableVertexAttribArray(4); glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, s, (void*)40);
    glBindVertexArray(0);
    m.sections = {{0, (uint32_t)idx.size(), 0}};
    m.mats.resize(1);
}

// 5x7 fallback font for ' '..'Z' (LSB = top row), downsampled from our own stroke font (tools/gen_open_textures.py
// font()); used when an asset set has no open_font texture.
static const uint8_t kFont[59][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x7F,0x7F,0x14},
    {0x6E,0x6B,0x7F,0x6B,0x3B},{0x63,0x33,0x1C,0x66,0x63},{0x7E,0x4F,0x5F,0x73,0x70},{0x00,0x00,0x07,0x00,0x00},
    {0x00,0x3E,0x7F,0x41,0x00},{0x00,0x41,0x7F,0x3E,0x00},{0x04,0x1C,0x7E,0x1C,0x04},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x00,0x40,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x00,0x40,0x00,0x00},{0x40,0x70,0x1C,0x07,0x01},
    {0x7F,0x71,0x5D,0x45,0x7F},{0x00,0x43,0x7F,0x40,0x40},{0x43,0x61,0x71,0x59,0x4F},{0x41,0x41,0x4D,0x4F,0x7B},
    {0x38,0x3C,0x36,0x7F,0x30},{0x4F,0x49,0x49,0x49,0x79},{0x7F,0x49,0x49,0x49,0x79},{0x01,0x41,0x71,0x1D,0x07},
    {0x7F,0x49,0x49,0x49,0x7F},{0x4F,0x59,0x59,0x59,0x7F},{0x00,0x00,0x26,0x00,0x00},{0x00,0x00,0x66,0x00,0x00},
    {0x08,0x1C,0x34,0x22,0x62},{0x14,0x14,0x14,0x14,0x14},{0x62,0x22,0x34,0x1C,0x08},{0x03,0x01,0x79,0x09,0x0F},
    {0x7F,0x7D,0x77,0x7D,0x7F},{0x7F,0x09,0x09,0x09,0x7F},{0x7F,0x49,0x49,0x49,0x7F},{0x7F,0x41,0x41,0x41,0x41},
    {0x7F,0x41,0x41,0x41,0x7F},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x7F,0x41,0x49,0x49,0x79},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x60,0x41,0x41,0x41,0x7F},{0x7F,0x1C,0x34,0x62,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x06,0x0C,0x06,0x7F},{0x7F,0x06,0x1C,0x30,0x7F},{0x7F,0x41,0x41,0x41,0x7F},
    {0x7F,0x09,0x09,0x09,0x0F},{0x7F,0x41,0x41,0x61,0x7F},{0x7F,0x09,0x19,0x39,0x6F},{0x4F,0x49,0x49,0x49,0x79},
    {0x01,0x01,0x7F,0x01,0x01},{0x7F,0x40,0x40,0x40,0x7F},{0x0F,0x3C,0x60,0x3C,0x0F},{0x7F,0x78,0x1C,0x78,0x7F},
    {0x63,0x3E,0x1C,0x3E,0x63},{0x03,0x06,0x7C,0x06,0x03},{0x61,0x71,0x5D,0x47,0x43}};
static GLuint g_fontTex = 0;
static GLuint g_softTex[2] = {0, 0};   // gaussian blob, soft ring
static GLint g_uUVRect = -1;

bool Renderer::init() {
    prog_ = glCreateProgram();
    glAttachShader(prog_, compile(GL_VERTEX_SHADER, kVS));
    glAttachShader(prog_, compile(GL_FRAGMENT_SHADER, kFS));
    glLinkProgram(prog_);
    GLint ok = 0;
    glGetProgramiv(prog_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[2048];
        glGetProgramInfoLog(prog_, sizeof buf, nullptr, buf);
        LOGE("program link failed: %s", buf);
        return false;
    }
    uMVP_ = glGetUniformLocation(prog_, "uMVP");
    uModel_ = glGetUniformLocation(prog_, "uModel");
    uShader_ = glGetUniformLocation(prog_, "uShader");
    uTint_ = glGetUniformLocation(prog_, "uTint");
    uEmissive_ = glGetUniformLocation(prog_, "uEmissive");
    uUV_ = glGetUniformLocation(prog_, "uUV");
    uHasDiffuse_ = glGetUniformLocation(prog_, "uHasDiffuse");
    uHasMask_ = glGetUniformLocation(prog_, "uHasMask");
    uTeam0_ = glGetUniformLocation(prog_, "uTeam0");
    uTeam1_ = glGetUniformLocation(prog_, "uTeam1");
    uEye_ = glGetUniformLocation(prog_, "uEye");
    uLightDir_ = glGetUniformLocation(prog_, "uLightDir");
    g_uUVRect = glGetUniformLocation(prog_, "uUVRect");
    uVColFade_ = glGetUniformLocation(prog_, "uVColFade");
    uUVOff_ = glGetUniformLocation(prog_, "uUVOff");
    uErode_ = glGetUniformLocation(prog_, "uErode");
    uRim_ = glGetUniformLocation(prog_, "uRim");
    uPattern_ = glGetUniformLocation(prog_, "uPattern");
    uPatCol_ = glGetUniformLocation(prog_, "uPatCol");
    uPatGlow_ = glGetUniformLocation(prog_, "uPatGlow");
    uSkinned_ = glGetUniformLocation(prog_, "uSkinned");
    uTuck_ = glGetUniformLocation(prog_, "uTuck");
    uTuckC_ = glGetUniformLocation(prog_, "uTuckC");
    uBones_ = glGetUniformLocation(prog_, "uBones");
    uDesign_ = glGetUniformLocation(prog_, "uDesign");
    uDesCol_ = glGetUniformLocation(prog_, "uDesCol");
    uDesGlow_ = glGetUniformLocation(prog_, "uDesGlow");
    glUseProgram(prog_);
    glUniform1i(uVColFade_, 0);
    glUseProgram(prog_);
    glUniform1i(glGetUniformLocation(prog_, "uDiffuse"), 0);
    glUniform1i(glGetUniformLocation(prog_, "uMask"), 1);
    glUniform1i(glGetUniformLocation(prog_, "uPatternTex"), 2);
    glUniform1i(glGetUniformLocation(prog_, "uFabric"), 3);
    glUniform1i(glGetUniformLocation(prog_, "uTuckTex"), 4);
    glUniform1i(uTuck_, 0);
    uChar_ = glGetUniformLocation(prog_, "uChar");
    uCloth_ = glGetUniformLocation(prog_, "uCloth");
    glUniform1i(uPattern_, 0);
    glUniform4f(g_uUVRect, 1, 1, 0, 0);

    // UV sphere
    {
        std::vector<float> v; std::vector<uint32_t> idx;
        const int R = 16, S = 24;
        for (int i = 0; i <= R; ++i)
            for (int j = 0; j <= S; ++j) {
                float th = kPi * i / R, ph = 2 * kPi * j / S;
                Vec3 n{std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph)};
                v.insert(v.end(), {n.x, n.y, n.z, n.x, n.y, n.z, float(j) / S, float(i) / R});
            }
        for (int i = 0; i < R; ++i)
            for (int j = 0; j < S; ++j) {
                uint32_t a = i * (S + 1) + j, b = a + S + 1;
                idx.insert(idx.end(), {a, a + 1, b, b, a + 1, b + 1});
            }
        buildPrimitive(sphere, v, idx);
    }
    // unit cube [-1,1]
    {
        std::vector<float> v; std::vector<uint32_t> idx;
        const Vec3 n[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (int f = 0; f < 6; ++f) {
            Vec3 a = n[f], u = std::fabs(a.y) > 0.5f ? Vec3{1, 0, 0} : Vec3{0, 1, 0}, w = cross(a, u);
            uint32_t base = v.size() / 8;
            for (int k = 0; k < 4; ++k) {
                float su = (k == 1 || k == 2) ? 1.f : -1.f, sw = (k >= 2) ? 1.f : -1.f;
                Vec3 p = a + u * su + w * sw;
                v.insert(v.end(), {p.x, p.y, p.z, a.x, a.y, a.z, (su + 1) / 2, (sw + 1) / 2});
            }
            idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        }
        buildPrimitive(box, v, idx);
    }
    // quad in XY plane [-0.5,0.5], facing +Z, uv (0,0) top-left
    {
        std::vector<float> v = {-0.5f, -0.5f, 0, 0, 0, 1, 0, 1,  0.5f, -0.5f, 0, 0, 0, 1, 1, 1,
                                0.5f, 0.5f, 0, 0, 0, 1, 1, 0,   -0.5f, 0.5f, 0, 0, 0, 1, 0, 0};
        buildPrimitive(quad, v, {0, 1, 2, 0, 2, 3});
    }
    // font atlas: 59 glyphs * 6 px wide, 8 px tall
    {
        const int W = 59 * 6, H = 8;
        std::vector<uint8_t> px(W * H * 4, 0);
        for (int g = 0; g < 59; ++g)
            for (int c = 0; c < 5; ++c)
                for (int r = 0; r < 7; ++r)
                    if (kFont[g][c] & (1 << r)) {
                        uint8_t* q = &px[((r)*W + g * 6 + c) * 4];
                        q[0] = q[1] = q[2] = q[3] = 255;
                    }
        glGenTextures(1, &g_fontTex);
        glBindTexture(GL_TEXTURE_2D, g_fontTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    // the open art set ships an anti-aliased stroke font in the same atlas layout (59 cells, ' '..'Z')
    fabricTex_ = loadTexture("open_fabric", false);   // garments' knit (repeat + mipmaps)
    if (GLuint f = loadTexture("open_font", false)) {
        g_fontTex = f;
        glBindTexture(GL_TEXTURE_2D, f);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    return true;
}

void Renderer::beginEye(const Mat4& view, const Mat4& proj, const Vec3& eye) {
    view_ = view; proj_ = proj; viewProj_ = proj * view; eye_ = eye;
    glUseProgram(prog_);
    glUniform3f(uEye_, eye.x, eye.y, eye.z);
    Vec3 l = normalize(Vec3{0.3f, -1.0f, 0.2f});
    glUniform3f(uLightDir_, l.x, l.y, l.z);
    glUniform4fv(uTeam0_, 1, team0_);
    glUniform4fv(uTeam1_, 1, team1_);
}

void Renderer::setTeamColors(const float c0[4], const float c1[4], float glow0, float glow1) {
    std::memcpy(team0_, c0, 16); team0_[3] = glow0;
    std::memcpy(team1_, c1, 16); team1_[3] = glow1;
}

void Renderer::setSideColors(bool on, const float c0[3], const float c1[3]) {
    sidesOn_ = on;
    for (int k = 0; k < 3; ++k) { side_[0][k] = c0[k]; side_[1][k] = c1[k]; }
}

void Renderer::draw(const Model& m, const Mat4& model, const DrawParams& dp, int pass) {
    if (!m.valid()) return;
    Mat4 mvp = viewProj_ * model;
    glUniformMatrix4fv(uMVP_, 1, GL_FALSE, mvp.m);
    glUniformMatrix4fv(uModel_, 1, GL_FALSE, model.m);
    const bool skin = m.skinned && dp.bones;
    glUniform1i(uSkinned_, skin ? 1 : 0);
    if (skin) glUniformMatrix4fv(uBones_, 7, GL_FALSE, dp.bones[0].m);
    glUniform1i(uTuck_, dp.tuck ? 1 : 0);
    if (dp.tuck) {
        glUniform3fv(uTuckC_, 1, dp.tuckC);
        glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, dp.tuck);
    }
    glBindVertexArray(m.vao);
    glFrontFace(dp.mirrored ? GL_CW : GL_CCW);
    for (const Section& s : m.sections) {
        const Material& mt = m.mats[s.material];
        uint32_t sh = dp.forceShader >= 0 ? uint32_t(dp.forceShader) : mt.shader;
        bool blended = (sh == SH_ADDITIVE || sh == SH_TRANSLUCENT || dp.alphaMul < 0.999f);
        if ((pass == 1) != blended) continue;
        if (sh == SH_ADDITIVE) { glEnable(GL_BLEND); blendAdditive(); glDepthMask(GL_FALSE); }
        else if (blended) { glEnable(GL_BLEND); blendAlpha(); glDepthMask(GL_FALSE); }
        else { glDisable(GL_BLEND); glDepthMask(GL_TRUE); }
        if (mt.flags & MAT_TWOSIDED || blended) glDisable(GL_CULL_FACE); else glEnable(GL_CULL_FACE);
        float tint[4] = {mt.tint[0] * dp.tintMul[0], mt.tint[1] * dp.tintMul[1], mt.tint[2] * dp.tintMul[2], mt.tint[3] * dp.tintMul[3] * dp.alphaMul};
        if (sh == SH_TRANSLUCENT && dp.glassMul != 1.f) tint[3] = std::min(1.f, tint[3] * dp.glassMul);
        float em[4] = {mt.emissive[0] + dp.emissiveAdd[0], mt.emissive[1] + dp.emissiveAdd[1], mt.emissive[2] + dp.emissiveAdd[2], mt.emissive[3]};
        const uint32_t ch = matChannel(mt.flags);
        if (dp.channels && ch && ch < 8) for (int k = 0; k < 3; ++k) tint[k] *= dp.channels[ch][k];
        if (sidesOn_ && (ch == 8 || ch == 9)) {   // a team end in a country's colour, as bright as it was built
            const float k0 = std::max(mt.tint[0], std::max(mt.tint[1], mt.tint[2]));
            for (int k = 0; k < 3; ++k) tint[k] = k0 * side_[ch - 8][k] * dp.tintMul[k];
        }
        const bool pat = dp.channels && dp.pattern && (mt.flags & MAT_PATTERN) && sh == SH_LIT;
        glUniform1i(uPattern_, pat ? 1 : 0);
        const bool design = dp.channels && (mt.flags & MAT_DESIGN) && mt.mask && sh == SH_LIT;
        glUniform1i(uDesign_, design ? 1 : 0);
        const bool chr = dp.rim[3] > 0 && sh == SH_LIT;   // avatar parts (players carry the team rim light)
        const bool cloth = chr && (mt.flags & MAT_CLOTH) && fabricTex_;
        glUniform1i(uChar_, chr ? 1 : 0);
        glUniform1i(uCloth_, cloth ? 1 : 0);
        if (cloth) { glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, fabricTex_); }
        if (design) { glUniform4fv(uDesCol_, 1, dp.channels[2]); glUniform4fv(uDesGlow_, 1, dp.channels[3]); }
        if (pat) {
            float pc[4] = {mt.tint[0] * dp.tintMul[0] * dp.channels[2][0], mt.tint[1] * dp.tintMul[1] * dp.channels[2][1],
                           mt.tint[2] * dp.tintMul[2] * dp.channels[2][2], 1};
            glUniform4fv(uPatCol_, 1, pc);
            glUniform4fv(uPatGlow_, 1, dp.channels[3]);
            glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, dp.pattern);
        }
        glUniform4fv(uRim_, 1, dp.rim);
        glUniform1i(uShader_, int(sh));
        glUniform2f(uUVOff_, dp.uvOff[0], dp.uvOff[1]);
        glUniform1f(uErode_, dp.erode);
        glUniform4fv(uTint_, 1, tint);
        glUniform4fv(uEmissive_, 1, em);
        glUniform1i(uUV_, int(mt.uv));
        glUniform1i(uHasDiffuse_, mt.diffuse ? 1 : 0);
        glUniform1i(uHasMask_, mt.mask ? 1 : 0);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, mt.diffuse ? mt.diffuse : 0);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, mt.mask ? mt.mask : 0);
        glDrawElements(GL_TRIANGLES, s.count, GL_UNSIGNED_INT, (void*)(uintptr_t)(s.first * 4));
        trisDrawn += s.count / 3;
    }
    glBindVertexArray(0);
    glFrontFace(GL_CCW);
    glDepthMask(GL_TRUE);
    glUniform2f(uUVOff_, 0, 0);   // text / ribbons don't go through draw(): leave neutral values behind
    glUniform1i(uSkinned_, 0);
    glUniform1i(uTuck_, 0);
    glUniform1i(uDesign_, 0);
    glUniform1i(uChar_, 0);
    glUniform1i(uCloth_, 0);
    glUniform1f(uErode_, 0);
    glUniform1i(uPattern_, 0);
    glUniform4f(uRim_, 0, 0, 0, 0);
}

static void setColor(Model& m, const float rgba[4], bool additive) {
    m.mats[0].shader = additive ? SH_ADDITIVE : (rgba[3] < 0.999f ? SH_TRANSLUCENT : SH_LIT);
    std::memcpy(m.mats[0].tint, rgba, 16);
}

void Renderer::drawSphere(const Vec3& c, float r, const float rgba[4], bool additive) {
    setColor(sphere, rgba, additive);
    Mat4 mm = Mat4::fromPose({Quat{}, c}, {r, r, r});
    DrawParams dp;
    draw(sphere, mm, dp, (additive || rgba[3] < 0.999f) ? 1 : 0);
}

void Renderer::drawBox(const Mat4& model, const float rgba[4], bool additive) {
    setColor(box, rgba, additive);
    DrawParams dp;
    draw(box, model, dp, (additive || rgba[3] < 0.999f) ? 1 : 0);
}

void Renderer::drawQuad(const Mat4& model, const float rgba[4], bool additive) {
    setColor(quad, rgba, additive);
    DrawParams dp;
    draw(quad, model, dp, (additive || rgba[3] < 0.999f) ? 1 : 0);
}

static GLuint makeSoftTexture(int kind) {
    const int N = 64;
    std::vector<uint8_t> px(N * N * 4);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float u = (x + 0.5f) / N * 2 - 1, v = (y + 0.5f) / N * 2 - 1, r = std::sqrt(u * u + v * v);
            float a = kind == 0 ? std::exp(-r * r * 5.5f) * (1.f - std::min(1.f, r) * 0.15f)
                                : std::exp(-(r - 0.72f) * (r - 0.72f) / (2 * 0.075f * 0.075f));
            if (r > 0.98f) a *= std::max(0.f, (1.f - r) / 0.02f);
            uint8_t c = uint8_t(std::min(255.f, a * 255.f));
            uint8_t* p = &px[(y * N + x) * 4];
            p[0] = p[1] = p[2] = c; p[3] = c;
        }
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, N, N, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

void Renderer::drawSpriteFlat(const Vec3& c, float size, const float rgba[4], const Vec3& n, int tex) {
    if (!g_softTex[0]) { g_softTex[0] = makeSoftTexture(0); g_softTex[1] = makeSoftTexture(1); }
    Quat q = fromTo({0, 0, 1}, normalize(n));
    std::memcpy(quad.mats[0].tint, rgba, 16);
    quad.mats[0].shader = SH_ADDITIVE;
    quad.mats[0].diffuse = g_softTex[tex & 1];
    quad.mats[0].flags = MAT_TWOSIDED;
    DrawParams dp;
    draw(quad, Mat4::fromPose({q, c}, {size, size, 1}), dp, 1);
    quad.mats[0].diffuse = 0;
}

void Renderer::drawSprite(const Vec3& c, float size, const float rgba[4], const Vec3& viewer, int tex, float roll) {
    Vec3 n = viewer - c;
    if (length2(n) < 1e-6f) n = {0, 0, 1};
    if (!g_softTex[0]) { g_softTex[0] = makeSoftTexture(0); g_softTex[1] = makeSoftTexture(1); }
    Quat q = fromTo({0, 0, 1}, normalize(n)) * Quat::axisAngle({0, 0, 1}, roll);
    std::memcpy(quad.mats[0].tint, rgba, 16);
    quad.mats[0].shader = SH_ADDITIVE;
    quad.mats[0].diffuse = g_softTex[tex & 1];
    quad.mats[0].flags = MAT_TWOSIDED;
    DrawParams dp;
    draw(quad, Mat4::fromPose({q, c}, {size, size, 1}), dp, 1);
    quad.mats[0].diffuse = 0;
}

void Renderer::drawPanel(const Mat4& model, const float rgba[4]) {
    setColor(quad, rgba, false);
    quad.mats[0].flags = MAT_TWOSIDED;
    DrawParams dp;
    dp.forceShader = SH_UNLIT;
    dp.alphaMul = 0.998f;   // below the 0.999 threshold: routes through the blended pass
    draw(quad, model, dp, 1);
}

void Renderer::drawText(const std::string& s, const Pose& pose, float height, const float rgba[4]) {
    // Each glyph is one textured quad; string is centred on `pose` (quad faces pose +Z).
    float cw = height * 6.f / 8.f;
    float total = cw * s.size();
    quad.mats[0].shader = SH_UNLIT;
    quad.mats[0].diffuse = g_fontTex;
    quad.mats[0].flags = MAT_TWOSIDED;
    std::memcpy(quad.mats[0].tint, rgba, 16);
    glEnable(GL_BLEND);
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        const bool under = c == '_';   // no '_' cell in the atlas: the dash, dropped to the baseline
        int g = under ? '-' - 32 : (c >= 32 && c <= 'Z') ? c - 32 : 0;
        glUniform4f(g_uUVRect, 6.f / (59 * 6), 1.f, (g * 6.f) / (59 * 6), 0.f);
        Vec3 off{-total * 0.5f + cw * (i + 0.5f), under ? -height * 0.42f : 0.f, 0};
        Pose p{pose.q, pose.transform(off)};
        Mat4 mm = Mat4::fromPose(p, {cw, height, 1});
        DrawParams dp;
        dp.alphaMul = 0.999f;   // route through the blended pass state
        Mat4 mvp = viewProj_ * mm;
        glUniformMatrix4fv(uMVP_, 1, GL_FALSE, mvp.m);
        glUniformMatrix4fv(uModel_, 1, GL_FALSE, mm.m);
        glUniform1i(uShader_, SH_UNLIT);
        glUniform4fv(uTint_, 1, rgba);
        float zero[4] = {0, 0, 0, 0};
        glUniform4fv(uEmissive_, 1, zero);
        glUniform1i(uUV_, 0);
        glUniform1i(uHasDiffuse_, 1);
        glUniform1i(uHasMask_, 0);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, g_fontTex);
        blendAlpha();
        glDisable(GL_CULL_FACE);
        glDepthMask(GL_FALSE);
        glBindVertexArray(quad.vao);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
    }
    glBindVertexArray(0);
    glUniform4f(g_uUVRect, 1, 1, 0, 0);
    quad.mats[0].diffuse = 0;
    glDepthMask(GL_TRUE);
}

void Renderer::drawRibbon(const Vec3* pts, int n, float width, const float rgba[4]) {
    if (n < 2) return;
    if (!ribbonVao_) {
        glGenVertexArrays(1, &ribbonVao_);
        glGenBuffers(1, &ribbonVbo_);
        glBindVertexArray(ribbonVao_);
        glBindBuffer(GL_ARRAY_BUFFER, ribbonVbo_);
        glBufferData(GL_ARRAY_BUFFER, 256 * 2 * 44, nullptr, GL_DYNAMIC_DRAW);
        const GLsizei st = 44;
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, st, (void*)0);
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, st, (void*)12);
        glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, st, (void*)24);
        glEnableVertexAttribArray(3); glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, st, (void*)32);
        glEnableVertexAttribArray(4); glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, (void*)40);
        glBindVertexArray(0);
    }
    n = std::min(n, 256);
    struct V { float p[3], nrm[3], uv0[2], uv1[2]; uint8_t c[4]; };
    static V buf[512];
    for (int i = 0; i < n; ++i) {
        Vec3 d = (i + 1 < n) ? pts[i + 1] - pts[i] : pts[i] - pts[i - 1];
        Vec3 side = normalize(cross(d, eye_ - pts[i]));
        float t = float(i) / float(n - 1);          // 0 = tail, 1 = head
        float w = width * (0.15f + 0.85f * t);
        uint8_t a = uint8_t(255 * t * t);
        for (int k = 0; k < 2; ++k) {
            V& v = buf[i * 2 + k];
            Vec3 q = pts[i] + side * (k ? w : -w);
            v.p[0] = q.x; v.p[1] = q.y; v.p[2] = q.z;
            v.nrm[0] = 0; v.nrm[1] = 1; v.nrm[2] = 0;
            v.uv0[0] = float(k); v.uv0[1] = t; v.uv1[0] = v.uv1[1] = 0;
            v.c[0] = v.c[1] = v.c[2] = 255; v.c[3] = a;
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, ribbonVbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, n * 2 * sizeof(V), buf);
    Mat4 id = Mat4::identity();
    glUniformMatrix4fv(uMVP_, 1, GL_FALSE, viewProj_.m);
    glUniformMatrix4fv(uModel_, 1, GL_FALSE, id.m);
    glUniform1i(uShader_, SH_UNLIT);
    glUniform4fv(uTint_, 1, rgba);
    float zero[4] = {0, 0, 0, 0};
    glUniform4fv(uEmissive_, 1, zero);
    glUniform1i(uUV_, 0);
    glUniform4f(g_uUVRect, 1, 1, 0, 0);   // uv0.x = 0..1 across the ribbon (feathered edges)
    glUniform1i(uHasDiffuse_, 0);
    glUniform1i(uHasMask_, 0);
    glEnable(GL_BLEND);
    blendAdditive();   // additive light
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);
    glUniform1i(uVColFade_, 1);
    glBindVertexArray(ribbonVao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, n * 2);
    glBindVertexArray(0);
    glUniform1i(uVColFade_, 0);
    glDepthMask(GL_TRUE);
}

static const char* kAlphaVS = R"(#version 300 es
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));   // one triangle over the view
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
static const char* kAlphaFS = R"(#version 300 es
precision highp float;
uniform sampler2D uTex;
out vec4 oColor;
vec3 toSrgb(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }
void main() {
    vec4 c = texelFetch(uTex, ivec2(gl_FragCoord.xy), 0);   // linear, premultiplied
    float a = clamp(max(c.a, max(c.r, max(c.g, c.b))), 0.0, 1.0);   // light added over nothing counts as cover
    vec3 s = a > 0.0 ? clamp(c.rgb / a, 0.0, 1.0) : vec3(0.0);
    oColor = vec4(toSrgb(s), a);
}
)";

void Renderer::transparentOut(GLuint srgbTex) {
    if (!alphaProg_) {
        GLuint vs = compile(GL_VERTEX_SHADER, kAlphaVS), fs = compile(GL_FRAGMENT_SHADER, kAlphaFS);
        alphaProg_ = glCreateProgram();
        glAttachShader(alphaProg_, vs);
        glAttachShader(alphaProg_, fs);
        glLinkProgram(alphaProg_);
        GLint ok = 0;
        glGetProgramiv(alphaProg_, GL_LINK_STATUS, &ok);
        if (!ok) LOGE("transparent output: shader link failed");
    }
    glUseProgram(alphaProg_);
    glUniform1i(glGetUniformLocation(alphaProg_, "uTex"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, srgbTex);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(quad.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glUseProgram(prog_);
    glEnable(GL_DEPTH_TEST);
}

void Renderer::drawScreenTexture(GLuint tex, float alpha) {
    if (!tex || alpha <= 0) return;
    Mat4 m = Mat4::identity();
    m.m[0] = 2; m.m[5] = 2; m.m[10] = 0;   // quad [-0.5, 0.5] -> clip [-1, 1], z = 0
    glUseProgram(prog_);
    glUniformMatrix4fv(uMVP_, 1, GL_FALSE, m.m);
    glUniformMatrix4fv(uModel_, 1, GL_FALSE, m.m);
    glUniform1i(uShader_, SH_UNLIT);
    const float tint[4] = {1, 1, 1, alpha}, zero[4] = {0, 0, 0, 0};
    glUniform4fv(uTint_, 1, tint);
    glUniform4fv(uEmissive_, 1, zero);
    glUniform1i(uUV_, 0);
    glUniform4f(g_uUVRect, 1, -1, 0, 1);   // framebuffer rows run bottom-up
    glUniform1i(uHasDiffuse_, 1);
    glUniform1i(uHasMask_, 0);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    blendAlpha();
    glDepthMask(GL_FALSE);
    glBindVertexArray(quad.vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
    glUniform4f(g_uUVRect, 1, 1, 0, 0);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
}

void Renderer::fadeToRoom(float k) {
    if (k <= 0) return;
    Mat4 m = Mat4::identity();
    m.m[0] = 2; m.m[5] = 2; m.m[10] = 0;   // quad [-0.5, 0.5] -> clip [-1, 1], z = 0
    glUseProgram(prog_);
    glUniformMatrix4fv(uMVP_, 1, GL_FALSE, m.m);
    glUniformMatrix4fv(uModel_, 1, GL_FALSE, m.m);
    glUniform1i(uShader_, SH_UNLIT);
    const float tint[4] = {0, 0, 0, std::min(1.f, k)}, zero[4] = {0, 0, 0, 0};
    glUniform4fv(uTint_, 1, tint);
    glUniform4fv(uEmissive_, 1, zero);
    glUniform1i(uUV_, 0);
    glUniform1i(uHasDiffuse_, 0);
    glUniform1i(uHasMask_, 0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);   // premultiplied: scale it all
    glDepthMask(GL_FALSE);
    glBindVertexArray(quad.vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
}

static const char* kDepthVS = R"(#version 300 es
layout(location=0) in vec3 aPos;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vW;
void main() { vec4 p = vec4(aPos, 1.0); vW = (uModel * p).xyz; gl_Position = uMVP * p; }
)";
static const char* kDepthFS = R"(#version 300 es
precision highp float;
precision highp sampler2DArray;
uniform sampler2DArray uDepth;
uniform int uLayer;
uniform mat4 uDepthVP;
uniform mat4 uDepthView;
uniform vec2 uNearFar;
uniform vec4 uCol;
in vec3 vW;
out vec4 oColor;
void main() {
    vec4 c = uDepthVP * vec4(vW, 1.0);
    if (c.w <= 0.0) discard;
    vec2 uv = c.xy / c.w * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) discard;   // outside the depth cameras' view
    // the nearest of 9 samples round the spot (1.5 texels apart): the depth map is coarse, so a real thing's edge
    // gets covered cleanly instead of speckling (a slightly wider dark border round it)
    vec2 px = 1.5 / vec2(textureSize(uDepth, 0).xy);
    float d = 1.0;
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i)
            d = min(d, texture(uDepth, vec3(uv + vec2(float(i), float(j)) * px, float(uLayer))).r);
    float n = uNearFar.x, f = uNearFar.y;
    // depth in [0, 1] to metres along the depth camera's axis (an infinite far plane when farZ is huge)
    float real = f > 1.0e5 ? n / max(1.0e-5, 1.0 - d) : (2.0 * n * f) / (f + n - (2.0 * d - 1.0) * (f - n));
    float glass = -(uDepthView * vec4(vW, 1.0)).z;
    if (real >= glass - 0.10) discard;   // the real surface is beyond the glass: your room shows there
    oColor = uCol;
}
)";

void Renderer::drawDepthBacking(const Mat4* quads, int n, GLuint depthTex, int layer, const Mat4& depthView, const Mat4& depthProj,
                                float nearZ, float farZ, const float rgba[4]) {
    if (!depthTex || n <= 0) return;
    if (!depthProg_) {
        depthProg_ = glCreateProgram();
        glAttachShader(depthProg_, compile(GL_VERTEX_SHADER, kDepthVS));
        glAttachShader(depthProg_, compile(GL_FRAGMENT_SHADER, kDepthFS));
        glLinkProgram(depthProg_);
        GLint ok = 0;
        glGetProgramiv(depthProg_, GL_LINK_STATUS, &ok);
        if (!ok) {
            char buf[2048];
            glGetProgramInfoLog(depthProg_, sizeof buf, nullptr, buf);
            LOGE("depth program link failed: %s", buf);
        }
        dMVP_ = glGetUniformLocation(depthProg_, "uMVP");
        dModel_ = glGetUniformLocation(depthProg_, "uModel");
        dDepthVP_ = glGetUniformLocation(depthProg_, "uDepthVP");
        dDepthView_ = glGetUniformLocation(depthProg_, "uDepthView");
        dNearFar_ = glGetUniformLocation(depthProg_, "uNearFar");
        dLayer_ = glGetUniformLocation(depthProg_, "uLayer");
        dTex_ = glGetUniformLocation(depthProg_, "uDepth");
        dCol_ = glGetUniformLocation(depthProg_, "uCol");
        LOGI("depth backing program %s", ok ? "ready" : "FAILED");
    }
    glUseProgram(depthProg_);
    const Mat4 dvp = depthProj * depthView;
    glUniformMatrix4fv(dDepthVP_, 1, GL_FALSE, dvp.m);
    glUniformMatrix4fv(dDepthView_, 1, GL_FALSE, depthView.m);
    glUniform2f(dNearFar_, nearZ, farZ > 1.0e6f || farZ != farZ ? 1.0e6f : farZ);
    glUniform1i(dLayer_, layer);
    const float col[4] = {rgba[0], rgba[1], rgba[2], 1.f};   // premultiplied, opaque
    glUniform4fv(dCol_, 1, col);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D_ARRAY, depthTex);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glUniform1i(dTex_, 5);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    // under what is already there: dst + (1 - dst coverage) * src
    glBlendFuncSeparate(GL_ONE_MINUS_DST_ALPHA, GL_ONE, GL_ONE_MINUS_DST_ALPHA, GL_ONE);
    glBindVertexArray(quad.vao);
    for (int i = 0; i < n; ++i) {
        const Mat4 mvp = viewProj_ * quads[i];
        glUniformMatrix4fv(dMVP_, 1, GL_FALSE, mvp.m);
        glUniformMatrix4fv(dModel_, 1, GL_FALSE, quads[i].m);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
    }
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    glActiveTexture(GL_TEXTURE0);
    glDepthMask(GL_TRUE);
    glUseProgram(prog_);
}

void Renderer::drawTexturedPanel(const Mat4& model, GLuint tex, const float rgba[4], bool flipV) {
    setColor(quad, rgba, false);
    quad.mats[0].flags = MAT_TWOSIDED;
    quad.mats[0].diffuse = tex;
    DrawParams dp;
    dp.forceShader = SH_UNLIT;
    dp.alphaMul = 0.998f;
    if (flipV) glUniform4f(g_uUVRect, 1, -1, 0, 1);   // render targets are bottom-up
    else glUniform4f(g_uUVRect, 1, 1, 0, 0);
    draw(quad, model, dp, 1);
    glUniform4f(g_uUVRect, 1, 1, 0, 0);
    quad.mats[0].diffuse = 0;
}
