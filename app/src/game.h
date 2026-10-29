// Sparc gameplay simulation (platform independent; unit-tested on desktop by tests/test_game.cpp).
#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include "math3d.h"

// collision tag bits (match tools/build_open_assets.py)
enum : uint32_t { TAG_ARENA = 1, TAG_RETURN = 2, TAG_FOUL = 4, TAG_TEAM0 = 8, TAG_TEAM1 = 16 };

struct CollisionTri {
    Vec3 a, b, c, n; uint32_t tags;
    Vec3 mid; float rad = -1.f;   // a sphere around it (Game::setArena): balls far from it skip the exact test
};
// The arena's collision as the game uses it (Game::setArena), shared by every copy of the game (the bot plays its
// tryouts out on copies): the triangles, and a grid of 1 m cells listing the triangles within kPad of each, so a ball
// (its radius at most 0.3 m) tests only those around it.
struct ArenaData {
    static constexpr float kCell = 1.f, kPad = 0.45f;
    std::vector<CollisionTri> tris;
    Vec3 lo;
    int n[3] = {0, 0, 0};
    std::vector<uint32_t> first;   // per cell (x fastest, then y, z): its first entry in `items`; one more at the end
    std::vector<uint16_t> items;   // triangle indices
};
// arena.col (tools/build_open_assets.py): "SCOL", the triangle count (u32), then per triangle 9 floats and tags (u32).
// Empty when the data isn't one (wrong magic, cut short).
std::vector<CollisionTri> parseCollision(const uint8_t* data, size_t size);
// The EXPERIMENTAL arenas (owner, 2026-10-29), Basic's rules in both:
// DIAMOND (the owner's drawing, the default): the classic court rolled 45 degrees along its length, as Sparc's 2017
// reviews describe a mode ("turns the long rectangular arena 45 degrees, making it more of a diamond shape"; Sparc's
// files we have hold no such room). 19 m long; its end a square as wide as the court (4.96 m, from arena.col), turned
// so its corners are straight up, down and to each side, 3.5 m from its centre (owner, 2026-10-29: Sparc's was squared
// off, every side as far as the court is wide; the court's own 4.96 x 3.88 m end, rolled, left one side 0.8 m short).
// The end's centre 1.2 m over the platforms' tops (y = 0): no flat floor, the two lower walls meet in a V under your
// feet. The players on round floating platforms at their usual start, which the ball goes through (it comes up at you
// from below; tun.solidPlatforms makes them solid); the ends are the back walls. No foul zone (owner, 2026-10-29): the
// foul line still recharges your ball and resets your shot clock, and a short throw still comes home.
// CORNERS: a square room turned 45 degrees, 7 m tall, its corners 9.5 m out from
// the centre (x = -/+9.5 behind the players, z = -/+9.5 to the sides); the players on round floating platforms
// (2.4 m across, their tops at y = 0) at their usual start, the room's floor 3 m below them and its ceiling 4 m above.
// The ball bounces off everything, the floor below included (it can come up at you); your platform is your foul
// zone; the corner walls behind each player (x beyond -/+7.1) are its back wall (TAG_RETURN).
namespace exparena {
constexpr float kCorner = 9.5f, kFloor = -3.f, kCeiling = 4.f, kPlatformR = 1.2f, kPlatformThick = 0.15f, kBackX = 7.1f;
constexpr float kFoulLineX = 5.8f;   // the platform's front edge: past it a throw recharges the ball
std::vector<CollisionTri> collision();   // CORNERS
constexpr float kRollLen = 9.5f, kRollW = 4.96f, kRollH = 4.96f, kRollCY = 1.2f;   // DIAMOND: ends at x = -/+9.5; the end (square); its centre height
constexpr float kRollReach = (0.5f * kRollW + 0.5f * kRollH) * 0.70710678f;   // the end's corners: this far up, down and to the sides
void diamondEnd(Vec3 v[4]);   // the end's corners (x = 0), round in order: the classic end rolled 45 degrees
std::vector<CollisionTri> diamondCollision(bool solidPlatforms = false);
}
enum : int { ARENA_CLASSIC, ARENA_CORNERS, ARENA_DIAMOND };

