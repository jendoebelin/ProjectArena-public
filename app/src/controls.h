// BUTTONS (SETTINGS > CONTROLLERS > BUTTONS): which job each controller button does. A player's report, 2026-11-17:
// let us change the push-to-talk button, and the controls in general; the owner: a page to remap them, with a
// LEFT-HANDED preset, and a grip may do push to talk or another job that does no harm squeezed by accident, or nothing.
// The trigger (grab, throw, point at the menu) and the menu button stay where they are. A button does one job (or
// none); a job may have several buttons. TALK, READY and RECENTER always keep at least one button the controllers in
// your hands have. Your own headset's choice (settings.cfg buttons=): nothing is sent, every version unaffected.
#pragma once
#include <cstdint>
#include <string>

namespace controls {

// The buttons, by their Quest Touch names (other controllers: slotName)
enum Slot { S_LGRIP, S_RGRIP, S_A, S_B, S_X, S_Y, S_LSTICK, S_RSTICK, S_COUNT };
// The jobs: NONE, TALK (held: push to talk), PEEK (held: PASSTHROUGH > PEEK), NAMES (PLAYERS' NAMES on / off),
// READY (READY UP, a lesson's A), RECENTER
enum Job { J_NONE, J_TALK, J_PEEK, J_NAMES, J_READY, J_RECENTER, J_COUNT };
// The controllers in your hands (the OpenXR interaction profile; UNKNOWN until it's known: every button)
enum Kind { K_UNKNOWN, K_TOUCH, K_INDEX, K_VIVE, K_WMR };

struct Map {
    uint8_t job[S_COUNT];
    bool leftSticks;   // spectating: the left stick turns and the right moves (the right turns as standard)
};

inline bool sameMap(const Map& p, const Map& q) {
    for (int s = 0; s < S_COUNT; ++s)
        if (p.job[s] != q.job[s]) return false;
    return p.leftSticks == q.leftSticks;
}

// As the game always had it: the left grip talks, A and X ready up, B and the right stick's click recenter, Y peeks,
// the left stick's click shows the players' names
inline Map standard() { return {{J_TALK, J_NONE, J_READY, J_RECENTER, J_READY, J_PEEK, J_NAMES, J_RECENTER}, false}; }

inline int mirrorSlot(int s) {
    static const int m[S_COUNT] = {S_RGRIP, S_LGRIP, S_X, S_Y, S_A, S_B, S_RSTICK, S_LSTICK};
    return m[s];
}
inline Map mirrored(const Map& in) {
    Map out = in;
    for (int s = 0; s < S_COUNT; ++s) out.job[mirrorSlot(s)] = in.job[s];
    out.leftSticks = !in.leftSticks;
    return out;
}
// LEFT-HANDED: the standard, mirrored (the right grip talks, Y and the left stick's click recenter, B peeks, the right
// stick's click shows the names; spectating, the left stick turns)
inline Map leftHanded() { return mirrored(standard()); }

inline bool isGrip(int s) { return s == S_LGRIP || s == S_RGRIP; }
inline int slotHand(int s) { return s == S_LGRIP || s == S_X || s == S_Y || s == S_LSTICK ? 0 : 1; }   // 0 left, 1 right
// The jobs a button may do: a grip only those that do no harm squeezed by accident in play (TALK, PEEK, NAMES; not
// READY or RECENTER)
inline bool allowedOn(int s, int j) {
    if (j < 0 || j >= J_COUNT) return false;
    return !isGrip(s) || j == J_NONE || j == J_TALK || j == J_PEEK || j == J_NAMES;
}
inline bool required(int j) { return j == J_TALK || j == J_READY || j == J_RECENTER; }

// Whether the controllers in your hands have this button (Vive: trackpads, no sticks, no Y; Windows Mixed Reality: no Y)
inline bool onController(int kind, int s) {
    if (kind == K_VIVE) return s != S_Y && s != S_LSTICK && s != S_RSTICK;
    if (kind == K_WMR) return s != S_Y;
    return s >= 0 && s < S_COUNT;
}

inline int count(const Map& m, int j, int kind) {
    int n = 0;
    for (int s = 0; s < S_COUNT; ++s) n += onController(kind, s) && m.job[s] == j;
    return n;
}

// The map as it works on these controllers: a job that must have a button and has none here takes the first free one
// of its usual buttons (TALK the left grip, then the right; READY A, X, B; RECENTER B, A, X), never a button another
// such job needs
inline Map effective(const Map& in, int kind) {
    Map m = in;
    for (int s = 0; s < S_COUNT; ++s)
        if (!allowedOn(s, m.job[s])) m.job[s] = J_NONE;
    static const int order[J_COUNT][3] = {{-1, -1, -1}, {S_LGRIP, S_RGRIP, -1}, {-1, -1, -1}, {-1, -1, -1},
                                          {S_A, S_X, S_B}, {S_B, S_A, S_X}};
    for (int j = 0; j < J_COUNT; ++j) {
        if (!required(j) || count(m, j, kind) > 0) continue;
        for (int k = 0; k < 3; ++k) {
            const int s = order[j][k];
            if (s < 0 || !onController(kind, s)) continue;
            const int was = m.job[s];
            if (required(was) && count(m, was, kind) <= 1) continue;
            m.job[s] = (uint8_t)j;
            break;
        }
    }
    return m;
}

// Whether a button may give up its job here (a job that must have a button keeps its last one)
inline bool canLeave(const Map& m, int s, int kind) {
    const int j = m.job[s];
    return !required(j) || count(m, j, kind) > 1;
}
// The next job for a button, pointing at it on the page: NONE, TALK, PEEK, NAMES, READY, RECENTER in turn (a grip:
// NONE, TALK, PEEK, NAMES). Its job unchanged when it can't give it up
inline int nextJob(const Map& m, int s, int kind) {
    if (!canLeave(m, s, kind)) return m.job[s];
    int j = m.job[s];
    do j = (j + 1) % J_COUNT; while (!allowedOn(s, j));
    return j;
}

// Whether a job's button is held (pressed: each button, this frame; m: effective)
inline bool held(const Map& m, int j, int kind, const bool pressed[S_COUNT]) {
    for (int s = 0; s < S_COUNT; ++s)
        if (m.job[s] == j && onController(kind, s) && pressed[s]) return true;
    return false;
}
inline int firstSlot(const Map& m, int j, int kind) {   // -1: none
    for (int s = 0; s < S_COUNT; ++s)
        if (m.job[s] == j && onController(kind, s)) return s;
    return -1;
}

// The button's name on these controllers (Index: A and B on each hand, the left trackpad pressed firmly for Y; Vive
// and Windows Mixed Reality: the trackpads' clicks and the right menu button)
inline const char* slotName(int kind, int s) {
    static const char* touch[S_COUNT] = {"LEFT GRIP", "RIGHT GRIP", "A", "B", "X", "Y", "LEFT STICK", "RIGHT STICK"};
    static const char* index[S_COUNT] = {"LEFT GRIP", "RIGHT GRIP", "RIGHT A", "RIGHT B", "LEFT A", "LEFT PAD", "LEFT STICK", "RIGHT STICK"};
    static const char* pads[S_COUNT] = {"LEFT GRIP", "RIGHT GRIP", "RIGHT PAD", "RIGHT MENU", "LEFT PAD", "Y", "LEFT STICK", "RIGHT STICK"};
    if (s < 0 || s >= S_COUNT) return "";
    return kind == K_INDEX ? index[s] : kind == K_VIVE || kind == K_WMR ? pads[s] : touch[s];
}
inline const char* jobName(int j) {
    static const char* n[J_COUNT] = {"NOTHING", "TALK", "PEEK", "NAMES", "READY", "RECENTER"};
    return j >= 0 && j < J_COUNT ? n[j] : "";
}

// settings.cfg: a digit for each button's job, then the sticks (0 standard, 1 left), e.g. standard "10454235" + "0"
inline std::string save(const Map& m) {
    std::string s;
    for (int k = 0; k < S_COUNT; ++k) s += (char)('0' + m.job[k]);
    s += m.leftSticks ? '1' : '0';
    return s;
}
// false (m unchanged): not a map this game reads (a job a button can't do, or TALK, READY or RECENTER without a button)
inline bool load(Map& m, const std::string& v) {
    if (v.size() != S_COUNT + 1) return false;
    Map r{};
    for (int k = 0; k < S_COUNT; ++k) {
        const int j = v[k] - '0';
        if (!allowedOn(k, j)) return false;
        r.job[k] = (uint8_t)j;
    }
    if (v[S_COUNT] != '0' && v[S_COUNT] != '1') return false;
    r.leftSticks = v[S_COUNT] == '1';
    for (int j = 0; j < J_COUNT; ++j)
        if (required(j) && count(r, j, K_UNKNOWN) == 0) return false;
    m = r;
    return true;
}

}   // namespace controls
