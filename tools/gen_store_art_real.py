"""Store art from real footage (2026-11-07, owner: all promo and store art is real footage of real people playing;
no bot scenes, no staged renders, no generated pictures).

    python -I tools/gen_store_art_real.py        ->  build/store_art/

Covers: the alpha YouTube thumbnail's shot (build/promo/Project_Arena_Thumbnail_safe.png, a real spectator capture)
rebuilt at Meta's sizes without "ALPHA": the picture is upscaled twice, the baked title, badge and HUD labels are
painted out along the panel's streaks, and the title is drawn again in Bahnschrift at full size with the crisp icon.
Screenshots: real headset captures (build/promo/headset/frames, build/promo/headset/selfie/picks) at 2560x1440.
Also: the thumbnail without ALPHA at 1280x720 (YouTube, SideQuest; text well inside SideQuest's 3:2 trim).

The store set carries no tagline (owner, 2026-11-08: the title and the rule stand alone). `--promo` makes the same
covers and thumbnail with THROW. BLOCK. DODGE. under the rule, for side promo (posts, ads), in build/promo/verbs/
(no screenshots: they don't change).
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'build', 'store_art')
THUMB = os.path.join(ROOT, 'build', 'promo', 'Project_Arena_Thumbnail_safe.png')
ICON = os.path.join(ROOT, 'build', 'store_art', 'icon_1024.png')   # from gen_store_art.py
FRAMES = os.path.join(ROOT, 'build', 'promo', 'headset', 'frames')
SELFIE = os.path.join(ROOT, 'build', 'promo', 'headset', 'selfie', 'picks')
FONT = 'C:/Windows/Fonts/bahnschrift.ttf'
THUMB_OUT = os.path.join(ROOT, 'build', 'promo', 'thumbnail_launch.png')
TAGLINE = None                      # the store set: none; --promo: the verbs
PROMO_TAGLINE = 'THROW. BLOCK. DODGE.'

# the real screenshots, in the listing's order (file, how it was made)
SCREENSHOTS = [
    # 0.3.27's set (the owner, 2026-11-28: "use those five"): the court's frosted strike zones and hex back walls, the
    # new outfits and facial hair. From the owner's sessions of 2026-11-27 23:20 (Quest) and 2026-11-28 00:48 (the PC
    # version through the headset's streaming), headset VideoShots. The bot stands at the far end, small and upright
    # (owner, 2026-11-07: ball shots are fine; mind how the bot stands). The same five, 1280x720, are the public
    # README's docs/images/play_1-5.jpg. The set before (2026-11-18) is in build/store_art/new_2026-11-18.
    ('20261128-004816_218.00.png', 'a SQUARE rally: your ball and the bot\'s cross the court'),
    ('20261128-004816_238.00.png', 'your ball spinning to the far end, the score over it'),
    ('20261127-232054_055.00.png', 'a charged ball in your gauntlet'),
    ('20261128-004816_037.00.png', 'the avatar builder: FACE 6, SHORT BEARD'),
    ('20261128-004816_141.00.png', 'the avatar builder: CHERRY POODLE SKIRT'),
]

# the thumbnail's parts at 1280x720 (x0, y0, x1, y1): painted out, then drawn again at full size
BOX_ICON = (140, 48, 270, 178)
BOX_TITLE = (140, 180, 560, 520)      # PROJECT, ARENA, the rule, the tagline
BOX_ALPHA = (140, 588, 300, 672)
BOX_HUD = [(880, 10, 960, 66), (1170, 10, 1245, 66), (1180, 696, 1280, 720)]   # BOT 3, BOT 3, ORANGE'S EYES: filled across
BOX_HUD_TEXT = [(680, 72, 1032, 100)]   # the rules line, over the ceiling and the head: only its own gray-white
# strokes are masked (the head's cyan lines stay) and filled from their neighbours


def font_for_cap(cap, name='SemiBold'):
    """Bahnschrift at the size whose capital letters are `cap` pixels tall."""
    f = ImageFont.truetype(FONT, 100)
    f.set_variation_by_name(name)
    b = f.getbbox('H')
    f = ImageFont.truetype(FONT, int(round(100 * cap / (b[3] - b[1]))))
    f.set_variation_by_name(name)
    return f


def draw_tracked(d, x, cap_top, text, font, fill, width=None, tracking=0.0):
    """Draw text with its capitals' top at cap_top; spread the letters to `width` if given. Returns the right edge."""
    b = font.getbbox('H')
    y = cap_top - b[1]
    adv = [font.getlength(c) for c in text]
    if width is not None and len(text) > 1:
        tracking = (width - sum(adv)) / (len(text) - 1)
    for c, a in zip(text, adv):
        d.text((x, y), c, font=font, fill=fill)
        x += a + tracking
    return x - tracking


