// Sparc gameplay simulation (platform independent; unit-tested on desktop by tests/test_game.cpp).
#pragma once
#include <cstdint>
#include <vector>
#include "math3d.h"

// collision tag bits (match tools/build_open_assets.py)
enum : uint32_t { TAG_ARENA = 1, TAG_RETURN = 2, TAG_FOUL = 4, TAG_TEAM0 = 8, TAG_TEAM1 = 16 };

struct CollisionTri { Vec3 a, b, c, n; uint32_t tags; };

struct Tunables {
    // ---- rule set. Basic = BeginnerBrawlActivityBP (the activity spawned by l_courtside_00); Advanced = BrawlActivityBP.
    //   Basic:    3 minute match, sudden death overtime on a tie, permanent knuckle guard deflectors,
    //             shield breaks on hit, recharge by throwing, aim assist medium
    //   Advanced: best of 3 rounds, each round first to 4 points, no time limit, shield breaks on hit,
    //             recharge by throwing, aim assist low
    bool advanced = false;
    float basicMatchTime = 180.0f;
    int advancedPointsToWin = 4, advancedRoundsToWin = 2;

    // ---- arena / layout
    Vec3 start[2] = {{-7.0f, 0, 0}, {7.0f, 0, 0}};           // Start_Player0/1 (UE x = -700/+700 cm)
    Vec3 spawner[2] = {{-6.48f, 0, 0}, {6.48f, 0.02f, 0}};  // ProjectileSpawner_Locator_Player0/1
    float foulLineX = 4.73f;                                  // FoulLine_Player0/1 at x = -/+473 cm
    float spawnerIdleHeight = 1.15f;                          // BrawlProjectIleSpawnerBP.IdleTransform z=115 cm
    float springK = 25.0f, springDamping = 16.0f;             // DampedSpring SpringConstant / DampingConstant
    float respawnDelay = 0.1f, postSpawnDelay = 0.1f;         // BrawlProjectIleSpawnerBP RespawnDelay/PostSpawnDelay

    // ---- throw
    float throwMinHandSpeed = 0.50f;          // ThrowHandler.MinSpeedThreshold = 50 cm/s
    float throwVelocityWindow = 0.15f;        // ThrowHandler.AverageVelocityWindowSizeSeconds
    // Launch speed
    //   t = clamp((hand - minInputSpeed) / (maxInputSpeed - minInputSpeed), 0, 1)
    //   speed = lerp(minProjectileThrownSpeed, maxProjectileThrownSpeed + strikes * SpeedIncreasePerStrike, t)
    float minInputSpeed = 0.0f, maxInputSpeed = 6.50f;        // Brawl tuning 0 / 650 cm/s (C++ defaults 150 / 800)
    float projectileMinSpeed = 0.0f, projectileMaxSpeed = 14.0f;
    float speedIncreasePerStrike = 0.5f;      // UProjectileSpeedParams.SpeedIncreasePerStrike 50 cm/s
    float scaleIncreasePerStrike = 0.25f;     // UProjectileLaunchParams.ScaleIncreasePerStrike
    int maxStrikes = 42;                      // MatchTuningData.MaxStrikes
    // Ball size per strike: the recovered +0.25 per strike for the first 4, then +0.05 per strike up to
    // maxBallScale (PORT: 0.25 x 42 would be an 11x ball). The ball grows smoothly into each new step.
    int fullScaleSteps = 4;
    float lateScaleIncreasePerStrike = 0.05f, maxBallScale = 3.0f, scaleGrowRate = 6.0f;
    // Your own ball can hit you once it has travelled away and is coming back (PORT, owner request); the point goes
    // to the opponent. selfHitGrace: seconds after a throw / return start before it can hit its owner.
    bool selfHitScores = true;
    float selfHitGrace = 0.35f;
    int maxBounces = 16;                      // UProjectileLaunchParams.MaxBounces
    // aim assist: Basic uses the BeginnerBrawl tuning ("medium"), Advanced the Brawl tuning ("low")
    float aimCorrectBasicDeg = 3.0f, aimCorrectAdvancedDeg = 1.5f;   // MaxAimCorrectionWithinThrowArcPlane

