"""Bring generated sounds (ElevenLabs sound effects, Suno music) into the game. See docs/SOUND_BRIEF.md.

Put the downloads in one folder, each named after its slot: Grab.wav, ThrowFast.wav, MatchEndWin.wav ... (any case;
a suffix after the slot name is fine: "Grab_v3.wav"). WAV only (16/24/32-bit PCM or 32-bit float, any rate, mono or
stereo): download WAV from ElevenLabs / Suno, or convert MP3s first.

Each file is made to fit its slot: mixed to mono, resampled to 48 kHz, the silence before the sound trimmed, cut to
the slot's longest length with a short fade, levelled to the slot's loudness (quiet for frequent sounds such as
Bounce), and the ambience loop gets a seamless crossfade. Music (Suno) usually needs a start and end: put
"MatchEndWin 12.5 20.0" (seconds) lines in trim.txt in the same folder.

Writes app/assets_open/audio/oa_<Slot>.wav and events.txt. The synthesized set stays reproducible with
tools/gen_open_audio.py (run that to go back). --dry-run shows what would change.
Usage: python -I tools/import_sounds.py FOLDER [--dry-run]
"""
import os
import struct
import sys
import wave

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'app', 'assets_open', 'audio')
SR = 48000

# slot: (longest seconds, loudness dBFS RMS of the loud part, loop). Keep in step with docs/SOUND_BRIEF.md.
SLOTS = {
    'Spawn': (1.0, -30, False), 'SpawnRise': (1.6, -30, False), 'Grab': (0.6, -28, False), 'Caught': (0.8, -27, False),
    'ThrowSlow': (0.9, -29, False), 'ThrowMed': (0.9, -27, False), 'ThrowFast': (1.0, -25, False),
    'ThrowFull': (1.1, -23, False), 'ThrowFail': (1.0, -30, False), 'Bounce': (0.5, -32, False),
    'ShieldBlock': (1.2, -24, False), 'ShieldBreak': (1.4, -23, False), 'KnuckleBlock': (0.7, -25, False),
    'HitPlayerOpponent': (1.6, -22, False), 'HitPlayerSelf': (1.4, -23, False), 'Returned': (1.4, -28, False),
    'Foul': (1.2, -25, False), 'Countdown': (0.6, -27, False), 'RoundStart': (1.8, -23, False),
    'RoundEndWin': (4.5, -22, False), 'RoundEndLose': (4.0, -24, False), 'MatchEndWin': (8.0, -20, False),
    'MatchEndLose': (7.0, -23, False), 'Recharge': (1.2, -27, False), 'Strike': (1.6, -24, False),
    'Overtime': (2.5, -22, False), 'FistBump': (0.9, -24, False), 'Ambience': (40.0, -34, True),
}


