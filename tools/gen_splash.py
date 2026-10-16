"""Quest loading splash (app/assets_open/vr_splash.png): our emblem and stroke font on black.
Run after tools/gen_open_textures.py. Usage: python -I tools/gen_splash.py"""
import os
import numpy as np
from PIL import Image, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEX = os.path.join(ROOT, 'build', 'open_src', 'tex')
W, H = 2048, 1024
img = Image.new('RGBA', (W, H), (0, 0, 0, 255))
em = Image.open(os.path.join(TEX, 'emblem.png')).convert('RGBA').resize((560, 560), Image.LANCZOS)
img.alpha_composite(em, ((W - 560) // 2, 120))
font = np.asarray(Image.open(os.path.join(TEX, 'font.png')).convert('RGBA'))
text, cw, ch, sc = 'PROJECT ARENA', 48, 64, 1.6
x0, y0 = (W - int(len(text) * cw * sc)) // 2, 720
for i, c in enumerate(text):
    g = ord(c) - 32
    cell = Image.fromarray(font[:, g * cw:(g + 1) * cw]).resize((int(cw * sc), int(ch * sc)), Image.LANCZOS)
    tint = Image.new('RGBA', cell.size, (120, 220, 255, 255))
    tint.putalpha(cell.split()[3])
    img.alpha_composite(tint, (x0 + int(i * cw * sc), y0))
glow = img.filter(ImageFilter.GaussianBlur(18)).point(lambda v: int(v * 0.6))
Image.alpha_composite(glow, img).convert('RGB').save(os.path.join(ROOT, 'app', 'assets_open', 'vr_splash.png'))
print('wrote vr_splash.png')
