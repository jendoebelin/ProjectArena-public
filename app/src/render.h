// GLES 3.0 renderer for Project Arena's baked assets (.mdl / .tex, see tools/build_open_assets.py).
#pragma once
#include "gl.h"
#include <string>
#include <vector>
#include <map>
#include "math3d.h"


// SH_LIGHTMAP: baked (Cycles) diffuse lighting in the diffuse texture (uv1) x tint, plus glossy reflections of the
// arena: emissive.rgb = specular colour F0, emissive.a = gloss strength.
enum ShaderKind : uint32_t { SH_LIT = 0, SH_COURTMASK = 1, SH_ADDITIVE = 2, SH_UNLIT = 3, SH_TRANSLUCENT = 4, SH_LIGHTMAP = 5 };
// material flags: bit 0 two-sided; bit 4 avatar pattern (SH_LIT: the DrawParams pattern mask on uv0);
// bits 8..11 avatar colour channel (1 primary, 2 secondary, 3 glow, 4 skin, 5 visor, 6 hair, 7 trim; 0 = none),
// or a team end's light / glow / glass (8 the blue end, 9 the orange end: NATIONS recolours them, setSideColors)
enum : uint32_t { MAT_TWOSIDED = 1, MAT_PATTERN = 16, MAT_DESIGN = 32, MAT_CLOTH = 64 };   // CLOTH: garment fabric   // DESIGN: mask texture R trim, G glow, B white
inline uint32_t matChannel(uint32_t flags) { return (flags >> 8) & 15u; }
// SH_LIT: emissive.a = gloss (reflections of the arena; visors, polished armour)

struct Material {
    uint32_t shader = SH_LIT;
    float tint[4] = {1, 1, 1, 1};
    float emissive[4] = {0, 0, 0, 0};
    uint32_t uv = 0, flags = 0;
    GLuint diffuse = 0, mask = 0;
};

struct Section { uint32_t first, count, material; };

struct Model {
    GLuint vao = 0, vbo = 0, ibo = 0;
    std::vector<Section> sections;
    std::vector<Material> mats;
    Vec3 bmin, bmax;
    std::vector<Vec3> secMin, secMax;   // each section's bounds (model space; the court's foul lines are found by them)
    bool skinned = false;   // v3 model: per-vertex bone indices / weights (avatar bodies)
    // FAR AWAY (0.3.22; <model>.lod, tools/make_lods.cpp): the same vertices, about a quarter of the triangles, each
    // section's own; DrawParams::distant draws them. lodType: GL_UNSIGNED_SHORT or GL_UNSIGNED_INT; 0: none
    GLuint vaoLod = 0, iboLod = 0;
    std::vector<Section> lodSections;
    GLenum lodType = 0;
    bool valid() const { return vao != 0; }
};

// a skirt's sway (TENNIS SKIRT / KILT, <model>.swish): the body's radii as a texture, the hips' centre and the most a
// vertex moves (c: x, z, max, 0), the rows (p: first row's height, 1 / row step, rows, columns), its height (waist, hem)
struct SwishShape { GLuint tex = 0; float c[4] = {0, 0, 0, 0}, p[4] = {0, 0, 0, 0}; float waistY = 0, hemY = 0; int kind = 0; };

struct DrawParams {
    float tintMul[4] = {1, 1, 1, 1};
    float emissiveAdd[4] = {0, 0, 0, 0};
    float alphaMul = 1.f;
    int forceShader = -1;
    bool mirrored = false;   // negative-determinant model matrix: flip front-face winding
    float glassMul = 1.f;    // translucent (glass) sections' opacity times this (PASSTHROUGH > GLASS WALLS)
    float glowMul = 1.f;     // additive (glowing) sections' brightness times this (the PC spectator's ARENA GLASS)
    float uvOff[2] = {0, 0};   // additive: UV scroll
    float erode = 0.f;         // additive: dissolve threshold 0..1
    uint64_t skip = 0;         // sections (the first 64, by index) not drawn
    int only = -1;             // >= 0: only this section is drawn
    // avatars: per-channel colours (index = material channel, 8 entries; rgb multiply the material tint),
    // a fresnel rim light (rgb * a) and the suit pattern mask (R -> channel 2 colour, G -> channel 3 glow)
    const float (*channels)[4] = nullptr;
    float rim[4] = {0, 0, 0, 0};
    GLuint pattern = 0;
    const Mat4* bones = nullptr;   // skinned models: the 9 avatar bone matrices (avatar.h BONE_*; bind -> world, before `model`)
    // THE REACH: the body bending at the waist: bone 0 (the torso) is `pelvis` below the waist, bones[0] above it, blended
    // over the waist by the bind-space height (avatar.h kWaistLow / kWaistHigh); null: the torso one piece
    const Mat4* pelvis = nullptr;
    const float* twist = nullptr;   // FOREARM TWIST: 8 vec4 (avatar::Rig::twist); null: none
    // TENNIS SKIRT / KILT: the swing (x, z, twist, lift: avatar::Swish) and the garment's shape (its .swish file)
    const float* swish = nullptr;
    const struct SwishShape* swishShape = nullptr;
    // hair under headgear: a tuckTexture() (per direction from tuckC, model space, the radius the hair stays inside)
    GLuint tuck = 0;
    float tuckC[3] = {0, 0, 0};
    // printed on a skinned garment (NATIONS: the flag on the jersey's back and chest): up to two boxes in the model's
    // bind space (decal[i]: bind space -> a unit box, x / y across the print, z through the cloth); the texture is
    // printed where the cloth lies inside a box and faces along its +z, so it bends with the cloth
    int decals = 0;
    Mat4 decal[2];
    GLuint decalTex = 0;
    bool distant = false;   // the model's lighter triangles if it has them (Model::vaoLod: an avatar far away)
};