struct Tunables {
    // ---- rule set. Basic = BeginnerBrawlActivityBP (the activity spawned by l_courtside_00); Advanced = BrawlActivityBP.
    //   Basic:    3 minute match, sudden death overtime on a tie, permanent knuckle guard deflectors,
    //             shield breaks on hit, recharge by throwing, aim assist medium
    //   Advanced: best of 3 rounds, each round first to 4 points, no time limit, shield breaks on hit,
    //             recharge by throwing, aim assist low
    bool advanced = false;
    bool experimental = false;   // the rules ask for an EXPERIMENTAL arena (Basic's rules there); Game::useArena loads it
    int expRoom = 0;             // which: 0 DIAMOND, 1 CORNERS
    bool solidPlatforms = false; // DIAMOND: the ball bounces off the platforms (default: through them, owner 2026-10-29)
    float basicMatchTime = 180.0f;
    int advancedPointsToWin = 4, advancedRoundsToWin = 2;
    // ---- MATCH OPTIONS (the host's, 0.3.8; owner's design 2026-10-21). shields false = HOTDOG MODE (the community's
    // name for playing without shields). knuckleMode -1 = as the rules say (Basic on, Advanced off), 0 off, 1 on.
    // basicMatchTime 0 = FREE PLAY: no clock and no winner, play on. advancedRoundTime 0 = no limit; otherwise each
    // round ends at time: more points wins the round, a tie goes to sudden death (the next point wins it).
    bool shields = true;
    int knuckleMode = -1;
    float advancedRoundTime = 0.0f;
    bool knuckleGuards() const { return knuckleMode < 0 ? !advanced : knuckleMode != 0; }
    bool freePlay() const { return !advanced && basicMatchTime <= 0.f; }
    bool timed() const { return advanced ? advancedRoundTime > 0.f : basicMatchTime > 0.f; }
    // statistics are kept by mode: 0 standard (the rules as they come), 1 HOTDOG (no shields, any other options),
    // 2 custom (other options)
    int statsMode() const {
        if (!shields) return 1;
        // (knuckle guards count by what is in play: "off" chosen in Basic and then Advanced picked is Advanced's own rule)
        const bool std_ = knuckleGuards() == !advanced && (advanced ? advancedRoundTime <= 0.f : basicMatchTime == 180.0f);
        return std_ ? 0 : 2;
    }

    // ---- arena / layout
    Vec3 start[2] = {{-7.0f, 0, 0}, {7.0f, 0, 0}};           // Start_Player0/1 (UE x = -700/+700 cm)
    Vec3 spawner[2] = {{-6.48f, 0, 0}, {6.48f, 0.02f, 0}};  // ProjectileSpawner_Locator_Player0/1
    float foulLineX = 4.73f;                                  // FoulLine_Player0/1 at x = -/+473 cm
    // a ball outside this box has left the arena (respawns): below outLow, above outHigh, |x| > outX, |z| > outZ
    float outLow = -1.f, outHigh = 6.f, outX = 11.f, outZ = 4.f;
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
    // to the opponent. selfHitGrace: seconds after a throw / return start / block (owner, 2026-10-27: a ball just blocked
    // could count against the blocker the moment it left the shield) before it can hit its owner.
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
    float foulReturnSpeed = 16.0f;   // a fouled ball: to the wall and home again (owner, 2026-10-21: faster)
    // Short throws (owner request, matches Sparc): a throw that has not crossed the owner's foul line and is slow
    // (below recallSpeed), heading back toward the owner, or still short after recallTime goes back to the spawner
    // (the idle spring pulls it home) instead of rattling round the owner's end.
    float recallSpeed = 2.0f, recallTime = 2.5f;
    // Stuck balls (players and owner, 2026-10-28; kept narrow, so play feels as in Sparc): a flying ball that goes
    // less than stuckDistance along the court in stuckTime (looping in a corner, between floor and ceiling), or that
    // its bounces have slowed under recallSpeed anywhere (after slowGrace, so a soft block still drops nearby), goes
    // home, harmless, as a short throw does. A fouled ball keeps its own speed (at least foulMinSpeed) to its wall.
    float stuckDistance = 1.0f, stuckTime = 2.0f, slowGrace = 1.0f, foulMinSpeed = 5.0f;
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
    Vec3 holdOffset = {-0.079f, 0.011f, -0.010f};         // where a held ball sits: in the fist (owner's TUNING export,
                                                          // 2026-10-19; was 15 cm out, before that the launcher point)
    Vec3 shieldOffset = {0.0f, 0.10f, -0.20f};             // ShieldTransform (20,0,10) cm, roll 180
    Vec3 shieldHalfExtents = {0.20f, 0.02f, 0.32f};        // BrawlShieldBP.SweepCollisionBoxDimensions (32,20,2) cm
    // Knuckle bumper = box fitted to the KnuckleShield_01 mesh as placed by KnuckleGuardTransform (10.5,0,0) +
    // BrawlKnuckleGuardVisual_BP.TempMesh (-9,0,0) right-hand grip-space centre / half extents below
    // (x is mirrored for the left hand). The face normal is grip -Z (the front of the fist). Sparc's centre was
    // (-0.022, 0.010, -0.137); the owner moved the glass onto the fist with the TUNING tool (2026-10-18), and the
    // bumper goes with it (what you see is what hits).
    Vec3 knuckleOffset = {0.0f, -0.026f, -0.057f};
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
    // placement 1 (the default): on the back of the hand, right-hand grip space (left mirrored); pitch / yaw / roll in
    // degrees (gripEuler). The owner's TUNING export (2026-10-18; was (0.07, 0, 0.01), roll -90 only). Live in
    // developer mode: SETTINGS > TUNING.
    Vec3 shieldBackPos = {0.076f, -0.024f, -0.122f}, shieldBackRot = {-20.0f, 19.1f, -90.6f};   // owner's export 2026-10-19

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
    // The meeting point (owner, 2026-10-28): a ring for each player over the centre line, at chest height between the
    // two. A fist closed inside its ring lights it (offered). A bump counts only between two fists in their rings, one
    // of them offered for meetOfferTime first: one person offers, the other meets it; nobody can force the start.
    float meetRingRadius = 0.22f, meetRingDepth = 0.16f, meetRingX = 0.07f, meetOfferTime = 0.2f;
    float bumpPause = 0.8f;          // after the bump: the confirmation (buzz, sound, FIST BUMP!) before the move
    float roundSummaryTime = 4.0f;

