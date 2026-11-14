// BOT ROOM (owner, 2026-11-12: "host a room with a couple bots playing", to test the queue kiosk from a headset):
// two pretend players on this PC, BOT ONE hosting and BOT TWO in its tank, both moved by the game's own practice-bot
// brain (Game::stepBot), so the room sees two people playing and a visitor's JOIN QUEUE waits in line.
// A developer's tool only: it talks to the relay it is given (a private one: tools/run_bot_room.py), never the
// community server. Usage: bot_room.exe RELAY_ADDRESS [SECONDS] [BOT_LEVEL 0-5]
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>
// (stepBot is private to the game: this tool builds the game's code with it open, bot_room_game.cpp)
#define private public
#include "game.h"
#undef private
#include "net.h"

bool netAcquireMulticastLock() { return false; }   // (Windows delivers LAN broadcasts without one)

struct Node {
    Net net;
    Game game;
    PlayerInput local;
    Node(const char* name, float x) {
        net.init();
        net.setAppVersion("0.3.18");
        net.setName(name);
        game.reset(true);
        local.headTracked = true;
        local.head = {Quat{}, {x, 1.6f, -4.2f}};
        for (int h = 0; h < 2; ++h) { local.hand[h].tracked = true; local.hand[h].grip = {Quat{}, {x + (h ? 0.2f : -0.2f), 1.1f, -4.0f}}; }
    }
};

