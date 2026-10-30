// TRAINING: the lessons, judging a ball against the targets, the coach's shots, and a lesson as it is played.
#include "training.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace training {

// a target: a disc facing you, anywhere in the court (on the far wall: x = kWallX)
static TargetDef T(float x, float y, float z, float r) { return {{x, y, z}, r, {}, 0.f, 0.f, 0.f, 0.f}; }
// CURVE: the target on the far wall, its ring half way down the court (both at one height)
static TargetDef ringT(float y, float z, float hz) { return {{kWallX, y, z}, 0.55f, {1.0f, y, hz}, 0.5f, 0.f, 0.f, 0.f}; }
// MOVING: a disc at depth x, sliding across the court
static TargetDef slideT(float x, float y, float r, float amp, float w, float ph) { return {{x, y, 0}, r, {}, 0.f, amp, w, ph}; }

const char* trackName(int t) { return t == T_THROW ? "THROWING" : t == T_BLOCK ? "BLOCKING" : "DODGING"; }

static Lesson mk(uint8_t id, Track track, const char* name, const char* teach, const char* tip1, const char* tip2, Kind kind,
                 Guard guard, int budget, int pass) {
    Lesson l{};
    l.id = id; l.track = track; l.name = name; l.teach = teach; l.tip1 = tip1; l.tip2 = tip2;
    l.kind = kind; l.guard = guard; l.budget = budget; l.pass = pass; l.atOnce = 0; l.aim = AIM_CHEST;
    return l;
}

