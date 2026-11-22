"""Self-test for The Crowd's notifications (Web Push): the encryption (RFC 8291's own example) and the VAPID signature,
the fans' choices, the push-address check, the device table (what is kept, who an event goes to, the push services'
answers, the limits), and the whole path through a local relay: a phone turns notifications on over the WebSocket, a
host opens an arena, two players and then four face off, the phone turns them off.
Needs the `cryptography` package (as the relay does for notifications; Ubuntu: python3-cryptography).
Usage: python test_relay_push.py"""
import asyncio
import json
import os
import sqlite3
import struct
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import projectarena_relay as R  # noqa: E402
import test_relay_crowd as TC  # noqa: E402  (its fake host and phone)

if not R.HAVE_CRYPTOGRAPHY:
    print('the cryptography package is not installed: notifications are off in the relay, nothing to test here')
    sys.exit(0)

from cryptography.hazmat.primitives import hashes, serialization  # noqa: E402
from cryptography.hazmat.primitives.asymmetric import ec, utils  # noqa: E402
from cryptography.hazmat.primitives.ciphers.aead import AESGCM  # noqa: E402
from cryptography.hazmat.primitives.kdf.hkdf import HKDF  # noqa: E402

TC.PORT, TC.WS = 47798, 47799
fails = passed = 0
NOON = 1795000000 - 1795000000 % 86400 + 12 * 3600     # a day's noon, UTC


def check(cond, msg):
    global fails, passed
    print(('  ok   ' if cond else '  FAIL ') + msg)
    fails += 0 if cond else 1
    passed += 1 if cond else 0


class Browser:
    """a browser's push subscription: its key pair and auth secret, and a decrypter for what the relay sends it"""
    def __init__(self, endpoint='https://fcm.googleapis.com/fcm/send/abc123'):
        self.endpoint, self.auth = endpoint, os.urandom(16)
        self.key = ec.generate_private_key(ec.SECP256R1())
        self.pub = self.key.public_key().public_bytes(serialization.Encoding.X962,
                                                      serialization.PublicFormat.UncompressedPoint)

    def sub(self):
        return {'endpoint': self.endpoint, 'keys': {'p256dh': R.b64u(self.pub), 'auth': R.b64u(self.auth)}}

    def decrypt(self, body):
        """RFC 8291 as a browser does it (cryptography's own HKDF, not the relay's)"""
        salt, rs, idlen = body[:16], struct.unpack('>I', body[16:20])[0], body[20]
        as_pub, ct = body[21:21 + idlen], body[21 + idlen:]
        peer = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), as_pub)
        shared = self.key.exchange(ec.ECDH(), peer)
        ikm = HKDF(hashes.SHA256(), 32, self.auth, b'WebPush: info\0' + self.pub + as_pub).derive(shared)
        cek = HKDF(hashes.SHA256(), 16, salt, b'Content-Encoding: aes128gcm\0').derive(ikm)
        nonce = HKDF(hashes.SHA256(), 12, salt, b'Content-Encoding: nonce\0').derive(ikm)
        plain = AESGCM(cek).decrypt(nonce, ct, None)
        assert rs == 4096 and plain.endswith(b'\2')
        return json.loads(plain[:-1])


def verify_es256(pub_b64, signed, sig):
    pub = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), R.b64u_dec(pub_b64))
    try:
        pub.verify(utils.encode_dss_signature(int.from_bytes(sig[:32], 'big'), int.from_bytes(sig[32:], 'big')),
                   signed, ec.ECDSA(hashes.SHA256()))
        return True
    except Exception:
        return False


