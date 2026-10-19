// Sparc gameplay simulation. See game.h for provenance conventions; rules quoted from the original's
// English text are in docs/game_text_en.txt.
#include "game.h"
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
        float toward = deflector == 0 ? 1.f : -1.f;   // +X is toward player 1's end
        float sp0 = length(out);
        Vec3 d = sp0 > 1e-4f ? out / sp0 : Vec3{toward, 0, 0};
        if (d.x * toward < 0.35f) { d.x = 0.35f * toward; d = normalize(d); }
        out = d * sp0;
    }
    float cap = tun.projectileMaxSpeed + tun.speedIncreasePerStrike * pl[1 - deflector].strikesAgainst;
    float sp = clampf(length(out), tun.minSpeed, cap);
    Vec3 dir = normalize(out), tgt = normalize(chestPoint(1 - deflector) - from);
    float ang = std::acos(clampf(dot(dir, tgt), -1.f, 1.f));
    if (ang > 1e-4f) dir = normalize(lerp(dir, tgt, std::min(1.f, deg2rad(assistDeg) / ang)));
    b.vel = dir * sp;
    b.speedLimit = sp;
    b.instigator = deflector;
    b.rechargePending = false;
    b.scored = false;
    b.spin *= 0.5f;
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
    float back = p == 0 ? -1.f : 1.f;   // behind player 0 is -X
    return tun.start[p] + Vec3{back * tun.strikeZoneBack, tun.strikeZoneHeight, 0};
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

float Game::throwSpeedFromHandSpeed(float s, int thrower) const {
    // lerp(minThrown, maxThrown + strikes * SpeedIncreasePerStrike, t)
    float t = clampf((s - tun.minInputSpeed) / std::max(1e-3f, tun.maxInputSpeed - tun.minInputSpeed), 0.f, 1.f);
    int strikes = pl[1 - thrower].strikesAgainst;
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
    case EventType::FistBump: return "FistBump";
    }
    return "?";
}

void Game::reset(bool player1IsBot) {
    for (int p = 0; p < 2; ++p) {
        PlayerState& s = pl[p];
        s = PlayerState{};
        s.isBot = (p == 1 && player1IsBot);
        s.in.head.p = tun.start[p] + Vec3{0, 1.65f, 0};
        spawnBall(p);
        s.ball.pos = idlePoint(p);
        s.botTimer = 1.0f;
    }
    match = MatchState::WaitingForPlayers;
    stateTimer = 0;
    meetBumped = false;
    round = 0;
    overtime = false;
    roundClock = tun.basicMatchTime;
    lastRoundWinner = matchWinner = -1;
    events.clear();
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
    b.scale = strikeScale(pl[1 - p].strikesAgainst);
}

float Game::strikeScale(int n) const {
    int early = std::min(n, tun.fullScaleSteps), late = std::max(0, n - tun.fullScaleSteps);
    return std::min(tun.maxBallScale, 1.f + tun.scaleIncreasePerStrike * early + tun.lateScaleIncreasePerStrike * late);
}

void Game::launch(int p, int h, Vec3 dir, float speed, Vec3 spin) {
    Ball& b = pl[p].ball;
    int opp = 1 - p;
    float assist = tun.advanced ? tun.aimCorrectAdvancedDeg : tun.aimCorrectBasicDeg;
    Vec3 to = normalize(chestPoint(opp) - b.pos);
    float ang = std::acos(clampf(dot(dir, to), -1.f, 1.f));
    if (ang > 1e-4f) dir = normalize(lerp(dir, to, std::min(1.f, deg2rad(assist) / ang)));
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
    emit(EventType::Throw, p, h, b.pos, b.speedLimit);
}

