// UPDATE NOW on PC (0.3.13, owner 2026-10-30): the newest release's zip from GitHub, checked against the release's own
// SHA256SUMS.txt, unpacked (Windows' tar), and, when the player says so, put over the game's folder by a small script
// that waits for the game to close and starts it again. A copy from Steam is Steam's to update; a folder the game
// can't write to (Program Files) gets the download page instead.
#pragma once
#include <string>

namespace updater {
enum State { IDLE, DOWNLOADING, CHECKING, UNPACKING, READY, FAILED };
// how this copy can be updated: 0 here (download and restart), 1 by Steam, 2 the download page only (why: the reason)
int how(std::string& why);
void start(const std::string& version);   // download and unpack that version, in the background
State state();
float progress();                         // 0..1 while downloading
std::string error();                      // FAILED: what went wrong
bool restart();                           // READY: start the script (it waits for this game to close); the caller quits
void openPage();                          // the release page in the browser
}
