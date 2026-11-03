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
// queue, rotation, spectator presence); 9 (0.3.12): FOUR CORNERS (four places), compact snapshots, the EXPERIMENTAL
// rooms' rules in beacons and snapshots
static const uint8_t kVersion = 10;   // 10: 0.3.13 (FIREBALL BLITZ: health, the ring and the fireballs in STATE)
// PING / PONG: the round trip between a host and each player (logged with the net stats). Builds without them ignore
// both, so the protocol version stays.
enum : uint8_t { MSG_BEACON = 1, MSG_JOIN = 2, MSG_INPUT = 3, MSG_STATE = 4, MSG_LEAVE = 5, MSG_SPECTATE = 6,
                 MSG_REJECT = 7, MSG_QUEUE = 8, MSG_ROOM = 9, MSG_VOICE = 10, MSG_CAMERAS = 11, MSG_PING = 12, MSG_PONG = 13,
                 MSG_KICKVOTE = 14, MSG_CROWD = 15, MSG_HEIRS = 16, MSG_HANDOFF = 17, MSG_CLOSING = 18 };
// (14: VOTE TO REMOVE, body: the target's id; older hosts ignore it)
// (15: The Crowd, host -> room and spectators: the relay's CROWD body, 12 bytes; older builds ignore it)
// (16-18, 0.3.13, HOST HANDOFF and IDLE ROOMS, host -> room. HEIRS: flags, the room's name, then who hosts next in
//  order (id, address, name); HANDOFF: the new host's id and whether the old one stays; CLOSING: seconds left, 0 closed)
static const double kHostQuiet = 5.0;             // a host silent this long: the next one takes over
static const double kHeirWait = 8.0;              // ... and this long for each heir to answer before the one after
static const double kResumeWait = 9.0;            // a room taken over waits this long for its players to come back
// community relay (server/projectarena_relay.py)
static const uint32_t kRelayMagic = 0x4C52414F;   // 'OARL'
static const uint16_t kRelayPort = 47780;
enum : uint8_t { R_HELLO = 1, R_WELCOME = 2, R_SEND = 3, R_PING = 4, R_BYE = 5, R_DELIVER = 6, R_ERROR = 7,
                 R_KEY = 8, R_TANK = 9, R_MATCH = 10, R_STATSREQ = 11, R_STATS = 12, R_CONFIRM = 13, R_DELETE = 14,
                 R_MYSTATS = 15, R_LIVE = 16, R_CROWD = 17,     // (8-15: player statistics; 16-17: The Crowd)
                 R_TRAIN = 18, R_BOARDREQ = 19, R_BOARD = 20 };  // (18-20: TRAINING's leaderboards, 0.3.12)
static const double kRotateDelay = 6.0;          // match summary -> both players out, the queue in
static const double kMemberTimeout = 4.0;

static const char* ipStr(uint32_t ip) { in_addr ia{}; ia.s_addr = ip; return inet_ntoa(ia); }