class Renderer {
public:
    uint64_t trisDrawn = 0;   // triangles submitted through draw() (the game logs it per frame: performance checks)
    bool init();
    bool loadModel(const char* name, Model& out);
    // pass 0 = opaque, pass 1 = blended
    void draw(const Model& m, const Mat4& model, const DrawParams& dp, int pass);
    void beginEye(const Mat4& view, const Mat4& proj, const Vec3& eyePos);
    void setTeamColors(const float c0[4], const float c1[4], float glow0, float glow1);
    // NATIONS: the two ends' colours for the arena's team-marked materials (channels 8 / 9); off = as built
    void setSideColors(bool on, const float c0[3], const float c1[3]);
    // simple geometry
    void drawSphere(const Vec3& c, float r, const float rgba[4], bool additive);
    void drawBox(const Mat4& model, const float rgba[4], bool additive);
    void drawText(const std::string& s, const Pose& pose, float height, const float rgba[4]);   // 7-segment HUD text
    void drawQuad(const Mat4& model, const float rgba[4], bool additive);
    void drawPanel(const Mat4& model, const float rgba[4]);   // unlit, alpha-blended UI quad
    // additive camera-facing glow sprite; tex 0 = soft gaussian blob, 1 = soft ring. Faces `viewer`, rolled by `roll`.
    void drawSprite(const Vec3& c, float size, const float rgba[4], const Vec3& viewer, int tex = 0, float roll = 0.f);
    // flat sprite lying in the plane with normal `n` (e.g. a glow on the floor)
    void drawSpriteFlat(const Vec3& c, float size, const float rgba[4], const Vec3& n, int tex = 0);
    // a soft dark blob lying in the plane with normal `n`, darkening what's under it by up to `alpha` (contact shadows)
    void drawShadowFlat(const Vec3& c, float size, float alpha, const Vec3& n);
    GLuint softTexture(int kind);   // the sprites' soft blob (0) or soft ring (1), for blended marks
    // camera-facing additive light ribbon through pts[0..n) (oldest first); width/alpha taper toward the tail
    void drawRibbon(const Vec3* pts, int n, float width, const float rgba[4], bool taper = true);   // taper: a trail (thin, faint tail)
    GLuint texture(const char* name, bool srgb) { return loadTexture(name, srgb); }   // tex/<name>.tex, cached
    // a hair tuck map (w x h radius bytes, rows from straight down; tools/build_open_assets.py write_tuck) as a texture
    GLuint tuckTexture(int w, int h, const uint8_t* v);
    GLuint swishTexture(int cols, int rows, const float* v);
    void drawScreenTexture(GLuint tex, float alpha);   // full-view overlay (cross-fades), bottom-up texture
    // a finished frame (sRGB texture, premultiplied, transparent behind) to the bound framebuffer as straight alpha for a
    // capture that keeps transparency (OBS Game Capture > Allow Transparency); additive glows become see-through light
    void transparentOut(GLuint srgbTex);
    void fadeToRoom(float k);   // PASSTHROUGH: the whole image (colour and coverage) times 1 - k: the room shows through
    // PASSTHROUGH > WINDOW with environment depth: on the given quads (the court's walls, just outside the glass), fill
    // in rgba *under* what is drawn wherever the real surface there (depth texture array, layer = eye) is nearer
    // than the quad: things in your room inside the court stop showing through the glass
    void drawDepthBacking(const Mat4* quads, int n, GLuint depthTex, int layer, const Mat4& depthView, const Mat4& depthProj,
                          float nearZ, float farZ, const float rgba[4]);
    // a quad showing a texture: flipV for render targets (bottom-up), false for loaded images
    // uv: a part of the texture (scale u, scale v, offset u, offset v; for loaded images), e.g. one strip of a flag
    void drawTexturedPanel(const Mat4& model, GLuint tex, const float rgba[4], bool flipV = true, const float* uv = nullptr);

