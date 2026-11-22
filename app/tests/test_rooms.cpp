// Desktop test of arena rooms (protocol v9): one host, six visitors and a spectator, each a real Net instance,
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
        net.setAppVersion("0.3.11");   // run_room_test.py checks the relay logs it
        net.setName(name);
        game.reset(true);
        local.headTracked = true;
        local.head = {Quat{}, {x, 1.6f, -4.2f}};
        for (int h = 0; h < 2; ++h) { local.hand[h].tracked = true; local.hand[h].grip = {Quat{}, {x + (h ? 0.2f : -0.2f), 1.1f, -4.0f}}; }
    }
};

static bool g_still = false;   // (IDLE ROOMS: everyone's head bobs a little, as a person's does, unless set)

static void pump(std::vector<std::unique_ptr<Node>>& ns, double seconds) {
    const double t0 = (double)std::chrono::steady_clock::now().time_since_epoch().count() * 1e-9;
    for (;;) {
        for (auto& n : ns) {
            if (!g_still) n->local.head.p.y = 1.6f + 0.04f * (float)std::sin((double)std::chrono::steady_clock::now().time_since_epoch().count() * 1.3e-9);
            PlayerInput in[kMaxPlayers];
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
        pump(ns, 2.0);   // (offered 0.2 s, the bump's 0.8 s confirmation, then the countdown)
        CHECK(H.game.match == MatchState::Countdown || H.game.match == MatchState::InProgress,
              "the host sees the bump and starts the countdown well before the 10 s fallback");
        CHECK(V.game.match == H.game.match, "the visitor's game follows");
        H.local = hostKeep; V.local = visKeep;
    }
    std::printf("[COUNTRY KIT over the network]\n");
    {
        Node& V = *ns[1];
        AvatarDesc mine = avatar::preset(2);   // VANGUARD, in CANADA's colours and flags (MY OUTFIT)
        avatar::set(mine, avatar::F_NATION, 2);
        avatar::setOwnKit(mine, true);
        AvatarDesc jersey = avatar::preset(3);   // the host: NIGHT RIDER under CANADA's team jersey
        avatar::set(jersey, avatar::F_NATION, 2);
        const PlayerInput hostKeep = H.local, visKeep = V.local;
        V.local.avatar = avatar::sentLook(mine);
        H.local.avatar = avatar::sentLook(jersey);
        pump(ns, 0.6);
        const AvatarDesc& atHost = H.game.pl[1].in.avatar;
        CHECK(avatar::ownKit(atHost) && avatar::uniform(atHost).suit == 2 && avatar::nationOf(atHost) == 2,
              "a player's own outfit with their country (MY OUTFIT) reaches the host");
        const AvatarDesc& atVisitor = V.game.pl[0].in.avatar;
        CHECK(!avatar::ownKit(atVisitor) && avatar::uniform(atVisitor).suit == 1 && avatar::nationOf(atVisitor) == 2,
              "and the host's team jersey reaches the player as the team jersey");
        V.local.avatar = avatar::sentLook(jersey);
        V.local.avatar.pattern = (uint8_t)(V.local.avatar.pattern | avatar::kOwnKitBit);
        avatar::setNation(V.local.avatar, 0);   // (the bit with no country, which 0.3.22 never sends: cleared on arrival)
        pump(ns, 0.4);
        CHECK(!avatar::ownKit(H.game.pl[1].in.avatar) && H.game.pl[1].in.avatar.pattern < 0x80, "without a country the bit is cleared");
        H.local = hostKeep; V.local = visKeep;
        pump(ns, 0.2);
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
    std::printf("[MUTED PLAYERS: a code for everyone here (0.3.24)]\n");
    {   // (review 2026-11-21: a guest hears only from the host, so it could ask the server for the host's code alone; the
        // host's ROOM now says whose virtual address is whose)
        Node& A = *ns[2];   // TWO, waiting
        Node& B = *ns[3];   // THREE, waiting
        uint8_t kA[16], kB[16], kS[16];
        std::memset(kA, 0x33, 16); std::memset(kB, 0x44, 16); std::memset(kS, 0x55, 16);
        A.net.setStatsKey(kA); B.net.setStatsKey(kB); S.net.setStatsKey(kS);
        pump(ns, 0.8);   // (the keys reach the relay; the host's ROOM carries everyone's address)
        A.net.askMuteCodes({B.net.session(), ns[1]->net.session(), S.net.session(), H.net.session()});
        S.net.askMuteCodes({B.net.session()});
        pump(ns, 0.8);
        const uint64_t ab = A.net.muteCode(B.net.session()), a1 = A.net.muteCode(ns[1]->net.session());
        const uint64_t as = A.net.muteCode(S.net.session()), ah = A.net.muteCode(H.net.session()), sb = S.net.muteCode(B.net.session());
        CHECK(ab && a1 && as && ah, "a waiting player gets the server's code for another waiting player, a tank player, the spectator and the host");
        CHECK(sb && sb != ab, "a spectator at the arena gets one too, its own (another game's code for the same person differs)");
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
    {   // ROOM TALK: PRIVATE (owner, 2026-11-02): the spectator sees the match but doesn't hear the room; the room still does
        Node* host = nullptr;
        for (auto& n : ns) if (n->net.mode() == Net::Mode::Host && n->net.room().slot[0]) host = n.get();
        CHECK(host != nullptr, "(the room's host)");
        if (host) {
            host->net.setPrivateTalk(true);
            for (auto& n : ns) n->net.voiceInbox().clear();
            ns[2]->net.sendVoice(frame);
            pump(ns, 0.4);
            bool specHears = false, roomHears = false;
            for (const Net::VoiceFrame& f : S.net.voiceInbox()) specHears |= f.speaker == ns[2]->net.session();
            for (const Net::VoiceFrame& f : waiter.net.voiceInbox()) roomHears |= f.speaker == ns[2]->net.session();
            CHECK(!specHears && roomHears, "ROOM TALK: PRIVATE: the spectator hears nothing; the one waiting in the room still does");
            host->net.setPrivateTalk(false);
        }
    }
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
    std::printf("[FOUR CORNERS and DIAMOND online (0.3.12, network version 9)]\n");
    {
        std::vector<std::unique_ptr<Node>> fs;
        const char* fnames[] = {"QUAD HOST", "QUAD ONE", "QUAD TWO", "QUAD THREE", "DIAMOND HOST", "DIAMOND ONE"};
        for (int i = 0; i < 6; ++i) fs.emplace_back(new Node(fnames[i], -3.f + i));
        for (auto& n : fs) n->net.goOnline(server);
        pump(fs, 1.0);
        Node& Q = *fs[0];
        Q.game.tun.experimental = true; Q.game.tun.expRoom = 1; Q.game.tun.players = 4; Q.game.tun.teamMode = TEAMS_2V2;
        Q.net.host();
        Node& D = *fs[4];
        D.game.tun.experimental = true; D.game.tun.expRoom = 0;
        D.net.host();
        pump(fs, 1.0);
        uint32_t qip = 0, dip = 0;
        bool listed = false;
        for (const LanGame& lg : fs[1]->net.games()) {
            if (std::strcmp(lg.name, "QUAD HOST") == 0) { qip = lg.ip; listed = lg.experimental && lg.expRoom == 1 && lg.players == 4 && lg.teamMode == TEAMS_2V2; }
            if (std::strcmp(lg.name, "DIAMOND HOST") == 0) dip = lg.ip;
        }
        CHECK(listed, "a FOUR CORNERS room says so in the games list (4 players, 2 v 2)");
        for (int i = 1; i <= 2; ++i) fs[i]->net.join(qip, false);
        fs[5]->net.join(dip, false);
        pump(fs, 1.0);
        Q.net.queue(true);
        for (int i = 1; i <= 2; ++i) fs[i]->net.queue(true);
        pump(fs, 2.0);
        CHECK(Q.game.np == 4 && Q.net.room().slot[0] && Q.net.room().slot[1] && Q.net.room().slot[2], "three people queue into a FOUR CORNERS tank");
        CHECK(Q.net.room().isBot(3) && Q.game.pl[3].isBot && fs[1]->game.pl[3].isBot, "the empty corner: a bot (host and visitors agree)");
        // MOVE PLAYERS (owner 2026-10-30): the host swaps itself into the bot's corner; the bot takes the host's
        const uint32_t hostId = Q.net.room().slot[0];
        CHECK(!fs[1]->net.hostSwap(0, 3, fs[1]->game), "only the host can move players");
        CHECK(Q.net.hostSwap(0, 3, Q.game), "the host swaps two corners");
        pump(fs, 1.0);
        CHECK(Q.net.room().slot[3] == hostId && !Q.net.room().slot[0] && Q.net.room().isBot(0) && Q.game.pl[0].isBot && !Q.game.pl[3].isBot,
              "the host plays the bot's corner, the bot the host's");
        CHECK(fs[1]->net.room().slot[3] == hostId && fs[1]->game.pl[0].isBot && fs[2]->net.room().isBot(0), "and the visitors see it so");
        Q.game.tun.teamMode = TEAMS_3V1;   // (from the next match: 3 v 1, the host THE ONE, owner 2026-10-30)
        fs[3]->net.join(qip, false);
        pump(fs, 1.0);
        fs[3]->net.queue(true);
        pump(fs, 2.5);
        bool four = Q.net.room().slot[3] != 0 && !Q.net.room().isBot(3);
        for (int i = 1; i <= 3; ++i) four &= fs[i]->net.localSlot() >= 0 && fs[i]->game.np == 4 && fs[i]->game.tun.players == 4;
        CHECK(four, "a fourth person takes the bot's corner: four people, every game with four players");
        bool agree = true;
        for (int i = 1; i <= 3; ++i)
            for (int p = 0; p < 4; ++p) agree &= length(fs[i]->game.pl[p].in.head.p - Q.game.pl[p].in.head.p) < 0.05f || p == fs[i]->net.localSlot();
        CHECK(agree, "everyone sees the same four players where the host does");
        {   // four playing, four more waiting (owner, 2026-11-02: is there room for them?); a ninth is turned away
            std::vector<std::unique_ptr<Node>> extra;
            const char* xn[] = {"WAIT ONE", "WAIT TWO", "WAIT THREE", "WAIT FOUR", "WAIT FIVE"};
            for (int i = 0; i < 5; ++i) { extra.emplace_back(new Node(xn[i], 4.f + i)); extra.back()->net.goOnline(server); }
            std::vector<std::unique_ptr<Node>>& all = fs;
            for (auto& n : extra) all.push_back(std::move(n));
            pump(all, 1.0);
            for (size_t i = all.size() - 5; i < all.size() - 1; ++i) all[i]->net.join(qip, false);
            pump(all, 1.5);
            all.back()->net.join(qip, false);
            pump(all, 1.5);
            int waiting = 0;
            for (const RoomInfo::Member& m : Q.net.room().members) waiting += m.role == RoomInfo::WAITING;
            const bool ninthIn = all.back()->net.mode() == Net::Mode::Client;
            char msg[112];
            std::snprintf(msg, sizeof msg, "a FOUR CORNERS room holds four playing and four waiting (%d waiting); a ninth is turned away", waiting);
            CHECK(waiting == 4 && !ninthIn, msg);
            for (size_t i = all.size() - 5; i < all.size(); ++i) { all[i]->net.leave(); }
            pump(all, 1.0);
            for (size_t i = all.size() - 5; i < all.size(); ++i) { if (all[i]->net.online()) all[i]->net.goOffline(); all[i]->net.shutdown(); }
            all.resize(all.size() - 5);
        }
        CHECK(fs[2]->game.tun.experimental && fs[2]->game.tun.expRoom == 1 && fs[2]->game.tun.teamMode == TEAMS_3V1, "and plays the host's rules (FOUR CORNERS, 3 v 1)");
        CHECK(Q.game.tun.theOne == Q.net.localSlot() && fs[2]->game.tun.theOne == Q.net.localSlot(), "3 v 1: the host is THE ONE (unless set to take turns)");
        CHECK(Q.net.localSlot() == 3 && Q.net.room().slot[0] == fs[3]->net.session(), "the fourth person took the bot's corner, where it was (corners stay)");
        {   // THE ONE goes with the player; the host can make another corner THE ONE, and it stays
            const uint32_t one = Q.net.room().slot[Q.game.tun.theOne];
            Q.net.hostSwap(3, 1, Q.game);
            pump(fs, 1.0);
            CHECK(Q.net.room().slot[Q.game.tun.theOne] == one && fs[2]->game.tun.theOne == Q.game.tun.theOne, "a swap: THE ONE goes with the player");
            CHECK(Q.net.hostMakeOne(2, Q.game), "the host makes another corner THE ONE");
            pump(fs, 1.0);
            const uint32_t two = Q.net.room().slot[2];
            Q.net.hostSwap(0, 3, Q.game);
            pump(fs, 1.0);
            CHECK(Q.game.tun.theOne == 2 && Q.net.room().slot[2] == two && fs[1]->game.tun.theOne == 2, "and it stays THE ONE through other swaps");
        }
        const size_t bytes = Net::snapshotBytes(Q.game);
        char msg[96];
        std::snprintf(msg, sizeof msg, "a four-player snapshot with a full event log fits the relay (%d of 1200 bytes)", (int)bytes);
        CHECK(bytes <= 1200, msg);
        fs[5]->net.queue(true);
        D.net.queue(true);
        pump(fs, 2.0);
        CHECK(fs[5]->game.tun.experimental && fs[5]->game.tun.expRoom == 0 && fs[5]->game.np == 2 && fs[5]->net.localSlot() >= 0,
              "a DIAMOND room: its player gets the host's EXPERIMENTAL rules");
        {   // FIREBALL BLITZ (0.3.13): its health, who plays, and its fireballs reach everyone; a full room fits the relay
            Game full; full.tun.experimental = true; full.tun.expRoom = ROOM_BLITZ; full.tun.players = 4;
            full.useArena(ARENA_OCTAGON, {}); full.resetWithBots(0xF);
            for (auto& row : full.fire) for (Ball& f : row) { f.state = BallState::Flying; f.pos = {1, 1, 1}; f.instigator = 0; }
            const size_t bytes = Net::snapshotBytes(full);
            char msg[112];
            std::snprintf(msg, sizeof msg, "FIREBALL BLITZ: four players and twelve fireballs with a full event log fit the relay (%d of 1200 bytes)", (int)bytes);
            CHECK(bytes <= 1200, msg);
            {   // a spectator's copy of the match it watches, from FIREBALL BLITZ to a DIAMOND room (owner, 2026-11-16: the
                // PC spectator drew DIAMOND's platforms at the ring's places, the players away from them)
                full.ringMask = 0xF; full.layoutRing(0xF);
                Game diamond; diamond.tun.experimental = true; diamond.tun.expRoom = ROOM_DIAMOND;
                diamond.useArena(ARENA_DIAMOND, {}); diamond.resetWithBots(2);
                Game seen;
                bool home = Net::snapshotInto(full, seen) && length(seen.tun.start[1] - full.tun.start[1]) < 1e-3f;
                home &= Net::snapshotInto(diamond, seen);
                for (int p = 0; p < 2; ++p)
                    home &= length(seen.tun.start[p] - diamond.tun.start[p]) < 1e-3f && length(seen.tun.spawner[p] - diamond.tun.spawner[p]) < 1e-3f;
                CHECK(home, "a watched match from FIREBALL BLITZ to DIAMOND: the players' places are the room's own again");
            }
            {   // BALL LOOK-AHEAD with four players (0.3.24): FOUR CORNERS hosts send the balls' spin, bounces and fouls
                // and their clock after the last byte older builds read; a player not hosting flies the balls on from it
                Game host; host.tun.experimental = true; host.tun.expRoom = ROOM_CORNERS; host.tun.players = 4;
                host.useArena(ARENA_FOURCORNERS, {}); host.resetWithBots(0);
                host.now = 12.5f;
                for (int p = 0; p < 4; ++p) {   // (everyone standing in their corner, facing the middle, the hands down)
                    PlayerInput& in = host.pl[p].in;
                    const Vec3 f = host.facing(p);
                    const Quat face = Quat::axisAngle({0, 1, 0}, std::atan2(-f.x, -f.z));
                    in.headTracked = true;
                    in.head = {face, host.tun.start[p] + Vec3{0, 1.65f, 0}};
                    for (int h = 0; h < 2; ++h) {
                        in.hand[h].tracked = true;
                        in.hand[h].trigger = 0;
                        in.hand[h].grip = {face, host.tun.start[p] + Vec3{0, 0.9f, 0} + face.rotate({h == 0 ? -0.25f : 0.25f, 0, 0})};
                    }
                }
                const Vec3 mine = host.tun.start[1], other = host.tun.start[3];
                auto shoot = [&](Game& g, const Vec3& from) {   // ball 2 flying at corner 1 from `from`, 10 m/s, curving
                    Ball& b = g.pl[2].ball;
                    b.state = BallState::Flying; b.instigator = 2; b.heldHand = -1; b.scale = 1.f;
                    b.pos = from;
                    const Vec3 to = mine + Vec3{0, 1.5f, 0} - from;
                    b.vel = normalize(Vec3{to.x, 0, to.z}) * 10.f;
                    b.spin = {0, 3.f, 0}; b.bounces = 2; b.fouled = true; b.homing = false;
                };
                shoot(host, {0, 1.5f, 0});
                Game seen, old;
                const bool read = Net::snapshotInto(host, seen) && seen.np == 4;
                const Ball& sb = seen.pl[2].ball;
                CHECK(read && std::fabs(seen.ahead.snapNow - 12.5f) < 1e-3f && std::fabs(sb.spin.y - 3.f) < 0.01f && sb.bounces == 2 && sb.fouled,
                      "four players: the snapshot carries each ball's spin, bounces and foul, and the host's clock (0.3.24)");
                CHECK(Net::snapshotInto(host, old, 4 * 7 + 4 + 1) && old.np == 4 && old.ahead.snapNow < 0 && old.pl[2].ball.spin.y == 0.f &&
                      old.tun.palmHold == seen.tun.palmHold, "... a host before 0.3.24 sends none: everything else read as before");
                // (0.3.25: THROW POWER and KNUCKLE RETURNS, the last byte in FOUR CORNERS; a 0.3.24 host sends none: REGULATION)
                host.tun.throwPower = 1; host.tun.knuckleReturns = 1;
                Game feel, feelOld;
                CHECK(Net::snapshotInto(host, feel) && feel.tun.naturalThrowNow() && feel.tun.punchReturnsNow() &&
                          Net::snapshotInto(host, feelOld, 1) && feelOld.tun.feelStandard() && feelOld.ahead.snapNow >= 0.f,
                      "FOUR CORNERS: THROW POWER and KNUCKLE RETURNS reach the players (0.3.25); a 0.3.24 host's: REGULATION, the rest as before");
                host.tun.throwPower = host.tun.knuckleReturns = 0;
                host.tun.expRoom = ROOM_BLITZ;
                Game blitz;
                CHECK(Net::snapshotInto(host, blitz) && blitz.pl[2].ball.spin.y == 0.f && blitz.ahead.fireKnown,
                      "... not in FIREBALL BLITZ, whose balls never fly (its fireballs have their own, below)");
                host.tun.expRoom = ROOM_CORNERS;
                const size_t four = Net::snapshotBytes(host);
                char m4[112];
                std::snprintf(m4, sizeof m4, "... a four-player snapshot with it and a full event log fits the relay (%d of 1200 bytes)", (int)four);
                CHECK(four <= 1200, m4);
                const float lead = 0.2f, dt = 1.f / 72;
                const Vec3 at = sb.pos;
                seen.lookAhead(1, lead, dt);
                const float flown = length(seen.pl[2].ball.pos - at);
                old.lookAhead(1, lead, dt);
                const float oldFlown = length(old.pl[2].ball.pos - at);
                char m5[128];
                std::snprintf(m5, sizeof m5, "... a ball from the middle at my corner is drawn ahead (%.2f m of %.2f); from an older host, as sent (%.2f)",
                              flown, 10.f * lead, oldFlown);
                CHECK(flown > 0.8f && flown < 2.1f && oldFlown < 1e-4f, m5);
                Game beside;   // (the same ball beside another corner: what that player does with it is drawn where it happens)
                shoot(host, other + host.facing(3) * 0.8f + Vec3{0, 1.5f, 0});
                Net::snapshotInto(host, beside);
                const Vec3 nearAt = beside.pl[2].ball.pos;
                beside.lookAhead(1, lead, dt);
                const float nearFlown = length(beside.pl[2].ball.pos - nearAt);
                char m6[112];
                std::snprintf(m6, sizeof m6, "... beside another player's corner, hardly ahead at all (%.2f m)", nearFlown);
                CHECK(nearFlown < flown / 3.f, m6);
            }
            {   // BALL LOOK-AHEAD in FIREBALL BLITZ (0.3.24): its hosts send each flying fireball's spin and bounces and
                // their clock; a player not hosting flies the fireballs on, and one at its last bounce goes out on time
                Game host; host.tun.experimental = true; host.tun.expRoom = ROOM_BLITZ; host.tun.players = 4;
                host.useArena(ARENA_OCTAGON, {}); host.resetWithBots(0);
                host.startMask = 0xF; host.ringMask = 0xF; host.layoutRing(0xF);
                for (int p = 0; p < 4; ++p) {
                    host.pl[p].health = 3;
                    PlayerInput& in = host.pl[p].in;
                    const Vec3 f = host.facing(p);
                    const Quat face = Quat::axisAngle({0, 1, 0}, std::atan2(-f.x, -f.z));
                    in.headTracked = true;
                    in.head = {face, host.tun.start[p] + Vec3{0, 1.65f, 0}};
                    for (int h = 0; h < 2; ++h) {
                        in.hand[h].tracked = true;
                        in.hand[h].trigger = 0;
                        in.hand[h].grip = {face, host.tun.start[p] + Vec3{0, 0.9f, 0} + face.rotate({h == 0 ? -0.25f : 0.25f, 0, 0})};
                    }
                }
                host.now = 31.25f;
                auto fireAt = [&](const Vec3& from, float vy, int bounces) {   // fireball 2/0 at place 1, 10 m/s, curving
                    Ball& f = host.fire[2][0];
                    f.state = BallState::Flying; f.instigator = 2; f.heldHand = -1; f.scale = 1.f; f.timer = 0;
                    f.pos = from;
                    const Vec3 to = host.tun.start[1] - from;
                    f.vel = normalize(Vec3{to.x, 0, to.z}) * 10.f;
                    f.vel.y = vy;
                    f.speedLimit = 10.f;
                    f.spin = {0, 2.f, 0}; f.bounces = bounces;
                };
                auto guest = [&](Game& g, size_t cut) {   // (a guest's game is in the octagon before the snapshot comes)
                    g.tun.experimental = true; g.tun.expRoom = ROOM_BLITZ; g.tun.players = 4;
                    g.useArena(ARENA_OCTAGON, {});
                    return Net::snapshotInto(host, g, cut);
                };
                fireAt({0, 1.0f, 0}, 0.f, 1);
                Game seen, old;
                const bool read = guest(seen, 0);
                const Ball& sf = seen.fire[2][0];
                CHECK(read && seen.ahead.fireKnown && std::fabs(seen.ahead.snapNow - 31.25f) < 1e-3f && std::fabs(sf.spin.y - 2.f) < 0.01f &&
                      sf.bounces == 1, "FIREBALL BLITZ: the snapshot carries each flying fireball's spin and bounces, and the host's clock (0.3.24)");
                CHECK(guest(old, 2 + 4) && !old.ahead.fireKnown && old.fire[2][0].state == BallState::Flying && old.fire[2][0].spin.y == 0.f,
                      "... a host before 0.3.24 sends none: the fireballs read as before");
                const float lead = 0.2f, dt = 1.f / 72;
                const Vec3 at = sf.pos;
                seen.lookAhead(1, lead, dt);
                const float flown = length(seen.fire[2][0].pos - at);
                old.lookAhead(1, lead, dt);
                const float oldFlown = length(old.fire[2][0].pos - at);
                char m7[128];
                std::snprintf(m7, sizeof m7, "... a fireball from the middle at my place is drawn ahead (%.2f m of %.2f); from an older host, as sent (%.2f)",
                              flown, 10.f * lead, oldFlown);
                CHECK(seen.fire[2][0].state == BallState::Flying && flown > 0.8f && flown < 2.1f && oldFlown < 1e-4f, m7);
                fireAt({0, 0.3f, 0}, -3.f, host.tun.fireBounces - 1);   // (about to hit the floor: its last bounce)
                Game last, more;
                guest(last, 0);
                last.lookAhead(1, lead, dt);
                fireAt({0, 0.3f, 0}, -3.f, 0);
                guest(more, 0);
                more.lookAhead(1, lead, dt);
                CHECK(last.fire[2][0].state != BallState::Flying && more.fire[2][0].state == BallState::Flying,
                      "... one at its last bounce goes out where it lands, one with bounces left hops on");
                host.fire[2][0].state = BallState::Dead;
                for (auto& row : host.fire) for (Ball& f : row) { f.state = BallState::Flying; f.pos = {1, 1, 1}; f.instigator = 0; f.spin = {0, 30.f, 0}; f.bounces = 3; }
                const size_t bytes = Net::snapshotBytes(host);
                char m8[112];
                std::snprintf(m8, sizeof m8, "... twelve fireballs with it and a full event log fit the relay (%d of 1200 bytes)", (int)bytes);
                CHECK(bytes <= 1200, m8);
                // (FIREBALL BLITZ in teams, 0.3.24: the teams the room plays, in a byte after the clock; an older host's
                // TEAMS byte, which it sends for every room, never made FIREBALL BLITZ play in teams)
                host.tun.teamMode = TEAMS_3V1; host.tun.theOne = 2;
                Game t3, older;
                const bool t3read = guest(t3, 0);
                CHECK(t3read && t3.tun.blitzTeams && t3.teamsNow() == TEAMS_3V1 && t3.team(2) == 1 && t3.team(0) == 0 && t3.maxHealth(2) == 15,
                      "FIREBALL BLITZ in teams: the guest plays the host's 3 v 1 (0.3.24)");
                CHECK(guest(older, 1) && !older.tun.blitzTeams && older.teamsNow() == TEAMS_FFA && older.team(2) == 2,
                      "... a host before 0.3.24 (its TEAMS set to 3 v 1 for FOUR CORNERS): everyone for themselves, as it played");
                host.tun.matchVersion = Tunables::kVerBlitzTeams - 1;   // (an older player in the room)
                Game fair;
                CHECK(guest(fair, 0) && fair.tun.blitzTeams && fair.teamsNow() == TEAMS_FFA,
                      "... with an older player in the room: the host says everyone for themselves");
                host.tun.matchVersion = Tunables::kVerAll; host.tun.teamMode = TEAMS_FFA;
            }
            Q.game.tun.expRoom = ROOM_BLITZ;
            Q.game.useArena(ARENA_OCTAGON, {});
            for (int i = 1; i <= 3; ++i) { pump(fs, 0.3); fs[i]->game.useArena(fs[i]->game.tun.expRoom == ROOM_BLITZ ? ARENA_OCTAGON : ARENA_CLASSIC, {}); }
            pump(fs, 2.5);   // READY UP (0.3.14): nothing starts until every person in the tank pressed A
            const bool held = Q.game.match == MatchState::WaitingForPlayers && Q.game.tun.readyUp;
            fs[0]->local.buttonA = true;
            pump(fs, 0.4);
            fs[0]->local.buttonA = false;
            pump(fs, 0.5);
            const bool seen = fs[1]->game.pl[Q.net.localSlot()].ready && Q.game.match == MatchState::WaitingForPlayers;
            CHECK(held && seen, "a room for four waits for everyone's READY (A), and the others see who is");
            for (int i = 1; i <= 3; ++i) fs[i]->local.buttonA = true;
            pump(fs, 0.4);
            for (int i = 1; i <= 3; ++i) fs[i]->local.buttonA = false;
            for (int i = 0; i < 40 && Q.game.match != MatchState::InProgress; ++i) pump(fs, 0.25);
            Q.game.pl[2].health = 3;
            Q.game.serve(0, Q.game.tun.start[0] + Vec3{0.5f, 1.3f, 0}, normalize(Vec3{1, 0.3f, 0.2f}), 8.f, {}, false);
            pump(fs, 0.3);
            bool same = true;
            for (int i = 1; i <= 3; ++i) {
                const Game& c = fs[i]->game;
                same &= c.octagon() && c.pl[2].health == 3 && c.startMask == Q.game.startMask && c.fireCount(0) == Q.game.fireCount(0) && Q.game.fireCount(0) == 1;
                same &= length(c.tun.start[1] - Q.game.tun.start[1]) < 1e-3f;
                // (the fireball where the host has it, or (BALL LOOK-AHEAD, 0.3.24) where it will within the lead, 0.4 s at
                // most: drawn ahead for the one it flies at)
                Game ah = Q.game;
                float best = length(c.fire[0][0].pos - ah.fire[0][0].pos);
                for (int s = 0; s < 36 && ah.fire[0][0].state == BallState::Flying; ++s) {
                    PlayerInput in[kMaxPlayers];
                    for (int p = 0; p < kMaxPlayers; ++p) in[p] = ah.pl[p].in;
                    ah.step(1.f / 90.f, in);
                    if (ah.fire[0][0].state == BallState::Flying) best = std::min(best, length(c.fire[0][0].pos - ah.fire[0][0].pos));
                }
                same &= best < 0.3f;
            }
            CHECK(Q.game.match == MatchState::InProgress && same, "FIREBALL BLITZ online: every visitor sees the health, the ring and the fireballs");
            // a match in a room for four ends: everyone back to the waiting area (owner, 2026-11-03), JOIN QUEUE again
            Q.game.matchWinner = 0;
            Q.game.match = MatchState::MatchSummary;
            pump(fs, 7.5);
            bool out = !Q.net.room().slot[0] && !Q.net.room().slot[1] && !Q.net.room().slot[2] && !Q.net.room().slot[3];
            for (int i = 0; i <= 3; ++i) out &= fs[i]->net.localWaiting();
            CHECK(out, "a room for four: after the match everyone goes back to the waiting area");
        }
        for (auto& n : fs) { if (n->net.online()) n->net.goOffline(); n->net.shutdown(); }
    }
    std::printf("[HOST HANDOFF and IDLE ROOMS (0.3.13)]\n");
    {   // owner, 2026-11-02: a host's headset going to sleep must not end everyone's room
        std::vector<std::unique_ptr<Node>> hs;
        const char* hn[] = {"OLD HOST", "HEIR ONE", "HEIR TWO", "GRIEFER"};
        for (int i = 0; i < 4; ++i) hs.emplace_back(new Node(hn[i], -1.f + i));
        for (auto& n : hs) n->net.goOnline(server);
        pump(hs, 1.0);
        Node& H = *hs[0];
        Node& A = *hs[1];
        Node& B = *hs[2];
        H.game.tun.advanced = false; H.game.tun.meetTime = 0;
        H.net.host();
        pump(hs, 1.0);
        uint32_t hip = 0;
        for (const LanGame& lg : A.net.games()) if (std::strcmp(lg.name, "OLD HOST") == 0) hip = lg.ip;
        A.net.join(hip, false);   // (A first: the longest in the room after the host, so next in line)
        pump(hs, 1.0);
        B.net.join(hip, false);
        pump(hs, 1.0);
        hs[3]->net.join(hip, false);   // (someone the host removes: they stay out under the next host too)
        pump(hs, 1.0);
        CHECK(H.net.hostKick(hs[3]->net.session()), "the host removes someone");
        pump(hs, 1.0);
        hs[3]->net.goOffline(); hs[3]->net.shutdown();
        hs.pop_back();
        A.net.queue(true);
        pump(hs, 0.5);
        B.net.queue(true);
        for (int i = 0; i < 40 && H.game.match != MatchState::InProgress; ++i) pump(hs, 0.25);
        H.game.pl[0].score = 3; H.game.pl[1].score = 2;
        pump(hs, 0.5);
        const float clock = H.game.roundClock;
        CHECK(H.game.match == MatchState::InProgress && H.net.room().slot[0] == A.net.session() && H.net.room().slot[1] == B.net.session(),
              "two players in a match, the host waiting outside");
        H.game.tun.foulShift = 2;   // FOUL LINE (0.3.20): the host's choice reaches the players with its rules
        pump(hs, 0.5);
        CHECK(B.game.tun.foulShift == 2 && std::fabs(B.game.foulLine() - H.game.foulLine()) < 1e-4f && B.game.foulLine() < 4.0f,
              "FOUL LINE: the host's line, moved, is the players' too");
        H.game.tun.foulShift = 0;
        pump(hs, 0.5);
        CHECK(B.game.tun.foulShift == 0, "... and back where the arena has it");
        // FAIR MATCHES (0.3.20): the host knows its players' games (these say 0.3.11) and an option one of them lacks
        // plays at regulation for the match
        CHECK(H.net.matchVersion() == Net::versionNumber("0.3.11"), "FAIR MATCHES: the host knows the oldest player's game");
        H.game.tun.matchVersion = H.net.matchVersion();
        H.game.tun.foulShift = 2; H.game.tun.strikeSpeed = 2;
        pump(hs, 0.5);
        CHECK(B.game.tun.foulShift == 0 && B.game.tun.strikeSpeed == 0 && H.game.tun.foulMove() == 0.f && H.game.tun.perStrike() == 0.5f,
              "... an older player in the match: FOUL LINE and STRIKE SPEED play at regulation, for the host too");
        CHECK(H.game.tun.foulShift == 2 && H.game.tun.strikeSpeed == 2, "... the host's choice kept for the next match");
        H.game.tun.foulShift = 0; H.game.tun.strikeSpeed = 0; H.game.tun.matchVersion = Tunables::kVerAll;
        pump(hs, 0.5);
        // ROOMS AND THEIR OPTIONS (0.3.21): the host's MAIDEN, AIM ASSIST, STRIKE ZONE and BALL GROWTH reach the players
        H.game.tun.maiden = true; H.game.tun.aimAssist = 3; H.game.tun.strikeZone = 2; H.game.tun.ballGrowth = 1;
        H.game.tun.advancedRoundsToWin = 3; H.game.tun.shieldSize = 2;
        B.game.tun.ballFeel = BALL_FEEL_DEFAULT; H.game.tun.ballFeel = BALL_CORE | BALL_SPIN;
        pump(hs, 0.5);
        CHECK(B.game.tun.ballFeel == (BALL_CORE | BALL_SPIN), "BALL FEEL: the host's ball (CORE SOLID, SPIN VISIBLE) is every player's at the arena");
        H.game.tun.ballFeel = BALL_FEEL_DEFAULT;
        CHECK(B.game.tun.maidenNow() && B.game.tun.aimAssistNow() == 3 && B.game.tun.strikeZoneNow() == 2 && B.game.tun.ballGrowthNow() == 1 &&
                  std::fabs(B.game.tun.strikeRadius() - 0.9f) < 1e-4f && B.game.tun.advancedRoundsToWin == 3 && B.game.tun.shieldSizeNow() == 2,
              "ROOM OPTIONS: the host's MAIDEN, AIM ASSIST, STRIKE ZONE and BALL GROWTH are the players' too");
        H.game.tun.matchVersion = 3020;   // (a player on 0.3.20: the competition build)
        pump(hs, 0.5);
        const int rulesAim = H.game.tun.advanced ? 1 : 2;
        CHECK(!B.game.tun.maidenNow() && B.game.tun.aimAssistNow() == rulesAim && B.game.tun.strikeZoneNow() == 1 &&
                  B.game.tun.ballGrowthNow() == 3 && !H.game.tun.maidenNow() && H.game.tun.strikeZoneNow() == 1,
              "... a player on 0.3.20 (the competition build) in the match: all as the rules say, for the host too");
        H.game.tun.maiden = false; H.game.tun.aimAssist = -1; H.game.tun.strikeZone = 1; H.game.tun.ballGrowth = 3;
        H.game.tun.advancedRoundsToWin = 2; H.game.tun.shieldSize = 0;
        H.game.tun.matchVersion = Tunables::kVerAll;
        pump(hs, 0.5);
        // BOUNCES (0.3.22): the host's WALLS, WALL GRIP and BANK ASSIST reach the players (with each ball's turn still to
        // come); a player on 0.3.21 in the match: STANDARD
        H.game.tun.wallsLively = true; H.game.tun.wallGrip = true; H.game.tun.bankAssist = 2;
        pump(hs, 0.5);
        CHECK(B.game.tun.wallsLivelyNow() && B.game.tun.wallGripNow() && B.game.tun.bankAssistNow() == 2 &&
                  std::fabs(B.game.tun.bankAssistDeg() - 1.6f) < 1e-4f,
              "BOUNCES: the host's LIVELY WALLS, WALL GRIP and BANK ASSIST MEDIUM are the players' too");
        H.game.tun.matchVersion = 3021;
        pump(hs, 0.5);
        CHECK(!B.game.tun.wallsLively && !B.game.tun.wallGrip && B.game.tun.bankAssist == 0 && H.game.tun.bouncesStandard() &&
                  H.game.tun.wallGrip,
              "... a player on 0.3.21 in the match: STANDARD, for the host too (its choice kept for the next match)");
        H.game.tun.wallsLively = H.game.tun.wallGrip = false; H.game.tun.bankAssist = 0;
        H.game.tun.matchVersion = Tunables::kVerAll;
        pump(hs, 0.5);
        // THROW POWER and KNUCKLE RETURNS (0.3.25): the host's reach the players, so their look-ahead throws and punches as
        // the host will; a player on 0.3.24 in the match: REGULATION
        H.game.tun.throwPower = 1; H.game.tun.knuckleReturns = 1;
        pump(hs, 0.5);
        CHECK(B.game.tun.naturalThrowNow() && B.game.tun.punchReturnsNow(),
              "THROW POWER: NATURAL and KNUCKLE RETURNS: PUNCH: the host's are the players' too");
        H.game.tun.matchVersion = 3024;
        pump(hs, 0.5);
        CHECK(B.game.tun.throwPower == 0 && B.game.tun.knuckleReturns == 0 && H.game.tun.feelStandard() && H.game.tun.throwPower == 1,
              "... a player on 0.3.24 in the match: REGULATION, for the host too (its choice kept for the next match)");
        H.game.tun.throwPower = H.game.tun.knuckleReturns = 0;
        H.game.tun.matchVersion = Tunables::kVerAll;
        pump(hs, 0.5);
        pump(hs, 2.0);   // (the shot clock has been running a while)
        CHECK(H.game.pl[0].shotClock < H.game.tun.shotClockSeconds - 1.5f && std::fabs(B.game.pl[0].shotClock - H.game.pl[0].shotClock) < 0.5f,
              "the shot clock runs, and every player sees it as the host does (the ring round the launchpad, 0.3.14)");
        std::printf("       (shot clocks: host %.1f / %.1f, player %.1f / %.1f, ball states %d %d)\n", H.game.pl[0].shotClock, H.game.pl[1].shotClock,
                    B.game.pl[0].shotClock, B.game.pl[1].shotClock, (int)H.game.pl[0].ball.state, (int)H.game.pl[1].ball.state);
        // the host's headset goes to sleep: no frames from it at all
        std::vector<std::unique_ptr<Node>> two;
        two.push_back(std::move(hs[1])); two.push_back(std::move(hs[2]));
        pump(two, 8.0);
        CHECK(A.net.mode() == Net::Mode::Host && A.net.inherited() && B.net.mode() == Net::Mode::Client && B.net.hostSession() == A.net.session(),
              "the host goes quiet: the longest in the room takes it over, the other follows");
        CHECK(A.net.room().slot[0] == A.net.session() && A.net.room().slot[1] == B.net.session() && B.net.localSlot() == 1,
              "and everyone keeps their place in the tank");
        CHECK(A.game.pl[0].score == 3 && A.game.pl[1].score == 2 && B.game.pl[0].score == 3 && A.game.roundClock <= clock && A.game.roundClock > clock - 15.f,
              "the match picks up where it was: the score and the clock");
        for (int i = 0; i < 24 && (A.game.match != MatchState::InProgress || B.game.match != MatchState::InProgress); ++i) pump(two, 0.25);   // (the player sees it a snapshot later)
        CHECK(A.game.match == MatchState::InProgress && B.game.match == MatchState::InProgress, "after a countdown it plays on");
        if (A.game.match != MatchState::InProgress || B.game.match != MatchState::InProgress)
            std::printf("       (host %d %.2f, player %d)\n", (int)A.game.match, A.game.stateTimer, (int)B.game.match);
        bool named = false;
        for (const LanGame& lg : B.net.games()) named |=std::strcmp(lg.name, "OLD HOST") == 0 && lg.continues == hip;
        CHECK(named, "the room keeps its name, and says which host it came from (spectators follow it)");
        // MAKE HOST: the new host hands the room on mid-match; it stays in its place
        A.game.pl[0].score = 4;
        pump(two, 0.3);
        CHECK(A.net.hostHandOff(B.net.session(), A.game), "the host makes someone else the host");
        pump(two, 3.0);
        CHECK(B.net.mode() == Net::Mode::Host && A.net.mode() == Net::Mode::Client && A.net.hostSession() == B.net.session() && !A.net.handingOver(),
              "MAKE HOST: the room changes hands at once");
        CHECK(B.game.pl[0].score == 4 && B.net.room().slot[0] == A.net.session() && A.net.localSlot() == 0, "the old host plays on in its place, the score as it was");
        {   // the one removed comes back under the new host: still turned away
            Node G("GRIEFER", 3.f);
            G.net.goOnline(server);
            std::vector<std::unique_ptr<Node>>& t = two;
            for (int i = 0; i < 10 && !G.net.onlineReady(); ++i) { pump(t, 0.1); PlayerInput in[kMaxPlayers]; G.net.exchange(G.local, in, G.game, 0.01f); }
            uint32_t bip = 0;
            for (int i = 0; i < 20 && !bip; ++i) {
                pump(t, 0.1); PlayerInput in[kMaxPlayers]; G.net.exchange(G.local, in, G.game, 0.01f);
                for (const LanGame& lg : G.net.games()) if (lg.session == B.net.session()) bip = lg.ip;
            }
            G.net.join(bip, false);
            for (int i = 0; i < 15; ++i) { pump(t, 0.1); PlayerInput in[kMaxPlayers]; G.net.exchange(G.local, in, G.game, 0.01f); }
            CHECK(bip && !G.net.inRoom(), "someone the old host removed can't come back under the new one");
            G.net.goOffline(); G.net.shutdown();
        }
        // the old host wakes up: its room went on without it; it finds where it went (its beacons) and rejoins
        hs[1] = std::move(two[0]); hs[2] = std::move(two[1]);
        pump(hs, 6.0);
        CHECK(H.net.mode() == Net::Mode::Client && H.net.hostSession() == B.net.session() && !H.net.handingOver() && H.net.localWaiting(),
              "the old host wakes up and rejoins the room, waiting outside the tank");
        // LEAVE GAME with people still in the room: it goes on with the next in line
        B.net.leave();
        pump(hs, 3.0);
        CHECK(A.net.mode() == Net::Mode::Host && H.net.mode() == Net::Mode::Client && H.net.hostSession() == A.net.session(),
              "a host leaving hands the room to the next in line rather than closing it");
        // a player's own Wi-Fi drops for 7 s: they take the room over, but nobody follows and the host is still there:
        // back to the real room (no copy of it under the same name)
        H.net.deafen(7.0);
        pump(hs, 7.0);
        const bool split = H.net.mode() == Net::Mode::Host;
        pump(hs, 5.0);
        CHECK(split && H.net.mode() == Net::Mode::Client && H.net.hostSession() == A.net.session() && !H.net.handingOver() && A.net.mode() == Net::Mode::Host,
              "a player whose own connection dropped takes the room over, finds the host still there, and goes back to it");
        {   // a player's headset sleeps 15 s while the host plays on (the owner's session, 2026-11-14: four times one back
            // from sleep took the room over for a second): on waking it asks the host to let it back in, and never hosts
            std::vector<std::unique_ptr<Node>> awake;
            awake.push_back(std::move(hs[1])); awake.push_back(std::move(hs[2]));
            pump(awake, 15.0);
            hs[1] = std::move(awake[0]); hs[2] = std::move(awake[1]);
            bool tookOver = false;
            for (int i = 0; i < 60; ++i) { pump(hs, 0.2); tookOver |= H.net.mode() == Net::Mode::Host; }
            CHECK(!tookOver && H.net.mode() == Net::Mode::Client && H.net.hostSession() == A.net.session() && A.net.mode() == Net::Mode::Host,
                  "a player back from 15 s asleep asks the host to let them back in, and never takes the room over");
            if (tookOver || H.net.mode() != Net::Mode::Client)
                std::printf("       (took over %d, mode %d, its host %08X, the host %08X)\n", tookOver, (int)H.net.mode(), H.net.hostSession(), A.net.session());
        }
        // IDLE ROOMS (shortened): nobody moves (these headsets never do), so the player leaves the tank, then the room closes
        g_still = true;
        A.local.head.p.x += 0.3f; H.local.head.p.x += 0.3f;   // (one last step each: the clocks start here)
        pump(hs, 0.3);
        A.net.setIdleTimes(2.0, 9.0);
        pump(hs, 0.3);
        if (A.net.localSlot() < 0) A.net.queue(true);
        pump(hs, 0.3);
        pump(hs, 3.0);
        CHECK(A.net.mode() == Net::Mode::Host && A.net.localSlot() < 0, "a player whose headset sits still leaves the tank for the waiting area");
        pump(hs, 6.0);
        CHECK(A.net.mode() == Net::Mode::Offline && H.net.mode() == Net::Mode::Offline, "a room where nobody moves closes, for everyone in it");
        for (auto& n : hs) { if (n->net.online()) n->net.goOffline(); n->net.shutdown(); }
    }
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