// IDLE ROOMS: has this person moved (head 5 cm, a hand 10 cm) or changed a button since `ref`? (A headset left on a
// desk still tracks: it just doesn't move.) Moving resets `ref`.
static bool stirred(Vec3 ref[3], uint8_t& refButtons, const PlayerInput& in) {
    uint8_t b = in.buttonA ? 1 : 0;
    for (int h = 0; h < 2; ++h) b |= (uint8_t)((in.hand[h].trigger > 0.5f ? 2 : 0) << h) | (uint8_t)((in.hand[h].squeeze > 0.5f ? 8 : 0) << h);
    const Vec3 now[3] = {in.head.p, in.hand[0].grip.p, in.hand[1].grip.p};
    bool any = b != refButtons;
    for (int k = 0; k < 3; ++k) any |= length(now[k] - ref[k]) > (k ? 0.10f : 0.05f);
    if (any) { for (int k = 0; k < 3; ++k) ref[k] = now[k]; refButtons = b; }
    return any;
}

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
// the tank players in STATE (0.3.12): what the others draw and the host's game needs, quantised so four of them fit
// the relay's 1200 bytes: rotations as four 16-bit numbers, triggers as bytes, no angular velocity (the host's alone)
static void putQuat(Writer& w, const Quat& q) {
    const float c[4] = {q.x, q.y, q.z, q.w};
    for (float v : c) w.put<int16_t>((int16_t)std::lround(clampf(v, -1.f, 1.f) * 32767.f));
}
static Quat getQuat(Reader& r) {
    Quat q;
    q.x = r.get<int16_t>() / 32767.f; q.y = r.get<int16_t>() / 32767.f; q.z = r.get<int16_t>() / 32767.f; q.w = r.get<int16_t>() / 32767.f;
    const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (l > 1e-3f && std::isfinite(l)) { q.x /= l; q.y /= l; q.z /= l; q.w /= l; } else q = Quat{};
    return q;
}
static uint8_t byteOf(float v) { return (uint8_t)std::lround(clampf(v, 0.f, 1.f) * 255.f); }
static void writeTankInput(Writer& w, const PlayerInput& in) {
    putQuat(w, in.head.q); w.put(in.head.p);
    w.put<uint8_t>((in.headTracked ? 1 : 0) | (in.buttonA ? 2 : 0) | (in.hand[0].tracked ? 4 : 0) | (in.hand[1].tracked ? 8 : 0));
    for (int h = 0; h < 2; ++h) { putQuat(w, in.hand[h].grip.q); w.put(in.hand[h].grip.p); w.put(byteOf(in.hand[h].trigger)); w.put(byteOf(in.hand[h].squeeze)); }
    w.put(in.avatar);
}
static void readTankInput(Reader& r, PlayerInput& in) {
    in.head.q = getQuat(r); in.head.p = r.get<Vec3>();
    const uint8_t f = r.get<uint8_t>();
    in.headTracked = f & 1; in.buttonA = (f & 2) != 0; in.hand[0].tracked = (f & 4) != 0; in.hand[1].tracked = (f & 8) != 0;
    for (int h = 0; h < 2; ++h) {
        in.hand[h].grip.q = getQuat(r); in.hand[h].grip.p = r.get<Vec3>();
        in.hand[h].trigger = r.get<uint8_t>() / 255.f; in.hand[h].squeeze = r.get<uint8_t>() / 255.f;
        in.hand[h].angVel = {};
    }
    in.avatar = r.get<AvatarDesc>();
    avatar::sanitize(in.avatar);
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
    w.put(seq);
    for (int p = 0; p < kMaxPlayers; ++p) w.put(ri.slot[p]);
    w.put<uint8_t>(ri.botMask);
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
    for (int p = 0; p < kMaxPlayers; ++p) n.slot[p] = r.get<uint32_t>();
    n.botMask = r.get<uint8_t>() & 0x0F;
    n.bot = n.isBot(1);
    uint8_t nm = r.get<uint8_t>();
    for (uint8_t i = 0; i < nm && i < 8 && r.ok; ++i) {
        RoomInfo::Member m;
        m.id = r.get<uint32_t>(); m.role = r.get<uint8_t>(); m.queuePos = r.get<int8_t>(); m.spot = r.get<uint8_t>() & 7;
        readPose(r, m.in);
        if (m.queuePos > 3) m.queuePos = 3;   // (four queue for a room for four; 0.3.12 said 1 for all after the first)
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

void Net::reportMatch(int winner, const MatchStats st[2], const uint8_t score[4], int mode) {
    // (a match taken over from another host: our tally only has its end, so careers don't count it)
    if (mode_ != Mode::Host || resumed_ || !online_ || !vip_ || np_ != 2 || winner < 0 || winner > 1 || !slot_[0] || !slot_[1]) return;
    uint8_t b[38];
    b[33] = (uint8_t)std::max(0, std::min(2, mode));   // (relays before 0.3.8 read the first 33 bytes)
    std::memcpy(b + 34, &st[0].returnHits, 2);         // (0.3.9: the points from returns; older relays: 34 bytes)
    std::memcpy(b + 36, &st[1].returnHits, 2);
    std::memcpy(b + 29, score, 4);
    b[0] = (uint8_t)winner;
    for (int k = 0; k < 2; ++k) {
        const uint16_t v[7] = {st[k].throws, st[k].onTarget, st[k].hits, st[k].blocks, st[k].faced, st[k].catches,
                               (uint16_t)std::min(6000.f, std::max(0.f, st[k].topSpeed * 100.f))};
        std::memcpy(b + 1 + 14 * k, v, 14);
    }
    relaySend(R_MATCH, b, sizeof b);
    LOGI("stats: match reported (winner slot %d, mode %d)", winner, mode);
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

void Net::submitTraining(int lessonId, float seconds, int successes, int attempts) {
    if (lessonId < 1 || lessonId > 255 || successes < 0 || attempts < 1) return;
    std::vector<uint8_t> b(8);
    b[0] = (uint8_t)lessonId; b[1] = 1;
    const uint32_t ms = (uint32_t)std::max(0.f, std::min(3600.f, seconds) * 1000.f);
    std::memcpy(&b[2], &ms, 4);
    b[6] = (uint8_t)std::min(255, successes); b[7] = (uint8_t)std::min(255, attempts);
    for (const auto& q : trainQ_) if (q == b) return;
    trainQ_.push_back(b);
}

void Net::requestBoard(int lessonId, int kind) {
    const uint8_t b[2] = {(uint8_t)lessonId, (uint8_t)(kind ? 1 : 0)};
    relaySend(R_BOARDREQ, b, 2);
}

const Net::Board* Net::board(int lessonId, int kind) const {
    for (const Board& x : boards_) if (x.lesson == lessonId && x.kind == kind) return &x;
    return nullptr;
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
    uint8_t b[38 + 16] = {};   // then this build's version (length + ASCII; relays before 0.3.11 ignore it)
    std::memcpy(b, &kRelayMagic, 4); b[4] = R_HELLO;   // token: zeros
    b[21] = kVersion;
    std::memcpy(b + 22, name_, 16);
    b[38] = (uint8_t)std::strlen(app_);
    std::memcpy(b + 39, app_, b[38]);
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
    if (!inherited_) std::memcpy(roomName_, name_, sizeof roomName_);        // (our room has our name)
    if (online_ && serverIp_) relayHello();   // the relay knows us by name (its log, player reports)
}

void Net::setAppVersion(const char* v) {
    std::snprintf(app_, sizeof app_, "%s", v ? v : "");
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
    me.id = session_; me.local = true; me.seen = now_; me.active = now_;
    members_.push_back(me);
    std::memcpy(roomName_, name_, sizeof roomName_);
    prevHost_ = 0; resumeWait_ = 0; resumed_ = false;
    for (uint32_t& s : slot_) s = 0;   // the host starts in the room's waiting area too: JOIN QUEUE to play (owner request)
    oneFixed_ = false;
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
    joinPlay_ = play;
    sendJoin(play);
    LOGI("net: joining host %s (%s)", ipStr(ip), play ? "play" : "room");
}

void Net::leave() {
    if (mode_ == Mode::Client && peerIp_) {   // tell the host right away
        Writer w; header(w, MSG_LEAVE, session_, 0);
        sendTo(w.b.data(), (int)w.b.size(), peerIp_, kPort);
    }
    if (mode_ == Mode::Host) {                // people still here: the room goes on with the next host (HOST HANDOFF)
        uint32_t next = 0;   // (someone who is here: not one still finding a room we took over)
        for (const Member& m : members_) if (!m.local && m.back && !next) next = m.id;
        if (next) handOver(next, false);
        else closeRoom();
    }
    if (mode_ == Mode::Host || mode_ == Mode::Client || mode_ == Mode::Spectate) LOGI("net: offline");
    stepDown();
    dropRoom(nullptr);
    matches_.clear();
}

void Net::closeRoom() {   // host: tell everyone in the room the arena is closing
    Writer w; header(w, MSG_LEAVE, session_, 0);
    for (const Member& m : members_) if (!m.local) sendTo(w.b.data(), (int)w.b.size(), m.ip, kPort);
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
        if (want && it == queue_.end() && !inTank(session_) && (int)queue_.size() < np_) queue_.push_back(session_);
        if (!want && it != queue_.end()) queue_.erase(it);
        hostRoomInfo();   // the tank fills on the next exchange
    }
}

int Net::localSlot() const {
    if (mode_ == Mode::Host) { for (int p = 0; p < kMaxPlayers; ++p) if (slot_[p] == session_) return p; return -1; }
    if (mode_ == Mode::Client) { for (int p = 0; p < kMaxPlayers; ++p) if (room_.slot[p] == session_) return p; return -1; }
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
    return room_.slot[p] != 0 || room_.isBot(p);
}

const char* Net::status() const {
    const char* where = online_ ? "ONLINE" : "LAN";
    switch (mode_) {
    case Mode::Offline: return "PRACTICE VS BOT";
    case Mode::Searching: return "SEARCHING LAN...";
    case Mode::Spectate: return "SPECTATING";
    case Mode::Host:
    case Mode::Client: {
        if (mode_ == Mode::Client && migrating_) { snprintf(status_, sizeof status_, "%s - CHANGING HOST", where); return status_; }
        if (mode_ == Mode::Client && !peerConnected()) { snprintf(status_, sizeof status_, "%s - HOST NOT RESPONDING", where); return status_; }
        int s = localSlot(), q = localQueuePos();
        int waiting = 0;
        for (const RoomInfo::Member& m : room_.members) waiting += m.role == RoomInfo::WAITING;
        int others = 0;
        for (int p = 0; p < kMaxPlayers; ++p) others += p != s && room_.slot[p];
        if (s >= 0 && np_ == 4) snprintf(status_, sizeof status_, "%s %s - %s%s", where, mode_ == Mode::Host ? "HOST" : "ARENA",
                                         expRoom_ == ROOM_BLITZ ? "FIREBALL BLITZ" : "FOUR CORNERS",
                                         waiting ? " - PLAYERS WAITING" : "");
        else if (s >= 0) snprintf(status_, sizeof status_, "%s %s - %s%s", where, mode_ == Mode::Host ? "HOST" : "ARENA",
                             others ? "VS PLAYER" : room_.bot ? "VS BOT" : "WAITING FOR AN OPPONENT",
                             waiting ? " - PLAYERS WAITING" : "");
        else if (q >= 0) snprintf(status_, sizeof status_, "%s - QUEUED %s", where, q == 0 ? "NEXT" : q == 1 ? "SECOND" : q == 2 ? "THIRD" : "FOURTH");
        else snprintf(status_, sizeof status_, "%s - WAITING OUTSIDE THE TANK", where);
        return status_;
    }
    }
    return "";
}

// ------------------------------------------------------------------ snapshot
static void writeState(Writer& w, const Game& g) {
    w.put<uint8_t>((uint8_t)g.match); w.put(g.stateTimer); w.put(g.roundClock); w.put<int16_t>((int16_t)g.round);
    w.put<int8_t>((int8_t)g.lastRoundWinner); w.put<int8_t>((int8_t)g.matchWinner); w.put<uint8_t>(g.overtime); w.put<uint8_t>(g.tun.advanced);
    // the rules: MATCH OPTIONS, and (0.3.12) the EXPERIMENTAL room, its players and teams
    w.put<uint8_t>(g.tun.shields); w.put<int8_t>((int8_t)g.tun.knuckleMode); w.put<float>(g.tun.basicMatchTime);
    w.put<float>(g.tun.advancedRoundTime);
    w.put<uint8_t>(g.tun.experimental); w.put<uint8_t>((uint8_t)g.tun.expRoom); w.put<uint8_t>((uint8_t)g.np);
    w.put<uint8_t>((uint8_t)g.tun.teamMode); w.put<uint8_t>((uint8_t)g.tun.theOne);
    for (int p = 0; p < g.np; ++p) {
        const PlayerState& s = g.pl[p];
        writeTankInput(w, s.in);
        w.put<uint8_t>((s.hands[0].shieldUp ? 1 : 0) | (s.hands[1].shieldUp ? 2 : 0) | (s.hands[0].knuckleUp ? 4 : 0) |
                       (s.hands[1].knuckleUp ? 8 : 0) | (s.ball.charged ? 16 : 0) | (s.ball.scored ? 32 : 0) | (s.isBot ? 64 : 0) |
                       (s.ready ? 128 : 0));   // (READY UP, 0.3.14)
        w.put<int16_t>((int16_t)s.score); w.put<uint8_t>((uint8_t)s.roundWins); w.put<uint8_t>((uint8_t)s.strikesAgainst);
        w.put<uint8_t>((uint8_t)s.strikesFor);
        w.put<uint8_t>((uint8_t)s.ball.state); w.put(s.ball.pos); w.put(s.ball.vel); w.put<int8_t>((int8_t)s.ball.heldHand);
        w.put<int8_t>((int8_t)s.ball.instigator); w.put(s.ball.scale);
    }
    if (g.tun.experimental && g.tun.expRoom == ROOM_BLITZ) {
        // FIREBALL BLITZ: everyone's health, who started and who stands in the ring, when it closes, its options, and the
        // fireballs in the room (centimetres and centimetres a second: a full room of them fits the relay's 1200 bytes)
        for (int p = 0; p < g.np; ++p) w.put<uint8_t>((uint8_t)std::max(0, std::min(255, g.pl[p].health)));
        w.put<uint8_t>(g.startMask); w.put<uint8_t>(g.ringMask);
        w.put<int16_t>((int16_t)(g.ringCloseAt < 0 ? -1 : std::lround(std::max(0.f, g.ringCloseAt - g.now) * 100.f)));
        w.put<uint8_t>((uint8_t)g.tun.health); w.put<uint8_t>((uint8_t)g.tun.fireBalls);
        auto cm = [](float v) { return (int16_t)std::max(-32000L, std::min(32000L, std::lround(v * 100.f))); };
        for (int p = 0; p < g.np; ++p) {
            uint8_t m = 0;
            for (int k = 0; k < Game::kMaxFire; ++k) m |= g.fire[p][k].state == BallState::Flying ? (uint8_t)(1u << k) : 0;
            w.put<uint8_t>(m);
            for (int k = 0; k < Game::kMaxFire; ++k) {
                const Ball& f = g.fire[p][k];
                if (f.state != BallState::Flying) continue;
                w.put<int16_t>(cm(f.pos.x)); w.put<int16_t>(cm(f.pos.y)); w.put<int16_t>(cm(f.pos.z));
                w.put<int16_t>(cm(f.vel.x)); w.put<int16_t>(cm(f.vel.y)); w.put<int16_t>(cm(f.vel.z));
                w.put<int8_t>((int8_t)f.instigator);
            }
        }
    }
    // (0.3.14) the two players' shot clocks, tenths of a second, for the ring round each launchpad; builds before it
    // stop reading before them
    if (g.np == 2)
        for (int p = 0; p < 2; ++p) w.put<uint8_t>((uint8_t)std::max(0L, std::min(255L, std::lround(g.pl[p].shotClock * 10.f))));
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
    g.match = (MatchState)r.get<uint8_t>(); g.stateTimer = r.get<float>(); g.roundClock = r.get<float>(); g.round = r.get<int16_t>();
    g.lastRoundWinner = r.get<int8_t>(); g.matchWinner = r.get<int8_t>(); g.overtime = r.get<uint8_t>(); g.tun.advanced = r.get<uint8_t>();
    g.tun.shields = r.get<uint8_t>() != 0;
    g.tun.knuckleMode = std::max(-1, std::min(1, (int)r.get<int8_t>()));
    const float mt = r.get<float>(), rt = r.get<float>();
    if (std::isfinite(mt) && mt >= 0 && mt <= 3600) g.tun.basicMatchTime = mt;
    if (std::isfinite(rt) && rt >= 0 && rt <= 3600) g.tun.advancedRoundTime = rt;
    g.tun.experimental = r.get<uint8_t>() != 0;
    g.tun.expRoom = std::min<int>(r.get<uint8_t>(), kRooms - 1);
    const int np = r.get<uint8_t>() == 4 ? 4 : 2;
    g.tun.teamMode = std::min(2, (int)r.get<uint8_t>());
    g.tun.theOne = r.get<uint8_t>() & 3;
    if (!r.ok) return false;
    g.np = np;
    g.tun.players = np;
    for (int p = 0; p < np; ++p) {
        PlayerState& s = g.pl[p];
        PlayerInput in; readTankInput(r, in);
        if (p != localIndex) s.in = in;   // keep our own latest local poses for latency-free hands
        const uint8_t f = r.get<uint8_t>();
        s.hands[0].shieldUp = f & 1; s.hands[1].shieldUp = (f & 2) != 0; s.hands[0].knuckleUp = (f & 4) != 0; s.hands[1].knuckleUp = (f & 8) != 0;
        s.ball.charged = (f & 16) != 0; s.ball.scored = (f & 32) != 0; s.isBot = (f & 64) != 0; s.ready = (f & 128) != 0;
        s.score = r.get<int16_t>(); s.roundWins = r.get<uint8_t>(); s.strikesAgainst = r.get<uint8_t>(); s.strikesFor = r.get<uint8_t>();
        s.ball.state = (BallState)std::min<uint8_t>(r.get<uint8_t>(), (uint8_t)BallState::Returning);
        s.ball.pos = r.get<Vec3>(); s.ball.vel = r.get<Vec3>(); s.ball.heldHand = r.get<int8_t>();
        if (s.ball.heldHand < -1 || s.ball.heldHand > 1) s.ball.heldHand = -1;
        s.ball.instigator = r.get<int8_t>(); s.ball.scale = r.get<float>();
        if (!std::isfinite(s.ball.scale) || s.ball.scale < 0.1f || s.ball.scale > 10.f) s.ball.scale = 1.f;
    }
    if (g.tun.experimental && g.tun.expRoom == ROOM_BLITZ) {
        for (int p = 0; p < np; ++p) g.pl[p].health = r.get<uint8_t>();
        g.startMask = r.get<uint8_t>() & 15;
        const uint8_t ring = r.get<uint8_t>() & 15;
        const int16_t closeIn = r.get<int16_t>();
        g.ringCloseAt = closeIn < 0 ? -1.f : g.now + closeIn / 100.f;
        g.tun.health = std::max(1, std::min(10, (int)r.get<uint8_t>()));
        g.tun.fireBalls = std::max(1, std::min(4, (int)r.get<uint8_t>()));
        if (r.ok && ring != g.ringMask) { g.ringMask = ring; g.layoutRing(ring); }   // (the room's layout as the host has it)
        for (int p = 0; p < np && r.ok; ++p) {
            const uint8_t m = r.get<uint8_t>();
            for (int k = 0; k < Game::kMaxFire; ++k) {
                Ball& f = g.fire[p][k];
                if (!((m >> k) & 1)) { f.state = BallState::Dead; continue; }
                Vec3 pos, vel;
                pos.x = r.get<int16_t>() / 100.f; pos.y = r.get<int16_t>() / 100.f; pos.z = r.get<int16_t>() / 100.f;
                vel.x = r.get<int16_t>() / 100.f; vel.y = r.get<int16_t>() / 100.f; vel.z = r.get<int16_t>() / 100.f;
                f.instigator = r.get<int8_t>();
                f.state = BallState::Flying; f.pos = pos; f.vel = vel; f.scale = 1.f;
                f.speedLimit = std::max(5.f, std::sqrt(vel.x * vel.x + vel.z * vel.z));   // (read ahead for its warning)
            }
        }
    }
    if (np == 2 && r.ok && r.e - r.p >= 2)   // the shot clocks (hosts from 0.3.14)
        for (int p = 0; p < 2; ++p) g.pl[p].shotClock = r.get<uint8_t>() / 10.f;
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

int Net::humans() const { int n = 0; for (int p = 0; p < kMaxPlayers; ++p) n += p < np_ && slot_[p] ? 1 : 0; return n; }
uint8_t Net::wantBots() const {   // the empty places a bot plays (two players: slot 1 only, as it always was)
    if (np_ == 2) return slot_[1] == 0 && botFill_ ? 2 : 0;
    uint8_t m = 0;   // (four players: any empty corner, once someone plays; the host can move people between corners)
    if (botFill_ && humans()) for (int p = 0; p < np_; ++p) if (!slot_[p]) m |= (uint8_t)(1u << p);
    return m;
}

void Net::freeSpot(Member& m) {   // the lowest holding-area spot no other waiting member stands on
    // (a room for four: eight spots, four on each side, so a full room with nobody playing has room for all; owner,
    // 2026-11-02)
    const int n = np_ == 4 ? 8 : 4;
    bool used[8] = {false, false, false, false, false, false, false, false};
    for (const Member& o : members_)
        if (&o != &m && !inTank(o.id)) used[o.spot % n] = true;
    if (m.spot < n && !used[m.spot]) return;
    for (int s = 0; s < n; ++s) if (!used[s]) { m.spot = (uint8_t)s; return; }
}

// humans fill slot 0 first; one human plays the bot; any change of who is in the tank restarts the match.
// Four players keep their corners (empty ones are bots): the host moves people between them (MOVE PLAYERS)
void Net::compactTank(Game& g, bool reset, bool nextOne) {
    np_ = g.tun.players == 4 ? 4 : 2;
    expRoom_ = g.tun.expRoom;
    for (int p = np_; p < kMaxPlayers; ++p)   // (fewer places: those beyond them go back to waiting)
        if (slot_[p]) { if (Member* m = member(slot_[p])) freeSpot(*m); slot_[p] = 0; }
    int k = 0;   // the people in the first places, in order
    if (np_ == 2)
        for (int p = 0; p < np_; ++p) if (slot_[p]) { const uint32_t id = slot_[p]; slot_[p] = 0; slot_[k++] = id; }
    for (Member& m : members_) if (!inTank(m.id)) freeSpot(m);
    if (reset) {
        if (np_ == 4 && g.tun.teamMode == TEAMS_3V1 && nextOne) {   // (online: the host is THE ONE, or the corner the host
            const int me = localSlot();                                //  picked, or it goes round; a host not playing:
            if (g.tun.oneTurns) oneFixed_ = false;                     //  it goes round)
            if (!oneFixed_) g.tun.theOne = g.tun.oneTurns || me < 0 ? (g.tun.theOne + 1) % 4 : me;
        }
        g.resetWithBots(wantBots());
        rotateAt_ = 0;
        eventLog_.clear();
        resumed_ = false; resumeWait_ = 0;
    }
    hostRoomInfo();
}

// the queue goes into the tank whenever it has a free place (or only the bot): never into a match of two humans
void Net::fillTank(Game& g) {
    bool changed = false;
    np_ = g.tun.players == 4 ? 4 : 2;
    expRoom_ = g.tun.expRoom;
    while (!queue_.empty() && humans() < np_) {
        uint32_t id = queue_.front();
        queue_.erase(queue_.begin());
        if (!member(id)) continue;
        for (int p = 0; p < np_; ++p) if (!slot_[p]) { slot_[p] = id; break; }
        changed = true;
        LOGI("net: queue -> tank");
    }
    if (changed) { compactTank(g, true); notice("NEXT PLAYERS IN THE TANK"); }
}

// match over: both players leave the tank (to the holding area), the queue goes in
void Net::rotate(Game& g) {
    LOGI("net: rotation (%d queued)", (int)queue_.size());
    for (uint32_t& s : slot_) s = 0;
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
    bool wasPlaying = inTank(id);
    for (uint32_t& s : slot_) if (s == id) s = 0;
    if (wasPlaying) { compactTank(g, true); fillTank(g); notice("PLAYER LEFT"); }
    else hostRoomInfo();
}

void Net::hostRoomInfo() {
    for (int p = 0; p < kMaxPlayers; ++p) room_.slot[p] = slot_[p];
    room_.botMask = (np_ == 4 ? humans() > 0 : slot_[0] != 0) ? wantBots() : 0;
    room_.bot = room_.isBot(1);
    room_.members.clear();
    for (const Member& m : members_) {
        if (inTank(m.id)) continue;
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
    if (mode_ == Mode::Host) return roomName_;
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
    for (int s = 0; s < kMaxPlayers; ++s) add(ri->slot[s], VOICE_PLAYER);   // (all four corners: 0.3.12 listed two)
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

bool Net::hostSwap(int a, int b, Game& g) {
    if (mode_ != Mode::Host || np_ != 4 || a < 0 || b < 0 || a >= np_ || b >= np_ || a == b) return false;
    std::swap(slot_[a], slot_[b]);
    if (g.tun.theOne == a) g.tun.theOne = b;   // (THE ONE goes with the player)
    else if (g.tun.theOne == b) g.tun.theOne = a;
    compactTank(g, true, false);
    notice("PLAYERS MOVED");
    LOGI("net: the host swapped corners %d and %d", a, b);
    return true;
}

bool Net::hostMakeOne(int corner, Game& g) {
    if (mode_ != Mode::Host || np_ != 4 || g.tun.teamMode != TEAMS_3V1 || corner < 0 || corner >= np_) return false;
    g.tun.theOne = corner;
    oneFixed_ = true;
    compactTank(g, true, false);
    notice("A NEW ONE");
    LOGI("net: the host made corner %d THE ONE", corner);
    return true;
}

// ------------------------------------------------------------------ host handoff
// host: who hosts next, to everyone in the room (`first` first, then the longest there), with the room's settings and
// the people it has removed (they stay out under the next host)
void Net::sendHeirs(uint32_t first) {
    lastHeirs_ = now_;
    Writer w; header(w, MSG_HEIRS, session_, 0);
    w.put<uint8_t>((botFill_ ? 1 : 0) | (privateTalk_ ? 2 : 0) | (oneTurns_ ? 4 : 0) | (oneFixed_ ? 8 : 0));
    writeName(w, roomName_);
    std::vector<const Member*> order;
    if (const Member* f = member(first)) if (!f->local) order.push_back(f);
    for (const Member& m : members_) if (!m.local && m.id != first) order.push_back(&m);
    const uint8_t n = (uint8_t)std::min<size_t>(order.size(), 12);
    w.put(n);
    for (uint8_t i = 0; i < n; ++i) { w.put(order[i]->id); w.put(order[i]->ip); writeName(w, order[i]->name); }
    const uint8_t k = (uint8_t)std::min<size_t>(removed_.size(), 16);   // (the latest 16)
    w.put(k);
    for (size_t i = removed_.size() - k; i < removed_.size(); ++i) { w.put(removed_[i].id); w.put(removed_[i].ip); writeName(w, removed_[i].name); }
    for (const Member& m : members_) if (!m.local) sendTo(w.b.data(), (int)w.b.size(), m.ip, kPort);
}

// host: the room goes to `target` (someone in it). HEIRS with them first, then HANDOFF, three times over (should
// every copy be lost, the room still finds them once we go quiet: they head the list)
void Net::handOver(uint32_t target, bool stay) {
    sendHeirs(target);
    Writer w; header(w, MSG_HANDOFF, session_, 0); w.put(target); w.put<uint8_t>(stay ? 1 : 0);
    for (int k = 0; k < 3; ++k)
        for (const Member& m : members_) if (!m.local) sendTo(w.b.data(), (int)w.b.size(), m.ip, kPort);
    const Member* t = member(target);
    LOGI("net: the room goes to %s%s", t && t->name[0] ? t->name : "the next player", stay ? " (we stay in it)" : "");
}

// host: no longer hosting (handed over, left, or woken to find the room moved on): the room's state goes, and our own
// ROOM TALK and BOT FILLS IN come back if the room was one we took over
void Net::stepDown() {
    if (inherited_) { privateTalk_ = ownTalk_; botFill_ = ownBotFill_; inherited_ = false; }
    members_.clear(); queue_.clear(); spectators_.clear(); eventLog_.clear(); kickVotes_.clear();
    for (uint32_t& s : slot_) s = 0;
    resumeWait_ = 0; prevHost_ = 0; promotedAt_ = 0;
}

void Net::sendJoin(bool play) {
    lastJoin_ = now_;
    Writer j; header(j, MSG_JOIN, session_, ++seq_); j.put<uint8_t>(play ? 1 : 0); writeName(j, name_);
    sendTo(j.b.data(), (int)j.b.size(), peerIp_, kPort);
}

// client: out of the room (the host closed it, turned us away, or nobody took it over)
void Net::dropRoom(const char* why) {
    if (why) notice(why);
    mode_ = Mode::Offline; peerIp_ = 0; peerSeen_ = 0; peerSession_ = 0; room_ = RoomInfo();
    migrating_ = false; scoutUntil_ = 0; heirs_.clear(); heirRemoved_.clear();
}

bool Net::heirBeacon(uint32_t id, uint32_t ip, double since) const {   // (a room handed over: not one of their own)
    for (const LanGame& lg : games_) if (lg.session == id && lg.ip == ip && lg.seen > since && lg.continues) return true;
    return false;
}

bool Net::hostHandOff(uint32_t target, const Game& g) {
    const Member* t = member(target);
    if (mode_ != Mode::Host || !t || t->local) return false;
    {   // the latest snapshot to everyone first (the people waiting get 30 a second): the new host goes on from here
        Writer w; header(w, MSG_STATE, session_, ++seq_); writeEvents(w, eventLog_); writeState(w, g);
        for (const Member& m : members_) if (!m.local) sendTo(w.b.data(), (int)w.b.size(), m.ip, kPort);
    }
    oneTurns_ = g.tun.oneTurns;
    handOver(target, true);
    std::vector<Heir> next;   // our own list, should the new host not answer
    for (const Member& m : members_) {
        if (m.local) continue;
        Heir h; h.id = m.id; h.ip = m.ip; std::memcpy(h.name, m.name, sizeof h.name);
        if (m.id == target) next.insert(next.begin(), h); else next.push_back(h);
    }
    heirFlags_ = (uint8_t)((botFill_ ? 1 : 0) | (privateTalk_ ? 2 : 0) | (oneTurns_ ? 4 : 0) | (oneFixed_ ? 8 : 0));
    std::memcpy(heirRoom_, roomName_, sizeof heirRoom_);
    heirRemoved_.clear();
    for (const Removed& x : removed_) { Heir h; h.id = x.id; h.ip = x.ip; std::memcpy(h.name, x.name, sizeof h.name); heirRemoved_.push_back(h); }
    stepDown();
    mode_ = Mode::Client;   // (room_ stays as we had it until the new host's comes)
    prevHostId_ = session_;
    heirs_ = next; heirAt_ = 1;
    follow(next.front());
    char msg[48];
    std::snprintf(msg, sizeof msg, "%s IS NOW THE HOST", next.front().name[0] ? next.front().name : "A PLAYER");
    notice(msg);
    return true;
}

// client: the room's host is now h: everything goes to them (JOIN until they list us; the room as we had it till then)
void Net::follow(const Heir& h) {
    peerIp_ = h.ip; peerSession_ = h.id; peerSeen_ = now_;
    lastRecvSeq_ = 0; lastRoomSeq_ = 0; lastEventId_ = 0; pendingEvents_.clear();
    if (!migrating_) migrateFrom_ = now_;
    migrating_ = true; migrateUntil_ = now_ + kHeirWait; scoutUntil_ = 0;
    sendJoin(false);
    LOGI("net: following the room to %s", h.name[0] ? h.name : ipStr(h.ip));
}

void Net::arrived() {   // client: the new host answers: the room is theirs
    migrating_ = false;
    const char* who = nullptr;
    for (const Heir& h : heirs_) if (h.id == peerSession_ && h.name[0]) who = h.name;
    char msg[48];
    std::snprintf(msg, sizeof msg, "%s IS NOW THE HOST", who ? who : "A PLAYER");
    notice(msg);
    LOGI("net: %s hosts the room now", who ? who : ipStr(peerIp_));
}

// client: the next in line takes the room (ourselves: we host it); nobody left: the room is gone
void Net::nextHeir(Game& g) {
    while (heirAt_ < heirs_.size()) {
        const Heir h = heirs_[heirAt_++];
        if (h.id == prevHostId_) continue;
        if (h.id == session_) { promote(g, false, prevHost_, prevHostId_); return; }
        follow(h);
        return;
    }
    LOGW("net: host lost, and nobody took the room over");
    dropRoom("CONNECTION TO HOST LOST");
}

// client -> host of the room we are in: its people (the old host too if it stays), their places in the tank, the
// queue and the holding area as the last ROOM had them, its settings from HEIRS, and the game from the last snapshot
void Net::promote(Game& g, bool oldStays, uint32_t oldIp, uint32_t oldId) {
    const RoomInfo was = room_;
    const std::vector<Heir> heirs = heirs_;
    mode_ = Mode::Host;
    if (!inherited_) { ownTalk_ = privateTalk_; ownBotFill_ = botFill_; }
    inherited_ = true;
    botFill_ = (heirFlags_ & 1) != 0; privateTalk_ = (heirFlags_ & 2) != 0; oneFixed_ = (heirFlags_ & 8) != 0;
    g.tun.oneTurns = oneTurns_ = (heirFlags_ & 4) != 0;
    std::memcpy(roomName_, heirRoom_[0] ? heirRoom_ : name_, sizeof roomName_);
    prevHost_ = oldIp; prevHostId_ = oldId; promotedAt_ = now_;
    members_.clear(); queue_.clear(); spectators_.clear(); kickVotes_.clear(); eventLog_.clear();
    for (const Heir& h : heirRemoved_) {   // (whom the room's hosts removed stay out)
        bool known = false;
        for (const Removed& x : removed_) known |= x.id == h.id;
        if (known) continue;
        Removed x; x.id = h.id; x.ip = h.ip; std::memcpy(x.name, h.name, sizeof x.name);
        removed_.push_back(x);
    }
    const RoomInfo::Member* none = nullptr;
    auto was_ = [&](uint32_t id) { for (const RoomInfo::Member& rm : was.members) if (rm.id == id) return &rm; return none; };
    Member me; me.id = session_; me.local = true; me.seen = now_; me.active = now_;
    if (const RoomInfo::Member* rm = was_(session_)) me.spot = rm->spot;
    members_.push_back(me);
    auto add = [&](uint32_t id, uint32_t ip, const char* nm) {
        if (!id || !ip || id == session_ || member(id)) return;
        Member m; m.id = id; m.ip = ip; m.active = now_; m.back = false;
        if (const RoomInfo::Member* rm = was_(id)) { m.spot = rm->spot; m.in = rm->in; }   // (where they stood, till they're back)
        for (int p = 0; p < kMaxPlayers; ++p) if (was.slot[p] == id && p < g.np) m.in = g.pl[p].in;
        m.seen = now_ + kResumeWait - kMemberTimeout;   // (time to find us: they go when that runs out)
        if (nm) std::memcpy(m.name, nm, 16);
        members_.push_back(m);
    };
    for (const Heir& h : heirs) if (h.id != oldId) add(h.id, h.ip, h.name);
    if (oldStays) add(oldId, oldIp, was.nameOf(oldId));
    bool lost = false;
    for (int p = 0; p < kMaxPlayers; ++p) {
        slot_[p] = was.slot[p] && member(was.slot[p]) ? was.slot[p] : 0;
        lost |= was.slot[p] && !slot_[p];
    }
    std::vector<RoomInfo::Member> q;
    for (const RoomInfo::Member& rm : was.members) if (rm.queuePos >= 0 && member(rm.id) && !inTank(rm.id)) q.push_back(rm);
    std::stable_sort(q.begin(), q.end(), [](const RoomInfo::Member& a, const RoomInfo::Member& b) { return a.queuePos < b.queuePos; });
    for (const RoomInfo::Member& rm : q) queue_.push_back(rm.id);
    np_ = g.np == 4 ? 4 : 2; expRoom_ = g.tun.expRoom;
    heirs_.clear(); heirRemoved_.clear(); migrating_ = false; scoutUntil_ = 0;
    peerIp_ = 0; peerSession_ = 0; peerSeen_ = now_;
    lastBeacon_ = 0; lastRoomSent_ = 0; lastHeirs_ = 0; rotateAt_ = 0; lastClosingWarn_ = 0;
    if (lost) {   // a player in the match is gone: it starts again, as when anyone leaves
        compactTank(g, true);
        resumeWait_ = 0;
        notice("YOU ARE NOW THE HOST - A NEW MATCH");
    } else {
        g.resumeAfterHandoff();
        resumed_ = true;
        resumeWait_ = humans() ? now_ + kResumeWait : 0;
        hostRoomInfo();
        notice("YOU ARE NOW THE HOST");
    }
    LOGI("net: we host the room now (%d people, %s)", (int)members_.size(), lost ? "a new match" : "the match goes on");
}

bool Net::hostKick(uint32_t target) {
    if (mode_ != Mode::Host || !target || target == session_ || !member(target)) return false;
    kickNow_ = target;
    kickByHost_ = true;
    return true;
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
        const uint8_t role = inTank(session_) ? VOICE_PLAYER : VOICE_WAITING;
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
    for (const Spectator& sp : spectators_)   // only spectators standing at this arena hear it (and not when the room's talk is private)
        if (!privateTalk_ && sp.where == 1 && !(sp.ip == exceptIp && sp.id == exceptId)) sendTo(w.b.data(), (int)w.b.size(), sp.ip, kPort);
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
bool Net::exchange(const PlayerInput& local, PlayerInput* in, Game& g, float dt) {
    if (kickNow_ && mode_ == Mode::Host) {   // VOTE TO REMOVE passed: out of the room, and not back this session
        if (Member* m = member(kickNow_)) {
            Removed x; x.id = m->id; x.ip = m->ip; std::memcpy(x.name, m->name, 16);
            removed_.push_back(x);
            char msg[48];
            std::snprintf(msg, sizeof msg, "%s WAS REMOVED%s", m->name[0] ? m->name : "A PLAYER", kickByHost_ ? "" : " BY VOTE");
            LOGI("net: %s removed by %s", m->name[0] ? m->name : ipStr(m->ip), kickByHost_ ? "the host" : "vote");
            reject(m->ip, kickByHost_ ? "REMOVED BY THE HOST" : "REMOVED BY VOTE");
            removeMember(kickNow_, g);
            notice(msg);
        }
        kickNow_ = 0;
        kickByHost_ = false;
    }
    now_ = monoNow();
    const double gap = lastExchange_ > 0 ? now_ - lastExchange_ : 0;   // (a headset asleep: no frames)
    lastExchange_ = now_;
    if (online_ && vip_ && gap > 8.0) {   // asleep that long: the relay has let our session go (10 s). A new one
        LOGW("net: no frames for %.0f s: a new session on the relay", gap);
        vip_ = 0;
        relayHello();
    }
    if (gap > kHostQuiet && mode_ == Mode::Host) {   // we slept: our room has another host by now. Rejoin it as a player
        std::vector<Heir> next;
        for (const Member& m : members_) {
            if (m.local) continue;
            Heir h; h.id = m.id; h.ip = m.ip; std::memcpy(h.name, m.name, sizeof h.name);
            next.push_back(h);
        }
        if (!next.empty()) {
            // (which of them hosts it now, the beacons say: listen a few seconds rather than knock on every door; nobody
            // hosting one: it has closed)
            LOGW("net: no frames for %.0f s while hosting: the room went on without us; looking for it", gap);
            stepDown();
            mode_ = Mode::Client; peerIp_ = 0; peerSession_ = 0; prevHostId_ = session_;
            room_ = RoomInfo();   // (we come back to the waiting area)
            heirs_ = next; heirAt_ = 0;
            migrating_ = true; migrateFrom_ = now_; scoutUntil_ = now_ + 4.0;
            notice("YOU WERE AWAY - REJOINING THE ROOM");
        }
    } else if (gap > 3.0 && mode_ == Mode::Client && !migrating_) {   // we slept: the host may have let us go. Ask again
        LOGW("net: no frames for %.0f s: asking the host to let us back in", gap);
        peerSeen_ = now_; lastJoin_ = 0;
        room_ = RoomInfo();
    }
    bool gotState = false;
    uint8_t buf[2048];
    for (;;) {
        sockaddr_in from{}; socklen_t fl = sizeof from;
        int n = sock_ >= 0 ? (int)recvfrom(sock_, (char*)buf, sizeof buf, 0, (sockaddr*)&from, &fl) : -1;
#ifdef _WIN32
        if (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK) continue;   // a stray ICMP error: keep reading
#endif
        if (n <= 0) break;
        if (now_ < deafUntil_) continue;   // (deafen: tests)
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
                    if (rr.e - rr.p >= 4 + 2 * 51 + 8) std::memcpy(&c.returnHits, rr.p + 4 + 2 * 51 + 4 * k, 4);   // 0.3.9 relays
                }
                continue;
            }
            if (t == R_BOARD) {   // a TRAINING leaderboard
                if (rr.e - rr.p < 3) continue;
                Board bd;
                bd.lesson = rr.p[0]; bd.kind = rr.p[1];
                const int n = std::min<int>(rr.p[2], 20);
                if (rr.e - rr.p < 3 + 20 * n + 8) continue;
                for (int i = 0; i < n; ++i) {
                    const uint8_t* q = rr.p + 3 + 20 * i;
                    BoardRow row{};
                    for (int k = 0; k < 16; ++k) row.name[k] = (q[k] >= 32 && q[k] < 127) ? (char)q[k] : 0;
                    std::memcpy(&row.value, q + 16, 4);
                    bd.rows.push_back(row);
                }
                const uint8_t* q = rr.p + 3 + 20 * n;
                uint16_t place, count;
                std::memcpy(&place, q, 2); std::memcpy(&bd.myValue, q + 2, 4); std::memcpy(&count, q + 6, 2);
                bd.myPlace = place; bd.players = count;
                bd.at = now_;
                boards_.erase(std::remove_if(boards_.begin(), boards_.end(),
                                             [&](const Board& x) { return x.lesson == bd.lesson && x.kind == bd.kind; }),
                              boards_.end());
                boards_.push_back(bd);
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
        if (mode_ == Mode::Client && (type == MSG_ROOM || type == MSG_STATE) && session != peerSession_ && now_ - peerSeen_ > 3.0) {
            // our host has gone quiet and the next in line already hosts the room: follow them now
            for (size_t i = 0; i < heirs_.size(); ++i) {
                if (heirs_[i].id != session || heirs_[i].ip != fromIp) continue;
                if (!migrating_) { prevHost_ = peerIp_; prevHostId_ = peerSession_; }
                heirAt_ = i + 1;
                follow(heirs_[i]);
                break;
            }
        }
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
            if (r.ok && r.e - r.p >= 10) {   // MATCH OPTIONS (0.3.8 hosts)
                lg.shields = r.get<uint8_t>() != 0;
                lg.knuckleMode = std::max(-1, std::min(1, (int)r.get<int8_t>()));
                lg.matchTime = r.get<float>(); lg.roundTime = r.get<float>();
                if (!std::isfinite(lg.matchTime) || lg.matchTime < 0) lg.matchTime = 180.f;
                if (!std::isfinite(lg.roundTime) || lg.roundTime < 0) lg.roundTime = 0.f;
            }
            if (r.ok && r.e - r.p >= 12) {   // 0.3.12: the EXPERIMENTAL room, its players and teams, the teams' scores
                lg.experimental = r.get<uint8_t>() != 0;
                lg.expRoom = std::min<int>(r.get<uint8_t>(), kRooms - 1);
                lg.players = r.get<uint8_t>() == 4 ? 4 : 2;
                lg.teamMode = std::min(2, (int)r.get<uint8_t>());
                for (int& t : lg.teamScores) t = r.get<int16_t>();
            }
            if (r.ok && r.e - r.p >= 4) lg.continues = r.get<uint32_t>();   // 0.3.13: handed over from that host
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
                LOGI("net: host closed the arena");
                dropRoom("HOST CLOSED THE ARENA");
            }
        } else if (type == MSG_REJECT && fromHost) {
            char why[33] = {};
            for (int i = 0; i < 32; ++i) why[i] = (char)r.get<uint8_t>();
            for (char& c : why) if (c && (c < 32 || c > 'Z')) c = ' ';
            LOGI("net: host refused: %s", why);
            dropRoom(why[0] ? why : "ARENA FULL");
        } else if (type == MSG_JOIN && mode_ == Mode::Host) {
            if (Member* m = member(session)) { if (m->ip == fromIp) { m->seen = now_; m->back = true; } continue; }
            if ((int)members_.size() >= np_ + kMaxWaiting) { reject(fromIp, "ARENA FULL"); continue; }
            Member m;
            m.ip = fromIp; m.id = session; m.seen = now_; m.active = now_;
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
            fromMember->back = true;
            peerSeen_ = now_;
            if (seq > fromMember->lastSeq) {
                fromMember->lastSeq = seq;
                readInput(r, fromMember->in);
                if (inTank(fromMember->id)) countSeq(seq);
            }
        } else if (type == MSG_KICKVOTE && fromMember) {
            const uint32_t target = r.get<uint32_t>();
            if (r.ok) countKickVote(target, session);
        } else if (type == MSG_QUEUE && fromMember) {
            const uint8_t v = r.get<uint8_t>();
            if (v == 2) { forfeit_ = session; continue; }
            bool want = v != 0;
            auto it = std::find(queue_.begin(), queue_.end(), session);
            if (want && it == queue_.end() && !inTank(session) && (int)queue_.size() < np_) queue_.push_back(session);   // full: stays waiting
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
                    forwardVoice(session, inTank(session) ? VOICE_PLAYER : VOICE_WAITING, pcm, fromIp, session);
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
        } else if (type == MSG_HEIRS && fromHost && !migrating_) {   // who hosts next, should our host go
            const uint8_t f = r.get<uint8_t>();
            char room[17] = {};
            readName(r, room);
            const uint8_t n = r.get<uint8_t>();
            std::vector<Heir> v;
            for (uint8_t i = 0; i < n && i < 12 && r.ok; ++i) {
                Heir h; h.id = r.get<uint32_t>(); h.ip = r.get<uint32_t>(); readName(r, h.name);
                if (r.ok && h.id && h.ip) v.push_back(h);
            }
            std::vector<Heir> out;   // (the people the host removed: in HEIRS since 0.3.13's review)
            const uint8_t k = r.p < r.e ? r.get<uint8_t>() : 0;
            for (uint8_t i = 0; i < k && i < 16 && r.ok; ++i) {
                Heir h; h.id = r.get<uint32_t>(); h.ip = r.get<uint32_t>(); readName(r, h.name);
                if (r.ok) out.push_back(h);
            }
            if (r.ok) { heirs_ = v; heirRemoved_ = out; heirFlags_ = f; std::memcpy(heirRoom_, room, sizeof heirRoom_); }
        } else if (type == MSG_HANDOFF && fromHost) {   // the host hands the room to someone (maybe us)
            const uint32_t target = r.get<uint32_t>();
            const bool stays = r.get<uint8_t>() != 0;
            if (!r.ok || target == peerSession_) continue;
            prevHost_ = peerIp_; prevHostId_ = peerSession_;
            if (target == session_) { promote(g, stays, peerIp_, peerSession_); continue; }
            for (size_t i = 0; i < heirs_.size(); ++i)
                if (heirs_[i].id == target) { heirAt_ = i + 1; follow(heirs_[i]); break; }
        } else if (type == MSG_CLOSING && fromHost) {   // IDLE ROOMS: the room is about to close (0: it has)
            const uint16_t secs = r.get<uint16_t>();
            if (!r.ok) continue;
            if (secs == 0) {
                LOGI("net: the room closed (nobody moved)");
                dropRoom("ROOM CLOSED - NOBODY WAS PLAYING");
            } else {
                char msg[48];
                std::snprintf(msg, sizeof msg, "ROOM CLOSING IN %d SECONDS - MOVE TO STAY", (int)secs);
                notice(msg);
            }
        } else if (type == MSG_ROOM && fromHost) {
            peerSession_ = session;
            peerSeen_ = now_;
            if (migrating_) arrived();
            readRoom(r, room_, lastRoomSeq_);
        } else if (type == MSG_STATE && fromHost) {
            peerSession_ = session;   // the host's session, learned from its beacon or first snapshot
            peerSeen_ = now_;
            if (migrating_) arrived();
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
        for (int k = 0; k < 4 && vip_ && keySent_ && !trainQ_.empty(); ++k) {   // (TRAINING results, a few at a time)
            relaySend(R_TRAIN, trainQ_.front().data(), (int)trainQ_.front().size());
            LOGI("stats: training result sent (lesson %d)", trainQ_.front()[0]);
            trainQ_.erase(trainQ_.begin());
        }
        if (vip_ && mode_ == Mode::Host && now_ - lastTank_ > 3.0) {   // who is in the tank (stats go to them)
            lastTank_ = now_;
            uint32_t v[2] = {0, 0};
            for (int k = 0; k < 2 && np_ == 2; ++k) {   // (four players: no careers, they compare one-on-one matches)
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
    case Mode::Offline:   // practice: the bot in slot 1 (FOUR CORNERS: bots in the other three corners)
        g.tun.readyUp = false;   // (practice starts when you are there)
        if (g.tun.players == 4 ? g.np != 4 || !(g.pl[1].isBot && g.pl[2].isBot && g.pl[3].isBot) : g.np != 2 || !g.pl[1].isBot)
            g.resetWithBots(g.tun.players == 4 ? 0xEu : 2u);
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
        oneTurns_ = g.tun.oneTurns;
        if (promotedAt_ > 0) {   // a room we took over: did the room come with us?
            bool anyBack = false;
            for (const Member& m : members_) anyBack |= !m.local && m.back;
            if (anyBack) promotedAt_ = 0;   // (yes: it's ours now)
            else
                for (const LanGame& lg : games_) {
                    // nobody followed, and the old host still announces its room: it never went quiet, only our own
                    // connection did. Back to it (a copy of the room under the same name helps nobody)
                    if (lg.ip != prevHost_ || lg.session != prevHostId_ || lg.seen < promotedAt_ + 0.5) continue;
                    LOGW("net: the old host is still there: we go back to its room");
                    Heir h; h.id = prevHostId_; h.ip = prevHost_; std::memcpy(h.name, lg.name, sizeof h.name);
                    stepDown();
                    mode_ = Mode::Client; room_ = RoomInfo(); heirs_.clear();
                    follow(h);
                    notice("BACK TO THE ROOM - ITS HOST WAS STILL THERE");
                    return false;
                }
        }
        // IDLE ROOMS: who has moved lately; a player gone still leaves the tank; a room gone still closes
        for (Member& m : members_) if (stirred(m.ref, m.refButtons, m.local ? local : m.in)) m.active = now_;
        for (int p = 0; p < np_; ++p) {
            Member* m = member(slot_[p]);
            if (!m || now_ - m->active < awayAfter_) continue;
            char msg[48];
            if (m->local) std::snprintf(msg, sizeof msg, "YOU WERE AWAY - MOVED TO THE WAITING AREA");
            else std::snprintf(msg, sizeof msg, "%s WAS AWAY - MOVED OUT", m->name[0] ? m->name : "A PLAYER");
            LOGI("net: %s stood still for %.0f s: out of the tank", m->local ? "the host" : m->name, now_ - m->active);
            for (uint32_t& s : slot_) if (s == m->id) s = 0;
            compactTank(g, true);
            notice(msg);
            break;
        }
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](uint32_t id) { const Member* m = member(id); return !m || now_ - m->active >= awayAfter_; }),
                     queue_.end());
        {
            double last = 0;
            for (const Member& m : members_) last = std::max(last, m.active);
            const double idle = now_ - last;
            if (idle >= idleClose_) {
                Writer w; header(w, MSG_CLOSING, session_, 0); w.put<uint16_t>(0);
                for (const Member& m : members_) if (!m.local) sendTo(w.b.data(), (int)w.b.size(), m.ip, kPort);
                LOGI("net: nobody moved for %.0f s: the room closes", idle);
                closeRoom();
                members_.clear();   // (so leave() closes it rather than handing it over)
                leave();
                notice("ROOM CLOSED - NOBODY WAS PLAYING");
                return false;
            }
            if (idle >= idleClose_ - 60.0 && now_ - lastClosingWarn_ >= 10.0) {
                lastClosingWarn_ = now_;
                const int secs = std::max(1, (int)std::ceil(idleClose_ - idle));
                Writer w; header(w, MSG_CLOSING, session_, 0); w.put<uint16_t>((uint16_t)secs);
                for (const Member& m : members_) if (!m.local) sendTo(w.b.data(), (int)w.b.size(), m.ip, kPort);
                char msg[48];
                std::snprintf(msg, sizeof msg, "ROOM CLOSING IN %d SECONDS - MOVE TO STAY", secs);
                notice(msg);
            } else if (idle < idleClose_ - 60.0) {
                lastClosingWarn_ = 0;
            }
        }
        if (now_ - lastHeirs_ > 1.0) sendHeirs();
        if (forfeit_) {   // a player gave up: out to the waiting area, the bot or the queue takes the place
            const uint32_t id = forfeit_;
            forfeit_ = 0;
            if (inTank(id)) {
                for (uint32_t& s : slot_) if (s == id) s = 0;
                compactTank(g, true);
                notice("PLAYER FORFEITED");
                LOGI("net: forfeit -> waiting area");
            }
        }
        if ((g.tun.players == 4 ? 4 : 2) != g.np || np_ != g.np) compactTank(g, true);   // the rules changed the places
        fillTank(g);
        // rotation: a finished match of two humans or with someone queued empties the tank after a short pause; a room
        // for four, after every match (owner, 2026-11-03: everyone back to the waiting area, JOIN QUEUE to play again)
        const bool rotateRule = humans() == np_ || (humans() >= 1 && (!queue_.empty() || np_ == 4));
        g.tun.readyUp = np_ == 4;   // READY UP: a room for four starts when every person in the tank pressed A
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
            for (int i = 0; i < 16; ++i) w.put<uint8_t>((uint8_t)roomName_[i]);
            w.put<uint8_t>(g.tun.advanced); w.put<uint8_t>((int)members_.size() >= np_ + kMaxWaiting);
            w.put<int16_t>((int16_t)g.teamScore(0)); w.put<int16_t>((int16_t)g.teamScore(1)); w.put<float>(g.roundClock);
            w.put<uint8_t>((uint8_t)std::min<size_t>(255, spectators_.size()));
            w.put<uint8_t>((uint8_t)waiting); w.put<uint8_t>((uint8_t)queue_.size()); w.put<uint8_t>((uint8_t)humans());
            w.put<uint8_t>(g.tun.shields); w.put<int8_t>((int8_t)g.tun.knuckleMode);   // MATCH OPTIONS (0.3.8)
            w.put<float>(g.tun.basicMatchTime); w.put<float>(g.tun.advancedRoundTime);
            w.put<uint8_t>(g.tun.experimental); w.put<uint8_t>((uint8_t)g.tun.expRoom);   // 0.3.12
            w.put<uint8_t>((uint8_t)g.np); w.put<uint8_t>((uint8_t)g.tun.teamMode);
            for (int t = 0; t < 4; ++t) w.put<int16_t>((int16_t)(t < g.teamCount() ? g.teamScore(t) : 0));
            w.put<uint32_t>(prevHost_);   // 0.3.13: a room handed over says from whom (its spectators follow it)
            sendTo(w.b.data(), (int)w.b.size(), INADDR_BROADCAST, kPort);
        }
        if (now_ - lastRoomSent_ > 1.0 / 15.0) { lastRoomSent_ = now_; sendRoom(); }
        // the game is stepped with the tank players' inputs; an empty place = a bot (Game fills its input)
        for (int p = 0; p < kMaxPlayers; ++p) {
            in[p] = PlayerInput();
            if (!slot_[p] || p >= np_) continue;
            if (Member* m = member(slot_[p])) in[p] = m->local ? local : m->in;
        }
        if (rotateRule && g.match == MatchState::MatchSummary)   // no skipping the rotation with A (A is READY before a match)
            for (int p = 0; p < kMaxPlayers; ++p) in[p].buttonA = false;
        if (!humans()) return false;   // empty tank: the arena idles until someone queues
        if (resumeWait_ > 0) {   // a match taken over: paused until its players have found us (or the wait is up)
            bool all = true;
            for (int p = 0; p < np_; ++p) if (const Member* m = member(slot_[p])) all &= m->back;
            if (!all && now_ < resumeWait_) return false;
            resumeWait_ = 0;
            notice("THE MATCH GOES ON");
            LOGI("net: the match goes on (%s)", all ? "everyone is back" : "waited long enough");
        }
        const uint8_t bots = wantBots();
        uint8_t now = 0;
        for (int p = 0; p < g.np; ++p) now |= g.pl[p].isBot ? (uint8_t)(1u << p) : 0;
        if (now != bots) g.resetWithBots(bots);
        return true;   // caller steps; snapshot is sent via sendState
    }
    case Mode::Client: {
        // the snapshots do not say who is a bot: take it from the room (only slot 1 can be the host's bot). A flag
        // left over from practice or the lobby made the host flicker between its own look and the bot's, unsmoothed.
        for (int p = 0; p < kMaxPlayers; ++p) g.pl[p].isBot = room_.isBot(p);
        if (localSlot() < 0)
            for (const LanGame& lg : games_) if (lg.ip == peerIp_ && now_ - lg.seen < 3.0) g.tun.advanced = lg.advanced;
        if (peerIp_) {   // (none while we look for where our room went)
            Writer w; header(w, MSG_INPUT, session_, ++seq_); writeInput(w, local);
            sendTo(w.b.data(), (int)w.b.size(), peerIp_, kPort);
        }
        const bool known = localSlot() >= 0 ||
                           std::any_of(room_.members.begin(), room_.members.end(), [&](const RoomInfo::Member& m) { return m.id == session_; });
        if (!known && !migrating_ && peerIp_ && now_ - lastJoin_ > 1.0) sendJoin(joinPlay_);   // the JOIN may have been lost: ask again until the room lists us
        const int s = localSlot();
        if (gotState) {
            Reader rs{lastState_.data(), lastState_.data() + lastState_.size()};
            readState(rs, g, s);
        }
        g.events.swap(pendingEvents_);
        pendingEvents_.clear();
        if (s >= 0) g.pl[s].in = local;
        (void)dt; (void)in;
        if (migrating_) {   // HOST HANDOFF: knocking on the new host's door until it answers (or the next one's)
            // one of the people in line already hosts the room (its beacon says so): straight there
            for (size_t i = 0; i < heirs_.size(); ++i)
                if (heirs_[i].id != peerSession_ && heirBeacon(heirs_[i].id, heirs_[i].ip, migrateFrom_)) { heirAt_ = i + 1; follow(heirs_[i]); break; }
            if (scoutUntil_ > 0) {   // (woken up: nobody hosts it any more)
                if (now_ > scoutUntil_) { LOGI("net: our room is gone"); dropRoom("THE ROOM HAS CLOSED"); }
            } else {
                if (now_ - lastJoin_ > 0.5) sendJoin(false);
                if (now_ > migrateUntil_) nextHeir(g);
            }
        } else if (!peerConnected() && now_ - peerSeen_ > kHostQuiet) {
            if (!heirs_.empty()) {   // the host is gone: the room goes on with the next in line
                LOGW("net: host lost: handing the room over");
                notice("THE HOST LEFT - HANDING THE ROOM OVER");
                prevHost_ = peerIp_; prevHostId_ = peerSession_; heirAt_ = 0;
                nextHeir(g);
            } else {
                LOGW("net: host lost");
                dropRoom("CONNECTION TO HOST LOST");
            }
        } else if (now_ - peerSeen_ > 1.5 && !heirs_.empty() && !noticeText()) {
            notice("HOST NOT RESPONDING - HOLD ON");
        }
        return false;
    }
    }
    return true;
}

size_t Net::snapshotBytes(const Game& g) {
    std::deque<SentEvent> log;
    for (int i = 0; i < 24; ++i) log.push_back({(uint32_t)i + 1, 0.0, GameEvent{EventType::Bounce, 0, -1, {}, 0.f}});
    Writer w; header(w, MSG_STATE, 1, 1); writeEvents(w, log); writeState(w, g);
    return w.b.size();
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
        const bool player = inTank(m.id);
        if (player || slow) sendTo(w.b.data(), (int)w.b.size(), m.ip, kPort);
    }
    if (slow) for (const Spectator& sp : spectators_) sendTo(w.b.data(), (int)w.b.size(), sp.ip, kPort);
}