const std::vector<Lesson>& lessons() {
    static const std::vector<Lesson> L = [] {
        std::vector<Lesson> v;
        Lesson l;
        // ---- THROWING: targets at every depth, a few up at a time, appearing as others go down
        l = mk(1, T_THROW, "FIRST THROWS", "TAKE YOUR BALL, SWING AND LET GO",
               "PULL THE TRIGGER AT YOUR BALL TO TAKE IT.", "SWING AT THE TARGET AND LET GO OF THE TRIGGER.", THROW, G_NONE, 15, 0);
        l.targets = {T(-1.0f, 1.5f, 0.0f, 0.55f), T(3.0f, 1.6f, -1.1f, 0.55f), T(6.0f, 1.4f, 1.1f, 0.55f), T(kWallX, 1.8f, 0.0f, 0.55f),
                     T(2.0f, 2.3f, 0.9f, 0.55f)};
        l.atOnce = 1; v.push_back(l);
        l = mk(2, T_THROW, "PICK YOUR SPOT", "NEAR AND FAR, HIGH AND LOW",
               "YOUR BALL GOES WHERE YOUR HAND IS GOING AS YOU LET GO.", "FINISH THE SWING POINTING AT THE TARGET.", THROW, G_NONE, 22, 0);
        l.targets = {T(0.0f, 0.9f, -1.5f, 0.38f), T(4.0f, 2.8f, 1.6f, 0.38f), T(kWallX, 0.8f, 1.9f, 0.38f), T(2.0f, 1.6f, 0.0f, 0.38f),
                     T(6.5f, 3.0f, -1.8f, 0.38f), T(-1.5f, 2.6f, 1.2f, 0.38f), T(kWallX, 3.2f, -0.8f, 0.38f), T(5.0f, 0.8f, 0.4f, 0.38f)};
        l.atOnce = 2; v.push_back(l);
        l = mk(3, T_THROW, "POWER", "THROW HARD",
               "THESE BREAK ONLY FOR A FAST BALL: 11 METRES A SECOND.", "SWING FROM THE SHOULDER AND FOLLOW THROUGH.", THROW, G_NONE, 15, 0);
        l.targets = {T(kWallX, 1.6f, 0.0f, 0.45f), T(7.0f, 2.4f, -1.2f, 0.45f), T(kWallX, 1.0f, 1.6f, 0.45f), T(5.0f, 1.8f, 1.4f, 0.45f),
                     T(kWallX, 2.6f, -1.6f, 0.45f)};
        l.minSpeed = 11.f; l.atOnce = 2; v.push_back(l);
        l = mk(4, T_THROW, "BANK SHOTS", "PLAY THE WALLS",
               "THESE COUNT ONLY OFF A SIDE WALL.", "AIM AT THE WALL: THE BALL LEAVES IT AT THE ANGLE IT CAME IN.", THROW, G_NONE, 16, 0);
        l.targets = {T(kWallX, 1.5f, -1.9f, 0.45f), T(6.0f, 1.6f, 2.0f, 0.45f), T(kWallX, 2.7f, 1.9f, 0.45f), T(4.0f, 2.2f, -2.0f, 0.45f)};
        l.bank = true; l.atOnce = 2; v.push_back(l);
        l = mk(5, T_THROW, "CURVE", "BEND YOUR THROW",
               "THROUGH THE RING, THEN INTO ITS TARGET.", "TWIST YOUR WRIST AS YOU LET GO: THE BALL BENDS THE WAY IT SPINS.", THROW, G_NONE, 16, 0);
        l.targets = {ringT(1.5f, 0.3f, -0.6f), ringT(1.5f, -0.3f, 0.6f), ringT(2.3f, 0.5f, -0.5f), ringT(2.3f, -0.5f, 0.5f)};
        l.atOnce = 1; v.push_back(l);
        l = mk(6, T_THROW, "MOVING TARGETS", "LEAD THE TARGET",
               "THROW WHERE THE TARGET WILL BE.", "WATCH ITS RHYTHM, THEN LET GO.", THROW, G_NONE, 18, 0);
        l.targets = {slideT(kWallX, 1.5f, 0.45f, 1.5f, 0.9f, 0), slideT(4.0f, 2.2f, 0.45f, 1.5f, 1.1f, 1), slideT(kWallX, 1.0f, 0.45f, 1.6f, 1.2f, 2),
                     slideT(6.0f, 2.8f, 0.42f, 1.4f, 1.4f, 3), slideT(2.0f, 1.6f, 0.42f, 1.6f, 1.6f, 4), slideT(kWallX, 1.3f, 0.40f, 1.6f, 1.8f, 5)};
        l.atOnce = 1; v.push_back(l);
        l = mk(7, T_THROW, "STRIKE ZONE", "EARN STRIKES",
               "A BALL THROUGH THE RING BEHIND THE OTHER PLAYER IS A STRIKE.", "STRIKES MAKE YOUR BALL BIGGER AND FASTER. GO PAST THE BODY.", STRIKES, G_NONE, 15, 5);
        v.push_back(l);
        l = mk(8, T_THROW, "CATCH", "CATCH YOUR BALL",
               "THROW AT THE FAR WALL: YOUR BALL COMES BACK TO YOU.", "HOLD THE TRIGGER AND MEET IT WITH YOUR HAND.", CATCH, G_NONE, 8, 5);
        v.push_back(l);
        // ---- BLOCKING AND DEFLECTING: the far end throws at you
        l = mk(21, T_BLOCK, "SHIELD", "BLOCK WITH YOUR SHIELD",
               "HOLD YOUR BALL: ITS CHARGE POWERS THE SHIELD ON THAT HAND.", "A BLOCK USES THE CHARGE. IN A MATCH, THROW TO RECHARGE.", DEFEND, G_SHIELD, 10, 7);
        l.serveSpeed = 7.f; l.serveSpeedUp = 0.3f; l.aim = AIM_CHEST; v.push_back(l);
        l = mk(22, T_BLOCK, "SHIELD ANGLES", "HIGH, LOW, LEFT AND RIGHT",
               "THE THROWS GO WIDE OF YOUR BODY NOW: REACH THE SHIELD OUT.", "WATCH THE BALL INTO THE SHIELD.", DEFEND, G_SHIELD, 10, 7);
        l.serveSpeed = 8.f; l.serveSpeedUp = 0.3f; l.aim = AIM_WIDE; v.push_back(l);
        l = mk(23, T_BLOCK, "KNUCKLE GUARDS", "PUNCH IT AWAY",
               "THE GLASS ON YOUR FISTS SENDS A BALL BACK. NO BALL NEEDED.", "PUT A FIST IN ITS WAY, OR PUNCH IT FOR MORE SPEED.", DEFEND, G_KNUCKLE, 10, 7);
        l.serveSpeed = 7.f; l.serveSpeedUp = 0.35f; l.aim = AIM_CHEST; v.push_back(l);
        l = mk(24, T_BLOCK, "SHIELD OR FIST", "WHATEVER IS NEAREST",
               "SHIELD AND KNUCKLE GUARDS BOTH ON: MEET EACH THROW WITH EITHER.", "THEY COME FASTER AND FROM EVERY SIDE.", DEFEND, G_EITHER, 12, 9);
        l.serveSpeed = 8.5f; l.serveSpeedUp = 0.3f; l.aim = AIM_MIXED; v.push_back(l);
        l = mk(25, T_BLOCK, "SEND IT BACK", "DEFENSE INTO ATTACK",
               "BLOCK OR PUNCH THE THROWS INTO THE TARGETS.", "TURN YOUR SHIELD OR FIST TOWARD THE TARGET.", DEFEND, G_SENDBACK, 15, 0);
        l.targets = {T(kWallX, 2.2f, -1.7f, 0.55f), T(kWallX, 2.2f, 1.7f, 0.55f), T(kWallX, 3.2f, 0, 0.55f)};
        l.serveSpeed = 7.f; v.push_back(l);
        // ---- DODGING: no shield, no guards
        l = mk(41, T_DODGE, "SIDESTEP", "STEP OUT OF THE WAY",
               "NO SHIELD AND NO GUARDS: THE THROWS COME AT YOUR BODY.", "ONE STEP TO THE SIDE AS IT COMES.", DEFEND, G_DODGE, 10, 7);
        l.serveSpeed = 6.f; l.serveSpeedUp = 0.2f; l.aim = AIM_CHEST; v.push_back(l);
        l = mk(42, T_DODGE, "DUCK", "GET UNDER IT",
               "THESE COME AT YOUR HEAD.", "BEND YOUR KNEES AND DROP: THE BALL GOES OVER.", DEFEND, G_DODGE, 10, 7);
        l.serveSpeed = 6.5f; l.serveSpeedUp = 0.25f; l.aim = AIM_HEAD; v.push_back(l);
        l = mk(43, T_DODGE, "LEAN", "LET IT PAST YOUR SHOULDER",
               "THESE COME AT ONE SHOULDER, THEN THE OTHER.", "LEAN AWAY FROM THE BALL AND KEEP YOUR FEET.", DEFEND, G_DODGE, 10, 7);
        l.serveSpeed = 6.5f; l.serveSpeedUp = 0.3f; l.aim = AIM_SHOULDERS; v.push_back(l);
        l = mk(44, T_DODGE, "EVERYTHING", "STEP, DUCK OR LEAN",
               "BODY, HEAD, SHOULDERS: ANY OF THEM, FASTER.", "WATCH THE THROWER'S HAND, THEN MOVE.", DEFEND, G_DODGE, 12, 8);
        l.serveSpeed = 7.5f; l.serveSpeedUp = 0.35f; l.aim = AIM_MIXED; v.push_back(l);
        return v;
    }();
    return L;
}

void refreshLive(const Lesson& L, std::vector<Target>& T) {
    int up = 0;
    for (Target& x : T) {
        x.live = x.down || L.atOnce <= 0 || up < L.atOnce;   // (a target taken stays shown: it bursts)
        if (!x.down && x.live) ++up;
    }
}

