"""NATIONS: the country table and flag textures (owner request 2026-10-19: country presets for representation).

One list below drives everything: the game's table (app/src/nations.inc: code, name, kit colours, team colours) and a
flag texture per country (app/assets_open/tex/flag_<code>.tex, 192 x 128, drawn from simple shapes at 4x and scaled
down). The list is APPEND-ONLY: a country's position is its number in AvatarDesc (sent over the network), so never
reorder or remove. 1-31 are the countries older builds can carry without a gauntlet change (see avatar.h).

Flags are simplified (complex emblems and coats of arms become plain shapes). National flags are free to use; the
Olympic rings and the word "Olympic" are not, and are not used anywhere.

Usage: python -I tools/gen_flags.py            (writes the table, the textures and build/flags_preview.png)
"""
import math
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
TEX = os.path.join(ROOT, 'app', 'assets_open', 'tex')
INC = os.path.join(ROOT, 'app', 'src', 'nations.inc')
W, H, S = 192, 128, 4          # texture size (3:2) and supersampling
CW, CH = W * S, H * S

WHITE, BLACK = '#FFFFFF', '#000000'


def rgb(h):
    h = h.lstrip('#')
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


class Flag:
    def __init__(self, bg=WHITE):
        self.im = Image.new('RGB', (CW, CH), rgb(bg))
        self.d = ImageDraw.Draw(self.im)

    def P(self, x, y):   # normalized (0..1 across, 0..1 down) to canvas pixels
        return (x * CW, y * CH)

    def rect(self, x0, y0, x1, y1, c):
        self.d.rectangle([x0 * CW, y0 * CH, x1 * CW - 1, y1 * CH - 1], fill=rgb(c))

    def poly(self, pts, c):
        self.d.polygon([self.P(x, y) for x, y in pts], fill=rgb(c))

    def hs(self, cols, w=None):   # horizontal stripes
        w = w or [1] * len(cols)
        t, y = float(sum(w)), 0.0
        for c, k in zip(cols, w):
            self.rect(0, y / t, 1, (y + k) / t, c)
            y += k

    def vs(self, cols, w=None):   # vertical stripes
        w = w or [1] * len(cols)
        t, x = float(sum(w)), 0.0
        for c, k in zip(cols, w):
            self.rect(x / t, 0, (x + k) / t, 1, c)
            x += k

    def disc(self, cx, cy, r, c):   # r as a fraction of the height
        rx, ry = r * CH / CW, r
        self.d.ellipse([(cx - rx) * CW, (cy - ry) * CH, (cx + rx) * CW, (cy + ry) * CH], fill=rgb(c))

    def ring(self, cx, cy, r, w, c):
        rx, ry = r * CH / CW, r
        self.d.ellipse([(cx - rx) * CW, (cy - ry) * CH, (cx + rx) * CW, (cy + ry) * CH], outline=rgb(c), width=max(1, int(w * CH)))

    def star(self, cx, cy, r, c, n=5, inner=0.382, rot=0.0):
        pts = []
        for i in range(2 * n):
            a = math.pi * i / n - math.pi / 2 + rot
            rr = r if i % 2 == 0 else r * inner
            pts.append((cx + rr * math.cos(a) * CH / CW, cy + rr * math.sin(a)))
        self.poly(pts, c)

    def nordic(self, bg, cross, inner=None, cx=0.375, w=0.2, wi=0.1):
        self.rect(0, 0, 1, 1, bg)
        hw = w / 2 * CH / CW
        self.rect(cx - hw, 0, cx + hw, 1, cross)
        self.rect(0, 0.5 - w / 2, 1, 0.5 + w / 2, cross)
        if inner:
            hw = wi / 2 * CH / CW
            self.rect(cx - hw, 0, cx + hw, 1, inner)
            self.rect(0, 0.5 - wi / 2, 1, 0.5 + wi / 2, inner)

    def hoist_tri(self, depth, c, y0=0.0, y1=1.0):
        self.poly([(0, y0), (depth, (y0 + y1) / 2), (0, y1)], c)

    def crescent(self, cx, cy, r, c, bg, off=0.25, k=0.8):
        self.disc(cx, cy, r, c)
        self.disc(cx + off * r * CH / CW, cy, r * k, bg)

    def band(self, p0, p1, w, c):   # a straight band of width w (fraction of height) between two points
        (x0, y0), (x1, y1) = self.P(*p0), self.P(*p1)
        dx, dy = x1 - x0, y1 - y0
        n = math.hypot(dx, dy)
        ox, oy = -dy / n * w * CH / 2, dx / n * w * CH / 2
        self.d.polygon([(x0 + ox, y0 + oy), (x1 + ox, y1 + oy), (x1 - ox, y1 - oy), (x0 - ox, y0 - oy)], fill=rgb(c))

    def union_jack(self, x0=0.0, y0=0.0, x1=1.0, y1=1.0):
        blue, red = '#012169', '#C8102E'
        sub = Flag(blue)
        sub.band((0, 0), (1, 1), 0.2, WHITE)
        sub.band((0, 1), (1, 0), 0.2, WHITE)
        sub.band((0, 0), (1, 1), 0.067, red)
        sub.band((0, 1), (1, 0), 0.067, red)
        sub.rect(0.5 - 0.111, 0, 0.5 + 0.111, 1, WHITE)
        sub.rect(0, 0.5 - 0.167, 1, 0.5 + 0.167, WHITE)
        sub.rect(0.5 - 0.067, 0, 0.5 + 0.067, 1, red)
        sub.rect(0, 0.5 - 0.1, 1, 0.5 + 0.1, red)
        w, h = int((x1 - x0) * CW), int((y1 - y0) * CH)
        self.im.paste(sub.im.resize((w, h), Image.LANCZOS), (int(x0 * CW), int(y0 * CH)))

    def stars_grid(self, x0, y0, x1, y1, rows, cols, r, c, stagger=True):
        for j in range(rows):
            for i in range(cols):
                if stagger and (i + j) % 2:
                    continue
                self.star(x0 + (x1 - x0) * (i + 0.5) / cols, y0 + (y1 - y0) * (j + 0.5) / rows, r, c)


# --------------------------------------------------------------------------------------------- the flags
def f_usa(f):
    f.hs(['#B22234', WHITE] * 6 + ['#B22234'])
    f.rect(0, 0, 0.4, 7 / 13, '#3C3B6E')
    f.stars_grid(0.01, 0.01, 0.39, 7 / 13 - 0.01, 9, 11, 0.022, WHITE)


def maple(f, cx, cy, s, c):
    pts = [(0, -1.0), (0.12, -0.78), (0.3, -0.86), (0.25, -0.42), (0.52, -0.66), (0.58, -0.52), (0.86, -0.58),
           (0.76, -0.3), (0.9, -0.22), (0.5, 0.1), (0.58, 0.28), (0.05, 0.22), (0.07, 0.62), (-0.07, 0.62),
           (-0.05, 0.22), (-0.58, 0.28), (-0.5, 0.1), (-0.9, -0.22), (-0.76, -0.3), (-0.86, -0.58), (-0.58, -0.52),
           (-0.52, -0.66), (-0.25, -0.42), (-0.3, -0.86), (-0.12, -0.78)]
    f.poly([(cx + x * s * CH / CW, cy + y * s) for x, y in pts], c)


def f_can(f):
    f.vs(['#D80621', WHITE, '#D80621'], [1, 2, 1])
    maple(f, 0.5, 0.52, 0.36, '#D80621')


def f_mex(f):
    f.vs(['#006847', WHITE, '#CE1126'])
    f.ring(0.5, 0.5, 0.2, 0.035, '#5C7A29')
    f.disc(0.5, 0.47, 0.13, '#8C5B2B')
    f.disc(0.5, 0.6, 0.05, '#2E7D32')


def f_bra(f):
    f.rect(0, 0, 1, 1, '#009C3B')
    f.poly([(0.08, 0.5), (0.5, 0.1), (0.92, 0.5), (0.5, 0.9)], '#FFDF00')
    f.disc(0.5, 0.5, 0.26, '#002776')
    f.band((0.36, 0.45), (0.64, 0.56), 0.05, WHITE)


def f_arg(f):
    f.hs(['#74ACDF', WHITE, '#74ACDF'])
    for i in range(16):
        a = i * math.pi / 8
        f.band((0.5, 0.5), (0.5 + 0.11 * math.cos(a) * CH / CW, 0.5 + 0.11 * math.sin(a)), 0.02, '#F6B40E')
    f.disc(0.5, 0.5, 0.075, '#F6B40E')


def f_gbr(f):
    f.union_jack()


def f_fra(f):
    f.vs(['#0055A4', WHITE, '#EF4135'])


def f_ger(f):
    f.hs([BLACK, '#DD0000', '#FFCE00'])


def f_ita(f):
    f.vs(['#009246', WHITE, '#CE2B37'])


def f_esp(f):
    f.hs(['#AA151B', '#F1BF00', '#AA151B'], [1, 2, 1])


def f_ned(f):
    f.hs(['#AE1C28', WHITE, '#21468B'])


def f_por(f):
    f.vs(['#006600', '#FF0000'], [2, 3])
    f.ring(0.4, 0.5, 0.2, 0.05, '#FFE900')
    f.rect(0.4 - 0.07, 0.38, 0.4 + 0.07, 0.62, WHITE)
    f.rect(0.4 - 0.05, 0.4, 0.4 + 0.05, 0.6, '#FF0000')


def f_irl(f):
    f.vs(['#169B62', WHITE, '#FF883E'])


def f_swe(f):
    f.nordic('#006AA7', '#FECC00')


def f_nor(f):
    f.nordic('#BA0C2F', WHITE, '#00205B', w=0.25, wi=0.125)


def f_den(f):
    f.nordic('#C8102E', WHITE, w=0.14)


def f_fin(f):
    f.nordic(WHITE, '#002F6C', w=0.27)


def f_pol(f):
    f.hs([WHITE, '#DC143C'])


def f_ukr(f):
    f.hs(['#0057B7', '#FFD700'])


def f_sui(f):
    f.rect(0, 0, 1, 1, '#DA291C')
    f.rect(0.5 - 0.06, 0.2, 0.5 + 0.06, 0.8, WHITE)
    f.rect(0.5 - 0.2 * CH / CW, 0.41, 0.5 + 0.2 * CH / CW, 0.59, WHITE)


def f_bel(f):
    f.vs([BLACK, '#FDDA24', '#EF3340'])


def f_aut(f):
    f.hs(['#C8102E', WHITE, '#C8102E'])


def f_jpn(f):
    f.disc(0.5, 0.5, 0.3, '#BC002D')


def f_kor(f):
    red, blue = '#CD2E3A', '#0047A0'
    cx, cy, r = 0.5, 0.5, 0.25
    f.disc(cx, cy, r, blue)
    rx = r * CH / CW
    f.d.pieslice([(cx - rx) * CW, (cy - r) * CH, (cx + rx) * CW, (cy + r) * CH], 180, 360, fill=rgb(red))
    f.disc(cx - rx / 2, cy, r / 2, red)
    f.disc(cx + rx / 2, cy, r / 2, blue)
    for (sx, sy) in ((-1, -1), (1, 1), (1, -1), (-1, 1)):   # the four trigrams, simplified to three bars
        bx, by = cx + sx * 0.3, cy + sy * 0.32
        ang = math.atan2(sy, sx * CW / CH) + math.pi / 2
        for k in (-1, 0, 1):
            ox, oy = math.cos(ang - math.pi / 2) * k * 0.045 * CH / CW, math.sin(ang - math.pi / 2) * k * 0.045
            f.band((bx + ox - math.cos(ang) * 0.06, by + oy - math.sin(ang) * 0.09),
                   (bx + ox + math.cos(ang) * 0.06, by + oy + math.sin(ang) * 0.09), 0.028, BLACK)


def f_chn(f):
    f.rect(0, 0, 1, 1, '#EE1C25')
    f.star(0.17, 0.27, 0.16, '#FFFF00')
    for x, y in ((0.33, 0.1), (0.4, 0.2), (0.4, 0.35), (0.33, 0.45)):
        f.star(x, y, 0.055, '#FFFF00', rot=0.4)


def f_ind(f):
    f.hs(['#FF9933', WHITE, '#138808'])
    f.ring(0.5, 0.5, 0.13, 0.02, '#000080')
    for i in range(12):
        a = i * math.pi / 12
        f.band((0.5 - 0.13 * math.cos(a) * CH / CW, 0.5 - 0.13 * math.sin(a)),
               (0.5 + 0.13 * math.cos(a) * CH / CW, 0.5 + 0.13 * math.sin(a)), 0.008, '#000080')


