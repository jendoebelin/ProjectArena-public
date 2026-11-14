"""Load test for the community relay: how many players one CPU core can carry.

    python -I tools/relay_load_test.py [--relay server/projectarena_relay.py] [--udp] [--seconds 3]

It runs the relay's own code (imported from --relay, never started as a server: no port is opened for it, no file is
written) and feeds it the packets the game really sends, at the game's rates, for rooms full of simulated players. It
measures the CPU time the relay needs for each second of play: 100 % means one core is fully busy. --udp makes the
relay's replies real UDP sends to a local socket that never reads them (the system's cost of sending is then counted
too, as on the live server); without it they are only counted.

The clock the relay sees is simulated (a second of traffic is a second for its rate limits), so the numbers don't
depend on how fast the machine replays them; packets the relay's limits refuse are counted as dropped.

On the server, without copying anything and at the lowest priority (the live relay keeps the CPU first):
    ssh -i ~/.ssh/oracle.key ubuntu@SERVER "nice -n 19 python3 -I - --relay /opt/projectarena/projectarena_relay.py --udp" < tools/relay_load_test.py

What the game sends (app/src/net.cpp, net.h; Quest at 72 frames a second):
  every member of a room   INPUT to its host every frame (72/s)
  every host               STATE every frame to the players in the tank, 30/s to the people waiting and to every
                           spectator; ROOM 15/s to every member and spectator; BEACON 2/s to everyone (the relay copies
                           it to every connected player); LIVE 1/s to the relay
  every spectator          SPECTATE 5/s to every live host it hears (the lobby's overlord view watches every arena),
                           and so receives every host's STATE (30/s) and ROOM (15/s)
  a person talking         25 voice frames a second to the host, which sends each on to everyone else in the room
  everyone                 PING every 2 s
Packet sizes are estimates (they change the bandwidth figures, hardly the CPU).
"""
import argparse
import importlib.util
import os
import socket
import struct
import sys
import time

FPS = 72
SIZE = {'input': 90, 'state': 420, 'room': 220, 'beacon': 70, 'live': 82, 'spectate': 40, 'voice': 660, 'ping': 0}


class SimClock:
    """The relay's time.monotonic(): advanced by the replay, one simulated second per second of traffic."""
    def __init__(self):
        self.t = 1000.0

    def monotonic(self):
        return self.t

    def time(self):
        return 1.7e9 + self.t

    def __getattr__(self, name):   # anything else from the time module
        return getattr(time, name)


class Transport:
    """Stands in for the relay's socket: counts what it sends; with udp=True also sends it for real."""
    def __init__(self, udp):
        self.sent = self.bytes = 0
        self.welcome = {}
        self.sock = self.sink = None
        if udp:
            self.sink = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.sink.bind(('127.0.0.1', 0))
            self.sink.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 16)
            self.dst = self.sink.getsockname()
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def sendto(self, data, addr):
        self.sent += 1
        self.bytes += len(data) + 28    # (IP and UDP headers)
        if data[4] == 2:                # WELCOME: keep the token and virtual address for the simulated client
            self.welcome[addr] = (data[5:21], struct.unpack_from('<I', data, 21)[0])
        if self.sock is not None:
            try:
                self.sock.sendto(data, self.dst)
            except OSError:
                pass


