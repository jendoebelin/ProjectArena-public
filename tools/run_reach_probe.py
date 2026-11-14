"""Reach probe: builds app/tests/reach_probe.cpp, runs it (catches in bot matches, the knuckle box around the hand,
backhands at several hand speeds) and draws the results to build/tests/reach/*.png, with a summary printed.
Usage: python -I tools/run_reach_probe.py"""
import csv
import os
import subprocess
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'build', 'tests', 'reach')
os.makedirs(OUT, exist_ok=True)
EXE = os.path.join(ROOT, 'build', 'tests', 'reach_probe.exe')
b = subprocess.run(['cmd.exe', '/c', os.path.join(ROOT, 'tools', 'run_reach_probe.bat')], capture_output=True, text=True)
if b.returncode:
    print(b.stdout[-3000:], b.stderr[-2000:])
    sys.exit('build failed')
r = subprocess.run([EXE, os.path.join(ROOT, 'app', 'assets_open', 'arena.col'), os.path.join(OUT, 'probe')], capture_output=True, text=True)
print(r.stdout)
if r.returncode:
    sys.exit(r.stderr[-2000:])


def rows(name):
    with open(os.path.join(OUT, 'probe_%s.csv' % name)) as f:
        return list(csv.DictReader(f))


import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.patches import Circle, Rectangle, FancyArrow  # noqa: E402

BG, FG, MUTED, CYAN, ORANGE, GOOD, BAD = '#0a121c', '#e4edf5', '#8fa4b7', '#45d3ff', '#ff9843', '#5fd38d', '#ff7a7a'
plt.rcParams.update({'figure.facecolor': BG, 'axes.facecolor': '#0f1b28', 'axes.edgecolor': '#22374c', 'axes.labelcolor': FG,
                     'xtick.color': MUTED, 'ytick.color': MUTED, 'text.color': FG, 'font.size': 10})

# ---- 1. catches
cat = rows('catches')
LEVELS = ['ROOKIE', 'AMATEUR', 'PRO', 'ALL-STAR', 'CHAMPION', 'UNBEATABLE']
per = defaultdict(lambda: [0, 0])
miss_by = []
for c in cat:
    lv = int(c['level'])
    per[lv][0] += 1
    per[lv][1] += int(c['caught'])
    if not int(c['caught']):
        miss_by.append(float(c['closest_cm']) - float(c['reach_cm']))
print('CATCHES (bot matches, six minutes a level): flights where a hand had the trigger held within 90 cm of its own ball')
for lv in sorted(per):
    n, k = per[lv]
    print('  %-11s %4d flights, %4d caught (%3.0f%%)' % (LEVELS[lv] if lv < len(LEVELS) else lv, n, k, 100.0 * k / max(1, n)))
near = [m for m in miss_by if m < 15]
print('  misses: %d; of them %d passed within 15 cm outside the reach (%s)' % (
    len(miss_by), len(near), ', '.join('%.0f' % m for m in sorted(near)[:12]) + (' ...' if len(near) > 12 else '')))
fig, ax = plt.subplots(figsize=(7, 3.2))
if miss_by:
    ax.hist([max(-40, min(80, m)) for m in miss_by], bins=30, color=ORANGE)
ax.axvline(0, color=CYAN, ls='--')
ax.set_xlabel('missed catches: how far outside the reach the ball passed (cm; 0 = the edge of the reach)')
ax.set_ylabel('flights')
fig.tight_layout()
fig.savefig(os.path.join(OUT, 'catch_misses.png'), dpi=130)

# ---- 2 + 3. the hand, and the backhands
hand = {h['name']: (float(h['x']), float(h['y']), float(h['z'])) for h in rows('hand')}
bh = rows('backhand')
print('BACKHANDS (right hand swung outward through the ball\'s path, the back of the hand leading; 160 swings a speed)')
print('  %6s %10s %10s %18s' % ('m/s', 'counted', 'touched', 'touched, not counted'))
by_speed = defaultdict(list)
for x in bh:
    by_speed[float(x['speed'])].append(x)
