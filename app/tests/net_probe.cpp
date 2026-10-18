// Headless network probe: runs the game's real Net + Game code as a command-line program on a Quest (adb shell),
// to test LAN and relay play between headsets without wearing them. Build: tools/build_net_probe.sh
// Usage: net_probe <host|join|spectate> <seconds> [server host:port] [name] [suit]
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../src/avatar.h"
#include "../src/log.h"
#include "../src/net.h"

bool netAcquireMulticastLock() { return false; }   // a shell process cannot take the Wi-Fi lock; broadcast still works

static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: net_probe <host|join|spectate> <seconds> [server] [name] [suit]\n"); return 2; }
    const std::string role = argv[1];
    const double secs = std::atof(argv[2]);
    const char* server = argc > 3 && std::strcmp(argv[3], "-") ? argv[3] : nullptr;
    Net net;
    net.init();
    net.setName(argc > 4 ? argv[4] : role.c_str());
    Game g;
    g.reset(true);
    if (server && !net.goOnline(server)) { std::printf("server did not resolve\n"); return 1; }
    PlayerInput local;
    local.headTracked = true;
    local.avatar = avatar::preset(argc > 5 ? std::atoi(argv[5]) : 0);
    for (int h = 0; h < 2; ++h) local.hand[h].tracked = true;
    const double t0 = now();
    double lastPrint = 0;
    bool joined = false, hosting = false, spectating = false;
    while (now() - t0 < secs) {
        const double t = now() - t0;
        local.head = {Quat::axisAngle({0, 1, 0}, 0.3f * std::sin((float)t)), {0, 1.6f + 0.05f * std::sin(3 * (float)t), 0}};
        for (int h = 0; h < 2; ++h) local.hand[h].grip = {Quat{}, {h ? 0.25f : -0.25f, 1.1f, -0.3f}};
        if (!server || net.onlineReady()) {
            if (role == "host" && !hosting) { net.host(); hosting = true; }
            if (role == "spectate" && !spectating && t > 1.0) { net.spectate(); spectating = true; }
            if ((role == "join" || role == "queue") && !joined) {
                std::vector<LanGame> gs = net.games();
                if (!gs.empty()) { net.join(gs[0].ip); joined = true; std::printf("[%5.1fs] joining %s\n", t, gs[0].name); }
            }
        }
        PlayerInput in[2];
        if (net.exchange(local, in, g, 1.f / 90.f)) { g.step(1.f / 90.f, in); net.sendState(g); }
        const int L = net.localIndex();
        if (role == "queue" && joined && net.localWaiting() && net.localQueuePos() < 0 && t > 3) net.queue(true);
        if (role == "host" && hosting && net.localWaiting() && net.localQueuePos() < 0 && t > 1) net.queue(true);   // play the bot
        if (t - lastPrint > 2.0) {
            lastPrint = t;
            const PlayerInput& other = g.pl[1 - L].in;
            std::printf("[%5.1fs] %s | online %d ready %d | games %zu | peer %d | remote look: %s / %s | matches %zu | %s\n", t,
                        net.status(), net.online(), net.onlineReady(), net.games().size(), net.peerConnected() ? 1 : 0,
                        avatar::optionName(avatar::F_SUIT, other.avatar.suit), avatar::optionName(avatar::F_HEADGEAR, other.avatar.headgear),
                        net.matches().size(), net.noticeText() ? net.noticeText() : "");
            std::fflush(stdout);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(11));
    }
    if (net.online()) net.goOffline(); else net.leave();
    net.shutdown();
    return 0;
}