def test_crypto():
    print('encryption and signature')
    d = R.b64u_dec
    # RFC 8291, appendix A: its keys, salt and message, and the body it gives
    as_priv = int.from_bytes(d('yfWPiYE-n46HLnH0KqZOF1fJJU3MYrct3AELtAQ-oRw'), 'big')
    ua_pub = d('BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4')
    body = R.webpush_encrypt(ua_pub, d('BTBZMqHH6r4Tts7J_aSIgg'), b'When I grow up, I want to be a watermelon',
                             _d=as_priv, _salt=d('DGv6ra1nlYgDCS1FRnbzlw'))
    head = ('DGv6ra1nlYgDCS1FRnbzlwAAEABBBP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6'
            'PBru3jl7A8')
    ct = '8pfeW0KbunFT06SuDKoJH9Ql87S1QUrdirN6GcG7sFz1y1sqLgVi1VhjVkHsUoEsbI_0LpXMuGvnzQ'
    check(R.b64u(body[:86]) == head and R.b64u(body[86:]) == ct,
          'RFC 8291\'s own example, byte for byte (its header and its ciphertext)')
    b = Browser()
    check(b.decrypt(R.webpush_encrypt(b.pub, b.auth, b'{"title":"hi"}')) == {'title': 'hi'},
          'a message decrypts as a browser would')
    k = 0xC9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721   # RFC 6979 A.2.5's key
    check(R.p256_public(k).hex() == '04' + '60fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb6'
          '7903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299', 'P-256 public key (RFC 6979 A.2.5)')
    check(verify_es256(R.b64u(R.p256_public(k)), b'sample', R.es256(k, b'sample')), 'ES256 signature verifies')
    for bad in (b'\4' + bytes(64), b'\4' + bytes(10), ua_pub[:-1] + bytes([ua_pub[-1] ^ 1]), 'text'):
        try:
            R.p256_point(bad)
            check(False, 'a bad browser key is refused (%r...)' % bad[:4])
        except (ValueError, TypeError):
            check(True, 'a bad browser key is refused (%r...)' % bad[:4])


def test_rules():
    print('choices and push addresses')
    p = R.push_prefs({'when': 'match', 'fav': ['ACE'], 'gap': 30, 'quiet': [22, 8], 'tz': -360, 'extra': 'x'})
    check(p == {'when': 'match', 'gap': 30, 'quiet': [22, 8], 'tz': -360},
          'choices are cleaned (no favorite players: following a player by name is not offered)')
    check(R.push_prefs({'when': 'fav', 'gap': 5, 'quiet': [3, 3], 'tz': 'x'}) ==
          {'when': 'any', 'gap': 60, 'quiet': None, 'tz': 0}, 'bad choices fall back to the defaults')
    check(R.push_prefs('nonsense') == {'when': 'any', 'gap': 60, 'quiet': None, 'tz': 0}, '... and nonsense too')
    ok = ['https://fcm.googleapis.com/fcm/send/x', 'https://updates.push.services.mozilla.com/wpush/v2/x',
          'https://web.push.apple.com/QOx', 'https://wns2-par02p.notify.windows.com/w/?token=x']
    bad = ['http://fcm.googleapis.com/x', 'https://evil.com/fcm.googleapis.com', 'https://fcm.googleapis.com.evil.com/x',
           'https://fcm.googleapis.com:8443/x', 'https://user@fcm.googleapis.com/x', 'https://127.0.0.1/x',
           'https://localhost/x', 'https://notify.windows.com.evil/x', 'ftp://web.push.apple.com/x',
           'https://fcm.googleapis.com/' + 'x' * 1100, 42, None]
    check(all(R.push_endpoint_ok(u) for u in ok), 'the browsers\' push services are allowed')
    check(not any(R.push_endpoint_ok(u) for u in bad), 'anything else is refused (the relay posts nowhere else)')


def make_push(folder, name, sent=None, status=201):
    n = R.Notifier(log=lambda m: None)
    n.run_inline = lambda job: (sent.append(R.notify_request(job)) if sent is not None else None) or status
    return R.Push(os.path.join(folder, name), 'https://example.com', n, log=lambda m: None), n


