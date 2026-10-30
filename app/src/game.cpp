// Sparc gameplay simulation. See game.h for provenance conventions; rules quoted from the original's
// English text are in docs/game_text_en.txt.
#include "game.h"
#include <algorithm>
#include <cstring>

static const Vec3 kUp{0, 1, 0};

float Game::rnd() {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return (rng & 0xFFFFFF) / float(0x1000000);
}

Vec3 Game::launcherPoint(int p, int h) const { return pl[p].in.hand[h].grip.transform(tun.launcherOffset); }
Vec3 Game::catcherPoint(int p, int h) const { return pl[p].in.hand[h].grip.transform(tun.catcherOffset); }
Vec3 Game::knucklePoint(int p, int h) const { return knucklePose(p, h).p; }
Pose Game::knucklePose(int p, int h) const {
    Vec3 o = tun.knuckleOffset;
    if (h == 0) o.x = -o.x;   // left hand mirror (HandnessRoot)
    const Pose& g = pl[p].in.hand[h].grip;
    return {g.q, g.transform(o)};
}
Vec3 Game::recentHandVel(int p, int h, const Vec3& local, float t) const {
    // velocity of a grip-local point over a short window, from the recorded grip poses
    const HandState& hs = pl[p].hands[h];
    if (hs.histN < 2) return {};
    int newest = (hs.histHead + 63) & 63, oldest = newest;
    for (int i = 1; i < hs.histN; ++i) {
        int k = (newest - i + 64) & 63;
        oldest = k;
        if (t - hs.histT[k] >= tun.handVelWindow) break;
    }
    float dt = hs.histT[newest] - hs.histT[oldest];
    if (dt <= 1e-4f) return {};
    return (hs.gripHist[newest].transform(local) - hs.gripHist[oldest].transform(local)) / dt;
}
bool Game::hitsBox(const Pose& box, const Vec3& half, const Vec3& from, const Vec3& to, float r, Vec3& n) const {
    Quat inv = box.q.conj();
    Vec3 a = inv.rotate(from - box.p), b = inv.rotate(to - box.p);
    Vec3 e = half + Vec3{r, r, r};
    float t0 = 0, t1 = 1;
    Vec3 d = b - a;
    const float* pa = &a.x; const float* pd = &d.x; const float* pe = &e.x;
    int axis = -1;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(pd[i]) < 1e-8f) { if (pa[i] < -pe[i] || pa[i] > pe[i]) return false; continue; }
        float ta = (-pe[i] - pa[i]) / pd[i], tb = (pe[i] - pa[i]) / pd[i];
        if (ta > tb) std::swap(ta, tb);
        if (ta > t0) { t0 = ta; axis = i; }
        t1 = std::min(t1, tb);
        if (t0 > t1) return false;
    }
    if (axis < 0) {   // already overlapping at segment start: push out along the least-penetrated face
        float best = 1e9f;
        for (int i = 0; i < 3; ++i) { float pen = pe[i] - std::fabs(pa[i]); if (pen < best) { best = pen; axis = i; } }
    }
    Vec3 ln{};
    (&ln.x)[axis] = (pa[axis] + pd[axis] * t0) >= 0 ? 1.f : -1.f;
    n = box.q.rotate(ln);
    if (dot(n, to - from) > 0) n = -n;
    return true;
}
void Game::deflectMoving(Ball& b, const Vec3& n, const Vec3& vs, int deflector, float assistDeg, const Vec3& from,
                         bool punch) {
    Vec3 rel = b.vel - vs;
    float vn = dot(rel, n);
    if (vn < 0) rel = rel - n * ((1.f + tun.deflectRestitution) * vn);
    Vec3 out = rel + vs;
    if (punch) {
        // a knuckle-guard hit is a punch: follow the fist's motion and always send the ball toward the opponent's end
        float hs = length(vs);
        if (hs > 1.5f) out = normalize(lerp(normalize(out), vs / hs, 0.5f)) * length(out);
        // (toward: the deflector's facing; two players: +X for player 0, -X for player 1, as it always was)
        const Vec3 toward = facing(deflector);
        float sp0 = length(out);
        Vec3 d = sp0 > 1e-4f ? out / sp0 : toward;
        const float along = dot(d, toward);
        if (along < 0.35f) { d = d - toward * along + toward * 0.35f; d = normalize(d); }
        out = d * sp0;
    }
    float cap = tun.projectileMaxSpeed + tun.speedIncreasePerStrike * strikesHeld(deflector);
    float sp = clampf(length(out), tun.minSpeed, cap);
    Vec3 dir = normalize(out), tgt = assistDir(deflector, from, normalize(out));
    float ang = std::acos(clampf(dot(dir, tgt), -1.f, 1.f));
    if (ang > 1e-4f) dir = normalize(lerp(dir, tgt, std::min(1.f, deg2rad(assistDeg) / ang)));
    b.vel = dir * sp;
    b.speedLimit = sp;
    b.instigator = deflector;
    b.rechargePending = false;
    b.scored = false;
    b.spin *= 0.5f;
    b.timer = 0;   // (the self-hit grace, after a block as after a throw)
    b.progX = b.pos.x; b.progZ = b.pos.z; b.progT = 0;
}
Pose Game::shieldLocal(int h) const {
    // Candidate placements (grip space: +X right, +Y up/thumb, -Z forward). side = +1 right hand, -1 left hand.
    float side = h == 1 ? 1.f : -1.f;
    switch (tun.shieldPlacement & 3) {
    default:
    case 0: return {Quat{}, tun.shieldOffset};                                                // above, ahead (ShieldTransform)
    // on the back of the hand, standing out from the outside of the arm and running back along the forearm (plate
    // normal = grip X, long side along the forearm), as in the owner's reference from the original (2026-10-17)
    case 1: return {gripEuler(tun.shieldBackRot, h == 0), {side * tun.shieldBackPos.x, tun.shieldBackPos.y, tun.shieldBackPos.z}};
    case 2: return {Quat::axisAngle({0, 0, 1}, -side * kPi * 0.5f), {side * 0.10f, 0.0f, -0.08f}};   // back of the hand
    case 3: return {Quat::axisAngle({0, 0, 1}, side * kPi * 0.5f), {-side * 0.10f, 0.0f, -0.08f}};   // palm side
    }
}
static float segSegDist(const Vec3& p1, const Vec3& q1, const Vec3& p2, const Vec3& q2) {
    const Vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const float a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r), c = dot(d1, r), b = dot(d1, d2);
    float s = 0, t = 0;
    const float den = a * e - b * b;
    if (den > 1e-9f) s = clampf((b * f - c * e) / den, 0.f, 1.f);
    t = e > 1e-9f ? (b * s + f) / e : 0.f;
    if (t < 0) { t = 0; s = a > 1e-9f ? clampf(-c / a, 0.f, 1.f) : 0.f; }
    else if (t > 1) { t = 1; s = a > 1e-9f ? clampf((b - c) / a, 0.f, 1.f) : 0.f; }
    return length((p1 + d1 * s) - (p2 + d2 * t));
}
float Game::fistGap(int p0, int h0, int p1, int h1) const {
    const Pose& g0 = pl[p0].in.hand[h0].grip;
    const Pose& g1 = pl[p1].in.hand[h1].grip;
    return segSegDist(g0.transform(tun.fistBack), g0.transform(tun.fistFront), g1.transform(tun.fistBack), g1.transform(tun.fistFront));
}
Pose Game::shieldPose(int p, int h) const { return pl[p].in.hand[h].grip * shieldLocal(h); }
Vec3 Game::strikeZoneCenter(int p) const {
    if (p < 2) {   // (as it always was: behind player 0 is -X)
        float back = p == 0 ? -1.f : 1.f;
        return tun.start[p] + Vec3{back * tun.strikeZoneBack, tun.strikeZoneHeight, 0};
    }
    return tun.start[p] - facing(p) * tun.strikeZoneBack + Vec3{0, tun.strikeZoneHeight, 0};
}
Vec3 Game::peakAngVel(int p, int h, float t) const {
    const HandState& hs = pl[p].hands[h];
    Vec3 best{};
    for (int i = 0; i < hs.histN; ++i) {
        int k = (hs.histHead - 1 - i + 128) & 63;
        if (t - hs.histT[k] > tun.spinWindow) break;
        if (length2(hs.angHist[k]) > length2(best)) best = hs.angHist[k];
    }
    return best;
}
Vec3 Game::chestPoint(int p) const { return pl[p].in.head.p - Vec3{0, 0.35f, 0}; }
int Game::team(int p) const {
    if (np == 2) return p;
    if (tun.teamMode == TEAMS_2V2) return p < 2 ? 0 : 1;   // partners in opposite corners
    if (tun.teamMode == TEAMS_3V1) return p == tun.theOne ? 1 : 0;
    return p;
}
int Game::leader() const {
    int w = -1, top = -1;
    for (int t = 0; t < teamCount(); ++t) {
        const int sc = teamScore(t);
        if (sc > top) { top = sc; w = t; } else if (sc == top) w = -1;
    }
    return w;
}
int Game::teamScore(int t) const {
    int s = 0;
    for (int p = 0; p < np; ++p) if (team(p) == t) s += pl[p].score;
    return s;
}
Vec3 Game::facing(int p) const {
    Vec3 d{-tun.start[p].x, 0, -tun.start[p].z};
    const float l = length(d);
    return l > 1e-4f ? d / l : Vec3{p == 1 ? -1.f : 1.f, 0, 0};
}
int Game::aimTarget(int p, const Vec3& from, const Vec3& dir) const {
    if (np == 2) return 1 - p;
    int best = -1;
    float bd = -2.f;
    for (int q = 0; q < np; ++q) {
        if (!rivals(p, q)) continue;
        const float d = dot(normalize(chestPoint(q) - from), dir);
        if (d > bd) { bd = d; best = q; }
    }
    return best >= 0 ? best : (p + 1) % np;
}

Vec3 Game::assistDir(int p, const Vec3& from, const Vec3& dir) const {
    if (aimAt[p].empty()) return normalize(chestPoint(aimTarget(p, from, dir)) - from);
    Vec3 to = dir;   // (TRAINING: the point nearest the ball's line)
    float best = -2.f;
    for (const Vec3& t : aimAt[p]) {
        const Vec3 d = normalize(t - from);
        if (dot(d, dir) > best) { best = dot(d, dir); to = d; }
    }
    return to;
}
float Game::throwSpeedFromHandSpeed(float s, int thrower) const {
    // lerp(minThrown, maxThrown + strikes * SpeedIncreasePerStrike, t)
    float t = clampf((s - tun.minInputSpeed) / std::max(1e-3f, tun.maxInputSpeed - tun.minInputSpeed), 0.f, 1.f);
    int strikes = strikesHeld(thrower);
    return lerpf(tun.projectileMinSpeed, tun.projectileMaxSpeed + strikes * tun.speedIncreasePerStrike, t);
}

Vec3 Game::handVelocity(int p, int h, float t) const {
    // Average velocity of the launcher point over ThrowHandler.AverageVelocityWindowSizeSeconds.
    const HandState& hs = pl[p].hands[h];
    if (hs.histN < 2) return {};
    int newest = (hs.histHead + 63) & 63;
    int oldest = newest;
    for (int i = 1; i < hs.histN; ++i) {
        int k = (newest - i + 64) & 63;
        oldest = k;
        if (t - hs.histT[k] >= tun.throwVelocityWindow) break;
    }
    float dt = hs.histT[newest] - hs.histT[oldest];
    if (dt <= 1e-4f) return {};
    return (hs.hist[newest] - hs.hist[oldest]) / dt;
}

const char* eventName(EventType t) {
    switch (t) {
    case EventType::Spawn: return "Spawn";               case EventType::Grab: return "Grab";
    case EventType::Throw: return "Throw";               case EventType::ThrowFail: return "ThrowFail";
    case EventType::Bounce: return "Bounce";             case EventType::ShieldBlock: return "ShieldBlock";
    case EventType::ShieldBreak: return "ShieldBreak";   case EventType::HitPlayer: return "HitPlayer";
    case EventType::Returned: return "Returned";         case EventType::Caught: return "Caught";
    case EventType::Foul: return "Foul";                 case EventType::RoundStart: return "RoundStart";
    case EventType::RoundEnd: return "RoundEnd";         case EventType::MatchEnd: return "MatchEnd";
    case EventType::Countdown: return "Countdown";       case EventType::KnuckleBlock: return "KnuckleBlock";
    case EventType::Recharge: return "Recharge";         case EventType::Strike: return "Strike";
    case EventType::Overtime: return "Overtime";         case EventType::SpawnRise: return "SpawnRise";
    case EventType::FistBump: return "FistBump";       case EventType::LeadChange: return "LeadChange";
    }
    return "?";
}

void Game::setArena(const std::vector<CollisionTri>& tris) {
    arenaKind = -1;
    auto A = std::make_shared<ArenaData>();
    A->tris = tris;
    if (tris.empty()) { arena = A; return; }
    Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    auto grow = [](Vec3& l, Vec3& h, const Vec3& v) {
        l = {std::min(l.x, v.x), std::min(l.y, v.y), std::min(l.z, v.z)};
        h = {std::max(h.x, v.x), std::max(h.y, v.y), std::max(h.z, v.z)};
    };
    for (CollisionTri& t : A->tris) {
        t.mid = (t.a + t.b + t.c) / 3.f;
        t.rad = std::max(length(t.a - t.mid), std::max(length(t.b - t.mid), length(t.c - t.mid)));
        for (const Vec3* v : {&t.a, &t.b, &t.c}) grow(lo, hi, *v);
    }
    if (A->tris.size() > 65535) { arena = A; return; }   // (no grid: every triangle is tested)
    const Vec3 pad{ArenaData::kPad, ArenaData::kPad, ArenaData::kPad};
    A->lo = lo - pad;
    for (int i = 0; i < 3; ++i) A->n[i] = std::max(1, (int)std::ceil(((&hi.x)[i] + ArenaData::kPad - (&A->lo.x)[i]) / ArenaData::kCell));
    const size_t cells = size_t(A->n[0]) * A->n[1] * A->n[2];
    // each triangle in every cell its box (grown by kPad) touches: count, then fill, in triangle order
    auto range = [&](const CollisionTri& t, int c0[3], int c1[3]) {
        Vec3 l{1e9f, 1e9f, 1e9f}, h{-1e9f, -1e9f, -1e9f};
        for (const Vec3* v : {&t.a, &t.b, &t.c}) grow(l, h, *v);
        for (int i = 0; i < 3; ++i) {
            c0[i] = std::max(0, (int)std::floor(((&l.x)[i] - ArenaData::kPad - (&A->lo.x)[i]) / ArenaData::kCell));
            c1[i] = std::min(A->n[i] - 1, (int)std::floor(((&h.x)[i] + ArenaData::kPad - (&A->lo.x)[i]) / ArenaData::kCell));
        }
    };
    std::vector<uint32_t> count(cells + 1, 0);
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            A->first.assign(cells + 1, 0);
            for (size_t c = 0; c < cells; ++c) A->first[c + 1] = A->first[c] + count[c];
            A->items.resize(A->first[cells]);
            std::fill(count.begin(), count.end(), 0);
        }
        for (size_t k = 0; k < A->tris.size(); ++k) {
            int c0[3], c1[3];
            range(A->tris[k], c0, c1);
            for (int z = c0[2]; z <= c1[2]; ++z)
                for (int y = c0[1]; y <= c1[1]; ++y)
                    for (int x = c0[0]; x <= c1[0]; ++x) {
                        const size_t c = (size_t(z) * A->n[1] + y) * A->n[0] + x;
                        if (pass == 1) A->items[A->first[c] + count[c]] = (uint16_t)k;
                        ++count[c];
                    }
        }
    }
    arena = A;
}