def bright_bbox(a, box, thresh):
    """The bounding box of the pixels brighter than `thresh` inside `box` (x0, y0, x1, y1), or None."""
    x0, y0, x1, y1 = box
    lum = a[y0:y1, x0:x1].max(axis=2)
    ys, xs = np.nonzero(lum > thresh)
    if not len(xs): return None
    return (x0 + xs.min(), y0 + ys.min(), x0 + xs.max() + 1, y0 + ys.max() + 1)


def colour_in(a, box, thresh):
    """The colour of the bright pixels inside `box`: the median of those above `thresh`."""
    x0, y0, x1, y1 = box
    r = a[y0:y1, x0:x1].reshape(-1, 3)
    r = r[r.max(axis=1) > thresh]
    return tuple(int(v) for v in np.median(r, axis=0))


def streak_direction(a, mask):
    """The direction of the panel's streaks (unit vector) from its gradients: across the strongest gradient."""
    lum = a.astype(np.float32).mean(axis=2)
    gy, gx = np.gradient(lum)
    w = ~mask
    w[:, 640:] = False
    jxx, jyy, jxy = (gx * gx)[w].sum(), (gy * gy)[w].sum(), (gx * gy)[w].sum()
    phi = 0.5 * np.arctan2(2 * jxy, jxx - jyy)     # the gradient's orientation
    return np.array([-np.sin(phi), np.cos(phi)])   # along the streak: across the gradient


def inpaint(a, mask, direction, max_steps=700):
    """Fill the masked pixels by interpolating, along `direction`, between the nearest unmasked pixels each way."""
    h, w = mask.shape
    out = a.astype(np.float32).copy()
    ys, xs = np.nonzero(mask)
    dx, dy = direction
    cols = []
    dists = []
    for sgn in (1, -1):
        col = np.zeros((len(xs), 3), np.float32)
        dist = np.full(len(xs), np.inf, np.float32)
        found = np.zeros(len(xs), bool)
        for s in range(1, max_steps):
            px = np.clip(np.round(xs + sgn * dx * s).astype(int), 0, w - 1)
            py = np.clip(np.round(ys + sgn * dy * s).astype(int), 0, h - 1)
            hit = ~found & ~mask[py, px]
            col[hit] = a[py[hit], px[hit]]
            dist[hit] = s
            found |= hit
            if found.all(): break
        cols.append(col)
        dists.append(dist)
    d0, d1 = dists
    both = np.isfinite(d0) & np.isfinite(d1)
    wgt = np.where(both, d1 / np.maximum(d0 + d1, 1e-6), np.where(np.isfinite(d0), 1.0, 0.0))[:, None]
    out[ys, xs] = cols[0] * wgt + cols[1] * (1 - wgt)
    return out


def inpaint_diffuse(a, mask, iters=300):
    """Fill thin masked strokes by averaging their four neighbours again and again."""
    out = a.astype(np.float32).copy()
    out[mask] = 0
    m = mask
    for _ in range(iters):
        n = (np.roll(out, 1, 0) + np.roll(out, -1, 0) + np.roll(out, 1, 1) + np.roll(out, -1, 1)) * 0.25
        out[m] = n[m]
    return out


def text_mask(a, box, dilate=1):
    """The gray-white strokes inside `box`: bright and unsaturated pixels, grown by `dilate`."""
    h, w = a.shape[:2]
    m = np.zeros((h, w), bool)
    x0, y0, x1, y1 = box
    r = a[y0:y1, x0:x1].astype(int)
    lum = r.max(axis=2)
    sat = r.max(axis=2) - r.min(axis=2)
    m[y0:y1, x0:x1] = (lum > 95) & (sat < 70)
    for _ in range(dilate):
        m = m | np.roll(m, 1, 0) | np.roll(m, -1, 0) | np.roll(m, 1, 1) | np.roll(m, -1, 1)
    return m