def test_store(folder):
    print('the device table')
    sent = []
    p, n = make_push(folder, 'push.db', sent)
    check(len(R.b64u_dec(p.public_key)) == 65 and os.path.exists(os.path.join(folder, 'push.db.vapid')),
          'a VAPID key is made once, beside the table')
    if os.name == 'posix':
        check(os.stat(os.path.join(folder, 'push.db.vapid')).st_mode & 0o077 == 0, 'the key is readable by the server only')
    cols = [r[1] for r in p.db.execute('PRAGMA table_info(subs)')]
    check(cols == ['endpoint', 'p256dh', 'auth', 'mode', 'gap', 'qa', 'qb', 'tz', 'last_sent', 'seen', 'fails'],
          'what is kept: the push address and its keys, the choices, when last notified and seen, refusals')
    b = Browser()
    check(p.subscribe(b.sub(), {'when': 'any'}), 'a subscription is kept')
    check(not p.subscribe(dict(b.sub(), endpoint='https://evil.com/x'), {}), 'not one for another address')
    check(not p.subscribe({'endpoint': b.endpoint, 'keys': {'p256dh': R.b64u(b'\4' + bytes(64)), 'auth': 'x'}}, {}),
          'not one with a bad key')
    check(not p.subscribe('nonsense', {}) and not p.subscribe(None, None)
          and not p.subscribe({'endpoint': b.endpoint, 'keys': {'p256dh': 5, 'auth': []}}, {}), 'not nonsense')
    before = p.db.total_changes
    for _ in range(20):
        p.subscribe(b.sub(), {'when': 'any'})
    check(p.db.total_changes == before, 'the same subscription again (the page opened again): no write')
    p.subscribe(b.sub(), {'when': 'any', 'gap': 30})
    check(p.db.total_changes == before + 1 and p.count() == 1, 'changed choices: one write, still one row')
    p.subscribe(b.sub(), {'when': 'any'})
    p.notify('hosted', 'ACE is hosting an arena', 'Tap to watch', 'ab12', now=NOON)
    check(len(sent) == 1, 'a notification goes to the device')
    msg = b.decrypt(sent[0][1])
    check(msg == {'title': 'ACE is hosting an arena', 'body': 'Tap to watch', 'id': 'ab12'}, 'its message decrypts')
    h = sent[0][2]
    check(h['Authorization'].endswith('k=' + p.public_key) and h['TTL'] == str(R.PUSH_TTL)
          and h['Content-Encoding'] == 'aes128gcm', 'Web Push headers (VAPID, TTL, aes128gcm)')
    head, claims, sig = h['Authorization'].split('t=')[1].split(',')[0].split('.')
    c = json.loads(R.b64u_dec(claims))
    check(c['aud'] == 'https://fcm.googleapis.com' and c['sub'] == 'https://example.com' and c['exp'] > time.time(),
          'the VAPID token names the push service and the contact')
    check(verify_es256(p.public_key, (head + '.' + claims).encode(), R.b64u_dec(sig)),
          'the VAPID token is signed with the server\'s key')
    p.notify('match', 'ACE vs KAI is live', 'Tap', 'ab12', now=NOON + 59 * 60)
    check(len(sent) == 1, 'nothing more within its hour')
    p.notify('match', 'ACE vs KAI is live', 'Tap', 'ab12', now=NOON + 61 * 60)
    check(len(sent) == 2, '... and again after it')

    print('who an event goes to')
    p.db.execute('DELETE FROM subs')
    want = {}
    for name, prefs in [('any', {'when': 'any'}), ('match', {'when': 'match'}),
                        ('quiet', {'when': 'any', 'quiet': [22, 8], 'tz': -360}),     # US Central: UTC - 6 h
                        ('quiet-day', {'when': 'any', 'quiet': [1, 5], 'tz': 0})]:
        br = Browser('https://fcm.googleapis.com/fcm/send/' + name)
        p.subscribe(br.sub(), prefs)
        want[br.endpoint] = name

    def who(kind, now):
        del sent[:]
        p.db.execute('UPDATE subs SET last_sent = 0')
        p.notify(kind, 't', 'b', 'cd34', now=now)
        return sorted(want[s[0]] for s in sent)
    check(who('hosted', NOON) == ['any', 'quiet-day'],
          'someone hosts: "any" yes, "match" no; quiet hours 10 PM-8 AM (6 AM there): no')
    check(who('match', NOON) == ['any', 'match', 'quiet-day'], 'a match starts: "match" too')
    check(who('hosted', NOON + 2 * 3600) == ['any', 'quiet', 'quiet-day'], 'quiet hours over at 8 AM there')
    check(who('hosted', NOON - 8 * 3600) == ['any'], '... and on from 10 PM there (and 1-5 AM UTC: 4 AM)')
    check(who('hosted', NOON - 9 * 3600) == ['any', 'quiet'], 'quiet hours that don\'t cross midnight (1-5 AM: 3 AM)')
    check(who('hosted', NOON - 7 * 3600) == ['any', 'quiet-day'], 'quiet hours that do (10 PM-8 AM: 11 PM there)')

    print('the push services\' answers')
    p.db.execute('DELETE FROM subs')
    p.subscribe(b.sub(), {})
    n.run_inline = lambda job: 410
    p.notify('hosted', 't', 'b', 'cd34', now=NOON)
    check(p.count() == 0, 'a device the push service says is gone (410) is forgotten')
    p.subscribe(b.sub(), {})
    n.run_inline = lambda job: 403
    for k in range(R.PUSH_FAILS):
        check(p.count() == 1, 'refused %d time%s (403): kept' % (k, '' if k == 1 else 's'))
        p.db.execute('UPDATE subs SET last_sent = 0')
        p.notify('hosted', 't', 'b', 'cd34', now=NOON)
    check(p.count() == 0, '... refused %d times in a row: forgotten' % R.PUSH_FAILS)
    p.subscribe(b.sub(), {})
    for st in (403, 403, 201, 403, 403, 429, 503, 0):
        n.run_inline = lambda job, st=st: st
        p.db.execute('UPDATE subs SET last_sent = 0')
        p.notify('hosted', 't', 'b', 'cd34', now=NOON)
    check(p.count() == 1, 'a delivery clears the count; "too many", a server error or no answer don\'t count')
    p.unsubscribe(b.endpoint)
    check(p.count() == 0, 'turning notifications off deletes the row')
    p.subscribe(b.sub(), {})
    p.db.execute('UPDATE subs SET seen = ?', (time.time() - 400 * 86400,))
    p.purge()
    check(p.count() == 0, 'a device not seen for a year is forgotten')
    p2, _ = make_push(folder, 'push.db')
    check(p2.public_key == p.public_key, 'the same key after a restart (subscriptions stay valid)')
    p2.db.close()

    print('limits')
    n.run_inline = lambda job: sent.append(R.notify_request(job)) or 201
    p.subscribe(b.sub(), {})
    n.sent, before = R.PUSH_PER_HOUR, len(sent)
    p.notify('hosted', 't', 'b', 'ef56', now=NOON)
    check(len(sent) == before and p.db.execute('SELECT last_sent FROM subs').fetchone()[0] == 0,
          'over the hourly limit: dropped, and the device is not marked as notified')
    n.hour -= 3601
    p.notify('hosted', 't', 'b', 'ef56', now=NOON)
    check(len(sent) == before + 1, 'the next hour: sent again')
    n.waiting = R.PUSH_QUEUE_MAX
    check(not n.submit(('webpush',) + (None,) * 7, lambda st: None), 'too many waiting: dropped, not queued')
    n.waiting = 0
    pub, auth = R.b64u(b.pub), R.b64u(b.auth)
    p.db.executemany('INSERT INTO subs (endpoint, p256dh, auth, mode, gap, tz, seen) VALUES (?, ?, ?, ?, 60, 0, ?)',
                     [('https://fcm.googleapis.com/fcm/send/f%d' % i, pub, auth, 'match', time.time())
                      for i in range(R.PUSH_MAX - p.count())])
    p.db.commit()
    check(not p.subscribe(Browser('https://fcm.googleapis.com/fcm/send/new').sub(), {}) and p.count() == R.PUSH_MAX,
          'a full table (%d): no new device' % R.PUSH_MAX)
    check(p.subscribe(b.sub(), {'gap': 30}), '... a device already in it can still change its choices')
    t = time.perf_counter()
    p.notify('hosted', 't', 'b', 'ef56', now=NOON + 7200)     # (one device wants a "hosted": the whole table read)
    quiet_ms = (time.perf_counter() - t) * 1000
    n.run_inline = lambda job: 201
    t = time.perf_counter()
    p.notify('match', 't', 'b', 'ef56', now=NOON + 7200)      # (everyone: PUSH_PER_EVENT handed on)
    busy_ms = (time.perf_counter() - t) * 1000
    check(quiet_ms < 30 and busy_ms < 150, 'a full table holds the relay\'s loop %.1f ms per event (%.0f ms when %d go '
          'out at once)' % (quiet_ms, busy_ms, R.PUSH_PER_EVENT))
    p.db.close()