void Game::reset(bool player1IsBot) { resetWithBots(player1IsBot ? 2u : 0u); }
void Game::resetWithBots(uint32_t botMask) {
    np = tun.players == 4 ? 4 : 2;
    if (np == 4) tun.advanced = false;   // FOUR CORNERS: Basic's rules (and no shot clock: stepPlayer)
    for (int p = 0; p < kMaxPlayers; ++p) {
        PlayerState& s = pl[p];
        s = PlayerState{};
        if (p >= np) continue;
        s.isBot = (botMask >> p) & 1u;
        s.botStance = -1;   // (a bot's stance: picked as its first step, new each match)
        s.in.head.p = tun.start[p] + Vec3{0, 1.65f, 0};
        spawnBall(p);
        s.ball.pos = idlePoint(p);
        s.botTimer = 1.0f;
    }
    match = MatchState::WaitingForPlayers;
    stateTimer = 0;
    meetBumped = false;
    meetLitT[0] = meetLitT[1] = -1;
    round = 0;
    overtime = false;
    roundClock = tun.advanced ? tun.advancedRoundTime : tun.basicMatchTime;
    lastRoundWinner = matchWinner = -1;
    events.clear();
}

Vec3 Game::meetRing(int p) const {
    float y = 0.5f * ((pl[0].in.headTracked ? pl[0].in.head.p.y : 1.62f) + (pl[1].in.headTracked ? pl[1].in.head.p.y : 1.62f));
    return {p == 0 ? -tun.meetRingX : tun.meetRingX, clampf(y - 0.45f, 0.9f, 1.5f), 0.f};   // chest height between them
}
bool Game::inMeetRing(int p, int h) const {
    const HandInput& hi = pl[p].in.hand[h];
    if (!hi.tracked) return false;
    const Vec3 d = hi.grip.transform(tun.fistFront) - meetRing(p);
    return std::fabs(d.x) < tun.meetRingDepth && d.y * d.y + d.z * d.z < tun.meetRingRadius * tun.meetRingRadius;
}
bool Game::meetLit(int p, int h) const {
    const HandInput& hi = pl[p].in.hand[h];
    return (hi.trigger > 0.5f || hi.squeeze > 0.5f) && inMeetRing(p, h);
}
void Game::useArena(int kind, const std::vector<CollisionTri>& classic) {
    using namespace exparena;
    if (arenaKind != kind) {   // (the collision only when it changes: callers may ask every frame)
        setArena(kind == ARENA_CORNERS ? collision() : kind == ARENA_FOURCORNERS ? collision(true)
                 : kind == ARENA_DIAMOND ? diamondCollision(tun.solidPlatforms) : classic);
        arenaKind = kind;
    }
    if (kind == ARENA_CORNERS || kind == ARENA_FOURCORNERS) {
        tun.foulLineX = kFoulLineX;
        tun.outLow = kFloor - 0.6f; tun.outHigh = kCeiling + 0.6f; tun.outX = tun.outZ = kCorner + 0.7f;
    } else if (kind == ARENA_DIAMOND) {
        tun.foulLineX = 4.73f;
        tun.outLow = kRollCY - kRollReach - 0.6f; tun.outHigh = kRollCY + kRollReach + 0.6f;
        tun.outX = kRollLen + 0.7f; tun.outZ = kRollReach + 0.35f * (kRollW - kRollH) + 0.6f;
    } else {
        tun.foulLineX = 4.73f;
        tun.outLow = -1.f; tun.outHigh = 6.f; tun.outX = 11.f; tun.outZ = 4.f;
    }
}
void Game::spawnBall(int p) {
    Ball& b = pl[p].ball;
    b.state = BallState::Spawning;
    b.timer = tun.respawnDelay + tun.postSpawnDelay + tun.spawnPreTime + tun.spawnRiseTime;
    b.vel = {};
    b.spin = {};
    b.heldHand = -1;
    b.instigator = p;
    b.charged = true;              // ABrawlProjectileSpawner::SpawnProjectileWithShieldRecharge
    b.scored = false;
    b.rechargePending = false;
    b.bounces = 0;
    b.fouled = b.homing = false;
    b.scale = strikeScale(strikesHeld(p));
}

float Game::strikeScale(int n) const {
    int early = std::min(n, tun.fullScaleSteps), late = std::max(0, n - tun.fullScaleSteps);
    return std::min(tun.maxBallScale, 1.f + tun.scaleIncreasePerStrike * early + tun.lateScaleIncreasePerStrike * late);
}

void Game::launch(int p, int h, Vec3 dir, float speed, Vec3 spin, bool assist) {
    Ball& b = pl[p].ball;
    if (assist) {
        const Vec3 to = assistDir(p, b.pos, dir);
        const float deg = tun.advanced ? tun.aimCorrectAdvancedDeg : tun.aimCorrectBasicDeg;
        const float ang = std::acos(clampf(dot(dir, to), -1.f, 1.f));
        if (ang > 1e-4f) dir = normalize(lerp(dir, to, std::min(1.f, deg2rad(deg) / ang)));
    }
    float sl = length(spin);
    if (sl > tun.maxSpin) spin *= tun.maxSpin / sl;
    b.state = BallState::Flying;
    b.vel = dir * std::max(speed, tun.minSpeed);
    b.speedLimit = length(b.vel);
    b.spin = spin;
    b.heldHand = -1;
    b.instigator = p;
    b.scored = false;
    b.rechargePending = true;      // "To recharge your ball, you must throw it past your foul line"
    b.bounces = 0;
    b.timer = 0;
    b.fouled = b.homing = false;
    b.progX = b.pos.x; b.progZ = b.pos.z; b.progT = 0;
    emit(EventType::Throw, p, h, b.pos, b.speedLimit);
}

void Game::serve(int p, const Vec3& from, const Vec3& dir, float speed, const Vec3& spin, bool assist) {
    pl[p].ball.pos = from;
    launch(p, -1, normalize(dir), speed, spin, assist);
    served_.push_back(events.back());   // (served between steps: the next step's events say so)
}

void Game::parkBall(int p) {
    Ball& b = pl[p].ball;
    b.state = BallState::Dead;
    b.timer = 1e9f;
    b.vel = b.spin = {};
    b.heldHand = -1;
    b.pos = tun.spawner[p] + Vec3{0, -2.f * tun.spawnDepth, 0};
}

void Game::trialFlight(int p, const Vec3& from, const Vec3& dir, float speed, const Vec3& spin, bool assist, float horizon,
                       float dt, std::vector<BallPathPt>& out) const {
    out.clear();
    Game g = *this;
    for (int q = 0; q < np; ++q) {   // (everyone out of the way)
        g.pl[q].in.head.p = {0, -50, 0};
        g.pl[q].in.headTracked = false;
        for (HandState& hs : g.pl[q].hands) hs.shieldUp = hs.knuckleUp = false;
    }
    g.serve(p, from, dir, speed, spin, assist);
    const Ball& b = g.pl[p].ball;
    out.push_back({g.now, b.pos, b.vel, false, b});
    for (float t = 0; t < horizon; t += dt) {
        g.now += dt;
        g.events.clear();
        g.stepBall(p, dt);
        bool strike = false;
        for (const GameEvent& e : g.events) strike |= e.type == EventType::Strike;
        out.push_back({g.now, b.pos, b.vel, strike, b});
        if (b.state != BallState::Flying) break;
    }
}

void Game::deflect(Ball& b, const Vec3& n, int deflector, float assistDeg, const Vec3& from) {
    float sp = length(b.vel);
    Vec3 r = b.vel - n * (2 * dot(b.vel, n));
    Vec3 dir = normalize(r), tgt = assistDir(deflector, from, dir);
    float ang = std::acos(clampf(dot(dir, tgt), -1.f, 1.f));
    if (ang > 1e-4f) dir = normalize(lerp(dir, tgt, std::min(1.f, deg2rad(assistDeg) / ang)));
    b.vel = dir * sp;
    b.instigator = deflector;      // OnRep_Instigator: scoring / zone rules follow the last deflector
    b.rechargePending = false;
    b.scored = false;
    b.spin *= 0.5f;
    b.timer = 0;   // (the self-hit grace, after a block as after a throw)
    b.progX = b.pos.x; b.progZ = b.pos.z; b.progT = 0;
}

bool Game::sweepArena(const Vec3& from, const Vec3& to, float r, Vec3& hitPos, Vec3& hitN, uint32_t& tags) const {
    // Discrete test at the end point (callers sub-step finely). Picks the deepest penetration,
    // preferring tagged zones (foul/return) over plain walls when both overlap.
    float best = 1e9f;
    bool hit = false;
    Vec3 motion = to - from;
    if (!arena) return false;
    const ArenaData& A = *arena;
    auto test = [&](const CollisionTri& t) {
        if (t.rad >= 0 && length2(to - t.mid) > (t.rad + r) * (t.rad + r)) return;   // nowhere near it
        Vec3 ab = t.b - t.a, ac = t.c - t.a, ap = to - t.a;
        float d1 = dot(ab, ap), d2 = dot(ac, ap);
        Vec3 q;
        if (d1 <= 0 && d2 <= 0) q = t.a;
        else {
            Vec3 bp = to - t.b; float d3 = dot(ab, bp), d4 = dot(ac, bp);
            if (d3 >= 0 && d4 <= d3) q = t.b;
            else {
                float vc = d1 * d4 - d3 * d2;
                if (vc <= 0 && d1 >= 0 && d3 <= 0) q = t.a + ab * (d1 / (d1 - d3));
                else {
                    Vec3 cp = to - t.c; float d5 = dot(ab, cp), d6 = dot(ac, cp);
                    if (d6 >= 0 && d5 <= d6) q = t.c;
                    else {
                        float vb = d5 * d2 - d1 * d6;
                        if (vb <= 0 && d2 >= 0 && d6 <= 0) q = t.a + ac * (d2 / (d2 - d6));
                        else {
                            float va = d3 * d6 - d5 * d4;
                            if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) q = t.b + (t.c - t.b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
                            else { float den = 1.f / (va + vb + vc); q = t.a + ab * (vb * den) + ac * (vc * den); }
                        }
                    }
                }
            }
        }
        Vec3 d = to - q;
        float dist = length(d);
        if (dist >= r) return;
        Vec3 n = dist > 1e-6f ? d / dist : t.n;
        // the foul zones are a slab 5 cm over the floor with no face at the line: a ball touching its edge bounced off
        // the corner, nearly straight up, then between floor and ceiling (players, 2026-10-28). They bounce as floor.
        if (t.tags & TAG_FOUL) n = t.n;
        if (dot(motion, n) >= 0) return;   // already separating
        float score = dist - ((t.tags & (TAG_FOUL | TAG_RETURN)) ? 0.05f : 0.f);
        if (score < best) { best = score; hitPos = q + n * r; hitN = n; tags = t.tags; hit = true; }
    };
    if (r <= ArenaData::kPad && !A.first.empty()) {   // only the triangles listed for the ball's cell can touch it
        int c[3];
        for (int i = 0; i < 3; ++i) {
            c[i] = (int)std::floor(((&to.x)[i] - (&A.lo.x)[i]) / ArenaData::kCell);
            if (c[i] < 0 || c[i] >= A.n[i]) return false;   // (outside the grid: nothing within kPad)
        }
        const size_t cell = (size_t(c[2]) * A.n[1] + c[1]) * A.n[0] + c[0];
        for (uint32_t k = A.first[cell]; k < A.first[cell + 1]; ++k) test(A.tris[A.items[k]]);
    } else {
        for (const CollisionTri& t : A.tris) test(t);
    }
    return hit;
}

bool Game::hitsBody(int v, const Vec3& pos, float r) const { return bodyTouches(pl[v].in.head.p, pos, r); }
bool Game::bodyTouches(const Vec3& head, const Vec3& pos, float r) const {
    if (length(pos - head) < tun.headRadius + r) return true;
    Vec3 a = head - Vec3{0, tun.torsoTop, 0};
    Vec3 b = head - Vec3{0, tun.torsoBottom, 0};
    if (b.y < 0.3f) b.y = 0.3f;
    Vec3 ab = b - a;
    float t = clampf(dot(pos - a, ab) / std::max(1e-6f, dot(ab, ab)), 0.f, 1.f);
    return length(pos - (a + ab * t)) < tun.torsoRadius + r;
}

bool Game::bodyTouchesPath(const Vec3& head, const Vec3& from, const Vec3& to, float r) const {
    if (segSegDist(from, to, head, head) < tun.headRadius + r) return true;
    Vec3 a = head - Vec3{0, tun.torsoTop, 0}, b = head - Vec3{0, tun.torsoBottom, 0};
    if (b.y < 0.3f) b.y = 0.3f;
    return segSegDist(from, to, a, b) < tun.torsoRadius + r;
}

bool Game::hitsShield(int p, int h, const Vec3& from, const Vec3& to, float r, Vec3& n) const {
    if (!pl[p].hands[h].shieldUp) return false;
    Pose s = shieldPose(p, h);
    Quat inv = s.q.conj();
    Vec3 a = inv.rotate(from - s.p), b = inv.rotate(to - s.p);
    Vec3 e = tun.shieldHalfExtents + Vec3{r, r, r};
    float t0 = 0, t1 = 1;
    Vec3 d = b - a;
    const float* pa = &a.x; const float* pd = &d.x; const float* pe = &e.x;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(pd[i]) < 1e-8f) { if (pa[i] < -pe[i] || pa[i] > pe[i]) return false; continue; }
        float ta = (-pe[i] - pa[i]) / pd[i], tb = (pe[i] - pa[i]) / pd[i];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta); t1 = std::min(t1, tb);
        if (t0 > t1) return false;
    }
    Vec3 up = s.q.rotate(kUp);
    n = dot(to - from, up) > 0 ? -up : up;
    return true;
}