int nextInTrack(int index) {
    const auto& L = lessons();
    for (int i = index + 1; i < (int)L.size(); ++i) if (L[i].track == L[index].track) return i;
    return -1;
}
int placeInTrack(int index, int* count) {
    const auto& L = lessons();
    int place = 0, n = 0;
    for (int i = 0; i < (int)L.size(); ++i)
        if (L[i].track == L[index].track) { ++n; if (i <= index) ++place; }
    if (count) *count = n;
    return place;
}

int lessonById(int id) {
    for (size_t i = 0; i < lessons().size(); ++i) if (lessons()[i].id == id) return (int)i;
    return -1;
}

static float segPoint(const Vec3& a, const Vec3& b, const Vec3& p) {
    const Vec3 ab = b - a;
    const float l2 = dot(ab, ab);
    const float t = l2 > 1e-12f ? clampf(dot(p - a, ab) / l2, 0.f, 1.f) : 0.f;
    return length(a + ab * t - p);
}

int judge(const Lesson& L, std::vector<Target>& T, Flight& f, const Ball& b, float ballR, const Vec3& to, float t,
          std::vector<CueEvent>* cues) {
    if (!f.on) return -1;
    const Vec3 from = f.prev;
    f.prev = to;
    if (b.bounces > f.bounces) {   // a bounce on the way: off a side wall?
        if (std::max(std::fabs(from.z), std::fabs(to.z)) > kSideZ - ballR - 0.35f) f.banked = true;
        f.bounces = b.bounces;
    }
    for (size_t i = 0; i < T.size(); ++i) {   // through a ring (it faces along the court)
        const Target& x = T[i];
        if (!x.live || x.down || x.d.hoopR <= 0) continue;
        const float hx = x.d.hoop.x;
        if ((from.x - hx) * (to.x - hx) > 0 || std::fabs(to.x - from.x) < 1e-6f) continue;
        const Vec3 c = lerp(from, to, (hx - from.x) / (to.x - from.x));
        if (length(Vec3{0, c.y - x.d.hoop.y, c.z - x.d.hoop.z}) < x.d.hoopR - 0.3f * ballR && f.hoop != (int)i) {
            f.hoop = (int)i;
            if (cues) cues->push_back({Cue::Ring, c, (int)i});
        }
    }
    for (size_t i = 0; i < T.size(); ++i) {
        Target& x = T[i];
        if (!x.live || x.down) continue;
        const Vec3 c = x.at(t);
        if (segPoint(from, to, c) > x.d.r + 0.6f * ballR) continue;
        Cue why = Cue::TargetDown;
        if (L.minSpeed > 0 && b.speedLimit < L.minSpeed) why = Cue::TooSlow;
        else if (L.bank && !f.banked) why = Cue::NotBanked;
        else if (x.d.hoopR > 0 && f.hoop != (int)i) why = Cue::NoRing;
        if (why == Cue::TargetDown) return (int)i;
        if (cues && !(f.told & (1u << i))) cues->push_back({why, c, (int)i});
        f.told |= 1u << i;
    }
    return -1;
}

// a side wall's mirror of a point (the ball's centre bounces a radius off the wall)
static Vec3 mirrored(const Vec3& p, float side) {
    const float wall = side * (kSideZ - 0.1f);
    return {p.x, p.y, 2.f * wall - p.z};
}

std::vector<Vec3> aimPoints(const Lesson& L, const std::vector<Target>& T, float t) {
    std::vector<Vec3> out;
    for (const Target& x : T) {
        if (!x.live || x.down) continue;
        if (x.d.hoopR > 0) out.push_back(x.d.hoop);   // (CURVE: toward the ring; the spin does the rest)
        else if (L.bank) { out.push_back(mirrored(x.at(t), 1.f)); out.push_back(mirrored(x.at(t), -1.f)); }
        else out.push_back(x.at(t));
    }
    return out;
}