static double now() { return (double)std::chrono::steady_clock::now().time_since_epoch().count() * 1e-9; }

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* relay = argc > 1 ? argv[1] : "127.0.0.1:47790";
    const double seconds = argc > 2 ? std::atof(argv[2]) : 3600.0;
    const int level = argc > 3 ? std::atoi(argv[3]) : 3;
    Node A("BOT ONE", -4.5f), B("BOT TWO", -3.5f);
    Node* ns[2] = {&A, &B};
    // the court's walls, as the game loads them (owner, 2026-11-12: without them the host's balls went through the
    // glass, "flying off knuckles out of court")
    std::vector<CollisionTri> col;
    {
        const char* colPath = argc > 4 ? argv[4] : "app/assets_open/arena.col";
        std::vector<uint8_t> file;
        if (FILE* f = std::fopen(colPath, "rb")) {
            uint8_t buf[65536];
            for (size_t k; (k = std::fread(buf, 1, sizeof buf, f)) > 0;) file.insert(file.end(), buf, buf + k);
            std::fclose(f);
        }
        col = parseCollision(file.data(), file.size());
        if (col.empty()) { std::printf("bot room: no court collision (%s)\n", colPath); return 1; }
        std::printf("bot room: the court's walls, %u triangles\n", (unsigned)col.size());
    }
    for (Node* n : ns) n->game.useArena(ARENA_CLASSIC, col);
    const float dt = 1.f / 72.f;
    PlayerInput botIn[kMaxPlayers];
    int botSlot[2] = {-1, -1};
    double requeueAt[2] = {0, 0};
    bool wasOut[kMaxPlayers] = {};
    int stage = 0;   // 0 online, 1 hosting, 2 the host in the tank, 3 BOT TWO joining, 4 BOT TWO queued, 5 playing
    double stageAt = now(), last = now(), lastLog = 0;
    A.net.goOnline(relay);
    B.net.goOnline(relay);
    std::printf("bot room: connecting to %s (bot level %d)\n", relay, level);
    const double t0 = now();
    while (now() - t0 < seconds) {
        const double t = now();
        if (stage == 0 && A.net.onlineReady() && B.net.onlineReady()) { A.net.host(); stage = 1; stageAt = t; std::printf("bot room: BOT ONE hosts\n"); }
        else if (stage == 1 && t - stageAt > 0.5) { A.net.queue(true); stage = 2; stageAt = t; }
        else if (stage == 2 && !B.net.games().empty()) {
            B.net.join(B.net.games()[0].ip);
            stage = 3; stageAt = t; std::printf("bot room: BOT TWO joins\n");
        } else if (stage == 3 && t - stageAt > 1.0) { B.net.queue(true); stage = 4; stageAt = t; }
        else if (stage == 4 && A.net.localSlot() >= 0 && B.net.localSlot() >= 0) {
            stage = 5; std::printf("bot room: both in the tank (slots %d and %d): playing\n", A.net.localSlot(), B.net.localSlot());
        }
        if (stage == 0 && t - t0 > 10) { std::printf("bot room: no relay at %s\n", relay); return 1; }
        // the two players' moves: the host's game knows the match, so its bot brain plays both of them
        // (whichever of the two is in the tank plays; one sent out to the queue after a match, when someone else's turn
        // came, joins the queue again: owner, 2026-11-12, the host stood still after a visitor's turn, 0 - 51)
        botSlot[0] = botSlot[1] = -1;
        if (stage == 5) {
            Game& g = A.game;
            g.tun.botLevel = level;
            g.tun.botAdapt = false;
            g.events.clear();
            for (int i = 0; i < 2; ++i) {
                Node& n = *ns[i];
                const int s = n.net.localSlot();
                if (s >= 0 && s < g.np && !g.pl[s].isBot) {
                    g.stepBot(s, dt);
                    botIn[s] = n.local = g.pl[s].in;
                    botSlot[i] = s;
                } else if (n.net.localWaiting()) {
                    // standing in the waiting area as a person does, swaying a little: one who stands perfectly still is
                    // away, and the host takes them out of the queue (IDLE ROOMS; BOT TWO's queue never held)
                    n.local.head.p.y = 1.6f + 0.04f * (float)std::sin(t * 1.3 + i);
                    if (n.net.localQueuePos() < 0 && t - requeueAt[i] > 3.0) {
                        requeueAt[i] = t;
                        n.net.queue(true);
                        std::printf("bot room: %s waits: back in the queue\n", i ? "BOT TWO" : "BOT ONE");
                    }
                }
            }
            // (their throws happen here, before the step, which clears the events: kept for the step to report, as a
            // served ball's are; the room heard none of the bots' throws)
            for (const GameEvent& e : g.events) g.served_.push_back(e);
            g.events.clear();
        }
        for (Node* n : ns) {
            if (n->game.arenaKind != ARENA_CLASSIC) n->game.useArena(ARENA_CLASSIC, col);
            PlayerInput in[kMaxPlayers];
            if (n->net.exchange(n->local, in, n->game, dt)) {
                // the host plays BOT TWO's moves as its brain made them, this frame: the ones that came over the network
                // are a few frames old, and measured against the brain's own record of the hand they made throws far
                // too fast (owner, 2026-11-12: "the balls are flying away")
                if (n == &A) for (int k : botSlot) if (k >= 0) in[k] = botIn[k];
                n->game.step(dt, in);
                n->net.sendState(n->game);
                if (n == &A) {
                    for (const GameEvent& e : n->game.events) {
                        if (e.type == EventType::Throw) std::printf("bot room: slot %d throws at %.1f m/s\n", e.player, e.value);
                        if (e.type == EventType::KnuckleBlock) std::printf("bot room: slot %d knuckle block (%.1f)\n", e.player, e.value);
                    }
                    for (int p = 0; p < n->game.np; ++p) {   // a ball leaving the court: where and how fast
                        const Ball& b = n->game.pl[p].ball;
                        const bool out = std::fabs(b.pos.x) > 9.8f || std::fabs(b.pos.z) > 2.7f || b.pos.y > 4.3f || b.pos.y < -1.f;
                        if (out && !wasOut[p])
                            std::printf("bot room: slot %d's ball OUT at (%.1f %.1f %.1f), %.1f m/s (%.1f %.1f %.1f), last touched by slot %d\n", p,
                                        b.pos.x, b.pos.y, b.pos.z, length(b.vel), b.vel.x, b.vel.y, b.vel.z, b.instigator);
                        wasOut[p] = out;
                    }
                }
            }
        }
        if (t - lastLog > 10 && stage == 5) {
            lastLog = t;
            const RoomInfo& r = A.net.room();
            int queued = 0;
            for (const RoomInfo::Member& m : r.members) queued += m.queuePos >= 0;
            std::printf("bot room: %s  %d - %d  waiting %d, queued %d\n", A.game.match == MatchState::InProgress ? "playing" : "between points",
                        A.game.pl[0].score, A.game.pl[1].score, (int)r.members.size(), queued);
        }
        const double next = last + dt;
        last = next;
        const double wait = next - now();
        if (wait > 0) std::this_thread::sleep_for(std::chrono::microseconds((long long)(wait * 1e6)));
        else last = now();
    }
    return 0;
}
