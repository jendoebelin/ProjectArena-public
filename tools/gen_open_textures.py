"""Procedural textures for the Project Arena art set (original work, no CCP content).

Writes PNGs to build/open_src/tex (used by the Blender bakes) and .tex files to app/assets_open/tex (used by the app).
Usage: python -I tools/gen_open_textures.py
"""
import os
import struct

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PNG = os.path.join(ROOT, 'build', 'open_src', 'tex')
TEX = os.path.join(ROOT, 'app', 'assets_open', 'tex')
rng = np.random.default_rng(2026)


def save(name, rgb, alpha=None):
    """rgb: float HxWx3 in 0..1 (display / sRGB values). alpha defaults to max(rgb)."""
    rgb = np.clip(rgb, 0, 1)
    if alpha is None:
        alpha = rgb.max(axis=2)
    img = np.dstack([rgb, np.clip(alpha, 0, 1)])
    u8 = (img * 255 + 0.5).astype(np.uint8)
    os.makedirs(PNG, exist_ok=True)
    os.makedirs(TEX, exist_ok=True)
    Image.fromarray(u8, 'RGBA').save(os.path.join(PNG, name + '.png'))
    h, w = u8.shape[:2]
    with open(os.path.join(TEX, 'open_' + name + '.tex'), 'wb') as f:
        f.write(b'STEX' + struct.pack('<3I', w, h, 0))
        f.write(u8.tobytes())
    print('  %-14s %4dx%-4d' % (name, w, h))


def grid(w, h):
    return np.meshgrid((np.arange(w) + 0.5) / w, (np.arange(h) + 0.5) / h)


def fbm(w, h, octaves=5, base=4, seed=0):
    """Tileable fractal value noise in 0..1."""
    r = np.random.default_rng(seed)
    out = np.zeros((h, w))
    amp, tot = 1.0, 0.0
    for o in range(octaves):
        n = base * 2 ** o
        g = r.random((n, n))
        big = np.tile(g, (3, 3))
        im = Image.fromarray((big * 255).astype(np.uint8)).resize((w * 3, h * 3), Image.BICUBIC)
        a = np.asarray(im, np.float64)[h:2 * h, w:2 * w] / 255.0
        out += a * amp
        tot += amp
        amp *= 0.5
    return out / tot


def fabric():
    """Tileable knit for garments (sampled ~28x across the body UVs): rows of V stitches over a soft heather,
    mean ~0.5 so the shader's 0.82 + 0.36 * value keeps the colour on average."""
    w = h = 256
    cols, rows = 16, 20
    u, v = grid(w, h)
    cu, cv = (u * cols) % 1.0, (v * rows) % 1.0
    lobe = lambda x0, slope: np.exp(-((cu - x0 - slope * (cv - 0.5)) / 0.16) ** 2)
    loops = np.maximum(lobe(0.30, 0.35), lobe(0.70, -0.35)) * (0.55 + 0.45 * np.sin(np.pi * cv))
    knit = 0.25 + 0.75 * loops
    heather = fbm(w, h, octaves=4, base=8, seed=11)
    val = 0.5 + 0.34 * (knit - knit.mean()) + 0.30 * (heather - heather.mean())
    save('fabric', np.dstack([val] * 3), np.ones_like(val))


def soft_strip():
    u, v = grid(64, 64)
    a = np.exp(-((u - 0.5) / 0.17) ** 2)
    save('soft_strip', np.dstack([a] * 3))


def soft_dot():
    u, v = grid(64, 64)
    r = np.hypot(u - 0.5, v - 0.5) * 2
    a = np.exp(-r * r * 5.5) * (r < 1)
    save('soft_dot', np.dstack([a] * 3))