def southern_cross(f, c, border=None):
    for x, y, r in ((0.75, 0.22, 0.06), (0.62, 0.48, 0.06), (0.86, 0.42, 0.06), (0.75, 0.8, 0.065), (0.79, 0.56, 0.035)):
        if border:
            f.star(x, y, r * 1.35, border)
        f.star(x, y, r, c)


def f_aus(f):
    f.rect(0, 0, 1, 1, '#012169')
    f.union_jack(0, 0, 0.5, 0.5)
    f.star(0.25, 0.75, 0.13, WHITE, n=7, inner=0.45)
    southern_cross(f, WHITE)


def f_nzl(f):
    f.rect(0, 0, 1, 1, '#012169')
    f.union_jack(0, 0, 0.5, 0.5)
    for x, y, r in ((0.75, 0.2, 0.06), (0.65, 0.45, 0.06), (0.86, 0.4, 0.055), (0.75, 0.78, 0.07)):
        f.star(x, y, r * 1.4, WHITE)
        f.star(x, y, r, '#C8102E')


def f_rsa(f):
    red, blue, green, gold = '#E03C31', '#001489', '#007749', '#FFB81C'
    f.hs([red, blue])
    f.band((0, 0), (0.44, 0.5), 0.36, WHITE)
    f.band((0, 1), (0.44, 0.5), 0.36, WHITE)
    f.rect(0.4, 0.5 - 0.18, 1, 0.5 + 0.18, WHITE)
    f.band((0, 0), (0.44, 0.5), 0.2, green)
    f.band((0, 1), (0.44, 0.5), 0.2, green)
    f.rect(0.42, 0.4, 1, 0.6, green)
    f.poly([(0, 0.16), (0.3, 0.5), (0, 0.84)], gold)
    f.poly([(0, 0.24), (0.24, 0.5), (0, 0.76)], BLACK)


def f_ngr(f):
    f.vs(['#008751', WHITE, '#008751'])


def f_phi(f):
    f.hs(['#0038A8', '#CE1126'])
    f.poly([(0, 0), (0.43, 0.5), (0, 1)], WHITE)
    f.disc(0.15, 0.5, 0.08, '#FCD116')
    for i in range(8):
        a = i * math.pi / 4
        f.band((0.15, 0.5), (0.15 + 0.13 * math.cos(a) * CH / CW, 0.5 + 0.13 * math.sin(a)), 0.02, '#FCD116')
    for x, y in ((0.05, 0.1), (0.05, 0.9), (0.36, 0.5)):
        f.star(x, y, 0.045, '#FCD116')


def f_ken(f):
    f.hs([BLACK, WHITE, '#BB0000', WHITE, '#006600'], [6, 1, 6, 1, 6])
    f.d.ellipse([0.44 * CW, 0.12 * CH, 0.56 * CW, 0.88 * CH], fill=rgb('#BB0000'), outline=rgb(BLACK), width=S * 3)
    f.d.ellipse([0.475 * CW, 0.3 * CH, 0.525 * CW, 0.7 * CH], fill=rgb(WHITE))


def f_jam(f):
    f.rect(0, 0, 1, 1, '#009B3A')
    f.poly([(0, 0), (0.5, 0.5), (0, 1)], BLACK)
    f.poly([(1, 0), (0.5, 0.5), (1, 1)], BLACK)
    f.band((0, 0), (1, 1), 0.13, '#FED100')
    f.band((0, 1), (1, 0), 0.13, '#FED100')


def f_egy(f):
    f.hs(['#CE1126', WHITE, BLACK])
    f.poly([(0.46, 0.38), (0.54, 0.38), (0.56, 0.5), (0.5, 0.62), (0.44, 0.5)], '#C09300')


def f_mar(f):
    f.rect(0, 0, 1, 1, '#C1272D')
    pts = []
    for i in range(5):
        a = -math.pi / 2 + i * 4 * math.pi / 5
        pts.append((0.5 + 0.2 * math.cos(a) * CH / CW, 0.5 + 0.2 * math.sin(a)))
    for i in range(5):
        f.band(pts[i], pts[(i + 1) % 5], 0.03, '#006233')


def f_tur(f):
    f.rect(0, 0, 1, 1, '#E30A17')
    f.crescent(0.36, 0.5, 0.25, WHITE, '#E30A17', off=0.25, k=0.8)
    f.star(0.53, 0.5, 0.11, WHITE, rot=-math.pi / 2)


def f_gre(f):
    f.hs(['#0D5EAF', WHITE] * 4 + ['#0D5EAF'])
    f.rect(0, 0, 5 / 13.5, 5 / 9, '#0D5EAF')
    f.rect(2 / 13.5, 0, 3 / 13.5, 5 / 9, WHITE)
    f.rect(0, 2 / 9, 5 / 13.5, 3 / 9, WHITE)


def f_cze(f):
    f.hs([WHITE, '#D7141A'])
    f.hoist_tri(0.5, '#11457E')


def f_hun(f):
    f.hs(['#CD2A3E', WHITE, '#436F4D'])


def f_rou(f):
    f.vs(['#002B7F', '#FCD116', '#CE1126'])


def f_cro(f):
    f.hs(['#FF0000', WHITE, '#171796'])
    for j in range(5):
        for i in range(5):
            f.rect(0.42 + i * 0.032, 0.3 + j * 0.06, 0.42 + (i + 1) * 0.032, 0.3 + (j + 1) * 0.06,
                   '#FF0000' if (i + j) % 2 == 0 else WHITE)


def f_isl(f):
    f.nordic('#02529C', WHITE, '#DC1E35', w=0.22, wi=0.11)


def f_est(f):
    f.hs(['#0072CE', BLACK, WHITE])


def f_col(f):
    f.hs(['#FCD116', '#003893', '#CE1126'], [2, 1, 1])


def f_chi(f):
    f.hs([WHITE, '#D52B1E'])
    f.rect(0, 0, 1 / 3, 0.5, '#0039A6')
    f.star(1 / 6, 0.25, 0.14, WHITE)


def f_per(f):
    f.vs(['#D91023', WHITE, '#D91023'])


def f_uru(f):
    f.hs([WHITE, '#0038A8'] * 4 + [WHITE])
    f.rect(0, 0, 0.36, 5 / 9, WHITE)
    for i in range(16):
        a = i * math.pi / 8
        f.band((0.18, 0.28), (0.18 + 0.17 * math.cos(a) * CH / CW, 0.28 + 0.17 * math.sin(a)), 0.025, '#FCD116')
    f.disc(0.18, 0.28, 0.1, '#FCD116')


def f_tha(f):
    f.hs(['#A51931', WHITE, '#2D2A4A', WHITE, '#A51931'], [1, 1, 2, 1, 1])


def f_vie(f):
    f.rect(0, 0, 1, 1, '#DA251D')
    f.star(0.5, 0.5, 0.3, '#FFFF00')


def f_ina(f):
    f.hs(['#FF0000', WHITE])


def f_sgp(f):
    f.hs(['#EF3340', WHITE])
    f.crescent(0.18, 0.25, 0.16, WHITE, '#EF3340', off=0.3, k=0.85)
    for i in range(5):
        a = -math.pi / 2 + i * 2 * math.pi / 5
        f.star(0.28 + 0.075 * math.cos(a) * CH / CW, 0.25 + 0.075 * math.sin(a), 0.03, WHITE)


def f_pak(f):
    f.vs([WHITE, '#01411C'], [1, 3])
    f.crescent(0.62, 0.5, 0.25, WHITE, '#01411C', off=0.28, k=0.82)
    f.star(0.74, 0.4, 0.08, WHITE, rot=0.3)


def f_ban(f):
    f.rect(0, 0, 1, 1, '#006A4E')
    f.disc(0.45, 0.5, 0.3, '#F42A41')


def f_gha(f):
    f.hs(['#CE1126', '#FCD116', '#006B3F'])
    f.star(0.5, 0.5, 0.15, BLACK)


def f_civ(f):
    f.vs(['#F77F00', WHITE, '#009E60'])


def f_sen(f):
    f.vs(['#00853F', '#FDEF42', '#E31B23'])
    f.star(0.5, 0.5, 0.14, '#00853F')


def f_uae(f):
    f.hs(['#00732F', WHITE, BLACK])
    f.rect(0, 0, 0.25, 1, '#FF0000')


def f_qat(f):
    f.rect(0, 0, 1, 1, '#8A1538')
    pts = [(0, 0), (0.3, 0)]
    for i in range(9):
        y0, y1 = i / 9, (i + 1) / 9
        pts += [(0.3, y0), (0.4, (y0 + y1) / 2)]
    pts += [(0.3, 1), (0, 1)]
    f.poly(pts, WHITE)


def f_isr(f):
    f.rect(0, 0.1, 1, 0.25, '#0038B8')
    f.rect(0, 0.75, 1, 0.9, '#0038B8')
    for rot in (0, math.pi):
        pts = [(0.5 + 0.17 * math.cos(-math.pi / 2 + rot + k * 2 * math.pi / 3) * CH / CW,
                0.5 + 0.17 * math.sin(-math.pi / 2 + rot + k * 2 * math.pi / 3)) for k in range(3)]
        for k in range(3):
            f.band(pts[k], pts[(k + 1) % 3], 0.035, '#0038B8')


def f_ltu(f):
    f.hs(['#FDB913', '#006A44', '#C1272D'])


def f_lat(f):
    f.hs(['#9E3039', WHITE, '#9E3039'], [2, 1, 2])


def f_svk(f):
    f.hs([WHITE, '#0B4EA2', '#EE1C25'])
    f.d.rounded_rectangle([0.2 * CW, 0.22 * CH, 0.36 * CW, 0.78 * CH], radius=S * 10, fill=rgb('#EE1C25'), outline=rgb(WHITE), width=S * 3)
    f.rect(0.272, 0.3, 0.288, 0.62, WHITE)
    f.rect(0.24, 0.36, 0.32, 0.4, WHITE)
    f.rect(0.25, 0.46, 0.31, 0.5, WHITE)
    f.disc(0.28, 0.68, 0.07, '#0B4EA2')


def f_slo(f):
    f.hs([WHITE, '#005DA4', '#ED1C24'])
    f.d.rounded_rectangle([0.22 * CW, 0.16 * CH, 0.34 * CW, 0.48 * CH], radius=S * 8, fill=rgb('#005DA4'), outline=rgb('#ED1C24'), width=S * 2)
    f.poly([(0.23, 0.42), (0.28, 0.28), (0.33, 0.42)], WHITE)


def f_bul(f):
    f.hs([WHITE, '#00966E', '#D62612'])


def f_cmr(f):
    f.vs(['#007A5E', '#CE1126', '#FCD116'])
    f.star(0.5, 0.5, 0.14, '#FCD116')


def f_eth(f):
    f.hs(['#078930', '#FCDD09', '#DA121A'])
    f.disc(0.5, 0.5, 0.22, '#0F47AF')
    pts = []
    for i in range(5):
        a = -math.pi / 2 + i * 4 * math.pi / 5
        pts.append((0.5 + 0.15 * math.cos(a) * CH / CW, 0.5 + 0.15 * math.sin(a)))
    for i in range(5):
        f.band(pts[i], pts[(i + 1) % 5], 0.025, '#FCDD09')


def f_tun(f):
    f.rect(0, 0, 1, 1, '#E70013')
    f.disc(0.5, 0.5, 0.25, WHITE)
    f.crescent(0.49, 0.5, 0.19, '#E70013', WHITE, off=0.25, k=0.8)
    f.star(0.53, 0.5, 0.1, '#E70013', rot=-math.pi / 2)


def f_alg(f):
    f.vs(['#006633', WHITE])
    f.crescent(0.52, 0.5, 0.25, '#D21034', WHITE, off=0.3, k=0.8)
    f.star(0.58, 0.5, 0.1, '#D21034', rot=-math.pi / 2)


def f_pur(f):
    f.hs(['#ED0000', WHITE] * 2 + ['#ED0000'])
    f.hoist_tri(0.45, '#0050F0')
    f.star(0.15, 0.5, 0.12, WHITE)


def f_cub(f):
    f.hs(['#002A8F', WHITE] * 2 + ['#002A8F'])
    f.hoist_tri(0.45, '#CF142B')
    f.star(0.15, 0.5, 0.12, WHITE)


def f_crc(f):
    f.hs(['#002B7F', WHITE, '#CE1126', WHITE, '#002B7F'], [1, 1, 2, 1, 1])


