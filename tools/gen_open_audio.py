"""Project Arena sound set (subtle, 2026-10-17): every sound synthesized here (no samples, no CCP content).
The first, punchier set is kept in sounds/classic (tools/gen_open_audio_classic.py rebuilds it).

Design: each sound is layered (transient + body + tail), shaped with FFT filters and pitch envelopes, given some
analogue warmth (soft saturation) and placed in a generated arena reverb. Crowd reactions are built from a few dozen
synthetic voices (jittered glottal pulse trains through vowel formants).

Writes app/assets_open/audio/*.wav (48 kHz mono 16-bit) and events.txt (keys used by app/src/audio.cpp).
Usage: python -I tools/gen_open_audio.py
"""
import os
import sys
import wave

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'app', 'assets_open', 'audio')
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


# ---------------------------------------------------------------- the sounds (subtle set, docs/SOUND_BRIEF.md)
# Owner, 2026-10-17: "better, more subtle sounds for each effect". Compared with the classic set (sounds/classic,
# tools/gen_open_audio_classic.py): sine and triangle bodies instead of saws, no saturation, softer and fewer noise
# layers, highs rolled off, short tails in a small dry room, and each slot cut and levelled by
# tools/import_sounds.py (frequent sounds quietest).
def room(x, wet=0.12, size=0.7):
    return reverb(x, size, wet, bright=3000, predelay=0.010)


def thump(f0, f1, sec, amp=1.0):
    return osc(sweep(f0, f1, sec)) * env(sec, 0.004, sec * 0.35) * amp


def s_spawn():
    """a small orb blooming into place"""
    sec = 0.5
    bloom = osc(sweep(420, 760, 0.18), shape='tri')
    bloom = lp(pad(bloom, sec), 2200) * env(sec, 0.018, 0.16) * 0.6
    glint = osc(1520, sec) * env(sec, 0.03, 0.14) * 0.07 + osc(2280, sec) * env(sec, 0.04, 0.10) * 0.03
    return room(mix(bloom, glint), 0.14)


def s_spawn_rise():
    """the launcher lifting: a gentle airy swell, rising"""
    sec = 1.25
    t = T(sec)
    air = bp_sweep(pink(sec), 260, 1300, q=2.4) * (t / sec) ** 1.6 * 0.5
    tone = lp(osc(sweep(150, 380, sec), shape='tri'), 1200) * (t / sec) ** 2 * 0.22
    x = mix(air, tone) * np.clip((sec - t) / 0.12, 0, 1)
    return room(x, 0.12)


def s_grab():
    """hand closing on the ball: muted thump, brief warm hum"""
    hum = osc(220, 0.2) * env(0.2, 0.008, 0.07) * 0.18 + osc(330.5, 0.2) * env(0.2, 0.008, 0.05) * 0.06
    return room(mix(thump(150, 95, 0.09), hum), 0.08)


def s_caught():
    """a firm padded catch with a quick soft rise"""
    zip_ = lp(osc(sweep(480, 1150, 0.11), shape='tri'), 2400) * env(0.11, 0.004, 0.05) * 0.22
    return room(mix(thump(170, 100, 0.11, 1.0), delay(zip_, 0.012)), 0.1)


def s_throw(tier):
    """air moving: a filtered whoosh, brighter and fuller with speed; the hardest throw gets a soft low push"""
    sec = 0.34 + 0.07 * tier
    centre = (850, 1150, 1550, 2000)[tier]
    whoosh = bp_sweep(pink(sec), centre * 1.5, centre * 0.55, q=1.3) * bell(sec, 0.30, 0.24)
    streak = lp(osc(sweep(centre * 0.7, centre * 0.4, sec)), 2500) * bell(sec, 0.35, 0.2) * 0.04 * tier
    push = thump(95, 55, 0.14, 0.35) if tier == 3 else np.zeros(1)
    return room(mix(whoosh * (0.75 + 0.1 * tier), streak, push), 0.1)