void Game::scorePoint(int scorer, int victim, const Vec3& where, int ball) {
    if (match != MatchState::InProgress) return;
    const int ledBy = np == 4 ? leader() : -1;
    // (3 v 1: THE ONE's hits count double)
    pl[scorer].score += np == 4 && tun.teamMode == TEAMS_3V1 && scorer == tun.theOne ? tun.onePoints : 1;
    if (np == 2 && tun.botAdapt && pl[scorer].isBot != pl[victim].isBot)   // MATCH MY LEVEL: toward close matches
        tun.botMatch = clampf(tun.botMatch + (pl[scorer].isBot ? -tun.botMatchStep : tun.botMatchStep), 0.f, kBotLevels - 1.f);
    pl[victim].strikesAgainst = 0;   // "If you get hit, any strikes against you are removed."
    pl[victim].strikesFor = 0;       // (four players: and the strikes they held)
    emit(EventType::HitPlayer, victim, -1, where, float(scorer), ball);
    if (np == 4 && !overtime && leader() >= 0 && leader() != ledBy) emit(EventType::LeadChange, -1, -1, {}, float(leader()));
    if (!tun.advanced) {
        if (overtime) {   // sudden death
            matchWinner = team(scorer);
            match = MatchState::MatchSummary;
            emit(EventType::MatchEnd, matchWinner, -1, {}, 0);   // (the winning team, as on the clock)
        }
    } else if (overtime || pl[scorer].score >= tun.advancedPointsToWin) {   // (overtime: a timed round's sudden death)
        endRound(scorer);
    }
}

void Game::endRound(int scorer) {
    lastRoundWinner = scorer;
    pl[scorer].roundWins++;
    match = MatchState::RoundSummary;
    stateTimer = tun.roundSummaryTime;
    emit(EventType::RoundEnd, scorer, -1, {}, float(round));
}

void Game::addStrike(int against, const Vec3& where, int ball, int by) {
    if (match != MatchState::InProgress) return;
    if (pl[against].strikesAgainst >= tun.maxStrikes) return;
    pl[against].strikesAgainst++;
    if (np == 4 && by >= 0 && by < np && pl[by].strikesFor < tun.maxStrikes) pl[by].strikesFor++;
    emit(EventType::Strike, against, -1, where, float(pl[against].strikesAgainst), ball);
}

void Game::stepBall(int p, float dt) {
    PlayerState& s = pl[p];
    Ball& b = s.ball;
    // stepped growth: ease toward the size for the current strike count
    {
        float tgt = strikeScale(strikesHeld(p));
        b.scale += (tgt - b.scale) * std::min(1.f, dt * tun.scaleGrowRate);
    }
    switch (b.state) {
    case BallState::Dead:
    case BallState::Spawning: {
        b.timer -= dt;
        const float pre = tun.respawnDelay + tun.postSpawnDelay + tun.spawnPreTime;
        const float total = pre + tun.spawnRiseTime;
        const float e = b.state == BallState::Spawning ? clampf(total - b.timer, 0.f, total) : 0.f;
        if (e < pre) {   // hidden under the platform while the pre-spawn rings play
            b.pos = tun.spawner[p] + Vec3{0, -2.f * tun.spawnDepth + tun.spawnDepth * (e / pre), 0};
        } else {         // appears at the launcher point and springs up to the idle point (slight overshoot)
            if (e - dt < pre) emit(EventType::SpawnRise, p, -1, tun.spawner[p] + Vec3{0, tun.spawnLauncherHeight, 0});
            float r = clampf((e - pre) / std::max(1e-3f, tun.spawnRiseTime), 0.f, 1.f);
            r = r * r * (3.f - 2.f * r);   // ease in as well as out: a smooth journey up
            const float c1 = 0.9f, c3 = c1 + 1.f;
            float k = 1.f + c3 * std::pow(r - 1.f, 3.f) + c1 * std::pow(r - 1.f, 2.f);   // easeOutBack
            b.pos = lerp(tun.spawner[p] + Vec3{0, tun.spawnLauncherHeight, 0}, idlePoint(p), k);
        }
        b.vel = {};
        if (b.timer <= 0) {
            b.state = BallState::Idle;
            b.pos = idlePoint(p);
            emit(EventType::Spawn, p, -1, b.pos);
        }
        return;
    }
    case BallState::Idle: {
        Vec3 acc = (idlePoint(p) - b.pos) * tun.springK - b.vel * tun.springDamping;   // DampedSpringComponent
        b.vel += acc * dt;
        b.pos += b.vel * dt;
        return;
    }
    case BallState::Held:
        b.pos = pl[p].in.hand[b.heldHand].grip.transform(tun.holdOffset);   // in the fist (throws use the launcher)
        return;
    case BallState::Returning:
    case BallState::Flying: break;
    }

    b.timer += dt;
    if (b.homing) {   // a fouled ball on its way home after its bounce off the wall
        const Vec3 d = idlePoint(p) - b.pos;
        if (length(d) < 0.35f || b.timer > 6.f) {
            b.state = BallState::Idle;
            b.homing = b.fouled = false;
            b.vel = b.vel * 0.3f;
            b.spin = {};
            return;
        }
        // a magnetic pull: the velocity swings toward home (a curve, not a sharp turn) at the return speed
        const Vec3 want = normalize(d) * tun.foulReturnSpeed;
        b.vel += (want - b.vel) * std::min(1.f, dt * 7.f);
    }
    auto goHome = [&]() {   // drawn home, harmless, by the same magnetic pull as a foul
        b.state = BallState::Returning;
        b.heldHand = -1;
        b.instigator = p;
        b.spin = {};
        b.scored = true;
        b.fouled = false;
        b.homing = true;
        b.rechargePending = false;
        b.timer = 0;
        b.speedLimit = tun.foulReturnSpeed;
        emit(EventType::ThrowFail, p, -1, b.pos, length(b.vel));
    };
    // a short throw goes home: not yet past the owner's foul line, and slow, heading back, or taking too long
    if (b.state == BallState::Flying && b.instigator == p && b.rechargePending) {
        // (along the player's facing: two players, +X or -X exactly as it always was)
        const bool behind = dot(b.pos, facing(p)) < -tun.foulLineX;
        const float forward = dot(b.vel, facing(p));   // toward the opponent
        if (behind && (b.speedLimit < tun.recallSpeed || forward < 0 || b.timer > tun.recallTime)) { goHome(); return; }
    }
    // a stuck ball goes home: no headway along the court, or slowed to a crawl by its bounces (see Tunables)
    if (b.state == BallState::Flying) {
        // (headway: along the court; four players, across the room's floor plan)
        const float moved = np == 2 ? std::fabs(b.pos.x - b.progX) : length(Vec3{b.pos.x - b.progX, 0, b.pos.z - b.progZ});
        if (moved >= tun.stuckDistance) { b.progX = b.pos.x; b.progZ = b.pos.z; b.progT = b.timer; }
        if (b.timer - b.progT > tun.stuckTime || (b.speedLimit < tun.recallSpeed && b.timer > tun.slowGrace)) { goHome(); return; }
    }
    // a fouled ball that finds no wall to turn home from (bouncing between floor and ceiling) turns home anyway
    if (b.fouled && b.timer > tun.stuckTime) {
        b.fouled = false;
        b.homing = true;
        b.timer = 0;
        b.speedLimit = tun.foulReturnSpeed;
    }
    const float r = ballRadius(p);
    const int sub = std::max(1, int(std::ceil(length(b.vel) * dt / 0.02f)));
    const float h = dt / sub;
    for (int i = 0; i < sub; ++i) {
        // Magnus curve from spin, at constant speed (SpeedLimit FixedToInitialSpeed)
        if (length2(b.spin) > 1e-4f) {
            b.vel += cross(b.spin, b.vel) * (tun.magnusFactor * h);
            b.vel = normalize(b.vel) * b.speedLimit;
            b.spin *= std::exp(-tun.spinDecayPerSecond * h);
        }
        Vec3 from = b.pos, to = b.pos + b.vel * h;
        // recharge once the owner's throw crosses the owner's foul line
        if (b.rechargePending && b.instigator == p && dot(to, facing(p)) > -tun.foulLineX) {
            b.rechargePending = false;
            if (!b.charged) emit(EventType::Recharge, p, -1, to);
            b.charged = true;
            pl[p].shotClock = tun.shotClockSeconds;                         // throw past the foul line resets it
            if (np == 2 && tun.useShotClock && pl[1 - p].shotClock <= 0) addStrike(1 - p, to);   // automatic strike (no ball's doing)
        }
        bool handled = b.state == BallState::Returning;   // a returning ball can't be blocked / score
        // shields and knuckle guards of the player who did NOT last touch the ball (four players: every rival of the
        // one who did; a teammate's ball passes through you)
        // shields take priority over the knuckle bumpers when both are in the ball's path
        const int by0 = b.instigator;
        for (int pass = 0; pass < 2 && !handled; ++pass)
        for (int qi = 0; qi < np && !handled; ++qi) {
        const int q = np == 2 ? 1 - by0 : qi;
        if (np == 2 ? qi > 0 : !rivals(by0, q)) continue;
        for (int hh = 0; hh < 2 && !handled; ++hh) {
            Vec3 n;
            if (pass == 0 && hitsShield(q, hh, from, to, r, n)) {
                Vec3 vs = recentHandVel(q, hh, shieldLocal(hh).p, now);
                deflectMoving(b, n, vs, q, tun.advanced ? tun.shieldAimAssistAdvancedDeg : tun.shieldAimAssistBasicDeg, from);
                if (!(np == 4 && tun.teamMode == TEAMS_3V1 && q == tun.theOne)) {   // (THE ONE's shield never breaks)
                    pl[q].ball.charged = false;        // "Shield breaks on hit": the held ball loses its charge
                    pl[q].hands[hh].shieldUp = false;
                }
                emit(EventType::ShieldBlock, q, hh, from, 0, p);
                emit(EventType::ShieldBreak, q, hh, from);
                to = from + b.vel * h;
                handled = true;
            } else if (pass == 1 && pl[q].hands[hh].knuckleUp && hitsBox(knucklePose(q, hh), tun.knuckleHalf, from, to, r, n)) {
                Vec3 lo = tun.knuckleOffset;
                if (hh == 0) lo.x = -lo.x;
                Vec3 vs = recentHandVel(q, hh, lo, now);
                // bounce off the guard's front face (fist forward), not whichever box face was entered first
                Vec3 face = knucklePose(q, hh).q.rotate({0, 0, -1});
                if (dot(face, b.vel - vs) > 0) face = -face;
                n = normalize(face * 0.75f + n * 0.25f);
                deflectMoving(b, n, vs, q, tun.knuckleAimAssistDeg, from, true);
                emit(EventType::KnuckleBlock, q, hh, from, length(b.vel), p);
                to = from + b.vel * h;
                handled = true;
            }
        }
        }
        // the player who did not last touch the ball gets hit (four players: any rival of the one who did)
        for (int vi = 0; vi < np; ++vi) {
        const int victim = np == 2 ? 1 - b.instigator : vi;
        if (np == 2 ? vi > 0 : !rivals(b.instigator, victim)) continue;
        if (b.state == BallState::Flying) {   // strike: the ball crosses the victim's strike-zone ring inside its radius
            Vec3 c = strikeZoneCenter(victim);
            if (np == 2) {
                if ((from.x - c.x) * (to.x - c.x) <= 0 && std::fabs(to.x - from.x) > 1e-6f) {
                    float t = (c.x - from.x) / (to.x - from.x);
                    Vec3 x = from + (to - from) * t;
                    if (length(Vec3{0, x.y - c.y, x.z - c.z}) < tun.strikeZoneRadius) addStrike(victim, x, p);
                }
            } else {   // (the ring faces its player's facing)
                const Vec3 f = facing(victim);
                const float a0 = dot(from - c, f), a1 = dot(to - c, f);
                if (a0 * a1 <= 0 && std::fabs(a1 - a0) > 1e-6f) {
                    const Vec3 x = from + (to - from) * (a0 / (a0 - a1));
                    const Vec3 off = x - c;
                    if (length(off - f * dot(off, f)) < tun.strikeZoneRadius) addStrike(victim, x, p, b.instigator);
                }
            }
        }
        if (!handled && !b.scored && hitsBody(victim, to, r)) {
            b.scored = true;
            scorePoint(b.instigator, victim, to, p);
            b.state = BallState::Dead;
            b.timer = tun.respawnDelay + tun.postSpawnDelay + 0.5f;
            return;
        }
        }
        // your own ball can hit you on its way back (rebound or slow return); the opponent gets the point
        {
            int self = b.instigator;
            Vec3 toSelf = pl[self].in.head.p - to;
            if (tun.selfHitScores && !b.scored && b.timer > tun.selfHitGrace && dot(b.vel, toSelf) > 0 &&
                pl[self].in.headTracked && hitsBody(self, to, r)) {
                b.scored = true;
                if (np == 2) scorePoint(1 - self, self, to, p);   // (four players: nobody's point, the ball is spent)
                b.state = BallState::Dead;
                b.timer = tun.respawnDelay + tun.postSpawnDelay + 0.5f;
                return;
            }
        }
        {
            Vec3 hp, hn; uint32_t tags = 0;
            if (sweepArena(from, to, r, hp, hn, tags)) {
                int zoneTeam = (tags & TAG_TEAM0) ? 0 : (tags & TAG_TEAM1) ? 1 : (tags & TAG_TEAM2) ? 2 : (tags & TAG_TEAM3) ? 3 : -1;
                bool own = (zoneTeam >= 0 && zoneTeam == b.instigator);   // team gating
                bool returning = b.state == BallState::Returning;
                if ((tags & TAG_FOUL) && own && !returning) {
                    // a foul. Deflecting the opponent's ball into your own foul zone gives them a strike.
                    // The original dissolved the ball; here (owner, 2026-10-21) it stays in the arena: a ball that never
                    // crossed its owner's foul line springs straight back to its float position; one that did bounces on,
                    // off the wall it is heading for, and flies home, harmless (it can't hit or score; its owner can catch it)
                    emit(EventType::Foul, b.instigator, -1, hp, float(zoneTeam));
                    if (b.instigator != p) {
                        // blocked or punched into the blocker's own zone: their strike, and the ball comes back to its
                        // owner as from their back wall, live: catch it, move, or be hit (owner, 2026-10-29)
                        addStrike(b.instigator, hp, p);
                        emit(EventType::Returned, p, -1, hp, 0.f);
                        b.state = BallState::Returning;
                        b.instigator = p;
                        b.rechargePending = false;
                        b.scored = false;
                        b.fouled = b.homing = false;
                        b.pos = hp + hn * 0.01f;
                        b.vel = normalize(chestPoint(p) - b.pos) * tun.returnSpeed;
                        b.speedLimit = tun.returnSpeed;
                        b.spin *= 0.3f;
                        b.bounces = 0;
                        b.timer = 0;
                        return;
                    }
                    if (b.instigator == p && b.rechargePending) {   // bounce off, then drawn home (owner: faster, magnetic)
                        b.state = BallState::Returning;
                        b.heldHand = -1;
                        b.spin = {};
                        b.rechargePending = false;
                        b.scored = true;
                        b.fouled = false;
                        b.homing = true;
                        b.timer = 0;
                        b.speedLimit = tun.foulReturnSpeed;
                        b.vel = (b.vel - hn * (2 * dot(b.vel, hn))) * 0.6f;
                        b.pos = hp + hn * 0.01f;
                        return;
                    }
                    b.state = BallState::Returning;
                    b.instigator = p;
                    b.rechargePending = false;
                    b.scored = true;
                    b.fouled = true;
                    b.homing = false;
                    b.timer = 0;
                    b.speedLimit = std::max(tun.foulMinSpeed, std::min(b.speedLimit, tun.foulReturnSpeed));   // its own speed
                    b.vel = normalize(b.vel - hn * (2 * dot(b.vel, hn))) * b.speedLimit;   // bounce off the floor
                    b.pos = hp + hn * 0.01f;
                    return;
                }
                if (b.fouled && std::fabs(hn.y) < 0.7f) {   // the wall it was heading for: bounce, then home
                    b.fouled = false;
                    b.homing = true;
                    b.timer = 0;
                    b.speedLimit = tun.foulReturnSpeed;
                    b.pos = hp + hn * 0.01f;
                    b.vel = normalize(idlePoint(p) - b.pos) * b.speedLimit;
                    emit(EventType::Bounce, p, -1, hp, 0.f);
                    return;
                }
                if (tags & TAG_RETURN) {
                    bool ownerWall = (zoneTeam == p);
                    if (ownerWall) {
                        // "Your ball has gone past you and hit your back wall ... your ball will respawn in front of you."
                        emit(EventType::Returned, p, -1, hp, 1.f);
                        spawnBall(p);
                        return;
                    }
                    if (!returning) {
                        // opponent's back wall: start the slow return toward the owner's end (not homing on the owner)
                        emit(EventType::Returned, p, -1, hp, 0.f);
                        // initial trajectory toward where the owner is now; it does not keep seeking afterwards
                        Vec3 aim = chestPoint(p);
                        b.state = BallState::Returning;
                        b.instigator = p;
                        b.rechargePending = false;
                        b.vel = normalize(aim - hp) * tun.returnSpeed;
                        b.speedLimit = tun.returnSpeed;
                        b.spin *= 0.3f;
                        b.bounces = 0;
                        b.timer = 0;
                        b.pos = hp + hn * 0.01f;
                        return;
                    }
                }
                Vec3 v = b.vel - hn * (2 * dot(b.vel, hn));
                // spin grips the wall: tangential kick, then lose some spin
                v += cross(b.spin, hn) * (tun.spinBounceFactor * r);
                b.spin *= (1.f - tun.spinBounceFactor);
                if (tun.bouncesChangeSpeed && !returning) b.speedLimit = std::max(tun.minSpeed, b.speedLimit * tun.elasticity);
                b.vel = normalize(v) * b.speedLimit;
                b.bounces++;
                emit(EventType::Bounce, p, -1, hp, b.speedLimit);
                to = hp;
                if (b.bounces > tun.maxBounces) { spawnBall(p); return; }
            }
        }
        b.pos = to;
        if (b.pos.y < tun.outLow || b.pos.y > tun.outHigh || std::fabs(b.pos.x) > tun.outX || std::fabs(b.pos.z) > tun.outZ) { spawnBall(p); return; }
    }
    if (b.timer > 10.f) spawnBall(p);
}