def f_pan(f):
    f.rect(0.5, 0, 1, 0.5, '#DA121A')
    f.rect(0, 0.5, 0.5, 1, '#072357')
    f.star(0.25, 0.25, 0.12, '#072357')
    f.star(0.75, 0.75, 0.12, '#DA121A')


def f_dom(f):
    f.rect(0, 0, 1, 1, WHITE)
    blue, red = '#002D62', '#CE1126'
    f.rect(0, 0, 0.44, 0.44, blue)
    f.rect(0.56, 0, 1, 0.44, red)
    f.rect(0, 0.56, 0.44, 1, red)
    f.rect(0.56, 0.56, 1, 1, blue)
    f.disc(0.5, 0.5, 0.06, '#00843D')


# (code, name, flag, kit (primary, secondary, accent), team colours by preference). APPEND-ONLY (see the top).
NATIONS = [
    ('USA', 'UNITED STATES', f_usa, ('#3C3B6E', WHITE, '#B22234'), ['#0A3161', '#B22234']),   # (Old Glory blue: the flag's navy glows violet)
    ('CAN', 'CANADA', f_can, ('#D80621', WHITE, '#D80621'), ['#D80621']),
    ('MEX', 'MEXICO', f_mex, ('#006847', WHITE, '#CE1126'), ['#006847', '#CE1126']),
    ('BRA', 'BRAZIL', f_bra, ('#FFDF00', '#009C3B', '#002776'), ['#FFDF00', '#009C3B']),
    ('ARG', 'ARGENTINA', f_arg, ('#74ACDF', WHITE, '#F6B40E'), ['#74ACDF', '#F6B40E']),
    ('GBR', 'GREAT BRITAIN', f_gbr, ('#012169', WHITE, '#C8102E'), ['#012169', '#C8102E']),
    ('FRA', 'FRANCE', f_fra, ('#0055A4', WHITE, '#EF4135'), ['#0055A4', '#EF4135']),
    ('GER', 'GERMANY', f_ger, (BLACK, '#DD0000', '#FFCE00'), ['#FFCE00', '#DD0000']),
    ('ITA', 'ITALY', f_ita, ('#0066CC', WHITE, '#009246'), ['#009246', '#CE2B37']),
    ('ESP', 'SPAIN', f_esp, ('#AA151B', '#F1BF00', '#F1BF00'), ['#AA151B', '#F1BF00']),
    ('NED', 'NETHERLANDS', f_ned, ('#FF6600', WHITE, '#21468B'), ['#FF6600', '#21468B']),
    ('POR', 'PORTUGAL', f_por, ('#FF0000', '#006600', '#FFE900'), ['#FF0000', '#006600']),
    ('IRL', 'IRELAND', f_irl, ('#169B62', WHITE, '#FF883E'), ['#169B62', '#FF883E']),
    ('SWE', 'SWEDEN', f_swe, ('#006AA7', '#FECC00', '#FECC00'), ['#FECC00', '#006AA7']),
    ('NOR', 'NORWAY', f_nor, ('#BA0C2F', WHITE, '#00205B'), ['#BA0C2F', '#00205B']),
    ('DEN', 'DENMARK', f_den, ('#C8102E', WHITE, '#C8102E'), ['#C8102E']),
    ('FIN', 'FINLAND', f_fin, (WHITE, '#002F6C', '#002F6C'), ['#002F6C']),
    ('POL', 'POLAND', f_pol, (WHITE, '#DC143C', '#DC143C'), ['#DC143C']),
    ('UKR', 'UKRAINE', f_ukr, ('#0057B7', '#FFD700', '#FFD700'), ['#0057B7', '#FFD700']),
    ('SUI', 'SWITZERLAND', f_sui, ('#DA291C', WHITE, '#DA291C'), ['#DA291C']),
    ('BEL', 'BELGIUM', f_bel, ('#EF3340', BLACK, '#FDDA24'), ['#EF3340', '#FDDA24']),
    ('AUT', 'AUSTRIA', f_aut, ('#C8102E', WHITE, '#C8102E'), ['#C8102E']),
    ('JPN', 'JAPAN', f_jpn, (WHITE, '#BC002D', '#BC002D'), ['#BC002D']),
    ('KOR', 'SOUTH KOREA', f_kor, (WHITE, '#CD2E3A', '#0047A0'), ['#CD2E3A', '#0047A0']),
    ('CHN', 'CHINA', f_chn, ('#EE1C25', '#FFFF00', '#FFFF00'), ['#EE1C25', '#FFFF00']),
    ('IND', 'INDIA', f_ind, ('#000080', '#FF9933', '#138808'), ['#FF9933', '#138808']),
    ('AUS', 'AUSTRALIA', f_aus, ('#00843D', '#FFCD00', '#FFCD00'), ['#FFCD00', '#00843D']),
    ('NZL', 'NEW ZEALAND', f_nzl, (BLACK, WHITE, WHITE), ['#C8102E', '#012169']),
    ('RSA', 'SOUTH AFRICA', f_rsa, ('#007749', '#FFB81C', '#E03C31'), ['#007749', '#FFB81C']),
    ('NGR', 'NIGERIA', f_ngr, ('#008751', WHITE, '#008751'), ['#008751']),
    ('PHI', 'PHILIPPINES', f_phi, ('#0038A8', WHITE, '#CE1126'), ['#0038A8', '#CE1126']),
    ('KEN', 'KENYA', f_ken, ('#BB0000', '#006600', BLACK), ['#BB0000', '#006600']),
    ('JAM', 'JAMAICA', f_jam, ('#009B3A', '#FED100', BLACK), ['#FED100', '#009B3A']),
    ('EGY', 'EGYPT', f_egy, ('#CE1126', WHITE, '#C09300'), ['#CE1126', '#C09300']),
    ('MAR', 'MOROCCO', f_mar, ('#C1272D', '#006233', '#006233'), ['#C1272D', '#006233']),
    ('TUR', 'TURKEY', f_tur, ('#E30A17', WHITE, '#E30A17'), ['#E30A17']),
    ('GRE', 'GREECE', f_gre, ('#0D5EAF', WHITE, '#0D5EAF'), ['#0D5EAF']),
    ('CZE', 'CZECHIA', f_cze, ('#11457E', WHITE, '#D7141A'), ['#11457E', '#D7141A']),
    ('HUN', 'HUNGARY', f_hun, ('#CD2A3E', WHITE, '#436F4D'), ['#CD2A3E', '#436F4D']),
    ('ROU', 'ROMANIA', f_rou, ('#FCD116', '#002B7F', '#CE1126'), ['#FCD116', '#002B7F']),
    ('CRO', 'CROATIA', f_cro, ('#FF0000', WHITE, '#171796'), ['#FF0000', '#171796']),
    ('ISL', 'ICELAND', f_isl, ('#02529C', WHITE, '#DC1E35'), ['#02529C', '#DC1E35']),
    ('EST', 'ESTONIA', f_est, ('#0072CE', BLACK, WHITE), ['#0072CE']),
    ('COL', 'COLOMBIA', f_col, ('#FCD116', '#003893', '#CE1126'), ['#FCD116', '#003893']),
    ('CHI', 'CHILE', f_chi, ('#D52B1E', WHITE, '#0039A6'), ['#D52B1E', '#0039A6']),
    ('PER', 'PERU', f_per, (WHITE, '#D91023', '#D91023'), ['#D91023']),
    ('URU', 'URUGUAY', f_uru, ('#5CBFEB', WHITE, '#FCD116'), ['#5CBFEB', '#FCD116']),
    ('THA', 'THAILAND', f_tha, ('#2D2A4A', WHITE, '#A51931'), ['#A51931', '#2D2A4A']),
    ('VIE', 'VIETNAM', f_vie, ('#DA251D', '#FFFF00', '#FFFF00'), ['#DA251D', '#FFFF00']),
    ('INA', 'INDONESIA', f_ina, ('#FF0000', WHITE, '#FF0000'), ['#FF0000']),
    ('SGP', 'SINGAPORE', f_sgp, ('#EF3340', WHITE, '#EF3340'), ['#EF3340']),
    ('PAK', 'PAKISTAN', f_pak, ('#01411C', WHITE, '#01411C'), ['#01411C']),
    ('BAN', 'BANGLADESH', f_ban, ('#006A4E', '#F42A41', '#F42A41'), ['#006A4E', '#F42A41']),
    ('GHA', 'GHANA', f_gha, ('#FCD116', '#006B3F', '#CE1126'), ['#FCD116', '#CE1126']),
    ('CIV', 'COTE D IVOIRE', f_civ, ('#F77F00', WHITE, '#009E60'), ['#F77F00', '#009E60']),
    ('SEN', 'SENEGAL', f_sen, ('#00853F', '#FDEF42', '#E31B23'), ['#00853F', '#FDEF42']),
    ('UAE', 'UNITED ARAB EMIRATES', f_uae, ('#00732F', WHITE, '#FF0000'), ['#00732F', '#FF0000']),
    ('QAT', 'QATAR', f_qat, ('#8A1538', WHITE, '#8A1538'), ['#8A1538']),
    ('ISR', 'ISRAEL', f_isr, ('#0038B8', WHITE, '#0038B8'), ['#0038B8']),
    ('LTU', 'LITHUANIA', f_ltu, ('#FDB913', '#006A44', '#C1272D'), ['#FDB913', '#006A44']),
    ('LAT', 'LATVIA', f_lat, ('#9E3039', WHITE, '#9E3039'), ['#9E3039']),
    ('SVK', 'SLOVAKIA', f_svk, ('#0B4EA2', WHITE, '#EE1C25'), ['#0B4EA2', '#EE1C25']),
    ('SLO', 'SLOVENIA', f_slo, ('#005DA4', WHITE, '#ED1C24'), ['#005DA4', '#ED1C24']),
    ('BUL', 'BULGARIA', f_bul, ('#00966E', WHITE, '#D62612'), ['#00966E', '#D62612']),
    ('CMR', 'CAMEROON', f_cmr, ('#007A5E', '#FCD116', '#CE1126'), ['#007A5E', '#FCD116']),
    ('ETH', 'ETHIOPIA', f_eth, ('#078930', '#FCDD09', '#DA121A'), ['#078930', '#FCDD09']),
    ('TUN', 'TUNISIA', f_tun, ('#E70013', WHITE, '#E70013'), ['#E70013']),
    ('ALG', 'ALGERIA', f_alg, ('#006633', WHITE, '#D21034'), ['#006633', '#D21034']),
    ('PUR', 'PUERTO RICO', f_pur, ('#0050F0', WHITE, '#ED0000'), ['#0050F0', '#ED0000']),
    ('CUB', 'CUBA', f_cub, ('#002A8F', WHITE, '#CF142B'), ['#002A8F', '#CF142B']),
    ('CRC', 'COSTA RICA', f_crc, ('#002B7F', WHITE, '#CE1126'), ['#002B7F', '#CE1126']),
    ('PAN', 'PANAMA', f_pan, ('#072357', WHITE, '#DA121A'), ['#DA121A', '#072357']),
    ('DOM', 'DOMINICAN REPUBLIC', f_dom, ('#002D62', WHITE, '#CE1126'), ['#002D62', '#CE1126']),
]


# --------------------------------------------------------------------------------------------- the rest of the world
# (owner, 2026-10-20: "inclusive and tolerant to all known nations around the world and through the galaxy")
def cross(f, c, w=0.2, cx=0.5):
    f.rect(cx - w / 2 * CH / CW, 0, cx + w / 2 * CH / CW, 1, c)
    f.rect(0, 0.5 - w / 2, 1, 0.5 + w / 2, c)


def saltire(f, c, w=0.2):
    f.band((0, 0), (1, 1), w, c)
    f.band((0, 1), (1, 0), w, c)


def emblem(f, c, cx=0.5, cy=0.5, r=0.16):   # a coat of arms, simplified to a shield
    w = r * CH / CW
    f.d.polygon([f.P(cx - w, cy - r), f.P(cx + w, cy - r), f.P(cx + w, cy + r * 0.3), f.P(cx, cy + r), f.P(cx - w, cy + r * 0.3)],
                fill=rgb(c))


def sun(f, cx, cy, r, c, rays=12, ray=1.7):
    for i in range(rays):
        a = i * 2 * math.pi / rays
        f.band((cx, cy), (cx + r * ray * math.cos(a) * CH / CW, cy + r * ray * math.sin(a)), r * 0.35, c)
    f.disc(cx, cy, r, c)


