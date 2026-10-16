"""Synthesize an original (CCP-asset-free) sound set for the Quest port.
Writes app/assets_clean/audio/*.wav (48 kHz mono 16-bit) and events.txt with the same event keys the
app uses (see app/src/audio.cpp). All sounds are generated here from oscillators, noise and envelopes.

Usage: python -I tools/gen_clean_audio.py
"""
import os, wave
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'app', 'assets_clean', 'audio')
SR = 48000
rng = np.random.default_rng(7)


def t(sec):
    return np.arange(int(sec * SR)) / SR


def env(n, a=0.005, d=0.2, sustain=0.0, shape=4.0):
    x = np.arange(n) / SR
    e = np.minimum(1.0, x / max(a, 1e-4))
    rel = np.exp(-np.maximum(0, x - a) * shape / max(d, 1e-4))
    return e * (sustain + (1 - sustain) * rel)


def noise(n):
    return rng.standard_normal(n)


def lowpass(x, cutoff):
    a = np.exp(-2 * np.pi * cutoff / SR)
    y = np.zeros_like(x)
    acc = 0.0
    for i in range(len(x)):
        acc = (1 - a) * x[i] + a * acc
        y[i] = acc
    return y


def bandnoise(n, lo, hi):
    return lowpass(noise(n), hi) - lowpass(noise(n), lo) * 0.0 - lowpass(lowpass(noise(n), lo), lo) * 0.5


def sweep(f0, f1, sec, wave='sine'):
    tt = t(sec)
    f = f0 * (f1 / f0) ** (tt / sec)
    ph = 2 * np.pi * np.cumsum(f) / SR
    return np.sin(ph) if wave == 'sine' else np.sign(np.sin(ph)) * 0.6


def tone(freq, sec, harmonics=(1.0,)):
    tt = t(sec)
    return sum(a * np.sin(2 * np.pi * freq * (k + 1) * tt) for k, a in enumerate(harmonics))