    // ---- projectile flight
    float elasticity = 0.9f;                  // PhysicsSettings.Elasticity
    bool bouncesChangeSpeed = true;           // PhysicsSettings.bBouncesCanChangeSpeedLimit
    float minSpeed = 1.0f;                    // PhysicsDiskMovementComponent.MinSpeed = 100 cm/s
    float projectileRadius = 0.10f;           // AProjectile ctor: CollisionMesh->SetSphereRadius(10)
    // Spin / Magnus. Field names, the Brawl MaxRotationalSpeed (30) and the presence of the effect are known
    // (FPhysicsDiskSettings: SimulateMagnusEffect, MagnusFactor, SpinBounceFactor, RotationalDecayPerSecond).
    // The integrator's exact formula/default coefficients are not yet recovered -> coefficients.
    float maxSpin = 30.0f;                    // UProjectileSpeedParams.MaxRotationalSpeed (Brawl) taken as rad/s
    float magnusFactor = 0.016f;              // a = magnusFactor * (spin x v)
    float spinWindow = 0.12f;                 // peak controller angular velocity over the last 0.12 s feeds spin
    float spinDecayPerSecond = 0.35f;         // RotationalDecayPerSecond
    float spinBounceFactor = 0.25f;           // SpinBounceFactor
    // Return
    // hitting the OPPONENT's back wall sends the ball back on a straight line aimed at the owner's position at
    // that moment (no further seeking) at ReturnSpeed. It can be caught, can hit the owner (point to the opponent, selfHitScores), or fly past into the owner's back
    // wall, where it dissolves and respawns rising from the floor spawner.
    float returnSpeed = 10.0f;
    // Short throws (owner request, matches Sparc): a throw that has not crossed the owner's foul line and is slow
    // (below recallSpeed), heading back toward the owner, or still short after recallTime goes back to the spawner
    // (the idle spring pulls it home) instead of rattling round the owner's end.
    float recallSpeed = 2.0f, recallTime = 2.5f;
    // Respawn the Pre_Spawn rings play (~1.5 s), then the
    // ball appears at LauncherTransform (15 cm) and the DampedSpring lifts it to IdleTransform (115 cm).
    // PORT tuning (owner): shorter hidden phase, slower and smoother rise with its own tracers + air-pressure sound.
    float spawnPreTime = 0.7f, spawnRiseTime = 0.9f, spawnLauncherHeight = 0.15f;
    // While hidden, the ball sits under the floor at y = -0.6 .. -0.3 (pre-spawn progress), so clients can
    // animate the spawner from the replicated position alone.
    float spawnDepth = 0.30f;

    // ---- hands
    // Offsets in OpenXR grip-local space (+X right, +Y up, -Z forward); UE (x,y,z) cm -> grip (y, z, -x)/100.
    float catchRadius = 0.125f, catchHalfHeight = 0.25f;   // BrawlCatcher.Collision capsule 12.5 / 25 cm
    Vec3 catcherOffset = {-0.01f, 0.0f, -0.22f};           // CatcherTransform (22,-1,0) cm
    Vec3 launcherOffset = {-0.02f, 0.02f, -0.25f};         // LauncherTransform (25,-2,2) cm (throw physics)
    Vec3 holdOffset = {-0.015f, 0.005f, -0.15f};          // where a held ball sits: in the fist (owner: it showed at
                                                          // the shield's tip when held at the launcher point)
    Vec3 shieldOffset = {0.0f, 0.10f, -0.20f};             // ShieldTransform (20,0,10) cm, roll 180
    Vec3 shieldHalfExtents = {0.20f, 0.02f, 0.32f};        // BrawlShieldBP.SweepCollisionBoxDimensions (32,20,2) cm
    // Knuckle bumper = box fitted to the KnuckleShield_01 mesh as placed by KnuckleGuardTransform (10.5,0,0) +
    // BrawlKnuckleGuardVisual_BP.TempMesh (-9,0,0) right-hand grip-space centre / half extents below
    // (x is mirrored for the left hand). The face normal is grip -Z (the front of the fist).
    Vec3 knuckleOffset = {-0.022f, 0.010f, -0.137f};
    Vec3 knuckleHalf = {0.064f, 0.048f, 0.029f};
    float knuckleRadius = 0.07f;                           // used for glow rendering only
    // Deflections treat the guard / shield as a moving surface: v_out = reflect(v_in - v_hand) * restitution + v_hand,
    // so a still fist returns the ball and a punch adds the fist's speed
    float deflectRestitution = 0.9f;
    float handVelWindow = 0.045f;                          // short window for punch velocity
    float shieldAimAssistBasicDeg = 8.0f, shieldAimAssistAdvancedDeg = 2.0f;   // ShieldDeflectionAimAssistDegrees
    float knuckleAimAssistDeg = 8.0f;                      // BeginnerBrawl KnuckleGuardsAimAssistDegrees
    // strike zone a 70 cm ring 0.6 m behind the player, 1.28 m up, facing the court.
    float strikeZoneRadius = 0.70f, strikeZoneBack = 0.60f, strikeZoneHeight = 1.28f;
    // shot clock runs while your ball is in your end, resets when you throw past your foul line;
    // once expired the opponent gets an automatic strike each time they throw successfully. Duration.
    bool useShotClock = true;                 // UProjectileLaunchParams.bUseShotClock default true
    float shotClockSeconds = 10.0f;
    // shield placement on the hand (cycled live for on-device tuning; 0 = BrawlLancerBP ShieldTransform mapping)
    int shieldPlacement = 1;   // user report on Quest 3: placement 0 (above) is on the wrong side of the gauntlet