    // ---- body hit volumes; torso offset below the head
    float headRadius = 0.12f, torsoRadius = 0.165f, torsoTop = 0.25f, torsoBottom = 0.85f;

    // ---- practice bot its level (botSkill below; practice MATCH OPTIONS), and its body. A person's reach and
    // shoulders, as the avatar draws them: each wrist (grip-local botWrist) stays within botArm of its shoulder (head +
    // botShoulder in the head's frame; x mirrored for the left), so the drawn arm never stretches (tests check it
    // against the avatar rig); its head moves within botBodyRange (sideways, down, toward the court) by stepping,
    // leaning and ducking.
    int botLevel = 2;
    // MATCH MY LEVEL (owner, 2026-10-26): instead of botLevel, a level between 0 (ROOKIE) and 5 (UNBEATABLE) that
    // follows the player: up botMatchStep each time the person scores against the bot, down each time the bot scores
    // (close matches: it settles where both score about as often)
    bool botAdapt = false;
    float botMatch = 2.f, botMatchStep = 0.15f;
    bool botMeet = false;   // practice: meet the bot in the centre for a fist bump first (owner, 2026-10-27)
    Vec3 botShoulder = {0.18f, -0.295f, 0.04f}, botWrist = {0.04f, 0.047f, 0.122f};
    float botArm = 0.52f, botHandAccel = 40.f;   // (0.52: room for the shoulder lifting with a raised arm)
    Vec3 botBodyRange = {1.0f, 0.50f, 0.35f};
};

