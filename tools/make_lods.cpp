// make_lods: the lighter triangle lists the game draws for avatars far away (0.3.22; the owner, 2026-11-19, after the
// GPU PROFILE: an avatar is about 66,000 triangles drawn once an eye, three of them 2.9 ms of FIREBALL BLITZ's 8.4).
// For each model given, <model>.lod beside it: each section's triangles simplified to about a quarter, every vertex
// the model's own (so its skin weights, the hair's swing, the tuck maps and the country prints are untouched; only
// a second index list). Quadric edge collapse onto existing vertices: a vertex folds onto a neighbour where that moves
// the surface least. A texture seam's two sides fold together along the seam, an open edge along itself; a vertex
// shared with another section (a material's edge) or in a knot of seams never moves, so no cracks open. A hard
// shading edge (the same place and texture, another normal: pleats, facets) is folded across, each triangle keeping
// the normal nearest its own (far away it doesn't show). A fold that would turn a triangle over is not made; it
// stops at the target or once the next fold would move the surface more than the error limit.
// .lod: "SLOD", version (1: u32 indices; 2: u16), the section count, then each section's first index and count (into
// the list), the index count, the indices. Usage: make_lods [--ratio 0.25] [--error 0.004] MODEL.mdl...
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct V3 { double x = 0, y = 0, z = 0; };
static V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static double len(V3 a) { return std::sqrt(dot(a, a)); }

struct Quadric {
    double q[10] = {};   // a00 a01 a02 a03 a11 a12 a13 a22 a23 a33 of the symmetric 4x4
    double w = 0;        // (the weight in it: the error is read as a mean squared distance)
    void plane(V3 n, double d, double wt) {
        const double v[4] = {n.x, n.y, n.z, d};
        int k = 0;
        for (int i = 0; i < 4; ++i)
            for (int j = i; j < 4; ++j) q[k++] += wt * v[i] * v[j];
        w += wt;
    }
    void add(const Quadric& o) { for (int i = 0; i < 10; ++i) q[i] += o.q[i]; w += o.w; }
    double eval(V3 p) const {
        const double x = p.x, y = p.y, z = p.z;
        const double e = q[0] * x * x + 2 * q[1] * x * y + 2 * q[2] * x * z + 2 * q[3] * x + q[4] * y * y + 2 * q[5] * y * z +
                         2 * q[6] * y + q[7] * z * z + 2 * q[8] * z + q[9];
        return std::max(0.0, e) / std::max(w, 1e-12);
    }
};

enum Kind { K_INNER, K_BORDER, K_SEAM, K_LOCKED };
enum EdgeType { E_INNER = 0, E_BORDER = 1, E_SEAM = 2, E_BAD = 3 };

static uint64_t ekey(uint32_t a, uint32_t b) { return a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a; }

struct Mesh {
    std::vector<uint32_t> canonOf;   // each vertex's place (welded by position)
    std::vector<uint32_t> texOf;     // each vertex's place and texture position (a texture seam: two of these at a place)
    std::vector<V3> cpos;            // each place's position
    std::vector<V3> normal;          // each vertex's normal
};

