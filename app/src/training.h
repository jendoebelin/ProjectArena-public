// TRAINING (owner, 2026-10-29): progressive lessons for new players in three tracks (owner: throwing, blocking and
// deflecting, dodging): targets at every depth and height, rings in the air, and a thrower at the far end for the
// defense tracks. Each lesson is passed to open the next in its track; its best TIME and ACCURACY go to the
// leaderboards; SHOW ME HOW plays a real run of it (docs/PLAN_TRAINING.md).
// Platform independent: tests/test_game.cpp plays every lesson.
#pragma once
#include <string>
#include <vector>
#include "game.h"

namespace training {

enum Kind : uint8_t { THROW, STRIKES, DEFEND, CATCH };
// DEFEND: how you meet the far end's throws (G_EITHER: shield or knuckle guard, whichever you put in the way)
enum Guard : uint8_t { G_NONE, G_SHIELD, G_KNUCKLE, G_DODGE, G_SENDBACK, G_EITHER };
// the tracks (owner, 2026-10-29)
enum Track : uint8_t { T_THROW, T_BLOCK, T_DODGE, kTracks };
const char* trackName(int t);
// DEFEND: where the throws go (round you as you stand)
enum Aim : uint8_t { AIM_CHEST, AIM_WIDE, AIM_HEAD, AIM_SHOULDERS, AIM_MIXED };

struct TargetDef {
    Vec3 pos;
    float r;
    Vec3 hoop;        // CURVE: through this ring first (it faces along the court); hoopR 0: none
    float hoopR;
    float slide, slideW, phase;   // MOVING: across the court, z + slide * sin(slideW * t + phase)
};

struct Lesson {
    uint8_t id;        // the server's number for it (never reused)
    Track track;
    const char* name;
    const char* teach;           // what you learn, one line
    const char* tip1;            // the coach's two lines
    const char* tip2;
    Kind kind;
    Guard guard;
    std::vector<TargetDef> targets;
    int budget;        // THROW, STRIKES: throws you have; DEFEND, CATCH: tries in the lesson
    int pass;          // STRIKES, DEFEND, CATCH: successes to pass (THROW: every target)
    float minSpeed;    // POWER: a target breaks only for a ball this fast (m/s)
    bool bank;         // BANK SHOTS: a target counts only off a side wall
    int atOnce;        // targets up at once (the next comes up as one goes down; 0: all of them)
    float serveSpeed, serveSpeedUp;   // DEFEND: the first throw (m/s), and how much faster each one comes
    Aim aim;
    bool timed() const { return kind != DEFEND || guard == G_SENDBACK; }   // (a TIME board too)
    int goal() const { return kind == THROW || guard == G_SENDBACK ? (int)targets.size() : pass; }
};
const std::vector<Lesson>& lessons();
int lessonById(int id);   // its index (-1: none)
int nextInTrack(int index);   // the next lesson of its track (-1: it was the last)
int placeInTrack(int index, int* count = nullptr);   // 1-based, and how many the track has

constexpr float kWallX = 9.42f;   // the far wall's targets (the wall at 9.5 m)
constexpr float kSideZ = 2.5f;    // the side walls

struct Target {
    TargetDef d;
    bool down = false;
    bool live = true;          // shown and in play (Lesson::atOnce of them: the next comes up as one goes down)
    float downT = -1;
    Vec3 at(float t) const { return d.pos + Vec3{0, 0, d.slide * std::sin(d.slideW * t + d.phase)}; }
};

void refreshLive(const Lesson& L, std::vector<Target>& T);   // the first atOnce targets not yet down are up

// A flight being judged: one ball, one throw (or block) of it
struct Flight {
    bool on = false;
    Vec3 prev;
    int bounces = 0;
    bool banked = false;   // off a side wall
    int hoop = -1;         // the target whose ring it went through
    uint32_t told = 0;     // targets it has already been told off for (too slow, not banked, no ring): once a flight
    void start(const Ball& b) { on = true; prev = b.pos; bounces = b.bounces; banked = false; hoop = -1; told = 0; }
};
enum class Cue : uint8_t { TargetDown, Ring, TooSlow, NotBanked, NoRing, Strike, BodyHit, Blocked, Punched, Dodged, Hit,
                           Caught, Dropped, Recharged, Passed, Failed, Go };
struct CueEvent { Cue cue; Vec3 pos; int value; };
// ball b moved to `to` at lesson time t: the target it took (-1: none); why one didn't count goes to `cues`
int judge(const Lesson& L, std::vector<Target>& T, Flight& f, const Ball& b, float ballR, const Vec3& to, float t,
          std::vector<CueEvent>* cues);

// a throw that takes target i from `from` (the coach's throws, and the tests' proof that every target can be had)
struct Shot { Vec3 dir, spin; float speed = 0; bool ok = false; };
Shot planShot(const Game& g, int p, const Vec3& from, const Lesson& L, const std::vector<Target>& T, int i, float t0,
              bool assist);
std::vector<Vec3> aimPoints(const Lesson& L, const std::vector<Target>& T, float t);   // aim assist toward (live targets)

struct Result {
    int lesson = -1;          // index
    bool passed = false;
    int successes = 0, attempts = 0;
    float time = 0;           // seconds, from GO to the last target (timed lessons)
    int permille() const { return attempts > 0 ? successes * 1000 / attempts : 0; }
};
// your bests, kept on the headset: per lesson id, the best passed TIME and ACCURACY (a better run replaces them)
struct Best {
    bool passed = false;
    float time = 0;           // 0: none
    int successes = 0, attempts = 0;
    int permille() const { return attempts > 0 ? successes * 1000 / attempts : 0; }
};
struct Bests {
    Best b[256];
    bool unlocked(int index) const;   // the first, and each one after a passed lesson
    bool add(const Result& r);        // true: a new best (time or accuracy)
    std::string save() const;         // "id passed time successes attempts" lines
    void load(const std::string& s);
    int total() const;                // the overall board: every lesson's best accuracy added up (permille)
};

// a person's throw, made by the hands (the tests' proof that people's throws count):
// reach for the ball, pull the trigger, swing back, sweep through toward where it should go, let go of the trigger,
// follow through; CATCH: then the trigger held, a hand where the ball comes back. Player p must face +X or -X
struct Thrower {
    int phase = 0, reps = 0, target = -1;   // (the target this throw is for)
    float t = 0;
    const char* caption = "";               // the step it shows ("3  SWING YOUR ARM BACK")
    PlayerInput step(const Game& g, int p, const Lesson& L, const std::vector<Target>& T, float clock, float dt);
private:
    void plan(const Game& g, int p, const Lesson& L, const std::vector<Target>& T, float clock);
    Vec3 hold_, back_, rel_, dir_, spin_, follow_;
    Quat q_;
    float vh_ = 5.f, ta_ = 0.1f, tc_ = 0.18f;
};

enum class Phase : uint8_t { Off, Card, Countdown, Play, Done };

class Trainer {
public:
    Phase phase = Phase::Off;
    int lesson = -1;
    std::vector<Target> targets;
    int successes = 0, attempts = 0, throws = 0;
    float clock = 0;              // the lesson's time from GO
    Result result;
    std::vector<CueEvent> cues;   // this step's (sounds, flashes)
    std::string line;             // the coach's word now ("TAKE YOUR BALL")

