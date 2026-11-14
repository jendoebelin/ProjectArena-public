"""Print-ready logos for Project Arena merch (t-shirts, hoodies, jerseys, sweatpants): vector SVG and PDF, and 300 dpi
PNG on transparency, in three colourways. Everything is drawn here as shapes: the emblem redrawn from the game's own
(tools/gen_open_textures.py emblem(): the orbit ring round a seamed ball, blue left, orange right) and the title
lettered as on the store art (Bahnschrift SemiBold, the letters spread to the same widths), the letters turned into
outlines so the printer needs no font.

    python -I tools/gen_merch_logos.py      ->  build/merch/<design>_<colourway>.svg / .pdf / .png, preview.png

Designs: emblem; stacked (PROJECT / ARENA / the rule, as on the store art); horizontal (emblem + stacked: a chest
print); vertical (emblem over the title, centred: a back or hoodie front); line (PROJECT ARENA on one line: a sleeve,
or rotated, a sweatpants leg) and line_vertical (the same, turned to run up a leg).
Colourways: color (for dark garments: white, cyan, orange), white (one colour), black (one colour).
"""
import os
import sys

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.font_manager import FontProperties  # noqa: E402
from matplotlib.patches import Circle, PathPatch, Rectangle, Wedge  # noqa: E402
from matplotlib.path import Path  # noqa: E402
from matplotlib.textpath import TextPath  # noqa: E402
from matplotlib.transforms import Affine2D  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'build', 'merch')
FONT_SRC = 'C:/Windows/Fonts/bahnschrift.ttf'
FONT_TMP = os.path.join(ROOT, 'build', 'merch_font_tmp', 'bahnschrift_semibold.ttf')   # (a working copy, not shipped)

CYAN, ORANGE, WHITE, NAVY, BLACK = '#45D3FF', '#FF9843', '#FFFFFF', '#0B1A2A', '#000000'
WAYS = {   # (PROJECT, ARENA, the rule, the emblem's left, right)
    'color': (WHITE, CYAN, ORANGE, CYAN, ORANGE),
    'white': (WHITE,) * 5,
    'black': (BLACK,) * 5,
}
DPI = 300


def semibold():
    if not os.path.exists(FONT_TMP):
        from fontTools.ttLib import TTFont
        from fontTools.varLib import instancer
        os.makedirs(os.path.dirname(FONT_TMP), exist_ok=True)
        instancer.instantiateVariableFont(TTFont(FONT_SRC), {'wght': 600, 'wdth': 100}).save(FONT_TMP)
    return FontProperties(fname=FONT_TMP)


FP = None


def letters(text, cap, width, x, y_top, align='left'):
    """The text as outline paths: capitals `cap` tall with their top at y_top (y up), spread to `width`."""
    probe = TextPath((0, 0), 'H', size=100, prop=FP)
    s = cap / (probe.get_extents().y1 - probe.get_extents().y0)
    paths, adv = [], []
    for c in text:
        p = TextPath((0, 0), c, size=100, prop=FP)
        paths.append(p)
        # the advance: the next glyph's start in a pair (TextPath of "cH" minus "H")
        adv.append((TextPath((0, 0), c + 'H', size=100, prop=FP).get_extents().x1 - probe.get_extents().x1) * s)
    track = (width - sum(adv)) / (len(text) - 1) if width and len(text) > 1 else 0.0
    total = sum(adv) + track * (len(text) - 1)
    if align == 'center':
        x -= total / 2
    out = []
    for p, a in zip(paths, adv):
        out.append(p.transformed(Affine2D().scale(s).translate(x, y_top - cap)))
        x += a + track
    return out, total


def emblem(ax, cx, cy, size, left, right):
    """The emblem, `size` across (its outer ring), centred at cx, cy."""
    k = size / 2 / (0.86 + 0.025)
    gap = 10.0   # degrees each side of straight down: the ring's opening
    for side, col, x0 in (('l', left, -1.2), ('r', right, 0.0)):
        clip = Rectangle((cx + x0 * k, cy - 1.2 * k), 1.2 * k, 2.4 * k, transform=ax.transData)
        parts = [Wedge((cx, cy), (0.86 + 0.025) * k, -90 + gap, 270 - gap, width=0.05 * k),     # the orbit ring
                 Wedge((cx, cy), (0.74 + 0.011) * k, -90 + gap, 270 - gap, width=0.022 * k),    # its thin inner ring
                 Wedge((cx, cy), (0.46 + 0.04) * k, 0, 360, width=0.08 * k)]                   # the ball's rim
        for pt in parts:
            pt.set_facecolor(col); pt.set_edgecolor('none')
            ax.add_patch(pt); pt.set_clip_path(clip)
        # the seam on this side: a circle centred outside the ball, only its part inside the rim
        sx = -0.78 if side == 'l' else 0.78
        seam = Wedge((cx + sx * k, cy), (0.56 + 0.026) * k, 0, 360, width=0.052 * k, facecolor=col, edgecolor='none')
        ax.add_patch(seam)
        seam.set_clip_path(Circle((cx, cy), 0.43 * k, transform=ax.transData))


def title(ax, x, y_top, h, cols, align='left'):
    """PROJECT / ARENA / the rule, as on the store art, `h` tall in all; returns the width."""
    s = h / 248.0   # (the store art's title at 1280 px: 72 + 27 + 86 + 55 + 8)
    proj, ar, rule = cols[0], cols[1], cols[2]
    wP, wA, wR = 390 * s, 385 * s, 330 * s
    xc = x if align == 'left' else x
    p1, _ = letters('PROJECT', 72 * s, wP, xc, y_top, align)
    p2, _ = letters('ARENA', 86 * s, wA, xc, y_top - (72 + 27) * s, align)
    for p in p1:
        ax.add_patch(PathPatch(p, facecolor=proj, edgecolor='none'))
    for p in p2:
        ax.add_patch(PathPatch(p, facecolor=ar, edgecolor='none'))
    ry = y_top - (72 + 27 + 86 + 55 + 8) * s
    rx = x if align == 'left' else x - wR / 2
    ax.add_patch(Rectangle((rx, ry), wR, 8 * s, facecolor=rule, edgecolor='none'))
    return max(wP, wA)


