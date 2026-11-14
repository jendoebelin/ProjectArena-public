"""The store trailer: the alpha trailer's cut with "ALPHA BUILD" painted out of the HUD corner and a new end card.

    python -I tools/retitle_trailer.py "D:/Downloads/Project_Arena_Motion_Revision_30s_1080p.mp4"
        ->  build/promo/trailer/project_arena_trailer.mp4  (30 s, 1920x1080, 60 fps, H.264; the audio copied as it is)

Owner, 2026-11-08: the alpha trailer's fast cuts, angles and music are what made it work, so it stays as it is;
only the ALPHA marks go. "ALPHA BUILD" was an editor overlay at a fixed place in the scoreboard band (x 1581..1751,
y 48..67 at 1080p): ffmpeg's delogo fills that box from its edges on every frame. The old end card (from 26.2 s, a
hard cut) said STEP ONTO THE COURT and META QUEST / ALPHA BUILD: the new one keeps the icon, the title, the Discord
invite and the blue-orange rule, says AVAILABLE ON THE META HORIZON STORE (the guidelines' wording), and drops the
tagline (as the covers did), ALPHA and the alpha channel's invite link (the store link goes in the YouTube
description). No Meta logo: in a trailer it must stand alone on its own card, and its use needs Meta's approval.
"""
import os
import subprocess
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_store_art_real import ICON, draw_tracked, font_for_cap  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'build', 'promo', 'trailer')
FFMPEG = ('%USERPROFILE%/AppData/Local/Programs/Python/Python310/lib/site-packages/imageio_ffmpeg/binaries/'
          'ffmpeg-win64-v4.2.2.exe')
W, H = 1920, 1080
END_AT = 26.2                                # the old end card's first frame
LABEL = (1572, 40, 190, 36)                  # x, y, w, h around ALPHA BUILD, a few pixels of margin
WHITE, GREY, BLUE, ORANGE, BG = (255, 255, 255), (150, 160, 175), (26, 179, 255), (255, 106, 26), (5, 7, 12)


def tracked_width(font, text, tracking):
    return sum(font.getlength(c) for c in text) + tracking * (len(text) - 1)


def centred(d, y, text, cap, name, fill, track=0.06):
    f = font_for_cap(cap, name)
    tr = track * cap
    draw_tracked(d, W / 2 - tracked_width(f, text, tr) / 2, y, text, f, fill, tracking=tr)
    return y + cap


def end_card(path):
    img = Image.new('RGB', (W, H), BG)
    d = ImageDraw.Draw(img)
    icon = Image.open(ICON).convert('RGB').resize((170, 170), Image.LANCZOS)
    img.paste(icon, (W // 2 - 85, 290))
    # Meta's brand guidelines (read 2026-11-08): the Meta Quest logo in a trailer "must appear by itself" on its own
    # card, and the brand centre says every use of the Meta logo needs approval, so no symbol here: the store's
    # name in the wording the guidelines give. The alpha link is gone too (it was the release channel's invite, a
    # login wall; the store address goes in the YouTube description instead).
    centred(d, 505, 'PROJECT ARENA', 78, 'SemiBold', WHITE, 0.08)
    centred(d, 665, 'AVAILABLE ON THE META HORIZON STORE', 30, 'Regular', WHITE, 0.12)
    centred(d, 745, 'discord.gg/KuNkSbVHzh', 30, 'Regular', GREY, 0.04)
    d.rectangle((W / 2 - 150, 850, W / 2 - 4, 856), fill=BLUE)
    d.rectangle((W / 2 + 4, 850, W / 2 + 150, 856), fill=ORANGE)
    img.save(path, optimize=True)


def main():
    if len(sys.argv) < 2:
        sys.exit('give the alpha trailer (the 1080p export)')
    src = sys.argv[1]
    os.makedirs(OUT, exist_ok=True)
    card = os.path.join(OUT, 'end_card.png')
    end_card(card)
    out = os.path.join(OUT, 'project_arena_trailer.mp4')
    x, y, w, h = LABEL
    fc = ('[0:v]fps=60,trim=end=%.3f,setpts=PTS-STARTPTS,delogo=x=%d:y=%d:w=%d:h=%d[g];'
          '[1:v]fps=60,format=yuv420p,trim=duration=30,setpts=PTS-STARTPTS,fade=t=in:st=0:d=0.3[e];'
          '[g][e]concat=n=2:v=1:a=0[v]') % (END_AT, x, y, w, h)
    cmd = [FFMPEG, '-y', '-loglevel', 'error', '-i', src, '-loop', '1', '-i', card, '-filter_complex', fc,
           '-map', '[v]', '-map', '0:a', '-c:a', 'copy', '-c:v', 'libx264', '-preset', 'slow', '-crf', '17',
           '-pix_fmt', 'yuv420p', '-movflags', '+faststart', '-shortest', out]
    subprocess.run(cmd, check=True)
    # a contact sheet for a look
    subprocess.run([FFMPEG, '-y', '-loglevel', 'error', '-i', out, '-vf', 'fps=1,scale=384:216,tile=6x5',
                    '-frames:v', '1', os.path.join(OUT, 'trailer_sheet.jpg')], check=True)
    print(out)


if __name__ == '__main__':
    main()