void Game::stepPlayer(int p, float dt) {
    PlayerState& s = pl[p];
    Ball& b = s.ball;
    // shot clock runs while my ball is in my end of the court
    if (match == MatchState::InProgress && tun.useShotClock && np == 2) {
        bool inMyEnd = (b.state != BallState::Flying) || dot(b.pos, facing(p)) < -tun.foulLineX;
        if (inMyEnd) s.shotClock = std::max(0.f, s.shotClock - dt);
    } else {
        s.shotClock = tun.shotClockSeconds;
    }
    for (int h = 0; h < 2; ++h) {
        HandState& hs = s.hands[h];
        const HandInput& hi = s.in.hand[h];
        hs.hist[hs.histHead] = launcherPoint(p, h);
        hs.angHist[hs.histHead] = hi.angVel;
        hs.gripHist[hs.histHead] = hi.grip;
        hs.histT[hs.histHead] = now;
        hs.histHead = (hs.histHead + 1) & 63;
        hs.histN = std::min(64, hs.histN + 1);

        bool pressed = hi.trigger > 0.55f && hs.prevTrigger <= 0.55f;
        bool released = hi.trigger < 0.35f;
        bool holding = (b.state == BallState::Held && b.heldHand == h);
        if (!s.isBot && hi.tracked) {
            if (holding && released) {
                Vec3 v = handVelocity(p, h, now);
                float sp = length(v);
                // throws count in a match, and as warm-up while waiting for an opponent (scoring needs a match)
                const bool canThrow = match == MatchState::InProgress || match == MatchState::WaitingForPlayers;
                if (sp < tun.throwMinHandSpeed || !canThrow) {
                    // dropped: the spawner's damped spring pulls the ball back to its idle point
                    b.state = BallState::Idle;
                    b.heldHand = -1;
                    b.vel = v;
                    emit(EventType::ThrowFail, p, h, b.pos, sp);
                } else {
                    launch(p, h, normalize(v), throwSpeedFromHandSpeed(sp, p), peakAngVel(p, h, now));
                }
            } else if (b.state == BallState::Idle &&
                       ((pressed && length(launcherPoint(p, h) - b.pos) < 0.25f) ||
                        (hi.trigger > 0.5f && length(catcherPoint(p, h) - b.pos) < tun.catchHalfHeight + ballRadius(p)))) {
                // off the spawner: a trigger press at the ball, or a swing through it with the trigger already held
                b.state = BallState::Held; b.heldHand = h; b.vel = {};
                emit(EventType::Grab, p, h, b.pos);
            } else if (hi.trigger > 0.5f && (b.state == BallState::Flying || b.state == BallState::Returning) && b.instigator == p &&
                       length(catcherPoint(p, h) - b.pos) < tun.catchHalfHeight + ballRadius(p)) {
                // catch your own ball ("You can't catch your opponent's ball!")
                b.state = BallState::Held; b.heldHand = h; b.vel = {}; b.spin = {};
                emit(EventType::Caught, p, h, b.pos);
            }
        }
        holding = (b.state == BallState::Held && b.heldHand == h);
        // "To activate your shield, you must first hold a charged ball."
        hs.shieldUp = hi.tracked && holding && b.charged && tun.shields;   // (MATCH OPTIONS: shields off = HOTDOG MODE)
        // in Basic the gauntlet bumpers are always active, trigger or not. (The tutorial line "Pulling the trigger will
        // deactivate the knuckle guard on that hand" belongs to the tutorial's own setup, not Brawl Basic.)
        hs.knuckleUp = tun.knuckleGuards() && hi.tracked;   // (MATCH OPTIONS can turn them on or off in either rules)
        hs.prevTrigger = hi.trigger;
    }
}

// ---- the practice bot (owner, 2026-10-26): a person's body and limits, its skill set by its level. Throws
// raised 2026-10-27 from the owner's recorded matches (their median throw 14.5 m/s, at the game's top speed): ROOKIE
// about 9 m/s, PRO 12, UNBEATABLE 14 (the most a throw gives)
static const BotSkill kBotSkills[kBotLevels] = {
    //  name         react  defend reads  misread hand body  accel throw  aim    hold         catch plan   strikes slap
    {"ROOKIE",      0.45f, 0.30f, false, 0.30f, 2.5f, 1.0f, 4.f,  4.2f, 0.60f, 2.0f, 4.0f, 0.2f, false, false, 0.00f},
    {"AMATEUR",     0.35f, 0.50f, false, 0.18f, 3.0f, 1.4f, 6.f,  4.8f, 0.45f, 1.6f, 3.4f, 0.5f, false, false, 0.00f},
    {"PRO",         0.25f, 0.65f, true,  0.10f, 3.8f, 1.8f, 7.f,  5.6f, 0.35f, 1.2f, 3.0f, 0.7f, false, false, 0.30f},
    {"ALL-STAR",    0.20f, 0.80f, true,  0.05f, 4.5f, 2.2f, 9.f,  6.0f, 0.20f, 0.9f, 2.2f, 0.9f, true,  false, 0.50f},
    {"CHAMPION",    0.17f, 0.92f, true,  0.02f, 5.2f, 2.6f, 11.f, 6.3f, 0.10f, 0.7f, 1.6f, 1.0f, true,  true,  0.65f},
    {"UNBEATABLE",  0.15f, 1.00f, true,  0.00f, 6.0f, 3.0f, 12.f, 6.5f, 0.00f, 0.6f, 1.2f, 1.0f, true,  true,  0.75f},
};
const BotSkill& botSkill(int level) { return kBotSkills[level < 0 ? 0 : level >= kBotLevels ? kBotLevels - 1 : level]; }
BotSkill botSkillAt(float level) {
    level = clampf(level, 0.f, kBotLevels - 1.f);
    const int i = std::min((int)level, kBotLevels - 2);
    const float f = level - i;
    const BotSkill &a = kBotSkills[i], &b = kBotSkills[i + 1];
    BotSkill s = f < 0.5f ? a : b;
    auto mix = [&](float BotSkill::*m) { s.*m = a.*m + (b.*m - a.*m) * f; };
    for (float BotSkill::*m : {&BotSkill::reaction, &BotSkill::defend, &BotSkill::readError, &BotSkill::handSpeed, &BotSkill::bodySpeed,
                               &BotSkill::bodyAccel, &BotSkill::throwHand, &BotSkill::aimError, &BotSkill::holdMin, &BotSkill::holdMax,
                               &BotSkill::catchChance, &BotSkill::slap})
        mix(m);
    return s;
}
BotSkill Game::botSkillOf(int p) const {
    if (pl[p].botLevel >= 0) return botSkill(pl[p].botLevel);
    return tun.botAdapt ? botSkillAt(tun.botMatch) : botSkill(tun.botLevel);
}

// the stances: each hand's grip from the head (head frame: x right, y up, z back; the right hand, the left mirrored)
// and its controller's pitch / yaw / roll (degrees), how low it crouches, and how much it bobs
struct BotStance { const char* name; Vec3 pos[2]; Vec3 rot[2]; float crouch, bob; };
static const BotStance kStances[kBotStances] = {
    // (the avatar puts an elbow behind its hand, along the hand's direction: hands tipped up keep the elbows down by
    // the ribs; owner, 2026-10-28: elbows stuck out in every stance. Where the hands are is the bot's defence: kept)
    {"KITTEN",  {{-0.25f, -0.40f, -0.25f}, {0.25f, -0.40f, -0.25f}}, {{28, 0, 0}, {28, 0, 0}}, 0.00f, 1.0f},       // paws up
    {"RELAXED", {{-0.19f, -0.86f, -0.02f}, {0.19f, -0.86f, -0.02f}}, {{-88, 0, 0}, {-88, 0, 0}}, 0.00f, 0.7f},     // arms down
    {"BOXER",   {{-0.11f, -0.17f, -0.34f}, {0.13f, -0.21f, -0.22f}}, {{50, -10, 25}, {50, 10, -25}}, 0.05f, 1.8f},  // fists up
    {"KARATE",  {{-0.06f, -0.34f, -0.44f}, {0.22f, -0.60f, 0.02f}}, {{10, 0, 70}, {0, 0, -90}}, 0.08f, 0.8f},     // lead hand out, one chambered
    {"MANTIS",  {{-0.14f, -0.10f, -0.34f}, {0.14f, -0.10f, -0.34f}}, {{40, 0, 0}, {40, 0, 0}}, 0.06f, 1.0f},      // raised, forearms up
    {"MONKEY",  {{-0.30f, -0.60f, -0.30f}, {0.30f, -0.60f, -0.30f}}, {{-20, 0, 0}, {-20, 0, 0}}, 0.18f, 2.2f},    // low, loose, bouncing
};
const char* botStanceName(int stance) { return kStances[stance < 0 ? 0 : stance >= kBotStances ? kBotStances - 1 : stance].name; }

// a person's movement toward a target: never faster than vmax, speeding up and slowing down at most amax (no
// teleporting, no instant stops)
static void approach(Vec3& x, Vec3& v, const Vec3& target, float vmax, float amax, float dt) {
    const Vec3 e = target - x;
    const float d = length(e);
    const Vec3 want = d > 1e-5f ? e / d * std::min(vmax, std::sqrt(2.f * amax * d)) : Vec3{};
    Vec3 dv = want - v;
    const float l = length(dv), m = amax * dt;
    if (l > m) dv *= m / l;
    v += dv;
    x += v * dt;
    if (d < 0.003f && length(v) < 0.5f) { x = target; v = {}; }   // there: settled (not trembling about it)
}
static float moveTime(float d, float vmax, float amax) {   // from standing still to standing still, d metres away
    return d < vmax * vmax / amax ? 2.f * std::sqrt(d / amax) : d / vmax + vmax / amax;
}

Vec3 Game::botShoulder(int p, int h) const {
    const Pose& hd = pl[p].in.head;   // (the bot's head stays level, facing the court)
    const Vec3 o = tun.botShoulder;
    return hd.p + hd.q.rotate({h == 1 ? o.x : -o.x, 0, o.z}) + Vec3{0, o.y, 0};
}
Vec3 Game::botWrist(int h, const Pose& grip) const {
    const Vec3 w = tun.botWrist;
    return grip.transform({h == 1 ? w.x : -w.x, w.y, w.z});
}
Pose Game::botTuck(int p, int h, const Vec3& head) const {
    const Quat q = pl[p].in.head.q;
    return {q * Quat::axisAngle({1, 0, 0}, deg2rad(-20)),
            head + q.rotate({h == 1 ? 0.20f : -0.20f, 0, 0.12f}) + Vec3{0, -0.48f, 0}};
}
Vec3 Game::botInReach(int p, int h, const Pose& grip) const {
    const Vec3 sh = botShoulder(p, h), w = botWrist(h, grip), d = w - sh;
    const float l = length(d);
    return l > tun.botArm ? grip.p + d * (tun.botArm / l - 1.f) : grip.p;   // (the hand moves in with its wrist)
}

void Game::predictBall(int owner, int reader, float horizon, float dt, std::vector<BallPathPt>& out) const {
    out.clear();
    Game g = *this;
    PlayerState& r = g.pl[reader];
    r.in.head.p = {0, -50, 0};   // the reader out of the way: no hit, no block
    for (HandState& hs : r.hands) hs.shieldUp = hs.knuckleUp = false;
    const Ball& b = g.pl[owner].ball;
    const int by = b.instigator;
    // (read during a step, before that step moves the ball: the ball is where the step before left it, and this
    // step's move comes first, at this step's time)
    out.push_back({g.now - dt, b.pos, b.vel, false, b});
    for (float t = 0; t < horizon; t += dt, g.now += dt) {
        g.events.clear();
        g.stepBall(owner, dt);
        // (a returning ball can only reach its owner)
        if ((b.state != BallState::Flying && b.state != BallState::Returning) || b.instigator != by ||
            (b.state == BallState::Returning && owner != reader))
            break;
        bool strike = false;
        for (const GameEvent& e : g.events) strike |= e.type == EventType::Strike && e.player == reader;
        out.push_back({g.now, b.pos, b.vel, strike, b});
    }
}

