// The selfie overlay's webcam (PC, 0.3.13, owner 2026-11-02: streamers talk to their audience over the broadcast).
// Windows Media Foundation reads the camera on its own thread; the latest picture waits here, mirrored as a selfie is,
// at most 640 wide, as RGBA (a round frame: the corners already clear). Nothing leaves this PC.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace webcam {
void start(int device);   // the device-th camera (wraps round); harmless without one (status() says so)
void stop();
int devices();            // how many cameras there are (asked again on each start)
// the latest picture, if it is newer than `seen` (its number then put in `seen`): w x h RGBA, top row first
bool latest(uint32_t& seen, std::vector<uint8_t>& rgba, int& w, int& h);
void setRound(bool round);   // round: everything outside the circle clear
std::string status();        // "" running; else why not (no camera, in use by another app, ...)
}
