#include "net.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "avatar.h"
#include "log.h"

#ifdef _WIN32   // desktop builds (tests, the PC tools): Winsock
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <chrono>
typedef int socklen_t;
static void closeSock(int s) { closesocket((SOCKET)s); }
static void setNonBlocking(int s) { u_long one = 1; ioctlsocket((SOCKET)s, FIONBIO, &one); }
static double monoNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
static void closeSock(int s) { close(s); }
static void setNonBlocking(int s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK); }
static double monoNow() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
#endif

static const uint16_t kPort = 47777;
static const uint32_t kMagic = 0x43525053;   // 'SPRC'
// 3: event log in STATE; 4: named beacons, LEAVE; 5: SpawnRise; 6: spectators; 7: avatars; 8: rooms (waiting players,
// queue, rotation, spectator presence)
static const uint8_t kVersion = 8;
// PING / PONG: the round trip between a host and each player (logged with the net stats). Builds without them ignore
// both, so the protocol version stays.
enum : uint8_t { MSG_BEACON = 1, MSG_JOIN = 2, MSG_INPUT = 3, MSG_STATE = 4, MSG_LEAVE = 5, MSG_SPECTATE = 6,
                 MSG_REJECT = 7, MSG_QUEUE = 8, MSG_ROOM = 9, MSG_VOICE = 10, MSG_CAMERAS = 11, MSG_PING = 12, MSG_PONG = 13,
                 MSG_KICKVOTE = 14, MSG_CROWD = 15 };   // (14: VOTE TO REMOVE, body: the target's id; older hosts ignore it)
// (15: The Crowd, host -> room and spectators: the relay's CROWD body, 12 bytes; older builds ignore it)
// community relay (server/projectarena_relay.py)
static const uint32_t kRelayMagic = 0x4C52414F;   // 'OARL'
static const uint16_t kRelayPort = 47780;
enum : uint8_t { R_HELLO = 1, R_WELCOME = 2, R_SEND = 3, R_PING = 4, R_BYE = 5, R_DELIVER = 6, R_ERROR = 7,
                 R_KEY = 8, R_TANK = 9, R_MATCH = 10, R_STATSREQ = 11, R_STATS = 12, R_CONFIRM = 13, R_DELETE = 14,
                 R_MYSTATS = 15, R_LIVE = 16, R_CROWD = 17 };   // (8-15: player statistics; 16-17: The Crowd)
static const double kRotateDelay = 6.0;          // match summary -> both players out, the queue in
static const double kMemberTimeout = 4.0;

static const char* ipStr(uint32_t ip) { in_addr ia{}; ia.s_addr = ip; return inet_ntoa(ia); }

// ------------------------------------------------------------------ serialization
struct Writer {
    std::vector<uint8_t> b;
    template <class T> void put(const T& v) { const uint8_t* p = (const uint8_t*)&v; b.insert(b.end(), p, p + sizeof(T)); }
    void pose(const Pose& p) { put(p.q); put(p.p); }
};
struct Reader {
    const uint8_t* p; const uint8_t* e;
    bool ok = true;
    template <class T> T get() { T v{}; if (p + sizeof(T) > e) { ok = false; return v; } std::memcpy(&v, p, sizeof(T)); p += sizeof(T); return v; }
    Pose pose() { Pose x; x.q = get<Quat>(); x.p = get<Vec3>(); return x; }
};

static void writeInput(Writer& w, const PlayerInput& in) {
    w.pose(in.head); w.put<uint8_t>(in.headTracked); w.put<uint8_t>(in.buttonA);
    for (int h = 0; h < 2; ++h) { w.pose(in.hand[h].grip); w.put(in.hand[h].angVel); w.put<uint8_t>(in.hand[h].tracked); w.put(in.hand[h].trigger); w.put(in.hand[h].squeeze); }
    w.put(in.avatar);
}
static void readInput(Reader& r, PlayerInput& in) {
    in.head = r.pose(); in.headTracked = r.get<uint8_t>(); in.buttonA = r.get<uint8_t>();
    for (int h = 0; h < 2; ++h) { in.hand[h].grip = r.pose(); in.hand[h].angVel = r.get<Vec3>(); in.hand[h].tracked = r.get<uint8_t>(); in.hand[h].trigger = r.get<float>(); in.hand[h].squeeze = r.get<float>(); }
    in.avatar = r.get<AvatarDesc>();
    avatar::sanitize(in.avatar);   // never trust indices from the network
}
// waiting players in ROOM packets: poses and look only
static void writePose(Writer& w, const PlayerInput& in) {
    w.pose(in.head); w.put<uint8_t>(in.headTracked);
    for (int h = 0; h < 2; ++h) { w.pose(in.hand[h].grip); w.put<uint8_t>(in.hand[h].tracked); }
    w.put(in.avatar);
}
static void readPose(Reader& r, PlayerInput& in) {
    in.head = r.pose(); in.headTracked = r.get<uint8_t>();
    for (int h = 0; h < 2; ++h) { in.hand[h].grip = r.pose(); in.hand[h].tracked = r.get<uint8_t>(); }
    in.avatar = r.get<AvatarDesc>();
    avatar::sanitize(in.avatar);
}

static void header(Writer& w, uint8_t type, uint32_t session, uint32_t seq) {
    w.put(kMagic); w.put(kVersion); w.put(type); w.put(session); w.put(seq);
}

static void writeRoom(Writer& w, const RoomInfo& ri, uint32_t seq) {
    w.put(seq); w.put(ri.slot[0]); w.put(ri.slot[1]); w.put<uint8_t>(ri.bot);
    w.put<uint8_t>((uint8_t)std::min<size_t>(ri.members.size(), 8));
    for (size_t i = 0; i < ri.members.size() && i < 8; ++i) {
        const RoomInfo::Member& m = ri.members[i];
        w.put(m.id); w.put(m.role); w.put(m.queuePos); w.put(m.spot); writePose(w, m.in);
    }
    w.put<uint8_t>((uint8_t)std::min<size_t>(ri.watchers.size(), 24));
    for (size_t i = 0; i < ri.watchers.size() && i < 24; ++i) {
        const RoomInfo::Watcher& s = ri.watchers[i];
        w.put(s.id); w.put(s.where); w.put(s.pos);
    }
    w.put<uint8_t>((uint8_t)std::min<size_t>(ri.names.size(), 16));
    for (size_t i = 0; i < ri.names.size() && i < 16; ++i) {
        w.put(ri.names[i].id);
        for (int k = 0; k < 16; ++k) w.put<uint8_t>((uint8_t)ri.names[i].name[k]);
    }
}

// a name from the network: 16 bytes, upper-case letters, digits and the keyboard's symbols only
static void readName(Reader& r, char out[17]) {
    for (int i = 0; i < 16; ++i) out[i] = (char)r.get<uint8_t>();
    out[16] = 0;
    for (int i = 0; i < 16; ++i) if (out[i] && (out[i] < 32 || out[i] > 'Z')) out[i] = ' ';
}
static void writeName(Writer& w, const char* name) {
    for (int i = 0; i < 16; ++i) w.put<uint8_t>((uint8_t)name[i]);
}

static bool readRoom(Reader& r, RoomInfo& ri, uint32_t& lastSeq) {
    uint32_t seq = r.get<uint32_t>();
    RoomInfo n;
    n.slot[0] = r.get<uint32_t>(); n.slot[1] = r.get<uint32_t>(); n.bot = r.get<uint8_t>() != 0;
    uint8_t nm = r.get<uint8_t>();
    for (uint8_t i = 0; i < nm && i < 8 && r.ok; ++i) {
        RoomInfo::Member m;
        m.id = r.get<uint32_t>(); m.role = r.get<uint8_t>(); m.queuePos = r.get<int8_t>(); m.spot = r.get<uint8_t>() & 3;
        readPose(r, m.in);
        if (m.queuePos > 1) m.queuePos = 1;
        n.members.push_back(m);
    }
    uint8_t nw = r.get<uint8_t>();
    for (uint8_t i = 0; i < nw && i < 24 && r.ok; ++i) {
        RoomInfo::Watcher s;
        s.id = r.get<uint32_t>(); s.where = r.get<uint8_t>(); s.pos = r.get<Vec3>();
        if (std::isfinite(s.pos.x) && std::isfinite(s.pos.y) && std::isfinite(s.pos.z)) n.watchers.push_back(s);
    }
    if (!r.ok || (lastSeq && seq <= lastSeq)) return false;
    if (r.p < r.e) {   // the names (older builds stop before them)
        uint8_t nn = r.get<uint8_t>();
        for (uint8_t i = 0; i < nn && i < 16 && r.ok; ++i) {
            RoomInfo::Name x;
            x.id = r.get<uint32_t>();
            readName(r, x.name);
            if (r.ok) n.names.push_back(x);
        }
    }
    lastSeq = seq;
    ri = n;
    return true;
}

