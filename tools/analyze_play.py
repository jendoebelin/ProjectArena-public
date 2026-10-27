#!/usr/bin/env python3
"""Reads RECORD MY MATCHES recordings (app/src/record.h has the file's layout) and measures how the player plays, to
tune the practice bot from real play (game.h: BotSkill, the levels in game.cpp).

Usage: python -I tools/analyze_play.py FILE_OR_FOLDER... [--json OUT.json] [--check]

  FILE_OR_FOLDER  .par recordings, or folders of them (copied off a headset: files/recordings)
  --json          also writes the numbers as JSON
  --check         reads the files and checks they are whole recordings (the tests use it); prints a line, exit 1 if not

Only the recording player's own motion is measured (record.h keeps no other person's). For each ball coming at them:
how soon they move (reaction), what they do (shield, punch, dodge, nothing) and how it ends; their hand and body speeds,
reach, leans and ducks; their throws (speed, spin, bank shots). The summary ends with the player's level from their
results against the bot (each match: the bot's level, up or down by how the points went), the level their speeds alone
are nearest (a skilled player can move little: results are the better guide), and their numbers as a BotSkill row.
"""
import glob
import json
import math
import os
import struct
import sys

# game.h enums
EV = ['Spawn', 'Grab', 'Throw', 'ThrowFail', 'Bounce', 'ShieldBlock', 'ShieldBreak', 'HitPlayer', 'Returned', 'Caught',
      'Foul', 'RoundStart', 'RoundEnd', 'MatchEnd', 'Countdown', 'KnuckleBlock', 'Recharge', 'Strike', 'Overtime',
      'SpawnRise', 'FistBump']
FLYING, RETURNING = 3, 5
IN_PROGRESS = 2
# the bot's levels (game.cpp kBotSkills): reaction s, hand m/s, body m/s, throw hand m/s
LEVELS = [('ROOKIE', 0.45, 2.5, 1.0, 4.2), ('AMATEUR', 0.35, 3.0, 1.4, 4.8), ('PRO', 0.25, 3.8, 1.8, 5.6),
          ('ALL-STAR', 0.20, 4.5, 2.2, 6.0), ('CHAMPION', 0.17, 5.2, 2.6, 6.3), ('UNBEATABLE', 0.15, 6.0, 3.0, 6.5)]
MAX_INPUT_SPEED, MAX_BALL_SPEED = 6.5, 14.0   # game.h: hand speed 6.5 m/s -> a 14 m/s ball


class Reader:
    def __init__(self, data):
        self.d, self.i = data, 0

    def take(self, fmt):
        n = struct.calcsize(fmt)
        if self.i + n > len(self.d):
            raise EOFError
        v = struct.unpack_from(fmt, self.d, self.i)
        self.i += n
        return v

    def vec(self):
        return self.take('<3f')

    def pose(self):
        x, y, z, qx, qy, qz, qw = self.take('<7f')
        return (x, y, z), (qx, qy, qz, qw)


def load(path):
    """the recording as a header dict and a list of steps (each: time, dt, match state, scores, players, balls, events)"""
    data = open(path, 'rb').read()
    if data[:6] != b'PAREC1':
        raise ValueError('not a recording')
    r = Reader(data)
    r.i = 6
    local, opp_bot, level, adapt, match_level = r.take('<BBBBf')
    advanced, shields, knuckles, _ = r.take('<BBbB')
    match_time, round_time = r.take('<2f')
    version = r.take('<16s')[0].split(b'\0')[0].decode('ascii', 'replace')
    head = dict(local=local, opponent_bot=bool(opp_bot), bot_level=level, match_my_level=bool(adapt),
                its_level=match_level, advanced=bool(advanced), shields=bool(shields), knuckle_mode=knuckles,
                match_time=match_time, round_time=round_time, version=version)
    steps, ended = [], False
    try:
        while r.i < len(data):
            tag = r.take('<B')[0]
            if tag == ord('F'):
                t, dt, match, s0, s1 = r.take('<2f3B')
                players = []
                for _ in range(2):
                    if r.take('<B')[0]:
                        hd = r.pose()
                        hands = []
                        for _ in range(2):
                            g = r.pose()
                            trig, sq, tracked = r.take('<2fB')
                            hands.append(dict(pos=g[0], q=g[1], trigger=trig, squeeze=sq, tracked=bool(tracked)))
                        players.append(dict(head=hd[0], hq=hd[1], hands=hands))
                    else:
                        players.append(None)
                balls = []
                for _ in range(2):
                    state, inst = r.take('<Bb')
                    pos, vel, spin = r.vec(), r.vec(), r.vec()
                    scale, charged, held = r.take('<fBb')
                    balls.append(dict(state=state, by=inst, pos=pos, vel=vel, spin=spin, scale=scale, held=held))
                steps.append(dict(t=t, dt=dt, match=match, score=(s0, s1), players=players, balls=balls, events=[]))
            elif tag == ord('E'):
                typ, player, hand, ball = r.take('<B3b')
                pos = r.vec()
                value = r.take('<f')[0]
                if steps:
                    steps[-1]['events'].append(dict(type=EV[typ] if typ < len(EV) else '?', player=player, hand=hand,
                                                    ball=ball, pos=pos, value=value))
            elif tag == ord('X'):
                r.take('<f')
                ended = True
                break
            else:
                raise ValueError('unknown record %r at %d' % (tag, r.i))
    except EOFError:
        pass   # (cut short: the game stopped mid-step; what came before still counts)
    head['whole'] = ended
    return head, steps


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def length(v):
    return math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])