def holo_ring():
    u, v = grid(512, 512)
    r = np.hypot(u - 0.5, v - 0.5) * 2
    ang = np.arctan2(v - 0.5, u - 0.5)
    a = np.zeros_like(r)
    for rr, w, k in ((0.95, 0.012, 1.0), (0.88, 0.006, 0.6), (0.62, 0.010, 0.8), (0.30, 0.012, 0.9)):
        a += k * np.exp(-((r - rr) / w) ** 2)
    ticks = (np.cos(ang * 48) > 0.85) * np.exp(-((r - 0.78) / 0.03) ** 2) * 0.7
    seg = ((np.cos(ang * 6) > 0.0) * np.exp(-((r - 0.70) / 0.008) ** 2)) * 0.7
    glow = np.exp(-((r - 0.62) / 0.12) ** 2) * 0.12 + np.exp(-(r / 0.25) ** 2) * 0.12
    a = (a + ticks + seg + glow) * (r < 1)
    save('holo_ring', np.dstack([a] * 3))


def hex_grid():
    """Hexagon line grid, tileable: 4 cells across, 2 * sqrt(3) * ... cells down (texture aspect 1 : sqrt(3))."""
    w, h = 512, 886
    u, v = grid(w, h)
    px, py = u * 4.0, v * 4.0 * np.sqrt(3)   # texture covers 4 x 4*sqrt(3) units: 4 hex periods each way
    sx, sy = 1.0, np.sqrt(3)
    ax, ay = np.mod(px, sx) - sx / 2, np.mod(py, sy) - sy / 2
    bx, by = np.mod(px - sx / 2, sx) - sx / 2, np.mod(py - sy / 2, sy) - sy / 2
    use_a = ax * ax + ay * ay < bx * bx + by * by
    gx, gy = np.where(use_a, ax, bx), np.where(use_a, ay, by)
    gx, gy = np.abs(gx), np.abs(gy)
    d = np.maximum(gx * 0.5 + gy * np.sqrt(3) / 2, gx)      # 0 at the centre, 0.5 on the edge
    line = np.exp(-((d - 0.5) / 0.022) ** 2) + 0.3 * np.exp(-((d - 0.5) / 0.08) ** 2)
    cid = np.where(use_a, np.floor(px / sx) * 3.1 + np.floor(py / sy) * 7.7, np.floor((px - .5) / sx) * 5.3 + np.floor((py - sy / 2) / sy) * 2.9)
    fill = 0.05 + 0.10 * (np.sin(cid * 12.9898) * 0.5 + 0.5) ** 3
    a = np.clip(line + fill * (d < 0.47), 0, 1)
    save('hex', np.dstack([a] * 3))


def slats():
    """Stadium light slats (bright vertical bars with soft edges), tileable in u."""
    w, h = 512, 256
    u, v = grid(w, h)
    nb = 16
    f = (u * nb) % 1.0
    bar = np.clip((0.5 - np.abs(f - 0.5)) / 0.5, 0, 1)
    bar = np.clip((bar - 0.18) / 0.10, 0, 1)
    idx = np.floor(u * nb).astype(int)
    var = 0.82 + 0.18 * rng.random(nb)[idx]
    vert = np.clip((0.5 - np.abs(v - 0.5)) / 0.5, 0, 1)
    vert = np.clip(vert / 0.08, 0, 1) * (0.85 + 0.15 * (1 - v))
    a = (0.10 + 0.90 * bar) * var * vert
    col = np.dstack([a * 0.92, a * 0.97, a * 1.0])
    save('slats', col)