    // GRAPHICS (0.3.22). COLOR GRADING: the filmic grade at the end of every surface's shading, from the next beginEye
    void setGrade(bool on) { grade_ = on; }
    // THE CUTAWAY (the PC spectator's preset cameras, 2026-11-19): until set again with n = 0, what the main shader draws
    // within w of any of n planes (n.xyz, d: the points where dot(n, p) = d; up to 4) is left out, where (y - y0) * sign
    // > 0 (sign -1: below y0, the floor's mirror image); roof (null: none): its height, footprint and shape (as uRoof in
    // the shader), what lies within w of it inside the footprint left out too. Nothing is left out while n = 0 and no
    // roof (the default)
    void setCut(int n, const float (*planes)[4], float y0, float sign, float w, const float* roof = nullptr);
    const Vec3& eyePos() const { return eye_; }
    // FAR AWAY off for everything (pictures: --nofar, to compare), or on (the default)
    void setFarOff(bool off) { lodOff_ = off; }
    // a mesh of the game's own making (v: position, normal, uv a vertex; one section, one material: set m.mats[0])
    void buildMesh(Model& m, const std::vector<float>& v, const std::vector<uint32_t>& idx) { buildPrimitive(m, v, idx); }
    // BLOOM: the bright parts of a finished frame (`src`, an sRGB texture w x h) blurred and their light added to
    // `dst` (a framebuffer holding that frame); `overAlpha`: only over what is drawn (passthrough's room, a keyed
    // background). Leaves `dst` bound, the viewport the frame's
    void bloom(GLuint src, int w, int h, GLuint dst, float threshold, float strength, bool overAlpha);
    // AMBIENT OCCLUSION and VIGNETTE (the PC spectator): `dst` (holding the frame `col`, its depth `depth`) darkened
    // where the depth says it is enclosed (ao: how much, 0 none; px, py the projection's x and y scale) and toward the
    // edges (vignette: how much at the corners). Leaves `dst` bound, the viewport the frame's
    void darken(GLuint col, GLuint depth, int w, int h, GLuint dst, float px, float py, float nearZ, float farZ, float ao, float vignette);

    Model sphere, box, quad, cylinder;   // cylinder: radius 1, from y = -1 up to its top at y = 0 (the experimental platforms)

private:
    GLuint loadTexture(const std::string& rel, bool srgb);
    GLuint prog_ = 0;
    GLuint depthProg_ = 0, alphaProg_ = 0;
    bool sidesOn_ = false;
    float side_[2][3] = {{0.1f, 0.7f, 1.0f}, {1.0f, 0.4f, 0.1f}};
    GLint dMVP_ = -1, dModel_ = -1, dDepthVP_ = -1, dDepthView_ = -1, dNearFar_ = -1, dLayer_ = -1, dTex_ = -1, dCol_ = -1;
    GLint uUVOff_ = -1, uErode_ = -1, uRim_ = -1, uPattern_ = -1, uPatCol_ = -1, uPatGlow_ = -1;
    GLint uSkinned_ = -1, uBones_ = -1, uBend_ = -1, uPelvis_ = -1, uTwist_ = -1, uTw_ = -1, uSwish_ = -1, uSw_ = -1, uSwC_ = -1, uSwP_ = -1, uDesign_ = -1, uDesCol_ = -1, uDesGlow_ = -1;
    GLint uChar_ = -1, uCloth_ = -1, uTuck_ = -1, uTuckC_ = -1, uDecals_ = -1, uDecalM_ = -1;
    GLuint fabricTex_ = 0;   // open_fabric: the knit for MAT_CLOTH
    GLint uMVP_, uModel_, uShader_, uTint_, uEmissive_, uUV_, uHasDiffuse_, uHasMask_, uTeam0_, uTeam1_, uEye_, uLightDir_;
    Mat4 view_, proj_, viewProj_;
    Vec3 eye_;
    GLuint ribbonVao_ = 0, ribbonVbo_ = 0;
    GLint uVColFade_ = -1;
    float team0_[4] = {0.1f, 0.7f, 1, 1}, team1_[4] = {1, 0.4f, 0.1f, 1};
    std::map<std::string, GLuint> texCache_;
    void buildPrimitive(Model& m, const std::vector<float>& v, const std::vector<uint32_t>& idx);
    // GRAPHICS' passes (their programs made when first used) and their pictures
    enum { PP_BLOOM_PRE, PP_BLOOM_DOWN, PP_BLOOM_UP, PP_BLOOM_OUT, PP_AO, PP_AO_BLUR, PP_DARKEN, kPostProgs };
    static constexpr int kBloomLevels = 5;   // (a quarter of the frame, then halved four times)
    GLuint postProgram(int k);
    void samplers();
    GLuint postProg_[kPostProgs] = {};
    GLuint bloomTex_[kBloomLevels] = {}, bloomFbo_[kBloomLevels] = {};
    int bloomLW_[kBloomLevels] = {}, bloomLH_[kBloomLevels] = {};
    int bloomW_ = 0, bloomH_ = 0;
    GLuint aoTex_[2] = {}, aoFbo_[2] = {};
    int aoW_ = 0, aoH_ = 0;
    GLuint linear_ = 0, nearest_ = 0;
    bool grade_ = false, lodOff_ = false;
    GLint uGrade_ = -1, uCutN_ = -1, uCut_ = -1, uCutY_ = -1, uRoof_ = -1;
};
