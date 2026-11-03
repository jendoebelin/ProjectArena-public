// Discord rich presence over Discord's local IPC pipe (see discord_rpc.h). A frame is a little-endian opcode and
// length, then JSON: 0 handshake {"v":1,"client_id":...} (Discord answers READY), 1 a command (SET_ACTIVITY), 2 close,
// 3 ping (answered with 4 pong, the same body).
#include "discord_rpc.h"
#include "../log.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace discord {
namespace {
// the owner's two Discord applications (Application IDs are public)
const char* kPlayApp = "1566867753594261634";        // "Project Arena"
const char* kSpectatorApp = "1566869054281482400";   // "Project Arena - Spectator"

std::mutex mu;
Activity want;
bool wantOn = false;
std::atomic<bool> running{false};
std::thread worker;

std::string esc(const std::string& s) {   // a JSON string's body
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}

std::string activityJson(const Activity& a) {
    std::string j = "{";
    auto add = [&](const std::string& kv) { if (j.size() > 1) j += ","; j += kv; };
    if (!a.details.empty()) add("\"details\":\"" + esc(a.details.substr(0, 120)) + "\"");
    if (!a.state.empty()) add("\"state\":\"" + esc(a.state.substr(0, 120)) + "\"");
    if (a.start > 0) add("\"timestamps\":{\"start\":" + std::to_string(a.start) + "}");
    add(std::string("\"assets\":{\"large_image\":\"logo\",\"large_text\":\"") + (a.spectator ? "Project Arena - Spectator" : "Project Arena") + "\"}");
    if (a.partySize > 0 && a.partyMax >= a.partySize)
        add("\"party\":{\"id\":\"pa-room\",\"size\":[" + std::to_string(a.partySize) + "," + std::to_string(a.partyMax) + "]}");
    return j + "}";
}

struct Pipe {
    HANDLE h = INVALID_HANDLE_VALUE;
    std::string app;   // the application it shook hands as
    bool open() const { return h != INVALID_HANDLE_VALUE; }
    void close() { if (open()) CloseHandle(h); h = INVALID_HANDLE_VALUE; app.clear(); }
    bool send(uint32_t op, const std::string& body) {
        std::vector<char> f(8 + body.size());
        const uint32_t len = (uint32_t)body.size();
        std::memcpy(f.data(), &op, 4); std::memcpy(f.data() + 4, &len, 4);
        std::memcpy(f.data() + 8, body.data(), body.size());
        DWORD w = 0;
        return WriteFile(h, f.data(), (DWORD)f.size(), &w, nullptr) && w == f.size();
    }
    // one frame if there is one (waiting up to ms for it): its opcode (-1 none, -2 the pipe broke)
    int read(std::string& body, int ms) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) return -2;
            if (avail >= 8) break;
            if (std::chrono::steady_clock::now() >= until) return -1;
            Sleep(10);
        }
        uint32_t hdr[2]; DWORD r = 0;
        if (!ReadFile(h, hdr, 8, &r, nullptr) || r != 8 || hdr[1] > (1u << 20)) return -2;
        body.resize(hdr[1]);
        DWORD got = 0;
        while (got < hdr[1]) {
            DWORD n = 0;
            if (!ReadFile(h, &body[got], hdr[1] - got, &n, nullptr) || n == 0) return -2;
            got += n;
        }
        return (int)hdr[0];
    }
};

bool connect(Pipe& p, const char* app) {
    for (int i = 0; i < 10; ++i) {
        char name[64];
        std::snprintf(name, sizeof name, "\\\\.\\pipe\\discord-ipc-%d", i);
        p.h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (p.open()) break;
    }
    if (!p.open()) return false;   // (Discord isn't running)
    if (!p.send(0, std::string("{\"v\":1,\"client_id\":\"") + app + "\"}")) { p.close(); return false; }
    std::string body;
    const int op = p.read(body, 3000);
    if (op != 1 || body.find("\"READY\"") == std::string::npos) {
        LOGW("discord: no READY from the Discord app (op %d): %.160s", op, body.c_str());
        p.close();
        return false;
    }
    p.app = app;
    LOGI("discord: connected as %s", app == kSpectatorApp ? "Project Arena - Spectator" : "Project Arena");
    return true;
}

void loop() {
    Pipe p;
    std::string sent;   // the activity as last sent ("" nothing)
    auto retry = std::chrono::steady_clock::now();
    int nonce = 0;
    while (running) {
        Activity a; bool on;
        { std::lock_guard<std::mutex> lk(mu); a = want; on = wantOn; }
        const char* app = a.spectator ? kSpectatorApp : kPlayApp;
        if (p.open()) {   // pings, answers, or the pipe gone
            std::string body;
            for (int op; (op = p.read(body, 0)) != -1;) {
                if (op == -2 || op == 2) { LOGI("discord: the Discord app closed the connection"); p.close(); sent.clear(); break; }
                if (op == 3) p.send(4, body);
            }
        }
        if (p.open() && on && p.app != app) { p.close(); sent.clear(); }   // (the other title: the other application)
        const std::string act = on ? activityJson(a) : "";
        if (on && !p.open() && std::chrono::steady_clock::now() >= retry) {
            if (!connect(p, app)) retry = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        }
        if (p.open() && act != sent) {
            const std::string cmd = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(GetCurrentProcessId()) +
                                    ",\"activity\":" + (act.empty() ? std::string("null") : act) + "},\"nonce\":\"" + std::to_string(++nonce) + "\"}";
            if (p.send(1, cmd)) sent = act;
            else { p.close(); sent.clear(); retry = std::chrono::steady_clock::now() + std::chrono::seconds(5); }
        }
        if (p.open() && !on && sent.empty()) p.close();   // (off: cleared, and let go)
        Sleep(250);
    }
    if (p.open()) {   // quitting: clear it (Discord also clears it when the pipe closes)
        p.send(1, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(GetCurrentProcessId()) + ",\"activity\":null},\"nonce\":\"0\"}");
        p.close();
    }
}
}   // namespace

void start() {
    if (running.exchange(true)) return;
    worker = std::thread(loop);
}
void set(const Activity& a) { std::lock_guard<std::mutex> lk(mu); want = a; wantOn = true; }
void clear() { std::lock_guard<std::mutex> lk(mu); wantOn = false; }
void stop() {
    if (!running.exchange(false)) return;
    if (worker.joinable()) worker.join();
}
}   // namespace discord