def f_alb(f):
    f.rect(0, 0, 1, 1, '#E41E20')
    f.poly([(0.5, 0.2), (0.6, 0.3), (0.68, 0.24), (0.66, 0.42), (0.58, 0.5), (0.62, 0.78), (0.5, 0.66),
            (0.38, 0.78), (0.42, 0.5), (0.34, 0.42), (0.32, 0.24), (0.4, 0.3)], BLACK)


def f_and(f):
    f.vs(['#10069F', '#FEDF00', '#D50032'], [8, 9, 8])
    emblem(f, '#C7A900', r=0.13)


def f_arm(f):
    f.hs(['#D90012', '#0033A0', '#F2A800'])


def f_aze(f):
    f.hs(['#0092BC', '#E4002B', '#00AF66'])
    f.crescent(0.48, 0.5, 0.12, WHITE, '#E4002B', off=0.3, k=0.8)
    f.star(0.56, 0.5, 0.05, WHITE, n=8, inner=0.5)


def f_blr(f):
    f.hs(['#C8313E', '#4AA657'], [2, 1])
    f.rect(0, 0, 0.11, 1, WHITE)
    for j in range(8):
        y = (j + 0.5) / 8
        f.poly([(0.055, y - 0.05), (0.095, y), (0.055, y + 0.05), (0.015, y)], '#C8313E')


def f_bih(f):
    f.rect(0, 0, 1, 1, '#002395')
    f.poly([(0.27, 0), (0.79, 0), (0.79, 1)], '#FECB00')
    for i in range(9):
        f.star(0.2 + i * 0.065, 0.02 + i * 0.12, 0.045, WHITE)


def f_cyp(f):
    f.rect(0, 0, 1, 1, WHITE)
    f.poly([(0.28, 0.42), (0.42, 0.3), (0.6, 0.28), (0.74, 0.22), (0.66, 0.36), (0.55, 0.45), (0.38, 0.5)], '#D57800')
    f.band((0.38, 0.68), (0.48, 0.62), 0.03, '#4E5B31')
    f.band((0.62, 0.68), (0.52, 0.62), 0.03, '#4E5B31')


def f_geo(f):
    f.rect(0, 0, 1, 1, WHITE)
    cross(f, '#FF0000', 0.2)
    for x, y in ((0.2, 0.2), (0.8, 0.2), (0.2, 0.8), (0.8, 0.8)):
        f.rect(x - 0.06 * CH / CW, y - 0.02, x + 0.06 * CH / CW, y + 0.02, '#FF0000')
        f.rect(x - 0.02 * CH / CW, y - 0.06, x + 0.02 * CH / CW, y + 0.06, '#FF0000')


def f_kos(f):
    f.rect(0, 0, 1, 1, '#244AA5')
    f.poly([(0.36, 0.42), (0.5, 0.36), (0.62, 0.42), (0.64, 0.6), (0.56, 0.74), (0.44, 0.76), (0.36, 0.62)], '#D0A650')
    for i in range(6):
        a = math.pi * (1.15 + i * 0.14)
        f.star(0.5 + 0.3 * math.cos(a) * CH / CW, 0.52 + 0.3 * math.sin(a), 0.04, WHITE)


def f_lie(f):
    f.hs(['#002B7F', '#CE1126'])
    f.poly([(0.16, 0.3), (0.2, 0.16), (0.25, 0.26), (0.29, 0.14), (0.33, 0.26), (0.38, 0.16), (0.42, 0.3)], '#FFD83D')


def f_lux(f):
    f.hs(['#EF3340', WHITE, '#00A3E0'])


def f_mlt(f):
    f.vs([WHITE, '#CF142B'])
    f.rect(0.05, 0.06, 0.14, 0.2, '#A0A0A0')
    f.rect(0.035, 0.11, 0.155, 0.15, '#A0A0A0')


def f_mda(f):
    f.vs(['#0046AE', '#FFD200', '#CC092F'])
    emblem(f, '#B07E3C', r=0.15)


def f_mon(f):
    f.hs(['#CE1126', WHITE])


def f_mne(f):
    f.rect(0, 0, 1, 1, '#D3AE3B')
    f.rect(0.04, 0.06, 0.96, 0.94, '#C40308')
    emblem(f, '#D3AE3B', r=0.2)


def f_mkd(f):
    f.rect(0, 0, 1, 1, '#D20000')
    for a in range(8):
        ang = a * math.pi / 4
        f.poly([(0.5, 0.5), (0.5 + 1.2 * math.cos(ang - 0.12), 0.5 + 1.2 * math.sin(ang - 0.12)),
                (0.5 + 1.2 * math.cos(ang + 0.12), 0.5 + 1.2 * math.sin(ang + 0.12))], '#FFE600')
    f.disc(0.5, 0.5, 0.16, '#FFE600')
    f.ring(0.5, 0.5, 0.18, 0.03, '#D20000')


def f_rus(f):
    f.hs([WHITE, '#0039A6', '#D52B1E'])


def f_smr(f):
    f.hs([WHITE, '#5EB6E4'])
    emblem(f, '#C8A951', r=0.16)


def f_srb(f):
    f.hs(['#C6363C', '#0C4076', WHITE])
    emblem(f, '#C6363C', cx=0.33, r=0.2)


def f_vat(f):
    f.vs(['#FFE000', WHITE])
    f.band((0.66, 0.36), (0.84, 0.66), 0.04, '#C0A040')
    f.band((0.84, 0.36), (0.66, 0.66), 0.04, '#A0A0A0')


def f_eng(f):
    f.rect(0, 0, 1, 1, WHITE)
    cross(f, '#CE1124', 0.2)


def f_sco(f):
    f.rect(0, 0, 1, 1, '#005EB8')
    saltire(f, WHITE, 0.2)


def f_wal(f):
    f.hs([WHITE, '#00B140'])
    f.poly([(0.25, 0.6), (0.32, 0.36), (0.45, 0.42), (0.55, 0.3), (0.72, 0.32), (0.66, 0.42), (0.78, 0.5),
            (0.7, 0.62), (0.58, 0.56), (0.5, 0.74), (0.4, 0.62)], '#D30731')


def f_afg(f):
    f.vs([BLACK, '#D32011', '#007A36'])
    f.ring(0.5, 0.5, 0.17, 0.035, WHITE)


def f_bhr(f):
    f.rect(0, 0, 1, 1, '#CE1126')
    pts = [(0, 0), (0.25, 0)]
    for i in range(5):
        y0, y1 = i / 5, (i + 1) / 5
        pts += [(0.25, y0), (0.37, (y0 + y1) / 2)]
    pts += [(0.25, 1), (0, 1)]
    f.poly(pts, WHITE)


def f_btn(f):
    f.poly([(0, 0), (1, 0), (0, 1)], '#FFD520')
    f.poly([(1, 0), (1, 1), (0, 1)], '#FF4E12')
    f.poly([(0.3, 0.62), (0.42, 0.48), (0.55, 0.5), (0.68, 0.36), (0.72, 0.44), (0.6, 0.56), (0.48, 0.6), (0.38, 0.7)], WHITE)


def f_brn(f):
    f.rect(0, 0, 1, 1, '#F7E017')
    f.band((0, 0.3), (1, 0.62), 0.16, WHITE)
    f.band((0, 0.42), (1, 0.74), 0.16, BLACK)
    f.disc(0.5, 0.52, 0.17, '#CF1126')


def f_khm(f):
    f.hs(['#032EA1', '#E00025', '#032EA1'], [1, 2, 1])
    f.rect(0.36, 0.42, 0.64, 0.6, WHITE)
    for x in (0.4, 0.5, 0.6):
        f.poly([(x - 0.03, 0.42), (x, 0.32), (x + 0.03, 0.42)], WHITE)


def f_hkg(f):
    f.rect(0, 0, 1, 1, '#DE2910')
    for i in range(5):
        a = -math.pi / 2 + i * 2 * math.pi / 5
        f.d.ellipse([(0.5 + 0.11 * math.cos(a) * CH / CW - 0.06 * CH / CW) * CW, (0.5 + 0.11 * math.sin(a) - 0.06) * CH,
                     (0.5 + 0.11 * math.cos(a) * CH / CW + 0.06 * CH / CW) * CW, (0.5 + 0.11 * math.sin(a) + 0.06) * CH], fill=rgb(WHITE))
        f.star(0.5 + 0.11 * math.cos(a) * CH / CW, 0.5 + 0.11 * math.sin(a), 0.02, '#DE2910')


def f_irn(f):
    f.hs(['#239F40', WHITE, '#DA0000'])
    f.disc(0.5, 0.5, 0.1, '#DA0000')
    f.disc(0.5, 0.5, 0.06, WHITE)


def f_irq(f):
    f.hs(['#CE1126', WHITE, BLACK])
    for x in (0.38, 0.5, 0.62):
        f.rect(x - 0.035, 0.44, x + 0.035, 0.56, '#007A3D')


def f_jor(f):
    f.hs([BLACK, WHITE, '#007A3D'])
    f.hoist_tri(0.5, '#CE1126')
    f.star(0.16, 0.5, 0.07, WHITE, n=7, inner=0.5)


def f_kaz(f):
    f.rect(0, 0, 1, 1, '#00AFCA')
    sun(f, 0.55, 0.42, 0.11, '#FEC50C', rays=24, ray=1.5)
    f.poly([(0.38, 0.7), (0.55, 0.62), (0.72, 0.7), (0.55, 0.66)], '#FEC50C')
    f.rect(0.05, 0.05, 0.09, 0.95, '#FEC50C')


def f_kwt(f):
    f.hs(['#007A3D', WHITE, '#CE1126'])
    f.poly([(0, 0), (0.25, 1 / 3), (0.25, 2 / 3), (0, 1)], BLACK)


def f_kgz(f):
    f.rect(0, 0, 1, 1, '#E8112D')
    sun(f, 0.5, 0.5, 0.16, '#FFEF00', rays=40, ray=1.75)
    f.ring(0.5, 0.5, 0.11, 0.025, '#E8112D')


def f_lao(f):
    f.hs(['#CE1126', '#002868', '#CE1126'], [1, 2, 1])
    f.disc(0.5, 0.5, 0.2, WHITE)


def f_lbn(f):
    f.hs(['#ED1C24', WHITE, '#ED1C24'], [1, 2, 1])
    f.poly([(0.5, 0.3), (0.62, 0.52), (0.56, 0.52), (0.66, 0.66), (0.34, 0.66), (0.44, 0.52), (0.38, 0.52)], '#00A651')


def f_mas(f):
    f.hs(['#CC0001', WHITE] * 7)
    f.rect(0, 0, 0.5, 8 / 14, '#010066')
    f.crescent(0.18, 0.29, 0.2, '#FFCC00', '#010066', off=0.35, k=0.85)
    f.star(0.32, 0.29, 0.13, '#FFCC00', n=14, inner=0.45)


def f_mdv(f):
    f.rect(0, 0, 1, 1, '#D21034')
    f.rect(0.25, 0.25, 0.75, 0.75, '#007E3A')
    f.crescent(0.5, 0.5, 0.18, WHITE, '#007E3A', off=0.3, k=0.85)


def f_mgl(f):
    f.vs(['#DA2032', '#015197', '#DA2032'])
    f.disc(0.165, 0.32, 0.05, '#FFD900')
    f.rect(0.125, 0.42, 0.205, 0.46, '#FFD900')
    f.rect(0.125, 0.66, 0.205, 0.7, '#FFD900')
    f.rect(0.13, 0.48, 0.2, 0.64, '#FFD900')


def f_mya(f):
    f.hs(['#FECB00', '#34B233', '#EA2839'])
    f.star(0.5, 0.53, 0.3, WHITE)


def f_npl(f):
    f.rect(0, 0, 1, 1, WHITE)
    f.poly([(0.15, 0.04), (0.72, 0.48), (0.38, 0.48), (0.72, 0.96), (0.15, 0.96)], '#003893')
    f.poly([(0.19, 0.12), (0.6, 0.44), (0.3, 0.44), (0.6, 0.92), (0.19, 0.92)], '#DC143C')
    f.crescent(0.3, 0.32, 0.07, WHITE, '#DC143C', off=-0.6, k=0.8)
    sun(f, 0.3, 0.72, 0.06, WHITE, rays=12, ray=1.5)


def f_prk(f):
    f.hs(['#024FA2', WHITE, '#ED1C27', WHITE, '#024FA2'], [6, 1, 17, 1, 6])
    f.disc(0.35, 0.5, 0.2, WHITE)
    f.star(0.35, 0.5, 0.18, '#ED1C27')