Pose Game::botBlockGrip(int p, int h, int plan, const BallPathPt& x) const {
    // the surface faces where the ball comes from; a good reader angles it to send the ball back at the opponent
    // (the reflection of the ball's direction d onto t needs a normal along t - d), never more than 60 deg off square
    const Vec3 d = normalize(x.vel);
    Vec3 n = -d;
    if (plan != BOT_CATCH && botSkillOf(p).readsFlight) {
        const Vec3 aim = normalize(normalize(chestPoint(botAim(p)) - x.pos) - d);
        n = dot(aim, -d) >= 0.5f ? aim : normalize(aim - d);
    }
    // turn `g` about n so that its local `upLocal` is as near `upWorld` as it goes
    auto roll = [&](Quat g, const Vec3& upLocal, const Vec3& upWorld) {
        Vec3 cur = g.rotate(upLocal), want = upWorld - n * dot(upWorld, n);
        cur = cur - n * dot(cur, n);
        if (length2(cur) > 1e-6f && length2(want) > 1e-6f) g = fromTo(normalize(cur), normalize(want)) * g;
        return g;
    };
    if (plan == BOT_SHIELD) {   // the plate (its normal = local +Y) facing n, its long side down
        const Quat plate = roll(fromTo({0, 1, 0}, n), {0, 0, 1}, {0, -1, 0});
        const Pose sl = shieldLocal(h);
        const Quat gq = plate * sl.q.conj();   // grip * shieldLocal = plate
        return {gq, x.pos - gq.rotate(sl.p)};
    }
    if (plan == BOT_KNUCKLE) {   // the front of the fist facing n, knuckles up
        const Quat gq = roll(fromTo({0, 0, -1}, n), {0, 1, 0}, {0, 1, 0});
        Vec3 ko = tun.knuckleOffset;
        if (h == 0) ko.x = -ko.x;
        return {gq, x.pos - gq.rotate(ko)};
    }
    const Quat gq = pl[p].in.head.q * Quat::axisAngle({1, 0, 0}, deg2rad(-20));   // a catch: the hand as at rest
    return {gq, x.pos - gq.rotate(tun.catcherOffset)};
}

int Game::botTry(int p, int k, int owner, int plan, int h, const Pose& grip, const Vec3& body, int tuck) const {
    if (botTriesLeft <= 0) { botTriesOut = true; return -1; }   // (none left this step: not known yet)
    --botTriesLeft;
    Game g = *this;
    g.botFrozen = true;
    PlayerState& s = g.pl[p];
    PlayerState::BotRead& r = s.botRead[k];
    r.plan = plan; r.hand = h; r.grip = grip.p; r.gripQ = grip.q; r.body = body; r.tuck = tuck;
    const BallPath& path = pl[p].botRead[k].path;   // (the read stays here: copies don't carry it)
    const Vec3 stand = tun.start[p] + Vec3{0, 1.62f, 0};
    Ball& b = g.pl[owner].ball;
    const int by = b.instigator;
    bool touched = false, near = false;
    int result = 2;
    // (while the ball is still far off, nothing of the bot can touch it: it follows the flight already read, so only
    // the part near the bot is played out; the read is from this step, in the same steps)
    const bool skip = botSkillOf(p).readsFlight && !path.empty() && std::fabs(path[0].t - (now - lastDt)) < 1e-5f;
    int step = 0;
    for (float t = 0; t < 3.f; t += lastDt) {
        if (step++ > 0) g.now += lastDt;   // (the first: the rest of this step, as the bot's own goes on from here)
        g.events.clear();
        g.stepBot(p, lastDt);
        g.stepPlayer(p, lastDt);
        if (!near && skip && step < (int)path.size() && length(path[step].pos - stand) > 3.f) {
            b = path[step].ball;
            continue;
        }
        near = true;
        // (on its way off and 8 m away: should it come back, there is time to read it again)
        if (dot(b.vel, b.pos - stand) > 0 && length(b.pos - stand) > 8.f) break;
        g.stepBall(owner, lastDt);
        for (const GameEvent& e : g.events) {
            if (e.type == EventType::HitPlayer && e.player == p) return 0;
            if (e.type == EventType::Strike && e.player == p) result = 1;
            touched |= (e.type == EventType::ShieldBlock || e.type == EventType::KnuckleBlock) && e.player == p;
        }
        // done: it stops, the opponent sends it back (a new ball to read), or it goes home to the opponent
        if (b.state != BallState::Flying && b.state != BallState::Returning) break;
        if ((touched || by == p) && rivals(b.instigator, p)) break;
        if (b.state == BallState::Returning && b.instigator != p) break;
    }
    return result;
}

bool Game::botReachable(int p, int h, const Pose& grip, float by) const {
    if (length(botWrist(h, grip) - botShoulder(p, h)) > tun.botArm - 0.02f) return false;
    const BotSkill& sk = botSkillOf(p);
    // (there a moment early, to settle: a hand still moving changes where the ball goes)
    return now + moveTime(length(grip.p - pl[p].botHand[h]), sk.handSpeed, tun.botHandAccel) + 0.10f <= by;
}

void Game::botWatch(int p, int k, const BotSkill& sk) {
    PlayerState& s = pl[p];
    PlayerState::BotRead& r = s.botRead[k];
    const int owner = k == 0 ? botThreat(p) : p;
    const Ball& B = pl[owner].ball;
    int mode = 0;
    if (B.state == BallState::Flying && rivals(B.instigator, p) && !B.scored) mode = 1;   // coming at it
    else if ((B.state == BallState::Flying || (B.state == BallState::Returning && owner == p)) && B.instigator == p &&
             !B.homing && !B.scored && dot(B.vel, s.in.head.p - B.pos) > 0)
        mode = 2;   // one it threw or deflected, coming back at it (off a wall, or returned)
    if (mode != r.mode || owner != r.owner) { r = PlayerState::BotRead{}; r.mode = mode; r.owner = owner; }   // (a new ball, or one that changed hands)
    if (!mode) return;
    if (r.noticeT < 0) {
        r.noticeT = now;
        // it stops where it is (a body still moving comes to rest a little further on) and watches
        r.body = s.botBody + s.botBodyVel * (length(s.botBodyVel) / (2.f * sk.bodyAccel));
        for (int h = 0; h < 2; ++h) { r.rel[h] = s.botHand[h] - s.in.head.p; r.relQ[h] = s.botHandQ[h]; }
        r.act = rnd() < sk.defend;
        r.catchIt = mode == 2 && rnd() < sk.catchChance;
        const Vec3 e{rnd() - 0.5f, rnd() - 0.5f, rnd() - 0.5f};
        r.err = normalize(e) * (sk.readError * rnd());
    }
    if (now - r.noticeT < sk.reaction) return;   // not seen yet
    bool fresh = r.path.empty();
    if (!fresh && sk.readsFlight) {   // still the flight it read? (where the read says the last step left the ball)
        const float tb = now - lastDt;
        size_t i = 0;
        while (i + 1 < r.path.size() && r.path[i].t < tb) ++i;
        Vec3 at = r.path[i].pos;
        if (i > 0 && r.path[i].t > r.path[i - 1].t)
            at = lerp(r.path[i - 1].pos, at, clampf((tb - r.path[i - 1].t) / (r.path[i].t - r.path[i - 1].t), 0.f, 1.f));
        // (and read on before it runs out: a slow ball can be in the air longer than one read reaches)
        fresh = r.path.back().t < now + (r.full ? 1.f : 0.f) || length(at - r.err - B.pos) > 0.08f;
    } else if (!fresh) {
        fresh = now - r.readT > 0.12f;   // a straight-line read, taken again as the ball comes
    }
    // (a shield up or down since it planned (it caught its ball, or blocked with it): its plan was tried out without
    // that, so a new one)
    const int hands = (s.hands[0].shieldUp ? 1 : 0) | (s.hands[1].shieldUp ? 2 : 0);
    if (!fresh && r.hands >= 0 && hands != r.hands && !r.path.empty()) {
        r.hands = hands;
        r.tried.clear();
        botPlan(p, k, sk);
        return;
    }
    r.hands = hands;
    if (!fresh && !r.again) return;
    if (!fresh) {   // the rest of a plan, the read unchanged
        r.again = false;
        botPlan(p, k, sk);
        return;
    }
    if (sk.readsFlight) {
        predictBall(owner, p, 2.5f, lastDt, r.path);
        r.full = r.path.back().t >= now + 2.5f - 2.f * lastDt;
    } else {
        r.path.clear();
        for (int i = 0; i <= 600; ++i) r.path.push_back({now + i / 240.f, B.pos + B.vel * (i / 240.f), B.vel, false, B});
    }
    for (BallPathPt& x : r.path) x.pos += r.err;
    r.tried.clear();
    r.readT = now;
    botPlan(p, k, sk);
}

int Game::botThreat(int p) const {
    if (np == 2) return 1 - p;
    // the attack that reaches it soonest (and nearest): a ball a rival last touched, flying its way. It keeps the one it
    // watches unless another is clearly sooner (a read changing every step would never get anywhere)
    const Vec3 head = pl[p].in.head.p;
    const int cur = pl[p].botRead[0].owner;
    int best = cur >= 0 && cur != p ? cur : -1;
    float bu = 1e9f, cu = 1e9f;
    for (int o = 0; o < np; ++o) {
        if (o == p) continue;
        const Ball& B = pl[o].ball;
        if (B.state != BallState::Flying || B.scored || !rivals(B.instigator, p)) continue;
        const Vec3 d = head - B.pos;
        const float v2 = length2(B.vel);
        if (v2 < 1e-4f || dot(B.vel, d) <= 0) continue;
        const float tca = dot(d, B.vel) / v2, miss = length(B.pos + B.vel * tca - head);
        const float u = tca + 0.25f * miss;
        if (o == cur) cu = u;
        if (u < bu) { bu = u; best = o; }
    }
    if (cur >= 0 && cur != p && cu < 1e8f && cu < bu + 0.15f) return cur;
    return best >= 0 ? best : (p + 1) % np;
}
int Game::botPickTarget(int p) {
    int r[kMaxPlayers], n = 0;
    for (int q = 0; q < np; ++q) if (rivals(p, q)) r[n++] = q;
    return n ? r[std::min(n - 1, (int)(rnd() * n))] : -1;
}