def line(ax, x, y_mid, cap, cols, rotate=False):
    """PROJECT ARENA on one line (PROJECT in the first colour, ARENA in the second), capitals `cap` tall."""
    pP, wP = letters('PROJECT', cap, None, x, y_mid + cap / 2)
    pA, wA = letters('ARENA', cap, None, x + wP + cap * 0.45, y_mid + cap / 2)
    for p in pP:
        ax.add_patch(PathPatch(p.transformed(rotate) if rotate else p, facecolor=cols[0], edgecolor='none'))
    for p in pA:
        ax.add_patch(PathPatch(p.transformed(rotate) if rotate else p, facecolor=cols[1], edgecolor='none'))
    return wP + cap * 0.45 + wA


def canvas(w_in, h_in):
    fig = plt.figure(figsize=(w_in, h_in))
    ax = fig.add_axes((0, 0, 1, 1))
    ax.set_xlim(0, w_in); ax.set_ylim(0, h_in); ax.set_aspect('equal'); ax.axis('off')
    return fig, ax


def save(fig, name):
    for ext in ('svg', 'pdf', 'png'):
        fig.savefig(os.path.join(OUT, '%s.%s' % (name, ext)), dpi=DPI, transparent=True)
    plt.close(fig)


def main():
    global FP
    os.makedirs(OUT, exist_ok=True)
    FP = semibold()
    M = 0.25   # margin (inches)
    for way, cols in WAYS.items():
        # the emblem alone: 10 in
        fig, ax = canvas(10 + 2 * M, 10 + 2 * M)
        emblem(ax, 5 + M, 5 + M, 10, cols[3], cols[4]); save(fig, 'emblem_' + way)
        # the stacked title: 12 in wide
        s = 12.0 / 390.0
        h = 248 * s
        fig, ax = canvas(12 + 2 * M, h + 2 * M)
        title(ax, M, h + M, h, cols); save(fig, 'stacked_' + way)
        # horizontal: emblem + stacked title, 12 in wide (a chest print)
        th = 2.6
        tw = 390 * th / 248
        es = th * 1.25
        W = es + 0.35 * th + tw
        fig, ax = canvas(W + 2 * M, es + 2 * M)
        emblem(ax, M + es / 2, M + es / 2, es, cols[3], cols[4])
        title(ax, M + es + 0.35 * th, M + es / 2 + th / 2, th, cols)
        fig.set_size_inches((W + 2 * M) * 12 / W, (es + 2 * M) * 12 / W)
        ax.set_xlim(0, W + 2 * M); ax.set_ylim(0, es + 2 * M)
        save(fig, 'horizontal_' + way)
        # vertical: the emblem over the title, centred, 12 in wide (a back print, a hoodie front)
        tw = 12.0
        th = 248 * tw / 390
        es = tw * 0.62
        Hh = es + 0.12 * tw + th
        fig, ax = canvas(tw + 2 * M, Hh + 2 * M)
        emblem(ax, M + tw / 2, M + Hh - es / 2, es, cols[3], cols[4])
        title(ax, M + tw / 2, M + th, th, cols, align='center')
        save(fig, 'vertical_' + way)
        # one line: PROJECT ARENA, 14 in long (a sleeve, or a sweatpants leg turned)
        cap = 1.0
        fig, ax = canvas(1, 1)
        wl = line(ax, 0, 0, cap, cols)
        plt.close(fig)
        k = 14.0 / wl
        cap *= k
        fig, ax = canvas(14 + 2 * M, cap + 2 * M)
        line(ax, M, M + cap / 2, cap, cols); save(fig, 'line_' + way)
        # ... the same running up a leg (reads bottom to top)
        fig, ax = canvas(cap + 2 * M, 14 + 2 * M)
        rot = Affine2D().rotate_deg(90)   # ((x, y) -> (-y, x): drawn below the x axis, it lands inside the page)
        line(ax, M, -M - cap / 2, cap, cols, rotate=rot); save(fig, 'line_vertical_' + way)
    preview()
    try:
        os.remove(FONT_TMP); os.rmdir(os.path.dirname(FONT_TMP))
    except OSError:
        pass
    print('wrote', OUT)


def preview():
    """Every design on a dark and a light garment colour, for a look before sending (not for print)."""
    from PIL import Image
    names = ['emblem', 'stacked', 'horizontal', 'vertical', 'line', 'line_vertical']
    tiles = []
    for way, bg in (('color', (11, 26, 42)), ('white', (60, 60, 64)), ('black', (235, 235, 230))):
        row = []
        for n in names:
            im = Image.open(os.path.join(OUT, '%s_%s.png' % (n, way))).convert('RGBA')
            im.thumbnail((440, 440), Image.LANCZOS)
            t = Image.new('RGBA', (480, 480), bg + (255,))
            t.alpha_composite(im, ((480 - im.width) // 2, (480 - im.height) // 2))
            row.append(t)
        tiles.append(row)
    sheet = Image.new('RGBA', (480 * len(names), 480 * len(tiles)), (0, 0, 0, 255))
    for r, row in enumerate(tiles):
        for c, t in enumerate(row):
            sheet.paste(t, (c * 480, r * 480))
    sheet.convert('RGB').save(os.path.join(OUT, 'preview.png'))


if __name__ == '__main__':
    sys.exit(main())