// The practice bot's levels (owner, 2026-10-26): it always moves like a person (reaction time, hand and body speed,
// arm's reach), and the levels differ in skill within those limits. UNBEATABLE reads every ball's real flight (walls,
// spin) and answers it with what it can reach in time: shield, knuckle guard or dodge; its throws are tried out first.
struct BotSkill {
    const char* name;
    float reaction;             // seconds from seeing a ball to the first move
    float defend;               // chance it does something about a ball coming at it
    bool readsFlight;           // reads curves and bank shots (else it expects a straight line, re-read as it comes)
    float readError;            // metres its read of the ball's path is off
    float handSpeed;            // m/s: its fastest hand move (blocking, reaching, catching)
    float bodySpeed, bodyAccel; // m/s, m/s^2: stepping, leaning and ducking
    float throwHand;            // m/s: the hand speed of its throws (through the real launch-speed mapping)
    float aimError;             // metres
    float holdMin, holdMax;     // seconds it holds the ball before a throw
    float catchChance;          // its own ball coming back
    bool planShots;             // tries out throws (direct, curved, banked) on a copy of the game and picks the best
    bool guardsStrikes;         // also blocks balls heading for its strike zone
};
constexpr int kBotLevels = 6;
// how the bot holds its hands when it has nothing to do (owner, 2026-10-27: more than the one "kitten hands"): one
// picked at random each match (ROOKIE and AMATEUR: only the open ones, KITTEN, RELAXED, MONKEY: a guard blocks by
// itself). KARATE bows at the fist bump.
enum : int { STANCE_KITTEN, STANCE_RELAXED, STANCE_BOXER, STANCE_KARATE, STANCE_MANTIS, STANCE_MONKEY, kBotStances };
const char* botStanceName(int stance);
const BotSkill& botSkill(int level);
BotSkill botSkillAt(float level);   // between two levels: their numbers blended (the name and yes-or-no skills: the nearer)

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
    int ball = -1;  // hits, strikes, blocks: the ball (its owner's index) behind it; -1 otherwise. Not sent: host only
};

// One match's counts for the community statistics (the host keeps them from its events; sent when a match between two
// people ends). THROW ACCURACY is literal (owner, 2026-10-26; GitHub #14, astrobleem): a throw counts on target at most
// once, at its first hit, strike or block. A ball the other player blocked or punched back is their return: a point it
// scores counts in hits and in returnHits, never as a throw on target. Your own ball hitting you, a foul's strike and the
// shot clock's automatic strike are nobody's shot.
struct MatchTally {
    uint16_t throws = 0, onTarget = 0, hits = 0, blocks = 0, faced = 0, catches = 0, strikes = 0, returnHits = 0;
    float topSpeed = 0;
};
struct StatCounter {
    MatchTally s[2];
    void add(const GameEvent& e);   // Throw, HitPlayer, Strike, ShieldBlock, KnuckleBlock, Caught (others: ignored)
private:
    struct Shot { int by = -1; bool thrown = false, counted = false; } shot_[2];   // the attack each ball carries
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
    // a foul (owner, 2026-10-21): the ball no longer dissolves. It bounces on, off the wall it is heading for, then
    // flies home to its launch spot (homing), harmless all the way; fouled = still before that wall
    bool fouled = false, homing = false;
    float progX = 0, progT = 0;   // stuck balls: where along the court (x) it was at progT (its timer), last metre on
};