for s in sorted(by_speed):
    xs = by_speed[s]
    c = sum(int(x['counted']) for x in xs)
    t = sum(int(x['touched']) for x in xs)
    tn = sum(1 for x in xs if int(x['touched']) and not int(x['counted']))
    print('  %6.0f %9.0f%% %9.0f%% %17.0f%%' % (s, 100.0 * c / len(xs), 100.0 * t / len(xs), 100.0 * tn / len(xs)))

fig, axs = plt.subplots(1, 2, figsize=(11, 5))
bmin, bmax, rball = hand['box_min'], hand['box_max'], hand['ball_r'][0]
for ax, (i, j, title, xl, yl) in zip(axs, [(0, 2, 'From above (right hand)', 'x: to the back of the hand (cm)', 'z: to the wrist (cm)  |  - forward'),
                                         (1, 2, 'From the side', 'y: up (cm)', 'z (cm)')]):
    ax.set_title(title)
    ax.set_aspect('equal')
    ax.add_patch(Rectangle((bmin[i] * 100, bmin[j] * 100), (bmax[i] - bmin[i]) * 100, (bmax[j] - bmin[j]) * 100,
                           fill=True, fc='#45d3ff33', ec=CYAN, lw=2, label='knuckle box (what hits)'))
    for name, col in (('grip', FG), ('catcher', GOOD), ('launcher', ORANGE), ('hold', MUTED)):
        p = hand[name]
        ax.plot(p[i] * 100, p[j] * 100, 'o', color=col, label=name)
    fb, ff = hand['fist_back'], hand['fist_front']
    ax.plot([fb[i] * 100, ff[i] * 100], [fb[j] * 100, ff[j] * 100], color='#f2c14e', lw=3, alpha=.7, label='the fist')
    ax.add_patch(Circle((hand['catcher'][i] * 100, hand['catcher'][j] * 100), 35, fill=False, ec=GOOD, ls=':', lw=1))
    # where the backhands that touched met the box (contact points of the ball's centre, grip space)
    tx = [float(x['contact_' + 'xyz'[i]]) * 100 for x in bh if int(x['touched'])]
    ty = [float(x['contact_' + 'xyz'[j]]) * 100 for x in bh if int(x['touched'])]
    cnt = [int(x['counted']) for x in bh if int(x['touched'])]
    ax.scatter([a for a, k in zip(tx, cnt) if k], [b for b, k in zip(ty, cnt) if k], s=10, color=GOOD, alpha=.6, label='touched and counted')
    ax.scatter([a for a, k in zip(tx, cnt) if not k], [b for b, k in zip(ty, cnt) if not k], s=10, color=BAD, alpha=.8, label='touched, not counted')
    if i == 0:
        ax.add_patch(FancyArrow(8, -5, 8, 0, width=0.6, color=ORANGE))
        ax.text(9, -3.5, 'backhand', color=ORANGE, fontsize=9)
    ax.set_xlim(-25, 25); ax.set_ylim(-30, 20)
    ax.set_xlabel(xl); ax.set_ylabel(yl)
    ax.grid(color='#22374c', lw=.5)
axs[0].legend(loc='lower left', fontsize=7.5, facecolor='#0f1b28', edgecolor='#22374c')
fig.tight_layout()
fig.savefig(os.path.join(OUT, 'knuckle_box.png'), dpi=130)

fig, ax = plt.subplots(figsize=(7, 3.4))
sp = sorted(by_speed)
ax.plot(sp, [100.0 * sum(int(x['counted']) for x in by_speed[s]) / len(by_speed[s]) for s in sp], 'o-', color=GOOD, label='the game counted a hit')
ax.plot(sp, [100.0 * sum(int(x['touched']) for x in by_speed[s]) / len(by_speed[s]) for s in sp], 's--', color=CYAN, label='the ball really touched the box')
ax.set_xlabel('hand speed of the backhand (m/s)'); ax.set_ylabel('% of 160 swings'); ax.set_ylim(0, 105)
ax.legend(facecolor='#0f1b28', edgecolor='#22374c'); ax.grid(color='#22374c', lw=.5)
fig.tight_layout()
fig.savefig(os.path.join(OUT, 'backhand_speed.png'), dpi=130)
print('pictures in', OUT)