def load_relay(path):
    spec = importlib.util.spec_from_file_location('relay_under_test', path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def scenario(players, lobby_share, four_share, waiting_share, talk_share):
    """Split `players` into rooms: duels (a host and one player, sometimes one waiting), four-player rooms (a host and
    three), and spectators in the lobby. Returns rooms as (host, members, waiting) index lists, the lobby, talkers."""
    lobby_n = int(round(players * lobby_share))
    four_n = int(round(players * four_share)) // 4 * 4
    rest = players - lobby_n - four_n
    ids = list(range(players))
    rooms, i = [], 0
    for _ in range(four_n // 4):
        rooms.append((ids[i], ids[i + 1:i + 4], []))
        i += 4
    duel_people = rest
    n_wait = int(round(duel_people * waiting_share / 3.0)) if waiting_share else 0   # rooms of three: host, player, waiting
    for _ in range(n_wait):
        if duel_people - 3 < 0:
            break
        rooms.append((ids[i], [ids[i + 1]], [ids[i + 2]]))
        i += 3
        duel_people -= 3
    while duel_people >= 2:
        rooms.append((ids[i], [ids[i + 1]], []))
        i += 2
        duel_people -= 2
    lobby = ids[i:]
    talkers = [r[0] for k, r in enumerate(rooms) if k < len(rooms) * talk_share]
    return rooms, lobby, talkers


LOBBY_STATE, LOBBY_ROOM = 30, 15   # what a host sends each lobby spectator a second (--lobby-state / --lobby-room: a what-if)


def one_second(rooms, lobby, talkers, n_clients):
    """The packets of one second of play: (sender, kind, destination, count); destination None = everyone."""
    out = []
    hosts = [r[0] for r in rooms]
    for host, tank, waiting in rooms:
        members = tank + waiting
        for m in members:
            out.append((m, 'input', host, FPS))
        for m in tank:
            out.append((host, 'state', m, FPS))
        for m in waiting:
            out.append((host, 'state', m, 30))
        for m in members:
            out.append((host, 'room', m, 15))
        for m in lobby:
            out.append((host, 'state', m, LOBBY_STATE))
            out.append((host, 'room', m, LOBBY_ROOM))
        out.append((host, 'beacon', None, 2))
        out.append((host, 'live', -1, 1))
        if host in talkers:   # someone in the room talking: the host sends each frame on to everyone in the room
            for m in members:
                out.append((host, 'voice', m, 25))
    for s in lobby:
        for h in hosts:
            out.append((s, 'spectate', h, 5))
    for c in range(n_clients):
        out.append((c, 'ping', -1, 1))   # (every 2 s; counted as 1 to keep whole numbers: a slight overestimate)
    return out


def run(mod, players, udp, seconds, mix):
    clock = SimClock()
    mod.time = clock                     # the relay's module sees the simulated clock
    tr = Transport(udp)
    relay = mod.Relay(10000, log=lambda *a: None)
    relay.connection_made(tr)
    addrs = [('10.%d.%d.%d' % (i >> 16 & 255, i >> 8 & 255, i & 255), 40000 + i % 20000) for i in range(players)]
    name = b'LOADTEST'.ljust(16, b'\0')
    for a in addrs:
        relay.datagram_received(struct.pack('<IB', mod.MAGIC, mod.HELLO) + b'\0' * 16 + bytes([10]) + name, a)
    tok = [tr.welcome[a][0] for a in addrs]
    vip = [tr.welcome[a][1] for a in addrs]
    rooms, lobby, talkers = scenario(players, *mix)
    plan = one_second(rooms, lobby, talkers, players)
    # build each packet once
    pkts = []
    for src, kind, dst, count in plan:
        head = struct.pack('<IB', mod.MAGIC, {'live': mod.LIVE, 'ping': mod.PING}.get(kind, mod.SEND)) + tok[src]
        if kind == 'live':
            body = b'\0' * SIZE['live']
        elif kind == 'ping':
            body = b''
        else:
            d = mod.BROADCAST if dst is None else vip[dst]
            payload = struct.pack('<I', mod.GAME_MAGIC) + bytes([0, 9]) + b'\0' * (SIZE[kind] - 6)
            body = struct.pack('<I', d) + payload
        pkts.append((head + body, addrs[src], count))
    per_second = sum(c for _, _, c in pkts)
    # interleave: each sender's packets spread over the second, as they arrive live
    stream = []
    for data, addr, count in pkts:
        for k in range(count):
            stream.append(((k + 0.5) / count, data, addr))
    stream.sort(key=lambda e: e[0])
    tr.sent = tr.bytes = 0
    relay.stats['forwarded'] = relay.stats['dropped'] = 0
    feed = relay.datagram_received
    cpu0 = time.process_time()
    for s in range(seconds):
        base = clock.t
        for frac, data, addr in stream:
            clock.t = base + frac
            feed(data, addr)
        clock.t = base + 1.0
    cpu = (time.process_time() - cpu0) / seconds
    return {'players': players, 'rooms': len(rooms), 'lobby': len(lobby), 'in_pps': per_second,
            'out_pps': tr.sent / seconds, 'out_mbps': tr.bytes * 8 / seconds / 1e6,
            'dropped': relay.stats['dropped'] / seconds, 'cpu': cpu}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--relay', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'server',
                                                    'projectarena_relay.py'))
    ap.add_argument('--udp', action='store_true', help='send the relay\'s packets for real to a local socket')
    ap.add_argument('--seconds', type=int, default=3)
    ap.add_argument('--players', default='10,25,50,75,100,150,200')
    ap.add_argument('--lobby-state', type=int, default=30, help='what-if: STATE a second to each lobby spectator')
    ap.add_argument('--lobby-room', type=int, default=15, help='what-if: ROOM a second to each lobby spectator')
    args = ap.parse_args()
    global LOBBY_STATE, LOBBY_ROOM
    LOBBY_STATE, LOBBY_ROOM = args.lobby_state, args.lobby_room
    mod = load_relay(args.relay)
    mixes = {
        'launch (20% in the lobby, 10% in rooms of four)': (0.20, 0.10, 0.0, 0.5),
        'busy (30% in the lobby, 20% in rooms of four, duels with someone waiting)': (0.30, 0.20, 0.6, 0.5),
        'no lobby (everyone in rooms)': (0.0, 0.20, 0.3, 0.5),
    }
    print('relay: %s  (python %s, %s%s)' % (os.path.abspath(args.relay), sys.version.split()[0], sys.platform,
                                           ', real UDP sends' if args.udp else ''))
    if (LOBBY_STATE, LOBBY_ROOM) != (30, 15):
        print('what-if: lobby spectators get %d STATE and %d ROOM a second from each arena' % (LOBBY_STATE, LOBBY_ROOM))
    for title, mix in mixes.items():
        print('\n' + title)
        print('  players  rooms  lobby   in pkt/s  out pkt/s   out Mbit/s  dropped/s   CPU of one core')
        for p in [int(x) for x in args.players.split(',')]:
            r = run(mod, p, args.udp, args.seconds, mix)
            print('  %7d  %5d  %5d  %9.0f  %9.0f  %11.1f  %9.0f   %6.0f %%' % (
                r['players'], r['rooms'], r['lobby'], r['in_pps'], r['out_pps'], r['out_mbps'], r['dropped'],
                r['cpu'] * 100))
            sys.stdout.flush()
            if r['cpu'] > 3.0:
                print('  (stopping this mix: far past one core)')
                break


if __name__ == '__main__':
    main()
