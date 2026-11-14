"""An alternative trailer cut from the headset recordings: 30 s, 1920x1080, 30 fps, H.264 + AAC.

NOT THE STORE TRAILER. The owner (2026-11-08) kept the alpha trailer's cut (fast edits to the music, several angles
per feature; tools/retitle_trailer.py takes ALPHA out of it). This slower cut stays as a starting point for later
promo pieces.

    python -I tools/make_trailer_real.py --music "D:/electronic fireball blitz.mp3" [--music-start 28.2]
        ->  build/promo/trailer/project_arena_trailer_newcut.mp4 (+ a contact sheet, newcut_sheet.jpg)

Footage: the headset recordings in build/promo/headset (real people playing: the Nov 4 online 1-v-1, the Nov 7
FIREBALL BLITZ and four-player room, a Nov 6 shield block) and the spectator's SELFIE still under the end card.
No bot scenes, no staged renders. Captions in Bahnschrift as the covers have them; the end card is the title, the
orange rule, FREE ON META QUEST and the Discord invite. Nothing says ALPHA.
Sound: the music file given (the trailer's track from the alpha cut, starting where it did) with the recordings' own
game sound low underneath; it fades out over the end card.
"""
import os
import subprocess
import sys
import wave

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_store_art_real import ICON, draw_tracked, font_for_cap  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADSET = os.path.join(ROOT, 'build', 'promo', 'headset')
SELFIE_STILL = os.path.join(HEADSET, 'selfie', 'picks', 'selfie_00558.png')
OUT = os.path.join(ROOT, 'build', 'promo', 'trailer')
FFMPEG = ('%USERPROFILE%/AppData/Local/Programs/Python/Python310/lib/site-packages/imageio_ffmpeg/binaries/'
          'ffmpeg-win64-v4.2.2.exe')
W, H, FPS, SR = 1920, 1080, 30, 48000
WHITE, CYAN, ORANGE = (255, 255, 255), (69, 211, 255), (255, 152, 67)

NOV4 = 'com.projectarena.game-20261104-225212-0.mp4'      # online 1-v-1 against a real player (no names shown)
NOV6 = 'com.projectarena.game-20261106-140328-0.mp4'      # training: the shield block (the coach stands far off)
BLITZ = 'com.projectarena.game-20261107-002041-0.mp4'     # FIREBALL BLITZ, four people, flags flying
ROOM = 'com.projectarena.game-20261107-003731-0.mp4'      # a four-player room seen through the glass

# the cut: (file, start in the recording, seconds, zoom, zoom anchor 0 = keep the top .. 1 = keep the bottom)
# zoom crops in on the picture (1.3 drops the training line at the top of the Nov 6 clip)
CUT = [
    (NOV4, 10.0, 3.5, 1.0, 0.5),      # the orange ball comes at you; it charges in your gauntlet (after the headset's
                                      # own menu and its button hint, which the recording's first seconds carry)
    (NOV4, 15.5, 4.5, 1.0, 0.5),      # your own throw: the blue streak down the court
    (NOV6, 17.9, 3.0, 1.25, 0.75),    # the shield comes up, the ball breaks on it (the crop drops the coach's line)
    (NOV4, 52.0, 4.0, 1.0, 0.5),      # the rally at 1 - 1
    (BLITZ, 12.0, 5.0, 1.0, 0.5),     # FIREBALL BLITZ: flags, fireballs, four players
    (BLITZ, 21.0, 3.5, 1.0, 0.5),     # a fireball straight at you
    (ROOM, 5.0, 2.5, 1.0, 0.5),       # two players trade lightning behind the glass
    (SELFIE_STILL, 0.0, 4.0, 1.0, 0.5),   # the end card over the spectator's selfie
]
# captions (from, to, text)
CAPTIONS = [
    (0.6, 3.3, 'THROW. BLOCK. DODGE.'),
    (4.0, 7.6, 'HEAD TO HEAD'),
    (11.6, 14.8, 'PLAY ANYONE ONLINE'),
    (15.6, 19.6, 'FOUR PLAYERS. FIREBALL BLITZ.'),
    (20.4, 25.6, 'PLAY TOGETHER'),
]
END_AT = 26.0
TOTAL = sum(c[2] for c in CUT)


def fade(t, a, b, ramp=0.3):
    if t < a or t > b:
        return 0.0
    return max(0.0, min(1.0, (t - a) / ramp, (b - t) / ramp))