async def test_live(folder):
    print('through the relay')
    loop = asyncio.get_running_loop()
    relay = R.Relay(8, log=lambda m: None)
    tr, _ = await loop.create_datagram_endpoint(lambda: relay, local_addr=('127.0.0.1', TC.PORT))
    sent = []
    push, n = make_push(folder, 'live.db')
    n.run_inline = lambda job: sent.append(R.notify_request(job)[1]) or 201
    crowd = R.Crowd(relay, lambda m: None, [TC.ORIGIN], push)
    srv = await asyncio.start_server(crowd.handle, '127.0.0.1', TC.WS, limit=16384)

    async def ticker():
        k = 0
        while True:
            await asyncio.sleep(R.CROWD_TICK)
            k += 1
            crowd.tick(k)
    tick = asyncio.ensure_future(ticker())
    db_path = os.path.join(folder, 'live.db')
    b = Browser()

    def sql(q):   # (this thread's own connection: the relay's belongs to its loop)
        db = sqlite3.connect(db_path, timeout=5)
        try:
            r = db.execute(q).fetchall()
            db.commit()
            return r
        finally:
            db.close()

    def titles():
        return [b.decrypt(x)['title'] for x in sent]

    def blocking():
        ph = TC.Phone()
        hello = ph.until(lambda m: m.get('t') == 'hello')
        check(hello is not None and hello.get('push') == push.public_key and 'ntfy' not in hello,
              'hello tells the page the server\'s key (and nothing about ntfy)')
        ph.send({'t': 'push', 'sub': b.sub(), 'prefs': {'when': 'any', 'gap': 30, 'tz': -360}})
        r = ph.until(lambda m: m.get('t') == 'push')
        check(r == {'t': 'push', 'ok': True} and sql('SELECT COUNT(*) FROM subs')[0][0] == 1,
              'the phone turns notifications on')
        host = TC.Host('ACE')
        host.hello()
        host.live(names=('ACE', 'OPEN'))
        time.sleep(0.6)
        check(titles() == ['ACE is hosting an arena'], 'someone hosts: "ACE is hosting an arena"')
        sql('UPDATE subs SET last_sent = 0')
        host.live(names=('ACE', 'KAI'))
        time.sleep(0.6)
        check(titles()[1:] == ['ACE vs KAI is live'], 'two players face off: "ACE vs KAI is live"')
        sql('UPDATE subs SET last_sent = 0')
        host.live(names=('ACE', 'KAI'))
        time.sleep(0.6)
        check(len(sent) == 2, 'the same arena never twice')
        host.s.close()
        sql('UPDATE subs SET last_sent = 0')
        h4 = TC.Host('ANNA')
        h4.hello()
        h4.live4()
        time.sleep(0.6)
        check(titles()[2:] == ['ANNA, BEN, CARA & DEV are playing FOUR CORNERS'],
              'four players: "ANNA, BEN, CARA & DEV are playing FOUR CORNERS" (a match on already: just that)')
        h4.s.close()
        sql('UPDATE subs SET last_sent = 0')
        hb = TC.Host('LEX')
        hb.hello()
        hb.live(names=('LEX', 'BOT'))
        time.sleep(0.6)
        sql('UPDATE subs SET last_sent = 0')
        hb.live(names=('LEX', 'BOT'))
        time.sleep(0.6)
        check(titles()[3:] == ['LEX is hosting an arena'], 'a match against the bot is no match (just the hosting)')
        hb.s.close()
        ph.send({'t': 'unpush', 'endpoint': b.endpoint})
        r = ph.until(lambda m: m.get('t') == 'push')
        check(r is not None and r.get('off') and sql('SELECT COUNT(*) FROM subs')[0][0] == 0,
              'the phone turns notifications off: the row is gone')
        ph.s.close()
        # one page connection: at most PUSH_MSG_BURST notification messages at once
        ph = TC.Phone(xff='198.51.100.40')
        ph.until(lambda m: m.get('t') == 'hello')
        for gap in (30, 60, 180, 30, 60, 180, 30, 60):
            ph.send({'t': 'push', 'sub': b.sub(), 'prefs': {'gap': gap}})
        replies = [ph.until(lambda m: m.get('t') == 'push') for _ in range(8)]
        check([bool(x and x['ok']) for x in replies] == [True] * R.PUSH_MSG_BURST + [False] * (8 - R.PUSH_MSG_BURST),
              'one connection: %d notification messages at once, then one every %d s'
              % (R.PUSH_MSG_BURST, R.PUSH_MSG_EVERY))
        ph.s.close()
        # one address: at most PUSH_NEW_PER_ADDR new devices a day, however many connections
        oks = []
        for k in range(R.PUSH_NEW_PER_ADDR + 2):
            ph = TC.Phone(xff='198.51.100.50')
            ph.until(lambda m: m.get('t') == 'hello')
            ph.send({'t': 'push', 'sub': Browser('https://fcm.googleapis.com/fcm/send/many%d' % k).sub(), 'prefs': {}})
            r = ph.until(lambda m: m.get('t') == 'push')
            oks.append(bool(r and r['ok']))
            ph.s.close()
        check(oks == [True] * R.PUSH_NEW_PER_ADDR + [False, False],
              'one address: %d new devices a day, then no more' % R.PUSH_NEW_PER_ADDR)
        ph = TC.Phone(xff='198.51.100.51')
        ph.until(lambda m: m.get('t') == 'hello')
        ph.send({'t': 'push', 'sub': Browser('https://fcm.googleapis.com/fcm/send/other').sub(), 'prefs': {}})
        r = ph.until(lambda m: m.get('t') == 'push')
        check(r is not None and r['ok'], '... another address is not held up')
        ph.send({'t': 'cheer', 'pad': 'x' * 1900})
        check(ph.until(lambda m: m.get('t') == 'list', 2.5) is not None, 'a 2 kB message (a long push address) is fine')
        ph.s.close()

    await loop.run_in_executor(None, blocking)
    tick.cancel()
    srv.close()
    tr.close()
    push.db.close()