Shot planShot(const Game& g0, int p, const Vec3& from, const Lesson& L, const std::vector<Target>& T, int i, float t0,
              bool assist) {
    Game g = g0;
    g.aimAt[p] = assist ? aimPoints(L, T, t0) : std::vector<Vec3>{};
    const float speed = L.minSpeed > 0 ? L.minSpeed + 2.f : 11.5f;
    const TargetDef& d = T[i].d;
    std::vector<Target> only = T;
    for (Target& x : only) x.live = false;
    only[i].live = true;
    only[i].down = false;
    std::vector<BallPathPt> path;
    auto dirOf = [](float yaw, float pitch) { return Vec3{std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw)}; };
    // how a throw goes: 0 it takes the target; otherwise how far off it was (metres, and more for a missed rule)
    int evals = 0;
    auto eval = [&](float yaw, float pitch, float spin) {
        ++evals;
        g.trialFlight(p, from, dirOf(yaw, pitch), speed, {0, spin, 0}, assist, 2.5f, 1.f / 72, path);
        std::vector<Target> tt = only;
        Flight f;
        f.start(path[0].ball);
        f.prev = path[0].pos;
        float miss = 1e9f, ringMiss = 1e9f;
        for (size_t k = 1; k < path.size(); ++k) {
            const float tk = t0 + (path[k].t - path[0].t);
            const float R = g.tun.projectileRadius * path[k].ball.scale;
            const Vec3 a = f.prev;
            if (judge(L, tt, f, path[k].ball, R, path[k].pos, tk, nullptr) == i) return 0.f;
            miss = std::min(miss, segPoint(a, path[k].pos, tt[i].at(tk)) - d.r);
            if (d.hoopR > 0) ringMiss = std::min(ringMiss, segPoint(a, path[k].pos, d.hoop) - (d.hoopR - 0.1f));
        }
        float m = std::max(0.f, miss);
        if (L.bank && !f.banked) m += 2.f;
        if (d.hoopR > 0 && f.hoop != i) m += 1.f + std::max(0.f, ringMiss);
        return std::max(0.001f, m);
    };
    struct Guess { float yaw, pitch, spin; };
    std::vector<Guess> guesses;
    auto toward = [&](const Vec3& at, float spin) {
        const Vec3 v = at - from;
        guesses.push_back({std::atan2(v.z, v.x), std::atan2(v.y, std::sqrt(v.x * v.x + v.z * v.z)), spin});
    };
    if (d.hoopR > 0) {   // a circle through you, the ring and the target (seen from above): its bend is the spin
        const float ax = from.x, az = from.z, bx = d.hoop.x, bz = d.hoop.z, cx = d.pos.x, cz = d.pos.z;
        const float D = 2.f * (ax * (bz - cz) + bx * (cz - az) + cx * (az - bz));
        if (std::fabs(D) > 1e-6f) {
            const float a2 = ax * ax + az * az, b2 = bx * bx + bz * bz, c2 = cx * cx + cz * cz;
            const float ux = (a2 * (bz - cz) + b2 * (cz - az) + c2 * (az - bz)) / D;
            const float uz = (a2 * (cx - bx) + b2 * (ax - cx) + c2 * (bx - ax)) / D;
            const float R = std::sqrt((ax - ux) * (ax - ux) + (az - uz) * (az - uz));
            Vec3 tan{-(az - uz), 0, ax - ux};   // perpendicular to the radius, toward the ring
            if (tan.x * (bx - ax) + tan.z * (bz - az) < 0) tan = -tan;
            const float s = 1.1f * speed / (R * g.tun.magnusFactor);   // (a little more: the spin dies away in flight)
            const float yaw = std::atan2(tan.z, tan.x), pitch = std::atan2(d.pos.y - from.y, cx - ax);
            guesses.push_back({yaw, pitch, s});
            guesses.push_back({yaw, pitch, -s});
        }
    } else if (L.bank) {
        toward(mirrored(d.pos, 1.f), 0);
        toward(mirrored(d.pos, -1.f), 0);
    } else {   // straight at it (moving: where it will be when the ball gets there)
        Vec3 at = T[i].at(t0);
        for (int k = 0; k < 3; ++k) at = T[i].at(t0 + length(at - from) / speed);
        toward(at, 0);
    }
    Shot best;
    best.speed = speed;
    float bestE = 1e9f;
    Guess bg{};
    for (const Guess& q : guesses) {
        const float e = eval(q.yaw, q.pitch, q.spin);
        if (e < bestE) { bestE = e; bg = q; }
        if (e == 0.f) break;
    }
    // then a pattern search round the best guess: yaw, pitch and spin a step either way, the steps halving
    float sy = deg2rad(1.5f), sp = deg2rad(1.5f), ss = d.hoopR > 0 ? 3.f : 0.f;
    while (bestE > 0.f && evals < 160 && sy > deg2rad(0.04f)) {
        bool better = false;
        const Guess tries[6] = {{bg.yaw + sy, bg.pitch, bg.spin}, {bg.yaw - sy, bg.pitch, bg.spin}, {bg.yaw, bg.pitch + sp, bg.spin},
                                {bg.yaw, bg.pitch - sp, bg.spin}, {bg.yaw, bg.pitch, bg.spin + ss}, {bg.yaw, bg.pitch, bg.spin - ss}};
        for (int k = 0; k < (ss > 0 ? 6 : 4) && bestE > 0.f; ++k) {
            const float e = eval(tries[k].yaw, tries[k].pitch, tries[k].spin);
            if (e < bestE) { bestE = e; bg = tries[k]; better = true; }
        }
        if (!better) { sy *= 0.5f; sp *= 0.5f; ss *= 0.5f; }
    }
    best.dir = dirOf(bg.yaw, bg.pitch);
    best.spin = {0, bg.spin, 0};
    best.ok = bestE == 0.f;
    return best;
}

// ------------------------------------------------------------------ bests
bool Bests::unlocked(int index) const {   // the first of each track, and each one after a passed lesson of its track
    const auto& L = lessons();
    if (index < 0 || index >= (int)L.size()) return false;
    for (int i = index - 1; i >= 0; --i) if (L[i].track == L[index].track) return b[L[i].id].passed;
    return true;
}
bool Bests::add(const Result& r) {
    if (!r.passed || r.lesson < 0 || r.lesson >= (int)lessons().size()) return false;
    const Lesson& L = lessons()[r.lesson];
    Best& x = b[L.id];
    bool better = !x.passed;
    x.passed = true;
    if (L.timed() && r.time > 0 && (x.time <= 0 || r.time < x.time)) { x.time = r.time; better = true; }
    if (x.attempts == 0 || r.permille() > x.permille() || (r.permille() == x.permille() && r.successes > x.successes)) {
        x.successes = r.successes; x.attempts = r.attempts; better = true;
    }
    return better;
}
std::string Bests::save() const {
    std::string s;
    char buf[64];
    for (int id = 0; id < 256; ++id)
        if (b[id].passed) {
            std::snprintf(buf, sizeof buf, "%d %d %.3f %d %d\n", id, 1, b[id].time, b[id].successes, b[id].attempts);
            s += buf;
        }
    return s;
}
void Bests::load(const std::string& s) {
    std::istringstream in(s);
    int id, passed, succ, att;
    float time;
    while (in >> id >> passed >> time >> succ >> att)
        if (id >= 0 && id < 256 && att >= 0 && att < 1000 && succ >= 0 && succ <= att && time >= 0 && time < 36000)
            b[id] = Best{passed != 0, time, succ, att};
}
int Bests::total() const {
    int t = 0;
    for (const Lesson& L : lessons()) t += b[L.id].permille();
    return t;
}

// ------------------------------------------------------------------ a lesson
bool Trainer::coachThere() const { return lesson >= 0 && (L().kind == STRIKES || L().kind == DEFEND); }

Vec3 Trainer::coachRelease(const Game& g, int p) const {   // where its throws leave its right hand
    const Vec3 f = g.facing(p), right = cross(f, Vec3{0, 1, 0});
    return g.tun.start[p] + Vec3{0, 1.325f, 0} + right * 0.18f + f * 0.41f;
}