// one point of a ball's expected flight (Game::predictBall): the step (its game time) whose move brings the ball here
// (the first point: the ball as it is, where the step before left it), where, how fast; strike: it crossed the
// reader's strike zone ring on the way here; the whole ball then (a tryout starts from it)
struct BallPathPt { float t; Vec3 pos, vel; bool strike; Ball ball; };
// the bot's read of a flight. Not copied with the game: the copies (the bot's tryouts) don't need it
struct BallPath : std::vector<BallPathPt> {
    BallPath() = default;
    BallPath(const BallPath&) {}
    BallPath& operator=(const BallPath&) { clear(); return *this; }
};
enum : int { BOT_NONE, BOT_SHIELD, BOT_KNUCKLE, BOT_DODGE, BOT_CATCH };

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
    // bot (Game::stepBot)
    int botLevel = -1;          // -1: tun.botLevel (the lobby's bots choose their own)
    int botStance = -1;         // STANCE_* (-1: picked as its first step of a match; no guard below PRO)
    float botBow = 0;           // 0 upright .. 1 bowed (KARATE, at the fist bump)
    int botBumpHand = 1;        // the hand it bumps with (kept unless their fist is clearly on its other side)
    float botSteady = 0;        // 0 bobbing as its stance does .. 1 still (it steadies its head while it watches a ball)
    float botTimer = 0;         // until its next throw
    Vec3 botBody, botBodyVel;   // its head's offset from where it stands (steps, leans, ducks), and that offset's speed
    Vec3 botHand[2], botHandVel[2];   // its grips, moving at a person's speed
    Quat botHandQ[2];
    bool botHandSet = false;
    // the balls it watches: [0] the opponent's, [1] its own. Each read: the path (Game::predictBall, or a straight
    // line), when it noticed, whether it acts, and its answer (BOT_*)
    struct BotRead {
        int mode = 0;           // 0 nothing to watch, 1 a ball coming at it (an attack), 2 one it threw or deflected coming back
        BallPath path;
        float noticeT = -1, readT = -1;
        bool act = false, catchIt = false;   // it does something about it (block, punch, dodge); it goes for the catch
        Vec3 err;               // its misread (lower levels)
        bool full = false;      // the read reaches its whole horizon (the ball still flying at its end)
        int tuck = -1;          // a dodge with this hand (its shield up) pulled in to its side, out of the ball's way
        bool again = false;     // its plan ran out of tryouts this step: it goes on with it the next
        int hands = -1;         // its shields up when it planned (bits by hand): a change, a new plan
        std::vector<std::pair<uint32_t, int8_t>> tried;   // this read's tryouts so far (which answer, how it went)
        Vec3 rel[2];            // where its hands hold still (from its head) while it watches the ball (as planned)
        Quat relQ[2];
        int plan = 0, hand = -1;
        float hitT = -1;             // when the ball would reach its body (or, missing it, its own hands where they are)
        Vec3 grip, body;             // the grip (block, catch) it moves to; its body offset (where it stands, or its dodge)
        Quat gripQ;
    } botRead[2];
    int botThrow = 0;                      // 0 holding, 1 winding up, 2 swinging
    float botThrowT = 0;
    Vec3 botShotDir, botShotSpin;          // the throw it will make, and how good it looked (planned shots)
    float botShotScore = -1e9f;
    int botShotsTried = 0;
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
    std::shared_ptr<const ArenaData> arena;
    std::vector<GameEvent> events;

    void setArena(const std::vector<CollisionTri>& tris);
    void rngSeed(uint32_t seed) { rng = seed ? seed : 1u; }   // (the bot's chances; tests vary them)
    void reset(bool player1IsBot);
    // Advance the simulation. `in` is overwritten for bot players.
    void step(float dt, PlayerInput in[2]);

    // helpers exposed for rendering / tests
    Vec3 launcherPoint(int p, int h) const;
    Vec3 catcherPoint(int p, int h) const;
    Vec3 knucklePoint(int p, int h) const;
    Pose shieldPose(int p, int h) const;
    float fistGap(int p0, int h0, int p1, int h1) const;   // closest distance between two fists (segments), metres
    // the arena to play in: the classic court's collision (kept for switching back) or the EXPERIMENTAL one; sets the
    // foul line and the out-of-arena box with it
    void useArena(int kind, const std::vector<CollisionTri>& classic);   // ARENA_*
    int arenaKind = -1;   // the collision in use: ARENA_* (-1: set some other way, e.g. tests' setArena)
    Vec3 meetRing(int p) const;           // the centre of p's ring at the meeting point
    bool inMeetRing(int p, int h) const;  // that fist's knuckles are inside p's ring
    bool meetLit(int p, int h) const;     // ... and closed: its ring lights up (offered)
    float meetLitT[2] = {-1, -1};         // host: since when each player has had a fist lit in its ring (-1: not)
    Pose shieldLocal(int h) const;           // plate pose in grip space (plate normal = local +Y)
    Vec3 strikeZoneCenter(int p) const;
    Vec3 peakAngVel(int p, int h, float now) const;
    Vec3 recentHandVel(int p, int h, const Vec3& localPoint, float now) const;   // velocity of a grip-local point
    Pose knucklePose(int p, int h) const;
    Vec3 idlePoint(int p) const { return tun.spawner[p] + Vec3{0, tun.spawnerIdleHeight, 0}; }
    Vec3 chestPoint(int p) const;
    float throwSpeedFromHandSpeed(float handSpeed, int thrower) const;
    void addStrike(int againstPlayer, const Vec3& where, int ball = -1);   // public for tests; ball: whose ball did it
    Vec3 handVelocity(int p, int h, float now) const;
    float ballRadius(int p) const { return tun.projectileRadius * pl[p].ball.scale; }
    float strikeScale(int strikes) const;
    // the flight of `owner`'s ball from now (up to `horizon` seconds, in steps of dt: the game's own, so it matches the
    // real flight step for step) as the game would play it if nothing of player `reader` touched it (its body and hands
    // out of the way); stops when the ball stops flying or someone else deflects it. A copy of the game runs it: a few
    // times a throw, not every frame.
    void predictBall(int owner, int reader, float horizon, float dt, std::vector<BallPathPt>& out) const;
    // the bot's body: where its shoulder is and where a grip puts its wrist (within tun.botArm of the shoulder), and
    // whether a ball of radius r at pos would touch a body whose head is at `head` (as hitsBody)
    Vec3 botShoulder(int p, int h) const;
    Vec3 botWrist(int h, const Pose& grip) const;
    Vec3 botInReach(int p, int h, const Pose& grip) const;   // the grip's position, moved in if the arm can't reach it
    Pose botTuck(int p, int h, const Vec3& head) const;       // hand h pulled in to its side (a shield out of the way)
    bool bodyTouches(const Vec3& head, const Vec3& pos, float r) const;
    bool bodyTouchesPath(const Vec3& head, const Vec3& from, const Vec3& to, float r) const;   // anywhere from `from` to `to`
    float now = 0;

