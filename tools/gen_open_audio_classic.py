"""FROZEN COPY (2026-10-17) of the generator that made the classic Project Arena sound set, kept to rebuild it exactly.
Project Arena sound set: every sound synthesized here (no samples, no CCP content).

Design: each sound is layered (transient + body + tail), shaped with FFT filters and pitch envelopes, given some
analogue warmth (soft saturation) and placed in a generated arena reverb. Crowd reactions are built from a few dozen
synthetic voices (jittered glottal pulse trains through vowel formants).

Writes app/assets_open/audio/*.wav (48 kHz mono 16-bit) and events.txt (keys used by app/src/audio.cpp).
Usage: python -I tools/gen_open_audio.py
"""
import os
import wave

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'sounds', 'classic')   # frozen: the original set (see sounds/classic/README.md)
SR = 48000
rng = np.random.default_rng(1977)


# ---------------------------------------------------------------- building blocks
def T(sec):
    return np.arange(int(sec * SR)) / SR


def n_(sec):
    return int(sec * SR)


def noise(sec):
    return rng.standard_normal(n_(sec))


def pink(sec):
    x = noise(sec)
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / SR)
    X /= np.sqrt(np.maximum(f, 20.0))
    y = np.fft.irfft(X, len(x))
    return y / (np.std(y) + 1e-9)


def osc(freq, sec=None, shape='sine', phase=0.0):
    """freq: scalar or per-sample array (Hz)."""
    if np.isscalar(freq):
        freq = np.full(n_(sec), float(freq))
    ph = 2 * np.pi * np.cumsum(freq) / SR + phase
    if shape == 'sine':
        return np.sin(ph)
    if shape == 'tri':
        return 2 / np.pi * np.arcsin(np.sin(ph))
    if shape == 'saw':   # band-limited-ish: sum of 12 harmonics, rolled off
        out = np.zeros_like(ph)
        for k in range(1, 13):
            out += np.sin(k * ph) / k * (np.where(freq * k < SR / 2.2, 1.0, 0.0))
        return out * 0.6
    if shape == 'square':
        out = np.zeros_like(ph)
        for k in range(1, 14, 2):
            out += np.sin(k * ph) / k * (np.where(freq * k < SR / 2.2, 1.0, 0.0))
        return out * 0.8
    raise ValueError(shape)


def sweep(f0, f1, sec, curve='exp'):
    t = np.linspace(0, 1, n_(sec))
    return f0 * (f1 / f0) ** t if curve == 'exp' else f0 + (f1 - f0) * t


def env(sec, a=0.005, d=0.3, s=0.0, r=None, curve=4.0):
    """Attack (linear) then exponential decay toward sustain level s; optional release tail length r."""
    t = T(sec)
    e = np.minimum(1.0, t / max(a, 1e-4))
    dec = s + (1 - s) * np.exp(-np.maximum(0, t - a) * curve / max(d, 1e-4))
    e = e * dec
    if r:
        rel = np.clip((sec - t) / r, 0, 1)
        e *= rel
    return e


def bell(sec, peak=0.4, width=0.25):
    t = np.linspace(0, 1, n_(sec))
    return np.exp(-((t - peak) / width) ** 2)


def fft_filter(x, gain_fn):
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / SR)
    return np.fft.irfft(X * gain_fn(f), len(x))


def lp(x, fc, order=2):
    return fft_filter(x, lambda f: 1 / np.sqrt(1 + (f / fc) ** (2 * order)))


def hp(x, fc, order=2):
    return fft_filter(x, lambda f: 1 / np.sqrt(1 + (fc / np.maximum(f, 1e-3)) ** (2 * order)))


def bp(x, fc, q=2.0):
    return fft_filter(x, lambda f: 1 / np.sqrt(1 + q * q * (f / fc - fc / np.maximum(f, 1e-3)) ** 2))


