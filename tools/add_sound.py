"""SOUND STUDIO: gets a proposed sound ready for the library on the community server (server/projectarena_relay.py
--sounds). Converts it to the game's format (WAV, mono, 48 kHz, 16-bit), trims the silence before and after, makes it
as loud as the game's own sound for that event, checks its length and name, and writes build/sound_library/
Event__name.wav. Then prints the two commands that put it on the server (the owner runs them; nothing here writes to
the server). The relay lists it within seconds; the studio downloads it the next time someone opens it.
Usage: python -I tools/add_sound.py FILE SLOT "name"     e.g.  tools/add_sound.py glass.wav S12 "glass crack"
FILE: WAV (any rate, channels, 16/24/32-bit or float); OGG, MP3, FLAC and others through third_party/vgmstream.
SLOT: the asset catalog's code (S01-S33, tools/gen_catalog.py SOUNDS) or the game's event name (ShieldBreak).
name: letters, digits and spaces (24 at most).
The server: sounds.local.cfg in the project root (gitignored), lines ssh=user@host and key=path-to-ssh-key.
Only with the maker's agreement; note what a sound is made from and under which terms (other licences than the open
assets' CC BY 4.0 are possible: listed in NOTICE)."""
import os
import re
import subprocess
import sys
import tempfile
import wave

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AUDIO = os.path.join(ROOT, 'app', 'assets_open', 'audio')
OUT = os.path.join(ROOT, 'build', 'sound_library')
VGM = os.path.join(ROOT, 'third_party', 'vgmstream', 'vgmstream-cli.exe')
SR = 48000
LONG = ('Ambience', 'Crowd', 'Cheer', 'MatchEndWin', 'MatchEndLose')   # (loops and fanfares may run longer)
MAX_SHORT, MAX_LONG = 3.0, 12.0


def events():
    with open(os.path.join(AUDIO, 'events.txt'), encoding='utf-8') as f:
        return [ln.split()[0] for ln in f if ln.strip()]


def slots():
    """The asset catalog's sound codes: code -> event (tools/gen_catalog.py SOUNDS, the one list of them)."""
    with open(os.path.join(ROOT, 'tools', 'gen_catalog.py'), encoding='utf-8') as f:
        return dict(re.findall(r"\('(S\d+)', '([A-Za-z]+)'", f.read()))