void Game::botPlan(int p, int k, const BotSkill& sk) {
    PlayerState& s = pl[p];
    PlayerState::BotRead& r = s.botRead[k];
    const Ball& B = pl[k == 0 ? (s.botRead[0].owner >= 0 ? s.botRead[0].owner : botThreat(p)) : p].ball;
    const float br = tun.projectileRadius * B.scale;
    // (it plans from where it stands still: r.body, chosen when it noticed the ball; its hands hold where they come
    // to rest now)
    const Vec3 still = r.body;
    for (int h = 0; h < 2; ++h) {
        const Vec3 v = s.botHand[h] + s.botHandVel[h] * (length(s.botHandVel[h]) / (2.f * tun.botHandAccel)) - s.in.head.p;
        r.rel[h] = v;
        r.relQ[h] = s.botHandQ[h];
        if (r.mode == 1) {   // a ball coming at it: from its stance into the ready one (hands up in a guard catch balls by
            const BotStance& k = kStances[STANCE_KITTEN];   // themselves, and send some into its own foul zone)
            r.rel[h] = s.in.head.q.rotate(k.pos[h]);
            r.relQ[h] = s.in.head.q * Quat::axisAngle({1, 0, 0}, deg2rad(k.rot[h].x));
        }
    }
    // (and its head steady: it stops bobbing while it watches)
    const Vec3 head = (match == MatchState::Meet ? Vec3{p == 0 ? -tun.meetSpot : tun.meetSpot, 1.62f, 0} : tun.start[p] + Vec3{0, 1.62f, 0}) + still;
    r.plan = BOT_NONE; r.hand = -1; r.hitT = -1; r.tuck = -1;
    float strikeT = -1;
    // the stretches of the read still to come that pass anywhere near where its body can be (within its reach of
    // steps, leans and ducks): only those can touch it. (The whole stretch between two points read: a fast ball moves
    // 15 cm or more between two.)
    std::vector<int> near;
    {
        const Vec3 mid = tun.start[p] + Vec3{0, 1.2f, 0};
        const float reach = length(tun.botBodyRange) + 0.5f * tun.torsoBottom + tun.torsoRadius + br + 0.1f;
        for (size_t i = 1; i < r.path.size(); ++i) {
            if (r.path[i].t < now) continue;
            if (segSegDist(r.path[i - 1].pos, r.path[i].pos, mid, mid) < reach) near.push_back((int)i);
            if (strikeT < 0 && r.path[i].strike) strikeT = r.path[i].t;
        }
    }
    for (int i : near)
        if (bodyTouchesPath(head, r.path[i - 1].pos, r.path[i].pos, br + 0.06f)) { r.hitT = r.path[i].t; break; }
    if (!r.act && !r.catchIt) return;
    const int owner = k == 0 ? (s.botRead[0].owner >= 0 ? s.botRead[0].owner : botThreat(p)) : p;
    const bool tries = sk.guardsStrikes;   // (the best try their answer out first)
    botTriesOut = false;
    struct Again {   // a plan that ran out of tryouts, with nothing found yet, goes on next step
        PlayerState::BotRead& r; const bool& out;
        ~Again() { if (out && r.plan == BOT_NONE) r.again = true; }
    } again_{r, botTriesOut};
    // a tryout, or how it went the last time (a plan carried on over steps goes on from where it was)
    auto tryOut = [&](uint32_t key, int plan, int h, const Pose& g, const Vec3& body, int tuck) {
        for (const auto& c : r.tried) if (c.first == key) return (int)c.second;
        const int v = botTry(p, k, owner, plan, h, g, body, tuck);
        if (v >= 0) r.tried.push_back({key, (int8_t)v});
        return v;
    };
    const Pose none{};
    // the hand holding its ball, its shield up: unless it blocks with it, the best pull it in to their side as a ball
    // comes (a shield where it happens to be can catch the ball, and send it back into them)
    const int sh = tries && r.mode == 1 && s.ball.state == BallState::Held && s.hands[s.ball.heldHand].shieldUp ? s.ball.heldHand : -1;
    // the ball misses its body: would it still get it, off a hand where it is (or tucked in)?
    int passive = 2;
    if (tries && r.mode == 1 && r.hitT < 0) {
        if (sh >= 0) passive = tryOut(0xFFFFFFFEu, BOT_NONE, -1, none, still, sh);
        if (passive == 2 && sh >= 0) r.tuck = sh;
        else passive = std::max(passive, tryOut(0xFFFFFFFFu, BOT_NONE, -1, none, still, -1));
        if (passive == 0)   // (its answer must come before the ball reaches it)
            for (const BallPathPt& x : r.path)
                if (x.t >= now && dot(x.pos - head, s.in.head.q.rotate({0, 0, -1})) < 0.6f) { r.hitT = x.t; break; }
    }
    const PlayerState::BotRead& other = s.botRead[1 - k];
    const int busy = other.plan == BOT_SHIELD || other.plan == BOT_KNUCKLE || other.plan == BOT_CATCH ? other.hand : -1;
    // the point of the path where hand h meets the ball best (in reach, in time, before `until`): nearest its shoulder
    // (tried out: it must take the ball and not send it back into it, nor into its own foul zone unless `need` allows)
    auto pick = [&](int h, int plan, float until, float& dist, int need) {
        dist = 1e9f;
        if (h < 0 || h == busy) return -1;
        std::vector<std::pair<float, int>> ok;
        for (size_t i = 0; i < r.path.size(); ++i) {
            const BallPathPt& x = r.path[i];
            if (x.t <= now || x.t > until) continue;
            const Pose g = botBlockGrip(p, h, plan, x);
            if (botReachable(p, h, g, x.t)) ok.push_back({length(botWrist(h, g) - botShoulder(p, h)), (int)i});
        }
        std::sort(ok.begin(), ok.end());
        for (size_t j = 0; j < ok.size() && j < 6; ++j) {
            if (plan != BOT_CATCH && tries &&
                tryOut(uint32_t(plan) << 24 | uint32_t(h) << 20 | uint32_t(ok[j].second), plan, h,
                       botBlockGrip(p, h, plan, r.path[ok[j].second]), still, -1) < need)
                continue;
            dist = ok[j].first;
            return ok[j].second;
        }
        return -1;
    };
    auto take = [&](int h, int plan, int i) {
        const Pose g = botBlockGrip(p, h, plan, r.path[i]);
        r.plan = plan; r.hand = h; r.grip = g.p; r.gripQ = g.q;
        r.body = still;   // (it holds still for it)
    };
    auto either = [&](int plan, float until, int need) {   // the better of the two hands
        float d0, d1;
        const int i0 = pick(0, plan, until, d0, need), i1 = pick(1, plan, until, d1, need);
        if (i0 < 0 && i1 < 0) return false;
        if (i1 >= 0 && (i0 < 0 || d1 < d0)) take(1, plan, i1); else take(0, plan, i0);
        return true;
    };
    // out of the ball's way: the smallest step, lean or duck that clears it, made in time
    // (tried out: its hands come along, and a shield up must not catch the ball on the way)
    auto dodge = [&](int need) {
        const Vec3 right = s.in.head.q.rotate({1, 0, 0});
        const Vec3 stand = tun.start[p] + Vec3{0, 1.62f, 0};
        struct Spot { float cost; Vec3 o; uint32_t key; };
        std::vector<Spot> ok;
        const Vec3 fwd = s.in.head.q.rotate({0, 0, -1});
        const float steps[3] = {0.f, 0.3f, -0.15f};   // (a step forward or back, for a ball coming in on a slant)
        // (how far the ball's path stays from a body standing at offset o: below zero, it touches)
        auto gap = [&](const Vec3& o) {
            const Vec3 hd = stand + o, a = hd - Vec3{0, tun.torsoTop, 0};
            Vec3 b2 = hd - Vec3{0, tun.torsoBottom, 0};
            if (b2.y < 0.3f) b2.y = 0.3f;
            float g = 9.f;
            for (int i : near) {
                const Vec3 &x0 = r.path[i - 1].pos, &x1 = r.path[i].pos;
                g = std::min(g, std::min(segSegDist(x0, x1, hd, hd) - tun.headRadius, segSegDist(x0, x1, a, b2) - tun.torsoRadius) - br);
            }
            return g;
        };
        float bestGap = -9.f;
        Vec3 bestO = still;
        for (int iff = 0; iff < 3; ++iff)
        for (int iy = 0; iy < 4; ++iy)
            for (int ix = -10; ix <= 10; ++ix) {
                const Vec3 o = right * (tun.botBodyRange.x * 0.1f * ix) + Vec3{0, -tun.botBodyRange.y * iy / 3.f, 0} + fwd * steps[iff];
                const float move = length(o - still);
                if (now + moveTime(move, sk.bodySpeed, sk.bodyAccel) + 0.02f > r.hitT) continue;
                const float g = gap(o);
                if (g > bestGap) { bestGap = g; bestO = o; }
                if (g > 0.06f) ok.push_back({move - 0.3f * o.y, o, 0x40000000u | uint32_t(iff * 4 + iy) << 8 | uint32_t(ix + 10)});   // (ducking costs a little more)
            }
        std::sort(ok.begin(), ok.end(), [](const Spot& a, const Spot& b) { return a.cost < b.cost; });
        // each as it stands, or with the arm holding its ball (shield up) pulled in, if that hand gets there in time
        int tried = 0;
        for (size_t j = 0; j < ok.size() && tried < 16; ++j)
            for (int v = 0; v < (sh >= 0 ? 2 : 1) && tried < 16; ++v) {
                const int tuck = v ? -1 : sh;   // (tucked in first)
                Pose g = none;
                if (tuck >= 0) {
                    g = botTuck(p, tuck, {});   // (from its head)
                    if (now + moveTime(length(stand + ok[j].o + g.p - s.botHand[tuck]), sk.handSpeed, tun.botHandAccel) + 0.05f > r.hitT)
                        continue;
                }
                ++tried;
                if (tries && tryOut(ok[j].key | uint32_t(v) << 16, BOT_DODGE, -1, none, ok[j].o, tuck) < need) continue;
                r.plan = BOT_DODGE; r.body = ok[j].o; r.tuck = tuck;
                return true;
            }
        if (need == 1 && ok.empty() && bestGap > -8.f) {   // nowhere clear in time: as far from it as it can get
            r.plan = BOT_DODGE; r.body = bestO; r.tuck = sh;
            return true;
        }
        return false;
    };
    float d;
    if (r.mode == 2) {   // a ball of its own coming back: catch it (only its own ball), or step out of its way
        if (k == 1 && r.catchIt && either(BOT_CATCH, r.hitT >= 0 ? r.hitT - 0.02f : 1e9f, 2)) return;
        // (a ball coming at it decides where its body goes: only then may it step out of its own ball's way)
        if (r.act && r.hitT >= 0 && (k == 0 || !s.botRead[0].mode) && !dodge(2)) dodge(1);
        return;
    }
    if (!r.act) return;
    const float until = r.hitT >= 0 ? r.hitT - 0.02f : sk.guardsStrikes && strikeT >= 0 ? strikeT - 0.02f : -1.f;
    if (until < 0) return;   // it misses (and the strike zone, or it doesn't mind)
    // the shield (on the hand holding a charged ball), then a knuckle guard, then a dodge; a block that fouls only when
    // nothing else will do
    const bool shield = s.ball.state == BallState::Held && s.ball.charged && tun.shields;
    for (int need = 2; need >= 1; --need) {
        if (shield) {
            const int i = pick(s.ball.heldHand, BOT_SHIELD, until, d, need);
            if (i >= 0) { take(s.ball.heldHand, BOT_SHIELD, i); return; }
        }
        if (tun.knuckleGuards() && either(BOT_KNUCKLE, until, need)) return;
        if (r.hitT >= 0 && dodge(need)) return;
    }
}

void Game::botPlanShot(int p, const BotSkill& sk) {
    PlayerState& s = pl[p];
    const int opp = botAim(p), k = s.botShotsTried++, h = s.ball.heldHand;
    const Vec3 fwd = s.in.head.q.rotate({0, 0, -1});
    const Vec3 rel = botShoulder(p, h) + fwd * 0.45f;   // about where its hand lets go
    Vec3 aim = chestPoint(opp) + Vec3{rnd() - 0.5f, rnd() - 0.5f, rnd() - 0.5f} * 0.15f, spin{};
    const float sgn = rnd() < 0.5f ? -1.f : 1.f;
    switch (k % 6) {
    case 0: break;                                          // the chest
    case 1: aim.y += sgn > 0 ? 0.25f : -0.30f; break;       // the head or the hips
    case 2: aim.z += sgn * 0.22f; break;                    // a side
    case 3: aim.z += sgn * 3.0f; break;                     // off the side wall
    default: {                                              // a curve (spin about up bends it along cross(up, fwd))
        spin = kUp * (sgn * lerpf(14.f, 26.f, rnd()));
        aim -= cross(kUp, fwd) * (sgn * lerpf(0.8f, 1.5f, rnd()));
    }
    }
    const Vec3 dir = normalize(aim - rel);
    Game g = *this;
    Ball& gb = g.pl[p].ball;
    gb.pos = rel;
    g.launch(p, h, dir, throwSpeedFromHandSpeed(sk.throwHand, p), spin);
    float score = -100;
    const float dt = 1.f / 120;
    for (float t = 0; t < 2.5f && gb.state == BallState::Flying; t += dt) {
        g.events.clear();
        g.now += dt;
        g.stepBall(p, dt);
        bool done = false;
        for (const GameEvent& e : g.events) {
            if (e.type == EventType::HitPlayer && e.player == opp) {   // it scores: soon, and far from their hands
                float far = 1.f;
                for (int hh = 0; hh < 2; ++hh) far = std::min(far, length(pl[opp].in.hand[hh].grip.p - e.pos));
                score = 1000.f - 150.f * t + 200.f * far + (length2(spin) > 0 || k % 6 == 3 ? 60.f : 0.f);
                done = true;
            } else if ((e.type == EventType::ShieldBlock || e.type == EventType::KnuckleBlock) && e.player == opp) {
                score = -500;
                done = true;
            } else if (e.type == EventType::Strike && e.player == opp) {
                score = std::max(score, 300.f);
            }
        }
        if (done) break;
    }
    if (score > s.botShotScore) { s.botShotScore = score; s.botShotDir = dir; s.botShotSpin = spin; }
}