def s_throw_fail():
    """the orb losing power: a soft falling tone with a faint fizzle"""
    sec = 0.6
    fall = lp(osc(sweep(520, 170, sec), shape='tri'), 1600) * env(sec, 0.01, 0.25) * 0.4
    fizz = np.zeros(n_(sec))
    for _ in range(14):
        i = rng.integers(0, n_(sec * 0.7))
        g = bp(noise(0.004), 2200, 1.5) * env(0.004, 0.0003, 0.0015)
        fizz[i:i + len(g)] += g[:n_(sec) - i] * 0.25 * rng.random()
    return room(mix(fall, lp(fizz, 3500) * np.linspace(1, 0.2, n_(sec))), 0.1)


def s_bounce():
    """a small ball tapping thick glass: a short soft tok"""
    tok = modal([540, 1310, 2470], [0.045, 0.03, 0.02], [1, 0.35, 0.12], 0.2)
    return room(lp(mix(tok * 0.6, thump(130, 95, 0.05, 0.4)), 3800), 0.08, 0.5)


def s_shield_block():
    """the deflect under the glass (both play on every hit): a quiet short energy thump"""
    return room(lp(mix(thump(120, 70, 0.16, 1.0), lp(bp(noise(0.08), 900, 0.8), 1600) * env(0.08, 0.004, 0.03) * 0.2), 2500), 0.1)


def s_shield_break():
    """the shield shattering like a crushed wine glass (owner, 2026-10-17): a sharp thin crack, a quick crunch of
    tiny shards, then a few small tinkles"""
    sec = 0.9
    out = np.zeros(n_(sec))

    def put(x, at):
        i = int(at * SR)
        if i < len(out):
            out[i:i + len(x)] += x[:len(out) - i]
    ring = modal([3150, 4720, 6240, 7900], [0.09, 0.06, 0.045, 0.03], [1, 0.7, 0.45, 0.25], 0.25)
    crack = bp(noise(0.025), 4800, 1.1) * env(0.025, 0.0004, 0.006)
    put(mix(crack * 0.9, ring * 0.35), 0.0)
    for _ in range(70):   # the crunch: many tiny shards in the first 0.2 s
        at = 0.004 + rng.beta(1.3, 4.0) * 0.2
        f = 2600 + rng.random() * 6500
        shard = modal([f, f * 1.53], [0.006 + rng.random() * 0.025, 0.005 + rng.random() * 0.015], [1, 0.4], 0.06)
        put(shard * (0.15 + 0.35 * rng.random()), at)
        if rng.random() < 0.4:
            put(hp(noise(0.003), 3500) * env(0.003, 0.0002, 0.001) * 0.3 * rng.random(), at)
    for k in range(13):   # small pieces settling
        at = 0.18 + rng.random() ** 1.4 * 0.55
        f = 3000 + rng.random() * 5000
        put(modal([f, f * 1.47], [0.05 + rng.random() * 0.06, 0.03], [1, 0.35], 0.18) * (0.22 * (1 - at / 0.9)), at)
    out = lp(out, 9000)
    return reverb(out * 0.9, 0.5, 0.08, bright=6000, predelay=0.006)


def s_knuckle_block():
    """gauntlet punches the ball away: a tight padded metallic knock"""
    knock = modal([720, 1680, 2900], [0.05, 0.035, 0.022], [0.6, 0.3, 0.12], 0.25)
    return room(lp(mix(thump(160, 85, 0.1, 1.0), knock * 0.55), 4000), 0.1)


def s_hit_opponent():
    """your point: a soft deep thud, a gentle discharge, then a small rising two-note chime"""
    sec = 1.2
    t = T(0.35)
    discharge = lp(hp(noise(0.35), 900), 3500) * (0.5 + 0.5 * np.sin(2 * np.pi * 23 * t)) ** 3 * env(0.35, 0.004, 0.1) * 0.35
    chime = mix(delay(osc(880, 0.45) * env(0.45, 0.006, 0.16) * 0.22, 0.12),
                delay(osc(1318.5, 0.5) * env(0.5, 0.006, 0.2) * 0.2, 0.24))
    return room(mix(thump(120, 52, 0.32, 1.0), discharge, chime, length=n_(sec)), 0.16, 1.0)


