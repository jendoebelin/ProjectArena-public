// Multiplayer for Project Arena (own protocol; not compatible with the original PC game).
// Every arena is a ROOM run by its host headset (host-authoritative simulation):
//   tank      two player slots (a human, the practice bot in slot 1, or empty); FOUR CORNERS: four (bots in the
//             empty ones when BOT FILLS IN)
//   waiting   up to 4 people in the holding area outside the tank: the last two players and anyone who joined;
//             they can JOIN QUEUE (max 2). When a match ends both players leave the tank and the queue goes in.
//   spectators any number, invisible to the players: in the lobby (overlord view) or near this arena
//   discovery : the host broadcasts BEACON on UDP 47777 every 0.5 s (room counts, score, clock)
//   members   : send INPUT (head / hands / triggers / avatar) every frame to the host; QUEUE to join / leave the queue
//   host      : steps Game with the two tank players' inputs; sends STATE (game + events) and ROOM (slots, waiting
//               players' poses and avatars, spectator positions) to every member and spectator
//   spectator : SPECTATE every 0.2 s to every host it hears, with where it is (lobby or near that arena)
// Online, the community relay (server/projectarena_relay.py) carries the same packets between virtual addresses.
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>
#include "game.h"

struct LanGame {
    uint32_t ip = 0, session = 0;
    char name[17] = {};
    bool advanced = false, full = false;
    // 0.3.12 (network version 9): an EXPERIMENTAL room (expRoom: 0 DIAMOND, 1 FOUR CORNERS), its players and teams, and
    // each team's score (four in FREE-FOR-ALL)
    bool experimental = false;
    int expRoom = 0, players = 2, teamMode = 0;
    int teamScores[4] = {0, 0, 0, 0};
    // MATCH OPTIONS (0.3.8 hosts; older ones: the defaults): shields off = HOTDOG, knuckle guards -1 as the rules say,
    // basic match time (0 = free play) and advanced round time (0 = none), seconds
    bool shields = true;
    int knuckleMode = -1;
    float matchTime = 180.f, roundTime = 0.f;
    int score0 = 0, score1 = 0, spectators = 0, waiting = 0, queued = 0, inTank = 0;
    float clock = 0;
    double seen = 0;
};

// One arena's people, as the host reports them.
struct RoomInfo {
    enum : uint8_t { WAITING = 2 };
    struct Member { uint32_t id = 0; uint8_t role = 0; int8_t queuePos = -1; uint8_t spot = 0; PlayerInput in; };
    struct Watcher { uint32_t id = 0; uint8_t where = 0; Vec3 pos; };   // where: 0 lobby (lobby space), 1 this arena
    uint32_t slot[kMaxPlayers] = {0, 0, 0, 0};   // member id in the tank; 0 = empty (or a bot, see botMask)
    bool bot = false;                   // slot 1 is the practice bot (two players)
    uint8_t botMask = 0;                // the slots the bots play (bit p); two players: 2 when `bot`
    bool isBot(int p) const { return (botMask >> p) & 1; }
    std::vector<Member> members;        // everyone in the room except the tank slots' poses (those are in STATE)
    std::vector<Watcher> watchers;
    // player names (REPORT / MUTE A PLAYER): the host, the tank, the waiting players and the spectators at this
    // arena. Sent after the rest of ROOM, so builds without it still read the packet.
    struct Name { uint32_t id = 0; char name[17] = {}; };
    std::vector<Name> names;
    const char* nameOf(uint32_t id) const { for (const Name& n : names) if (n.id == id) return n.name; return nullptr; }
};

// The Crowd: fans following an online match on their phones (the crowd web app, through the relay). Cheers in the
// last half second, hype 0..100 and fans waving a flag, for each end; at = when it arrived (Net's clock).
struct CrowdInfo { int fans = 0, cheers[2] = {0, 0}, hype[2] = {0, 0}, waves[2] = {0, 0}; double at = -100; };

