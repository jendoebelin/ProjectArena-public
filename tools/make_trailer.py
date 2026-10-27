"""A short trailer from a recorded bot-vs-bot match: title, captions, an end card and the game's own sounds.

1. Record the frames (the PC build, no headset needed):
     ProjectArena.exe --selftest x.ppm --record D:/trailer/frames 28 --view 1080 1080 0 0 0 0 0 62 --nations USA BRA
   (writes D:/trailer/frames/f00000.ppm ... and events.csv: every game event with its time)
2. python -I tools/make_trailer.py D:/trailer/frames D:/trailer/project_arena_trailer.mp4 [--ffmpeg PATH]
Writes the MP4 (H.264 + AAC, 30 fps, square, for LinkedIn and the like) and a small GIF next to it.
Sounds: app/assets_open/audio (the game's own, CC BY 4.0), placed at each event's time; the crowd swells after points.
Text: the Bahnschrift font that comes with Windows."""
import csv
import os
import subprocess
import sys
import wave

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AUDIO = os.path.join(ROOT, 'app', 'assets_open', 'audio')
FPS, SR = 30, 48000
BLUE, ORANGE, DARK, WHITE = (26, 179, 255), (255, 106, 26), (7, 11, 22), (255, 255, 255)
FONT = r'C:\Windows\Fonts\bahnschrift.ttf'

# the captions (seconds): what the trailer says while the match plays
CAPTIONS = [
    (4.0, 9.0, 'THROW, CURVE, BLOCK AND PUNCH'),
    (10.0, 14.3, 'PLAY FOR YOUR COUNTRY'),
    (14.8, 19.3, 'ONLINE: QUEST AND PC PLAYERS TOGETHER'),
    (19.6, 23.2, 'FANS CHEER LIVE FROM THEIR PHONES'),
]
END_CARD_AT = 23.6          # the end card fades in over the finish
TITLE_UNTIL = 3.4           # the title over the countdown


def font(size):
    return ImageFont.truetype(FONT, size)


def fade(t, a, b, ramp=0.35):
    """0..1: in over `ramp` after a, out over `ramp` before b"""
    if t < a or t > b:
        return 0.0
    return max(0.0, min(1.0, (t - a) / ramp, (b - t) / ramp))


