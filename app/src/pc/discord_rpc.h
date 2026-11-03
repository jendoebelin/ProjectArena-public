// Discord rich presence on PC (0.3.13, owner 2026-11-02): "Playing Project Arena" / "Playing Project Arena - Spectator"
// in your Discord profile while the game runs. Talks to the Discord app on this PC through its local pipe
// (\\.\pipe\discord-ipc-N): no library, no sign-in, nothing sent to our servers. Two Discord applications (one for
// each title, the owner's); a thread does the talking, so a missing or slow Discord never holds up a frame.
#pragma once
#include <cstdint>
#include <string>

namespace discord {
struct Activity {
    bool spectator = false;   // which application: "Project Arena - Spectator", or "Project Arena"
    std::string details, state;   // the two lines under the title (empty: left out)
    int64_t start = 0;            // unix seconds it began (Discord shows the time since); 0 none
    int partySize = 0, partyMax = 0;   // "(3 of 8)"; 0 none
};
void start();                       // the thread (once; harmless if Discord isn't running: it looks again now and then)
void set(const Activity& a);        // what to show (sent when it changes)
void clear();                       // nothing (DISCORD STATUS: OFF, or quitting)
void stop();                        // clears it and ends the thread
}
