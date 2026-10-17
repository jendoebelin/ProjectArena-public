#!/usr/bin/env python3
"""Tests the relay's moderation tools on a local copy (127.0.0.1:47791): the ban list (by name and by address,
re-read while running) and the reported player's address added to player reports.
Run: python -I server/test_relay_moderation.py"""
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = 47791
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


def hello(sock, name):
    sock.sendto(struct.pack('<IB', MAGIC, 1) + bytes(16) + bytes([8]) + name.encode().ljust(16, b'\0'), ('127.0.0.1', PORT))
    try:
        data, _ = sock.recvfrom(2048)
    except socket.timeout:
        return 'NO ANSWER'
    t = data[4]
    if t == 2:
        return 'WELCOME'
    if t == 7:
        return data[5:].split(b'\0')[0].decode()
    return 'TYPE %d' % t


def report(text):
    body = text.encode()
    with socket.create_connection(('127.0.0.1', PORT), 5) as s:
        s.sendall(b'PABUG1\n' + struct.pack('<I', len(body)) + body)
        return s.recv(64)


def main():
    work = tempfile.mkdtemp(prefix='pa_relay_test_')
    bans = os.path.join(work, 'bans.txt')
    reports = os.path.join(work, 'reports')
    with open(bans, 'w') as f:
        f.write('# test\nname:TROLL\n')
    relay = subprocess.Popen([sys.executable, '-I', os.path.join(HERE, 'projectarena_relay.py'), '--port', str(PORT),
                              '--bind', '127.0.0.1', '--reports', reports, '--bans', bans],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        time.sleep(1.5)
        a, b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM), socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        a.settimeout(2)
        b.settimeout(2)
        a.bind(('127.0.0.1', 0))
        check(hello(a, 'MOJO') == 'WELCOME', 'a player connects')
        check(hello(b, 'TROLL') == 'BANNED', 'a name on the ban list is refused')
        print(report('Project Arena player report\nplayer: SPARKY\nreported: MOJO (session 1234ABCD, WAITING)\nreason: OTHER\n'))
        files = [f for f in os.listdir(reports) if f.startswith('player_report_')]
        text = open(os.path.join(reports, files[0])).read() if files else ''
        port_a = a.getsockname()[1]
        check(('127.0.0.1:%d' % port_a) in text, "a player report gets the reported player's address (moderators only)")
        report('Project Arena bug report\nplayer: SPARKY\ncategory: OTHER\n')
        check(any(not f.startswith('player_report_') for f in os.listdir(reports)), 'bug reports are saved as before')
        time.sleep(1.1)   # a newer modification time
        with open(bans, 'a') as f:
            f.write('name:MOJO   # banned while connected\n')
        a.settimeout(5)
        try:
            data, _ = a.recvfrom(2048)
            got = data[5:].split(b'\0')[0].decode() if data[4] == 7 else 'TYPE %d' % data[4]
        except socket.timeout:
            got = 'NO ANSWER'
        check(got == 'BANNED', 'a player banned while connected is told and disconnected')
        a.settimeout(2)
        check(hello(a, 'MOJO') == 'BANNED', '... and refused when they try again')
        with open(bans, 'w') as f:
            f.write('127.0.0.1\n')
        time.sleep(2.5)
        check(hello(b, 'NEWNAME') == 'BANNED', 'an address ban covers any name')
    finally:
        relay.terminate()
        out = relay.communicate(timeout=5)[0].decode(errors='replace')
        print('\n'.join('  relay: ' + l for l in out.splitlines() if 'ban' in l or 'report' in l))
        shutil.rmtree(work, ignore_errors=True)
    print('\n%d passed, %d failed' % (passed, failed))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