def sky():
    w, h = 2048, 1024
    u, v = grid(w, h)
    elev = (0.5 - v) * np.pi            # +pi/2 top .. -pi/2 bottom
    s = np.sin(elev)
    base = np.dstack([0.004 + 0.016 * np.exp(-np.abs(s) * 6), 0.006 + 0.028 * np.exp(-np.abs(s) * 6),
                      0.014 + 0.050 * np.exp(-np.abs(s) * 5)])
    n1 = fbm(w, h, 6, 3, seed=11)
    n2 = fbm(w, h, 6, 5, seed=12)
    neb = np.clip((n1 - 0.42) * 3.0, 0, 1) ** 1.6 * np.clip(s + 0.15, 0, 1)
    base += np.dstack([neb * 0.060 + n2 * neb * 0.06, neb * 0.022, neb * 0.110 + n2 * neb * 0.05])
    base += np.dstack([0.015, 0.03, 0.06]) * np.exp(-np.abs(s - 0.02) * 18)[..., None]   # horizon glow
    stars = np.zeros((h, w))
    n = 9000
    xs, ys = rng.integers(0, w, n), rng.integers(0, int(h * 0.48), n)
    b = rng.random(n) ** 6
    stars[ys, xs] = b
    for dx, dy, k in ((1, 0, .3), (-1, 0, .3), (0, 1, .3), (0, -1, .3)):
        stars += np.roll(np.roll(stars, dx, 1), dy, 0) * k * (stars > 0.5)
    base += np.dstack([stars * 0.9, stars * 0.95, stars])
    save('sky', base, alpha=np.ones((h, w)))


def pulse():
    """Energy pulse travelling along a neon tube: soft across (u), a bright head with a fading tail along (v)."""
    w, h = 64, 256
    u, v = grid(w, h)
    across = np.exp(-((u - 0.5) / 0.16) ** 2)
    f = np.mod(v, 1.0)
    head = np.exp(-((f - 0.9) / 0.012) ** 2)
    tail = np.where(f < 0.9, np.exp(-(0.9 - f) * 9.0), 0.0)
    a = across * np.clip(head + 0.55 * tail, 0, 1)
    save('pulse', np.dstack([a] * 3))


def twinkle():
    """Crowd lights: brightness varies along u (each light samples one u; the app scrolls u over time)."""
    w, h = 1024, 32
    u, v = grid(w, h)
    t = fbm(w, 1, 4, 32, seed=21)[0]
    t = np.clip((t - 0.30) * 3.0, 0.15, 1) ** 1.2
    prof = np.exp(-((v - 0.5) / 0.22) ** 2)
    a = t[None, :] * prof
    save('twinkle', np.dstack([a] * 3))


def veins():
    w = h = 256
    n = fbm(w, h, 5, 4, seed=31)
    m = fbm(w, h, 4, 8, seed=32)
    line = np.exp(-((n - 0.5) / 0.025) ** 2) + 0.6 * np.exp(-((m - 0.5) / 0.03) ** 2)
    a = np.clip(line * 0.9 + 0.12 + 0.15 * n, 0, 1)
    save('veins', np.dstack([a] * 3))


def emblem():
    """Project Arena emblem: an orbit ring around a seamed ball; left half blue, right half orange (the two teams)."""
    w = h = 512
    u, v = grid(w, h)
    x, y = (u - 0.5) * 2, (0.5 - v) * 2
    r = np.hypot(x, y)
    ring = np.exp(-((r - 0.86) / 0.025) ** 2) + 0.5 * np.exp(-((r - 0.74) / 0.012) ** 2)
    ring *= np.clip((np.abs(np.arctan2(y, x) + np.pi / 2) - 0.12) / 0.10, 0, 1)   # soft opening at the bottom
    R = 0.46
    inside = np.clip((R - r) / 0.01, 0, 1)

    outline = np.abs(r - R)
    # two curved seams bowing in from the sides, like a tennis ball (circles centred outside the ball)
    seams = np.minimum(np.abs(np.hypot(x + 0.78, y) - 0.56), np.abs(np.hypot(x - 0.78, y) - 0.56))
    shape = np.exp(-(outline / 0.042) ** 6) + inside * np.exp(-(seams / 0.028) ** 6)
    shape += inside * 0.10   # a faint fill so the ball reads as solid
    a = np.clip(ring + shape, 0, 1)
    left = x < 0
    col = np.dstack([np.where(left, 0.25, 1.0), np.where(left, 0.80, 0.55), np.where(left, 1.0, 0.25)]) * a[..., None]
    col = col * 0.8 + 0.2 * a[..., None]
    save('emblem', col, alpha=a)