static void makeCoach(Game& g, int p, bool watch, const Vec3& from, float speed) {
    PlayerState& s = g.pl[p];
    s.isBot = true;
    s.botLevel = kBotLevels - 1;   // (its blocks, dodges and catches as good as they come)
    s.coach = PlayerState::Coach{};
    s.coach.on = true;
    s.coach.watch = watch;
    s.coach.from = from;
    s.coach.speed = speed;
    s.coach.dir = {p == 0 ? 1.f : -1.f, 0, 0};
}

void Trainer::setRules(Game& g, bool atFar) const {
    Tunables& t = g.tun;
    t = saved_;
    t.advanced = false;
    t.experimental = false;
    t.players = 2;
    t.basicMatchTime = 0;   // (no clock: the lesson keeps its own)
    t.useShotClock = false;
    t.meetTime = 0;
    t.botMeet = false;
    t.botAdapt = false;
    const Lesson& l = L();
    t.shields = l.guard != G_KNUCKLE && l.guard != G_DODGE;
    t.knuckleMode = l.guard == G_SHIELD || l.guard == G_DODGE || l.kind == STRIKES ? 0 : 1;   // (G_EITHER: both on)
    if (!atFar) t.start[1].y = -50.f;   // (nobody at the far end: no body to hit, no ring)
}

void Trainer::begin(Game& g, int idx, const std::vector<CollisionTri>& classic) {
    if (!hadSaved_) { saved_ = g.tun; hadSaved_ = true; }
    lesson = std::max(0, std::min((int)lessons().size() - 1, idx));
    phase = Phase::Card;
    const Lesson& l = L();
    targets.clear();
    for (const TargetDef& d : l.targets) targets.push_back({d});
    refreshLive(l, targets);
    successes = attempts = throws = 0;
    clock = 0;
    result = Result{};
    line.clear();
    setRules(g, coachThere());
    g.useArena(ARENA_CLASSIC, classic);
    g.resetWithBots(coachThere() ? 2u : 0u);
    if (coachThere()) makeCoach(g, 1, false, coachRelease(g, 1), l.serveSpeed > 0 ? l.serveSpeed : 8.f);
    else g.parkBall(1);
    if (l.kind == STRIKES) g.parkBall(1);
    g.match = MatchState::InProgress;   // (warming up while the card shows: nothing counts)
    flight_[0] = flight_[1] = Flight{};
    serve_ = 0; serving_ = false; serveT_ = waitT_ = 0; rechargeT_ = respawnT_ = -1; caughtArmed_ = false;
}

// ------------------------------------------------------------------ a person's throw, by the hands
static float ease(float u) { u = clampf(u, 0.f, 1.f); return u * u * (3.f - 2.f * u); }
static Quat along(const Vec3& d) {   // a controller pointing along d (its -Z), level
    return Quat::axisAngle({0, 1, 0}, std::atan2(-d.x, -d.z)) * Quat::axisAngle({1, 0, 0}, std::asin(clampf(d.y, -1.f, 1.f)));
}

void Thrower::plan(const Game& g, int p, const Lesson& L, const std::vector<Target>& T, float clock) {
    const Vec3 f = g.facing(p), right = cross(f, Vec3{0, 1, 0});
    const Vec3 stand = g.tun.start[p] + Vec3{0, 1.65f, 0}, shoulder = stand + Vec3{0, -0.30f, 0} + right * 0.19f;
    const float S = L.kind == CATCH ? 10.f : L.minSpeed > 0 ? L.minSpeed + 2.f : 11.5f;   // (as planShot plans it)
    float lo = 0.3f, hi = g.tun.maxInputSpeed;   // the hand speed that throws it so fast
    for (int k = 0; k < 30; ++k) { const float m = 0.5f * (lo + hi); (g.throwSpeedFromHandSpeed(m, p) < S ? lo : hi) = m; }
    vh_ = 0.5f * (lo + hi);
    const float dA = 0.2f;
    ta_ = 2.f * dA / vh_;
    tc_ = 0.18f;   // (steady through the game's 0.15 s window: the ball leaves as the hand goes)
    target = -1;
    Vec3 point;
    if (L.kind == THROW) {
        for (int i = 0; i < (int)T.size() && target < 0; ++i) if (T[i].live && !T[i].down) target = i;
        if (target < 0) target = 0;
        point = T[target].at(clock);
    } else if (L.kind == STRIKES) point = g.strikeZoneCenter(1) + Vec3{0, 0.02f, (reps & 1) ? 0.45f : -0.45f};
    else point = {9.5f, 1.7f, (reps & 1) ? 0.4f : -0.4f};
    dir_ = normalize(point - (shoulder + f * 0.45f));
    spin_ = {};
    for (int it = 0; it < 2; ++it) {
        rel_ = shoulder + dir_ * 0.45f;
        q_ = along(dir_);
        const Vec3 from = rel_ - dir_ * (vh_ / 72.f) + q_.rotate(g.tun.holdOffset);   // (where the ball is as it leaves)
        if (L.kind == THROW && !T.empty()) {
            const Shot s = planShot(g, p, from, L, T, target, clock + 0.45f + ta_ + tc_, true);
            dir_ = s.dir; spin_ = s.spin;
        } else {
            dir_ = normalize(point - from);
        }
    }
    back_ = rel_ - dir_ * (dA + vh_ * tc_);
}

