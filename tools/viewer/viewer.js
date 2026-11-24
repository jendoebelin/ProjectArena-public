// Project Arena asset viewer (tools/gen_viewer.py puts this inside tools/viewer/index.html).
// The game's own files (models/*.mdl.gz, tex/*.png, snd/*.wav, data.json), drawn with a copy of the game's shader
// (app/src/render.cpp), its part selection (app/src/avatar.cpp drawList) and its rig (tools/avatar_pose.cpp, in data.json).
import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { EffectComposer } from 'three/addons/postprocessing/EffectComposer.js';
import { RenderPass } from 'three/addons/postprocessing/RenderPass.js';
import { OutputPass } from 'three/addons/postprocessing/OutputPass.js';

const $ = (id) => document.getElementById(id);
const SH = { LIT: 0, COURT: 1, ADD: 2, UNLIT: 3, TRANS: 4, GLASS: 5 };
const MAT_TWOSIDED = 1, MAT_PATTERN = 16, MAT_DESIGN = 32, MAT_CLOTH = 64;
const CHANNEL = ['none', 'main colour', 'trim colour', 'glow', 'skin', 'shield tint', 'hair', 'metal trim'];
const CHANNEL_SHOW = [[0.30, 0.30, 0.32], [0.05, 0.25, 0.95], [0.95, 0.35, 0.05], [0.10, 0.95, 0.95], [0.95, 0.55, 0.60],
  [0.55, 0.20, 0.85], [0.35, 0.18, 0.05], [0.95, 0.85, 0.15]];
const PATTERNS = ['', 'circuit', 'stripes', 'chevron', 'hex', 'split', 'pixel', 'bands'];
// (as the game's CUSTOMIZE AVATAR, 0.3.22: the body and country first, then the parts from head to toe)
const FIELD_ORDER = ['BODY TYPE', 'BUILD', 'COUNTRY', 'HEADGEAR', 'HAIR', 'FACE', 'FACE SHIELD', 'MASK', 'SUIT', 'PATTERN', 'GAUNTLET',
  'LOWER BODY'];
const COLOR_FIELDS = [['MAIN COLOR', 'primary'], ['TRIM COLOR', 'secondary'], ['GLOW', 'glow'], ['HAIR COLOR', 'haircolor'],
  ['SHIELD TINT', 'visortint'], ['SKIN', 'skin']];
const KEY = { 'BODY TYPE': 'body', FACE: 'face', SUIT: 'suit', BUILD: 'build', 'LOWER BODY': 'lower', GAUNTLET: 'gauntlet',
  PATTERN: 'pattern', HAIR: 'hair', HEADGEAR: 'headgear', 'FACE SHIELD': 'visor', MASK: 'mask', COUNTRY: 'nation' };

let D = null;                      // data.json
const state = { section: 'avatar', look: null, pose: 'ready', team: 0, surface: 0, wire: false, ghost: false, bones: false,
  grid: true, item: null, focus: 'full', animRun: null };
