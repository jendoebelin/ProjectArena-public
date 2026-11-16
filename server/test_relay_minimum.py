#!/usr/bin/env python3
"""Tests the relay's minimum version (--minimum FILE) on a local copy (127.0.0.1:47793): no minimum lets every game on;
a game older than the minimum (or from before 0.3.11, which doesn't say its version) is told UPDATE TO ... OR NEWER
TO PLAY ONLINE and not let on; the file is re-read while running, and games already online that are older than a new
minimum are told and disconnected.
Run: python -I server/test_relay_minimum.py"""
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import projectarena_relay as R  # noqa: E402

PORT = 47793
MAGIC = 0x4C52414F
passed = failed = 0


def check(ok, msg):
    global passed, failed
    if ok:
        passed += 1
        print('  ok   ' + msg)
    else:
        failed += 1
        print('  FAIL ' + msg)


def answer(sock):
    try:
        data, _ = sock.recvfrom(2048)
    except socket.timeout:
        return 'NO ANSWER'
    if data[4] == 2:
        return 'WELCOME'
    if data[4] == 7:
        return data[5:].split(b'\0')[0].decode()
    return 'TYPE %d' % data[4]


def hello(sock, name, app):
    body = bytes([8]) + name.encode().ljust(16, b'\0')
    if app is not None:   # (games before 0.3.11 stop after the name)
        body += bytes([len(app)]) + app.encode()
    sock.sendto(struct.pack('<IB', MAGIC, 1) + bytes(16) + body, ('127.0.0.1', PORT))
    return answer(sock)


def sock():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(1.0)
    return s


def write(path, text):
    with open(path, 'w') as f:
        f.write(text)
    t = time.time() + 1   # (a new modification time even within the same second)
    os.utime(path, (t, t))


def main():
    # the comparisons themselves
    check(R.version_tuple('0.3.20') == (0, 3, 20) and R.version_tuple('') == (), 'versions read part by part')
    check(R.too_old('0.3.9', '0.3.20') and not R.too_old('0.3.20', '0.3.20') and not R.too_old('0.3.21', b'0.3.20'),
          '0.3.9 is older than 0.3.20; 0.3.20 and 0.3.21 are not')
    check(R.too_old('', '0.3.20'), 'a game from before 0.3.11 (no version) is older than any minimum')
    check(not R.too_old('', '') and not R.too_old('0.1.0', b''), 'no minimum: nobody is too old')
    check(not R.too_old('0.4.0', '0.3.20') and R.too_old('0.2.99', '0.3.0'), 'the minor number counts before the patch')
    m = R.update_message(b'0.3.20')
    check(m == 'UPDATE TO 0.3.20 OR NEWER TO PLAY ONLINE' and len(m) <= 48 and m == m.upper(),
          'the message fits the 48-letter notice: ' + m)
    check(len(R.update_message('1' * 15)) <= 48, 'a 15-letter version still fits')

    work = tempfile.mkdtemp(prefix='pa_relay_min_')
    minimum = os.path.join(work, 'minimum_version.txt')
    relay = subprocess.Popen([sys.executable, '-I', os.path.join(HERE, 'projectarena_relay.py'), '--port', str(PORT),
                              '--bind', '127.0.0.1', '--reports', os.path.join(work, 'reports'),
                              '--bans', os.path.join(work, 'bans.txt'), '--minimum', minimum,
                              '--latest', os.path.join(work, 'latest.txt'), '--stats', os.path.join(work, 'stats.db'),
                              '--sounds', os.path.join(work, 'sounds')],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        time.sleep(1.0)
        old, pre, new = sock(), sock(), sock()
        check(hello(old, 'OLDER', '0.3.19') == 'WELCOME', 'no minimum file: a 0.3.19 game is let on')
        check(hello(pre, 'ANCIENT', None) == 'WELCOME', 'no minimum file: a game from before 0.3.11 is let on')

        write(minimum, '0.3.20\n')
        time.sleep(2.6)   # (the relay looks every 2 s)
        check(answer(old) == 'UPDATE TO 0.3.20 OR NEWER TO PLAY ONLINE', 'a 0.3.19 game already online is told to update')
        check(answer(pre) == 'UPDATE TO 0.3.20 OR NEWER TO PLAY ONLINE', 'so is one from before 0.3.11')
        check(hello(old, 'OLDER', '0.3.19') == 'UPDATE TO 0.3.20 OR NEWER TO PLAY ONLINE', 'and it is not let back on')
        check(hello(old, 'OLDER', '0.3.19') == 'UPDATE TO 0.3.20 OR NEWER TO PLAY ONLINE', 'each hello is answered')
        check(hello(pre, 'ANCIENT', None) == 'UPDATE TO 0.3.20 OR NEWER TO PLAY ONLINE', 'nor the one before 0.3.11')
        check(hello(new, 'CURRENT', '0.3.20') == 'WELCOME', 'a 0.3.20 game is let on')
        check(hello(sock(), 'NEWER', '0.3.21') == 'WELCOME', 'and a newer one')

        write(minimum, '')
        time.sleep(2.6)
        check(hello(old, 'OLDER', '0.3.19') == 'WELCOME', 'the minimum emptied: the 0.3.19 game is let on again')
        check(answer(new) == 'NO ANSWER', 'a game on the minimum was never disconnected')
    finally:
        relay.terminate()
        out = relay.communicate(timeout=5)[0].decode(errors='replace')
        shutil.rmtree(work, ignore_errors=True)
    refusals = [l for l in out.splitlines() if 'refused' in l]
    check(any('refused 0.3.19: older than the minimum 0.3.20' in l for l in refusals), 'the log says what was refused')
    check(len([l for l in refusals if '0.3.19' in l]) == 1, 'a game saying hello again is logged once, not every second')
    check(any('older than the minimum 0.3.20' in l and 'leave' in l for l in out.splitlines()),
          'the disconnections are logged')
    check(any('minimum version: 0.3.20' in l for l in out.splitlines()) and
          any('minimum version: none' in l for l in out.splitlines()), 'the log says each new minimum')
    print('relay minimum version test: %d passed, %d failed' % (passed, failed))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