PlayerInput Thrower::step(const Game& g, int p, const Lesson& L, const std::vector<Target>& T, float clock, float dt) {
    const Ball& b = g.pl[p].ball;
    const Vec3 f = g.facing(p), right = cross(f, Vec3{0, 1, 0});
    const Quat face = along(f);
    const Vec3 stand = g.tun.start[p] + Vec3{0, 1.65f, 0};
    const Vec3 restR = stand + f * 0.12f + Vec3{0, -0.72f, 0} + right * 0.26f, restL = stand + f * 0.12f + Vec3{0, -0.72f, 0} - right * 0.26f;
    const Quat restQ = face * Quat::axisAngle({1, 0, 0}, deg2rad(-35.f));
    PlayerInput in;
    in.headTracked = true;
    in.head = {face, stand};
    in.hand[0].tracked = in.hand[1].tracked = true;
    in.hand[0].grip = {restQ, restL};
    t += dt;
    Vec3 pos = restR;
    Quat q = restQ;
    float trig = 0;
    Vec3 ang{};
    auto go = [&](int ph) { phase = ph; t = 0; };
    switch (phase) {
    case 0:   // at rest: then reach for the ball on its spawner
        caption = "1  REACH FOR YOUR BALL";
        if (t > 0.6f && b.state == BallState::Idle) go(1);
        break;
    case 1: {
        const Vec3 at = b.pos - restQ.rotate(g.tun.launcherOffset);
        pos = lerp(restR, at, ease(t / 0.6f));
        trig = t >= 0.6f ? 1.f : 0.f;
        caption = t < 0.6f ? "1  REACH FOR YOUR BALL" : "2  PULL THE TRIGGER TO TAKE IT";
        if (b.state == BallState::Held) { hold_ = pos; go(2); }
        else if (t > 1.6f) go(0);
        break;
    }
    case 2:   // holding it (the trigger stays held)
        pos = hold_; trig = 1;
        caption = "2  KEEP THE TRIGGER HELD: THE BALL STAYS IN YOUR HAND";
        if (t > 0.35f) { plan(g, p, L, T, clock); go(3); }
        break;
    case 3:   // swing back
        pos = lerp(hold_, back_, ease(t / 0.45f)); q = nlerp(restQ, q_, ease(t / 0.45f)); trig = 1;
        caption = "3  SWING YOUR ARM BACK";
        if (t >= 0.45f) go(4);
        break;
    case 4: {   // sweep it through: speeding up, then steady as the trigger is let go
        const float s = t < ta_ ? 0.5f * (vh_ / ta_) * t * t : 0.5f * vh_ * ta_ + vh_ * (t - ta_);
        pos = back_ + dir_ * s; q = q_; trig = 1; ang = spin_;
        caption = L.minSpeed > 0 ? "4  SWEEP IT THROUGH - FAST!" : spin_.y != 0 ? "4  SWEEP THROUGH AND TWIST YOUR WRIST"
                : L.bank ? "4  SWEEP THROUGH TOWARD THE SIDE WALL" : L.kind == STRIKES ? "4  SWEEP THROUGH, PAST THE BODY"
                : L.kind == CATCH ? "4  SWEEP THROUGH TOWARD THE FAR WALL" : "4  SWEEP THROUGH TOWARD THE TARGET";
        if (t >= ta_ + tc_) { trig = 0; follow_ = pos; go(5); }
        break;
    }
    case 5:   // let go, and follow through
        pos = follow_ + dir_ * (0.3f * (1.f - (1.f - clampf(t / 0.35f, 0.f, 1.f)) * (1.f - clampf(t / 0.35f, 0.f, 1.f)))); q = q_;
        caption = "5  LET GO OF THE TRIGGER AS YOUR HAND COMES FORWARD";
        if (t >= 0.35f) { follow_ = pos; go(6); }
        break;
    case 6:   // back to rest, watching the ball
        pos = lerp(follow_, restR, ease(t / 0.8f)); q = nlerp(q_, restQ, ease(t / 0.8f));
        caption = "AND WATCH IT GO";
        if (L.kind == CATCH && b.state == BallState::Returning) { follow_ = pos; go(7); }
        else if (t > 1.6f && b.state != BallState::Flying && b.state != BallState::Returning) { ++reps; go(0); }
        else if (t > 5.f) { ++reps; go(0); }
        break;
    case 7: {   // CATCH: the trigger held, the hand where the ball will pass in front of you
        const Vec3 m0 = stand + f * 0.5f + Vec3{0, -0.3f, 0} + right * 0.15f;
        const Vec3 v = length(b.vel) > 1e-3f ? normalize(b.vel) : -f;
        const Vec3 c = b.pos + v * dot(m0 - b.pos, v);
        const Vec3 want = c - restQ.rotate(g.tun.catcherOffset);
        const Vec3 d = want - follow_;
        const float step = 3.5f * dt;
        follow_ = length(d) > step ? follow_ + normalize(d) * step : want;
        pos = follow_; trig = 1;
        caption = "HOLD THE TRIGGER AND MEET IT WITH YOUR HAND";
        if (b.state == BallState::Held) { hold_ = pos; ++reps; go(2); }
        else if (b.state != BallState::Returning) { ++reps; go(0); }
        break;
    }
    }
    in.hand[1].grip = {q, pos};
    in.hand[1].trigger = trig;
    in.hand[1].angVel = ang;
    return in;
}

void Trainer::start(Game& g) {
    if (phase == Phase::Off) return;
    const Lesson& l = L();
    for (Target& x : targets) { x.down = false; x.downT = -1; }
    refreshLive(l, targets);
    successes = attempts = throws = 0;
    clock = 0;
    result = Result{};
    flight_[0] = flight_[1] = Flight{};
    serve_ = 0; serving_ = false; serveT_ = waitT_ = 0; rechargeT_ = respawnT_ = -1; caughtArmed_ = false;
    if (coachThere()) {   // its ball back on its spawner, nothing of its in the air
        g.pl[1].coach.fire = false;
        if (l.kind == STRIKES) g.parkBall(1); else if (g.pl[1].ball.state != BallState::Held) g.respawnBall(1);
    }
    phase = Phase::Countdown;
    g.events.clear();
    g.startCountdown();
    line.clear();
}

