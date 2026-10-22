// Desktop test of arena rooms (protocol v8): one host, six visitors and a spectator, each a real Net instance,
// talking through a local copy of the community relay (server/projectarena_relay.py on 127.0.0.1:47781).
// Build / run: tools/run_room_test.bat
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>
#include "../src/avatar.h"
#include "../src/net.h"
#include "../src/voice_codec.h"
#include <algorithm>
#include <cstdlib>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#define SOCKET int
#define closesocket close
#endif

bool netAcquireMulticastLock() { return false; }

static int g_fail = 0, g_pass = 0;
#define CHECK(c, msg) do { if (c) { ++g_pass; std::printf("  ok   %s\n", msg); } else { ++g_fail; std::printf("  FAIL %s  (line %d)\n", msg, __LINE__); } } while (0)

struct Node {
    Net net;
    Game game;
    PlayerInput local;
    explicit Node(const char* name, float x) {
        net.init();
        net.setName(name);
        game.reset(true);
        local.headTracked = true;
        local.head = {Quat{}, {x, 1.6f, -4.2f}};
        for (int h = 0; h < 2; ++h) { local.hand[h].tracked = true; local.hand[h].grip = {Quat{}, {x + (h ? 0.2f : -0.2f), 1.1f, -4.0f}}; }
    }
};

static void pump(std::vector<std::unique_ptr<Node>>& ns, double seconds) {
    const double t0 = (double)std::chrono::steady_clock::now().time_since_epoch().count() * 1e-9;
    for (;;) {
        for (auto& n : ns) {
            PlayerInput in[2];
            if (n->net.exchange(n->local, in, n->game, 1.f / 90.f)) { n->game.step(1.f / 90.f, in); n->net.sendState(n->game); }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(11));
        double t = (double)std::chrono::steady_clock::now().time_since_epoch().count() * 1e-9;
        if (t - t0 > seconds) break;
    }
}

static int waitingCount(const RoomInfo& r) { return (int)r.members.size(); }