def s_hit_self():
    """you were hit: a muffled heavy thud and a low downward wobble"""
    sec = 1.0
    t = T(0.55)
    wobble = lp(osc(sweep(300, 120, 0.55) * (1 + 0.05 * np.sin(2 * np.pi * 9 * t)), shape='tri'), 900) * env(0.55, 0.01, 0.2) * 0.3
    return room(lp(mix(thump(92, 38, 0.45, 1.0), delay(wobble, 0.03), length=n_(sec)), 2400), 0.14, 0.9)


def s_returned():
    """the ball flying home: an airy whoosh moving away, falling slightly"""
    sec = 1.0
    air = bp_sweep(pink(sec), 1300, 480, q=2.0) * bell(sec, 0.25, 0.35) * 0.6
    tone = lp(osc(sweep(620, 420, sec)), 1500) * bell(sec, 0.3, 0.3) * 0.06
    return room(mix(air, tone), 0.14)


def s_foul():
    """a polite penalty tone: two rounded falling notes"""
    a = lp(osc(392, 0.2, 'tri'), 1400) * env(0.2, 0.01, 0.12, s=0.4, r=0.06)
    b = lp(osc(311.1, 0.3, 'tri'), 1300) * env(0.3, 0.01, 0.16, s=0.4, r=0.1)
    return room(mix(a, delay(b, 0.19)) * 0.8, 0.12)


def s_countdown():
    """one calm low beep"""
    x = osc(660, 0.2) * env(0.2, 0.006, 0.08) + osc(1320, 0.2) * env(0.2, 0.006, 0.03) * 0.12
    return room(x, 0.12)


def s_clock_tick():
    """the last 10 seconds of a timed match or round, once a second: a very soft, short wooden tick"""
    sec = 0.25
    click = lp(noise(sec), 3500) * env(sec, 0.0008, 0.012) * 0.5
    body = osc(880, sec) * env(sec, 0.001, 0.035) + osc(1760, sec) * env(sec, 0.001, 0.015) * 0.25
    return room(lp(mix(click, body), 4000), 0.08)


def s_round_start():
    """GO: a short bright swell landing on a soft chord"""
    sec = 1.3
    t = T(sec)
    rise = bp_sweep(pink(0.45), 300, 2400, q=2.2) * np.linspace(0, 1, n_(0.45)) ** 2 * 0.35
    chord = sum(osc(f, sec) * a for f, a in ((523.3, 0.22), (659.3, 0.16), (784.0, 0.13), (1046.5, 0.06)))
    chord = delay(chord * env(sec, 0.012, 0.45), 0.42)
    return room(mix(rise, chord, delay(thump(110, 70, 0.18, 0.5), 0.42)), 0.18, 1.2)


def s_round_end(win):
    sec = 3.0 if win else 2.6
    v = crowd(sec, 'ah' if win else 'oh', voices=30, pitch_glide=(0.97, 1.08) if win else (1.03, 0.86),
              swell=(0.15, 0.6), base=185 if win else 160, excite=1.0 if win else 0.75)
    c = claps(sec, 34 if win else 10, np.clip(np.linspace(1, 0.1, 100), 0, 1)) * (0.35 if win else 0.1)
    return reverb(lp(mix(v * 0.7, c), 3200), 1.6, 0.28, bright=2600)


def s_match_end(win):
    sec = 5.5 if win else 4.5
    v = crowd(sec, 'ah' if win else 'oh', voices=48, pitch_glide=(0.97, 1.12) if win else (1.0, 0.8),
              swell=(0.2, 0.7), base=195 if win else 155, excite=1.15 if win else 0.65)
    c = claps(sec, 110 if win else 30, np.clip(np.linspace(0.6, 1.0, 100) * np.linspace(1, 0.25, 100), 0, 1))
    if win:
        chord = sum(osc(f, sec) * a for f, a in ((261.6, 0.12), (329.6, 0.09), (392.0, 0.08), (523.3, 0.05)))
    else:
        chord = sum(osc(sweep(f, f * 0.94, sec)) * 0.08 for f in (220.0, 261.6, 329.6))
    chord *= env(sec, 0.08, 2.2)
    return reverb(lp(mix(v * 0.7, c * (0.4 if win else 0.18), chord), 3400), 2.0, 0.3, bright=2800)