// A live match a spectator is subscribed to: the host's snapshots applied to a local Game (not simulated).
struct SpecMatch {
    uint32_t ip = 0;
    char name[17] = {};
    Game game;
    RoomInfo room;
    uint32_t lastSeq = 0, lastEventId = 0, lastRoomSeq = 0;
    double lastSeen = 0;
    std::vector<uint8_t> state;
    bool fresh = false;
    std::vector<GameEvent> events;   // new events since they were last taken
    CrowdInfo crowd;                 // its fans (The Crowd), as its host passes them on
};

// Bug reports go to the community server (same host and port as the relay, over TCP): "PABUG1\n", u32 length, text.
// Blocking with timeouts (call it off the render thread). err: a short upper-case reason on failure.
bool sendBugReport(const char* hostPort, const std::string& text, std::string& err);

class Net {
public:
    enum class Mode { Offline, Searching, Host, Client, Spectate };
    static constexpr int kMaxWaiting = 4, kMaxQueue = 4;   // (the queue: as many as the tank takes, 2 or 4)
    void init();
    void shutdown();
    void toggleLan();                 // Offline <-> LAN (search, then host or join): Y button / adb trigger
    void host();                      // lobby: host an arena (beacons with our name); we start in the tank
    // lobby: enter the arena hosted at ip. play: take the bot's place, or queue (JOIN ROOM: just wait outside)
    void join(uint32_t ip, bool play = true);
    void setBotFill(bool on) { botFill_ = on; }   // host: the practice bot plays a lone player (else they wait)
    bool botFill() const { return botFill_; }
    void leave();                     // back to practice vs bot (tells the other side)
    void spectate();                  // watch every live arena (leaves any room first)
    void queue(bool join);            // waiting: join / leave the arena's queue
    void forfeit();                   // in the tank: give up the match and go back to the room's waiting area
    std::vector<SpecMatch>& matches() { return matches_; }
    int spectatorCount() const { return (int)spectators_.size(); }
    void setName(const char* name);
    const char* name() const { return name_; }
    void setAppVersion(const char* v);   // this build ("0.3.11"): told to the relay in HELLO, for its log
    uint32_t session() const { return session_; }
    std::vector<LanGame> games() const;   // hosts seen in the last 2 s
    const char* noticeText() const;       // short-lived banner ("PLAYER JOINED", "HOST LOST", ...) or null
    const char* latestVersion() const { return latest_; }   // the current release, as the relay says ("" = not told)
    // Player statistics, kept by the community server per player key (server/projectarena_relay.py KEY .. STATS).
    // The host names its two tank players (TANK) and reports each finished match between two people (MATCH);
    // anyone asks for a host's players' careers (requestStats) and reads the answer with statsFor.
    struct CareerStats {
        bool known = false;           // the server has (or will keep) stats for this player
        char name[17] = {};
        uint32_t matches = 0, wins = 0, throws = 0, onTarget = 0, hits = 0, blocks = 0, faced = 0, catches = 0;
        uint32_t returnHits = 0;      // of the hits: points scored by balls the player blocked or punched back
        float topSpeed = 0;           // m/s
    };
    using MatchStats = MatchTally;   // (game.h)
    void setStatsKey(const uint8_t key[16]) { std::memcpy(statsKey_, key, 16); keySent_ = false; }
    // host, online, two people in the tank; score = points 0, points 1, rounds 0, rounds 1 (the other player confirms)
    // mode: Tunables::statsMode() (0 standard, 1 HOTDOG, 2 custom): careers keep each mode apart as well (0.3.8)
    void reportMatch(int winner, const MatchStats st[2], const uint8_t score[4], int mode = 0);
    void confirmMatch(int winner, const uint8_t score[4]);   // a tank player who is not the host: what it saw
    void requestMyStats();            // our own career: the answer is statsFor(kMyStats) (slot 0)
    void deleteMyStats();             // erase our statistics on the server
    static constexpr uint32_t kMyStats = 0xFFFFFFFFu;
    void requestStats(uint32_t hostVip);
    // TRAINING's leaderboards (0.3.12, the relay's TRAIN / BOARDREQ / BOARD): a passed lesson's result goes out once
    // we are online with our key (queued until then); a board is asked for and its latest answer read
    void submitTraining(int lessonId, float seconds, int successes, int attempts);
    struct BoardRow { char name[17]; uint32_t value; };
    struct Board {
        int lesson = -1, kind = 0;     // lesson id (0: overall); 0 TIME (ms), 1 ACCURACY (tenths of a percent)
        std::vector<BoardRow> rows;
        int myPlace = 0, players = 0;  // (0: not on it)
        uint32_t myValue = 0;
        double at = 0;                 // when it came
    };
    void requestBoard(int lessonId, int kind);
    const Board* board(int lessonId, int kind) const;   // null: no answer yet
    void boardForPictures(const Board& b) { boards_.push_back(b); }   // (--selftest pictures of the page)
    bool statsFor(uint32_t hostVip, CareerStats out[2]) const;   // the latest answer for that host (false: none)
    uint32_t roomHostVip() const;
    // The Crowd (0.3.7): an online host tells the relay about its match every second (LIVE); the relay answers with
    // the fans' cheers (CROWD), which the host passes on to its room and spectators (MSG_CROWD).
    struct LiveInfo {
        uint8_t state = 0, score[4] = {0, 0, 0, 0}, nation[2] = {0, 0}, rgb[2][3] = {}, flags = 0;   // flags: 1 advanced, 2 bot
        float clock = 0;
        char name[2][17] = {};
        MatchStats st[2];             // this match so far (for the fans' stats table)
        uint8_t round = 0, overtime = 0;
        // the flags byte also says: 4 HOTDOG (no shields), 8 free play, 16 knuckle guards on, 32 a round timer
    };
    void setLive(const LiveInfo& li) { live_ = li; liveSet_ = true; }
    // the fans of our room's match (host / client), or of the match at hostIp (spectator); null: none lately
    const CrowdInfo* crowd(uint32_t hostIp = 0) const;     // online: our room's host (ourselves when hosting); 0 = none
    void noticeLocal(const char* msg) { notice(msg); }
    uint32_t peerIp() const { return peerIp_; }
    float pingMs() const { return inRoom() ? lastPingMs_ : -1.f; }   // the latest round trip to the host / a player; -1 none
    // Online: the community relay makes the internet look like the LAN; addresses become virtual ones.
    bool goOnline(const char* hostPort);   // "host[:port]"; false if the address does not resolve
    void goOffline();
    bool online() const { return online_; }
    bool onlineReady() const { return online_ && vip_ != 0; }
    const char* serverName() const { return serverName_; }
    // a game on the network runs another protocol version (both headsets need the same build); 0 = none seen
    int otherVersion() const { return now_ - otherVerSeen_ < 3.0 ? otherVer_ : 0; }
    // Once per frame. `in` receives the two tank players' inputs to step the Game with (host / offline: in[0] is
    // the local player when we are in slot 0). Client: applies the host's snapshot to `g` and sends `local`.
    // Returns true when the caller should run Game::step (offline, or hosting with someone in the tank).
    // `in`: kMaxPlayers entries (g.np of them used)
    bool exchange(const PlayerInput& local, PlayerInput* in, Game& g, float dt);
    // the room we are in (host or client)
    bool inRoom() const { return mode_ == Mode::Host || mode_ == Mode::Client; }
    const RoomInfo& room() const { return room_; }
    int localSlot() const;            // 0 .. 3 in the tank; -1 waiting outside (or not in a room = 0 offline)
    int localIndex() const { int s = localSlot(); return s < 0 ? 0 : s; }
    bool localWaiting() const { return inRoom() && localSlot() < 0; }
    int localQueuePos() const;        // -1 not queued
    int localSpot() const;            // holding-area spot when waiting
    bool slotFilled(int p) const;     // a human or the bot is in tank slot p
    // spectators report where they are (blue markers): where 0 = lobby (pos in lobby space), 1 = near `watching`
    void setPresence(uint8_t where, const Vec3& pos, uint32_t watching) { presWhere_ = where; presPos_ = pos; presWatch_ = watching; }
    // Voice chat inside an arena room: frames go to the host, which forwards them to everyone else in that room
    // (the tank players, the waiting players and spectators standing at that arena). 40 ms of 16 kHz mu-law.
    static constexpr int kVoiceSamples = 640;
    enum : uint8_t { VOICE_PLAYER = 0, VOICE_WAITING = 1, VOICE_SPECTATOR = 2 };
    struct VoiceFrame { uint32_t speaker = 0; uint8_t role = 0; uint8_t data[kVoiceSamples]; };
    bool voiceZone() const { return inRoom() || (mode_ == Mode::Spectate && presWhere_ == 1 && presWatch_); }
    void sendVoice(const uint8_t* ulaw);
    std::vector<VoiceFrame>& voiceInbox() { return voiceIn_; }
    // Director cameras shared with a team: a spectator sends its set to every arena host it hears; hosts pass it to
    // their spectators; the app keeps a set only when the team name matches.
    void shareCameras(const char* team, const std::vector<uint8_t>& blob);
    bool takeCameras(char team[17], std::vector<uint8_t>& blob);
    // REPORT / MUTE A PLAYER: everyone else in our room, or at the arena a spectator stands at (role: VOICE_*)
    struct Person { uint32_t id = 0; char name[17] = {}; uint8_t role = 0; };
    // VOTE TO REMOVE (REPORT PLAYER): everyone in an arena room may vote to remove someone else from it; when more
    // than half of the other people in the room (and at least two) agree, the host removes them and turns them away
    // from that arena for the rest of its session (their session, address and name). The host can't be removed.
    bool voteKick(uint32_t target);   // false: not allowed (not in a room, the host, yourself, or already voted)
    bool hostKick(uint32_t target);   // the host removes someone from its arena at once (0.3.8; owner's decision)
    uint32_t hostSession() const { return mode_ == Mode::Host ? session_ : mode_ == Mode::Client ? peerSession_ : 0; }
    int kickVotesNeeded() const;      // host: how many votes remove someone now (0 = too few people to vote)
    std::vector<Person> peopleHere() const;
    std::string hereName() const;     // that room's name (its host's name; empty: none)
    bool peerConnected() const { return peerSeen_ > 0 && (now_ - peerSeen_) < 3.0; }
    Mode mode() const { return mode_; }
    const char* status() const;
    void sendState(const Game& g);    // host: the post-step snapshot to every member and spectator
    static size_t snapshotBytes(const Game& g);   // a STATE packet's size with a full event log (tests: the relay's 1200)
    struct SentEvent { uint32_t id; double t; GameEvent e; };

private:
    struct Member {                   // host: someone in the room (the host itself is one, local = true)
        uint32_t ip = 0, id = 0;
        double seen = 0;
        uint32_t lastSeq = 0;
        PlayerInput in;
        int8_t queuePos = -1;
        uint8_t spot = 0;
        bool local = false;
        char name[17] = {};
    };
    struct Spectator { uint32_t ip = 0, id = 0; double seen = 0; uint8_t where = 0; Vec3 pos; char name[17] = {}; };
    void sendTo(const void* d, int n, uint32_t ip, uint16_t port);
    void rawSend(const void* d, int n, uint32_t ip, uint16_t port);
    void relayHello();
    void notice(const char* msg);
    // host room logic
    Member* member(uint32_t id);
    int humans() const;
    bool inTank(uint32_t id) const { for (int p = 0; p < kMaxPlayers; ++p) if (id && slot_[p] == id) return true; return false; }
    int np_ = 2;                      // host: the tank's places this match (the game's players)
    uint8_t wantBots() const;         // host: the slots the bots should play now
    void freeSpot(Member& m);
    void fillTank(Game& g);
    void rotate(Game& g);
    void compactTank(Game& g, bool reset);
    void removeMember(uint32_t id, Game& g);
    void hostRoomInfo();              // rebuild room_ from the host's members
    void sendRoom();
    void reject(uint32_t ip, const char* why);

