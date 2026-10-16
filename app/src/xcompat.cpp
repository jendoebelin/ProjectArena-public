// EXPERIMENTAL SOFTWARE - cross-compatibility connection to Sparc (UE 4.15) servers. See xcompat.h.
#include "xcompat.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "log.h"

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
static void xcClose(int s) { closesocket((SOCKET)s); }
static void xcNonBlocking(int s) { u_long one = 1; ioctlsocket((SOCKET)s, FIONBIO, &one); }
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
static void xcClose(int s) { close(s); }
static void xcNonBlocking(int s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
#endif

namespace xc {

void socketsInit() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

// ---------------------------------------------------------------- protocol constants (UE 4.15)
static const uint32_t MAX_PACKETID = 16384, MAX_CHANNELS = 10240, MAX_CHSEQUENCE = 1024, CHTYPE_MAX = 8;
static const uint32_t MAX_BUNCH_BITS = 1024 * 8;   // UIpNetDriver MaxPacket 1024 bytes (checked: every bunch parses)
static const uint32_t CHTYPE_CONTROL = 1;
enum : uint8_t { NMT_Hello = 0, NMT_Welcome = 1, NMT_Upgrade = 2, NMT_Challenge = 3, NMT_Netspeed = 4, NMT_Login = 5,
                 NMT_Failure = 6, NMT_Join = 9 };
// ASSUMPTION: the server checks these against its own build; they are the values the stock Sparc client sends.
static const uint32_t kNetworkVersion = 2092372988u;
static const char* kLoginMap = "/Game/Maps/l_startup";
static const char* kBuildOptions = "?GitBranch=master?GitSHA1=a1cd45eca61d0f0e82364059103b683b2032524e?GitIsDirty=true";
// ASSUMPTION: the hardware string a PC client with a Rift / Quest over SteamVR sends; a Quest on its own is not a
// Sparc hardware type, so we present as that one.
static const char* kHardCaps = "?HardCaps=10010DT_SteamVrRift";
static const uint32_t kNetspeed = 10000000;

static int32_t bestSignedDifference(int32_t v, int32_t ref, int32_t max) { return ((v - ref + max / 2) & (max - 1)) - max / 2; }
static int32_t makeRelative(int32_t v, int32_t ref, int32_t max) { return ref + bestSignedDifference(v, ref, max); }

// ---------------------------------------------------------------- bit streams
void BitWriter::bit(bool v) {
    if ((num >> 3) >= buf.size()) buf.push_back(0);
    if (v) buf[num >> 3] |= (uint8_t)(1u << (num & 7));
    ++num;
}
void BitWriter::bits(uint32_t v, int n) { for (int i = 0; i < n; ++i) bit((v >> i) & 1u); }
void BitWriter::writeInt(uint32_t v, uint32_t max) {
    uint32_t nv = 0;
    for (uint32_t mask = 1; nv + mask < max && mask; mask <<= 1) {
        bool b = (v & mask) != 0;
        bit(b);
        if (b) nv += mask;
    }
}
void BitWriter::intPacked(uint32_t v) {
    do {
        uint8_t b = (uint8_t)((v & 0x7f) << 1);
        v >>= 7;
        if (v) b |= 1;
        u8(b);
    } while (v);
}
void BitWriter::fstring(const std::string& s) {
    if (s.empty()) { i32(0); return; }
    i32((int32_t)s.size() + 1);
    for (char c : s) u8((uint8_t)c);
    u8(0);
}
void BitWriter::appendBits(const uint8_t* data, uint32_t nbits, uint32_t from) {
    for (uint32_t i = from; i < from + nbits; ++i) bit((data[i >> 3] >> (i & 7)) & 1);
}
void BitWriter::append(const BitWriter& o) { appendBits(o.buf.data(), o.num); }

bool BitReader::bit() {
    if (pos >= n) { error = true; return false; }
    bool v = (d[pos >> 3] >> (pos & 7)) & 1;
    ++pos;
    return v;
}
uint32_t BitReader::bits(int k) {
    uint32_t v = 0;
    for (int i = 0; i < k; ++i) if (bit()) v |= 1u << i;
    return v;
}
uint32_t BitReader::readInt(uint32_t max) {
    uint32_t v = 0;
    for (uint32_t mask = 1; v + mask < max && mask; mask <<= 1)
        if (bit()) v |= mask;
    return v;
}
uint32_t BitReader::intPacked() {
    uint32_t v = 0;
    for (int shift = 0; shift < 35 && !error; shift += 7) {
        uint8_t b = u8();
        v |= (uint32_t)(b >> 1) << shift;
        if (!(b & 1)) break;
    }
    return v;
}
float BitReader::f32() {
    uint32_t u = u32();
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
std::string BitReader::fstring() {
    int32_t len = i32();
    std::string s;
    if (len == 0 || error) return s;
    if (len < 0) {   // UTF-16: keep the low bytes (names here are ASCII)
        if (-len > 4096) { error = true; return s; }
        for (int i = 0; i < -len; ++i) { uint32_t c = bits(16); if (c) s += (char)(c < 128 ? c : '?'); }
        return s;
    }
    if (len > 4096) { error = true; return s; }
    for (int i = 0; i < len; ++i) { char c = (char)u8(); if (c) s += c; }
    return s;
}

// ---------------------------------------------------------------- link
const char* SparcLink::stateText() const {
    switch (state_) {
    case State::Off: return "OFF";
    case State::Handshake: return "HANDSHAKE";
    case State::Hello: return "HELLO";
    case State::Login: return "LOGGING IN";
    case State::Loading: return "JOINING";
    case State::Joined: return "CONNECTED";
    case State::Failed: return "FAILED";
    }
    return "?";
}

std::string SparcLink::loginUrl(const std::string& playerName) {
    return std::string(kLoginMap) + "?Name=" + playerName + kBuildOptions + kHardCaps;
}

std::vector<std::string> SparcLink::playerNames() const {
    std::vector<std::string> v;
    for (auto& kv : actors_)
        if (kv.second.path.find("PlayerState") != std::string::npos && !kv.second.name.empty()) v.push_back(kv.second.name);
    return v;
}

int SparcLink::countOf(const char* part) const {
    int n = 0;
    for (auto& kv : actors_) if (kv.second.path.find(part) != std::string::npos) ++n;
    return n;
}

void SparcLink::fail(const char* why) {
    LOGW("xcompat: %s", why);
    error_ = why;
    disconnect(nullptr);
    state_ = State::Failed;
}

bool SparcLink::connect(const char* hostPort, const std::string& playerName, const std::string& uniqueId) {
    disconnect(nullptr);
    error_.clear();
    std::string host = hostPort ? hostPort : "";
    port_ = 7777;
    size_t c = host.rfind(':');
    if (c != std::string::npos) { port_ = (uint16_t)std::atoi(host.c_str() + c + 1); host = host.substr(0, c); }
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (host.empty() || getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) { fail("SERVER ADDRESS NOT FOUND"); return false; }
    addr_ = ((sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    sock_ = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ < 0) { fail("NO SOCKET"); return false; }
    xcNonBlocking(sock_);
    name_ = playerName;
    uniqueId_ = uniqueId;
    state_ = State::Handshake;
    stateAt_ = -1;   // the clocks start at the first update()
    sendHandshakeHello();
    LOGI("xcompat: connecting to %s:%u as %s", host.c_str(), port_, name_.c_str());
    return true;
}

void SparcLink::testBegin(const std::string& playerName, const std::string& uniqueId) {
    disconnect(nullptr);
    testMode_ = true;
    name_ = playerName;
    uniqueId_ = uniqueId;
    state_ = State::Hello;
    BitWriter hello;
    hello.u8(NMT_Hello);
    hello.u8(1);   // little endian
    hello.u32(kNetworkVersion);
    queueControl(hello, true);
    flush(now_, true);
}

void SparcLink::disconnect(const char* why) {
    if (sock_ >= 0) {
        // ASSUMPTION: closing the control channel is how a UE client says goodbye; the server also times us out
        if (state_ == State::Joined || state_ == State::Loading) {
            BitWriter b;
            b.bit(0); b.bit(1); b.bit(0); b.bit(1); b.bit(0);   // control: open 0, close 1, dormant 0
            b.bit(0); b.bit(1);                                  // paused 0, reliable 1
            b.writeInt(0, MAX_CHANNELS); b.bit(0); b.bit(0); b.bit(0);
            uint32_t seq = ++outSeq_[0];
            b.writeInt(seq % MAX_CHSEQUENCE, MAX_CHSEQUENCE);
            b.writeInt(CHTYPE_CONTROL, CHTYPE_MAX);
            b.writeInt(0, MAX_BUNCH_BITS);
            outBunches_.push_back(b);
            flush(now_, true);
        }
        xcClose(sock_);
    }
    sock_ = -1;
    if (why) { error_ = why; LOGI("xcompat: disconnected (%s)", why); }
    state_ = State::Off;
    outPacketId_ = 0;
    pendingAcks_.clear();
    outSeq_.clear();
    unacked_.clear();
    outBunches_.clear();
    inCh_.clear();
    partial_.clear();
    guids_.clear();
    actors_.clear();
    controlOpen_ = joinedSent_ = false;
    handshakeReply_.clear();
    map_.clear();
    game_.clear();
    packetsIn_ = packetsOut_ = 0;
    decodeErrors_ = 0;
    testMode_ = false;
}

void SparcLink::sendRaw(const uint8_t* data, size_t len) {
    ++packetsOut_;
    lastSend_ = now_;
    if (testMode_) { testSent.emplace_back(data, data + len); return; }
    if (sock_ < 0) return;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port_);
    to.sin_addr.s_addr = addr_;
    sendto(sock_, (const char*)data, (int)len, 0, (sockaddr*)&to, sizeof to);
}

void SparcLink::sendHandshakeHello() {
    // StatelessConnectHandlerComponent initial packet: handshake 1, secret 0, timestamp 0, cookie 0, terminator
    uint8_t p[25] = {};
    p[0] = 0x01;
    p[24] = 0x04;
    sendRaw(p, sizeof p);
}

void SparcLink::queueReliable(uint32_t ch, uint32_t chType, const BitWriter& payload, bool open) {
    BitWriter b;
    b.bit(0);              // not an ack
    b.bit(open);           // bControl (open or close follows)
    if (open) { b.bit(1); b.bit(0); }
    b.bit(0);              // bIsReplicationPaused
    b.bit(1);              // bReliable
    b.writeInt(ch, MAX_CHANNELS);
    b.bit(0); b.bit(0); b.bit(0);   // exports, must-be-mapped, partial
    uint32_t seq = ++outSeq_[ch];
    b.writeInt(seq % MAX_CHSEQUENCE, MAX_CHSEQUENCE);
    b.writeInt(chType, CHTYPE_MAX);
    b.writeInt(payload.num, MAX_BUNCH_BITS);
    b.append(payload);
    outBunches_.push_back(b);
    unacked_.push_back({ch, b, 0xffffffffu, 0});
}

void SparcLink::queueControl(const BitWriter& msg, bool open) {
    if (open) controlOpen_ = true;
    queueReliable(0, CHTYPE_CONTROL, msg, open);
}

void SparcLink::flush(double now, bool force) {
    if (!testMode_ && sock_ < 0) return;
    // resend reliable bunches whose packet was not acked in time
    for (OutReliable& r : unacked_)
        if (r.packetId != 0xffffffffu && now - r.sentAt > 0.40) { outBunches_.push_back(r.bunch); r.packetId = 0xfffffffeu; }
    const bool keepalive = now - lastSend_ > 0.20;
    if (!force && outBunches_.empty() && pendingAcks_.empty() && !keepalive) return;
    while (true) {
        BitWriter p;
        p.bit(0);   // PacketHandler: not a handshake packet
        const uint32_t pid = outPacketId_;
        p.writeInt(pid % MAX_PACKETID, MAX_PACKETID);
        size_t a = 0;
        for (; a < pendingAcks_.size() && p.num < 6000; ++a) {
            p.bit(1);
            p.writeInt(pendingAcks_[a] % MAX_PACKETID, MAX_PACKETID);
            p.bit(0);           // bHasServerFrameTime
            p.intPacked(0);     // RemoteInKBytesPerSecond (the stock client sends 0 early on)
        }
        pendingAcks_.erase(pendingAcks_.begin(), pendingAcks_.begin() + a);
        size_t used = 0;
        for (; used < outBunches_.size(); ++used) {
            if (p.num + outBunches_[used].num > 7600 && used > 0) break;
            p.append(outBunches_[used]);
            for (OutReliable& r : unacked_)   // stamp reliable bunches with this packet id
                if ((r.packetId == 0xffffffffu || r.packetId == 0xfffffffeu) && r.bunch.buf == outBunches_[used].buf) { r.packetId = pid; r.sentAt = now; }
        }
        outBunches_.erase(outBunches_.begin(), outBunches_.begin() + used);
        p.bit(1);   // UNetConnection terminator
        p.bit(1);   // PacketHandler terminator
        ++outPacketId_;
        sendRaw(p.buf.data(), p.buf.size());
        if (outBunches_.empty() && pendingAcks_.empty()) break;
    }
}

void SparcLink::update(double now) {
    now_ = now;
    if (state_ == State::Off || state_ == State::Failed || testMode_) return;
    if (stateAt_ < 0) stateAt_ = lastSend_ = lastRecv_ = now;
    uint8_t buf[2048];
    for (int k = 0; k < 256; ++k) {
        sockaddr_in from{};
#ifdef _WIN32
        int fl = sizeof from;
#else
        socklen_t fl = sizeof from;
#endif
        int n = (int)recvfrom(sock_, (char*)buf, sizeof buf, 0, (sockaddr*)&from, &fl);
        if (n <= 0) break;
        if (from.sin_addr.s_addr != addr_ || ntohs(from.sin_port) != port_) continue;
        receivePacket(buf, (size_t)n, now);
        if (sock_ < 0) return;
    }
    if (state_ == State::Handshake) {
        if (now - stateAt_ > 10) { fail("NO ANSWER FROM SERVER"); return; }
        if (now - lastSend_ > 1.0) sendHandshakeHello();
        return;
    }
    maybeJoin(now);
    if (now - lastRecv_ > 20) { fail("SERVER STOPPED RESPONDING"); return; }
    if ((state_ == State::Hello || state_ == State::Login) && now - stateAt_ > 15) { fail("LOGIN TIMED OUT"); return; }
    flush(now, false);
}

void SparcLink::maybeJoin(double now) {
    if (state_ == State::Loading && now - stateAt_ > 1.0 && !joinedSent_) {
        // ASSUMPTION: the server does not need the client to take as long as a real map load before NMT_Join
        BitWriter j;
        j.u8(NMT_Join);
        queueControl(j);
        joinedSent_ = true;
    }
}

void SparcLink::testReceive(const uint8_t* data, size_t len, double now) {
    now_ = now;
    receivePacket(data, len, now);
    maybeJoin(now);
    flush(now, false);
}

void SparcLink::receivePacket(const uint8_t* data, size_t len, double now) {
    if (len == 0 || data[len - 1] == 0) { ++decodeErrors_; return; }
    ++packetsIn_;
    lastRecv_ = now;
    uint32_t last = data[len - 1], top = 7;
    while (!(last >> top & 1)) --top;
    uint32_t n = (uint32_t)(len - 1) * 8 + top;   // handler payload bits
    BitReader r(data, n);
    if (r.bit()) {   // handshake packet: the challenge (or the server restarting the handshake)
        if (state_ == State::Handshake && len == 25) {
            // the client's response repeats the challenge exactly (secret id, timestamp, cookie)
            sendRaw(data, len);
            state_ = State::Hello;
            stateAt_ = now;
            BitWriter hello;
            hello.u8(NMT_Hello);
            hello.u8(1);
            hello.u32(kNetworkVersion);
            queueControl(hello, true);
            flush(now, true);
        }
        return;
    }
    // the connection's own terminator: the last 1 bit before n
    uint32_t inner = n - 1;
    while (inner > 1 && !((data[inner >> 3] >> (inner & 7)) & 1)) --inner;
    r.n = inner;
    uint32_t pid = r.readInt(MAX_PACKETID);
    pendingAcks_.push_back(pid);
    while (r.left() > 0 && !r.error) {
        if (r.bit()) {   // ack of one of our packets
            uint32_t ack = r.readInt(MAX_PACKETID);
            if (r.bit()) r.u8();   // server frame time
            r.intPacked();
            unacked_.erase(std::remove_if(unacked_.begin(), unacked_.end(), [&](const OutReliable& o) {
                               return o.packetId != 0xffffffffu && o.packetId != 0xfffffffeu && (o.packetId % MAX_PACKETID) == ack;
                           }), unacked_.end());
            continue;
        }
        bool control = r.bit();
        bool open = control ? r.bit() : false;
        bool close = control ? r.bit() : false;
        if (close) r.bit();   // dormant
        r.bit();              // replication paused
        bool reliable = r.bit();
        uint32_t ch = r.readInt(MAX_CHANNELS);
        bool exports = r.bit(), mustMap = r.bit(), partial = r.bit();
        uint32_t seq = reliable ? r.readInt(MAX_CHSEQUENCE) : 0;
        bool pInitial = partial ? r.bit() : false;
        bool pFinal = partial ? r.bit() : false;
        if (reliable || open) r.readInt(CHTYPE_MAX);
        uint32_t nbits = r.readInt(MAX_BUNCH_BITS);
        if (r.error || nbits > r.left()) { ++decodeErrors_; return; }
        BitReader body(data, r.pos + nbits);
        body.pos = r.pos;
        r.pos += nbits;
        handleBunch(ch, open, close, reliable, seq, exports, mustMap, partial, pInitial, pFinal, body, now);
        if (state_ == State::Failed || state_ == State::Off) return;
    }
    if (r.error) ++decodeErrors_;
}

void SparcLink::handleBunch(uint32_t ch, bool open, bool close, bool reliable, uint32_t rawSeq, bool exports, bool mustMap,
                            bool partial, bool pInitial, bool pFinal, BitReader body, double now) {
    // copy the bunch bits out (the packet buffer does not outlive this call)
    std::vector<uint8_t> bits((body.left() + 7) / 8 + 1, 0);
    uint32_t nb = body.left();
    for (uint32_t i = 0; i < nb; ++i) if (body.bit()) bits[i >> 3] |= (uint8_t)(1u << (i & 7));
    // pack the header flags we still need into the queued copy
    auto deliver = [&](std::vector<uint8_t>& b, uint32_t nbits, uint32_t flags) {
        bool o = flags & 1, c = flags & 2, ex = flags & 4, mm = flags & 8, pa = flags & 16, pi = flags & 32, pf = flags & 64;
        if (pa) {
            Partial& P = partial_[ch];
            if (pi) { P = Partial(); P.open = o; P.exports = ex; P.mustMap = mm; }
            else if (P.nbits == 0 && !P.open && !P.exports) return;   // missed the start: drop
            BitWriter w;
            w.buf = P.data;
            w.num = P.nbits;
            w.appendBits(b.data(), nbits);
            P.data = w.buf;
            P.nbits = w.num;
            if (!pf) return;
            Partial done = P;
            partial_.erase(ch);
            BitReader rd(done.data.data(), done.nbits);
            processBunch(ch, done.open, c, done.exports, done.mustMap, rd, now);
            return;
        }
        BitReader rd(b.data(), nbits);
        processBunch(ch, o, c, ex, mm, rd, now);
    };
    uint32_t flags = (open ? 1 : 0) | (close ? 2 : 0) | (exports ? 4 : 0) | (mustMap ? 8 : 0) | (partial ? 16 : 0) |
                     (pInitial ? 32 : 0) | (pFinal ? 64 : 0);
    if (!reliable) { deliver(bits, nb, flags); return; }
    InChannel& ic = inCh_[ch];
    int32_t seq = makeRelative((int32_t)rawSeq, (int32_t)ic.inReliable, (int32_t)MAX_CHSEQUENCE);
    if (seq <= (int32_t)ic.inReliable) return;   // duplicate
    if (seq != (int32_t)ic.inReliable + 1) {     // out of order: keep until the gap fills
        if (ic.queued.size() < 256) {
            std::vector<uint8_t> tagged = bits;
            tagged.push_back((uint8_t)flags);
            ic.queued[(uint32_t)seq] = {tagged, nb};
        }
        return;
    }
    ic.inReliable = (uint32_t)seq;
    deliver(bits, nb, flags);
    while (true) {
        auto it = ic.queued.find(ic.inReliable + 1);
        if (it == ic.queued.end()) break;
        std::vector<uint8_t> b = it->second.first;
        uint32_t f = b.back();
        b.pop_back();
        uint32_t qn = it->second.second;
        ic.queued.erase(it);
        ic.inReliable++;
        deliver(b, qn, f);
    }
}

uint32_t SparcLink::loadObject(BitReader& rd, bool exporting, int depth) {
    uint32_t g = rd.intPacked();
    if (g == 0 || rd.error) return 0;
    uint8_t flags = 0;
    if (g == 1 || exporting) flags = rd.u8();
    if (flags & 1) {   // has path
        uint32_t outer = depth < 16 ? loadObject(rd, exporting, depth + 1) : 0;
        std::string path = rd.fstring();
        if (flags & 4) rd.u32();   // network checksum
        if (!rd.error) guids_[g] = {outer, path};
    }
    return g;
}

std::string SparcLink::pathOf(uint32_t g) const {
    std::vector<std::string> parts;
    for (int k = 0; k < 16; ++k) {
        auto it = guids_.find(g);
        if (it == guids_.end()) break;
        parts.push_back(it->second.second);
        g = it->second.first;
    }
    std::string s;
    for (size_t i = parts.size(); i-- > 0;) { s += parts[i]; if (i) s += '.'; }
    return s;
}

void SparcLink::processBunch(uint32_t ch, bool open, bool close, bool exports, bool mustMap, BitReader rd, double now) {
    if (ch == 0) {
        if (close) { fail("SERVER CLOSED THE CONNECTION"); return; }
        controlMessages(rd, now);
        return;
    }
    if (exports) {
        if (rd.bit()) { ++decodeErrors_; return; }   // net field export block (replays only)
        int32_t count = rd.i32();
        if (count < 0 || count > 4096) { ++decodeErrors_; return; }
        for (int32_t i = 0; i < count && !rd.error; ++i) loadObject(rd, true);
    }
    if (mustMap) {
        uint32_t m = rd.bits(16);
        for (uint32_t i = 0; i < m && !rd.error; ++i) rd.intPacked();
    }
    if (open && !actors_.count(ch)) {
        ActorInfo a;
        a.guid = loadObject(rd, false);
        if (a.guid && !(a.guid & 1)) {   // dynamic: archetype, then the spawn transform
            a.archetype = loadObject(rd, false);
            if (rd.bit()) {   // location: FVector_NetQuantize10
                uint32_t bitsPer = rd.readInt(24);
                int32_t bias = 1 << (bitsPer + 1);
                uint32_t mx = 1u << (bitsPer + 2);
                for (float& v : a.loc) v = ((int32_t)rd.readInt(mx) - bias) / 10.0f;
            }
            a.path = pathOf(a.archetype);
        } else {
            a.path = pathOf(a.guid);
        }
        if (rd.error) ++decodeErrors_;
        actors_[ch] = a;
        if (state_ == State::Loading && joinedSent_) {   // the server is sending us the world: we are in
            state_ = State::Joined;
            joinedAt_ = now;
            LOGI("xcompat: joined as %s", name_.c_str());
        }
    }
    if (close) { actors_.erase(ch); return; }
    // properties and RPCs (the rest of the bunch) are not decoded yet; player names are picked out of player-state
    // bunches: the first well-formed short FString (checked against captures: PlayerName comes first)
    auto it = actors_.find(ch);
    if (it != actors_.end() && it->second.path.find("PlayerState") != std::string::npos) {
        for (uint32_t off = rd.pos; off + 64 <= rd.n; ++off) {
            BitReader q(rd.d, rd.n);
            q.pos = off;
            int32_t len = q.i32();
            if (len < 2 || len > 33 || q.left() < (uint32_t)len * 8) continue;
            std::string str;
            bool ok = true;
            for (int32_t i = 0; i < len && ok; ++i) {
                uint8_t c = q.u8();
                if (i == len - 1) ok = c == 0;
                else if (c < 32 || c > 126) ok = false;
                else str += (char)c;
            }
            if (ok) { it->second.name = str; break; }
        }
    }
}

void SparcLink::controlMessages(BitReader rd, double now) {
    while (rd.left() >= 8 && !rd.error) {
        uint8_t type = rd.u8();
        switch (type) {
        case NMT_Challenge: {
            std::string challenge = rd.fstring();
            (void)challenge;
            BitWriter m;
            m.u8(NMT_Login);
            m.fstring("0");   // ASSUMPTION: the stock client answers "0" (no online subsystem auth with -nosteam)
            m.fstring(loginUrl(name_));
            // FUniqueNetIdRepl: int32 size (the stock client sends twice the string length incl. the terminator)
            m.i32((int32_t)(uniqueId_.size() + 1) * 2);
            m.fstring(uniqueId_);
            queueControl(m);
            state_ = State::Login;
            stateAt_ = now;
            flush(now, true);
            break;
        }
        case NMT_Welcome: {
            map_ = rd.fstring();
            game_ = rd.fstring();
            if (rd.left() >= 32) rd.fstring();   // redirect
            BitWriter m;
            m.u8(NMT_Netspeed);
            m.i32((int32_t)kNetspeed);
            queueControl(m);
            state_ = State::Loading;
            stateAt_ = now;
            joinedSent_ = false;
            flush(now, true);
            LOGI("xcompat: welcome to %s (%s)", map_.c_str(), game_.c_str());
            break;
        }
        case NMT_Failure: {
            std::string e = "SERVER SAYS: " + rd.fstring();
            fail(e.c_str());
            return;
        }
        case NMT_Upgrade:
            fail("SERVER RUNS ANOTHER VERSION");
            return;
        case NMT_Netspeed: rd.i32(); break;
        default:
            return;   // unknown message: its length is unknown, stop reading this bunch
        }
    }
}

}  // namespace xc