def read_wav(path):
    """Mono float samples and the rate, from a PCM (8/16/24/32-bit) or float WAV."""
    with open(path, 'rb') as f:
        d = f.read()
    if d[:4] != b'RIFF' or d[8:12] != b'WAVE':
        raise ValueError('not a WAV file')
    at, fmt, data = 12, None, None
    while at + 8 <= len(d):
        cid, size = d[at:at + 4], int.from_bytes(d[at + 4:at + 8], 'little')
        body = d[at + 8:at + 8 + size]
        if cid == b'fmt ':
            tag, ch, rate = int.from_bytes(body[0:2], 'little'), int.from_bytes(body[2:4], 'little'), int.from_bytes(body[4:8], 'little')
            bits = int.from_bytes(body[14:16], 'little')
            if tag == 0xFFFE and len(body) >= 26:   # (WAVE_FORMAT_EXTENSIBLE: the real tag in its sub-format)
                tag = int.from_bytes(body[24:26], 'little')
            fmt = (tag, ch, rate, bits)
        elif cid == b'data':
            data = body
        at += 8 + size + (size & 1)
    if fmt is None or data is None:
        raise ValueError('a WAV without its format or data')
    tag, ch, rate, bits = fmt
    if tag == 3 and bits == 32:
        x = np.frombuffer(data, '<f4').astype(np.float64)
    elif tag == 3 and bits == 64:
        x = np.frombuffer(data, '<f8')
    elif tag == 1 and bits == 8:
        x = (np.frombuffer(data, np.uint8).astype(np.float64) - 128) / 128
    elif tag == 1 and bits == 16:
        x = np.frombuffer(data, '<i2') / 32768.0
    elif tag == 1 and bits == 24:
        b = np.frombuffer(data[:len(data) // 3 * 3], np.uint8).reshape(-1, 3).astype(np.int32)
        x = ((b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)) << 8 >> 8) / 8388608.0
    elif tag == 1 and bits == 32:
        x = np.frombuffer(data, '<i4') / 2147483648.0
    else:
        raise ValueError('an unusual WAV (format %d, %d-bit): export it as 16-bit PCM' % (tag, bits))
    x = x[:len(x) // ch * ch].reshape(-1, ch).mean(axis=1)
    return x, rate


def decode(path):
    if path.lower().endswith('.wav'):
        return read_wav(path)
    if not os.path.exists(VGM):
        raise ValueError('only WAV here (third_party/vgmstream is missing for other formats)')
    out = os.path.join(tempfile.mkdtemp(), 'decoded.wav')
    r = subprocess.run([VGM, '-o', out, path], capture_output=True, text=True)
    if r.returncode or not os.path.exists(out):
        raise ValueError('could not decode it: ' + (r.stderr or r.stdout)[-200:])
    return read_wav(out)


def resample(x, rate):
    if rate == SR:
        return x
    n = int(round(len(x) * SR / rate))
    return np.interp(np.arange(n) * (rate / SR), np.arange(len(x)), x)


def rms(x):
    return float(np.sqrt(np.mean(x * x))) if len(x) else 0.0


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    src, ev, name = sys.argv[1], sys.argv[2], sys.argv[3]
    evs, codes = events(), slots()
    code = ev.upper() if re.fullmatch(r'[sS]\d{1,2}', ev) else ''
    if code:   # a code (S12, or S2 for S02): its event
        code = 'S%02d' % int(code[1:])
        if code not in codes:
            print('no such sound slot: %s (the codes go from S01 to S%02d)' % (code, len(codes)))
            return 1
        ev = codes[code]
    if ev not in evs:
        print('no such event: %s\nthe slots: %s' % (ev, ' '.join('%s %s' % kv for kv in sorted(codes.items()))))
        return 1
    code = code or next((c for c, e in codes.items() if e == ev), '')
    slug = re.sub(r'[^a-z0-9]+', '_', name.strip().lower()).strip('_')
    if not slug or len(slug) > 24 or not re.fullmatch(r'[a-z0-9 ]+', name.strip().lower()):
        print('the name: letters, digits and spaces, 24 at most (it shows in the studio in capitals): %r' % name)
        return 1
    fn = '%s__%s.wav' % (ev, slug)
    dst = os.path.join(OUT, fn)
    if os.path.exists(dst):
        print('there is already a %s (build/sound_library): choose another name' % fn)
        return 1
    try:
        x, rate = decode(src)
    except (OSError, ValueError) as e:
        print('can\'t use it: %s' % e)
        return 1
    x = resample(x, rate)
    # trim: from 5 ms before the first sound above -50 dB to 50 ms after the last (with short fades)
    loud = np.nonzero(np.abs(x) > 10 ** (-50 / 20) * max(1e-9, np.max(np.abs(x))))[0]
    if not len(loud):
        print('it is silent')
        return 1
    a, b = max(0, loud[0] - SR // 200), min(len(x), loud[-1] + SR // 20)
    x = x[a:b].copy()
    fade = min(len(x) // 4, SR // 200)
    if fade:
        x[:fade] *= np.linspace(0, 1, fade)
        x[-fade:] *= np.linspace(1, 0, fade)
    secs = len(x) / SR
    limit = MAX_LONG if ev in LONG else MAX_SHORT
    if secs > limit:
        print('it is %.1f s long: a %s sound may be %.0f s at most' % (secs, ev, limit))
        return 1
    # as loud as the game's own sound for this event (its RMS), the peaks kept under -1 dBFS
    ref, _ = read_wav(os.path.join(AUDIO, 'oa_%s.wav' % ev))
    gain = rms(ref) / max(1e-9, rms(x))
    x *= gain
    peak = np.max(np.abs(x))
    if peak > 10 ** (-1 / 20):
        x *= 10 ** (-1 / 20) / peak
    os.makedirs(OUT, exist_ok=True)
    w = wave.open(dst, 'wb')
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
    w.writeframes((np.clip(x, -1, 1) * 32767).astype('<i2').tobytes())
    w.close()
    print('%s (%s): %.2f s, as loud as the game\'s own %s (RMS %.3f), %d KB\n  %s' % (fn, code, secs, ev, rms(ref), os.path.getsize(dst) // 1024, dst))
    cfg = {}
    p = os.path.join(ROOT, 'sounds.local.cfg')
    if os.path.exists(p):
        for ln in open(p, encoding='utf-8'):
            k, _, v = ln.strip().partition('=')
            if k and v:
                cfg[k.strip()] = v.strip()
    ssh, key = cfg.get('ssh', 'USER@SERVER'), cfg.get('key', '~/.ssh/YOUR_KEY')
    local = dst.replace(os.sep, '/')
    print('\nTo put it in the library (Git Bash):')
    print('  scp -i %s "%s" %s:/tmp/' % (key, local, ssh))
    print('  ssh -i %s %s "sudo install -o projectarena -g projectarena -m 0644 /tmp/%s /opt/projectarena/sound_library/ && rm /tmp/%s"'
          % (key, ssh, fn, fn))
    return 0


if __name__ == '__main__':
    sys.exit(main())