    // ---- match flow
    float countdown = 3.0f;
    // Match start between two people (owner, 2026-10-16, as in Sparc): both join in the centre of the court; a fist
    // bump with the triggers pulled sends them to their ends at once, or the countdown does it after meetTime.
    // 0 = no meeting (the practice bot never meets).
    float meetTime = 10.0f;
    float meetSpot = 0.45f;          // each player stands this far from the centre line (heads ~0.9 m apart)
    // a fist = the segment from the back of the hand to the knuckles you see (grip frame; the visible fist front is 6-7
    // cm in front of the grip). Two closed fists whose segments come closer than fistBumpGap = a bump. (Measuring the
    // grips, then Sparc's knuckle-glass point 13.7 cm out, both missed real bumps on 2026-10-17.)
    Vec3 fistBack = {0.0f, 0.0f, 0.03f}, fistFront = {0.0f, 0.0f, -0.065f};
    float fistBumpGap = 0.11f;
    float bumpPause = 0.8f;          // after the bump: the confirmation (buzz, sound, FIST BUMP!) before the move
    float roundSummaryTime = 4.0f;

    // ---- body hit volumes; torso offset below the head
    float headRadius = 0.12f, torsoRadius = 0.165f, torsoTop = 0.25f, torsoBottom = 0.85f;

    // ---- practice bot
    float botHoldMin = 1.2f, botHoldMax = 3.0f;
    float botBlockChance = 0.6f, botReaction = 0.22f, botAimError = 0.35f;
    float botHandSpeed = 4.8f;   // simulated hand speed fed through the real launch-speed mapping
};

struct HandInput {
    Pose grip;          // controller grip pose in world (app) space
    Vec3 angVel;        // controller angular velocity in world space (rad/s), from XrSpaceVelocity
    bool tracked = false;
    float trigger = 0;
    float squeeze = 0;
};

// Avatar look indices into the option tables in avatar.cpp. Travels with every input so the
// opponent and spectators see it (16 bytes, sent raw).
struct AvatarDesc {
    uint8_t suit = 0, build = 1, hair = 2, hairColor = 1, headgear = 0, visor = 1, visorTint = 0, mask = 0;
    uint8_t pattern = 1, primary = 0, secondary = 1, glow = 0, skin = 2, lower = 0, body = 0, gauntlet = 0;   // lower: shorts (0 = waist up); body: 0 masc, 1 fem
};
static_assert(sizeof(AvatarDesc) == 16, "AvatarDesc is sent raw");

struct PlayerInput {
    Pose head;
    bool headTracked = false;
    HandInput hand[2];   // 0 = left, 1 = right
    bool buttonA = false;   // restart / ready
    AvatarDesc avatar;   // cosmetic only; the simulation ignores it
};

enum class BallState : uint8_t { Spawning, Idle, Held, Flying, Dead, Returning };
// Meet (appended, so older builds keep their numbers): two players start a match face to face in the centre
enum class MatchState : uint8_t { WaitingForPlayers, Countdown, InProgress, RoundSummary, MatchSummary, Meet };
enum class EventType : uint8_t { Spawn, Grab, Throw, ThrowFail, Bounce, ShieldBlock, ShieldBreak, HitPlayer,
                                 Returned, Caught, Foul, RoundStart, RoundEnd, MatchEnd, Countdown,
                                 KnuckleBlock, Recharge, Strike, Overtime, SpawnRise, FistBump };

// the event's name (logs, sound mapping). One table for everything: two private copies once fell behind the enum
// and a new event (FistBump) read past their end and crashed both headsets. Unknown values give "?".
const char* eventName(EventType t);

struct GameEvent {
    EventType type;
    int player;     // owning / affected player
    int hand;       // hand index where relevant (-1 otherwise)
    Vec3 pos;
    float value;
};

struct Ball {
    BallState state = BallState::Spawning;
    Vec3 pos, vel, spin;        // spin: angular velocity (rad/s, world)

    float speedLimit = 0;
    float timer = 0;
    int heldHand = -1;
    int instigator = -1;        // who threw / last deflected it (decides scoring and zone rules)
    bool charged = true;        // powers the holder's shield; drained by a block, recharged past the foul line
    bool scored = false;
    bool rechargePending = false;
    int bounces = 0;
    float scale = 1.f;          // grows with strikes against the opponent
};