void Game::stepBot(int p, float dt) {
    // Practice opponent following the same rules (shield only while holding a charged ball), moving as a person
    // can: a body that steps, leans and ducks, two arms of a person's reach and speed, either hand for everything
    PlayerState& s = pl[p];
    const BotSkill& sk = botSkillOf(p);
    if (np == 4 && (s.botTarget < 0 || !rivals(p, s.botTarget))) s.botTarget = botPickTarget(p);
    const int opp = botAim(p);
    // (facing the centre: two players exactly as they always were)
    const Quat q = Quat::axisAngle(kUp, p < 2 ? deg2rad(p == 0 ? -90.f : 90.f) : std::atan2(-facing(p).x, -facing(p).z));
    const Vec3 right = q.rotate({1, 0, 0}), fwd = q.rotate({0, 0, -1});
    // where it stands: its end, or its side of the centre for a fist bump
    const bool meet = match == MatchState::Meet;
    const Vec3 stand = meet ? Vec3{p == 0 ? -tun.meetSpot : tun.meetSpot, 1.62f, 0} : tun.start[p] + Vec3{0, 1.62f, 0};
    if (s.botStance < 0 && s.coach.on) s.botStance = s.coach.stance;
    if (s.botStance < 0) {   // its stance this match. Beginners hold no guard (hands up in front block by themselves)
        static const int kOpen[3] = {STANCE_KITTEN, STANCE_RELAXED, STANCE_MONKEY};
        const float level = s.botLevel >= 0 ? (float)s.botLevel : tun.botAdapt ? tun.botMatch : (float)tun.botLevel;
        s.botStance = level < 1.5f ? kOpen[(int)(rnd() * 3) % 3] : (int)(rnd() * kBotStances) % kBotStances;
    }
    const BotStance& st = kStances[s.botStance >= kBotStances ? 0 : s.botStance];
    // (steady while it watches a ball, and at the fist bump: a body swaying against a fist held out folded the arm into
    // the torso, owner's report 2026-10-29)
    s.botSteady += ((s.botRead[0].mode || s.botRead[1].mode || meet ? 1.f : 0.f) - s.botSteady) * std::min(1.f, dt * 5.f);
    auto bob = [&]() { return Vec3{0, 0.03f * st.bob * (1.f - s.botSteady) * std::sin(now * 2.1f * std::sqrt(st.bob)), 0}; };
    // a bow (KARATE, at the fist bump): the head down and forward
    s.botBow += ((s.botStance == STANCE_KARATE && meet && meetBumped ? 1.f : 0.f) - s.botBow) * std::min(1.f, dt * 6.f);
    auto headAt = [&]() {
        return Pose{q * Quat::axisAngle({1, 0, 0}, -0.6f * s.botBow), stand + s.botBody + bob() + fwd * (0.10f * s.botBow) - Vec3{0, 0.12f * s.botBow, 0}};
    };
    Ball& b = s.ball;
    s.in.headTracked = true;
    s.in.head = headAt();
    auto side = [&](int h) { return right * (h == 1 ? 1.f : -1.f); };
    auto stanceQ = [&](int h) {
        const Vec3 r = st.rot[h];
        return q * Quat::axisAngle({1, 0, 0}, deg2rad(r.x)) * Quat::axisAngle({0, 1, 0}, deg2rad(r.y)) * Quat::axisAngle({0, 0, 1}, deg2rad(r.z));
    };
    const Quat restQ = q * Quat::axisAngle({1, 0, 0}, deg2rad(-20));   // (a hand reaching for something: forward, a little down)
    auto rest = [&](int h) { return s.in.head.p + q.rotate(st.pos[h]); };
    if (!s.botHandSet) {
        for (int h = 0; h < 2; ++h) { s.botHand[h] = rest(h); s.botHandVel[h] = {}; s.botHandQ[h] = stanceQ(h); }
        s.botHandSet = true;
    }
    for (int h = 0; h < 2; ++h) {
        HandInput& hi = s.in.hand[h];
        hi.tracked = true;
        hi.trigger = 0;
        hi.angVel = {};
        hi.grip = {s.botHandQ[h], s.botHand[h]};
    }
    s.botTimer -= dt;
    botTriesLeft = kBotTriesPerStep;
    if (!botFrozen && !(s.coach.on && !s.coach.watch)) {   // (in a tryout: the plan being tried, as it is)
        botWatch(p, 0, sk);
        botWatch(p, 1, sk);
    }
    const PlayerState::BotRead* reads[2] = {&s.botRead[0], &s.botRead[1]};
    auto handBusy = [&](int h) {
        for (const PlayerState::BotRead* r : reads)
            if (r->hand == h && (r->plan == BOT_SHIELD || r->plan == BOT_KNUCKLE || r->plan == BOT_CATCH)) return true;
        return false;
    };
    // where everything goes: the hands at rest (the ball held ready), the body swaying a little where it stands
    Pose want[2] = {{stanceQ(0), rest(0)}, {stanceQ(1), rest(1)}};
    float speed[2] = {sk.handSpeed, sk.handSpeed};
    if (b.state == BallState::Held) want[b.heldHand].p = s.in.head.p + Vec3{0, -0.15f, 0} + side(b.heldHand) * 0.30f - fwd * 0.10f;
    // (it stops swaying while it watches a ball)
    Vec3 bodyWant = right * (meet ? 0.f : 0.15f * std::sin(now * 0.9f)) - Vec3{0, st.crouch, 0};
    bool threatened = false;
    for (const PlayerState::BotRead* r : reads) {
        // (no throw while a ball comes at it: a wind-up could swing its shield into it. Four players: only an attack that
        // reaches it within a second, or there would always be one on its way and the bot would hardly ever throw)
        threatened |= r->mode != 0 && (np == 2 || (r->mode == 1 && r->hitT >= 0 && r->hitT - now < 1.0f));
        if (r->plan == BOT_SHIELD || r->plan == BOT_KNUCKLE || r->plan == BOT_CATCH) want[r->hand] = {r->gripQ, r->grip};
        if (r->mode && r->plan != BOT_CATCH && (r == reads[0] || !reads[0]->mode)) bodyWant = r->body;   // (still while it watches; or its dodge)
    }
    // while it watches a ball, the hands it isn't using keep their place (from its head, so a dodge carries them):
    // what its tryouts assumed (a hand still moving could swing a shield into the ball)
    bool watching = false;
    Vec3 carried[2] = {{0, 0, 0}, {0, 0, 0}};   // (from its head: placed once the body has moved this step)
    for (int h = 0; h < 2; ++h) carried[h].y = 1e9f;
    for (const PlayerState::BotRead* r : reads) {
        if (!r->mode || watching) continue;
        watching = true;
        for (int h = 0; h < 2; ++h)
            if (!handBusy(h)) {
                want[h] = (r->plan == BOT_DODGE || r->plan == BOT_NONE) && r->tuck == h ? botTuck(p, h, s.in.head.p) : Pose{r->relQ[h], s.in.head.p + r->rel[h]};
                carried[h] = want[h].p - s.in.head.p;
            }
    }
    // its ball off the spawner, with the nearer free hand (four players: unless an attack reaches it within a second;
    // there is nearly always a ball to watch)
    bool soon = false;
    for (const PlayerState::BotRead* r : reads) soon |= r->mode == 1 && r->hitT >= 0 && r->hitT - now < 1.0f;
    if (match == MatchState::InProgress && b.state == BallState::Idle && (np == 2 ? !watching : !soon)) {
        int h = -1;
        float bd = 1e9f;
        for (int k = 0; k < 2; ++k) {
            const float d = length(botShoulder(p, k) - b.pos);
            if (!handBusy(k) && d < bd) { bd = d; h = k; }
        }
        if (s.coach.on && !handBusy(s.coach.hand)) h = s.coach.hand;
        if (h >= 0) {
            want[h] = {restQ, b.pos - restQ.rotate(tun.launcherOffset)};
            if (length(launcherPoint(p, h) - b.pos) < 0.25f) {
                b.state = BallState::Held; b.heldHand = h; b.vel = {};
                emit(EventType::Grab, p, h, b.pos);
                s.botTimer = lerpf(sk.holdMin, sk.holdMax, rnd()) * (np == 4 ? 0.5f : 1.f);   // (four players: quicker, more to aim at)
                s.botThrow = 0; s.botShotsTried = 0; s.botShotScore = -1e9f;
            }
        }
    }
    // a throw: wind up behind the shoulder, swing through at a throw's hand speed, let go
    const bool holding = b.state == BallState::Held;
    if (!holding || match != MatchState::InProgress) s.botSlap = false;
    if (!holding || (threatened && !s.botSlap) || match != MatchState::InProgress) s.botThrow = 0;   // (a slap shot goes on)
    if (holding && !b.charged && !s.coach.on) s.botTimer = std::min(s.botTimer, 0.35f);   // recharge by throwing
    if (holding && (!threatened || s.botSlap) && match == MatchState::InProgress) {
        const int h = b.heldHand;
        if (s.botThrow == 0 && s.coach.on) {   // a coach: the throw it is told, when it is told
            if (s.coach.fire) {
                s.coach.fire = false;
                s.botShotDir = normalize(s.coach.dir);
                s.botShotSpin = s.coach.spin;
                s.botThrow = 1;
                s.botThrowT = now;
            }
        } else if (s.botThrow == 0) {
            // one try a step while it waits (not in a step its reads already spent tryouts in)
            if (sk.planShots && s.botShotsTried < 12 && botTriesLeft == kBotTriesPerStep) botPlanShot(p, sk);
            // the best keep their ball (their shield) while the opponent can throw, unless their shot clock runs low
            const Ball& ob = pl[opp].ball;
            const bool patient = sk.guardsStrikes && tun.shields && !tun.knuckleGuards() && b.charged &&
                                 (ob.state == BallState::Held || ob.state == BallState::Idle) && s.shotClock > 3.f &&
                                 s.botTimer > -4.f;
            if (s.botTimer <= 0 && !patient) {
                if (!(sk.planShots && s.botShotScore > 0)) {   // at the chest, off the side wall, or curved
                    const Vec3 rel = botShoulder(p, h) + fwd * 0.45f;
                    Vec3 target = chestPoint(opp) + Vec3{rnd() - 0.5f, rnd() - 0.5f, rnd() - 0.5f} * sk.aimError;
                    s.botShotSpin = {};
                    if (rnd() < 0.3f) {   // off a side wall (four players: to one side of the line to them)
                        const float sw = (rnd() < 0.5f ? -1.f : 1.f) * 3.0f;
                        if (np == 2) target.z += sw; else target += normalize(cross(kUp, target - rel)) * sw;
                    }
                    else if (rnd() < 0.4f) s.botShotSpin = kUp * ((rnd() < 0.5f ? -1.f : 1.f) * lerpf(12.f, 28.f, rnd()));
                    s.botShotDir = normalize(target - rel);
                } else {   // the planned throw, made as well as it can
                    s.botShotDir = normalize(s.botShotDir + Vec3{rnd() - 0.5f, rnd() - 0.5f, rnd() - 0.5f} * (sk.aimError * 0.1f));
                }
                s.botThrow = 1;
                s.botThrowT = now;
            }
        }
        if (s.botThrow == 1) {   // wind up (a coach: always as long, so its throws leave when they are planned to)
            want[h].p = botShoulder(p, h) + Vec3{0, 0.12f, 0} - fwd * 0.22f + side(h) * 0.06f;
            if (s.coach.on ? now - s.botThrowT >= 0.45f - 1e-4f : length(s.botHand[h] - want[h].p) < 0.06f || now - s.botThrowT > 0.6f) {
                s.botThrow = 2; s.botThrowT = now;
            }
        } else if (s.botThrow == 2) {   // the swing, forward along the throw
            Vec3 flat = s.botShotDir;
            flat.y *= 0.3f;
            want[h].p = botShoulder(p, h) + normalize(flat) * 0.50f;
            speed[h] = s.coach.on ? std::max(2.f, s.coach.speed / 2.2f) : sk.throwHand;
            if (s.coach.on ? now - s.botThrowT >= 0.25f - 1e-4f : length(s.botHand[h] - want[h].p) < 0.15f || now - s.botThrowT > 0.4f) {
                b.pos = launcherPoint(p, h);
                if (s.coach.on) {   // (the direction as it is now: a coach throwing at you aims where you are)
                    b.pos = s.coach.from;
                    launch(p, h, normalize(s.coach.dir), s.coach.speed, s.coach.spin, false);
                }
                else launch(p, h, s.botShotDir, throwSpeedFromHandSpeed(sk.throwHand, p), s.botShotSpin);
                s.botThrow = 0;
                s.botSlap = false;
                s.botTimer = lerpf(sk.holdMin, sk.holdMax, rnd()) * (np == 4 ? 0.5f : 1.f);   // (four players: quicker, more to aim at)
                s.botShotsTried = 0;
                s.botShotScore = -1e9f;
                if (np == 4) s.botTarget = botPickTarget(p);   // (four players: someone, maybe someone else, next)
            }
        }
    }
    // move: the body (within a person's step, lean and duck), then each hand (never beyond its reach)
    approach(s.botBody, s.botBodyVel, bodyWant, sk.bodySpeed, sk.bodyAccel, dt);
    {
        const Vec3 R = tun.botBodyRange;
        const float lat = clampf(dot(s.botBody, right), -R.x, R.x), fw = clampf(dot(s.botBody, fwd), -0.15f, R.z);
        const Vec3 o = right * lat + fwd * fw + Vec3{0, clampf(s.botBody.y, -R.y, 0.05f), 0};
        if (length2(o - s.botBody) > 1e-10f) s.botBodyVel = {};   // (at the edge of where it can go)
        s.botBody = o;
    }
    s.in.head = headAt();
    int bump = -1;   // the fist bump (owner, 2026-10-28): one offers a fist in its ring, the other meets it there
    bool bumpClosed = false;
    if (meet && !meetBumped) {
        int oh = -1;
        float ox = 9.f;
        for (int k = 0; k < 2; ++k)
            if (pl[opp].in.hand[k].tracked && std::fabs(pl[opp].in.hand[k].grip.p.x) < ox) { ox = std::fabs(pl[opp].in.hand[k].grip.p.x); oh = k; }
        int lit = -1;   // their fist offered in their ring
        for (int k = 0; k < 2; ++k) if (meetLit(opp, k)) lit = k;
        if (lit >= 0) oh = lit;
        if (oh >= 0) {   // the nearer fist; a fist straight in front keeps the hand it has (no switching back and forth)
            const float across = dot(pl[opp].in.hand[oh].grip.p - s.in.head.p, right);
            if (std::fabs(across) > 0.08f) s.botBumpHand = across > 0 ? 1 : 0;
        }
        bump = s.botBumpHand;
        const Quat fq = q;   // the fist level, knuckles to the front
        const float waited = tun.meetTime - stateTimer;
        Vec3 at;
        if (lit >= 0 && meetLitT[opp] >= 0 && now - meetLitT[opp] > sk.reaction + 0.15f) {   // they offer: meet it, there
            at = pl[opp].in.hand[lit].grip.transform(tun.fistFront) - fq.rotate(tun.fistFront);
            bumpClosed = true;
        } else if (waited > 1.5f + sk.reaction) {   // nobody has offered (or not yet seen): it offers, its fist still in its ring
            at = meetRing(p) + fwd * 0.03f - fq.rotate(tun.fistFront);
            bumpClosed = true;
        } else {   // ready: the hand low in front, by its body
            at = s.in.head.p + fwd * 0.10f - Vec3{0, 0.55f, 0} + side(bump) * 0.15f;   // (outside its ring)
        }
        want[bump] = {fq, at};
        speed[bump] = std::min(speed[bump], 1.6f);   // a calm reach, not a punch
        carried[bump].y = 1e9f;
    }
    for (int h = 0; h < 2; ++h)   // the hands it holds still go with the body (in step with it, as its tryouts have them)
        if (carried[h].y < 1e8f) want[h].p = s.in.head.p + carried[h];
    for (int h = 0; h < 2; ++h) {
        approach(s.botHand[h], s.botHandVel[h], botInReach(p, h, want[h]), speed[h],
                 tun.botHandAccel * (speed[h] > sk.handSpeed ? 2.f : 1.f), dt);
        s.botHandQ[h] = nlerp(s.botHandQ[h], want[h].q, std::min(1.f, dt * 18.f));
        s.botHand[h] = botInReach(p, h, {s.botHandQ[h], s.botHand[h]});   // (an arm comes along with the body)
        HandInput& hi = s.in.hand[h];
        hi.grip = {s.botHandQ[h], s.botHand[h]};
        hi.trigger = (b.state == BallState::Held && b.heldHand == h) || (h == bump && bumpClosed) ? 1.f : 0.f;   // (a fist for the bump)
    }
    // a catch: its own ball, in its catcher
    const PlayerState::BotRead& c = s.botRead[1];
    if (c.plan == BOT_CATCH && (b.state == BallState::Flying || b.state == BallState::Returning) && b.instigator == p) {
        s.in.hand[c.hand].trigger = 1;
        if (length(catcherPoint(p, c.hand) - b.pos) < tun.catchHalfHeight + ballRadius(p)) {
            b.state = BallState::Held; b.heldHand = c.hand; b.vel = {}; b.spin = {};
            emit(EventType::Caught, p, c.hand, b.pos);
            s.botTimer = lerpf(sk.holdMin, sk.holdMax, rnd()) * (np == 4 ? 0.5f : 1.f);   // (four players: quicker, more to aim at)
            s.botThrow = 0; s.botShotsTried = 0; s.botShotScore = -1e9f;
            // the slap shot (owner, 2026-10-29): caught on the swing and sent straight back, no hold, no wind-up
            if (!s.coach.on && match == MatchState::InProgress && sk.slap > 0.f && rnd() < sk.slap) {   // (its shot chosen as its throws are)
                const Vec3 from = launcherPoint(p, c.hand);
                Vec3 to = chestPoint(opp) + Vec3{rnd() - 0.5f, rnd() - 0.5f, rnd() - 0.5f} * sk.aimError;
                s.botShotSpin = {};
                if (rnd() < 0.3f) {   // off a side wall
                    const float sw = (rnd() < 0.5f ? -1.f : 1.f) * 3.0f;
                    if (np == 2) to.z += sw; else to += normalize(cross(kUp, to - from)) * sw;
                } else if (rnd() < 0.6f) {
                    s.botShotSpin = kUp * ((rnd() < 0.5f ? -1.f : 1.f) * lerpf(12.f, 28.f, rnd()));
                }
                s.botShotDir = normalize(to - from);
                s.botThrow = 2;
                s.botThrowT = now;
                s.botSlap = true;
            }
        }
    }
}

void Game::startCountdown() {
    match = MatchState::Countdown;
    stateTimer = tun.countdown;
    round = 1;
    roundClock = tun.advanced ? tun.advancedRoundTime : tun.basicMatchTime;
    overtime = false;
    emit(EventType::Countdown, -1, -1, {}, tun.countdown);
}