def screen():
    """Jumbotron face: emblem on a dark gradient with scanlines and a border."""
    w, h = 512, 256
    u, v = grid(w, h)
    em = np.asarray(Image.open(os.path.join(PNG, 'emblem.png')).resize((200, 200), Image.BICUBIC), np.float64) / 255
    img = np.dstack([0.02 + 0.03 * (1 - v), 0.03 + 0.05 * (1 - v), 0.06 + 0.08 * (1 - v)])
    img *= (0.85 + 0.15 * (np.sin(v * h * np.pi) ** 2))[..., None]
    y0, x0 = (h - 200) // 2, (w - 200) // 2
    img[y0:y0 + 200, x0:x0 + 200] += em[..., :3] * em[..., 3:4]
    border = ((u < 0.012) | (u > 0.988) | (v < 0.024) | (v > 0.976)).astype(float)
    img += border[..., None] * np.array([0.35, 0.55, 0.75])
    save('screen', img, alpha=np.ones((h, w)))


# ---------------------------------------------------------------- avatar suit patterns
# Mask textures on the suits' uv0 (tools/blender/make_avatar.py): u = around the body (0 back, 0.25 right side,
# 0.5 front centre, 0.75 left side), v = 0 at the neck .. 1 at the waist. R = trim-colour regions, G = glow lines.
PW, PH = 512, 256


def _seg_cov(lines, width, closed=False):
    """Anti-aliased coverage of polylines given in uv (0..1), width in pixels; wraps across u."""
    yy, xx = np.mgrid[0:PH, 0:PW].astype(np.float64) + 0.5
    dmin = np.full((PH, PW), 1e9)
    for line in lines:
        pts = [(u * PW, v * PH) for u, v in line]
        for (ax, ay), (bx, by) in zip(pts[:-1], pts[1:]):
            for shift in (-PW, 0, PW):
                dx, dy = bx - ax, by - ay
                L2 = dx * dx + dy * dy
                t = np.clip(((xx - ax - shift) * dx + (yy - ay) * dy) / max(L2, 1e-9), 0, 1)
                d = np.hypot(xx - shift - (ax + dx * t), yy - (ay + dy * t))
                dmin = np.minimum(dmin, d)
    return np.clip(width / 2 + 0.5 - dmin, 0, 1)


def _dots(centres, r_out, r_in=0.0):
    yy, xx = np.mgrid[0:PH, 0:PW].astype(np.float64) + 0.5
    cov = np.zeros((PH, PW))
    for u, v in centres:
        for shift in (-PW, 0, PW):
            d = np.hypot(xx - u * PW - shift, yy - v * PH)
            cov = np.maximum(cov, np.clip(r_out + 0.5 - d, 0, 1) * np.clip(d - r_in + 0.5, 0, 1))
    return cov


def _save_pattern(name, R, G):
    save('pat_' + name, np.dstack([R, G, np.zeros_like(R)]), alpha=np.ones_like(R))