class FakePushService:
    """a local HTTP server standing in for a push service: keeps what it is sent"""
    def __init__(self):
        import http.server
        import threading
        got = self.got = []

        class H(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                body = self.rfile.read(int(self.headers['Content-Length']))
                got.append((self.path, dict(self.headers), body, time.time()))
                self.send_response(201)
                self.send_header('Content-Length', '0')
                self.end_headers()

            def log_message(self, *a):
                pass
        self.srv = http.server.ThreadingHTTPServer(('127.0.0.1', 0), H)
        self.url = 'http://127.0.0.1:%d' % self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()


async def test_process():
    print('the notification process')
    svc = FakePushService()
    n = R.Notifier(log=lambda m: None)
    b = Browser()
    done = []
    # a burst: 200 Web Push messages (encrypted and signed in the other process) while the relay's loop keeps time
    worst = 0.0
    d = 0xC9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721
    for i in range(200):
        n.submit(('webpush', svc.url + '/push/%d' % i, b.pub, b.auth, b'{"title":"burst"}', d, 'https://x', 'pub'),
                 done.append)
    pid = next(iter(n.pool._processes))
    end = time.time() + 60
    while len(done) < 200 and time.time() < end:
        t = time.perf_counter()
        await asyncio.sleep(0.005)
        worst = max(worst, time.perf_counter() - t - 0.005)
    check(len(done) == 200 and all(s == 201 for s in done), 'all 200 sent from the notification process')
    if hasattr(os, 'getpriority'):
        check(os.getpriority(os.PRIO_PROCESS, pid) == 19, 'it runs at the lowest CPU priority (nice 19)')
    check(worst < 0.05, 'the relay\'s loop stays responsive meanwhile (worst extra delay %.1f ms)' % (worst * 1000))
    check(bool(svc.got) and b.decrypt(svc.got[0][2]) == {'title': 'burst'}, 'what the process sends decrypts')
    n.pool.shutdown()
    svc.srv.shutdown()


def main():
    folder = tempfile.mkdtemp()
    test_crypto()
    test_rules()
    test_store(folder)
    asyncio.run(test_live(folder))
    asyncio.run(test_process())
    import shutil
    shutil.rmtree(folder, ignore_errors=True)
    print('%d passed, %d failed' % (passed, fails))
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