// one section's triangles (vertex indices), simplified
static std::vector<uint32_t> simplify(const std::vector<uint32_t>& tris, const Mesh& M, const std::unordered_set<uint32_t>& lockedOutside,
                                      double ratio, double maxErr) {
    const std::vector<uint32_t>& canonOf = M.canonOf;
    const std::vector<uint32_t>& texOf = M.texOf;
    const std::vector<V3>& cpos = M.cpos;
    const size_t nt = tris.size() / 3;
    const size_t target = std::max<size_t>(std::min<size_t>(nt, 24), size_t(nt * ratio));
    if (nt <= target) return tris;
    std::vector<uint32_t> t = tris;   // vertex indices, changed as folds happen
    std::vector<uint8_t> alive(nt, 1);
    // each place's triangles, its texture positions, its vertices
    std::unordered_map<uint32_t, std::vector<uint32_t>> adj;
    std::unordered_map<uint32_t, std::unordered_set<uint32_t>> texs, acts;
    for (size_t i = 0; i < nt; ++i)
        for (int k = 0; k < 3; ++k) {
            const uint32_t x = t[i * 3 + k], c = canonOf[x];
            adj[c].push_back((uint32_t)i);
            texs[c].insert(texOf[x]);
            acts[c].insert(x);
        }
    // edges: how many triangles, and whether the two sides have the same texture positions at its ends (not: a seam)
    struct EInfo { int n = 0; uint32_t a0 = 0, b0 = 0; bool seam = false; };
    std::unordered_map<uint64_t, EInfo> edges;
    for (size_t i = 0; i < nt; ++i)
        for (int k = 0; k < 3; ++k) {
            const uint32_t va = t[i * 3 + k], vb = t[i * 3 + (k + 1) % 3];
            const uint32_t ca = canonOf[va], cb = canonOf[vb];
            if (ca == cb) continue;
            EInfo& e = edges[ekey(ca, cb)];
            const uint32_t lo = ca < cb ? texOf[va] : texOf[vb], hi = ca < cb ? texOf[vb] : texOf[va];
            if (e.n == 0) { e.a0 = lo; e.b0 = hi; }
            else if (e.a0 != lo || e.b0 != hi) e.seam = true;
            ++e.n;
        }
    std::unordered_map<uint64_t, int> etype;
    for (auto& kv : edges) etype[kv.first] = kv.second.n == 1 ? E_BORDER : kv.second.n > 2 ? E_BAD : kv.second.seam ? E_SEAM : E_INNER;
    std::unordered_map<uint32_t, int> kind;
    for (auto& kv : adj) {
        const uint32_t c = kv.first;
        int nb = 0, ns = 0, bad = 0;
        std::unordered_set<uint32_t> seen;
        for (uint32_t ti : kv.second)
            for (int k = 0; k < 3; ++k) {
                const uint32_t o = canonOf[t[ti * 3 + k]];
                if (o == c || !seen.insert(o).second) continue;
                const int ty = etype[ekey(c, o)];
                nb += ty == E_BORDER; ns += ty == E_SEAM; bad += ty == E_BAD;
            }
        const size_t nw = texs[c].size();
        int kd;
        if (bad || lockedOutside.count(c)) kd = K_LOCKED;
        else if (nb == 0 && ns == 0) kd = nw == 1 ? K_INNER : K_LOCKED;
        else if (nb == 2 && ns == 0 && nw == 1) kd = K_BORDER;
        else if (ns == 2 && nb == 0 && nw == 2) kd = K_SEAM;
        else kd = K_LOCKED;
        kind[c] = kd;
    }
    // quadrics: each triangle's plane (by its area); along open edges and seams a plane standing on the edge, so they
    // keep their line
    std::unordered_map<uint32_t, Quadric> Q;
    for (size_t i = 0; i < nt; ++i) {
        const V3 p0 = cpos[canonOf[t[i * 3]]], p1 = cpos[canonOf[t[i * 3 + 1]]], p2 = cpos[canonOf[t[i * 3 + 2]]];
        V3 n = cross(p1 - p0, p2 - p0);
        const double l = len(n), area = 0.5 * l;
        if (l < 1e-20) continue;
        n = {n.x / l, n.y / l, n.z / l};
        const double d = -dot(n, p0);
        for (int k = 0; k < 3; ++k) Q[canonOf[t[i * 3 + k]]].plane(n, d, area);
        for (int k = 0; k < 3; ++k) {
            const uint32_t ca = canonOf[t[i * 3 + k]], cb = canonOf[t[i * 3 + (k + 1) % 3]];
            const int ty = etype[ekey(ca, cb)];
            if (ty != E_BORDER && ty != E_SEAM) continue;
            const V3 e = cpos[cb] - cpos[ca];
            V3 bn = cross(e, n);
            const double bl = len(bn);
            if (bl < 1e-20) continue;
            bn = {bn.x / bl, bn.y / bl, bn.z / bl};
            const double bw = (ty == E_BORDER ? 10.0 : 4.0) * dot(e, e);
            Q[ca].plane(bn, -dot(bn, cpos[ca]), bw);
            Q[cb].plane(bn, -dot(bn, cpos[ca]), bw);
        }
    }
    struct Cand { double cost; uint32_t u, v; uint32_t vu, vv; bool operator<(const Cand& o) const { return cost > o.cost; } };
    std::priority_queue<Cand> heap;
    std::unordered_map<uint32_t, uint32_t> ver;
    auto allowed = [&](uint32_t u, uint32_t v) {
        const int ku = kind[u];
        if (ku == K_LOCKED) return false;
        const auto it = etype.find(ekey(u, v));
        const int ty = it != etype.end() ? it->second : E_INNER;
        if (ku == K_INNER) return ty == E_INNER;
        if (ku == K_BORDER) return ty == E_BORDER;
        return ty == E_SEAM;   // (K_SEAM)
    };
    auto pushPair = [&](uint32_t u, uint32_t v) {
        if (allowed(u, v)) heap.push({Q[u].eval(cpos[v]), u, v, ver[u], ver[v]});
        if (allowed(v, u)) heap.push({Q[v].eval(cpos[u]), v, u, ver[v], ver[u]});
    };
    for (auto& kv : edges) pushPair(uint32_t(kv.first >> 32), uint32_t(kv.first & 0xffffffffu));
    size_t live = nt;
    std::unordered_set<uint32_t> dead;
    const double maxErr2 = maxErr * maxErr;
    while (live > target && !heap.empty()) {
        const Cand c = heap.top();
        heap.pop();
        if (dead.count(c.u) || dead.count(c.v) || ver[c.u] != c.vu || ver[c.v] != c.vv) continue;
        if (c.cost > maxErr2) break;
        const uint32_t u = c.u, v = c.v;
        // which of v's vertices each of u's becomes: the texture side as the triangles on the edge have it, then the
        // vertex there with the nearest normal (the triangle's own on the edge)
        std::unordered_map<uint32_t, uint32_t> texMap, direct;
        std::vector<uint32_t> keep;   // (u's triangles not on the edge)
        for (uint32_t ti : adj[u]) {
            if (!alive[ti]) continue;
            int iu = -1, iv = -1;
            for (int k = 0; k < 3; ++k) {
                const uint32_t ck = canonOf[t[ti * 3 + k]];
                if (ck == u) iu = k;
                if (ck == v) iv = k;
            }
            if (iu < 0) continue;
            if (iv >= 0) {
                texMap[texOf[t[ti * 3 + iu]]] = texOf[t[ti * 3 + iv]];
                direct[t[ti * 3 + iu]] = t[ti * 3 + iv];
            } else keep.push_back(ti);
        }
        bool ok = true;
        std::unordered_map<uint32_t, uint32_t> remap;
        for (uint32_t x : acts[u]) {
            auto d = direct.find(x);
            if (d != direct.end()) { remap[x] = d->second; continue; }
            auto tm = texMap.find(texOf[x]);
            if (tm == texMap.end()) { ok = false; break; }   // (a side with no way across)
            double best = -2;
            uint32_t pick = 0;
            for (uint32_t y : acts[v])
                if (texOf[y] == tm->second) {
                    const double s = dot(M.normal[x], M.normal[y]);
                    if (s > best) { best = s; pick = y; }
                }
            if (best < -1.5) { ok = false; break; }
            remap[x] = pick;
        }
        // no triangle turned over, none made a sliver
        for (uint32_t ti : keep) {
            if (!ok) break;
            V3 p[3], q[3];
            for (int k = 0; k < 3; ++k) {
                const uint32_t ck = canonOf[t[ti * 3 + k]];
                p[k] = cpos[ck];
                q[k] = ck == u ? cpos[v] : cpos[ck];
            }
            const V3 n0 = cross(p[1] - p[0], p[2] - p[0]), n1 = cross(q[1] - q[0], q[2] - q[0]);
            const double l0 = len(n0), l1 = len(n1);
            if (l1 < 1e-14 || dot(n0, n1) < 0.25 * l0 * l1) ok = false;
        }
        if (!ok) continue;
        // fold
        for (uint32_t ti : adj[u]) {
            if (!alive[ti]) continue;
            bool hasV = false;
            for (int k = 0; k < 3; ++k) hasV |= canonOf[t[ti * 3 + k]] == v;
            if (hasV) { alive[ti] = 0; --live; continue; }
            for (int k = 0; k < 3; ++k)
                if (canonOf[t[ti * 3 + k]] == u) t[ti * 3 + k] = remap[t[ti * 3 + k]];
            adj[v].push_back(ti);
        }
        // u's edges become v's (an open edge or seam stays one)
        std::unordered_set<uint32_t> nbrs;
        for (uint32_t ti : adj[v]) {
            if (!alive[ti]) continue;
            for (int k = 0; k < 3; ++k) { const uint32_t ck = canonOf[t[ti * 3 + k]]; if (ck != v) nbrs.insert(ck); }
        }
        for (uint32_t w : nbrs) {
            const uint64_t ku = ekey(u, w), kv = ekey(v, w);
            auto iu = etype.find(ku);
            auto iv = etype.find(kv);
            if (iu != etype.end() && (iv == etype.end() || iv->second == E_INNER)) etype[kv] = iu->second;
            else if (iv == etype.end()) etype[kv] = E_INNER;
        }
        dead.insert(u);
        Q[v].add(Q[u]);
        ++ver[v];   // (v's quadric grew: its folds costed again, both ways)
        for (uint32_t w : nbrs) pushPair(v, w);
        // (the neighbours' other edges: their costs still hold, the vertices there didn't move; a fold that would now
        // turn a triangle over is caught when it comes up)
    }
    std::vector<uint32_t> out;
    out.reserve(live * 3);
    for (size_t i = 0; i < nt; ++i) if (alive[i]) for (int k = 0; k < 3; ++k) out.push_back(t[i * 3 + k]);
    return out;
}