// (0.3.26) ANIMATIONS: the practice bot at ease, its taunts, celebrations and victory poses, and the skirts' swing
// (tools/bot_anims.cpp: the game's own bot and rig), a file a body and build (anims/anims_<body><build>.json, 12 frames a
// second, blended); `live`: the avatar's meshes each frame moves
const animFiles = {};
let live = null;
async function animsFor(combo) {
  if (!animFiles[combo]) animFiles[combo] = fetch(`anims/anims_${combo}.json`).then((r) => { if (!r.ok) throw new Error(r.status); return r.json(); });
  return animFiles[combo];
}
// a kept frame as the rig the page draws with: its 3 x 4 matrices made whole, each hand's gauntlet frames from its grip,
// fist and cuff as gen_viewer.py rig_data puts them
function m34(a, o) {
  return [a[o], a[o + 1], a[o + 2], 0, a[o + 3], a[o + 4], a[o + 5], 0, a[o + 6], a[o + 7], a[o + 8], 0, a[o + 9], a[o + 10], a[o + 11], 1];
}
const MIRL = [-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
function mulA(a, b) { return new THREE.Matrix4().fromArray(a).multiply(new THREE.Matrix4().fromArray(b)).toArray(); }
function rigOfFrame(fr) {
  const skin = [];
  for (let b = 0; b < 9; b++) skin.push(m34(fr.s, b * 12));
  const L = D.handFrames.lancer, K = D.handFrames.knuckle, hands = [];
  for (let h = 0; h < 2; h++) {
    const G = m34(fr.g, h * 36), F = m34(fr.g, h * 36 + 12), C = m34(fr.g, h * 36 + 24);
    const Gm = h === 0 ? mulA(G, MIRL) : G;
    hands.push({ fist: mulA(F, L), cuff: mulA(C, L), lancer: mulA(Gm, L), knuckle: mulA(Gm, K) });
  }
  const r = { skin, head: m34(fr.h, 0), hands, bones: null };
  if (fr.p) r.pelvis = m34(fr.p, 0);
  if (fr.t) r.twist = fr.t;
  return r;
}
function blendFrame(a, b, t) {
  const mix = (x, y) => (x && y ? x.map((v, i) => v + (y[i] - v) * t) : (t < 0.5 ? x : y));
  return { s: mix(a.s, b.s), h: mix(a.h, b.h), g: mix(a.g, b.g), w: mix(a.w, b.w), p: mix(a.p, b.p), t: mix(a.t, b.t),
    x: mix(a.x, b.x), xs: a.xs !== undefined && b.xs !== undefined ? a.xs + (b.xs - a.xs) * t : (t < 0.5 ? a.xs : b.xs) };
}
// a skirt's swing for the page's copy of the shader's (render.cpp uSwish): its shape, and this frame's swing for its kind
// (0 the tennis skirt and the dress, 1 the kilt), the hem lifted as it swings out (main.cpp); none moving: still, but
// kept out of the body as the game keeps it
function swingFor(model, fr) {
  const sh = D.swish && D.swish[model];
  if (!sh) return null;
  const k = sh.kind ? 3 : 0;
  const x = fr && fr.w ? fr.w[k] : 0, z = fr && fr.w ? fr.w[k + 1] : 0, tw = fr && fr.w ? fr.w[k + 2] : 0;
  const len = Math.max(0.1, (sh.waist - sh.hem) * 0.6);
  return { x, z, tw, lift: Math.min(0.018, (x * x + z * z) / (2 * len)), sh };
}
function swRadius(sh, y, th) {   // the body's radius at this height and angle (render.cpp swishRadius)
  const rows = sh.p[2], cols = sh.p[3], R = sh.r;
  const fy = Math.min(rows - 1, Math.max(0, (y - sh.p[0]) * sh.p[1]));
  const lo = Math.floor(fy), hi = Math.min(lo + 1, rows - 1);
  const uu = th * 0.1591549 + 1, u = (uu - Math.floor(uu)) * cols;
  const i = Math.floor(u) % cols, j = (i + 1) % cols, f = u - Math.floor(u);
  const ra = R[lo * cols + i] + (R[lo * cols + j] - R[lo * cols + i]) * f;
  const rb = R[hi * cols + i] + (R[hi * cols + j] - R[hi * cols + i]) * f;
  return ra + (rb - ra) * (fy - lo);
}
// AIR GUITAR's guitar (0.3.26; Game::botGuitar, main.cpp): a hologram in the avatar's glow colour, where the frame has it
function placeProp(fr) {
  if (!live || !live.prop) return;
  live.prop.visible = !!(fr && fr.x);
  if (fr && fr.x) live.prop.matrix.fromArray(m34(fr.x, 0));
}
// each frame of an animation: the frame blended, the skinned parts skinned again, the head and hands moved
let animLast = 0;
function animStep(now) {
  const A = state.animRun;
  if (!A || !live || now - animLast < 30) return false;
  animLast = now;
  const n = A.frames.length, f = (((now - A.start) / 1000) * D.anims.fps) % n;
  const i0 = Math.floor(f), fr = blendFrame(A.frames[i0], A.frames[(i0 + 1) % n], f - i0);
  const rig = rigOfFrame(fr);
  for (const p of live.skinned) {
    const [pos, nrm] = skin(p.M, rig, swingFor(p.model, fr));
    const g = p.mesh.geometry;
    g.attributes.position.array.set(pos); g.attributes.position.needsUpdate = true;
    g.attributes.normal.array.set(nrm); g.attributes.normal.needsUpdate = true;
  }
  for (const m of live.heads) m.matrix.fromArray(rig.head);
  for (const h of live.hands) h.mesh.matrix.fromArray(rig.hands[h.hand][h.slot]);
  placeProp(fr);
  return true;
}

// ---------------------------------------------------------------- loading the game's files
async function gunzip(buf) {
  const b = new Uint8Array(buf);
  if (b[0] !== 0x1f || b[1] !== 0x8b) return buf;   // (the host may have unpacked it already)
  const s = new Blob([buf]).stream().pipeThrough(new DecompressionStream('gzip'));
  return await new Response(s).arrayBuffer();
}

function parseMdl(buf) {
  const dv = new DataView(buf);
  const ver = dv.getUint32(4, true), nv = dv.getUint32(8, true), ni = dv.getUint32(12, true);
  const ns = dv.getUint32(16, true), nm = dv.getUint32(20, true);
  const stride = ver >= 3 ? 52 : 44;
  const pos = new Float32Array(nv * 3), nrm = new Float32Array(nv * 3), uv0 = new Float32Array(nv * 2), uv1 = new Float32Array(nv * 2);
  const col = new Uint8Array(nv * 4), bi = new Uint8Array(nv * 4), bw = new Uint8Array(nv * 4);
  let o = 24;
  for (let i = 0; i < nv; i++, o += stride) {
    for (let k = 0; k < 3; k++) { pos[i * 3 + k] = dv.getFloat32(o + k * 4, true); nrm[i * 3 + k] = dv.getFloat32(o + 12 + k * 4, true); }
    uv0[i * 2] = dv.getFloat32(o + 24, true); uv0[i * 2 + 1] = dv.getFloat32(o + 28, true);
    uv1[i * 2] = dv.getFloat32(o + 32, true); uv1[i * 2 + 1] = dv.getFloat32(o + 36, true);
    for (let k = 0; k < 4; k++) col[i * 4 + k] = dv.getUint8(o + 40 + k);
    if (ver >= 3) for (let k = 0; k < 4; k++) { bi[i * 4 + k] = dv.getUint8(o + 44 + k); bw[i * 4 + k] = dv.getUint8(o + 48 + k); }
  }
  const idx = new Uint32Array(buf.slice(o, o + ni * 4));
  o += ni * 4;
  const secs = [];
  for (let s = 0; s < ns; s++, o += 12) secs.push([dv.getUint32(o, true), dv.getUint32(o + 4, true), dv.getUint32(o + 8, true)]);
  const str = (at) => { let t = ''; for (let k = 0; k < 64; k++) { const c = dv.getUint8(at + k); if (!c) break; t += String.fromCharCode(c); } return t; };
  const mats = [];
  for (let m = 0; m < nm; m++) {
    const f = (k) => dv.getFloat32(o + 4 + k * 4, true);
    mats.push({ shader: dv.getUint32(o, true), tint: [f(0), f(1), f(2), f(3)], em: [f(4), f(5), f(6), f(7)],
      uv: dv.getUint32(o + 36, true), flags: dv.getUint32(o + 40, true), diffuse: str(o + 44).replace(/\//g, '_'), mask: str(o + 108).replace(/\//g, '_') });
    o += 44 + 128;
  }
  return { ver, nv, pos, nrm, uv0, uv1, col, bi, bw, idx, secs, mats };
}

const mdlCache = new Map(), bundleCache = new Map();
function fromBase64(t) {
  const bin = atob(t.trim()), b = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) b[i] = bin.charCodeAt(i);
  return b.buffer;
}
function loadModel(name) {
  if (!mdlCache.has(name)) {
    let got;
    if (D.modelBundle) {   // (base64 of the gzipped files, a few to a bundle: for hosts that serve only web types and few files)
      const k = D.modelBundle[name];
      if (k === undefined) return Promise.reject(new Error(name + ': not in the viewer'));
      if (!bundleCache.has(k)) bundleCache.set(k, fetch(`models/bundle_${k}.json`).then((r) => {
        if (!r.ok) throw new Error('bundle ' + k + ': ' + r.status);
        return r.json();
      }));
      got = bundleCache.get(k).then((B) => fromBase64(B[name]));
    } else {
      got = fetch('models/' + name + (D.modelExt || '.mdl.gz')).then((r) => {
        if (!r.ok) throw new Error(name + ': ' + r.status);
        return r.arrayBuffer();
      });
    }
    mdlCache.set(name, got.then(gunzip).then(parseMdl));
  }
  return mdlCache.get(name);
}

const texLoader = new THREE.TextureLoader();
const texCache = new Map();
const white = new THREE.DataTexture(new Uint8Array([255, 255, 255, 255]), 1, 1);
white.needsUpdate = true;
function tex(name, srgb) {
  if (!name || !D.textures[name]) return null;
  const key = name + (srgb ? '#s' : '#l');
  if (!texCache.has(key)) {
    const t = texLoader.load('tex/' + name + '.png', () => { dirty = true; });
    t.flipY = false;   // (the game uploads row 0 at v = 0)
    t.colorSpace = srgb ? THREE.SRGBColorSpace : THREE.NoColorSpace;
    t.wrapS = t.wrapT = THREE.RepeatWrapping;
    t.anisotropy = 4;
    texCache.set(key, t);
  }
  return texCache.get(key);
}

// ---------------------------------------------------------------- the game's shader (render.cpp kFS), for three.js
const VS = `
in vec2 aUV0;
in vec2 aUV1;
in vec4 aCol;
in vec3 aDec0;   // (NATIONS: where the point lies in each print's box, bind space: worked out before skinning, as the game)
in vec3 aDec1;
in vec2 aDecN;
uniform int uUV;
out vec3 vN;
out vec3 vW;
out vec2 vUV;
out vec4 vCol;
out vec3 vDec0;
out vec3 vDec1;
out vec2 vDecN;
void main() {
  vec4 w = modelMatrix * vec4(position, 1.0);
  vW = w.xyz;
  vN = mat3(modelMatrix) * normal;
  vUV = uUV == 1 ? aUV1 : aUV0;
  vCol = aCol;
  vDec0 = aDec0; vDec1 = aDec1; vDecN = aDecN;
  gl_Position = projectionMatrix * viewMatrix * w;
}`;
const FS = `
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
uniform vec3 uLightDir;
uniform vec4 uRim;
uniform int uPattern;
uniform sampler2D uPatternTex;
uniform vec4 uPatCol;
uniform vec4 uPatGlow;
uniform int uDesign;
uniform vec4 uDesCol;
uniform vec4 uDesGlow;
uniform int uChar;
uniform int uCloth;
uniform sampler2D uFabric;
uniform int uMode;      // the viewer's SURFACE: 0 the game's look, 1 neutral clay, 2 colour channels
uniform vec3 uFlat;     // (2: this material's channel colour)
uniform int uDecals;    // NATIONS: the flags printed on the jersey (two: the back, the chest)
uniform sampler2D uDecalTex;
in vec3 vDec0;
in vec3 vDec1;
in vec2 vDecN;
out vec4 oColor;
vec3 printed(vec3 alb, vec3 d, float facing, inout float cover) {   // a print where this point lies in its box, facing out
  if (facing <= 0.0 || abs(d.x) > 0.5 || abs(d.y) > 0.5 || abs(d.z) > 0.5) return alb;   // (render.cpp)
  vec4 t = texture(uDecalTex, vec2(d.x + 0.5, 0.5 - d.y));
  cover = max(cover, t.a);
  return mix(alb, t.rgb, t.a);
}
float printCover() {   // COUNTRY KIT: under a print a suit's own glow gives way to it (its glowing lines and pulses)
  float a = 0.0;
  vec3 x = vec3(0.0);
  if (uDecals > 0) x = printed(x, vDec0, vDecN.x, a);
  if (uDecals > 1) x = printed(x, vDec1, vDecN.y, a);
  return a;
}
vec3 arenaEnv(vec3 r) {
  float band = exp(-pow((r.y - 0.10) / 0.09, 2.0));
  float cols = pow(0.5 + 0.5 * sin(atan(r.z, r.x) * 26.0), 6.0);
  vec3 c = mix(vec3(0.004, 0.006, 0.012), vec3(0.04, 0.055, 0.08), smoothstep(-0.2, 1.0, r.y));
  float sides = smoothstep(0.35, 0.85, abs(r.z));
  return c + vec3(0.42, 0.47, 0.55) * band * sides * (0.15 + 0.85 * cols);
}
void main() {
  vec3 n = normalize(vN);
  if (!gl_FrontFacing) n = -n;
  vec3 v = normalize(cameraPosition - vW);
  if (uMode != 0 && (uShader == 0 || uShader == 1 || uShader == 4 || uShader == 5)) {
    // clay or colour channels: one colour, the avatar's wrapped key light and a fill from the viewer
    vec3 alb = uMode == 1 ? vec3(0.62) : uFlat;
    float hemi = 0.5 + 0.5 * n.y;
    float wrap = clamp((dot(n, -uLightDir) + 0.5) / 1.5, 0.0, 1.0);
    float fill = max(dot(n, v), 0.0);
    float a = uShader == 4 ? max(uTint.a, 0.35) : uTint.a;
    oColor = vec4(alb * (0.10 + 0.12 * hemi + 0.42 * wrap + 0.36 * fill), a);
    return;
  }
  vec4 base = uTint;
  if (uHasDiffuse == 1) base *= texture(uDiffuse, vUV);
  if (uShader == 5) {
    vec3 r = reflect(-v, n);
    float fres = pow(1.0 - max(dot(n, v), 0.0), 5.0);
    vec3 F = uEmissive.rgb + (vec3(1.0) - uEmissive.rgb) * fres * 0.6;
    oColor = vec4(base.rgb + arenaEnv(r) * F * uEmissive.a, base.a);
    return;
  }
  if (uShader == 1) {
    vec4 m;
    if (uHasMask == 1) m = texture(uMask, vUV);
    else {
      float inside = step(abs(vW.z), 2.7) * step(abs(vW.x), 9.7) * step(vW.y, 4.2);
      inside *= abs(vW.x) > 9.4 ? 0.35 : 1.0;
      m = vec4(clamp((-vW.x - 4.6) / 4.9, 0.0, 1.0) * inside, vCol.a, clamp((vW.x - 4.6) / 4.9, 0.0, 1.0) * inside, 1.0);
    }
    vec3 alb = base.rgb * vCol.rgb * vCol.rgb;
    float metal = clamp(uEmissive.a, 0.0, 1.0);
    vec3 r = reflect(-v, n);
    vec3 env = mix(vec3(0.010, 0.014, 0.026), vec3(0.16, 0.24, 0.34), smoothstep(-0.2, 0.9, r.y));
    float fres = 0.04 + 0.96 * pow(1.0 - max(dot(n, v), 0.0), 5.0);
    vec3 spec = env * mix(vec3(0.04 + fres * 0.3), alb * 1.1 + 0.03, metal);
    vec3 diff = alb * (1.0 - 0.85 * metal) * (0.05 + 0.85 * m.g * m.g);
    vec3 glow = (uTeam0.rgb * m.r * m.r * uTeam0.a + uTeam1.rgb * m.b * m.b * uTeam1.a) * 0.30;
    oColor = vec4(diff + spec + glow + uEmissive.rgb, base.a);
  } else if (uShader == 2) {
    if (uDecals > 0 && printCover() > 0.5) discard;   // (a suit's pulses under a print)
    vec4 tx = uHasDiffuse == 1 ? texture(uDiffuse, vUV) : vec4(1.0);
    oColor = vec4(uTint.rgb * tx.rgb * uTint.a + uEmissive.rgb, 1.0);
  } else if (uShader == 3) {
    oColor = vec4(base.rgb + uEmissive.rgb, base.a);
  } else {
    vec3 alb = base.rgb, glowAdd = vec3(0.0);
    if (uDesign == 1) {
      vec4 dm = texture(uMask, vUV);
      alb = mix(alb, uDesCol.rgb, dm.r);
      alb = mix(alb, vec3(0.80), dm.b);
      glowAdd += uDesGlow.rgb * dm.g;
    }
    alb *= vCol.r;
    if (uPattern == 1) {
      vec4 pt = texture(uPatternTex, vUV);
      alb = mix(alb, uPatCol.rgb, pt.r);
      glowAdd += uPatGlow.rgb * pt.g;
    }
    if (uDecals > 0) {   // NATIONS: the flags printed on the garment (lit and knit like the rest of it), over its glow
      float cover = 0.0;
      alb = printed(alb, vDec0, vDecN.x, cover);
      if (uDecals > 1) alb = printed(alb, vDec1, vDecN.y, cover);
      glowAdd *= 1.0 - cover;
    }
    float hemi = 0.5 + 0.5 * n.y;
    float ndl = max(dot(n, -uLightDir), 0.0);
    vec3 h = normalize(v - uLightDir);
    float nv = max(dot(n, v), 0.0);
    float matte = uEmissive.a < 0.0 ? -uEmissive.a : 0.0;
    float spec = pow(max(dot(n, h), 0.0), 32.0) * 0.25 * (1.0 - matte);
    vec3 c = alb * (0.10 + 0.18 * hemi + 0.80 * ndl) + spec + uEmissive.rgb + glowAdd;
    if (uChar == 1) {
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
    if (uEmissive.a > 0.0 && uCloth == 0) {
      float fres = 0.04 + 0.96 * pow(1.0 - nv, 5.0);
      c += arenaEnv(reflect(-v, n)) * (0.25 + fres) * (uEmissive.a * 1.5) + pow(max(dot(n, h), 0.0), 96.0) * uEmissive.a * 0.8;
    }
    c += uRim.rgb * (uRim.a * pow(1.0 - nv, 2.5) * (1.0 - 0.75 * matte));
    oColor = vec4(c, base.a);
  }
}`;
const LIGHT = new THREE.Vector3(0.3, -1.0, 0.2).normalize();

// a section's material as render.cpp draw() sets it up; ctx: channels (8 rgb), rim (rgba), pattern (texture),
// tintMul, alphaMul, emAdd (rgb)
function material(mt, ctx) {
  const sh = ctx.forceShader ?? mt.shader;
  const alphaMul = (ctx.alphaMul ?? 1) * (state.ghost && (sh === SH.LIT || sh === SH.COURT || sh === SH.GLASS) ? 0.35 : 1);
  const blended = sh === SH.ADD || sh === SH.TRANS || alphaMul < 0.999;
  const tm = ctx.tintMul || [1, 1, 1, 1];
  const tint = [mt.tint[0] * tm[0], mt.tint[1] * tm[1], mt.tint[2] * tm[2], mt.tint[3] * (tm[3] ?? 1) * alphaMul];
  const em = [mt.em[0], mt.em[1], mt.em[2], mt.em[3]];
  if (ctx.emAdd) for (let k = 0; k < 3; k++) em[k] += ctx.emAdd[k];
  const ch = (mt.flags >> 8) & 0xff;
  const C = ctx.channels;
  if (C && ch && ch < 8) for (let k = 0; k < 3; k++) tint[k] *= C[ch][k];
  const pat = !!(C && ctx.pattern && (mt.flags & MAT_PATTERN) && sh === SH.LIT);
  const maskTex = tex(mt.mask, false);
  const design = !!(C && (mt.flags & MAT_DESIGN) && maskTex && sh === SH.LIT);
  const rim = ctx.rim || [0, 0, 0, 0];
  const chr = rim[3] > 0 && sh === SH.LIT;
  const fabric = tex('open_fabric', false);
  const cloth = chr && (mt.flags & MAT_CLOTH) && !!fabric;
  const diffuse = tex(mt.diffuse, true);
  const v4 = (a) => new THREE.Vector4(a[0], a[1], a[2], a[3] ?? 1);
  const flat = CHANNEL_SHOW[ch < 8 ? ch : 0];
  const u = {
    uShader: { value: sh }, uTint: { value: v4(tint) }, uEmissive: { value: v4(em) },
    uHasDiffuse: { value: diffuse ? 1 : 0 }, uDiffuse: { value: diffuse || white },
    uHasMask: { value: maskTex ? 1 : 0 }, uMask: { value: maskTex || white },
    uTeam0: { value: v4(D.teams[0]) }, uTeam1: { value: v4(D.teams[1]) }, uLightDir: { value: LIGHT },
    uRim: { value: v4(rim) }, uPattern: { value: pat ? 1 : 0 }, uPatternTex: { value: (pat && ctx.pattern) || white },
    uPatCol: { value: C ? v4([mt.tint[0] * tm[0] * C[2][0], mt.tint[1] * tm[1] * C[2][1], mt.tint[2] * tm[2] * C[2][2], 1]) : v4([1, 1, 1, 1]) },
    uPatGlow: { value: C ? v4(C[3]) : v4([0, 0, 0, 0]) }, uDesign: { value: design ? 1 : 0 },
    uDesCol: { value: C ? v4(C[2]) : v4([1, 1, 1, 1]) }, uDesGlow: { value: C ? v4(C[3]) : v4([0, 0, 0, 0]) },
    uChar: { value: chr ? 1 : 0 }, uCloth: { value: cloth ? 1 : 0 }, uFabric: { value: fabric || white },
    uUV: { value: mt.uv === 1 ? 1 : 0 }, uMode: { value: state.surface }, uFlat: { value: new THREE.Vector3(...flat) },
    uDecals: { value: ctx.decalTex && (sh === SH.LIT || sh === SH.ADD) ? 2 : 0 }, uDecalTex: { value: ctx.decalTex || white },
  };
  const m = new THREE.ShaderMaterial({ uniforms: u, vertexShader: VS, fragmentShader: FS, glslVersion: THREE.GLSL3,
    wireframe: state.wire });
  if (sh === SH.ADD) {
    m.blending = THREE.CustomBlending; m.blendEquation = THREE.AddEquation;
    m.blendSrc = THREE.OneFactor; m.blendDst = THREE.OneFactor; m.blendSrcAlpha = THREE.ZeroFactor; m.blendDstAlpha = THREE.OneFactor;
    m.transparent = true; m.depthWrite = false;
  } else if (blended) {
    m.blending = THREE.CustomBlending; m.blendEquation = THREE.AddEquation;
    m.blendSrc = THREE.SrcAlphaFactor; m.blendDst = THREE.OneMinusSrcAlphaFactor;
    m.blendSrcAlpha = THREE.OneFactor; m.blendDstAlpha = THREE.OneMinusSrcAlphaFactor;
    m.transparent = true; m.depthWrite = false;
  } else {
    m.blending = THREE.NoBlending; m.transparent = false;
  }
  m.side = (mt.flags & MAT_TWOSIDED) || blended ? THREE.DoubleSide : THREE.FrontSide;
  m.userData.channel = ch;
  return m;
}

// ---------------------------------------------------------------- the rig: linear blend skinning as render.cpp's vertex shader
function mul4(A, B, out) {   // column-major 4x4: out = A * B
  for (let c = 0; c < 4; c++) for (let r = 0; r < 4; r++) {
    out[c * 4 + r] = A[r] * B[c * 4] + A[4 + r] * B[c * 4 + 1] + A[8 + r] * B[c * 4 + 2] + A[12 + r] * B[c * 4 + 3];
  }
  return out;
}

function skin(M, rig, sw) {
  const nv = M.nv, P = M.pos, N = M.nrm, BI = M.bi, BW = M.bw, UV1 = M.uv1;
  const op = new Float32Array(nv * 3), on = new Float32Array(nv * 3);
  const B = rig.skin.map((a) => Float64Array.from(a));
  const pel = rig.pelvis ? Float64Array.from(rig.pelvis) : null;
  const tw = rig.twist || null;
  const S = new Float64Array(16), T = new Float64Array(16), Rt = new Float64Array(16), TB = new Float64Array(16);
  const twG = [0, 0, 0, 0];
  for (let v = 0; v < nv; v++) {
    let px = P[v * 3], py = P[v * 3 + 1], pz = P[v * 3 + 2];
    let nx0 = N[v * 3], ny0 = N[v * 3 + 1], nz0 = N[v * 3 + 2];
    const sww = sw ? UV1[v * 2] : 0;
    if (sww > 0) {   // TENNIS SKIRT / KILT / DRESS (0.3.26): render.cpp's swing, its weight and room from the body in aUV1
      const a = sw.tw * sww, c = Math.cos(a), sn = Math.sin(a), Cc = sw.sh.c;
      const dx = px - Cc[0], dz = pz - Cc[1];
      let qx = Cc[0] + c * dx + sn * dz + sw.x * sww, qy = py + sw.lift * sww, qz = Cc[1] - sn * dx + c * dz + sw.z * sww;
      const mx = qx - px, my = qy - py, mz = qz - pz, Lm = Math.hypot(mx, my, mz), bound = Cc[2] * sww;
      if (Lm > bound) { qx = px + mx * bound / Lm; qy = py + my * bound / Lm; qz = pz + mz * bound / Lm; }
      const ex = qx - Cc[0], ez = qz - Cc[1], r = Math.hypot(ex, ez);
      const target = swRadius(sw.sh, qy, Math.atan2(ex, -ez)) + UV1[v * 2 + 1];
      if (r < target) {
        if (r > 1e-6) { qx = Cc[0] + ex * target / r; qz = Cc[1] + ez * target / r; } else { qx = Cc[0] + target; qz = Cc[1]; }
      }
      px = qx; py = qy; pz = qz;
      const rnx = c * nx0 + sn * nz0, rnz = -sn * nx0 + c * nz0;
      nx0 = rnx; nz0 = rnz;
    }
    S.fill(0);
    for (let i = 0; i < 4; i++) {
      twG[i] = 0;
      const w = BW[v * 4 + i] / 255;
      if (w === 0) continue;
      const b = BI[v * 4 + i];
      let Mb = B[b] || B[0];
      if (pel && b === 0) {
        const x = Math.min(1, Math.max(0, (py + 0.50) / 0.24)), t = x * x * (3 - 2 * x);
        for (let k = 0; k < 16; k++) T[k] = pel[k] * (1 - t) + B[0][k] * t;
        Mb = T;
      }
      if (tw && (b === 5 || b === 6)) {
        const k = b - 5, e0 = 4 * k, d = 4 * (2 + k), E = 4 * (4 + k), A = 4 * (6 + k);
        let s = ((px - tw[e0]) * tw[d] + (py - tw[e0 + 1]) * tw[d + 1] + (pz - tw[e0 + 2]) * tw[d + 2]) * tw[e0 + 3];
        s = Math.min(1, Math.max(0, s));
        const th = -tw[d + 3] * (1 - s * s * (3 - 2 * s));
        const ax = tw[A], ay = tw[A + 1], az = tw[A + 2], c = Math.cos(th), sn = Math.sin(th), ic = 1 - c;
        // R = c I + sn [a]x + (1 - c) a a^T, column-major; Rt = R with the translation E - R E
        const R = [c + ic * ax * ax, sn * az + ic * ax * ay, -sn * ay + ic * ax * az,
          -sn * az + ic * ay * ax, c + ic * ay * ay, sn * ax + ic * ay * az,
          sn * ay + ic * az * ax, -sn * ax + ic * az * ay, c + ic * az * az];
        const ex = tw[E], ey = tw[E + 1], ez = tw[E + 2];
        Rt.fill(0);
        for (let cc = 0; cc < 3; cc++) for (let r = 0; r < 3; r++) Rt[cc * 4 + r] = R[cc * 3 + r];
        Rt[12] = ex - (R[0] * ex + R[3] * ey + R[6] * ez);
        Rt[13] = ey - (R[1] * ex + R[4] * ey + R[7] * ez);
        Rt[14] = ez - (R[2] * ex + R[5] * ey + R[8] * ez);
        Rt[15] = 1;
        Mb = mul4(Rt, Mb, TB).slice();
        twG[i] = tw[d + 3] * 6 * s * (1 - s) * tw[E + 3];
      }
      for (let k = 0; k < 16; k++) S[k] += Mb[k] * w;
    }
    const x = S[0] * px + S[4] * py + S[8] * pz + S[12], y = S[1] * px + S[5] * py + S[9] * pz + S[13], z = S[2] * px + S[6] * py + S[10] * pz + S[14];
    let nx = S[0] * nx0 + S[4] * ny0 + S[8] * nz0, ny = S[1] * nx0 + S[5] * ny0 + S[9] * nz0, nz = S[2] * nx0 + S[6] * ny0 + S[10] * nz0;
    if (tw) {
      for (let i = 0; i < 4; i++) {
        if (!twG[i]) continue;
        const k = BI[v * 4 + i] - 5, A = 4 * (6 + k), E = 4 * (4 + k);
        const ax = tw[A], ay = tw[A + 1], az = tw[A + 2];
        const qx = x - tw[E], qy = y - tw[E + 1], qz = z - tw[E + 2];
        const cx = ay * qz - az * qy, cy = az * qx - ax * qz, cz = ax * qy - ay * qx;
        const f = (BW[v * 4 + i] / 255) * twG[i] * (cx * nx + cy * ny + cz * nz);
        nx -= f * ax; ny -= f * ay; nz -= f * az;
      }
    }
    op[v * 3] = x; op[v * 3 + 1] = y; op[v * 3 + 2] = z;
    on[v * 3] = nx; on[v * 3 + 1] = ny; on[v * 3 + 2] = nz;
  }
  return [op, on];
}

// hair under headgear and face shields, kept inside them (render.cpp uTuck; main.cpp hairTuck: the two maps combined)
function tucked(M, maps) {
  const T = maps[0];
  const nv = M.nv, P = M.pos, out = new Float32Array(P);
  const val = (u, t) => {
    const x = u * T.w - 0.5, y = Math.min(T.h - 1, Math.max(0, t * T.h - 0.5));
    const x0 = Math.floor(x), y0 = Math.floor(y), fx = x - x0, fy = y - y0;
    const at = (i, j) => {
      i = ((i % T.w) + T.w) % T.w; j = Math.min(T.h - 1, Math.max(0, j));
      let m = 255;
      for (const Q of maps) m = Math.min(m, Q.v[j * T.w + i]);
      return m;
    };
    return ((at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy) / 255;
  };
  const [cx, cy, cz] = T.c;
  for (let v = 0; v < nv; v++) {
    const dx = P[v * 3] - cx, dy = P[v * 3 + 1] - cy, dz = P[v * 3 + 2] - cz;
    const r = Math.max(Math.hypot(dx, dy, dz), 1e-4);
    const u = Math.atan2(dx, dz) * 0.1591549 + 0.5, t = Math.asin(Math.min(1, Math.max(-1, dy / r))) * 0.3183099 + 0.5;
    const lim = 0.05 + 0.5 * val(u, t);
    const x = r - (lim - 0.006);
    if (x > 0) {
      const k = (lim - 0.006 + 0.006 * (1 - Math.exp(-x / 0.006))) / r;
      out[v * 3] = cx + dx * k; out[v * 3 + 1] = cy + dy * k; out[v * 3 + 2] = cz + dz * k;
    }
  }
  return out;
}

const tuckMaps = new Map();
function tuckMap(name) {
  if (!D.tuck[name]) return null;
  if (!tuckMaps.has(name)) {
    const t = D.tuck[name], bin = atob(t.v), v = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) v[i] = bin.charCodeAt(i);
    tuckMaps.set(name, { w: t.w, h: t.h, c: t.c, v });
  }
  return tuckMaps.get(name);
}

// a model as a mesh: one geometry, a group and a material per section
function meshOf(name, M, ctx, positions, normals, decals) {
  const g = new THREE.BufferGeometry();
  g.setAttribute('position', new THREE.BufferAttribute(positions || M.pos, 3));
  g.setAttribute('normal', new THREE.BufferAttribute(normals || M.nrm, 3));
  g.setAttribute('aUV0', new THREE.BufferAttribute(M.uv0, 2));
  g.setAttribute('aUV1', new THREE.BufferAttribute(M.uv1, 2));
  g.setAttribute('aCol', new THREE.BufferAttribute(M.col, 4, true));
  if (decals) {   // (NATIONS: the jersey's prints, decalCoords)
    g.setAttribute('aDec0', new THREE.BufferAttribute(decals[0], 3));
    g.setAttribute('aDec1', new THREE.BufferAttribute(decals[1], 3));
    g.setAttribute('aDecN', new THREE.BufferAttribute(decals[2], 2));
  }
  g.setIndex(new THREE.BufferAttribute(M.idx, 1));
  const mats = [];
  M.secs.forEach(([first, count, mi], si) => { g.addGroup(first, count, si); mats.push(material(M.mats[mi], ctx)); });
  g.computeBoundingSphere();
  g.computeBoundingBox();
  const mesh = new THREE.Mesh(g, mats);
  mesh.name = name;
  mesh.frustumCulled = false;
  return mesh;
}

// ---------------------------------------------------------------- the avatar: avatar.cpp channelColors and drawList
function field(name) { return D.fields.find((f) => f.name === name); }
function optRGB(name, i) { const o = field(name).options[i]; return o ? o.rgb.slice() : [0.5, 0.5, 0.5]; }

// NATIONS (COUNTRY, F IDs): the look's country (null: none)
function nationOf(look) { return (look.nation && D.nations && D.nations[look.nation - 1]) || null; }
const lin = (c) => Math.pow(c / 255, 2.2);   // (avatar.cpp lin)
function kitTrim(N) {   // the kit's trim: the flag's accent colour, or its second when the accent is the jersey's own
  let d = 0;
  for (let k = 0; k < 3; k++) d += Math.abs(N.kit[2][k] - N.kit[0][k]);
  return d > 90 ? 2 : 1;
}
function glowOf(c, fallback) {   // a kit colour as a glow (brightest 1)
  const v = c.map(lin), m = Math.max(...v);
  return m < 0.02 ? fallback.slice() : v.map((x) => x / m);
}
// the colour of the wearer's end of the court: blue / orange, or the country's (avatar.cpp sideColors, one player)
function teamColor(look, team) { const N = nationOf(look); return N ? N.team[0].slice() : D.teams[team].slice(); }
// with a country, the team kit: the ARENA JERSEY, no pattern, ATHLETIC SHORTS (bike and long shorts and the skirts stay);
// with COUNTRY KIT = MY OUTFIT (look.kit 1, avatar.cfg kit=1) your own suit, pattern and lower body in its colours
function uniformLook(look) {
  if (!nationOf(look) || look.kit) return look;
  const d = Object.assign({}, look);
  d.suit = 1; d.pattern = 0;
  if (d.lower === 0 || d.lower === 2) d.lower = 1;
  return d;
}
// main.cpp's two prints: a box in bind space each (bind space -> the unit box at the pose: x, y across, z through the
// cloth), the flag high on the back just under the collar and small on the left chest
const DEG = Math.PI / 180;
// (COUNTRY KIT: on a suit whose own design is in those places, main.cpp moves them: ECHO SUIT's back print over its
// identity disc, VECTOR's and NIGHTFORGE's on the left shoulder blade, VANGUARD's feminine crest on the chest plate,
// PELURU LAYERED DRESS's lying on its flounced neckline along the hem)
function printBoxes(fem, suit) {
  const qx = (a) => new THREE.Quaternion().setFromAxisAngle(new THREE.Vector3(1, 0, 0), a);
  const qy = (a) => new THREE.Quaternion().setFromAxisAngle(new THREE.Vector3(0, 1, 0), a);
  const back = { q: qx(15 * DEG), p: fem ? [0, -0.11, 0.090] : [0, -0.12, 0.125], size: 0.13 };
  const crest = { q: qy(Math.PI).multiply(qx(-(fem ? 45 : 18) * DEG)), p: fem ? [-0.08, -0.118, -0.147] : [-0.08, -0.150, -0.131] };
  const along = (b, x, y) => { const v = new THREE.Vector3(x, y, 0).applyQuaternion(b.q); return [b.p[0] + v.x, b.p[1] + v.y, b.p[2] + v.z]; };
  if (suit === 5 || suit === 12) { back.p = along(back, 0, fem ? -0.033 : -0.015); back.size = 0.21; }
  if (suit === 14 || suit === 15) { back.p = along(back, fem ? -0.077 : -0.090, 0); back.size = fem ? 0.09 : 0.10; }
  if (suit === 2 && fem) { crest.q = qy(Math.PI).multiply(qx(-10 * DEG)); crest.p = [-0.08, -0.150, -0.186]; }
  crest.depth = 0.10; back.depth = 0.16;
  if (suit === 18) {   // PELURU LAYERED DRESS: each on the upper flounce, its bottom edge along the hem
    const qz = (a) => new THREE.Quaternion().setFromAxisAngle(new THREE.Vector3(0, 0, 1), a);
    crest.q = qy(-167.7 * DEG).multiply(qx(-26.5 * DEG)).multiply(qz(10.1 * DEG)); crest.p = [-0.0816, -0.1273, -0.1871]; crest.depth = 0.04;
    back.q = qx(-26.6 * DEG); back.p = [0, -0.1281, 0.1300]; back.size = 0.075; back.depth = 0.04;
  }
  return [
    { q: back.q, p: back.p, size: [back.size, back.size / 1.5, back.depth] },
    { q: crest.q, p: crest.p, size: [0.055, 0.055 / 1.5, crest.depth] },
  ];
}
function decalCoords(M, fem, suit) {   // each vertex's place in both boxes, and which way its normal faces through them
  const boxes = printBoxes(fem, suit).map((b) => ({ inv: b.q.clone().invert(), p: new THREE.Vector3(...b.p), size: b.size }));
  const d0 = new Float32Array(M.nv * 3), d1 = new Float32Array(M.nv * 3), dn = new Float32Array(M.nv * 2);
  const v = new THREE.Vector3(), n = new THREE.Vector3();
  for (let i = 0; i < M.nv; i++) {
    boxes.forEach((b, k) => {
      v.set(M.pos[i * 3], M.pos[i * 3 + 1], M.pos[i * 3 + 2]).sub(b.p).applyQuaternion(b.inv);
      const out = k ? d1 : d0;
      out[i * 3] = v.x / b.size[0]; out[i * 3 + 1] = v.y / b.size[1]; out[i * 3 + 2] = v.z / b.size[2];
      n.set(M.nrm[i * 3], M.nrm[i * 3 + 1], M.nrm[i * 3 + 2]).applyQuaternion(b.inv);
      dn[i * 2 + k] = n.z / b.size[2];
    });
  }
  return [d0, d1, dn];
}

function channels(look, team) {
  const tc = teamColor(look, team);
  const out = [];
  for (let c = 0; c < 8; c++) out.push([1, 1, 1]);
  out[1] = optRGB('MAIN COLOR', look.primary);
  out[2] = optRGB('TRIM COLOR', look.secondary);
  out[3] = look.glow === 0 ? tc.slice() : optRGB('GLOW', look.glow);
  const N = nationOf(look);
  if (N) {   // the team kit in the flag's own colours
    const t = kitTrim(N);
    out[1] = N.kit[0].map(lin); out[2] = N.kit[t].map(lin); out[3] = glowOf(N.kit[2], N.team[0]);
  }
  out[4] = optRGB('SKIN', look.skin);
  const nT = field('SHIELD TINT').options.length, nH = field('HAIR COLOR').options.length;
  out[5] = look.visortint === nT - 1 ? tc.map((x) => x * 0.45) : optRGB('SHIELD TINT', look.visortint);
  out[6] = look.haircolor === nH - 1 ? out[3].slice() : optRGB('HAIR COLOR', look.haircolor);
  const metal = [0.50, 0.52, 0.56];
  out[7] = metal.map((m, k) => m * 0.7 + out[2][k] * 0.3);
  out[3] = out[3].map((x) => x * 1.15);
  return out;
}

function has(name) { return !!D.models[name]; }

// the parts drawList adds for a look: { model, at: 'skin' | 'head' | 'hand', hand, field, extra }
function partsOf(look) {
  const b = look.body ? 'f' : 'm', face = look.face | 0, fk = b + (face ? face : '');
  const out = [];
  const add = (model, at, fieldName, extra = {}) => {
    if (!has(model) && /\d$/.test(model) && has(model.replace(/\d$/, ''))) model = model.replace(/\d$/, '');   // (the base head's)
    if (has(model)) out.push(Object.assign({ model, at, field: fieldName }, extra));
  };
  add(`av_top_${look.suit}_${b}`, 'skin', 'SUIT');
  add(`av_bhead_${fk}`, 'skin', 'FACE');
  if (look.suit === 18) {}   // (PELURU LAYERED DRESS: its skirt takes the LOWER BODY's place)
  else if (look.lower >= 1 && look.lower <= 4 && (look.suit === 14 || look.suit === 15)) add(`av_sbottom_${look.suit}_${look.lower}_${b}`, 'skin', 'LOWER BODY');
  else if (look.lower) add(`av_bottom_${look.lower}_${b}`, 'skin', 'LOWER BODY');
  else add(`av_wfade_${b}`, 'skin', 'LOWER BODY');
  if (look.headgear) add(`av_gear_${look.headgear}_${b}`, 'head', 'HEADGEAR');
  if (look.suit >= 10 && look.suit <= 13) add(`av_tpulse_${look.suit}_${b}`, 'skin', 'SUIT');
  const neon = look.haircolor === field('HAIR COLOR').options.length - 1;
  if (look.hair) {
    add(`av_hair_${look.hair}_${b}`, 'head', 'HAIR', { hair: true, neon });
    if (look.hair === 3 || look.hair === 7 || look.hair === 9) add(`av_hair_${look.hair}t_${b}`, 'head', 'HAIR', { hair: true, neon });
  }
  if (look.visor) add(`av_visor_${look.visor}_${fk}`, 'head', 'FACE SHIELD');
  if (look.mask) add(`av_mask_${look.mask}_${fk}`, 'head', 'MASK');
  if (look.mask === 4) add(`av_mask_4t_${b}`, 'head', 'MASK', { hair: true });   // (the BANDANA's tails)
  if (look.suit === 18) add(`av_top_18e_${b}`, 'head', 'SUIT', { hair: true });  // (the dress's hoop earrings)
  for (let h = 0; h < 2; h++) {
    if (look.gauntlet !== 3) {   // (plate gauntlets: 0-2, and 4-6 whose fingers open on the trigger: shown closed)
      add(`gauntlet_${look.gauntlet}_hand`, 'hand', 'GAUNTLET', { hand: h, slot: 'fist' });
      if (look.gauntlet >= 4) add(`gauntlet_${look.gauntlet}_fist`, 'hand', 'GAUNTLET', { hand: h, slot: 'fist' });
      add(`gauntlet_${look.gauntlet}_cuff`, 'hand', 'GAUNTLET', { hand: h, slot: 'cuff' });
    } else add('lancer', 'hand', 'GAUNTLET', { hand: h, slot: 'lancer' });
    add('knuckleguard', 'hand', 'GAUNTLET', { hand: h, slot: 'knuckle', knuckle: true });
  }
  return out;
}

// a part or preset the release doesn't have yet says so (data.json 'since', as Country kit does)
const since = (x) => (x && x.since ? `  (from ${x.since})` : '');
function idFor(fieldName, value) {
  const e = D.avatar.find((a) => a.field === fieldName && a.value === value);
  return e ? e.id : '';
}

function defaultLook() {
  const p = D.presets[1] || D.presets[0];
  const look = { suit: 0, build: 1, pattern: 0, primary: 0, secondary: 1, glow: 0, hair: 0, haircolor: 0, headgear: 0, visor: 0,
    visortint: 0, mask: 0, skin: 2, lower: 0, body: 0, gauntlet: 0, face: 0, nation: 0, kit: 0 };
  return Object.assign(look, p.look);
}

// ---------------------------------------------------------------- three.js
const canvas = $('view'), stage = $('stage');
const renderer = new THREE.WebGLRenderer({ canvas, antialias: false, powerPreference: 'high-performance' });
renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
renderer.outputColorSpace = THREE.SRGBColorSpace;
renderer.sortObjects = false;   // (the game's order: everything opaque, then the blended layers as listed)
const scene = new THREE.Scene();
scene.background = new THREE.Color(0x05070c);
const camera = new THREE.PerspectiveCamera(32, 1, 0.02, 600);
const controls = new OrbitControls(camera, canvas);
controls.enableDamping = true;
controls.dampingFactor = 0.12;
const rt = new THREE.WebGLRenderTarget(4, 4, { type: THREE.HalfFloatType, samples: 4 });
const composer = new EffectComposer(renderer, rt);
composer.addPass(new RenderPass(scene, camera));
composer.addPass(new OutputPass());
const root = new THREE.Group();
scene.add(root);
const overlay = new THREE.Group();
scene.add(overlay);
const grid = new THREE.GridHelper(6, 24, 0x3a4a5c, 0x1d2733);
grid.material.transparent = true;
grid.material.opacity = 0.6;
scene.add(grid);
let dirty = true;
controls.addEventListener('change', () => { dirty = true; });

function resize() {
  const r = stage.getBoundingClientRect();
  const w = Math.max(1, Math.floor(r.width)), h = Math.max(1, Math.floor(r.height));
  renderer.setSize(w, h, false);
  composer.setSize(w, h);
  camera.aspect = w / h;
  camera.updateProjectionMatrix();
  dirty = true;
}
new ResizeObserver(resize).observe(stage);
function frame(now) {
  requestAnimationFrame(frame);
  if (document.hidden) return;
  if (controls.update()) dirty = true;
  if (state.animRun && animStep(now || performance.now())) dirty = true;   // (0.3.26: an animation playing)
  if (!dirty || !$('flat').hidden) return;
  dirty = false;
  composer.render();
}

function clear() {
  state.animRun = null;   // (an animation stops with what it moved)
  live = null;
  for (const g of [root, overlay]) {
    while (g.children.length) {
      const o = g.children.pop();
      o.traverse((x) => { if (x.geometry) x.geometry.dispose(); (Array.isArray(x.material) ? x.material : [x.material]).forEach((m) => m && m.dispose && m.dispose()); });
    }
  }
}

function setCamera(target, dist, dir) {
  const d = new THREE.Vector3(...dir).normalize();
  camera.fov = state.section === 'arena' ? 50 : 32;
  controls.target.set(...target);
  camera.position.copy(controls.target).addScaledVector(d, dist);
  camera.near = Math.max(0.01, dist / 200);
  camera.far = Math.max(50, dist * 40);
  camera.updateProjectionMatrix();
  controls.update();
  dirty = true;
}

function viewDir(v) {
  return { front: [0.08, 0.12, 1], side: [1, 0.12, 0.02], back: [-0.08, 0.12, -1], top: [0.001, 1, 0.25] }[v] || [0.08, 0.12, 1];
}

let lastBox = null;
function fitBox(box, dir) {
  const s = box.getBoundingSphere(new THREE.Sphere());
  const dist = s.radius / Math.sin((camera.fov * Math.PI) / 360) * 1.05;
  setCamera([s.center.x, s.center.y, s.center.z], dist, dir);
}

// ---------------------------------------------------------------- the sections
let token = 0;   // (a newer choice cancels an older load)
function busy(on, text) { $('busy').hidden = !on; if (text) $('busy').textContent = text; }

async function showAvatar(keepCamera) {
  const my = ++token;
  const look = uniformLook(state.look);   // (a country: its team kit)
  const N = nationOf(look);
  const combo = `${look.body ? 1 : 0}${look.build}`;
  let rig = D.rig[`${combo}_${state.pose}`], run = null;
  if (state.pose.startsWith('anim:')) {   // (0.3.26) an ANIMATION: its frames for this body and build
    let frames;
    try { frames = (await animsFor(combo))[state.pose.slice(5)]; } catch (e) { return fail(e); }
    if (my !== token) return;
    if (!frames || !frames.length) return fail(new Error('no frames for ' + state.pose));
    run = { frames, start: performance.now() };
    rig = rigOfFrame(frames[0]);
  }
  const parts = partsOf(look);
  busy(true, 'loading ' + parts.length + ' parts');
  let models;
  let propM = null;   // (an animation with a thing in its hands: AIR GUITAR's guitar)
  try {
    models = await Promise.all(parts.map((p) => loadModel(p.model)));
    if (run && run.frames.some((f) => f.x) && D.models.guitar) propM = await loadModel('guitar');
  } catch (e) { return fail(e); }
  if (my !== token) return;
  clear();
  state.animRun = run;
  live = { skinned: [], heads: [], hands: [] };
  const C = channels(look, state.team);
  const rim = [...teamColor(look, state.team), N ? 0.25 : 0.5];   // (main.cpp lookOf: softer over a country's kit)
  const b = look.body ? 'f' : 'm';
  const pattern = look.pattern ? tex(`open_pat_${PATTERNS[look.pattern]}_${b}`, false) : null;
  const ctx = { channels: C, rim, pattern };
  const flagTex = N ? tex(N.flag, true) : null;
  const head = new THREE.Matrix4().fromArray(rig.head);
  // hair under the headgear and the face shield: their tuck maps (main.cpp hairTuck)
  const maps = [];
  if (look.headgear && tuckMap(`av_gear_${look.headgear}_${b}`)) maps.push(tuckMap(`av_gear_${look.headgear}_${b}`));
  if (look.visor && tuckMap(`av_visor_${look.visor}_${b}`)) {
    const V = tuckMap(`av_visor_${look.visor}_${b}`);
    if (!maps.length || (maps[0].w === V.w && maps[0].h === V.h)) maps.push(V);
  }
  let verts = 0, tris = 0;
  const rows = [];
  parts.forEach((p, i) => {
    const M = models[i];
    let mesh;
    if (p.at === 'skin') {
      const [pos, nrm] = skin(M, rig, swingFor(p.model, run ? run.frames[0] : null));
      // (main.cpp: the prints on the jersey or your own top, and its pulses give way to them)
      const jersey = flagTex && (p.model.startsWith('av_top_') || p.model.startsWith('av_tpulse_'));
      mesh = meshOf(p.model, M, jersey ? Object.assign({}, ctx, { decalTex: flagTex }) : ctx, pos, nrm,
        jersey ? decalCoords(M, !!look.body, look.suit) : null);
      live.skinned.push({ mesh, M, model: p.model });
    } else if (p.at === 'head') {
      const c = Object.assign({}, ctx);
      if (p.neon) c.emAdd = C[3].map((x) => x * 0.35);
      mesh = meshOf(p.model, M, c, p.hair && maps.length ? tucked(M, maps) : null, null);
      mesh.matrixAutoUpdate = false;
      mesh.matrix.copy(head);
      live.heads.push(mesh);
    } else {
      const c = Object.assign({}, ctx);
      if (p.knuckle) {
        const t = teamColor(look, state.team);
        c.alphaMul = 0.35; c.tintMul = t.map((x) => 0.75 + 0.25 * x); c.forceShader = SH.TRANS;   // (main.cpp kp)
        c.channels = null; c.rim = null;
      }
      mesh = meshOf(p.model, M, c);
      mesh.matrixAutoUpdate = false;
      mesh.matrix.fromArray(rig.hands[p.hand][p.slot]);
      live.hands.push({ mesh, hand: p.hand, slot: p.slot });
    }
    root.add(mesh);
    if (!(p.at === 'hand' && p.hand === 0)) {
      verts += M.nv; tris += M.idx.length / 3;
      rows.push({ id: p.field === 'SUIT' && p.model.startsWith('av_tpulse') ? idFor('SUIT', look.suit) : idFor(p.field, KEY[p.field] ? look[KEY[p.field]] : 0),
        file: p.model, v: M.nv, t: M.idx.length / 3, note: p.hair && maps.length ? 'kept inside the headgear' : (p.at === 'hand' ? 'each hand' : '') });
    }
  });
  if (propM) {   // (main.cpp: additive, the glow colour over a little white)
    const glow = C[3];
    live.prop = meshOf('guitar', propM, { channels: null, rim: null, tintMul: [0.35 + 0.65 * glow[0], 0.35 + 0.65 * glow[1], 0.35 + 0.65 * glow[2], 1] });
    live.prop.matrixAutoUpdate = false;
    root.add(live.prop);
    placeProp(run.frames[0]);
    rows.push({ id: 'P13', file: 'guitar', v: propM.nv, t: propM.idx.length / 3, note: 'AIR GUITAR: a hologram in the glow colour' });
  }
  if (state.bones && rig.bones) drawBones(rig);   // (an animation's frames carry no bones)
  root.updateMatrixWorld(true);
  grid.visible = state.grid;
  grid.position.y = 0;
  busy(false);
  partsList(rows, `${N ? N.id + ' ' + titleCase(N.name) + ': the team kit, the flag on the back and the chest · ' : ''}`
    + `${look.pattern ? idFor('PATTERN', look.pattern) + ' pattern ' + PATTERNS[look.pattern] + ' · ' : ''}${idFor('BUILD', look.build)} build`);
  const headP = new THREE.Vector3().setFromMatrixPosition(head);
  state.headP = headP;
  const hp = rig.hands.map((h) => new THREE.Vector3().setFromMatrixPosition(new THREE.Matrix4().fromArray(h.fist)));
  state.handsP = hp[0].clone().add(hp[1]).multiplyScalar(0.5);
  if (!keepCamera) focus(state.focus);
  const e = state.item && D.avatar.find((a) => a.id === state.item);
  title(e ? e.id : '', e ? e.name + since(e) : 'Avatar builder');
  note(`${verts.toLocaleString()} vertices · ${Math.round(tris).toLocaleString()} triangles · ${$('pose').selectedOptions[0].text.toLowerCase()} pose · `
    + `${N ? titleCase(N.name) + "'s colours" : ['blue', 'orange'][state.team] + ' end'} · ${$('surface').selectedOptions[0].text.toLowerCase()}`);
  dirty = true;
}

function drawBones(rig) {
  const pts = [];
  for (const s of rig.bones) pts.push(s[0], s[1], s[2], s[3], s[4], s[5]);
  const g = new THREE.BufferGeometry();
  g.setAttribute('position', new THREE.Float32BufferAttribute(pts, 3));
  const lines = new THREE.LineSegments(g, new THREE.LineBasicMaterial({ color: 0xf8bd5c, depthTest: false, transparent: true }));
  lines.renderOrder = 10;
  overlay.add(lines);
  const dots = new THREE.Points(g.clone(), new THREE.PointsMaterial({ color: 0xffe2a8, size: 7, sizeAttenuation: false, depthTest: false, transparent: true }));
  dots.renderOrder = 11;
  overlay.add(dots);
}

function focus(f) {
  state.focus = f;
  const dir = viewDir('front');
  if (state.section === 'avatar') {
    if (f === 'head' && state.headP) return setCamera([state.headP.x, state.headP.y - 0.02, state.headP.z], 0.85, dir);
    if (f === 'hands' && state.handsP) return setCamera([state.handsP.x, state.handsP.y, state.handsP.z], 1.0, [0.25, 0.35, 1]);
    if (f === 'full' || f === 'fit') return setCamera([0, 1.12, 0], 3.1, dir);
  }
  if (state.section === 'arena') return setCamera([1.5, 0.6, 0], 15.5, [-0.92, 0.30, 0.27]);   // (above the blue end, inside the stadium)
  const box = new THREE.Box3().setFromObject(root);
  if (!box.isEmpty()) fitBox(box, [0.9, 0.35, 0.6]);
}

function turn(v) {
  const d = new THREE.Vector3(...viewDir(v)).normalize();
  // (no view yet, the camera on its target: a turn by nothing left the orbit controls without a number for good, and the
  // view black; the item's own view first)
  let dist = camera.position.distanceTo(controls.target);
  if (!(dist > 1e-3)) { focus(state.focus); dist = camera.position.distanceTo(controls.target); }
  if (!(dist > 1e-3)) return;
  camera.position.copy(controls.target).addScaledVector(d, dist);
  controls.update();
  dirty = true;
}

async function showModels(names, place, keepCamera, extraRows) {
  const my = ++token;
  busy(true, 'loading ' + names.length + (names.length > 1 ? ' models' : ' model'));
  let models;
  try { models = await Promise.all(names.map(loadModel)); } catch (e) { return fail(e); }
  if (my !== token) return;
  clear();
  const look = state.look;
  const ctx = { channels: channels(look, state.team), rim: [0, 0, 0, 0] };
  let verts = 0, tris = 0;
  const rows = [];
  names.forEach((n, i) => {
    const M = models[i];
    const mesh = meshOf(n, M, place && place.ctx ? Object.assign({}, ctx, place.ctx(n)) : ctx);
    const m = place && place.matrix && place.matrix(n);
    if (m) { mesh.matrixAutoUpdate = false; mesh.matrix.fromArray(m); }
    root.add(mesh);
    verts += M.nv; tris += M.idx.length / 3;
    rows.push({ id: (extraRows && extraRows[n]) || '', file: n, v: M.nv, t: M.idx.length / 3, note: '' });
  });
  root.updateMatrixWorld(true);
  const box = new THREE.Box3().setFromObject(root);
  grid.visible = state.grid && state.section !== 'arena';
  if (!box.isEmpty()) grid.position.y = state.section === 'arena' ? 0 : box.min.y;
  busy(false);
  partsList(rows);
  if (!keepCamera) focus('fit');
  note(`${verts.toLocaleString()} vertices · ${Math.round(tris).toLocaleString()} triangles · ${$('surface').selectedOptions[0].text.toLowerCase()}`);
  dirty = true;
}

function fail(e) {
  busy(false);
  $('err').hidden = false;
  $('err').textContent = 'This part could not be loaded (' + (e && e.message ? e.message : e) + '). Reload the page to try again.';
}

function partsList(rows, footer) {
  const el = $('parts');
  el.replaceChildren();
  for (const r of rows) {
    const d = document.createElement('div');
    d.className = 'part';
    d.innerHTML = '<span class="iid"></span><span class="file"></span><span class="meta"></span>';
    d.children[0].textContent = r.id;
    d.children[1].textContent = r.file;
    d.children[2].textContent = `${r.v.toLocaleString()} v · ${Math.round(r.t).toLocaleString()} tris${r.note ? ' · ' + r.note : ''}`;
    el.append(d);
  }
  if (footer) { const p = document.createElement('p'); p.textContent = footer; el.append(p); }
}

function title(id, name) {
  const h = $('title');
  h.replaceChildren();
  if (id) { const s = document.createElement('span'); s.className = 'id'; s.textContent = id; h.append(s); }
  h.append(document.createTextNode(name));
}
function note(t) { $('note').textContent = t; }

// ---------------------------------------------------------------- the sidebar
function el(tag, attrs = {}, text) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
  if (text != null) e.textContent = text;
  return e;
}
const toSRGB = (c) => Math.round(255 * Math.min(1, Math.pow(Math.max(0, c), 1 / 2.2)));
const css = (rgb) => `rgb(${toSRGB(rgb[0])},${toSRGB(rgb[1])},${toSRGB(rgb[2])})`;

function buildAvatarPanel() {
  const ps = $('preset');
  ps.append(el('option', { value: '-1' }, 'Your own choices'));
  D.presets.forEach((p, i) => ps.append(el('option', { value: String(i) }, p.name + since(p))));
  const box = $('avFields');
  for (const fname of FIELD_ORDER) {
    const f = field(fname);
    const id = 'f_' + KEY[fname];
    const row = el('div', { class: 'row' });
    row.append(el('label', { for: id }, fname.charAt(0) + fname.slice(1).toLowerCase()));
    const s = el('select', { id });
    f.options.forEach((o, i) => {
      const vid = fname === 'COUNTRY' ? (i && D.nations[i - 1] ? D.nations[i - 1].id : '') : idFor(fname, i);
      s.append(el('option', { value: String(i) }, (vid ? vid + '  ' : '') + o.name + since(D.avatar.find((a) => a.id === vid))));
    });
    s.addEventListener('change', () => {
      state.look[KEY[fname]] = +s.value;
      $('preset').value = '-1';
      if (fname === 'COUNTRY') syncAvatarPanel();   // (COUNTRY KIT's row, and the rows a country decides)
      state.item = idFor(fname, +s.value) || null;
      const e = D.avatar.find((a) => a.id === state.item);
      route(state.item, { keepCamera: !e || e.view === state.focus, fromPanel: true });
    });
    row.append(s);
    if (KIT_ROWS.includes(KEY[fname])) row.append(el('div', { class: 'locked', 'data-k': KEY[fname], hidden: '' }));
    box.append(row);
    if (fname === 'COUNTRY') {   // COUNTRY KIT (0.3.22): under the country, as in the game's builder
      const kr = el('div', { class: 'row', id: 'kitRow' });
      kr.append(el('label', { for: 'f_kit' }, 'Country kit (from 0.3.22)'));   // (said, while 0.3.21 is the release)
      const ks = el('select', { id: 'f_kit' });
      ks.append(el('option', { value: '0' }, 'Team jersey'), el('option', { value: '1' }, 'My outfit (its colours and flags)'));
      ks.addEventListener('change', () => {
        state.look.kit = +ks.value;
        $('preset').value = '-1';
        syncAvatarPanel();
        showAvatar(true);
      });
      kr.append(ks);
      box.append(kr);
    }
  }
  const cbox = $('avColors');
  for (const [fname, key] of COLOR_FIELDS) {
    const f = field(fname);
    const row = el('div', { class: 'row' });
    row.append(el('label', {}, fname.charAt(0) + fname.slice(1).toLowerCase()));
    const sw = el('div', { class: 'swatches', role: 'group', 'aria-label': fname.toLowerCase() });
    f.options.forEach((o, i) => {
      const team = (fname === 'GLOW' && i === 0) || (fname === 'SHIELD TINT' && i === f.options.length - 1) || (fname === 'HAIR COLOR' && i === f.options.length - 1);
      const b = el('button', { type: 'button', class: 'swatch' + (team ? ' team' : ''), title: o.name + (team ? ' (the team colour)' : ''),
        'aria-label': o.name, 'data-k': key, 'data-i': String(i) });
      b.style.background = css(team ? teamColor(state.look, state.team) : o.rgb);
      b.addEventListener('click', () => { state.look[key] = i; $('preset').value = '-1'; syncAvatarPanel(); showAvatar(true); });
      sw.append(b);
    });
    row.append(sw);
    row.append(el('div', { class: 'locked', 'data-k': key }));
    cbox.append(row);
  }
  ps.addEventListener('change', () => {
    if (+ps.value < 0) return;
    state.look = Object.assign(defaultLook(), D.presets[+ps.value].look);
    state.item = null;
    syncAvatarPanel();
    showAvatar(true);
    setHash('');
  });
  if (D.anims && !$('pose').querySelector('optgroup')) {   // (0.3.26) the ANIMATIONS, grouped, after the poses
    let og = null, grp = '';
    for (const a of D.anims.anims) {
      if (a.group !== grp) {
        og = document.createElement('optgroup');
        og.label = 'Animation: ' + a.group.charAt(0) + a.group.slice(1).toLowerCase();
        $('pose').append(og);
        grp = a.group;
      }
      const o = document.createElement('option');
      o.value = 'anim:' + a.id;
      o.textContent = a.name;
      og.append(o);
    }
  }
  $('pose').addEventListener('change', () => { state.pose = $('pose').value; showAvatar(true); });
  $('team').addEventListener('change', () => { state.team = +$('team').value; syncAvatarPanel(); rebuild(true); });
  $('random').addEventListener('click', () => {
    const L = {};
    for (const fname of FIELD_ORDER) L[KEY[fname]] = Math.floor(Math.random() * field(fname).options.length);
    for (const [fname, key] of COLOR_FIELDS) L[key] = Math.floor(Math.random() * field(fname).options.length);
    if (Math.random() < 0.5) L.headgear = 0;
    if (Math.random() < 0.6) L.mask = 0;
    if (Math.random() < 0.4) L.visor = 0;
    if (Math.random() < 0.85) L.nation = 0;   // (a country now and then: its kit covers the suit)
    state.look = Object.assign(defaultLook(), L);
    state.item = null;
    $('preset').value = '-1';
    syncAvatarPanel();
    showAvatar(true);
    setHash('');
  });
}

// with a country the game's builder shows the rows it decides as TEAM KIT (MY OUTFIT: only the colours, FLAG COLORS)
const KIT_ROWS = ['suit', 'pattern', 'lower'], KIT_COLORS = ['primary', 'secondary', 'glow'];
function syncAvatarPanel() {
  for (const fname of FIELD_ORDER) $('f_' + KEY[fname]).value = String(state.look[KEY[fname]]);
  const nat = !!nationOf(state.look), own = nat && !!state.look.kit;
  $('kitRow').hidden = !nat;
  $('f_kit').value = String(own ? 1 : 0);
  document.querySelectorAll('.locked').forEach((d) => {   // (in place of the choice: its select, or its swatches)
    const colours = KIT_COLORS.includes(d.dataset.k);
    const on = nat && (colours || (!own && KIT_ROWS.includes(d.dataset.k)));
    d.hidden = !on;
    d.textContent = !on ? '' : colours ? (own ? 'Flag colors: the country decides these' : 'Team kit: the country decides these')
      : 'Team kit (from 0.3.22, Country kit: My outfit chooses your own)';
    d.previousElementSibling.hidden = on;
  });
  document.querySelectorAll('.swatch').forEach((b) => {
    b.setAttribute('aria-pressed', String(state.look[b.dataset.k] === +b.dataset.i));
    if (b.classList.contains('team')) b.style.background = css(teamColor(state.look, state.team));
  });
}

function itemButton(id, name, meta, onClick) {
  const b = el('button', { type: 'button', class: 'item', 'data-id': id });
  b.append(el('span', { class: 'iid' }, id), el('span', { class: 'nm' }, name), el('span', { class: 'meta' }, meta || ''));
  b.addEventListener('click', onClick);
  return b;
}

function buildList() {
  const L = $('list');
  L.replaceChildren();
  const s = state.section;
  $('pList').hidden = s === 'avatar';
  $('pAvatar').hidden = s !== 'avatar';
  $('pView').hidden = s === 'tex' || s === 'snd' || s === 'code' || s === 'flags';
  $('bonesRow').hidden = s !== 'avatar';
  $('focus').querySelectorAll('[data-f="head"],[data-f="hands"],[data-f="full"]').forEach((b) => { b.hidden = s !== 'avatar'; });
  if (s === 'props') {
    $('listTitle').textContent = 'Props and gauntlets';
    for (const p of D.props) L.append(itemButton(p.id, p.name, kv(p.models), () => route(p.id)));
  } else if (s === 'arena') {
    $('listTitle').textContent = 'Arena layers (tick to show)';
    const all = itemButton('A', 'Everything together', '', () => route('A'));
    L.append(all);
    for (const a of D.arena) {
      const row = el('div', { style: 'display:grid;grid-template-columns:auto minmax(0,1fr);gap:6px;align-items:center' });
      const cb = el('input', { type: 'checkbox', id: 'lay_' + a.id, 'aria-label': 'Show ' + a.name });
      cb.checked = state.layers.has(a.id);
      cb.addEventListener('change', () => { if (cb.checked) state.layers.add(a.id); else state.layers.delete(a.id); state.item = 'A'; showArena(true); });
      row.append(cb, itemButton(a.id, a.name, kv([a.model]), () => route(a.id)));
      L.append(row);
    }
  } else if (s === 'tex') {
    $('listTitle').textContent = 'Textures';
    for (const t of D.tex) if (D.textures[t.file]) L.append(itemButton(t.id, t.name, D.textures[t.file].join('×'), () => route(t.id)));
    L.append(el('h2', { style: 'margin-top:10px' }, 'Every texture file'));
    for (const f of Object.keys(D.textures).sort()) {
      if (D.tex.some((t) => t.file === f)) continue;
      L.append(itemButton('', f, D.textures[f].join('×'), () => route('tex:' + f)));
    }
  } else if (s === 'snd') {
    $('listTitle').textContent = 'Sounds (press to play)';
    for (const x of D.sounds) L.append(itemButton(x.id, x.name, x.licensed ? 'open set' : '', () => route(x.id)));
  } else if (s === 'code') {
    $('listTitle').textContent = 'Effects drawn in code';
    for (const c of D.code) L.append(itemButton(c.id, c.name, '', () => route(c.id)));
  } else if (s === 'flags') {
    $('listTitle').textContent = 'Country flags (the team kits)';
    for (const N of D.nations) L.append(itemButton(N.id, titleCase(N.name), N.code, () => route(N.id)));
  }
  markCurrent();
}
function kv(models) {
  const n = models.reduce((a, m) => a + (D.models[m] ? D.models[m].kb : 0), 0);
  return n > 1024 ? (n / 1024).toFixed(1) + ' MB' : n + ' kB';
}
function markCurrent() {
  document.querySelectorAll('.item').forEach((b) => b.setAttribute('aria-current', String(!!state.item && b.dataset.id === state.item)));
  showDownload();
}

// ---------------------------------------------------------------- downloads (the GitHub page; the Claude artifact's frame
// refuses downloads, so there it links to that page)
const size = (kb) => (kb >= 1024 ? (kb / 1024).toFixed(1) + ' MB' : kb + ' kB');
function showDownload() {
  const box = $('dlBox');
  if (!box || !D) return;
  box.replaceChildren();
  const id = state.item;
  if (!D.dl) {
    const p = el('p', { class: 'hint' }, 'Downloads are on the GitHub page: ');
    p.append(el('a', { href: D.dlHome + (id && !id.startsWith('tex:') ? '#' + id : ''), target: '_blank', rel: 'noopener' }, 'open it there'));
    box.append(p);
    return;
  }
  const d = id && D.dl[id];
  if (d) {
    box.append(el('a', { class: 'dlbtn', href: d.f, download: d.name }, 'Download ' + (id.startsWith('tex:') ? id.slice(4) : id)));
    box.append(el('p', { class: 'hint' }, `${d.name} · ${d.what} · ${size(d.kb)}`));
    return;
  }
  const a = state.section === 'avatar' && D.avatar.find((x) => x.id === id);
  const why = a && a.field === 'BUILD' ? 'A build sizes the body: it has no file of its own. The template pack has the bodies.'
    : state.section === 'avatar' ? 'Pick a part in the list (its V ID) to download it.'
    : state.section === 'arena' ? 'Pick one layer (A01 to A10) to download it.'
    : state.section === 'code' ? 'Drawn by the game\'s code: there is no file to download.'
    : 'Pick an item to download it.';
  box.append(el('p', { class: 'hint' }, why));
}
function setupDownloads() {
  const t = D.dl && D.dl.templates;
  if (t) {
    $('tplMeta').textContent = `(${size(t.kb)}, made from the game ${t.version}'s bodies)`;
  } else {
    $('tplLink').removeAttribute('download');
    $('tplLink').setAttribute('href', D.dlHome);
    $('tplLink').setAttribute('target', '_blank');
    $('tplLink').setAttribute('rel', 'noopener');
    $('tplMeta').textContent = '(on the GitHub page)';
  }
}

// ---------------------------------------------------------------- props, arena, textures, sounds, code effects
function showProp(p, keep) {
  title(p.id, p.name);
  showFlat(false);
  const gl = D.gauntletLocal;
  const place = {
    matrix: (n) => (n.endsWith('_hand') ? gl.fist : n.endsWith('_cuff') ? gl.cuff : null),
    ctx: (n) => (n === 'knuckleguard' ? { channels: null, alphaMul: 0.35, forceShader: SH.TRANS, tintMul: D.teams[state.team].map((x) => 0.75 + 0.25 * x) } : {}),
  };
  const ids = {};
  p.models.forEach((m) => { ids[m] = p.id; });
  showModels(p.models, place, keep, ids).then(() => describe(p.desc, p.made));
}

function showArena(keep) {
  const list = state.item && state.item !== 'A' ? D.arena.filter((a) => a.id === state.item) : D.arena.filter((a) => state.layers.has(a.id));
  const one = state.item && state.item !== 'A' ? list[0] : null;
  title(one ? one.id : 'A01–A10', one ? one.name : 'Arena and stadium');
  showFlat(false);
  const ids = {};
  list.forEach((a) => { ids[a.model] = a.id; });
  showModels(list.map((a) => a.model), null, keep, ids).then(() => describe(one ? one.desc : 'The square room as the game draws it, every layer at once. Tick the layers to show or hide them; choose one to see it alone.', one ? one.made : ''));
}

function describe(desc, made) {
  const el2 = $('parts');
  const p = document.createElement('p');
  p.textContent = desc;
  el2.prepend(p);
  if (made) { const m = document.createElement('p'); m.textContent = 'Made by ' + made; el2.append(m); }
}

function showFlat(on) {
  $('flat').hidden = !on;
  canvas.style.visibility = on ? 'hidden' : 'visible';
  if (!on) dirty = true;
}

function showTexture(file, t) {
  ++token;
  showFlat(true);
  const f = $('flat');
  f.replaceChildren();
  const img = el('img', { class: 'tex', alt: (t ? t.name : file) + ' texture', src: 'tex/' + file + '.png' });
  f.append(img);
  const used = Object.entries(D.texUse).filter(([, ts]) => ts.includes(file)).map(([m]) => m);
  f.append(el('div', { class: 'info' }, `${file}.png · ${D.textures[file].join(' × ')} px${used.length ? ' · used by ' + used.slice(0, 8).join(', ') + (used.length > 8 ? ` and ${used.length - 8} more` : '') : ''}`));
  title(t ? t.id : '', t ? t.name : file);
  partsList([]);
  if (t) describe(t.desc, 'tools/gen_open_textures.py');
  note('');
}

let audio = null;
async function showSound(x) {
  ++token;
  showFlat(true);
  const f = $('flat');
  f.replaceChildren();
  const card = el('div', { class: 'card' });
  const h = el('h3');
  h.append(el('span', { class: 'iid' }, x.id), document.createTextNode(x.name));
  card.append(h, el('p', {}, x.desc));
  if (x.licensed) card.append(el('p', {}, `This is the open set's sound (${x.file}). The store game plays ${x.licensed} here instead; licensed sounds are listed in NOTICE.txt and are not in the public files.`));
  const play = el('button', { type: 'button' }, 'Play ' + x.id);
  card.append(play);
  const cv = el('canvas', { width: '900', height: '180', 'aria-label': 'Waveform of ' + x.name });
  f.append(card, cv);
  play.addEventListener('click', () => {
    if (audio) audio.pause();
    audio = new Audio('snd/' + x.file);
    audio.play().catch(() => {});
  });
  title(x.id, x.name);
  partsList([{ id: x.id, file: 'audio/' + x.file, v: 0, t: 0, note: '' }].map((r) => Object.assign(r, { v: 0 })));
  $('parts').replaceChildren(el('p', {}, `app/assets_open/audio/${x.file} · the event "${x.event}"`));
  note('');
  try {
    const buf = await (await fetch('snd/' + x.file)).arrayBuffer();
    const ac = new (window.OfflineAudioContext || window.webkitOfflineAudioContext)(1, 44100, 44100);
    const ab = await ac.decodeAudioData(buf);
    const d = ab.getChannelData(0), g = cv.getContext('2d'), W = cv.width, H = cv.height;
    g.fillStyle = '#0a1018'; g.fillRect(0, 0, W, H);
    g.strokeStyle = '#33404d'; g.beginPath(); g.moveTo(0, H / 2); g.lineTo(W, H / 2); g.stroke();
    g.fillStyle = '#f8bd5c';
    for (let x2 = 0; x2 < W; x2++) {
      const a = Math.floor((x2 / W) * d.length), b = Math.floor(((x2 + 1) / W) * d.length);
      let lo = 0, hi = 0;
      for (let i = a; i < b; i++) { if (d[i] < lo) lo = d[i]; if (d[i] > hi) hi = d[i]; }
      g.fillRect(x2, H / 2 - hi * H * 0.46, 1, Math.max(1, (hi - lo) * H * 0.46));
    }
    g.fillStyle = '#aabac9'; g.font = '12px system-ui'; g.fillText(ab.duration.toFixed(2) + ' s', 8, 16);
  } catch (e) { /* (the waveform is optional) */ }
}

function titleCase(s) { return s.toLowerCase().replace(/(^|[\s-])\S/g, (m) => m.toUpperCase()); }
const KIT_PART = ['jersey', 'trim', 'accent'];
function showFlag(N) {
  ++token;
  showFlat(true);
  const f = $('flat');
  f.replaceChildren();
  f.append(el('img', { class: 'tex flag', alt: 'The flag of ' + titleCase(N.name), src: 'tex/' + N.flag + '.png' }));
  const card = el('div', { class: 'card' });
  const h = el('h3');
  h.append(el('span', { class: 'iid' }, N.id), document.createTextNode(titleCase(N.name)));
  card.append(h, el('p', {}, `${N.code} · country ${N.n} of ${D.nations.length} in the game's table. With it, a player wears the `
    + 'team kit in its colours, with this flag printed high on the back and small on the chest (or, with COUNTRY KIT: MY '
    + 'OUTFIT, their own outfit in its colours with the flags); their end of the court, its strike zone and the balls take its '
    + 'colour, and their fans\' flags in the stands are this one.'));
  const kit = el('div', { class: 'kit', role: 'group', 'aria-label': 'the team kit colours' });
  N.kit.forEach((c, i) => { const sw = el('span', { title: KIT_PART[i] + ` rgb(${c.join(', ')})` }); sw.style.background = `rgb(${c.join(',')})`; kit.append(sw); });
  const end = el('span', { title: 'its end of the court' });
  end.style.background = css(N.team[0]);
  kit.append(el('em', {}, 'end'), end);
  card.append(kit);
  const wear = el('button', { type: 'button' }, 'See the kit on the avatar');
  wear.addEventListener('click', () => {
    state.look.nation = N.n;
    state.section = 'avatar';
    $('section').value = 'avatar';
    buildList();
    syncAvatarPanel();
    state.focus = 'full';
    route('');
  });
  card.append(wear);
  f.append(card);
  title(N.id, titleCase(N.name));
  $('parts').replaceChildren(el('p', {}, `app/assets_open/tex/${N.flag}.tex · 192 × 128 px · drawn from simple shapes by tools/gen_flags.py`));
  note('');
}

function showCode(c) {
  ++token;
  showFlat(true);
  const f = $('flat');
  f.replaceChildren();
  const card = el('div', { class: 'card' });
  const h = el('h3');
  h.append(el('span', { class: 'iid' }, c.id), document.createTextNode(c.name));
  card.append(h, el('p', {}, c.desc), el('p', {}, 'There is no file to swap: the game draws this itself. Describe a change by its ID.'));
  const code = el('code', {}, c.where);
  card.append(code);
  f.append(card);
  title(c.id, c.name);
  $('parts').replaceChildren(el('p', {}, c.where));
  note('');
}

// ---------------------------------------------------------------- routing: a section, or an ID (#V12, #P04, #S12)
function setHash(id) {
  try { history.replaceState(null, '', id ? '#' + id : location.pathname + location.search); } catch (e) { /* (some hosts refuse it) */ }
}

function sectionOf(id) {
  if (!id) return null;
  if (id.startsWith('tex:')) return 'tex';
  return { V: 'avatar', P: 'props', A: 'arena', T: 'tex', S: 'snd', C: 'code', F: 'flags' }[id[0]] || null;
}

function route(id, opts = {}) {
  $('err').hidden = true;
  id = (id || '').trim();
  if (/^[vpatsc]\d+$/i.test(id)) id = id[0].toUpperCase() + id.slice(1).padStart(2, '0');
  else if (/^f\d+$/i.test(id)) id = 'F' + id.slice(1).padStart(3, '0');
  const sec = sectionOf(id);
  if (sec && sec !== state.section) { state.section = sec; $('section').value = sec; buildList(); }
  state.item = id || null;
  markCurrent();
  if (id && !id.startsWith('tex:') && id !== 'A') setHash(id);
  const s = state.section;
  if (s === 'avatar') {
    const e = D.avatar.find((a) => a.id === id);
    if (e && !opts.fromPanel) { Object.assign(state.look, e.look); syncAvatarPanel(); }
    if (e && !opts.keepCamera) state.focus = e.view;
    showFlat(false);
    return showAvatar(!!opts.keepCamera);
  }
  if (s === 'props') { const p = D.props.find((x) => x.id === id) || D.props[0]; state.item = p.id; markCurrent(); return showProp(p, opts.keepCamera); }
  if (s === 'arena') {
    if (!D.arena.some((a) => a.id === id)) state.item = 'A';
    markCurrent();
    return showArena(opts.keepCamera);
  }
  if (s === 'tex') {
    if (id.startsWith('tex:')) return showTexture(id.slice(4), null);
    const t = D.tex.find((x) => x.id === id) || D.tex[0];
    state.item = t.id; markCurrent();
    return showTexture(t.file, t);
  }
  if (s === 'snd') { const x = D.sounds.find((y) => y.id === id) || D.sounds[0]; state.item = x.id; markCurrent(); return showSound(x); }
  if (s === 'code') { const c = D.code.find((y) => y.id === id) || D.code[0]; state.item = c.id; markCurrent(); return showCode(c); }
  if (s === 'flags') { const N = D.nations.find((y) => y.id === id) || D.nations[0]; state.item = N.id; markCurrent(); return showFlag(N); }
}

function rebuild(keep) {   // the view's settings changed: draw the same thing again
  if (state.section === 'avatar') return showAvatar(keep);
  if (state.section === 'props' || state.section === 'arena') return route(state.item, { keepCamera: keep });
}

// ---------------------------------------------------------------- start
async function start() {
  try {
    D = await (await fetch('data.json')).json();
  } catch (e) {
    $('err').hidden = false;
    $('err').textContent = 'The catalog (data.json) did not load. Reload the page to try again.';
    return;
  }
  state.look = defaultLook();
  state.layers = new Set(D.arena.filter((a) => a.id !== 'A10').map((a) => a.id));
  buildAvatarPanel();
  syncAvatarPanel();
  setupDownloads();
  const legend = $('legend');
  CHANNEL.forEach((c, i) => { const s = el('span', {}, c); s.style.setProperty('--c', css(CHANNEL_SHOW[i])); legend.append(s); });
  $('section').addEventListener('change', () => {
    state.section = $('section').value;
    buildList();
    const first = { avatar: '', props: 'P01', arena: 'A', tex: 'T01', snd: 'S01', code: 'C01', flags: 'F001' }[state.section];
    route(first);
  });
  $('find').addEventListener('keydown', (e) => { if (e.key === 'Enter') { route($('find').value); $('find').select(); } });
  $('surface').addEventListener('change', () => { state.surface = +$('surface').value; $('legend').hidden = state.surface !== 2; rebuild(true); });
  $('wire').addEventListener('change', () => { state.wire = $('wire').checked; rebuild(true); });
  $('ghost').addEventListener('change', () => { state.ghost = $('ghost').checked; rebuild(true); });
  $('bones').addEventListener('change', () => { state.bones = $('bones').checked; rebuild(true); });
  $('grid').addEventListener('change', () => { state.grid = $('grid').checked; grid.visible = state.grid && state.section !== 'arena'; dirty = true; });
  $('views').addEventListener('click', (e) => { const v = e.target.closest('[data-v]'); if (v) turn(v.dataset.v); });
  $('focus').addEventListener('click', (e) => { const f = e.target.closest('[data-f]'); if (f) focus(f.dataset.f); });
  window.addEventListener('hashchange', () => { const h = location.hash.slice(1); if (h && h !== state.item) route(h); });
  buildList();
  resize();
  frame();
  const h = location.hash.slice(1);
  route(h && sectionOf(h) ? h : '');
}
start();