def glow_text(img, xy, text, f, fill, glow, alpha, anchor='mm'):
    if alpha <= 0:
        return
    layer = Image.new('RGBA', img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    d.text(xy, text, font=f, fill=glow + (int(220 * alpha),), anchor=anchor)
    layer = layer.filter(ImageFilter.GaussianBlur(max(4, f.size // 9)))
    d = ImageDraw.Draw(layer)
    d.text(xy, text, font=f, fill=fill + (int(255 * alpha),), anchor=anchor)
    img.alpha_composite(layer)


def shade(img, alpha, top=True, bottom=True):
    """darken the picture behind text (a soft band at the top / bottom)"""
    if alpha <= 0:
        return
    w, h = img.size
    a = np.zeros((h, w), np.float32)
    y = np.arange(h, dtype=np.float32)[:, None] / h
    if bottom:
        a = np.maximum(a, np.clip((y - 0.62) / 0.38, 0, 1) ** 1.2)
    if top:
        a = np.maximum(a, np.clip((0.40 - y) / 0.40, 0, 1) ** 1.2)
    a = (a * 0.85 * alpha * 255).astype(np.uint8)
    band = Image.new('RGBA', img.size, DARK + (0,))
    band.putalpha(Image.fromarray(np.broadcast_to(a, (h, w)).copy()))
    img.alpha_composite(band)


def overlay(img, t, total):
    w, h = img.size
    s = w / 1080.0
    # the title over the countdown
    ta = fade(t, -1, TITLE_UNTIL, 0.6)
    if ta > 0:
        shade(img, ta, top=True, bottom=True)
        glow_text(img, (w // 2, int(h * 0.42)), 'PROJECT ARENA', font(int(118 * s)), WHITE, BLUE, ta)
        glow_text(img, (w // 2, int(h * 0.53)), 'A FREE 1v1 VR ARENA SPORT', font(int(44 * s)), WHITE, ORANGE, ta)
    # captions
    for a, b, text in CAPTIONS:
        ca = fade(t, a, b)
        if ca > 0:
            shade(img, ca * 0.8, top=False, bottom=True)
            f = font(int(46 * s))
            d = ImageDraw.Draw(img)
            tw = d.textlength(text, font=f)
            y = int(h * 0.88)
            bar = Image.new('RGBA', img.size, (0, 0, 0, 0))
            bd = ImageDraw.Draw(bar)
            bd.rectangle((w / 2 - tw / 2 - 22 * s, y - 34 * s, w / 2 - tw / 2 - 14 * s, y + 34 * s), fill=BLUE + (int(255 * ca),))
            bd.rectangle((w / 2 + tw / 2 + 14 * s, y - 34 * s, w / 2 + tw / 2 + 22 * s, y + 34 * s), fill=ORANGE + (int(255 * ca),))
            img.alpha_composite(bar)
            glow_text(img, (w // 2, y), text, f, WHITE, BLUE, ca)
    # the end card
    ea = max(0.0, min(1.0, (t - END_CARD_AT) / 0.8))
    if ea > 0:
        dim = Image.new('RGBA', img.size, DARK + (int(200 * ea),))
        img.alpha_composite(dim)
        glow_text(img, (w // 2, int(h * 0.30)), 'PROJECT ARENA', font(int(112 * s)), WHITE, BLUE, ea)
        line = Image.new('RGBA', img.size, (0, 0, 0, 0))
        ld = ImageDraw.Draw(line)
        ld.rectangle((w * 0.30, h * 0.375, w * 0.495, h * 0.381), fill=BLUE + (int(255 * ea),))
        ld.rectangle((w * 0.505, h * 0.375, w * 0.70, h * 0.381), fill=ORANGE + (int(255 * ea),))
        img.alpha_composite(line)
        rows = [('FREE ON META QUEST  \u00b7  PC VR  \u00b7  OPEN SOURCE', 40, ORANGE),
                ('github.com/jendoebelin/ProjectArena-public', 34, BLUE),
                ('Discord: discord.gg/KuNkSbVHzh', 34, BLUE),
                ('Cheer live: crowd.aftersparc.com', 34, BLUE)]
        y = h * 0.47
        for text, size, glow in rows:
            glow_text(img, (w // 2, int(y)), text, font(int(size * s)), WHITE, glow, ea)
            y += h * 0.085


# ---------------------------------------------------------------- sound
def load(name):
    with wave.open(os.path.join(AUDIO, name + '.wav')) as wf:
        n, ch, rate = wf.getnframes(), wf.getnchannels(), wf.getframerate()
        x = np.frombuffer(wf.readframes(n), '<i2').astype(np.float32) / 32768.0
    if ch == 2:
        x = x.reshape(-1, 2).mean(1)
    if rate != SR:
        x = np.interp(np.arange(int(len(x) * SR / rate)) * rate / SR, np.arange(len(x)), x)
    return x


def mix_audio(events, seconds):
    out = np.zeros(int(seconds * SR) + SR, np.float32)

    def add(name, t, gain):
        x = load(name) * gain
        i = int(t * SR)
        n = min(len(x), len(out) - i)
        if n > 0:
            out[i:i + n] += x[:n]
    crowd = load('oa_Crowd')
    level = np.full(len(out), 0.10, np.float32)   # the crowd's bed, louder for a few seconds after a point
    for t, ev, player, value in events:
        if ev == 'Throw':
            add('oa_ThrowSlow' if value < 6 else 'oa_ThrowMed' if value < 9 else 'oa_ThrowFast' if value < 12 else 'oa_ThrowFull', t, 0.55)
        elif ev == 'HitPlayer':
            add('oa_HitPlayerOpponent', t, 0.8)
            add('oa_Cheer', t + 0.15, 0.55)
            i = int(t * SR)
            level[i:i + 3 * SR] = np.maximum(level[i:i + 3 * SR], np.linspace(0.45, 0.12, len(level[i:i + 3 * SR])))
        elif ev in ('ShieldBlock', 'KnuckleBlock', 'Strike', 'Bounce', 'Caught', 'Countdown', 'RoundStart'):
            add({'ShieldBlock': 'oa_ShieldBlock', 'KnuckleBlock': 'oa_KnuckleBlock', 'Strike': 'oa_Strike', 'Bounce': 'oa_Bounce',
                 'Caught': 'oa_Caught', 'Countdown': 'oa_Countdown', 'RoundStart': 'oa_RoundStart'}[ev], t,
                0.35 if ev == 'Bounce' else 0.6)
        elif ev == 'MatchEnd':
            add('oa_MatchEndWin', t, 0.8)
            add('oa_Cheer', t + 0.2, 0.7)
            i = int(t * SR)
            level[i:] = np.maximum(level[i:], 0.4)
    reps = int(np.ceil(len(out) / len(crowd)))
    out += np.tile(crowd, reps)[:len(out)] * level
    out = out[:int(seconds * SR)]
    f = min(len(out), int(1.2 * SR))   # fade out at the end
    out[-f:] *= np.linspace(1, 0, f)
    out /= max(1e-6, np.max(np.abs(out))) / 0.89
    return out


def main():
    frames_dir, out_mp4 = sys.argv[1], sys.argv[2]
    ffmpeg = sys.argv[sys.argv.index('--ffmpeg') + 1] if '--ffmpeg' in sys.argv else 'ffmpeg'
    names = sorted(f for f in os.listdir(frames_dir) if f.startswith('f') and f.endswith('.ppm'))
    total = len(names) / FPS
    events = []
    with open(os.path.join(frames_dir, 'events.csv')) as fh:
        for r in csv.DictReader(fh):
            events.append((float(r['t']), r['event'], int(r['player']), float(r['value'])))
    work = os.path.join(frames_dir, 'edited')
    os.makedirs(work, exist_ok=True)
    for i, n in enumerate(names):
        img = Image.open(os.path.join(frames_dir, n)).convert('RGBA')
        overlay(img, i / FPS, total)
        img.convert('RGB').save(os.path.join(work, 'e%05d.png' % i), compress_level=1)
    audio = mix_audio(events, total)
    wav = os.path.join(frames_dir, 'trailer.wav')
    with wave.open(wav, 'wb') as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(SR)
        wf.writeframes((audio * 32767).astype('<i2').tobytes())
    subprocess.run([ffmpeg, '-y', '-loglevel', 'error', '-framerate', str(FPS), '-i', os.path.join(work, 'e%05d.png'), '-i', wav,
                    '-c:v', 'libx264', '-preset', 'slow', '-crf', '19', '-pix_fmt', 'yuv420p', '-movflags', '+faststart',
                    '-c:a', 'aac', '-b:a', '160k', '-shortest', out_mp4], check=True)
    gif = os.path.splitext(out_mp4)[0] + '.gif'
    subprocess.run([ffmpeg, '-y', '-loglevel', 'error', '-i', out_mp4, '-vf',
                    'fps=10,scale=360:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=96:stats_mode=diff[p];'
                    '[b][p]paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle',
                    gif], check=True)
    print('%s (%.1f s), %s' % (out_mp4, total, gif))


if __name__ == '__main__':
    main()
