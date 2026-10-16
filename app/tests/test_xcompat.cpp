// Tests for the experimental Sparc cross-compatibility client (app/src/xcompat.cpp).
//   test_xcompat.exe [capture.bin]
// Without a capture: bit-stream round trips. With one (tools/xc_export_capture.py, from a pktmon capture of a real
// Sparc client): our Hello / Login bunches must match the stock client's bit for bit (given its name and id), and
// the whole server side of the capture must decode with no errors and end with the same actor channels open.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../src/xcompat.h"

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    (ok ? g_pass : g_fail)++;
}

struct Rec { int dir; double t; std::vector<uint8_t> data; };

static std::vector<Rec> load(const char* path) {
    std::vector<Rec> v;
    FILE* f = std::fopen(path, "rb");
    if (!f) return v;
    while (true) {
        uint8_t dir;
        double t;
        uint16_t len;
        if (std::fread(&dir, 1, 1, f) != 1 || std::fread(&t, 8, 1, f) != 1 || std::fread(&len, 2, 1, f) != 1) break;
        Rec r{dir, t, std::vector<uint8_t>(len)};
        if (len && std::fread(r.data.data(), 1, len, f) != len) break;
        v.push_back(std::move(r));
    }
    std::fclose(f);
    return v;
}

// the control-channel bunch payloads in a client packet (bits as '0'/'1' strings)
static std::vector<std::string> controlPayloads(const std::vector<uint8_t>& pkt) {
    std::vector<std::string> out;
    if (pkt.empty() || pkt.back() == 0) return out;
    uint32_t top = 7;
    while (!(pkt.back() >> top & 1)) --top;
    uint32_t n = (uint32_t)(pkt.size() - 1) * 8 + top;
    xc::BitReader r(pkt.data(), n);
    if (r.bit()) return out;
    uint32_t inner = n - 1;
    while (inner > 1 && !((pkt[inner >> 3] >> (inner & 7)) & 1)) --inner;
    r.n = inner;
    r.readInt(16384);
    while (r.left() > 0 && !r.error) {
        if (r.bit()) { r.readInt(16384); if (r.bit()) r.u8(); r.intPacked(); continue; }
        bool control = r.bit();
        bool open = control ? r.bit() : false;
        bool close = control ? r.bit() : false;
        if (close) r.bit();
        r.bit();
        bool rel = r.bit();
        uint32_t ch = r.readInt(10240);
        r.bit(); r.bit();
        bool partial = r.bit();
        if (rel) r.readInt(1024);
        if (partial) { r.bit(); r.bit(); }
        if (rel || open) r.readInt(8);
        uint32_t nb = r.readInt(8192);
        std::string s;
        for (uint32_t i = 0; i < nb; ++i) s += r.bit() ? '1' : '0';
        if (ch == 0) out.push_back(s);
    }
    return out;
}

int main(int argc, char** argv) {
    std::printf("bit streams\n");
    {
        xc::BitWriter w;
        w.writeInt(78, 128); w.intPacked(410); w.fstring("Hello"); w.writeInt(5000, 10240); w.bit(1);
        xc::BitReader r(w.buf.data(), w.num);
        check(r.readInt(128) == 78, "SerializeInt round trip");
        check(r.intPacked() == 410, "SerializeIntPacked round trip");
        check(r.fstring() == "Hello", "FString round trip");
        check(r.readInt(10240) == 5000, "SerializeInt with a non power of two max");
        check(r.bit() && r.left() == 0 && !r.error, "stream ends where it should");
    }
    check(xc::SparcLink::loginUrl("mojo_PA").find("?Name=mojo_PA?") != std::string::npos, "login URL carries the tagged name");

    const char* path = argc > 1 ? argv[1] : nullptr;
    std::vector<Rec> cap = path ? load(path) : std::vector<Rec>();
    if (cap.empty()) {
        std::printf("no capture given: skipping the replay tests\n");
    } else {
        std::printf("replay of %s (%zu packets)\n", path, cap.size());
        // the stock client's name and id, read from its login URL, so our bunches can be compared bit for bit
        std::string name, id;
        std::vector<std::string> stockControl;
        for (const Rec& r : cap) {
            if (r.dir != 0 || r.data.size() == 25) continue;
            for (const std::string& s : controlPayloads(r.data)) stockControl.push_back(s);
        }
        // decode the stock login (2nd control message) for the name and id
        if (stockControl.size() >= 2) {
            const std::string& s = stockControl[1];
            std::vector<uint8_t> bytes((s.size() + 7) / 8);
            for (size_t i = 0; i < s.size(); ++i) if (s[i] == '1') bytes[i >> 3] |= (uint8_t)(1 << (i & 7));
            xc::BitReader r(bytes.data(), (uint32_t)s.size());
            r.u8(); r.fstring();
            std::string url = r.fstring();
            r.i32();
            id = r.fstring();
            size_t a = url.find("?Name="), b = url.find('?', a + 1);
            if (a != std::string::npos) name = url.substr(a + 6, b - a - 6);
            check(url == xc::SparcLink::loginUrl(name), "our login URL matches the stock one (apart from the name)");
        }
        xc::SparcLink link;
        link.testBegin(name, id);
        for (const Rec& r : cap) {
            if (r.dir != 1 || r.data.size() == 25) continue;   // server side; the handshake is not replayed
            link.testReceive(r.data.data(), r.data.size(), r.t);
        }
        std::vector<std::string> ours;
        for (auto& p : link.testSent)   // in a replay the server's acks name the stock client's packets, so ours get resent
            for (const std::string& s : controlPayloads(p))
                if (std::find(ours.begin(), ours.end(), s) == ours.end()) ours.push_back(s);
        check(ours.size() >= 4, "we sent Hello, Login, Netspeed and Join");
        for (size_t i = 0; i < 4 && i < ours.size() && i < stockControl.size(); ++i) {
            static const char* names[4] = {"Hello", "Login", "Netspeed", "Join"};
            char what[96];
            std::snprintf(what, sizeof what, "%s matches the stock client bit for bit", names[i]);
            check(ours[i] == stockControl[i], what);
        }
        check(link.map() == "/Game/Maps/Courtside/l_courtside_00", "welcome: map");
        check(link.decodeErrors() == 0, "server stream decodes with no errors");
        std::printf("    %d actor channels open, %d decode errors, state %s\n", link.channelCount(), (int)link.decodeErrors(),
                    link.stateText());
        check(link.channelCount() == 61, "61 actor channels open at the end (as tools/ue415_actors.py finds)");
        check(link.countOf("SparcPlayerController") == 1, "our player controller is open");
        check(link.countOf("CourtPlayerQueue_BP") == 2, "both court queues are open");
        check(link.state() == xc::SparcLink::State::Joined, "state: joined");
        bool found = false;
        for (const std::string& n : link.playerNames()) {
            std::printf("    player: %s\n", n.c_str());
            if (!name.empty() && n == name.substr(0, n.size()) && n.size() >= 8) found = true;
        }
        check(found, "our own name comes back in a player state");
    }
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