void Game::deflect(Ball& b, const Vec3& n, int deflector, float assistDeg, const Vec3& from) {
    float sp = length(b.vel);
    Vec3 r = b.vel - n * (2 * dot(b.vel, n));
    Vec3 dir = normalize(r), tgt = normalize(chestPoint(1 - deflector) - from);
    float ang = std::acos(clampf(dot(dir, tgt), -1.f, 1.f));
    if (ang > 1e-4f) dir = normalize(lerp(dir, tgt, std::min(1.f, deg2rad(assistDeg) / ang)));
    b.vel = dir * sp;
    b.instigator = deflector;      // OnRep_Instigator: scoring / zone rules follow the last deflector
    b.rechargePending = false;
    b.scored = false;
    b.spin *= 0.5f;
}

bool Game::sweepArena(const Vec3& from, const Vec3& to, float r, Vec3& hitPos, Vec3& hitN, uint32_t& tags) const {
    // Discrete test at the end point (callers sub-step finely). Picks the deepest penetration,
    // preferring tagged zones (foul/return) over plain walls when both overlap.
    float best = 1e9f;
    bool hit = false;
    Vec3 motion = to - from;
    for (const CollisionTri& t : arena) {
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
        if (dist >= r) continue;
        Vec3 n = dist > 1e-6f ? d / dist : t.n;
        if (dot(motion, n) >= 0) continue;   // already separating
        float score = dist - ((t.tags & (TAG_FOUL | TAG_RETURN)) ? 0.05f : 0.f);
        if (score < best) { best = score; hitPos = q + n * r; hitN = n; tags = t.tags; hit = true; }
    }
    return hit;
}