struct HandState {
    Vec3 hist[64];
    Vec3 angHist[64];
    Pose gripHist[64];
    float histT[64];
    int histN = 0, histHead = 0;
    bool shieldUp = false;
    bool knuckleUp = false;
    float prevTrigger = 0;
};

struct PlayerState {
    PlayerInput in;
    HandState hands[2];
    Ball ball;
    int score = 0;          // points (this round in Advanced)
    int roundWins = 0;
    int strikesAgainst = 0; // strikes the opponent holds against this player
    float shotClock = 0;    // seconds remaining on this player's shot clock
    bool isBot = false;
    // bot
    float botTimer = 0;
    float botNoticeT = -1;
    bool botWillBlock = false;
    int botBlockHand = 0;
    Vec3 botLean;               // the bot's body leaning / dipping toward a far block (smoothed)
    Vec3 botHand[2];            // the bot's hands, moving toward their targets (no teleporting)
    bool botHandSet = false;
};

class Game {
public:
    Tunables tun;
    PlayerState pl[2];
    MatchState match = MatchState::WaitingForPlayers;
    float stateTimer = 0;
    bool meetBumped = false;    // host: the fist bump happened; the short pause before both go to their ends
    float roundClock = 0;       // Basic: match clock; Advanced: unused (no time limit)
    bool overtime = false;
    int round = 0;
    int lastRoundWinner = -1, matchWinner = -1;
    std::vector<CollisionTri> arena;
    std::vector<GameEvent> events;

    void setArena(const std::vector<CollisionTri>& tris) { arena = tris; }
    void reset(bool player1IsBot);
    // Advance the simulation. `in` is overwritten for bot players.
    void step(float dt, PlayerInput in[2]);

    // helpers exposed for rendering / tests
    Vec3 launcherPoint(int p, int h) const;
    Vec3 catcherPoint(int p, int h) const;
    Vec3 knucklePoint(int p, int h) const;
    Pose shieldPose(int p, int h) const;
    float fistGap(int p0, int h0, int p1, int h1) const;   // closest distance between two fists (segments), metres
    Pose shieldLocal(int h) const;           // plate pose in grip space (plate normal = local +Y)
    Vec3 strikeZoneCenter(int p) const;
    Vec3 peakAngVel(int p, int h, float now) const;
    Vec3 recentHandVel(int p, int h, const Vec3& localPoint, float now) const;   // velocity of a grip-local point
    Pose knucklePose(int p, int h) const;
    Vec3 idlePoint(int p) const { return tun.spawner[p] + Vec3{0, tun.spawnerIdleHeight, 0}; }
    Vec3 chestPoint(int p) const;
    float throwSpeedFromHandSpeed(float handSpeed, int thrower) const;
    void addStrike(int againstPlayer, const Vec3& where);   // public for tests
    Vec3 handVelocity(int p, int h, float now) const;
    float ballRadius(int p) const { return tun.projectileRadius * pl[p].ball.scale; }
    float strikeScale(int strikes) const;
    float now = 0;

private:
    void stepMatch(float dt);
    void startCountdown();   // round 1 of a new match: the 3-2-1
    float lastFistLog = 0;
public:
    bool meetLog = false;       // a meeting diagnostic is ready (main.cpp logs it): closest fist gap, triggers
    float meetGap = 0, meetTrig[2] = {0, 0};
private:
    void stepPlayer(int p, float dt);
    void stepBall(int p, float dt);
    void stepBot(int p, float dt);
    void launch(int p, int h, Vec3 dir, float speed, Vec3 spin);
    void spawnBall(int p);
    void deflect(Ball& b, const Vec3& n, int deflector, float assistDeg, const Vec3& from);
    bool sweepArena(const Vec3& from, const Vec3& to, float r, Vec3& hitPos, Vec3& hitN, uint32_t& tags) const;
    bool hitsBody(int victim, const Vec3& pos, float r) const;
    bool hitsShield(int p, int h, const Vec3& from, const Vec3& to, float r, Vec3& n) const;
    bool hitsBox(const Pose& box, const Vec3& half, const Vec3& from, const Vec3& to, float r, Vec3& n) const;
    void deflectMoving(Ball& b, const Vec3& n, const Vec3& surfaceVel, int deflector, float assistDeg, const Vec3& from,
                       bool punch = false);
    void scorePoint(int scorer, const Vec3& where);
    void emit(EventType t, int p, int h, const Vec3& pos, float v = 0) { events.push_back({t, p, h, pos, v}); }
    float rnd();
    uint32_t rng = 0x12345u;
};