    int sock_ = -1;
    Mode mode_ = Mode::Offline;
    double now_ = 0, searchStart_ = 0, lastBeacon_ = 0, peerSeen_ = 0, lastRoomSent_ = 0, rotateAt_ = 0, lastSlowState_ = 0;
    uint32_t peerIp_ = 0;                 // client: the host; host: unused
    char latest_[16] = {};
    uint8_t statsKey_[16] = {};
    bool keySent_ = false;
    double lastTank_ = 0;
    uint32_t statsHost_ = 0;
    CareerStats stats_[2];
    std::vector<std::vector<uint8_t>> trainQ_;   // TRAIN bodies waiting for the relay (and our key)
    std::vector<Board> boards_;
    void relaySend(uint8_t type, const void* body, int n);
    LiveInfo live_;
    bool liveSet_ = false;
    double lastLive_ = 0;
    CrowdInfo crowd_;                     // host / client: our room's fans
    static bool readCrowd(const uint8_t* p, const uint8_t* e, CrowdInfo& c, double now);
    uint32_t peerSession_ = 0;            // client: the host's session id: packets from anyone else are ignored
    uint32_t forfeit_ = 0;                // host: a tank player who gave up (handled on the next exchange)
    bool online_ = false;
    uint32_t serverIp_ = 0;               // network order
    uint16_t serverPort_ = 0;             // host order
    uint8_t token_[16] = {};
    uint32_t vip_ = 0;                    // our virtual address on the relay
    double lastHello_ = 0, lastRelayPing_ = 0, lastRelayHeard_ = 0, lastJoin_ = 0;
    bool botFill_ = true, joinPlay_ = true;
    char serverName_[64] = {};
    int otherVer_ = 0;
    double otherVerSeen_ = -10;
    uint32_t seq_ = 0, lastRecvSeq_ = 0, lastRoomSeq_ = 0;
    uint32_t statRecv_ = 0, statLost_ = 0, statLate_ = 0;
    float pingMin_ = 0, pingMax_ = 0, lastPingMs_ = -1;   // round trips (ms) since the last stats line; the latest
    double pingSum_ = 0, lastPingSent_ = 0;
    uint32_t pingN_ = 0;
    double statStart_ = 0;
    void resetStats() { statRecv_ = statLost_ = statLate_ = 0; pingSum_ = 0; pingN_ = 0; statStart_ = now_; }
    void countSeq(uint32_t seq) {
        if (lastRecvSeq_ && seq > lastRecvSeq_ + 1) statLost_ += seq - lastRecvSeq_ - 1;
        ++statRecv_; lastRecvSeq_ = seq;
    }
    uint32_t session_ = 0;
    std::vector<Member> members_;         // host
    struct Removed { uint32_t id = 0, ip = 0; char name[17] = {}; };
    std::vector<Removed> removed_;        // host: voted out of this arena for this session
    std::vector<std::pair<uint32_t, uint32_t>> kickVotes_;   // host: (target, voter)
    std::vector<uint32_t> myKickVotes_;   // the people we voted to remove (one vote each)
    uint32_t kickNow_ = 0;                // host: removed on the next exchange
    bool kickByHost_ = false;             // (by the host's own decision, not by a vote)
    void countKickVote(uint32_t target, uint32_t voter);
    uint32_t slot_[kMaxPlayers] = {0, 0, 0, 0};   // host: member id in each tank slot (0 = empty / bot)
    std::vector<uint32_t> queue_;         // host: queued member ids, front first
    RoomInfo room_;                       // host: built each frame; client: from ROOM packets
    std::vector<uint8_t> lastState_;
    std::deque<SentEvent> eventLog_;      // host: recent events, resent in every snapshot
    uint32_t nextEventId_ = 1, lastEventId_ = 0;
    std::vector<GameEvent> pendingEvents_;   // client: new events since the last frame
    std::vector<LanGame> games_;
    std::vector<Spectator> spectators_;   // host: who to stream to
    std::vector<SpecMatch> matches_;      // spectator: what we watch
    std::vector<VoiceFrame> voiceIn_;     // received voice, taken by the app each frame
    char camTeam_[17] = {};
    std::vector<uint8_t> camBlob_;
    uint32_t camFrom_ = 0;
    bool camFresh_ = false;
    void forwardVoice(uint32_t speaker, uint8_t role, const uint8_t* ulaw, uint32_t exceptIp, uint32_t exceptId);
    double lastSpectatePing_ = 0;
    uint8_t presWhere_ = 0;
    Vec3 presPos_;
    uint32_t presWatch_ = 0;
    char name_[17] = "QUEST";
    char app_[16] = {};
    mutable char status_[64] = {};
    char notice_[48] = {};
    double noticeUntil_ = 0;
};