    const Lesson& L() const { return lessons()[lesson]; }
    // the lesson set up (its card), the coach showing it; `saved` keeps the rules the game had, for stop()
    void begin(Game& g, int lessonIndex, const std::vector<CollisionTri>& classic);
    void start(Game& g);          // 3-2-1, then the clock runs
    void stop(Game& g);           // out of training: the game's rules as they were
    void step(Game& g, float dt, PlayerInput* in);   // steps g (and the coach's game while it shows the lesson)
    bool active() const { return phase != Phase::Off; }
    const Tunables& savedRules() const { return saved_; }   // the game's rules before the lesson (what is saved)
    bool coachThere() const;      // the coach at the far end of the real game (drawn from it)

private:
    Tunables saved_;
    bool hadSaved_ = false;
    Flight flight_[2];            // [0] your ball's throw, [1] the coach's ball once you blocked it (SEND IT BACK)
    int serve_ = 0;               // DEFEND: throws made
    bool serving_ = false;        // one on its way
    float serveT_ = 0, waitT_ = 0, rechargeT_ = -1;
    bool caughtArmed_ = false;    // CATCH: your ball is coming back from the far wall
    float respawnT_ = -1;         // DEFEND: the coach's ball you blocked flies on a moment before its next one
    // the coach's game
    void setRules(Game& g, bool coachAtFar) const;
    void stepPlay(Game& g, float dt);
    void finish(bool passed);
    Vec3 coachRelease(const Game& g, int p) const;
};

}  // namespace training