def save(name, x, peak=0.8):
    x = np.asarray(x, float)
    m = np.max(np.abs(x)) or 1
    x = x / m * peak
    # 5 ms fade-out to avoid clicks
    f = min(len(x), SR // 200)
    x[-f:] *= np.linspace(1, 0, f)
    with wave.open(os.path.join(OUT, name + '.wav'), 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes((x * 32767).astype(np.int16).tobytes())


def whoosh(sec, f0, f1, brightness):
    n = int(sec * SR)
    body = lowpass(noise(n), brightness) * env(n, 0.03, sec * 0.6, 0, 3)
    tonal = sweep(f0, f1, sec) * env(n, 0.01, sec * 0.5, 0, 4) * 0.4
    return body + tonal


def impact(sec, freq, ring=0.2, click=1.0):
    n = int(sec * SR)
    hit = lowpass(noise(n), 3000) * env(n, 0.001, 0.03, 0, 6) * click
    body = tone(freq, sec, (1.0, 0.5, 0.25)) * env(n, 0.001, ring, 0, 5)
    return hit + body


def main():
    os.makedirs(OUT, exist_ok=True)
    clips = {}
    clips['Spawn'] = sweep(220, 880, 0.35) * env(int(0.35 * SR), 0.01, 0.3, 0.2, 3) + tone(1320, 0.35) * env(int(0.35 * SR), 0.15, 0.2) * 0.3
    clips['Grab'] = impact(0.25, 660, 0.12, 0.5) + sweep(500, 900, 0.25) * env(int(0.25 * SR), 0.005, 0.2) * 0.4
    clips['Caught'] = impact(0.4, 520, 0.25, 0.8) + sweep(300, 1200, 0.4) * env(int(0.4 * SR), 0.005, 0.3) * 0.5
    for name, (sec, f0, f1, br) in {'ThrowSlow': (0.45, 300, 160, 1200), 'ThrowMed': (0.45, 400, 180, 2000),
                                    'ThrowFast': (0.40, 600, 200, 3500), 'ThrowFull': (0.40, 800, 220, 6000)}.items():
        clips[name] = whoosh(sec, f0, f1, br)
    clips['ThrowFail'] = sweep(600, 150, 0.5) * env(int(0.5 * SR), 0.01, 0.45, 0, 2) + lowpass(noise(int(0.5 * SR)), 900) * env(int(0.5 * SR), 0.01, 0.4) * 0.3
    clips['Bounce'] = impact(0.35, 180, 0.15, 1.0) + tone(1100, 0.35) * env(int(0.35 * SR), 0.001, 0.08) * 0.25
    clips['ShieldBlock'] = impact(0.8, 440, 0.5, 1.2) + tone(880, 0.8, (1, 0.3)) * env(int(0.8 * SR), 0.002, 0.6) * 0.5
    clips['ShieldBreak'] = lowpass(noise(int(0.7 * SR)), 5000) * env(int(0.7 * SR), 0.001, 0.4, 0, 3) + sweep(900, 120, 0.7) * env(int(0.7 * SR), 0.001, 0.5) * 0.6
    clips['KnuckleBlock'] = impact(0.3, 260, 0.1, 1.5)
    clips['HitPlayerOpponent'] = impact(0.9, 120, 0.6, 1.5) + sweep(1600, 200, 0.9) * env(int(0.9 * SR), 0.001, 0.5) * 0.5
    clips['HitPlayerSelf'] = impact(0.9, 80, 0.7, 2.0) + lowpass(noise(int(0.9 * SR)), 600) * env(int(0.9 * SR), 0.001, 0.6) * 0.8
    clips['Returned'] = sweep(300, 1500, 0.5) * env(int(0.5 * SR), 0.01, 0.5, 0.1, 2) * 0.6
    clips['Foul'] = sweep(500, 90, 0.6, 'square') * env(int(0.6 * SR), 0.01, 0.5, 0, 2) * 0.5
    clips['Countdown'] = tone(880, 0.15, (1, 0.3)) * env(int(0.15 * SR), 0.002, 0.12)
    clips['RoundStart'] = np.concatenate([tone(660, 0.12) * env(int(0.12 * SR), 0.002, 0.1), tone(990, 0.4, (1, 0.4)) * env(int(0.4 * SR), 0.002, 0.35)])
    chord = lambda fs, sec: sum(tone(f, sec, (1, 0.3)) for f in fs) * env(int(sec * SR), 0.02, sec * 0.8, 0.2, 2)
    clips['RoundEndWin'] = chord((523, 659, 784), 1.2)
    clips['RoundEndLose'] = chord((392, 466, 587), 1.2)
    clips['MatchEndWin'] = np.concatenate([chord((523, 659, 784), 0.5), chord((587, 740, 880), 0.5), chord((659, 831, 988, 1319), 1.6)])
    clips['MatchEndLose'] = np.concatenate([chord((440, 523, 659), 0.6), chord((392, 466, 587), 0.6), chord((349, 415, 523), 1.4)])
    clips['Recharge'] = sweep(200, 1600, 0.6) * env(int(0.6 * SR), 0.02, 0.6, 0.3, 2) * 0.6
    clips['Strike'] = np.concatenate([tone(1200, 0.08) * env(int(0.08 * SR), 0.001, 0.07), np.zeros(int(0.04 * SR)),
                                      tone(1600, 0.25, (1, 0.5)) * env(int(0.25 * SR), 0.001, 0.2)])
    clips['Overtime'] = np.concatenate([tone(1000, 0.1) * env(int(0.1 * SR), 0.001, 0.09)] * 2 + [tone(1500, 0.3) * env(int(0.3 * SR), 0.001, 0.25)])
    # ambience: low drone + filtered air, 20 s seamless-ish loop
    n = 20 * SR
    tt = np.arange(n) / SR
    amb = 0.4 * np.sin(2 * np.pi * 55 * tt) + 0.25 * np.sin(2 * np.pi * 82.5 * tt + np.sin(2 * np.pi * 0.05 * tt))
    amb += 0.6 * lowpass(noise(n), 250)
    amb *= 0.8 + 0.2 * np.sin(2 * np.pi * 0.1 * tt)
    clips['Ambience'] = amb
    lines = []
    for key, x in clips.items():
        name = 'clean_' + key
        save(name, x, 0.35 if key == 'Ambience' else 0.75)
        lines.append('%s %s' % (key, name))
        print('  %-18s %.2fs' % (key, len(x) / SR))
    open(os.path.join(OUT, 'events.txt'), 'w', newline='\n').write('\n'.join(lines) + '\n')
    print('wrote %d clips to %s' % (len(lines), OUT))


if __name__ == '__main__':
    main()