def band(img, alpha):
    """A dark gradient along the foot of the picture, for the captions."""
    if alpha <= 0:
        return
    a = np.zeros((H, W), np.float32)
    y = np.arange(H, dtype=np.float32)[:, None] / H
    a[:] = np.clip((y - 0.55) / 0.45, 0, 1) ** 1.3 * 0.8 * alpha
    layer = Image.new('RGBA', (W, H), (4, 8, 16, 0))
    layer.putalpha(Image.fromarray((a * 255).astype(np.uint8)))
    img.alpha_composite(layer)


def tracked_width(font, text, tracking):
    return sum(font.getlength(c) for c in text) + tracking * (len(text) - 1)


def centred(d, y, text, cap, name, fill, track):
    """Tracked text (tracking `track` x the cap height, in pixels) centred on the frame."""
    f = font_for_cap(cap, name)
    tr = track * cap
    draw_tracked(d, W / 2 - tracked_width(f, text, tr) / 2, y, text, f, fill, tracking=tr)


def caption(img, text, alpha):
    if alpha <= 0:
        return
    band(img, alpha)
    layer = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    x, cap, y = 150, 50, 880
    d.rectangle((x, y - 36, x + 180, y - 28), fill=ORANGE + (int(255 * alpha),))
    f = font_for_cap(cap, 'SemiBold')
    draw_tracked(d, x, y, text, f, WHITE + (int(255 * alpha),), tracking=0.06 * cap)
    img.alpha_composite(layer)