// The Crowd: a phone on the web page, as far as the relay can tell (a WebSocket client: masked text frames)
struct Fan {
    SOCKET s = (SOCKET)-1;
    std::string got;   // every text message received, one per line
    bool open(uint16_t port) {
        s = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(0x7F000001);
        if (connect(s, (sockaddr*)&a, sizeof a) != 0) return false;
#ifdef _WIN32
        DWORD ms = 100;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof ms);
#else
        timeval tv{0, 100000};
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
        const char* req = "GET /ws HTTP/1.1\r\nHost: crowd\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
        send(s, req, (int)std::strlen(req), 0);
        std::string head;
        char c;
        for (int i = 0; i < 4000 && head.find("\r\n\r\n") == std::string::npos; ++i)
            if (recv(s, &c, 1, 0) == 1) head += c;
        return head.find(" 101 ") != std::string::npos;
    }
    void say(const std::string& text) {
        std::string f;
        f += (char)0x81; f += (char)(0x80 | text.size());
        const char mask[4] = {0x11, 0x22, 0x33, 0x44};
        f.append(mask, 4);
        for (size_t i = 0; i < text.size(); ++i) f += (char)(text[i] ^ mask[i & 3]);
        send(s, f.data(), (int)f.size(), 0);
    }
    void read() {   // whatever has arrived (frames from the server are unmasked)
        std::string buf;
        char tmp[4096];
        for (;;) { int n = recv(s, tmp, sizeof tmp, 0); if (n <= 0) break; buf.append(tmp, n); }
        size_t i = 0;
        while (i + 2 <= buf.size()) {
            size_t n = (uint8_t)buf[i + 1] & 0x7F, off = 2;
            if (n == 126) { if (i + 4 > buf.size()) break; n = ((uint8_t)buf[i + 2] << 8) | (uint8_t)buf[i + 3]; off = 4; }
            if (i + off + n > buf.size()) break;
            got += buf.substr(i + off, n) + "\n";
            i += off + n;
        }
    }
    ~Fan() { if (s != (SOCKET)-1) closesocket(s); }
};

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* server = "127.0.0.1:47781";
    std::vector<std::unique_ptr<Node>> ns;
    const char* names[] = {"HOST", "ONE", "TWO", "THREE", "FOUR", "FIVE", "SIX", "WATCHER"};
    for (int i = 0; i < 8; ++i) ns.emplace_back(new Node(names[i], -4.5f + i));
    for (auto& n : ns) n->net.goOnline(server);
    pump(ns, 1.0);
    bool ready = true;
    for (auto& n : ns) ready &= n->net.onlineReady();
    CHECK(ready, "everyone is connected to the relay");
    CHECK(std::strcmp(ns[0]->net.latestVersion(), "9.9.9") == 0, "the relay's welcome says the current release (UPDATE AVAILABLE)");
    Node& H = *ns[0];
    H.net.host();
    pump(ns, 0.3);
    std::printf("[host]\n");
    CHECK(H.net.localWaiting() && H.net.room().slot[0] == 0, "the host starts in the room's waiting area, not in the tank");
    H.net.queue(true);
    pump(ns, 0.5);
    CHECK(H.net.room().slot[0] == H.net.session() && H.net.room().bot, "the host's JOIN QUEUE: into the tank against the bot");
    for (int k = 0; k < 40 && ns[1]->net.games().empty(); ++k) pump(ns, 0.1);
    std::printf("[join]\n");
    if (ns[1]->net.games().empty()) { std::printf("  FAIL no beacon from the host\n"); return 1; }
    const uint32_t hostIp = ns[1]->net.games()[0].ip;
    ns[1]->game.pl[0].isBot = ns[1]->game.pl[1].isBot = true;   // as if coming from the lobby's two bots
    ns[1]->net.join(hostIp);
    pump(ns, 1.0);
    CHECK(ns[1]->net.localWaiting() && H.net.room().slot[1] == 0, "a visitor (even asking to play now) arrives in the waiting area");
    ns[1]->net.queue(true);
    pump(ns, 1.0);
    CHECK(H.net.room().slot[0] == H.net.session() && H.net.room().slot[1] == ns[1]->net.session(), "their JOIN QUEUE takes the bot's place in the tank");
    CHECK(ns[1]->net.localSlot() == 1, "the visitor knows it plays slot 1");
    std::printf("[ping between the host and a player]\n");
    pump(ns, 2.5);
    CHECK(ns[1]->net.pingMs() >= 0.f && ns[1]->net.pingMs() < 250.f, "the player measures its round trip to the host (PING / PONG)");
    CHECK(H.net.pingMs() >= 0.f && H.net.pingMs() < 250.f, "and the host its round trip to the player");
    std::printf("    ping: player -> host %.1f ms, host -> player %.1f ms (local relay)\n", ns[1]->net.pingMs(), H.net.pingMs());
    std::printf("[player statistics on the server]\n");
    {
        uint8_t k0[16], k1[16];
        std::memset(k0, 0x11, 16); std::memset(k1, 0x22, 16);
        H.net.setStatsKey(k0); ns[1]->net.setStatsKey(k1);
        pump(ns, 3.5);   // the keys go to the relay; the host names its tank (every 3 s)
        Net::MatchStats st[2];
        st[0].throws = 10; st[0].onTarget = 6; st[0].hits = 3; st[0].blocks = 2; st[0].faced = 5; st[0].catches = 1; st[0].topSpeed = 22.5f;
        st[1].throws = 8; st[1].onTarget = 4; st[1].hits = 2; st[1].blocks = 1; st[1].faced = 4; st[1].topSpeed = 19.f;
        const uint8_t score[4] = {7, 4, 0, 0};
        H.net.reportMatch(0, st, score);
        ns[1]->net.confirmMatch(0, score);   // the other player's game agrees (the host's result alone counts for nobody)
        pump(ns, 0.4);
        Node& W = *ns[7];
        const uint32_t hv = H.net.roomHostVip();
        W.net.requestStats(hv);
        pump(ns, 0.5);
        Net::CareerStats c[2];
        const bool got = W.net.statsFor(hv, c);
        CHECK(got && c[0].known && c[0].matches == 1 && c[0].wins == 1 && c[0].throws == 10 && c[0].onTarget == 6 &&
              c[1].matches == 1 && c[1].wins == 0 && c[1].hits == 2, "a reported match reaches the players' careers, and anyone can read them");
        CHECK(got && std::fabs(c[0].topSpeed - 22.5f) < 0.02f && !std::strcmp(c[0].name, "HOST") && !std::strcmp(c[1].name, "ONE"),
              "with the top speed and the names");
        CHECK(ns[1]->net.roomHostVip() == hv, "a player in the room asks for the same host's players");
        ns[1]->net.requestMyStats();   // MENU > MY STATS
        pump(ns, 0.4);
        Net::CareerStats me[2];
        CHECK(ns[1]->net.statsFor(Net::kMyStats, me) && me[0].matches == 1 && me[0].hits == 2 && !std::strcmp(me[0].name, "ONE"),
              "a player reads their own career (MY STATS)");
        ns[1]->net.deleteMyStats();
        pump(ns, 0.7);
        ns[1]->net.requestMyStats();
        pump(ns, 0.4);
        CHECK(ns[1]->net.statsFor(Net::kMyStats, me) && me[0].matches == 0, "DELETE MY STATS erases them on the server");
    }
    std::printf("[fist bump over the network]\n");
    {
        Node& V = *ns[1];
        CHECK(H.game.match == MatchState::Meet, "two people in the tank: they meet in the centre");
        CHECK(!V.game.pl[0].isBot && !V.game.pl[1].isBot,
              "the visitor (who came from practice / the lobby) does not take the host for a bot (it flickered)");
        // knuckles 8 cm apart in the middle of the court, both triggers held: the visitor's hand reaches the host
        // only through its INPUT packets (the 2026-10-17 test: the bump never registered)
        auto fistAt = [&](Node& n, const Vec3& at, const Vec3& facing) {   // the visible fist's front at `at`
            n.local.hand[1].trigger = 1;
            n.local.hand[1].grip.q = fromTo({0, 0, -1}, facing);
            n.local.hand[1].grip.p = at - n.local.hand[1].grip.q.rotate(H.game.tun.fistFront);
        };
        const PlayerInput hostKeep = H.local, visKeep = V.local;
        fistAt(H, {-0.01f, 1.2f, 0.f}, {1, 0, 0});
        fistAt(V, {0.01f, 1.2f, 0.f}, {-1, 0, 0});
        pump(ns, 1.2);
        CHECK(H.game.match == MatchState::Countdown || H.game.match == MatchState::InProgress,
              "the host sees the bump and starts the countdown well before the 10 s fallback");
        CHECK(V.game.match == H.game.match, "the visitor's game follows");
        H.local = hostKeep; V.local = visKeep;
    }
    for (int i = 2; i <= 6; ++i) ns[i]->net.join(hostIp, false);   // JOIN ROOM: wait outside
    pump(ns, 1.5);
    CHECK(waitingCount(H.net.room()) == 4, "four more wait outside the tank");
    CHECK(ns[6]->net.mode() == Net::Mode::Offline, "a seventh person is refused (arena full)");
    bool spots[4] = {false, false, false, false}, allWaiting = true;
    for (int i = 2; i <= 5; ++i) { allWaiting &= ns[i]->net.localWaiting(); spots[ns[i]->net.localSpot() & 3] = true; }
    CHECK(allWaiting && spots[0] && spots[1] && spots[2] && spots[3], "each waiting player has their own holding spot");
    CHECK(waitingCount(ns[2]->net.room()) == 4 && ns[2]->net.room().members[0].in.avatar.suit == ns[2]->net.room().members[0].in.avatar.suit,
          "waiting players see each other (poses and looks in ROOM)");
    std::printf("[queue]\n");
    ns[2]->net.queue(true); pump(ns, 0.3);
    ns[3]->net.queue(true); pump(ns, 0.3);
    ns[4]->net.queue(true); pump(ns, 0.5);
    CHECK(ns[2]->net.localQueuePos() == 0 && ns[3]->net.localQueuePos() == 1, "two join the queue in order");
    CHECK(ns[4]->net.localQueuePos() < 0 && ns[4]->net.mode() == Net::Mode::Client, "a third is not queued (max 2) but stays in the room");
    std::printf("[spectator presence]\n");
    Node& S = *ns[7];
    S.net.setPresence(1, {0, 3, 6}, hostIp);
    S.net.spectate();
    pump(ns, 1.0);
    bool seen = false;
    for (const RoomInfo::Watcher& w : H.net.room().watchers) seen |= w.id == S.net.session() && w.where == 1;
    CHECK(seen, "the host lists the spectator near its arena (blue marker)");
    if (!S.net.matches().empty()) std::printf("  (spectator: %zu matches, room members %zu, slots %u %u)\n", S.net.matches().size(),
                                             S.net.matches()[0].room.members.size(), S.net.matches()[0].room.slot[0], S.net.matches()[0].room.slot[1]);
    CHECK(!S.net.matches().empty() && waitingCount(S.net.matches()[0].room) == 4, "the spectator sees the waiting players (red markers)");
    std::printf("[names: report / mute a player]\n");
    {
        auto has = [](const std::vector<Net::Person>& v, const char* name, uint8_t role) {
            for (const Net::Person& p : v) if (std::strcmp(p.name, name) == 0 && p.role == role) return true;
            return false;
        };
        const char* n2 = H.net.room().nameOf(ns[2]->net.session());
        CHECK(n2 && std::strcmp(n2, "TWO") == 0, "the host knows a visitor's name from their JOIN");
        const std::vector<Net::Person> two = ns[2]->net.peopleHere();
        bool self = false;
        for (const Net::Person& p : two) self |= p.id == ns[2]->net.session();
        CHECK(has(two, "HOST", Net::VOICE_PLAYER) && has(two, "ONE", Net::VOICE_PLAYER) && has(two, "THREE", Net::VOICE_WAITING),
              "a waiting player sees everyone's names and roles in ROOM");
        CHECK(has(two, "WATCHER", Net::VOICE_SPECTATOR) && !self, "... the spectator at the arena too, but not themselves");
        CHECK(has(S.net.peopleHere(), "TWO", Net::VOICE_WAITING) && has(S.net.peopleHere(), "HOST", Net::VOICE_PLAYER),
              "a spectator at the arena sees the room's names");
        CHECK(H.net.hereName() == "HOST" && ns[2]->net.hereName() == "HOST", "everyone knows the room's name");
    }
    std::printf("[rotation]\n");
    H.game.match = MatchState::MatchSummary;   // the match ends
    pump(ns, 7.5);
    const RoomInfo& r = H.net.room();
    CHECK(r.slot[0] == ns[2]->net.session() && r.slot[1] == ns[3]->net.session(), "both players leave; the queue enters the tank");
    CHECK(H.net.localWaiting() && ns[1]->net.localWaiting(), "the two who played wait outside");
    CHECK(waitingCount(r) == 4 && ns[2]->net.localSlot() == 0 && ns[3]->net.localSlot() == 1, "the room is consistent on every headset");
    CHECK(H.game.match != MatchState::MatchSummary && !H.game.pl[1].isBot, "a new match of two humans starts");
    std::printf("[leaving]\n");
    ns[3]->net.leave();
    pump(ns, 0.8);
    CHECK(H.net.room().slot[0] == ns[2]->net.session() && H.net.room().bot, "a player leaving mid-match: the bot takes the place");
    ns[1]->net.queue(true);
    pump(ns, 0.8);
    CHECK(H.net.room().slot[1] == ns[1]->net.session(), "someone queuing while the bot plays goes straight in");
    std::printf("[forfeit]\n");
    {
        const uint32_t was = H.net.room().slot[1];
        Node* p1 = nullptr;
        for (auto& n : ns) if (n->net.session() == was) p1 = n.get();
        if (p1) {
            p1->net.forfeit();
            pump(ns, 0.8);
            CHECK(p1->net.localWaiting() && H.net.room().slot[1] != was, "FORFEIT MATCH: the player goes back to the waiting area");
            p1->net.queue(true);
            pump(ns, 0.8);
            CHECK(H.net.room().slot[1] == was, "and can queue back in");
        } else {
            CHECK(false, "a player in slot 1 to forfeit");
        }
    }
    std::printf("[voice]\n");
    {
        int maxErr = 0;
        for (int v = -32000; v <= 32000; v += 97) {
            int d = std::abs(ulawDecode(ulawEncode((int16_t)v)) - v);
            maxErr = std::max(maxErr, d * 100 / std::max(1, std::abs(v) + 64));
        }
        CHECK(maxErr <= 7, "mu-law round trip within a few percent");
    }
    for (auto& n : ns) n->net.voiceInbox().clear();
    uint8_t frame[Net::kVoiceSamples];
    std::memset(frame, 0x55, sizeof frame);
    Node& waiter = *ns[4];                    // FOUR waits outside
    waiter.net.sendVoice(frame);
    S.net.setPresence(0, {0, 1.6f, 0}, 0);    // the spectator moves to the lobby
    pump(ns, 0.6);
    for (auto& n : ns) n->net.voiceInbox().clear();
    waiter.net.sendVoice(frame);
    pump(ns, 0.4);
    auto heard = [&](Node& n, uint8_t role) {
        for (const Net::VoiceFrame& f : n.net.voiceInbox()) if (f.speaker == waiter.net.session() && f.role == role) return true;
        return false;
    };
    CHECK(heard(H, Net::VOICE_WAITING) && heard(*ns[2], Net::VOICE_WAITING), "a waiting player is heard by the host and the tank players");
    CHECK(waiter.net.voiceInbox().empty(), "nobody hears themselves");
    CHECK(S.net.voiceInbox().empty(), "a spectator in the lobby hears nothing from the arena");
    S.net.setPresence(1, {0, 3, 6}, hostIp);
    pump(ns, 0.6);
    for (auto& n : ns) n->net.voiceInbox().clear();
    ns[2]->net.sendVoice(frame);              // TWO plays in the tank
    pump(ns, 0.4);
    bool specHeard = false;
    for (const Net::VoiceFrame& f : S.net.voiceInbox()) specHeard |= f.speaker == ns[2]->net.session() && f.role == Net::VOICE_PLAYER;
    CHECK(specHeard, "a spectator standing at the arena hears the players");
    for (auto& n : ns) n->net.voiceInbox().clear();
    S.net.sendVoice(frame);
    pump(ns, 0.4);
    bool fromSpec = false;
    for (const Net::VoiceFrame& f : ns[2]->net.voiceInbox()) fromSpec |= f.speaker == S.net.session() && f.role == Net::VOICE_SPECTATOR;
    CHECK(fromSpec, "players hear a spectator at their arena, tagged as a spectator (for MUTE SPECTATORS)");
    std::printf("[team cameras]\n");
    {
        Node& S2 = *ns[6];   // SIX (refused earlier) spectates now
        S2.net.setPresence(0, {1, 1.6f, 0}, 0);
        S2.net.spectate();
        pump(ns, 1.0);
        std::vector<uint8_t> blob = {1, 9, 9, 9, 9, 8, 8, 8, 8, 7, 7, 7, 7, 6, 6, 6, 6, 1, 1, 2};
        S.net.shareCameras("ALPHA", blob);
        pump(ns, 0.5);
        char team[17];
        std::vector<uint8_t> got;
        bool ok = S2.net.takeCameras(team, got) && std::strcmp(team, "ALPHA") == 0 && got == blob;
        CHECK(ok, "a spectator's shared cameras reach another spectator through the arena host");
        CHECK(!S.net.takeCameras(team, got), "the sender does not get its own cameras back");
    }
    std::printf("[play now / bot off]\n");
    {
        Node& B = *ns[6];   // SIX hosts a second arena with the bot switched off
        B.net.setBotFill(false);
        B.net.host();
        pump(ns, 0.5);
        B.net.queue(true);
        pump(ns, 1.0);
        CHECK(!B.net.room().bot && !B.game.pl[1].isBot && B.net.localSlot() == 0, "bot off: the host waits in the tank with no bot");
        uint32_t ip2 = 0;
        for (const LanGame& lg : ns[5]->net.games()) if (std::strcmp(lg.name, "SIX") == 0) ip2 = lg.ip;
        CHECK(ip2 != 0, "the second arena is listed");
        ns[5]->net.join(ip2, false);
        pump(ns, 1.0);
        ns[5]->net.queue(true);         // JOIN QUEUE: the free place
        pump(ns, 1.0);
        CHECK(B.net.room().slot[1] == ns[5]->net.session() && ns[5]->net.localSlot() == 1, "JOIN QUEUE takes the free place in the tank");
        ns[4]->net.join(ip2, false);
        pump(ns, 1.0);
        ns[4]->net.queue(true);         // the tank is busy: queued
        pump(ns, 1.0);
        CHECK(ns[4]->net.localWaiting() && ns[4]->net.localQueuePos() == 0, "JOIN QUEUE into a busy tank waits first in line");
        ns[3]->net.join(ip2, false);   // JOIN ROOM: waits, not queued
        pump(ns, 1.0);
        CHECK(ns[3]->net.localWaiting() && ns[3]->net.localQueuePos() < 0, "JOIN ROOM waits outside without queuing");
        std::printf("[vote to remove]\n");
        const uint32_t target = ns[3]->net.session();
        CHECK(!ns[4]->net.voteKick(B.net.session()), "nobody can vote the host out of its own arena");
        CHECK(ns[4]->net.voteKick(target), "a waiting player votes to remove someone");
        pump(ns, 0.6);
        CHECK(ns[3]->net.inRoom(), "one vote of the two needed (4 people: more than half of the other 3) is not enough");
        CHECK(!ns[4]->net.voteKick(target), "one vote each");
        CHECK(ns[5]->net.voteKick(target), "a second person votes");
        pump(ns, 1.0);
        bool gone = !ns[3]->net.inRoom();
        for (const RoomInfo::Member& m : B.net.room().members) gone &= m.id != target;
        CHECK(gone, "two votes: removed from the arena");
        ns[3]->net.join(ip2, false);   // back again in the same session: turned away
        pump(ns, 1.0);
        CHECK(!ns[3]->net.inRoom(), "a removed player can't come back to that arena this session");
        CHECK(ns[4]->net.inRoom() && ns[5]->net.inRoom(), "everyone else stays");
    }
    std::printf("[the crowd]\n");
    {
        Node& B = *ns[6];   // SIX's arena: FIVE plays, FOUR waits; WATCHER spectates
        Net::LiveInfo li;
        li.state = 2; li.score[0] = 3; li.score[1] = 1; li.nation[0] = 1; li.nation[1] = 7;
        std::snprintf(li.name[0], 17, "SIX"); std::snprintf(li.name[1], 17, "FIVE");
        li.clock = 95.f;
        li.st[0].throws = 20; li.st[0].onTarget = 12; li.st[0].strikes = 2; li.st[0].topSpeed = 25.f; li.st[1].hits = 4;
        li.round = 2;
        B.net.setLive(li);
        pump(ns, 1.6);
        Fan fan;
        const bool opened = fan.open(47782);
        CHECK(opened, "a phone connects to the relay's crowd port (WebSocket)");
        pump(ns, 1.2);
        fan.read();
        const size_t at = fan.got.find("\"host\":\"SIX\"");
        CHECK(at != std::string::npos && fan.got.find("\"s\":[3,1,0,0]") != std::string::npos,
              "the host's LIVE reaches the phones' list: its match and score");
        CHECK(fan.got.find("\"throws\":20,\"on\":12") != std::string::npos && fan.got.find("\"strikes\":2") != std::string::npos &&
              fan.got.find("\"top\":90") != std::string::npos && fan.got.find("\"hits\":4") != std::string::npos &&
              fan.got.find("\"rnd\":2") != std::string::npos, "and this match so far: throws, accuracy, strikes, top speed, round");
        std::string id;
        const size_t k = fan.got.rfind("\"id\":\"", at);
        if (k != std::string::npos) id = fan.got.substr(k + 6, 8);
        fan.say("{\"t\":\"watch\",\"id\":\"" + id + "\"}");
        for (int i = 0; i < 6; ++i) {
            fan.say("{\"t\":\"cheer\",\"side\":0,\"n\":3}");
            fan.say("{\"t\":\"flag\",\"side\":1}");
            pump(ns, 0.25);
        }
        pump(ns, 0.6);
        const CrowdInfo* hc = B.net.crowd();
        CHECK(hc && hc->fans == 1 && hc->hype[0] > 0 && hc->hype[1] == 0 && hc->waves[1] == 1,
              "the host hears its fan: one fan, the cheered end's hype, a flag waved for the other end");
        const CrowdInfo* pc = ns[5]->net.crowd();
        CHECK(pc && pc->fans == 1 && pc->hype[0] > 0, "the tank player hears the crowd too (the host passes it on)");
        const CrowdInfo* wc = ns[4]->net.crowd();
        CHECK(wc && wc->fans == 1, "and the waiting player");
        uint32_t ip2 = 0;
        for (const LanGame& lg : S.net.games()) if (std::strcmp(lg.name, "SIX") == 0) ip2 = lg.ip;
        const CrowdInfo* sc = S.net.crowd(ip2);
        CHECK(sc && sc->fans == 1, "a spectator watching that arena gets its crowd");
        CHECK(!ns[1]->net.crowd() || ns[1]->net.crowd()->fans == 0, "another arena's players don't");
        fan.read();
        CHECK(fan.got.find("\"t\":\"m\"") != std::string::npos && fan.got.find("\"fans\":1") != std::string::npos,
              "the phone follows the match it watches");
    }
    std::printf("[match options and the host's kick]\n");
    {
        Node& B = *ns[6];   // SIX hosts; FIVE plays, FOUR waits
        B.game.tun.shields = false; B.game.tun.basicMatchTime = 300.f; B.game.tun.knuckleMode = 0;
        pump(ns, 1.5);
        const Tunables& t = ns[5]->game.tun;
        CHECK(!t.shields && t.basicMatchTime == 300.f && t.knuckleMode == 0, "the host's MATCH OPTIONS reach the player in its tank");
        bool listed = false;
        for (const LanGame& lg : ns[1]->net.games())
            if (std::strcmp(lg.name, "SIX") == 0) listed = !lg.shields && lg.matchTime == 300.f && lg.knuckleMode == 0;
        CHECK(listed, "and the games list shows them (HOTDOG, 5 minutes, no knuckle guards)");
        const uint32_t four = ns[4]->net.session();
        CHECK(!ns[5]->net.hostKick(four), "only the host can remove someone directly");
        CHECK(B.net.hostKick(four), "the host removes a waiting player at once");
        pump(ns, 1.0);
        CHECK(!ns[4]->net.inRoom(), "removed by the host: out of the arena");
        uint32_t ip2 = 0;
        for (const LanGame& lg : ns[4]->net.games()) if (std::strcmp(lg.name, "SIX") == 0) ip2 = lg.ip;
        ns[4]->net.join(ip2, false);
        pump(ns, 1.0);
        CHECK(!ns[4]->net.inRoom(), "and can't come back this session");
        CHECK(!B.net.hostKick(B.net.session()), "the host can't remove itself");
    }
    for (auto& n : ns) { if (n->net.online()) n->net.goOffline(); n->net.shutdown(); }
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