def f_omn(f):
    f.hs([WHITE, '#DB161B', '#008000'])
    f.rect(0, 0, 0.3, 1, '#DB161B')
    f.band((0.08, 0.1), (0.22, 0.26), 0.03, WHITE)
    f.band((0.22, 0.1), (0.08, 0.26), 0.03, WHITE)


def f_ple(f):
    f.hs([BLACK, WHITE, '#007A3D'])
    f.hoist_tri(0.4, '#CE1126')


def f_ksa(f):
    f.rect(0, 0, 1, 1, '#006C35')
    for k, w in enumerate((0.5, 0.42, 0.46)):   # the inscription, as lines of script
        f.band((0.5 - w / 2, 0.32 + k * 0.07), (0.5 + w / 2, 0.32 + k * 0.07), 0.035, WHITE)
    f.band((0.3, 0.7), (0.7, 0.7), 0.025, WHITE)
    f.rect(0.68, 0.66, 0.72, 0.74, WHITE)


def f_sri(f):
    f.rect(0, 0, 1, 1, '#FFBE29')
    f.rect(0.04, 0.06, 0.13, 0.94, '#00534E')
    f.rect(0.13, 0.06, 0.22, 0.94, '#EB7400')
    f.rect(0.26, 0.06, 0.96, 0.94, '#8D153A')
    f.poly([(0.42, 0.7), (0.48, 0.38), (0.62, 0.32), (0.74, 0.4), (0.8, 0.7), (0.68, 0.62), (0.56, 0.66)], '#FFBE29')


def f_syr(f):   # the independence flag, again the national flag since 2025
    f.hs(['#007A3D', WHITE, BLACK])
    for x in (0.34, 0.5, 0.66):
        f.star(x, 0.5, 0.1, '#CE1126')


def f_tjk(f):
    f.hs(['#CC0000', WHITE, '#006600'], [2, 3, 2])
    f.poly([(0.44, 0.56), (0.46, 0.46), (0.5, 0.5), (0.54, 0.46), (0.56, 0.56)], '#F8C300')
    for i in range(7):
        a = math.pi * (1.1 + i * 0.133)
        f.star(0.5 + 0.17 * math.cos(a) * CH / CW, 0.56 + 0.17 * math.sin(a), 0.025, '#F8C300')


def f_tls(f):
    f.rect(0, 0, 1, 1, '#DC241F')
    f.hoist_tri(0.5, '#FFC726')
    f.hoist_tri(0.33, BLACK)
    f.star(0.12, 0.5, 0.11, WHITE, rot=-0.3)