def patterns():
    r = np.random.default_rng(77)
    u, v = grid(PW, PH)
    # CIRCUIT: traces running down from the collar with 45-degree jogs, ending in pads (Tron)
    lines, pads = [], []
    for k in range(26):
        x = (k + 0.5) / 26 + r.uniform(-0.008, 0.008)
        if abs(x - 0.5) < 0.012:
            continue
        y = r.uniform(0.0, 0.18)
        pts = [(x, y)]
        for _ in range(r.integers(2, 4)):
            y2 = min(0.95, y + r.uniform(0.12, 0.32))
            pts.append((x, y2))
            j = r.choice([-1, 1]) * r.uniform(0.015, 0.035)
            x, y = x + j, y2 + abs(j) * 2.0
            pts.append((x, y))
        lines.append(pts)
        pads.append(pts[-1])
    lines.append([(0, 0.04), (1, 0.04)])
    G = np.maximum(_seg_cov(lines, 3.6), _dots(pads, 6.0, 2.6))
    _save_pattern('circuit', np.zeros_like(G), G)
    # STRIPES: twin racing stripes front and back, a pinstripe down each side
    R = np.zeros((PH, PW))
    for c in (0.5, 0.0, 1.0):
        for o in (-0.052, 0.052):
            R = np.maximum(R, (np.abs(u - (c + o)) < 0.017).astype(float))
    for c in (0.25, 0.75):
        R = np.maximum(R, (np.abs(u - c) < 0.006).astype(float))
    G = _seg_cov([[(0.5 + s * 0.035, 0), (0.5 + s * 0.035, 1)] for s in (-1, 1)], 2.2)
    _save_pattern('stripes', R, G)
    # CHEVRON: a broad V across the chest, a smaller echo below it
    def vband(apex_v, spread, thick):
        dv = v - (apex_v - np.abs(u - 0.5) * spread)
        return ((dv > 0) & (dv < thick) & (np.abs(u - 0.5) < 0.26)).astype(float)
    R = np.maximum(vband(0.48, 2.2, 0.10), vband(0.72, 2.2, 0.05))
    edges = []
    for apex, th in ((0.48, 0.10), (0.72, 0.05)):
        for off in (0, th):
            a = apex + off
            edges.append([(0.24, a - 0.26 * 2.2), (0.5, a), (0.76, a - 0.26 * 2.2)])
    G = _seg_cov(edges, 2.4)
    _save_pattern('chevron', R, G)
    # HEX: honeycomb outlines with scattered filled cells
    s = 0.05
    hx, hy = u / s, v * 0.5 / s        # texture spans 1 m around x 0.5 m down: regular cells
    q = (2.0 / 3.0) * hx
    rr = (-1.0 / 3.0) * hx + (np.sqrt(3) / 3.0) * hy
    # cube rounding to the nearest hex
    cx, cz = q, rr
    cy = -cx - cz
    rx, ry, rz = np.round(cx), np.round(cy), np.round(cz)
    dx, dy, dz = np.abs(rx - cx), np.abs(ry - cy), np.abs(rz - cz)
    fixx = (dx > dy) & (dx > dz)
    fixy = ~fixx & (dy > dz)
    rx = np.where(fixx, -ry - rz, rx)
    ry = np.where(fixy, -rx - rz, ry)
    rz = np.where(~fixx & ~fixy, -rx - ry, rz)
    dist = np.maximum(np.maximum(np.abs(rx - cx), np.abs(ry - cy)), np.abs(rz - cz))
    cell = (rx * 7919 + rz * 104729).astype(np.int64)
    filled = ((cell * 2654435761) % 1000) < 280
    R = filled.astype(float) * (dist < 0.40)
    G = np.clip((dist - 0.465) / 0.03, 0, 1) * 0.65
    _save_pattern('hex', R, G)
    # SPLIT: the left half in the trim colour, glowing seams down the front and back centre
    R = ((u > 0.5) & (u < 1.0)).astype(float)
    G = _seg_cov([[(0.5, 0), (0.5, 1)], [(0.0, 0), (0.0, 1)]], 3.0)
    _save_pattern('split', R, G)
    # PIXEL: digital camo in two block sizes
    big = (r.random((PH // 16, PW // 16)) < 0.30).repeat(16, 0).repeat(16, 1)
    small = (r.random((PH // 8, PW // 8)) < 0.18).repeat(8, 0).repeat(8, 1)
    R = np.maximum(big, small).astype(float)
    G = np.zeros_like(R)
    _save_pattern('pixel', R, G)
    # BANDS: a chest band and a waist band, glow lines on their edges
    R = (((v > 0.24) & (v < 0.36)) | ((v > 0.80) & (v < 0.87))).astype(float)
    G = _seg_cov([[(0, y), (1, y)] for y in (0.235, 0.365, 0.795, 0.875)], 2.4)
    _save_pattern('bands', R, G)


def trace_pulse():
    """CIRCUIT RUNNER traces: a faint steady glow along the whole trace, and a pulse with a long tail (the game scrolls
    v). Wider across than pulse() (the strips are only 6 mm wide)."""
    w, h = 32, 256
    u, v = grid(w, h)
    across = np.exp(-((u - 0.5) / 0.34) ** 2)
    f = np.mod(v, 1.0)
    head = np.exp(-((f - 0.88) / 0.03) ** 2)
    tail = np.where(f < 0.88, np.exp(-(0.88 - f) * 7.0), 0.0)
    a = across * np.clip(0.34 + head + 0.6 * tail, 0, 1)   # (sRGB: 0.34 shows as a steady dim glow; the pulse ~5 cm)
    save('trace_pulse', np.dstack([a] * 3))


def pipe_pulse():
    """LIVE suits: their piping glows as brightly as the painted neon it replaces (base 0.62), with the same pulses."""
    w, h = 32, 256
    u, v = grid(w, h)
    across = np.exp(-((u - 0.5) / 0.38) ** 2)
    f = np.mod(v, 1.0)
    head = np.exp(-((f - 0.88) / 0.03) ** 2)
    tail = np.where(f < 0.88, np.exp(-(0.88 - f) * 7.0), 0.0)
    save('pipe_pulse', np.dstack([across * np.clip(0.62 + head + 0.5 * tail, 0, 1)] * 3))


def main():
    print('textures ->', TEX)
    patterns()
    fabric()
    soft_strip()
    soft_dot()
    holo_ring()
    hex_grid()
    slats()
    sky()
    twinkle()
    pulse()
    trace_pulse()
    pipe_pulse()
    veins()
    emblem()
    screen()
    font()


# ---------------------------------------------------------------- Project Arena stroke font
# Glyphs for ' '..'Z' as polylines in a unit box (x 0..1, y 0..1 bottom-up), chamfered "techno" style.
C = 0.2
_O = [(C, 0), (1 - C, 0), (1, C), (1, 1 - C), (1 - C, 1), (C, 1), (0, 1 - C), (0, C), (C, 0)]


def _dot(y):
    return [(0.44, y), (0.56, y)]


GLYPHS = {
    ' ': [],
    '!': [[(0.5, 1), (0.5, 0.32)], _dot(0.04)],
    '"': [[(0.32, 1), (0.32, 0.72)], [(0.68, 1), (0.68, 0.72)]],
    '#': [[(0.3, 0.05), (0.42, 0.95)], [(0.62, 0.05), (0.74, 0.95)], [(0.08, 0.35), (0.92, 0.35)], [(0.12, 0.65), (0.96, 0.65)]],
    '$': [[(1, 0.85), (C, 0.85), (0, 0.65), (C, 0.5), (1 - C, 0.5), (1, 0.35), (1 - C, 0.15), (0, 0.15)], [(0.5, 1), (0.5, 0)]],
    '%': [[(0.05, 0.02), (0.95, 0.98)], [(0.1, 0.85), (0.3, 0.85)], [(0.7, 0.15), (0.9, 0.15)]],
    '&': [[(1, 0), (0.15, 0.75), (0.3, 1), (0.55, 1), (0.65, 0.8), (0, 0.3), (0.15, 0), (0.6, 0), (1, 0.4)]],
    "'": [[(0.5, 1), (0.5, 0.72)]],
    '(': [[(0.68, 1), (0.42, 0.78), (0.42, 0.22), (0.68, 0)]],
    ')': [[(0.32, 1), (0.58, 0.78), (0.58, 0.22), (0.32, 0)]],
    '*': [[(0.5, 0.85), (0.5, 0.15)], [(0.2, 0.7), (0.8, 0.3)], [(0.2, 0.3), (0.8, 0.7)]],
    '+': [[(0.5, 0.2), (0.5, 0.8)], [(0.2, 0.5), (0.8, 0.5)]],
    ',': [[(0.55, 0.1), (0.4, -0.12)]],
    '-': [[(0.15, 0.5), (0.85, 0.5)]],
    '.': [_dot(0.04)],
    '/': [[(0.1, 0), (0.9, 1)]],
    '0': [_O, [(0.3, 0.3), (0.7, 0.7)]],
    '1': [[(0.22, 0.78), (0.55, 1), (0.55, 0)], [(0.22, 0), (0.88, 0)]],
    '2': [[(0, 1 - C), (C, 1), (1 - C, 1), (1, 1 - C), (1, 0.58), (0, 0), (1, 0)]],
    '3': [[(0, 1), (1, 1), (0.42, 0.56), (1 - C, 0.56), (1, 0.56 - C), (1, C), (1 - C, 0), (0, 0)]],
    '4': [[(0.75, 0), (0.75, 1), (0, 0.32), (1, 0.32)]],
    '5': [[(1, 1), (0, 1), (0, 0.56), (1 - C, 0.56), (1, 0.56 - C), (1, C), (1 - C, 0), (0, 0)]],
    '6': [[(1, 1), (C, 1), (0, 1 - C), (0, C), (C, 0), (1 - C, 0), (1, C), (1, 0.56 - C), (1 - C, 0.56), (0, 0.56)]],
    '7': [[(0, 1), (1, 1), (0.38, 0)]],
    '8': [[(C, 0.55), (0, 0.55 + C * 0.8), (0, 1 - C), (C, 1), (1 - C, 1), (1, 1 - C), (1, 0.55 + C * 0.8), (1 - C, 0.55),
           (C, 0.55), (0, 0.55 - C), (0, C), (C, 0), (1 - C, 0), (1, C), (1, 0.55 - C), (1 - C, 0.55)]],
    '9': [[(0, 0), (1 - C, 0), (1, C), (1, 1 - C), (1 - C, 1), (C, 1), (0, 1 - C), (0, 0.44 + C), (C, 0.44), (1, 0.44)]],
    ':': [_dot(0.2), _dot(0.72)],
    ';': [_dot(0.72), [(0.55, 0.2), (0.4, -0.05)]],
    '<': [[(0.85, 0.85), (0.15, 0.5), (0.85, 0.15)]],
    '=': [[(0.15, 0.35), (0.85, 0.35)], [(0.15, 0.65), (0.85, 0.65)]],
    '>': [[(0.15, 0.85), (0.85, 0.5), (0.15, 0.15)]],
    '?': [[(0, 1 - C), (C, 1), (1 - C, 1), (1, 1 - C), (1, 0.66), (0.5, 0.46), (0.5, 0.3)], _dot(0.04)],
    '@': [[(0.7, 0.3), (0.7, 0.7), (0.35, 0.7), (0.35, 0.3), (1, 0.3), (1, 1 - C), (1 - C, 1), (C, 1), (0, 1 - C), (0, C), (C, 0), (1, 0)]],
    'A': [[(0, 0), (0, 1 - C), (C, 1), (1 - C, 1), (1, 1 - C), (1, 0)], [(0, 0.48), (1, 0.48)]],
    'B': [[(0, 0), (0, 1), (0.82, 1), (0.96, 0.86), (0.96, 0.64), (0.82, 0.52), (0, 0.52)],
          [(0.82, 0.52), (1, 0.38), (1, 0.14), (0.86, 0), (0, 0)]],
    'C': [[(1, 1), (C, 1), (0, 1 - C), (0, C), (C, 0), (1, 0)]],
    'D': [[(0, 0), (0, 1), (0.7, 1), (1, 0.7), (1, 0.3), (0.7, 0), (0, 0)]],
    'E': [[(1, 1), (0, 1), (0, 0), (1, 0)], [(0, 0.5), (0.72, 0.5)]],
    'F': [[(1, 1), (0, 1), (0, 0)], [(0, 0.5), (0.72, 0.5)]],
    'G': [[(1, 1), (C, 1), (0, 1 - C), (0, C), (C, 0), (1, 0), (1, 0.46), (0.55, 0.46)]],
    'H': [[(0, 0), (0, 1)], [(1, 0), (1, 1)], [(0, 0.5), (1, 0.5)]],
    'I': [[(0.2, 1), (0.8, 1)], [(0.5, 1), (0.5, 0)], [(0.2, 0), (0.8, 0)]],
    'J': [[(0.3, 1), (1, 1), (1, C), (1 - C, 0), (C, 0), (0, C), (0, 0.28)]],
    'K': [[(0, 0), (0, 1)], [(1, 1), (0.25, 0.5), (1, 0)], [(0, 0.5), (0.25, 0.5)]],
    'L': [[(0, 1), (0, 0), (1, 0)]],
    'M': [[(0, 0), (0, 1), (0.5, 0.52), (1, 1), (1, 0)]],
    'N': [[(0, 0), (0, 1), (1, 0), (1, 1)]],
    'O': [_O],
    'P': [[(0, 0), (0, 1), (1 - C, 1), (1, 1 - C), (1, 0.48 + C), (1 - C, 0.48), (0, 0.48)]],
    'Q': [_O, [(0.62, 0.28), (1.0, -0.08)]],
    'R': [[(0, 0), (0, 1), (1 - C, 1), (1, 1 - C), (1, 0.5 + C), (1 - C, 0.5), (0, 0.5)], [(0.45, 0.5), (1, 0)]],
    'S': [[(1, 1), (C, 1), (0, 1 - C), (0, 0.5 + C), (C, 0.5), (1 - C, 0.5), (1, 0.5 - C), (1, C), (1 - C, 0), (0, 0)]],
    'T': [[(0, 1), (1, 1)], [(0.5, 1), (0.5, 0)]],
    'U': [[(0, 1), (0, C), (C, 0), (1 - C, 0), (1, C), (1, 1)]],
    'V': [[(0, 1), (0.5, 0), (1, 1)]],
    'W': [[(0, 1), (0.2, 0), (0.5, 0.62), (0.8, 0), (1, 1)]],
    'X': [[(0, 0), (1, 1)], [(0, 1), (1, 0)]],
    'Y': [[(0, 1), (0.5, 0.5), (1, 1)], [(0.5, 0.5), (0.5, 0)]],
    'Z': [[(0, 1), (1, 1), (0, 0), (1, 0)]],
}


def font():
    """Anti-aliased stroke font atlas, same layout as the app's built-in 5x7 font (59 cells, ' '..'Z', 6:8 cells)."""
    cw, ch = 48, 64
    W = 59 * cw
    yy, xx = np.mgrid[0:ch, 0:cw].astype(np.float64) + 0.5
    atlas = np.zeros((ch, W))
    x0, x1, y0, y1 = 7.0, 37.0, 57.0, 13.0       # glyph box inside the cell (pixel y grows downward)
    width = 5.2
    for g in range(59):
        strokes = GLYPHS.get(chr(32 + g), [])
        dmin = np.full((ch, cw), 1e9)
        for line in strokes:
            pts = [(x0 + (x1 - x0) * px, y0 - (y0 - y1) * py) for px, py in line]
            for (ax, ay), (bx, by) in zip(pts[:-1], pts[1:]):
                dx, dy = bx - ax, by - ay
                L2 = dx * dx + dy * dy
                t = np.clip(((xx - ax) * dx + (yy - ay) * dy) / max(L2, 1e-9), 0, 1)
                d = np.hypot(xx - (ax + dx * t), yy - (ay + dy * t))
                dmin = np.minimum(dmin, d)
        cov = np.clip(width / 2 + 0.5 - dmin, 0, 1)
        glow = 0.22 * np.exp(-np.maximum(dmin - width / 2, 0) / 3.0) * (dmin < 1e8)
        atlas[:, g * cw:(g + 1) * cw] = np.maximum(cov, glow)
    rgb = np.dstack([np.ones_like(atlas)] * 3)
    save('font', rgb, alpha=atlas)


if __name__ == '__main__':
    main()
