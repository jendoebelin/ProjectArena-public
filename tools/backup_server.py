"""Backs up the community server's own data to this PC, encrypted for this Windows account (2026-11-20).

What the server keeps exists nowhere else: the players' careers and TRAINING boards (player_stats.db) and the salt
their keys are kept under (without it a restored copy can't be matched to anyone's key), the deletion notes (a
restored copy's deleted statistics go again at the relay's next start), the SOUND STUDIO library and
its votes, the bans and name words, the bug and player reports, the release and minimum version files, The Crowd's
settings and its notification key (crowd_push.db.vapid: fans' subscriptions are tied to it; not the devices' table,
which pages fill again as they are opened) and the relay's service file. Once a run (Windows Task Scheduler, daily), this:

  1. asks the server over SSH for one gzipped tar of all of it, made in memory by the relay's own account: the
     database read through SQLite's backup into memory (a consistent copy while the relay runs), the rest read as it
     is. Nothing is written on the server;
  2. checks it here: the tar opens, the database passes SQLite's integrity check and has the counts the server said;
  3. encrypts it with Windows DPAPI for the account running this (only that account, on this PC, can open it) and
     writes <out>/pa-server-YYYY-MM-DD_HHMMSS.bak, then reads it back to be sure;
  4. keeps the newest --keep (14) such files in <out> and deletes older ones;
  5. adds one line to <out>/backup_log.txt (never the server's address).

The server and key come from the repository's sounds.local.cfg (ssh=, key=; not in git), or --cfg.

  python -I tools/backup_server.py [--out DIR] [--keep N] [--cfg FILE]
  python -I tools/backup_server.py --check FILE        what a backup holds (the manifest and the counts)
  python -I tools/backup_server.py --restore FILE DIR  the files, decrypted, into DIR (to rebuild a server:
                                                        docs/SERVER_SETUP.md, "Restoring the server's data")
Windows only (DPAPI). Python 3.7 or newer."""
import argparse
import ctypes
import ctypes.wintypes
import datetime
import io
import json
import os
import shutil
import sqlite3
import subprocess
import sys
import tarfile
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUT = os.path.join(os.path.expanduser('~'), 'ProjectArena-server-backups')
MAGIC = b'PABK1\n'
ENTROPY = b'Project Arena community server backup, v1'

# Run on the server as the relay's account (python3 -I -, this text on stdin): one gzipped tar on stdout.
REMOTE = r'''
import hashlib, io, json, os, sqlite3, sys, tarfile, time
D = '/opt/projectarena'
FILES = ['player_stats.db.salt', 'player_stats.db.deletions', 'bans.txt', 'latest_version.txt', 'minimum_version.txt',
         'crowd.env', 'crowd_push.db.vapid']
DIRS = ['sound_library', 'bug_reports']
UNIT = '/etc/systemd/system/projectarena-relay.service'
buf = io.BytesIO()
man = {'made': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'files': {}}
def put(tar, name, data):
    ti = tarfile.TarInfo(name)
    ti.size, ti.mtime, ti.mode = len(data), int(time.time()), 0o600
    tar.addfile(ti, io.BytesIO(data))
    man['files'][name] = len(data)
with tarfile.open(fileobj=buf, mode='w:gz') as tar:
    src = sqlite3.connect('file:%s/player_stats.db?mode=ro' % D, uri=True)
    mem = sqlite3.connect(':memory:')
    src.backup(mem)
    src.close()
    man['tables'] = {t: mem.execute('SELECT COUNT(*) FROM "%s"' % t).fetchone()[0]
                     for (t,) in mem.execute("SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name")}
    put(tar, 'player_stats.db', mem.serialize())
    for f in FILES:
        p = os.path.join(D, f)
        if os.path.isfile(p):
            put(tar, f, open(p, 'rb').read())
    for d in DIRS:
        top = os.path.join(D, d)
        for base, _, names in os.walk(top):
            for n in sorted(names):
                p = os.path.join(base, n)
                put(tar, os.path.relpath(p, D).replace(os.sep, '/'), open(p, 'rb').read())
    if os.path.isfile(UNIT):
        put(tar, 'service/projectarena-relay.service', open(UNIT, 'rb').read())
    relay = open(os.path.join(D, 'projectarena_relay.py'), 'rb').read()
    man['relay_sha256_16'] = hashlib.sha256(relay).hexdigest()[:16]
    put(tar, 'manifest.json', json.dumps(man, indent=1, sort_keys=True).encode())
sys.stdout.buffer.write(buf.getvalue())
'''