def read_wav(path):
    """mono float32 at its own rate; PCM 16/24/32 or IEEE float 32 (wave cannot read float or 24-bit everywhere)"""
    b = open(path, 'rb').read()
    if b[:4] != b'RIFF' or b[8:12] != b'WAVE':
        raise ValueError('not a WAV file (convert MP3 to WAV first)')
    p, fmt, data = 12, None, None
    while p + 8 <= len(b):
        cid, size = b[p:p + 4], struct.unpack_from('<I', b, p + 4)[0]
        body = b[p + 8:p + 8 + size]
        if cid == b'fmt ':
            fmt = struct.unpack_from('<HHIIHH', body)
            if fmt[0] == 0xFFFE and len(body) >= 26:   # WAVE_FORMAT_EXTENSIBLE: the real format in the sub-format GUID
                fmt = (struct.unpack_from('<H', body, 24)[0],) + fmt[1:]
        elif cid == b'data':
            data = body
        p += 8 + size + (size & 1)
    if not fmt or data is None:
        raise ValueError('no fmt / data chunk')
    tag, ch, rate, _, _, bits = fmt
    if tag == 3 and bits == 32:
        x = np.frombuffer(data, '<f4').astype(np.float64)
    elif tag == 1 and bits == 16:
        x = np.frombuffer(data, '<i2') / 32768.0
    elif tag == 1 and bits == 24:
        u = np.frombuffer(data[:len(data) // 3 * 3], np.uint8).reshape(-1, 3).astype(np.int32)
        v = u[:, 0] | (u[:, 1] << 8) | (u[:, 2] << 16)
        x = np.where(v >= 1 << 23, v - (1 << 24), v) / float(1 << 23)
    elif tag == 1 and bits == 32:
        x = np.frombuffer(data, '<i4') / 2147483648.0
    else:
        raise ValueError('unsupported WAV format (tag %d, %d bit)' % (tag, bits))
    x = x[:len(x) // ch * ch].reshape(-1, ch).mean(1)
    return x, rate


def resample(x, rate):
    if rate == SR:
        return x
    n = int(round(len(x) * SR / rate))
    X = np.fft.rfft(x)
    Y = np.zeros(n // 2 + 1, complex)
    k = min(len(X), len(Y))
    Y[:k] = X[:k]
    return np.fft.irfft(Y, n) * (n / len(x))


def rms_db(x):
    return 20 * np.log10(np.sqrt(np.mean(x * x)) + 1e-12)


def fit(slot, x, rate, trim=None):
    longest, level, loop = SLOTS[slot]
    if trim:
        a, b = int(trim[0] * rate), int(trim[1] * rate) if trim[1] > 0 else len(x)
        x = x[a:b]
    x = resample(x - np.mean(x), rate)
    # the sound starts at once: trim leading silence (-50 dB of the peak), keep 4 ms
    if not loop:
        peak = np.max(np.abs(x)) + 1e-12
        on = np.nonzero(np.abs(x) > peak * 10 ** (-50 / 20))[0]
        if len(on):
            x = x[max(0, on[0] - int(0.004 * SR)):]
        # and ends where it has faded (-60 dB), at most `longest`
        on = np.nonzero(np.abs(x) > peak * 10 ** (-60 / 20))[0]
        if len(on):
            x = x[:on[-1] + int(0.02 * SR)]
    x = x[:int(longest * SR)]
    if loop:   # seamless: crossfade the last 2 s into the start
        f = int(2.0 * SR)
        if len(x) > 3 * f:
            w = np.linspace(0, 1, f)
            x = np.concatenate([x[f:-f], x[-f:] * (1 - w) + x[:f] * w])
    else:
        f = min(int(0.04 * SR), len(x) // 4)
        x[-f:] *= np.linspace(1, 0, f) ** 2
        x[:int(0.002 * SR)] *= np.linspace(0, 1, int(0.002 * SR))
    # loudness of the loud part (the top 30% of 20 ms windows), then a ceiling at -1 dBFS
    w = int(0.02 * SR)
    frames = x[:len(x) // w * w].reshape(-1, w) if len(x) >= w else x[None, :]
    e = np.sqrt((frames ** 2).mean(1))
    loud = frames[e >= np.quantile(e, 0.7)] if len(e) > 3 else frames
    x = x * 10 ** ((level - rms_db(loud.ravel())) / 20)
    peak = np.max(np.abs(x))
    if peak > 0.89:
        x = np.tanh(x / 0.89 * 1.2) / np.tanh(1.2) * 0.89
    return x


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    dry = '--dry-run' in sys.argv
    if not args:
        sys.exit(__doc__)
    src = args[0]
    trims = {}
    tp = os.path.join(src, 'trim.txt')
    if os.path.exists(tp):
        for line in open(tp, encoding='utf-8'):
            f = line.split('#')[0].split()
            if len(f) == 3:
                trims[f[0].lower()] = (float(f[1]), float(f[2]))
    by_lower = {s.lower(): s for s in SLOTS}
    done = {}
    for fn in sorted(os.listdir(src)):
        if not fn.lower().endswith('.wav'):
            continue
        stem = os.path.splitext(fn)[0].lower()
        slot = by_lower.get(stem) or next((by_lower[s] for s in sorted(by_lower, key=len, reverse=True)
                                           if stem.startswith(s) and not stem[len(s):len(s) + 1].isalnum()), None)
        if not slot:
            print('skip %s (no slot of that name)' % fn)
            continue
        if slot in done:
            print('skip %s (%s already taken by %s)' % (fn, slot, done[slot]))
            continue
        try:
            x, rate = read_wav(os.path.join(src, fn))
        except ValueError as e:
            print('skip %s: %s' % (fn, e))
            continue
        y = fit(slot, x, rate, trims.get(slot.lower()))
        done[slot] = fn
        print('%-18s <- %-28s %5.2f s -> %5.2f s, %5.1f dB' % (slot, fn, len(x) / rate, len(y) / SR, rms_db(y)))
        if not dry:
            with wave.open(os.path.join(OUT, 'oa_%s.wav' % slot), 'wb') as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(SR)
                w.writeframes((np.clip(y, -1, 1) * 32767).astype('<i2').tobytes())
    if not dry and done:
        have = [s for s in SLOTS if os.path.exists(os.path.join(OUT, 'oa_%s.wav' % s))]
        with open(os.path.join(OUT, 'events.txt'), 'w', newline='\n') as f:
            for s in have:
                f.write('%s oa_%s\n' % (s, s))
    missing = [s for s in SLOTS if s not in done]
    print('%d imported%s; not in this folder: %s' % (len(done), ' (dry run)' if dry else '', ', '.join(missing) or 'none'))


if __name__ == '__main__':
    main()