static bool readFile(const char* p, std::vector<uint8_t>& d) {
    FILE* f = std::fopen(p, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    d.resize((size_t)std::ftell(f));
    std::fseek(f, 0, SEEK_SET);
    const size_t r = std::fread(d.data(), 1, d.size(), f);
    std::fclose(f);
    return r == d.size();
}

int main(int argc, char** argv) {
    double ratio = 0.25, maxErr = 0.004;
    int done = 0;
    size_t trisIn = 0, trisOut = 0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--ratio") && i + 1 < argc) { ratio = std::atof(argv[++i]); continue; }
        if (!std::strcmp(argv[i], "--error") && i + 1 < argc) { maxErr = std::atof(argv[++i]); continue; }
        std::vector<uint8_t> d;
        if (!readFile(argv[i], d) || d.size() < 24 || std::memcmp(d.data(), "SMDL", 4)) { std::fprintf(stderr, "%s: not a model\n", argv[i]); return 1; }
        uint32_t hdr[5];
        std::memcpy(hdr, d.data() + 4, 20);
        const uint32_t nv = hdr[1], ni = hdr[2], ns = hdr[3];
        const size_t vsz = hdr[0] >= 3 ? 52 : 44;
        const uint8_t* verts = d.data() + 24;
        const uint8_t* idx = verts + size_t(nv) * vsz;
        const uint8_t* secs = idx + size_t(ni) * 4;
        if (secs + size_t(ns) * 12 > d.data() + d.size()) { std::fprintf(stderr, "%s: cut short\n", argv[i]); return 1; }
        // the places (welded by position), and each place's texture positions
        Mesh M;
        M.canonOf.resize(nv);
        M.texOf.resize(nv);
        M.normal.resize(nv);
        std::unordered_map<std::string, uint32_t> at, atTex;
        for (uint32_t v = 0; v < nv; ++v) {
            float p[3], n[3], uv[2];
            std::memcpy(p, verts + size_t(v) * vsz, 12);
            std::memcpy(n, verts + size_t(v) * vsz + 12, 12);
            std::memcpy(uv, verts + size_t(v) * vsz + 24, 8);
            M.normal[v] = {n[0], n[1], n[2]};
            const std::string key((const char*)p, 12);
            auto it = at.find(key);
            if (it == at.end()) { at[key] = (uint32_t)M.cpos.size(); M.canonOf[v] = (uint32_t)M.cpos.size(); M.cpos.push_back({p[0], p[1], p[2]}); }
            else M.canonOf[v] = it->second;
            const std::string tkey = key + std::string((const char*)uv, 8);
            auto jt = atTex.find(tkey);
            if (jt == atTex.end()) { const uint32_t id = (uint32_t)atTex.size(); atTex[tkey] = id; M.texOf[v] = id; }
            else M.texOf[v] = jt->second;
        }
        // the places more than one section uses (a material's edge): never moved
        std::unordered_map<uint32_t, int> firstSec;
        std::unordered_set<uint32_t> shared;
        std::vector<uint32_t> first(ns), count(ns);
        for (uint32_t s = 0; s < ns; ++s) {
            std::memcpy(&first[s], secs + s * 12, 4);
            std::memcpy(&count[s], secs + s * 12 + 4, 4);
            for (uint32_t k = first[s]; k < first[s] + count[s] && k < ni; ++k) {
                uint32_t vi;
                std::memcpy(&vi, idx + size_t(k) * 4, 4);
                if (vi >= nv) continue;
                auto it = firstSec.find(M.canonOf[vi]);
                if (it == firstSec.end()) firstSec[M.canonOf[vi]] = (int)s;
                else if (it->second != (int)s) shared.insert(M.canonOf[vi]);
            }
        }
        std::vector<uint32_t> outIdx, outFirst(ns), outCount(ns);
        for (uint32_t s = 0; s < ns; ++s) {
            std::vector<uint32_t> tris;
            for (uint32_t k = first[s]; k + 3 <= first[s] + count[s] && k + 3 <= ni; k += 3) {
                uint32_t tri[3];
                std::memcpy(tri, idx + size_t(k) * 4, 12);
                if (tri[0] >= nv || tri[1] >= nv || tri[2] >= nv) continue;
                tris.insert(tris.end(), tri, tri + 3);
            }
            const std::vector<uint32_t> o = simplify(tris, M, shared, ratio, maxErr);
            outFirst[s] = (uint32_t)outIdx.size();
            outCount[s] = (uint32_t)o.size();
            outIdx.insert(outIdx.end(), o.begin(), o.end());
        }
        std::string op = argv[i];
        op = op.substr(0, op.size() - 4) + ".lod";
        FILE* f = std::fopen(op.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "%s: can't write\n", op.c_str()); return 1; }
        const bool small = nv <= 65535;
        const uint32_t h[2] = {small ? 2u : 1u, ns};
        std::fwrite("SLOD", 1, 4, f);
        std::fwrite(h, 4, 2, f);
        for (uint32_t s = 0; s < ns; ++s) { std::fwrite(&outFirst[s], 4, 1, f); std::fwrite(&outCount[s], 4, 1, f); }
        const uint32_t n = (uint32_t)outIdx.size();
        std::fwrite(&n, 4, 1, f);
        if (small) {
            std::vector<uint16_t> s16(outIdx.begin(), outIdx.end());
            std::fwrite(s16.data(), 2, s16.size(), f);
        } else std::fwrite(outIdx.data(), 4, outIdx.size(), f);
        std::fclose(f);
        trisIn += ni / 3;
        trisOut += outIdx.size() / 3;
        std::printf("%s: %u -> %zu triangles\n", argv[i], ni / 3, outIdx.size() / 3);
        ++done;
    }
    std::printf("%d models: %zu -> %zu triangles (%.0f%%)\n", done, trisIn, trisOut, trisIn ? 100.0 * trisOut / trisIn : 0.0);
    return 0;
}