void Game::stepMatch(float dt) {
    switch (match) {
    case MatchState::WaitingForPlayers: {
        bool all = true;
        for (int p = 0; p < np; ++p) all &= pl[p].in.headTracked;
        if (all) {
            // two people meet in the centre first; a person and the bot too when asked (practice); two bots never;
            // four players never (the centre is far from every corner)
            if (np == 2 && (pl[0].isBot != pl[1].isBot ? tun.botMeet : !pl[0].isBot) && tun.meetTime > 0) {
                match = MatchState::Meet;
                stateTimer = tun.meetTime;
            } else {
                startCountdown();
            }
        }
        break;
    }
    case MatchState::Meet: {
        stateTimer -= dt;
        bool bump = false;
        Vec3 at;
        float best = 9.f;
        auto closed = [](const HandInput& x) { return x.trigger > 0.5f || x.squeeze > 0.5f; };   // a fist
        bool offered[2];
        for (int p = 0; p < 2; ++p) {   // a fist lit in its ring: offered once it has been there meetOfferTime
            const bool lit = meetLit(p, 0) || meetLit(p, 1);
            if (!lit) meetLitT[p] = -1;
            else if (meetLitT[p] < 0) meetLitT[p] = now;
            offered[p] = meetLitT[p] >= 0 && now - meetLitT[p] >= tun.meetOfferTime;
        }
        for (int h0 = 0; h0 < 2; ++h0)
            for (int h1 = 0; h1 < 2; ++h1) {
                const HandInput& a = pl[0].in.hand[h0];
                const HandInput& b = pl[1].in.hand[h1];
                if (!a.tracked || !b.tracked) continue;
                const float gap = fistGap(0, h0, 1, h1);
                best = std::min(best, gap);
                // either fist closed is enough: in the headset the two triggers were rarely held at the same moment
                // (log 2026-10-17: fists 1-3 cm apart with triggers 1/0, 0/1)
                // only at the meeting point, and one of them offered first (nobody can force the start)
                if ((closed(a) || closed(b)) && gap < tun.fistBumpGap && inMeetRing(0, h0) && inMeetRing(1, h1) &&
                    (offered[0] || offered[1])) {
                    bump = true;
                    at = (a.grip.transform(tun.fistFront) + b.grip.transform(tun.fistFront)) * 0.5f;
                }
            }
        if (now - lastFistLog > 0.5f && best < 0.6f) {   // diagnostics while they reach for each other
            lastFistLog = now;
            const float t0 = std::max(pl[0].in.hand[0].trigger, pl[0].in.hand[1].trigger);
            const float t1 = std::max(pl[1].in.hand[0].trigger, pl[1].in.hand[1].trigger);
            meetLog = true; meetGap = best; meetTrig[0] = t0; meetTrig[1] = t1;
        }
        if (bump && !meetBumped) {   // confirm it (buzz, sound, FIST BUMP!) for a moment, then both go to their ends
            meetBumped = true;
            emit(EventType::FistBump, -1, -1, at, 0);
            stateTimer = std::min(stateTimer, tun.bumpPause);
        }
        if (stateTimer <= 0) { meetBumped = false; startCountdown(); }
        break;
    }
    case MatchState::Countdown: {
        float before = stateTimer;
        stateTimer -= dt;
        if (std::floor(before) != std::floor(stateTimer) && stateTimer > 0) emit(EventType::Countdown, -1, -1, {}, std::ceil(stateTimer));
        if (stateTimer <= 0) {
            match = MatchState::InProgress;
            emit(EventType::RoundStart, -1, -1, {}, float(round));
        }
        break;
    }
    case MatchState::InProgress:
        // the clock: Basic's match time (none in FREE PLAY), Advanced's round time when the host set one
        if (tun.timed() && !overtime) {
            roundClock -= dt;
            if (roundClock <= 0) {
                roundClock = 0;
                const int w = leader();   // the team ahead (a tie: sudden death)
                if (w >= 0) {
                    if (tun.advanced) endRound(w);   // the round goes to the one ahead
                    else {
                        matchWinner = w;
                        match = MatchState::MatchSummary;
                        emit(EventType::MatchEnd, matchWinner, -1, {}, 0);
                    }
                } else {
                    overtime = true;   // "Sudden death overtime on a tie" (and a timed round's tie: the next point)
                    emit(EventType::Overtime, -1, -1, {}, 0);
                }
            }
        }
        break;
    case MatchState::RoundSummary:
        stateTimer -= dt;
        if (stateTimer <= 0) {
            int w = pl[0].roundWins >= tun.advancedRoundsToWin ? 0 : pl[1].roundWins >= tun.advancedRoundsToWin ? 1 : -1;
            for (int p = 0; p < np; ++p) { pl[p].strikesAgainst = 0; pl[p].strikesFor = 0; spawnBall(p); }
            if (w >= 0) {
                matchWinner = w;
                match = MatchState::MatchSummary;
                emit(EventType::MatchEnd, w, -1, {}, 0);
            } else {
                round++;
                pl[0].score = pl[1].score = 0;
                overtime = false;
                roundClock = tun.advancedRoundTime;   // (a timed round starts its clock again)
                match = MatchState::Countdown;
                stateTimer = tun.countdown;
                emit(EventType::Countdown, -1, -1, {}, tun.countdown);
            }
        }
        break;
    case MatchState::MatchSummary:
        if (pl[0].in.buttonA) {
            uint32_t bots = 0;
            for (int p = 0; p < np; ++p) bots |= pl[p].isBot ? 1u << p : 0u;
            resetWithBots(bots);
        }
        break;
    }
}

void Game::step(float dt, PlayerInput* in) {
    events.clear();
    events.swap(served_);
    if (dt <= 0) return;
    dt = std::min(dt, 0.05f);
    now += dt;
    lastDt = dt;
    for (int p = 0; p < np; ++p) {
        if (pl[p].isBot) stepBot(p, dt);
        else pl[p].in = in[p];
        in[p] = pl[p].in;
    }
    stepMatch(dt);
    for (int p = 0; p < np; ++p) stepPlayer(p, dt);
    for (int p = 0; p < np; ++p) stepBall(p, dt);
}

namespace exparena {
std::vector<CollisionTri> collision(bool four) {
    std::vector<CollisionTri> out;
    auto tri = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 in, uint32_t tags) {   // in: the side its face looks to
        Vec3 n = normalize(cross(b - a, c - a));
        if (dot(n, in) < 0) { std::swap(b, c); n = -n; }
        CollisionTri t;
        t.a = a; t.b = b; t.c = c; t.n = n; t.tags = tags;
        out.push_back(t);
    };
    auto quad = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 in, uint32_t tags) { tri(a, b, c, in, tags); tri(a, c, d, in, tags); };
    const Vec3 corner[4] = {{-kCorner, 0, 0}, {0, 0, kCorner}, {kCorner, 0, 0}, {0, 0, -kCorner}};
    const int kSeg = 8;
    for (int w = 0; w < 4; ++w) {   // the walls, in strips; near a player's corner they are its back wall
        const Vec3 c0 = corner[w], c1 = corner[(w + 1) % 4];
        for (int i = 0; i < kSeg; ++i) {
            const Vec3 a = lerp(c0, c1, i / float(kSeg)), b = lerp(c0, c1, (i + 1) / float(kSeg)), mid = (a + b) * 0.5f;
            uint32_t tags = TAG_ARENA;
            if (std::fabs(mid.x) > kBackX) tags |= TAG_RETURN | (mid.x < 0 ? TAG_TEAM0 : TAG_TEAM1);
            else if (four && std::fabs(mid.z) > kBackX) tags |= TAG_RETURN | (mid.z < 0 ? TAG_TEAM2 : TAG_TEAM3);
            quad(a + Vec3{0, kFloor, 0}, b + Vec3{0, kFloor, 0}, b + Vec3{0, kCeiling, 0}, a + Vec3{0, kCeiling, 0},
                 Vec3{-mid.x, 0, -mid.z}, tags);
        }
    }
    for (int k = 0; k < 2; ++k) {   // the floor below and the ceiling
        const float y = k ? kCeiling : kFloor;
        quad(corner[0] + Vec3{0, y, 0}, corner[1] + Vec3{0, y, 0}, corner[2] + Vec3{0, y, 0}, corner[3] + Vec3{0, y, 0},
             Vec3{0, k ? -1.f : 1.f, 0}, TAG_ARENA);
    }
    const int kRim = four ? 0 : 24;   // (FOUR CORNERS: the ball goes through the platforms, as in DIAMOND)
    for (int p = 0; p < 2; ++p) {   // the platforms: the top is its player's foul zone; the rim and underside bounce
        const Vec3 c{p ? 7.f : -7.f, 0, 0};
        const uint32_t team = p ? TAG_TEAM1 : TAG_TEAM0;
        for (int i = 0; i < kRim; ++i) {
            const float a0 = i * (6.2831853f / kRim), a1 = (i + 1) * (6.2831853f / kRim);
            const Vec3 r0{std::cos(a0) * kPlatformR, 0, std::sin(a0) * kPlatformR}, r1{std::cos(a1) * kPlatformR, 0, std::sin(a1) * kPlatformR};
            const Vec3 dn{0, -kPlatformThick, 0};
            tri(c, c + r0, c + r1, {0, 1, 0}, TAG_ARENA | TAG_FOUL | team);
            tri(c + dn, c + r0 + dn, c + r1 + dn, {0, -1, 0}, TAG_ARENA);
            quad(c + r0, c + r1, c + r1 + dn, c + r0 + dn, (r0 + r1) * 0.5f, TAG_ARENA);
        }
    }
    return out;
}

void diamondEnd(Vec3 v[4]) {
    const float c = 0.70710678f, hw = 0.5f * kRollW, hh = 0.5f * kRollH;
    const float zy[4][2] = {{hw, hh}, {-hw, hh}, {-hw, -hh}, {hw, -hh}};
    // (shifted sideways so the V's bottom corner is straight under the platforms, were the end not square)
    const float shift = c * (hw - hh);
    for (int k = 0; k < 4; ++k) v[k] = {0, kRollCY + c * (zy[k][0] + zy[k][1]), c * (zy[k][0] - zy[k][1]) + shift};
}

std::vector<CollisionTri> diamondCollision(bool solidPlatforms) {
    std::vector<CollisionTri> out;
    auto tri = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 in, uint32_t tags) {   // in: the side its face looks to
        Vec3 n = normalize(cross(b - a, c - a));
        if (dot(n, in) < 0) { std::swap(b, c); n = -n; }
        CollisionTri t;
        t.a = a; t.b = b; t.c = c; t.n = n; t.tags = tags;
        out.push_back(t);
    };
    auto quad = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 in, uint32_t tags) { tri(a, b, c, in, tags); tri(a, c, d, in, tags); };
    const Vec3 ctr{0, kRollCY, 0};
    Vec3 v[4];
    diamondEnd(v);
    // along the court in strips (no foul zone in DIAMOND, owner 2026-10-29: every wall just bounces)
    std::vector<float> xs = {-kRollLen, -4.73f};
    for (float x = -4.f; x <= 4.01f; x += 1.f) xs.push_back(x);
    xs.push_back(4.73f); xs.push_back(kRollLen);
    for (size_t i = 0; i + 1 < xs.size(); ++i) {
        const float x0 = xs[i], x1 = xs[i + 1];
        const uint32_t tags = TAG_ARENA;
        for (int k = 0; k < 4; ++k) {
            const Vec3 a = v[k], b = v[(k + 1) % 4];
            const Vec3 in = ctr - (a + b) * 0.5f;
            quad(a + Vec3{x0, 0, 0}, b + Vec3{x0, 0, 0}, b + Vec3{x1, 0, 0}, a + Vec3{x1, 0, 0}, {0, in.y, in.z}, tags);
        }
    }
    for (int p = 0; p < 2; ++p) {   // the ends: the back walls
        const float x = p ? kRollLen : -kRollLen;
        const Vec3 o{x, 0, 0};
        quad(v[0] + o, v[1] + o, v[2] + o, v[3] + o, {p ? -1.f : 1.f, 0, 0}, TAG_ARENA | TAG_RETURN | (p ? TAG_TEAM1 : TAG_TEAM0));
    }
    // the platforms: the ball goes through them by default (owner, 2026-10-29: so it comes up at you from below);
    // tun.solidPlatforms puts them back (they bounce; no foul zone in DIAMOND)
    const int kRim = solidPlatforms ? 24 : 0;
    for (int p = 0; p < 2; ++p) {   // the platforms, as in CORNERS
        const Vec3 c{p ? 7.f : -7.f, 0, 0};
        for (int i = 0; i < kRim; ++i) {
            const float a0 = i * (6.2831853f / kRim), a1 = (i + 1) * (6.2831853f / kRim);
            const Vec3 r0{std::cos(a0) * kPlatformR, 0, std::sin(a0) * kPlatformR}, r1{std::cos(a1) * kPlatformR, 0, std::sin(a1) * kPlatformR};
            const Vec3 dn{0, -kPlatformThick, 0};
            tri(c, c + r0, c + r1, {0, 1, 0}, TAG_ARENA);
            tri(c + dn, c + r0 + dn, c + r1 + dn, {0, -1, 0}, TAG_ARENA);
            quad(c + r0, c + r1, c + r1 + dn, c + r0 + dn, (r0 + r1) * 0.5f, TAG_ARENA);
        }
    }
    return out;
}
}

std::vector<CollisionTri> parseCollision(const uint8_t* d, size_t size) {
    std::vector<CollisionTri> out;
    if (size < 8 || std::memcmp(d, "SCOL", 4) != 0) return out;
    uint32_t n;
    std::memcpy(&n, d + 4, 4);
    if (n > (size - 8) / 40) return out;
    for (const uint8_t* p = d + 8; n--; p += 40) {
        float v[9]; uint32_t tags;
        std::memcpy(v, p, 36); std::memcpy(&tags, p + 36, 4);
        CollisionTri t{{v[0], v[1], v[2]}, {v[3], v[4], v[5]}, {v[6], v[7], v[8]}, {}, tags};
        t.n = normalize(cross(t.b - t.a, t.c - t.a));
        out.push_back(t);
    }
    return out;
}

void StatCounter::add(const GameEvent& e) {
    auto bump = [](uint16_t& v) { if (v < 3000) ++v; };
    const int p = e.player, k = e.ball;
    if (p < 0 || p > 1) return;
    // is ball k an attack by `who`? 0 no, 1 yes (already on target), 2 yes and now on target for the first time
    auto attack = [&](int who) {
        if (k < 0 || k > 1 || shot_[k].by != who) return 0;
        if (shot_[k].counted) return 1;
        shot_[k].counted = true;
        if (shot_[k].thrown) bump(s[who].onTarget);
        return 2;
    };
    switch (e.type) {
    case EventType::Throw:   // a player throws their own ball
        bump(s[p].throws);
        s[p].topSpeed = std::max(s[p].topSpeed, e.value);
        shot_[p] = Shot{p, true, false};
        break;
    case EventType::HitPlayer: {   // player = the one hit, value = the scorer
        const int sc = (int)e.value, a = sc == 1 - p ? attack(sc) : 0;
        if (a) {
            bump(s[sc].hits);
            if (!shot_[k].thrown) bump(s[sc].returnHits);
            if (a == 2) bump(s[p].faced);
        }
        break;
    }
    case EventType::Strike:   // player = the one struck against
        bump(s[1 - p].strikes);
        if (attack(1 - p) == 2) bump(s[p].faced);
        break;
    case EventType::ShieldBlock: case EventType::KnuckleBlock:   // player = the blocker: the ball is now their return
        bump(s[p].blocks);
        bump(s[p].faced);
        attack(1 - p);
        if (k >= 0 && k < 2) shot_[k] = Shot{p, false, false};
        break;
    case EventType::Caught:
        bump(s[p].catches);
        break;
    default: break;
    }
}
