// GLES 3.0 renderer for baked Sparc assets (.mdl / .tex, see tools/build_open_assets.py).
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
// bits 8..11 avatar colour channel (1 primary, 2 secondary, 3 glow, 4 skin, 5 visor, 6 hair, 7 trim; 0 = none)
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
    bool skinned = false;   // v3 model: per-vertex bone indices / weights (avatar bodies)
    bool valid() const { return vao != 0; }
};

struct DrawParams {
    float tintMul[4] = {1, 1, 1, 1};
    float emissiveAdd[4] = {0, 0, 0, 0};
    float alphaMul = 1.f;
    int forceShader = -1;
    bool mirrored = false;   // negative-determinant model matrix: flip front-face winding
    float uvOff[2] = {0, 0};   // additive: UV scroll
    float erode = 0.f;         // additive: dissolve threshold 0..1
    // avatars: per-channel colours (index = material channel, 8 entries; rgb multiply the material tint),
    // a fresnel rim light (rgb * a) and the suit pattern mask (R -> channel 2 colour, G -> channel 3 glow)
    const float (*channels)[4] = nullptr;
    float rim[4] = {0, 0, 0, 0};
    GLuint pattern = 0;
    const Mat4* bones = nullptr;   // skinned models: the 7 avatar bone matrices (bind -> world, before `model`)
};

class Renderer {
public:
    bool init();
    bool loadModel(const char* name, Model& out);
    // pass 0 = opaque, pass 1 = blended
    void draw(const Model& m, const Mat4& model, const DrawParams& dp, int pass);
    void beginEye(const Mat4& view, const Mat4& proj, const Vec3& eyePos);
    void setTeamColors(const float c0[4], const float c1[4], float glow0, float glow1);
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
    // camera-facing additive light ribbon through pts[0..n) (oldest first); width/alpha taper toward the tail
    void drawRibbon(const Vec3* pts, int n, float width, const float rgba[4]);
    GLuint texture(const char* name, bool srgb) { return loadTexture(name, srgb); }   // tex/<name>.tex, cached
    void drawScreenTexture(GLuint tex, float alpha);   // full-view overlay (cross-fades), bottom-up texture
    void drawTexturedPanel(const Mat4& model, GLuint tex, const float rgba[4]);   // a quad showing a render target

    Model sphere, box, quad;

private:
    GLuint loadTexture(const std::string& rel, bool srgb);
    GLuint prog_ = 0;
    GLint uUVOff_ = -1, uErode_ = -1, uRim_ = -1, uPattern_ = -1, uPatCol_ = -1, uPatGlow_ = -1;
    GLint uSkinned_ = -1, uBones_ = -1, uDesign_ = -1, uDesCol_ = -1, uDesGlow_ = -1;
    GLint uChar_ = -1, uCloth_ = -1;
    GLuint fabricTex_ = 0;   // open_fabric: the knit for MAT_CLOTH
    GLint uMVP_, uModel_, uShader_, uTint_, uEmissive_, uUV_, uHasDiffuse_, uHasMask_, uTeam0_, uTeam1_, uEye_, uLightDir_;
    Mat4 view_, proj_, viewProj_;
    Vec3 eye_;
    GLuint ribbonVao_ = 0, ribbonVbo_ = 0;
    GLint uVColFade_ = -1;
    float team0_[4] = {0.1f, 0.7f, 1, 1}, team1_[4] = {1, 0.4f, 0.1f, 1};
    std::map<std::string, GLuint> texCache_;
    void buildPrimitive(Model& m, const std::vector<float>& v, const std::vector<uint32_t>& idx);
};