// ------------------------------------------------------------------ lifecycle
// Android filters broadcast/multicast on Wi-Fi unless a MulticastLock is held. The app has no Java
// code (NativeActivity), so main.cpp takes the lock through JNI. Failure is non-fatal (logged).
extern bool netAcquireMulticastLock();

bool sendBugReport(const char* hostPort, const std::string& text, std::string& err) {
    std::string host = hostPort ? hostPort : "";
    int port = 47780;
    size_t c = host.rfind(':');
    if (c != std::string::npos) { port = std::atoi(host.c_str() + c + 1); host = host.substr(0, c); }
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (host.empty() || getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) { err = "SERVER NOT FOUND"; return false; }
    sockaddr_in to = *(sockaddr_in*)res->ai_addr;
    freeaddrinfo(res);
    to.sin_port = htons((uint16_t)port);
    int s = (int)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) { err = "NO SOCKET"; return false; }
#ifdef _WIN32
    DWORD tv = 8000;
#else
    timeval tv{8, 0};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);
    if (connect(s, (sockaddr*)&to, sizeof to) != 0) { closeSock(s); err = "SERVER NOT REACHABLE"; return false; }
    std::string msg = "PABUG1\n";
    const uint32_t n = (uint32_t)std::min<size_t>(text.size(), 200000);
    for (int i = 0; i < 4; ++i) msg += (char)((n >> (8 * i)) & 0xff);
    msg.append(text, 0, n);
    size_t sent = 0;
    while (sent < msg.size()) {
        int k = (int)send(s, msg.data() + sent, (int)(msg.size() - sent), 0);
        if (k <= 0) { closeSock(s); err = "SEND FAILED"; return false; }
        sent += (size_t)k;
    }
    char reply[64] = {};
    int k = (int)recv(s, reply, sizeof reply - 1, 0);
    closeSock(s);
    if (k >= 2 && reply[0] == 'O' && reply[1] == 'K') return true;
    err = k > 0 ? std::string(reply, reply + std::min(k, 40)) : "NO ANSWER";
    while (!err.empty() && (err.back() == '\n' || err.back() == '\r')) err.pop_back();
    return false;
}

void Net::init() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    sock_ = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ < 0) { LOGE("net: socket failed"); return; }
    int one = 1;
#ifndef _WIN32   // (on Windows SO_REUSEADDR would let several instances share the port)
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
#endif
    setsockopt(sock_, SOL_SOCKET, SO_BROADCAST, (const char*)&one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_port = htons(kPort); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock_, (sockaddr*)&a, sizeof a) < 0) {   // another instance on this machine: online play still works
        a.sin_port = 0;
        bind(sock_, (sockaddr*)&a, sizeof a);
        LOGW("net: UDP %u busy, using a free port (online only)", kPort);
    }
    setNonBlocking(sock_);
#ifdef _WIN32
    {   // Windows reports ICMP "port unreachable" as WSAECONNRESET on the next recvfrom: switch that off
        BOOL off = FALSE; DWORD ret = 0;
        WSAIoctl((SOCKET)sock_, _WSAIOW(IOC_VENDOR, 12), &off, sizeof off, nullptr, 0, &ret, nullptr, nullptr);
    }
#endif
    session_ = (uint32_t)(monoNow() * 1000) ^ 0x5A5A1234u ^ (uint32_t)(uintptr_t)this;
    if (!session_) session_ = 1;
    LOGI("net: ready on UDP %u (multicast lock %s)", kPort, netAcquireMulticastLock() ? "held" : "unavailable");
}

void Net::shutdown() { if (sock_ >= 0) closeSock(sock_); sock_ = -1; }

void Net::sendTo(const void* d, int n, uint32_t ip, uint16_t port) {
    if (online_) {   // through the relay: [magic][SEND][token][dst virtual address][game packet]
        if (!vip_ || n > 1200) return;
        uint8_t b[1300];
        uint32_t dst = ip == INADDR_BROADCAST ? 0xFFFFFFFFu : ip;
        std::memcpy(b, &kRelayMagic, 4); b[4] = R_SEND; std::memcpy(b + 5, token_, 16); std::memcpy(b + 21, &dst, 4);
        std::memcpy(b + 25, d, n);
        rawSend(b, 25 + n, serverIp_, serverPort_);
        return;
    }
    rawSend(d, n, ip, port);
}

void Net::rawSend(const void* d, int n, uint32_t ip, uint16_t port) {
    if (sock_ < 0) return;
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = ip;
    sendto(sock_, (const char*)d, n, 0, (sockaddr*)&a, sizeof a);
}

void Net::relaySend(uint8_t type, const void* body, int n) {
    if (!online_ || !vip_ || n < 0 || n > 200) return;
    uint8_t b[221] = {};
    std::memcpy(b, &kRelayMagic, 4); b[4] = type; std::memcpy(b + 5, token_, 16);
    if (n) std::memcpy(b + 21, body, n);
    rawSend(b, 21 + n, serverIp_, serverPort_);
}

uint32_t Net::roomHostVip() const {
    if (!online_ || !vip_) return 0;
    if (mode_ == Mode::Host) return vip_;
    if (mode_ == Mode::Client) return peerIp_;
    return 0;
}

void Net::reportMatch(int winner, const MatchStats st[2], const uint8_t score[4]) {
    if (mode_ != Mode::Host || !online_ || !vip_ || winner < 0 || winner > 1 || !slot_[0] || !slot_[1]) return;
    uint8_t b[33];
    std::memcpy(b + 29, score, 4);
    b[0] = (uint8_t)winner;
    for (int k = 0; k < 2; ++k) {
        const uint16_t v[7] = {st[k].throws, st[k].onTarget, st[k].hits, st[k].blocks, st[k].faced, st[k].catches,
                               (uint16_t)std::min(6000.f, std::max(0.f, st[k].topSpeed * 100.f))};
        std::memcpy(b + 1 + 14 * k, v, 14);
    }
    relaySend(R_MATCH, b, sizeof b);
    LOGI("stats: match reported (winner slot %d)", winner);
}

void Net::confirmMatch(int winner, const uint8_t score[4]) {
    if (mode_ != Mode::Client || !online_ || !vip_ || !peerIp_ || winner < 0 || winner > 1) return;
    uint8_t b[9];
    std::memcpy(b, &peerIp_, 4);
    b[4] = (uint8_t)winner;
    std::memcpy(b + 5, score, 4);
    relaySend(R_CONFIRM, b, sizeof b);
    LOGI("stats: match confirmed (winner slot %d)", winner);
}

bool Net::readCrowd(const uint8_t* p, const uint8_t* e, CrowdInfo& c, double now) {
    if (e - p < 12) return false;
    uint16_t v[3], w[2];
    std::memcpy(v, p, 6); std::memcpy(w, p + 8, 4);
    c.fans = v[0]; c.cheers[0] = v[1]; c.cheers[1] = v[2];
    c.hype[0] = std::min<int>(100, p[6]); c.hype[1] = std::min<int>(100, p[7]);
    c.waves[0] = w[0]; c.waves[1] = w[1];
    c.at = now;
    return true;
}

const CrowdInfo* Net::crowd(uint32_t hostIp) const {
    const CrowdInfo* c = nullptr;
    if (mode_ == Mode::Spectate) { for (const SpecMatch& m : matches_) if (m.ip == hostIp) c = &m.crowd; }
    else if (inRoom()) c = &crowd_;
    return c && now_ - c->at < 3.0 ? c : nullptr;
}

void Net::requestMyStats() { relaySend(R_MYSTATS, nullptr, 0); }
void Net::deleteMyStats() { relaySend(R_DELETE, nullptr, 0); LOGI("stats: delete requested"); }

void Net::requestStats(uint32_t hostVip) {
    if (hostVip) relaySend(R_STATSREQ, &hostVip, 4);
}

bool Net::statsFor(uint32_t hostVip, CareerStats out[2]) const {
    if (!hostVip || hostVip != statsHost_) return false;
    out[0] = stats_[0]; out[1] = stats_[1];
    return true;
}

void Net::relayHello() {
    uint8_t b[38] = {};
    std::memcpy(b, &kRelayMagic, 4); b[4] = R_HELLO;   // token: zeros
    b[21] = kVersion;
    std::memcpy(b + 22, name_, 16);
    rawSend(b, sizeof b, serverIp_, serverPort_);
    lastHello_ = now_ > 0 ? now_ : monoNow();
}