# ---------------------------------------------------------------- DPAPI (this Windows account only)
class _Blob(ctypes.Structure):
    _fields_ = [('cbData', ctypes.wintypes.DWORD), ('pbData', ctypes.POINTER(ctypes.c_char))]


def _blob(data):
    buf = ctypes.create_string_buffer(data, len(data))
    return _Blob(len(data), ctypes.cast(buf, ctypes.POINTER(ctypes.c_char))), buf


def _dpapi(data, protect):
    c = ctypes.windll.crypt32
    k = ctypes.windll.kernel32
    k.LocalFree.argtypes = [ctypes.c_void_p]
    inp, keep1 = _blob(data)
    ent, keep2 = _blob(ENTROPY)
    out = _Blob()
    flags = 0x1   # CRYPTPROTECT_UI_FORBIDDEN
    if protect:
        ok = c.CryptProtectData(ctypes.byref(inp), ctypes.c_wchar_p('Project Arena server backup'), ctypes.byref(ent),
                                None, None, flags, ctypes.byref(out))
    else:
        ok = c.CryptUnprotectData(ctypes.byref(inp), None, ctypes.byref(ent), None, None, flags, ctypes.byref(out))
    if not ok:
        raise OSError('DPAPI %s failed (Windows error %d)' % ('encryption' if protect else 'decryption',
                                                             ctypes.GetLastError()))
    try:
        return ctypes.string_at(out.pbData, out.cbData)
    finally:
        k.LocalFree(ctypes.cast(out.pbData, ctypes.c_void_p))


def encrypt(data):
    return MAGIC + _dpapi(data, True)


def decrypt(data):
    if not data.startswith(MAGIC):
        raise ValueError('not a Project Arena server backup')
    return _dpapi(data[len(MAGIC):], False)


# ---------------------------------------------------------------- the backup
def read_cfg(path):
    cfg = {}
    with open(path, encoding='utf-8') as f:
        for line in f:
            if '=' in line and not line.lstrip().startswith('#'):
                k, v = line.split('=', 1)
                cfg[k.strip()] = v.strip()
    if not cfg.get('ssh') or not cfg.get('key'):
        raise ValueError('%s needs ssh= and key= lines' % path)
    return cfg


def fetch(cfg):
    ssh = shutil.which('ssh') or r'C:\Windows\System32\OpenSSH\ssh.exe'
    r = subprocess.run([ssh, '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=20', '-i', cfg['key'], cfg['ssh'],
                        'sudo -u projectarena python3 -I -'], input=REMOTE.encode(), stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, timeout=300,
                       creationflags=0x08000000)   # (CREATE_NO_WINDOW: run by pythonw from Task Scheduler, no window)
    if r.returncode != 0 or not r.stdout:
        err = r.stderr.decode(errors='replace').strip().splitlines()
        raise RuntimeError('the server gave no backup (exit %d): %s' % (r.returncode, err[-1][:200] if err else '-'))
    return r.stdout


def inspect(tgz):
    """The manifest and what the copy really holds; raises if anything doesn't add up."""
    with tarfile.open(fileobj=io.BytesIO(tgz), mode='r:gz') as tar:
        names = tar.getnames()
        man = json.loads(tar.extractfile('manifest.json').read().decode())
        db = tar.extractfile('player_stats.db').read()
        salt = tar.extractfile('player_stats.db.salt').read() if 'player_stats.db.salt' in names else b''
    if not salt:
        raise RuntimeError('the salt is missing: a restored database could not be matched to anyone')
    fd, path = tempfile.mkstemp(suffix='.db')
    try:
        os.write(fd, db)
        os.close(fd)
        con = sqlite3.connect(path)
        try:
            ok = con.execute('PRAGMA integrity_check').fetchone()[0]
            counts = {t: con.execute('SELECT COUNT(*) FROM "%s"' % t).fetchone()[0]
                      for (t,) in con.execute("SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name")}
        finally:
            con.close()
    finally:
        os.remove(path)
    if ok != 'ok':
        raise RuntimeError('the database copy fails its integrity check: %s' % ok)
    if counts != man.get('tables'):
        raise RuntimeError('the database copy has other counts than the server said')
    return man, names, counts