def clean_plate(a):
    """The thumbnail without its title, badge, icon and HUD labels (1280x720, float RGB)."""
    h, w = a.shape[:2]
    panel = np.zeros((h, w), bool)
    for x0, y0, x1, y1 in (BOX_ICON, BOX_TITLE, BOX_ALPHA): panel[y0:y1, x0:x1] = True
    hud = np.zeros((h, w), bool)
    for x0, y0, x1, y1 in BOX_HUD: hud[y0:y1, x0:x1] = True
    strokes = np.zeros((h, w), bool)
    for box in BOX_HUD_TEXT: strokes |= text_mask(a, box)
    d = streak_direction(a, panel | hud | strokes)
    print('panel streaks run along (%.2f, %.2f)' % tuple(d))
    out = inpaint(a, panel, d)
    out = inpaint(out, hud, np.array([1.0, 0.0]))   # the HUD labels over the ceiling and the court: fill across
    out = inpaint_diffuse(out, strokes)             # the rules line: its strokes alone, filled from around them
    print('rules line: %d stroke pixels filled' % strokes.sum())
    # a little grain on the filled panel so it doesn't look painted
    rng = np.random.default_rng(7)
    out[panel] += rng.normal(0, 1.2, (panel.sum(), 3))
    return np.clip(out, 0, 255)


def upscale(img, w, h):
    """Lanczos to w x h with a light unsharp mask (the captures are 1920x1080 or less)."""
    return img.resize((w, h), Image.LANCZOS).filter(ImageFilter.UnsharpMask(radius=1.6, percent=55, threshold=2))


def title_block(d, x, cap_top, scale, col_arena, col_rule, icon=None, img=None, gap=1.0):
    """The title as the thumbnail has it (icon, PROJECT, ARENA, the rule, the tagline if any) at `scale` x the 1280
    version. Returns the bottom."""
    s = scale
    y = cap_top
    if icon is not None:
        sz = int(round(114 * s))
        img.paste(icon.resize((sz, sz), Image.LANCZOS), (int(x), int(y)))
        y += sz + 28 * s * gap
    draw_tracked(d, x, y, 'PROJECT', font_for_cap(72 * s, 'SemiBold'), (255, 255, 255), width=390 * s)
    y += 72 * s + 27 * s * gap
    draw_tracked(d, x, y, 'ARENA', font_for_cap(86 * s, 'SemiBold'), col_arena, width=385 * s)
    y += 86 * s + 55 * s * gap
    d.rectangle((x, y, x + 330 * s, y + 8 * s), fill=col_rule)
    y += 8 * s
    if not TAGLINE:
        return y
    y += 29 * s * gap
    draw_tracked(d, x, y, TAGLINE, font_for_cap(23 * s, 'Regular'), (255, 255, 255), width=308 * s)
    return y + 23 * s


def wide_plate(plate1, W, H, px0):
    """A wide picture with no text: the clean plate at H high from x px0, the panel's edge colour carried on to the
    left (a little darker towards the far left), the seam blended over 120 px, and the court's edge mirrored and faded
    to black on the right."""
    pw = int(round(H * 16 / 9))
    out = Image.new('RGB', (W, H))
    pic = upscale(Image.fromarray(plate1.astype(np.uint8)), pw, H)
    out.paste(pic, (px0, 0))
    pa = np.asarray(pic).astype(np.float32)
    left = pa[:, :24].mean(axis=1)                       # the panel's edge colour, row by row
    fill = np.repeat(left[:, None, :], px0, axis=1)
    fade = np.linspace(0.78, 1.0, px0)[None, :, None]
    rng = np.random.default_rng(3)
    fill = fill * fade + rng.normal(0, 1.2, fill.shape)
    a = np.asarray(out).astype(np.float32)
    a[:, :px0] = fill
    for i in range(120):
        t = i / 120.0
        a[:, px0 + i] = a[:, px0 + i] * t + fill[:, -1] * (1 - t)
    rx = px0 + pw
    mirror = pa[:, ::-1][:, :W - rx]
    fo = np.linspace(1.0, 0.0, W - rx)[None, :, None]
    a[:, rx:] = mirror * fo
    return Image.fromarray(a.clip(0, 255).astype(np.uint8))


