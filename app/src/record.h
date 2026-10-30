// RECORD MY MATCHES the matches this headset plays, frame by frame, for tuning the practice
// bot from real play (tools/analyze_play.py). Off unless the player turns it on (MENU > MY STATS); the files stay on
// the headset (files/recordings, the newest kKeep kept) and leave it only if the player sends one.
// Only the player's own motion is kept: an opponent who is a person appears only as their ball and the game's events;
// a bot opponent's motion is kept too (it is not a person).
// File (little-endian): "PAREC1", then the header, then records, each starting with a tag byte:
//   header: u8 local player, u8 opponent is a bot, u8 bot level, u8 MATCH MY LEVEL on, f32 its level then,
//           u8 Advanced, u8 shields, i8 knuckle mode, u8 TRAINING's lesson (its id; 0: none, and before 0.3.12),
//           f32 match time, f32 round time, char[16] game version
//   'F' a step: f32 game time, f32 step length, u8 match state, u8 score[2]; per player: u8 motion kept, and if so
//       head (pos xyz, quat xyzw), per hand (pos, quat, trigger, squeeze, u8 tracked); per player's ball: u8 state,
//       i8 instigator, pos, vel, spin, f32 scale, u8 charged, i8 held hand
//   'E' an event of that step: u8 type, i8 player, i8 hand, i8 ball, pos, f32 value
//   'X' the end: f32 game time
#pragma once
#include <cstdio>
#include <string>
#include <vector>
#include "game.h"

namespace rec {

constexpr int kKeep = 20;              // recordings kept (the oldest go first)
constexpr float kMaxSeconds = 900.f;   // one recording ends after 15 minutes of play (about 25 MB)

struct Meta {
    int local = 0;
    const char* version = "";
    int lesson = 0;    // TRAINING: the lesson played (its id)
};

class Recorder {
public:
    ~Recorder() { close(); }
    bool active() const { return fp_ != nullptr; }
    bool begin(const std::string& path, const Game& g, const Meta& m);   // false: the file can't be written
    void frame(const Game& g, float dt);                                 // after each step: the step and its events
    void end(const Game& g);
    const std::string& path() const { return path_; }

private:
    void close();
    std::FILE* fp_ = nullptr;
    std::string path_;
    int local_ = 0;
    bool keepOpp_ = false;
    float seconds_ = 0;
};

// A recording read back (TRAINING's tutorial videos replay one): each step's players and balls, and its events
struct Replay {
    struct BallAt { BallState state; int instigator, heldHand; Vec3 pos, vel, spin; float scale; bool charged; };
    struct Frame {
        float t = 0;
        MatchState match = MatchState::WaitingForPlayers;
        bool kept[2] = {false, false};   // that player's motion is in it
        PlayerInput in[2];
        BallAt ball[2];
        std::vector<GameEvent> events;
    };
    int local = 0, lesson = 0;
    std::vector<Frame> frames;
    bool load(const uint8_t* data, size_t size);   // false: not a recording (or cut short before its first step)
};

// the recordings in `dir` beyond the newest `keep` are deleted (names sort by time: match_YYYYMMDD_HHMMSS.par)
void prune(const std::string& dir, int keep);

}  // namespace rec