def pct(xs, p):
    if not xs:
        return None
    xs = sorted(xs)
    k = (len(xs) - 1) * p / 100.0
    lo = int(math.floor(k))
    hi = min(lo + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def analyze(head, steps):
    """the player's numbers from one recording"""
    L, O = head['local'], 1 - head['local']
    out = dict(steps=len(steps), seconds=sum(s['dt'] for s in steps), throws=[], incoming=[], head_speeds=[],
               hand_speeds=[], reach=[], lean=[], duck=[], catches=0, points=0, points_against=0)
    me = [s['players'][L] for s in steps]
    # where they stand: the median head position while a match is on (leans and ducks are from there)
    live = [i for i, s in enumerate(steps) if s['match'] == IN_PROGRESS and me[i]]
    if not live:
        return out
    hx = sorted(me[i]['head'][0] for i in live)[len(live) // 2]
    hy = sorted(me[i]['head'][1] for i in live)[len(live) // 2]
    hz = sorted(me[i]['head'][2] for i in live)[len(live) // 2]
    # speeds, step to step (head; each hand), reach (head to hand), leans and ducks
    for i in live:
        if i == 0 or not me[i - 1] or steps[i]['dt'] <= 0:
            continue
        dt = steps[i]['dt']
        out['head_speeds'].append(length(sub(me[i]['head'], me[i - 1]['head'])) / dt)
        for h in range(2):
            if me[i]['hands'][h]['tracked'] and me[i - 1]['hands'][h]['tracked']:
                out['hand_speeds'].append(length(sub(me[i]['hands'][h]['pos'], me[i - 1]['hands'][h]['pos'])) / dt)
                out['reach'].append(length(sub(me[i]['hands'][h]['pos'], me[i]['head'])))
        out['lean'].append(abs(me[i]['head'][2] - hz))
        out['duck'].append(max(0.0, hy - me[i]['head'][1]))
    # events
    for i, s in enumerate(steps):
        for e in s['events']:
            if e['type'] == 'Throw' and e['player'] == L:
                b = s['balls'][L]
                # the hand's top speed in the moment before (the throw's own swing)
                peak = 0.0
                for j in range(max(1, i - 30), i + 1):
                    if me[j] and me[j - 1] and steps[j]['dt'] > 0:
                        hand = e['hand'] if e['hand'] in (0, 1) else 1
                        peak = max(peak, length(sub(me[j]['hands'][hand]['pos'], me[j - 1]['hands'][hand]['pos'])) / steps[j]['dt'])
                # a bank shot: off a side wall before it reaches the far end
                bank = False
                for k in range(i + 1, min(len(steps), i + 200)):
                    bb = steps[k]['balls'][L]
                    if bb['state'] != FLYING or bb['by'] != L:
                        break
                    if any(x['type'] == 'Bounce' and x['player'] == L and abs(x['pos'][2]) > 1.5 for x in steps[k]['events']):
                        bank = True
                        break
                out['throws'].append(dict(speed=e['value'], hand_peak=peak, spin=length(b['spin']), bank=bank))
            elif e['type'] == 'Caught' and e['player'] == L:
                out['catches'] += 1
            elif e['type'] == 'HitPlayer':
                if e['player'] == L:
                    out['points_against'] += 1
                elif e['player'] == O:
                    out['points'] += 1
    # each ball the opponent threw that came at them: reaction, what they did, how it ended
    for i, s in enumerate(steps):
        for e in s['events']:
            if e['type'] != 'Throw' or e['player'] != O or not me[i]:
                continue
            t0, closest, end, react, moved = s['t'], 9.0, None, None, 0.0
            start = me[i]['head']
            for k in range(i + 1, min(len(steps), i + 400)):
                st = steps[k]
                b = st['balls'][O]
                p = me[k]
                if not p:
                    continue
                if b['state'] == FLYING and b['by'] == O:
                    closest = min(closest, length(sub(b['pos'], p['head'])))
                    moved = max(moved, length(sub(p['head'], start)))
                # the first real move after the throw: the head or a hand off at speed, two steps running
                if react is None and k >= i + 2 and me[k - 1] and me[k - 2] and st['dt'] > 0:
                    def moving(a, b_, c, dt):
                        if length(sub(a['head'], b_['head'])) / dt > 0.6 or length(sub(b_['head'], c['head'])) / dt > 0.6:
                            return length(sub(a['head'], b_['head'])) / dt > 0.6 and length(sub(b_['head'], c['head'])) / dt > 0.6
                        return any(length(sub(a['hands'][h]['pos'], b_['hands'][h]['pos'])) / dt > 1.5 and
                                   length(sub(b_['hands'][h]['pos'], c['hands'][h]['pos'])) / dt > 1.5 for h in range(2))
                    if moving(p, me[k - 1], me[k - 2], st['dt']):
                        react = st['t'] - t0
                for x in st['events']:
                    if x['type'] == 'HitPlayer' and x['player'] == L and x['ball'] == O:
                        end = 'hit'
                    elif x['type'] == 'ShieldBlock' and x['player'] == L and x['ball'] == O:
                        end = 'shield'
                    elif x['type'] == 'KnuckleBlock' and x['player'] == L and x['ball'] == O:
                        end = 'punch'
                if end or b['state'] != FLYING or b['by'] != O:
                    break
            if closest > 1.2 and end is None:
                continue   # (it never came near them)
            if end is None:
                end = 'dodged' if closest < 0.6 and moved > 0.15 else 'missed'   # (a dodge: they moved out of its way)
            out['incoming'].append(dict(speed=e['value'], reaction=react, closest=closest, end=end))
    return out


def summary(results):
    throws = [t for r in results for t in r['throws']]
    inc = [b for r in results for b in r['incoming']]
    near = [b for b in inc if b['end'] != 'missed']
    reactions = [b['reaction'] for b in inc if b['reaction'] is not None and b['reaction'] < 1.0]
    heads = [v for r in results for v in r['head_speeds'] if v < 6]
    hands = [v for r in results for v in r['hand_speeds'] if v < 15]
    reach = [v for r in results for v in r['reach']]
    lean = [v for r in results for v in r['lean']]
    duck = [v for r in results for v in r['duck']]
    ends = {k: sum(1 for b in near if b['end'] == k) for k in ('hit', 'shield', 'punch', 'dodged')}
    n = max(1, len(near))
    # their level from their results: each match against the bot, its level moved by how the points went (winning by
    # as many as were scored is two levels up), weighted by the points in it
    est, wsum = 0.0, 0.0
    for r in results:
        h = r['head']
        if not h['opponent_bot']:
            continue
        level = h['its_level'] if h['match_my_level'] else float(min(h['bot_level'], 5))
        pf, pa = r['points'], r['points_against']
        w = pf + pa + 2
        est += w * max(0.0, min(5.0, level + max(-2.0, min(2.0, 2.0 * (pf - pa) / w))))
        wsum += w
    s = dict(recordings=len(results), minutes=sum(r['seconds'] for r in results) / 60.0,
             level_from_results=est / wsum if wsum else None,
             points=sum(r['points'] for r in results), points_against=sum(r['points_against'] for r in results),
             throws=len(throws), throw_speed_median=pct([t['speed'] for t in throws], 50),
             throw_speed_p90=pct([t['speed'] for t in throws], 90),
             throw_hand_peak_median=pct([t['hand_peak'] for t in throws], 50),
             curved_share=sum(1 for t in throws if t['spin'] > 8) / max(1, len(throws)),
             bank_share=sum(1 for t in throws if t['bank']) / max(1, len(throws)),
             balls_at_them=len(near), ends={k: v / n for k, v in ends.items()},
             defended_share=1.0 - ends['hit'] / n, reaction_median=pct(reactions, 50), reaction_p25=pct(reactions, 25),
             head_speed_p95=pct(heads, 95), hand_speed_p95=pct(hands, 95), reach_p99=pct(reach, 99),
             lean_p95=pct(lean, 95), duck_p95=pct(duck, 95), catches=sum(r['catches'] for r in results))
    # the nearest level: reaction, hand, body and throw speeds against the bot's (each scaled by its spread)
    if s['reaction_median'] and s['hand_speed_p95'] and s['head_speed_p95']:
        th = (s['throw_speed_median'] or 0) * MAX_INPUT_SPEED / MAX_BALL_SPEED
        def dist(l):
            return (abs(l[1] - s['reaction_median']) / 0.3 + abs(l[2] - s['hand_speed_p95']) / 3.5 +
                    abs(l[3] - s['head_speed_p95']) / 2.0 + abs(l[4] - th) / 2.7)
        s['nearest_level_by_speed'] = min(LEVELS, key=dist)[0]
        s['suggested_row'] = dict(reaction=round(s['reaction_median'], 3), defend=round(s['defended_share'], 2),
                                  handSpeed=round(s['hand_speed_p95'], 2), bodySpeed=round(s['head_speed_p95'], 2),
                                  throwHand=round(th, 2))
    return s


def fmt(v, unit='', k=2):
    return '-' if v is None else ('%.*f%s' % (k, v, unit))


def files(args):
    out = []
    for a in args:
        out += sorted(glob.glob(os.path.join(a, '*.par'))) if os.path.isdir(a) else [a]
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    js = sys.argv[sys.argv.index('--json') + 1] if '--json' in sys.argv else None
    if js in args:
        args.remove(js)
    paths = files(args)
    if not paths:
        print(__doc__)
        return 2
    if '--check' in sys.argv:
        bad = 0
        for p in paths:
            try:
                h, st = load(p)
                ok = h['whole'] and len(st) > 10
            except (ValueError, OSError):
                ok = False
            print('%s %s' % ('ok  ' if ok else 'BAD ', p))
            bad += not ok
        return 1 if bad else 0
    results = []
    for p in paths:
        h, st = load(p)
        r = analyze(h, st)
        r['file'], r['head'] = os.path.basename(p), h
        results.append(r)
        opp = ('the bot (%s%s)' % (LEVELS[min(h['bot_level'], 5)][0], ', MATCH MY LEVEL' if h['match_my_level'] else '')
               if h['opponent_bot'] else 'a person')
        print('%s: %.1f min against %s, %d-%d%s' % (r['file'], r['seconds'] / 60, opp, r['points'], r['points_against'],
                                                    '' if h['whole'] else ' (cut short)'))
    s = summary(results)
    print('\n%d recordings, %.1f minutes, points %d - %d' % (s['recordings'], s['minutes'], s['points'], s['points_against']))
    print('THROWS     %d: speed %s (top tenth %s), hand at its fastest %s; curved %d%%, off a wall %d%%' % (
        s['throws'], fmt(s['throw_speed_median'], ' m/s', 1), fmt(s['throw_speed_p90'], ' m/s', 1),
        fmt(s['throw_hand_peak_median'], ' m/s', 1), s['curved_share'] * 100, s['bank_share'] * 100))
    e = s['ends']
    print('DEFENDING  %d balls at them: shield %d%%, punch %d%%, dodged %d%%, hit %d%%; reaction %s (quickest quarter %s)' % (
        s['balls_at_them'], e['shield'] * 100, e['punch'] * 100, e['dodged'] * 100, e['hit'] * 100,
        fmt(s['reaction_median'], ' s'), fmt(s['reaction_p25'], ' s')))
    print('MOVING     hands up to %s, head up to %s; reach %s; lean %s, duck %s; catches %d' % (
        fmt(s['hand_speed_p95'], ' m/s', 1), fmt(s['head_speed_p95'], ' m/s', 1), fmt(s['reach_p99'], ' m'),
        fmt(s['lean_p95'], ' m'), fmt(s['duck_p95'], ' m'), s['catches']))
    if s['level_from_results'] is not None:
        lv = s['level_from_results']
        lo, hi = LEVELS[min(4, int(lv))][0], LEVELS[min(5, int(lv) + 1)][0]
        print('THEIR LEVEL  %.1f from their results against the bot (%s)' % (
            lv, LEVELS[int(round(lv))][0] if abs(lv - round(lv)) < 0.2 else 'between %s and %s' % (lo, hi)))
    if 'nearest_level_by_speed' in s:
        print('SPEEDS ALONE  nearest %s (a skilled player can move little);  as a BotSkill row: %s' % (
            s['nearest_level_by_speed'], json.dumps(s['suggested_row'])))
    if js:
        with open(js, 'w') as f:
            json.dump(dict(summary=s, recordings=[{k: v for k, v in r.items() if k in ('file', 'head', 'points', 'points_against', 'seconds')}
                                                  for r in results]), f, indent=1)
        print('wrote %s' % js)
    return 0


if __name__ == '__main__':
    sys.exit(main())
