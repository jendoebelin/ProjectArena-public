// AUTO ROTATION on the PC spectator (owner, 2026-11-12: "a mix of presets and your own cameras"): one list of every
// preset camera and every saved camera that isn't an action camera, each with an IN AUTO tick and a hold time; AUTO
// plays the ticked ones in the list's order. Until the streamer changes the list (custom), its ticks are what AUTO
// played before: the presets (side, blue's eyes, blue end, high, orange's eyes, orange end; 9 s each) while no camera
// of yours is in turn, your cameras only from then on. Action cameras are not in the list: they still cut in on hits.
// Kept free of the app so the tests can run it (app/tests/test_autorot.cpp).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace autorot {

struct Item {
    bool mine = false;   // one of MY CAMERAS (id: its number from 0), or a preset (id: the view, 1-9)
    int id = 1;
    bool on = true;      // IN AUTO
    float hold = 9.f;    // a preset's seconds (a camera of yours keeps its own hold, shared with the headset)
};

// the presets in the order AUTO played them (1 side, 6 blue's eyes, 3 blue end, 2 high, 7 orange's eyes, 4 orange end),
// then 5 TOP, which it never played; then 8 CORNER and 9 COURTSIDE (2026-11-19), in AUTO, and unticked in a list the
// streamer had made before them
static const int kPresets[9] = {1, 6, 3, 2, 7, 4, 5, 8, 9};
static const int kTopPreset = 5;   // (the one AUTO leaves out)
static const float kPresetHolds[4] = {2, 4, 9, 15};

inline bool same(const std::vector<Item>& a, const std::vector<Item>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].mine != b[i].mine || a[i].id != b[i].id || a[i].on != b[i].on || a[i].hold != b[i].hold) return false;
    return true;
}

inline int rowOf(const std::vector<Item>& L, bool mine, int id) {
    for (size_t i = 0; i < L.size(); ++i)
        if (L[i].mine == mine && L[i].id == id) return (int)i;
    return -1;
}

// the list made whole for the cameras there are now (trig: each of my cameras' type, 0 timer, 1 action, 2 follow):
// every preset once, each camera of yours that isn't an action camera once (a new one at the end, ticked), none that
// is gone. Not custom: the ticks are AUTO's old ones, your cameras in their numbers' order. True if anything changed
inline bool sync(std::vector<Item>& L, bool custom, const std::vector<uint8_t>& trig) {
    const std::vector<Item> before = L;
    std::vector<Item> kept;
    for (const Item& it : L) {
        if (it.mine ? it.id < 0 || it.id >= (int)trig.size() || trig[it.id] == 1 : it.id < 1 || it.id > 9) continue;
        if (rowOf(kept, it.mine, it.id) >= 0) continue;   // (once each)
        kept.push_back(it);
    }
    L = kept;
    for (int p : kPresets)
        if (rowOf(L, false, p) < 0) { Item it; it.id = p; it.on = p != kTopPreset && !custom; L.push_back(it); }
    for (int i = 0; i < (int)trig.size(); ++i)
        if (trig[i] != 1 && rowOf(L, true, i) < 0) { Item it; it.mine = true; it.id = i; L.push_back(it); }
    if (!custom) {
        bool anyMine = false;
        for (uint8_t t : trig) anyMine |= t != 1;
        std::vector<Item> presets, mine;
        for (Item& it : L) {
            it.on = it.mine || (!anyMine && it.id != kTopPreset);
            (it.mine ? mine : presets).push_back(it);
        }
        for (size_t i = 1; i < mine.size(); ++i)   // (by number, as AUTO went round them)
            for (size_t j = i; j > 0 && mine[j].id < mine[j - 1].id; --j) std::swap(mine[j], mine[j - 1]);
        L = presets;
        L.insert(L.end(), mine.begin(), mine.end());
    }
    return !same(before, L);
}

// the next ticked row after row `from` (-1: from the top), round to the start; -1 if none is ticked
inline int next(const std::vector<Item>& L, int from) {
    const int n = (int)L.size();
    for (int k = 1; k <= n; ++k) {
        const int i = ((from < 0 ? -1 : from) + k) % n;
        if (L[i].on) return i;
    }
    return -1;
}

// a preset's next hold time (2, 4, 9, 15 s, round again)
inline float nextHold(float h) {
    for (int i = 0; i < 4; ++i)
        if (h < kPresetHolds[i] - 0.01f) return kPresetHolds[i];
    return kPresetHolds[0];
}

// autocams.cfg: written once the streamer has changed the list; one row a line, "p 3 1 9" (preset 3, ticked, 9 s),
// "m 0 1" (my camera 1, ticked)
inline std::string save(const std::vector<Item>& L) {
    std::string s;
    char b[48];
    for (const Item& it : L) {
        if (it.mine) snprintf(b, sizeof b, "m %d %d\n", it.id, it.on ? 1 : 0);
        else snprintf(b, sizeof b, "p %d %d %.0f\n", it.id, it.on ? 1 : 0, it.hold);
        s += b;
    }
    return s;
}

inline std::vector<Item> load(const std::string& s) {
    std::vector<Item> L;
    size_t at = 0;
    while (at < s.size() && L.size() < 32) {
        size_t e = s.find('\n', at);
        if (e == std::string::npos) e = s.size();
        const std::string line = s.substr(at, e - at);
        at = e + 1;
        char k = 0;
        int id = 0, on = 1;
        float hold = 9.f;
        const int got = sscanf(line.c_str(), " %c %d %d %f", &k, &id, &on, &hold);
        if (got < 3 || (k != 'p' && k != 'm')) continue;
        Item it;
        it.mine = k == 'm'; it.id = id; it.on = on != 0;
        it.hold = hold >= 1.f && hold <= 60.f ? hold : 9.f;
        L.push_back(it);
    }
    return L;
}

}  // namespace autorot