void Trainer::stop(Game& g) {
    if (hadSaved_) g.tun = saved_;
    hadSaved_ = false;
    for (auto& v : g.aimAt) v.clear();
    for (int p = 0; p < kMaxPlayers; ++p) g.pl[p].coach = PlayerState::Coach{};
    phase = Phase::Off;
    lesson = -1;
    targets.clear();
}

void Trainer::finish(bool passed) {
    phase = Phase::Done;
    result.lesson = lesson;
    result.passed = passed;
    result.successes = successes;
    result.attempts = attempts;
    result.time = clock;
    cues.push_back({passed ? Cue::Passed : Cue::Failed, {}, successes});
    line = passed ? "PASSED" : "NOT THIS TIME";
}

void Trainer::step(Game& g, float dt, PlayerInput* in) {
    cues.clear();
    const Lesson& l = L();
    if (!coachThere()) {   // nobody at the far end
        in[1] = PlayerInput{};
        in[1].head.p = {7.f, -50.f, 0};
    }
    // aim assist toward the targets (as in a match it turns your throw a little toward the other player)
    if (l.kind == THROW || l.guard == G_SENDBACK) g.aimAt[0] = aimPoints(l, targets, clock);
    else if (l.kind == STRIKES) g.aimAt[0] = {g.strikeZoneCenter(1) + Vec3{0, 0.02f, 0.45f}, g.strikeZoneCenter(1) + Vec3{0, 0.02f, -0.45f}};
    else if (l.kind == CATCH) g.aimAt[0] = {{9.4f, 1.6f, 0}};
    else g.aimAt[0].clear();
    g.step(dt, in);
    if ((!coachThere() || l.kind == STRIKES) && g.pl[1].ball.state != BallState::Dead) g.parkBall(1);
    switch (phase) {
    case Phase::Countdown:
        if (g.match == MatchState::InProgress) { phase = Phase::Play; clock = 0; cues.push_back({Cue::Go, {}, 0}); }
        break;
    case Phase::Play: clock += dt; stepPlay(g, dt); break;
    default: break;
    }
}