def bp_sweep(x, f0, f1, q=3.0, segs=48):
    """Time-varying band-pass by overlapping segments (Hann crossfades)."""
    n = len(x)
    out = np.zeros(n)
    hop = n // segs
    win = 2 * hop
    w = np.hanning(win)
    for i in range(segs + 1):
        s0 = max(0, i * hop - hop // 2)
        s1 = min(n, s0 + win)
        if s1 - s0 < 16:
            continue
        frac = min(1.0, (s0 + s1) / 2 / n)
        fc = f0 * (f1 / f0) ** frac
        seg = x[s0:s1] * w[:s1 - s0]
        out[s0:s1] += bp(seg, fc, q)
    return out


def modal(freqs, decays, amps, sec, jitter=0.0):
    """Sum of exponentially decaying sines (struck objects)."""
    t = T(sec)
    out = np.zeros_like(t)
    for f, d, a in zip(freqs, decays, amps):
        f = f * (1 + jitter * rng.standard_normal())
        out += a * np.sin(2 * np.pi * f * t + rng.random() * 6.28) * np.exp(-t / d)
    return out


def sat(x, drive=1.5):
    drive = 1.0 + (drive - 1.0) * 0.6   # softened: less edge
    return np.tanh(x * drive) / np.tanh(drive)


def pad(x, sec):
    n = n_(sec)
    return np.pad(x, (0, max(0, n - len(x))))[:n] if len(x) < n else x[:n]


def mix(*parts, length=None):
    L = length or max(len(p) for p in parts)
    out = np.zeros(L)
    for p in parts:
        m = min(L, len(p))
        out[:m] += p[:m]
    return out


def delay(x, sec):
    return np.concatenate([np.zeros(n_(sec)), x])


_ir_cache = {}


def reverb(x, size=1.2, wet=0.25, bright=4000.0, predelay=0.012):
    """Convolution with a generated arena impulse response (decaying, darkening noise with early reflections)."""
    key = (size, bright, predelay)
    if key not in _ir_cache:
        t = T(size * 1.3)
        ir = rng.standard_normal(len(t)) * np.exp(-t * 6.9 / size)
        # darken the tail: blend a low-passed copy in over time
        ir = ir * np.exp(-t * 2.0) + lp(ir, bright * 0.25) * (1 - np.exp(-t * 2.0))
        for dt, g in ((0.017, 0.5), (0.029, 0.35), (0.043, 0.3), (0.061, 0.22)):
            k = n_(dt)
            if k < len(ir):
                ir[k] += g * 3
        ir = np.concatenate([np.zeros(n_(predelay)), ir])
        ir /= np.sqrt(np.sum(ir ** 2)) + 1e-9
        _ir_cache[key] = ir
    ir = _ir_cache[key]
    n = len(x) + len(ir) - 1
    N = 1 << (n - 1).bit_length()
    y = np.fft.irfft(np.fft.rfft(x, N) * np.fft.rfft(ir, N), N)[:n]
    dry = np.pad(x, (0, n - len(x)))
    return dry * (1 - wet * 0.5) + y * wet * 2.0


def soften(x, glue=0.14):
    """Owner feedback: 'more sharp than I want'. Roll the highs off gently (first-order low-pass at 6.5 kHz blended
    with the dry signal), round off the first milliseconds of every transient, and put everything in one shared
    arena space so the set blends."""
    y = 0.35 * x + 0.65 * fft_filter(x, lambda f: 1 / np.sqrt(1 + (f / 6500.0) ** 2))
    y = fft_filter(y, lambda f: 1 / np.sqrt(1 + (f / 11000.0) ** 4))
    # transient softener: limit how fast the envelope may rise (about 4 ms attack)
    env_ = np.abs(y)
    k = n_(0.004)
    smooth = np.convolve(env_, np.ones(k) / k, mode='same')
    lim = np.maximum(smooth * 2.2, np.max(env_) * 0.05)
    y = y * np.minimum(1.0, lim / np.maximum(env_, 1e-9))
    return reverb(y, 1.1, glue, bright=3500, predelay=0.018)


def finish(x, peak=0.85, fade=0.02, trim=True, soft=True):
    x = np.asarray(x, float)
    x -= np.mean(x)
    if soft:
        x = soften(x)
    if trim:   # drop the silent tail
        thr = np.max(np.abs(x)) * 0.004   # -48 dB
        idx = np.nonzero(np.abs(x) > thr)[0]
        if len(idx):
            x = x[:idx[-1] + n_(0.02)]
    f = min(len(x) // 2, n_(fade))
    x[-f:] *= np.linspace(1, 0, f) ** 2
    a = min(len(x) // 4, n_(0.0015))
    x[:a] *= np.linspace(0, 1, a)
    m = np.max(np.abs(x)) or 1.0
    return x / m * peak


# ---------------------------------------------------------------- crowd voices
FORMANTS = {'ah': [(750, 90), (1200, 110), (2600, 160)], 'oh': [(500, 80), (880, 90), (2450, 150)],
            'eh': [(560, 80), (1750, 120), (2550, 160)], 'oo': [(320, 60), (800, 80), (2300, 150)]}


def formant_filter(x, vowel, shift=1.0):
    fm = FORMANTS[vowel]

    def g(f):
        out = np.zeros_like(f)
        for k, (fc, bw) in enumerate(fm):
            fc *= shift
            out += (1.0 / (k + 1)) / np.sqrt(1 + ((f - fc) / (bw * 1.6)) ** 2)
        return out * (f > 70)
    return fft_filter(x, g)


def crowd(sec, vowel='ah', voices=40, pitch_glide=(1.0, 1.0), swell=(0.25, 0.6), base=170.0, excite=1.0):
    """A crowd vocalisation: many jittered voices (mixed male / female ranges) through vowel formants."""
    t = T(sec)
    total = np.zeros_like(t)
    glide = np.linspace(pitch_glide[0], pitch_glide[1], len(t))
    for v in range(voices):
        f0 = base * (0.62 if rng.random() < 0.5 else 1.25) * (1 + 0.12 * rng.standard_normal())
        vib = 1 + 0.012 * np.sin(2 * np.pi * (4.5 + 2 * rng.random()) * t + rng.random() * 6.28)
        drift = 1 + 0.03 * np.cumsum(rng.standard_normal(len(t))) / np.sqrt(len(t)) * 3
        f = f0 * glide * vib * drift
        src = osc(f, shape='saw') + 0.35 * rng.standard_normal(len(t))
        onset = rng.random() * swell[0] * sec
        length = sec * (swell[1] + 0.35 * rng.random())
        e = np.clip((t - onset) / 0.12, 0, 1) * np.clip((onset + length - t) / 0.35, 0, 1)
        total += src * e * (0.6 + 0.4 * rng.random())
    shift = 1.0 + 0.08 * (excite - 1.0)
    y = formant_filter(total, vowel, shift) + 0.25 * formant_filter(pink(sec), vowel, shift) * bell(sec, 0.5, 0.45)
    return hp(y, 90)


def claps(sec, rate=18.0, density_env=None):
    """Applause: many short filtered noise bursts."""
    out = np.zeros(n_(sec))
    n = int(rate * sec)
    burst = hp(bp(noise(0.03), 1800, 0.8), 400) * env(0.03, 0.001, 0.015)
    for _ in range(n):
        p = rng.random()
        if density_env is not None and rng.random() > density_env[int(p * (len(density_env) - 1))]:
            continue
        i = int(p * (len(out) - len(burst)))
        out[i:i + len(burst)] += burst * (0.4 + 0.6 * rng.random())
    return out


# ---------------------------------------------------------------- the sounds
def s_spawn():
    body = osc(sweep(900, 300, 0.18), shape='sine') * env(0.18, 0.002, 0.08)
    sub = osc(sweep(260, 120, 0.25)) * env(0.25, 0.002, 0.12) * 0.6
    click = bp(noise(0.02), 3500, 1.2) * env(0.02, 0.0005, 0.006) * 0.5
    shimmer = osc(1800, 0.35) * env(0.35, 0.01, 0.15) * 0.08
    return reverb(mix(body, sub, click, shimmer), 0.9, 0.22)


def s_spawn_rise():
    sec = 1.3
    air = bp_sweep(pink(sec), 260, 2600, q=2.2) * (np.linspace(0.2, 1, n_(sec)) ** 1.5) * env(sec, 0.02, 9, r=0.18)
    tone = osc(sweep(180, 560, sec), shape='tri') * np.linspace(0, 1, n_(sec)) ** 2 * env(sec, 0.05, 9, r=0.15) * 0.18
    hiss = hp(noise(sec), 6000) * np.linspace(0, 1, n_(sec)) ** 3 * 0.08
    return reverb(mix(air, tone, hiss) * 0.9, 1.0, 0.25)


def s_grab():
    thunk = osc(sweep(170, 110, 0.12)) * env(0.12, 0.001, 0.05)
    tick = hp(noise(0.01), 5000) * env(0.01, 0.0002, 0.003) * 0.5
    hum = lp(osc(220, 0.3, 'saw') + osc(221.7, 0.3, 'saw'), 1600) * env(0.3, 0.01, 0.12) * 0.25
    hum *= 1 + 0.3 * np.sin(2 * np.pi * 34 * T(0.3))
    return reverb(sat(mix(thunk, tick, hum), 1.3), 0.6, 0.15)


def s_caught():
    thunk = osc(sweep(200, 120, 0.14)) * env(0.14, 0.001, 0.06)
    zip_ = osc(sweep(420, 1700, 0.12), shape='tri') * env(0.12, 0.002, 0.06) * 0.35
    tick = hp(noise(0.012), 4500) * env(0.012, 0.0002, 0.004) * 0.6
    ring = modal([1320, 2780], [0.12, 0.07], [0.15, 0.08], 0.4)
    return reverb(sat(mix(thunk, zip_, tick, ring), 1.4), 0.7, 0.18)


def s_throw(tier):
    sec = 0.55 + 0.05 * tier
    bright = [1400, 2200, 3400, 5200][tier]
    whoosh = bp_sweep(pink(sec), bright * 1.4, bright * 0.35, q=1.6) * bell(sec, 0.28, 0.22)
    zap = osc(sweep(900 + 300 * tier, 160, sec * 0.5), shape='saw')
    zap = lp(zap, 2500 + 900 * tier) * env(sec * 0.5, 0.002, 0.12) * (0.18 + 0.06 * tier)
    thump = osc(sweep(120, 55, 0.12)) * env(0.12, 0.001, 0.05) * (0.25 * tier)
    return reverb(sat(mix(whoosh * (0.7 + 0.15 * tier), zap, thump), 1.2 + 0.2 * tier), 0.9, 0.2)


def s_throw_fail():
    sec = 0.7
    crackle = np.zeros(n_(sec))
    for _ in range(60):
        i = rng.integers(0, n_(sec) - 400)
        crackle[i:i + 300] += hp(noise(300 / SR), 2500) * np.exp(-np.arange(300) / 60) * rng.random()
    crackle *= np.linspace(1, 0.1, n_(sec))
    fall = osc(sweep(700, 140, sec), shape='tri') * env(sec, 0.01, 0.4) * 0.3
    return reverb(lp(mix(crackle * 0.5, fall), 5000), 0.8, 0.2)


def s_bounce():
    knock = modal([410, 1185, 2330, 3920, 5600], [0.10, 0.07, 0.05, 0.035, 0.02], [1, 0.6, 0.45, 0.3, 0.2], 0.4)
    click = hp(noise(0.006), 3000) * env(0.006, 0.0002, 0.002)
    low = osc(sweep(140, 90, 0.08)) * env(0.08, 0.001, 0.03) * 0.6
    return reverb(sat(mix(knock * 0.5, click * 0.7, low), 1.2), 1.1, 0.25)


def s_shield_block():
    sec = 1.1
    punch = osc(sweep(110, 55, 0.25)) * env(0.25, 0.001, 0.12)
    ring = modal([523, 1046 * 1.01, 1568, 2637, 3520], [0.5, 0.35, 0.25, 0.18, 0.12], [0.6, 0.4, 0.35, 0.25, 0.15], sec)
    burst = bp(noise(0.25), 2400, 0.9) * env(0.25, 0.001, 0.07) * 0.5
    vwoom = lp(osc(sweep(300, 90, 0.5), shape='saw'), 1200) * env(0.5, 0.004, 0.2) * 0.35
    return reverb(sat(mix(punch, ring * 0.6, burst, vwoom), 1.4), 1.3, 0.3)


def s_shield_break():
    sec = 1.4
    out = np.zeros(n_(sec))
    for _ in range(26):   # glassy shards
        start = int(rng.random() ** 2 * n_(0.6))
        f = 1800 + rng.random() * 5200
        shard = modal([f, f * 1.47, f * 2.13], [0.06, 0.04, 0.03], [1, 0.5, 0.3], 0.25)
        out[start:start + len(shard)] += shard[:len(out) - start] * (0.3 + 0.7 * rng.random())
    burst = hp(noise(0.3), 800) * env(0.3, 0.001, 0.1) * 0.8
    fall = osc(sweep(900, 120, 0.8), shape='saw')
    fall = lp(fall, 2000) * env(0.8, 0.003, 0.35) * 0.3
    boom = osc(sweep(90, 40, 0.4)) * env(0.4, 0.001, 0.2) * 0.8
    return reverb(sat(mix(out * 0.35, burst, fall, boom), 1.3), 1.4, 0.3)


def s_knuckle_block():
    thump = osc(sweep(160, 70, 0.12)) * env(0.12, 0.001, 0.05)
    glass = modal([620, 1490, 2910, 4300], [0.08, 0.06, 0.04, 0.03], [0.7, 0.5, 0.35, 0.2], 0.35)
    snap = bp(noise(0.02), 3000, 1.0) * env(0.02, 0.0005, 0.006) * 0.8
    return reverb(sat(mix(thump, glass * 0.6, snap), 1.5), 0.8, 0.2)


def s_hit_opponent():
    """You scored: plasma discharge, a bright zap and a sparkle tail."""
    sec = 1.6
    t = T(0.6)
    crack = hp(noise(0.6), 1500) * (np.abs(np.sin(2 * np.pi * 37 * t)) ** 6) * env(0.6, 0.001, 0.25) * 0.9
    zap = osc(sweep(2400, 300, 0.35), shape='saw')
    zap = lp(zap, 6000) * env(0.35, 0.001, 0.12) * 0.5
    boom = osc(sweep(130, 50, 0.4)) * env(0.4, 0.001, 0.18) * 0.9
    sparkle = np.zeros(n_(sec))
    for _ in range(40):
        i = int((0.1 + rng.random() * 0.9) * n_(sec) * 0.8)
        f = 2500 + rng.random() * 6000
        s = osc(f, 0.06) * env(0.06, 0.001, 0.02)
        sparkle[i:i + len(s)] += s[:len(sparkle) - i] * 0.25 * rng.random()
    return reverb(sat(mix(crack, zap, boom, sparkle), 1.3), 1.6, 0.32)


def s_hit_self():
    """You were hit: heavy, muffled impact."""
    sec = 1.3
    boom = osc(sweep(95, 34, 0.7)) * env(0.7, 0.001, 0.35)
    crunch = sat(lp(noise(0.25), 1800) * env(0.25, 0.001, 0.08) * 3, 3) * 0.5
    zap = lp(osc(sweep(800, 90, 0.4), shape='square'), 1500) * env(0.4, 0.002, 0.15) * 0.3
    rumble = lp(pink(sec), 220) * env(sec, 0.01, 0.6) * 0.4
    return reverb(sat(mix(boom, crunch, zap, rumble), 1.6), 1.3, 0.25, bright=2500)


def s_returned():
    sec = 1.1
    f = sweep(300, 950, sec) * (1 + 0.02 * np.sin(2 * np.pi * 6 * T(sec)))
    tone = osc(f, shape='tri') * bell(sec, 0.55, 0.35) * 0.5 + osc(f * 2.01) * bell(sec, 0.6, 0.3) * 0.12
    air = bp_sweep(pink(sec), 600, 2400, q=2.5) * bell(sec, 0.6, 0.3) * 0.5
    return reverb(mix(tone, air), 1.2, 0.3)


def s_foul():
    a = lp(osc(440, 0.22, 'square'), 1800) * env(0.22, 0.004, 0.15, s=0.5, r=0.05)
    b = lp(osc(311, 0.32, 'square'), 1500) * env(0.32, 0.004, 0.2, s=0.5, r=0.08)
    fizz = hp(noise(0.5), 3000) * env(0.5, 0.01, 0.2) * 0.12
    return reverb(sat(mix(a, delay(b, 0.2), fizz) * 0.6, 1.2), 0.9, 0.22)


def s_countdown():
    x = osc(880, 0.18) * env(0.18, 0.003, 0.07) + osc(1760, 0.18) * env(0.18, 0.003, 0.03) * 0.25
    return reverb(x, 0.8, 0.2)


def s_round_start():
    sec = 1.5
    t = T(sec)
    chord = sum(lp(osc(f * (1 + 0.004 * k), sec, 'saw'), 3000) for k, f in enumerate((220, 277.2, 329.6, 440)))
    chord *= env(sec, 0.02, 0.6) * 0.3
    rise = bp_sweep(pink(sec), 400, 5000, q=2.0) * np.clip(1 - t / 0.5, 0, 1) * 0.4
    hit = osc(sweep(140, 60, 0.3)) * env(0.3, 0.001, 0.12) * 0.8
    return reverb(sat(mix(chord, rise, hit), 1.2), 1.5, 0.32)


def s_round_end(win):
    sec = 2.6
    v = crowd(sec, 'ah' if win else 'oh', voices=36, pitch_glide=(0.95, 1.12) if win else (1.05, 0.82),
              swell=(0.15, 0.65), base=190 if win else 160, excite=1.3 if win else 0.9)
    c = claps(sec, 50, np.clip(np.linspace(1, 0.1, 100), 0, 1)) * (0.5 if win else 0.12)
    return reverb(mix(v * 0.8, c), 2.0, 0.35, bright=3000)


def s_match_end(win):
    sec = 5.0 if win else 4.0
    v = crowd(sec, 'ah' if win else 'oh', voices=56, pitch_glide=(0.95, 1.18) if win else (1.0, 0.75),
              swell=(0.2, 0.75), base=200 if win else 155, excite=1.5 if win else 0.8)
    c = claps(sec, 160 if win else 25, np.clip(np.linspace(0.6, 1.0, 100) * np.linspace(1, 0.3, 100), 0, 1))
    if win:
        chord = sum(lp(osc(f, sec, 'saw'), 3500) for f in (261.6, 329.6, 392.0, 523.3)) * env(sec, 0.05, 2.5) * 0.18
    else:
        chord = sum(lp(osc(sweep(f, f * 0.85, sec), shape='saw'), 1800) for f in (220.0, 261.6, 311.1)) * env(sec, 0.05, 2.0) * 0.14
    return reverb(mix(v * 0.75, c * (0.5 if win else 0.15), chord), 2.4, 0.38, bright=3200)


def s_recharge():
    sec = 1.0
    f = sweep(220, 1760, 0.7)
    charge = pad(osc(f, shape='saw'), sec)
    charge = lp(charge, 4000) * pad(np.linspace(0.2, 1, n_(0.7)) ** 1.5, sec) * 0.35
    ding = delay(osc(1760, 0.35) * env(0.35, 0.002, 0.12) + osc(2637, 0.35) * env(0.35, 0.002, 0.08) * 0.4, 0.68)
    sparkle = hp(noise(sec), 7000) * pad(np.linspace(0, 1, n_(0.7)) ** 2, sec) * 0.08
    return reverb(mix(charge, ding * 0.6, sparkle), 1.0, 0.25)


def s_strike():
    crack = hp(noise(0.05), 2000) * env(0.05, 0.0005, 0.012) * 1.2
    ping = modal([1568, 3136, 4700], [0.25, 0.15, 0.1], [0.6, 0.3, 0.2], 0.6)
    low = osc(sweep(180, 90, 0.15)) * env(0.15, 0.001, 0.06) * 0.6
    return reverb(sat(mix(crack, ping, low), 1.4), 1.2, 0.3)


def s_overtime():
    tones = []
    for i, f in enumerate((660, 880, 660, 880)):
        tones.append(delay(lp(osc(f, 0.16, 'square'), 2500) * env(0.16, 0.004, 0.1, s=0.4, r=0.03), i * 0.19))
    swell = bp_sweep(pink(1.0), 300, 3000, q=2.0) * np.linspace(0, 1, n_(1.0)) ** 2 * 0.3
    return reverb(sat(mix(*tones, swell) * 0.6, 1.3), 1.2, 0.28)


def s_ambience():
    sec = 32.0
    t = T(sec)
    hum = (osc(55, sec) * 0.5 + osc(110.3, sec) * 0.2 + osc(82.4, sec) * 0.12) * (0.85 + 0.15 * np.sin(2 * np.pi * 0.07 * t))
    air = lp(pink(sec), 900) * 0.5 + hp(lp(pink(sec), 5000), 2000) * 0.05
    murmur = crowd(sec, 'eh', voices=30, swell=(0.0, 1.2), base=150) * 0.12
    murmur = lp(murmur, 1800) * (0.7 + 0.3 * np.sin(2 * np.pi * 0.05 * t + 1.0))
    x = reverb(mix(hum * 0.35, air, murmur), 2.5, 0.4, bright=2500)[:len(t)]
    # seamless loop: crossfade the last 2 s into the start
    xf = n_(2.0)
    head, tail = x[:xf].copy(), x[-xf:]
    w = np.linspace(0, 1, xf)
    x = x[:-xf]
    x[:xf] = head * w + tail * (1 - w)
    return x


SOUNDS = [
    ('Spawn', s_spawn), ('SpawnRise', s_spawn_rise), ('Grab', s_grab), ('Caught', s_caught),
    ('ThrowSlow', lambda: s_throw(0)), ('ThrowMed', lambda: s_throw(1)), ('ThrowFast', lambda: s_throw(2)),
    ('ThrowFull', lambda: s_throw(3)), ('ThrowFail', s_throw_fail), ('Bounce', s_bounce),
    ('ShieldBlock', s_shield_block), ('ShieldBreak', s_shield_break), ('KnuckleBlock', s_knuckle_block),
    ('HitPlayerOpponent', s_hit_opponent), ('HitPlayerSelf', s_hit_self), ('Returned', s_returned), ('Foul', s_foul),
    ('Countdown', s_countdown), ('RoundStart', s_round_start), ('RoundEndWin', lambda: s_round_end(True)),
    ('RoundEndLose', lambda: s_round_end(False)), ('MatchEndWin', lambda: s_match_end(True)),
    ('MatchEndLose', lambda: s_match_end(False)), ('Recharge', s_recharge), ('Strike', s_strike),
    ('Overtime', s_overtime), ('Ambience', s_ambience),
]
LEVEL = {'Ambience': 0.32, 'Bounce': 0.6, 'Countdown': 0.55, 'Grab': 0.7, 'SpawnRise': 0.6, 'Spawn': 0.6}


def save(name, x):
    os.makedirs(OUT, exist_ok=True)
    with wave.open(os.path.join(OUT, name + '.wav'), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes((np.clip(x, -1, 1) * 32767).astype('<i2').tobytes())


def main():
    for f in os.listdir(OUT) if os.path.isdir(OUT) else []:
        if f.endswith('.wav'):
            os.remove(os.path.join(OUT, f))
    lines = []
    for key, fn in SOUNDS:
        x = fn()
        x = finish(x, LEVEL.get(key, 0.85), trim=(key != 'Ambience'), soft=(key != 'Ambience'))
        name = 'oa_' + key
        save(name, x)
        lines.append('%s %s' % (key, name))
        print('  %-18s %5.2fs' % (key, len(x) / SR))
    with open(os.path.join(OUT, 'events.txt'), 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print('wrote %d sounds -> %s' % (len(lines), OUT))


if __name__ == '__main__':
    main()