def shade(img, y0, y1, top_alpha, bottom_alpha):
    """Darken rows y0..y1 with alpha running from top_alpha to bottom_alpha."""
    a = np.asarray(img).astype(np.float32)
    al = np.linspace(top_alpha, bottom_alpha, y1 - y0)[:, None, None]
    a[y0:y1] *= (1 - al)
    return Image.fromarray(a.clip(0, 255).astype(np.uint8))


def main():
    global OUT, THUMB_OUT, TAGLINE
    promo = '--promo' in sys.argv
    if promo:
        TAGLINE = PROMO_TAGLINE
        OUT = os.path.join(ROOT, 'build', 'promo', 'verbs')
        THUMB_OUT = os.path.join(OUT, 'thumbnail_verbs.png')
    os.makedirs(OUT, exist_ok=True)
    if not os.path.exists(ICON):
        sys.exit('make the icon first: python -I tools/gen_store_art.py')
    thumb = np.asarray(Image.open(THUMB).convert('RGB'))
    col_arena = colour_in(thumb, (150, 290, 550, 390), 150)
    col_rule = colour_in(thumb, (150, 430, 490, 452), 120)
    print('ARENA colour', col_arena, 'rule colour', col_rule)
    plate1 = clean_plate(thumb)
    Image.fromarray(plate1.astype(np.uint8)).save(os.path.join(OUT, 'cover_plate_1280.png'))
    plate = upscale(Image.fromarray(plate1.astype(np.uint8)), 2560, 1440)
    icon = Image.open(ICON).convert('RGB')

    # cover_landscape 2560x1440: the thumbnail's own layout, twice the size, no ALPHA
    img = plate.copy()
    d = ImageDraw.Draw(img)
    title_block(d, 296, 110, 2.0, col_arena, col_rule, icon, img)
    img.save(os.path.join(OUT, 'cover_landscape.png'), optimize=True)
    # the thumbnail for YouTube and SideQuest, 1280x720, from the same picture (text starts 148 px in: inside
    # SideQuest's ~98 px trim)
    img.resize((1280, 720), Image.LANCZOS).save(THUMB_OUT, optimize=True)
    print('cover_landscape  2560x1440  (+ %s 1280x720)' % os.path.relpath(THUMB_OUT, ROOT).replace(os.sep, '/'))

    # hero_cover 3000x900: the picture at the right, inside Meta's safe area (x 17.5..82.5 %), the panel carried on
    # to the left for the title; the far right fades out
    hero = wide_plate(plate1, 3000, 900, 1060)   # (1060: the avatar's head ends inside x 82.5 %)
    d = ImageDraw.Draw(hero)
    title_block(d, 600, 150, 1.55, col_arena, col_rule, icon, hero, gap=0.9)
    hero.save(os.path.join(OUT, 'hero_cover.png'), optimize=True)
    print('hero_cover       3000x900')
    # mini_landscape 1080x360: the hero's middle
    hero.crop((150, 0, 2850, 900)).resize((1080, 360), Image.LANCZOS).save(os.path.join(OUT, 'mini_landscape.png'), optimize=True)
    print('mini_landscape   1080x360')
    # sidequest_banner 3840x1240: SideQuest's wide banner (the listing's "app banner", also what its promotions show).
    # No text: SideQuest lays the title logo over it, as Steam's library hero. Replaces the staged bot render
    # (library_hero.png) the listing had (2026-11-08).
    if not promo:
        wide_plate(plate1, 3840, 1240, 1336).save(os.path.join(OUT, 'sidequest_banner.png'), optimize=True)
        print('sidequest_banner 3840x1240')
        # sidequest_icon 1440x1440: SideQuest's square App icon. Its search results crop it to a centred 16:9 tile
        # (rows 315..1125), which cut the square cover's title off, so the whole landscape cover sits in that band and
        # the text-free picture, mirrored, blurred and darkened, fills above and below (uploaded 2026-11-08).
        S, Hb = 1440, 810
        top = (S - Hb) // 2
        land = Image.open(os.path.join(OUT, 'cover_landscape.png')).convert('RGB').resize((S, Hb), Image.LANCZOS)
        pl = np.asarray(Image.fromarray(plate1.astype(np.uint8)).resize((S, Hb), Image.LANCZOS)).astype(np.float32)

        def band(rows, at_top):
            src = pl[:rows][::-1] if at_top else pl[-rows:][::-1]
            a = np.asarray(Image.fromarray(src.clip(0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(14)))
            f = np.linspace(0.08, 0.6, rows) if at_top else np.linspace(0.6, 0.08, rows)
            return a.astype(np.float32) * f[:, None, None]
        o = np.zeros((S, S, 3), np.float32)
        o[:top] = band(top, True)
        o[top:top + Hb] = np.asarray(land).astype(np.float32)
        o[top + Hb:] = band(S - top - Hb, False)
        Image.fromarray(o.clip(0, 255).astype(np.uint8)).save(os.path.join(OUT, 'sidequest_icon.png'), optimize=True)
        print('sidequest_icon   1440x1440')

    # cover_square 1440x1440 and cover_portrait 1008x1440: the avatar, the title in a dark band at the foot
    for name, x0, w in (('cover_square', 1120, 1440), ('cover_portrait', 1500, 1008)):
        img = plate.crop((x0, 0, x0 + w, 1440))
        img = shade(img, 880, 1440, 0.0, 0.88)
        d = ImageDraw.Draw(img)
        s = w / 1440.0 * 1.35
        margin = int(70 * s) if name == 'cover_square' else int(60 * s)
        y = 1440 - 60 * s
        # from the bottom up: the tagline if any, the rule, ARENA, PROJECT
        if TAGLINE:
            y -= 23 * s
            draw_tracked(d, margin, y, TAGLINE, font_for_cap(23 * s, 'Regular'), (255, 255, 255), width=308 * s)
            y -= 26 * s
        y -= 8 * s
        d.rectangle((margin, y, margin + 330 * s, y + 8 * s), fill=col_rule)
        y -= 86 * s + 40 * s
        draw_tracked(d, margin, y, 'ARENA', font_for_cap(86 * s, 'SemiBold'), col_arena, width=385 * s)
        y -= 72 * s + 24 * s
        draw_tracked(d, margin, y, 'PROJECT', font_for_cap(72 * s, 'SemiBold'), (255, 255, 255), width=390 * s)
        img.save(os.path.join(OUT, name + '.png'), optimize=True)
        print('%-16s %dx%d' % (name, w, 1440))

    # the screenshots (the store set only: no title on them)
    for i, (f, what, *crop) in enumerate(SCREENSHOTS if not promo else [], 1):
        p = os.path.join(SELFIE if f.startswith('selfie') else FRAMES, f)
        im = Image.open(p).convert('RGB')
        if crop: im = im.crop(crop[0])
        if im.size != (2560, 1440): im = upscale(im, 2560, 1440)
        im.save(os.path.join(OUT, 'screenshot_%d.png' % i), optimize=True)
        print('screenshot_%d     2560x1440  %s (%s)' % (i, what, f))

    # previews for a look
    cov = Image.new('RGB', (2560, 2420), (20, 20, 20))
    cov.paste(Image.open(os.path.join(OUT, 'cover_landscape.png')).resize((1280, 720)), (0, 0))
    cov.paste(Image.open(os.path.join(OUT, 'cover_square.png')).resize((720, 720)), (1300, 0))
    cov.paste(Image.open(os.path.join(OUT, 'cover_portrait.png')).resize((504, 720)), (2040, 0))
    cov.paste(Image.open(os.path.join(OUT, 'hero_cover.png')).resize((2560, 768)), (0, 740))
    cov.paste(Image.open(os.path.join(OUT, 'mini_landscape.png')).resize((1080, 360)), (0, 1530))
    cov.paste(Image.open(THUMB_OUT), (1120, 1530))
    cov.paste(Image.fromarray(plate1.astype(np.uint8)).resize((640, 360)), (0, 1920))
    cov.save(os.path.join(OUT, 'covers_preview.jpg'), quality=88)
    if promo:
        print('wrote', OUT)
        return
    sh = Image.new('RGB', (2560, 1440 + 20), (20, 20, 20))
    for i in range(5):
        im = Image.open(os.path.join(OUT, 'screenshot_%d.png' % (i + 1))).resize((1280 if i < 2 else 853, 720 if i < 2 else 480))
        sh.paste(im, ((i % 2) * 1280, 0) if i < 2 else ((i - 2) * 853, 740))
    sh.save(os.path.join(OUT, 'screenshots_preview.jpg'), quality=88)
    print('wrote', OUT)


if __name__ == '__main__':
    main()
