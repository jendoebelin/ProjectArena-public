// SOUND STUDIO (0.3.16, owner 2026-11-05; the idea from a community member's shield-break sound proposal): try sounds
// that aren't in the game yet in the moment they belong to, and vote on them. The library lives on the community
// server (server/projectarena_relay.py --sounds); the studio downloads it into the game's folder (sound_library),
// keeping it in step (a sound gone from the server goes here too). Alternate sounds play only inside the studio.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "net.h"

namespace studio {

// the studio's sections: the events that belong together, and the TRAINING lesson whose moment plays them (its id;
// 0: none, the section's sounds are tried with PLAY)
struct Section { const char* name; std::vector<const char*> events; int lesson; const char* how; };
const std::vector<Section>& sections();
std::string prettyEvent(const std::string& event);   // "ShieldBreak" -> "SHIELD BREAK"
// the asset catalog's code and name for an event's sound slot (tools/gen_catalog.py SOUNDS: S01-S33): "ShieldBreak" ->
// "S12", "SHIELD BREAK"; codeOf "" and the spaced event name for an event it doesn't know
std::string codeOf(const std::string& event);
std::string slotName(const std::string& event);      // "S12 SHIELD BREAK"
std::string prettyName(const std::string& file);     // "ShieldBreak__glass_crack.wav" -> "GLASS CRACK"
std::string eventOf(const std::string& file);        // -> "ShieldBreak"
std::string sha256Hex(const uint8_t* data, size_t n);

// a sound to try: one of the library's, or (official) the game's own sound for another slot, tried here (owner,
// 2026-11-06: with an empty library there was nothing to try; any of the game's sounds can be tried in any slot)
struct Sound { std::string file, event, name; int vote = 0; bool ready = false; bool official = false; bool mine = false; std::string path; };
std::vector<Sound> officialSounds();   // the game's own, one per slot (file "official:Event"; name "S13 KNUCKLE PUNCH")
// MINE (0.3.16): your own .wav files in `folder` (my_sounds in the game's folder), only on this headset. A name starting
// with a slot's code or event ("S12_glass.wav", "ShieldBreak__glass.wav") puts it first in that slot; any other name is
// offered in every slot. The folder is made (with a README) if it isn't there
std::vector<Sound> mySounds(const std::string& folder);
std::string slotFromName(const std::string& file);   // "S12_glass.wav" / "ShieldBreak__x.wav" -> "ShieldBreak" ("" none)

class Library {
public:
    enum State { IDLE, LOADING, READY, FAILED };
    // fetch the catalog and every sound not yet here (checked by SHA-256), delete the ones gone from it; on a thread
    void sync(const std::string& server, const std::string& folder, const uint8_t key[16]);
    State state() const { return state_.load(); }
    std::string error() const { std::lock_guard<std::mutex> lk(mu_); return err_; }
    // the sounds once synced (READY): new results are taken on the render thread with take()
    bool take(std::vector<Sound>& out);
    std::string path(const std::string& file) const { return folder_ + "/" + file; }
    int progress() const { return done_.load(); }
    int total() const { return total_.load(); }
    ~Library() { if (th_.joinable()) th_.join(); }
private:
    void run(std::string server, std::string folder, std::vector<uint8_t> key);
    mutable std::mutex mu_;
    std::thread th_;
    std::atomic<State> state_{IDLE};
    std::atomic<int> done_{0}, total_{0};
    std::vector<Sound> result_;
    bool fresh_ = false;
    std::string err_, folder_;
};

// for one event: OFFICIAL (-1) first, then the library's sounds made for it, then yours (MINE) made for it, then yours
// without a slot, then the library's (and your) others, then the game's own sounds of the other slots (each in the
// catalog's order); indices into `lib`
std::vector<int> choicesFor(const std::string& event, const std::vector<Sound>& lib);

}  // namespace studio
