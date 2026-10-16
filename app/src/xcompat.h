// EXPERIMENTAL SOFTWARE - cross-compatibility connection.
// Lets Project Arena join Sparc games on the Sparc Reviver community server as a regular UE 4.15 client, so Quest
// players can play with the Sparc community without a PC. Each side draws the other with its own models.
// Status: TEST BUILDS ONLY. Everything here comes from captures of a real Sparc client
// (the developers' research notes). Each value copied from a capture
// rather than derived from the engine is marked ASSUMPTION; those are the things test sessions should confirm.
// What works (v1): the UE 4.15 stateless handshake, NMT_Hello / Login / Welcome / Netspeed / Join, packet and
// bunch framing, acks, reliable ordering and resends, partial bunch reassembly, NetGUID exports, and new-actor
// headers. That covers connecting, staying connected as a spectator, and listing what the server opens.
// Not yet: decoding replicated properties (movement, scores, names), sending our pose, queueing, or playing.
#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace xc {

void socketsInit();   // Windows: WSAStartup for tools that run without the rest of the app (no-op elsewhere)

// LSB-first bit streams, like FBitReader / FBitWriter.
struct BitWriter {
    std::vector<uint8_t> buf;
    uint32_t num = 0;
    void bit(bool v);
    void bits(uint32_t v, int n);
    void writeInt(uint32_t v, uint32_t max);   // SerializeInt
    void intPacked(uint32_t v);                // SerializeIntPacked
    void u8(uint8_t v) { bits(v, 8); }
    void u32(uint32_t v) { bits(v, 32); }
    void i32(int32_t v) { bits((uint32_t)v, 32); }
    void fstring(const std::string& s);        // int32 length incl. terminator, ANSI chars
    void append(const BitWriter& o);
    void appendBits(const uint8_t* data, uint32_t nbits, uint32_t from = 0);
};

struct BitReader {
    const uint8_t* d = nullptr;
    uint32_t n = 0, pos = 0;
    bool error = false;
    BitReader() = default;
    BitReader(const uint8_t* data, uint32_t nbits) : d(data), n(nbits) {}
    uint32_t left() const { return pos < n ? n - pos : 0; }
    bool bit();
    uint32_t bits(int k);
    uint32_t readInt(uint32_t max);
    uint32_t intPacked();
    uint8_t u8() { return (uint8_t)bits(8); }
    uint32_t u32() { return bits(32); }
    int32_t i32() { return (int32_t)bits(32); }
    float f32();
    std::string fstring();
};

// What the server opened on one actor channel.
struct ActorInfo {
    uint32_t guid = 0, archetype = 0;
    std::string path;          // archetype path (dynamic actors) or the actor's own path (static ones)
    std::string name;          // player states: the player's name (see SparcLink::playerNames)
    float loc[3] = {0, 0, 0};
};

class SparcLink {
public:
    enum class State { Off, Handshake, Hello, Login, Loading, Joined, Failed };
    // hostPort "ip[:port]" (port 7777 by default); playerName as it should appear (already tagged, e.g. "mojo_PA");
    // uniqueId: a stable id for this install (no PC or device names)
    bool connect(const char* hostPort, const std::string& playerName, const std::string& uniqueId);
    void disconnect(const char* why = nullptr);
    void update(double now);   // receive, reply, keep alive; call every frame (cheap when off)

    State state() const { return state_; }
    const char* stateText() const;
    const std::string& error() const { return error_; }
    const std::string& map() const { return map_; }
    const std::string& game() const { return game_; }
    const std::string& nameSent() const { return name_; }
    double connectedFor(double now) const { return state_ == State::Joined ? now - joinedAt_ : 0; }
    int channelCount() const { return (int)actors_.size(); }
    int countOf(const char* pathPart) const;   // open actor channels whose path contains pathPart
    uint64_t packetsIn() const { return packetsIn_; }
    uint64_t packetsOut() const { return packetsOut_; }
    uint32_t decodeErrors() const { return decodeErrors_; }
    const std::map<uint32_t, ActorInfo>& actors() const { return actors_; }
    // names of the players on the server (from their player states; the first string the server sends in a
    // PlayerState bunch is PlayerName, cut to 20 characters by the engine). Heuristic until properties are decoded.
    std::vector<std::string> playerNames() const;

    // --- for tests: run the protocol without a socket
    void testBegin(const std::string& playerName, const std::string& uniqueId);   // as if the handshake just finished
    void testReceive(const uint8_t* data, size_t len, double now);
    std::vector<std::vector<uint8_t>> testSent;   // packets the link would have sent (test mode)
    static std::string loginUrl(const std::string& playerName);

private:
    struct OutReliable { uint32_t ch; BitWriter bunch; uint32_t packetId; double sentAt; };
    struct Partial { std::vector<uint8_t> data; uint32_t nbits = 0; bool open = false, exports = false, mustMap = false; };
    struct InChannel { uint32_t inReliable = 0; std::map<uint32_t, std::pair<std::vector<uint8_t>, uint32_t>> queued; };

    void sendRaw(const uint8_t* data, size_t len);
    void sendHandshakeHello();
    void receivePacket(const uint8_t* data, size_t len, double now);
    void handleBunch(uint32_t ch, bool open, bool close, bool reliable, uint32_t seq, bool exports, bool mustMap,
                     bool partial, bool pInitial, bool pFinal, BitReader body, double now);
    void processBunch(uint32_t ch, bool open, bool close, bool exports, bool mustMap, BitReader rd, double now);
    void controlMessages(BitReader rd, double now);
    void queueControl(const BitWriter& msg, bool open = false);
    void queueReliable(uint32_t ch, uint32_t chType, const BitWriter& payload, bool open);
    void flush(double now, bool force);
    void maybeJoin(double now);
    uint32_t loadObject(BitReader& rd, bool exporting, int depth = 0);
    std::string pathOf(uint32_t guid) const;
    void fail(const char* why);

    State state_ = State::Off;
    std::string error_, map_, game_, name_, uniqueId_;
    int sock_ = -1;
    uint32_t addr_ = 0;   // network byte order
    uint16_t port_ = 7777;
    bool testMode_ = false;
    double now_ = 0, stateAt_ = 0, lastSend_ = 0, lastRecv_ = 0, joinedAt_ = 0;
    std::vector<uint8_t> handshakeReply_;

    uint32_t outPacketId_ = 0;
    std::vector<uint32_t> pendingAcks_;
    std::map<uint32_t, uint32_t> outSeq_;   // channel -> last reliable sequence sent
    std::deque<OutReliable> unacked_;
    std::vector<BitWriter> outBunches_;       // bunches waiting for the next packet (already headered)
    std::map<uint32_t, InChannel> inCh_;
    std::map<uint32_t, Partial> partial_;
    std::map<uint32_t, std::pair<uint32_t, std::string>> guids_;   // guid -> (outer, path)
    std::map<uint32_t, ActorInfo> actors_;   // channel -> actor
    bool controlOpen_ = false, joinedSent_ = false;
    uint64_t packetsIn_ = 0, packetsOut_ = 0;
    uint32_t decodeErrors_ = 0;
};

}  // namespace xc
