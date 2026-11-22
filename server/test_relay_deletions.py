#!/usr/bin/env python3
"""Tests DELETIONS (2026-11-20): a player's DELETE leaves a note beside the statistics database, and every start applies
the notes again, so a restored backup can't bring deleted statistics back; what the player earns after deleting stays.
Runs the relay's Stats class on a temporary database (no relay, no network).
Run: python -I server/test_relay_deletions.py"""
import os
import shutil
import sqlite3
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import projectarena_relay as R  # noqa: E402

passed = failed = 0


def check(ok, msg):
    global passed, failed
    if ok:
        passed += 1
        print('  ok   ' + msg)
    else:
        failed += 1
        print('  FAIL ' + msg)


def open_stats(path, lines):
    st = R.Stats(path, lines.append)
    st.db.execute('CREATE TABLE IF NOT EXISTS sound_votes (sound TEXT, key TEXT, vote INTEGER, updated INTEGER, '
                  'PRIMARY KEY (sound, key))')
    return st


def rows(path, key):
    db = sqlite3.connect(path)
    try:
        return tuple(db.execute('SELECT COUNT(*) FROM %s WHERE key = ?' % t, (key,)).fetchone()[0]
                     for t in ('players', 'player_modes', 'training', 'sound_votes'))
    finally:
        db.close()


def play(st, key, name, when):
    """A match, a TRAINING best and a SOUND STUDIO vote for this player, dated `when`."""
    st.add(key, name, True, (3, 2, 1, 0, 4, 1), 1500, mode=0)
    st.train(key, name, 1, 30000, 8, 10)
    st.db.execute('INSERT OR REPLACE INTO sound_votes VALUES (?, ?, ?, ?)', ('Bounce__x.wav', key, 1, when))
    st.db.execute('UPDATE players SET first_seen = ?, last_seen = ? WHERE key = ?', (when, when, key))
    st.db.execute('UPDATE training SET updated = ? WHERE key = ?', (when, key))
    st.db.commit()


def main():
    work = tempfile.mkdtemp(prefix='pa_deletions_')
    try:
        path = os.path.join(work, 'player_stats.db')
        lines = []
        st = open_stats(path, lines)
        alice, bob = st.ident(b'A' * 16), st.ident(b'B' * 16)
        now = int(time.time())
        play(st, alice, 'ALICE', now - 3600)
        play(st, bob, 'BOB', now - 3600)
        st.db.close()
        shutil.copy(path, path + '.backup')   # (last night's backup: both players)

        st = open_stats(path, lines)
        st.delete(alice)
        st.note_deletion(alice)
        notes = st.deletions()
        check(len(notes) == 1 and notes[0][1] == alice and abs(notes[0][0] - time.time()) < 5,
              "DELETE leaves one note: the time and the key's salted hash")
        with open(st.deletions_path, encoding='ascii') as f:
            text = f.read()
        check('ALICE' not in text and len(text.split()) == 2, 'the note holds nothing else (no name)')
        st.db.close()

        shutil.copy(path + '.backup', path)   # the server's database restored from the backup
        check(rows(path, alice) == (1, 1, 1, 1), 'the restored copy has the deleted player back (before the start)')
        lines.clear()
        st = open_stats(path, lines)
        st.db.close()
        check(rows(path, alice) == (0, 0, 0, 0), 'the next start applies the deletion again: career, modes, TRAINING, votes')
        check(rows(path, bob) == (1, 1, 1, 1), "nobody else's statistics are touched")
        check(any('1 deletions applied again' in l for l in lines), 'the log says a deletion was applied again')

        st = open_stats(path, lines)   # Alice plays on after deleting: a new career, newer than the note
        play(st, alice, 'ALICE', int(time.time()) + 5)
        st.db.close()
        lines.clear()
        st = open_stats(path, lines)
        st.db.close()
        check(rows(path, alice) == (1, 1, 1, 1), 'what she earns after deleting stays (a start later)')
        check(not any('applied again' in l for l in lines), 'and nothing is said to be applied again')

        st = open_stats(path, lines)   # a note past 30 days: ignored, and the daily purge drops it from the file
        old = int(time.time()) - 31 * 86400
        st.write_deletions([(old, bob)] + st.deletions())
        check(len(st.deletions()) == 1, 'a note older than 30 days counts for nothing')
        st.purge()
        with open(st.deletions_path, encoding='ascii') as f:
            check(len(f.read().splitlines()) == 1, 'the daily purge removes it from the file')
        st.db.close()
        check(rows(path, bob) == (1, 1, 1, 1), 'and the player it named keeps their statistics')

        os.remove(path + '.deletions')
        lines.clear()
        st = open_stats(path, lines)
        st.db.close()
        check(not any('deletion notes' in l or 'applied again' in l for l in lines),
              'without a notes file: nothing happens, nothing said')
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('%d passed, %d failed' % (passed, failed))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