bool Net::goOnline(const char* hostPort) {
    char host[64] = {};
    std::snprintf(host, sizeof host, "%s", hostPort ? hostPort : "");
    uint16_t port = kRelayPort;
    if (char* c = std::strrchr(host, ':')) { *c = 0; port = (uint16_t)std::atoi(c + 1); }
    for (char* c = host; *c; ++c) if (*c >= 'A' && *c <= 'Z') *c += 32;   // typed upper-case in VR
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    if (!host[0] || getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) { notice("SERVER NOT FOUND"); return false; }
    serverIp_ = ((sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    leave();
    games_.clear();
    serverPort_ = port ? port : kRelayPort;
    std::snprintf(serverName_, sizeof serverName_, "%s", hostPort);
    online_ = true; vip_ = 0; lastRelayHeard_ = now_;
    relayHello();
    LOGI("net: going online via %s", serverName_);
    return true;
}

void Net::goOffline() {
    if (!online_) return;
    leave();
    uint8_t b[21] = {};
    std::memcpy(b, &kRelayMagic, 4); b[4] = R_BYE; std::memcpy(b + 5, token_, 16);
    rawSend(b, sizeof b, serverIp_, serverPort_);
    online_ = false; vip_ = 0; games_.clear();
    LOGI("net: back on the LAN");
}

void Net::setName(const char* n) {
    std::snprintf(name_, sizeof name_, "%s", n && *n ? n : "QUEST");
    for (char* c = name_; *c; ++c) if (*c >= 'a' && *c <= 'z') *c -= 32;   // HUD font is upper-case only
    if (online_ && serverIp_) relayHello();   // the relay knows us by name (its log, player reports)
}

std::vector<LanGame> Net::games() const {
    std::vector<LanGame> out;
    for (const LanGame& g : games_) if (now_ - g.seen < 2.0) out.push_back(g);
    return out;
}

void Net::notice(const char* msg) { std::snprintf(notice_, sizeof notice_, "%s", msg); noticeUntil_ = now_ + 5.0; }
const char* Net::noticeText() const { return now_ < noticeUntil_ ? notice_ : nullptr; }

void Net::host() {
    leave();
    mode_ = Mode::Host; peerIp_ = 0; peerSeen_ = 0; lastBeacon_ = 0;
    members_.clear();
    Member me;
    me.id = session_; me.local = true; me.seen = now_;
    members_.push_back(me);
    slot_[0] = slot_[1] = 0;   // the host starts in the room's waiting area too: JOIN QUEUE to play (owner request)
    queue_.clear();
    rotateAt_ = 0;
    hostRoomInfo();
    notice("ROOM OPEN - JOIN THE QUEUE TO PLAY");
    LOGI("net: hosting as \"%s\"", name_);
}

void Net::join(uint32_t ip, bool play) {
    leave();
    mode_ = Mode::Client; peerIp_ = ip; peerSeen_ = now_; lastRecvSeq_ = 0; lastRoomSeq_ = 0;
    peerSession_ = 0;
    for (const LanGame& lg : games_) if (lg.ip == ip) peerSession_ = lg.session;
    lastEventId_ = 0; pendingEvents_.clear(); resetStats();
    room_ = RoomInfo();
    lastJoin_ = now_; joinPlay_ = play;
    Writer w; header(w, MSG_JOIN, session_, ++seq_); w.put<uint8_t>(play ? 1 : 0); writeName(w, name_);
    sendTo(w.b.data(), (int)w.b.size(), peerIp_, kPort);
    LOGI("net: joining host %s (%s)", ipStr(ip), play ? "play" : "room");
}

void Net::leave() {
    if (mode_ == Mode::Client && peerIp_) {   // tell the host right away
        Writer w; header(w, MSG_LEAVE, session_, 0);
        sendTo(w.b.data(), (int)w.b.size(), peerIp_, kPort);
    }
    if (mode_ == Mode::Host) {                // tell everyone in the room the arena is closing
        Writer w; header(w, MSG_LEAVE, session_, 0);
        for (const Member& m : members_) if (!m.local) sendTo(w.b.data(), (int)w.b.size(), m.ip, kPort);
    }
    if (mode_ == Mode::Host || mode_ == Mode::Client || mode_ == Mode::Spectate) LOGI("net: offline");
    mode_ = Mode::Offline; peerIp_ = 0; peerSeen_ = 0; peerSession_ = 0;
    matches_.clear();
    spectators_.clear();
    members_.clear();
    queue_.clear();
    slot_[0] = slot_[1] = 0;
    room_ = RoomInfo();
}

void Net::spectate() {
    leave();
    mode_ = Mode::Spectate;
    matches_.clear();
    lastSpectatePing_ = 0;
    LOGI("net: spectating");
}

void Net::toggleLan() {
    if (mode_ == Mode::Offline) { mode_ = Mode::Searching; searchStart_ = monoNow(); peerSeen_ = 0; LOGI("net: searching for host"); }
    else leave();
}

// QUEUE with 2: leave the tank (older hosts read it as a queue request, which does nothing for a tank player)
void Net::forfeit() {
    if (mode_ == Mode::Client) {
        Writer w; header(w, MSG_QUEUE, session_, 0); w.put<uint8_t>(2);
        sendTo(w.b.data(), (int)w.b.size(), peerIp_, kPort);
    } else if (mode_ == Mode::Host) {
        forfeit_ = session_;
    }
    LOGI("net: forfeit");
}

void Net::queue(bool want) {
    if (mode_ == Mode::Client) {
        Writer w; header(w, MSG_QUEUE, session_, 0); w.put<uint8_t>(want ? 1 : 0);
        sendTo(w.b.data(), (int)w.b.size(), peerIp_, kPort);
    } else if (mode_ == Mode::Host) {
        auto it = std::find(queue_.begin(), queue_.end(), session_);
        const bool inTank = slot_[0] == session_ || slot_[1] == session_;
        if (want && it == queue_.end() && !inTank && (int)queue_.size() < kMaxQueue) queue_.push_back(session_);
        if (!want && it != queue_.end()) queue_.erase(it);
        hostRoomInfo();   // the tank fills on the next exchange
    }
}

int Net::localSlot() const {
    if (mode_ == Mode::Host) return slot_[0] == session_ ? 0 : slot_[1] == session_ ? 1 : -1;
    if (mode_ == Mode::Client) return room_.slot[0] == session_ ? 0 : room_.slot[1] == session_ ? 1 : -1;
    return 0;
}

int Net::localQueuePos() const {
    for (const RoomInfo::Member& m : room_.members) if (m.id == session_) return m.queuePos;
    return -1;
}

int Net::localSpot() const {
    for (const RoomInfo::Member& m : room_.members) if (m.id == session_) return m.spot;
    return 0;
}

bool Net::slotFilled(int p) const {
    if (!inRoom()) return true;
    return room_.slot[p] != 0 || (p == 1 && room_.bot);
}

const char* Net::status() const {
    const char* where = online_ ? "ONLINE" : "LAN";
    switch (mode_) {
    case Mode::Offline: return "PRACTICE VS BOT";
    case Mode::Searching: return "SEARCHING LAN...";
    case Mode::Spectate: return "SPECTATING";
    case Mode::Host:
    case Mode::Client: {
        if (mode_ == Mode::Client && !peerConnected()) { snprintf(status_, sizeof status_, "%s - LOST HOST", where); return status_; }
        int s = localSlot(), q = localQueuePos();
        int waiting = 0;
        for (const RoomInfo::Member& m : room_.members) waiting += m.role == RoomInfo::WAITING;
        if (s >= 0) snprintf(status_, sizeof status_, "%s %s - %s%s", where, mode_ == Mode::Host ? "HOST" : "ARENA",
                             room_.slot[1 - s] ? "VS PLAYER" : room_.bot ? "VS BOT" : "WAITING FOR AN OPPONENT",
                             waiting ? " - PLAYERS WAITING" : "");
        else if (q >= 0) snprintf(status_, sizeof status_, "%s - QUEUED %s", where, q == 0 ? "NEXT" : "SECOND");
        else snprintf(status_, sizeof status_, "%s - WAITING OUTSIDE THE TANK", where);
        return status_;
    }
    }
    return "";
}

// ------------------------------------------------------------------ snapshot
static void writeState(Writer& w, const Game& g) {
    w.put<uint8_t>((uint8_t)g.match); w.put(g.stateTimer); w.put(g.roundClock); w.put<int32_t>(g.round);
    w.put<int8_t>((int8_t)g.lastRoundWinner); w.put<int8_t>((int8_t)g.matchWinner); w.put<uint8_t>(g.overtime); w.put<uint8_t>(g.tun.advanced);
    for (int p = 0; p < 2; ++p) {
        const PlayerState& s = g.pl[p];
        writeInput(w, s.in);
        w.put<uint8_t>(s.hands[0].shieldUp); w.put<uint8_t>(s.hands[1].shieldUp);
        w.put<uint8_t>(s.hands[0].knuckleUp); w.put<uint8_t>(s.hands[1].knuckleUp);
        w.put<int32_t>(s.score); w.put<int32_t>(s.roundWins); w.put<int32_t>(s.strikesAgainst);
        w.put<uint8_t>((uint8_t)s.ball.state); w.put(s.ball.pos); w.put(s.ball.vel); w.put<int8_t>((int8_t)s.ball.heldHand);
        w.put<uint8_t>(s.ball.charged); w.put<int8_t>((int8_t)s.ball.instigator); w.put(s.ball.scale);
    }
}

// Events ride at the front of every STATE packet: the last ~0.3 s of events with consecutive ids, so a lost or
// overtaken snapshot does not drop a throw / hit; the client keeps only ids it has not seen.
static void writeEvents(Writer& w, const std::deque<Net::SentEvent>& log) {
    uint8_t n = (uint8_t)std::min<size_t>(log.size(), 24);
    size_t first = log.size() - n;
    w.put<uint32_t>(n ? log[first].id : 0); w.put(n);
    for (size_t i = first; i < log.size(); ++i) {
        const GameEvent& e = log[i].e;
        w.put<uint8_t>((uint8_t)e.type); w.put<int8_t>((int8_t)e.player); w.put<int8_t>((int8_t)e.hand); w.put(e.pos); w.put(e.value);
    }
}

static bool readState(Reader& r, Game& g, int localIndex) {
    g.match = (MatchState)r.get<uint8_t>(); g.stateTimer = r.get<float>(); g.roundClock = r.get<float>(); g.round = r.get<int32_t>();
    g.lastRoundWinner = r.get<int8_t>(); g.matchWinner = r.get<int8_t>(); g.overtime = r.get<uint8_t>(); g.tun.advanced = r.get<uint8_t>();
    for (int p = 0; p < 2; ++p) {
        PlayerState& s = g.pl[p];
        PlayerInput in; readInput(r, in);
        if (p != localIndex) s.in = in;   // keep our own latest local poses for latency-free hands
        s.hands[0].shieldUp = r.get<uint8_t>(); s.hands[1].shieldUp = r.get<uint8_t>();
        s.hands[0].knuckleUp = r.get<uint8_t>(); s.hands[1].knuckleUp = r.get<uint8_t>();
        s.score = r.get<int32_t>(); s.roundWins = r.get<int32_t>(); s.strikesAgainst = r.get<int32_t>();
        s.ball.state = (BallState)r.get<uint8_t>(); s.ball.pos = r.get<Vec3>(); s.ball.vel = r.get<Vec3>(); s.ball.heldHand = r.get<int8_t>();
        s.ball.charged = r.get<uint8_t>(); s.ball.instigator = r.get<int8_t>(); s.ball.scale = r.get<float>();
    }
    return r.ok;
}

static void readEvents(Reader& r, uint32_t& lastId, std::vector<GameEvent>& out) {
    uint32_t id = r.get<uint32_t>(); uint8_t n = r.get<uint8_t>();
    for (uint8_t i = 0; i < n && r.ok; ++i, ++id) {
        GameEvent e;
        e.type = (EventType)r.get<uint8_t>(); e.player = r.get<int8_t>(); e.hand = r.get<int8_t>(); e.pos = r.get<Vec3>(); e.value = r.get<float>();
        if (r.ok && id > lastId) { out.push_back(e); lastId = id; }
    }
}

// ------------------------------------------------------------------ host: the room
Net::Member* Net::member(uint32_t id) {
    for (Member& m : members_) if (m.id == id) return &m;
    return nullptr;
}

int Net::humans() const { return (slot_[0] ? 1 : 0) + (slot_[1] ? 1 : 0); }

void Net::freeSpot(Member& m) {   // the lowest holding-area spot no other waiting member stands on
    bool used[4] = {false, false, false, false};
    for (const Member& o : members_)
        if (&o != &m && o.id != slot_[0] && o.id != slot_[1]) used[o.spot & 3] = true;
    if (!used[m.spot & 3]) return;
    for (int s = 0; s < 4; ++s) if (!used[s]) { m.spot = (uint8_t)s; return; }
}

// humans fill slot 0 first; one human plays the bot; any change of who is in the tank restarts the match
void Net::compactTank(Game& g, bool reset) {
    if (!slot_[0] && slot_[1]) { slot_[0] = slot_[1]; slot_[1] = 0; }
    for (Member& m : members_) if (m.id != slot_[0] && m.id != slot_[1]) freeSpot(m);
    if (reset) {
        g.reset(slot_[1] == 0 && botFill_);
        rotateAt_ = 0;
        eventLog_.clear();
    }
    hostRoomInfo();
}

// the queue goes into the tank whenever it has a free place (or only the bot): never into a match of two humans
void Net::fillTank(Game& g) {
    bool changed = false;
    while (!queue_.empty() && humans() < 2) {
        uint32_t id = queue_.front();
        queue_.erase(queue_.begin());
        if (!member(id)) continue;
        if (!slot_[0]) slot_[0] = id; else slot_[1] = id;
        changed = true;
        LOGI("net: queue -> tank");
    }
    if (changed) { compactTank(g, true); notice("NEXT PLAYERS IN THE TANK"); }
}

// match over: both players leave the tank (to the holding area), the queue goes in
void Net::rotate(Game& g) {
    LOGI("net: rotation (%d queued)", (int)queue_.size());
    slot_[0] = slot_[1] = 0;
    compactTank(g, false);
    fillTank(g);
    if (!humans()) compactTank(g, true);
}

void Net::removeMember(uint32_t id, Game& g) {
    auto it = std::find_if(members_.begin(), members_.end(), [&](const Member& m) { return m.id == id && !m.local; });
    if (it == members_.end()) return;
    kickVotes_.erase(std::remove_if(kickVotes_.begin(), kickVotes_.end(),
                                    [&](const std::pair<uint32_t, uint32_t>& v) { return v.first == id || v.second == id; }), kickVotes_.end());
    LOGI("net: %s left the room", ipStr(it->ip));
    members_.erase(it);
    queue_.erase(std::remove(queue_.begin(), queue_.end(), id), queue_.end());
    bool wasPlaying = slot_[0] == id || slot_[1] == id;
    if (slot_[0] == id) slot_[0] = 0;
    if (slot_[1] == id) slot_[1] = 0;
    if (wasPlaying) { compactTank(g, true); fillTank(g); notice("PLAYER LEFT"); }
    else hostRoomInfo();
}

void Net::hostRoomInfo() {
    room_.slot[0] = slot_[0]; room_.slot[1] = slot_[1];
    room_.bot = slot_[0] != 0 && slot_[1] == 0 && botFill_;
    room_.members.clear();
    for (const Member& m : members_) {
        if (m.id == slot_[0] || m.id == slot_[1]) continue;
        RoomInfo::Member rm;
        rm.id = m.id; rm.role = RoomInfo::WAITING; rm.spot = m.spot; rm.in = m.in;
        auto q = std::find(queue_.begin(), queue_.end(), m.id);
        rm.queuePos = q == queue_.end() ? -1 : (int8_t)(q - queue_.begin());
        room_.members.push_back(rm);
    }
    room_.watchers.clear();
    for (const Spectator& sp : spectators_)
        if (sp.where <= 1) room_.watchers.push_back({sp.id, sp.where, sp.pos});
    room_.names.clear();
    for (const Member& m : members_) {
        RoomInfo::Name x; x.id = m.id;
        std::memcpy(x.name, m.local ? name_ : m.name, 16);
        if (x.name[0]) room_.names.push_back(x);
    }
    for (const Spectator& sp : spectators_)
        if (sp.where == 1 && sp.name[0]) { RoomInfo::Name x; x.id = sp.id; std::memcpy(x.name, sp.name, 16); room_.names.push_back(x); }
}

std::string Net::hereName() const {
    if (mode_ == Mode::Host) return name_;
    const uint32_t ip = mode_ == Mode::Client ? peerIp_ : (mode_ == Mode::Spectate && presWhere_ == 1) ? presWatch_ : 0;
    for (const LanGame& lg : games_) if (ip && lg.ip == ip) return lg.name;
    return "";
}

std::vector<Net::Person> Net::peopleHere() const {
    std::vector<Person> v;
    const RoomInfo* ri = inRoom() ? &room_ : nullptr;
    if (mode_ == Mode::Spectate && presWhere_ == 1)
        for (const SpecMatch& m : matches_) if (m.ip == presWatch_) ri = &m.room;
    if (!ri) return v;
    auto add = [&](uint32_t id, uint8_t role) {
        if (!id || id == session_) return;
        for (const Person& p : v) if (p.id == id) return;
        Person p; p.id = id; p.role = role;
        if (const char* n = ri->nameOf(id)) std::memcpy(p.name, n, 16);
        else std::snprintf(p.name, sizeof p.name, "PLAYER %04X", id & 0xffff);   // a build without names
        v.push_back(p);
    };
    for (int s = 0; s < 2; ++s) add(ri->slot[s], VOICE_PLAYER);
    for (const RoomInfo::Member& m : ri->members) add(m.id, VOICE_WAITING);
    for (const RoomInfo::Watcher& w : ri->watchers) if (w.where == 1) add(w.id, VOICE_SPECTATOR);
    return v;
}

void Net::sendRoom() {
    static uint32_t roomSeq = 0;
    Writer w; header(w, MSG_ROOM, session_, 0);
    writeRoom(w, room_, ++roomSeq);
    for (const Member& m : members_) if (!m.local) sendTo(w.b.data(), (int)w.b.size(), m.ip, kPort);
    for (const Spectator& sp : spectators_) sendTo(w.b.data(), (int)w.b.size(), sp.ip, kPort);
}

int Net::kickVotesNeeded() const {
    const int people = (int)members_.size();   // everyone in the room, the host included
    const int others = people - 1;              // all but the one being voted on
    const int need = std::max(2, others / 2 + 1);
    return need <= others ? need : 0;
}

void Net::countKickVote(uint32_t target, uint32_t voter) {
    if (mode_ != Mode::Host || target == session_ || target == voter) return;   // (the host can't be removed)
    if (!member(target) || !member(voter)) return;
    for (const auto& v : kickVotes_) if (v.first == target && v.second == voter) return;
    kickVotes_.push_back({target, voter});
    int n = 0;
    for (const auto& v : kickVotes_) if (v.first == target && member(v.second)) ++n;
    const int need = kickVotesNeeded();
    const Member* t = member(target);
    LOGI("net: a vote to remove %s (%d of %d needed)", t->name[0] ? t->name : ipStr(t->ip), n, need);
    char msg[48];
    if (need && n >= need) {
        kickNow_ = target;
    } else {
        std::snprintf(msg, sizeof msg, "VOTE TO REMOVE %s  %d OF %d", t->name[0] ? t->name : "A PLAYER", n, need ? need : 2);
        notice(msg);
    }
}

bool Net::voteKick(uint32_t target) {
    if (!inRoom() || !target || target == session_ || target == hostSession()) return false;
    if (std::find(myKickVotes_.begin(), myKickVotes_.end(), target) != myKickVotes_.end()) return false;
    myKickVotes_.push_back(target);
    if (mode_ == Mode::Host) { countKickVote(target, session_); return true; }
    Writer w; header(w, MSG_KICKVOTE, session_, 0); w.put(target);
    sendTo(w.b.data(), (int)w.b.size(), peerIp_, kPort);
    LOGI("net: voted to remove %08x", target);
    return true;
}

void Net::reject(uint32_t ip, const char* why) {
    Writer w; header(w, MSG_REJECT, session_, 0);
    char t[32] = {};
    std::snprintf(t, sizeof t, "%s", why);
    for (char c : t) w.put<uint8_t>((uint8_t)c);
    sendTo(w.b.data(), (int)w.b.size(), ip, kPort);
}

// ------------------------------------------------------------------ voice
// body: speaker id (u32, set by the host), role (u8), kVoiceSamples mu-law bytes
void Net::sendVoice(const uint8_t* ulaw) {
    if (mode_ == Mode::Host) {   // we talk: straight to everyone in our room
        const uint8_t role = (slot_[0] == session_ || slot_[1] == session_) ? VOICE_PLAYER : VOICE_WAITING;
        forwardVoice(session_, role, ulaw, 0, session_);
        return;
    }
    uint32_t to = mode_ == Mode::Client ? peerIp_ : (mode_ == Mode::Spectate && presWhere_ == 1) ? presWatch_ : 0;
    if (!to) return;
    Writer w; header(w, MSG_VOICE, session_, 0);
    w.put<uint32_t>(0); w.put<uint8_t>(0);
    w.b.insert(w.b.end(), ulaw, ulaw + kVoiceSamples);
    sendTo(w.b.data(), (int)w.b.size(), to, kPort);
}

void Net::forwardVoice(uint32_t speaker, uint8_t role, const uint8_t* ulaw, uint32_t exceptIp, uint32_t exceptId) {
    Writer w; header(w, MSG_VOICE, session_, 0);
    w.put(speaker); w.put(role);
    w.b.insert(w.b.end(), ulaw, ulaw + kVoiceSamples);
    for (const Member& m : members_)
        if (!m.local && !(m.ip == exceptIp && m.id == exceptId)) sendTo(w.b.data(), (int)w.b.size(), m.ip, kPort);
    for (const Spectator& sp : spectators_)   // only spectators standing at this arena hear it
        if (sp.where == 1 && !(sp.ip == exceptIp && sp.id == exceptId)) sendTo(w.b.data(), (int)w.b.size(), sp.ip, kPort);
    if (speaker != session_ && voiceIn_.size() < 64) {   // the host hears it too
        VoiceFrame f; f.speaker = speaker; f.role = role; std::memcpy(f.data, ulaw, kVoiceSamples);
        voiceIn_.push_back(f);
    }
}

// ------------------------------------------------------------------ team cameras
// body: original sender (u32), team (16), blob length (u16), blob
void Net::shareCameras(const char* team, const std::vector<uint8_t>& blob) {
    if (blob.size() > 400) return;
    Writer w; header(w, MSG_CAMERAS, session_, 0);
    w.put(session_);
    char t[16] = {};
    std::snprintf(t, sizeof t, "%s", team);
    for (char c : t) w.put<uint8_t>((uint8_t)c);
    w.put<uint16_t>((uint16_t)blob.size());
    w.b.insert(w.b.end(), blob.begin(), blob.end());
    for (const LanGame& lg : games_) if (now_ - lg.seen < 2.5) sendTo(w.b.data(), (int)w.b.size(), lg.ip, kPort);
}

bool Net::takeCameras(char team[17], std::vector<uint8_t>& blob) {
    if (!camFresh_) return false;
    camFresh_ = false;
    std::memcpy(team, camTeam_, 17);
    blob = camBlob_;
    return camFrom_ != session_;
}

// ------------------------------------------------------------------ per-frame
bool Net::exchange(const PlayerInput& local, PlayerInput in[2], Game& g, float dt) {
    if (kickNow_ && mode_ == Mode::Host) {   // VOTE TO REMOVE passed: out of the room, and not back this session
        if (Member* m = member(kickNow_)) {
            Removed x; x.id = m->id; x.ip = m->ip; std::memcpy(x.name, m->name, 16);
            removed_.push_back(x);
            char msg[48];
            std::snprintf(msg, sizeof msg, "%s WAS REMOVED BY VOTE", m->name[0] ? m->name : "A PLAYER");
            LOGI("net: %s removed by vote", m->name[0] ? m->name : ipStr(m->ip));
            reject(m->ip, "REMOVED BY VOTE");
            removeMember(kickNow_, g);
            notice(msg);
        }
        kickNow_ = 0;
    }
    now_ = monoNow();
    bool gotState = false;
    uint8_t buf[2048];
    for (;;) {
        sockaddr_in from{}; socklen_t fl = sizeof from;
        int n = sock_ >= 0 ? (int)recvfrom(sock_, (char*)buf, sizeof buf, 0, (sockaddr*)&from, &fl) : -1;
#ifdef _WIN32
        if (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK) continue;   // a stray ICMP error: keep reading
#endif
        if (n <= 0) break;
        const uint8_t* pkt = buf;
        if (online_) {   // only the relay talks to us; unwrap DELIVER and treat the sender's virtual address as its IP
            if (from.sin_addr.s_addr != serverIp_ || ntohs(from.sin_port) != serverPort_) continue;
            Reader rr{buf, buf + n};
            uint32_t m = rr.get<uint32_t>(); uint8_t t = rr.get<uint8_t>();
            if (!rr.ok || m != kRelayMagic) continue;
            lastRelayHeard_ = now_;
            if (t == R_WELCOME) {
                if (rr.e - rr.p < 20) continue;
                std::memcpy(token_, rr.p, 16); std::memcpy(&vip_, rr.p + 16, 4);
                keySent_ = false;   // (a new session on the relay: name our stats key again)
                LOGI("net: online (relay %s)", serverName_);
                if (rr.e - rr.p >= 21) {   // the current release (relays from 2026-10-19 on)
                    const size_t n = std::min<size_t>(15, std::min<size_t>(rr.p[20], (size_t)(rr.e - rr.p - 21)));
                    std::memset(latest_, 0, sizeof latest_);
                    for (size_t i = 0; i < n; ++i) {
                        const char c = (char)rr.p[21 + i];
                        latest_[i] = (c >= '0' && c <= '9') || c == '.' ? c : 0;
                    }
                }
                continue;
            }
            if (t == R_STATS) {   // a host's two players' careers
                if (rr.e - rr.p < 4 + 2 * 51) continue;
                std::memcpy(&statsHost_, rr.p, 4);
                for (int k = 0; k < 2; ++k) {
                    const uint8_t* q = rr.p + 4 + 51 * k;
                    CareerStats& c = stats_[k];
                    c = CareerStats{};
                    c.known = q[0] != 0;
                    for (int i = 0; i < 16; ++i) c.name[i] = (q[1 + i] >= 32 && q[1 + i] < 127) ? (char)q[1 + i] : 0;
                    uint32_t v[8];
                    std::memcpy(v, q + 17, 32);
                    c.matches = v[0]; c.wins = v[1]; c.throws = v[2]; c.onTarget = v[3];
                    c.hits = v[4]; c.blocks = v[5]; c.faced = v[6]; c.catches = v[7];
                    uint16_t top;
                    std::memcpy(&top, q + 49, 2);
                    c.topSpeed = top / 100.f;
                }
                continue;
            }
            if (t == R_CROWD) {   // our fans (we host an online match): keep them, and pass them on to the room
                if (mode_ != Mode::Host || !readCrowd(rr.p, rr.e, crowd_, now_)) continue;
                Writer w; header(w, MSG_CROWD, session_, 0);
                w.b.insert(w.b.end(), rr.p, rr.p + 12);
                for (const Member& m : members_) if (!m.local) sendTo(w.b.data(), (int)w.b.size(), m.ip, kPort);
                for (const Spectator& sp : spectators_) sendTo(w.b.data(), (int)w.b.size(), sp.ip, kPort);
                continue;
            }
            if (t == R_ERROR) {
                char msg[49] = {};
                std::memcpy(msg, rr.p, std::min<size_t>(48, rr.e - rr.p));
                for (char& c : msg) if (c && (c < 32 || c > 'Z')) c = ' ';
                notice(msg);
                LOGW("net: relay says %s", msg);
                continue;
            }
            if (t != R_DELIVER) continue;
            uint32_t src = rr.get<uint32_t>();
            if (!rr.ok) continue;
            from.sin_addr.s_addr = src;
            pkt = rr.p; n = (int)(rr.e - rr.p);
        }
        Reader r{pkt, pkt + n};
        uint32_t magic = r.get<uint32_t>(); uint8_t ver = r.get<uint8_t>(), type = r.get<uint8_t>();
        uint32_t session = r.get<uint32_t>(), seq = r.get<uint32_t>();
        if (r.ok && magic == kMagic && ver != kVersion && type == MSG_BEACON && session != session_) {
            if (otherVer_ != ver) LOGW("net: a game on protocol v%d is on the network (this build is v%d)", ver, kVersion);
            otherVer_ = ver; otherVerSeen_ = now_;
            continue;
        }
        if (!r.ok || magic != kMagic || ver != kVersion || session == session_) continue;   // ignore our own broadcasts
        const uint32_t fromIp = from.sin_addr.s_addr;
        // a client only listens to its host's session; a host only to its members (by address + session)
        const bool fromHost = mode_ == Mode::Client && fromIp == peerIp_ && (peerSession_ == 0 || session == peerSession_);
        Member* fromMember = nullptr;
        if (mode_ == Mode::Host) { Member* m = member(session); if (m && !m->local && m->ip == fromIp) fromMember = m; }

        if (type == MSG_BEACON) {
            LanGame lg{}; lg.ip = fromIp; lg.session = session; lg.seen = now_;
            for (int i = 0; i < 16; ++i) lg.name[i] = (char)r.get<uint8_t>();
            lg.name[16] = 0; lg.advanced = r.get<uint8_t>() != 0; lg.full = r.get<uint8_t>() != 0;
            lg.score0 = r.get<int16_t>(); lg.score1 = r.get<int16_t>(); lg.clock = r.get<float>(); lg.spectators = r.get<uint8_t>();
            lg.waiting = r.get<uint8_t>(); lg.queued = r.get<uint8_t>(); lg.inTank = r.get<uint8_t>();
            for (char& c : lg.name) if (c && (c < 32 || c > 'Z')) c = ' ';
            if (r.ok) {
                bool found = false;
                for (LanGame& x : games_) if (x.ip == lg.ip) { x = lg; found = true; }
                if (!found && games_.size() < 16) games_.push_back(lg);
            }
            if (mode_ == Mode::Searching && r.ok && !lg.full) join(lg.ip);
        } else if (type == MSG_SPECTATE && mode_ == Mode::Host) {
            uint8_t where = r.get<uint8_t>(); Vec3 pos = r.get<Vec3>();
            if (!r.ok || !std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) { where = 2; pos = {}; }
            char who[17] = {};
            if (r.ok && r.e - r.p >= 16) readName(r, who);   // 0.3.1+
            Spectator* sp = nullptr;
            for (Spectator& x : spectators_) if (x.ip == fromIp && x.id == session) sp = &x;
            if (!sp && spectators_.size() < 24) {
                spectators_.push_back({fromIp, session, now_, where, pos});
                sp = &spectators_.back();
                LOGI("net: spectator %s watching", ipStr(fromIp));
                if (where == 1) notice("A SPECTATOR IS WATCHING");
            }
            if (sp) { sp->seen = now_; sp->where = where; sp->pos = pos; if (who[0]) std::memcpy(sp->name, who, sizeof who); }
        } else if ((type == MSG_STATE || type == MSG_ROOM) && mode_ == Mode::Spectate) {
            SpecMatch* m = nullptr;
            for (SpecMatch& x : matches_) if (x.ip == fromIp) m = &x;
            if (!m && matches_.size() < 8) {
                matches_.emplace_back();
                m = &matches_.back();
                m->ip = fromIp;
                for (const LanGame& lg : games_) if (lg.ip == m->ip) std::memcpy(m->name, lg.name, sizeof m->name);
                LOGI("net: watching %s", ipStr(fromIp));
            }
            if (m) {
                m->lastSeen = now_;
                if (type == MSG_ROOM) readRoom(r, m->room, m->lastRoomSeq);
                else {
                    readEvents(r, m->lastEventId, m->events);
                    if (m->events.size() > 64) m->events.erase(m->events.begin(), m->events.end() - 64);
                    if (seq > m->lastSeq) { m->lastSeq = seq; m->state.assign(r.p, r.e); m->fresh = true; }
                }
            }
        } else if (type == MSG_LEAVE) {
            if (mode_ == Mode::Host && fromMember) removeMember(session, g);
            else if (fromHost) {
                LOGI("net: host closed the arena"); notice("HOST CLOSED THE ARENA");
                mode_ = Mode::Offline; peerIp_ = 0; peerSeen_ = 0; room_ = RoomInfo();
            }
        } else if (type == MSG_REJECT && fromHost) {
            char why[33] = {};
            for (int i = 0; i < 32; ++i) why[i] = (char)r.get<uint8_t>();
            for (char& c : why) if (c && (c < 32 || c > 'Z')) c = ' ';
            notice(why[0] ? why : "ARENA FULL");
            LOGI("net: host refused: %s", why);
            mode_ = Mode::Offline; peerIp_ = 0; peerSeen_ = 0; room_ = RoomInfo();
        } else if (type == MSG_JOIN && mode_ == Mode::Host) {
            if (Member* m = member(session)) { if (m->ip == fromIp) m->seen = now_; continue; }
            if ((int)members_.size() >= 2 + kMaxWaiting) { reject(fromIp, "ARENA FULL"); continue; }
            Member m;
            m.ip = fromIp; m.id = session; m.seen = now_;
            r.get<uint8_t>();   // "play now" from older builds: everyone now starts in the room's waiting area
            if (r.e - r.p >= 16) readName(r, m.name);   // 0.3.1+
            bool out = false;   // voted out of this arena for this session: by session, address or name
            for (const Removed& x : removed_) out |= x.id == session || x.ip == fromIp || (x.name[0] && !std::strcmp(x.name, m.name));
            if (out) { reject(fromIp, "REMOVED FROM THIS ARENA"); continue; }
            members_.push_back(m);
            compactTank(g, false);   // ... and gets into the tank through the queue (owner request)
            notice("A PLAYER IS WAITING");
            peerSeen_ = now_;
            LOGI("net: %s entered the room (%d people)", ipStr(fromIp), (int)members_.size());
        } else if (type == MSG_PING && (fromHost || fromMember)) {   // answer at once with the sender's own clock
            const double t = r.get<double>();
            if (r.ok) {
                Writer w; header(w, MSG_PONG, session_, 0); w.put(t);
                sendTo(w.b.data(), (int)w.b.size(), fromIp, kPort);
            }
        } else if (type == MSG_PONG && (fromHost || fromMember)) {
            const double t = r.get<double>();
            const float ms = float((now_ - t) * 1000.0);
            if (r.ok && ms >= 0.f && ms < 5000.f) {
                if (!pingN_ || ms < pingMin_) pingMin_ = ms;
                if (!pingN_ || ms > pingMax_) pingMax_ = ms;
                pingSum_ += ms;
                ++pingN_;
                lastPingMs_ = ms;
            }
        } else if (type == MSG_INPUT && fromMember) {
            fromMember->seen = now_;
            peerSeen_ = now_;
            if (seq > fromMember->lastSeq) {
                fromMember->lastSeq = seq;
                readInput(r, fromMember->in);
                if (fromMember->id == slot_[0] || fromMember->id == slot_[1]) countSeq(seq);
            }
        } else if (type == MSG_KICKVOTE && fromMember) {
            const uint32_t target = r.get<uint32_t>();
            if (r.ok) countKickVote(target, session);
        } else if (type == MSG_QUEUE && fromMember) {
            const uint8_t v = r.get<uint8_t>();
            if (v == 2) { forfeit_ = session; continue; }
            bool want = v != 0;
            auto it = std::find(queue_.begin(), queue_.end(), session);
            const bool inTank = slot_[0] == session || slot_[1] == session;
            if (want && it == queue_.end() && !inTank && (int)queue_.size() < kMaxQueue) queue_.push_back(session);   // full: stays waiting
            if (!want && it != queue_.end()) queue_.erase(it);
            hostRoomInfo();
        } else if (type == MSG_VOICE) {
            r.get<uint32_t>(); r.get<uint8_t>();
            if (!r.ok || r.e - r.p < kVoiceSamples) continue;
            const uint8_t* pcm = r.p;
            if (mode_ == Mode::Host) {   // a member or a spectator at this arena talks: pass it on to the room
                Spectator* sp = nullptr;
                for (Spectator& x : spectators_) if (x.ip == fromIp && x.id == session && x.where == 1) sp = &x;
                if (fromMember)
                    forwardVoice(session, (session == slot_[0] || session == slot_[1]) ? VOICE_PLAYER : VOICE_WAITING, pcm, fromIp, session);
                else if (sp)
                    forwardVoice(session, VOICE_SPECTATOR, pcm, fromIp, session);
            } else if (fromHost || (mode_ == Mode::Spectate && presWhere_ == 1 && fromIp == presWatch_)) {
                Reader h{pkt + 14, pkt + n};   // re-read the speaker / role the host filled in
                VoiceFrame f; f.speaker = h.get<uint32_t>(); f.role = h.get<uint8_t>();
                if (f.speaker != session_ && f.role <= VOICE_SPECTATOR && voiceIn_.size() < 64) {
                    std::memcpy(f.data, pcm, kVoiceSamples);
                    voiceIn_.push_back(f);
                }
            }
        } else if (type == MSG_CAMERAS) {
            const uint8_t* body = r.p;
            uint32_t origin = r.get<uint32_t>();
            char team[17] = {};
            for (int i = 0; i < 16; ++i) team[i] = (char)r.get<uint8_t>();
            uint16_t len = r.get<uint16_t>();
            if (!r.ok || len > 400 || r.e - r.p < len) continue;
            if (mode_ == Mode::Host) {   // pass a teammate's cameras on to our spectators (not back to the sender)
                Writer w; header(w, MSG_CAMERAS, session_, 0);
                w.b.insert(w.b.end(), body, r.p + len);
                for (const Spectator& sp : spectators_) if (sp.id != origin) sendTo(w.b.data(), (int)w.b.size(), sp.ip, kPort);
            } else if (mode_ == Mode::Spectate && origin != session_) {
                std::memcpy(camTeam_, team, 17);
                camBlob_.assign(r.p, r.p + len);
                camFrom_ = origin;
                camFresh_ = true;
            }
        } else if (type == MSG_CROWD) {
            if (fromHost) readCrowd(r.p, r.e, crowd_, now_);
            else if (mode_ == Mode::Spectate)
                for (SpecMatch& m : matches_) if (m.ip == fromIp) readCrowd(r.p, r.e, m.crowd, now_);
        } else if (type == MSG_ROOM && fromHost) {
            peerSession_ = session;
            peerSeen_ = now_;
            readRoom(r, room_, lastRoomSeq_);
        } else if (type == MSG_STATE && fromHost) {
            peerSession_ = session;   // the host's session, learned from its beacon or first snapshot
            peerSeen_ = now_;
            readEvents(r, lastEventId_, pendingEvents_);   // every packet, even one overtaken by a newer snapshot
            if (seq > lastRecvSeq_) { countSeq(seq); lastState_.assign(r.p, r.e); gotState = true; } else ++statLate_;
        }
    }
    if (online_) {   // keep the relay session alive; say hello again until welcomed
        if (!vip_ && now_ - lastHello_ > 1.0) relayHello();
        else if (vip_ && now_ - lastRelayPing_ > 2.0) {
            lastRelayPing_ = now_;
            uint8_t b[21] = {};
            std::memcpy(b, &kRelayMagic, 4); b[4] = R_PING; std::memcpy(b + 5, token_, 16);
            rawSend(b, sizeof b, serverIp_, serverPort_);
        }
        if (now_ - lastRelayHeard_ > 12.0 && vip_) { vip_ = 0; notice("SERVER NOT RESPONDING"); LOGW("net: relay silent"); }
        bool anyKey = false;
        for (uint8_t k : statsKey_) anyKey |= k != 0;
        if (vip_ && anyKey && !keySent_) { relaySend(R_KEY, statsKey_, 16); keySent_ = true; }
        if (vip_ && mode_ == Mode::Host && now_ - lastTank_ > 3.0) {   // who is in the tank (stats go to them)
            lastTank_ = now_;
            uint32_t v[2] = {0, 0};
            for (int k = 0; k < 2; ++k) {
                if (!slot_[k]) continue;
                for (const Member& m : members_) if (m.id == slot_[k]) v[k] = m.local ? vip_ : m.ip;
            }
            relaySend(R_TANK, v, 8);
        }
        if (vip_ && mode_ == Mode::Host && liveSet_ && now_ - lastLive_ > 1.0) {   // The Crowd: our match, for the fans
            lastLive_ = now_;
            uint8_t b[82] = {};
            b[0] = live_.state;
            std::memcpy(b + 1, live_.score, 4);
            b[5] = live_.nation[0]; b[6] = live_.nation[1];
            std::memcpy(b + 7, live_.rgb, 6);
            b[13] = live_.flags;
            const uint16_t cl = (uint16_t)std::min(65535.f, std::max(0.f, live_.clock * 10.f));
            std::memcpy(b + 14, &cl, 2);
            std::memcpy(b + 16, live_.name[0], 16); std::memcpy(b + 32, live_.name[1], 16);
            for (int k = 0; k < 2; ++k) {   // this match so far
                const MatchStats& s = live_.st[k];
                const uint16_t v[8] = {s.throws, s.onTarget, s.hits, s.blocks, s.faced, s.catches, s.strikes,
                                       (uint16_t)std::min(6000.f, std::max(0.f, s.topSpeed * 100.f))};
                std::memcpy(b + 48 + 16 * k, v, 16);
            }
            b[80] = live_.round; b[81] = live_.overtime;
            relaySend(R_LIVE, b, sizeof b);
        }
    }
    if (inRoom() && now_ - lastPingSent_ > 2.0) {   // the round trip: the client to its host, the host to each player
        lastPingSent_ = now_;
        Writer w; header(w, MSG_PING, session_, 0); w.put(now_);
        if (mode_ == Mode::Client && peerIp_) sendTo(w.b.data(), (int)w.b.size(), peerIp_, kPort);
        if (mode_ == Mode::Host)
            for (const Member& m : members_)
                if (!m.local && m.ip) sendTo(w.b.data(), (int)w.b.size(), m.ip, kPort);
    }
    if (inRoom() && peerSeen_ > 0 && now_ - statStart_ > 10.0) {
        double secs = now_ - statStart_;
        char ping[64] = "";
        if (pingN_) std::snprintf(ping, sizeof ping, ", ping %.0f ms (%.0f - %.0f)", pingSum_ / pingN_, pingMin_, pingMax_);
        if (statRecv_)
            LOGI("net stats: %s recv %.0f pkt/s, lost %u of %u (%.1f%%), late/dup %u%s", mode_ == Mode::Host ? "host" : "client",
                 statRecv_ / secs, statLost_, statRecv_ + statLost_, 100.0 * statLost_ / std::max(1u, statRecv_ + statLost_), statLate_, ping);
        resetStats();
    }
    spectators_.erase(std::remove_if(spectators_.begin(), spectators_.end(), [&](const Spectator& sp) { return now_ - sp.seen > 4.0; }),
                      spectators_.end());
    switch (mode_) {
    case Mode::Spectate: {
        if (now_ - lastSpectatePing_ > 0.2) {   // subscribe to every live host we can hear, saying where we are
            lastSpectatePing_ = now_;
            for (const LanGame& lg : games_) {
                if (now_ - lg.seen > 2.5) continue;
                uint8_t where = presWhere_ == 0 ? 0 : (presWatch_ == lg.ip ? 1 : 2);
                Writer w; header(w, MSG_SPECTATE, session_, 0); w.put(where); w.put(presPos_); writeName(w, name_);
                sendTo(w.b.data(), (int)w.b.size(), lg.ip, kPort);
            }
        }
        for (SpecMatch& m : matches_) {
            if (!m.fresh) continue;
            Reader rs{m.state.data(), m.state.data() + m.state.size()};
            readState(rs, m.game, -1);
            m.fresh = false;
            for (const LanGame& lg : games_) if (lg.ip == m.ip) std::memcpy(m.name, lg.name, sizeof m.name);
        }
        matches_.erase(std::remove_if(matches_.begin(), matches_.end(), [&](const SpecMatch& m) { return now_ - m.lastSeen > 4.0; }),
                       matches_.end());
        return false;   // the local game is not simulated while spectating
    }
    case Mode::Offline:
        if (!g.pl[1].isBot) g.reset(true);
        in[0] = local;
        return true;
    case Mode::Searching:
        if (now_ - searchStart_ > 1.5) { host(); LOGI("net: no host found, hosting"); }
        in[0] = local;
        return true;   // keep playing the bot while searching
    case Mode::Host: {
        // members who went quiet leave; our own input updates our member (we may be in the tank or waiting)
        std::vector<uint32_t> gone;
        for (const Member& m : members_) if (!m.local && now_ - m.seen > kMemberTimeout) gone.push_back(m.id);
        for (uint32_t id : gone) removeMember(id, g);
        if (Member* me = member(session_)) { me->in = local; me->seen = now_; }
        if (forfeit_) {   // a player gave up: out to the waiting area, the bot or the queue takes the place
            const uint32_t id = forfeit_;
            forfeit_ = 0;
            if (slot_[0] == id || slot_[1] == id) {
                if (slot_[0] == id) slot_[0] = 0;
                if (slot_[1] == id) slot_[1] = 0;
                compactTank(g, true);
                notice("PLAYER FORFEITED");
                LOGI("net: forfeit -> waiting area");
            }
        }
        fillTank(g);
        // rotation: a finished match of two humans (or with someone queued) empties the tank after a short pause
        const bool rotateRule = humans() == 2 || (humans() == 1 && !queue_.empty());
        if (rotateRule && g.match == MatchState::MatchSummary) {
            if (rotateAt_ == 0) { rotateAt_ = now_ + kRotateDelay; notice("NEXT PLAYERS UP SOON"); }
            else if (now_ > rotateAt_) rotate(g);
        } else if (g.match != MatchState::MatchSummary) {
            rotateAt_ = 0;
        }
        hostRoomInfo();
        if (now_ - lastBeacon_ > 0.5) {
            lastBeacon_ = now_;
            int waiting = (int)room_.members.size();
            Writer w; header(w, MSG_BEACON, session_, 0);   // beacons stay out of the sequence (loss stats)
            for (int i = 0; i < 16; ++i) w.put<uint8_t>((uint8_t)name_[i]);
            w.put<uint8_t>(g.tun.advanced); w.put<uint8_t>((int)members_.size() >= 2 + kMaxWaiting);
            w.put<int16_t>((int16_t)g.pl[0].score); w.put<int16_t>((int16_t)g.pl[1].score); w.put<float>(g.roundClock);
            w.put<uint8_t>((uint8_t)std::min<size_t>(255, spectators_.size()));
            w.put<uint8_t>((uint8_t)waiting); w.put<uint8_t>((uint8_t)queue_.size()); w.put<uint8_t>((uint8_t)humans());
            sendTo(w.b.data(), (int)w.b.size(), INADDR_BROADCAST, kPort);
        }
        if (now_ - lastRoomSent_ > 1.0 / 15.0) { lastRoomSent_ = now_; sendRoom(); }
        // the game is stepped with the tank players' inputs; slot 1 empty = the bot (Game fills its input)
        for (int p = 0; p < 2; ++p) {
            in[p] = PlayerInput();
            if (!slot_[p]) continue;
            if (Member* m = member(slot_[p])) in[p] = m->local ? local : m->in;
        }
        if (rotateRule) { in[0].buttonA = false; in[1].buttonA = false; }   // no skipping the rotation with A
        if (!humans()) return false;   // empty tank: the arena idles until someone queues
        const bool wantBot = slot_[1] == 0 && botFill_;
        if (g.pl[1].isBot != wantBot) g.reset(wantBot);
        return true;   // caller steps; snapshot is sent via sendState
    }
    case Mode::Client: {
        // the snapshots do not say who is a bot: take it from the room (only slot 1 can be the host's bot). A flag
        // left over from practice or the lobby made the host flicker between its own look and the bot's, unsmoothed.
        g.pl[0].isBot = false;
        g.pl[1].isBot = room_.bot;
        if (localSlot() < 0)
            for (const LanGame& lg : games_) if (lg.ip == peerIp_ && now_ - lg.seen < 3.0) g.tun.advanced = lg.advanced;
        Writer w; header(w, MSG_INPUT, session_, ++seq_); writeInput(w, local);
        sendTo(w.b.data(), (int)w.b.size(), peerIp_, kPort);
        const bool known = room_.slot[0] == session_ || room_.slot[1] == session_ ||
                           std::any_of(room_.members.begin(), room_.members.end(), [&](const RoomInfo::Member& m) { return m.id == session_; });
        if (!known && now_ - lastJoin_ > 1.0) {   // the JOIN may have been lost: ask again until the room lists us
            lastJoin_ = now_;
            Writer j; header(j, MSG_JOIN, session_, ++seq_); j.put<uint8_t>(joinPlay_ ? 1 : 0); writeName(j, name_);
            sendTo(j.b.data(), (int)j.b.size(), peerIp_, kPort);
        }
        const int s = localSlot();
        if (gotState) {
            Reader rs{lastState_.data(), lastState_.data() + lastState_.size()};
            readState(rs, g, s);
        }
        g.events.swap(pendingEvents_);
        pendingEvents_.clear();
        if (s >= 0) g.pl[s].in = local;
        (void)dt; (void)in;
        if (!peerConnected() && now_ - peerSeen_ > 5.0) {
            mode_ = Mode::Offline; peerIp_ = 0; room_ = RoomInfo();
            LOGW("net: host lost"); notice("CONNECTION TO HOST LOST");
        }
        return false;
    }
    }
    return true;
}

void Net::sendState(const Game& g) {
    if (mode_ != Mode::Host) return;
    for (const GameEvent& e : g.events) eventLog_.push_back({nextEventId_++, now_, e});
    while (!eventLog_.empty() && (now_ - eventLog_.front().t > 0.3 || eventLog_.size() > 24)) eventLog_.pop_front();
    bool anyone = !spectators_.empty();
    for (const Member& m : members_) anyone |= !m.local;
    if (!anyone) return;
    // full rate (every frame) to the two players in the tank; 30 Hz to the people outside and spectators (bandwidth,
    // and the relay's per-player rate limit)
    const bool slow = now_ - lastSlowState_ >= 1.0 / 30.0;
    if (slow) lastSlowState_ = now_;
    Writer w; header(w, MSG_STATE, session_, ++seq_); writeEvents(w, eventLog_); writeState(w, g);
    for (const Member& m : members_) {
        if (m.local) continue;
        const bool player = m.id == slot_[0] || m.id == slot_[1];
        if (player || slow) sendTo(w.b.data(), (int)w.b.size(), m.ip, kPort);
    }
    if (slow) for (const Spectator& sp : spectators_) sendTo(w.b.data(), (int)w.b.size(), sp.ip, kPort);
}