private:
    void stepMatch(float dt);
    void startCountdown();   // round 1 of a new match: the 3-2-1
    void endRound(int winner);   // Advanced: a round won (by points, at time, or in sudden death)
    float lastFistLog = 0;
public:
    bool meetLog = false;       // a meeting diagnostic is ready (main.cpp logs it): closest fist gap, triggers
    float meetGap = 0, meetTrig[2] = {0, 0};
private:
    void stepPlayer(int p, float dt);
    void stepBall(int p, float dt);
    void stepBot(int p, float dt);
    void botWatch(int p, int k, const BotSkill& sk);           // keeps botRead[k] up to date and plans the answer
    void botPlan(int p, int k, const BotSkill& sk);
    bool botReachable(int p, int h, const Pose& grip, float by) const;   // in reach, and the hand gets there by game time `by`
    // a plan for read k (BOT_*: hand h at `grip`, its body at `body`, hand `tuck` pulled in) tried on a copy of the game
    // while `owner`'s ball flies: the copy's own stepBot carries it out (botFrozen: no new reads or plans there), so it
    // moves exactly as the bot will. 2 nothing comes of it, 1 a strike against it (a block into its own foul zone, or
    // its strike zone), 0 it is hit; -1 not known yet (no tryouts left this step)
    int botTry(int p, int k, int owner, int plan, int h, const Pose& grip, const Vec3& body, int tuck) const;
    bool botFrozen = false;
    float lastDt = 1.f / 72;    // the last step's length (the bot's reads run in the same steps)
    // the bot's tryouts left this step (six at most: the rest of a plan waits for the next step), and whether a plan
    // wanted more
    static constexpr int kBotTriesPerStep = 6;
    mutable int botTriesLeft = 0;
    mutable bool botTriesOut = false;
    void botPlanShot(int p, const BotSkill& sk);               // tries one more throw (planned shots)
    BotSkill botSkillOf(int p) const;
    Pose botBlockGrip(int p, int h, int plan, const BallPathPt& at) const;   // the grip that puts the shield / guard / catcher on the ball
    void launch(int p, int h, Vec3 dir, float speed, Vec3 spin);
    void spawnBall(int p);
    void deflect(Ball& b, const Vec3& n, int deflector, float assistDeg, const Vec3& from);
    bool sweepArena(const Vec3& from, const Vec3& to, float r, Vec3& hitPos, Vec3& hitN, uint32_t& tags) const;
    bool hitsBody(int victim, const Vec3& pos, float r) const;
    bool hitsShield(int p, int h, const Vec3& from, const Vec3& to, float r, Vec3& n) const;
    bool hitsBox(const Pose& box, const Vec3& half, const Vec3& from, const Vec3& to, float r, Vec3& n) const;
    void deflectMoving(Ball& b, const Vec3& n, const Vec3& surfaceVel, int deflector, float assistDeg, const Vec3& from,
                       bool punch = false);
    void scorePoint(int scorer, const Vec3& where, int ball);
    void emit(EventType t, int p, int h, const Vec3& pos, float v = 0, int ball = -1) {
        events.push_back({t, p, h, pos, v, ball});
    }
    float rnd();
    uint32_t rng = 0x12345u;
};