def s_recharge():
    """power back: a short soft rise and a gentle bright ding"""
    sec = 0.9
    rise = lp(osc(sweep(330, 990, 0.45), shape='tri'), 2400) * np.linspace(0.2, 1, n_(0.45)) * 0.18
    ding = delay(osc(1318.5, 0.45) * env(0.45, 0.005, 0.15) * 0.3 + osc(1975.5, 0.45) * env(0.45, 0.005, 0.1) * 0.1, 0.42)
    return room(mix(rise, ding, length=n_(sec)), 0.14)


def s_strike():
    """a strike: a clear two-note ding-ding over a soft swell"""
    sec = 1.3
    d1 = osc(1046.5, 0.5) * env(0.5, 0.004, 0.16) * 0.3
    d2 = delay(osc(1318.5, 0.6) * env(0.6, 0.004, 0.2) * 0.3, 0.16)
    swell = lp(osc(261.6, 0.8), 800) * env(0.8, 0.05, 0.3) * 0.15
    return room(mix(d1, d2, swell, length=n_(sec)), 0.16, 1.0)


def s_overtime():
    """sudden death: a deep pulsing swell with two heartbeat thumps"""
    sec = 2.2
    t = T(sec)
    swell = lp(bp_sweep(pink(sec), 120, 700, q=1.8), 900) * (t / sec) ** 1.5 * np.clip((sec - t) / 0.3, 0, 1) * 0.45
    beats = mix(thump(70, 45, 0.18, 0.8), delay(thump(70, 45, 0.18, 0.6), 0.22),
                delay(thump(70, 45, 0.18, 0.8), 1.0), delay(thump(70, 45, 0.18, 0.6), 1.22), length=n_(sec))
    pad_ = osc(110, sec) * 0.05 * (0.5 + 0.5 * np.sin(2 * np.pi * 1.2 * t))
    return room(mix(swell, beats, pad_), 0.15, 1.0)


def s_target_hit():
    """TRAINING: a target taken: a bright glassy ping over a quick sparkle of breaking light (0.3.12)"""
    sec = 0.9
    ping = (osc(1760.0, 0.8) * 0.22 + osc(2637.0, 0.8) * 0.10 + osc(3520.0, 0.8) * 0.05) * env(0.8, 0.002, 0.28)
    knock = lp(noise(0.06), 2500) * env(0.06, 0.001, 0.03) * 0.35
    sparkle = hp(noise(0.5), 6000) * env(0.5, 0.01, 0.16) * 0.10
    return room(mix(ping, knock, delay(sparkle, 0.02), length=n_(sec)), 0.12, 0.8)


def s_lead_change():
    """FOUR CORNERS: someone new leads: a bright rising three-note chime with a light shimmer (0.3.12)"""
    sec = 1.0
    notes = [(784.0, 0.0), (987.8, 0.09), (1318.5, 0.18)]
    parts = [delay(osc(f, 0.6) * env(0.6, 0.003, 0.22) * 0.28 + osc(2 * f, 0.6) * env(0.6, 0.002, 0.08) * 0.06, d) for f, d in notes]
    shimmer = lp(noise(0.5), 9000) * env(0.5, 0.05, 0.2) * 0.04
    return room(mix(*parts, delay(shimmer, 0.18), length=n_(sec)), 0.14, 1.0)


def s_fist_bump():
    """two gauntlets bumping: a solid friendly knock and a warm pulse blooming out"""
    sec = 0.8
    knock = modal([610, 1450, 2550], [0.06, 0.04, 0.025], [0.55, 0.3, 0.12], 0.3)
    pulse = osc(sweep(196, 262, 0.5)) * env(0.5, 0.03, 0.16) * 0.25
    return room(lp(mix(thump(170, 95, 0.12, 1.0), knock * 0.6, delay(pulse, 0.03), length=n_(sec)), 4000), 0.14)


def s_ambience():
    sec = 34.0
    t = T(sec)
    hum = (osc(55, sec) * 0.5 + osc(110.3, sec) * 0.15) * (0.88 + 0.12 * np.sin(2 * np.pi * 0.06 * t))
    air = lp(pink(sec), 700) * 0.45
    murmur = crowd(sec, 'eh', voices=24, swell=(0.0, 1.1), base=150) * 0.07
    murmur = lp(murmur, 1400) * (0.7 + 0.3 * np.sin(2 * np.pi * 0.045 * t + 1.0))
    return reverb(mix(hum * 0.3, air, murmur), 2.2, 0.32, bright=2200)[:len(t)]