void Trainer::stepPlay(Game& g, float dt) {
    const Lesson& l = L();
    Ball& mine = g.pl[0].ball;
    auto endOf = [&](int owner, const Ball& b, Vec3& to) {   // where a ball got to this step (a wall it turned at)
        for (const GameEvent& e : g.events) if (e.type == EventType::Returned && e.player == owner) { to = e.pos; return true; }
        if (b.state == BallState::Flying) { to = b.pos; return true; }
        return false;
    };
    auto dropReturned = [&](int owner) {   // a ball a target took: no "back at you" for it
        g.events.erase(std::remove_if(g.events.begin(), g.events.end(),
                                      [&](const GameEvent& e) { return e.type == EventType::Returned && e.player == owner; }),
                       g.events.end());
    };
    auto allDown = [&]() {
        for (const Target& x : targets) if (!x.down) return false;
        return !targets.empty();
    };
    auto takeTarget = [&](int i) {
        targets[i].down = true;
        targets[i].downT = clock;
        ++successes;
        cues.push_back({Cue::TargetDown, targets[i].at(clock), i});
        refreshLive(l, targets);
    };
    if (l.kind == THROW) {
        for (const GameEvent& e : g.events)
            if (e.type == EventType::Throw && e.player == 0) { ++throws; ++attempts; flight_[0].start(mine); flight_[0].prev = e.pos; }
        Vec3 to;
        if (flight_[0].on && endOf(0, mine, to)) {
            const int i = judge(l, targets, flight_[0], mine, g.ballRadius(0), to, clock, &cues);
            if (i >= 0) { takeTarget(i); g.respawnBall(0); dropReturned(0); flight_[0].on = false; }
        }
        if (mine.state != BallState::Flying) flight_[0].on = false;
        if (allDown()) finish(true);
        else if (throws >= l.budget && !flight_[0].on) finish(false);
        line = throws >= l.budget - 2 && !allDown() ? "LAST THROWS" : "";
        return;
    }
    if (l.kind == STRIKES) {
        for (const GameEvent& e : g.events) {
            if (e.type == EventType::Throw && e.player == 0) { ++throws; ++attempts; flight_[0].on = true; }
            if (e.type == EventType::Strike && e.player == 1 && e.ball == 0) { ++successes; cues.push_back({Cue::Strike, e.pos, successes}); }
            if (e.type == EventType::HitPlayer && e.player == 1 && e.ball == 0) cues.push_back({Cue::BodyHit, e.pos, 0});
        }
        g.pl[1].strikesAgainst = 0;   // (your ball stays its size: the next strike as hard as the first)
        if (mine.state != BallState::Flying) flight_[0].on = false;
        if (successes >= l.pass) finish(true);
        else if (throws >= l.budget && !flight_[0].on) finish(false);
        return;
    }
    if (l.kind == CATCH) {
        for (const GameEvent& e : g.events) {
            if (e.type == EventType::Returned && e.player == 0 && e.value < 0.5f && !caughtArmed_) { caughtArmed_ = true; ++attempts; }
            else if (caughtArmed_ && e.type == EventType::Caught && e.player == 0) {
                ++successes; caughtArmed_ = false; cues.push_back({Cue::Caught, e.pos, successes});
            } else if (caughtArmed_ && ((e.type == EventType::HitPlayer && e.player == 0) ||
                                        (e.type == EventType::Returned && e.player == 0 && e.value > 0.5f))) {
                caughtArmed_ = false; cues.push_back({Cue::Dropped, e.pos, 0});
            }
        }
        if (caughtArmed_ && mine.state != BallState::Returning && mine.state != BallState::Flying && mine.state != BallState::Held) {
            caughtArmed_ = false; cues.push_back({Cue::Dropped, mine.pos, 0});
        }
        if (attempts >= l.budget && !caughtArmed_) finish(successes >= l.pass);
        return;
    }
    // DEFEND: the coach at the far end throws at you
    PlayerState& co = g.pl[1];
    Ball& cb = co.ball;
    waitT_ += dt;
    if (rechargeT_ >= 0 && (rechargeT_ -= dt) < 0) {   // SHIELD: here the charge comes back by itself
        if (!mine.charged) cues.push_back({Cue::Recharged, mine.pos, 0});
        mine.charged = true;
    }
    // where it aims (round you as you stand now): the lesson's pattern
    const int k = serve_;
    static const float kSide[10] = {0, 0.18f, -0.18f, 0.08f, -0.25f, 0.25f, -0.08f, 0.15f, -0.15f, 0};
    const Vec3 chest = g.chestPoint(0), head = g.pl[0].in.head.p;
    auto pattern = [&](int a, int n) {
        switch (a) {
        case AIM_WIDE: {   // out of reach of a still body: high, low, left and right
            static const Vec3 kWide[6] = {{0, 0.30f, 0.34f}, {0, -0.25f, -0.36f}, {0, 0.05f, 0.42f}, {0, 0.32f, -0.30f}, {0, -0.28f, 0.30f}, {0, 0.02f, -0.42f}};
            return chest + kWide[n % 6];
        }
        case AIM_HEAD: return head + Vec3{0, -0.02f, (n & 1) ? 0.06f : -0.06f};
        case AIM_SHOULDERS: return chest + Vec3{0, 0.22f, (n & 1) ? 0.20f : -0.20f};
        default: return chest + Vec3{0, 0, kSide[n % 10]};
        }
    };
    static const int kMix[5] = {AIM_CHEST, AIM_HEAD, AIM_SHOULDERS, AIM_WIDE, AIM_SHOULDERS};
    const Vec3 aim = l.aim == AIM_MIXED ? pattern(kMix[k % 5], k / 5 + k) : pattern(l.aim, k);
    co.coach.dir = aim - co.coach.from;
    const bool ready = (l.guard != G_SHIELD && l.guard != G_EITHER) || mine.state == BallState::Held || waitT_ > 6.f;
    if (!serving_ && serve_ < l.budget && cb.state == BallState::Held && co.botThrow == 0 && waitT_ > 1.0f) {
        line = ready ? "" : "TAKE YOUR BALL: ITS CHARGE POWERS YOUR SHIELD";
        if (ready) {
            co.coach.speed = l.serveSpeed + l.serveSpeedUp * serve_;
            co.coach.fire = true;
            serving_ = true;
            serveT_ = 0;
            ++serve_;
            ++attempts;
            flight_[1] = Flight{};
        }
    }
    if (serving_) {
        serveT_ += dt;
        bool done = false, ok = false;
        Vec3 at = cb.pos;
        for (const GameEvent& e : g.events) {
            if (done) break;
            if ((e.type == EventType::ShieldBlock || e.type == EventType::KnuckleBlock) && e.player == 0 && e.ball == 1) {
                const bool shield = e.type == EventType::ShieldBlock;
                if (shield && (l.guard == G_SHIELD || l.guard == G_EITHER)) rechargeT_ = 0.5f;
                if (l.guard == G_SENDBACK) { flight_[1].start(cb); flight_[1].prev = e.pos; continue; }   // (now: into a target?)
                cues.push_back({shield ? Cue::Blocked : Cue::Punched, e.pos, 0});
                done = true; ok = l.guard == G_SHIELD || l.guard == G_KNUCKLE || l.guard == G_EITHER; at = e.pos;
            } else if (e.type == EventType::HitPlayer && e.player == 0 && e.ball == 1) {
                cues.push_back({Cue::Hit, e.pos, 0});
                done = true; at = e.pos;
            } else if (e.type == EventType::Returned && e.player == 1 && !flight_[1].on) {   // past you, into your wall
                ok = l.guard == G_DODGE;
                cues.push_back({ok ? Cue::Dodged : Cue::Dropped, e.pos, 0});
                done = true; at = e.pos;
            } else if (e.type == EventType::HitPlayer && e.player == 1 && e.ball == 1 && flight_[1].on) {
                cues.push_back({Cue::BodyHit, e.pos, 0});
                done = true; at = e.pos;
            }
        }
        if (!done && flight_[1].on) {   // SEND IT BACK: your block on its way to the far end
            Vec3 to;
            if (endOf(1, cb, to)) {
                const int i = judge(l, targets, flight_[1], cb, g.ballRadius(1), to, clock, &cues);
                if (i >= 0) { takeTarget(i); done = true; ok = true; dropReturned(1); }
            }
            if (!done && cb.state != BallState::Flying) { done = true; cues.push_back({Cue::Dropped, cb.pos, 0}); }
        }
        if (!done && serveT_ > (flight_[1].on ? 6.f : 4.f) && cb.state != BallState::Held) {   // (it never got there)
            ok = l.guard == G_DODGE;
            cues.push_back({ok ? Cue::Dodged : Cue::Dropped, at, 0});
            done = true;
        }
        if (done) {
            serving_ = false;
            waitT_ = 0;
            flight_[1].on = false;
            if (ok && l.guard != G_SENDBACK) ++successes;
            if (cb.state == BallState::Flying && cb.instigator == 0) respawnT_ = 0.8f;   // (your block flies on a moment)
            else if (cb.state != BallState::Held && cb.state != BallState::Spawning) g.respawnBall(1);   // (its next ball)
        }
    }
    if (respawnT_ >= 0 && (respawnT_ -= dt) < 0 && cb.state != BallState::Held && cb.state != BallState::Spawning) g.respawnBall(1);
    if (l.guard == G_SENDBACK) {
        if (allDown()) finish(true);
        else if (serve_ >= l.budget && !serving_) finish(false);
    } else if (serve_ >= l.budget && !serving_) {
        finish(successes >= l.pass);
    }
}

}  // namespace training