def end_card(img, alpha, icon):
    if alpha <= 0:
        return
    dim = Image.new('RGBA', (W, H), (6, 10, 20, int(215 * alpha)))
    img.alpha_composite(dim)
    layer = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    a = int(255 * alpha)
    sz = 150
    ic = icon.resize((sz, sz), Image.LANCZOS).convert('RGBA')
    ic.putalpha(Image.eval(ic.getchannel('A'), lambda v: v * a // 255))
    layer.alpha_composite(ic, (W // 2 - sz // 2, 250))
    y = 440
    for text, cap, col in (('PROJECT', 96, WHITE), ('ARENA', 114, CYAN)):
        centred(d, y, text, cap, 'SemiBold', col + (a,), 0.06)
        y += cap + (34 if text == 'PROJECT' else 60)
    d.rectangle((W / 2 - 220, y, W / 2 + 220, y + 8), fill=ORANGE + (a,))
    y += 50
    for text, cap, col in (('FREE ON META QUEST', 30, WHITE), ('discord.gg/KuNkSbVHzh', 26, CYAN)):
        centred(d, y, text, cap, 'Regular', col + (a,), 0.08)
        y += cap + 36
    img.alpha_composite(layer)


def frames_of(path, start, seconds, zoom, anchor):
    """Decode a segment to 1920x1080 RGB frames at 30 fps (a still becomes a slow push-in)."""
    n = int(round(seconds * FPS))
    if path.lower().endswith('.png'):
        still = Image.open(path).convert('RGB')
        for i in range(n):
            z = 1.0 + 0.06 * i / max(1, n - 1)
            cw, ch = still.width / z, still.height / z
            x0, y0 = (still.width - cw) / 2, (still.height - ch) / 2
            yield np.asarray(still.crop((int(x0), int(y0), int(x0 + cw), int(y0 + ch))).resize((W, H), Image.LANCZOS))
        return
    cmd = [FFMPEG, '-loglevel', 'error', '-ss', '%.3f' % start, '-t', '%.3f' % (seconds + 0.5), '-i', path,
           '-vf', 'fps=%d,scale=%d:%d' % (FPS, W, H), '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-']
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)   # (it's cut off at n frames)
    size = W * H * 3
    got = 0
    last = None
    while got < n:
        buf = p.stdout.read(size)
        if len(buf) < size:
            break
        fr = np.frombuffer(buf, np.uint8).reshape(H, W, 3)
        if zoom > 1.0:
            cw, ch = int(W / zoom), int(H / zoom)
            x0 = (W - cw) // 2
            y0 = int((H - ch) * anchor)
            fr = np.asarray(Image.fromarray(fr[y0:y0 + ch, x0:x0 + cw]).resize((W, H), Image.LANCZOS))
        last = fr
        got += 1
        yield fr
    p.kill()
    p.stdout.close()
    p.wait()
    while got < n and last is not None:   # a recording that ends early holds its last frame
        got += 1
        yield last


def audio_of(path, start, seconds):
    """A segment's own sound, mono 48 kHz, exactly `seconds` long (silence for a still)."""
    n = int(round(seconds * SR))
    if path.lower().endswith('.png'):
        return np.zeros(n, np.float32)
    cmd = [FFMPEG, '-loglevel', 'error', '-ss', '%.3f' % start, '-t', '%.3f' % (seconds + 0.2), '-i', path,
           '-vn', '-ac', '1', '-ar', str(SR), '-f', 's16le', '-']
    raw = subprocess.run(cmd, stdout=subprocess.PIPE, check=True).stdout
    x = np.frombuffer(raw, '<i2').astype(np.float32) / 32768.0
    out = np.zeros(n, np.float32)
    out[:min(n, len(x))] = x[:n]
    return out


def music_of(path, start, seconds):
    cmd = [FFMPEG, '-loglevel', 'error', '-ss', '%.3f' % start, '-t', '%.3f' % (seconds + 0.2), '-i', path,
           '-vn', '-ac', '1', '-ar', str(SR), '-f', 's16le', '-']
    raw = subprocess.run(cmd, stdout=subprocess.PIPE, check=True).stdout
    x = np.frombuffer(raw, '<i2').astype(np.float32) / 32768.0
    n = int(round(seconds * SR))
    out = np.zeros(n, np.float32)
    out[:min(n, len(x))] = x[:n]
    return out


def main():
    if '--music' not in sys.argv:
        sys.exit('give the music file: --music PATH [--music-start SECONDS]')
    music = sys.argv[sys.argv.index('--music') + 1]
    mstart = float(sys.argv[sys.argv.index('--music-start') + 1]) if '--music-start' in sys.argv else 28.2
    os.makedirs(OUT, exist_ok=True)
    icon = Image.open(ICON).convert('RGB')
    out_mp4 = os.path.join(OUT, 'project_arena_trailer_newcut.mp4')

    # sound: the music, the recordings low underneath, a fade in and a fade out over the end card
    game = np.concatenate([audio_of(os.path.join(HEADSET, f) if not f.endswith('.png') else f, s, d) for f, s, d, _, _ in CUT])
    mus = music_of(music, mstart, TOTAL)
    mix = mus * 0.9 + game * 0.3
    n = len(mix)
    fi = int(0.4 * SR)
    mix[:fi] *= np.linspace(0, 1, fi)
    fo = int(2.0 * SR)
    mix[-fo:] *= np.linspace(1, 0, fo)
    mix /= max(1e-6, np.abs(mix).max()) / 0.95
    wav = os.path.join(OUT, 'trailer.wav')
    with wave.open(wav, 'wb') as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(SR)
        wf.writeframes((mix * 32767).astype('<i2').tobytes())

    enc = subprocess.Popen([FFMPEG, '-y', '-loglevel', 'error', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
                            '-s', '%dx%d' % (W, H), '-r', str(FPS), '-i', '-', '-i', wav,
                            '-c:v', 'libx264', '-preset', 'slow', '-crf', '18', '-pix_fmt', 'yuv420p',
                            '-movflags', '+faststart', '-c:a', 'aac', '-b:a', '192k', '-shortest', out_mp4],
                           stdin=subprocess.PIPE)
    sheet = []
    t = 0.0
    for f, s, d, zoom, anchor in CUT:
        path = f if f.endswith('.png') else os.path.join(HEADSET, f)
        for fr in frames_of(path, s, d, zoom, anchor):
            img = Image.fromarray(fr).convert('RGBA')
            for a, b, text in CAPTIONS:
                caption(img, text, fade(t, a, b))
            end_card(img, max(0.0, min(1.0, (t - END_AT) / 0.7)), icon)
            if t < 0.4:   # from black
                img = Image.blend(Image.new('RGBA', (W, H), (0, 0, 0, 255)), img, t / 0.4)
            rgb = img.convert('RGB')
            enc.stdin.write(rgb.tobytes())
            if abs(t - round(t)) < 0.5 / FPS:
                sheet.append(rgb.resize((384, 216), Image.LANCZOS))
            t += 1.0 / FPS
        print('%-45s %5.1f s  -> %5.1f s' % (os.path.basename(f), s, t))
    enc.stdin.close()
    enc.wait()
    if enc.returncode:
        sys.exit('ffmpeg failed')
    cols = 6
    rows = (len(sheet) + cols - 1) // cols
    sh = Image.new('RGB', (cols * 384, rows * 216), (20, 20, 20))
    for i, im in enumerate(sheet):
        sh.paste(im, ((i % cols) * 384, (i // cols) * 216))
    sh.save(os.path.join(OUT, 'newcut_sheet.jpg'), quality=88)
    print('%s (%.1f s, %dx%d, %d fps)' % (out_mp4, TOTAL, W, H, FPS))


if __name__ == '__main__':
    main()
