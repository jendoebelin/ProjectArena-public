"""TRAINING's tutorial videos: a recording of a lesson (RECORD MY MATCHES) made into the video SHOW ME HOW plays.

Usage: python -I tools/make_tutorial.py RECORDING.par LESSON_ID [--seconds N] [--skip S]
Writes app/assets_open/tutorials/lesson_<LESSON_ID>.par: from the lesson's GO (its first RoundStart) to the end of the
run (or N seconds after GO), every second step kept (36 a second: the game replays them smoothly), the events of the
steps left out moved onto the next one kept, and the lesson's id in the header. --skip S starts S seconds after GO.
Recordings from 0.3.12 on are one lesson each and name it (the header's lesson byte); older ones need --seconds.
The recording holds only the player's head and hand movement, the balls and the game's events: nothing else.
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = 6 + 4 + 4 + 4 + 4 + 4 + 16   # "PAREC1", u8 x4, f32, u8 x4, f32 x2, char[16]
LESSON_AT = 6 + 4 + 4 + 3              # the lesson byte (0.3.12; 0 before)
ROUND_START = 11                       # EventType::RoundStart


def frame_size(data, i):
    """the length of the 'F' record at i (the tag included)"""
    n = 1 + 4 + 4 + 1 + 2
    for _ in range(2):
        kept = data[i + n]
        n += 1
        if kept:
            n += 28 + 2 * (28 + 4 + 4 + 1)
    n += 2 * (1 + 1 + 12 * 3 + 4 + 1 + 1)
    return n


def main():
    args = [a for a in sys.argv[1:]]
    if len(args) < 2:
        print(__doc__)
        return 2
    src, lesson = args[0], int(args[1])
    seconds = float(args[args.index('--seconds') + 1]) if '--seconds' in args else None
    skip = float(args[args.index('--skip') + 1]) if '--skip' in args else 0.0
    data = open(src, 'rb').read()
    if data[:6] != b'PAREC1':
        raise SystemExit('not a recording: ' + src)
    head = bytearray(data[:HEADER])
    head[LESSON_AT] = lesson
    # the records: [(time, frame bytes, [event bytes])]
    steps, i = [], HEADER
    while i < len(data):
        tag = data[i]
        if tag == ord('F'):
            n = frame_size(data, i)
            if i + n > len(data):
                break
            steps.append([struct.unpack_from('<f', data, i + 1)[0], data[i:i + n], []])
            i += n
        elif tag == ord('E'):
            if i + 21 > len(data) or not steps:
                break
            steps[-1][2].append(data[i:i + 21])
            i += 21
        else:
            break
    go = next((k for k, s in enumerate(steps) if any(e[1] == ROUND_START for e in s[2])), None)
    if go is None:
        raise SystemExit('no GO (RoundStart) in ' + src)
    t0 = steps[go][0] + skip
    first = next(k for k in range(go, len(steps)) if steps[k][0] >= t0)
    out, pending, kept = bytearray(head), [], 0
    for k in range(first, len(steps)):
        t, frame, events = steps[k]
        if seconds is not None and t - t0 > seconds:
            break
        pending += events
        if (k - first) % 2 and k + 1 < len(steps):
            continue   # (its events go with the next step kept)
        out += frame
        for e in pending:
            out += e
        pending = []
        kept += 1
    out += b'X' + struct.pack('<f', steps[min(len(steps) - 1, k)][0])
    dst = os.path.join(ROOT, 'app', 'assets_open', 'tutorials', 'lesson_%d.par' % lesson)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, 'wb').write(out)
    print('%s: %d steps, %.1f s, %d KB' % (dst, kept, steps[min(len(steps) - 1, k)][0] - t0, len(out) // 1024))
    return 0


if __name__ == '__main__':
    sys.exit(main())
