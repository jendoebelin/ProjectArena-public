"""The Crowd's app icons (server/crowd_web/icon-*.png): a blue and an orange arc (the two ends) round a glowing ball.
Usage: python -I tools/gen_crowd_icons.py"""
import os

from PIL import Image, ImageChops, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'server', 'crowd_web')
BG, BLUE, ORANGE = (7, 11, 22), (26, 179, 255), (255, 106, 26)


def icon(size, pad=0.0):
    S = 4 * size
    im = Image.new('RGB', (S, S), BG)
    glow = Image.new('RGB', (S, S), (0, 0, 0))
    m = S * (0.17 + pad)
    box = (m, m, S - m, S - m)
    w = int(S * (0.085 - 0.3 * pad))
    c, r = S / 2, S * (0.11 - 0.4 * pad)
    for dr in (ImageDraw.Draw(im), ImageDraw.Draw(glow)):
        dr.arc(box, 200, 340, fill=BLUE, width=w)
        dr.arc(box, 20, 160, fill=ORANGE, width=w)
    ImageDraw.Draw(glow).ellipse((c - r * 1.5, c - r * 1.5, c + r * 1.5, c + r * 1.5), fill=(120, 200, 255))
    out = ImageChops.screen(im, glow.filter(ImageFilter.GaussianBlur(S * 0.035)))
    ImageDraw.Draw(out).ellipse((c - r, c - r, c + r, c + r), fill=(235, 248, 255))
    return out.resize((size, size), Image.LANCZOS)


def main():
    for size, name, pad in ((192, 'icon-192.png', 0), (180, 'icon-180.png', 0), (512, 'icon-512.png', 0),
                            (512, 'icon-512-maskable.png', 0.08)):
        icon(size, pad).save(os.path.join(OUT, name), optimize=True)
    print('icons -> %s' % OUT)


if __name__ == '__main__':
    main()