bool Game::hitsBody(int v, const Vec3& pos, float r) const {
    const Vec3 head = pl[v].in.head.p;
    if (length(pos - head) < tun.headRadius + r) return true;
    Vec3 a = head - Vec3{0, tun.torsoTop, 0};
    Vec3 b = head - Vec3{0, tun.torsoBottom, 0};
    if (b.y < 0.3f) b.y = 0.3f;
    Vec3 ab = b - a;
    float t = clampf(dot(pos - a, ab) / std::max(1e-6f, dot(ab, ab)), 0.f, 1.f);
    return length(pos - (a + ab * t)) < tun.torsoRadius + r;
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

void Game::scorePoint(int scorer, const Vec3& where) {
    if (match != MatchState::InProgress) return;
    int victim = 1 - scorer;
    pl[scorer].score++;
    pl[victim].strikesAgainst = 0;   // "If you get hit, any strikes against you are removed."
    emit(EventType::HitPlayer, victim, -1, where, float(scorer));
    if (!tun.advanced) {
        if (overtime) {   // sudden death
            matchWinner = scorer;
            match = MatchState::MatchSummary;
            emit(EventType::MatchEnd, scorer, -1, {}, 0);
        }
    } else if (pl[scorer].score >= tun.advancedPointsToWin) {
        lastRoundWinner = scorer;
        pl[scorer].roundWins++;
        match = MatchState::RoundSummary;
        stateTimer = tun.roundSummaryTime;
        emit(EventType::RoundEnd, scorer, -1, {}, float(round));
    }
}

void Game::addStrike(int against, const Vec3& where) {
    if (match != MatchState::InProgress) return;
    if (pl[against].strikesAgainst >= tun.maxStrikes) return;
    pl[against].strikesAgainst++;
    emit(EventType::Strike, against, -1, where, float(pl[against].strikesAgainst));
}

void Game::stepBall(int p, float dt) {
    PlayerState& s = pl[p];
    Ball& b = s.ball;
    // stepped growth: ease toward the size for the current strike count
    {
        float tgt = strikeScale(pl[1 - p].strikesAgainst);
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
    // a short throw goes home: not yet past the owner's foul line, and slow, heading back, or taking too long
    if (b.state == BallState::Flying && b.instigator == p && b.rechargePending) {
        const bool behind = p == 0 ? b.pos.x < -tun.foulLineX : b.pos.x > tun.foulLineX;
        const float forward = p == 0 ? b.vel.x : -b.vel.x;   // toward the opponent
        if (behind && (b.speedLimit < tun.recallSpeed || forward < 0 || b.timer > tun.recallTime)) {
            b.state = BallState::Idle;   // the spawner's damped spring brings it back
            b.heldHand = -1;
            b.spin = {};
            emit(EventType::ThrowFail, p, -1, b.pos, length(b.vel));
            return;
        }
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
        if (b.rechargePending && b.instigator == p && (p == 0 ? to.x > -tun.foulLineX : to.x < tun.foulLineX)) {
            b.rechargePending = false;
            if (!b.charged) emit(EventType::Recharge, p, -1, to);
            b.charged = true;
            pl[p].shotClock = tun.shotClockSeconds;                         // throw past the foul line resets it
            if (tun.useShotClock && pl[1 - p].shotClock <= 0) addStrike(1 - p, to);   // automatic strike
        }
        bool handled = b.state == BallState::Returning;   // a returning ball can't be blocked / score
        // shields and knuckle guards of the player who did NOT last touch the ball
        int q = 1 - b.instigator;
        // shields take priority over the knuckle bumpers when both are in the ball's path
        for (int pass = 0; pass < 2 && !handled; ++pass)
        for (int hh = 0; hh < 2 && !handled; ++hh) {
            Vec3 n;
            if (pass == 0 && hitsShield(q, hh, from, to, r, n)) {
                Vec3 vs = recentHandVel(q, hh, shieldLocal(hh).p, now);
                deflectMoving(b, n, vs, q, tun.advanced ? tun.shieldAimAssistAdvancedDeg : tun.shieldAimAssistBasicDeg, from);
                pl[q].ball.charged = false;            // "Shield breaks on hit": the held ball loses its charge
                pl[q].hands[hh].shieldUp = false;
                emit(EventType::ShieldBlock, q, hh, from, 0);
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
                emit(EventType::KnuckleBlock, q, hh, from, length(b.vel));
                to = from + b.vel * h;
                handled = true;
            }
        }
        // the player who did not last touch the ball gets hit
        int victim = 1 - b.instigator;
        if (b.state == BallState::Flying) {   // strike: the ball crosses the victim's strike-zone ring inside its radius
            Vec3 c = strikeZoneCenter(victim);
            if ((from.x - c.x) * (to.x - c.x) <= 0 && std::fabs(to.x - from.x) > 1e-6f) {
                float t = (c.x - from.x) / (to.x - from.x);
                Vec3 x = from + (to - from) * t;
                if (length(Vec3{0, x.y - c.y, x.z - c.z}) < tun.strikeZoneRadius) addStrike(victim, x);
            }
        }
        if (!handled && !b.scored && hitsBody(victim, to, r)) {
            b.scored = true;
            scorePoint(b.instigator, to);
            b.state = BallState::Dead;
            b.timer = tun.respawnDelay + tun.postSpawnDelay + 0.5f;
            return;
        }
        // your own ball can hit you on its way back (rebound or slow return); the opponent gets the point
        {
            int self = b.instigator;
            Vec3 toSelf = pl[self].in.head.p - to;
            if (tun.selfHitScores && !b.scored && b.timer > tun.selfHitGrace && dot(b.vel, toSelf) > 0 &&
                pl[self].in.headTracked && hitsBody(self, to, r)) {
                b.scored = true;
                scorePoint(1 - self, to);
                b.state = BallState::Dead;
                b.timer = tun.respawnDelay + tun.postSpawnDelay + 0.5f;
                return;
            }
        }
        {
            Vec3 hp, hn; uint32_t tags = 0;
            if (sweepArena(from, to, r, hp, hn, tags)) {
                int zoneTeam = (tags & TAG_TEAM0) ? 0 : (tags & TAG_TEAM1) ? 1 : -1;
                bool own = (zoneTeam >= 0 && zoneTeam == b.instigator);   // team gating
                bool returning = b.state == BallState::Returning;
                if ((tags & TAG_FOUL) && own && !returning) {
                    // DMTE_HitFoulZone: ball dissolves. Deflecting the opponent's ball into your own foul zone
                    // gives them a strike.
                    emit(EventType::Foul, b.instigator, -1, hp, float(zoneTeam));
                    if (b.instigator != p) addStrike(b.instigator, hp);
                    spawnBall(p);
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
        if (b.pos.y < -1 || b.pos.y > 6 || std::fabs(b.pos.x) > 11 || std::fabs(b.pos.z) > 4) { spawnBall(p); return; }
    }
    if (b.timer > 10.f) spawnBall(p);
}

void Game::stepPlayer(int p, float dt) {
    PlayerState& s = pl[p];
    Ball& b = s.ball;
    // shot clock runs while my ball is in my end of the court
    if (match == MatchState::InProgress && tun.useShotClock) {
        bool inMyEnd = (b.state != BallState::Flying) || (p == 0 ? b.pos.x < -tun.foulLineX : b.pos.x > tun.foulLineX);
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
        hs.shieldUp = hi.tracked && holding && b.charged;
        // in Basic the gauntlet bumpers are always active, trigger or not. (The tutorial line "Pulling the trigger will
        // deactivate the knuckle guard on that hand" belongs to the tutorial's own setup, not Brawl Basic.)
        hs.knuckleUp = !tun.advanced && hi.tracked;
        hs.prevTrigger = hi.trigger;
    }
}

void Game::stepBot(int p, float dt) {
    // Practice opponent following the same rules (shield only while holding a charged ball).
    PlayerState& s = pl[p];
    int opp = 1 - p;
    Quat q = Quat::axisAngle(kUp, deg2rad(p == 0 ? -90.f : 90.f));
    float sway = std::sin(now * 0.9f) * 0.25f;
    s.in.headTracked = true;
    s.in.head = {q, tun.start[p] + Vec3{0, 1.62f + 0.03f * std::sin(now * 2.1f), sway} + s.botLean};
    Vec3 right = q.rotate({1, 0, 0}), fwd = q.rotate({0, 0, -1});
    Vec3 chest = s.in.head.p - Vec3{0, 0.4f, 0};
    Quat handQ = q * Quat::axisAngle({1, 0, 0}, deg2rad(-20));
    for (int h = 0; h < 2; ++h) {
        HandInput& hi = s.in.hand[h];
        hi.tracked = true;
        hi.trigger = 0;
        hi.angVel = {};
        hi.grip = {handQ, chest + right * (h == 0 ? -0.25f : 0.25f) + fwd * 0.25f};
    }
    Ball& b = s.ball;
    s.botTimer -= dt;
    const int hand = 1;
    if (match == MatchState::InProgress && b.state == BallState::Idle) {
        b.state = BallState::Held; b.heldHand = hand;
        emit(EventType::Grab, p, hand, b.pos);
        s.botTimer = lerpf(tun.botHoldMin, tun.botHoldMax, rnd());
    }
    // incoming opponent ball?
    const Ball& ob = pl[opp].ball;
    bool incoming = ob.state == BallState::Flying && ob.instigator == opp && dot(ob.vel, fwd) < -0.5f;
    if (incoming) {
        if (s.botNoticeT < 0) { s.botNoticeT = now; s.botWillBlock = rnd() < tun.botBlockChance; }
    } else {
        s.botNoticeT = -1;
    }
    bool holding = b.state == BallState::Held;
    int blockHand = -1;
    if (incoming && s.botWillBlock && now - s.botNoticeT > tun.botReaction) {
        if (holding && b.charged) blockHand = b.heldHand;                 // raise the shield arm
        else if (!tun.advanced) blockHand = holding ? 1 - b.heldHand : 0;  // punch with a knuckle guard
    }
    if (holding) {
        HandInput& hi = s.in.hand[b.heldHand];
        hi.trigger = 1;
        hi.grip.p = chest + right * 0.3f + Vec3{0, 0.25f, 0} - fwd * 0.10f;
    }
    if (blockHand >= 0) {
        float planeX = chest.x + fwd.x * 0.45f;
        float tHit = (planeX - ob.pos.x) / (std::fabs(ob.vel.x) > 1e-3f ? ob.vel.x : 1e-3f);
        Vec3 cp = ob.pos + ob.vel * std::max(0.f, tHit);
        cp.y = clampf(cp.y, 0.6f, 2.1f);
        cp.z = clampf(cp.z, s.in.head.p.z - 0.7f, s.in.head.p.z + 0.7f);
        HandInput& hi = s.in.hand[blockHand];
        if (holding && blockHand == b.heldHand) {
            Vec3 n = normalize(-ob.vel);
            Quat gq = fromTo({0, 1, 0}, n);
            Vec3 curZ = gq.rotate({0, 0, 1});
            Vec3 wantZ = normalize(Vec3{0, -1, 0} - n * dot(Vec3{0, -1, 0}, n));
            gq = fromTo(normalize(curZ - n * dot(curZ, n)), wantZ) * gq;   // plate orientation in world
            Pose sl = shieldLocal(blockHand);
            Quat gripQ = gq * sl.q.conj();                                 // grip such that grip*shieldLocal = plate
            hi.grip = {gripQ, cp - gripQ.rotate(sl.p)};
            hi.trigger = 1;
        } else {
            Vec3 ko = tun.knuckleOffset;
            if (blockHand == 0) ko.x = -ko.x;
            hi.grip.p = cp - hi.grip.q.rotate(ko);
            hi.trigger = 0;
        }
    }
    // the body follows a far block (leans sideways, dips for low balls) instead of the arm stretching out, and the
    // hands move to their targets quickly rather than teleporting (owner: the bot looked stiff reaching for a ball)
    Vec3 leanWant{};
    if (blockHand >= 0) {
        const Vec3 shoulder = chest + right * (blockHand == 1 ? 0.19f : -0.19f) + Vec3{0, 0.12f, 0};
        Vec3 d = s.in.hand[blockHand].grip.p - shoulder;
        d.x *= 0.3f;   // barely along the court
        const float dist = length(d), excess = dist - 0.50f;
        if (excess > 0 && dist > 1e-4f) leanWant = d / dist * std::min(excess * 0.9f, 0.40f);
        leanWant.y = clampf(leanWant.y, -0.35f, 0.05f);
    }
    s.botLean += (leanWant - s.botLean) * std::min(1.f, dt * 7.f);
    for (int h = 0; h < 2; ++h) {
        Vec3& cur = s.botHand[h];
        if (!s.botHandSet) cur = s.in.hand[h].grip.p;
        cur += (s.in.hand[h].grip.p - cur) * std::min(1.f, dt * 20.f);
        s.in.hand[h].grip.p = cur;
    }
    s.botHandSet = true;
    if (holding && !b.charged) s.botTimer = std::min(s.botTimer, 0.35f);   // recharge by throwing
    if (holding && !incoming && s.botTimer <= 0) {
        if (match == MatchState::InProgress) {
            b.pos = launcherPoint(p, b.heldHand);
            Vec3 target = chestPoint(opp) + Vec3{(rnd() - 0.5f) * tun.botAimError, (rnd() - 0.5f) * tun.botAimError, (rnd() - 0.5f) * tun.botAimError};
            Vec3 spin{};
            if (rnd() < 0.3f) target.z += (rnd() < 0.5f ? -1.f : 1.f) * 3.0f;              // bank shot
            else if (rnd() < 0.4f) spin = kUp * ((rnd() < 0.5f ? -1.f : 1.f) * lerpf(12.f, 28.f, rnd()));   // curve
            launch(p, b.heldHand, normalize(target - b.pos), throwSpeedFromHandSpeed(tun.botHandSpeed * (0.85f + 0.3f * rnd()), p), spin);
        }
        s.botTimer = lerpf(tun.botHoldMin, tun.botHoldMax, rnd());
    }
}

void Game::startCountdown() {
    match = MatchState::Countdown;
    stateTimer = tun.countdown;
    round = 1;
    roundClock = tun.basicMatchTime;
    overtime = false;
    emit(EventType::Countdown, -1, -1, {}, tun.countdown);
}

void Game::stepMatch(float dt) {
    switch (match) {
    case MatchState::WaitingForPlayers:
        if (pl[0].in.headTracked && pl[1].in.headTracked) {
            if (!pl[0].isBot && !pl[1].isBot && tun.meetTime > 0) {   // two people: meet in the centre first
                match = MatchState::Meet;
                stateTimer = tun.meetTime;
            } else {
                startCountdown();
            }
        }
        break;
    case MatchState::Meet: {
        stateTimer -= dt;
        bool bump = false;
        Vec3 at;
        float best = 9.f;
        auto closed = [](const HandInput& x) { return x.trigger > 0.5f || x.squeeze > 0.5f; };   // a fist
        for (int h0 = 0; h0 < 2; ++h0)
            for (int h1 = 0; h1 < 2; ++h1) {
                const HandInput& a = pl[0].in.hand[h0];
                const HandInput& b = pl[1].in.hand[h1];
                if (!a.tracked || !b.tracked) continue;
                const float gap = fistGap(0, h0, 1, h1);
                best = std::min(best, gap);
                // either fist closed is enough: in the headset the two triggers were rarely held at the same moment
                // (log 2026-10-17: fists 1-3 cm apart with triggers 1/0, 0/1)
                if ((closed(a) || closed(b)) && gap < tun.fistBumpGap) {
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
        if (!tun.advanced && !overtime) {
            roundClock -= dt;
            if (roundClock <= 0) {
                roundClock = 0;
                if (pl[0].score != pl[1].score) {
                    matchWinner = pl[0].score > pl[1].score ? 0 : 1;
                    match = MatchState::MatchSummary;
                    emit(EventType::MatchEnd, matchWinner, -1, {}, 0);
                } else {
                    overtime = true;   // "Sudden death overtime on a tie"
                    emit(EventType::Overtime, -1, -1, {}, 0);
                }
            }
        }
        break;
    case MatchState::RoundSummary:
        stateTimer -= dt;
        if (stateTimer <= 0) {
            int w = pl[0].roundWins >= tun.advancedRoundsToWin ? 0 : pl[1].roundWins >= tun.advancedRoundsToWin ? 1 : -1;
            for (int p = 0; p < 2; ++p) { pl[p].strikesAgainst = 0; spawnBall(p); }
            if (w >= 0) {
                matchWinner = w;
                match = MatchState::MatchSummary;
                emit(EventType::MatchEnd, w, -1, {}, 0);
            } else {
                round++;
                pl[0].score = pl[1].score = 0;
                match = MatchState::Countdown;
                stateTimer = tun.countdown;
                emit(EventType::Countdown, -1, -1, {}, tun.countdown);
            }
        }
        break;
    case MatchState::MatchSummary:
        if (pl[0].in.buttonA) {
            bool bot = pl[1].isBot;
            reset(bot);
        }
        break;
    }
}

void Game::step(float dt, PlayerInput in[2]) {
    events.clear();
    if (dt <= 0) return;
    dt = std::min(dt, 0.05f);
    now += dt;
    for (int p = 0; p < 2; ++p) {
        if (pl[p].isBot) stepBot(p, dt);
        else pl[p].in = in[p];
        in[p] = pl[p].in;
    }
    stepMatch(dt);
    for (int p = 0; p < 2; ++p) stepPlayer(p, dt);
    for (int p = 0; p < 2; ++p) stepBall(p, dt);
}