def summary(man, names, counts):
    sounds = len([n for n in names if n.startswith('sound_library/') and n.endswith('.wav')])
    reports = len([n for n in names if n.startswith('bug_reports/')])
    return '%d players, %d training rows, %d sounds, %d report files, relay %s, made %s' % (
        counts.get('players', 0), counts.get('training', 0), sounds, reports, man.get('relay_sha256_16', '?'),
        man.get('made', '?'))


def log(out, line):
    with open(os.path.join(out, 'backup_log.txt'), 'a', encoding='utf-8') as f:
        f.write('%s %s\n' % (datetime.datetime.now().strftime('%Y-%m-%d %H:%M'), line))


def backup(args):
    os.makedirs(args.out, exist_ok=True)
    try:
        tgz = fetch(read_cfg(args.cfg))
        man, names, counts = inspect(tgz)
        name = 'pa-server-%s.bak' % datetime.datetime.now().strftime('%Y-%m-%d_%H%M%S')
        path = os.path.join(args.out, name)
        blob = encrypt(tgz)
        with open(path + '.tmp', 'wb') as f:
            f.write(blob)
        with open(path + '.tmp', 'rb') as f:
            if decrypt(f.read()) != tgz:
                raise RuntimeError('the written file does not read back the same')
        os.replace(path + '.tmp', path)
        kept = sorted(n for n in os.listdir(args.out) if n.startswith('pa-server-') and n.endswith('.bak'))
        gone = kept[:-args.keep] if args.keep > 0 else []
        for n in gone:
            os.remove(os.path.join(args.out, n))
        line = 'OK %s (%d KB): %s; %d kept%s' % (name, len(blob) // 1024, summary(man, names, counts),
                                                 len(kept) - len(gone), ', %d older deleted' % len(gone) if gone else '')
        log(args.out, line)
        print(line)
        return 0
    except Exception as e:   # (the log says it; Task Scheduler sees the exit code)
        line = 'FAILED: %s' % e
        log(args.out, line)
        print(line, file=sys.stderr)
        return 1


def check(path):
    with open(path, 'rb') as f:
        tgz = decrypt(f.read())
    man, names, counts = inspect(tgz)
    print(summary(man, names, counts))
    for t, n in sorted(counts.items()):
        print('  table %-16s %d rows' % (t, n))
    print('  %d files' % len(names))
    return 0


def restore(path, dest):
    with open(path, 'rb') as f:
        tgz = decrypt(f.read())
    man, names, counts = inspect(tgz)
    os.makedirs(dest, exist_ok=True)
    base = os.path.realpath(dest)
    with tarfile.open(fileobj=io.BytesIO(tgz), mode='r:gz') as tar:
        for m in tar.getmembers():
            target = os.path.realpath(os.path.join(dest, m.name))
            if not m.isfile() or not target.startswith(base + os.sep):
                continue   # (only plain files, only inside DIR)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, 'wb') as f:
                f.write(tar.extractfile(m).read())
    print('restored into %s: %s' % (dest, summary(man, names, counts)))
    print('next: docs/SERVER_SETUP.md, "Restoring the server\'s data"')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--out', default=DEFAULT_OUT)
    ap.add_argument('--keep', type=int, default=14)
    ap.add_argument('--cfg', default=os.path.join(ROOT, 'sounds.local.cfg'))
    ap.add_argument('--check', metavar='FILE')
    ap.add_argument('--restore', nargs=2, metavar=('FILE', 'DIR'))
    args = ap.parse_args()
    if args.check:
        return check(args.check)
    if args.restore:
        return restore(*args.restore)
    return backup(args)


if __name__ == '__main__':
    sys.exit(main())