def f_tkm(f):
    f.rect(0, 0, 1, 1, '#00843D')
    f.rect(0.12, 0, 0.3, 1, '#C8102E')
    for j in range(5):
        f.disc(0.21, 0.12 + j * 0.19, 0.05, '#E5B352')
    f.crescent(0.45, 0.25, 0.1, WHITE, '#00843D', off=0.35, k=0.8)
    for i in range(5):
        f.star(0.52 + (i % 3) * 0.05, 0.16 + (i // 3) * 0.1, 0.025, WHITE)


def f_uzb(f):
    f.hs(['#0099B5', '#CE1126', WHITE, '#CE1126', '#1EB53A'], [10, 1, 10, 1, 10])
    f.crescent(0.14, 0.16, 0.1, WHITE, '#0099B5', off=0.35, k=0.85)
    for j in range(3):
        for i in range(4):
            f.star(0.24 + i * 0.045, 0.08 + j * 0.08, 0.018, WHITE)


def f_yem(f):
    f.hs(['#CE1126', WHITE, BLACK])


def f_tpe(f):
    f.rect(0, 0, 1, 1, '#FE0000')
    f.rect(0, 0, 0.5, 0.5, '#000095')
    sun(f, 0.25, 0.25, 0.085, WHITE, rays=12, ray=1.8)
    f.ring(0.25, 0.25, 0.085, 0.012, '#000095')


def f_ago(f):
    f.hs(['#CC092F', BLACK])
    f.ring(0.52, 0.5, 0.15, 0.04, '#FFCB00')
    f.band((0.42, 0.62), (0.62, 0.38), 0.04, '#FFCB00')
    f.star(0.48, 0.42, 0.05, '#FFCB00')


def f_ben(f):
    f.rect(0, 0, 0.4, 1, '#008751')
    f.rect(0.4, 0, 1, 0.5, '#FCD116')
    f.rect(0.4, 0.5, 1, 1, '#E8112D')


def f_bot(f):
    f.hs(['#75AADB', WHITE, BLACK, WHITE, '#75AADB'], [9, 1, 4, 1, 9])


def f_bur(f):
    f.hs(['#EF2B2D', '#009E49'])
    f.star(0.5, 0.5, 0.15, '#FCD116')


def f_bdi(f):
    f.poly([(0, 0), (1, 0), (0.5, 0.5)], '#CE1126')
    f.poly([(0, 1), (1, 1), (0.5, 0.5)], '#CE1126')
    f.poly([(0, 0), (0.5, 0.5), (0, 1)], '#1EB53A')
    f.poly([(1, 0), (0.5, 0.5), (1, 1)], '#1EB53A')
    saltire(f, WHITE, 0.14)
    f.disc(0.5, 0.5, 0.26, WHITE)
    for x, y in ((0.5, 0.39), (0.43, 0.56), (0.57, 0.56)):
        f.star(x, y, 0.065, '#CE1126', n=6, inner=0.5)


def f_cpv(f):
    f.hs(['#003893', WHITE, '#CF2027', WHITE, '#003893'], [6, 1, 1, 1, 3])
    for i in range(10):
        a = i * 2 * math.pi / 10
        f.star(0.375 + 0.2 * math.cos(a) * CH / CW, 0.625 + 0.2 * math.sin(a), 0.035, '#F7D116')


def f_caf(f):
    f.hs(['#003082', WHITE, '#289728', '#FFCE00'])
    f.rect(0.45, 0, 0.55, 1, '#D21034')
    f.star(0.12, 0.12, 0.08, '#FFCE00')


def f_cha(f):
    f.vs(['#002664', '#FECB00', '#C60C30'])


def f_com(f):
    f.hs(['#FFC61E', WHITE, '#CE1126', '#3A75C4'])
    f.hoist_tri(0.45, '#3D8E33')
    f.crescent(0.14, 0.5, 0.14, WHITE, '#3D8E33', off=0.35, k=0.8)
    for j in range(4):
        f.star(0.2, 0.36 + j * 0.09, 0.025, WHITE)


def f_cod(f):
    f.rect(0, 0, 1, 1, '#007FFF')
    f.band((0, 1.05), (1, -0.05), 0.34, '#F7D618')
    f.band((0, 1.05), (1, -0.05), 0.24, '#CE1021')
    f.star(0.16, 0.22, 0.13, '#F7D618')


def f_cgo(f):
    f.poly([(0, 0), (0.6, 0), (0, 1)], '#009543')
    f.poly([(1, 0), (1, 1), (0.4, 1)], '#DC241F')
    f.poly([(0, 0), (0.6, 0), (0, 1)], '#009543')
    f.poly([(0.6, 0), (1, 0), (0.4, 1), (0, 1)], '#FBDE4A')   # the yellow band, corner to corner


def f_dji(f):
    f.hs(['#6AB2E7', '#12AD2B'])
    f.hoist_tri(0.5, WHITE)
    f.star(0.15, 0.5, 0.09, '#D7141A')


def f_gnq(f):
    f.hs(['#3E9A00', WHITE, '#E32118'])
    f.hoist_tri(0.25, '#0073CE')
    emblem(f, '#A0A0A0', r=0.1)


def f_eri(f):
    f.poly([(0, 0), (1, 0.5), (0, 1)], '#EA0437')
    f.poly([(0, 0), (1, 0), (1, 0.5)], '#12AD2B')
    f.poly([(0, 1), (1, 1), (1, 0.5)], '#4189DD')
    f.poly([(0, 0), (1, 0.5), (0, 1)], '#EA0437')
    f.ring(0.25, 0.5, 0.16, 0.03, '#FFC726')
    f.rect(0.245, 0.4, 0.255, 0.62, '#FFC726')


def f_swz(f):
    f.hs(['#3E5EB9', '#FFD900', '#B10C0C', '#FFD900', '#3E5EB9'], [3, 1, 6, 1, 3])
    f.d.ellipse([0.36 * CW, 0.38 * CH, 0.64 * CW, 0.62 * CH], fill=rgb(WHITE))
    f.d.pieslice([0.36 * CW, 0.38 * CH, 0.64 * CW, 0.62 * CH], 90, 270, fill=rgb(BLACK))


def f_gab(f):
    f.hs(['#009E60', '#FCD116', '#3A75C4'])


def f_gam(f):
    f.hs(['#CE1126', WHITE, '#0C1C8C', WHITE, '#3A7728'], [6, 1, 4, 1, 6])


def f_gui(f):
    f.vs(['#CE1126', '#FCD116', '#009460'])


def f_gbs(f):
    f.rect(0, 0, 0.33, 1, '#CE1126')
    f.rect(0.33, 0, 1, 0.5, '#FCD116')
    f.rect(0.33, 0.5, 1, 1, '#009E49')
    f.star(0.165, 0.5, 0.12, BLACK)


def f_les(f):
    f.hs(['#00209F', WHITE, '#009543'], [3, 4, 3])
    f.poly([(0.5, 0.36), (0.58, 0.56), (0.62, 0.6), (0.38, 0.6), (0.42, 0.56)], BLACK)


def f_lbr(f):
    f.hs(['#BF0A30', WHITE] * 5 + ['#BF0A30'])
    f.rect(0, 0, 0.3, 5 / 11, '#002868')
    f.star(0.15, 2.5 / 11, 0.13, WHITE)


def f_lby(f):
    f.hs(['#E70013', BLACK, '#239E46'], [1, 2, 1])
    f.crescent(0.48, 0.5, 0.12, WHITE, BLACK, off=0.3, k=0.8)
    f.star(0.55, 0.5, 0.05, WHITE, rot=-math.pi / 2)


def f_mad(f):
    f.rect(0, 0, 1 / 3, 1, WHITE)
    f.rect(1 / 3, 0, 1, 0.5, '#FC3D32')
    f.rect(1 / 3, 0.5, 1, 1, '#007E3A')


def f_maw(f):
    f.hs([BLACK, '#CE1126', '#339E35'])
    f.d.pieslice([0.38 * CW, 0.12 * CH, 0.62 * CW, 0.48 * CH], 180, 360, fill=rgb('#CE1126'))


def f_mli(f):
    f.vs(['#14B53A', '#FCD116', '#CE1126'])


def f_mtn(f):
    f.hs(['#D01C1F', '#00A95C', '#D01C1F'], [1, 6, 1])
    f.crescent(0.5, 0.42, 0.22, '#FFD700', '#00A95C', off=-0.0, k=0.0)
    f.disc(0.5, 0.36, 0.2, '#00A95C')
    f.star(0.5, 0.4, 0.07, '#FFD700')


def f_mri(f):
    f.hs(['#EA2839', '#1A206D', '#FFD500', '#00A551'])


def f_moz(f):
    f.hs(['#007168', WHITE, BLACK, WHITE, '#FCE100'], [5, 1, 5, 1, 5])
    f.hoist_tri(0.4, '#D21034')
    f.star(0.14, 0.5, 0.11, '#FCE100')


def f_nam(f):
    f.poly([(0, 0), (0.85, 0), (0, 0.95)], '#003580')
    f.poly([(1, 1), (0.15, 1), (1, 0.05)], '#009543')
    f.band((0, 1), (1, 0), 0.34, WHITE)
    f.band((0, 1), (1, 0), 0.26, '#D21034')
    sun(f, 0.18, 0.2, 0.07, '#FFCE00', rays=12, ray=1.6)


def f_nig(f):
    f.hs(['#E05206', WHITE, '#0DB02B'])
    f.disc(0.5, 0.5, 0.11, '#E05206')


def f_rwa(f):
    f.hs(['#00A1DE', '#FAD201', '#20603D'], [2, 1, 1])
    sun(f, 0.85, 0.25, 0.06, '#FAD201', rays=24, ray=1.6)


def f_stp(f):
    f.hs(['#12AD2B', '#FFCE00', '#12AD2B'], [2, 3, 2])
    f.hoist_tri(0.3, '#D21034')
    f.star(0.53, 0.5, 0.11, BLACK)
    f.star(0.76, 0.5, 0.11, BLACK)


def f_sey(f):
    cols = ['#003F87', '#FCD856', '#D62828', WHITE, '#007A3D']
    edge = [(0, 0), (1 / 3, 0), (2 / 3, 0), (1, 0), (1, 0.5), (1, 1)]
    for k, c in enumerate(cols):
        f.poly([(0, 1), edge[k], edge[k + 1]], c)


def f_sle(f):
    f.hs(['#1EB53A', WHITE, '#0072C6'])


def f_som(f):
    f.rect(0, 0, 1, 1, '#4189DD')
    f.star(0.5, 0.5, 0.25, WHITE)


def f_ssd(f):
    f.hs([BLACK, WHITE, '#DA121A', WHITE, '#078930'], [6, 1, 6, 1, 6])
    f.hoist_tri(0.4, '#0F47AF')
    f.star(0.13, 0.5, 0.1, '#FCDD09')


def f_sud(f):
    f.hs(['#D21034', WHITE, BLACK])
    f.hoist_tri(0.33, '#007229')


def f_tan(f):
    f.poly([(0, 0), (1, 0), (0, 1)], '#1EB53A')
    f.poly([(1, 0), (1, 1), (0, 1)], '#00A3DD')
    f.band((0, 1), (1, 0), 0.34, '#FCD116')
    f.band((0, 1), (1, 0), 0.24, BLACK)


def f_tog(f):
    f.hs(['#006A4E', '#FFCE00'] * 2 + ['#006A4E'])
    f.rect(0, 0, 0.4, 0.6, '#D21034')
    f.star(0.2, 0.3, 0.15, WHITE)


def f_uga(f):
    f.hs([BLACK, '#FCDC04', '#D90000'] * 2)
    f.disc(0.5, 0.5, 0.18, WHITE)
    f.poly([(0.46, 0.62), (0.5, 0.38), (0.56, 0.44), (0.54, 0.62)], '#9CA69C')


def f_zam(f):
    f.rect(0, 0, 1, 1, '#198A00')
    for k, c in enumerate(('#DE2010', BLACK, '#EF7D00')):
        f.rect(0.7 + k * 0.1, 0.4, 0.8 + k * 0.1, 1, c)
    f.poly([(0.74, 0.22), (0.85, 0.16), (0.96, 0.22), (0.85, 0.3)], '#EF7D00')


def f_zim(f):
    f.hs(['#006400', '#FFD200', '#D40000', BLACK, '#D40000', '#FFD200', '#006400'])
    f.poly([(0, 0), (0.45, 0.5), (0, 1)], BLACK)
    f.poly([(0, 0.04), (0.41, 0.5), (0, 0.96)], WHITE)
    f.star(0.14, 0.5, 0.1, '#D40000')


def f_ant(f):
    f.rect(0, 0, 1, 1, '#CE1126')
    f.poly([(0, 0), (1, 0), (0.5, 1)], BLACK)
    f.poly([(0.21, 0.42), (0.79, 0.42), (0.67, 0.66), (0.33, 0.66)], '#0072C6')
    f.poly([(0.33, 0.66), (0.67, 0.66), (0.5, 1)], WHITE)
    f.d.pieslice([0.38 * CW, 0.22 * CH, 0.62 * CW, 0.62 * CH], 180, 360, fill=rgb('#FCD116'))


def f_bah(f):
    f.hs(['#00778B', '#FFC72C', '#00778B'])
    f.hoist_tri(0.42, BLACK)


def f_brb(f):
    f.vs(['#00267F', '#FFC726', '#00267F'])
    f.rect(0.49, 0.32, 0.51, 0.75, BLACK)
    f.poly([(0.42, 0.3), (0.45, 0.48), (0.55, 0.48), (0.58, 0.3), (0.55, 0.42), (0.45, 0.42)], BLACK)


def f_blz(f):
    f.hs(['#CE1126', '#003F87', '#CE1126'], [1, 8, 1])
    f.disc(0.5, 0.5, 0.28, WHITE)
    f.ring(0.5, 0.5, 0.26, 0.03, '#3A7728')


def f_bol(f):
    f.hs(['#D52B1E', '#F9E300', '#007934'])


def f_dma(f):
    f.rect(0, 0, 1, 1, '#006B3F')
    cross(f, BLACK, 0.18, cx=0.5)
    f.rect(0.5 - 0.09 * CH / CW, 0, 0.5 - 0.03 * CH / CW, 1, '#FCD116')
    f.rect(0, 0.41, 1, 0.47, '#FCD116')
    f.rect(0.5 + 0.03 * CH / CW, 0, 0.5 + 0.09 * CH / CW, 1, WHITE)
    f.rect(0, 0.53, 1, 0.59, WHITE)
    f.disc(0.5, 0.5, 0.18, '#D41C30')
    f.disc(0.5, 0.5, 0.06, '#9461C9')


def f_ecu(f):
    f.hs(['#FFDD00', '#034EA2', '#ED1C24'], [2, 1, 1])
    emblem(f, '#7A5C2E', r=0.14)


def f_esa(f):
    f.hs(['#0047AB', WHITE, '#0047AB'])
    f.poly([(0.44, 0.58), (0.5, 0.42), (0.56, 0.58)], '#FFD700')


def f_grn(f):
    f.rect(0, 0, 1, 1, '#CE1126')
    f.poly([(0.08, 0.12), (0.92, 0.12), (0.5, 0.5)], '#FCD116')
    f.poly([(0.08, 0.88), (0.92, 0.88), (0.5, 0.5)], '#FCD116')
    f.poly([(0.08, 0.12), (0.5, 0.5), (0.08, 0.88)], '#007A5E')
    f.poly([(0.92, 0.12), (0.5, 0.5), (0.92, 0.88)], '#007A5E')
    f.disc(0.5, 0.5, 0.1, '#CE1126')
    f.star(0.5, 0.5, 0.07, '#FCD116')
    for x in (0.25, 0.5, 0.75):
        f.star(x, 0.06, 0.035, '#FCD116')
        f.star(x, 0.94, 0.035, '#FCD116')


def f_gua(f):
    f.vs(['#4997D0', WHITE, '#4997D0'])
    f.ring(0.5, 0.5, 0.13, 0.03, '#2E7D32')


def f_guy(f):
    f.rect(0, 0, 1, 1, '#009E49')
    f.poly([(0, 0), (1, 0.5), (0, 1)], WHITE)
    f.poly([(0, 0.03), (0.96, 0.5), (0, 0.97)], '#FCD116')
    f.poly([(0, 0), (0.5, 0.5), (0, 1)], BLACK)
    f.poly([(0, 0.05), (0.45, 0.5), (0, 0.95)], '#CE1126')


def f_hai(f):
    f.hs(['#00209F', '#D21034'])
    f.rect(0.38, 0.36, 0.62, 0.64, WHITE)
    f.disc(0.5, 0.5, 0.08, '#3A7728')


def f_hon(f):
    f.hs(['#0073CF', WHITE, '#0073CF'])
    for x, y in ((0.5, 0.5), (0.4, 0.43), (0.4, 0.57), (0.6, 0.43), (0.6, 0.57)):
        f.star(x, y, 0.035, '#0073CF')


def f_nca(f):
    f.hs(['#0067C6', WHITE, '#0067C6'])
    f.poly([(0.44, 0.58), (0.5, 0.42), (0.56, 0.58)], '#2E8B57')


def f_par(f):
    f.hs(['#D52B1E', WHITE, '#0038A8'])
    f.ring(0.5, 0.5, 0.12, 0.025, '#2E7D32')
    f.star(0.5, 0.5, 0.05, '#FFD700')


def f_skn(f):
    f.poly([(0, 0), (1, 0), (0, 1)], '#009E49')
    f.poly([(1, 0), (1, 1), (0, 1)], '#CE1126')
    f.band((0, 1), (1, 0), 0.34, '#FCD116')
    f.band((0, 1), (1, 0), 0.24, BLACK)
    f.star(0.38, 0.6, 0.07, WHITE, rot=-0.5)
    f.star(0.62, 0.4, 0.07, WHITE, rot=-0.5)


def f_lca(f):
    f.rect(0, 0, 1, 1, '#66CCFF')
    f.poly([(0.5, 0.15), (0.72, 0.85), (0.28, 0.85)], WHITE)
    f.poly([(0.5, 0.22), (0.68, 0.85), (0.32, 0.85)], BLACK)
    f.poly([(0.5, 0.5), (0.72, 0.85), (0.28, 0.85)], '#FCD116')


def f_vin(f):
    f.vs(['#002674', '#FCD116', '#009E60'], [1, 2, 1])
    for x, y in ((0.42, 0.42), (0.58, 0.42), (0.5, 0.6)):
        f.poly([(x, y - 0.1), (x + 0.04, y), (x, y + 0.1), (x - 0.04, y)], '#009E60')


def f_sur(f):
    f.hs(['#377E3F', WHITE, '#B40A2D', WHITE, '#377E3F'], [2, 1, 4, 1, 2])
    f.star(0.5, 0.5, 0.16, '#ECC81D')


def f_tto(f):
    f.rect(0, 0, 1, 1, '#DA1A35')
    f.band((0, 0), (1, 1), 0.34, WHITE)
    f.band((0, 0), (1, 1), 0.26, BLACK)


def f_ven(f):
    f.hs(['#FFCC00', '#00247D', '#CF142B'])
    for i in range(8):
        a = math.pi * (1.15 + i * 0.1)
        f.star(0.5 + 0.22 * math.cos(a) * CH / CW, 0.62 + 0.22 * math.sin(a), 0.03, WHITE)


def f_fij(f):
    f.rect(0, 0, 1, 1, '#68BFE5')
    f.union_jack(0, 0, 0.5, 0.5)
    emblem(f, WHITE, cx=0.75, cy=0.55, r=0.17)
    f.rect(0.68, 0.38, 0.82, 0.46, '#CE1126')


def f_kir(f):
    f.rect(0, 0, 1, 0.55, '#CE1126')
    sun(f, 0.5, 0.52, 0.12, '#FCD116', rays=14, ray=1.7)
    f.rect(0, 0.55, 1, 1, '#003F87')
    for k in range(3):
        f.rect(0, 0.62 + k * 0.12, 1, 0.66 + k * 0.12, WHITE)
    f.poly([(0.36, 0.22), (0.5, 0.14), (0.64, 0.22), (0.5, 0.2)], '#FCD116')


def f_mhl(f):
    f.rect(0, 0, 1, 1, '#003893')
    f.band((0, 0.98), (1, 0.2), 0.07, '#DD7500')
    f.band((0, 0.9), (1, 0.12), 0.07, WHITE)
    f.star(0.2, 0.25, 0.13, WHITE, n=12, inner=0.45)


def f_fsm(f):
    f.rect(0, 0, 1, 1, '#75B2DD')
    for x, y in ((0.5, 0.28), (0.5, 0.72), (0.36, 0.5), (0.64, 0.5)):
        f.star(x, y, 0.08, WHITE)


def f_nru(f):
    f.rect(0, 0, 1, 1, '#002B7F')
    f.rect(0, 0.46, 1, 0.54, '#FFC61E')
    f.star(0.27, 0.72, 0.1, WHITE, n=12, inner=0.5)


def f_plw(f):
    f.rect(0, 0, 1, 1, '#4AADD6')
    f.disc(0.45, 0.5, 0.3, '#FFDE00')


def f_png(f):
    f.poly([(0, 0), (1, 0), (1, 1)], '#CE1126')
    f.poly([(0, 0), (1, 1), (0, 1)], BLACK)
    f.poly([(0.6, 0.18), (0.82, 0.28), (0.74, 0.34), (0.86, 0.46), (0.66, 0.38), (0.6, 0.3)], '#FCD116')
    for x, y in ((0.25, 0.55), (0.18, 0.7), (0.32, 0.7), (0.25, 0.86), (0.28, 0.68)):
        f.star(x, y, 0.035, WHITE)


def f_sam(f):
    f.rect(0, 0, 1, 1, '#CE1126')
    f.rect(0, 0, 0.5, 0.5, '#002B7F')
    for x, y in ((0.25, 0.1), (0.15, 0.24), (0.35, 0.22), (0.25, 0.4), (0.29, 0.3)):
        f.star(x, y, 0.045, WHITE)


def f_sol(f):
    f.poly([(0, 0), (1, 0), (0, 1)], '#0051BA')
    f.poly([(1, 0), (1, 1), (0, 1)], '#215B33')
    f.band((0, 1), (1, 0), 0.06, '#FCD116')
    for x, y in ((0.1, 0.1), (0.26, 0.1), (0.1, 0.3), (0.26, 0.3), (0.18, 0.2)):
        f.star(x, y, 0.045, WHITE)


def f_tga(f):
    f.rect(0, 0, 1, 1, '#C10000')
    f.rect(0, 0, 0.42, 0.5, WHITE)
    f.rect(0.18, 0.08, 0.24, 0.42, '#C10000')
    f.rect(0.09, 0.21, 0.33, 0.29, '#C10000')


def f_tuv(f):
    f.rect(0, 0, 1, 1, '#5B97B1')
    f.union_jack(0, 0, 0.5, 0.5)
    for x, y in ((0.62, 0.3), (0.7, 0.5), (0.85, 0.4), (0.8, 0.72), (0.6, 0.72), (0.9, 0.86), (0.7, 0.88), (0.94, 0.6), (0.55, 0.88)):
        f.star(x, y, 0.04, '#FFCE00')


def f_van(f):
    f.hs(['#D21034', '#009543'])
    f.poly([(0, 0), (0.42, 0.5), (0, 1)], BLACK)
    f.band((0, 0.02), (0.45, 0.5), 0.05, '#FDCE12')
    f.band((0, 0.98), (0.45, 0.5), 0.05, '#FDCE12')
    f.rect(0.4, 0.47, 1, 0.53, '#FDCE12')
    f.ring(0.17, 0.5, 0.1, 0.025, '#FDCE12')


def f_earth(f):   # Earth: the blue marble
    f.rect(0, 0, 1, 1, '#0B1E46')
    f.disc(0.5, 0.5, 0.36, '#1F6FD0')
    f.poly([(0.38, 0.26), (0.52, 0.22), (0.56, 0.36), (0.46, 0.5), (0.5, 0.66), (0.42, 0.72), (0.36, 0.52)], '#3FA04A')
    f.poly([(0.58, 0.4), (0.66, 0.36), (0.68, 0.5), (0.6, 0.58)], '#3FA04A')
    f.d.ellipse([0.43 * CW, 0.15 * CH, 0.57 * CW, 0.22 * CH], fill=rgb(WHITE))


def f_galaxy(f):   # the Galaxy: a spiral of stars
    f.rect(0, 0, 1, 1, '#07041A')
    rnd = __import__('random').Random(3)
    for i in range(60):
        f.disc(rnd.random(), rnd.random(), 0.006, '#C8C8FF')
    for arm in range(2):
        for i in range(34):
            t = i / 34
            a = arm * math.pi + t * 3.6
            r = 0.04 + t * 0.4
            f.disc(0.5 + r * math.cos(a) * CH / CW, 0.5 + r * math.sin(a), 0.03 * (1 - t) + 0.008, '#E8D8FF' if i % 3 else '#9F7BFF')
    f.disc(0.5, 0.5, 0.07, '#FFF2C8')


WORLD = [
    ('RUS', 'RUSSIA', f_rus, (WHITE, '#0039A6', '#D52B1E'), ['#0039A6', '#D52B1E']),
    ('BLR', 'BELARUS', f_blr, ('#C8313E', WHITE, '#4AA657'), ['#C8313E', '#4AA657']),
    ('TPE', 'TAIWAN', f_tpe, ('#FE0000', WHITE, '#000095'), ['#FE0000', '#000095']),
    ('ALB', 'ALBANIA', f_alb, ('#E41E20', BLACK, BLACK), ['#E41E20']),
    ('AND', 'ANDORRA', f_and, ('#10069F', '#FEDF00', '#D50032'), ['#10069F', '#FEDF00']),
    ('ARM', 'ARMENIA', f_arm, ('#D90012', '#0033A0', '#F2A800'), ['#D90012', '#F2A800']),
    ('AZE', 'AZERBAIJAN', f_aze, ('#0092BC', WHITE, '#E4002B'), ['#0092BC', '#00AF66']),
    ('BIH', 'BOSNIA AND HERZEGOVINA', f_bih, ('#002395', WHITE, '#FECB00'), ['#002395', '#FECB00']),
    ('CYP', 'CYPRUS', f_cyp, (WHITE, '#D57800', '#4E5B31'), ['#D57800']),
    ('GEO', 'GEORGIA', f_geo, (WHITE, '#FF0000', '#FF0000'), ['#FF0000']),
    ('KOS', 'KOSOVO', f_kos, ('#244AA5', WHITE, '#D0A650'), ['#244AA5', '#D0A650']),
    ('LIE', 'LIECHTENSTEIN', f_lie, ('#002B7F', WHITE, '#CE1126'), ['#002B7F', '#CE1126']),
    ('LUX', 'LUXEMBOURG', f_lux, ('#00A3E0', WHITE, '#EF3340'), ['#00A3E0', '#EF3340']),
    ('MLT', 'MALTA', f_mlt, ('#CF142B', WHITE, '#CF142B'), ['#CF142B']),
    ('MDA', 'MOLDOVA', f_mda, ('#0046AE', '#FFD200', '#CC092F'), ['#0046AE', '#FFD200']),
    ('MON', 'MONACO', f_mon, ('#CE1126', WHITE, '#CE1126'), ['#CE1126']),
    ('MNE', 'MONTENEGRO', f_mne, ('#C40308', '#D3AE3B', '#D3AE3B'), ['#C40308', '#D3AE3B']),
    ('MKD', 'NORTH MACEDONIA', f_mkd, ('#D20000', '#FFE600', '#FFE600'), ['#D20000', '#FFE600']),
    ('SMR', 'SAN MARINO', f_smr, ('#5EB6E4', WHITE, '#C8A951'), ['#5EB6E4']),
    ('SRB', 'SERBIA', f_srb, ('#C6363C', WHITE, '#0C4076'), ['#C6363C', '#0C4076']),
    ('VAT', 'VATICAN CITY', f_vat, ('#FFE000', WHITE, '#FFE000'), ['#FFE000']),
    ('ENG', 'ENGLAND', f_eng, (WHITE, '#CE1124', '#CE1124'), ['#CE1124']),
    ('SCO', 'SCOTLAND', f_sco, ('#005EB8', WHITE, WHITE), ['#005EB8']),
    ('WAL', 'WALES', f_wal, ('#D30731', WHITE, '#00B140'), ['#D30731', '#00B140']),
    ('AFG', 'AFGHANISTAN', f_afg, (BLACK, '#D32011', '#007A36'), ['#D32011', '#007A36']),
    ('BRN', 'BAHRAIN', f_bhr, ('#CE1126', WHITE, '#CE1126'), ['#CE1126']),
    ('BHU', 'BHUTAN', f_btn, ('#FF4E12', '#FFD520', WHITE), ['#FF4E12', '#FFD520']),
    ('BRU', 'BRUNEI', f_brn, ('#F7E017', BLACK, '#CF1126'), ['#F7E017', '#CF1126']),
    ('CAM', 'CAMBODIA', f_khm, ('#032EA1', WHITE, '#E00025'), ['#032EA1', '#E00025']),
    ('HKG', 'HONG KONG', f_hkg, ('#DE2910', WHITE, '#DE2910'), ['#DE2910']),
    ('IRI', 'IRAN', f_irn, ('#239F40', WHITE, '#DA0000'), ['#239F40', '#DA0000']),
    ('IRQ', 'IRAQ', f_irq, ('#CE1126', WHITE, '#007A3D'), ['#CE1126', '#007A3D']),
    ('JOR', 'JORDAN', f_jor, (BLACK, WHITE, '#CE1126'), ['#CE1126', '#007A3D']),
    ('KAZ', 'KAZAKHSTAN', f_kaz, ('#00AFCA', WHITE, '#FEC50C'), ['#00AFCA', '#FEC50C']),
    ('KUW', 'KUWAIT', f_kwt, ('#007A3D', WHITE, '#CE1126'), ['#007A3D', '#CE1126']),
    ('KGZ', 'KYRGYZSTAN', f_kgz, ('#E8112D', '#FFEF00', '#FFEF00'), ['#E8112D', '#FFEF00']),
    ('LAO', 'LAOS', f_lao, ('#002868', WHITE, '#CE1126'), ['#002868', '#CE1126']),
    ('LBN', 'LEBANON', f_lbn, (WHITE, '#ED1C24', '#00A651'), ['#ED1C24', '#00A651']),
    ('MAS', 'MALAYSIA', f_mas, ('#010066', WHITE, '#CC0001'), ['#010066', '#CC0001']),
    ('MDV', 'MALDIVES', f_mdv, ('#D21034', WHITE, '#007E3A'), ['#D21034', '#007E3A']),
    ('MGL', 'MONGOLIA', f_mgl, ('#015197', '#DA2032', '#FFD900'), ['#015197', '#DA2032']),
    ('MYA', 'MYANMAR', f_mya, ('#34B233', '#FECB00', '#EA2839'), ['#34B233', '#FECB00']),
    ('NEP', 'NEPAL', f_npl, ('#DC143C', WHITE, '#003893'), ['#DC143C', '#003893']),
    ('PRK', 'NORTH KOREA', f_prk, ('#ED1C27', WHITE, '#024FA2'), ['#ED1C27', '#024FA2']),
    ('OMA', 'OMAN', f_omn, ('#DB161B', WHITE, '#008000'), ['#DB161B', '#008000']),
    ('PLE', 'PALESTINE', f_ple, (BLACK, WHITE, '#007A3D'), ['#007A3D', '#CE1126']),
    ('KSA', 'SAUDI ARABIA', f_ksa, ('#006C35', WHITE, WHITE), ['#006C35']),
    ('SRI', 'SRI LANKA', f_sri, ('#8D153A', '#FFBE29', '#EB7400'), ['#8D153A', '#FFBE29']),
    ('SYR', 'SYRIA', f_syr, ('#007A3D', WHITE, '#CE1126'), ['#007A3D', '#CE1126']),
    ('TJK', 'TAJIKISTAN', f_tjk, (WHITE, '#CC0000', '#006600'), ['#CC0000', '#006600']),
    ('TLS', 'TIMOR-LESTE', f_tls, ('#DC241F', '#FFC726', BLACK), ['#DC241F', '#FFC726']),
    ('TKM', 'TURKMENISTAN', f_tkm, ('#00843D', WHITE, '#C8102E'), ['#00843D', '#C8102E']),
    ('UZB', 'UZBEKISTAN', f_uzb, ('#0099B5', WHITE, '#1EB53A'), ['#0099B5', '#1EB53A']),
    ('YEM', 'YEMEN', f_yem, ('#CE1126', WHITE, BLACK), ['#CE1126']),
    ('ANG', 'ANGOLA', f_ago, ('#CC092F', BLACK, '#FFCB00'), ['#CC092F', '#FFCB00']),
    ('BEN', 'BENIN', f_ben, ('#008751', '#FCD116', '#E8112D'), ['#008751', '#FCD116']),
    ('BOT', 'BOTSWANA', f_bot, ('#75AADB', BLACK, WHITE), ['#75AADB']),
    ('BUR', 'BURKINA FASO', f_bur, ('#EF2B2D', '#009E49', '#FCD116'), ['#EF2B2D', '#009E49']),
    ('BDI', 'BURUNDI', f_bdi, ('#1EB53A', WHITE, '#CE1126'), ['#1EB53A', '#CE1126']),
    ('CPV', 'CABO VERDE', f_cpv, ('#003893', WHITE, '#F7D116'), ['#003893', '#F7D116']),
    ('CAF', 'CENTRAL AFRICAN REPUBLIC', f_caf, ('#003082', WHITE, '#D21034'), ['#003082', '#D21034']),
    ('CHA', 'CHAD', f_cha, ('#002664', '#FECB00', '#C60C30'), ['#002664', '#FECB00']),
    ('COM', 'COMOROS', f_com, ('#3D8E33', WHITE, '#FFC61E'), ['#3D8E33', '#FFC61E']),
    ('COD', 'DR CONGO', f_cod, ('#007FFF', '#F7D618', '#CE1021'), ['#007FFF', '#CE1021']),
    ('CGO', 'CONGO', f_cgo, ('#009543', '#FBDE4A', '#DC241F'), ['#009543', '#DC241F']),
    ('DJI', 'DJIBOUTI', f_dji, ('#6AB2E7', WHITE, '#12AD2B'), ['#6AB2E7', '#12AD2B']),
    ('GEQ', 'EQUATORIAL GUINEA', f_gnq, ('#3E9A00', WHITE, '#E32118'), ['#3E9A00', '#E32118']),
    ('ERI', 'ERITREA', f_eri, ('#EA0437', '#FFC726', '#4189DD'), ['#EA0437', '#4189DD']),
    ('SWZ', 'ESWATINI', f_swz, ('#3E5EB9', '#FFD900', '#B10C0C'), ['#3E5EB9', '#B10C0C']),
    ('GAB', 'GABON', f_gab, ('#009E60', '#FCD116', '#3A75C4'), ['#009E60', '#FCD116']),
    ('GAM', 'GAMBIA', f_gam, ('#0C1C8C', WHITE, '#CE1126'), ['#CE1126', '#3A7728']),
    ('GUI', 'GUINEA', f_gui, ('#CE1126', '#FCD116', '#009460'), ['#CE1126', '#009460']),
    ('GBS', 'GUINEA-BISSAU', f_gbs, ('#CE1126', '#FCD116', '#009E49'), ['#CE1126', '#009E49']),
    ('LES', 'LESOTHO', f_les, ('#00209F', WHITE, '#009543'), ['#00209F', '#009543']),
    ('LBR', 'LIBERIA', f_lbr, ('#002868', WHITE, '#BF0A30'), ['#BF0A30', '#002868']),
    ('LBA', 'LIBYA', f_lby, (BLACK, WHITE, '#E70013'), ['#239E46', '#E70013']),
    ('MAD', 'MADAGASCAR', f_mad, ('#FC3D32', WHITE, '#007E3A'), ['#FC3D32', '#007E3A']),
    ('MAW', 'MALAWI', f_maw, (BLACK, '#CE1126', '#339E35'), ['#CE1126', '#339E35']),
    ('MLI', 'MALI', f_mli, ('#14B53A', '#FCD116', '#CE1126'), ['#14B53A', '#FCD116']),
    ('MTN', 'MAURITANIA', f_mtn, ('#00A95C', '#FFD700', '#D01C1F'), ['#00A95C', '#FFD700']),
    ('MRI', 'MAURITIUS', f_mri, ('#1A206D', '#FFD500', '#EA2839'), ['#EA2839', '#00A551']),
    ('MOZ', 'MOZAMBIQUE', f_moz, ('#007168', '#FCE100', '#D21034'), ['#007168', '#FCE100']),
    ('NAM', 'NAMIBIA', f_nam, ('#003580', WHITE, '#D21034'), ['#003580', '#009543']),
    ('NIG', 'NIGER', f_nig, ('#E05206', WHITE, '#0DB02B'), ['#E05206', '#0DB02B']),
    ('RWA', 'RWANDA', f_rwa, ('#00A1DE', '#FAD201', '#20603D'), ['#00A1DE', '#FAD201']),
    ('STP', 'SAO TOME AND PRINCIPE', f_stp, ('#12AD2B', '#FFCE00', '#D21034'), ['#12AD2B', '#FFCE00']),
    ('SEY', 'SEYCHELLES', f_sey, ('#003F87', '#FCD856', '#D62828'), ['#003F87', '#D62828']),
    ('SLE', 'SIERRA LEONE', f_sle, ('#1EB53A', WHITE, '#0072C6'), ['#1EB53A', '#0072C6']),
    ('SOM', 'SOMALIA', f_som, ('#4189DD', WHITE, WHITE), ['#4189DD']),
    ('SSD', 'SOUTH SUDAN', f_ssd, (BLACK, '#DA121A', '#078930'), ['#078930', '#0F47AF']),
    ('SUD', 'SUDAN', f_sud, ('#D21034', WHITE, '#007229'), ['#D21034', '#007229']),
    ('TAN', 'TANZANIA', f_tan, ('#1EB53A', '#FCD116', '#00A3DD'), ['#1EB53A', '#00A3DD']),
    ('TOG', 'TOGO', f_tog, ('#006A4E', '#FFCE00', '#D21034'), ['#006A4E', '#FFCE00']),
    ('UGA', 'UGANDA', f_uga, (BLACK, '#FCDC04', '#D90000'), ['#FCDC04', '#D90000']),
    ('ZAM', 'ZAMBIA', f_zam, ('#198A00', '#EF7D00', '#DE2010'), ['#198A00', '#EF7D00']),
    ('ZIM', 'ZIMBABWE', f_zim, ('#006400', '#FFD200', '#D40000'), ['#006400', '#FFD200']),
    ('ANT', 'ANTIGUA AND BARBUDA', f_ant, ('#CE1126', BLACK, '#FCD116'), ['#CE1126', '#0072C6']),
    ('BAH', 'BAHAMAS', f_bah, ('#00778B', '#FFC72C', BLACK), ['#00778B', '#FFC72C']),
    ('BAR', 'BARBADOS', f_brb, ('#00267F', '#FFC726', BLACK), ['#00267F', '#FFC726']),
    ('BIZ', 'BELIZE', f_blz, ('#003F87', WHITE, '#CE1126'), ['#003F87', '#CE1126']),
    ('BOL', 'BOLIVIA', f_bol, ('#007934', '#F9E300', '#D52B1E'), ['#D52B1E', '#007934']),
    ('DMA', 'DOMINICA', f_dma, ('#006B3F', '#FCD116', '#D41C30'), ['#006B3F', '#D41C30']),
    ('ECU', 'ECUADOR', f_ecu, ('#FFDD00', '#034EA2', '#ED1C24'), ['#FFDD00', '#034EA2']),
    ('ESA', 'EL SALVADOR', f_esa, ('#0047AB', WHITE, '#0047AB'), ['#0047AB']),
    ('GRN', 'GRENADA', f_grn, ('#CE1126', '#FCD116', '#007A5E'), ['#CE1126', '#007A5E']),
    ('GUA', 'GUATEMALA', f_gua, ('#4997D0', WHITE, '#4997D0'), ['#4997D0']),
    ('GUY', 'GUYANA', f_guy, ('#009E49', '#FCD116', '#CE1126'), ['#009E49', '#FCD116']),
    ('HAI', 'HAITI', f_hai, ('#00209F', WHITE, '#D21034'), ['#00209F', '#D21034']),
    ('HON', 'HONDURAS', f_hon, ('#0073CF', WHITE, '#0073CF'), ['#0073CF']),
    ('NCA', 'NICARAGUA', f_nca, ('#0067C6', WHITE, '#0067C6'), ['#0067C6']),
    ('PAR', 'PARAGUAY', f_par, ('#0038A8', WHITE, '#D52B1E'), ['#D52B1E', '#0038A8']),
    ('SKN', 'SAINT KITTS AND NEVIS', f_skn, ('#009E49', '#FCD116', '#CE1126'), ['#009E49', '#CE1126']),
    ('LCA', 'SAINT LUCIA', f_lca, ('#66CCFF', '#FCD116', BLACK), ['#66CCFF', '#FCD116']),
    ('VIN', 'SAINT VINCENT', f_vin, ('#002674', '#FCD116', '#009E60'), ['#002674', '#009E60']),
    ('SUR', 'SURINAME', f_sur, ('#377E3F', WHITE, '#B40A2D'), ['#377E3F', '#B40A2D']),
    ('TTO', 'TRINIDAD AND TOBAGO', f_tto, ('#DA1A35', BLACK, WHITE), ['#DA1A35']),
    ('VEN', 'VENEZUELA', f_ven, ('#FFCC00', '#00247D', '#CF142B'), ['#FFCC00', '#00247D']),
    ('FIJ', 'FIJI', f_fij, ('#68BFE5', WHITE, '#012169'), ['#68BFE5', '#012169']),
    ('KIR', 'KIRIBATI', f_kir, ('#CE1126', '#FCD116', '#003F87'), ['#CE1126', '#003F87']),
    ('MHL', 'MARSHALL ISLANDS', f_mhl, ('#003893', WHITE, '#DD7500'), ['#003893', '#DD7500']),
    ('FSM', 'MICRONESIA', f_fsm, ('#75B2DD', WHITE, WHITE), ['#75B2DD']),
    ('NRU', 'NAURU', f_nru, ('#002B7F', '#FFC61E', WHITE), ['#002B7F', '#FFC61E']),
    ('PLW', 'PALAU', f_plw, ('#4AADD6', '#FFDE00', '#FFDE00'), ['#4AADD6', '#FFDE00']),
    ('PNG', 'PAPUA NEW GUINEA', f_png, (BLACK, '#CE1126', '#FCD116'), ['#CE1126', '#FCD116']),
    ('SAM', 'SAMOA', f_sam, ('#CE1126', WHITE, '#002B7F'), ['#CE1126', '#002B7F']),
    ('SOL', 'SOLOMON ISLANDS', f_sol, ('#0051BA', '#FCD116', '#215B33'), ['#0051BA', '#215B33']),
    ('TGA', 'TONGA', f_tga, ('#C10000', WHITE, '#C10000'), ['#C10000']),
    ('TUV', 'TUVALU', f_tuv, ('#5B97B1', WHITE, '#FFCE00'), ['#5B97B1', '#FFCE00']),
    ('VAN', 'VANUATU', f_van, ('#D21034', '#FDCE12', '#009543'), ['#D21034', '#009543']),
    ('ERT', 'EARTH', f_earth, ('#1F6FD0', WHITE, '#3FA04A'), ['#1F6FD0', '#3FA04A']),
    ('GLX', 'THE GALAXY', f_galaxy, ('#07041A', '#9F7BFF', '#E8D8FF'), ['#9F7BFF', '#E8D8FF']),
]


def f_rainbow(f):
    f.hs(['#E40303', '#FF8C00', '#FFED00', '#008026', '#004DFF', '#750787'])


WORLD.append(('RBW', 'RAINBOW', f_rainbow, ('#004DFF', '#FFED00', '#E40303'), ['#E40303', '#008026']))   # (owner, 2026-10-20)
NATIONS += WORLD   # (appended: the first 73 keep their numbers)


def lin(c):   # sRGB 0..255 -> linear 0..1
    c = np.array(c) / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def team_glow(hexc):
    """A flag colour as a glow: linear, brightest channel 1 (dark navy becomes a vivid blue)."""
    v = lin(rgb(hexc))
    m = v.max()
    if m < 1e-4:
        return (1.0, 1.0, 1.0)
    return tuple(float(x) for x in np.clip(v / m, 0, 1))


def draw(nat):
    code, name, fn, kit, team = nat
    f = Flag()
    fn(f)
    return f.im.resize((W, H), Image.LANCZOS)


def write_tex(path, im):
    a = np.asarray(im.convert('RGBA'))
    with open(path, 'wb') as fh:
        fh.write(b'STEX' + struct.pack('<3I', W, H, 0))
        fh.write(a.tobytes())


def main():
    assert len(NATIONS) <= 255
    codes = [n[0] for n in NATIONS]
    assert len(set(codes)) == len(codes), 'duplicate code'
    os.makedirs(TEX, exist_ok=True)
    ims = []
    for n in NATIONS:
        im = draw(n)
        write_tex(os.path.join(TEX, 'flag_%s.tex' % n[0].lower()), im)
        ims.append(im)
    lines = ['// Generated by tools/gen_flags.py: do not edit. NATIONS (country presets): APPEND-ONLY, the position + 1 is',
             '// the country number in AvatarDesc. kit = jersey primary, secondary, accent (sRGB); team = glow colours (linear,',
             '// brightest channel 1) by preference.']
    for code, name, fn, kit, team in NATIONS:
        k = ', '.join('{%d, %d, %d}' % rgb(c) for c in kit)
        t = list(team) + [team[0]] * (2 - len(team))
        tg = ', '.join('{%.3ff, %.3ff, %.3ff}' % team_glow(c) for c in t[:2])
        lines.append('    {"%s", "%s", "flag_%s", {%s}, {%s}},' % (code, name, code.lower(), k, tg))
    open(INC, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines) + '\n')
    cols = 10
    rows = (len(ims) + cols - 1) // cols
    sheet = Image.new('RGB', (cols * (W + 8) + 8, rows * (H + 24) + 8), (40, 40, 48))
    dr = ImageDraw.Draw(sheet)
    for i, (im, n) in enumerate(zip(ims, NATIONS)):
        x, y = 8 + (i % cols) * (W + 8), 8 + (i // cols) * (H + 24)
        sheet.paste(im, (x, y))
        dr.text((x, y + H + 4), '%d %s' % (i + 1, n[0]), fill=(230, 230, 230))
    os.makedirs(os.path.join(ROOT, 'build'), exist_ok=True)
    sheet.save(os.path.join(ROOT, 'build', 'flags_preview.png'))
    print('%d nations: %s and app/assets_open/tex/flag_*.tex; preview build/flags_preview.png' % (len(NATIONS), INC))


if __name__ == '__main__':
    sys.exit(main())