def s_crowd():
    """The Crowd (fans on phones): an excited stadium crowd, looped; the game turns it up with the fans' hype"""
    sec = 14.0
    t = T(sec)
    v = mix(crowd(sec, 'ah', voices=40, swell=(0.0, 1.1), base=175, excite=1.1) * 0.6,
            crowd(sec, 'eh', voices=30, swell=(0.0, 1.1), base=160) * 0.4)
    v = lp(v, 2600) * (0.8 + 0.2 * np.sin(2 * np.pi * 0.21 * t) * np.sin(2 * np.pi * 0.07 * t + 0.5))
    c = claps(sec, 26) * 0.22
    return reverb(mix(v, c), 2.0, 0.3, bright=2600)[:len(t)]


def s_cheer():
    """the crowd erupts: a rising cheer with applause"""
    sec = 3.4
    v = crowd(sec, 'ah', voices=56, pitch_glide=(0.94, 1.14), swell=(0.12, 0.65), base=190, excite=1.25)
    c = claps(sec, 90, np.clip(np.linspace(0.3, 1.0, 100) * np.linspace(1, 0.3, 100), 0, 1)) * 0.35
    return reverb(lp(mix(v * 0.75, c), 3400), 1.9, 0.3, bright=2700)


SOUNDS = [
    ('Spawn', s_spawn), ('SpawnRise', s_spawn_rise), ('Grab', s_grab), ('Caught', s_caught),
    ('ThrowSlow', lambda: s_throw(0)), ('ThrowMed', lambda: s_throw(1)), ('ThrowFast', lambda: s_throw(2)),
    ('ThrowFull', lambda: s_throw(3)), ('ThrowFail', s_throw_fail), ('Bounce', s_bounce),
    ('ShieldBlock', s_shield_block), ('ShieldBreak', s_shield_break), ('KnuckleBlock', s_knuckle_block),
    ('HitPlayerOpponent', s_hit_opponent), ('HitPlayerSelf', s_hit_self), ('Returned', s_returned), ('Foul', s_foul),
    ('Countdown', s_countdown), ('RoundStart', s_round_start), ('RoundEndWin', lambda: s_round_end(True)),
    ('RoundEndLose', lambda: s_round_end(False)), ('MatchEndWin', lambda: s_match_end(True)),
    ('MatchEndLose', lambda: s_match_end(False)), ('Recharge', s_recharge), ('Strike', s_strike),
    ('Overtime', s_overtime), ('FistBump', s_fist_bump), ('Ambience', s_ambience), ('ClockTick', s_clock_tick),
    ('Crowd', s_crowd), ('Cheer', s_cheer),   # (last: the sounds above keep their random draws)
    ('LeadChange', s_lead_change),            # (0.3.12, after them: the earlier sounds stay exactly as they were)
    ('TargetHit', s_target_hit),              # (TRAINING, 0.3.12)
]


def save(name, x):
    os.makedirs(OUT, exist_ok=True)
    with wave.open(os.path.join(OUT, name + '.wav'), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes((np.clip(x, -1, 1) * 32767).astype('<i2').tobytes())


def main():
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import import_sounds   # each slot's length and loudness (one table for generated and imported sounds)
    for f in os.listdir(OUT) if os.path.isdir(OUT) else []:
        if f.endswith('.wav'):
            os.remove(os.path.join(OUT, f))
    lines = []
    for key, fn in SOUNDS:
        x = fn()
        x = x - np.mean(x)
        x = import_sounds.fit(key, x, SR)
        name = 'oa_' + key
        save(name, x)
        lines.append('%s %s' % (key, name))
        print('  %-18s %5.2fs  %5.1f dB' % (key, len(x) / SR, import_sounds.rms_db(x)))
    with open(os.path.join(OUT, 'events.txt'), 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print('wrote %d sounds -> %s' % (len(lines), OUT))


if __name__ == '__main__':
    main()
