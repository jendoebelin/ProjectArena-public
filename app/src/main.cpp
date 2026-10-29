// Project Arena -- native OpenXR entry point for Meta Quest (Android, GLES 3) and PC VR (Windows, OpenGL).
// Responsibilities: OpenXR instance/session/swapchains, controller input + haptics, the platform lifecycle,
// mapping tracking space onto the court layout, rendering, and driving Game (game.cpp).
#include <atomic>
#include <chrono>
#include <ctime>
#include <mutex>
#include <thread>
#include <cstring>
#include <deque>
#include <array>
#include <map>
#include <random>
#include <string>
#include <vector>

#ifdef __ANDROID__
#include <android/asset_manager.h>
#include <android/native_activity.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <jni.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include "gl.h"
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#else
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <xinput.h>   // (the structs only: XInputGetState is loaded at run time)
#include <setupapi.h>
#include <hidsdi.h>
#include <atomic>
#include <mutex>
#include <thread>
#include "gl.h"
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
bool xrLoaderInit();   // pc/xr_loader.cpp
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "platform.h"

#include "avatar.h"
#include "game.h"
#include "log.h"
#include "render.h"
#include "audio.h"
#include "net.h"
#include "record.h"
#include "xcompat.h"
#if __has_include("servers_default.h")
#include "servers_default.h"   // generated at build time (tools/write_servers_header.py)
#endif
#ifndef PA_DEFAULT_SERVER
#define PA_DEFAULT_SERVER ""
#define PA_SPARC_SERVER ""
#endif
#include "voice_codec.h"

#define XR_CHECK(x) do { XrResult r_ = (x); if (XR_FAILED(r_)) { LOGE("%s failed: %d (%s:%d)", #x, (int)r_, __FILE__, __LINE__); return false; } } while (0)

// ------------------------------------------------------------------ app state
#ifdef __ANDROID__
typedef XrSwapchainImageOpenGLESKHR SwapImage;
static const XrStructureType kSwapImageType = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
#else
typedef XrSwapchainImageOpenGLKHR SwapImage;
static const XrStructureType kSwapImageType = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
#endif

struct Swap {
    XrSwapchain handle = XR_NULL_HANDLE;
    int32_t w = 0, h = 0;
    std::vector<SwapImage> images;
    GLuint depth = 0, fbo = 0;
};

struct App {
    std::string dataPath;          // per-player files: settings, avatar, name, cameras, log
    int lanTriggerPoll = 0;
    bool resumed = false;
    bool quit = false;
#ifdef __ANDROID__
    android_app* android = nullptr;
    // EGL
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLConfig config = nullptr;
#else
    HWND hwnd = nullptr;           // desktop window: mirrors the left eye (also a capture source for streaming)
    HDC hdc = nullptr;
    HGLRC glrc = nullptr;
    int winW = 1280, winH = 720;
    // DESKTOP SPECTATOR (PC without a headset, or --spectate): a window, mouse and keyboard cameras, for streamers
    bool desktop = false;
    int dcView = 1;                 // 1 side, 2 high, 3 blue end, 4 orange end, 5 top, 0 free camera
    bool dcAuto = true, dcHelp = false, dcHubPinned = false, dcFull = false;   // dcHelp: the help bar in the picture
    int dcCmd = 0;                  // a button pressed in the Spectator Controls window (handled next frame)
    double dcHintUntil = 0;         // "F1: CONTROLS" in the picture for a few seconds after start
    double dcAutoAt = 0, dcWatchAt = 0;
    Vec3 dcPos{0, 2.4f, 7.5f};
    float dcYaw = 0, dcPitch = -0.15f, dcFov = 66.f;
    float dcWheel = 0;
    // desktop spectator broadcast moments (the fist bump, the match won): which, how far the camera has moved to it,
    // since when, and where the winner is (smoothed)
    int dcEventKind = 0;
    float dcEventMix = 0;
    double dcEventT0 = 0;
    Vec3 dcWinT{0, 0, 0};
    int dcKeyCol = 3;               // VIEW LAYERS > KEY: always 3, transparent (owner, 2026-10-21: no colour keys any more;
                                    // the green / magenta / blue code paths stay for --keycol 0-2 in the test pictures)
    int dcEventSkip = 0;            // a camera picked during a broadcast moment (fist bump / winner): that moment's
                                    // close-up stays off until the moment ends
    int dcWinStyle = 0;             // WIN SCREEN: 0 split (winner and loser side by side), 1 the winner up close, 2 your camera
    std::string dcTag;              // --tag NAME: "Project Arena - Spectator - NAME", so OBS can tell several windows apart
    Vec3 dcClose[2];                // the win screen's close-ups: each player's head, smoothed
    double dcCloseT = 0;
    bool dcKey[256] = {};
    uint16_t dcPadPrev = 0;   // the game controller's buttons last frame
    bool dcPadOk = false;
    GLuint dFbo = 0, dCol = 0, dDep = 0;
    int dW = 0, dH = 0;
#ifndef __ANDROID__
    POINT dcMouse{};
    RECT dcWinRect{};
    HWND dcPanel = nullptr, dcStatus = nullptr;   // the Spectator Controls window (not part of the stream)
#endif
#endif

    // OpenXR
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    XrSpace appSpace = XR_NULL_HANDLE;
    XrReferenceSpaceType spaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XrSpace headSpace = XR_NULL_HANDLE;
    std::vector<XrViewConfigurationView> viewCfg;
    std::vector<XrView> views;
    Swap swaps[2];
    bool hasLocalFloor = false, hasRefreshRate = false;
    // floor guard: the headset can lose its floor (more often with a stationary boundary) and put it at eye height,
    // which left the view down in the court's floor (owner, 2026-10-18). Your usual head height is tracked; a head
    // impossibly low for a while lifts the view by floorFix until the floor is back.
    float standH = 1.62f, floorFix = 0.f;
    double lowSince = 0, okSince = 0;
    // PASSTHROUGH (settings): your room around the arena (XR_FB_passthrough: Quest, and Quest Link when the Meta app
    // allows it). Everything outside the arena is hidden; the rendered image carries alpha for the compositor.
    bool hasPassthrough = false, ptRunning = false, ptFailed = false;
    // SETTINGS > PASSTHROUGH page: the mode and how the court and your room look (settings.cfg)
    int ptMode = 0;                 // PT_OFF, PT_ARENA, PT_WINDOW, PT_OUTLINE
    float ptFloor = 0.8f;           // court floor opacity (0 = your real floor)
    float ptGlass = 1.0f;           // glass walls / roof: their normal opacity times this
    float ptRoom = 1.0f;            // your room's brightness (passthrough opacity over black)
    bool ptMono = false, ptEdges = false;   // the room in grey; outlines on real objects
    bool ptStyleDirty = true, ptPage = false;
    // safety and convenience, in every mode (OFF too): the room fades in near the edge of your play space, and while
    // you hold Y (left controller) during play
    bool ptEdgeReveal = true, ptPeekOn = true, peekHeld = false;
    float ptReveal = 0.f;            // how far the game is faded out to show the room now (0..1)
    float ptFadeNow = 0.f;           // this frame's fade, applied under the menus (menus never fade)
    double ptRevealLogAt = 0;
    XrSpace stageSpace = XR_NULL_HANDLE;
    bool hasBounds = false;
    XrExtent2Df stageExt{0, 0};
    double boundsAt = 0;
    PFN_xrGetReferenceSpaceBoundsRect xrGetBoundsRect_ = nullptr;   // (looked up: the PC loader exports only the basics)
    // WINDOW with depth (XR_META_environment_depth, Quest 3 / 3S; needs the spatial data permission, USE_SCENE): real
    // surfaces nearer than the glass are covered, so only what lies beyond the court shows through it
    // hasDepth: the runtime has the extension; the provider is made once the permission is granted (it can't be
    // before: the first build never asked, owner's test 2026-10-18)
    bool hasDepth = false, depthReady = false, depthFailed = false, depthRunning = false, depthAsked = false, scenePerm = false,
         depthImgValid = false;
    double scenePermAt = 0;
    XrEnvironmentDepthProviderMETA depthProvider = XR_NULL_HANDLE;
    XrEnvironmentDepthSwapchainMETA depthSwap = XR_NULL_HANDLE;
    std::vector<SwapImage> depthImages;
    XrEnvironmentDepthImageMETA depthImg{XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_META};
    Mat4 depthView[2], depthProj[2];
    int curEye = 0;
    PFN_xrStartEnvironmentDepthProviderMETA xrStartDepth_ = nullptr;
    PFN_xrStopEnvironmentDepthProviderMETA xrStopDepth_ = nullptr;
    PFN_xrAcquireEnvironmentDepthImageMETA xrAcquireDepth_ = nullptr;
    PFN_xrPassthroughLayerSetStyleFB xrPassthroughLayerSetStyleFB_ = nullptr;
    bool mrView = false;            // draw for passthrough this frame (also the --selftest --passthrough picture)
    XrPassthroughFB ptHandle = XR_NULL_HANDLE;
    XrPassthroughLayerFB ptLayer = XR_NULL_HANDLE;
    PFN_xrCreatePassthroughFB xrCreatePassthroughFB_ = nullptr;
    PFN_xrPassthroughStartFB xrPassthroughStartFB_ = nullptr;
    PFN_xrPassthroughPauseFB xrPassthroughPauseFB_ = nullptr;
    PFN_xrCreatePassthroughLayerFB xrCreatePassthroughLayerFB_ = nullptr;
    PFN_xrPassthroughLayerResumeFB xrPassthroughLayerResumeFB_ = nullptr;
    PFN_xrPassthroughLayerPauseFB xrPassthroughLayerPauseFB_ = nullptr;
    PFN_xrRequestDisplayRefreshRateFB xrRequestDisplayRefreshRateFB = nullptr;

    // input
    XrActionSet actionSet = XR_NULL_HANDLE;
    XrAction aimPose = XR_NULL_HANDLE;
    XrSpace aimSpace[2] = {};
    int toolFrame = 1;            // 0 = OpenXR grip orientation, 1 = aim orientation (legacy Oculus controller frame)
    bool loggedFrames = false;
    XrAction gripPose = XR_NULL_HANDLE, trigger = XR_NULL_HANDLE, squeeze = XR_NULL_HANDLE, buttonA = XR_NULL_HANDLE,
             buttonB = XR_NULL_HANDLE, buttonY = XR_NULL_HANDLE, buttonX = XR_NULL_HANDLE, thumbClick = XR_NULL_HANDLE, menu = XR_NULL_HANDLE, haptic = XR_NULL_HANDLE;
    bool prevY = false, prevMenu = false;
    bool onlinePref = true;         // the community server by default (owner); ONLINE: OFF = home Wi-Fi only
    // RECORD MY MATCHES (record.h; MENU > MY STATS; settings.cfg record): off unless the player turns it on
    bool recordOn = false;
    rec::Recorder recorder;
    int clockTick = 0;            // the last second the round-end tick sounded for (its last 10 seconds)
    bool botFill = true;       // settings.cfg botfill: the bot plays a lone host   // settings.cfg online=1: go online at start-up
    XrPath handPath[2] = {};
    XrSpace handSpace[2] = {};

    // mapping from tracking space to the court ("app space")
    Pose trackToWorld;
    bool recentered = false;
    bool prevRecenter = false;

    Renderer gfx;
    Model arena, environment, projectile, shell, spawner, shield, lancer, knuckle, head, torso, strikezone;
    Model gauntlets[3];   // GAUNTLET styles 0..2 (STRIKER, TITAN, PHANTOM); 3 = CONTROLLER (the lancer)
    Model gauntletHand[3], gauntletCuff[3];   // the same, split at the wrist (the cuff can be moved: TUNING)
    // open art set only (absent in the original set): glossy floor + its mirrored light sources, additive glow layers
    Model floor, floorReflect, courtFx, envFx, courtPulse;
    Model stadiumFx, floorReflectEnv;   // the stadium's light beams / halos and its reflection in the floor (not in PASSTHROUGH)
    Model courtOutline;                 // PASSTHROUGH > OUTLINE: the neon edges and the floor lines
    double startTime = 0;
    struct Trail {
        Vec3 core[48], helixA[48], helixB[48];
        int n = 0;
        float spinAngle = 0;
        float strikeFlash = 0;
        Vec3 burstPos; float burst = 0;   // dissolve burst at the back wall
    } trail[2];
    XrAction thumbstick = XR_NULL_HANDLE, thumbstickL = XR_NULL_HANDLE;
    // push-to-talk, the left hand's grip as suits each controller: Quest Touch its squeeze, Valve Index a firm squeeze (its
    // force: Index reads 'squeeze' as soon as the strapped controller is held, which kept a tester's mic open), Vive / WMR
    // the grip click (owner, 2026-10-21)
    XrAction pushToTalk = XR_NULL_HANDLE;
    // live gauntlet tuning (applied to the tool frame): see the TUNING tool
    // gauntlet on the controller: pitch (deg), forward and up (m), and x / yaw / roll (right hand; the left is
    // mirrored; TUNING only, not in settings.cfg). Defaults = the owner's tuning in the headset with the TUNING tool
    // (second export 2026-10-18: +26 deg, yaw -16, roll -1, 5.4 cm forward, 3 cm down, 2.6 cm right; the first that
    // day: +21 deg, yaw -14, 9 cm forward; before: +35 deg, 9 cm forward, 2 cm up; 0 and -25 before that)
    float tunePitchDeg = 27.f, tuneFwd = 0.004f, tuneUp = -0.014f;   // the owner's TUNING export, 2026-10-19
    float tuneX = -0.006f, tuneYawDeg = -16.f, tuneRollDeg = -1.f;
    // developer tuning tool (SETTINGS > TUNING in developer mode): a mirror like the avatar builder's and a panel of
    // values for the gauntlet parts and the skeleton; the thumbsticks change the picked value (files/tuning.cfg)
    bool tuneOpen = false, prevTuneTrig = true;
    Pose tunePose;
    int tunePart = 0, tuneRow = 0, tuneHover = -1;
    bool tuneCoarse = false, tuneProps = true, tuneMarkers = true;
    int tuneStickDir = 0;
    // TUNING by hand: holding grip on one hand carries the picked piece on the other hand (its pose when the grab
    // started, and the grabbing hand's); the controllers' own grips before the GAUNTLET ON CONTROLLER offset
    int tuneGrab = -1;
    Pose tuneGrabD0, tuneGrabW0;
    Pose rawGrip[2];
    double tuneRepeatAt = 0, tuneSaveAt = 0, tuneStatusUntil = 0;
    bool tuneLatchY = false, tuneLatchL = false, tuneSettingsDirty = false;
    std::string tuneStatus;
    Vec3 tuneHit;
    bool tuneHitValid = false;
    Game game;
    Audio audio;
    Net net;
    double lastTime = 0;
    float hitFlash[2] = {0, 0};
    float bumpFlash = 0;            // FIST BUMP! (1 -> 0 over a second)
    float foulFlash = 0;            // FOUL where a ball landed in its zone (1.2 -> 0)
    Vec3 foulAt;
    double winAt = 0;               // when the match was won (the confetti; 0 = not in MatchSummary)
    // what the world shows (the desktop spectator's VIEW LAYERS; the headset always draws everything): 0 FULL,
    // 1 NO WALLS (the court's outline instead of its glass), 2 MINIMAL (no stadium, strike zones, spawners,
    // scoreboard), 3 KEY (only the players and the balls, on the key colour). onlyPlayer >= 0: that player alone
    int viewLayer = 0;
    int onlyPlayer = -1;
    // NATIONS (country presets): each end's country (the player's avatar; 0 = none) and colour, set every frame by
    // updateSides(); with no country anywhere the ends stay blue / orange and everything looks as before
    // player statistics (the community server keeps them per player key, net.h CareerStats): this match's counts on
    // the host, and the two players' careers as the server last answered (careersOk) for statsHost
    StatCounter matchCount;
    Net::MatchStats liveStats[2];   // The Crowd: this match so far (kept through the match summary)
    Net::CareerStats careers[2];
    bool careersOk = false;
    uint32_t statsHost = 0;
    double statsAskAt = 0;
    // the PC spectator's VS screen (two portraits, VS, their stats) at the fist bump and the countdown; M toggles,
    // and shows it any time
    bool statsOpen = false;         // MENU > MY STATS: the player's own career as a dashboard
    Net::CareerStats myCareer;
    bool myCareerOk = false;
    double myStatsAskAt = 0, deleteArmedUntil = 0;
    int dcMatchup = 1;
    bool dcMatchupForce = false;
    double dcVsT = 0;
    int sideNation[2] = {0, 0};
    float sideRGB[2][3] = {{0.1f, 0.7f, 1.0f}, {1.0f, 0.4f, 0.1f}};
    bool nationsOn = false;
    // The Crowd (fans on phones): each end's light flare from cheers (1 -> 0), the crowd sound's level, the last
    // update seen, whether an end's cheer has played (until its hype settles); testCrowd: --crowd in the pictures
    float crowdFlare[2] = {0, 0}, crowdLevel = 0;
    double crowdAt = -1;
    bool crowdCheered[2] = {false, false};
    CrowdInfo testCrowd;
    bool testCrowdOn = false;
    // cheer bursts: hearts, stars and little flags rising out of an end's stands (age < 0: not yet out)
    struct CrowdFx { Vec3 p, v; float age = 0, life = 1, size = 0.3f, roll = 0; int kind = 0, side = 0; };
    std::vector<CrowdFx> crowdFx;
    double meetTick = 0;            // the meeting's reminder buzz, and the closeness buzz as the fists come together
    // hit fireworks (PORT stand-in for the original's hit VFX): sparks + delayed shells that burst above the court
    struct HitFx { Vec3 pos, dir; float t; int team, victim; uint32_t seed; };
    std::vector<HitFx> hits;
    uint32_t fxSeed = 12345;
    // lobby menu (MENU button): floating panel, right-controller laser + trigger
    bool menuOpen = false, prevMenuTrig = true;
    Pose menuPose;
    Pose aim[2];
    bool aimValid[2] = {false, false};
    int menuHover = -1;
    Vec3 menuHit;
    bool menuHitValid = false;
    // spectating: hub of miniature live arenas, or one match watched full size from a floating gallery seat
    bool spectating = false;
    uint32_t watchIp = 0;          // 0 = hub
    Game savedGame;                // the local game, restored when spectating ends
    Pose viewHead;                 // the viewer's head in world space (player or spectator)
    int miniHover = -1;
    bool prevMiniTrig = true;
    uint32_t viewKey = 0;
    // avatars (open art set): part models by name, suit pattern masks, the local look and the bot's look
    std::map<std::string, Model> avModels;
    // hair that moves (av_hair_*.swing, tools/build_open_assets.py): per model its spring chains in head space (bone 0 =
    // the head, each chain's segments the next bones), and their state per drawn avatar (players, waiting players,
    // the menu showcase), stepped as they are drawn
    struct SwingChain { float k = 110, c = 7, follow = 0.35f, limitCos = 0.26f; int n = 0, bone0 = 1; Vec3 R[7]; float rmin[7] = {}; };
    struct SwingRig { int id = 0; bool exits = false; Vec3 centre{0, 0, 0}; std::vector<SwingChain> chains; };
    struct SwingState { bool init = false; double t = 0; Vec3 X[6][7], V[6][7]; };
    std::map<std::string, SwingRig> swingRigs;
    std::map<int64_t, SwingState> swings;   // drawn avatar * 1024 + rig id
    // hair under headgear (av_gear_* / av_visor_*.tuck): per body the maps, and a texture per body, gear and face shield
    struct Tuck { bool ok = false; int w = 0, h = 0; float c[3] = {0, 0, 0}; std::vector<uint8_t> v; };
    Tuck tuckGear[2][8], tuckVisor[2][8];
    std::map<int, GLuint> tuckTex;
    GLuint patternTex[8] = {};
    AvatarDesc myAvatar, botAvatar;
    PlayerInput localIn;           // the latest local input (the builder's mirror runs while the game is frozen)
    // avatar builder: a mirror 0.85 m ahead (the reflection copies you) and a pointer-driven panel beside it
    bool builderOpen = false, prevBuilderTrig = true;
    AvatarDesc builderStart;   // the look when the builder opened (CANCEL puts it back)
    Pose builderPose;
    Vec3 mirrorPoint, mirrorNormal;
    int builderHover = -1, presetIdx = 0;
    int lastNation = 1;             // PRESET > COUNTRY FLAG: the country it picks first (the last one chosen)
    std::vector<uint32_t> kickVoted;   // REPORT PLAYER > VOTE TO REMOVE: who we voted on (this session)
    bool statsNoticeOpen = false, statsNoticeSeen = false;   // the one-time notice about statistics (settings.cfg)
    Vec3 builderHit;
    bool builderHitValid = false;
    PlayerInput viewIn[2];         // what is drawn for each player (remote ones smoothed)
    bool viewInValid[2] = {false, false};
    bool titleOpen = false;        // title screen (game start): the menu shows PLAY / MULTIPLAYER / ...
    bool settingsOpen = false;
    int pointerHand = 1;           // hand that points at menus (settings: right / left)
    int fxLevel = 0;               // hit effects: 0 full, 1 soft, 2 low (photosensitivity)
    // FIELD OF VIEW (owner, 2026-10-29): how big the arena feels, -4..+4 in 5% steps. A headset's lenses fix its real
    // field of view; this draws the eyes closer together (the room feels bigger) or further apart (smaller). Picture
    // only: hands, reach and the court stay true, and the headset still gets the real eye poses.
    int roomFeel = 0;
    std::vector<CollisionTri> classicCol;   // the classic court's collision (arena.col), for switching back from EXPERIMENTAL
    int crowdMode = 0;             // The Crowd in the arena: 0 all, 1 no sound, 2 off (no sound, flags or flaring lights)
    int turnMode = 0;              // spectating, the right stick turns: 0 snap (30 deg), 1 smooth, 2 smooth fast (per player)
    int volume = 8;                // 0..10
    bool firstHelp = false;
    bool startTitle = true;        // show HOW TO PLAY on the first launch
    bool helpOpen = false;         // the menu shows HOW TO PLAY (first launch, or MENU > HOW TO PLAY)
    bool nameEdit = false;
    int editTarget = 0;            // keyboard edits: 0 the lobby name, 1 the community server address, 2 team, 3 Sparc server,
                                   // 4 bug note, 5 player report note
    // EXPERIMENTAL SOFTWARE (title menu): cross-play test with the Sparc Reviver server (app/src/xcompat.h)
    bool xpOpen = false;
    GLuint emblemTex = 0;           // the logo above the main menu (open_emblem)
    // REPORT A BUG page: a category, an optional note; the report is saved and sent to the community server
    bool bugOpen = false, bugReturnTitle = false;
    int bugCat = -1;
    std::string bugNote;
    std::mutex bugMx;
    std::string bugStatus;          // last result, shown on the page and as a notice
    std::atomic<bool> bugSending{false};
    std::atomic<int> bugDone{0};    // 1 sent, 2 saved (will retry)
    // REPORT / MUTE A PLAYER page (top-left of the menu in a room or spectating): someone here, a reason, a note.
    // The report goes the same way as a bug report; a mute silences that person's voice for this session.
    bool reportOpen = false;
    // CHOOSE THE RULES page: HOST A ROOM and PLAY PRACTICE open it (ADVANCED first; the last choice is remembered)
    bool rulesOpen = false, rulesForHost = false;
    // MATCH OPTIONS (0.3.8): step 1 of the rules page. rulesEdit: opened from SETTINGS to change this arena's rules
    // (not to host or start practice); the choices wait in opt* until START / APPLY; rulesPending: applied when the
    // match in progress ends (a live match keeps its rules)
    int rulesStep = 0;
    bool rulesEdit = false, rulesPending = false;
    // the player's own rules and MATCH OPTIONS, kept aside while a guest in someone's arena (the host's rules come in
    // with its snapshots): restored on leaving, and what settings.cfg keeps (code review, 2026-10-21)
    Tunables ownRules;
    bool guestRules = false;
    bool optAdvanced = false, optShields = true, optExperimental = false;
    int optExpRoom = 0;            // EXPERIMENTAL's room: 0 DIAMOND, 1 CORNERS
    int optKnuckle = -1;
    float optMatchTime = 180.f, optRoundTime = 0.f;
    int optBotLevel = 2;           // the practice bot's level (botSkill, game.h)
    bool optBotAdapt = false;      // MATCH MY LEVEL (Tunables::botAdapt)
    bool optBotMeet = false;       // FIST BUMP FIRST (Tunables::botMeet)
    bool quitArmed = false;         // QUIT GAME pressed once: the next press quits
    // THE LOBBY (owner, 2026-10-17): where you arrive, and come back to from a room, spectating or practice. A balcony
    // over the practice court where two bots play; posters around it; point at the court to play the bot.
    bool lobby = true;
    int lobbyPrevMode = 0;          // Net::Mode last frame (back to Offline = back to the lobby)
    float lobbySummaryT = 0;        // bot match over: a new one after a few seconds
    int lobbyHover = -1;            // poster under the pointer (kPosterCount = the court)
    bool prevLobbyTrig = true, prevLobbyPoke = false;
    float rawTrigger[2] = {0, 0};   // the triggers as pulled (the open menu zeroes the game's copy)
    uint32_t stateKey = 0xFFFFFFFFu;   // net mode / tank slot / spectating: a change closes the menu and director panel
    int cfgVersion = 0;             // settings.cfg cfgver= (2: older files may hold a test server address; 3: gauntlet pitch default)
    uint32_t reportWho = 0;
    char reportName[17] = {};
    int reportWhy = -1;
    std::string reportNote;
    std::vector<uint32_t> mutedIds;
    std::vector<Net::Person> reportFake;   // self-test pictures only (--menu report)
    xc::SparcLink sparc;
    std::string sparcAddr = PA_SPARC_SERVER;       // settings.cfg sparcserver= (default from the build)
    std::string sparcId;                           // anonymous install id (files/xc_id), never a device name
    std::string serverOverride, serverSaved;   // PC --server host:port for this session; the saved choice stays
    std::string serverAddr;        // community relay "host[:port]" (settings.cfg server=...)         // the menu shows a keyboard for the lobby name
    std::string nameBuf;
    bool devMode = false;
    bool noOnline = false;   // --selftest: never connect to the community server
    // voice chat: push-to-talk (left grip) by default, open mic, or off; mutes by who is talking
    int voiceMode = 0;             // 0 push-to-talk, 1 open mic, 2 off
    bool muteSpectators = false, mutePlayers = false, muteAll = false;
    bool voicePage = false;        // settings sub-page
    bool gfxPage = false;          // settings: GRAPHICS (the arena shows around the buttons)
    bool ptt = false, talking = false, micAsked = false, micDenied = false;   // micDenied: asked, not granted (yet)
    bool micFailed = false;        // allowed, but the microphone wouldn't open (tried again every 10 s)
    double micCheckAt = 0;
    float micGain = 1.0f;          // automatic microphone gain
    int voiceSent = 0, voiceRecv = 0;
    float pttGripMax = 0; int pttHeldFrames = 0;   // push-to-talk diagnostics: the left grip's peak, frames held (per log line)
    double voiceLogAt = 0;
    std::vector<int16_t> micBuf;
    std::map<uint32_t, double> spokeAt;   // speaker -> last voice frame (talking indicators)
    bool viewing = false;          // spectating, or waiting outside the tank: both players are drawn, no haptics
    bool hideOutside = false;      // settings: hide the people outside the tank while you play
    RoomInfo watchRoom;            // spectating a match full size: its room (waiting players, spectators)
    bool prevQueueTrig = true;
    int queueHover = -1;
    bool prevQueuePoke = false;
    // spectator camera (watching a match): fly with the sticks, warp points (shared by every arena: they all have
    // the same layout), an auto camera cycling them, and cross-fades between views
    struct WarpCam {               // a director camera: head pose in arena space + how the auto camera uses it
        Pose pose;
        uint8_t hold = 1;          // 0..3 -> 2 / 4 / 8 / 15 s
        uint8_t trans = 1;         // 0 cut, 1 cross-fade
        uint8_t trig = 0;          // 0 timer (in turn), 1 action (cut here for hits / strikes nearby), 2 follow (glide with the ball)
    };
    std::vector<WarpCam> warps;
    int warpIdx = -1;
    bool autoCam = false, prevA2 = true, prevX = true, prevY2 = true, turnLatch = false;
    double autoNext = 0, lastFly = 0;
    bool fadeCapture = false, fadePending = false;      // capture this frame, then move and fade it out
    Pose fadeTarget;
    float fadeT = 0;               // > 0 while the captured view fades out over the new one
    GLuint capTex[2] = {}, capFbo[2] = {};
    // director tools (spectators): the toolbar, the info panel with its rendered preview, team sharing
    bool dirOpen = false, prevDirTrig = true, rttPass = false;
    Pose dirPose;
    int dirHover = -1;
    Vec3 dirHit;
    bool dirHitValid = false;
    int infoKind = -1;             // option shown in the info panel: 0 hold, 1 cut, 2 cross-fade, 3 timer, 4 action, 5 follow
    double infoStart = 0;
    GLuint rttTex[2] = {}, rttFbo[2] = {}, rttDepth = 0;
    int dirRequest = -1;           // camera the director wants next (set by game events)
    std::string team;              // team name for shared cameras (settings.cfg team=...)          // <files>/dev_mode exists: tuning sticks, readouts and diagnostic logs
};

static const char* dataDir(const App& a) { return a.dataPath.empty() ? nullptr : a.dataPath.c_str(); }

#ifdef __ANDROID__
static JavaVM* g_vm = nullptr;
static jobject g_activity = nullptr;
static jobject g_multicastLock = nullptr;

// WifiManager.MulticastLock via JNI (NativeActivity has no Java code). Needed for LAN discovery.
bool netAcquireMulticastLock() {
    if (!g_vm || !g_activity) return false;
    JNIEnv* env = nullptr;
    g_vm->AttachCurrentThread(&env, nullptr);
    jclass actCls = env->GetObjectClass(g_activity);
    jmethodID getSys = env->GetMethodID(actCls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jobject wifi = env->CallObjectMethod(g_activity, getSys, env->NewStringUTF("wifi"));
    if (env->ExceptionCheck() || !wifi) { env->ExceptionClear(); return false; }
    jclass wmCls = env->GetObjectClass(wifi);
    jmethodID create = env->GetMethodID(wmCls, "createMulticastLock", "(Ljava/lang/String;)Landroid/net/wifi/WifiManager$MulticastLock;");
    jobject lock = env->CallObjectMethod(wifi, create, env->NewStringUTF("projectarena"));
    if (env->ExceptionCheck() || !lock) { env->ExceptionClear(); return false; }
    jclass lockCls = env->GetObjectClass(lock);
    env->CallVoidMethod(lock, env->GetMethodID(lockCls, "setReferenceCounted", "(Z)V"), JNI_FALSE);
    env->CallVoidMethod(lock, env->GetMethodID(lockCls, "acquire", "()V"));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    g_multicastLock = env->NewGlobalRef(lock);
    return true;
}

// runtime permissions (RECORD_AUDIO for voice, USE_SCENE for WINDOW's depth): ask once, the headset shows its own prompt.
// The answer comes later: callers check again (once a second) and never take "not granted yet" as a refusal. Checked
// from the render thread, which never returns to Java, so every local reference is freed here (or the table fills up).
static bool androidPermission(const char* name, bool request, int code) {
    if (!g_vm || !g_activity) return false;
    JNIEnv* env = nullptr;
    g_vm->AttachCurrentThread(&env, nullptr);
    if (env->PushLocalFrame(8) != 0) { env->ExceptionClear(); return false; }
    jclass cls = env->GetObjectClass(g_activity);
    jstring perm = env->NewStringUTF(name);
    jmethodID check = env->GetMethodID(cls, "checkSelfPermission", "(Ljava/lang/String;)I");
    jint r = check ? env->CallIntMethod(g_activity, check, perm) : -1;
    if (env->ExceptionCheck()) { env->ExceptionClear(); r = -1; }
    else if (r != 0 && request) {
        jclass strCls = env->FindClass("java/lang/String");
        jobjectArray arr = env->NewObjectArray(1, strCls, perm);
        jmethodID req = env->GetMethodID(cls, "requestPermissions", "([Ljava/lang/String;I)V");
        if (req) env->CallVoidMethod(g_activity, req, arr, code);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    env->PopLocalFrame(nullptr);
    return r == 0;
}
static bool micPermission(bool request) { return androidPermission("android.permission.RECORD_AUDIO", request, 1); }
static bool scenePermission(bool request) { return androidPermission("com.oculus.permission.USE_SCENE", request, 2); }
#else
bool netAcquireMulticastLock() { return false; }   // Windows delivers LAN broadcasts without one
static bool micPermission(bool) { return true; }   // Windows asks for microphone access itself
static bool scenePermission(bool) { return true; }
#endif

static double g_clockOverride = -1.0;   // --record: the recording's own clock (frames are rendered slower than real time)
static double nowSeconds() {
    if (g_clockOverride >= 0.0) return g_clockOverride;
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static Pose fromXr(const XrPosef& p) { return {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w}, {p.position.x, p.position.y, p.position.z}}; }

// ------------------------------------------------------------------ GL context
#ifdef __ANDROID__
static bool initGraphics(App& a) {
    a.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(a.display, nullptr, nullptr);
    const EGLint cfgAttr[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 0,
                              EGL_STENCIL_SIZE, 0, EGL_SAMPLES, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                              EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLint n = 0;
    if (!eglChooseConfig(a.display, cfgAttr, &a.config, 1, &n) || n == 0) { LOGE("eglChooseConfig failed"); return false; }
    const EGLint ctxAttr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    a.context = eglCreateContext(a.display, a.config, EGL_NO_CONTEXT, ctxAttr);
    if (a.context == EGL_NO_CONTEXT) { LOGE("eglCreateContext failed"); return false; }
    const EGLint pb[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    a.surface = eglCreatePbufferSurface(a.display, a.config, pb);
    if (!eglMakeCurrent(a.display, a.surface, a.surface, a.context)) { LOGE("eglMakeCurrent failed"); return false; }
    LOGI("GL: %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    return true;
}
#else
static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    App* a = (App*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_CLOSE: if (a) a->quit = true; return 0;
    case WM_SIZE: if (a) { a->winW = LOWORD(l); a->winH = HIWORD(l); } return 0;
    case WM_MOUSEWHEEL: if (a) a->dcWheel += (float)GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA; return 0;
    default: return DefWindowProcW(h, m, w, l);
    }
}

// a window with a desktop OpenGL 4.3+ compatibility context (GLES 3 shaders via ARB_ES3_compatibility)
static bool initGraphics(App& a) {
    WNDCLASSW wc{};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));   // (app/src/pc/ProjectArena.rc)
    wc.lpszClassName = L"ProjectArena";
    RegisterClassW(&wc);
    RECT r{0, 0, a.winW, a.winH};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    a.hwnd = CreateWindowW(L"ProjectArena", L"Project Arena", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                           r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!a.hwnd) { LOGE("CreateWindow failed"); return false; }
    SetWindowLongPtrW(a.hwnd, GWLP_USERDATA, (LONG_PTR)&a);
    a.hdc = GetDC(a.hwnd);
    PIXELFORMATDESCRIPTOR pfd{sizeof pfd, 1};
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;   // the spectator's TRANSPARENT key: the window keeps an alpha channel for OBS
    pfd.iLayerType = PFD_MAIN_PLANE;
    int pf = ChoosePixelFormat(a.hdc, &pfd);
    if (!pf || !SetPixelFormat(a.hdc, pf, &pfd)) { LOGE("SetPixelFormat failed"); return false; }
    PIXELFORMATDESCRIPTOR got{};
    DescribePixelFormat(a.hdc, pf, sizeof got, &got);
    LOGI("window: %d-bit colour, %d-bit alpha", got.cColorBits, got.cAlphaBits);
    HGLRC tmp = wglCreateContext(a.hdc);
    if (!tmp || !wglMakeCurrent(a.hdc, tmp)) { LOGE("wglCreateContext failed"); return false; }
    typedef HGLRC(WINAPI * CreateCtxAttribs)(HDC, HGLRC, const int*);
    auto createAttribs = (CreateCtxAttribs)wglGetProcAddress("wglCreateContextAttribsARB");
    if (createAttribs) {
        const int attrs[] = {0x2091 /* MAJOR */, 4, 0x2092 /* MINOR */, 3, 0x9126 /* PROFILE_MASK */, 0x2 /* COMPATIBILITY */, 0};
        if (HGLRC ctx = createAttribs(a.hdc, nullptr, attrs)) {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(tmp);
            tmp = ctx;
            wglMakeCurrent(a.hdc, ctx);
        }
    }
    a.glrc = tmp;
    typedef BOOL(WINAPI * SwapInterval)(int);
    if (auto si = (SwapInterval)wglGetProcAddress("wglSwapIntervalEXT")) si(0);   // the headset paces frames, not the monitor
    if (!glLoadFunctions()) { LOGE("this graphics driver lacks OpenGL 4.3"); return false; }
    glEnable(GL_FRAMEBUFFER_SRGB);   // GLES always encodes into sRGB targets; desktop GL only when asked
    ShowWindow(a.hwnd, SW_SHOWNORMAL);
    LOGI("GL: %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    return true;
}

// left eye -> the desktop window (letterboxed); the sRGB values are copied as they are
static void mirrorToWindow(App& a, const Swap& s) {
    if (a.winW <= 0 || a.winH <= 0) return;
    float sc = std::min((float)a.winW / s.w, (float)a.winH / s.h);
    int w = (int)(s.w * sc), h = (int)(s.h * sc), x = (a.winW - w) / 2, y = (a.winH - h) / 2;
    glDisable(GL_FRAMEBUFFER_SRGB);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, a.winW, a.winH);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBlitFramebuffer(0, 0, s.w, s.h, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glEnable(GL_FRAMEBUFFER_SRGB);
}
#endif

// ------------------------------------------------------------------ OpenXR
static XrPath path(App& a, const char* s) { XrPath p; xrStringToPath(a.instance, s, &p); return p; }

static bool initInstance(App& a) {
#ifdef __ANDROID__
    PFN_xrInitializeLoaderKHR initLoader = nullptr;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction*)&initLoader);
    if (initLoader) {
        XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        li.applicationVM = a.android->activity->vm;
        li.applicationContext = a.android->activity->clazz;
        initLoader((const XrLoaderInitInfoBaseHeaderKHR*)&li);
    }
#else
    if (!xrLoaderInit()) return false;
#endif
    uint32_t n = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
    std::vector<XrExtensionProperties> exts(n, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data());
#ifdef __ANDROID__
    std::vector<const char*> enable = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME};
#else
    std::vector<const char*> enable = {XR_KHR_OPENGL_ENABLE_EXTENSION_NAME};
#endif
    for (auto& e : exts) {
        if (!strcmp(e.extensionName, XR_EXT_LOCAL_FLOOR_EXTENSION_NAME)) { a.hasLocalFloor = true; enable.push_back(XR_EXT_LOCAL_FLOOR_EXTENSION_NAME); }
        if (!strcmp(e.extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) { a.hasRefreshRate = true; enable.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME); }
        if (!strcmp(e.extensionName, XR_FB_PASSTHROUGH_EXTENSION_NAME)) { a.hasPassthrough = true; enable.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME); }
        if (!strcmp(e.extensionName, XR_META_ENVIRONMENT_DEPTH_EXTENSION_NAME)) { a.hasDepth = true; enable.push_back(XR_META_ENVIRONMENT_DEPTH_EXTENSION_NAME); }
    }
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
#ifdef __ANDROID__
    XrInstanceCreateInfoAndroidKHR android{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    android.applicationVM = a.android->activity->vm;
    android.applicationActivity = a.android->activity->clazz;
    ci.next = &android;
#endif
    strcpy(ci.applicationInfo.applicationName, "Project Arena");
    ci.applicationInfo.applicationVersion = 2;
    strcpy(ci.applicationInfo.engineName, "projectarena-native");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
    ci.enabledExtensionCount = (uint32_t)enable.size();
    ci.enabledExtensionNames = enable.data();
    XR_CHECK(xrCreateInstance(&ci, &a.instance));
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(a.instance, &ip);
    LOGI("OpenXR runtime: %s %u.%u.%u (local_floor=%d refresh=%d)", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
         XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion), a.hasLocalFloor, a.hasRefreshRate);
    XrSystemGetInfo sg{XR_TYPE_SYSTEM_GET_INFO};
    sg.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult sr = xrGetSystem(a.instance, &sg, &a.system);
    if (sr == XR_ERROR_FORM_FACTOR_UNAVAILABLE) { LOGE("no headset found: connect it and start SteamVR / Link"); return false; }
    XR_CHECK(sr);
    if (a.hasRefreshRate)
        xrGetInstanceProcAddr(a.instance, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction*)&a.xrRequestDisplayRefreshRateFB);
    return true;
}

static bool initActions(App& a) {
    XrActionSetCreateInfo as{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(as.actionSetName, "gameplay");
    strcpy(as.localizedActionSetName, "Gameplay");
    XR_CHECK(xrCreateActionSet(a.instance, &as, &a.actionSet));
    a.handPath[0] = path(a, "/user/hand/left");
    a.handPath[1] = path(a, "/user/hand/right");
    auto mk = [&](XrAction& act, XrActionType t, const char* name, bool hands) -> bool {
        XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
        ci.actionType = t;
        strcpy(ci.actionName, name);
        strcpy(ci.localizedActionName, name);
        if (hands) { ci.countSubactionPaths = 2; ci.subactionPaths = a.handPath; }
        XR_CHECK(xrCreateAction(a.actionSet, &ci, &act));
        return true;
    };
    if (!mk(a.gripPose, XR_ACTION_TYPE_POSE_INPUT, "grip_pose", true)) return false;
    if (!mk(a.aimPose, XR_ACTION_TYPE_POSE_INPUT, "aim_pose", true)) return false;
    if (!mk(a.trigger, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", true)) return false;
    if (!mk(a.squeeze, XR_ACTION_TYPE_FLOAT_INPUT, "squeeze", true)) return false;
    if (!mk(a.buttonA, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_a", false)) return false;
    if (!mk(a.buttonB, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_b", false)) return false;
    if (!mk(a.buttonY, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_y", false)) return false;
    if (!mk(a.buttonX, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_x", false)) return false;
    if (!mk(a.thumbClick, XR_ACTION_TYPE_BOOLEAN_INPUT, "thumb_click", true)) return false;
    if (!mk(a.menu, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", false)) return false;
    if (!mk(a.haptic, XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", true)) return false;
    if (!mk(a.thumbstick, XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick", false)) return false;
    if (!mk(a.thumbstickL, XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick_left", false)) return false;
    if (!mk(a.pushToTalk, XR_ACTION_TYPE_FLOAT_INPUT, "push_to_talk", false)) return false;

    std::vector<XrActionSuggestedBinding> b = {
        {a.gripPose, path(a, "/user/hand/left/input/grip/pose")},   {a.gripPose, path(a, "/user/hand/right/input/grip/pose")},
        {a.aimPose, path(a, "/user/hand/left/input/aim/pose")},     {a.aimPose, path(a, "/user/hand/right/input/aim/pose")},
        {a.trigger, path(a, "/user/hand/left/input/trigger/value")}, {a.trigger, path(a, "/user/hand/right/input/trigger/value")},
        {a.squeeze, path(a, "/user/hand/left/input/squeeze/value")}, {a.squeeze, path(a, "/user/hand/right/input/squeeze/value")},
        {a.buttonA, path(a, "/user/hand/right/input/a/click")},      {a.buttonA, path(a, "/user/hand/left/input/x/click")},
        {a.buttonB, path(a, "/user/hand/right/input/b/click")},      {a.buttonY, path(a, "/user/hand/left/input/y/click")},
        {a.buttonX, path(a, "/user/hand/left/input/x/click")},
        {a.thumbClick, path(a, "/user/hand/left/input/thumbstick/click")}, {a.thumbClick, path(a, "/user/hand/right/input/thumbstick/click")},
        {a.menu, path(a, "/user/hand/left/input/menu/click")},
        {a.thumbstick, path(a, "/user/hand/right/input/thumbstick")},
        {a.thumbstickL, path(a, "/user/hand/left/input/thumbstick")},
        {a.pushToTalk, path(a, "/user/hand/left/input/squeeze/value")},
        {a.haptic, path(a, "/user/hand/left/output/haptic")},        {a.haptic, path(a, "/user/hand/right/output/haptic")},
    };
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = path(a, "/interaction_profiles/oculus/touch_controller");
    sb.suggestedBindings = b.data();
    sb.countSuggestedBindings = (uint32_t)b.size();
    XR_CHECK(xrSuggestInteractionProfileBindings(a.instance, &sb));
    // PC VR controllers. A = right A / left X, B = right B (recenter), menu = left B (Index) or menu buttons.
    auto suggest = [&](const char* profile, const std::vector<std::pair<XrAction, const char*>>& list) {
        std::vector<XrActionSuggestedBinding> v;
        for (const auto& e : list) v.push_back({e.first, path(a, e.second)});
        XrInteractionProfileSuggestedBinding pb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        pb.interactionProfile = path(a, profile);
        pb.suggestedBindings = v.data();
        pb.countSuggestedBindings = (uint32_t)v.size();
        XrResult r = xrSuggestInteractionProfileBindings(a.instance, &pb);
        if (XR_FAILED(r)) LOGW("bindings for %s not accepted (%d)", profile, (int)r);
    };
    const char* L = "/user/hand/left/input/";
    const char* R = "/user/hand/right/input/";
    auto P = [](const char* side, const char* rest) { static std::deque<std::string> keep; keep.push_back(std::string(side) + rest); return keep.back().c_str(); };
    std::vector<std::pair<XrAction, const char*>> common = {
        {a.gripPose, P(L, "grip/pose")}, {a.gripPose, P(R, "grip/pose")}, {a.aimPose, P(L, "aim/pose")}, {a.aimPose, P(R, "aim/pose")},
        {a.haptic, "/user/hand/left/output/haptic"}, {a.haptic, "/user/hand/right/output/haptic"}};
    auto with = [&](std::vector<std::pair<XrAction, const char*>> extra) { extra.insert(extra.end(), common.begin(), common.end()); return extra; };
    suggest("/interaction_profiles/valve/index_controller", with({
        {a.trigger, P(L, "trigger/value")}, {a.trigger, P(R, "trigger/value")}, {a.squeeze, P(L, "squeeze/value")}, {a.squeeze, P(R, "squeeze/value")},
        {a.buttonA, P(R, "a/click")}, {a.buttonA, P(L, "a/click")}, {a.buttonX, P(L, "a/click")}, {a.buttonB, P(R, "b/click")},
        {a.menu, P(L, "b/click")}, {a.buttonY, P(L, "trackpad/force")}, {a.pushToTalk, P(L, "squeeze/force")},
        {a.thumbClick, P(L, "thumbstick/click")}, {a.thumbClick, P(R, "thumbstick/click")},
        {a.thumbstick, P(R, "thumbstick")}, {a.thumbstickL, P(L, "thumbstick")}}));
    suggest("/interaction_profiles/htc/vive_controller", with({
        {a.trigger, P(L, "trigger/value")}, {a.trigger, P(R, "trigger/value")}, {a.squeeze, P(L, "squeeze/click")}, {a.squeeze, P(R, "squeeze/click")},
        {a.buttonA, P(R, "trackpad/click")}, {a.buttonX, P(L, "trackpad/click")}, {a.buttonA, P(L, "trackpad/click")},
        {a.buttonB, P(R, "menu/click")}, {a.menu, P(L, "menu/click")}, {a.pushToTalk, P(L, "squeeze/click")},
        {a.thumbstick, P(R, "trackpad")}, {a.thumbstickL, P(L, "trackpad")}}));
    suggest("/interaction_profiles/microsoft/motion_controller", with({
        {a.trigger, P(L, "trigger/value")}, {a.trigger, P(R, "trigger/value")}, {a.squeeze, P(L, "squeeze/click")}, {a.squeeze, P(R, "squeeze/click")},
        {a.buttonA, P(R, "trackpad/click")}, {a.buttonX, P(L, "trackpad/click")}, {a.buttonA, P(L, "trackpad/click")},
        {a.buttonB, P(R, "menu/click")}, {a.menu, P(L, "menu/click")}, {a.pushToTalk, P(L, "squeeze/click")},
        {a.thumbClick, P(L, "thumbstick/click")}, {a.thumbClick, P(R, "thumbstick/click")},
        {a.thumbstick, P(R, "thumbstick")}, {a.thumbstickL, P(L, "thumbstick")}}));
    suggest("/interaction_profiles/khr/simple_controller", with({
        {a.trigger, P(L, "select/click")}, {a.trigger, P(R, "select/click")}, {a.menu, P(L, "menu/click")}, {a.buttonB, P(R, "menu/click")}}));
    return true;
}

// ------------------------------------------------------------------ passthrough (XR_FB_passthrough)
// Created paused once the session exists; updatePassthrough() starts it while PASSTHROUGH is on and pauses it
// otherwise (the cameras cost power). Any failure turns the option off for this run (N/A on the settings page).
enum { PT_OFF, PT_ARENA, PT_WINDOW, PT_OUTLINE, PT_COUNT };
static const char* kPtModeName[PT_COUNT] = {"OFF", "ARENA", "WINDOW", "OUTLINE"};
static const char* kPtModeHelp[PT_COUNT] = {"THE FULL STADIUM", "THE COURT IN YOUR ROOM: THE STADIUM IS GONE",
                                            "INSIDE THE COURT IS ALL GAME: YOUR ROOM LOOKS IN THROUGH THE GLASS",
                                            "ONLY THE COURT'S EDGES AND LINES: SEE YOUR WHOLE ROOM"};
static const float kPtModeFloor[PT_COUNT] = {0.8f, 0.8f, 1.0f, 0.0f};   // the floor each mode starts with
template <class F> static bool xrFn(App& a, const char* name, F& f) {
    return XR_SUCCEEDED(xrGetInstanceProcAddr(a.instance, name, (PFN_xrVoidFunction*)&f)) && f;
}
static void initPassthrough(App& a) {
    if (!a.hasPassthrough) return;
    PFN_xrDestroyPassthroughFB destroy = nullptr;
    bool ok = xrFn(a, "xrCreatePassthroughFB", a.xrCreatePassthroughFB_) && xrFn(a, "xrPassthroughStartFB", a.xrPassthroughStartFB_) &&
              xrFn(a, "xrPassthroughPauseFB", a.xrPassthroughPauseFB_) && xrFn(a, "xrCreatePassthroughLayerFB", a.xrCreatePassthroughLayerFB_) &&
              xrFn(a, "xrPassthroughLayerResumeFB", a.xrPassthroughLayerResumeFB_) &&
              xrFn(a, "xrPassthroughLayerPauseFB", a.xrPassthroughLayerPauseFB_) && xrFn(a, "xrDestroyPassthroughFB", destroy);
    xrFn(a, "xrPassthroughLayerSetStyleFB", a.xrPassthroughLayerSetStyleFB_);   // optional: ROOM brightness / colour / edges
    XrResult r = XR_ERROR_FUNCTION_UNSUPPORTED;
    if (ok) {
        XrPassthroughCreateInfoFB pc{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
        r = a.xrCreatePassthroughFB_(a.session, &pc, &a.ptHandle);
        ok = XR_SUCCEEDED(r);
    }
    if (ok) {
        XrPassthroughLayerCreateInfoFB lc{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
        lc.passthrough = a.ptHandle;
        lc.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
        r = a.xrCreatePassthroughLayerFB_(a.session, &lc, &a.ptLayer);
        ok = XR_SUCCEEDED(r);
        if (!ok && destroy) { destroy(a.ptHandle); a.ptHandle = XR_NULL_HANDLE; }
    }
    a.ptFailed = !ok;
    LOGI("passthrough: %s (%d)", ok ? "ready" : "not available", (int)r);
}
// how close you are to the edge of your play space (m, negative = outside): the head (head) and the nearer hand
// (hands) against the STAGE rectangle (Quest: the largest rectangle inside your boundary), or, without one, a 1.5 m
// circle round the tracking origin
static void edgeDistance(App& a, XrTime t, float& head, float& hands) {
    const double now = nowSeconds();
    if (now > a.boundsAt) {
        a.boundsAt = now + 3.0;
        XrExtent2Df e{0, 0};
        if (!a.xrGetBoundsRect_) xrFn(a, "xrGetReferenceSpaceBoundsRect", a.xrGetBoundsRect_);
        const bool ok = a.stageSpace != XR_NULL_HANDLE && a.xrGetBoundsRect_ &&
                        XR_SUCCEEDED(a.xrGetBoundsRect_(a.session, XR_REFERENCE_SPACE_TYPE_STAGE, &e)) && e.width > 0.5f && e.height > 0.5f;
        if (ok != a.hasBounds || (ok && (std::fabs(e.width - a.stageExt.width) > 0.05f || std::fabs(e.height - a.stageExt.height) > 0.05f)))
            LOGI("play space: %s", ok ? "boundary rectangle" : "no boundary rectangle (1.5 m circle)");
        a.hasBounds = ok;
        if (ok) a.stageExt = e;
    }
    head = hands = 99.f;
    const XrSpace pts[3] = {a.headSpace, a.handSpace[0], a.handSpace[1]};
    for (int i = 0; i < 3; ++i) {
        XrSpaceLocation l{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(pts[i], a.hasBounds ? a.stageSpace : a.appSpace, t, &l)) ||
            !(l.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) continue;
        const float x = l.pose.position.x, z = l.pose.position.z;
        float di = a.hasBounds ? std::min(a.stageExt.width * 0.5f - std::fabs(x), a.stageExt.height * 0.5f - std::fabs(z))
                               : 1.5f - std::sqrt(x * x + z * z);
        if (i == 0) head = di;
        else hands = std::min(hands, di);
    }
}

// environment depth: created stopped; runs only in WINDOW (with passthrough on and the spatial data permission)
static void initDepth(App& a) {
    a.depthFailed = true;   // (one try per run)
    PFN_xrCreateEnvironmentDepthProviderMETA createP = nullptr;
    PFN_xrCreateEnvironmentDepthSwapchainMETA createS = nullptr;
    PFN_xrEnumerateEnvironmentDepthSwapchainImagesMETA enumI = nullptr;
    PFN_xrGetEnvironmentDepthSwapchainStateMETA state = nullptr;
    bool ok = xrFn(a, "xrCreateEnvironmentDepthProviderMETA", createP) && xrFn(a, "xrCreateEnvironmentDepthSwapchainMETA", createS) &&
              xrFn(a, "xrEnumerateEnvironmentDepthSwapchainImagesMETA", enumI) && xrFn(a, "xrGetEnvironmentDepthSwapchainStateMETA", state) &&
              xrFn(a, "xrStartEnvironmentDepthProviderMETA", a.xrStartDepth_) && xrFn(a, "xrStopEnvironmentDepthProviderMETA", a.xrStopDepth_) &&
              xrFn(a, "xrAcquireEnvironmentDepthImageMETA", a.xrAcquireDepth_);
    XrResult r = XR_ERROR_FUNCTION_UNSUPPORTED;
    if (ok) {
        XrEnvironmentDepthProviderCreateInfoMETA pc{XR_TYPE_ENVIRONMENT_DEPTH_PROVIDER_CREATE_INFO_META};
        r = createP(a.session, &pc, &a.depthProvider);
        ok = XR_SUCCEEDED(r);
    }
    if (ok) {
        XrEnvironmentDepthSwapchainCreateInfoMETA sc{XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_CREATE_INFO_META};
        r = createS(a.depthProvider, &sc, &a.depthSwap);
        ok = XR_SUCCEEDED(r);
    }
    uint32_t n = 0;
    if (ok) ok = XR_SUCCEEDED(enumI(a.depthSwap, 0, &n, nullptr)) && n > 0;
    if (ok) {
        a.depthImages.assign(n, {kSwapImageType});
        ok = XR_SUCCEEDED(enumI(a.depthSwap, n, &n, (XrSwapchainImageBaseHeader*)a.depthImages.data()));
    }
    XrEnvironmentDepthSwapchainStateMETA st{XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_STATE_META};
    if (ok) state(a.depthSwap, &st);
    a.depthReady = ok;
    a.depthFailed = !ok;
    LOGI("environment depth: %s (%d; %u images %ux%u)", ok ? "ready" : "could not be set up", (int)r, n, st.width, st.height);
}
static void updateDepth(App& a, XrTime t) {
    if (!a.hasDepth) return;
    const double now = nowSeconds();
    const bool window = a.ptRunning && a.ptMode == PT_WINDOW;
    if (window && now > a.scenePermAt) {   // (checked once a second; the prompt comes up the first time WINDOW is chosen)
        a.scenePermAt = now + 1.0;
        const bool was = a.scenePerm;
        a.scenePerm = scenePermission(!a.depthAsked);
        if (!a.depthAsked) LOGI("environment depth: asking for the spatial data permission");
        a.depthAsked = true;
        if (a.scenePerm != was) LOGI("environment depth: spatial data permission %s", a.scenePerm ? "granted" : "not granted");
    }
    if (window && a.scenePerm && !a.depthReady && !a.depthFailed) initDepth(a);
    const bool want = window && a.scenePerm && a.depthReady;
    if (want != a.depthRunning) {
        const XrResult r = want ? a.xrStartDepth_(a.depthProvider) : a.xrStopDepth_(a.depthProvider);
        LOGI("environment depth: %s (%d)", want ? "started" : "stopped", (int)r);
        a.depthRunning = want && XR_SUCCEEDED(r);
        a.depthImgValid = false;
        if (want && XR_FAILED(r)) a.scenePermAt = now + 5.0;   // try again in a while
    }
    if (!a.depthRunning) return;
    XrEnvironmentDepthImageAcquireInfoMETA ai{XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_ACQUIRE_INFO_META};
    ai.space = a.appSpace;
    ai.displayTime = t;
    XrEnvironmentDepthImageMETA img{XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_META};
    img.views[0].type = img.views[1].type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_VIEW_META;
    if (XR_SUCCEEDED(a.xrAcquireDepth_(a.depthProvider, &ai, &img)) && img.swapchainIndex < a.depthImages.size()) {
        a.depthImg = img;
        a.depthImgValid = true;
        for (int i = 0; i < 2; ++i) {   // the depth cameras' views, in arena space
            const XrFovf& f = img.views[i].fov;
            a.depthView[i] = Mat4::viewFromPose(a.trackToWorld * fromXr(img.views[i].pose));
            a.depthProj[i] = Mat4::projection(f.angleLeft, f.angleRight, f.angleUp, f.angleDown, 0.05f, 100.f);   // (x, y only)
        }
    }
}

static void updatePassthrough(App& a) {
    // PEEK (hold Y) in any mode, OFF included: the room runs while Y is held and until the game has faded back in
    const bool peek = a.ptPeekOn && !a.spectating && (a.peekHeld || a.ptReveal > 0.f);
    const bool want = (a.ptMode != PT_OFF || peek) && a.ptLayer != XR_NULL_HANDLE && !a.ptFailed;
    if (want && a.ptRunning && a.ptStyleDirty && a.xrPassthroughLayerSetStyleFB_) {   // ROOM brightness, grey, edges
        a.ptStyleDirty = false;
        XrPassthroughBrightnessContrastSaturationFB bcs{XR_TYPE_PASSTHROUGH_BRIGHTNESS_CONTRAST_SATURATION_FB};
        bcs.brightness = 0.f;
        bcs.contrast = 1.f;
        bcs.saturation = a.ptMono ? 0.f : 1.f;
        XrPassthroughStyleFB st{XR_TYPE_PASSTHROUGH_STYLE_FB};
        st.next = &bcs;
        st.textureOpacityFactor = clampf(a.ptRoom, 0.f, 1.f);
        st.edgeColor = a.ptEdges ? XrColor4f{0.25f, 0.85f, 1.0f, 1.0f} : XrColor4f{0, 0, 0, 0};
        const XrResult r = a.xrPassthroughLayerSetStyleFB_(a.ptLayer, &st);
        LOGI("passthrough style: room %.0f%% %s, edges %s (%d)", a.ptRoom * 100, a.ptMono ? "grey" : "colour", a.ptEdges ? "on" : "off", (int)r);
    }
    if (want == a.ptRunning) return;
    XrResult r1, r2;
    if (want) { r1 = a.xrPassthroughStartFB_(a.ptHandle); r2 = a.xrPassthroughLayerResumeFB_(a.ptLayer); }
    else { r2 = a.xrPassthroughLayerPauseFB_(a.ptLayer); r1 = a.xrPassthroughPauseFB_(a.ptHandle); }
    if (want && (XR_FAILED(r1) || XR_FAILED(r2))) {
        LOGW("passthrough: start failed (%d, %d)", (int)r1, (int)r2);
        a.ptFailed = true;
        a.ptMode = PT_OFF;
        return;
    }
    a.ptRunning = want;
    a.ptStyleDirty = true;
    LOGI("passthrough: %s%s", want ? "running" : "paused", a.ptMode == PT_OFF ? " (peek)" : "");
}

static bool initSession(App& a) {
#ifdef __ANDROID__
    PFN_xrGetOpenGLESGraphicsRequirementsKHR getReq = nullptr;
    xrGetInstanceProcAddr(a.instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getReq);
    XrGraphicsRequirementsOpenGLESKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    XR_CHECK(getReq(a.instance, a.system, &req));
    XrGraphicsBindingOpenGLESAndroidKHR gb{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    gb.display = a.display;
    gb.config = a.config;
    gb.context = a.context;
#else
    PFN_xrGetOpenGLGraphicsRequirementsKHR getReq = nullptr;
    xrGetInstanceProcAddr(a.instance, "xrGetOpenGLGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getReq);
    XrGraphicsRequirementsOpenGLKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
    if (!getReq) { LOGE("runtime has no OpenGL support"); return false; }
    XR_CHECK(getReq(a.instance, a.system, &req));
    XrGraphicsBindingOpenGLWin32KHR gb{XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
    gb.hDC = a.hdc;
    gb.hGLRC = a.glrc;
#endif
    XrSessionCreateInfo sc{XR_TYPE_SESSION_CREATE_INFO};
    sc.next = &gb;
    sc.systemId = a.system;
    XR_CHECK(xrCreateSession(a.instance, &sc, &a.session));

    // Reference space: LOCAL_FLOOR (floor-level, user-facing origin) > STAGE > LOCAL.
    uint32_t n = 0;
    xrEnumerateReferenceSpaces(a.session, 0, &n, nullptr);
    std::vector<XrReferenceSpaceType> types(n);
    xrEnumerateReferenceSpaces(a.session, n, &n, types.data());
    a.spaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    for (auto t : types) if (t == XR_REFERENCE_SPACE_TYPE_STAGE) a.spaceType = t;
    if (a.hasLocalFloor) for (auto t : types) if (t == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT) a.spaceType = t;
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = a.spaceType;
    rs.poseInReferenceSpace.orientation.w = 1;
    if (a.spaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) rs.poseInReferenceSpace.position.y = -1.6f;   // approx. floor
    XR_CHECK(xrCreateReferenceSpace(a.session, &rs, &a.appSpace));
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    rs.poseInReferenceSpace.position.y = 0;
    XR_CHECK(xrCreateReferenceSpace(a.session, &rs, &a.headSpace));
    LOGI("reference space type %d", (int)a.spaceType);
    for (auto t : types)
        if (t == XR_REFERENCE_SPACE_TYPE_STAGE) {   // the play space's rectangle (ROOM AT EDGES)
            XrReferenceSpaceCreateInfo st{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
            st.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
            st.poseInReferenceSpace.orientation.w = 1;
            if (XR_FAILED(xrCreateReferenceSpace(a.session, &st, &a.stageSpace))) a.stageSpace = XR_NULL_HANDLE;
        }
    initPassthrough(a);
    LOGI("environment depth: %s", a.hasDepth ? "supported (asked for when WINDOW is chosen)" : "not on this headset");

    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo ac{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        ac.action = a.gripPose;
        ac.subactionPath = a.handPath[h];
        ac.poseInActionSpace.orientation.w = 1;
        XR_CHECK(xrCreateActionSpace(a.session, &ac, &a.handSpace[h]));
        ac.action = a.aimPose;
        XR_CHECK(xrCreateActionSpace(a.session, &ac, &a.aimSpace[h]));
    }
    XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    at.countActionSets = 1;
    at.actionSets = &a.actionSet;
    XR_CHECK(xrAttachSessionActionSets(a.session, &at));

    // swapchains
    xrEnumerateViewConfigurationViews(a.instance, a.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr);
    a.viewCfg.assign(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(a.instance, a.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n, a.viewCfg.data());
    a.views.assign(n, {XR_TYPE_VIEW});
    uint32_t nf = 0;
    xrEnumerateSwapchainFormats(a.session, 0, &nf, nullptr);
    std::vector<int64_t> formats(nf);
    xrEnumerateSwapchainFormats(a.session, nf, &nf, formats.data());
    int64_t fmt = formats.empty() ? GL_RGBA8 : formats[0];
    for (auto f : formats) if (f == GL_SRGB8_ALPHA8) fmt = f;
    for (uint32_t i = 0; i < n && i < 2; ++i) {
        Swap& s = a.swaps[i];
        s.w = a.viewCfg[i].recommendedImageRectWidth;
        s.h = a.viewCfg[i].recommendedImageRectHeight;
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        ci.format = fmt;
        ci.sampleCount = 1;
        ci.width = s.w;
        ci.height = s.h;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        XR_CHECK(xrCreateSwapchain(a.session, &ci, &s.handle));
        uint32_t ni = 0;
        xrEnumerateSwapchainImages(s.handle, 0, &ni, nullptr);
        s.images.assign(ni, {kSwapImageType});
        xrEnumerateSwapchainImages(s.handle, ni, &ni, (XrSwapchainImageBaseHeader*)s.images.data());
        glGenRenderbuffers(1, &s.depth);
        glBindRenderbuffer(GL_RENDERBUFFER, s.depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, s.w, s.h);
        glGenFramebuffers(1, &s.fbo);
        LOGI("eye %u swapchain %dx%d fmt 0x%llx images %u", i, s.w, s.h, (long long)fmt, ni);
    }
    return true;
}

// ------------------------------------------------------------------ assets
static std::vector<CollisionTri> loadCollision() {
    std::vector<CollisionTri> out;
    std::vector<uint8_t> file;
    if (!assetRead("arena.col", file) || file.size() < 8) { LOGE("arena.col missing"); return out; }
    out = parseCollision(file.data(), file.size());
    if (out.empty()) LOGE("arena.col unreadable");
    LOGI("collision: %u tris", (unsigned)out.size());
    return out;
}

// gltf mesh space -> UE cm -> UE motion-controller transform -> OpenXR grip space (see tools/build_open_assets.py)
static Mat4 ueLocalToGrip(Vec3 locCm, Vec3 pyrDeg, Vec3 scale) {
    float p = deg2rad(pyrDeg.x), y = deg2rad(pyrDeg.y), r = deg2rad(pyrDeg.z);
    float sp = std::sin(p), cp = std::cos(p), sy = std::sin(y), cy = std::cos(y), sr = std::sin(r), cr = std::cos(r);
    float R[3][3] = {{cp * cy, cp * sy, sp}, {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp}, {-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp}};
    Mat4 T = Mat4::identity();
    for (int c = 0; c < 3; ++c) for (int rr = 0; rr < 3; ++rr) T.m[c * 4 + rr] = R[c][rr] * (&scale.x)[c];
    T.m[12] = locCm.x; T.m[13] = locCm.y; T.m[14] = locCm.z;
    Mat4 S{};   // gltf (x,y,z) m -> UE (x,z,y) cm
    S.m[0] = 100; S.m[6] = 100; S.m[9] = 100; S.m[15] = 1;
    Mat4 C{};   // UE cm -> grip (y, z, -x) m
    C.m[2] = -0.01f; C.m[4] = 0.01f; C.m[9] = 0.01f; C.m[15] = 1;
    return C * T * S;
}

static Mat4 g_lancerLocal, g_shieldLocal, g_knuckleLocal, g_plateLocal;

// <model>.swing: 'centre x y z', then per chain 'chain k c follow limit' and its rest points 'x y z' (head space)
static void loadSwing(App& a, const char* model) {
    std::vector<uint8_t> raw;
    if (!assetRead(std::string(model) + ".swing", raw)) return;
    const std::string txt(raw.begin(), raw.end());
    App::SwingRig rig;
    int next = 1;   // the next free bone (0 is the head)
    for (size_t i = 0; i < txt.size();) {
        size_t e = txt.find('\n', i);
        if (e == std::string::npos) e = txt.size();
        const std::string ln = txt.substr(i, e - i);
        i = e + 1;
        float x, y, z, w;
        if (std::sscanf(ln.c_str(), "centre %f %f %f", &x, &y, &z) == 3) {
            rig.centre = {x, y, z};
        } else if (ln.compare(0, 4, "exit") == 0) {
            rig.exits = true;
        } else if (std::sscanf(ln.c_str(), "chain %f %f %f %f", &x, &y, &z, &w) == 4) {
            if (rig.chains.size() >= 6) { LOGW("%s.swing: too many chains", model); return; }
            App::SwingChain c;
            c.k = x; c.c = y; c.follow = z; c.limitCos = std::cos(deg2rad(w)); c.bone0 = next;
            rig.chains.push_back(c);
        } else if (std::sscanf(ln.c_str(), "%f %f %f", &x, &y, &z) == 3 && !rig.chains.empty()) {
            App::SwingChain& c = rig.chains.back();
            if (c.n >= 7) { LOGW("%s.swing: a chain too long", model); return; }
            if (c.n) ++next;
            c.R[c.n++] = {x, y, z};
        }
    }
    for (App::SwingChain& c : rig.chains) {
        if (c.n < 2) { LOGW("%s.swing: a chain without a segment", model); return; }
        for (int j = 0; j < c.n; ++j) c.rmin[j] = 0.97f * length(c.R[j] - rig.centre);   // (it can't swing into the head)
    }
    if (rig.chains.empty() || next > 7) { LOGW("%s.swing: no chains or more than 6 bones", model); return; }
    rig.id = (int)a.swingRigs.size();
    a.swingRigs[model] = rig;
}

// <model>.tuck: 'TUCK', u32 w, h, f32 centre xyz, w*h radius bytes
static void loadTuck(const char* model, App::Tuck& t) {
    std::vector<uint8_t> d;
    if (!assetRead(std::string(model) + ".tuck", d) || d.size() < 24 || std::memcmp(d.data(), "TUCK", 4) != 0) return;
    uint32_t w, h;
    std::memcpy(&w, d.data() + 4, 4);
    std::memcpy(&h, d.data() + 8, 4);
    if (w == 0 || h == 0 || w > 256 || h > 256 || d.size() < 24 + (size_t)w * h) return;
    std::memcpy(t.c, d.data() + 12, 12);
    t.w = (int)w; t.h = (int)h;
    t.v.assign(d.begin() + 24, d.begin() + 24 + (size_t)w * h);
    t.ok = true;
}

static bool initContent(App& a) {
    const double loadStart = nowSeconds();   // (the log says how long loading took: compressed assets, 2026-10-18)
    if (!a.gfx.init()) return false;
    bool ok = a.gfx.loadModel("arena", a.arena);
    a.gfx.loadModel("environment", a.environment);
    a.gfx.loadModel("projectile", a.projectile);
    a.gfx.loadModel("projectile_shell", a.shell);
    a.gfx.loadModel("spawner", a.spawner);
    a.gfx.loadModel("shield", a.shield);
    a.gfx.loadModel("lancer", a.lancer);
    a.gfx.loadModel("knuckleguard", a.knuckle);
    for (int k = 0; k < 3; ++k) { char gn[16]; snprintf(gn, sizeof gn, "gauntlet_%d", k); a.gfx.loadModel(gn, a.gauntlets[k]); }
    for (int k = 0; k < 3; ++k) {   // optional (the open asset set has them)
        char gn[24];
        snprintf(gn, sizeof gn, "gauntlet_%d_hand", k); a.gfx.loadModel(gn, a.gauntletHand[k]);
        snprintf(gn, sizeof gn, "gauntlet_%d_cuff", k); a.gfx.loadModel(gn, a.gauntletCuff[k]);
    }
    a.gfx.loadModel("head", a.head);
    a.gfx.loadModel("torso", a.torso);
    if (assetExists("floor.mdl")) {
        a.gfx.loadModel("floor", a.floor);
        a.gfx.loadModel("floor_reflect", a.floorReflect);
        a.gfx.loadModel("court_fx", a.courtFx);
        a.gfx.loadModel("stadium_fx", a.stadiumFx);               // (open asset set; older sets have them in court_fx)
        a.gfx.loadModel("floor_reflect_env", a.floorReflectEnv);
        a.gfx.loadModel("court_outline", a.courtOutline);
        a.gfx.loadModel("env_fx", a.envFx);
        a.gfx.loadModel("court_pulse", a.courtPulse);
    }
    a.startTime = nowSeconds();
    {   // The Crowd's burst pictures appear mid-match: loaded now (a texture loaded on first use stalls that frame; a
        // tester's frame-time graph showed small spikes). A country's flag loads when a player first shows it, before
        // the match (all 205 at start-up took 1.6 s on a PC: too long a wait)
        const double t0 = nowSeconds();
        for (const char* fx : {"fx_heart", "fx_star", "fx_hotdog", "fx_smile", "fx_foam"}) a.gfx.texture(fx, true);
        LOGI("preloaded the crowd's pictures in %.0f ms", (nowSeconds() - t0) * 1000.0);
    }
    a.gfx.loadModel("strikezone", a.strikezone);
    if (assetExists("av_bhead_m.mdl")) {
        char n[32];
        auto part = [&](const char* name) { Model m; if (a.gfx.loadModel(name, m)) a.avModels[name] = m; };
        for (const char bk : {'m', 'f'}) {   // the skinned MPFB bodies (tools/build_body_parts.py)
            snprintf(n, sizeof n, "av_bhead_%c", bk); part(n);
            snprintf(n, sizeof n, "av_wfade_%c", bk); part(n);
            for (int k = 0; k < 14; ++k) { snprintf(n, sizeof n, "av_top_%d_%c", k, bk); part(n); }
            for (int k = 10; k < 14; ++k) { snprintf(n, sizeof n, "av_tpulse_%d_%c", k, bk); part(n); }   // the moving pulses
            for (int k = 1; k <= 3; ++k) { snprintf(n, sizeof n, "av_bottom_%d_%c", k, bk); part(n); }
            // head parts fitted to this body's head (tools/blender/make_avatar.py --head)
            const int bi = bk == 'f' ? 1 : 0;
            for (int k = 1; k <= 8; ++k) { snprintf(n, sizeof n, "av_hair_%d_%c", k, bk); part(n); loadSwing(a, n); }
            for (int k : {3, 7}) { snprintf(n, sizeof n, "av_hair_%dt_%c", k, bk); part(n); loadSwing(a, n); }   // tail, loose ends
            for (int k = 1; k <= 7; ++k) { snprintf(n, sizeof n, "av_visor_%d_%c", k, bk); part(n); loadTuck(n, a.tuckVisor[bi][k]); }
            for (int k = 1; k <= 6; ++k) { snprintf(n, sizeof n, "av_gear_%d_%c", k, bk); part(n); loadTuck(n, a.tuckGear[bi][k]); }
            for (int k = 1; k <= 5; ++k) { snprintf(n, sizeof n, "av_mask_%d_%c", k, bk); part(n); }
            for (int f = 1; f < avatar::kFaces; ++f) {   // FACE 2..4: their heads, and face shields / masks fitted to them
                snprintf(n, sizeof n, "av_bhead_%c%d", bk, f); part(n);
                for (int k = 1; k <= 7; ++k) { snprintf(n, sizeof n, "av_visor_%d_%c%d", k, bk, f); part(n); }
                for (int k = 1; k <= 5; ++k) { snprintf(n, sizeof n, "av_mask_%d_%c%d", k, bk, f); part(n); }
            }
        }
        for (int k = 1; k < 8; ++k) if (const char* t = avatar::patternTexture(k)) a.patternTex[k] = a.gfx.texture(t, false);
        LOGI("avatars: %d part models, %d hair swing rigs", (int)a.avModels.size(), (int)a.swingRigs.size());
    }
    // VisualTransform (1,-0.5,0) pitch 5; ShieldTransform (20,0,10) roll 180 scale 10;
    // KnuckleGuardTransform (10.5,0,0)
    g_lancerLocal = ueLocalToGrip({1, -0.5f, 0}, {5, 0, 0}, {1, 1, 1});
    g_shieldLocal = ueLocalToGrip({20, 0, 10}, {0, 0, 180}, {10, 10, 10});
    g_knuckleLocal = ueLocalToGrip({1.5f, 0, 0}, {0, 0, 0}, {1, 1, 1});   // KnuckleGuardTransform (10.5) + TempMesh (-9)
    g_plateLocal = ueLocalToGrip({0, 0, 0}, {0, 0, 180}, {10, 10, 10});   // Shield_01 plate centred, normal = grip Y
    a.classicCol = loadCollision();
    a.game.useArena(ARENA_CLASSIC, a.classicCol);
    a.game.reset(true);
    // team 0 = (0.1,0.7,1.0), team 1 = (1.0,0.4,0.1)
    float c0[4] = {0.1f, 0.7f, 1.0f, 1}, c1[4] = {1.0f, 0.4f, 0.1f, 1};
    a.gfx.setTeamColors(c0, c1, 1.0f, 1.0f);
    a.audio.init();
    LOGI("content loaded in %.2f s", nowSeconds() - loadStart);
    return ok;
}

// ------------------------------------------------------------------ frame
static void openMenu(App& a, const PlayerInput& in);
static void updateMenu(App& a, PlayerInput& in);
static void openBuilder(App& a, const PlayerInput& in);
static void openTuning(App& a, const PlayerInput& in);
static void closeTuning(App& a);
static void updateTuning(App& a, PlayerInput& in);
static void loadTuning(App& a);
static void openDirector(App& a, const PlayerInput& in);
static void updateDirector(App& a, PlayerInput& in);
static std::vector<uint8_t> packWarps(const App& a);
static bool unpackWarps(App& a, const std::vector<uint8_t>& b);
static void savePlayerName(App& a, const char* name);
static void saveSettings(App& a);
static void closeBuilder(App& a);
static void updateBuilder(App& a, PlayerInput& in);

// ------------------------------------------------------------------ bug reports
static const char* kBugCats[6] = {"GAMEPLAY", "MULTIPLAYER", "AVATAR / OUTFIT", "MENUS / TEXT", "GRAPHICS", "OTHER"};
static const char* kAppVersion = "0.3.11";

// "0.3.4" newer than "0.3.3" (dotted numbers, compared part by part)
static bool versionNewer(const char* a, const char* b) {
    for (;;) {
        char* ea;
        char* eb;
        const long x = std::strtol(a, &ea, 10), y = std::strtol(b, &eb, 10);
        if (x != y) return x > y;
        if (*ea != '.' && *eb != '.') return false;
        a = *ea == '.' ? ea + 1 : ea;
        b = *eb == '.' ? eb + 1 : eb;
        if (!*a && !*b) return false;
    }
}
// the relay says a newer release is out: the title panel (and the PC spectator's hub) say where to get it
static const char* updateAvailable(App& a) {
    const char* v = a.net.latestVersion();
    if (!v[0] || !versionNewer(v, kAppVersion)) return nullptr;
    static std::string shown;
    if (shown != v) { shown = v; LOGI("update: %s is out (this is %s)", v, kAppVersion); }
    return v;
}
static const size_t kNoteMax = 120;   // bug / player report notes (was 60: testers ran out)
static const char* kReportWhy[4] = {"HARASSMENT OR HATE", "OFFENSIVE NAME OR LOOK", "CHEATING", "OTHER"};

static std::vector<Net::Person> reportPeople(App& a) { return a.reportFake.empty() ? a.net.peopleHere() : a.reportFake; }
static bool isMuted(const App& a, uint32_t id) { return std::find(a.mutedIds.begin(), a.mutedIds.end(), id) != a.mutedIds.end(); }
static const char* roleText(uint8_t role) {
    return role == Net::VOICE_PLAYER ? "PLAYING" : role == Net::VOICE_SPECTATOR ? "WATCHING" : "WAITING";
}

// what: the report's own lines (a player report); empty: the bug page's category and note
static std::string bugReportText(App& a, const char* title = "Project Arena bug report", const std::string& what = "") {
    std::string r;
    char b[256];
    const time_t now = time(nullptr);
    strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", localtime(&now));
    r += std::string(title) + "\n";
    r += std::string("player: ") + a.net.name() + "\n";
    r += std::string("time: ") + b + "\n";
    if (what.empty()) {
        r += std::string("category: ") + (a.bugCat >= 0 ? kBugCats[a.bugCat] : "-") + "\n";
        r += "note: " + (a.bugNote.empty() ? std::string("-") : a.bugNote) + "\n";
    } else {
        r += what;
    }
#ifdef __ANDROID__
    char model[92] = "?";
    __system_property_get("ro.product.model", model);
    snprintf(b, sizeof b, "build: %s Quest (%s)\n", kAppVersion, model);
#else
    snprintf(b, sizeof b, "build: %s PC\n", kAppVersion);
#endif
    r += b;
    snprintf(b, sizeof b, "network: %s, %s, server %s\n", a.net.status(), a.net.online() ? "online" : "LAN",
             a.serverAddr.empty() ? "-" : a.serverAddr.c_str());
    r += b;
    if (a.net.pingMs() >= 0) {   // the latest round trip to the host (or, hosting, to a player)
        snprintf(b, sizeof b, "ping: %.0f ms\n", a.net.pingMs());
        r += b;
    }
    snprintf(b, sizeof b, "room: in room %d, slot %d, waiting %d, queue %d, spectating %d\n", a.net.inRoom() ? 1 : 0, a.net.localSlot(),
             a.net.localWaiting() ? 1 : 0, a.net.localQueuePos(), a.spectating ? 1 : 0);
    r += b;
    snprintf(b, sizeof b, "match: state %d, score %d-%d, rules %s\n", (int)a.game.match, a.game.pl[0].score, a.game.pl[1].score,
             a.game.tun.advanced ? "advanced" : "basic");
    r += b;
    const Vec3 h = a.localIn.head.p;
    snprintf(b, sizeof b, "head: (%.2f %.2f %.2f)\n", h.x, h.y, h.z);
    r += b;
    const AvatarDesc& d = a.myAvatar;
    snprintf(b, sizeof b, "avatar: body %d suit %d build %d lower %d hair %d gear %d visor %d mask %d gauntlet %d\n", d.body, d.suit, d.build,
             d.lower, d.hair, d.headgear, d.visor, d.mask, d.gauntlet);
    r += b;
    snprintf(b, sizeof b, "settings: volume %d, pointer %s, voice %d, fx %d, bot fill %d\n", a.volume, a.pointerHand ? "right" : "left",
             a.voiceMode, a.fxLevel, a.botFill ? 1 : 0);
    r += b;
    // the end of the log (what happened just before)
    if (const char* dir = dataDir(a))
        if (FILE* f = fopen((std::string(dir) + "/projectarena_log.txt").c_str(), "rb")) {
            fseek(f, 0, SEEK_END);
            long size = ftell(f), from = size > 60000 ? size - 60000 : 0;
            fseek(f, from, SEEK_SET);
            std::string tail((size_t)(size - from), '\0');
            size_t got = fread(&tail[0], 1, tail.size(), f);
            tail.resize(got);
            fclose(f);
            if (from > 0) { size_t nl = tail.find('\n'); if (nl != std::string::npos) tail.erase(0, nl + 1); }
            r += "\n---- log (last " + std::to_string(got / 1024) + " kB) ----\n" + tail;
        }
    return r;
}

// saved in files/bug_reports/; anything not delivered stays listed in bug_outbox.txt and is retried
static void bugSendAll(App& a, std::vector<std::string> files) {
    if (a.bugSending.exchange(true)) return;
    const std::string server = a.serverAddr, dir = dataDir(a) ? dataDir(a) : "";
    std::thread([&a, files, server, dir]() {
        std::vector<std::string> left;
        std::string err;
        int sent = 0;
        for (const std::string& fn : files) {
            std::string text;
            if (FILE* f = fopen(fn.c_str(), "rb")) {
                char buf[8192];
                size_t k;
                while ((k = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, k);
                fclose(f);
            } else continue;
            if (!server.empty() && sendBugReport(server.c_str(), text, err)) ++sent;
            else left.push_back(fn);
        }
        if (!dir.empty())
            if (FILE* o = fopen((dir + "/bug_outbox.txt").c_str(), "w")) {
                for (const std::string& fn : left) fprintf(o, "%s\n", fn.c_str());
                fclose(o);
            }
        {
            std::lock_guard<std::mutex> lk(a.bugMx);
            if (left.empty()) a.bugStatus = sent ? "REPORT SENT - THANK YOU" : "";
            else a.bugStatus = std::string("SAVED - WILL SEND LATER (") + (server.empty() ? "NO SERVER SET" : err) + ")";
        }
        LOGI("bug reports: %d sent, %d waiting%s%s", sent, (int)left.size(), left.empty() ? "" : ": ", left.empty() ? "" : err.c_str());
        a.bugDone = left.empty() ? 1 : 2;
        a.bugSending = false;
    }).detach();
}

static std::vector<std::string> bugOutbox(App& a) {
    std::vector<std::string> v;
    if (const char* dir = dataDir(a))
        if (FILE* f = fopen((std::string(dir) + "/bug_outbox.txt").c_str(), "r")) {
            char line[512];
            while (fgets(line, sizeof line, f)) {
                std::string l = line;
                while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
                if (!l.empty() && fileExists(l)) v.push_back(l);
            }
            fclose(f);
        }
    return v;
}

static void submitReport(App& a, const std::string& text, const char* what) {
    std::string fn;
    if (const char* dir = dataDir(a)) {
        std::string folder = std::string(dir) + "/bug_reports";
#ifdef _WIN32
        CreateDirectoryA(folder.c_str(), nullptr);
#else
        mkdir(folder.c_str(), 0770);
#endif
        char name[64];
        const time_t now = time(nullptr);
        strftime(name, sizeof name, "/report_%Y%m%d_%H%M%S", localtime(&now));
        fn = folder + name;
        for (int k = 2; fileExists(fn + ".txt"); ++k) fn = folder + name + "_" + std::to_string(k);
        fn += ".txt";
        if (FILE* f = fopen(fn.c_str(), "wb")) { fwrite(text.data(), 1, text.size(), f); fclose(f); }
    }
    LOGI("%s -> %s", what, fn.c_str());
    std::vector<std::string> files = bugOutbox(a);
    if (!fn.empty()) files.push_back(fn);
    { std::lock_guard<std::mutex> lk(a.bugMx); a.bugStatus = "SENDING..."; }
    bugSendAll(a, files);
}

static void bugSubmit(App& a) {
    std::string what = std::string("bug report: ") + (a.bugCat >= 0 ? kBugCats[a.bugCat] : "-") + ", \"" + a.bugNote + "\"";
    submitReport(a, bugReportText(a), what.c_str());
}

// a player report: who, why, where, and everyone else who was there (for the moderators)
static void playerReportSubmit(App& a) {
    char b[160];
    std::string w;
    std::string role = "-";
    for (const Net::Person& p : reportPeople(a)) if (p.id == a.reportWho) role = roleText(p.role);
    snprintf(b, sizeof b, "reported: %s (session %08X, %s)\n", a.reportName, a.reportWho, role.c_str());
    w += b;
    w += std::string("reason: ") + (a.reportWhy >= 0 ? kReportWhy[a.reportWhy] : "-") + "\n";
    w += "note: " + (a.reportNote.empty() ? std::string("-") : a.reportNote) + "\n";
    snprintf(b, sizeof b, "reporter: %s (session %08X)\n", a.net.name(), a.net.session());
    w += b;
    w += "where: room " + (a.net.hereName().empty() ? std::string("-") : a.net.hereName()) + (a.spectating ? " (spectating)" : "") + "\n";
    w += "people here:";
    for (const Net::Person& p : reportPeople(a)) {
        snprintf(b, sizeof b, " %s (%08X, %s);", p.name, p.id, roleText(p.role));
        w += b;
    }
    w += "\n";
    LOGI("player report: %s, %s", a.reportName, a.reportWhy >= 0 ? kReportWhy[a.reportWhy] : "-");
    submitReport(a, bugReportText(a, "Project Arena player report", w), "player report");
}

// EXPERIMENTAL cross-play: the player's own name with the Project Arena tag, so the Sparc server (and its
// leaderboard) can tell where a player comes from: "mojo" -> "mojo_PA". The Reviver server takes letters, digits
// and underscores only (Blakey, 2026-10-15), so anything else becomes an underscore.
static std::string sparcName(App& a) {
    std::string n;
    for (const char* c = a.net.name(); *c && n.size() < 16; ++c) {
        char ch = *c;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_') n += ch;
        else if (ch == ' ' || ch == '.' || ch == '-' || ch == ':') n += '_';
    }
    if (n.empty()) n = "PLAYER";
    return n + "_PA";
}

// a random id kept with the app's data (the stock client sends "<PC name>-<id>"; we never send a device name)
static std::string sparcId(App& a) {
    if (!a.sparcId.empty()) return a.sparcId;
    std::string f = dataDir(a) ? std::string(dataDir(a)) + "/xc_id" : std::string();
    if (!f.empty())
        if (FILE* fp = fopen(f.c_str(), "r")) {
            char b[80] = {};
            if (fgets(b, sizeof b, fp)) a.sparcId = b;
            fclose(fp);
            while (!a.sparcId.empty() && (a.sparcId.back() == '\n' || a.sparcId.back() == '\r')) a.sparcId.pop_back();
        }
    if (a.sparcId.size() < 16) {
        uint32_t x = (uint32_t)(nowSeconds() * 1e6) ^ 0x9e3779b9u ^ (uint32_t)(uintptr_t)&a;
        char hex[33];
        for (int i = 0; i < 32; ++i) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            hex[i] = "0123456789ABCDEF"[x & 15];
        }
        hex[32] = 0;
        a.sparcId = std::string("ProjectArena-") + hex;
        if (!f.empty())
            if (FILE* fp = fopen(f.c_str(), "w")) { fprintf(fp, "%s\n", a.sparcId.c_str()); fclose(fp); }
    }
    return a.sparcId;
}

static void haptic(App& a, int hand, float amp, float seconds) {
    if (a.session == XR_NULL_HANDLE || hand < 0 || hand > 1) return;   // no headset (desktop self-tests)
    XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
    v.amplitude = amp;
    v.duration = (XrDuration)(seconds * 1e9);
    v.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
    hi.action = a.haptic;
    hi.subactionPath = a.handPath[hand];
    xrApplyHapticFeedback(a.session, &hi, (XrHapticBaseHeader*)&v);
}

static bool getBool(App& a, XrAction act, XrPath sub = XR_NULL_PATH) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = act; gi.subactionPath = sub;
    XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
    xrGetActionStateBoolean(a.session, &gi, &s);
    return s.isActive && s.currentState;
}
static float getFloat(App& a, XrAction act, XrPath sub) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = act; gi.subactionPath = sub;
    XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
    xrGetActionStateFloat(a.session, &gi, &s);
    return s.isActive ? s.currentState : 0.f;
}

// Place the player at Start_Player<local> facing the opponent (player 0 faces +X, player 1 faces -X),
// keeping their real height.
// Where the viewer stands and faces: a player at their end of the court, a spectator in the hub (facing the
// miniatures) or on the floating gallery seat beside the court (1 m up, looking across it).
// the lobby: a balcony 2.5 m up on the +Z side of the practice court, facing it
static const float kLobbyFloor = 2.5f;
static const Vec3 kLobbySpot = {0, kLobbyFloor, 5.2f};
static bool inLobby(const App& a) { return a.lobby && !a.spectating && a.net.mode() == Net::Mode::Offline; }
static float floorHere(const App& a) { return inLobby(a) ? kLobbyFloor : 0.f; }

// holding area outside the tank (waiting players): four spots along the -Z side, facing the glass
static Vec3 holdingSpot(int spot) { return {-4.5f + 3.0f * (spot & 3), 0, -4.2f}; }

// the fist bump start, face to face in the centre: in a room, and in practice against the bot (FIST BUMP FIRST)
static bool meetHere(const App& a) {
    return a.game.match == MatchState::Meet && (a.net.inRoom() || (a.net.mode() == Net::Mode::Offline && !inLobby(a)));
}

static void viewTarget(const App& a, int local, Vec3& pos, Vec3& fwd) {
    if (inLobby(a)) {
        pos = kLobbySpot;
        fwd = {0, 0, -1};
    } else if (a.net.localWaiting()) {
        pos = holdingSpot(a.net.localSpot());
        fwd = {0, 0, 1};
    } else if (a.spectating) {
        if (a.watchIp) { pos = {0, 1.0f, 5.4f}; fwd = {0, 0, -1}; }
        else { pos = {0, 0, 0}; fwd = {1, 0, 0}; }
    } else if (meetHere(a)) {   // face to face in the centre (fist bump start)
        pos = {local == 0 ? -a.game.tun.meetSpot : a.game.tun.meetSpot, 0, 0};
        fwd = local == 0 ? Vec3{1, 0, 0} : Vec3{-1, 0, 0};
    } else {
        pos = a.game.tun.start[local];
        fwd = local == 0 ? Vec3{1, 0, 0} : Vec3{-1, 0, 0};
    }
}

static void saveWarps(App& a);

// Place the viewer's head at `target` (arena space, facing target's forward) by moving the tracking origin.
static void placeHead(App& a, const Pose& headTrack, const Pose& target) {
    Vec3 f = headTrack.q.rotate({0, 0, -1});
    float yaw = std::atan2(-f.x, -f.z);
    Vec3 tf = target.q.rotate({0, 0, -1});
    Quat q = Quat::axisAngle({0, 1, 0}, std::atan2(-tf.x, -tf.z)) * Quat::axisAngle({0, 1, 0}, -yaw);
    a.trackToWorld.q = q;
    a.trackToWorld.p = target.p - q.rotate(headTrack.p);
}

static void startWarp(App& a, const Pose& target) {
    a.fadeTarget = target;
    a.fadeCapture = true;   // this frame is captured; the move and the cross-fade start on the next one
}

static const float kHoldSecs[4] = {2, 4, 8, 15};

static void goToCam(App& a, const Pose& headTrack, int idx) {
    if (idx < 0 || idx >= (int)a.warps.size()) return;
    a.warpIdx = idx;
    if (a.warps[idx].trans == 0) placeHead(a, headTrack, a.warps[idx].pose);   // cut
    else startWarp(a, a.warps[idx].pose);                                      // cross-fade into it
}

// the ball the director follows: the one in flight, else the one closest to being thrown
static bool liveBall(const App& a, Vec3& out) {
    for (int p = 0; p < 2; ++p) {
        const Ball& b = a.game.pl[p].ball;
        if (b.state == BallState::Flying || b.state == BallState::Returning) { out = b.pos; return true; }
    }
    for (int p = 0; p < 2; ++p) if (a.game.pl[p].ball.state == BallState::Held) { out = a.game.pl[p].ball.pos; return true; }
    return false;
}

// Auto camera: timer and follow cameras take turns (each for its hold time); an action camera takes over when a
// hit / strike / match end happens near it; a follow camera glides along the court with the ball.
static void director(App& a, const Pose& headTrack, const PlayerInput& in, float dt, double t) {
    if (!a.autoCam || a.warps.empty() || a.fadeCapture || a.fadePending) return;
    if (a.dirRequest >= 0) {
        int r = a.dirRequest;
        a.dirRequest = -1;
        goToCam(a, headTrack, r);
        a.autoNext = t + kHoldSecs[a.warps[r].hold & 3];
        return;
    }
    if (t > a.autoNext) {
        int n = (int)a.warps.size();
        for (int k = 1; k <= n; ++k) {
            int i = (a.warpIdx + k + n) % n;
            if (a.warps[i].trig != 1 || k == n) { goToCam(a, headTrack, i); a.autoNext = t + kHoldSecs[a.warps[i].hold & 3]; break; }
        }
        return;
    }
    if (a.warpIdx >= 0 && a.warpIdx < (int)a.warps.size() && a.warps[a.warpIdx].trig == 2) {
        Vec3 b;
        if (liveBall(a, b)) {
            float target = clampf(b.x, -8.f, 8.f);
            a.trackToWorld.p.x += (target - in.head.p.x) * (1.f - std::exp(-dt * 1.8f));
        }
    }
}

// game events pick action cameras: the nearest action camera to a hit / strike / match end
static void directorEvents(App& a) {
    if (!a.autoCam || a.warps.empty()) return;
    for (const GameEvent& e : a.game.events) {
        if (e.type != EventType::HitPlayer && e.type != EventType::Strike && e.type != EventType::MatchEnd) continue;
        int best = -1;
        float bd = 1e9f;
        for (int i = 0; i < (int)a.warps.size(); ++i) {
            if (a.warps[i].trig != 1 || i == a.warpIdx) continue;
            float d = length(a.warps[i].pose.p - e.pos);
            if (d < bd) { bd = d; best = i; }
        }
        if (best >= 0) a.dirRequest = best;
    }
}

static void addCamHere(App& a, const PlayerInput& in) {
    Vec3 f = in.head.q.rotate({0, 0, -1});
    f.y = 0;
    f = length(f) > 1e-3f ? normalize(f) : Vec3{0, 0, -1};
    App::WarpCam w;
    w.pose = {Quat::axisAngle({0, 1, 0}, std::atan2(-f.x, -f.z)), in.head.p};
    if (a.warps.size() >= 8) a.warps.erase(a.warps.begin());
    a.warps.push_back(w);
    a.warpIdx = (int)a.warps.size() - 1;
    saveWarps(a);
    a.net.noticeLocal("CAMERA SAVED");
}

// Watching a match: left stick flies (head-relative), right stick turns (SETTINGS > TURNING: snap 30 deg, or smooth)
// and rises / sinks;
// A saves a camera here, X goes to the next one, Y toggles the auto camera. MENU > DIRECTOR TOOLS sets them up.
static void spectatorCamera(App& a, const Pose& headTrack, const PlayerInput& in) {
    const double t = nowSeconds();
    const float dt = a.lastFly > 0 ? (float)std::min(0.05, t - a.lastFly) : 0.f;
    a.lastFly = t;
    auto stick = [&](XrAction act) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = act;
        XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
        xrGetActionStateVector2f(a.session, &gi, &st);
        return st.isActive ? Vec2{st.currentState.x, st.currentState.y} : Vec2{};
    };
    Vec2 l = stick(a.thumbstickL), r = stick(a.thumbstick);
    Vec3 f = in.head.q.rotate({0, 0, -1});
    f.y = 0;
    f = length(f) > 1e-3f ? normalize(f) : Vec3{0, 0, -1};
    Vec3 right = cross(f, Vec3{0, 1, 0});
    auto dz = [](float v) { return std::fabs(v) < 0.15f ? 0.f : v; };
    Vec3 v = (f * dz(l.y) + right * dz(l.x)) * 3.0f + Vec3{0, dz(r.y) * 1.6f, 0};
    if (length2(v) > 0) { a.trackToWorld.p += v * dt; if (a.autoCam) a.net.noticeLocal("AUTO CAMERA OFF"); a.autoCam = false; }
    if (a.turnMode != 0) {   // smooth: turn while the stick is held (faster the further it is pushed)
        const float x = std::fabs(r.x) < 0.2f ? 0.f : (r.x - (r.x > 0 ? 0.2f : -0.2f)) / 0.8f;
        if (x != 0.f) {
            const float rate = (a.turnMode == 2 ? 150.f : 75.f) * kPi / 180.f;   // degrees a second at full tilt
            Quat q = Quat::axisAngle({0, 1, 0}, -x * rate * dt);
            Vec3 h = in.head.p;
            a.trackToWorld.q = q * a.trackToWorld.q;
            a.trackToWorld.p = h + q.rotate(a.trackToWorld.p - h);
        }
    } else if (!a.turnLatch && std::fabs(r.x) > 0.75f) {
        a.turnLatch = true;
        Quat q = Quat::axisAngle({0, 1, 0}, r.x > 0 ? -kPi / 6 : kPi / 6);
        Vec3 h = in.head.p;
        a.trackToWorld.q = q * a.trackToWorld.q;
        a.trackToWorld.p = h + q.rotate(a.trackToWorld.p - h);
    } else if (std::fabs(r.x) < 0.3f) {
        a.turnLatch = false;
    }
    const bool X = getBool(a, a.buttonX), A = getBool(a, a.buttonA) && !X, Y = getBool(a, a.buttonY);
    if (A && !a.prevA2 && !X && !a.dirOpen) {
        addCamHere(a, in);
        haptic(a, 1, 0.4f, 0.04f);
    }
    if (X && !a.prevX && !a.warps.empty()) {
        goToCam(a, headTrack, (a.warpIdx + 1) % (int)a.warps.size());
        a.autoCam = false;
    }
    if (Y && !a.prevY2) {
        a.autoCam = !a.autoCam && !a.warps.empty();
        a.autoNext = t + 0.1;
        a.net.noticeLocal(a.autoCam ? "AUTO CAMERA ON" : "AUTO CAMERA OFF");
    }
    director(a, headTrack, in, dt, t);
    a.prevA2 = A; a.prevX = X; a.prevY2 = Y;
}

static void recenter(App& a, const Pose& headTrack, int local) {
    Vec3 f = headTrack.q.rotate({0, 0, -1});
    float yaw = std::atan2(-f.x, -f.z);   // heading of the head in tracking space (0 = -Z)
    Vec3 pos, fwd;
    viewTarget(a, local, pos, fwd);
    // rotation that takes the head's horizontal forward onto the target direction
    Quat toX = Quat::axisAngle({0, 1, 0}, std::atan2(-fwd.x, -fwd.z)) * Quat::axisAngle({0, 1, 0}, -yaw);
    Vec3 headXZ{headTrack.p.x, 0, headTrack.p.z};
    a.trackToWorld.q = toX;
    a.trackToWorld.p = pos - toX.rotate(headXZ);
    a.trackToWorld.p.y += a.floorFix;   // (the floor guard, if the headset has lost its floor)
    a.recentered = true;
    LOGI("recentered: yaw %.1f deg", rad2deg(yaw));
}

static int g_lastLocal = 0;
static PlayerInput readInput(App& a, XrTime t, int local) {
    PlayerInput in;
    XrActiveActionSet aas{a.actionSet, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &aas;
    xrSyncActions(a.session, &si);

    XrSpaceLocation hl{XR_TYPE_SPACE_LOCATION};
    xrLocateSpace(a.headSpace, a.appSpace, t, &hl);
    bool headOk = (hl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (hl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
    Pose headTrack = fromXr(hl.pose);
    bool rc = getBool(a, a.buttonB) || getBool(a, a.thumbClick, a.handPath[0]) || getBool(a, a.thumbClick, a.handPath[1]);
    if (local != g_lastLocal) { a.recentered = false; g_lastLocal = local; }
    {   // moving between playing / the hub / watching a match re-places the viewer
        uint32_t key = a.spectating ? (a.watchIp ? a.watchIp : 1u) : a.net.localWaiting() ? 10u + (uint32_t)a.net.localSpot()
                     : meetHere(a) ? 2u : inLobby(a) ? 3u : 0u;   // the centre meeting, then your end
        if (key != a.viewKey) { a.viewKey = key; a.recentered = false; }
    }
    if (headOk && !a.spectating) {   // floor guard (see App::floorFix)
        const double now = nowSeconds();
        const float hy = headTrack.p.y;   // above the headset's floor
        if (hy > 1.0f && hy < 2.2f && a.floorFix == 0.f) a.standH += (hy - a.standH) * 0.002f;   // your usual height
        if (hy < 0.45f || hy > 2.4f) {   // impossibly low or high: the headset's floor is wrong (it once jumped 2.8 m)
            if (a.lowSince == 0) a.lowSince = now;
            if (a.floorFix == 0.f && now - a.lowSince > 1.5) {
                a.floorFix = a.standH - hy;
                a.trackToWorld.p.y += a.floorFix;
                LOGW("floor lost: the head reads %.2f m above the floor; view moved %+.2f m (usual height %.2f m)", hy, a.floorFix, a.standH);
            }
        } else {
            a.lowSince = 0;
        }
        if (a.floorFix != 0.f && hy > 1.0f && hy < 2.2f) {   // the headset found its floor again
            if (a.okSince == 0) a.okSince = now;
            if (now - a.okSince > 1.0) {
                a.trackToWorld.p.y -= a.floorFix;
                LOGI("floor back: the head reads %.2f m; the lift of %.2f m removed", hy, a.floorFix);
                a.floorFix = 0.f;
                a.okSince = 0;
            }
        } else {
            a.okSince = 0;
        }
    }
    if (a.fadePending && headOk) {   // the previous frame was captured: move now, fade the old view out
        a.fadePending = false;
        placeHead(a, headTrack, a.fadeTarget);
        a.fadeT = 0.5f;
    }
    if (headOk && (!a.recentered || (rc && !a.prevRecenter))) {
        LOGI("recenter cause: %s", !a.recentered ? "initial/reference-space change" : "button (B / thumbstick click)");
        recenter(a, headTrack, local);
    }
    bool y = getBool(a, a.buttonY);
    a.prevY = y;
    a.peekHeld = y && !a.spectating;   // PASSTHROUGH > PEEK: the room while Y is held
    // adb trigger for testing without controllers: touch <externalDataPath>/lan_toggle
    if (++a.lanTriggerPoll >= 36) {
        a.lanTriggerPoll = 0;
        const char* ext = dataDir(a);
        if (ext) {
            std::string f = std::string(ext) + "/lan_toggle";
            if (fileExists(f)) {
                fileRemove(f);
                LOGI("net: lan_toggle trigger file");
                a.net.toggleLan();
            }
            std::string sf = std::string(ext) + "/spectate_toggle";   // test hook: enter / leave spectator mode
            if (fileExists(sf)) {
                fileRemove(sf);
                if (a.net.mode() == Net::Mode::Spectate) a.net.leave(); else { a.net.spectate(); a.watchIp = 0; }
            }
            std::string wf = std::string(ext) + "/watch_first";        // test hook: watch the first live match
            if (fileExists(wf)) {
                fileRemove(wf);
                if (!a.net.matches().empty()) a.watchIp = a.net.matches()[0].ip;
            }
        }
    }
    a.prevRecenter = rc;
    in.headTracked = headOk;
    in.head = a.trackToWorld * headTrack;
    if (a.net.localWaiting() && std::fabs(in.head.p.x) < 9.9f && in.head.p.z > -2.9f) {
        a.trackToWorld.p.z -= in.head.p.z + 2.9f;   // the glass stops you: you stay in the holding area
        in.head = a.trackToWorld * headTrack;
    }
    for (int h = 0; h < 2; ++h) {
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        XrSpaceVelocity vel{XR_TYPE_SPACE_VELOCITY};
        loc.next = &vel;
        xrLocateSpace(a.handSpace[h], a.appSpace, t, &loc);
        in.hand[h].tracked = (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
        in.hand[h].grip = a.trackToWorld * fromXr(loc.pose);
        // The original attaches the gauntlet to the legacy Oculus Touch controller pose, whose forward axis is the
        // controller's pointing (aim) direction, not the OpenXR grip (handle) axis. Use the aim orientation at the
        // grip position as the tool frame.
        XrSpaceLocation al{XR_TYPE_SPACE_LOCATION};
        xrLocateSpace(a.aimSpace[h], a.appSpace, t, &al);
        a.aimValid[h] = (al.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) && (al.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
        if (al.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) {
            Pose aim = a.trackToWorld * fromXr(al.pose);
            a.aim[h] = aim;
            if (!a.loggedFrames && h == 1 && in.hand[h].tracked) {
                Quat rel = in.hand[h].grip.q.conj() * aim.q;
                Vec3 off = in.hand[h].grip.q.conj().rotate(aim.p - in.hand[h].grip.p);
                LOGI("aim vs grip (right): rot axis-angle %.1f deg (q %.3f %.3f %.3f %.3f), aim origin in grip (%.3f %.3f %.3f)",
                     rad2deg(2 * std::acos(std::min(1.f, std::fabs(rel.w)))), rel.x, rel.y, rel.z, rel.w, off.x, off.y, off.z);
                a.loggedFrames = true;
            }
            if (a.toolFrame == 1) in.hand[h].grip.q = aim.q;
            a.rawGrip[h] = in.hand[h].grip;
            Quat tq = in.hand[h].grip.q * gripEuler({a.tunePitchDeg, a.tuneYawDeg, a.tuneRollDeg}, h == 0);
            in.hand[h].grip = {tq, in.hand[h].grip.p + tq.rotate({(h == 0 ? -1.f : 1.f) * a.tuneX, a.tuneUp, -a.tuneFwd})};
        }
        if (vel.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT)
            in.hand[h].angVel = a.trackToWorld.q.rotate({vel.angularVelocity.x, vel.angularVelocity.y, vel.angularVelocity.z});
        in.hand[h].trigger = getFloat(a, a.trigger, a.handPath[h]);
        in.hand[h].squeeze = getFloat(a, a.squeeze, a.handPath[h]);
    }
    in.buttonA = getBool(a, a.buttonA);
    in.avatar = avatar::uniform(a.myAvatar);   // (with a country: the team kit, which older builds can show too)
    const float pttGrip = getFloat(a, a.pushToTalk, XR_NULL_PATH);   // (bound per controller: see pushToTalk)
    a.ptt = pttGrip > 0.6f;
    a.pttGripMax = std::max(a.pttGripMax, pttGrip);
    if (a.ptt) ++a.pttHeldFrames;
    if (a.spectating && a.watchIp && !a.menuOpen) spectatorCamera(a, headTrack, in);
    if (a.startTitle && headOk && a.recentered) {
        a.startTitle = false;
        openMenu(a, in);
        a.titleOpen = true;
        if (a.firstHelp) { a.firstHelp = false; a.helpOpen = true; }
    }
    bool menu = getBool(a, a.menu);
    if (menu && !a.prevMenu) {
        if (a.builderOpen) closeBuilder(a);
        else if (a.tuneOpen) closeTuning(a);
        else if (a.menuOpen) { a.menuOpen = false; a.nameEdit = false; a.helpOpen = false; a.settingsOpen = false; a.titleOpen = false; a.quitArmed = false; LOGI("menu: closed"); }
        else openMenu(a, in);
    }
    a.prevMenu = menu;
    updateTuning(a, in);
    if (a.lanTriggerPoll == 0 && dataDir(a)) {   // adb test hook, like lan_toggle
        std::string f = std::string(dataDir(a)) + "/menu_toggle";
        if (fileExists(f)) {
            fileRemove(f);
            if (a.menuOpen) a.menuOpen = false; else openMenu(a, in);
        }
    }
    for (int h = 0; h < 2; ++h) a.rawTrigger[h] = in.hand[h].trigger;   // before the menu clears it (posters, kiosk)
    updateMenu(a, in);
    updateBuilder(a, in);
    updateDirector(a, in);
    return in;
}

// ------------------------------------------------------------------ lobby menu
enum class MenuAct { None, MainMenu, Nav, Quit, Bug, BugCat, BugNote, BugSend, BugCancel, RulesPick, RulesBack, Report, ReportWho, ReportWhy, ReportNote, ReportMute, ReportKick, StatsNoticeOk,
                     ReportSend, ReportBack, Forfeit, Experimental, XpBack, XpServer, XpConnect, Host, Leave, Rules, Close, Join, Spectate, Watch, Hub, Avatar, Name, Key, KeyDel, KeyDone, Help, HelpDone, Play, Multi, Settings, Back, VolDown, VolUp, Hand, Fx, Online, Server, ServerDefault, DeleteStats, ClearWarps, Queue, HideOutside, VoicePage, VoiceMode, MuteSpec, MutePlayers, MuteAll, Director, JoinRoom, BotFill, Tuning, Passthrough, PtMode, PtAdjust, PtMono, PtEdges, PtPreset, PtReveal, PtPeek, Crowd, Turn, OptShields, OptKnuckle, OptTime, OptGo, OptBack, ReportHostKick, OptBot, Record, OptMeet, GfxPage, FovDown, FovUp, OptRoom };
struct MenuItem {
    std::string label;
    float x, y, w, h;
    MenuAct act;
    uint32_t ip;
    bool enabled;
};

static void haptic(App& a, int hand, float amp, float seconds);

// ---- MATCH OPTIONS: the page's choices <-> the rules, their labels, and whether a match is on
static void optionsFrom(App& a, const Tunables& t) {
    a.optAdvanced = t.advanced; a.optShields = t.shields; a.optKnuckle = t.knuckleMode; a.optExperimental = t.experimental; a.optExpRoom = t.expRoom;
    a.optMatchTime = t.basicMatchTime; a.optRoundTime = t.advancedRoundTime;
    a.optBotLevel = t.botLevel; a.optBotAdapt = t.botAdapt; a.optBotMeet = t.botMeet;
}
// CORNERS is hidden until it becomes the 4-player room (owner, 2026-10-29): DIAMOND is the only EXPERIMENTAL room
static constexpr bool kShowCorners = false;

static void optionsInto(const App& a, Tunables& t) {
    t.advanced = a.optAdvanced; t.shields = a.optShields; t.knuckleMode = a.optKnuckle; t.experimental = a.optExperimental && !a.optAdvanced; t.expRoom = kShowCorners ? a.optExpRoom : 0;
    t.basicMatchTime = a.optMatchTime; t.advancedRoundTime = a.optRoundTime;
    t.botLevel = a.optBotLevel; t.botAdapt = a.optBotAdapt; t.botMeet = a.optBotMeet;
}
static std::string timeLabel(float secs, bool rounds) {
    if (secs <= 0.f) return rounds ? "OFF (NO LIMIT)" : "FREE PLAY (NO LIMIT)";
    char b[16];
    std::snprintf(b, sizeof b, "%d:%02d", (int)secs / 60, (int)secs % 60);
    return b;
}
// "ADVANCED", "BASIC - HOTDOG - 5 MIN", "ADVANCED - 2 MIN ROUNDS - KNUCKLE GUARDS": the rules and what differs
static std::string rulesLine(const Tunables& t) {
    std::string s = t.advanced ? "ADVANCED" : t.experimental ? (t.expRoom == 1 && kShowCorners ? "EXPERIMENTAL CORNERS" : "EXPERIMENTAL DIAMOND") : "BASIC";
    if (!t.shields) s += " - HOTDOG";
    if (t.advanced && t.advancedRoundTime > 0) s += " - " + std::to_string((int)t.advancedRoundTime / 60) + " MIN ROUNDS";
    if (!t.advanced && t.basicMatchTime <= 0) s += " - FREE PLAY";
    else if (!t.advanced && t.basicMatchTime != 180.f) s += " - " + std::to_string((int)t.basicMatchTime / 60) + " MIN";
    if (t.knuckleMode >= 0 && (t.knuckleMode != 0) != !t.advanced) s += t.knuckleMode ? " - KNUCKLE GUARDS" : " - NO KNUCKLE GUARDS";
    return s;
}
// the clock as the boards show it: 2:41, FREE PLAY, SUDDEN DEATH (empty: an untimed Advanced round)
static std::string clockLine(const Game& g) {
    if (g.overtime) return "SUDDEN DEATH";
    if (g.tun.freePlay()) return "FREE PLAY";
    if (!g.tun.timed()) return "";
    const int secs = (int)std::ceil(std::max(0.f, g.roundClock));
    char b[16];
    std::snprintf(b, sizeof b, "%d:%02d", secs / 60, secs % 60);
    return b;
}
static bool rulesLive(const App& a) {   // a match against a person is on in our arena (its rules stay until it ends)
    if (a.net.mode() != Net::Mode::Host || a.game.pl[1].isBot) return false;
    const MatchState m = a.game.match;
    return m == MatchState::Meet || m == MatchState::Countdown || m == MatchState::InProgress || m == MatchState::RoundSummary;
}
static void saveSettings(App& a);
static void applyRules(App& a) {
    if (rulesLive(a)) {   // the match in progress keeps its rules
        a.rulesPending = true;
        Tunables t;
        optionsInto(a, t);
        a.net.noticeLocal(("NEXT MATCH: " + rulesLine(t)).substr(0, 47).c_str());
        return;
    }
    optionsInto(a, a.game.tun);
    if (a.guestRules) optionsInto(a, a.ownRules);   // (a guest choosing practice rules: ours, kept for after the room)
    a.game.reset(a.game.pl[1].isBot);
    saveSettings(a);
}

// side menu (left of the panel): the areas of the game, the current one lit
enum { NAV_LOBBY, NAV_HOME, NAV_PRACTICE, NAV_MULTI, NAV_SPECTATE, NAV_AVATAR, NAV_STATS, NAV_SETTINGS, NAV_REPORT, NAV_BUG,
       NAV_HELP, NAV_QUIT, kNavCount };
static const char* kNavLabels[kNavCount] = {"LOBBY", "MAIN MENU", "PRACTICE", "MULTIPLAYER", "SPECTATE", "AVATAR", "MY STATS",
                                            "SETTINGS", "REPORT PLAYER", "REPORT BUG", "HOW TO PLAY", "QUIT GAME"};
static const float kNavX = -0.67f, kNavTop = 0.30f, kNavStep = 0.0745f;

static int navCurrent(const App& a) {
    if (a.titleOpen || a.xpOpen) return NAV_HOME;
    if (inLobby(a) && !a.menuOpen) return NAV_LOBBY;
    if (a.rulesOpen) return a.rulesForHost ? NAV_MULTI : NAV_PRACTICE;
    if (a.settingsOpen) return NAV_SETTINGS;
    if (a.statsOpen) return NAV_STATS;
    if (a.reportOpen) return NAV_REPORT;
    if (a.bugOpen) return NAV_BUG;
    if (a.helpOpen) return NAV_HELP;
    return a.net.mode() == Net::Mode::Spectate ? NAV_SPECTATE : NAV_MULTI;
}

static std::vector<MenuItem> menuItemsBase(App& a);
static std::vector<MenuItem> menuItems(App& a) {
    std::vector<MenuItem> v = menuItemsBase(a);
    if (!a.nameEdit) {   // the side menu: every area one click away
        const Net::Mode m = a.net.mode();
        const bool offline = m == Net::Mode::Offline || m == Net::Mode::Searching;
        const bool people = a.net.inRoom() || m == Net::Mode::Spectate;
        for (int i = 0; i < kNavCount; ++i) {
            bool on = true;
            if (i == NAV_SPECTATE) on = !a.net.inRoom();
            if (i == NAV_AVATAR) on = offline && !a.avModels.empty();
            if (i == NAV_REPORT) on = people;
            if (i == NAV_QUIT) { v.push_back({a.quitArmed ? "SURE? QUIT" : "QUIT GAME", kNavX, kNavTop - kNavStep * i, 0.24f, 0.062f, MenuAct::Quit, 0, true}); continue; }
            if (i == NAV_LOBBY) on = !inLobby(a);
            v.push_back({kNavLabels[i], kNavX, kNavTop - kNavStep * i, 0.24f, 0.062f, MenuAct::Nav, (uint32_t)i, on});
        }
    }
    return v;
}

static std::vector<MenuItem> menuItemsBase(App& a) {
    std::vector<MenuItem> v;
    if (a.statsNoticeOpen && !a.nameEdit) {   // the one-time notice before the first online match
        v.push_back({"OK", 0, -0.33f, 0.30f, 0.07f, MenuAct::StatsNoticeOk, 0, true});
        return v;
    }
    if (a.statsOpen && !a.nameEdit) {   // MY STATS: the dashboard is drawn by drawStats; here its one button
        const bool armed = nowSeconds() < a.deleteArmedUntil;
        v.push_back({armed ? "PRESS AGAIN TO DELETE" : "DELETE MY STATS", 0.24f, -0.495f, 0.40f, 0.055f, MenuAct::DeleteStats, 0,
                     a.net.onlineReady() && a.myCareerOk && a.myCareer.known});
        v.push_back({a.recordOn ? "RECORD MY MATCHES: ON" : "RECORD MY MATCHES: OFF", 0.24f, -0.41f, 0.40f, 0.05f, MenuAct::Record, 0, true});
        return v;
    }
    if (a.bugOpen && !a.nameEdit) {
        for (int i = 0; i < 6; ++i)
            v.push_back({kBugCats[i], (i % 2) ? 0.21f : -0.21f, 0.13f - 0.085f * (i / 2), 0.40f, 0.07f, MenuAct::BugCat, (uint32_t)i, true});
        std::string note = a.bugNote.empty() ? std::string("ADD A NOTE (OPTIONAL)") : "NOTE: " + a.bugNote;
        if (note.size() > 40) note = note.substr(0, 37) + "...";
        v.push_back({note, 0, -0.145f, 0.82f, 0.07f, MenuAct::BugNote, 0, true});
        v.push_back({a.bugSending ? "SENDING..." : "SEND REPORT", 0.19f, -0.25f, 0.36f, 0.08f, MenuAct::BugSend, 0, a.bugCat >= 0 && !a.bugSending});
        v.push_back({"CANCEL", -0.19f, -0.25f, 0.36f, 0.08f, MenuAct::BugCancel, 0, true});
        return v;
    }
    if (a.rulesOpen && a.rulesStep == 1 && !a.nameEdit) {   // MATCH OPTIONS
        Tunables t;
        optionsInto(a, t);
        const bool bot = !a.rulesForHost;   // practice: the bot's level too (a row more: the rows closer together)
        const float y[4] = {bot ? 0.15f : 0.13f, bot ? 0.065f : 0.04f, bot ? -0.02f : -0.05f, -0.105f};
        if (t.experimental && kShowCorners) {   // EXPERIMENTAL: which room, beside SHIELDS
            v.push_back({t.shields ? "SHIELDS: ON" : "SHIELDS: OFF (HOTDOG)", -0.19f, y[0], 0.36f, 0.075f, MenuAct::OptShields, 0, true});
            v.push_back({t.expRoom == 1 ? "ROOM: CORNERS" : "ROOM: DIAMOND", 0.19f, y[0], 0.36f, 0.075f, MenuAct::OptRoom, 0, true});
        } else {
            v.push_back({t.shields ? "SHIELDS: ON" : "SHIELDS: OFF  (HOTDOG MODE)", 0, y[0], 0.74f, 0.075f, MenuAct::OptShields, 0, true});
        }
        v.push_back({t.knuckleGuards() ? "KNUCKLE GUARDS: ON" : "KNUCKLE GUARDS: OFF", 0, y[1], 0.74f, 0.075f, MenuAct::OptKnuckle, 0, true});
        const std::string tl = std::string(t.advanced ? "ROUND TIME: " : "MATCH TIME: ") + timeLabel(t.advanced ? t.advancedRoundTime : t.basicMatchTime, t.advanced);
        v.push_back({"-", -0.335f, y[2], 0.07f, 0.075f, MenuAct::OptTime, 0, true});
        v.push_back({tl, 0, y[2], 0.56f, 0.075f, MenuAct::None, 0, true});
        v.push_back({"+", 0.335f, y[2], 0.07f, 0.075f, MenuAct::OptTime, 1, true});
        if (bot) {   // - / +, the slider (one step a level, lit up to the one chosen), and MATCH MY LEVEL
            const std::string bl = t.botAdapt ? std::string("BOT: MATCH MY LEVEL (NOW ") + botSkillAt(a.game.tun.botMatch).name + ")"
                                              : std::string("BOT: ") + botSkill(t.botLevel).name;
            v.push_back({"-", -0.335f, y[3], 0.07f, 0.075f, MenuAct::OptBot, 0, t.botAdapt || t.botLevel > 0});
            v.push_back({bl, 0, y[3], 0.56f, 0.075f, MenuAct::None, 0, true});
            v.push_back({"+", 0.335f, y[3], 0.07f, 0.075f, MenuAct::OptBot, 1, t.botAdapt || t.botLevel < kBotLevels - 1});
            for (int l = 0; l < kBotLevels; ++l)
                v.push_back({"", -0.33f + 0.07f * l, -0.165f, 0.062f, 0.04f, MenuAct::OptBot, (uint32_t)(10 + l), true});
            v.push_back({"MATCH MY LEVEL", 0.225f, -0.165f, 0.29f, 0.04f, MenuAct::OptBot, 2, true});
            v.push_back({t.botMeet ? "FIST BUMP FIRST: ON" : "FIST BUMP FIRST: OFF", 0, -0.395f, 0.46f, 0.05f, MenuAct::OptMeet, 0, true});
        }
        const char* go = a.rulesEdit ? (rulesLive(a) ? "APPLY FROM THE NEXT MATCH" : "APPLY") : a.rulesForHost ? "OPEN MY ARENA" : "START PRACTICE";
        v.push_back({go, 0.19f, bot ? -0.32f : -0.24f, 0.36f, 0.08f, MenuAct::OptGo, 0, true});
        v.push_back({"BACK", -0.19f, bot ? -0.32f : -0.24f, 0.36f, 0.08f, MenuAct::OptBack, 0, true});
        return v;
    }
    if (a.rulesOpen && !a.nameEdit) {
        v.push_back({"ADVANCED", 0, 0.12f, 0.70f, 0.10f, MenuAct::RulesPick, 1, true});
        v.push_back({"BASIC", 0, -0.04f, 0.70f, 0.10f, MenuAct::RulesPick, 0, true});
        const bool hosting = a.rulesForHost || a.net.mode() == Net::Mode::Host || a.net.mode() == Net::Mode::Client;   // (online: next)
        v.push_back({hosting ? "EXPERIMENTAL - PRACTICE ONLY FOR NOW" : "EXPERIMENTAL", 0, -0.20f, 0.70f, 0.08f, MenuAct::RulesPick, 2, !hosting});
        v.push_back({"BACK", 0, -0.35f, 0.36f, 0.06f, MenuAct::RulesBack, 0, true});
        return v;
    }
    if (a.reportOpen && !a.nameEdit) {
        const std::vector<Net::Person> people = reportPeople(a);
        for (size_t i = 0; i < people.size() && i < 8; ++i) {
            const Net::Person& p = people[i];
            std::string l = std::string(p.name) + (isMuted(a, p.id) ? " - MUTED" : std::string(" (") + roleText(p.role) + ")");
            v.push_back({l, (i % 2) ? 0.21f : -0.21f, 0.15f - 0.07f * (float)(i / 2), 0.40f, 0.06f, MenuAct::ReportWho, p.id, true});
        }
        bool here = false;
        for (const Net::Person& p : people) here |= p.id == a.reportWho;
        for (int i = 0; i < 4; ++i)
            v.push_back({kReportWhy[i], (i % 2) ? 0.21f : -0.21f, -0.15f - 0.07f * (i / 2), 0.40f, 0.06f, MenuAct::ReportWhy, (uint32_t)i, here});
        std::string note = a.reportNote.empty() ? std::string("ADD A NOTE (OPTIONAL)") : "NOTE: " + a.reportNote;
        if (note.size() > 40) note = note.substr(0, 39) + ".";
        v.push_back({note, 0, -0.30f, 0.82f, 0.06f, MenuAct::ReportNote, 0, here});
        v.push_back({here && isMuted(a, a.reportWho) ? "UNMUTE" : "MUTE", -0.375f, -0.39f, 0.17f, 0.07f, MenuAct::ReportMute, 0, here});
        // VOTE TO REMOVE: people in this arena room only (not spectators), never the host
        const bool voted = std::find(a.kickVoted.begin(), a.kickVoted.end(), a.reportWho) != a.kickVoted.end();
        bool inRoomHere = false;
        if (a.net.inRoom()) for (const Net::Person& p : people) inRoomHere |= p.id == a.reportWho && p.role != Net::VOICE_SPECTATOR;
        if (a.net.mode() == Net::Mode::Host)   // (owner, 2026-10-21: the host has kick rights regardless of votes)
            v.push_back({"REMOVE FROM ARENA", -0.135f, -0.39f, 0.29f, 0.07f, MenuAct::ReportHostKick, 0,
                         here && inRoomHere && a.reportWho != a.net.session()});
        else
            v.push_back({voted ? "VOTED" : "VOTE TO REMOVE", -0.135f, -0.39f, 0.29f, 0.07f, MenuAct::ReportKick, 0,
                         here && inRoomHere && !voted && a.reportWho != a.net.hostSession()});
        v.push_back({a.bugSending ? "SENDING..." : "SEND REPORT", 0.135f, -0.39f, 0.23f, 0.07f, MenuAct::ReportSend, 0,
                     here && a.reportWhy >= 0 && !a.bugSending});
        v.push_back({"BACK", 0.365f, -0.39f, 0.17f, 0.07f, MenuAct::ReportBack, 0, true});
        return v;
    }
    // the keyboard comes before the pages that open it (the title page's CHANGE NAME, SETTINGS' SERVER): they
    // stay open underneath, and their buttons used to be offered instead (astrobleem's report, 2026-10-27)
    if (a.nameEdit) {   // name keyboard: four rows of keys, then SPACE / DELETE / DONE
        static const char* rows[4] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM.:"};
        for (int r = 0; r < 4; ++r)
            for (int c = 0; rows[r][c]; ++c) {
                char k[2] = {rows[r][c], 0};
                float x = -0.3825f + 0.085f * c + (r == 3 ? 0.085f * 0.5f : 0.f);
                v.push_back({k, x, 0.10f - 0.09f * r, 0.075f, 0.075f, MenuAct::Key, (uint32_t)(uint8_t)rows[r][c], true});
            }
        v.push_back({"SPACE", -0.25f, -0.27f, 0.30f, 0.07f, MenuAct::Key, (uint32_t)' ', true});
        v.push_back({"DELETE", 0.06f, -0.27f, 0.24f, 0.07f, MenuAct::KeyDel, 0, !a.nameBuf.empty()});
        v.push_back({"DONE", 0.31f, -0.27f, 0.20f, 0.07f, MenuAct::KeyDone, 0, true});
        return v;
    }
    if (a.helpOpen) {
        v.push_back({"GOT IT", 0, -0.33f, 0.36f, 0.07f, MenuAct::HelpDone, 0, true});
        return v;
    }
    if (a.settingsOpen && a.ptPage) {
        for (int m = 0; m < PT_COUNT; ++m)
            v.push_back({kPtModeName[m], -0.30f + 0.20f * m, 0.15f, 0.18f, 0.065f, MenuAct::PtMode, (uint32_t)m, true});
        v.push_back({"INDOOR PRESET", -0.19f, 0.075f, 0.36f, 0.06f, MenuAct::PtPreset, 0, true});
        v.push_back({"OUTDOOR PRESET", 0.19f, 0.075f, 0.36f, 0.06f, MenuAct::PtPreset, 1, true});
        const bool on = a.ptMode != PT_OFF;
        char b[48];
        auto row = [&](const char* label, float val, float y, int id, bool en) {
            snprintf(b, sizeof b, "%s %d%%", label, (int)std::lround(val * 100));
            v.push_back({b, 0, y, 0.40f, 0.06f, MenuAct::None, 0, en});
            v.push_back({"-", -0.27f, y, 0.08f, 0.06f, MenuAct::PtAdjust, (uint32_t)(id * 2), en});
            v.push_back({"+", 0.27f, y, 0.08f, 0.06f, MenuAct::PtAdjust, (uint32_t)(id * 2 + 1), en});
        };
        row("COURT FLOOR", a.ptFloor, -0.005f, 0, on);
        row("GLASS WALLS", a.ptGlass, -0.075f, 1, on && a.ptMode != PT_OUTLINE);
        row("ROOM BRIGHTNESS", a.ptRoom, -0.145f, 2, true);
        v.push_back({a.ptMono ? "ROOM: GREY" : "ROOM: COLOUR", -0.19f, -0.22f, 0.36f, 0.06f, MenuAct::PtMono, 0, true});
        v.push_back({a.ptEdges ? "OBJECT EDGES: ON" : "OBJECT EDGES: OFF", 0.19f, -0.22f, 0.36f, 0.06f, MenuAct::PtEdges, 0, true});
        v.push_back({a.ptEdgeReveal ? "ROOM AT EDGES: ON" : "ROOM AT EDGES: OFF", -0.19f, -0.29f, 0.36f, 0.06f, MenuAct::PtReveal, 0, true});
        v.push_back({a.ptPeekOn ? "PEEK (HOLD Y): ON" : "PEEK (HOLD Y): OFF", 0.19f, -0.29f, 0.36f, 0.06f, MenuAct::PtPeek, 0, true});
        v.push_back({"BACK", 0, -0.375f, 0.36f, 0.06f, MenuAct::Back, 0, true});
        return v;
    }
    if (a.settingsOpen && a.gfxPage) {   // GRAPHICS: field of view (room size feel) and hit effects; the arena around it
        char b[48];
        snprintf(b, sizeof b, a.roomFeel == 0 ? "NORMAL" : "%+d%%", a.roomFeel * 5);
        v.push_back({b, 0, -0.04f, 0.20f, 0.075f, MenuAct::None, 0, true});
        v.push_back({"-", -0.17f, -0.04f, 0.09f, 0.075f, MenuAct::FovDown, 0, a.roomFeel > -4});
        v.push_back({"+", 0.17f, -0.04f, 0.09f, 0.075f, MenuAct::FovUp, 0, a.roomFeel < 4});
        static const char* fx[3] = {"HIT EFFECTS: FULL", "HIT EFFECTS: SOFT", "HIT EFFECTS: LOW"};
        v.push_back({fx[a.fxLevel % 3], 0, -0.22f, 0.40f, 0.07f, MenuAct::Fx, 0, true});
        v.push_back({"BACK", 0, -0.33f, 0.36f, 0.06f, MenuAct::Back, 0, true});
        return v;
    }
    if (a.settingsOpen && a.voicePage) {
        static const char* modes[3] = {"VOICE: PUSH TO TALK (LEFT GRIP)", "VOICE: OPEN MIC", "VOICE: OFF"};
        v.push_back({modes[a.voiceMode % 3], 0, 0.12f, 0.80f, 0.08f, MenuAct::VoiceMode, 0, true});
        v.push_back({a.muteSpectators ? "SPECTATORS: MUTED" : "SPECTATORS: ON", 0, 0.02f, 0.80f, 0.08f, MenuAct::MuteSpec, 0, true});
        v.push_back({a.mutePlayers ? "OTHER PLAYERS: MUTED" : "OTHER PLAYERS: ON", 0, -0.08f, 0.80f, 0.08f, MenuAct::MutePlayers, 0, true});
        v.push_back({a.muteAll ? "MUTE ALL: ON" : "MUTE ALL: OFF", 0, -0.18f, 0.80f, 0.08f, MenuAct::MuteAll, 0, true});
        v.push_back({"BACK", 0, -0.31f, 0.36f, 0.07f, MenuAct::Back, 0, true});
        return v;
    }
    if (a.settingsOpen) {
        char b[48];
        snprintf(b, sizeof b, "VOLUME %d%%", a.volume * 10);
        v.push_back({b, -0.19f, 0.13f, 0.20f, 0.075f, MenuAct::None, 0, true});
        v.push_back({"-", -0.335f, 0.13f, 0.07f, 0.075f, MenuAct::VolDown, 0, a.volume > 0});
        v.push_back({"+", -0.045f, 0.13f, 0.07f, 0.075f, MenuAct::VolUp, 0, a.volume < 10});
        v.push_back({!a.hasPassthrough || a.ptFailed ? "PASSTHROUGH: N/A" : std::string("PASSTHROUGH: ") + kPtModeName[a.ptMode % PT_COUNT],
                     0.19f, 0.13f, 0.36f, 0.075f, MenuAct::Passthrough, 0, a.hasPassthrough && !a.ptFailed});
        v.push_back({a.pointerHand == 1 ? "POINTER: RIGHT" : "POINTER: LEFT", -0.19f, 0.04f, 0.36f, 0.075f, MenuAct::Hand, 0, true});
        v.push_back({a.botFill ? "BOT FILLS IN: ON" : "BOT FILLS IN: OFF", 0.19f, 0.04f, 0.36f, 0.075f, MenuAct::BotFill, 0, true});
        v.push_back({"GRAPHICS AND VIEW", -0.19f, -0.05f, 0.36f, 0.075f, MenuAct::GfxPage, 0, true});
        v.push_back({a.hideOutside ? "OUTSIDE: HIDDEN" : "OUTSIDE: SHOWN", 0.19f, -0.05f, 0.36f, 0.075f, MenuAct::HideOutside, 0, true});
        v.push_back({"RULES: " + rulesLine(a.game.tun), -0.19f, -0.14f, 0.36f, 0.075f, MenuAct::Rules, 0,
                     a.net.mode() != Net::Mode::Client && a.net.mode() != Net::Mode::Spectate});
        v.push_back({"VOICE AND MUTES", 0.19f, -0.14f, 0.36f, 0.075f, MenuAct::VoicePage, 0, true});
        // The Crowd (fans on phones): their sound, flags in the stands and the ends' flaring lights
        static const char* crowd[3] = {"CROWD: FULL", "CROWD: FLAGS ONLY", "CROWD: OFF"};
        v.push_back({crowd[a.crowdMode % 3], -0.19f, -0.23f, 0.36f, 0.075f, MenuAct::Crowd, 0, true});
        static const char* turn[3] = {"TURNING: SNAP", "TURNING: SMOOTH", "TURNING: SMOOTH FAST"};
        v.push_back({turn[a.turnMode % 3], 0.19f, -0.23f, 0.36f, 0.075f, MenuAct::Turn, 0, true});
        std::string sv = "SERVER: " + (a.serverAddr.empty() ? std::string("NOT SET") : a.serverAddr);
        if (sv.size() > 34) sv = sv.substr(0, 33) + ".";
        const bool custom = PA_DEFAULT_SERVER[0] && a.serverAddr != PA_DEFAULT_SERVER;
        v.push_back({sv, custom ? -0.12f : 0.f, -0.31f, custom ? 0.50f : 0.74f, 0.065f, MenuAct::Server, 0, true});
        if (custom) v.push_back({"USE DEFAULT", 0.26f, -0.31f, 0.22f, 0.065f, MenuAct::ServerDefault, 0, true});
        v.push_back({"BACK", a.devMode ? -0.19f : 0.f, -0.385f, 0.36f, 0.055f, MenuAct::Back, 0, true});
        if (a.devMode) v.push_back({"TUNING (DEVELOPER)", 0.19f, -0.385f, 0.36f, 0.055f, MenuAct::Tuning, 0, true});
        return v;
    }
    if (a.xpOpen) {
        std::string sv = "SERVER: " + a.sparcAddr;
        if (sv.size() > 34) sv = sv.substr(0, 33) + ".";
        const bool live = a.sparc.state() != xc::SparcLink::State::Off && a.sparc.state() != xc::SparcLink::State::Failed;
        v.push_back({sv, 0, 0.075f, 0.74f, 0.07f, MenuAct::XpServer, 0, !live});
        v.push_back({live ? "DISCONNECT" : "CONNECT (SPECTATE ONLY)", 0, -0.025f, 0.56f, 0.08f, MenuAct::XpConnect, 0, true});
        v.push_back({"BACK", 0, -0.37f, 0.36f, 0.06f, MenuAct::XpBack, 0, true});
        return v;
    }
    if (a.titleOpen) {
        v.push_back({"PLAY PRACTICE", 0, 0.08f, 0.56f, 0.085f, MenuAct::Play, 0, true});
        v.push_back({"MULTIPLAYER", 0, -0.02f, 0.56f, 0.085f, MenuAct::Multi, 0, true});
        v.push_back({"CUSTOMIZE AVATAR", 0, -0.12f, 0.56f, 0.085f, MenuAct::Avatar, 0, !a.avModels.empty()});
        v.push_back({"SETTINGS", -0.145f, -0.22f, 0.27f, 0.07f, MenuAct::Settings, 0, true});
        v.push_back({"HOW TO PLAY", 0.145f, -0.22f, 0.27f, 0.07f, MenuAct::Help, 0, true});
        v.push_back({"CHANGE NAME", 0, -0.31f, 0.56f, 0.07f, MenuAct::Name, 0, true});
        v.push_back({"EXPERIMENTAL", -0.145f, -0.385f, 0.27f, 0.045f, MenuAct::Experimental, 0, true});
        v.push_back({"REPORT A BUG", 0.145f, -0.385f, 0.27f, 0.045f, MenuAct::Bug, 0, true});
        return v;
    }
    const Net::Mode m = a.net.mode();
    const bool inGame = (m == Net::Mode::Host || m == Net::Mode::Client);
    const float lx = -0.25f, rx = 0.25f, bw = 0.42f;
    const bool spec = m == Net::Mode::Spectate;
    if (spec) {
        // spectators: their own column (no hosting, avatar or spectate entries)
        v.push_back({"STOP SPECTATING", lx, 0.12f, bw, 0.08f, MenuAct::Leave, 0, true});
        v.push_back({"BACK TO HUB", lx, 0.02f, bw, 0.08f, MenuAct::Hub, 0, a.watchIp != 0});
        v.push_back({"DIRECTOR TOOLS", lx, -0.08f, bw, 0.08f, MenuAct::Director, 0, a.watchIp != 0});
        v.push_back({"SETTINGS", lx, -0.17f, bw, 0.08f, MenuAct::Settings, 0, true});
        v.push_back({"CLEAR CAMERAS", lx, -0.26f, bw, 0.08f, MenuAct::ClearWarps, 0, !a.warps.empty()});
        v.push_back({"CLOSE MENU", lx, -0.35f, bw, 0.06f, MenuAct::Close, 0, true});
        v.push_back({"CHANGE NAME", rx, -0.35f, bw, 0.06f, MenuAct::Name, 0, true});
        bool full = false;
        for (const LanGame& lg : a.net.games()) if (lg.ip == a.watchIp) full = lg.full;
        v.push_back({full ? "ARENA FULL" : "JOIN THIS ARENA", rx, -0.17f, bw, 0.07f, MenuAct::Join, a.watchIp, a.watchIp != 0 && !full});
        v.push_back({a.net.online() ? "ONLINE: ON" : "ONLINE: OFF (LAN)", rx, -0.26f, bw, 0.06f, MenuAct::Online, 0, true});
    } else {
        if (a.net.localWaiting())
            v.push_back({a.net.localQueuePos() < 0 ? "JOIN QUEUE" : "LEAVE QUEUE", lx, 0.12f, bw, 0.08f, MenuAct::Queue, 0, true});
        else if (inGame)
            v.push_back({"FORFEIT MATCH", lx, 0.12f, bw, 0.08f, MenuAct::Forfeit, 0, true});
        else
            v.push_back({m == Net::Mode::Host ? "HOSTING" : "HOST A ROOM", lx, 0.12f, bw, 0.08f, MenuAct::Host, 0, m != Net::Mode::Host});
        v.push_back({inGame ? "LEAVE GAME" : "PRACTICE VS BOT", lx, 0.02f, bw, 0.08f, MenuAct::Leave, 0, inGame});
        v.push_back({"SETTINGS", lx, -0.08f, bw, 0.08f, MenuAct::Settings, 0, true});
        v.push_back({"SPECTATE GAMES", lx, -0.17f, bw, 0.08f, MenuAct::Spectate, 0, !inGame});
        v.push_back({"CUSTOMIZE AVATAR", lx, -0.26f, bw, 0.08f, MenuAct::Avatar, 0,
                     !a.avModels.empty() && (m == Net::Mode::Offline || m == Net::Mode::Searching)});
        v.push_back({"CLOSE MENU", lx, -0.35f, bw, 0.06f, MenuAct::Close, 0, true});
        v.push_back({"CHANGE NAME", rx, -0.35f, bw, 0.06f, MenuAct::Name, 0, true});
        v.push_back({a.net.online() ? "ONLINE: ON" : "ONLINE: OFF (LAN)", rx, -0.26f, bw, 0.06f, MenuAct::Online, 0, !inGame});
    }
    std::vector<LanGame> games = a.net.games();
    for (size_t i = 0; i < games.size() && i < (spec ? 3u : 4u); ++i) {   // spectators: JOIN THIS ARENA sits below
        const LanGame& lg = games[i];
        bool mine = (m == Net::Mode::Client && lg.ip == a.net.peerIp());
        char buf[64];
        if (spec) {
            snprintf(buf, sizeof buf, "%s  %s%s  %d-%d%s", lg.name, lg.advanced ? "ADV" : "BASIC", lg.shields ? "" : " HOTDOG",
                     lg.score0, lg.score1, lg.ip == a.watchIp ? "  (WATCHING)" : "");
            v.push_back({buf, rx, 0.08f - 0.09f * i, bw, 0.07f, MenuAct::Watch, lg.ip, lg.ip != a.watchIp});
        } else {
            Tunables lt;
            lt.advanced = lg.advanced; lt.shields = lg.shields; lt.knuckleMode = lg.knuckleMode;
            lt.basicMatchTime = lg.matchTime; lt.advancedRoundTime = lg.roundTime;
            snprintf(buf, sizeof buf, "JOIN  %s  %s%s", lg.name, rulesLine(lt).c_str(), mine ? "  (IN)" : lg.full ? "  FULL" : "");
            v.push_back({buf, rx, 0.08f - 0.09f * i, bw, 0.07f, MenuAct::JoinRoom, lg.ip, !lg.full && !mine});   // into the room's waiting area
        }
    }
    return v;
}

static void openMenu(App& a, const PlayerInput& in) {
    Vec3 f = in.head.q.rotate({0, 0, -1});
    f.y = 0;
    f = length(f) > 1e-3f ? normalize(f) : Vec3{1, 0, 0};
    float yaw = std::atan2(-f.x, -f.z);   // panel +Z faces the player
    a.menuPose = {Quat::axisAngle({0, 1, 0}, yaw), in.head.p + f * 0.9f + Vec3{0, -0.15f, 0}};
    if (inLobby(a))   // the lobby: always in the middle, between the posters, facing the balcony (owner)
        a.menuPose = {Quat{}, {kLobbySpot.x, in.head.p.y - 0.22f, kLobbySpot.z - 0.95f}};
    a.menuOpen = true;
    a.prevMenuTrig = true;   // ignore a trigger that is already held
    LOGI("menu: open");
}

static void menuActivate(App& a, const MenuItem& it) {
    LOGI("menu: %s", it.label.c_str());
    if (it.act != MenuAct::Quit) a.quitArmed = false;
    switch (it.act) {
    case MenuAct::Experimental: a.xpOpen = true; break;
    case MenuAct::Quit:
        if (!a.quitArmed) { a.quitArmed = true; break; }   // the second press quits
        LOGI("menu: quit game");
        saveSettings(a);
        if (a.net.inRoom()) a.net.leave();
        if (a.session != XR_NULL_HANDLE) xrRequestExitSession(a.session); else a.quit = true;
        break;
    case MenuAct::Nav: {
        a.titleOpen = a.settingsOpen = a.voicePage = a.ptPage = a.gfxPage = a.xpOpen = a.bugOpen = a.reportOpen = a.rulesOpen = a.helpOpen = false;
        a.statsOpen = false;
        a.nameEdit = false;
        auto go = [&](MenuAct act) { MenuItem x{"", 0, 0, 0, 0, act, 0, true}; menuActivate(a, x); };
        switch ((int)it.ip) {
        case NAV_LOBBY:   // leave whatever you are in; the frame loop puts the bots back on the court
            if (a.net.mode() != Net::Mode::Offline) a.net.leave();
            a.lobby = true;
            a.menuOpen = false;
            break;
        case NAV_HOME: a.titleOpen = true; break;
        case NAV_PRACTICE: go(MenuAct::Play); break;
        case NAV_MULTI: break;   // the multiplayer page (rooms; the spectator's page while spectating)
        case NAV_SPECTATE: if (a.net.mode() != Net::Mode::Spectate) go(MenuAct::Spectate); break;
        case NAV_AVATAR: go(MenuAct::Avatar); break;
        case NAV_SETTINGS: a.settingsOpen = true; break;
        case NAV_STATS: a.statsOpen = true; a.myStatsAskAt = 0; a.deleteArmedUntil = 0; break;
        case NAV_REPORT: go(MenuAct::Report); break;
        case NAV_BUG: go(MenuAct::Bug); break;
        case NAV_HELP: a.helpOpen = true; break;
        default: break;
        }
        break;
    }
    case MenuAct::MainMenu: a.titleOpen = true; a.settingsOpen = false; break;
    case MenuAct::Record:   // RECORD MY MATCHES (record.h): on this headset only
        a.recordOn = !a.recordOn;
        if (!a.recordOn && a.recorder.active()) a.recorder.end(a.game);
        a.net.noticeLocal(a.recordOn ? "RECORDING YOUR MATCHES - THEY STAY ON THIS HEADSET" : "NOT RECORDING YOUR MATCHES");
        LOGI("record my matches: %s", a.recordOn ? "on" : "off");
        saveSettings(a);
        break;
    case MenuAct::DeleteStats:   // two presses within 4 s
        if (nowSeconds() < a.deleteArmedUntil) {
            a.net.deleteMyStats();
            a.deleteArmedUntil = 0;
            a.myCareer = Net::CareerStats{};
            a.myCareerOk = false;
            a.myStatsAskAt = nowSeconds() + 1.0;
            a.net.noticeLocal("YOUR STATS ARE DELETED");
        } else {
            a.deleteArmedUntil = nowSeconds() + 4.0;
        }
        break;
    case MenuAct::Bug:
        a.bugReturnTitle = a.titleOpen; a.titleOpen = false; a.settingsOpen = false; a.xpOpen = false;
        a.bugOpen = true; a.bugCat = -1; a.bugNote.clear();
        { std::lock_guard<std::mutex> lk(a.bugMx); a.bugStatus.clear(); }
        break;
    case MenuAct::BugCat: a.bugCat = (int)it.ip; break;
    case MenuAct::BugNote: a.nameEdit = true; a.editTarget = 4; a.nameBuf = a.bugNote; break;
    case MenuAct::BugSend: bugSubmit(a); a.bugOpen = false; a.titleOpen = a.bugReturnTitle; break;
    case MenuAct::BugCancel: a.bugOpen = false; a.titleOpen = a.bugReturnTitle; break;
    case MenuAct::Report:
        a.settingsOpen = false; a.voicePage = false; a.ptPage = false; a.gfxPage = false; a.reportOpen = true;
        a.reportWho = 0; a.reportName[0] = 0; a.reportWhy = -1; a.reportNote.clear();
        break;
    case MenuAct::ReportWho:
        a.reportWho = it.ip;
        for (const Net::Person& p : reportPeople(a)) if (p.id == it.ip) std::memcpy(a.reportName, p.name, sizeof a.reportName);
        break;
    case MenuAct::ReportWhy: a.reportWhy = (int)it.ip; break;
    case MenuAct::ReportNote: a.nameEdit = true; a.editTarget = 5; a.nameBuf = a.reportNote; break;
    case MenuAct::ReportMute:
        if (isMuted(a, a.reportWho)) a.mutedIds.erase(std::remove(a.mutedIds.begin(), a.mutedIds.end(), a.reportWho), a.mutedIds.end());
        else if (a.reportWho) a.mutedIds.push_back(a.reportWho);
        LOGI("voice: %s %s", a.reportName, isMuted(a, a.reportWho) ? "muted" : "unmuted");
        break;
    case MenuAct::ReportKick:
        if (a.net.voteKick(a.reportWho)) {
            a.kickVoted.push_back(a.reportWho);
            char m[48];
            std::snprintf(m, sizeof m, "YOUR VOTE TO REMOVE %s IS IN", a.reportName);
            a.net.noticeLocal(m);
        }
        break;
    case MenuAct::ReportHostKick:
        if (a.net.hostKick(a.reportWho)) { a.reportOpen = false; a.menuOpen = false; }
        break;
    case MenuAct::StatsNoticeOk: a.statsNoticeOpen = false; a.statsNoticeSeen = true; a.menuOpen = false; saveSettings(a); break;
    case MenuAct::ReportSend: playerReportSubmit(a); a.reportOpen = false; break;
    case MenuAct::ReportBack: a.reportOpen = false; break;
    case MenuAct::Forfeit: a.net.forfeit(); a.menuOpen = false; break;
    case MenuAct::XpBack: a.xpOpen = false; break;
    case MenuAct::XpServer: a.nameEdit = true; a.editTarget = 3; a.nameBuf = a.sparcAddr; break;
    case MenuAct::XpConnect:
        if (a.sparc.state() != xc::SparcLink::State::Off && a.sparc.state() != xc::SparcLink::State::Failed) a.sparc.disconnect("LEFT");
        else a.sparc.connect(a.sparcAddr.c_str(), sparcName(a), sparcId(a));
        break;
    case MenuAct::Host: a.rulesOpen = true; a.rulesForHost = true; a.rulesEdit = false; a.rulesStep = 0; optionsFrom(a, a.game.tun); break;   // CHOOSE THE RULES, then the room opens
    case MenuAct::RulesPick:   // then MATCH OPTIONS (nothing changes until START / APPLY)
        a.optAdvanced = it.ip == 1;
        a.optExperimental = it.ip == 2;   // EXPERIMENTAL: the diagonal arena, Basic's rules (owner, 2026-10-29)
        a.rulesStep = 1;
        break;
    case MenuAct::OptShields: a.optShields = !a.optShields; break;
    case MenuAct::OptRoom: a.optExpRoom = a.optExpRoom == 1 ? 0 : 1; break;
    case MenuAct::OptKnuckle: {   // stored as "as the rules say" when it matches them
        Tunables t;
        optionsInto(a, t);
        const bool on = !t.knuckleGuards();
        a.optKnuckle = on == !a.optAdvanced ? -1 : (on ? 1 : 0);
        break;
    }
    case MenuAct::OptTime: {   // - / + through the choices (Basic: FREE PLAY 0, 1 to 15 min; Advanced rounds: OFF, 1 to 5 min)
        static const float basic[7] = {0, 60, 120, 180, 300, 600, 900}, rounds[5] = {0, 60, 120, 180, 300};
        const float* l = a.optAdvanced ? rounds : basic;
        const int n = a.optAdvanced ? 5 : 7;
        float& v = a.optAdvanced ? a.optRoundTime : a.optMatchTime;
        int k = 0;
        for (int i = 0; i < n; ++i) if (std::fabs(l[i] - v) < 1.f) k = i;
        k = std::max(0, std::min(n - 1, k + (it.ip ? 1 : -1)));
        v = l[k];
        break;
    }
    case MenuAct::OptBot:   // - / + or a step of the slider (a set level), or MATCH MY LEVEL
        if (it.ip == 2) { a.optBotAdapt = true; break; }
        if (a.optBotAdapt) a.optBotLevel = (int)std::lround(a.game.tun.botMatch);   // (- / + go on from where it got to)
        a.optBotLevel = it.ip >= 10 ? (int)it.ip - 10 : a.optBotLevel + (a.optBotAdapt ? 0 : it.ip ? 1 : -1);
        a.optBotLevel = std::max(0, std::min(kBotLevels - 1, a.optBotLevel));
        a.optBotAdapt = false;
        break;
    case MenuAct::OptMeet: a.optBotMeet = !a.optBotMeet; break;   // FIST BUMP FIRST (practice)
    case MenuAct::OptBack: a.rulesStep = 0; break;
    case MenuAct::OptGo: {
        applyRules(a);
        LOGI("rules -> %s (%s)", rulesLine(a.game.tun).c_str(), a.rulesEdit ? "changed" : a.rulesForHost ? "host a room" : "practice");
        a.rulesOpen = false;
        a.rulesStep = 0;
        if (a.rulesEdit) { a.settingsOpen = true; a.rulesEdit = false; break; }
        if (a.rulesForHost) {
            a.net.host();
        } else {
            a.titleOpen = false; a.menuOpen = false;
            a.lobby = false;   // into the court against the bot
            if (a.net.mode() == Net::Mode::Offline) a.game.reset(true);
        }
        break;
    }
    case MenuAct::RulesBack:
        a.rulesOpen = false;
        if (a.rulesEdit) { a.settingsOpen = true; a.rulesEdit = false; }
        else if (!a.rulesForHost) a.titleOpen = true;
        break;
    case MenuAct::Leave: a.net.leave(); break;
    case MenuAct::Join: a.net.join(it.ip, false); a.menuOpen = false; break;         // the room's waiting area, then JOIN QUEUE
    case MenuAct::JoinRoom: a.net.join(it.ip, false); a.menuOpen = false; break;
    case MenuAct::BotFill: a.botFill = !a.botFill; a.net.setBotFill(a.botFill); saveSettings(a); break;
    case MenuAct::Rules:   // the rules and MATCH OPTIONS of this arena (or of practice)
        a.settingsOpen = false;
        a.rulesOpen = true; a.rulesStep = 0; a.rulesEdit = true;
        a.rulesForHost = a.net.mode() == Net::Mode::Host;
        optionsFrom(a, a.game.tun);
        break;
    case MenuAct::Close: a.menuOpen = false; a.nameEdit = false; a.helpOpen = false; a.settingsOpen = false; a.titleOpen = false; a.xpOpen = false; a.rulesOpen = false; break;
    case MenuAct::Spectate: a.net.spectate(); a.watchIp = 0; a.menuOpen = false; break;
    case MenuAct::Watch: a.watchIp = it.ip; a.menuOpen = false; break;
    case MenuAct::Hub: a.watchIp = 0; a.autoCam = false; a.dirOpen = false; break;
    case MenuAct::Queue: a.net.queue(a.net.localQueuePos() < 0); break;
    case MenuAct::HideOutside: a.hideOutside = !a.hideOutside; saveSettings(a); break;
    case MenuAct::ClearWarps: a.warps.clear(); a.warpIdx = -1; a.autoCam = false; saveWarps(a); break;
    case MenuAct::Director: openDirector(a, a.localIn); break;
    case MenuAct::Avatar: a.titleOpen = false; openBuilder(a, a.localIn); break;
    case MenuAct::Play:   // CHOOSE THE RULES first (then MATCH OPTIONS)
        a.titleOpen = false; a.rulesOpen = true; a.rulesForHost = false; a.rulesEdit = false; a.rulesStep = 0;
        optionsFrom(a, a.game.tun);
        break;
    case MenuAct::Multi: a.titleOpen = false; break;
    case MenuAct::Settings: a.settingsOpen = true; break;
    case MenuAct::Back: if (a.voicePage) a.voicePage = false; else if (a.ptPage) a.ptPage = false; else if (a.gfxPage) a.gfxPage = false; else a.settingsOpen = false; saveSettings(a); break;
    case MenuAct::GfxPage: a.gfxPage = true; break;
    case MenuAct::FovDown: a.roomFeel = std::max(-4, a.roomFeel - 1); saveSettings(a); break;
    case MenuAct::FovUp: a.roomFeel = std::min(4, a.roomFeel + 1); saveSettings(a); break;
    case MenuAct::VoicePage: a.voicePage = true; break;
    case MenuAct::Passthrough: a.ptPage = true; break;
    case MenuAct::PtMode:
        if ((int)it.ip != a.ptMode) { a.ptMode = (int)it.ip % PT_COUNT; a.ptFloor = kPtModeFloor[a.ptMode]; }
        LOGI("passthrough mode %s", kPtModeName[a.ptMode]);
        saveSettings(a);
        break;
    case MenuAct::PtAdjust: {   // 0/1 floor -/+, 2/3 glass, 4/5 room
        float* f[3] = {&a.ptFloor, &a.ptGlass, &a.ptRoom};
        const float lo[3] = {0.f, 0.f, 0.1f}, hi[3] = {1.f, 2.f, 1.f};
        const int k = (int)it.ip / 2;
        *f[k] = clampf(std::round((*f[k] + ((it.ip & 1) ? 0.1f : -0.1f)) * 10.f) / 10.f, lo[k], hi[k]);
        a.ptStyleDirty = true;
        saveSettings(a);
        break;
    }
    case MenuAct::PtMono: a.ptMono = !a.ptMono; a.ptStyleDirty = true; saveSettings(a); break;
    case MenuAct::PtPreset:   // INDOOR: all game inside the court; OUTDOOR: see everything, real objects outlined
        if (it.ip == 0) { a.ptMode = PT_WINDOW; a.ptFloor = 1.f; a.ptGlass = 1.f; a.ptRoom = 1.f; a.ptMono = false; a.ptEdges = false; }
        else { a.ptMode = PT_OUTLINE; a.ptFloor = 0.f; a.ptGlass = 1.f; a.ptRoom = 0.7f; a.ptMono = false; a.ptEdges = true; }
        a.ptEdgeReveal = true;
        a.ptStyleDirty = true;
        LOGI("passthrough preset %s", it.ip == 0 ? "indoor" : "outdoor");
        saveSettings(a);
        break;
    case MenuAct::PtReveal: a.ptEdgeReveal = !a.ptEdgeReveal; saveSettings(a); break;
    case MenuAct::PtPeek: a.ptPeekOn = !a.ptPeekOn; saveSettings(a); break;
    case MenuAct::PtEdges: a.ptEdges = !a.ptEdges; a.ptStyleDirty = true; saveSettings(a); break;
    case MenuAct::Tuning: a.settingsOpen = false; openTuning(a, a.localIn); break;
    case MenuAct::VoiceMode: a.voiceMode = (a.voiceMode + 1) % 3; a.micDenied = false; saveSettings(a); break;
    case MenuAct::MuteSpec: a.muteSpectators = !a.muteSpectators; saveSettings(a); break;
    case MenuAct::MutePlayers: a.mutePlayers = !a.mutePlayers; saveSettings(a); break;
    case MenuAct::MuteAll: a.muteAll = !a.muteAll; saveSettings(a); break;
    case MenuAct::VolDown: a.volume = std::max(0, a.volume - 1); a.audio.setVolume(a.volume / 10.f); saveSettings(a); break;
    case MenuAct::VolUp: a.volume = std::min(10, a.volume + 1); a.audio.setVolume(a.volume / 10.f); saveSettings(a); break;
    case MenuAct::Hand: a.pointerHand = 1 - a.pointerHand; saveSettings(a); break;
    case MenuAct::Fx: a.fxLevel = (a.fxLevel + 1) % 3; saveSettings(a); break;
    case MenuAct::Crowd: a.crowdMode = (a.crowdMode + 1) % 3; saveSettings(a); break;
    case MenuAct::Turn: a.turnMode = (a.turnMode + 1) % 3; saveSettings(a); break;
    case MenuAct::Name: a.nameEdit = true; a.editTarget = 0; a.nameBuf = a.net.name(); break;
    case MenuAct::Server: a.nameEdit = true; a.editTarget = 1; a.nameBuf = a.serverAddr; break;
    case MenuAct::ServerDefault:
        a.serverAddr = PA_DEFAULT_SERVER;
        saveSettings(a);
        LOGI("server -> default %s", a.serverAddr.c_str());
        if (a.net.online()) { a.net.goOffline(); a.net.goOnline(a.serverAddr.c_str()); }
        break;
    case MenuAct::Online: {
        const bool wasSpec = a.net.mode() == Net::Mode::Spectate;
        if (a.net.online()) a.net.goOffline();
        else if (a.serverAddr.empty()) { a.nameEdit = true; a.editTarget = 1; a.nameBuf.clear(); }
        else a.net.goOnline(a.serverAddr.c_str());
        if (wasSpec) { a.net.spectate(); a.watchIp = 0; }   // keep spectating, now on the other network
        a.onlinePref = a.net.online();
        saveSettings(a);
        break;
    }
    case MenuAct::Help: a.helpOpen = true; break;
    case MenuAct::HelpDone:
        a.helpOpen = false;
        if (!a.titleOpen) a.menuOpen = false;
        if (const char* ext = dataDir(a))
            if (FILE* fp = fopen((std::string(ext) + "/help_seen").c_str(), "w")) fclose(fp);
        break;
    case MenuAct::Key: if (a.nameBuf.size() < (a.editTarget >= 4 ? kNoteMax : a.editTarget == 1 || a.editTarget == 3 ? 48u : 16u) && !(it.ip == ' ' && (a.nameBuf.empty() || a.nameBuf.back() == ' '))) a.nameBuf += (char)it.ip; break;
    case MenuAct::KeyDel: if (!a.nameBuf.empty()) a.nameBuf.pop_back(); break;
    case MenuAct::KeyDone:
        while (!a.nameBuf.empty() && a.nameBuf.back() == ' ') a.nameBuf.pop_back();
        if (a.editTarget == 5) { a.reportNote = a.nameBuf; }
        else if (a.editTarget == 4) { a.bugNote = a.nameBuf; }
        else if (a.editTarget == 3) { if (!a.nameBuf.empty()) a.sparcAddr = a.nameBuf; saveSettings(a); }
        else if (a.editTarget == 2) { a.team = a.nameBuf.substr(0, 16); saveSettings(a); a.menuOpen = false; }
        else if (a.editTarget == 1) {
            a.serverAddr = a.nameBuf;
            if (!a.serverAddr.empty()) { a.onlinePref = true; a.net.goOnline(a.serverAddr.c_str()); }
            saveSettings(a);
        }
        else if (!a.nameBuf.empty()) savePlayerName(a, a.nameBuf.c_str());
        a.nameEdit = false;
        break;
    default: break;
    }
}

// pointer: right-controller aim ray against the panel plane; a trigger click activates the hovered item
static void updateMenu(App& a, PlayerInput& in) {
    a.menuHover = -1;
    a.menuHitValid = false;
    if (!a.menuOpen) return;
    const bool trig = in.hand[a.pointerHand].trigger > 0.7f;
    if (a.aimValid[a.pointerHand]) {
        Vec3 o = a.aim[a.pointerHand].p, d = a.aim[a.pointerHand].q.rotate({0, 0, -1});
        Vec3 nz = a.menuPose.q.rotate({0, 0, 1});
        float den = dot(d, nz);
        if (den < -1e-3f) {
            float t = dot(a.menuPose.p - o, nz) / den;
            if (t > 0 && t < 4) {
                Vec3 p = o + d * t;
                Vec3 lp = a.menuPose.q.conj().rotate(p - a.menuPose.p);
                const bool onPanel = std::fabs(lp.x) < 0.5f && std::fabs(lp.y + 0.09f) < 0.47f;
                const bool onSide = !a.nameEdit && lp.x > kNavX - 0.15f && lp.x < kNavX + 0.15f && std::fabs(lp.y + 0.09f) < 0.47f;
                if (onPanel || onSide) { a.menuHit = p; a.menuHitValid = true; }
                std::vector<MenuItem> items = menuItems(a);
                for (size_t i = 0; i < items.size(); ++i) {
                    const MenuItem& it = items[i];
                    if (it.enabled && std::fabs(lp.x - it.x) < it.w * 0.5f && std::fabs(lp.y - it.y) < it.h * 0.5f) a.menuHover = (int)i;
                }
                if (trig && !a.prevMenuTrig && a.menuHover >= 0) {
                    menuActivate(a, items[a.menuHover]);
                    haptic(a, a.pointerHand, 0.4f, 0.03f);
                }
            }
        }
    }
    a.prevMenuTrig = trig;
    for (int h = 0; h < 2; ++h) { in.hand[h].trigger = 0; in.hand[h].squeeze = 0; }   // no grabbing / shields in the menu
}

// MENU > MY STATS: the player's career from the community server as a dashboard: three rings (win rate, throw
// accuracy, blocks), four number tiles, the top speed on a bar. Asked again every 5 s while open.
static void drawStats(App& a) {
    auto at = [&](float x, float y, float z) { return a.menuPose.transform({x, y, z}); };
    auto quad = [&](float x, float y, float w, float h, float z, const float* c, float ang = 0.f) {
        a.gfx.drawPanel(Mat4::fromPose({a.menuPose.q * Quat::axisAngle({0, 0, 1}, ang), at(x, y, z)}, {w, h, 1}), c);
    };
    auto text = [&](const std::string& s, float x, float y, float h, const float* c) {
        a.gfx.drawText(s, {a.menuPose.q, at(x, y, 0.006f)}, h, c);
    };
    const float white[4] = {1, 1, 1, 1}, grey[4] = {0.45f, 0.5f, 0.56f, 1}, cyan[4] = {0.3f, 0.85f, 1.0f, 1};
    const float dim[4] = {0.06f, 0.09f, 0.13f, 1}, tile[4] = {0.02f, 0.04f, 0.07f, 1};
    char buf_stats[64];
    const double t = nowSeconds();
    if (a.net.onlineReady() && t > a.myStatsAskAt) { a.myStatsAskAt = t + 5.0; a.net.requestMyStats(); }
    Net::CareerStats mine[2];
    if (a.net.statsFor(Net::kMyStats, mine)) { a.myCareer = mine[0]; a.myCareerOk = true; }
    text("MY STATS", 0, 0.29f, 0.055f, cyan);
    text(a.net.name(), 0, 0.235f, 0.026f, white);
    const Net::CareerStats& c = a.myCareer;
    if (!a.net.onlineReady() && !a.myCareerOk) { text("CONNECT TO THE COMMUNITY SERVER TO SEE YOUR STATS", 0, 0.05f, 0.022f, white); return; }
    if (!a.myCareerOk) { text("ASKING THE SERVER...", 0, 0.05f, 0.022f, grey); return; }
    if (!c.matches) {
        text("NO MATCHES YET", 0, 0.08f, 0.032f, white);
        text("PLAY AN ONLINE MATCH AGAINST ANOTHER PLAYER TO START YOUR STATS", 0, 0.02f, 0.019f, grey);
    }
    // the rings: a background track, the filled share clockwise from the top, the number in the middle
    struct Ring { const char* label; float frac; const float* col; };
    const float green[4] = {0.25f, 0.95f, 0.55f, 1}, gold[4] = {1.0f, 0.78f, 0.25f, 1};
    const Ring rings[3] = {{"WIN RATE", c.matches ? float(c.wins) / c.matches : 0.f, cyan},
                           {"THROW ACCURACY", c.throws ? float(c.onTarget) / c.throws : 0.f, green},
                           {"BLOCKS", c.faced ? float(c.blocks) / c.faced : 0.f, gold}};
    const int N = 48;
    for (int r = 0; r < 3 && c.matches; ++r) {
        const float cx = -0.32f + 0.32f * r, cy = 0.075f, R = 0.085f;
        for (int i = 0; i < N; ++i) {
            const float ang = kPi * 0.5f - (i + 0.5f) * 2.f * kPi / N;
            const bool on = (i + 0.5f) / N <= rings[r].frac;
            quad(cx + R * std::cos(ang), cy + R * std::sin(ang), 2.f * kPi * R / N * 1.08f, 0.018f, on ? 0.003f : 0.002f,
                 on ? rings[r].col : dim, ang - kPi * 0.5f);
        }
        std::snprintf(buf_stats, sizeof buf_stats, "%d%%", (int)std::lround(rings[r].frac * 100.f));
        text(buf_stats, cx, cy, 0.040f, white);
        text(rings[r].label, cx, cy - 0.125f, 0.018f, grey);
    }
    if (!c.matches) return;
    // the tiles
    const char* tl[5] = {"MATCHES", "WINS", "HITS", "FROM RETURNS", "CATCHES"};   // (hits: from throws and returns)
    const uint32_t tv[5] = {c.matches, c.wins, c.hits, c.returnHits, c.catches};
    for (int k = 0; k < 5; ++k) {
        const float x = -0.40f + 0.20f * k, y = -0.17f;
        quad(x, y, 0.18f, 0.10f, 0.002f, tile);
        std::snprintf(buf_stats, sizeof buf_stats, "%u", tv[k]);
        text(buf_stats, x, y + 0.012f, 0.040f, white);
        text(tl[k], x, y - 0.033f, 0.015f, grey);
    }
    // the top speed on a 0-100 km/h bar
    const float kmh = c.topSpeed * 3.6f, fr = std::min(1.f, kmh / 100.f), y = -0.31f, w = 0.80f;
    quad(0, y, w, 0.026f, 0.002f, dim);
    quad(-w * 0.5f + w * fr * 0.5f, y, w * fr, 0.026f, 0.003f, gold);
    std::snprintf(buf_stats, sizeof buf_stats, "TOP THROW SPEED  %d KM/H", (int)std::lround(kmh));
    text(buf_stats, 0, y + 0.04f, 0.020f, white);
    text("0", -w * 0.5f, y - 0.035f, 0.014f, grey);
    text("100 KM/H", w * 0.5f - 0.04f, y - 0.035f, 0.014f, grey);
    // left of the DELETE button (x 0.04 to 0.44): each line under 0.36 wide (a letter is 0.75 x its height)
    text("YOUR STATS ARE SHOWN TO OTHER PLAYERS", -0.22f, -0.457f, 0.0125f, grey);
    text("AND SPECTATORS", -0.22f, -0.482f, 0.0125f, grey);
    text("DELETE ERASES THEM FROM THE SERVER", -0.22f, -0.507f, 0.0125f, grey);
}

static void drawMenu(App& a) {
    if (!a.menuOpen) return;
    auto at = [&](float x, float y, float z) { return a.menuPose.transform({x, y, z}); };
    auto quad = [&](float x, float y, float w, float h, float z, const float* c) {
        a.gfx.drawPanel(Mat4::fromPose({a.menuPose.q, at(x, y, z)}, {w, h, 1}), c);
    };
    auto text = [&](const std::string& s, float x, float y, float h, const float* c) {
        if (std::fabs(x) < 0.5f) {   // on the panel: keep inside its edges (the font's glyphs are 0.75 x their height)
            const float room = 2.f * (0.485f - std::fabs(x)), w = 0.75f * h * (float)s.size();
            if (w > room) h *= room / w;
        }
        a.gfx.drawText(s, {a.menuPose.q, at(x, y, 0.006f)}, h, c);
    };
    const float ua = a.ptRunning ? 0.998f : 0.f;   // over your room the menus are solid (never see-through)
    const float bg[4] = {0.004f, 0.007f, 0.016f, std::max(ua, 0.90f)}, edge[4] = {0.05f, 0.35f, 0.60f, std::max(ua, 0.9f)};
    const float btn[4] = {0.03f, 0.05f, 0.08f, std::max(ua, 0.95f)}, hov[4] = {0.05f, 0.40f, 0.75f, std::max(ua, 0.98f)},
                dis[4] = {0.015f, 0.015f, 0.02f, std::max(ua, 0.9f)};
    const float white[4] = {1, 1, 1, 1}, grey[4] = {0.35f, 0.38f, 0.42f, 1}, cyan[4] = {0.3f, 0.85f, 1.0f, 1};
    const bool airy = a.settingsOpen && a.gfxPage && !a.nameEdit;   // GRAPHICS: the arena shows around its buttons
    if (!airy) {
        quad(0, -0.09f, 1.04f, 0.98f, -0.002f, edge);
        quad(0, -0.09f, 1.0f, 0.94f, 0, bg);
    }
    if (!a.nameEdit && !airy) {   // side menu
        quad(kNavX, -0.09f, 0.30f, 0.98f, -0.002f, edge);
        quad(kNavX, -0.09f, 0.28f, 0.94f, 0, bg);
    }
    char buf[96];
    if (a.statsNoticeOpen && !a.nameEdit) {
        text("ONLINE MATCH STATISTICS", 0, 0.27f, 0.045f, cyan);
        text("WHEN YOU PLAY AN ONLINE MATCH AGAINST ANOTHER PLAYER,", 0, 0.16f, 0.021f, white);
        text("THE COMMUNITY SERVER KEEPS YOUR CAREER STATISTICS:", 0, 0.12f, 0.021f, white);
        text("MATCHES, WINS, THROWS, HITS, BLOCKS, CATCHES, TOP SPEED", 0, 0.07f, 0.019f, grey);
        text("THEY ARE SHOWN TO OTHER PLAYERS AND SPECTATORS WITH YOUR NAME", 0, 0.01f, 0.021f, white);
        text("NO ACCOUNT, NO EMAIL: THEY ARE TIED TO A RANDOM KEY YOUR GAME MADE", 0, -0.04f, 0.019f, grey);
        text("SEE OR DELETE THEM ANY TIME IN MENU - MY STATS", 0, -0.11f, 0.021f, white);
        text("TO PLAY WITHOUT THEM: MULTIPLAYER - ONLINE: OFF", 0, -0.16f, 0.019f, grey);
        text("PRIVACY POLICY: GITHUB.COM/JENDOEBELIN/PROJECTARENA-PUBLIC", 0, -0.22f, 0.016f, grey);
    } else if (a.statsOpen && !a.nameEdit) {
        drawStats(a);
    } else if (a.bugOpen && !a.nameEdit) {
        text("REPORT A BUG", 0, 0.29f, 0.055f, cyan);
        text("WHAT KIND OF PROBLEM? PICK ONE, ADD A NOTE IF YOU LIKE", 0, 0.215f, 0.022f, white);
        text("SENDS THE NOTE, YOUR SETTINGS AND THE GAME'S RECENT LOG - NOTHING ELSE", 0, -0.33f, 0.018f, grey);
        text("TIP: REPORT RIGHT AFTER IT HAPPENS", 0, -0.37f, 0.018f, grey);
    } else if (a.rulesOpen && a.rulesStep == 1 && !a.nameEdit) {
        Tunables t;
        optionsInto(a, t);
        text("MATCH OPTIONS", 0, 0.29f, 0.055f, cyan);
        text(std::string(t.advanced ? "ADVANCED RULES" : t.experimental ? "EXPERIMENTAL (BASIC RULES)" : "BASIC RULES") +
             (a.rulesForHost || a.net.mode() == Net::Mode::Host ? " - ONLY THE HOST CAN CHANGE THESE" : " - FOR PRACTICE AGAINST THE BOT"),
             0, 0.215f, 0.022f, white);
        const char* hint = t.freePlay() ? "FREE PLAY: NO CLOCK AND NO WINNER - PRACTICE OR PLAY ON"
                         : t.advanced && t.advancedRoundTime > 0 ? "AT TIME, MORE POINTS WINS THE ROUND - A TIE GOES TO SUDDEN DEATH"
                         : t.advanced ? "NO TIME LIMIT: FIRST TO 4 POINTS WINS THE ROUND"
                         : "AT TIME, MORE POINTS WINS - A TIE GOES TO SUDDEN DEATH";
        const bool bot = !a.rulesForHost;
        text(hint, 0, bot ? -0.235f : -0.125f, 0.018f, grey);
        if (!t.shields) text("HOTDOG MODE: NO SHIELDS - DODGE, CATCH AND PUNCH", 0, bot ? -0.26f : -0.155f, 0.018f, grey);
        if (bot) {   // what the level means (every level moves as a person can: reach, speed, reaction)
            static const char* kBotLine[kBotLevels] = {
                "SLOW TO REACT, OFTEN MISREADS THE BALL - A GENTLE START",
                "BLOCKS SOME, MISSES CURVES AND BANK SHOTS",
                "A SOLID PLAYER - READS CURVES AND BANKS",
                "QUICK HANDS, ANGLES ITS BLOCKS, PICKS ITS SHOTS",
                "RARELY MISSES - GUARDS ITS STRIKE ZONE",
                "READS EVERY BALL - CAN YOU SCORE ONE?"};
            text(t.botAdapt ? "IT GETS BETTER WHEN YOU SCORE, EASIER WHEN IT DOES - CLOSE MATCHES"
                            : kBotLine[std::max(0, std::min(kBotLevels - 1, t.botLevel))], 0, -0.205f, 0.018f, white);
        }
        if (rulesLive(a) && a.rulesEdit) text("A MATCH IS ON: THE NEW RULES START WITH THE NEXT MATCH", 0, -0.33f, 0.018f, grey);
    } else if (a.rulesOpen && !a.nameEdit) {
        text("CHOOSE THE RULES", 0, 0.29f, 0.055f, cyan);
        text(a.rulesForHost ? "FOR YOUR ROOM - EVERYONE WHO JOINS PLAYS THESE RULES" : "FOR PRACTICE AGAINST THE BOT", 0, 0.215f, 0.022f, white);
        text("BEST OF 3 ROUNDS, FIRST TO 4 POINTS - LOW AIM ASSIST", 0, 0.045f, 0.019f, grey);
        text("3 MINUTE MATCH - KNUCKLE GUARDS - MORE AIM ASSIST", 0, -0.115f, 0.019f, grey);
        text("DIAMOND: THE COURT ROLLED 45 DEGREES, ON FLOATING PLATFORMS", 0, -0.265f, 0.019f, grey);
    } else if (a.reportOpen && !a.nameEdit) {
        text("REPORT OR MUTE A PLAYER", 0, 0.29f, 0.050f, cyan);
        if (reportPeople(a).empty()) {
            text("NOBODY ELSE IS HERE", 0, 0.12f, 0.030f, white);
            text("JOIN A ROOM, OR STAND AT AN ARENA WHILE SPECTATING", 0, 0.06f, 0.020f, grey);
        } else {
            text("WHO? PICK THE PERSON, THEN WHY", 0, 0.215f, 0.022f, white);
        }
        text("SENT TO THE PROJECT ARENA TEAM WITH THE NAMES HERE AND THE RECENT LOG", 0, -0.50f, 0.017f, grey);
        text("MUTE SILENCES THEIR VOICE FOR YOU UNTIL YOU QUIT THE GAME", 0, -0.53f, 0.017f, grey);
    } else if (a.xpOpen && !a.nameEdit) {
        const float amber[4] = {1.0f, 0.75f, 0.2f, 1};
        text("EXPERIMENTAL SOFTWARE", 0, 0.29f, 0.050f, cyan);
        text("UNFINISHED FEATURES FOR TEST BUILDS - THINGS WILL BREAK", 0, 0.225f, 0.020f, amber);
        text("CROSS-PLAY TEST: JOIN THE SPARC REVIVER SERVER", 0, 0.155f, 0.028f, white);
        const double now = nowSeconds();
        snprintf(buf, sizeof buf, "STATUS: %s", a.sparc.stateText());
        if (a.sparc.state() == xc::SparcLink::State::Joined) {
            int s = (int)a.sparc.connectedFor(now);
            snprintf(buf, sizeof buf, "STATUS: CONNECTED %d:%02d AS %s", s / 60, s % 60, a.sparc.nameSent().c_str());
        } else if (a.sparc.state() == xc::SparcLink::State::Off && !a.sparc.error().empty()) {
            snprintf(buf, sizeof buf, "STATUS: OFF (%s)", a.sparc.error().c_str());
        } else if (a.sparc.state() == xc::SparcLink::State::Failed) {
            snprintf(buf, sizeof buf, "FAILED: %s", a.sparc.error().c_str());
        }
        text(buf, 0, -0.105f, 0.026f, a.sparc.state() == xc::SparcLink::State::Failed ? amber : white);
        if (a.sparc.state() == xc::SparcLink::State::Joined || a.sparc.state() == xc::SparcLink::State::Loading) {
            std::string m = a.sparc.map();
            size_t sl = m.rfind('/');
            if (sl != std::string::npos) m = m.substr(sl + 1);
            snprintf(buf, sizeof buf, "MAP %s   OBJECTS %d   PLAYERS %d", m.c_str(), a.sparc.channelCount(),
                     a.sparc.countOf("PlaygroundPlayerState"));
            text(buf, 0, -0.15f, 0.022f, grey);
            std::string who;
            for (const std::string& n : a.sparc.playerNames()) {
                if (n.rfind("vmi", 0) == 0) continue;   // the dedicated server's own player state
                if (!who.empty()) who += ", ";
                who += n;
            }
            if (who.size() > 60) who = who.substr(0, 57) + "...";
            text("ON THE SERVER: " + (who.empty() ? std::string("-") : who), 0, -0.225f, 0.020f, white);
            snprintf(buf, sizeof buf, "PACKETS IN %llu  OUT %llu  ERRORS %u", (unsigned long long)a.sparc.packetsIn(),
                     (unsigned long long)a.sparc.packetsOut(), a.sparc.decodeErrors());
            text(buf, 0, -0.19f, 0.022f, grey);
        } else {
            snprintf(buf, sizeof buf, "YOU WILL APPEAR AS %s", sparcName(a).c_str());
            text(buf, 0, -0.15f, 0.022f, grey);
        }
        text("THIS TEST ONLY CONNECTS AND STAYS IN AS A SPECTATOR.", 0, -0.25f, 0.020f, grey);
        text("SEEING THE MATCH AND PLAYING ARE NOT BUILT YET.", 0, -0.285f, 0.020f, grey);
    } else if (a.titleOpen && !a.helpOpen && !a.nameEdit && !a.settingsOpen) {
        if (!a.emblemTex) a.emblemTex = a.gfx.texture("open_emblem", true);
        if (a.emblemTex) {   // the emblem floats above the panel, a soft glow behind it
            const float glow[4] = {0.10f, 0.45f, 0.75f, 0.35f}, white[4] = {1, 1, 1, 1};
            quad(0, 0.585f, 0.36f, 0.36f, -0.004f, edge);   // a badge like the panel (hides the scoreboard behind)
            quad(0, 0.585f, 0.33f, 0.33f, -0.002f, bg);
            a.gfx.drawSprite(at(0, 0.585f, 0.0f), 0.42f, glow, a.viewHead.p, 1);
            a.gfx.drawTexturedPanel(Mat4::fromPose({a.menuPose.q, at(0, 0.585f, 0.004f)}, {0.29f, 0.29f, 1}), a.emblemTex, white, false);
        }
        text("PROJECT ARENA", 0, 0.27f, 0.085f, cyan);
        snprintf(buf, sizeof buf, "WELCOME, %s", a.net.name());
        text(buf, 0, 0.185f, 0.03f, white);
        if (const char* v = updateAvailable(a)) {
            const float amber[4] = {1.0f, 0.75f, 0.2f, 1};
            snprintf(buf, sizeof buf, "UPDATE AVAILABLE: %s  -  FROM THE STORE LIBRARY, OR GITHUB IF YOU SIDELOADED", v);
            text(buf, 0, 0.145f, 0.020f, amber);
        }
    } else if (a.settingsOpen && !a.nameEdit) {
        text(a.voicePage ? "VOICE CHAT" : a.ptPage ? "PASSTHROUGH" : a.gfxPage ? "GRAPHICS AND VIEW" : "SETTINGS", 0, 0.29f, 0.055f, cyan);
        if (a.gfxPage) {
            const float strip[4] = {0.004f, 0.007f, 0.016f, 0.985f};
            quad(0, -0.01f, 0.88f, 0.25f, -0.001f, strip);   // a strip behind the slider only: the arena shows around it
            text("FIELD OF VIEW", 0, 0.08f, 0.04f, white);
            text("HOW BIG THE ARENA FEELS: + WIDER, - CLOSER. LOOK AROUND WHILE YOU SET IT", 0, 0.03f, 0.019f, grey);
            for (int k = -4; k <= 4; ++k) {   // the slider: a notch for each step, the chosen one lit
                const float on[4] = {0.3f, 0.85f, 1.0f, 1}, off[4] = {0.10f, 0.16f, 0.22f, 1};
                quad(0.035f * k, -0.10f, k == a.roomFeel ? 0.022f : 0.012f, k == a.roomFeel ? 0.04f : 0.022f, 0.004f, k == a.roomFeel ? on : off);
            }
        }
        if (a.voicePage) text("ONLY PEOPLE AT YOUR ARENA CAN HEAR YOU", 0, 0.205f, 0.024f, grey);
        if (a.ptPage) {
            std::string h = kPtModeHelp[a.ptMode % PT_COUNT];
            if (a.ptMode == PT_WINDOW)   // what hides the real things inside the court (environment depth)
                h = !a.hasDepth ? "WINDOW: YOUR ROOM THROUGH THE GLASS (NO DEPTH ON THIS HEADSET)"
                    : a.depthRunning ? "WINDOW: THINGS INSIDE THE COURT HIDDEN (ROOM DEPTH ON)"
                    : !a.scenePerm ? "WINDOW: ALLOW SPATIAL DATA TO HIDE THINGS INSIDE THE COURT"
                    : a.depthFailed ? "WINDOW: ROOM DEPTH COULD NOT START" : "WINDOW: STARTING ROOM DEPTH";
            text(h, 0, 0.205f, 0.022f, grey);
        }
    } else if (a.helpOpen && !a.nameEdit) {
        text("HOW TO PLAY", 0, 0.29f, 0.055f, cyan);
        static const char* lines[] = {
            "GRAB: REACH FOR YOUR BALL, HOLD THE TRIGGER",
            "THROW: SWING AND RELEASE - FLICK TO CURVE IT",
            "HIT YOUR OPPONENT TO SCORE",
            "HOLDING A CHARGED BALL RAISES A SHIELD",
            "PUNCH BALLS AWAY WITH YOUR KNUCKLE GUARD",
            "BALL THROUGH THEIR RING = STRIKE: BIGGER, FASTER",
            "THROW PAST YOUR FOUL LINE TO RECHARGE",
            "",
            "B / STICK CLICK: RECENTER    MENU: GAMES, AVATAR"};
        for (int i = 0; i < 9; ++i) text(lines[i], 0, 0.20f - 0.055f * i, 0.024f, i == 8 ? grey : white);
    } else if (a.nameEdit) {
        text(a.editTarget == 5 ? "REPORT NOTE" : a.editTarget == 4 ? "BUG NOTE" : a.editTarget == 3 ? "SPARC SERVER" : a.editTarget == 2 ? "TEAM NAME" : a.editTarget ? "COMMUNITY SERVER" : "YOUR NAME", 0, 0.29f, 0.055f, cyan);
        {
            std::string shown = a.nameBuf + "_";
            if (shown.size() > 34) shown = "..." + shown.substr(shown.size() - 31);   // the end of a long note
            text(shown, 0, 0.205f, 0.040f, white);
            const size_t cap = a.editTarget >= 4 ? kNoteMax : (a.editTarget == 1 || a.editTarget == 3) ? 48u : 16u;
            const size_t left = cap > a.nameBuf.size() ? cap - a.nameBuf.size() : 0;
            snprintf(buf, sizeof buf, "%zu LEFT", left);
            const float amber[4] = {1.0f, 0.75f, 0.2f, 1};
            text(buf, 0.40f, 0.155f, 0.020f, left < 10 ? amber : grey);   // bottom right of the field
        }
    } else if (!a.titleOpen && !a.settingsOpen) {
        text("MULTIPLAYER", 0, 0.29f, 0.055f, cyan);
        if (a.net.inRoom())
            snprintf(buf, sizeof buf, "YOU: %s   -   %s   -   %s RULES", a.net.name(), a.net.status(), a.game.tun.advanced ? "ADVANCED" : "BASIC");
        else
            snprintf(buf, sizeof buf, "YOU: %s   -   %s", a.net.name(), inLobby(a) ? "LOBBY" : a.net.status());
        text(buf, 0, 0.215f, 0.026f, white);
        if (a.net.online() && !a.net.onlineReady()) {
            const float amber[4] = {1.0f, 0.75f, 0.2f, 1};
            snprintf(buf, sizeof buf, "CONNECTING TO %s...", a.serverAddr.c_str());
            text(buf, 0.25f, 0.165f, 0.020f, amber);
        } else {
            text(a.net.online() ? "GAMES ONLINE" : "GAMES ON YOUR NETWORK", 0.25f, 0.165f, 0.026f, grey);
        }
    }
    std::vector<MenuItem> items = menuItems(a);
    int games = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        const MenuItem& it = items[i];
        const bool row = (it.act == MenuAct::Join || it.act == MenuAct::JoinRoom) && it.ip;
        if ((it.act == MenuAct::Join || it.act == MenuAct::JoinRoom || it.act == MenuAct::Watch) && it.ip) ++games;
        const bool picked = (it.act == MenuAct::Nav && (int)it.ip == navCurrent(a)) ||
                            (it.act == MenuAct::BugCat && (int)it.ip == a.bugCat) ||
                            (it.act == MenuAct::RulesPick && (int)it.ip == (a.game.tun.advanced ? 1 : a.game.tun.experimental ? 2 : 0)) || (it.act == MenuAct::ReportWho && it.ip == a.reportWho) ||
                            (it.act == MenuAct::ReportWhy && (int)it.ip == a.reportWhy) || (it.act == MenuAct::PtMode && (int)it.ip == a.ptMode) ||
                            (it.act == MenuAct::OptBot && it.ip >= 10 &&
                             (int)it.ip - 10 <= (a.optBotAdapt ? (int)std::lround(a.game.tun.botMatch) : a.optBotLevel)) ||
                            (it.act == MenuAct::OptBot && it.ip == 2 && a.optBotAdapt);
        quad(it.x, it.y, it.w, it.h, 0.003f, !it.enabled && !row ? dis : ((int)i == a.menuHover || picked) ? hov : btn);
        float th = row ? 0.026f : 0.034f;
        const float tw = th * 0.75f * (float)it.label.size();          // the font's glyphs are 0.75 x their height
        if (tw > it.w * 0.90f) th *= it.w * 0.90f / tw;                 // long labels shrink instead of spilling over
        text(it.label, it.x, it.y, th, it.enabled || row ? white : grey);
    }
    if (games == 0 && !a.nameEdit && !a.helpOpen && !a.titleOpen && !a.settingsOpen && !a.xpOpen && !a.bugOpen && !a.reportOpen && !a.rulesOpen &&
        !a.statsOpen && !a.statsNoticeOpen) {
        if (int ov = a.net.otherVersion()) {
            const float amber[4] = {1.0f, 0.75f, 0.2f, 1};
            snprintf(buf, sizeof buf, "A GAME ON VERSION %d IS NEARBY", ov);
            text(buf, 0.25f, -0.12f, 0.022f, amber);
            text("UPDATE BOTH HEADSETS TO PLAY", 0.25f, -0.16f, 0.022f, amber);
        }
        text(a.spectating ? "NO LIVE GAMES YET" : "NO GAMES FOUND YET", 0.25f, 0.08f, 0.026f, grey);
        text("ON THE OTHER HEADSET:", 0.25f, 0.0f, 0.022f, grey);
        text("MENU - HOST A ROOM", 0.25f, -0.05f, 0.022f, grey);
        if (!a.net.otherVersion())   // both headsets have to be on the same network to see each other
            text(a.net.online() ? "WITH ONLINE: ON" : "WITH ONLINE: OFF (LAN)", 0.25f, -0.10f, 0.022f, grey);
    }
    if (!a.statsOpen) text("POINT AND PULL THE TRIGGER TO SELECT", 0, -0.455f, 0.022f, grey);
    if (a.spectating && a.watchIp && !a.titleOpen && !a.settingsOpen && !a.nameEdit && !a.helpOpen) {
        snprintf(buf, sizeof buf, "STICKS FLY - A SAVE CAMERA - X NEXT - Y AUTO CAMERA   (%d CAMERAS)", (int)a.warps.size());
        text(buf, 0, -0.495f, 0.02f, grey);
    }
    // laser + cursor
    if (a.aimValid[a.pointerHand]) {
        Vec3 o = a.aim[a.pointerHand].p, d = a.aim[a.pointerHand].q.rotate({0, 0, -1});
        float len = a.menuHitValid ? length(a.menuHit - o) : 1.5f;
        const float laser[4] = {0.15f, 0.6f, 1.0f, 1};
        a.gfx.drawBox(Mat4::fromPose({a.aim[a.pointerHand].q, o + d * (len * 0.5f)}, {0.003f, 0.003f, len * 0.5f}), laser, true);
        if (a.menuHitValid) a.gfx.drawSphere(a.menuHit, 0.008f, cyan, true);
    }
}

// ------------------------------------------------------------------ hit / spawn VFX
// Port designs (soft sprites + feathered ribbons). The original Cascade systems they replace (CharHit_3rdPerson_P,
// ProjectileSpawn_Line_P) are documented in the developers' research notes.
static uint32_t fxHash(uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
static float fxRnd(uint32_t seed, uint32_t k) { return (fxHash(seed * 2654435761u + k * 40503u) >> 8) * (1.f / 16777216.f); }
static void addHitFx(App& a, const Vec3& at, int victim, int scorer) {
    App::HitFx h;
    h.pos = at;
    h.dir = victim == 1 ? Vec3{1, 0, 0} : Vec3{-1, 0, 0};   // the ball travels toward the victim's end
    h.t = 0;
    h.team = scorer & 1;
    h.victim = victim;
    h.seed = a.fxSeed = fxHash(a.fxSeed + 0x9e37u);
    if (a.hits.size() < 6) a.hits.push_back(h);
}

static void updateFx(App& a, float dt) {
    for (size_t i = 0; i < a.hits.size();) {
        a.hits[i].t += dt;
        if (a.hits[i].t > 2.2f) { a.hits[i] = a.hits.back(); a.hits.pop_back(); } else ++i;
    }
}

static float easeOut(float u) { u = clampf(u, 0.f, 1.f); return 1.f - (1.f - u) * (1.f - u) * (1.f - u); }

// PORT design (owner: "catch the essence", smoother than the original meshes): a plasma burst made of soft glow
// sprites and feathered ribbons. Energy flashes white-hot, a shock ring rolls out, plasma puffs swirl around the hit
// axis and thin out, curling wisps uncoil, and a few sparks linger. Colour = the scorer's team.
static void drawHitFx(App& a, const App::HitFx& h, int local) {
    const float team[2][3] = {{0.1f, 0.7f, 1.0f}, {1.0f, 0.4f, 0.1f}};
    const float* tc = team[h.team];
    const float t = h.t;
    const Vec3 eye = a.viewHead.p;
    auto col = [&](float white, float k, float out[4]) {
        const float lvl = a.fxLevel == 0 ? 1.f : a.fxLevel == 1 ? 0.55f : 0.25f;
        for (int i = 0; i < 3; ++i) out[i] = (tc[i] * (1 - white) + white) * k * lvl;
        out[3] = 1;
    };
    float c[4];
    const Vec3 ax = h.dir;
    const Vec3 s1 = normalize(cross(ax, {0, 1, 0})), s2 = cross(s1, ax);
    // 1. flash: white-hot core that blooms and cools to the team colour
    if (t < 0.45f) {
        float u = t / 0.45f;
        col(0.85f * (1 - u), 1.6f * (1 - u) * (1 - u), c);
        a.gfx.drawSprite(h.pos, 0.35f + 0.9f * easeOut(t / 0.12f), c, eye, 0);
        col(1.f, 1.2f * (1 - u) * (1 - u), c);
        a.gfx.drawSprite(h.pos, 0.18f, c, eye, 0);
    }
    // 2. shock ring rolling outward
    if (t < 0.7f) {
        float u = t / 0.7f;
        col(0.3f * (1 - u), 0.9f * (1 - u) * (1 - u), c);
        a.gfx.drawSprite(h.pos, 0.25f + 2.4f * easeOut(u), c, eye, 1, fxRnd(h.seed, 7) * 6.28f);
    }
    // 3. plasma cloud: soft puffs that burst out, slow down (drag), swirl around the hit axis and fade
    for (int k = 0; k < 28; ++k) {
        float life = 0.9f + 0.6f * fxRnd(h.seed, 10 + k);
        if (t > life) continue;
        float u = t / life;
        Vec3 r{fxRnd(h.seed, 40 + k) - 0.5f, fxRnd(h.seed, 70 + k) - 0.5f, fxRnd(h.seed, 100 + k) - 0.5f};
        Vec3 d = normalize(normalize(r + Vec3{1e-3f, 0, 0}) + ax * 0.55f);
        float speed = 1.4f + 2.2f * fxRnd(h.seed, 130 + k);
        float dist = speed / 3.f * (1.f - std::exp(-3.f * t));
        float swirl = (k & 1 ? 1.f : -1.f) * (1.5f + 2.f * fxRnd(h.seed, 160 + k)) * t;
        Vec3 off = d * dist;
        float along = dot(off, ax);
        Vec3 radial = off - ax * along;
        Vec3 pos = h.pos + ax * along + Quat::axisAngle(ax, swirl).rotate(radial) + Vec3{0, 0.12f * t * t, 0};
        float fade = std::min(1.f, u * 12.f) * std::pow(1.f - u, 1.6f);
        col(0.25f * (1 - u), 0.55f * fade, c);
        a.gfx.drawSprite(pos, 0.12f + 0.5f * easeOut(u), c, eye, 0, swirl);
    }
    // 4. curling wisps: feathered ribbons that uncoil from the hit and dissolve tail-first
    for (int k = 0; k < 7; ++k) {
        float ang = k * 6.2832f / 7.f + fxRnd(h.seed, 200 + k);
        Vec3 r0 = s1 * std::cos(ang) + s2 * std::sin(ang);
        float sgn = (k & 1) ? 1.f : -1.f;
        float head = easeOut(t / 0.6f), tail = clampf((t - 0.25f) / 0.9f, 0.f, 1.f);
        if (tail >= head - 0.02f) continue;
        Vec3 pts[20];
        for (int i = 0; i < 20; ++i) {
            float sp = tail + (head - tail) * (i / 19.f);
            Vec3 radial = Quat::axisAngle(ax, sgn * sp * 2.6f).rotate(r0) * (0.15f + 0.85f * sp);
            pts[i] = h.pos + radial + ax * (0.55f * sp) + Vec3{0, 0.15f * sp * sp, 0};
        }
        float fade = clampf(1.f - t / 1.3f, 0.f, 1.f);
        col(0.4f * fade, 0.9f * fade, c);
        a.gfx.drawRibbon(pts, 20, 0.035f, c);
    }
    // 5. lingering sparks that drift up and twinkle out
    for (int k = 0; k < 24; ++k) {
        float life = 1.2f + 0.9f * fxRnd(h.seed, 300 + k);
        if (t > life) continue;
        float u = t / life;
        Vec3 v{fxRnd(h.seed, 330 + k) - 0.5f, fxRnd(h.seed, 360 + k) * 0.6f, fxRnd(h.seed, 390 + k) - 0.5f};
        v = v * 1.6f + ax * 0.5f;
        Vec3 pos = h.pos + v * (1.f - std::exp(-1.5f * t)) / 1.5f + Vec3{0, 0.25f * t, 0};
        float tw = 0.65f + 0.35f * std::sin(18.f * t + k * 1.7f);
        col(0.6f, 0.9f * (1 - u) * tw, c);
        a.gfx.drawSprite(pos, 0.035f, c, eye, 0);
    }
    // 6. you were hit: a brief soft wash of the scorer's colour in front of your eyes
    if (h.victim == local && !a.viewing && t < 0.6f) {
        float u = t / 0.6f;
        const Pose& hd = a.game.pl[local].in.head;
        col(0.2f, 0.35f * (1 - u) * (1 - u), c);
        a.gfx.drawSprite(hd.p + hd.q.rotate({0, 0, -0.25f}), 0.9f, c, hd.p, 0);
    }
}

// Spawn (PORT design, owner request): no rings. A soft glow gathers on the platform while the ball is hidden, then the
// ball floats up with its own tracers (see the trail update) and the original air-pressure sound (SpawnRise).
static void drawSpawnFx(App& a, int p) {
    const Game& g = a.game;
    const Ball& b = g.pl[p].ball;
    if (b.state != BallState::Spawning) return;
    const float team[2][3] = {{0.1f, 0.7f, 1.0f}, {1.0f, 0.4f, 0.1f}};
    const float* tc = team[p];
    const Vec3 base = g.tun.spawner[p] + Vec3{0, 0.035f, 0};
    float glow;
    if (b.pos.y < 0) glow = clampf((b.pos.y + 2.f * g.tun.spawnDepth) / g.tun.spawnDepth, 0.f, 1.f);   // gathering
    else glow = 1.f - clampf((b.pos.y - g.tun.spawnLauncherHeight) / std::max(0.05f, g.idlePoint(p).y - g.tun.spawnLauncherHeight), 0.f, 1.f);
    glow = glow * glow * (3 - 2 * glow);
    float c[4] = {tc[0] * 0.7f * glow, tc[1] * 0.7f * glow, tc[2] * 0.7f * glow, 1};
    a.gfx.drawSpriteFlat(base, 0.25f + 0.2f * glow, c, {0, 1, 0}, 0);
    float w[4] = {(tc[0] * 0.5f + 0.5f) * 0.5f * glow, (tc[1] * 0.5f + 0.5f) * 0.5f * glow, (tc[2] * 0.5f + 0.5f) * 0.5f * glow, 1};
    a.gfx.drawSpriteFlat(base + Vec3{0, 0.002f, 0}, 0.1f, w, {0, 1, 0}, 0);
}

static void drawFx(App& a) {
    const int L = a.net.localIndex();
    for (const App::HitFx& h : a.hits) drawHitFx(a, h, L);
    for (int p = 0; p < 2; ++p) drawSpawnFx(a, p);
}

// the host counts each match for the community server's statistics (StatCounter, game.h: throws, throws on target,
// hits and the points from returns, blocks, on-target balls faced, catches, the top throw speed); sent when a match
// between two people ends
static std::array<uint8_t, 4> finalScore(const Game& g) {   // what both players' games agree on (CONFIRM)
    auto c = [](int v) { return (uint8_t)std::max(0, std::min(255, v)); };
    return {c(g.pl[0].score), c(g.pl[1].score), c(g.pl[0].roundWins), c(g.pl[1].roundWins)};
}

static void countStat(App& a, const GameEvent& e) {
    Net::MatchStats* s = a.matchCount.s;
    switch (e.type) {
    case EventType::RoundStart:
        if ((int)e.value <= 1) a.matchCount = StatCounter{};
        break;
    case EventType::MatchEnd:
        a.liveStats[0] = s[0]; a.liveStats[1] = s[1];
        if (e.player >= 0 && e.player < 2 && !a.game.pl[1].isBot)
            a.net.reportMatch(e.player, s, finalScore(a.game).data(), a.game.tun.statsMode());
        a.matchCount = StatCounter{};
        return;
    default:
        a.matchCount.add(e);
        break;
    }
    a.liveStats[0] = s[0]; a.liveStats[1] = s[1];
}

static void handleEvents(App& a) {
    const int L = a.net.localIndex();
    for (GameEvent e : a.game.events) {
        bool mine = !a.viewing && e.player == L;
        if (a.net.mode() == Net::Mode::Host && a.net.online()) countStat(a, e);
        if (e.type == EventType::MatchEnd && a.net.mode() == Net::Mode::Client && a.net.online() && a.net.localSlot() >= 0 &&
            e.player >= 0 && e.player < 2)
            a.net.confirmMatch(e.player, finalScore(a.game).data());   // (the host's result counts once we agree)
        if (e.type != EventType::Bounce && e.type != EventType::Countdown)
            LOGI("event %s player %d hand %d value %.2f pos (%.2f %.2f %.2f) spin %.1f", eventName(e.type), e.player, e.hand, e.value,
                 e.pos.x, e.pos.y, e.pos.z, e.player >= 0 ? length(a.game.pl[e.player].ball.spin) : 0.f);
        switch (e.type) {
        // haptics: strong and long enough to feel through the Touch controllers' motors
        case EventType::Grab: if (mine && e.hand >= 0) haptic(a, e.hand, 0.85f, 0.12f); break;
        case EventType::Caught: if (mine && e.hand >= 0) haptic(a, e.hand, 1.0f, 0.18f); break;
        case EventType::ShieldBlock: if (mine && e.hand >= 0) haptic(a, e.hand, 1.0f, 0.28f); break;
        case EventType::ShieldBreak: if (mine && e.hand >= 0) haptic(a, e.hand, 1.0f, 0.35f); break;
        case EventType::KnuckleBlock: if (mine && e.hand >= 0) haptic(a, e.hand, 1.0f, 0.20f); break;
        case EventType::Throw: if (mine && e.hand >= 0) haptic(a, e.hand, 0.6f, 0.08f); break;
        case EventType::ThrowFail: if (mine && e.hand >= 0) haptic(a, e.hand, 0.4f, 0.06f); break;
        case EventType::Foul: a.foulFlash = 1.2f; a.foulAt = e.pos; break;
        case EventType::FistBump:
            a.bumpFlash = 1.0f;
            if (!a.net.localWaiting() && !a.spectating) { haptic(a, 0, 1, 0.35f); haptic(a, 1, 1, 0.35f); }
            break;
        case EventType::HitPlayer:
            a.hitFlash[e.player] = a.fxLevel == 0 ? 0.6f : a.fxLevel == 1 ? 0.3f : 0.1f;
            addHitFx(a, e.pos, e.player, (int)e.value);
            if (mine) { haptic(a, 0, 1, 0.5f); haptic(a, a.pointerHand, 1, 0.5f); }
            break;
        default: break;
        }
        if (e.player >= 0 && L == 1) e.player = 1 - e.player;   // audio keys use "player 0 = me"
        a.audio.onEvent(e, a.viewing ? a.viewHead.p : a.game.pl[L].in.head.p, L);
    }
}


// ------------------------------------------------------------------ avatars
static const float kTeamRGB[3][3] = {{0.1f, 0.7f, 1.0f}, {1.0f, 0.4f, 0.1f}, {0.85f, 0.85f, 0.9f}};   // 2 = outside the tank

struct AvatarLook { float ch[avatar::CH_COUNT][4]; float rim[4]; GLuint pattern; };

static AvatarLook lookOf(const App& a, const AvatarDesc& din, int team) {
    AvatarLook L;
    const AvatarDesc d = avatar::uniform(din);   // (a country: the team kit)
    const int t = team < 0 || team > 2 ? 2 : team;
    const float* tc = t < 2 ? a.sideRGB[t] : kTeamRGB[2];   // the end's colour (a country's, with NATIONS)
    avatar::channelColors(d, tc, L.ch);
    for (int k = 0; k < 3; ++k) L.rim[k] = tc[k];
    L.rim[3] = avatar::nationOf(d) ? 0.25f : 0.5f;   // the team-colour rim light that marks every player (like the
                                                     // original's); softer over a country's kit, which marks them already
    L.pattern = d.pattern < 8 ? a.patternTex[d.pattern] : 0;
    return L;
}

// NATIONS: each end's country and colour from the players' avatars (as drawn), and the arena's team lights with them
static void updateSides(App& a) {
    const int L = a.net.localIndex();
    for (int p = 0; p < 2; ++p) {
        const AvatarDesc& d = (p == L && !a.viewing) ? a.myAvatar : a.viewIn[p].avatar;
        a.sideNation[p] = a.net.slotFilled(p) || a.spectating || a.viewing ? avatar::nationOf(d) : 0;
    }
    avatar::sideColors(a.sideNation[0], a.sideNation[1], a.sideRGB);
    a.nationsOn = a.sideNation[0] || a.sideNation[1];
    float lit[2][3];   // The Crowd: an end's lights flare with its fans' cheers
    for (int p = 0; p < 2; ++p)
        for (int k = 0; k < 3; ++k) lit[p][k] = a.sideRGB[p][k] * (1.f + 0.9f * a.crowdFlare[p]);
    a.gfx.setSideColors(a.nationsOn || a.crowdFlare[0] > 0.01f || a.crowdFlare[1] > 0.01f, lit[0], lit[1]);
}

// the two players' careers from the community server: the room we are in, or the match we watch (every 5 s)
static void updateStats(App& a) {
    if (!a.statsNoticeSeen && !a.statsNoticeOpen && a.net.online() && a.net.inRoom() && !a.menuOpen && !a.viewing) {
        // the one-time notice about statistics: the first time you are in an online arena room
        a.statsNoticeOpen = true;
        a.menuOpen = true;
        a.titleOpen = a.settingsOpen = a.statsOpen = a.reportOpen = a.bugOpen = a.helpOpen = a.rulesOpen = false;
        a.menuPose = {a.viewHead.q, a.viewHead.transform({0, -0.05f, -0.95f})};
    }
    const uint32_t hv = a.watchIp ? a.watchIp : a.net.roomHostVip();   // (watchIp: spectating a match)
    const double t = nowSeconds();
    if (hv != a.statsHost) { a.statsHost = hv; a.careersOk = false; a.statsAskAt = 0; }
    if (hv && a.net.onlineReady() && t > a.statsAskAt) { a.statsAskAt = t + 5.0; a.net.requestStats(hv); }
    if (hv && a.net.statsFor(hv, a.careers)) a.careersOk = true;
}

// one player's career as display lines ("62%" etc.); n = how many lines
static int careerLines(const App& a, int p, char out[5][48]) {
    const Net::CareerStats& c = a.careers[p];
    if (!a.careersOk || !c.known) { std::snprintf(out[0], 48, "NO STATS YET"); return 1; }
    if (!c.matches) { std::snprintf(out[0], 48, "FIRST ONLINE MATCH"); return 1; }
    std::snprintf(out[0], 48, "%u WINS  /  %u MATCHES", c.wins, c.matches);
    std::snprintf(out[1], 48, "THROW ACCURACY %d%%", c.throws ? (int)std::lround(100.0 * c.onTarget / c.throws) : 0);
    // (each line about as wide as the first: they share a column on the jumbotron)
    std::snprintf(out[2], 48, "BLOCKS %d%%  -  %u CATCHES", c.faced ? (int)std::lround(100.0 * c.blocks / c.faced) : 0, c.catches);
    std::snprintf(out[3], 48, "TOP SPEED %d KM/H", (int)std::lround(c.topSpeed * 3.6f));
    std::snprintf(out[4], 48, "%u HITS (%u FROM RETURNS)", c.hits, c.returnHits);
    return 5;
}

static void drawFlag(App& a, int nation, const Pose& at, float w, const float* rgba = nullptr);
static const char* sideName(const App& a, int p);

// a tank player's name on banners: the room's name for them, their career's, or the end's (BLUE, a country)
static std::string playerLabel(const App& a, int p) {
    if (p == 1 && a.game.pl[1].isBot) return "BOT";
    const RoomInfo& rm = a.watchIp != 0 ? a.watchRoom : a.net.room();
    if (const char* n = rm.nameOf(rm.slot[p])) return n;
    if (a.careersOk && a.careers[p].name[0]) return a.careers[p].name;
    return sideName(a, p);
}

// The Crowd's web app (fans follow online matches on their phones)
static const char* kCrowdSite = "CROWD.AFTERSPARC.COM";
static const CrowdInfo* crowdNow(const App& a) { return a.testCrowdOn ? &a.testCrowd : a.net.crowd(a.watchIp); }

// the screen block over centre court (four faces, 1.705 m out, 8 m up): each player's name, flag or team colour,
// the score and their career stats; the blue end's player on the side nearer the blue end
static void drawJumbotron(App& a) {
    const Game& g = a.game;
    const float white[4] = {1, 1, 1, 1}, grey[4] = {0.70f, 0.74f, 0.80f, 1}, board[4] = {0.004f, 0.008f, 0.02f, 0.92f};
    char lines[2][5][48];
    const int nl[2] = {careerLines(a, 0, lines[0]), careerLines(a, 1, lines[1])};
    const std::string names[2] = {playerLabel(a, 0), playerLabel(a, 1)};
    const CrowdInfo* crowd = crowdNow(a);
    for (int f = 0; f < 4; ++f) {
        const Quat q = Quat::axisAngle({0, 1, 0}, f * kPi * 0.5f);
        const Vec3 n = q.rotate({0, 0, 1}), c = Vec3{0, 8.f, 0} + n * 1.712f;
        const bool flip = q.rotate({1, 0, 0}).x < -0.5f;   // (this face's left is the orange end)
        auto at = [&](float u, float v, float w = 0.f) { return Pose{q, c + q.rotate({u, v, w})}; };
        a.gfx.drawPanel(Mat4::fromPose(at(0, 0), {3.15f, 1.55f, 1}), board);
        char buf[48];
        a.gfx.drawText("-", at(0, 0.42f, 0.004f), 0.22f, grey);
        for (int s = 0; s < 2; ++s) {
            const int p = flip ? 1 - s : s;
            const float u = s ? 0.98f : -0.98f;
            std::snprintf(buf, sizeof buf, "%d", g.pl[p].score);
            a.gfx.drawText(buf, at(s ? 0.30f : -0.30f, 0.42f, 0.004f), 0.30f, white);
            const float col[4] = {0.65f * a.sideRGB[p][0] + 0.35f, 0.65f * a.sideRGB[p][1] + 0.35f, 0.65f * a.sideRGB[p][2] + 0.35f, 1};
            a.gfx.drawText(names[p], at(u, 0.58f, 0.004f), names[p].size() > 10 ? 0.10f : 0.13f, col);
            if (a.sideNation[p]) drawFlag(a, a.sideNation[p], at(u, 0.30f, 0.003f), 0.42f);
            else {
                const float bar[4] = {a.sideRGB[p][0], a.sideRGB[p][1], a.sideRGB[p][2], 0.95f};
                a.gfx.drawPanel(Mat4::fromPose(at(u, 0.33f, 0.002f), {0.62f, 0.05f, 1}), bar);
            }
            for (int k = 0; k < nl[p]; ++k) {   // (long lines shrink to their 1 m column: a character is ~0.72 x the size)
                const float sz = std::min(0.070f, 1.0f / (0.72f * std::max<size_t>(1, std::strlen(lines[p][k]))));
                a.gfx.drawText(lines[p][k], at(u, 0.05f - 0.15f * k, 0.004f), sz, k ? grey : white);
            }
        }
        // The Crowd, in the middle: how many fans follow on their phones and each end's hype; or where to follow
        if (crowd && crowd->fans > 0) {
            std::snprintf(buf, sizeof buf, "%d", crowd->fans);
            a.gfx.drawText(buf, at(0, 0.12f, 0.004f), 0.13f, white);
            a.gfx.drawText(crowd->fans == 1 ? "FAN" : "FANS", at(0, -0.02f, 0.004f), 0.06f, grey);
            for (int s = 0; s < 2; ++s) {
                const int p = flip ? 1 - s : s;
                const float h = 0.40f * std::max(0.04f, crowd->hype[p] / 100.f);
                const float bar[4] = {a.sideRGB[p][0], a.sideRGB[p][1], a.sideRGB[p][2], 0.95f};
                a.gfx.drawPanel(Mat4::fromPose(at(s ? 0.08f : -0.08f, -0.58f + 0.5f * h, 0.003f), {0.08f, h, 1}), bar);
            }
            a.gfx.drawText("HYPE", at(0, -0.66f, 0.004f), 0.05f, grey);
        } else if (a.net.online() || a.testCrowdOn) {
            a.gfx.drawText("FOLLOW LIVE ON YOUR PHONE", at(0, -0.60f, 0.004f), 0.045f, grey);
            a.gfx.drawText(kCrowdSite, at(0, -0.68f, 0.004f), 0.055f, white);
        }
    }
}

// The Crowd (0.3.7): an online host tells the community server about its match every second (Net::setLive); fans
// on phones cheer for an end and wave its flag; everyone in the arena (and its spectators) hears them: a crowd sound
// that swells with the hype, a cheer when an end's hype passes 60, the end's lights flaring with each burst of
// cheers, and the jumbotron's fan count and hype bars.
static void burstCrowdFx(App& a, int side, int count);
static void stepCrowdFx(App& a, float dt);
static void updateCrowd(App& a, float dt) {
    if (a.net.mode() == Net::Mode::Host && a.net.onlineReady()) {
        const Game& g = a.game;
        Net::LiveInfo li;
        li.state = (uint8_t)g.match;
        const auto sc = finalScore(g);
        std::memcpy(li.score, sc.data(), 4);
        for (int p = 0; p < 2; ++p) {
            li.nation[p] = (uint8_t)a.sideNation[p];
            for (int k = 0; k < 3; ++k) li.rgb[p][k] = (uint8_t)std::lround(clampf(a.sideRGB[p][k], 0.f, 1.f) * 255.f);
            const std::string n = a.net.slotFilled(p) ? playerLabel(a, p) : std::string("OPEN");
            std::snprintf(li.name[p], sizeof li.name[p], "%s", n.c_str());
        }
        li.flags = (g.tun.advanced ? 1 : 0) | (g.pl[1].isBot ? 2 : 0) | (!g.tun.shields ? 4 : 0) | (g.tun.freePlay() ? 8 : 0) |
                   (g.tun.knuckleGuards() ? 16 : 0) | (g.tun.advanced && g.tun.advancedRoundTime > 0 ? 32 : 0);
        li.clock = g.roundClock;
        li.st[0] = a.liveStats[0]; li.st[1] = a.liveStats[1];
        li.round = (uint8_t)std::max(0, std::min(255, g.round));
        li.overtime = g.overtime ? 1 : 0;
        a.net.setLive(li);
    }
    const CrowdInfo* c = crowdNow(a);
    for (int p = 0; p < 2; ++p) a.crowdFlare[p] = std::max(0.f, a.crowdFlare[p] - dt * 1.2f);
    const bool sound = a.crowdMode == 0, show = a.crowdMode < 2;   // SETTINGS > CROWD
    if (!show) { a.crowdFlare[0] = a.crowdFlare[1] = 0.f; a.crowdFx.clear(); }
    stepCrowdFx(a, dt);
    if (c && c->at != a.crowdAt) {   // a new update (twice a second)
        a.crowdAt = c->at;
        for (int p = 0; p < 2; ++p) {
            if (c->cheers[p] > 0 && show) {
                a.crowdFlare[p] = std::max(a.crowdFlare[p], std::min(1.f, 0.35f + c->cheers[p] / (4.f + 2.f * c->fans)));
                burstCrowdFx(a, p, std::min(40, 2 * c->cheers[p]));   // rapid cheering pours more out
            }
            if (c->hype[p] >= 60 && !a.crowdCheered[p]) { a.crowdCheered[p] = true; if (sound) a.audio.play("oa_Cheer", 0.8f); }
            if (c->hype[p] < 35) a.crowdCheered[p] = false;
        }
    }
    const float hype = c ? std::max(c->hype[0], c->hype[1]) / 100.f : 0.f;
    const float target = c && c->fans > 0 && sound ? 0.12f + 0.6f * hype : 0.f;
    a.crowdLevel += (target - a.crowdLevel) * std::min(1.f, dt * 2.f);
    if (target == 0.f && a.crowdLevel < 0.003f) a.crowdLevel = 0.f;
    a.audio.setLoop("oa_Crowd", a.crowdLevel);
}

// a country's flag (w x h metres, centred on the pose, facing its +Z); nothing for no country
static void drawFlag(App& a, int nation, const Pose& at, float w, const float* rgba) {
    const avatar::Nation* N = avatar::nation(nation);
    if (!N) return;
    const float white[4] = {1, 1, 1, 1};
    a.gfx.drawTexturedPanel(Mat4::fromPose(at, {w, w / 1.5f, 1}), a.gfx.texture(N->flag, true), rgba ? rgba : white, false);
}

// the name a side goes by on banners: the country (UNITED STATES) or the end's colour
static const char* sideName(const App& a, int p) {
    if (const avatar::Nation* N = avatar::nation(a.sideNation[p])) return N->name;
    return a.sideRGB[p][2] > a.sideRGB[p][0] ? "BLUE" : "ORANGE";   // (the colour it shows: next to a country it can swap)
}

static void applyLook(DrawParams& dp, const AvatarLook& L) {
    dp.channels = L.ch;
    std::memcpy(dp.rim, L.rim, sizeof dp.rim);
    dp.pattern = L.pattern;
}

// Hair that moves: each chain hangs from its first point, which moves with the head; every further point is pulled
// toward its rest place (the rest shape turns fully with the head's heading but only `follow` of its tilt: 1 = gelled
// hair, lower = it hangs), with springs and damping, so it lags, swings and settles. Lengths are kept; no point swings
// more than its chain's limit from rest, or into the head (a sphere round the rig's centre). out[0] = identity (the
// head); each chain segment's bone maps head space at rest to head space now (hair is drawn with the head's matrix).
static void swingBones(App& a, const App::SwingRig& rig, int key, const Pose& head, Mat4 out[7], double now) {
    App::SwingState& s = a.swings[int64_t(key) * 1024 + rig.id];
    const int nc = (int)rig.chains.size();
    bool reset = !s.init || now - s.t > 0.5;   // first sight, a long pause or a warp: at rest
    for (int c = 0; c < nc && !reset; ++c) reset = length(s.X[c][0] - head.transform(rig.chains[c].R[0])) > 0.6f;
    if (reset) {
        for (int c = 0; c < nc; ++c)
            for (int j = 0; j < rig.chains[c].n; ++j) { s.X[c][j] = head.transform(rig.chains[c].R[j]); s.V[c][j] = {0, 0, 0}; }
        s.init = true;
        s.t = now;
    }
    const float dt = (float)std::min(0.05, now - s.t);
    if (dt > 0.0005f) {
        s.t = now;
        Vec3 f = head.q.rotate({0, 0, -1});
        f.y = 0;
        const Quat yawQ = length(f) > 1e-4f ? Quat::axisAngle({0, 1, 0}, std::atan2(-f.x, -f.z)) : head.q;
        const Vec3 Cw = head.transform(rig.centre);
        const int sub = 3;
        const float h = dt / sub;
        for (int c = 0; c < nc; ++c) {
            const App::SwingChain& ch = rig.chains[c];
            const Quat qt = nlerp(yawQ, head.q, ch.follow);
            const float sinL = std::sqrt(std::max(0.f, 1.f - ch.limitCos * ch.limitCos));
            Vec3* X = s.X[c];
            Vec3* V = s.V[c];
            for (int it = 0; it < sub; ++it) {
                X[0] = head.transform(ch.R[0]);
                for (int j = 1; j < ch.n; ++j) {
                    const Vec3 rest = qt.rotate(ch.R[j] - ch.R[j - 1]);
                    const float L = length(ch.R[j] - ch.R[j - 1]);
                    const Vec3 rd = rest / std::max(L, 1e-5f);
                    const Vec3 prev = X[j];
                    V[j] += ((X[j - 1] + rest - X[j]) * ch.k - V[j] * ch.c) * h;
                    X[j] += V[j] * h;
                    const Vec3 o = X[j] - Cw;   // not into the head
                    const float ol = length(o);
                    if (ol < ch.rmin[j] && ol > 1e-5f) X[j] = Cw + o * (ch.rmin[j] / ol);
                    Vec3 d = X[j] - X[j - 1];
                    const float dl = length(d);
                    d = dl > 1e-5f ? d / dl : rd;
                    if (dot(d, rd) < ch.limitCos) {   // past the limit: back onto its cone
                        Vec3 perp = d - rd * dot(d, rd);
                        perp = length(perp) > 1e-5f ? normalize(perp) : Vec3{0, -1, 0};
                        d = rd * ch.limitCos + perp * sinL;
                    }
                    X[j] = X[j - 1] + d * L;
                    V[j] = (X[j] - prev) / h;
                }
            }
        }
    }
    const Quat hinv = head.q.conj();
    for (int b = 0; b < 7; ++b) out[b] = Mat4::identity();
    for (int c = 0; c < nc; ++c) {
        const App::SwingChain& ch = rig.chains[c];
        for (int j = 0; j + 1 < ch.n; ++j) {
            const Vec3 from = normalize(ch.R[j + 1] - ch.R[j]);                       // head space, at rest
            const Vec3 to = normalize(hinv.rotate(s.X[c][j + 1] - s.X[c][j]));         // head space, now
            const Quat q = fromTo(from, to);
            const Vec3 at = hinv.rotate(s.X[c][j] - head.p);                            // the segment's root now
            out[ch.bone0 + j] = Mat4::fromPose({q, at - q.rotate(ch.R[j])});
        }
    }
}

// Hair under this avatar's headgear and face shield: their tuck maps combined (the nearer surface wins), as a texture
// made the first time the pair is drawn
static void hairTuck(App& a, const AvatarDesc& d, DrawParams& dp) {
    const int b = avatar::bodyType(d), g = d.headgear < 8 ? d.headgear : 0, v = d.visor < 8 ? d.visor : 0;
    const App::Tuck* G = g && a.tuckGear[b][g].ok ? &a.tuckGear[b][g] : nullptr;
    const App::Tuck* V = v && a.tuckVisor[b][v].ok ? &a.tuckVisor[b][v] : nullptr;
    if (G && V && (G->w != V->w || G->h != V->h)) V = nullptr;
    if (!G && !V) return;
    const App::Tuck& T = G ? *G : *V;
    const int key = b * 100 + g * 10 + v;
    auto it = a.tuckTex.find(key);
    if (it == a.tuckTex.end()) {
        std::vector<uint8_t> m = T.v;
        if (G && V) for (size_t i = 0; i < m.size(); ++i) m[i] = std::min(m[i], V->v[i]);
        it = a.tuckTex.emplace(key, a.gfx.tuckTexture(T.w, T.h, m.data())).first;
    }
    dp.tuck = it->second;
    std::memcpy(dp.tuckC, T.c, sizeof dp.tuckC);
}

// A ponytail under a helmet that covers its tie comes out under the helmet's rim instead: the offset (head space) that
// moves its root straight down the back of the head to just inside the rim (zero when nothing covers the root).
static Vec3 tailExit(const App& a, const AvatarDesc& d, const App::SwingRig& rig) {
    const int b = avatar::bodyType(d), g = d.headgear < 8 ? d.headgear : 0;
    if (!g || !a.tuckGear[b][g].ok || rig.chains.empty()) return {0, 0, 0};
    const App::Tuck& T = a.tuckGear[b][g];
    const Vec3 C{T.c[0], T.c[1], T.c[2]};
    auto radius = [&](float az, float el) {   // nearest cell
        const int i = ((int)std::floor((az + kPi) / (2 * kPi) * T.w) % T.w + T.w) % T.w;
        const int j = std::max(0, std::min(T.h - 1, (int)std::floor((el + kPi / 2) / kPi * T.h)));
        return 0.05f + 0.5f * T.v[(size_t)j * T.w + i] / 255.f;
    };
    const Vec3 root = rig.chains[0].R[0], dr = root - C;
    const float r = length(dr);
    if (r < 1e-4f) return {0, 0, 0};
    const float az = std::atan2(dr.x, dr.z);
    float el = std::asin(std::max(-1.f, std::min(1.f, dr.y / r)));
    if (radius(az, el) > 0.45f) return {0, 0, 0};
    const float step = kPi / T.h * 0.5f;
    float rim = radius(az, el);
    while (el > -1.4f && radius(az, el - step) <= 0.45f) { el -= step; rim = radius(az, el); }
    const Vec3 dir{std::cos(el) * std::sin(az), std::sin(el), std::cos(el) * std::cos(az)};
    return C + dir * (rim - 0.012f) - root;
}

// selftests: the hair of the avatar drawn as `key` stepped through a head turn (`deg` degrees in `turn` s, then `hold`
// s still), so the picture shows it mid-swing
static void preSwing(App& a, int key, const PlayerInput& in, float deg, float turn, float hold) {
    avatar::Rig rig = avatar::solveRig(in, in.avatar);
    std::vector<avatar::PartDraw> list;
    avatar::drawList(rig, in, in.avatar, list, false);
    Mat4 bones[7];
    const int steps = (int)std::lround((turn + hold) * 100.f);
    for (const avatar::PartDraw& p : list) {
        auto rg = (p.flags & avatar::PART_HAIR) ? a.swingRigs.find(p.model) : a.swingRigs.end();
        if (rg == a.swingRigs.end()) continue;
        for (int f = 0; f <= steps; ++f) {
            const float t = f / 100.f, k = std::min(1.f, t / turn);
            Pose h = in.head;
            h.q = Quat::axisAngle({0, 1, 0}, deg2rad(deg * (1.f - k))) * in.head.q;
            swingBones(a, rg->second, key, h, bones, 1000.0 + t);
        }
        a.swings[int64_t(key) * 1024 + rg->second.id].t = nowSeconds();   // (the picture then draws this state)
    }
}

// Body (torso, neck, head and its parts, arms) for one player; `pre` places it (miniatures, the builder mirror),
// preMirror when `pre` has a negative determinant. Hands are drawn by the caller.
static void drawAvatarBody(App& a, const PlayerInput& in, int team, int pass, const Mat4& pre, bool preMirror, bool lod,
                           float flash, int swingKey = -1) {
    avatar::Rig rig = avatar::solveRig(in, in.avatar);
    if (pass == 1 && !lod && avatar::nationOf(in.avatar)) {
        // NATIONS: the country's flag high on the jersey's back, just under the collar, leaning with the upper back
        // (bind pose; owner, 2026-10-29: on the number panel it sat at the waist)
        const bool fem = avatar::bodyType(in.avatar) == 1;
        const Pose at{Quat::axisAngle({1, 0, 0}, deg2rad(15.f)), fem ? Vec3{0, -0.11f, 0.090f} : Vec3{0, -0.12f, 0.125f}};
        const avatar::Nation* N = avatar::nation(avatar::nationOf(in.avatar));
        const float white[4] = {1, 1, 1, 1};
        a.gfx.drawTexturedPanel(pre * rig.skin[avatar::BONE_TORSO] * Mat4::fromPose(at, {0.13f, 0.13f / 1.5f, 1}),
                                a.gfx.texture(N->flag, true), white, false);
        // and a small emblem on the left chest, facing forward (-Z), leaning with the chest (measured on av_top_1)
        const Pose crest{Quat::axisAngle({0, 1, 0}, kPi) * Quat::axisAngle({1, 0, 0}, -deg2rad(fem ? 45.f : 18.f)),
                         fem ? Vec3{-0.08f, -0.118f, -0.147f} : Vec3{-0.08f, -0.150f, -0.131f}};
        a.gfx.drawTexturedPanel(pre * rig.skin[avatar::BONE_TORSO] * Mat4::fromPose(crest, {0.055f, 0.055f / 1.5f, 1}),
                                a.gfx.texture(N->flag, true), white, false);
    }
    static std::vector<avatar::PartDraw> list;
    list.clear();
    avatar::drawList(rig, in, in.avatar, list, lod);
    const AvatarLook L = lookOf(a, in.avatar, team);
    for (const avatar::PartDraw& p : list) {
        auto it = a.avModels.find(p.model);
        if (it == a.avModels.end()) {   // a face's own part missing (e.g. a face shield not fitted yet): the base head's
            std::string base = p.model;
            if (base.size() > 2 && base.back() >= '1' && base.back() <= '9' && (base[base.size() - 2] == 'm' || base[base.size() - 2] == 'f'))
                it = a.avModels.find(base.substr(0, base.size() - 1));
            if (it == a.avModels.end()) continue;
        }
        DrawParams dp;
        applyLook(dp, L);
        dp.mirrored = p.mirrored != preMirror;
        if (flash > 0) { dp.emissiveAdd[0] = flash; dp.emissiveAdd[1] = flash * 0.3f; }
        if (p.flags & avatar::PART_NEON)
            for (int k = 0; k < 3; ++k) dp.emissiveAdd[k] += L.ch[avatar::CH_GLOW][k] * 0.35f;
        if (p.flags & avatar::PART_SKINNED) dp.bones = rig.skin;
        if (p.flags & avatar::PART_PULSE) dp.uvOff[1] = -(float)std::fmod(nowSeconds() - a.startTime, 1000.0) * 1.6f;   // ~40 cm/s
        Mat4 swing[avatar::BONE_COUNT], model = pre * p.m;   // (hair: its own bones, the first 7; the renderer reads them all)
        for (Mat4& b : swing) b = Mat4::identity();
        if (p.flags & avatar::PART_HAIR) {   // hair: a style that moves swings (its chains per drawn avatar); under headgear, tucked in
            auto rg = a.swingRigs.find(it->first);
            Vec3 off{0, 0, 0};
            if (rg != a.swingRigs.end()) {
                const App::SwingRig* rig = &rg->second;
                if (rig->exits) off = tailExit(a, in.avatar, *rig);
                const bool moved = length(off) > 1e-4f;
                if (moved) {   // out under the helmet's rim: the rig moved there (made once per helmet)
                    char k[48];
                    std::snprintf(k, sizeof k, "%s@%d", it->first.c_str(), (int)in.avatar.headgear);
                    auto sr = a.swingRigs.find(k);
                    if (sr == a.swingRigs.end()) {
                        App::SwingRig m = *rig;
                        m.exits = false;
                        for (App::SwingChain& c : m.chains)
                            for (int j = 0; j < c.n; ++j) { c.R[j] += off; c.rmin[j] = 0.97f * length(c.R[j] - m.centre); }
                        m.id = (int)a.swingRigs.size();
                        sr = a.swingRigs.emplace(k, m).first;
                    }
                    rig = &sr->second;
                }
                swingBones(a, *rig, swingKey >= 0 ? swingKey : team, in.head, swing, nowSeconds());
                if (moved) {   // its bones work on the moved rest pose: the part is drawn moved, its bones in its own frame
                    const Mat4 to = Mat4::fromPose({Quat{}, off}), back = Mat4::fromPose({Quat{}, -off});
                    for (Mat4& b : swing) b = back * b * to;
                    model = model * to;
                }
                dp.bones = swing;
            }
            hairTuck(a, in.avatar, dp);
            dp.tuckC[0] -= off.x; dp.tuckC[1] -= off.y; dp.tuckC[2] -= off.z;   // (the tuck map is in head space)
        }
        a.gfx.draw(it->second, model, dp, pass);
    }
}

// reflection through the plane at P with unit normal n
static Mat4 reflectMat(const Vec3& P, const Vec3& n) {
    Mat4 r = Mat4::identity();
    const float nv[3] = {n.x, n.y, n.z};
    for (int c = 0; c < 3; ++c)
        for (int rr = 0; rr < 3; ++rr) r.m[c * 4 + rr] = (c == rr ? 1.f : 0.f) - 2.f * nv[rr] * nv[c];
    const float d = 2.f * dot(P, n);
    r.m[12] = d * n.x; r.m[13] = d * n.y; r.m[14] = d * n.z;
    return r;
}

static void saveAvatar(App& a) {
    const char* ext = dataDir(a);
    if (!ext) return;
    std::string f = std::string(ext) + "/avatar.cfg";
    if (FILE* fp = fopen(f.c_str(), "w")) {
        std::string t = avatar::toText(a.myAvatar);
        fputs(t.c_str(), fp);
        fclose(fp);
        LOGI("avatar: saved %s", f.c_str());
    }
}

// <externalDataPath>/avatar.cfg (key=value lines, editable over adb); a first run picks a random look
static void loadAvatar(App& a) {
    uint32_t seed = (uint32_t)(nowSeconds() * 1000.0);
    for (const char* c = a.net.name(); *c; ++c) seed = seed * 31u + (uint8_t)*c;
    a.botAvatar = avatar::randomLook(seed ^ 0xB07B07u);
    const char* ext = dataDir(a);
    bool ok = false;
    if (ext) {
        std::string f = std::string(ext) + "/avatar.cfg";
        if (FILE* fp = fopen(f.c_str(), "r")) {
            std::string t;
            char buf[256];
            while (fgets(buf, sizeof buf, fp)) t += buf;
            fclose(fp);
            ok = avatar::fromText(t, a.myAvatar);
        }
    }
    if (!ok) { a.myAvatar = avatar::randomLook(seed); saveAvatar(a); }
    LOGI("avatar: %s / %s", avatar::optionName(avatar::F_SUIT, a.myAvatar.suit), avatar::optionName(avatar::F_HEADGEAR, a.myAvatar.headgear));
}

// ---- builder panel layout (panel space: metres, +Z toward the player)
static const float kBRow0 = 0.36f, kBRowH = 0.046f, kBPrev = -0.03f, kBNext = 0.33f, kBValue = 0.15f, kBLabel = -0.25f;
static float builderPresetY() { return kBRow0 - avatar::kRowCount * kBRowH - 0.014f; }
static float builderCountryY() { return builderPresetY() - kBRowH; }   // PRESET > COUNTRY FLAG: the country under it
static float builderButtonsY(const App& a) { return (avatar::nationOf(a.myAvatar) ? builderCountryY() : builderPresetY()) - 0.08f; }
// the PRESET row: the presets, then COUNTRY FLAG (the team kit of a country, chosen in the row below)
static int presetSlots() { return avatar::presetCount() + 1; }
static int presetSlot(const App& a) { return ((a.presetIdx % presetSlots()) + presetSlots()) % presetSlots(); }
static const char* presetLabel(const App& a) {
    return presetSlot(a) == avatar::presetCount() ? "COUNTRY FLAG" : avatar::presetName(presetSlot(a));
}

// hit codes: field * 2 (+1 = next), 100 / 101 preset prev / next, 102 random, 103 save and apply, 104 / 105 country
// prev / next, 106 cancel
static int builderHitTest(const App& a, float lx, float ly) {
    for (int f = 0; f < avatar::kRowCount; ++f) {
        float y = kBRow0 - f * kBRowH;
        if (std::fabs(ly - y) < kBRowH * 0.46f) {
            if (std::fabs(lx - kBPrev) < 0.03f) return f * 2;
            if (std::fabs(lx - kBNext) < 0.03f) return f * 2 + 1;
        }
    }
    float yp = builderPresetY();
    if (std::fabs(ly - yp) < kBRowH * 0.46f) {
        if (std::fabs(lx - kBPrev) < 0.03f) return 100;
        if (std::fabs(lx - kBNext) < 0.03f) return 101;
    }
    if (avatar::nationOf(a.myAvatar) && std::fabs(ly - builderCountryY()) < kBRowH * 0.46f) {
        if (std::fabs(lx - kBPrev) < 0.03f) return 104;
        if (std::fabs(lx - kBNext) < 0.03f) return 105;
    }
    float yb = builderButtonsY(a);
    if (std::fabs(ly - yb) < 0.032f) {
        if (std::fabs(lx + 0.24f) < 0.105f) return 102;
        if (std::fabs(lx) < 0.105f) return 106;
        if (std::fabs(lx - 0.24f) < 0.12f) return 103;
    }
    return -1;
}

static void openBuilder(App& a, const PlayerInput& in) {
    Vec3 f = in.head.q.rotate({0, 0, -1});
    f.y = 0;
    f = length(f) > 1e-3f ? normalize(f) : Vec3{1, 0, 0};
    const Vec3 right = cross(f, Vec3{0, 1, 0});
    a.mirrorNormal = -f;
    a.mirrorPoint = Vec3{in.head.p.x, 0, in.head.p.z} + f * 0.85f;
    Vec3 pp = in.head.p + f * 0.95f + right * 0.66f + Vec3{0, -0.30f, 0};
    Vec3 to = in.head.p - pp;
    to.y = 0;
    to = normalize(to);
    a.builderPose = {Quat::axisAngle({0, 1, 0}, std::atan2(to.x, to.z)), pp};
    a.builderOpen = true;
    a.builderStart = a.myAvatar;
    a.menuOpen = false;
    a.prevBuilderTrig = true;
    a.presetIdx = avatar::nationOf(a.myAvatar) ? avatar::presetCount() : 0;   // (wearing a country: COUNTRY FLAG)
    LOGI("avatar builder: open");
}

static void closeBuilder(App& a) {
    if (!a.builderOpen) return;
    a.builderOpen = false;
    saveAvatar(a);
    LOGI("avatar builder: closed");
}

static void builderActivate(App& a, int hit) {
    AvatarDesc& d = a.myAvatar;
    if (hit >= 0 && hit < 100) {
        avatar::Field f = avatar::rowField(hit / 2);
        avatar::set(d, f, avatar::get(d, f) + ((hit & 1) ? 1 : -1));
        LOGI("avatar: %s -> %s", avatar::fieldName(f), avatar::optionName(f, avatar::get(d, f)));
    } else if (hit == 100 || hit == 101) {
        a.presetIdx = presetSlot(a) + (hit == 101 ? 1 : -1);
        if (presetSlot(a) == avatar::presetCount()) {   // COUNTRY FLAG: the look stays, the country's kit goes on
            avatar::set(d, avatar::F_NATION, std::max(1, std::min(avatar::nationCount(), a.lastNation)));
        } else {
            d = avatar::preset(presetSlot(a));
        }
        LOGI("avatar: preset %s", presetLabel(a));
    } else if (hit == 104 || hit == 105) {   // the country (1..count, wrapping)
        const int n = avatar::nationCount();
        const int v = (avatar::nationOf(d) - 1 + (hit == 105 ? 1 : n - 1)) % n + 1;
        avatar::setNation(d, v);
        a.lastNation = v;
        LOGI("avatar: country %s", avatar::nation(v)->name);
    } else if (hit == 102) {
        d = avatar::randomLook((uint32_t)(nowSeconds() * 1000.0));
    } else if (hit == 103) {   // SAVE AND APPLY (a tester's report: the look shows live, but DONE didn't say it saves)
        closeBuilder(a);
        a.net.noticeLocal("LOOK SAVED");
    } else if (hit == 106) {   // CANCEL: back to the look from before
        a.myAvatar = a.builderStart;
        closeBuilder(a);
        a.net.noticeLocal("CHANGES CANCELLED");
    }
}

static void updateBuilder(App& a, PlayerInput& in) {
    a.builderHover = -1;
    a.builderHitValid = false;
    if (!a.builderOpen) return;
    const bool trig = in.hand[a.pointerHand].trigger > 0.7f;
    if (a.aimValid[a.pointerHand]) {
        Vec3 o = a.aim[a.pointerHand].p, d = a.aim[a.pointerHand].q.rotate({0, 0, -1});
        Vec3 nz = a.builderPose.q.rotate({0, 0, 1});
        float den = dot(d, nz);
        if (den < -1e-3f) {
            float t = dot(a.builderPose.p - o, nz) / den;
            if (t > 0 && t < 4) {
                Vec3 p = o + d * t;
                Vec3 lp = a.builderPose.q.conj().rotate(p - a.builderPose.p);
                if (std::fabs(lp.x) < 0.44f && lp.y < 0.52f && lp.y > builderButtonsY(a) - 0.06f) { a.builderHit = p; a.builderHitValid = true; }
                a.builderHover = builderHitTest(a, lp.x, lp.y);
                if (trig && !a.prevBuilderTrig && a.builderHover >= 0) {
                    builderActivate(a, a.builderHover);
                    haptic(a, a.pointerHand, 0.35f, 0.025f);
                }
            }
        }
    }
    a.prevBuilderTrig = trig;
    for (int h = 0; h < 2; ++h) { in.hand[h].trigger = 0; in.hand[h].squeeze = 0; }   // no grabbing while customizing
}

// the player's GAUNTLET style (the classic controller-in-glove when the style is missing)
static const Model& gauntletOf(const App& a, const AvatarDesc& d) {
    const int k = avatar::gauntletOf(d);
    return k < 3 && a.gauntlets[k].valid() ? a.gauntlets[k] : a.lancer;
}

// a gauntlet: gm = (pre *) grip * the hand's mirror. Plate gauntlets are drawn as the hand and the cuff, the cuff in
// its tuned pose against the hand (avatar::cuffPose; identity unless the TUNING tool moved it)
static void drawGauntlet(App& a, const AvatarDesc& d, const Mat4& gm, const DrawParams& dp, int pass) {
    const int k = avatar::gauntletOf(d);
    if (k < 3 && a.gauntletHand[k].valid() && a.gauntletCuff[k].valid()) {
        a.gfx.draw(a.gauntletHand[k], gm * g_lancerLocal, dp, pass);
        a.gfx.draw(a.gauntletCuff[k], gm * Mat4::fromPose(avatar::cuffPose(false)) * g_lancerLocal, dp, pass);
        return;
    }
    a.gfx.draw(gauntletOf(a, d), gm * g_lancerLocal, dp, pass);
}

static void drawLancerAt(App& a, const Mat4& grip, int h, const AvatarLook& L, int pass, const Mat4& pre, bool preMirror,
                         const AvatarDesc& d) {
    Mat4 mirror = Mat4::identity();
    if (h == 0) mirror.m[0] = -1;
    DrawParams dp;
    applyLook(dp, L);
    dp.mirrored = (h == 0) != preMirror;
    drawGauntlet(a, d, pre * grip * mirror, dp, pass);
}

// main menu: your avatar on a slowly turning pedestal to the right of the menu, your name above it
static void drawTitleShowcase(App& a, int pass) {
    if (!a.menuOpen || !a.titleOpen || a.builderOpen || a.helpOpen || a.avModels.empty()) return;
    const Vec3 right = a.menuPose.q.rotate({1, 0, 0}), away = a.menuPose.q.rotate({0, 0, -1});
    Vec3 base = a.menuPose.p + right * 1.9f + away * 1.4f;   // clear of the panel (about 40 degrees to the right)
    if (inLobby(a)) base = kLobbySpot + Vec3{1.45f * std::sin(deg2rad(40.f)), 0, -1.45f * std::cos(deg2rad(40.f))};   // to the right
    base.y = floorHere(a);
    Vec3 toUser = a.viewHead.p - base;
    toUser.y = 0;
    toUser = length(toUser) > 1e-3f ? normalize(toUser) : Vec3{0, 0, 1};
    const float face = std::atan2(-toUser.x, -toUser.z);   // the avatar looks at the player (it had its back turned)
    const float yaw = face + 0.55f * std::sin((float)nowSeconds() * 0.35f);   // turns side to side
    const Quat q = Quat::axisAngle({0, 1, 0}, yaw);
    PlayerInput in;
    in.headTracked = true;
    in.head = {q, base + Vec3{0, 1.62f, 0}};
    in.avatar = a.myAvatar;
    for (int h = 0; h < 2; ++h) {   // arms relaxed, fists a little forward
        in.hand[h].tracked = true;
        const float s = h ? 1.f : -1.f;
        in.hand[h].grip = {q * Quat::axisAngle({1, 0, 0}, -1.05f) * Quat::axisAngle({0, 0, 1}, s * 0.25f),
                           in.head.p + q.rotate({s * 0.24f, -0.80f, -0.10f})};
    }
    drawAvatarBody(a, in, 0, pass, Mat4::identity(), false, false, 0, 100);   // (its own ponytail chain)
    const AvatarLook look = lookOf(a, in.avatar, 0);
    for (int h = 0; h < 2; ++h) drawLancerAt(a, Mat4::fromPose(in.hand[h].grip), h, look, pass, Mat4::identity(), false, in.avatar);
    if (pass != 1) return;
    // a hologram disc under the avatar (the bodies end above the knee), and its beam down to the floor
    const float ring[4] = {0.10f, 0.55f, 0.90f, 1}, ring2[4] = {0.04f, 0.20f, 0.40f, 1}, white[4] = {1, 1, 1, 1};
    a.gfx.drawSpriteFlat(base + Vec3{0, 0.62f, 0}, 0.75f, ring, {0, 1, 0}, 1);
    a.gfx.drawSpriteFlat(base + Vec3{0, 0.006f, 0}, 0.9f, ring2, {0, 1, 0}, 1);
    const Vec3 pts[2] = {base + Vec3{0, 0.02f, 0}, base + Vec3{0, 0.60f, 0}};
    const float beam[4] = {0.06f, 0.30f, 0.55f, 0.5f};
    a.gfx.drawRibbon(pts, 2, 0.32f, beam);
    const Quat lq = Quat::axisAngle({0, 1, 0}, face + kPi);   // text quads face +Z
    a.gfx.drawText(a.net.name(), {lq, base + Vec3{0, 1.90f, 0}}, 0.08f, white);
}

// the builder's mirror (also the tuning tool's): your reflection, moving with you, and the glass
static AvatarLook drawMirror(App& a, int pass) {
    const int L = a.net.localIndex();
    const PlayerInput& in = a.localIn;
    const Mat4 R = reflectMat(a.mirrorPoint, a.mirrorNormal);
    drawAvatarBody(a, in, L, pass, R, true, false, 0);
    const AvatarLook look = lookOf(a, in.avatar, L);
    for (int h = 0; h < 2; ++h)
        if (in.hand[h].tracked) drawLancerAt(a, Mat4::fromPose(in.hand[h].grip), h, look, pass, R, true, in.avatar);
    if (pass != 1) return look;
    // the mirror: a faint glass pane with a glowing frame, a light ring where the reflection stands
    const Vec3 n = a.mirrorNormal;
    const Quat mq = Quat::axisAngle({0, 1, 0}, std::atan2(n.x, n.z));   // quad +Z = n (toward the player)
    const Vec3 mc = a.mirrorPoint + Vec3{0, 1.05f, 0};
    const float glass[4] = {0.02f, 0.05f, 0.08f, 0.10f};
    a.gfx.drawPanel(Mat4::fromPose({mq, mc}, {1.2f, 2.1f, 1}), glass);
    const float edge[4] = {look.rim[0] * 0.8f, look.rim[1] * 0.8f, look.rim[2] * 0.8f, 1};
    const Vec3 side = mq.rotate({1, 0, 0});
    a.gfx.drawBox(Mat4::fromPose({mq, mc + side * 0.6f}, {0.004f, 1.05f, 0.004f}), edge, true);
    a.gfx.drawBox(Mat4::fromPose({mq, mc - side * 0.6f}, {0.004f, 1.05f, 0.004f}), edge, true);
    a.gfx.drawBox(Mat4::fromPose({mq, mc + Vec3{0, 1.05f, 0}}, {0.6f, 0.004f, 0.004f}), edge, true);
    Vec3 foot{in.head.p.x, 0.004f, in.head.p.z};
    Vec3 rf = foot - n * (2.f * dot(foot - a.mirrorPoint, n));
    const float ring[4] = {look.rim[0] * 0.5f, look.rim[1] * 0.5f, look.rim[2] * 0.5f, 1};
    a.gfx.drawSpriteFlat({rf.x, 0.006f, rf.z}, 1.1f, ring, {0, 1, 0}, 1);
    return look;
}

static void drawBuilder(App& a, int pass) {
    if (!a.builderOpen) return;
    const int L = a.net.localIndex();
    const AvatarLook look = drawMirror(a, pass);
    if (pass != 1) return;
    const float edge[4] = {look.rim[0] * 0.8f, look.rim[1] * 0.8f, look.rim[2] * 0.8f, 1};
    // the panel
    auto at = [&](float x, float y, float z) { return a.builderPose.transform({x, y, z}); };
    auto quad = [&](float x, float y, float w, float h, float z, const float* c) {
        a.gfx.drawPanel(Mat4::fromPose({a.builderPose.q, at(x, y, z)}, {w, h, 1}), c);
    };
    auto text = [&](const std::string& t, float x, float y, float h, const float* c) {
        a.gfx.drawText(t, {a.builderPose.q, at(x, y, 0.006f)}, h, c);
    };
    const float ua = a.ptRunning ? 0.998f : 0.f;   // solid over your room
    const float bg[4] = {0.004f, 0.007f, 0.016f, std::max(ua, 0.90f)}, frame[4] = {edge[0] * 0.6f, edge[1] * 0.6f, edge[2] * 0.6f, std::max(ua, 0.9f)};
    const float btn[4] = {0.03f, 0.05f, 0.08f, 0.95f}, hov[4] = {0.05f, 0.40f, 0.75f, 0.98f};
    const float white[4] = {1, 1, 1, 1}, grey[4] = {0.45f, 0.5f, 0.56f, 1}, cyan[4] = {0.3f, 0.85f, 1.0f, 1};
    const float top = 0.52f, bottom = builderButtonsY(a) - 0.06f;
    quad(0, (top + bottom) * 0.5f, 0.90f, top - bottom + 0.03f, -0.002f, frame);
    quad(0, (top + bottom) * 0.5f, 0.87f, top - bottom, 0, bg);
    text("CUSTOMIZE AVATAR", 0, 0.455f, 0.042f, cyan);
    const AvatarDesc& d = a.myAvatar;
    const int nat = avatar::nationOf(d);
    for (int f = 0; f < avatar::kRowCount + 2; ++f) {
        if (f == avatar::kRowCount + 1) {   // PRESET > COUNTRY FLAG: the country, with its flag
            if (!nat) break;
            const float y = builderCountryY();
            text("COUNTRY", kBLabel, y, 0.024f, grey);
            quad(kBPrev, y, 0.05f, 0.044f, 0.003f, a.builderHover == 104 ? hov : btn);
            quad(kBNext, y, 0.05f, 0.044f, 0.003f, a.builderHover == 105 ? hov : btn);
            text("<", kBPrev, y, 0.028f, white);
            text(">", kBNext, y, 0.028f, white);
            const float nh = std::min(0.024f, 0.24f / (0.75f * (float)std::strlen(avatar::nation(nat)->name)));   // (long names fit)
            text(avatar::nation(nat)->name, kBValue, y, nh, white);
            drawFlag(a, nat, {a.builderPose.q, at(0.395f, y, 0.004f)}, 0.048f);
            continue;
        }
        const bool presetRow = f == avatar::kRowCount;
        const float y = presetRow ? builderPresetY() : kBRow0 - f * kBRowH;
        const int hp = presetRow ? 100 : f * 2;
        text(presetRow ? "PRESET" : avatar::fieldName(avatar::rowField(f)), kBLabel, y, 0.024f, grey);
        quad(kBPrev, y, 0.05f, 0.044f, 0.003f, a.builderHover == hp ? hov : btn);
        quad(kBNext, y, 0.05f, 0.044f, 0.003f, a.builderHover == hp + 1 ? hov : btn);
        text("<", kBPrev, y, 0.028f, white);
        text(">", kBNext, y, 0.028f, white);
        if (presetRow) {
            text(presetLabel(a), kBValue, y, 0.024f, white);
            continue;
        }
        const avatar::Field fld = avatar::rowField(f);
        const bool kit = nat && (fld == avatar::F_SUIT || fld == avatar::F_PATTERN || fld == avatar::F_PRIMARY ||
                                 fld == avatar::F_SECONDARY || fld == avatar::F_GLOW || fld == avatar::F_LOWER);
        if (kit) { text("TEAM KIT", kBValue, y, 0.024f, grey); continue; }   // (the country's jersey decides these)
        text(avatar::optionName(fld, avatar::get(d, fld)), kBValue, y, 0.024f, white);
        if (avatar::isColor(fld)) {
            float c3[3];
            avatar::optionColor(fld, avatar::get(d, fld), kTeamRGB[L & 1], c3);
            const float sw[4] = {c3[0], c3[1], c3[2], 1};
            quad(0.395f, y, 0.032f, 0.032f, 0.003f, sw);
        }
    }
    const float yb = builderButtonsY(a);
    quad(-0.24f, yb, 0.20f, 0.06f, 0.003f, a.builderHover == 102 ? hov : btn);
    quad(0.0f, yb, 0.20f, 0.06f, 0.003f, a.builderHover == 106 ? hov : btn);
    quad(0.24f, yb, 0.24f, 0.06f, 0.003f, a.builderHover == 103 ? hov : btn);
    text("RANDOM", -0.24f, yb, 0.026f, white);
    text("CANCEL", 0.0f, yb, 0.026f, white);
    text("SAVE AND APPLY", 0.24f, yb, 0.019f, white);
    // laser + cursor
    if (a.aimValid[a.pointerHand]) {
        Vec3 o = a.aim[a.pointerHand].p, dd = a.aim[a.pointerHand].q.rotate({0, 0, -1});
        float len = a.builderHitValid ? length(a.builderHit - o) : 1.5f;
        const float laser[4] = {0.15f, 0.6f, 1.0f, 1};
        a.gfx.drawBox(Mat4::fromPose({a.aim[a.pointerHand].q, o + dd * (len * 0.5f)}, {0.003f, 0.003f, len * 0.5f}), laser, true);
        if (a.builderHitValid) a.gfx.drawSphere(a.builderHit, 0.008f, cyan, true);
    }
}

// ------------------------------------------------------------------ developer tuning tool (SETTINGS > TUNING, dev mode)
// The gauntlet parts and the avatar skeleton, adjusted live in front of a mirror (owner request 2026-10-17). Every
// value is in the right hand's grip space (+X right, +Y up / thumb, -Z forward) or the rig's frames; the left hand is
// the mirror image. Values are kept in files/tuning.cfg (read only in developer mode); EXPORT writes
// files/tuning_export.txt and sends it with the log as a bug report, so the values can become the defaults.
enum TuneUnit { TU_M, TU_DEG, TU_SCALE };
struct TuneParam { int part; const char* key; const char* label; float* v; TuneUnit unit; float lo, hi; };
static const char* kTuneParts[] = {"GAUNTLET ON CONTROLLER", "WRIST CUFF (BRACER)", "FOREARM END", "BALL IN FIST", "SHIELD",
                                   "KNUCKLE GUARD", "SHOULDERS", "ARMS AND ELBOWS", "TORSO AND NECK"};
static const int kTunePartCount = (int)(sizeof kTuneParts / sizeof kTuneParts[0]);
static const char* kTunePartKey[] = {"grip", "cuff", "forearm", "ball", "shield", "knuckle", "shoulder", "arm", "torso"};
static const char* kTunePartHelp[] = {
    "THE WHOLE GAUNTLET AGAINST THE CONTROLLER (MOVES EVERYTHING)",
    "THE CUFF AGAINST THE HAND, TURNING ABOUT THE WRIST",
    "WHERE THE FOREARM ENDS INSIDE THE CUFF",
    "WHERE A HELD BALL SITS",
    "THE SHIELD ON THE BACK OF THE HAND",
    "THE KNUCKLE GLASS AND ITS BUMPER (WHAT HITS THE BALL), AND THE BUMPER'S SIZE",
    "SHOULDER JOINTS (BOTH SIDES), AND HOW FAR THEY FOLLOW A REACH",
    "BONE LENGTHS, THICKNESS, AND WHERE THE ELBOWS POINT",
    "HOW THE CHEST TURNS TOWARD THE HANDS, AND THE NECK",
};

// the controller part's six values (App fields); a struct of the same shape holds their defaults
struct GripTuneRef { float *x, *up, *fwd, *pitch, *yaw, *roll; };
static std::vector<TuneParam> tuneParamsFor(const GripTuneRef& gr, avatar::RigTuning& r, Tunables& t) {
    std::vector<TuneParam> v;
    auto xyz = [&](int part, Vec3& p) {
        v.push_back({part, "x", "X (RIGHT)", &p.x, TU_M, -0.25f, 0.25f});
        v.push_back({part, "y", "Y (UP)", &p.y, TU_M, -0.25f, 0.25f});
        v.push_back({part, "z", "Z (BACK)", &p.z, TU_M, -0.35f, 0.35f});
    };
    auto pyr = [&](int part, Vec3& d) {
        v.push_back({part, "pitch", "PITCH", &d.x, TU_DEG, -180.f, 180.f});
        v.push_back({part, "yaw", "YAW", &d.y, TU_DEG, -180.f, 180.f});
        v.push_back({part, "roll", "ROLL", &d.z, TU_DEG, -180.f, 180.f});
    };
    v.push_back({0, "x", "X (RIGHT)", gr.x, TU_M, -0.2f, 0.2f});
    v.push_back({0, "up", "UP", gr.up, TU_M, -0.2f, 0.2f});
    v.push_back({0, "fwd", "FORWARD", gr.fwd, TU_M, -0.2f, 0.2f});
    v.push_back({0, "pitch", "PITCH", gr.pitch, TU_DEG, -90.f, 90.f});
    v.push_back({0, "yaw", "YAW", gr.yaw, TU_DEG, -180.f, 180.f});
    v.push_back({0, "roll", "ROLL", gr.roll, TU_DEG, -180.f, 180.f});
    xyz(1, r.cuffPos); pyr(1, r.cuffRot);
    xyz(2, r.forearmEnd);
    xyz(3, t.holdOffset);
    xyz(4, t.shieldBackPos); pyr(4, t.shieldBackRot);
    xyz(5, t.knuckleOffset);
    v.push_back({5, "w", "BUMPER WIDTH", &t.knuckleHalf.x, TU_M, 0.01f, 0.12f});
    v.push_back({5, "h", "BUMPER HEIGHT", &t.knuckleHalf.y, TU_M, 0.01f, 0.12f});
    v.push_back({5, "d", "BUMPER DEPTH", &t.knuckleHalf.z, TU_M, 0.005f, 0.10f});
    v.push_back({6, "out", "OUT", &r.shoulder.x, TU_M, -0.15f, 0.15f});
    v.push_back({6, "up", "UP", &r.shoulder.y, TU_M, -0.15f, 0.15f});
    v.push_back({6, "back", "BACK", &r.shoulder.z, TU_M, -0.15f, 0.15f});
    v.push_back({6, "reach", "REACH (MAX)", &r.reach, TU_M, 0.f, 0.2f});
    v.push_back({6, "reachstart", "REACH STARTS AT", &r.reachStart, TU_SCALE, 0.7f, 1.0f});
    v.push_back({7, "upper", "UPPER ARM LENGTH", &r.upperLen, TU_SCALE, 0.7f, 1.4f});
    v.push_back({7, "fore", "FOREARM LENGTH", &r.foreLen, TU_SCALE, 0.7f, 1.4f});
    v.push_back({7, "thick", "THICKNESS", &r.armThick, TU_SCALE, 0.6f, 1.6f});
    v.push_back({7, "elbowout", "ELBOW OUT", &r.elbowPole.x, TU_SCALE, -1.5f, 1.5f});
    v.push_back({7, "elbowdown", "ELBOW DOWN", &r.elbowPole.y, TU_SCALE, -2.0f, 1.0f});
    v.push_back({7, "elbowback", "ELBOW BACK", &r.elbowPole.z, TU_SCALE, -1.5f, 1.5f});
    v.push_back({7, "elbowpull", "ELBOW PULL", &r.elbowPull, TU_M, 0.f, 0.5f});
    v.push_back({8, "follow", "CHEST FOLLOWS HANDS", &r.torsoFollow, TU_SCALE, 0.f, 2.f});
    v.push_back({8, "limit", "CHEST TURN LIMIT", &r.torsoLimitDeg, TU_DEG, 0.f, 90.f});
    v.push_back({8, "neckup", "NECK UP", &r.neck.y, TU_M, -0.1f, 0.1f});
    v.push_back({8, "neckback", "NECK BACK", &r.neck.z, TU_M, -0.1f, 0.1f});
    return v;
}
static std::vector<TuneParam> tuneParams(App& a) {
    return tuneParamsFor({&a.tuneX, &a.tuneUp, &a.tuneFwd, &a.tunePitchDeg, &a.tuneYawDeg, &a.tuneRollDeg},
                         avatar::rigTuning(), a.game.tun);
}
// the shipped defaults, in the same order as tuneParams
static const std::vector<float>& tuneDefaults() {
    static std::vector<float> d;
    if (d.empty()) {
        static float g[6] = {-0.006f, -0.014f, 0.004f, 27.f, -16.f, -1.f};   // App's defaults (the owner's tuning, 2026-10-19)
        static avatar::RigTuning r;
        static Tunables t;
        for (const TuneParam& p : tuneParamsFor({&g[0], &g[1], &g[2], &g[3], &g[4], &g[5]}, r, t)) d.push_back(*p.v);
    }
    return d;
}
static float tuneStep(const App& a, TuneUnit u) {
    if (u == TU_M) return a.tuneCoarse ? 0.01f : 0.002f;      // 1 cm / 2 mm
    if (u == TU_DEG) return a.tuneCoarse ? 5.f : 1.f;
    return a.tuneCoarse ? 0.05f : 0.01f;
}
static std::string tuneValueText(const TuneParam& p, float v) {
    char b[32];
    if (p.unit == TU_M) snprintf(b, sizeof b, "%+.1f CM", v * 100.f);
    else if (p.unit == TU_DEG) snprintf(b, sizeof b, "%+.0f DEG", v);
    else snprintf(b, sizeof b, "%.2f", v);
    return b;
}
static std::vector<int> tuneRows(App& a, const std::vector<TuneParam>& ps) {
    std::vector<int> r;
    for (int i = 0; i < (int)ps.size(); ++i) if (ps[i].part == a.tunePart) r.push_back(i);
    return r;
}
static void tuneStatus(App& a, const std::string& s) { a.tuneStatus = s; a.tuneStatusUntil = nowSeconds() + 4.0; }

static void saveTuning(App& a) {
    if (!dataDir(a)) return;
    const std::string f = std::string(dataDir(a)) + "/tuning.cfg";
    FILE* fp = fopen(f.c_str(), "w");
    if (!fp) { LOGW("tuning: cannot write %s", f.c_str()); return; }
    fprintf(fp, "# Project Arena developer tuning (SETTINGS > TUNING); read only in developer mode\n");
    for (const TuneParam& p : tuneParams(a)) fprintf(fp, "%s.%s=%.4f\n", kTunePartKey[p.part], p.key, *p.v);
    fclose(fp);
    if (a.tuneSettingsDirty) { saveSettings(a); a.tuneSettingsDirty = false; }   // the controller part is in settings too
}
static void loadTuning(App& a) {
    if (!dataDir(a) || !a.devMode) return;
    const std::string f = std::string(dataDir(a)) + "/tuning.cfg";
    FILE* fp = fopen(f.c_str(), "r");
    if (!fp) return;
    std::vector<TuneParam> ps = tuneParams(a);
    char line[160];
    int n = 0;
    while (fgets(line, sizeof line, fp)) {
        char* eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        const float v = (float)atof(eq + 1);
        for (const TuneParam& p : ps)
            if (std::string(kTunePartKey[p.part]) + "." + p.key == line) { *p.v = clampf(v, p.lo, p.hi); ++n; }
    }
    fclose(fp);
    LOGI("tuning: %d values from tuning.cfg", n);
}
// every value that differs from the shipped default, ready to become the new default
static std::string tuningText(App& a) {
    std::vector<TuneParam> ps = tuneParams(a);
    const std::vector<float>& d = tuneDefaults();
    std::string s;
    char b[160];
    int changed = 0;
    for (size_t i = 0; i < ps.size(); ++i) {
        const bool diff = std::fabs(*ps[i].v - d[i]) > 1e-5f;
        changed += diff;
        snprintf(b, sizeof b, "%s %s.%s = %.4f (default %.4f)\n", diff ? "*" : " ", kTunePartKey[ps[i].part], ps[i].key, *ps[i].v, d[i]);
        s += b;
    }
    snprintf(b, sizeof b, "%d of %d values changed (* = changed); right-hand grip space, metres / degrees\n", changed, (int)ps.size());
    return std::string(b) + s;
}
static void exportTuning(App& a) {
    saveTuning(a);
    const std::string t = tuningText(a);
    for (size_t p = 0, q; p < t.size(); p = q + 1) {   // into the log, line by line (the bug report carries the log)
        q = t.find('\n', p);
        if (q == std::string::npos) q = t.size();
        if (t[p] == '*' || p == 0) LOGI("tuning export: %s", t.substr(p, q - p).c_str());
    }
    if (dataDir(a))
        if (FILE* fp = fopen((std::string(dataDir(a)) + "/tuning_export.txt").c_str(), "w")) { fputs(t.c_str(), fp); fclose(fp); }
    submitReport(a, bugReportText(a, "Project Arena tuning export", t), "tuning export");
    tuneStatus(a, "EXPORTED: SAVED TUNING_EXPORT.TXT AND SENT IT TO THE SERVER");
}

// gripEuler's inverse (right hand): pitch, yaw, roll in degrees
static Vec3 eulerOfGrip(const Quat& q) {
    const Vec3 ex = q.rotate({1, 0, 0}), ey = q.rotate({0, 1, 0}), ez = q.rotate({0, 0, 1});
    return {rad2deg(std::asin(clampf(-ez.y, -1.f, 1.f))), rad2deg(std::atan2(ez.x, ez.z)), rad2deg(std::atan2(ex.y, ey.y))};
}
static Pose poseInverse(const Pose& p) { return {p.q.conj(), p.q.conj().rotate(-p.p)}; }
static Pose mirrorPose(const Pose& p) { return {{p.q.x, -p.q.y, -p.q.z, p.q.w}, {-p.p.x, p.p.y, p.p.z}}; }   // left <-> right

// The pieces the tool moves by hand (the skeleton parts are tuned with the values only): a piece's pose in its frame,
// right-hand values (the gauntlet against the controller, the cuff, the shield; the forearm end, the ball and the
// knuckle bumper are points)
static bool tunePiece(int part) { return part >= 0 && part <= 5; }
static Pose tuneLocal(const App& a, int part) {
    const Tunables& t = a.game.tun;
    switch (part) {
    case 0: return {gripEuler({a.tunePitchDeg, a.tuneYawDeg, a.tuneRollDeg}, false), {a.tuneX, a.tuneUp, -a.tuneFwd}};
    case 1: return avatar::cuffPose(false);
    case 2: return {Quat{}, avatar::rigTuning().forearmEnd};
    case 3: return {Quat{}, t.holdOffset};
    case 4: return {gripEuler(t.shieldBackRot, false), t.shieldBackPos};
    default: return {Quat{}, t.knuckleOffset};
    }
}
// the frame a piece is in, in the world, on hand h (the gauntlet: the controller; the forearm end: the cuff)
static Pose tuneFrame(const App& a, int part, int h) {
    const Pose& g = a.localIn.hand[h].grip;
    if (part == 0) return a.rawGrip[h];
    if (part == 2) return g * avatar::cuffPose(h == 0);
    return g;
}
static Pose tuneWorld(const App& a, int part, int h) {
    const Pose l = tuneLocal(a, part);
    return tuneFrame(a, part, h) * (h == 0 ? mirrorPose(l) : l);
}
static void tuneClampPart(App& a, int part) {
    for (const TuneParam& p : tuneParams(a)) if (p.part == part) *p.v = clampf(*p.v, p.lo, p.hi);
    if (part == 0) a.tuneSettingsDirty = true;
    a.tuneSaveAt = nowSeconds() + 1.0;
}
static void tuneSetLocal(App& a, int part, const Pose& l) {
    Tunables& t = a.game.tun;
    avatar::RigTuning& r = avatar::rigTuning();
    switch (part) {
    case 0: {
        const Vec3 e = eulerOfGrip(l.q);
        a.tunePitchDeg = e.x; a.tuneYawDeg = e.y; a.tuneRollDeg = e.z;
        a.tuneX = l.p.x; a.tuneUp = l.p.y; a.tuneFwd = -l.p.z;
        break;
    }
    case 1:   // cuffPose = {q, pivot + cuffPos - q pivot}
        r.cuffRot = eulerOfGrip(l.q);
        r.cuffPos = l.p - avatar::kCuffPivot + l.q.rotate(avatar::kCuffPivot);
        break;
    case 2: r.forearmEnd = l.p; break;
    case 3: t.holdOffset = l.p; break;
    case 4: t.shieldBackRot = eulerOfGrip(l.q); t.shieldBackPos = l.p; break;
    case 5: t.knuckleOffset = l.p; break;
    default: return;
    }
    tuneClampPart(a, part);
}

static void tuneChange(App& a, int dir) {
    std::vector<TuneParam> ps = tuneParams(a);
    std::vector<int> rows = tuneRows(a, ps);
    if (rows.empty()) return;
    a.tuneRow = std::min(a.tuneRow, (int)rows.size() - 1);
    const TuneParam& p = ps[rows[a.tuneRow]];
    const float st = tuneStep(a, p.unit);
    if (p.unit == TU_DEG && (p.part == 0 || p.part == 1 || p.part == 4)) {
        // a turn about the piece's own axis (side = pitch, up = yaw, long = roll), whatever its other angles are, so
        // every row turns it a different way
        const Vec3 axis = !strcmp(p.key, "pitch") ? Vec3{1, 0, 0} : !strcmp(p.key, "yaw") ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
        Pose l = tuneLocal(a, p.part);
        const Vec3 pivot = p.part == 1 ? l.transform(avatar::kCuffPivot) : l.p;   // (the cuff turns about the wrist)
        l.q = l.q * Quat::axisAngle(axis, deg2rad(dir * st));
        if (p.part == 1) l.p = pivot - l.q.rotate(avatar::kCuffPivot);
        tuneSetLocal(a, p.part, l);
        LOGI("tuning: %s turned %+.0f deg about its %s axis", kTuneParts[p.part], dir * st, p.key);
        return;
    }
    *p.v = clampf(*p.v + dir * st, p.lo, p.hi);
    if (p.part == 0) a.tuneSettingsDirty = true;
    a.tuneSaveAt = nowSeconds() + 1.0;
    LOGI("tuning: %s %s = %.4f", kTuneParts[p.part], p.key, *p.v);
}
static void tuneReset(App& a, bool all) {
    std::vector<TuneParam> ps = tuneParams(a);
    const std::vector<float>& d = tuneDefaults();
    for (size_t i = 0; i < ps.size(); ++i)
        if (all || ps[i].part == a.tunePart) { *ps[i].v = d[i]; if (ps[i].part == 0) a.tuneSettingsDirty = true; }
    a.tuneSaveAt = nowSeconds() + 0.2;
    tuneStatus(a, all ? "EVERY VALUE IS BACK TO ITS DEFAULT" : std::string(kTuneParts[a.tunePart]) + ": DEFAULTS");
    LOGI("tuning: reset %s", all ? "all" : kTuneParts[a.tunePart]);
}

// the panel: rows of the picked part (- value +), then the buttons. Ids: 2r / 2r+1 = row r minus / plus, 100 + r =
// the row itself, the rest below.
enum { TB_PREV = 200, TB_NEXT, TB_STEP, TB_PROPS, TB_MARKERS, TB_RESET, TB_RESETALL, TB_EXPORT, TB_DONE };
struct TuneBtn { float x, y, w, h; int id; std::string label; bool on; };
static const float kTuneW = 0.92f, kTuneTop = 0.50f, kTuneBottom = -0.50f;
static std::vector<TuneBtn> tuneButtons(App& a) {
    std::vector<TuneBtn> v;
    std::vector<TuneParam> ps = tuneParams(a);
    std::vector<int> rows = tuneRows(a, ps);
    v.push_back({-0.40f, 0.36f, 0.07f, 0.06f, TB_PREV, "<", false});
    v.push_back({0.40f, 0.36f, 0.07f, 0.06f, TB_NEXT, ">", false});
    for (int r = 0; r < (int)rows.size(); ++r) {
        const float y = 0.235f - r * 0.06f;
        v.push_back({-0.12f, y, 0.62f, 0.052f, 100 + r, ps[rows[r]].label, r == a.tuneRow});
        v.push_back({0.25f, y, 0.07f, 0.052f, 2 * r, "-", false});
        v.push_back({0.39f, y, 0.07f, 0.052f, 2 * r + 1, "+", false});
    }
    v.push_back({-0.30f, -0.28f, 0.28f, 0.06f, TB_STEP, a.tuneCoarse ? "STEP: COARSE" : "STEP: FINE", a.tuneCoarse});
    v.push_back({0.0f, -0.28f, 0.28f, 0.06f, TB_PROPS, a.tuneProps ? "BALL + SHIELD: ON" : "BALL + SHIELD: OFF", a.tuneProps});
    v.push_back({0.30f, -0.28f, 0.28f, 0.06f, TB_MARKERS, a.tuneMarkers ? "MARKERS: ON" : "MARKERS: OFF", a.tuneMarkers});
    v.push_back({-0.30f, -0.36f, 0.28f, 0.06f, TB_RESET, "RESET PART", false});
    v.push_back({0.0f, -0.36f, 0.28f, 0.06f, TB_RESETALL, "RESET ALL", false});
    v.push_back({0.30f, -0.36f, 0.28f, 0.06f, TB_EXPORT, "EXPORT + SEND", false});
    v.push_back({0.0f, -0.445f, 0.36f, 0.06f, TB_DONE, "DONE", false});
    return v;
}

static void openTuning(App& a, const PlayerInput& in) {
    if (!a.devMode) return;
    Vec3 f = in.head.q.rotate({0, 0, -1});
    f.y = 0;
    f = length(f) > 1e-3f ? normalize(f) : Vec3{1, 0, 0};
    const Vec3 right = cross(f, Vec3{0, 1, 0});
    a.mirrorNormal = -f;   // the builder's mirror, 0.85 m ahead
    a.mirrorPoint = Vec3{in.head.p.x, 0, in.head.p.z} + f * 0.85f;
    Vec3 pp = in.head.p + f * 1.0f + right * 0.78f + Vec3{0, -0.22f, 0};
    Vec3 to = in.head.p - pp;
    to.y = 0;
    to = normalize(to);
    a.tunePose = {Quat::axisAngle({0, 1, 0}, std::atan2(to.x, to.z)), pp};
    a.tuneOpen = true;
    a.menuOpen = false;
    a.prevTuneTrig = true;
    LOGI("tuning: open");
}
static void closeTuning(App& a) {
    if (!a.tuneOpen) return;
    a.tuneOpen = false;
    saveTuning(a);
    LOGI("tuning: closed");
}
static void tuneActivate(App& a, int id) {
    std::vector<TuneParam> ps = tuneParams(a);
    const int nrows = (int)tuneRows(a, ps).size();
    if (id >= 0 && id < 2 * nrows) { a.tuneRow = id / 2; tuneChange(a, (id & 1) ? 1 : -1); return; }
    if (id >= 100 && id < 100 + nrows) { a.tuneRow = id - 100; return; }
    switch (id) {
    case TB_PREV: a.tunePart = (a.tunePart + kTunePartCount - 1) % kTunePartCount; a.tuneRow = 0; break;
    case TB_NEXT: a.tunePart = (a.tunePart + 1) % kTunePartCount; a.tuneRow = 0; break;
    case TB_STEP: a.tuneCoarse = !a.tuneCoarse; break;
    case TB_PROPS: a.tuneProps = !a.tuneProps; break;
    case TB_MARKERS: a.tuneMarkers = !a.tuneMarkers; break;
    case TB_RESET: tuneReset(a, false); break;
    case TB_RESETALL: tuneReset(a, true); break;
    case TB_EXPORT: {   // one export per press (a press once sent two)
        static double last = -10;
        if (nowSeconds() - last > 3.0) { last = nowSeconds(); exportTuning(a); }
        break;
    }
    case TB_DONE: closeTuning(a); break;
    default: break;
    }
}

static void stickState(App& a, XrAction act, float& x, float& y) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = act;
    XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
    x = y = 0;
    if (act != XR_NULL_HANDLE && xrGetActionStateVector2f(a.session, &gi, &st) == XR_SUCCESS && st.isActive) { x = st.currentState.x; y = st.currentState.y; }
}

// the sticks (right: up / down picks a value, left / right changes it, held = repeats; left: left / right = the
// part) and the pointer on the panel; no grabbing while tuning
static void updateTuning(App& a, PlayerInput& in) {
    a.tuneHover = -1;
    a.tuneHitValid = false;
    const double t = nowSeconds();
    if (a.tuneSaveAt > 0 && t > a.tuneSaveAt) { a.tuneSaveAt = 0; saveTuning(a); }
    if (!a.tuneOpen) return;
    for (int h = 0; h < 2; ++h) a.localIn.hand[h].grip = in.hand[h].grip;   // (the pieces' frames: this frame's hands)
    std::vector<TuneParam> ps = tuneParams(a);
    const int nrows = (int)tuneRows(a, ps).size();
    float rx, ry, lx, ly;
    stickState(a, a.thumbstick, rx, ry);
    stickState(a, a.thumbstickL, lx, ly);
    if (!a.tuneLatchY && std::fabs(ry) > 0.7f && std::fabs(ry) > std::fabs(rx)) {
        a.tuneLatchY = true;
        a.tuneRow = (a.tuneRow + (ry > 0 ? nrows - 1 : 1)) % std::max(1, nrows);   // stick up = the row above
    } else if (std::fabs(ry) < 0.3f) {
        a.tuneLatchY = false;
    }
    const int dir = std::fabs(rx) > 0.6f && std::fabs(rx) > std::fabs(ry) ? (rx > 0 ? 1 : -1) : 0;
    if (dir != a.tuneStickDir) {
        a.tuneStickDir = dir;
        if (dir) { tuneChange(a, dir); a.tuneRepeatAt = t + 0.35; }
    } else if (dir && t > a.tuneRepeatAt) {
        tuneChange(a, dir);
        a.tuneRepeatAt = t + 0.07;
    }
    if (!a.tuneLatchL && std::fabs(lx) > 0.7f) {
        a.tuneLatchL = true;
        tuneActivate(a, lx > 0 ? TB_NEXT : TB_PREV);
    } else if (std::fabs(lx) < 0.3f) {
        a.tuneLatchL = false;
    }
    const bool trig = in.hand[a.pointerHand].trigger > 0.7f;
    if (a.aimValid[a.pointerHand]) {
        const Vec3 o = a.aim[a.pointerHand].p, d = a.aim[a.pointerHand].q.rotate({0, 0, -1});
        const Vec3 nz = a.tunePose.q.rotate({0, 0, 1});
        const float den = dot(d, nz);
        if (den < -1e-3f) {
            const float tt = dot(a.tunePose.p - o, nz) / den;
            if (tt > 0 && tt < 4) {
                const Vec3 lp = a.tunePose.q.conj().rotate(o + d * tt - a.tunePose.p);
                if (std::fabs(lp.x) < kTuneW * 0.5f && lp.y < kTuneTop && lp.y > kTuneBottom) {
                    a.tuneHit = o + d * tt;
                    a.tuneHitValid = true;
                    std::vector<TuneBtn> bs = tuneButtons(a);
                    for (const TuneBtn& b : bs)
                        if (std::fabs(lp.x - b.x) < b.w * 0.5f && std::fabs(lp.y - b.y) < b.h * 0.5f) a.tuneHover = b.id;
                    if (trig && !a.prevTuneTrig && a.tuneHover >= 0) { tuneActivate(a, a.tuneHover); haptic(a, a.pointerHand, 0.35f, 0.025f); }
                }
            }
        }
    }
    a.prevTuneTrig = trig;
    // by hand: hold grip on one hand and the picked piece on the other hand follows it (moves and turns with it);
    // let go to leave it there
    if (a.tuneGrab >= 0 && (in.hand[a.tuneGrab].squeeze < 0.4f || !in.hand[a.tuneGrab].tracked || !tunePiece(a.tunePart))) {
        LOGI("tuning: %s placed by hand", kTuneParts[a.tunePart]);
        a.tuneGrab = -1;
    }
    if (a.tuneGrab < 0 && tunePiece(a.tunePart))
        for (int d = 0; d < 2; ++d)
            if (in.hand[d].squeeze > 0.7f && in.hand[d].tracked && in.hand[1 - d].tracked) {
                a.tuneGrab = d;
                a.tuneGrabD0 = in.hand[d].grip;
                a.tuneGrabW0 = tuneWorld(a, a.tunePart, 1 - d);
                haptic(a, d, 0.4f, 0.03f);
                break;
            }
    if (a.tuneGrab >= 0) {
        const int d = a.tuneGrab, h = 1 - d;
        Pose w = in.hand[d].grip * poseInverse(a.tuneGrabD0) * a.tuneGrabW0;
        Pose l = poseInverse(tuneFrame(a, a.tunePart, h)) * w;
        if (h == 0) l = mirrorPose(l);
        if (a.tunePart == 2 || a.tunePart == 3 || a.tunePart == 5) l.q = Quat{};   // points: where, not which way
        tuneSetLocal(a, a.tunePart, l);
    }
    for (int h = 0; h < 2; ++h) { in.hand[h].trigger = 0; in.hand[h].squeeze = 0; }
}

// the knuckle glass follows the bumper: the knuckleguard model is authored round Sparc's bumper centre
// (make_props.py knuckleguard), the bumper is wherever Tunables::knuckleOffset puts it
static Mat4 knuckleShift(const Game& g) {
    static const Vec3 kGlassCentre{-0.022f, 0.010f, -0.137f};
    return Mat4::fromPose({Quat{}, g.tun.knuckleOffset - kGlassCentre});
}

// the ball, the shield and the knuckle glass on both of your hands, and the markers: in the room (R = identity) and
// in the mirror (R = the reflection)
static void drawTuneHands(App& a, int pass, const Mat4& R, bool refl) {
    const Game& g = a.game;
    const PlayerInput& in = a.localIn;
    const int L = a.net.localIndex();
    const float teamCol[2][4] = {{0.1f, 0.7f, 1.0f, 1}, {1.0f, 0.4f, 0.1f, 1}};
    const float* tc = teamCol[L & 1];
    auto RP = [&](const Vec3& p) {   // a world point in the mirror
        if (!refl) return p;
        return p - a.mirrorNormal * (2.f * dot(p - a.mirrorPoint, a.mirrorNormal));
    };
    auto line = [&](const Vec3& p0, const Vec3& p1, float th, const float* c) {
        const Vec3 q0 = RP(p0), q1 = RP(p1), dd = q1 - q0;
        const float len = length(dd);
        if (len < 1e-4f) return;
        a.gfx.drawBox(Mat4::fromPose({fromTo({0, 0, 1}, dd / len), (q0 + q1) * 0.5f}, {th, th, len * 0.5f}), c, true);
    };
    for (int h = 0; h < 2; ++h) {
        if (!in.hand[h].tracked) continue;
        const Pose& gp = in.hand[h].grip;
        const Mat4 grip = Mat4::fromPose(gp);
        Mat4 mirror = Mat4::identity();
        if (h == 0) mirror.m[0] = -1;
        const float sx = h == 0 ? -1.f : 1.f;
        auto G = [&](const Vec3& v) { return gp.transform({sx * v.x, v.y, v.z}); };   // right-hand grip point, mirrored
        if (a.tuneProps) {
            DrawParams bp;
            bp.tintMul[0] = tc[0] * 2.f; bp.tintMul[1] = tc[1] * 2.f; bp.tintMul[2] = tc[2] * 2.f;
            bp.mirrored = refl;
            const float s = g.tun.projectileRadius / 0.08f;
            a.gfx.draw(a.projectile, R * Mat4::fromPose({Quat{}, G(g.tun.holdOffset)}, {s, s, s}), bp, pass);
            DrawParams sp;
            sp.tintMul[0] = tc[0]; sp.tintMul[1] = tc[1]; sp.tintMul[2] = tc[2];
            sp.emissiveAdd[0] = tc[0] * 0.4f; sp.emissiveAdd[1] = tc[1] * 0.4f; sp.emissiveAdd[2] = tc[2] * 0.4f;
            sp.alphaMul = 0.7f;
            sp.mirrored = refl;
            a.gfx.draw(a.shield, R * grip * Mat4::fromPose(g.shieldLocal(h)) * g_plateLocal, sp, pass);
            DrawParams kp;
            kp.mirrored = (h == 0) != refl;
            kp.forceShader = SH_TRANSLUCENT;
            kp.tintMul[0] = 0.75f + 0.25f * tc[0]; kp.tintMul[1] = 0.75f + 0.25f * tc[1]; kp.tintMul[2] = 0.75f + 0.25f * tc[2];
            kp.alphaMul = 0.35f;
            a.gfx.draw(a.knuckle, R * grip * mirror * knuckleShift(g) * g_knuckleLocal, kp, pass);
        }
        if (pass == 1 && tunePiece(a.tunePart)) {   // the picked value's axis through the piece (red X, green Y, blue Z)
            std::vector<TuneParam> ps = tuneParams(a);
            std::vector<int> rows = tuneRows(a, ps);
            if (!rows.empty()) {
                const TuneParam& p = ps[rows[std::min(a.tuneRow, (int)rows.size() - 1)]];
                const char* k = p.key;
                const int ax = (!strcmp(k, "x") || !strcmp(k, "pitch") || !strcmp(k, "w")) ? 0
                             : (!strcmp(k, "y") || !strcmp(k, "up") || !strcmp(k, "yaw") || !strcmp(k, "h")) ? 1 : 2;
                const Pose w = tuneWorld(a, a.tunePart, h);
                // positions move along the frame's axes, turns are about the piece's own axes
                const Quat q = p.unit == TU_DEG ? w.q : tuneFrame(a, a.tunePart, h).q;
                Vec3 e{0, 0, 0};
                (ax == 0 ? e.x : ax == 1 ? e.y : e.z) = 1;
                if (h == 0 && ax == 0) e.x = -1;
                const Vec3 o = (p.part == 1 && p.unit == TU_DEG) ? w.transform(h == 0 ? Vec3{-avatar::kCuffPivot.x, avatar::kCuffPivot.y,
                               avatar::kCuffPivot.z} : avatar::kCuffPivot) : w.p;
                const float col[3][4] = {{1, 0.25f, 0.25f, 1}, {0.3f, 1, 0.3f, 1}, {0.3f, 0.5f, 1, 1}};
                glDisable(GL_DEPTH_TEST);
                line(o - q.rotate(e) * 0.08f, o + q.rotate(e) * 0.08f, 0.0035f, col[ax]);
                a.gfx.drawSphere(RP(o + q.rotate(e) * 0.08f), 0.009f, col[ax], true);   // the + end
                glEnable(GL_DEPTH_TEST);
            }
        }
        if (!a.tuneMarkers || pass != 1) continue;
        glDisable(GL_DEPTH_TEST);   // markers show through the ball and the gauntlet
        const float cyan[4] = {0.2f, 0.9f, 1.0f, 1}, green[4] = {0.3f, 1.0f, 0.4f, 1}, yellow[4] = {1.0f, 0.9f, 0.2f, 1};
        const float magenta[4] = {1.0f, 0.3f, 0.9f, 1}, white[4] = {1, 1, 1, 1}, orange[4] = {1.0f, 0.55f, 0.15f, 1};
        a.gfx.drawSphere(RP(G(g.tun.launcherOffset)), 0.008f, cyan, true);    // throws leave from here
        a.gfx.drawSphere(RP(G(g.tun.catcherOffset)), 0.008f, green, true);    // catches are measured from here
        a.gfx.drawSphere(RP(G(g.tun.holdOffset)), 0.006f, orange, true);
        line(G(g.tun.fistBack), G(g.tun.fistFront), 0.003f, yellow);           // the fist (fist bumps)
        const Vec3 fe = avatar::cuffPose(false).transform(avatar::rigTuning().forearmEnd);
        a.gfx.drawSphere(RP(G(fe)), 0.008f, magenta, true);                    // the forearm ends here
        a.gfx.drawSphere(RP(G(avatar::cuffPose(false).transform(avatar::kCuffPivot))), 0.006f, white, true);   // wrist pivot
        // the knuckle bumper box (what hits the ball)
        const Vec3 kc = g.tun.knuckleOffset, kh = g.tun.knuckleHalf;
        const float kb[4] = {0.5f, 0.8f, 1.0f, 1};
        for (int e = 0; e < 4; ++e) {
            const float ex = (e & 1) ? 1.f : -1.f, ey = (e & 2) ? 1.f : -1.f;
            line(G(kc + Vec3{ex * kh.x, ey * kh.y, -kh.z}), G(kc + Vec3{ex * kh.x, ey * kh.y, kh.z}), 0.0015f, kb);
            line(G(kc + Vec3{ex * kh.x, -kh.y, ey * kh.z}), G(kc + Vec3{ex * kh.x, kh.y, ey * kh.z}), 0.0015f, kb);
            line(G(kc + Vec3{-kh.x, ex * kh.y, ey * kh.z}), G(kc + Vec3{kh.x, ex * kh.y, ey * kh.z}), 0.0015f, kb);
        }
        glEnable(GL_DEPTH_TEST);
    }
    if (!refl || !a.tuneMarkers || pass != 1 || in.head.p.y < 0.3f) return;
    // the skeleton, in the mirror (you have no body of your own): neck, shoulders, elbows, wrists
    const avatar::Rig rig = avatar::solveRig(in, in.avatar);
    const float bone[4] = {1.0f, 0.85f, 0.3f, 1}, joint[4] = {1.0f, 0.4f, 0.2f, 1};
    glDisable(GL_DEPTH_TEST);   // seen through the body
    line(rig.neck.a, rig.neck.b, 0.003f, bone);
    for (int k = 0; k < 2; ++k) {
        line(rig.neck.b, rig.upper[k].a, 0.003f, bone);
        line(rig.upper[k].a, rig.upper[k].b, 0.003f, bone);
        line(rig.fore[k].a, rig.fore[k].b, 0.003f, bone);
        a.gfx.drawSphere(RP(rig.upper[k].a), 0.012f, joint, true);
        a.gfx.drawSphere(RP(rig.upper[k].b), 0.012f, joint, true);
        a.gfx.drawSphere(RP(rig.fore[k].b), 0.010f, joint, true);
    }
    a.gfx.drawSphere(RP(rig.neck.a), 0.010f, joint, true);
    glEnable(GL_DEPTH_TEST);
}

static void drawTuning(App& a, int pass) {
    if (!a.tuneOpen) return;
    const AvatarLook look = drawMirror(a, pass);
    drawTuneHands(a, pass, Mat4::identity(), false);
    drawTuneHands(a, pass, reflectMat(a.mirrorPoint, a.mirrorNormal), true);
    if (pass != 1) return;
    const Pose& P = a.tunePose;
    auto at = [&](float x, float y, float z) { return P.transform({x, y, z}); };
    auto quad = [&](float x, float y, float w, float h, float z, const float* c) {
        a.gfx.drawPanel(Mat4::fromPose({P.q, at(x, y, z)}, {w, h, 1}), c);
    };
    auto text = [&](const std::string& t, float x, float y, float h, const float* c) { a.gfx.drawText(t, {P.q, at(x, y, 0.006f)}, h, c); };
    const float ua = a.ptRunning ? 0.998f : 0.f;   // solid over your room
    const float bg[4] = {0.004f, 0.007f, 0.016f, std::max(ua, 0.92f)}, frame[4] = {look.rim[0] * 0.5f, look.rim[1] * 0.5f, look.rim[2] * 0.5f, std::max(ua, 0.9f)};
    const float btn[4] = {0.03f, 0.05f, 0.08f, 0.95f}, hov[4] = {0.05f, 0.40f, 0.75f, 0.98f}, on[4] = {0.06f, 0.22f, 0.36f, 0.98f};
    const float white[4] = {1, 1, 1, 1}, grey[4] = {0.5f, 0.55f, 0.62f, 1}, cyan[4] = {0.3f, 0.85f, 1.0f, 1}, amber[4] = {1.0f, 0.8f, 0.3f, 1};
    quad(0, 0, kTuneW + 0.03f, kTuneTop - kTuneBottom + 0.03f, -0.002f, frame);
    quad(0, 0, kTuneW, kTuneTop - kTuneBottom, 0, bg);
    text("TUNING (DEVELOPER)", 0, 0.45f, 0.036f, cyan);
    text(kTuneParts[a.tunePart], 0, 0.36f, 0.028f, white);
    text(kTunePartHelp[a.tunePart], 0, 0.30f, 0.016f, grey);
    std::vector<TuneParam> ps = tuneParams(a);
    std::vector<int> rows = tuneRows(a, ps);
    const std::vector<float>& d = tuneDefaults();
    for (const TuneBtn& b : tuneButtons(a)) {
        const bool row = b.id >= 100 && b.id < 100 + (int)rows.size();
        quad(b.x, b.y, b.w, b.h, 0.003f, b.id == a.tuneHover ? hov : b.on ? on : btn);
        if (row) {
            const int r = b.id - 100;
            const TuneParam& p = ps[rows[r]];
            const bool changed = std::fabs(*p.v - d[rows[r]]) > 1e-5f;
            text(b.label, b.x - 0.13f, b.y, 0.020f, white);
            text(tuneValueText(p, *p.v), b.x + 0.19f, b.y, 0.020f, changed ? amber : white);
        } else {
            text(b.label, b.x, b.y, b.h > 0.055f && b.w < 0.1f ? 0.03f : 0.018f, white);
        }
    }
    text("RIGHT STICK: UP / DOWN PICKS A VALUE, LEFT / RIGHT CHANGES IT", 0, -0.170f, 0.016f, grey);
    text("LEFT STICK: NEXT PART    AMBER = CHANGED FROM THE DEFAULT", 0, -0.196f, 0.016f, grey);
    if (tunePiece(a.tunePart))
        text("OR HOLD GRIP ON ONE HAND: THE PIECE ON THE OTHER HAND MOVES WITH IT", 0, -0.222f, 0.016f, grey);
    if (nowSeconds() < a.tuneStatusUntil) text(a.tuneStatus, 0, -0.495f, 0.016f, amber);
    if (a.aimValid[a.pointerHand]) {
        const Vec3 o = a.aim[a.pointerHand].p, dd = a.aim[a.pointerHand].q.rotate({0, 0, -1});
        const float len = a.tuneHitValid ? length(a.tuneHit - o) : 1.5f;
        const float laser[4] = {0.15f, 0.6f, 1.0f, 1};
        a.gfx.drawBox(Mat4::fromPose({a.aim[a.pointerHand].q, o + dd * (len * 0.5f)}, {0.003f, 0.003f, len * 0.5f}), laser, true);
        if (a.tuneHitValid) a.gfx.drawSphere(a.tuneHit, 0.008f, cyan, true);
    }
}

// ------------------------------------------------------------------ director tools (spectators)
struct DirBtn { float x, y, w, h; int act, idx; std::string label; bool on; };
enum { DB_AUTO = 1, DB_ADD, DB_DONE, DB_TEAM, DB_SHARE, DB_GO, DB_HOLD, DB_TRANS, DB_TRIG, DB_DEL, DB_INFOCLOSE };
static const float kDirW = 1.12f, kDirH = 0.98f;

static std::vector<DirBtn> dirButtons(const App& a) {
    std::vector<DirBtn> v;
    v.push_back({-0.36f, 0.33f, 0.32f, 0.07f, DB_AUTO, 0, a.autoCam ? "AUTO CAMERA: ON" : "AUTO CAMERA: OFF", a.autoCam});
    v.push_back({0.0f, 0.33f, 0.34f, 0.07f, DB_ADD, 0, "ADD CAMERA HERE", false});
    v.push_back({0.38f, 0.33f, 0.24f, 0.07f, DB_DONE, 0, "DONE", false});
    v.push_back({-0.18f, 0.245f, 0.68f, 0.06f, DB_TEAM, 0, "TEAM: " + (a.team.empty() ? std::string("NONE (TAP TO SET)") : a.team), false});
    v.push_back({0.38f, 0.245f, 0.24f, 0.06f, DB_SHARE, 0, "SHARE", false});
    static const char* hold[4] = {"HOLD 2S", "HOLD 4S", "HOLD 8S", "HOLD 15S"};
    static const char* trig[3] = {"TIMER", "ACTION", "FOLLOW"};
    for (int i = 0; i < (int)a.warps.size(); ++i) {
        const float y = 0.15f - 0.068f * i;
        char b[24];
        snprintf(b, sizeof b, "CAM %d  GO", i + 1);
        v.push_back({-0.44f, y, 0.20f, 0.056f, DB_GO, i, b, i == a.warpIdx});
        v.push_back({-0.235f, y, 0.18f, 0.056f, DB_HOLD, i, hold[a.warps[i].hold & 3], false});
        v.push_back({-0.04f, y, 0.19f, 0.056f, DB_TRANS, i, a.warps[i].trans ? "CROSS-FADE" : "CUT", false});
        v.push_back({0.16f, y, 0.19f, 0.056f, DB_TRIG, i, trig[std::min<int>(2, a.warps[i].trig)], false});
        v.push_back({0.40f, y, 0.20f, 0.056f, DB_DEL, i, "DELETE", false});
    }
    return v;
}

static void openDirector(App& a, const PlayerInput& in) {
    Vec3 f = in.head.q.rotate({0, 0, -1});
    f.y = 0;
    f = length(f) > 1e-3f ? normalize(f) : Vec3{0, 0, -1};
    const Vec3 right = cross(f, Vec3{0, 1, 0});
    Vec3 pp = in.head.p + f * 0.95f - right * 0.35f + Vec3{0, -0.22f, 0};
    Vec3 to = in.head.p - pp;
    to.y = 0;
    to = normalize(to);
    a.dirPose = {Quat::axisAngle({0, 1, 0}, std::atan2(to.x, to.z)), pp};
    a.dirOpen = true;
    a.menuOpen = false;
    a.prevDirTrig = true;
}

static void dirActivate(App& a, const DirBtn& b) {
    const double t = nowSeconds();
    switch (b.act) {
    case DB_AUTO: a.autoCam = !a.autoCam && !a.warps.empty(); a.autoNext = t + 0.1; break;
    case DB_ADD: addCamHere(a, a.localIn); break;
    case DB_DONE: a.dirOpen = false; a.infoKind = -1; break;
    case DB_TEAM: openMenu(a, a.localIn); a.nameEdit = true; a.editTarget = 2; a.nameBuf = a.team; break;
    case DB_SHARE:
        if (a.team.empty()) { openMenu(a, a.localIn); a.nameEdit = true; a.editTarget = 2; a.nameBuf.clear(); }
        else { a.net.shareCameras(a.team.c_str(), packWarps(a)); a.net.noticeLocal("CAMERAS SHARED WITH YOUR TEAM"); }
        break;
    case DB_GO: if (b.idx < (int)a.warps.size()) { a.autoCam = false; a.dirRequest = -1; startWarp(a, a.warps[b.idx].pose); a.warpIdx = b.idx; } break;
    case DB_HOLD: a.warps[b.idx].hold = (a.warps[b.idx].hold + 1) & 3; a.infoKind = 0; a.infoStart = t; saveWarps(a); break;
    case DB_TRANS: a.warps[b.idx].trans ^= 1; a.infoKind = a.warps[b.idx].trans ? 2 : 1; a.infoStart = t; saveWarps(a); break;
    case DB_TRIG: a.warps[b.idx].trig = (a.warps[b.idx].trig + 1) % 3; a.infoKind = 3 + a.warps[b.idx].trig; a.infoStart = t; saveWarps(a); break;
    case DB_DEL:
        a.warps.erase(a.warps.begin() + b.idx);
        if (a.warpIdx >= (int)a.warps.size()) a.warpIdx = (int)a.warps.size() - 1;
        saveWarps(a);
        break;
    case DB_INFOCLOSE: a.infoKind = -1; break;
    default: break;
    }
}

static Pose infoPose(const App& a) { return a.dirPose * Pose{Quat{}, {kDirW * 0.5f + 0.42f, 0.02f, 0.06f}}; }

static void updateDirector(App& a, PlayerInput& in) {
    a.dirHover = -1;
    a.dirHitValid = false;
    if (!a.dirOpen || a.menuOpen) return;
    const bool trig = in.hand[a.pointerHand].trigger > 0.7f;
    if (a.aimValid[a.pointerHand]) {
        Vec3 o = a.aim[a.pointerHand].p, d = a.aim[a.pointerHand].q.rotate({0, 0, -1});
        auto hitPlane = [&](const Pose& P, Vec3& lp) {
            Vec3 nz = P.q.rotate({0, 0, 1});
            float den = dot(d, nz);
            if (den > -1e-3f) return -1.f;
            float t = dot(P.p - o, nz) / den;
            if (t <= 0 || t > 4) return -1.f;
            lp = P.q.conj().rotate(o + d * t - P.p);
            return t;
        };
        Vec3 lp;
        float t = hitPlane(a.dirPose, lp);
        if (t > 0 && std::fabs(lp.x) < kDirW * 0.5f && std::fabs(lp.y) < kDirH * 0.5f) {
            a.dirHit = o + d * t; a.dirHitValid = true;
            std::vector<DirBtn> bs = dirButtons(a);
            for (size_t i = 0; i < bs.size(); ++i)
                if (std::fabs(lp.x - bs[i].x) < bs[i].w * 0.5f && std::fabs(lp.y - bs[i].y) < bs[i].h * 0.5f) a.dirHover = (int)i;
            if (trig && !a.prevDirTrig && a.dirHover >= 0) { dirActivate(a, bs[a.dirHover]); haptic(a, a.pointerHand, 0.35f, 0.025f); }
        } else if (a.infoKind >= 0) {   // the info panel's close button
            float t2 = hitPlane(infoPose(a), lp);
            if (t2 > 0 && std::fabs(lp.x - 0.28f) < 0.05f && std::fabs(lp.y - 0.27f) < 0.03f) {
                a.dirHit = o + d * t2; a.dirHitValid = true; a.dirHover = 1000;
                if (trig && !a.prevDirTrig) a.infoKind = -1;
            }
        }
    }
    a.prevDirTrig = trig;
    for (int h = 0; h < 2; ++h) in.hand[h].trigger = 0;
}

static const char* kInfoTitle[6] = {"HOLD TIME", "CUT", "CROSS-FADE", "TIMER CAMERA", "ACTION CAMERA", "FOLLOW CAMERA"};
static const char* kInfoText[6][2] = {
    {"HOW LONG THE AUTO CAMERA STAYS ON THIS", "CAMERA BEFORE MOVING TO THE NEXT ONE"},
    {"SWITCHES INSTANTLY TO THIS CAMERA", "SHARP, GOOD FOR FAST ACTION"},
    {"THE OLD VIEW FADES INTO THIS CAMERA", "SMOOTH, EASY TO WATCH"},
    {"TAKES ITS TURN WITH THE OTHER TIMER", "AND FOLLOW CAMERAS, IN ORDER"},
    {"JUMPS HERE WHEN A HIT, STRIKE OR MATCH", "END HAPPENS NEAR THIS CAMERA"},
    {"GLIDES ALONG THE COURT WITH THE BALL", "YOU STILL CHOOSE WHERE TO LOOK"}};

// ---- the info preview: a 6 s loop rendered into a texture. 0-3 s: a wide view showing the camera's path with a
// demo ball; 3-6 s: what that camera sees. The demo is scripted so it works with or without a live match.
static Vec3 demoBall(float u) {   // u 0..1: one throw down the court, curving, hitting at the far end
    return {-6.f + 12.f * u, 1.3f + 0.55f * std::sin(kPi * u), 0.8f * std::sin(2.f * kPi * u) * (1.f - u)};
}

// camera pose for the demo at u, and whether it has switched (for cut / fade); kind = info option
static Pose demoCam(int kind, float u, int& shot) {
    auto look = [](Vec3 from, Vec3 at) {
        Vec3 f = normalize(at - from);
        float yaw = std::atan2(-f.x, -f.z), pitch = std::asin(clampf(f.y, -1, 1));
        return Pose{Quat::axisAngle({0, 1, 0}, yaw) * Quat::axisAngle({1, 0, 0}, pitch), from};
    };
    shot = 0;
    if (kind == 5) {   // follow: dolly along the side with the ball, looking at it
        Vec3 b = demoBall(u), lag = demoBall(std::max(0.f, u - 0.08f));   // trails the ball a little
        return look({lag.x - 1.0f, 2.0f, 3.6f}, b);
    }
    if (kind == 4) {   // action: wide until the hit near the end, then the action camera by the hit
        if (u < 0.9f) return look({0, 3.4f, 5.2f}, {0, 1.2f, 0});
        shot = 1;
        return look({5.4f, 1.7f, 2.3f}, demoBall(0.95f));
    }
    // timer / hold / cut / cross-fade: camera A, then camera B halfway
    if (u < 0.5f) return look({-3.f, 2.2f, 3.6f}, {0, 1.3f, 0});
    shot = 1;
    return look({4.f, 2.0f, -3.4f}, {0, 1.3f, 0});
}

static void ensureRtt(App& a) {
    if (a.rttFbo[0]) return;
    glGenRenderbuffers(1, &a.rttDepth);
    glBindRenderbuffer(GL_RENDERBUFFER, a.rttDepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 640, 360);
    for (int k = 0; k < 2; ++k) {
        glGenTextures(1, &a.rttTex[k]);
        glBindTexture(GL_TEXTURE_2D, a.rttTex[k]);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, 640, 360);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glGenFramebuffers(1, &a.rttFbo[k]);
        glBindFramebuffer(GL_FRAMEBUFFER, a.rttFbo[k]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, a.rttTex[k], 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, a.rttDepth);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void drawWorld(App& a, int pass);

static void renderView(App& a, int fbo, const Pose& cam, float fovDeg, float u, bool showRig, const Pose& rig, int kind) {
    glBindFramebuffer(GL_FRAMEBUFFER, a.rttFbo[fbo]);
    glViewport(0, 0, 640, 360);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glClearColor(0.01f, 0.012f, 0.02f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    const float tv = std::tan(deg2rad(fovDeg) * 0.5f), th = tv * 16.f / 9.f;
    a.gfx.beginEye(Mat4::viewFromPose(cam), Mat4::projection(-std::atan(th), std::atan(th), std::atan(tv), -std::atan(tv), 0.05f, 2000.f), cam.p);
    const Pose keepHead = a.viewHead;
    a.viewHead = cam;   // sprites face the preview camera
    a.rttPass = true;
    drawWorld(a, 0);
    drawWorld(a, 1);
    a.rttPass = false;
    // the demo ball with its trail, and (wide view) the camera rig and its path
    const float ball[4] = {0.3f, 0.85f, 1.0f, 1}, core[4] = {1, 1, 1, 1};
    Vec3 b = demoBall(u);
    a.gfx.drawSprite(b, 0.35f, ball, cam.p, 0);
    a.gfx.drawSprite(b, 0.12f, core, cam.p, 0);
    Vec3 trail[16];
    for (int i = 0; i < 16; ++i) trail[i] = demoBall(std::max(0.f, u - 0.012f * (15 - i)));
    a.gfx.drawRibbon(trail, 16, 0.06f, ball);
    if (kind == 4 && u > 0.9f) { const float hit[4] = {1.0f, 0.6f, 0.3f, 1}; a.gfx.drawSprite(demoBall(0.95f), 1.2f * (u - 0.88f) * 8, hit, cam.p, 1); }
    if (showRig) {
        const float camCol[4] = {1.0f, 0.85f, 0.2f, 1};
        a.gfx.drawBox(Mat4::fromPose(rig, {0.18f, 0.12f, 0.22f}), camCol, true);
        a.gfx.drawBox(Mat4::fromPose(rig * Pose{Quat{}, {0, 0, -0.3f}}, {0.08f, 0.08f, 0.12f}), camCol, true);
        Vec3 path[24];
        int dummy;
        for (int i = 0; i < 24; ++i) path[i] = demoCam(kind, i / 23.f, dummy).p;
        a.gfx.drawRibbon(path, 24, 0.05f, camCol);
    }
    a.viewHead = keepHead;
}

// called before the eye views each frame while the info panel is open
static void renderInfoPreview(App& a) {
    if (!a.dirOpen || a.infoKind < 0) return;
    ensureRtt(a);
    const float t = (float)std::fmod(nowSeconds() - a.infoStart, 6.0);
    const bool wide = t < 3.f;
    const float u = std::fmod(t, 3.f) / 3.f;
    int shot;
    Pose cam = demoCam(a.infoKind, u, shot);
    if (wide) {
        Pose over{Quat::axisAngle({0, 1, 0}, 0) * Quat::axisAngle({1, 0, 0}, -0.62f), {0, 10.5f, 13.0f}};
        renderView(a, 0, over, 60, u, true, cam, a.infoKind);
    } else {
        renderView(a, 0, cam, 70, u, false, cam, a.infoKind);
        // cross-fade preview: blend the previous shot over the new one for 0.5 s after a switch
        const bool fade = a.infoKind == 2 || (a.infoKind >= 3 && a.infoKind != 1);
        const float uSwitch = a.infoKind == 4 ? 0.9f : 0.5f;
        if (a.infoKind != 5 && fade && shot == 1 && u < uSwitch + 0.17f) {
            int s0;
            Pose prev = demoCam(a.infoKind, uSwitch - 0.01f, s0);
            renderView(a, 1, prev, 70, u, false, prev, a.infoKind);
            glBindFramebuffer(GL_FRAMEBUFFER, a.rttFbo[0]);
            glViewport(0, 0, 640, 360);
            a.gfx.drawScreenTexture(a.rttTex[1], 1.f - (u - uSwitch) / 0.17f);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void drawDirector(App& a) {
    if (!a.dirOpen || a.rttPass) return;
    auto at = [&](const Pose& P, float x, float y, float z) { return P.transform({x, y, z}); };
    auto quad = [&](const Pose& P, float x, float y, float w, float h, float z, const float* c) {
        a.gfx.drawPanel(Mat4::fromPose({P.q, at(P, x, y, z)}, {w, h, 1}), c);
    };
    auto text = [&](const Pose& P, const std::string& t, float x, float y, float h, const float* c) {
        a.gfx.drawText(t, {P.q, at(P, x, y, 0.006f)}, h, c);
    };
    const float bg[4] = {0.004f, 0.007f, 0.016f, 0.92f}, frame[4] = {0.55f, 0.42f, 0.08f, 0.9f};
    const float btn[4] = {0.03f, 0.05f, 0.08f, 0.95f}, hov[4] = {0.75f, 0.55f, 0.10f, 0.98f}, on[4] = {0.10f, 0.35f, 0.55f, 0.98f};
    const float white[4] = {1, 1, 1, 1}, grey[4] = {0.5f, 0.55f, 0.62f, 1}, gold[4] = {1.0f, 0.82f, 0.3f, 1};
    const Pose& P = a.dirPose;
    quad(P, 0, 0, kDirW + 0.03f, kDirH + 0.03f, -0.002f, frame);
    quad(P, 0, 0, kDirW, kDirH, 0, bg);
    text(P, "DIRECTOR TOOLS", 0, 0.43f, 0.04f, gold);
    std::vector<DirBtn> bs = dirButtons(a);
    for (size_t i = 0; i < bs.size(); ++i) {
        quad(P, bs[i].x, bs[i].y, bs[i].w, bs[i].h, 0.003f, (int)i == a.dirHover ? hov : bs[i].on ? on : btn);
        text(P, bs[i].label, bs[i].x, bs[i].y, bs[i].h > 0.06f ? 0.024f : 0.019f, white);
    }
    if (a.warps.empty()) {
        text(P, "NO CAMERAS YET: FLY TO A SPOT AND PRESS", 0, 0.12f, 0.022f, grey);
        text(P, "ADD CAMERA HERE (OR A ON THE CONTROLLER)", 0, 0.08f, 0.022f, grey);
    }
    text(P, "CLICK AN OPTION TO CHANGE IT AND SEE WHAT IT DOES", 0, -0.445f, 0.02f, grey);
    // info panel with the preview
    if (a.infoKind >= 0) {
        const Pose I = infoPose(a);
        quad(I, 0, 0, 0.74f, 0.66f, -0.002f, frame);
        quad(I, 0, 0, 0.71f, 0.63f, 0, bg);
        text(I, kInfoTitle[a.infoKind], -0.03f, 0.27f, 0.034f, gold);
        quad(I, 0.28f, 0.27f, 0.09f, 0.05f, 0.003f, a.dirHover == 1000 ? hov : btn);
        text(I, "X", 0.28f, 0.27f, 0.026f, white);
        text(I, kInfoText[a.infoKind][0], 0, 0.21f, 0.02f, white);
        text(I, kInfoText[a.infoKind][1], 0, 0.175f, 0.02f, white);
        const float img[4] = {1, 1, 1, 0.999f};
        if (a.rttTex[0]) {
            Mat4 m = Mat4::fromPose({I.q, at(I, 0, -0.05f, 0.004f)}, {0.64f, 0.36f, 1});
            a.gfx.drawTexturedPanel(m, a.rttTex[0], img);
        }
        const float t = (float)std::fmod(nowSeconds() - a.infoStart, 6.0);
        text(I, t < 3.f ? "1  WIDE VIEW: WHAT THE CAMERA DOES" : "2  WHAT THE CAMERA SEES", 0, -0.27f, 0.02f, grey);
    }
    // laser + cursor
    if (a.aimValid[a.pointerHand]) {
        Vec3 o = a.aim[a.pointerHand].p, d = a.aim[a.pointerHand].q.rotate({0, 0, -1});
        float len = a.dirHitValid ? length(a.dirHit - o) : 1.5f;
        const float laser[4] = {1.0f, 0.75f, 0.2f, 1};
        a.gfx.drawBox(Mat4::fromPose({a.aim[a.pointerHand].q, o + d * (len * 0.5f)}, {0.003f, 0.003f, len * 0.5f}), laser, true);
        if (a.dirHitValid) a.gfx.drawSphere(a.dirHit, 0.008f, gold, true);
    }
}

// ------------------------------------------------------------------ the arena room: waiting players, queue, spectators
static const float kRed[4] = {1.0f, 0.18f, 0.12f, 1}, kBlue[4] = {0.15f, 0.45f, 1.0f, 1};

static bool insideTank(const Vec3& p) { return std::fabs(p.x) < 9.9f && std::fabs(p.z) < 2.9f; }

static const RoomInfo* currentRoom(const App& a) {
    if (a.net.inRoom()) return &a.net.room();
    if (a.spectating && a.watchIp) return &a.watchRoom;
    return nullptr;
}

// kiosk in front of the local waiting player's spot: JOIN QUEUE / LEAVE QUEUE
static Pose queueKioskPose(const App& a) {
    Vec3 s = holdingSpot(a.net.localSpot());
    return {Quat::axisAngle({0, 1, 0}, kPi), {s.x, 0.95f, s.z + 0.55f}};   // quad +Z faces the player (-Z side)
}

static void updateQueueKiosk(App& a, const PlayerInput& in) {
    a.queueHover = -1;
    // the menu takes the laser only where it is pointing at the menu (it can be open in front of the kiosk)
    if (!a.net.localWaiting() || (a.menuOpen && a.menuHitValid) || !a.aimValid[a.pointerHand]) { a.prevQueueTrig = true; return; }
    const Pose k = queueKioskPose(a);
    Vec3 o = a.aim[a.pointerHand].p, d = a.aim[a.pointerHand].q.rotate({0, 0, -1});
    Vec3 nz = k.q.rotate({0, 0, 1});
    float den = dot(d, nz);
    if (den < -1e-3f) {
        float t = dot(k.p - o, nz) / den;
        Vec3 lp = k.q.conj().rotate(o + d * t - k.p);
        if (t > 0 && t < 3 && std::fabs(lp.x) < 0.25f && std::fabs(lp.y) < 0.08f) a.queueHover = 0;
    }
    // pressing it with either hand counts too (the button is at hand height in front of the spot)
    bool poked = false;
    for (int h = 0; h < 2; ++h) {
        if (!in.hand[h].tracked) continue;
        Vec3 lp = k.q.conj().rotate(in.hand[h].grip.p - k.p);
        if (std::fabs(lp.x) < 0.22f && std::fabs(lp.y) < 0.08f && lp.z < 0.04f && lp.z > -0.10f) poked = true;
    }
    const bool trig = a.rawTrigger[a.pointerHand] > 0.7f;   // the open menu zeroes the game's trigger
    const bool press = (trig && !a.prevQueueTrig && a.queueHover == 0) || (poked && !a.prevQueuePoke);
    if (press) {
        const bool join = a.net.localQueuePos() < 0;
        LOGI("queue kiosk: %s (%s)", join ? "JOIN QUEUE" : "LEAVE QUEUE", poked ? "pressed by hand" : "trigger");
        a.net.queue(join);
        haptic(a, a.pointerHand, 0.4f, 0.03f);
    }
    a.prevQueueTrig = trig;
    a.prevQueuePoke = poked;
}

// ---------------------------------------------------------------- the lobby
static void startLobbyMatch(App& a) {
    a.game.reset(true);
    a.game.pl[0].isBot = true;
    a.game.pl[0].botLevel = a.game.pl[1].botLevel = 3;   // (ALL-STARS: a good match to watch, whatever practice is set to)
    a.game.pl[0].in.avatar = avatar::preset(10);
    a.game.pl[1].in.avatar = avatar::preset(12);
    a.lobbySummaryT = 0;
    LOGI("lobby: bots playing");
}

struct Poster { const char* title; const char* line1; const char* line2; float ang; };
static const Poster kPosters[] = {
    {"PLAY THE BOT", "POINT AT THE COURT", "AND PULL THE TRIGGER", -75},
    {"MULTIPLAYER", "HOST OR JOIN A ROOM", "ONLINE OR ON YOUR WI-FI", -105},
    {"HOW TO PLAY", "THROW, CURVE, PUNCH", "AND SHIELD", -135},
    {"CUSTOMIZE", "YOUR LOOK: OUTFITS,", "HAIR, GAUNTLETS", 75},
    {"SPECTATE", "WATCH LIVE MATCHES", "AND DIRECT CAMERAS", 105},
    {"SETTINGS", "VOLUME, VOICE,", "SERVER AND RULES", 135},
};
static const int kPosterCount = (int)(sizeof kPosters / sizeof kPosters[0]);

static Pose posterPose(int i) {
    const float ang = deg2rad(kPosters[i].ang), R = 1.4f;
    const Vec3 p = kLobbySpot + Vec3{R * std::sin(ang), 1.40f, -R * std::cos(ang)};
    return {Quat::axisAngle({0, 1, 0}, -ang), p};   // quad +Z faces the middle of the balcony
}

// pointer ray against the posters and the court; -1 nothing, 0..kPosterCount-1 a poster, kPosterCount the court
static int lobbyRayHit(const App& a, const Vec3& o, const Vec3& d, float& dist) {
    int hit = -1;
    dist = 1e9f;
    for (int i = 0; i < kPosterCount; ++i) {
        const Pose k = posterPose(i);
        const Vec3 nz = k.q.rotate({0, 0, 1});
        const float den = dot(d, nz);
        if (den > -1e-3f) continue;
        const float t = dot(k.p - o, nz) / den;
        const Vec3 lp = k.q.conj().rotate(o + d * t - k.p);
        if (t > 0 && t < dist && std::fabs(lp.x) < 0.33f && std::fabs(lp.y) < 0.27f) { dist = t; hit = i; }
    }
    // the court (the glass tank): slab test
    const Vec3 lo{-9.45f, 0.f, -2.45f}, hi{9.45f, 3.6f, 2.45f};
    float t0 = 0.f, t1 = 60.f;
    const float* po = &o.x; const float* pd = &d.x; const float* pl = &lo.x; const float* ph = &hi.x;
    bool inside = true;
    for (int k = 0; k < 3 && inside; ++k) {
        if (std::fabs(pd[k]) < 1e-6f) { if (po[k] < pl[k] || po[k] > ph[k]) inside = false; continue; }
        float ta = (pl[k] - po[k]) / pd[k], tb = (ph[k] - po[k]) / pd[k];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta); t1 = std::min(t1, tb);
        if (t0 > t1) inside = false;
    }
    if (inside && t0 < dist) { dist = t0; hit = kPosterCount; }
    return hit;
}

static void lobbyAction(App& a, int which, const PlayerInput& in) {
    LOGI("lobby: %s", which == kPosterCount ? "the court" : kPosters[which].title);
    auto menuPage = [&]() {
        openMenu(a, in);
        a.titleOpen = a.settingsOpen = a.voicePage = a.ptPage = a.gfxPage = a.helpOpen = a.xpOpen = a.rulesOpen = a.bugOpen = a.reportOpen = false;
    };
    switch (which) {
    case 0: case kPosterCount: menuPage(); a.rulesOpen = true; a.rulesForHost = false; break;   // PLAY THE BOT / the court
    case 1: menuPage(); break;                                                       // MULTIPLAYER page
    case 2: menuPage(); a.helpOpen = true; break;
    case 3: if (!a.avModels.empty()) openBuilder(a, in); break;
    case 4: a.net.spectate(); a.watchIp = 0; break;
    case 5: menuPage(); a.settingsOpen = true; break;
    default: break;
    }
}

static void updateLobby(App& a, const PlayerInput& in) {
    a.lobbyHover = -1;
    if (!inLobby(a) || a.builderOpen || a.tuneOpen || (a.menuOpen && a.menuHitValid) || !a.aimValid[a.pointerHand]) {
        a.prevLobbyTrig = true;
        return;
    }
    float dist;
    a.lobbyHover = lobbyRayHit(a, a.aim[a.pointerHand].p, a.aim[a.pointerHand].q.rotate({0, 0, -1}), dist);
    if (a.menuOpen && a.lobbyHover == kPosterCount) a.lobbyHover = -1;   // the open menu sits in front of the court
    // posters can be pressed by hand too
    int poked = -1;
    for (int h = 0; h < 2 && poked < 0; ++h) {
        if (!in.hand[h].tracked) continue;
        for (int i = 0; i < kPosterCount; ++i) {
            const Pose k = posterPose(i);
            const Vec3 lp = k.q.conj().rotate(in.hand[h].grip.p - k.p);
            if (std::fabs(lp.x) < 0.31f && std::fabs(lp.y) < 0.25f && lp.z < 0.05f && lp.z > -0.12f) poked = i;
        }
    }
    const bool trig = a.rawTrigger[a.pointerHand] > 0.7f;   // works with the main menu open (it opens on arrival)
    if (trig && !a.prevLobbyTrig && a.lobbyHover >= 0) { lobbyAction(a, a.lobbyHover, in); haptic(a, a.pointerHand, 0.4f, 0.03f); }
    else if (poked >= 0 && !a.prevLobbyPoke) { lobbyAction(a, poked, in); haptic(a, a.pointerHand, 0.4f, 0.03f); }
    a.prevLobbyTrig = trig;
    a.prevLobbyPoke = poked >= 0;
}

static void drawLobby(App& a, int pass) {
    if (!inLobby(a)) return;
    // your own hands (the court only draws the two bots' hands)
    const AvatarLook look = lookOf(a, a.myAvatar, 2);
    for (int h = 0; h < 2; ++h)
        if (a.localIn.hand[h].tracked)
            drawLancerAt(a, Mat4::fromPose(a.localIn.hand[h].grip), h, look, pass, Mat4::identity(), false, a.myAvatar);
    if (pass != 1) return;
    const float cyan[4] = {0.3f, 0.85f, 1.0f, 1}, white[4] = {1, 1, 1, 1}, grey[4] = {0.62f, 0.68f, 0.76f, 1};
    const float deck[4] = {0.02f, 0.03f, 0.05f, 0.92f}, glow[4] = {0.10f, 0.55f, 0.85f, 1};
    // the balcony: a dark deck with glowing edges and a front rail
    const Vec3 c{0, kLobbyFloor, kLobbySpot.z - 0.4f};
    a.gfx.drawPanel(Mat4::fromPose({Quat::axisAngle({1, 0, 0}, -kPi * 0.5f), c + Vec3{0, -0.005f, 0}}, {5.4f, 2.6f, 1}), deck);
    // (the unit box spans -1..1: its scale is a half size)
    a.gfx.drawBox(Mat4::fromPose({Quat{}, c + Vec3{0, 0.0f, -1.3f}}, {2.7f, 0.004f, 0.004f}), glow, true);
    a.gfx.drawBox(Mat4::fromPose({Quat{}, c + Vec3{0, 1.0f, -1.3f}}, {2.7f, 0.006f, 0.006f}), glow, true);   // the rail
    for (float x : {-2.7f, 2.7f}) a.gfx.drawBox(Mat4::fromPose({Quat{}, c + Vec3{x, 0.0f, 0}}, {0.004f, 0.004f, 1.3f}), glow, true);
    for (float x : {-2.6f, -1.3f, 0.f, 1.3f, 2.6f}) a.gfx.drawBox(Mat4::fromPose({Quat{}, c + Vec3{x, 0.5f, -1.3f}}, {0.004f, 0.5f, 0.004f}), glow, true);
    const float ring[4] = {0.15f, 0.6f, 0.9f, 1};
    a.gfx.drawSpriteFlat({kLobbySpot.x, kLobbyFloor + 0.006f, kLobbySpot.z}, 1.0f, ring, {0, 1, 0}, 1);
    // the rail sign: the two things everyone needs
    const Pose sign{Quat{}, c + Vec3{0, 0.82f, -1.31f}};
    a.gfx.drawPanel(Mat4::fromPose(sign, {2.3f, 0.2f, 1}), deck);
    a.gfx.drawText("LEFT MENU BUTTON: MAIN MENU    -    HOLD THE META BUTTON: RECENTER", sign * Pose{Quat{}, {0, 0.03f, 0.004f}}, 0.035f, white);
    a.gfx.drawText("POINT AT A POSTER OR THE COURT AND PULL THE TRIGGER", sign * Pose{Quat{}, {0, -0.04f, 0.004f}}, 0.026f, grey);
    // posters on stands
    for (int i = 0; i < kPosterCount; ++i) {
        const Pose k = posterPose(i);
        const bool hov = a.lobbyHover == i;
        const float frame[4] = {hov ? 0.4f : 0.08f, hov ? 0.9f : 0.42f, hov ? 1.0f : 0.62f, 1};
        a.gfx.drawPanel(Mat4::fromPose(k * Pose{Quat{}, {0, 0, -0.003f}}, {0.66f, 0.54f, 1}), frame);
        a.gfx.drawPanel(Mat4::fromPose(k, {0.62f, 0.50f, 1}), deck);
        a.gfx.drawText(kPosters[i].title, k * Pose{Quat{}, {0, 0.12f, 0.004f}}, 0.065f, hov ? white : cyan);
        a.gfx.drawText(kPosters[i].line1, k * Pose{Quat{}, {0, -0.02f, 0.004f}}, 0.030f, white);
        a.gfx.drawText(kPosters[i].line2, k * Pose{Quat{}, {0, -0.08f, 0.004f}}, 0.030f, white);
        a.gfx.drawText(hov ? "PULL THE TRIGGER" : "POINT OR TOUCH", k * Pose{Quat{}, {0, -0.19f, 0.004f}}, 0.022f, hov ? cyan : grey);
        a.gfx.drawBox(Mat4::fromPose({Quat{}, Vec3{k.p.x, kLobbyFloor + (1.40f - 0.27f) * 0.5f, k.p.z}}, {0.006f, (1.40f - 0.27f) * 0.5f, 0.006f}), glow, true);   // the stand, up to the poster's lower edge
    }
    // the court as a button
    if (a.lobbyHover == kPosterCount) {
        const float gold[4] = {1.0f, 0.85f, 0.3f, 1};
        a.gfx.drawText("PULL THE TRIGGER TO PLAY THE BOT", {Quat{}, {0, 3.95f, 2.6f}}, 0.22f, gold);
    }
    // the pointer
    if (!a.menuOpen && !a.builderOpen && !a.tuneOpen && a.aimValid[a.pointerHand]) {
        const Vec3 o = a.aim[a.pointerHand].p, d = a.aim[a.pointerHand].q.rotate({0, 0, -1});
        float dist;
        const int hit = lobbyRayHit(a, o, d, dist);
        const float len = hit >= 0 ? std::min(dist, 25.f) : 2.0f;
        const float laser[4] = {0.3f, 0.8f, 1.0f, 1};
        a.gfx.drawBox(Mat4::fromPose({a.aim[a.pointerHand].q, o + d * (len * 0.5f)}, {0.003f, 0.003f, len * 0.5f}), laser, true);
        if (hit >= 0) a.gfx.drawSphere(o + d * len, hit == kPosterCount ? 0.05f : 0.008f, laser, true);
    }
}

static void drawRoomPeople(App& a, int pass) {
    const RoomInfo* rm = currentRoom(a);
    if (!rm) return;
    const bool inTank = a.net.inRoom() && !a.net.localWaiting();
    const uint32_t me = a.net.session();
    // waiting players stand in the holding area: full avatars, a neutral rim light; the players may hide them
    if (!(inTank && a.hideOutside)) {
        for (const RoomInfo::Member& m : rm->members) {
            if (m.id == me || !m.in.headTracked || insideTank(m.in.head.p)) continue;
            if (!a.avModels.empty()) drawAvatarBody(a, m.in, 2, pass, Mat4::identity(), false, false, 0, 1000 + int(m.id & 0xFFFFF));
            const AvatarLook look = lookOf(a, m.in.avatar, 2);
            for (int h = 0; h < 2; ++h)
                if (m.in.hand[h].tracked && !insideTank(m.in.hand[h].grip.p))
                    drawLancerAt(a, Mat4::fromPose(m.in.hand[h].grip), h, look, pass, Mat4::identity(), false, m.in.avatar);
            if (pass == 1 && m.queuePos >= 0) {
                Vec3 f = normalize(Vec3{a.viewHead.p.x - m.in.head.p.x, 0, a.viewHead.p.z - m.in.head.p.z});
                a.gfx.drawText(m.queuePos == 0 ? "NEXT" : "2ND", {Quat::axisAngle({0, 1, 0}, std::atan2(f.x, f.z)), m.in.head.p + Vec3{0, 0.32f, 0}},
                               0.07f, kRed);
            }
        }
    }
    // your own hands while you wait outside (the tank only draws the two players' hands), and a pointer for the kiosk
    if (a.net.localWaiting() && pass == 1 && !a.menuOpen && a.aimValid[a.pointerHand]) {
        const Pose k = queueKioskPose(a);
        Vec3 o = a.aim[a.pointerHand].p, d = a.aim[a.pointerHand].q.rotate({0, 0, -1});
        Vec3 nz = k.q.rotate({0, 0, 1});
        float len = 1.2f;
        const float den = dot(d, nz);
        if (a.queueHover == 0 && den < -1e-3f) len = dot(k.p - o, nz) / den;
        const float laser[4] = {1.0f, 0.35f, 0.25f, 1};
        a.gfx.drawBox(Mat4::fromPose({a.aim[a.pointerHand].q, o + d * (len * 0.5f)}, {0.003f, 0.003f, len * 0.5f}), laser, true);
        if (a.queueHover == 0) a.gfx.drawSphere(o + d * len, 0.008f, laser, true);
    }
    if (a.net.localWaiting()) {
        const AvatarLook look = lookOf(a, a.myAvatar, 2);
        for (int h = 0; h < 2; ++h)
            if (a.localIn.hand[h].tracked)
                drawLancerAt(a, Mat4::fromPose(a.localIn.hand[h].grip), h, look, pass, Mat4::identity(), false, a.myAvatar);
    }
    if (pass != 1) return;
    // the holding area: a glowing strip with four spots (everyone in the room sees it, players included)
    for (int s = 0; s < 4; ++s) {
        const float ring[4] = {0.35f, 0.08f, 0.06f, 1};
        Vec3 c = holdingSpot(s);
        a.gfx.drawSpriteFlat({c.x, 0.006f, c.z}, 1.1f, ring, {0, 1, 0}, 1);
    }
    // spectators near this arena: small blue lights, only for people outside the tank (never for the players)
    if (!inTank)
        for (const RoomInfo::Watcher& w : rm->watchers)
            if (w.where == 1 && w.id != me) a.gfx.drawSprite(w.pos, 0.09f, kBlue, a.viewHead.p, 0);
    // our own JOIN QUEUE kiosk
    if (a.net.localWaiting()) {
        const Pose k = queueKioskPose(a);
        const int q = a.net.localQueuePos();
        const float bg[4] = {0.02f, 0.02f, 0.03f, 0.9f}, btn[4] = {0.10f, 0.03f, 0.03f, 0.95f}, hov[4] = {0.75f, 0.15f, 0.10f, 0.98f};
        const float white[4] = {1, 1, 1, 1}, grey[4] = {0.55f, 0.6f, 0.68f, 1};
        a.gfx.drawPanel(Mat4::fromPose(k * Pose{Quat{}, {0, 0.03f, -0.002f}}, {0.46f, 0.20f, 1}), bg);
        a.gfx.drawPanel(Mat4::fromPose(k, {0.40f, 0.09f, 1}), a.queueHover == 0 ? hov : btn);
        a.gfx.drawText(q < 0 ? "JOIN QUEUE" : "LEAVE QUEUE", k * Pose{Quat{}, {0, 0, 0.004f}}, 0.04f, white);
        int queued = 0;
        for (const RoomInfo::Member& m : rm->members) queued += m.queuePos >= 0;
        char buf[48];
        if (q >= 0) snprintf(buf, sizeof buf, "YOU ARE %s IN LINE", q == 0 ? "NEXT" : "SECOND");
        else snprintf(buf, sizeof buf, "%d OF 2 IN THE QUEUE", queued);
        a.gfx.drawText(buf, k * Pose{Quat{}, {0, 0.085f, 0.004f}}, 0.022f, grey);
        a.gfx.drawText(a.game.tun.advanced ? "ADVANCED RULES" : "BASIC RULES", k * Pose{Quat{}, {0, -0.055f, 0.004f}}, 0.020f, grey);
    }
}

// ------------------------------------------------------------------ spectator hub: live miniature arenas
static void drawMiniMatch(App& a, const Game& g, const Vec3& at, const Quat& q, float s, int pass, bool hover, const RoomInfo* rmini = nullptr) {
    auto M = [&](const Mat4& local) { return Mat4::fromPose({q, at}, {s, s, s}) * local; };
    auto W = [&](const Vec3& p) { return at + q.rotate(p * s); };
    const float teamCol[2][4] = {{0.1f, 0.7f, 1.0f, 1}, {1.0f, 0.4f, 0.1f, 1}};
    DrawParams dp;
    a.gfx.draw(a.floor.valid() ? a.floor : a.arena, M(Mat4::identity()), dp, pass);
    a.gfx.draw(a.arena, M(Mat4::identity()), dp, pass);
    if (pass == 1) {
        DrawParams cp;
        if (hover) for (int k = 0; k < 3; ++k) cp.tintMul[k] = 1.6f;
        a.gfx.draw(a.courtFx, M(Mat4::identity()), cp, 1);
        DrawParams pp;
        pp.uvOff[1] = -float(nowSeconds() - a.startTime) * 0.9f;
        a.gfx.draw(a.courtPulse, M(Mat4::identity()), pp, 1);
    }
    if (pass == 1 && rmini) {   // the people in that arena's room: red = waiting / queued, blue = spectators there
        const uint32_t me = a.net.session();
        for (const RoomInfo::Member& m : rmini->members)
            if (m.id != me && m.in.headTracked) a.gfx.drawSprite(W(m.in.head.p), 0.035f, kRed, a.viewHead.p, 0);
        for (const RoomInfo::Watcher& w : rmini->watchers)
            if (w.where == 1 && w.id != me) a.gfx.drawSprite(W(w.pos), 0.03f, kBlue, a.viewHead.p, 0);
    }
    for (int p = 0; p < 2; ++p) {
        if (rmini && !rmini->slot[p] && !(p == 1 && rmini->bot)) continue;   // empty tank slot
        const PlayerState& ps = g.pl[p];
        DrawParams bp;
        for (int k = 0; k < 3; ++k) bp.tintMul[k] = 0.7f + 0.3f * teamCol[p][k];
        Pose hp = ps.in.head;
        if (hp.p.y > 0.3f && !a.avModels.empty()) {
            drawAvatarBody(a, ps.in, p, pass, M(Mat4::identity()), false, true, 0);
        } else if (hp.p.y > 0.3f) {
            Vec3 f = hp.q.rotate({0, 0, -1});
            float yaw = std::atan2(-f.x, -f.z);
            a.gfx.draw(a.torso, M(Mat4::fromPose({Quat::axisAngle({0, 1, 0}, yaw + kPi), {hp.p.x, hp.p.y - 1.66f, hp.p.z}})), bp, pass);
            Quat hq = hp.q * Quat::axisAngle({0, 1, 0}, kPi);
            a.gfx.draw(a.head, M(Mat4::fromPose({hq, hp.p - hq.rotate(Vec3{0, 1.66f, 0})})), bp, pass);
        }
        const AvatarLook look = lookOf(a, ps.in.avatar, p);
        for (int h = 0; h < 2; ++h) {
            if (!ps.in.hand[h].tracked) continue;
            Mat4 mirror = Mat4::identity();
            if (h == 0) mirror.m[0] = -1;
            DrawParams hp2 = bp;
            if (!a.avModels.empty()) { hp2 = DrawParams(); applyLook(hp2, look); }
            hp2.mirrored = (h == 0);
            drawGauntlet(a, ps.in.avatar, M(Mat4::fromPose(ps.in.hand[h].grip) * mirror), hp2, pass);
        }
        const Ball& b = ps.ball;
        if (pass == 1 && b.state != BallState::Dead && b.pos.y > 0.f && (rmini == nullptr || rmini->slot[p] || (p == 1 && rmini->bot))) {
            float c[4] = {teamCol[p][0] * 1.4f, teamCol[p][1] * 1.4f, teamCol[p][2] * 1.4f, 1};
            a.gfx.drawSprite(W(b.pos), g.ballRadius(p) * s * 5.0f, c, a.viewHead.p, 0);
            float w[4] = {0.9f, 0.9f, 0.9f, 1};
            a.gfx.drawSprite(W(b.pos), g.ballRadius(p) * s * 1.8f, w, a.viewHead.p, 0);
        }
    }
}

static void drawHub(App& a, int pass) {
    DrawParams dp;
    a.gfx.draw(a.environment, Mat4::identity(), dp, pass);
    if (a.floor.valid()) a.gfx.draw(a.floor, Mat4::identity(), dp, pass);   // stand on the polished arena floor
    std::vector<SpecMatch>& ms = a.net.matches();
    const int n = (int)ms.size();
    const float s = 1.f / 17.f;
    const Vec3 eye = a.viewHead.p;
    const float white[4] = {1, 1, 1, 1}, grey[4] = {0.55f, 0.6f, 0.68f, 1}, cyan[4] = {0.3f, 0.85f, 1.0f, 1};
    if (pass == 1) {   // the hub platform: a glowing disc in the dark arena
        const float pc[4] = {0.03f, 0.09f, 0.14f, 1}, rc[4] = {0.12f, 0.45f, 0.65f, 1};
        a.gfx.drawSpriteFlat({0, 0.004f, 0}, 5.0f, pc, {0, 1, 0}, 0);
        a.gfx.drawSpriteFlat({0, 0.006f, 0}, 5.4f, rc, {0, 1, 0}, 1);
    }
    for (int i = 0; i < n; ++i) {
        // an arc in front of the viewer (hub faces +X), up to 4 per row, rows step back and up
        int row = i / 4, col = i % 4, inRow = std::min(4, n - row * 4);
        float ang = inRow == 1 ? 0.f : deg2rad(-40.f + 80.f * col / (inRow - 1));
        float rad = 1.35f + 0.9f * row;
        Vec3 at{rad * std::cos(ang), 0.92f + 0.35f * row, rad * std::sin(ang)};
        Quat q = Quat::axisAngle({0, 1, 0}, -ang + kPi * 0.5f);   // long axis across the view
        bool hover = a.miniHover == i;
        if (pass == 0) {
            float ped[4] = {0.05f, 0.055f, 0.065f, 1};
            a.gfx.drawBox(Mat4::fromPose({q, at - Vec3{0, 0.025f, 0}}, {1.25f, 0.04f, 0.38f}), ped, false);
            a.gfx.drawBox(Mat4::fromPose({q, Vec3{at.x, at.y * 0.5f - 0.03f, at.z}}, {0.14f, at.y - 0.06f, 0.14f}), ped, false);
        }
        drawMiniMatch(a, ms[i].game, at, q, s, pass, hover, &ms[i].room);
        if (pass == 1) {
            const float ring[4] = {hover ? 0.4f : 0.08f, hover ? 0.9f : 0.3f, hover ? 1.0f : 0.45f, 1};
            a.gfx.drawSpriteFlat(at + Vec3{0, 0.002f, 0}, 1.5f, ring, {0, 1, 0}, 1);
            Vec3 lf = normalize(Vec3{eye.x - at.x, 0, eye.z - at.z});
            Quat lq = Quat::axisAngle({0, 1, 0}, std::atan2(lf.x, lf.z));
            char buf[64];
            const Game& g = ms[i].game;
            a.gfx.drawText(ms[i].name[0] ? ms[i].name : "LIVE GAME", {lq, at + Vec3{0, 0.46f, 0}}, 0.05f, hover ? cyan : white);
            if (g.tun.advanced) snprintf(buf, sizeof buf, "%s   %d  -  %d   ROUND %d", g.tun.shields ? "ADVANCED" : "ADVANCED HOTDOG",
                                         g.pl[0].score, g.pl[1].score, std::max(1, g.round));
            else snprintf(buf, sizeof buf, "%s   %d  -  %d   %s", g.tun.shields ? "BASIC" : "BASIC HOTDOG", g.pl[0].score, g.pl[1].score,
                          clockLine(g).c_str());
            a.gfx.drawText(buf, {lq, at + Vec3{0, 0.39f, 0}}, 0.04f, grey);
        }
    }
    if (pass == 1) {   // other spectators in the lobby: blue lights where they stand (each host lists them)
        std::vector<uint32_t> seen;
        for (const SpecMatch& m : ms)
            for (const RoomInfo::Watcher& w : m.room.watchers) {
                if (w.where != 0 || w.id == a.net.session() || std::find(seen.begin(), seen.end(), w.id) != seen.end()) continue;
                seen.push_back(w.id);
                a.gfx.drawSprite(w.pos, 0.09f, kBlue, a.viewHead.p, 0);
            }
    }
    if (pass == 1) {
        Quat fq = Quat::axisAngle({0, 1, 0}, -kPi * 0.5f);   // facing a viewer looking +X
        a.gfx.drawText("SPECTATOR HUB", {fq, {2.4f, 2.1f, 0}}, 0.14f, cyan);
        if (n == 0) {
            a.gfx.drawText("NO LIVE GAMES ON YOUR NETWORK YET", {fq, {2.4f, 1.6f, 0}}, 0.07f, white);
            a.gfx.drawText("WHEN A PLAYER HOSTS A GAME IT APPEARS HERE", {fq, {2.4f, 1.45f, 0}}, 0.05f, grey);
        } else {
            a.gfx.drawText("POINT AT AN ARENA AND PULL THE TRIGGER TO WATCH IT", {fq, {2.4f, 1.85f, 0}}, 0.045f, grey);
        }
        // pointer laser in the hub
        if (a.aimValid[a.pointerHand] && !a.menuOpen) {
            Vec3 o = a.aim[a.pointerHand].p, d = a.aim[a.pointerHand].q.rotate({0, 0, -1});
            const float laser[4] = {0.12f, 0.45f, 0.8f, 1};
            a.gfx.drawBox(Mat4::fromPose({a.aim[a.pointerHand].q, o + d * 1.0f}, {0.0025f, 0.0025f, 2.0f}), laser, true);
        }
        drawMenu(a);
    }
}

// hub pointing: ray from the right controller against each miniature's footprint; trigger to watch
static void updateHub(App& a, const PlayerInput& in) {
    a.miniHover = -1;
    if (!a.spectating || a.watchIp || a.menuOpen || !a.aimValid[a.pointerHand]) { a.prevMiniTrig = true; return; }
    std::vector<SpecMatch>& ms = a.net.matches();
    const int n = (int)ms.size();
    Vec3 o = a.aim[a.pointerHand].p, d = a.aim[a.pointerHand].q.rotate({0, 0, -1});
    float best = 1e9f;
    for (int i = 0; i < n; ++i) {
        int row = i / 4, col = i % 4, inRow = std::min(4, n - row * 4);
        float ang = inRow == 1 ? 0.f : deg2rad(-40.f + 80.f * col / (inRow - 1));
        float rad = 1.35f + 0.9f * row;
        Vec3 c{rad * std::cos(ang), 0.92f + 0.35f * row + 0.12f, rad * std::sin(ang)};
        Vec3 oc = c - o;
        float t = dot(oc, d);
        if (t < 0) continue;
        float dist = length(oc - d * t);
        if (dist < 0.38f && t < best) { best = t; a.miniHover = i; }
    }
    bool trig = in.hand[a.pointerHand].trigger > 0.7f;
    if (trig && !a.prevMiniTrig && a.miniHover >= 0) {
        a.watchIp = ms[a.miniHover].ip;
        LOGI("spectate: watching %s", ms[a.miniHover].name);
        haptic(a, a.pointerHand, 0.5f, 0.05f);
    }
    a.prevMiniTrig = trig;
}

// where a speaker is (for panning and the talking indicator): tank players, waiting players, spectators
static bool speakerPos(const App& a, uint32_t id, Vec3& out) {
    const RoomInfo* rm = a.net.inRoom() ? &a.net.room() : (a.spectating && a.watchIp) ? &a.watchRoom : nullptr;
    if (!rm) return false;
    for (int p = 0; p < 2; ++p) if (rm->slot[p] == id) { out = a.viewIn[p].head.p; return true; }
    for (const RoomInfo::Member& m : rm->members) if (m.id == id) { out = m.in.head.p; return true; }
    for (const RoomInfo::Watcher& w : rm->watchers) if (w.id == id) { out = w.pos; return true; }
    return false;
}

// Voice: the microphone runs only inside an arena room (or at an arena as a spectator) with voice on and the
// permission given; 40 ms frames are sent while push-to-talk is held (or always with open mic, above a noise gate).
static void updateVoice(App& a) {
    const bool zone = a.net.voiceZone() && a.voiceMode != 2;
    // the permission: asked once; then checked once a second, so a prompt answered later (or a grant from the headset's
    // settings) starts the microphone without changing VOICE (GitHub #15, astrobleem: a pending prompt once read as no)
    const double now = nowSeconds();
    if (zone && !a.audio.capturing() && now >= a.micCheckAt) {
        a.micCheckAt = now + 1.0;
        const bool granted = micPermission(!a.micAsked);
        a.micAsked = true;
        a.micDenied = !granted;
        a.micFailed = granted && !a.audio.startCapture();
        if (a.micFailed) a.micCheckAt = now + 10.0;   // (busy or missing: not every second, nor a log line each time)
    }
    if (!zone && a.audio.capturing()) a.audio.stopCapture();
    a.talking = false;
    if (a.audio.capturing()) {
        int16_t tmp[4096];
        int n = a.audio.readCapture(tmp, 4096);
        a.micBuf.insert(a.micBuf.end(), tmp, tmp + n);
        const bool keyed = a.voiceMode == 1 || a.ptt;
        while ((int)a.micBuf.size() >= Net::kVoiceSamples) {
            const int16_t* f = a.micBuf.data();
            double e = 0;
            for (int i = 0; i < Net::kVoiceSamples; ++i) e += (double)f[i] * f[i];
            const float rms = (float)std::sqrt(e / Net::kVoiceSamples);
            const bool loud = rms > 350.0f;
            // automatic gain: bring speech up to a steady level (up to 6x), never boost the background hiss
            const float want = rms > 150.0f ? std::min(6.0f, std::max(1.0f, 3000.0f / rms)) : 1.0f;
            a.micGain += (want - a.micGain) * (want < a.micGain ? 0.5f : 0.15f);
            if (keyed && (a.voiceMode == 0 || loud)) {
                uint8_t u[Net::kVoiceSamples];
                for (int i = 0; i < Net::kVoiceSamples; ++i) {
                    float x = f[i] * a.micGain;
                    x = x > 30000.f ? 30000.f : x < -30000.f ? -30000.f : x;
                    u[i] = ulawEncode((int16_t)x);
                }
                a.net.sendVoice(u);
                a.talking = true;
                ++a.voiceSent;
            }
            a.micBuf.erase(a.micBuf.begin(), a.micBuf.begin() + Net::kVoiceSamples);
        }
        if (keyed && a.voiceMode == 0) a.talking = true;
    } else {
        a.micBuf.clear();
    }
    // incoming voice: mutes by role, panned toward the speaker
    const double t = nowSeconds();
    Vec3 f = a.viewHead.q.rotate({0, 0, -1}), right = normalize(cross(f, Vec3{0, 1, 0}));
    for (const Net::VoiceFrame& v : a.net.voiceInbox()) {
        if (a.muteAll || (v.role == Net::VOICE_SPECTATOR && a.muteSpectators) || (v.role != Net::VOICE_SPECTATOR && a.mutePlayers)) continue;
        if (isMuted(a, v.speaker)) continue;   // REPORT PLAYER > MUTE
        int16_t pcm[Net::kVoiceSamples];
        for (int i = 0; i < Net::kVoiceSamples; ++i) pcm[i] = ulawDecode(v.data[i]);
        float gain = 1.f, pan = 0.f;
        Vec3 p;
        if (speakerPos(a, v.speaker, p)) {
            Vec3 d = p - a.viewHead.p;
            float dist = length(d);
            gain = clampf(2.5f / (1.f + dist), 0.4f, 1.2f);
            pan = dist > 0.2f ? clampf(dot(right, d / dist), -0.8f, 0.8f) : 0.f;
        }
        a.audio.pushVoice(v.speaker, pcm, Net::kVoiceSamples, gain * 1.4f, pan);
        a.spokeAt[v.speaker] = t;
        ++a.voiceRecv;
    }
    if (t - a.voiceLogAt > 10.0) {   // what voice actually did, for testing
        if (a.voiceSent || a.voiceRecv || a.pttHeldFrames)
            LOGI("voice: sent %d frames, received %d (mic gain %.1fx, %s; mic %s, left grip peak %.2f, held %d frames)", a.voiceSent,
                 a.voiceRecv, a.micGain, a.voiceMode == 0 ? "push to talk" : a.voiceMode == 1 ? "open mic" : "off",
                 a.audio.capturing() ? "on" : "off", a.pttGripMax, a.pttHeldFrames);
        a.voiceSent = a.voiceRecv = 0;
        a.pttGripMax = 0;
        a.pttHeldFrames = 0;
        a.voiceLogAt = t;
    }
    a.net.voiceInbox().clear();
}

// HUD near the left hand: the push-to-talk hint, TALKING, or mic state; small white lights over people talking
static void drawVoiceHud(App& a) {
    if (!a.net.voiceZone()) return;
    const float grey[4] = {0.6f, 0.65f, 0.72f, 1}, red[4] = {1.0f, 0.3f, 0.25f, 1}, white[4] = {1, 1, 1, 1};
    const PlayerInput& in = a.localIn;
    if (in.hand[0].tracked && !a.menuOpen) {
        const char* t = a.voiceMode == 2 ? "VOICE OFF" : a.micDenied ? "MIC NEEDS PERMISSION" : a.micFailed ? "MIC NOT AVAILABLE" : a.muteAll ? "ALL MUTED"
                        : a.voiceMode == 1 ? (a.talking ? "OPEN MIC - TALKING" : "OPEN MIC")
                        : a.talking ? "TALKING" : "HOLD LEFT GRIP TO TALK";
        Vec3 at = in.hand[0].grip.p + Vec3{0, 0.12f, 0};
        Vec3 to = a.viewHead.p - at;
        to.y = 0;
        to = normalize(to);
        a.gfx.drawText(t, {Quat::axisAngle({0, 1, 0}, std::atan2(to.x, to.z)), at}, 0.022f, a.talking ? red : grey);
    }
    const double now = nowSeconds();
    for (const auto& kv : a.spokeAt) {
        if (now - kv.second > 0.3) continue;
        Vec3 p;
        if (speakerPos(a, kv.first, p)) a.gfx.drawSprite(p + Vec3{0, 0.25f, 0}, 0.06f, white, a.viewHead.p, 0);
    }
}

static Pose blendPose(const Pose& from, const Pose& to, float k) { return {nlerp(from.q, to.q, k), lerp(from.p, to.p, k)}; }

// Remote players arrive at the network's packet rhythm; draw them eased toward the latest update (~35 ms) so
// their avatars move smoothly. The local player and the bot are drawn as simulated. Big jumps snap.
static void smoothPlayers(App& a, float dt) {
    const int L = a.net.localIndex();
    const Net::Mode m = a.net.mode();
    const bool networked = a.viewing || m == Net::Mode::Host || m == Net::Mode::Client;
    const float k = 1.f - std::exp(-std::max(dt, 0.f) * 28.f);
    for (int p = 0; p < 2; ++p) {
        const PlayerInput& t = a.game.pl[p].in;
        PlayerInput& v = a.viewIn[p];
        const bool remote = networked && (a.viewing || p != L) && !a.game.pl[p].isBot;
        if (!remote || !a.viewInValid[p] || length(t.head.p - v.head.p) > 0.6f) { v = t; a.viewInValid[p] = true; continue; }
        PlayerInput s = t;
        s.head = blendPose(v.head, t.head, k);
        for (int h = 0; h < 2; ++h)
            if (t.hand[h].tracked && v.hand[h].tracked && length(t.hand[h].grip.p - v.hand[h].grip.p) < 0.6f)
                s.hand[h].grip = blendPose(v.hand[h].grip, t.hand[h].grip, k);
        v = s;
    }
}

// The Crowd in the stands. Places for each end (tools/blender/make_arena.py: tiers 0.95 m deep and 0.62 m higher each,
// the side stands' first tier 8.4 m out, the end stands' 14.4 m out): the side stands on that end's half first, then
// the stand behind that end. Each place: where a fan sits (the seat's top) and which way the court is (yaw: +Z there).
struct CrowdSpot { Vec3 p; float yaw, phase; };
static const std::vector<CrowdSpot>& crowdSpots(int p) {
    static std::vector<CrowdSpot> spots[2];
    if (spots[0].empty()) {
        uint32_t h = 12345u;
        auto rnd = [&h]() { h = h * 1664525u + 1013904223u; return (h >> 8) / 16777216.f; };
        for (int e = 0; e < 2; ++e) {
            const float sx = e == 0 ? -1.f : 1.f;
            for (int i = 0; i < 40; ++i) {
                const int k = (int)(rnd() * 8.99f);
                const float y = 4.2f + k * 0.62f;
                CrowdSpot s;
                if (i < 30) {   // the side stands on this half
                    const float sz = (i % 2) ? 1.f : -1.f;
                    s.p = {sx * (1.5f + rnd() * 12.f), y, sz * (8.5f + k * 0.95f)};
                    s.yaw = sz > 0 ? kPi : 0.f;
                } else {        // then behind the end zone
                    s.p = {sx * (14.5f + k * 0.95f), y, (rnd() - 0.5f) * (12.f + k * 1.9f)};
                    s.yaw = sx < 0 ? kPi * 0.5f : -kPi * 0.5f;
                }
                s.phase = rnd() * 6.28f;
                spots[e].push_back(s);
            }
        }
    }
    return spots[p];
}

// Flags in the stands: one for each fan waving that end's flag on their phone (the country's flag; without a country,
// a foam finger in the end's colour). The pole stands still in the stand; the cloth flies in the wind, its ripples running out from the pole
// (drawn in strips), livelier as the end's hype rises.
static void drawFanFlags(App& a, float now) {
    const CrowdInfo* c = crowdNow(a);
    if (a.crowdMode >= 2) return;   // (SETTINGS > CROWD: OFF)
    const int kStrips = 6;
    const float W = 0.8f, H = 0.53f, poleH = 1.9f;   // the flag's top edge at the pole's top
    for (int p = 0; p < 2 && c; ++p) {
        const int n = std::min(40, c->waves[p]);   // one flag per phone
        const float wind = 1.f + 0.8f * c->hype[p] / 100.f;
        const avatar::Nation* N = avatar::nation(a.sideNation[p]);
        const GLuint tex = N ? a.gfx.texture(N->flag, true) : 0;
        for (int i = 0; i < n; ++i) {
            const CrowdSpot& s = crowdSpots(p)[i];
            const Quat q = Quat::axisAngle({0, 1, 0}, s.yaw);
            if (!N) {   // no country: a foam finger (#1) in the end's colour, waved from the wrist (owner, 2026-10-21)
                const float sway = 0.35f * std::sin(now * 4.2f * wind + s.phase);
                const Quat qf = q * Quat::axisAngle({0, 0, 1}, sway);
                const Vec3 wrist = s.p + Vec3{0, 1.15f, 0};
                const float col[4] = {a.sideRGB[p][0], a.sideRGB[p][1], a.sideRGB[p][2], 1};
                a.gfx.drawTexturedPanel(Mat4::fromPose({qf, wrist + qf.rotate({0, 0.32f, 0})}, {0.64f, 0.64f, 1}),
                                        a.gfx.texture("fx_foam", true), col, false);
                continue;
            }
            const Vec3 top = s.p + Vec3{0, poleH, 0};
            const float pole[4] = {0.55f, 0.56f, 0.6f, 1};
            a.gfx.drawBox(Mat4::fromPose({q, s.p + Vec3{0, poleH * 0.5f, 0}}, {0.015f, poleH * 0.5f, 0.015f}), pole, false);   // (half sizes)
            // the cloth: strips out from the pole along the stand (+X), each bent by a wave that grows toward the free end
            const Pose base{q, top + q.rotate({0, -H * 0.5f, 0})};
            float zPrev = 0.f;
            for (int k = 0; k < kStrips; ++k) {
                const float u0 = (float)k / kStrips, u1 = (float)(k + 1) / kStrips, um = 0.5f * (u0 + u1);
                auto z = [&](float u) { return 0.11f * u * std::sin(now * 5.5f * wind - u * 7.f + s.phase); };
                const float z1 = z(u1);
                const float dx = W / kStrips, dz = z1 - zPrev;
                const float ang = -std::atan2(dz, dx);
                const Vec3 cpos = base.transform({um * W, -0.02f * um * um, 0.5f * (zPrev + z1)});   // (a slight droop)
                const float shade = 0.80f + 0.20f * std::cos(ang * 4.f);
                const float col[4] = {N ? shade : a.sideRGB[p][0] * shade, N ? shade : a.sideRGB[p][1] * shade,
                                      N ? shade : a.sideRGB[p][2] * shade, 1};
                const float uv[4] = {1.f / kStrips, 1.f, u0, 0.f};
                a.gfx.drawTexturedPanel(Mat4::fromPose({q * Quat::axisAngle({0, 1, 0}, ang), cpos},
                                                       {std::sqrt(dx * dx + dz * dz) * 1.02f, H, 1}), tex, col, false, uv);
                zPrev = z1;
            }
        }
    }
    // cheer bursts: hearts, stars and little flags pouring up out of that end's stands
    const Vec3 eye = a.viewHead.p;
    for (const App::CrowdFx& f : a.crowdFx) {
        if (f.age < 0) continue;
        const float t = f.age / f.life;
        const float alpha = std::min(1.f, f.age * 6.f) * (1.f - t * t);
        Vec3 d = eye - f.p;
        const float L = length(d);
        if (L < 1e-3f) continue;
        d = d / L;
        const Quat q = Quat::axisAngle({0, 1, 0}, std::atan2(d.x, d.z)) * Quat::axisAngle({1, 0, 0}, -std::asin(clampf(d.y, -1, 1))) *
                       Quat::axisAngle({0, 0, 1}, f.roll * f.age);
        const float sz = f.size * (0.6f + 0.4f * std::min(1.f, f.age * 4.f));
        const avatar::Nation* N = avatar::nation(a.sideNation[f.side]);
        const float white[4] = {1, 1, 1, alpha};
        const float* sc = a.sideRGB[f.side];
        if (f.kind == 2 && N) {
            a.gfx.drawTexturedPanel(Mat4::fromPose({q, f.p}, {sz, sz / 1.5f, 1}), a.gfx.texture(N->flag, true), white, false);
        } else if (f.kind == 2) {   // no country: a foam finger in the end's colour
            const float col[4] = {sc[0], sc[1], sc[2], alpha};
            a.gfx.drawTexturedPanel(Mat4::fromPose({q, f.p}, {sz * 1.2f, sz * 1.2f, 1}), a.gfx.texture("fx_foam", true), col, false);
        } else if (!a.game.tun.shields) {   // HOTDOG MODE: hot dogs and smiley faces (the community's nod)
            a.gfx.drawTexturedPanel(Mat4::fromPose({q, f.p}, {sz * 1.1f, sz * 1.1f, 1}),
                                    a.gfx.texture(f.kind == 0 ? "fx_hotdog" : "fx_smile", true), white, false);
        } else {
            const bool star = f.kind == 1;
            const float col[4] = {star ? 1.f : 0.55f + 0.45f * sc[0], star ? 0.85f : 0.25f + 0.45f * sc[1],
                                  star ? 0.3f : 0.35f + 0.45f * sc[2], alpha};
            a.gfx.drawTexturedPanel(Mat4::fromPose({q, f.p}, {sz, sz, 1}), a.gfx.texture(star ? "fx_star" : "fx_heart", true), col, false);
        }
    }
}

// the bursts move: each rises out of the stand, drifts toward the court, slows and fades
static void stepCrowdFx(App& a, float dt) {
    for (App::CrowdFx& f : a.crowdFx) {
        f.age += dt;
        if (f.age <= 0) continue;
        f.p += f.v * dt;
        f.v.y -= 1.4f * dt;
        f.v *= std::max(0.f, 1.f - 0.4f * dt);
    }
    a.crowdFx.erase(std::remove_if(a.crowdFx.begin(), a.crowdFx.end(), [](const App::CrowdFx& f) { return f.age > f.life; }),
                    a.crowdFx.end());
}

static void burstCrowdFx(App& a, int side, int count) {
    static uint32_t h = 777u;
    auto rnd = [&]() { h = h * 1664525u + 1013904223u; return (h >> 8) / 16777216.f; };
    const std::vector<CrowdSpot>& spots = crowdSpots(side);
    for (int i = 0; i < count && a.crowdFx.size() < 400; ++i) {
        const CrowdSpot& s = spots[(size_t)(rnd() * 39.99f)];
        const Vec3 toCourt = Quat::axisAngle({0, 1, 0}, s.yaw).rotate({0, 0, 1});
        App::CrowdFx f;
        f.p = s.p + Vec3{(rnd() - 0.5f) * 1.5f, 0.9f + rnd() * 0.4f, (rnd() - 0.5f) * 1.5f};
        f.v = toCourt * (0.6f + rnd() * 0.8f) + Vec3{(rnd() - 0.5f) * 0.6f, 2.6f + rnd() * 1.6f, (rnd() - 0.5f) * 0.6f};
        f.age = -rnd() * 0.5f;            // spread over the half second until the next update
        f.life = 1.6f + rnd() * 0.6f;
        f.size = 0.30f + rnd() * 0.15f;
        f.kind = (int)(rnd() * 2.99f);    // heart, star, flag
        f.side = side;
        f.roll = (rnd() - 0.5f) * 1.5f;
        a.crowdFx.push_back(f);
    }
}

// the EXPERIMENTAL arenas' floating platforms (and the fist bump's, in the centre): pass 0 the discs, pass 1 their glow
static void drawPlatforms(App& a, int pass, float now, float belowY) {
    using namespace exparena;
    const bool meeting = a.game.match == MatchState::Meet || a.bumpFlash > 0;
    if (pass == 0) {
        if (meeting) {
            DrawParams mp;
            for (int k = 0; k < 3; ++k) mp.tintMul[k] = 0.30f;
            a.gfx.draw(a.gfx.cylinder, Mat4::fromPose({Quat{}, {0, 0, 0}}, {1.4f, kPlatformThick, 1.4f}), mp, 0);
        }
        for (int p = 0; p < 2; ++p) {   // a solid slab you stand on (your ball's spawner on it), steel with your colour's glow
            DrawParams pp;
            for (int k = 0; k < 3; ++k) { pp.tintMul[k] = 0.30f + 0.12f * a.sideRGB[p][k]; pp.emissiveAdd[k] = 0.05f * a.sideRGB[p][k]; }
            a.gfx.draw(a.gfx.cylinder, Mat4::fromPose({Quat{}, {p ? 7.f : -7.f, 0, 0}}, {kPlatformR, kPlatformThick, kPlatformR}), pp, 0);
        }
        return;
    }
    for (int p = 0; p < 2; ++p) {   // its top: rings and lines of light, so it reads as a floor to stand on
        const float px = p ? 7.f : -7.f;
        const float lineC[4] = {a.sideRGB[p][0] * 0.45f, a.sideRGB[p][1] * 0.45f, a.sideRGB[p][2] * 0.45f, 1};
        for (int k = -2; k <= 2; ++k) {
            const float off = 0.4f * k, half = std::sqrt(std::max(0.f, kPlatformR * kPlatformR - off * off)) - 0.05f;
            const Vec3 a0[2] = {{px + off, 0.006f, -half}, {px + off, 0.006f, half}}, a1[2] = {{px - half, 0.006f, off}, {px + half, 0.006f, off}};
            a.gfx.drawRibbon(a0, 2, 0.006f, lineC, false);
            a.gfx.drawRibbon(a1, 2, 0.006f, lineC, false);
        }
    }
    if (meeting)
        for (int p = 0; p < 2; ++p) {
            const float r[4] = {a.sideRGB[p][0] * 0.8f, a.sideRGB[p][1] * 0.8f, a.sideRGB[p][2] * 0.8f, 1};
            a.gfx.drawSpriteFlat({p ? 0.02f : -0.02f, 0.008f, 0}, 2.9f, r, {0, 1, 0}, 1);
        }
    for (int p = 0; p < 2; ++p) {   // a glowing rim in the player's colour, a soft glow underneath (it floats)
        const float px = p ? 7.f : -7.f;
        const float pulse = 0.85f + 0.15f * std::sin(now * 2.f + p * 3.f);
        const float rim[4] = {a.sideRGB[p][0] * pulse, a.sideRGB[p][1] * pulse, a.sideRGB[p][2] * pulse, 1};
        const float under[4] = {a.sideRGB[p][0] * 0.25f, a.sideRGB[p][1] * 0.25f, a.sideRGB[p][2] * 0.25f, 1};
        const float rimSoft[4] = {rim[0] * 0.6f, rim[1] * 0.6f, rim[2] * 0.6f, 1};
        a.gfx.drawSpriteFlat({px, 0.008f, 0}, 2.f * kPlatformR + 0.12f, rimSoft, {0, 1, 0}, 1);
        a.gfx.drawSpriteFlat({px, -kPlatformThick - 0.02f, 0}, 3.4f, under, {0, -1, 0}, 0);
        a.gfx.drawSpriteFlat({px, belowY + 0.02f, 0}, 2.6f, under, {0, 1, 0}, 0);   // its light on what is below
    }
}

// The EXPERIMENTAL arena DIAMOND (exparena, the owner's drawing 2026-10-29): the court as a glass tube rolled 45 degrees,
// its end at each end (its player's back wall, in its colour), a neon ring every metre along the court and the foul
// lines in the players' colours (no foul zone here: the line recharges the ball), the floating platforms.
static void drawDiamondArena(App& a, int pass, float now) {
    using namespace exparena;
    const float cy = kRollCY, len = 2.f * kRollLen;
    Vec3 v[4];
    diamondEnd(v);
    if (pass == 0) { drawPlatforms(a, 0, now, cy - kRollReach); return; }
    auto panel = [](const Vec3& c, const Vec3& ax, const Vec3& ay, float w, float h) {   // a quad from its centre and edges
        Mat4 m = Mat4::identity();
        const Vec3 az = normalize(cross(ax, ay));
        m.m[0] = ax.x * w; m.m[1] = ax.y * w; m.m[2] = ax.z * w;
        m.m[4] = ay.x * h; m.m[5] = ay.y * h; m.m[6] = ay.z * h;
        m.m[8] = az.x; m.m[9] = az.y; m.m[10] = az.z;
        m.m[12] = c.x; m.m[13] = c.y; m.m[14] = c.z;
        return m;
    };
    const float glass[4] = {0.02f, 0.06f, 0.11f, 0.22f};
    for (int k = 0; k < 4; ++k) {   // the four long walls (two 4.96 m across, two 3.88 m)
        const Vec3 m = (v[k] + v[(k + 1) % 4]) * 0.5f, e = v[(k + 1) % 4] - v[k];
        const float wide = length(e);
        a.gfx.drawPanel(panel(m, {1, 0, 0}, e / wide, len, wide), glass);
    }
    for (int p = 0; p < 2; ++p) {   // the ends: the back walls
        const Vec3 o{p ? kRollLen : -kRollLen, 0, 0};
        const float tint[4] = {a.sideRGB[p][0] * 0.4f, a.sideRGB[p][1] * 0.4f, a.sideRGB[p][2] * 0.4f, 0.30f};
        const Vec3 ax = v[0] - v[1], ay = v[0] - v[3];
        a.gfx.drawPanel(panel((v[0] + v[2]) * 0.5f + o, normalize(ax), normalize(ay), length(ax), length(ay)), tint);
    }
    const float grid[4] = {0.06f, 0.32f, 0.55f, 1}, bright[4] = {0.25f, 0.8f, 1.0f, 1};
    for (int k = 0; k < 4; ++k) {   // the four long edges
        const Vec3 ln[2] = {v[k] + Vec3{-kRollLen, 0, 0}, v[k] + Vec3{kRollLen, 0, 0}};
        a.gfx.drawRibbon(ln, 2, 0.03f, bright, false);
    }
    for (float x = -kRollLen; x <= kRollLen + 0.01f; x += 1.f) {   // a diamond ring every metre
        const bool end = std::fabs(x) > kRollLen - 0.01f;
        const Vec3 o{x, 0, 0};
        const Vec3 ring[5] = {v[0] + o, v[1] + o, v[2] + o, v[3] + o, v[0] + o};
        a.gfx.drawRibbon(ring, 5, end ? 0.035f : 0.010f, end ? bright : grid, false);
    }
    for (int p = 0; p < 2; ++p) {   // the foul lines, round the tube in each player's colour
        const Vec3 o{p ? 4.73f : -4.73f, 0, 0};
        const Vec3 ring[5] = {v[0] + o, v[1] + o, v[2] + o, v[3] + o, v[0] + o};
        const float c[4] = {a.sideRGB[p][0], a.sideRGB[p][1], a.sideRGB[p][2], 1};
        a.gfx.drawRibbon(ring, 5, 0.03f, c, false);
    }
    drawPlatforms(a, 1, now, cy - kRollReach);
}

// The EXPERIMENTAL arena (exparena, owner 2026-10-29): glass walls in a diamond round the court with a neon grid on
// them and on the floor below, the corners behind the players (their back walls) lit in their colours, and the two
// floating platforms. Pass 0: the platforms; pass 1: the rest (blended, so the crowd shows through the glass).
static void drawExpArena(App& a, int pass, float now) {
    using namespace exparena;
    const Vec3 c[4] = {{-kCorner, 0, 0}, {0, 0, kCorner}, {kCorner, 0, 0}, {0, 0, -kCorner}};
    const float side = length(c[1] - c[0]), midY = 0.5f * (kFloor + kCeiling), tall = kCeiling - kFloor;
    if (pass == 0) { drawPlatforms(a, 0, now, kFloor); return; }
    // the glass: four walls, the floor below (nearly solid, so it reads as a floor) and the ceiling
    const float glass[4] = {0.02f, 0.06f, 0.11f, 0.20f}, floorC[4] = {0.006f, 0.014f, 0.03f, 0.92f}, ceil[4] = {0.012f, 0.03f, 0.06f, 0.35f};
    for (int w = 0; w < 4; ++w) {
        const Vec3 d = normalize(c[(w + 1) % 4] - c[w]), m = (c[w] + c[(w + 1) % 4]) * 0.5f;
        a.gfx.drawPanel(Mat4::fromPose({Quat::axisAngle({0, 1, 0}, std::atan2(-d.z, d.x)), m + Vec3{0, midY, 0}}, {side, tall, 1}), glass);
    }
    const Quat flat = Quat::axisAngle({0, 1, 0}, kPi * 0.25f) * Quat::axisAngle({1, 0, 0}, -kPi * 0.5f);
    a.gfx.drawPanel(Mat4::fromPose({flat, {0, kFloor, 0}}, {side, side, 1}), floorC);
    a.gfx.drawPanel(Mat4::fromPose({flat, {0, kCeiling, 0}}, {side, side, 1}), ceil);
    // the back walls: the corner behind each player, in its colour
    for (int p = 0; p < 2; ++p) {
        const int ci = p ? 2 : 0;
        const float tint[4] = {a.sideRGB[p][0] * 0.35f, a.sideRGB[p][1] * 0.35f, a.sideRGB[p][2] * 0.35f, 0.22f};
        for (int nb = -1; nb <= 1; nb += 2) {   // the two walls meeting in that corner, as far as the back wall goes
            const Vec3 o = c[(ci + nb + 4) % 4], d = normalize(o - c[ci]);
            const float len = (kCorner - kBackX) / std::fabs(d.x);
            const Vec3 m = c[ci] + d * (0.5f * len);
            a.gfx.drawPanel(Mat4::fromPose({Quat::axisAngle({0, 1, 0}, std::atan2(-d.z, d.x)), m + Vec3{0, midY, 0} - normalize(Vec3{m.x, 0, m.z}) * 0.01f}, {len, tall, 1}), tint);
            const float edge[4] = {a.sideRGB[p][0], a.sideRGB[p][1], a.sideRGB[p][2], 1};
            const Vec3 e = c[ci] + d * len;
            const Vec3 ln[2] = {e + Vec3{0, kFloor, 0}, e + Vec3{0, kCeiling, 0}};
            a.gfx.drawRibbon(ln, 2, 0.025f, edge, false);
        }
    }
    // the neon grid: lines up the walls at every strip, rings round the room every metre, the corners brighter
    const float grid[4] = {0.06f, 0.32f, 0.55f, 1}, bright[4] = {0.25f, 0.8f, 1.0f, 1};
    for (int w = 0; w < 4; ++w)
        for (int i = 0; i <= 8; ++i) {
            const Vec3 q = lerp(c[w], c[(w + 1) % 4], i / 8.f);
            const Vec3 ln[2] = {q + Vec3{0, kFloor, 0}, q + Vec3{0, kCeiling, 0}};
            a.gfx.drawRibbon(ln, 2, i == 0 ? 0.035f : 0.010f, i == 0 ? bright : grid, false);
        }
    for (float y = kFloor; y <= kCeiling + 0.01f; y += 1.f) {
        const bool rim = y <= kFloor + 0.01f || y >= kCeiling - 0.01f;
        const Vec3 ring[5] = {c[0] + Vec3{0, y, 0}, c[1] + Vec3{0, y, 0}, c[2] + Vec3{0, y, 0}, c[3] + Vec3{0, y, 0}, c[0] + Vec3{0, y, 0}};
        a.gfx.drawRibbon(ring, 5, rim ? 0.03f : 0.010f, rim ? bright : grid, false);
    }
    for (int i = 1; i < 8; ++i)   // the floor below: a grid along the walls
        for (int k = 0; k < 2; ++k) {
            const Vec3 a0 = lerp(c[0], c[k ? 3 : 1], i / 8.f), a1 = lerp(c[k ? 1 : 3], c[2], i / 8.f);
            const Vec3 ln[2] = {a0 + Vec3{0, kFloor + 0.01f, 0}, a1 + Vec3{0, kFloor + 0.01f, 0}};
            a.gfx.drawRibbon(ln, 2, 0.012f, grid, false);
        }
    drawPlatforms(a, 1, now, kFloor);
}

static void drawWorld(App& a, int pass) {
    if (a.spectating && !a.watchIp) { drawHub(a, pass); return; }
    Game& g = a.game;
    const int L = a.net.localIndex();
    DrawParams dp;
    const float now = float(nowSeconds() - a.startTime);
    const int ptm = a.mrView ? a.ptMode : PT_OFF;
    const int only = a.onlyPlayer;   // the win screen's close-ups: one player alone
    const bool keyBg = a.viewLayer >= 3 || only >= 0, minimal = a.viewLayer >= 2 || only >= 0, noWalls = a.viewLayer >= 1;
    const bool outline = ((ptm == PT_OUTLINE) || noWalls) && a.courtOutline.valid();
    const bool exp = g.arenaKind == ARENA_CORNERS || g.arenaKind == ARENA_DIAMOND;   // an EXPERIMENTAL room
    // (CORNERS reaches 9.5 m to the sides: the stadium widened to stand round it)
    const Mat4 outside = g.arenaKind == ARENA_CORNERS ? Mat4::fromPose({Quat{}, {0, 0, 0}}, {1.35f, 1.f, 1.35f}) : Mat4::identity();
    updateSides(a);
    updateStats(a);
    // the match won: bursts of confetti over the winner, in their team colour and gold (fewer with softer effects)
    if (g.match == MatchState::MatchSummary && g.matchWinner >= 0 && g.matchWinner < 2 && (only < 0 || only == g.matchWinner)) {
        if (a.winAt <= 0) a.winAt = nowSeconds();
        const float age = float(nowSeconds() - a.winAt);
        const int n = a.fxLevel == 0 ? 90 : a.fxLevel == 1 ? 40 : 0;
        if (pass == 1 && n && age < 6.5f) {
            const int wn = g.matchWinner;
            const Vec3 o = a.viewIn[wn].head.p + Vec3{0, 0.45f, 0};
            float tcol[2][3] = {{0.25f, 0.75f, 1.0f}, {1.0f, 0.55f, 0.2f}};
            if (a.nationsOn) for (int p = 0; p < 2; ++p) for (int k = 0; k < 3; ++k) tcol[p][k] = 0.83f * a.sideRGB[p][k] + 0.17f;
            for (int burst = 0; burst < 3; ++burst) {
                const float bt = age - burst * 1.6f;
                if (bt < 0 || bt > 2.6f) continue;
                for (int i = 0; i < n; ++i) {
                    uint32_t h = (uint32_t)(i * 2654435761u) ^ (uint32_t)(burst * 40503u + wn * 977u);
                    auto rnd = [&]() { h ^= h << 13; h ^= h >> 17; h ^= h << 5; return (h & 0xFFFF) / 65535.f; };
                    const float az = rnd() * 6.2832f, el = 0.6f + rnd() * 0.9f, sp = 2.0f + rnd() * 2.2f;
                    const Vec3 v{std::cos(el) * std::cos(az) * sp, std::sin(el) * sp, std::cos(el) * std::sin(az) * sp};
                    const float drag = std::exp(-bt * 1.4f);
                    const Vec3 p = o + v * ((1.f - drag) / 1.4f) + Vec3{0, -1.2f * bt * bt, 0};
                    const bool goldBit = (i % 3) == 0;
                    const float fade = std::max(0.f, 1.f - bt / 2.6f);
                    const float c[4] = {goldBit ? 1.0f : tcol[wn][0], goldBit ? 0.85f : tcol[wn][1], goldBit ? 0.3f : tcol[wn][2], fade};
                    a.gfx.drawSprite(p, 0.07f + 0.05f * rnd(), c, a.viewHead.p, 0);
                }
            }
        }
    } else if (g.match != MatchState::MatchSummary) {
        a.winAt = 0;
    }
    if (pass == 1 && a.floor.valid() && !exp && !(a.mrView && a.ptFloor <= 0.01f) && !keyBg) {
        // glossy floor: mirrored light sources (and the balls) seen through the semi-transparent floor
        DrawParams rp;
        if (!minimal) a.gfx.draw(a.floorReflect, Mat4::identity(), rp, 1);   // (MINIMAL: no stadium in the floor either)
        if (!a.mrView && !minimal && a.floorReflectEnv.valid()) a.gfx.draw(a.floorReflectEnv, Mat4::identity(), rp, 1);
        const Vec3 eye = a.viewHead.p;
        const float (*tcol)[3] = a.sideRGB;
        for (int p = 0; p < 2; ++p) {
            const Ball& b = g.pl[p].ball;
            if (b.state == BallState::Dead || b.pos.y < 0.f) continue;
            Vec3 m{b.pos.x, -b.pos.y, b.pos.z};
            float k = 0.45f / (1.f + 0.6f * b.pos.y);
            float c[4] = {tcol[p][0] * k, tcol[p][1] * k, tcol[p][2] * k, 1};
            a.gfx.drawSprite(m, 0.35f * g.pl[p].ball.scale, c, eye, 0);
            float w[4] = {0.5f * k, 0.5f * k, 0.5f * k, 1};
            a.gfx.drawSprite(m, 0.12f * g.pl[p].ball.scale, w, eye, 0);
            // light pool on the floor under the ball
            float pc[4] = {tcol[p][0] * 0.25f / (1.f + b.pos.y * b.pos.y), tcol[p][1] * 0.25f / (1.f + b.pos.y * b.pos.y),
                           tcol[p][2] * 0.25f / (1.f + b.pos.y * b.pos.y), 1};
            a.gfx.drawSpriteFlat({b.pos.x, 0.004f, b.pos.z}, 0.9f + 0.4f * b.pos.y, pc, {0, 1, 0}, 0);
        }
        if (!minimal && !a.mrView) {   // team accents (owner, 2026-10-28): each half washed low in its player's colour,
            for (int p = 0; p < 2; ++p) {   // on the side walls and along the floor's edges, stronger toward their end
                const float sx = p == 0 ? -1.f : 1.f;
                for (int i = 0; i < 12; ++i) {
                    const float x = sx * (1.2f + 0.72f * i), k = 0.55f + 0.45f * (i / 11.f);
                    const float wc[4] = {tcol[p][0] * 0.07f * k, tcol[p][1] * 0.07f * k, tcol[p][2] * 0.07f * k, 1};
                    const float fc[4] = {tcol[p][0] * 0.045f * k, tcol[p][1] * 0.045f * k, tcol[p][2] * 0.045f * k, 1};
                    for (int s = -1; s <= 1; s += 2) {
                        a.gfx.drawSpriteFlat({x, 0.55f, s * 2.45f}, 1.7f, wc, {0, 0, -(float)s}, 0);
                        a.gfx.drawSpriteFlat({x, 0.005f, s * 2.15f}, 1.2f, fc, {0, 1, 0}, 0);
                    }
                }
            }
        }
        DrawParams fp;
        fp.alphaMul = a.mrView ? std::min(0.998f, a.ptFloor) : 0.80f;   // PASSTHROUGH: COURT FLOOR
        a.gfx.draw(a.floor, Mat4::identity(), fp, 1);
    }
    if (!a.mrView && !minimal) a.gfx.draw(a.environment, outside, dp, pass);   // PASSTHROUGH: your room instead
    if (keyBg) {
        // (nothing of the court)
    } else if (exp) {
        if (g.arenaKind == ARENA_DIAMOND) drawDiamondArena(a, pass, now); else drawExpArena(a, pass, now);
    } else if (outline) {
        a.gfx.draw(a.courtOutline, Mat4::identity(), dp, pass);
    } else {
        DrawParams ap;
        ap.glassMul = a.mrView ? a.ptGlass : 1.f;   // PASSTHROUGH: GLASS WALLS
        a.gfx.draw(a.arena, Mat4::identity(), ap, pass);
    }
    if (pass == 1 && ptm == PT_WINDOW && !exp) {   // WINDOW: a night sky over the court, so only the glass walls look out
        const float sky[4] = {0.006f, 0.010f, 0.022f, 0.998f};
        a.gfx.drawPanel(Mat4::fromPose({Quat::axisAngle({1, 0, 0}, kPi * 0.5f), {0, 4.0f, 0}}, {19.0f, 5.0f, 1}), sky);
        if (a.depthRunning && a.depthImgValid && !a.rttPass) {   // real things nearer than the glass: covered (night behind the glass)
            const Mat4 walls[4] = {Mat4::fromPose({Quat{}, {0, 2.0f, 2.52f}}, {19.04f, 4.0f, 1}),
                                   Mat4::fromPose({Quat{}, {0, 2.0f, -2.52f}}, {19.04f, 4.0f, 1}),
                                   Mat4::fromPose({Quat::axisAngle({0, 1, 0}, kPi * 0.5f), {9.52f, 2.0f, 0}}, {5.04f, 4.0f, 1}),
                                   Mat4::fromPose({Quat::axisAngle({0, 1, 0}, kPi * 0.5f), {-9.52f, 2.0f, 0}}, {5.04f, 4.0f, 1})};
            const int e = a.curEye & 1;
            a.gfx.drawDepthBacking(walls, 4, a.depthImages[a.depthImg.swapchainIndex].image, e, a.depthView[e], a.depthProj[e],
                                   a.depthImg.nearZ, a.depthImg.farZ, sky);
        }
    }
    if (pass == 1 && a.mrView && inLobby(a)) {   // PASSTHROUGH: the balcony is gone; a platform to stand on instead
        const float pc[4] = {0.03f, 0.08f, 0.12f, 1}, rc[4] = {0.15f, 0.5f, 0.75f, 1};
        a.gfx.drawSpriteFlat({kLobbySpot.x, kLobbySpot.y + 0.004f, kLobbySpot.z}, 1.8f, pc, {0, 1, 0}, 0);
        a.gfx.drawSpriteFlat({kLobbySpot.x, kLobbySpot.y + 0.006f, kLobbySpot.z}, 2.0f, rc, {0, 1, 0}, 1);
    }
    if (pass == 1 && a.spectating && !minimal) {   // the floating gallery seat the spectator stands on
        const float pc[4] = {0.03f, 0.08f, 0.12f, 1}, rc[4] = {0.15f, 0.5f, 0.75f, 1};
        a.gfx.drawSpriteFlat({0, 1.004f, 5.4f}, 1.8f, pc, {0, 1, 0}, 0);
        a.gfx.drawSpriteFlat({0, 1.006f, 5.4f}, 2.0f, rc, {0, 1, 0}, 1);
    }
    if (pass == 1 && a.nationsOn && !minimal && !a.mrView && !exp) {   // (EXPERIMENTAL: the stadium is widened; no wall flags yet)
        // NATIONS: each country's flag on the walls outside the tank: the end wall (13.6 m out, behind the end cameras)
        // and one on each side wall on its half (owner, 2026-10-20: one flag a wall, none in front of the tank). Drawn
        // before the court's glow layers, so the rings and lines in front of a flag show over it (owner, 2026-10-21:
        // the end wall's flag covered the back ring); the side flags sit on the middle of a panel, between the
        // pillars (every 4 m from 10 m out; they were on the 6 m pillar)
        for (int p = 0; p < 2; ++p) {
            if (!a.sideNation[p]) continue;
            const float sx = p == 0 ? -1.f : 1.f;
            drawFlag(a, a.sideNation[p], {Quat::axisAngle({0, 1, 0}, deg2rad(-90.f * sx)), {13.45f * sx, 2.1f, 0}}, 3.2f);
            for (int s = -1; s <= 1; s += 2)
                drawFlag(a, a.sideNation[p], {Quat::axisAngle({0, 1, 0}, s < 0 ? 0.f : kPi), {sx * 8.f, 2.1f, 7.45f * s}}, 2.6f);
        }
    }
    if (pass == 1) {
        DrawParams xp;
        xp.uvOff[0] = now * 0.035f;   // crowd lights twinkle (env_fx = crowd lights only; they sample a scrolling twinkle strip)
        if (!a.mrView && !minimal) a.gfx.draw(a.envFx, outside, xp, 1);
        DrawParams cp;   // static glow layers: light halos, holographic rings, end-zone energy, light beams
        if (!outline && !minimal && !exp) a.gfx.draw(a.courtFx, Mat4::identity(), cp, 1);
        if (!a.mrView && !minimal && a.stadiumFx.valid()) a.gfx.draw(a.stadiumFx, outside, cp, 1);
        DrawParams pp;   // energy pulses racing along the neon outline
        pp.uvOff[1] = -now * 0.9f;
        if (!keyBg && !exp) a.gfx.draw(a.courtPulse, Mat4::identity(), pp, 1);
    }
    const float teamCol[2][4] = {{a.sideRGB[0][0], a.sideRGB[0][1], a.sideRGB[0][2], 1}, {a.sideRGB[1][0], a.sideRGB[1][1], a.sideRGB[1][2], 1}};
    if (pass == 1 && !minimal && !a.mrView && !exp) drawFanFlags(a, now);   // The Crowd: flags waving in the stands (not yet in the widened stadium)
    if (pass == 1 && !minimal && !a.mrView) drawJumbotron(a);   // the screen over centre court: names, score, stats
    for (int p = 0; p < 2 && !minimal; ++p) {
        const BallState st = g.pl[p].ball.state;
        float glow = (st == BallState::Spawning || st == BallState::Dead) ? 0.55f : 0.16f;
        DrawParams sp;
        sp.emissiveAdd[0] = teamCol[p][0] * glow; sp.emissiveAdd[1] = teamCol[p][1] * glow; sp.emissiveAdd[2] = teamCol[p][2] * glow;
        a.gfx.draw(a.spawner, Mat4::fromPose({Quat{}, g.tun.spawner[p]}), sp, pass);
    }
    for (int p = 0; p < 2; ++p) {
        if (!a.spectating && !a.net.slotFilled(p)) continue;
        if (only >= 0 && p != only) continue;
        if (a.spectating && a.watchIp && !a.watchRoom.slot[p] && !(p == 1 && a.watchRoom.bot) &&
            (a.watchRoom.slot[0] || a.watchRoom.slot[1] || a.watchRoom.bot || !a.watchRoom.members.empty())) continue;
        PlayerState& ps = g.pl[p];
        const PlayerInput& vin = a.viewIn[p];   // remote players smoothed between network updates
        // hands: lancer tool, knuckle guard, shield (gauntlets wear the player's avatar colours)
        const AvatarDesc& look = (p == L && !a.viewing) ? a.myAvatar : vin.avatar;
        const AvatarLook lk = lookOf(a, look, p);
        for (int h = 0; h < 2; ++h) {
            if (!vin.hand[h].tracked) continue;
            Mat4 grip = Mat4::fromPose(vin.hand[h].grip);
            Mat4 mirror = Mat4::identity();
            if (h == 0) mirror.m[0] = -1;   // left hand: mirror across grip X (HandnessRoot)
            DrawParams hp;
            hp.mirrored = (h == 0);
            if (!a.avModels.empty()) applyLook(hp, lk);
            else { hp.tintMul[0] = 0.6f + 0.4f * teamCol[p][0]; hp.tintMul[1] = 0.6f + 0.4f * teamCol[p][1]; hp.tintMul[2] = 0.6f + 0.4f * teamCol[p][2]; }
            drawGauntlet(a, look, grip * mirror, hp, pass);
            if (ps.hands[h].shieldUp) {
                DrawParams sp;
                sp.tintMul[0] = teamCol[p][0]; sp.tintMul[1] = teamCol[p][1]; sp.tintMul[2] = teamCol[p][2];
                sp.emissiveAdd[0] = teamCol[p][0] * 0.4f; sp.emissiveAdd[1] = teamCol[p][1] * 0.4f; sp.emissiveAdd[2] = teamCol[p][2] * 0.4f;
                sp.alphaMul = 0.7f;
                a.gfx.draw(a.shield, grip * Mat4::fromPose(g.shieldLocal(h)) * g_plateLocal, sp, pass);
            }
            if (ps.hands[h].knuckleUp) {
                // clear glass shell over the front of the fist (KnuckleShield_01 / Glass_SingleSided2), faint team tint
                DrawParams kp;
                kp.mirrored = (h == 0);
                kp.forceShader = SH_TRANSLUCENT;
                kp.tintMul[0] = 0.75f + 0.25f * teamCol[p][0]; kp.tintMul[1] = 0.75f + 0.25f * teamCol[p][1]; kp.tintMul[2] = 0.75f + 0.25f * teamCol[p][2];
                kp.emissiveAdd[0] = teamCol[p][0] * 0.06f; kp.emissiveAdd[1] = teamCol[p][1] * 0.06f; kp.emissiveAdd[2] = teamCol[p][2] * 0.06f;
                kp.alphaMul = 0.35f;
                a.gfx.draw(a.knuckle, grip * mirror * knuckleShift(g) * g_knuckleLocal, kp, pass);
            }
        }
        // opponent body (local player has no body, like the original first-person view); spectators see both
        if ((p != L || a.viewing) && !a.avModels.empty() && vin.head.p.y > 0.3f) {
            drawAvatarBody(a, vin, p, pass, Mat4::identity(), false, false, a.hitFlash[p]);
        } else if (p != L || a.viewing) {
            Pose hpose = vin.head;
            Vec3 f = hpose.q.rotate({0, 0, -1});
            float yaw = std::atan2(-f.x, -f.z);
            // skeletal meshes face UE +Y (= gltf +Z); turn to face the head's heading
            Quat bodyYaw = Quat::axisAngle({0, 1, 0}, yaw + kPi);
            Vec3 off{hpose.p.x, hpose.p.y - 1.66f, hpose.p.z};
            DrawParams bp;
            for (int k = 0; k < 3; ++k) bp.tintMul[k] = 0.7f + 0.3f * teamCol[p][k];
            if (a.hitFlash[p] > 0) { bp.emissiveAdd[0] = a.hitFlash[p]; bp.emissiveAdd[1] = a.hitFlash[p] * 0.3f; }
            Mat4 torsoM = Mat4::fromPose({bodyYaw, {hpose.p.x, off.y, hpose.p.z}});
            a.gfx.draw(a.torso, torsoM, bp, pass);
            // head follows full head orientation, pivoting around the neck
            Quat headQ = hpose.q * Quat::axisAngle({0, 1, 0}, kPi);
            Pose headPose{headQ, hpose.p - headQ.rotate(Vec3{0, 1.66f, 0})};
            a.gfx.draw(a.head, Mat4::fromPose(headPose), bp, pass);
        }
        // projectile
        const Ball& b = ps.ball;
        if (only < 0 && b.state != BallState::Dead && (b.state != BallState::Spawning || b.pos.y > 0.f)) {
            DrawParams bp;
            // on its way home after a foul, a short throw or getting stuck: harmless, so faded (owner, 2026-10-28)
            const bool harmless = b.state == BallState::Returning && b.scored;
            float glow = harmless ? 0.35f : b.charged ? 2.0f : 0.6f;   // uncharged ball: "your Ball has lost its charge"
            bp.tintMul[0] = teamCol[p][0] * glow; bp.tintMul[1] = teamCol[p][1] * glow; bp.tintMul[2] = teamCol[p][2] * glow;
            float s = b.scale * (g.tun.projectileRadius / 0.08f);   // visual sphere is 8 cm; collision radius 10 cm
            if (b.state == BallState::Spawning) {
                float r = clampf((b.pos.y - g.tun.spawnLauncherHeight) / std::max(0.05f, g.idlePoint(p).y - g.tun.spawnLauncherHeight), 0.f, 1.f);
                s *= 0.35f + 0.65f * std::min(1.f, r * 2.5f);
            }
            Mat4 m = Mat4::fromPose({Quat{}, b.pos}, {s, s, s});
            {
                DrawParams pp = bp;   // energy veins drift across the plasma sphere
                float tt = float(nowSeconds() - a.startTime);
                pp.uvOff[0] = tt * 0.21f + a.trail[p].spinAngle * 0.05f;
                pp.uvOff[1] = tt * 0.13f;
                a.gfx.draw(a.projectile, m, pp, pass);
            }
            {   // the shell fragments spin with the ball (VFX_Projectile_Shell)
                float sl = length(b.spin);
                Quat sq = sl > 1e-3f ? Quat::axisAngle(b.spin / sl, a.trail[p].spinAngle) : Quat{};
                a.gfx.draw(a.shell, Mat4::fromPose({sq, b.pos}, {s, s, s}), bp, pass);
            }
            if (pass == 1 && a.trail[p].n > 1) {
                const App::Trail& tr = a.trail[p];
                float tc[4] = {teamCol[p][0] * 1.2f, teamCol[p][1] * 1.2f, teamCol[p][2] * 1.2f, 1};
                a.gfx.drawRibbon(tr.core, tr.n, g.ballRadius(p) * 0.7f, tc);
                if (length(b.spin) > 3.0f || b.state == BallState::Spawning) {   // twisting tracers: spin, or the spawn rise
                    float hc[4] = {0.9f, 0.9f, 0.9f, 1};
                    a.gfx.drawRibbon(tr.helixA, tr.n, 0.012f, hc);
                    a.gfx.drawRibbon(tr.helixB, tr.n, 0.012f, hc);
                }
            }
            if (pass == 1 && b.state != BallState::Spawning) {
                // electric arcs crackling over the plasma sphere: jagged great-circle segments, re-rolled ~18x/s
                const float now2 = float(nowSeconds() - a.startTime);
                const uint32_t tick = (uint32_t)(now2 * 18.f);
                const float R0 = g.ballRadius(p) * 1.08f;
                const int nArcs = harmless ? 0 : b.charged ? 3 : 1;
                for (int k = 0; k < nArcs; ++k) {
                    uint32_t sd = fxHash(tick * 131u + (uint32_t)p * 7919u + (uint32_t)k * 104729u);
                    Vec3 ax = normalize(Vec3{fxRnd(sd, 1) - 0.5f, fxRnd(sd, 2) - 0.5f, fxRnd(sd, 3) - 0.5f} + Vec3{1e-3f, 0, 0});
                    Vec3 u0 = normalize(cross(ax, std::fabs(ax.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}));
                    float start = fxRnd(sd, 4) * 6.2832f, span = 1.2f + 1.4f * fxRnd(sd, 5);
                    Vec3 pts[12];
                    for (int i = 0; i < 12; ++i) {
                        float t = i / 11.f;
                        Vec3 d = Quat::axisAngle(ax, start + span * t).rotate(u0);
                        float jit = (i == 0 || i == 11) ? 0.f : (fxRnd(sd, 10 + i) - 0.5f) * 0.35f;
                        Vec3 side = normalize(cross(ax, d));
                        pts[i] = b.pos + (d * (1.f + 0.12f * jit) + side * jit * 0.45f) * R0;
                    }
                    float flick = 0.55f + 0.45f * fxRnd(sd, 30);
                    float ac[4] = {0.45f + 0.55f * teamCol[p][0] * flick, 0.45f + 0.55f * teamCol[p][1] * flick,
                                   0.45f + 0.55f * teamCol[p][2] * flick, 1};
                    a.gfx.drawRibbon(pts, 12, 0.0035f + 0.002f * flick, ac);
                }
            }
            if (pass == 1) {
                float core[4] = {teamCol[p][0], teamCol[p][1], teamCol[p][2], 0.9f};
                if (!b.charged) core[3] = 0.25f;
                a.gfx.drawSphere(b.pos, g.ballRadius(p) * 0.55f, core, true);
            }
        }
    }
    if (pass == 1 && only < 0)
        for (int p = 0; p < 2; ++p)
            if (a.trail[p].burst > 0) {   // ball dissolving at the back wall
                float k = a.trail[p].burst / 0.45f;
                float c[4] = {teamCol[p][0] * k * 2, teamCol[p][1] * k * 2, teamCol[p][2] * k * 2, 1};
                a.gfx.drawSphere(a.trail[p].burstPos, 0.1f + (1 - k) * 0.6f, c, true);
            }
    for (int p = 0; p < 2 && !minimal; ++p) {   // strike-zone rings
        DrawParams zp;
        float f = 0.35f + 1.5f * a.trail[p].strikeFlash;
        zp.forceShader = SH_ADDITIVE;
        zp.tintMul[0] = teamCol[p][0] * f; zp.tintMul[1] = teamCol[p][1] * f; zp.tintMul[2] = teamCol[p][2] * f;
        a.gfx.draw(a.strikezone, Mat4::fromPose({Quat{}, g.strikeZoneCenter(p)}), zp, pass);
    }
    if (!minimal) drawRoomPeople(a, pass);
    if (!minimal) drawLobby(a, pass);
    if (pass == 1 && !a.rttPass) drawVoiceHud(a);
    if (pass == 1 && only < 0) drawFx(a);
    if (pass == 1 && !minimal) {
        // Holographic scoreboard over centre court, facing the local player (mirrored for the far-end player)
        char buf[96];
        const float white[4] = {1, 1, 1, 1}, grey[4] = {0.55f, 0.6f, 0.68f, 1};
        const float* myCol = teamCol[L];
        const float* opCol = teamCol[1 - L];
        int secs = (int)std::ceil(std::max(0.f, g.roundClock));
        const float sx = (L == 0) ? 1.f : -1.f;
        Quat faceP0 = Quat::axisAngle({0, 1, 0}, deg2rad(L == 0 ? -90.f : 90.f));   // quad +Z -> toward local player
        Vec3 c0{-0.5f * sx, 2.95f, 0};
        if (a.spectating || inLobby(a)) { faceP0 = Quat{}; c0 = {0, 4.75f, 0.2f}; }   // above the roof, facing the gallery seat / lobby
        else if (a.net.localWaiting()) { faceP0 = Quat::axisAngle({0, 1, 0}, kPi); c0 = {0, 4.75f, -0.2f}; }   // facing the holding area
        auto at = [&](float u, float v, float w = 0.f) { return c0 + faceP0.rotate({u, v, w}); };
        auto txt = [&](const char* t, float u, float v, float h, const float* c) { a.gfx.drawText(t, {faceP0, at(u, v, 0.004f)}, h, c); };
        const float board[4] = {0.004f, 0.008f, 0.018f, 0.72f};
        a.gfx.drawPanel(Mat4::fromPose({faceP0, at(0, 0, -0.01f)}, {2.4f, 0.66f, 1}), board);
        const float edgeL[4] = {myCol[0], myCol[1], myCol[2], 0.95f}, edgeR[4] = {opCol[0], opCol[1], opCol[2], 0.95f};
        a.gfx.drawPanel(Mat4::fromPose({faceP0, at(-1.19f, 0, -0.005f)}, {0.025f, 0.66f, 1}), edgeL);
        a.gfx.drawPanel(Mat4::fromPose({faceP0, at(1.19f, 0, -0.005f)}, {0.025f, 0.66f, 1}), edgeR);
        const float myC[4] = {myCol[0] * 0.6f + 0.4f, myCol[1] * 0.6f + 0.4f, myCol[2] * 0.6f + 0.4f, 1};
        const float opC[4] = {opCol[0] * 0.6f + 0.4f, opCol[1] * 0.6f + 0.4f, opCol[2] * 0.6f + 0.4f, 1};
        snprintf(buf, sizeof buf, "%d", g.pl[L].score);
        txt(buf, -0.80f, 0.04f, 0.36f, myC);
        snprintf(buf, sizeof buf, "%d", g.pl[1 - L].score);
        txt(buf, 0.80f, 0.04f, 0.36f, opC);
        drawFlag(a, a.sideNation[L], {faceP0, at(-0.50f, 0.04f, 0.003f)}, 0.21f);   // NATIONS: each side's flag
        drawFlag(a, a.sideNation[1 - L], {faceP0, at(0.50f, 0.04f, 0.003f)}, 0.21f);
        (void)secs;
        const std::string clk = clockLine(g);
        if (g.tun.advanced) {
            snprintf(buf, sizeof buf, "ROUND %d", std::max(1, g.round));
            txt(buf, 0, 0.14f, 0.11f, white);
            snprintf(buf, sizeof buf, "%d - %d", g.pl[L].roundWins, g.pl[1 - L].roundWins);
            txt(buf, 0, -0.02f, 0.09f, grey);
            if (!clk.empty()) txt(clk.c_str(), 0, -0.13f, clk.size() > 6 ? 0.05f : 0.07f, g.overtime ? edgeR : white);   // a timed round
        } else {
            txt(clk.c_str(), 0, 0.06f, clk.size() > 6 ? 0.10f : 0.20f, g.overtime ? edgeR : white);
            int queued = 0;   // FREE PLAY never ends a match: say so when people are waiting (owner, 2026-10-21: A + C)
            for (const RoomInfo::Member& m : a.net.room().members) queued += m.queuePos >= 0;
            if (g.tun.freePlay() && queued > 0 && a.net.inRoom())
                txt(queued == 1 ? "1 PLAYER WAITING - FORFEIT TO HAND OVER" : "PLAYERS WAITING - FORFEIT TO HAND OVER", 0, -0.11f, 0.045f, edgeR);
            else
                txt(g.tun.shields ? "BASIC" : "BASIC - HOTDOG", 0, -0.11f, 0.055f, grey);
        }
        snprintf(buf, sizeof buf, "STRIKES %d", g.pl[1 - L].strikesAgainst);
        txt(buf, -0.80f, -0.22f, 0.05f, grey);
        snprintf(buf, sizeof buf, "STRIKES %d", g.pl[L].strikesAgainst);
        txt(buf, 0.80f, -0.22f, 0.05f, grey);
        txt(g.pl[L].ball.charged ? "BALL CHARGED" : "NO CHARGE", 0, -0.24f, 0.045f, g.pl[L].ball.charged ? myC : grey);
        snprintf(buf, sizeof buf, "%s  -  %s", a.net.status(), rulesLine(g.tun).c_str());
        txt(buf, 0, -0.42f, 0.045f, grey);
        const char* banner = nullptr;
        switch (g.match) {
        case MatchState::WaitingForPlayers: banner = "WAITING FOR PLAYERS"; break;
        case MatchState::Meet: snprintf(buf, sizeof buf, "FIST BUMP TO START   %d", (int)std::ceil(g.stateTimer)); banner = buf; break;
        case MatchState::Countdown: snprintf(buf, sizeof buf, "ROUND %d   %d", g.round, (int)std::ceil(g.stateTimer)); banner = buf; break;
        case MatchState::RoundSummary: banner = g.lastRoundWinner == L ? "ROUND WON" : "ROUND LOST"; break;
        case MatchState::MatchSummary:
            if (inLobby(a) || a.spectating || a.net.localWaiting()) {   // BLUE WINS, or the country: UNITED STATES WINS
                snprintf(buf, sizeof buf, "%s WINS", sideName(a, g.matchWinner == 1 ? 1 : 0));
                banner = buf;
            } else banner = g.matchWinner == L ? "YOU WIN  -  PRESS A" : "YOU LOSE  -  PRESS A";
            break;
        default: break;
        }
        if (banner) a.gfx.drawText(banner, {faceP0, {-1.5f * sx, 2.2f, 0}}, 0.15f, white);
        if (a.bumpFlash > 0 && !a.spectating && !a.net.localWaiting()) {
            const float gold[4] = {1.0f, 0.85f, 0.3f, std::min(1.f, a.bumpFlash * 2.f)};
            const Quat face = Quat::axisAngle({0, 1, 0}, deg2rad(L == 0 ? -90.f : 90.f));
            a.gfx.drawText("FIST BUMP!", {face, {L == 0 ? 1.6f : -1.6f, 2.30f, 0}}, 0.12f, gold);
        }
        if (a.foulFlash > 0) {   // FOUL over the spot, rising and fading, turned to whoever is looking
            const float amber[4] = {1.0f, 0.62f, 0.2f, std::min(1.f, a.foulFlash * 1.6f)};
            const Vec3 at = a.foulAt + Vec3{0, 0.45f + 0.25f * (1.2f - a.foulFlash), 0};
            const Vec3 to = a.viewHead.p - at;
            const Quat face = Quat::axisAngle({0, 1, 0}, std::atan2(-to.x, -to.z) + kPi);
            a.gfx.drawText("FOUL", {face, at}, 0.22f, amber);
        }
        if (g.match == MatchState::Meet || a.bumpFlash > 0) {   // the meeting point: a ring for each player, in their colour
            // faded until a closed fist is in it (lit: offered), stronger as the two fists come together, gold at the bump
            float gap = 9.f;
            for (int h0 = 0; h0 < 2; ++h0)
                for (int h1 = 0; h1 < 2; ++h1)
                    if (g.inMeetRing(0, h0) && g.inMeetRing(1, h1)) gap = std::min(gap, g.fistGap(0, h0, 1, h1));
            const float nearness = clampf(1.f - gap / 0.35f, 0.f, 1.f);
            for (int p = 0; p < 2; ++p) {
                const bool lit = g.meetLit(p, 0) || g.meetLit(p, 1);
                const Vec3 c = g.meetRing(p);
                Vec3 pts[41];
                for (int i = 0; i < 41; ++i) {
                    const float t = i * (6.2832f / 40.f);
                    pts[i] = c + Vec3{0, std::sin(t), std::cos(t)} * g.tun.meetRingRadius;
                }
                float k = lit ? 0.75f + 0.25f * nearness : 0.10f + 0.25f * nearness;
                float rgb[3] = {a.sideRGB[p][0], a.sideRGB[p][1], a.sideRGB[p][2]};
                if (a.bumpFlash > 0) { k = a.bumpFlash; rgb[0] = 1.0f; rgb[1] = 0.85f; rgb[2] = 0.3f; }
                const float col[4] = {rgb[0] * (0.4f + 1.2f * k), rgb[1] * (0.4f + 1.2f * k), rgb[2] * (0.4f + 1.2f * k), 0.25f + 0.75f * k};
                a.gfx.drawRibbon(pts, 41, lit || a.bumpFlash > 0 ? 0.012f + 0.008f * nearness : 0.007f, col, false);
            }
        }
        if (g.match == MatchState::Meet && a.bumpFlash <= 0 && !a.spectating && !a.net.localWaiting()) {   // between the two players
            const float cyanT[4] = {0.3f, 0.85f, 1.0f, 1};
            const Quat face = Quat::axisAngle({0, 1, 0}, deg2rad(L == 0 ? -90.f : 90.f));
            a.gfx.drawText("OFFER A FIST IN YOUR RING, OR MEET THEIRS", {face, {L == 0 ? 1.6f : -1.6f, 2.15f, 0}}, 0.07f, cyanT);   // above the other player
            snprintf(buf, sizeof buf, "OR THE MATCH STARTS IN %d", (int)std::ceil(g.stateTimer));
            a.gfx.drawText(buf, {face, {L == 0 ? 1.6f : -1.6f, 2.03f, 0}}, 0.05f, white);
        }
        if (const char* note = a.net.noticeText()) {
            const float amber[4] = {1.0f, 0.75f, 0.2f, 1};
            a.gfx.drawText(note, {faceP0, {-1.5f * sx, 1.9f, 0}}, 0.12f, amber);
        }
        if (!a.rttPass && a.ptFadeNow > 0.f) a.gfx.fadeToRoom(a.ptFadeNow);   // the world toward the room, not the menus
        if (!a.rttPass) drawMenu(a);
    }
    if (!a.rttPass) drawBuilder(a, pass);
    if (!a.rttPass) drawTuning(a, pass);
    if (!a.rttPass) drawTitleShowcase(a, pass);
    if (pass == 1) drawDirector(a);
}

// One frame of the game for the local viewer: menus, rooms, spectating, the match, voice, events and effects.
// The headset frame (renderFrame) and the desktop spectator window (desktopFrame) both run it.
// RECORD MY MATCHES: from a match's start to its end, while this player plays it (not spectating, waiting or in the
// lobby). MATCH MY LEVEL's level is kept when a match ends.
static void recordStep(App& a, float dt) {
    bool start = false, over = false;
    for (const GameEvent& e : a.game.events) {
        start |= e.type == EventType::RoundStart && e.value <= 1.f;
        over |= e.type == EventType::MatchEnd;
    }
    if (over && a.game.tun.botAdapt) saveSettings(a);
    const bool playing = a.recordOn && !a.viewing && !inLobby(a) && a.net.mode() != Net::Mode::Spectate;
    if (a.recorder.active() && (!playing || a.game.match == MatchState::WaitingForPlayers || a.game.match == MatchState::Meet)) {
        a.recorder.end(a.game);
        LOGI("record: stopped (%s)", a.recorder.path().c_str());
    }
    if (playing && start && !a.recorder.active()) {
        if (const char* dir = dataDir(a)) {
            const std::string folder = std::string(dir) + "/recordings";
#ifdef _WIN32
            CreateDirectoryA(folder.c_str(), nullptr);
#else
            mkdir(folder.c_str(), 0770);
#endif
            rec::prune(folder, rec::kKeep - 1);
            char name[48];
            const time_t now = time(nullptr);
            strftime(name, sizeof name, "/match_%Y%m%d_%H%M%S.par", localtime(&now));
            rec::Meta m;
            m.local = a.net.localIndex();
            m.version = kAppVersion;
            if (a.recorder.begin(folder + name, a.game, m)) LOGI("record: %s", a.recorder.path().c_str());
            else LOGE("record: can't write %s", (folder + name).c_str());
        }
    }
    if (!a.recorder.active()) return;
    a.recorder.frame(a.game, dt);
    if (over) {
        a.recorder.end(a.game);
        LOGI("record: match over (%s)", a.recorder.path().c_str());
    }
}

static void stepFrame(App& a, PlayerInput in[2], int L, double t, float dt) {
    a.sparc.update(t);   // EXPERIMENTAL cross-play link (does nothing unless connected)
    // the EXPERIMENTAL arena: practice against the bot for now (the lobby, rooms and spectating stay on the classic court)
    const Net::Mode nm = a.net.mode();
    const bool exp = a.game.tun.experimental && !inLobby(a) && !a.spectating && !a.net.inRoom() &&
                     nm != Net::Mode::Host && nm != Net::Mode::Client && nm != Net::Mode::Spectate;
    a.game.useArena(!exp ? ARENA_CLASSIC : a.game.tun.expRoom == 1 && kShowCorners ? ARENA_CORNERS : ARENA_DIAMOND, a.classicCol);
    if (int done = a.bugDone.exchange(0)) {
        std::lock_guard<std::mutex> lk(a.bugMx);
        if (!a.bugStatus.empty()) a.net.noticeLocal(a.bugStatus.c_str());
        (void)done;
    }

    a.localIn = in[L];
    a.viewHead = in[L].head;
    a.bumpFlash = std::max(0.f, a.bumpFlash - dt);
    a.foulFlash = std::max(0.f, a.foulFlash - dt);
    updateCrowd(a, dt);
    {   // a guest plays by the host's rules; ours come back when we leave (and are never saved over)
        const bool guest = a.net.mode() == Net::Mode::Client || a.net.mode() == Net::Mode::Spectate;
        if (!guest && a.guestRules) {
            Tunables& t = a.game.tun;
            t.advanced = a.ownRules.advanced; t.shields = a.ownRules.shields; t.knuckleMode = a.ownRules.knuckleMode;
            t.basicMatchTime = a.ownRules.basicMatchTime; t.advancedRoundTime = a.ownRules.advancedRoundTime;
            a.guestRules = false;
            if (a.game.match == MatchState::WaitingForPlayers || a.net.mode() == Net::Mode::Offline) a.game.reset(a.game.pl[1].isBot);
            LOGI("rules -> %s (our own again)", rulesLine(a.game.tun).c_str());
        } else if (!guest) {
            a.ownRules = a.game.tun;
        } else {
            a.guestRules = true;
        }
    }
    if (a.rulesPending && !rulesLive(a)) {   // MATCH OPTIONS changed during a match: in force from now on
        a.rulesPending = false;
        const bool reset = a.game.match == MatchState::WaitingForPlayers;
        optionsInto(a, a.game.tun);
        if (reset) a.game.reset(a.game.pl[1].isBot);
        saveSettings(a);
        a.net.noticeLocal("NEW RULES IN FORCE");
        LOGI("rules -> %s (the match ended)", rulesLine(a.game.tun).c_str());
    }
    {   // moving into a room, the tank, spectating or back out closes the menu and the director panel (testers:
        // they stayed open over the match); a report being typed stays open
        const uint32_t key = (uint32_t)a.net.mode() * 64u + (uint32_t)(a.net.localSlot() + 2) * 4u + (a.spectating ? 1u : 0u);
        if (a.stateKey != 0xFFFFFFFFu && key != a.stateKey) {
            if (a.dirOpen) { a.dirOpen = false; a.infoKind = -1; }
            if (a.menuOpen && !a.nameEdit && !a.bugOpen && !a.reportOpen) {
                a.menuOpen = false; a.titleOpen = a.settingsOpen = a.voicePage = a.ptPage = a.gfxPage = a.helpOpen = a.xpOpen = a.rulesOpen = false;
                LOGI("menu: closed (moved to %s)", a.net.status());
            }
        }
        a.stateKey = key;
    }
    if (meetHere(a) && !a.net.localWaiting() && !a.spectating) {
        // closer fists buzz harder (you can feel the other hand coming); otherwise a light reminder every 2 s
        const PlayerState& o = a.game.pl[1 - L];
        float best = 9.f;
        int bh = 1;
        for (int h = 0; h < 2; ++h)
            for (int oh = 0; oh < 2; ++oh)
                if (in[L].hand[h].tracked && o.in.hand[oh].tracked) {
                    const float d = a.game.fistGap(L, h, 1 - L, oh);
                    if (d < best) { best = d; bh = h; }
                }
        if (a.game.meetLog) {
            a.game.meetLog = false;
            LOGI("meet: closest fists %.0f cm (bump under %.0f), triggers %.2f / %.2f", a.game.meetGap * 100, a.game.tun.fistBumpGap * 100,
                 a.game.meetTrig[0], a.game.meetTrig[1]);
        }
        if (best < 0.45f && t - a.meetTick > 0.12) { a.meetTick = t; haptic(a, bh, 0.15f + 0.6f * (1.f - best / 0.45f), 0.05f); }
        else if (t - a.meetTick > 2.0) { a.meetTick = t; haptic(a, 0, 0.25f, 0.06f); haptic(a, 1, 0.25f, 0.06f); }
    }
    if (a.game.pl[1].isBot) a.game.pl[1].in.avatar = a.botAvatar;
    updateHub(a, in[L]);
    updateQueueKiosk(a, in[L]);
    updateLobby(a, in[L]);
    if (a.net.mode() == Net::Mode::Spectate) {
        if (!a.spectating) { a.savedGame = a.game; a.spectating = true; }
        const PlayerInput mine = in[L];
        a.net.setPresence(a.watchIp ? 1 : 0, mine.head.p, a.watchIp);
        a.net.exchange(mine, in, a.game, dt);
        SpecMatch* w = nullptr;
        for (SpecMatch& m : a.net.matches()) if (m.ip == a.watchIp) w = &m;
        if (a.watchIp && !w) { a.watchIp = 0; LOGI("spectate: match ended, back to the hub"); }
        if (w) {   // watched match: its state and events drive the normal renderer, audio and effects
            const std::shared_ptr<const ArenaData> keep = a.game.arena;   // (our arena, not the copy's)
            a.game = w->game;
            a.game.arena = keep;
            a.game.events.swap(w->events);
            w->events.clear();
            a.watchRoom = w->room;
            directorEvents(a);
        } else {
            a.game.events.clear();
        }
    } else {
        if (a.spectating) { a.game = a.savedGame; a.spectating = false; a.watchIp = 0; }
        {   // back to Offline from a room or spectating: back to the lobby
            const int m = (int)a.net.mode();
            if (m == (int)Net::Mode::Offline && a.lobbyPrevMode != m) a.lobby = true;
            a.lobbyPrevMode = m;
            if (inLobby(a)) {
                if (!a.game.pl[0].isBot) startLobbyMatch(a);
                if (a.game.match == MatchState::MatchSummary) {
                    if ((a.lobbySummaryT += dt) > 6.f) startLobbyMatch(a);
                } else {
                    a.lobbySummaryT = 0;
                }
            }
        }
        const PlayerInput mine = in[L];
        if (a.net.exchange(mine, in, a.game, dt)) {
            // practice is paused while you customize your avatar (the builder only opens offline)
            const bool paused = (a.builderOpen || a.tuneOpen || (a.menuOpen && a.titleOpen)) && a.net.mode() != Net::Mode::Host && !inLobby(a);
            if (!paused) a.game.step(dt, in);
            a.net.sendState(a.game);
        }
    }
    a.viewing = a.spectating || a.net.localWaiting() || inLobby(a);
    updateVoice(a);
    {   // team cameras shared by a teammate
        char team[17];
        std::vector<uint8_t> blob;
        if (a.net.takeCameras(team, blob) && !a.team.empty() && a.team == team && unpackWarps(a, blob)) {
            saveWarps(a);
            a.net.noticeLocal("TEAM CAMERAS RECEIVED");
        }
    }
    recordStep(a, dt);
    handleEvents(a);
    {   // the last 10 seconds of a timed match or round: a very soft tick each second (owner, 2026-10-27)
        const Game& g = a.game;
        const int left = g.match == MatchState::InProgress && g.tun.timed() && !g.overtime && g.roundClock > 0.f && !inLobby(a)
                             ? (int)std::ceil(g.roundClock) : 0;
        if (left >= 1 && left <= 10 && left != a.clockTick) a.audio.play("oa_ClockTick", 0.6f);
        a.clockTick = left;
    }
    smoothPlayers(a, dt);
    a.fadeT = std::max(0.f, a.fadeT - dt);
    updateFx(a, dt);
    for (float& f : a.hitFlash) f = std::max(0.f, f - dt);
    for (int p = 0; p < 2; ++p) {
        App::Trail& tr = a.trail[p];
        const Ball& b = a.game.pl[p].ball;
        tr.strikeFlash = std::max(0.f, tr.strikeFlash - dt);
        tr.burst = std::max(0.f, tr.burst - dt);
        const bool rising = b.state == BallState::Spawning && b.pos.y > 0.f;
        if (b.state != BallState::Flying && b.state != BallState::Returning && !rising) { tr.n = 0; continue; }
        float sl = rising ? 10.f : length(b.spin);
        tr.spinAngle += sl * dt;
        Vec3 axis = rising ? Vec3{0, 1, 0} : sl > 1e-3f ? b.spin / sl : Vec3{0, 1, 0};
        Vec3 perp = normalize(cross(axis, std::fabs(axis.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}));
        float hr = a.game.ballRadius(p) * 1.3f;
        Vec3 offA = Quat::axisAngle(axis, tr.spinAngle).rotate(perp) * hr;
        if (tr.n == 48) {
            for (int i = 1; i < 48; ++i) { tr.core[i - 1] = tr.core[i]; tr.helixA[i - 1] = tr.helixA[i]; tr.helixB[i - 1] = tr.helixB[i]; }
            tr.n = 47;
        }
        tr.core[tr.n] = b.pos;
        tr.helixA[tr.n] = b.pos + offA;
        tr.helixB[tr.n] = b.pos - offA;
        tr.n++;
    }
    for (const GameEvent& e : a.game.events)
        if (e.type == EventType::Strike && e.player >= 0) a.trail[e.player].strikeFlash = 1.0f;
    if (a.devMode) {   // diagnostics: when the opponent's ball passes the local player's hands, log ball vs bumper geometry
        static float prevX[2] = {0, 0};
        const int Lp = a.net.localIndex(), O = 1 - Lp;
        const Ball& ob = a.game.pl[O].ball;
        float hx = a.game.pl[Lp].in.head.p.x;
        if (ob.state == BallState::Flying && ob.instigator == O && (prevX[O] - hx) * (ob.pos.x - hx) < 0) {
            for (int h = 0; h < 2; ++h) {
                Pose kp = a.game.knucklePose(Lp, h);
                Vec3 loc = kp.q.conj().rotate(ob.pos - kp.p);
                LOGI("diag ball@head-plane: hand %d knuckleUp %d trigger %.2f ball-in-bumper-frame (%.2f %.2f %.2f) dist %.2f",
                     h, (int)a.game.pl[Lp].hands[h].knuckleUp, a.game.pl[Lp].in.hand[h].trigger, loc.x, loc.y, loc.z, length(ob.pos - kp.p));
            }
        }
        prevX[O] = ob.pos.x;
        for (const GameEvent& e : a.game.events)
            if (e.type == EventType::HitPlayer && e.player == Lp)
                for (int h = 0; h < 2; ++h) {
                    Pose kp = a.game.knucklePose(Lp, h);
                    Vec3 loc = kp.q.conj().rotate(e.pos - kp.p);
                    Vec3 fwd = kp.q.rotate({0, 0, -1});
                    LOGI("diag HIT: hand %d hit-in-bumper-frame (%.2f %.2f %.2f) dist %.2f bumperFwd (%.2f %.2f %.2f) head (%.2f %.2f %.2f)",
                         h, loc.x, loc.y, loc.z, length(e.pos - kp.p), fwd.x, fwd.y, fwd.z,
                         a.game.pl[Lp].in.head.p.x, a.game.pl[Lp].in.head.p.y, a.game.pl[Lp].in.head.p.z);
                }
    }
    for (const GameEvent& e : a.game.events)
        if (e.type == EventType::Returned && e.player >= 0) { a.trail[e.player].burst = 0.45f; a.trail[e.player].burstPos = e.pos; a.trail[e.player].n = 0; }

}

static bool renderFrame(App& a) {
    XrFrameWaitInfo fw{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    XR_CHECK(xrWaitFrame(a.session, &fw, &fs));
    XrFrameBeginInfo fb{XR_TYPE_FRAME_BEGIN_INFO};
    XR_CHECK(xrBeginFrame(a.session, &fb));

    double t = nowSeconds();
    float dt = a.lastTime > 0 ? float(t - a.lastTime) : 0.f;
    a.lastTime = t;
    XrTime predicted = fs.predictedDisplayTime;
    const int L = a.net.localIndex();
    PlayerInput in[2];
    in[L] = readInput(a, predicted, L);
    stepFrame(a, in, L, t, dt);

    std::vector<XrCompositionLayerProjectionView> projViews(a.views.size(), {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::vector<XrCompositionLayerBaseHeader*> layers;
    {   // ROOM AT EDGES and PEEK (Y): fade the game toward the room
        float target = 0.f;
        // only with a passthrough mode on: OFF is off (owner, 2026-10-18: with OFF picked the room still showed)
        if (a.hasPassthrough && !a.ptFailed && !a.spectating && a.ptMode != PT_OFF) {
            if (a.ptEdgeReveal) {
                // the head from 35 cm inside the edge (full at 10 cm), a hand only once it reaches past the edge (full
                // 15 cm out). A small (stationary) boundary: the hands only (owner, 2026-10-18: a 1.2 m boundary kept
                // the court half faded all the time, the desk showing through the floor and the roof)
                float dh, dk;
                edgeDistance(a, predicted, dh, dk);
                const bool smallBounds = a.hasBounds && std::min(a.stageExt.width, a.stageExt.height) < 1.8f;
                const float th = smallBounds ? 0.f : clampf((0.35f - dh) / 0.25f, 0.f, 1.f);
                const float tk = clampf(-dk / 0.15f, 0.f, 1.f);
                // the head itself outside the boundary: it does not describe where you play (owner, 2026-10-19: a
                // 1.2 m stationary boundary, the head 0.58 m outside it, kept the room at 85% all the time); the Quest
                // shows your room itself when you really leave it
                const bool outside = a.hasBounds && dh < 0.f;
                target = outside ? 0.f : std::max(th, tk) * 0.85f;
                if (outside && t > a.ptRevealLogAt) {
                    a.ptRevealLogAt = t + 30.0;
                    LOGI("room at edges: the head is %.2f m outside the boundary (%.2f x %.2f m), ignored", -dh,
                         a.stageExt.width, a.stageExt.height);
                }
                if (target > 0 && t > a.ptRevealLogAt) {   // why the room showed (bug reports carry the log)
                    a.ptRevealLogAt = t + 5.0;
                    LOGI("room at edges: head %.2f m, hand %.2f m from the edge (%s %.2f x %.2f m)", dh, dk,
                         a.hasBounds ? "boundary" : "circle", a.stageExt.width, a.stageExt.height);
                }
            }
        }
        if (a.hasPassthrough && !a.ptFailed && !a.spectating && a.ptPeekOn && a.peekHeld) target = 0.95f;   // PEEK (Y)
        a.ptReveal += (target - a.ptReveal) * std::min(1.f, dt * (target > a.ptReveal ? 10.f : 4.f));
        if (a.ptReveal < 0.003f) a.ptReveal = 0.f;
    }
    updatePassthrough(a);
    a.mrView = a.ptRunning && a.ptMode != PT_OFF && !(a.menuOpen && a.settingsOpen && a.gfxPage);   // (GRAPHICS: the arena)
    updateDepth(a, predicted);
    XrCompositionLayerPassthroughFB ptl{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    const bool ptOn = a.ptRunning;
    if (ptOn) {   // your room first; the game over it where its image is opaque
        ptl.layerHandle = a.ptLayer;
        ptl.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        layers.push_back((XrCompositionLayerBaseHeader*)&ptl);
        layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;   // premultiplied: glows add light to the room
    }
    if (fs.shouldRender) renderInfoPreview(a);
    if (fs.shouldRender) {
        XrViewLocateInfo vl{XR_TYPE_VIEW_LOCATE_INFO};
        vl.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vl.displayTime = predicted;
        vl.space = a.appSpace;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        uint32_t nv = 0;
        XR_CHECK(xrLocateViews(a.session, &vl, &vs, (uint32_t)a.views.size(), &nv, a.views.data()));
        for (uint32_t i = 0; i < nv && i < 2; ++i) {
            Swap& s = a.swaps[i];
            uint32_t idx = 0;
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XR_CHECK(xrAcquireSwapchainImage(s.handle, &ai, &idx));
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = XR_INFINITE_DURATION;
            XR_CHECK(xrWaitSwapchainImage(s.handle, &wi));
            glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.images[idx].image, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s.depth);
            glViewport(0, 0, s.w, s.h);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
            if (a.mrView) glClearColor(0, 0, 0, 0);   // nothing drawn = your room
            else glClearColor(0.01f, 0.012f, 0.02f, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            Pose eye = a.trackToWorld * fromXr(a.views[i].pose);
            if (a.roomFeel != 0 && nv >= 2) {   // FIELD OF VIEW: eye separation x 1/(1 + 5% a step); the headset gets the real poses
                const Vec3 mid = (a.trackToWorld * fromXr(a.views[0].pose)).p * 0.5f + (a.trackToWorld * fromXr(a.views[1].pose)).p * 0.5f;
                eye.p = mid + (eye.p - mid) * (1.f / (1.f + 0.05f * a.roomFeel));
            }
            a.ptFadeNow = ptOn ? a.ptReveal : 0.f;   // ROOM AT EDGES / PEEK (drawWorld fades the world under the menus)
            a.curEye = (int)i;
            const XrFovf& f = a.views[i].fov;
            Mat4 proj = Mat4::projection(f.angleLeft, f.angleRight, f.angleUp, f.angleDown, 0.05f, 2000.f);
            a.gfx.beginEye(Mat4::viewFromPose(eye), proj, eye.p);
            drawWorld(a, 0);
            drawWorld(a, 1);
            if (a.fadeT > 0 && a.capTex[i]) a.gfx.drawScreenTexture(a.capTex[i], std::min(1.f, a.fadeT / 0.5f));
            if (a.fadeCapture) {   // keep this view to cross-fade from after the warp
                if (!a.capTex[i]) {
                    glGenTextures(1, &a.capTex[i]);
                    glBindTexture(GL_TEXTURE_2D, a.capTex[i]);
                    glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, s.w, s.h);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glGenFramebuffers(1, &a.capFbo[i]);
                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, a.capFbo[i]);
                    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, a.capTex[i], 0);
                }
                glBindFramebuffer(GL_READ_FRAMEBUFFER, s.fbo);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, a.capFbo[i]);
                glBlitFramebuffer(0, 0, s.w, s.h, 0, 0, s.w, s.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            }
#ifndef __ANDROID__
            if (i == 0) mirrorToWindow(a, s);
#endif
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            XR_CHECK(xrReleaseSwapchainImage(s.handle, &ri));
            projViews[i].pose = a.views[i].pose;
            projViews[i].fov = a.views[i].fov;
            projViews[i].subImage.swapchain = s.handle;
            projViews[i].subImage.imageRect = {{0, 0}, {s.w, s.h}};
        }
        if (a.fadeCapture) { a.fadeCapture = false; a.fadePending = true; }
        layer.space = a.appSpace;
        layer.viewCount = (uint32_t)projViews.size();
        layer.views = projViews.data();
        layers.push_back((XrCompositionLayerBaseHeader*)&layer);
    }
    XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO};
    fe.displayTime = predicted;
    fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fe.layerCount = (uint32_t)layers.size();
    fe.layers = layers.data();
    XR_CHECK(xrEndFrame(a.session, &fe));
    {   // performance, every 10 s in the log (bug reports carry it): frame rate, the slowest frame, triangles per frame
        static double since = 0, worst = 0;
        static int frames = 0;
        static uint64_t tris0 = 0;
        if (since == 0) { since = t; tris0 = a.gfx.trisDrawn; }
        ++frames;
        worst = std::max(worst, (double)dt);
        if (t - since >= 10.0) {
            LOGI("perf: %.1f fps, slowest frame %.1f ms, %llu triangles per frame (both eyes)", frames / (t - since), worst * 1000.0,
                 (unsigned long long)((a.gfx.trisDrawn - tris0) / (uint64_t)std::max(1, frames)));
            since = t; frames = 0; worst = 0; tris0 = a.gfx.trisDrawn;
        }
    }
    return true;
}

static void pollXrEvents(App& a) {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(a.instance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            auto* sc = (XrEventDataSessionStateChanged*)&ev;
            a.state = sc->state;
            LOGI("session state -> %d", (int)a.state);
            if (a.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(a.session, &bi))) {
                    a.running = true;
                    if (a.xrRequestDisplayRefreshRateFB) a.xrRequestDisplayRefreshRateFB(a.session, 90.0f);
                }
            } else if (a.state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(a.session);
                a.running = false;
            } else if (a.state == XR_SESSION_STATE_EXITING || a.state == XR_SESSION_STATE_LOSS_PENDING) {
                a.running = false;
                a.quit = true;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
            // Quest re-estimates LOCAL_FLOOR / STAGE origins while tracking settles (and on system recenter).
            // Keep the court fixed relative to the real room by composing the origin shift into trackToWorld:
            // world = trackToWorld_old * pose_prev(newOrigin) * p_new. The user recenters explicitly with B.
            auto* rc = (XrEventDataReferenceSpaceChangePending*)&ev;
            if (rc->referenceSpaceType != a.spaceType) break;
            const XrPosef& d = rc->poseInPreviousSpace;
            const float moved = std::sqrt(d.position.x * d.position.x + d.position.z * d.position.z);
            const float turned = 2.f * std::acos(std::min(1.f, std::fabs(d.orientation.w))) * 57.2958f;
            if (rc->poseValid && a.recentered && moved < 0.25f && turned < 10.f) {
                // small: tracking settling. Keep the court fixed across the room, but follow a new floor height
                // (compensating the height kept the court at the old, wrong floor)
                XrPosef dxz = rc->poseInPreviousSpace;
                dxz.position.y = 0;
                a.trackToWorld = a.trackToWorld * fromXr(dxz);
                LOGI("reference space shifted (%.3f, %.3f, %.3f): compensated across, floor %+.3f m followed",
                     d.position.x, d.position.y, d.position.z, d.position.y);
            } else if (rc->poseValid && a.recentered) {
                // large: the player held the Meta button (system recenter): put them back on their spot, facing in
                LOGI("system recenter (moved %.2f m, turned %.0f deg): re-placing the player", moved, turned);
                a.recentered = false;
            } else {
                a.recentered = false;   // no valid delta: re-anchor on the next tracked frame
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            a.quit = true;
            break;
        default: break;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

#ifdef __ANDROID__
static void onAppCmd(android_app* app, int32_t cmd) {
    App& a = *(App*)app->userData;
    switch (cmd) {
    case APP_CMD_RESUME: a.resumed = true; a.audio.resume(); break;
    case APP_CMD_PAUSE: a.resumed = false; a.audio.pause(); a.lastTime = 0; break;
    case APP_CMD_DESTROY: a.quit = true; break;
    default: break;
    }
}
#endif

// <externalDataPath>/settings.cfg: rules, shield placement and gauntlet tuning survive restarts
// <externalDataPath>/warps.cfg: one warp per line "x y z yaw" (arena space; shared by every arena)
static std::vector<uint8_t> packWarps(const App& a) {   // for team sharing: 19 bytes per camera
    std::vector<uint8_t> b;
    b.push_back((uint8_t)a.warps.size());
    for (const App::WarpCam& w : a.warps) {
        Vec3 f = w.pose.q.rotate({0, 0, -1});
        float v[4] = {w.pose.p.x, w.pose.p.y, w.pose.p.z, std::atan2(-f.x, -f.z)};
        const uint8_t* q = (const uint8_t*)v;
        b.insert(b.end(), q, q + 16);
        b.push_back(w.hold); b.push_back(w.trans); b.push_back(w.trig);
    }
    return b;
}

static bool unpackWarps(App& a, const std::vector<uint8_t>& b) {
    if (b.empty() || b[0] > 8 || b.size() < 1 + 19u * b[0]) return false;
    std::vector<App::WarpCam> ws;
    for (int i = 0; i < b[0]; ++i) {
        float v[4];
        std::memcpy(v, &b[1 + 19 * i], 16);
        if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2]) || std::fabs(v[0]) > 60 || std::fabs(v[1]) > 30 || std::fabs(v[2]) > 60) return false;
        App::WarpCam w;
        w.pose = {Quat::axisAngle({0, 1, 0}, v[3]), {v[0], v[1], v[2]}};
        w.hold = b[17 + 19 * i] & 3; w.trans = b[18 + 19 * i] & 1; w.trig = std::min<uint8_t>(2, b[19 + 19 * i]);
        ws.push_back(w);
    }
    a.warps = ws;
    a.warpIdx = -1;
    return true;
}

static void saveWarps(App& a) {
    const char* ext = dataDir(a);
    if (!ext) return;
    if (FILE* fp = fopen((std::string(ext) + "/warps.cfg").c_str(), "w")) {
        for (const App::WarpCam& w : a.warps) {
            Vec3 f = w.pose.q.rotate({0, 0, -1});
            fprintf(fp, "%.3f %.3f %.3f %.4f %d %d %d\n", w.pose.p.x, w.pose.p.y, w.pose.p.z, std::atan2(-f.x, -f.z), w.hold, w.trans, w.trig);
        }
        fclose(fp);
    }
}

static void loadWarps(App& a) {
    const char* ext = dataDir(a);
    if (!ext) return;
    FILE* fp = fopen((std::string(ext) + "/warps.cfg").c_str(), "r");
    if (!fp) return;
    char line[128];
    while (a.warps.size() < 8 && fgets(line, sizeof line, fp)) {
        float x, y, z, yaw;
        int hold = 1, trans = 1, trig = 0;
        if (sscanf(line, "%f %f %f %f %d %d %d", &x, &y, &z, &yaw, &hold, &trans, &trig) < 4) continue;
        App::WarpCam w;
        w.pose = {Quat::axisAngle({0, 1, 0}, yaw), {x, y, z}};
        w.hold = (uint8_t)(hold & 3); w.trans = (uint8_t)(trans & 1); w.trig = (uint8_t)std::min(2, std::max(0, trig));
        a.warps.push_back(w);
    }
    fclose(fp);
}

static void saveSettings(App& a) {
    const char* ext = dataDir(a);
    if (!ext) return;
    std::string f = std::string(ext) + "/settings.cfg";
    if (FILE* fp = fopen(f.c_str(), "w")) {
        fprintf(fp, "cfgver=8\n");
        fprintf(fp, "advanced=%d\nshield=%d\npitch=%.1f\nfwd=%.3f\nup=%.3f\nvolume=%d\nhand=%d\nfx=%d\nhideoutside=%d\nvoice=%d\nmutespec=%d\nmuteplayers=%d\nmuteall=%d\nonline=%d\nbotfill=%d\npassthrough=%d\nptfloor=%.2f\nptglass=%.2f\nptroom=%.2f\nptmono=%d\nptedges=%d\nptreveal=%d\nptpeek=%d\nteam=%s\nserver=%s\nsparcserver=%s\nstatsnotice=%d\ncrowd=%d\nshields=%d\nknuckles=%d\nmatchtime=%.0f\nroundtime=%.0f\nturn=%d\nbotlevel=%d\nbotadapt=%d\nbotmatch=%.2f\nrecord=%d\nbotmeet=%d\nroomfeel=%d\nexperimental=%d\nexproom=%d\n",
                (a.guestRules ? a.ownRules : a.game.tun).advanced ? 1 : 0, a.game.tun.shieldPlacement, a.tunePitchDeg, a.tuneFwd, a.tuneUp, a.volume,
                a.pointerHand, a.fxLevel, a.hideOutside ? 1 : 0, a.voiceMode, a.muteSpectators ? 1 : 0, a.mutePlayers ? 1 : 0,
                a.muteAll ? 1 : 0, a.onlinePref ? 1 : 0, a.botFill ? 1 : 0, a.ptMode, a.ptFloor, a.ptGlass, a.ptRoom, a.ptMono ? 1 : 0, a.ptEdges ? 1 : 0, a.ptEdgeReveal ? 1 : 0, a.ptPeekOn ? 1 : 0, a.team.c_str(),
                (a.serverOverride.empty() ? a.serverAddr : a.serverSaved).c_str(),
                a.sparcAddr.c_str(), a.statsNoticeSeen ? 1 : 0, a.crowdMode, (a.guestRules ? a.ownRules : a.game.tun).shields ? 1 : 0,
                (a.guestRules ? a.ownRules : a.game.tun).knuckleMode, (a.guestRules ? a.ownRules : a.game.tun).basicMatchTime,
                (a.guestRules ? a.ownRules : a.game.tun).advancedRoundTime, a.turnMode, (a.guestRules ? a.ownRules : a.game.tun).botLevel,
                (a.guestRules ? a.ownRules : a.game.tun).botAdapt ? 1 : 0, (a.guestRules ? a.ownRules : a.game.tun).botMatch, a.recordOn ? 1 : 0,
                (a.guestRules ? a.ownRules : a.game.tun).botMeet ? 1 : 0, a.roomFeel, (a.guestRules ? a.ownRules : a.game.tun).experimental ? 1 : 0, (a.guestRules ? a.ownRules : a.game.tun).expRoom);
        fclose(fp);
    }
}

static void loadSettings(App& a) {
    const char* ext = dataDir(a);
    if (!ext) return;
    FILE* fp = fopen((std::string(ext) + "/settings.cfg").c_str(), "r");
    if (!fp) return;
    char line[96];
    while (fgets(line, sizeof line, fp)) {
        if (std::strncmp(line, "team=", 5) == 0) {
            a.team = line + 5;
            while (!a.team.empty() && (a.team.back() == '\n' || a.team.back() == '\r' || a.team.back() == ' ')) a.team.pop_back();
            continue;
        }
        if (std::strncmp(line, "sparcserver=", 12) == 0) {
            std::string v = line + 12;
            while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
            if (!v.empty()) a.sparcAddr = v;
            continue;
        }
        if (std::strncmp(line, "server=", 7) == 0) {
            a.serverAddr = line + 7;
            while (!a.serverAddr.empty() && (a.serverAddr.back() == '\n' || a.serverAddr.back() == '\r' || a.serverAddr.back() == ' '))
                a.serverAddr.pop_back();
            continue;
        }
        char key[32];
        float v;
        if (sscanf(line, "%31[^=]=%f", key, &v) != 2) continue;
        std::string k = key;
        if (k == "cfgver") a.cfgVersion = (int)v;
        else if (k == "advanced") a.game.tun.advanced = v != 0;
        else if (k == "shield") a.game.tun.shieldPlacement = ((int)v) & 3;
        else if (k == "pitch") a.tunePitchDeg = clampf(v, -90, 90);
        else if (k == "fwd") a.tuneFwd = clampf(v, -0.2f, 0.2f);
        else if (k == "up") a.tuneUp = clampf(v, -0.2f, 0.2f);
        else if (k == "volume") a.volume = std::max(0, std::min(10, (int)v));
        else if (k == "hand") a.pointerHand = ((int)v) ? 1 : 0;
        else if (k == "fx") a.fxLevel = std::max(0, std::min(2, (int)v));
        else if (k == "roomfeel") a.roomFeel = std::max(-4, std::min(4, (int)v));
        else if (k == "experimental") a.game.tun.experimental = v != 0;
        else if (k == "exproom") a.game.tun.expRoom = (int)v == 1 ? 1 : 0;
        else if (k == "crowd") a.crowdMode = std::max(0, std::min(2, (int)v));
        else if (k == "turn") a.turnMode = std::max(0, std::min(2, (int)v));
        else if (k == "botlevel") a.game.tun.botLevel = std::max(0, std::min(kBotLevels - 1, (int)v));
        else if (k == "botadapt") a.game.tun.botAdapt = v != 0;
        else if (k == "botmatch") a.game.tun.botMatch = clampf(v, 0.f, kBotLevels - 1.f);
        else if (k == "record") a.recordOn = v != 0;
        else if (k == "botmeet") a.game.tun.botMeet = v != 0;
        else if (k == "shields") a.game.tun.shields = v != 0;   // MATCH OPTIONS (the last ones chosen)
        else if (k == "knuckles") a.game.tun.knuckleMode = std::max(-1, std::min(1, (int)v));
        else if (k == "matchtime") a.game.tun.basicMatchTime = clampf(v, 0.f, 3600.f);
        else if (k == "roundtime") a.game.tun.advancedRoundTime = clampf(v, 0.f, 3600.f);
        else if (k == "hideoutside") a.hideOutside = v != 0;
        else if (k == "passthrough") {}   // always OFF when the game starts (owner, 2026-10-18); the page keeps the rest
        else if (k == "ptfloor") a.ptFloor = clampf(v, 0.f, 1.f);
        else if (k == "ptglass") a.ptGlass = clampf(v, 0.f, 2.f);
        else if (k == "ptroom") a.ptRoom = clampf(v, 0.1f, 1.f);
        else if (k == "ptmono") a.ptMono = v != 0;
        else if (k == "ptedges") a.ptEdges = v != 0;
        else if (k == "ptreveal") a.ptEdgeReveal = v != 0;
        else if (k == "ptpeek") a.ptPeekOn = v != 0;
        else if (k == "statsnotice") a.statsNoticeSeen = v != 0;
        else if (k == "voice") a.voiceMode = std::max(0, std::min(2, (int)v));
        else if (k == "mutespec") a.muteSpectators = v != 0;
        else if (k == "muteplayers") a.mutePlayers = v != 0;
        else if (k == "muteall") a.muteAll = v != 0;
        else if (k == "online") a.onlinePref = v != 0;
        else if (k == "botfill") a.botFill = v != 0;
    }
    fclose(fp);
    a.game.reset(true);
    a.audio.setVolume(a.volume / 10.f);
    LOGI("settings: rules %s, shield %d", a.game.tun.advanced ? "advanced" : "basic", a.game.tun.shieldPlacement);
}

static void savePlayerName(App& a, const char* name) {
    a.net.setName(name);
    if (const char* ext = dataDir(a)) {
        std::string f = std::string(ext) + "/player_name.txt";
        if (FILE* fp = fopen(f.c_str(), "w")) { fprintf(fp, "%s\n", a.net.name()); fclose(fp); }
    }
    LOGI("player name: %s", a.net.name());
}

// Lobby name: <externalDataPath>/player_name.txt (editable over adb); generated once as "PLAYER nnn".
static void loadPlayerName(App& a) {
    a.net.setAppVersion(kAppVersion);   // (the relay logs it)
    const char* ext = dataDir(a);
    if (!ext) return;
    std::string f = std::string(ext) + "/player_name.txt";
    char name[32] = {};
    if (FILE* fp = fopen(f.c_str(), "r")) {
        if (fgets(name, sizeof name, fp)) name[strcspn(name, "\r\n")] = 0;
        fclose(fp);
    }
    if (!name[0]) {
        // (the clock's milliseconds; on the PC the clock is large, and an int of it wrapped to names like PLAYER -648)
        snprintf(name, sizeof name, "PLAYER %03d", (int)std::fmod(nowSeconds() * 1000.0, 1000.0));
        if (FILE* fp = fopen(f.c_str(), "w")) { fprintf(fp, "%s\n", name); fclose(fp); }
    }
    name[16] = 0;
    a.net.setName(name);
    LOGI("player name: %s", a.net.name());
}

// a LAN / loopback address ("192.168.1.20:47780", "10.x", "172.16-31.x", "127.x", "localhost")
static bool isPrivateHost(const std::string& hp) {
    const std::string h = hp.substr(0, hp.find(':'));
    int b0 = -1, b1 = -1;
    if (h == "localhost") return true;
    if (sscanf(h.c_str(), "%d.%d", &b0, &b1) != 2) return false;
    return b0 == 10 || b0 == 127 || (b0 == 192 && b1 == 168) || (b0 == 172 && b1 >= 16 && b1 <= 31);
}

// the private key the community server keeps this player's statistics under (made once, files/stats_key.bin)
static void loadStatsKey(App& a) {
    uint8_t k[16] = {};
    const char* dir = dataDir(a);
    const std::string f = dir ? std::string(dir) + "/stats_key.bin" : std::string();
    bool ok = false;
    if (dir)
        if (FILE* fp = fopen(f.c_str(), "rb")) { ok = fread(k, 1, 16, fp) == 16; fclose(fp); }
    if (!ok) {
        std::random_device rd;
        const uint64_t t = (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
        for (int i = 0; i < 16; i += 4) {
            const uint32_t r = rd() ^ (uint32_t)(t >> (i * 2)) * 2654435761u;
            std::memcpy(k + i, &r, 4);
        }
        if (dir)
            if (FILE* fp = fopen(f.c_str(), "wb")) { fwrite(k, 1, 16, fp); fclose(fp); }
        LOGI("stats: a new player key");
    }
    a.net.setStatsKey(k);
}

static void startUp(App& a) {
    logOpenFile(dataDir(a));
    a.net.init();
    loadPlayerName(a);
    loadAvatar(a);
    loadStatsKey(a);
    a.game.tun.advanced = true;   // the default rules (owner: Advanced is the most played); settings.cfg advanced= wins
    loadSettings(a);
    if (a.serverAddr.empty() && PA_DEFAULT_SERVER[0]) a.serverAddr = PA_DEFAULT_SERVER;   // the community server of this build
    // settings from before 0.3.1 can hold a local test server (a developer PC on the home network, e.g. 192.168.x.x):
    // it made two headsets look for each other on different servers. Those move to this build's server, once.
    if (a.cfgVersion < 2 && PA_DEFAULT_SERVER[0] && a.serverAddr != PA_DEFAULT_SERVER && isPrivateHost(a.serverAddr)) {
        LOGI("settings: server %s (a local test address from an older build) -> %s", a.serverAddr.c_str(), PA_DEFAULT_SERVER);
        a.serverAddr = PA_DEFAULT_SERVER;
        saveSettings(a);
    }
    // the gauntlet's default placement changed (0, then -25 deg): a file still holding an old default moves to the new
    // one once; a value someone tuned themselves is kept
    if (a.cfgVersion < 4 && (a.tunePitchDeg == 0.f || a.tunePitchDeg == -25.f) && a.tuneFwd == 0.f && a.tuneUp == 0.f) {
        LOGI("settings: gauntlet %.0f deg -> 35 deg, 9 cm forward, 2 cm up (new default)", a.tunePitchDeg);
        a.tunePitchDeg = 35.f; a.tuneFwd = 0.09f; a.tuneUp = 0.02f;
        saveSettings(a);
    }
    // a file still holding an earlier default (35 deg / 9 cm / 2 cm up, or 21 deg / 9 cm / level) moves to the owner's
    // TUNING export of 2026-10-18 once; values someone tuned themselves are kept
    const bool oldDef = (a.tunePitchDeg == 35.f && std::fabs(a.tuneFwd - 0.09f) < 1e-4f && std::fabs(a.tuneUp - 0.02f) < 1e-4f) ||
                        (a.tunePitchDeg == 21.f && std::fabs(a.tuneFwd - 0.09f) < 1e-4f && std::fabs(a.tuneUp) < 1e-4f);
    if (a.cfgVersion < 7 && oldDef) {
        LOGI("settings: gauntlet %.0f deg -> 26 deg, 5.4 cm forward, 3 cm down (new default)", a.tunePitchDeg);
        a.tunePitchDeg = 26.f; a.tuneFwd = 0.054f; a.tuneUp = -0.03f;
        saveSettings(a);
    }
    // and from that default (26 deg / 5.4 cm / 3 cm down) to the owner's TUNING export of 2026-10-19, once
    if (a.cfgVersion < 8 && a.tunePitchDeg == 26.f && std::fabs(a.tuneFwd - 0.054f) < 1e-4f && std::fabs(a.tuneUp + 0.03f) < 1e-4f) {
        LOGI("settings: gauntlet 26 deg -> 27 deg, 0.4 cm forward, 1.4 cm down (new default)");
        a.tunePitchDeg = 27.f; a.tuneFwd = 0.004f; a.tuneUp = -0.014f;
        saveSettings(a);
    }
    if (a.cfgVersion < 5 && !a.onlinePref) {   // older builds started on home Wi-Fi: online is the default now, once
        a.onlinePref = true;
        LOGI("settings: online on (new default)");
        saveSettings(a);
    }
    if (a.cfgVersion < 2 && a.sparcAddr != PA_SPARC_SERVER) {   // EXPERIMENTAL cross-play: test addresses from older builds
        LOGI("settings: cross-play server %s -> %s", a.sparcAddr.c_str(), PA_SPARC_SERVER[0] ? PA_SPARC_SERVER : "(none)");
        a.sparcAddr = PA_SPARC_SERVER;
        saveSettings(a);
    }
    a.net.setBotFill(a.botFill);
    if (!a.serverOverride.empty()) { a.serverSaved = a.serverAddr; a.serverAddr = a.serverOverride; a.onlinePref = true; }
    // (pictures and probes stay off the community server: --selftest renders once showed up there as players)
    if (a.onlinePref && !a.serverAddr.empty() && !a.noOnline) a.net.goOnline(a.serverAddr.c_str());
    if (!a.serverAddr.empty()) { std::vector<std::string> ob = bugOutbox(a); if (!ob.empty()) bugSendAll(a, ob); }   // reports saved earlier
    loadWarps(a);
    if (const char* ext = dataDir(a)) a.firstHelp = !fileExists(std::string(ext) + "/help_seen");
    if (const char* ext = dataDir(a)) a.devMode = fileExists(std::string(ext) + "/dev_mode");
    LOGI("developer mode %s", a.devMode ? "on" : "off");
    loadTuning(a);   // developer mode only
    LOGI("initialised; entering main loop");
}

static void shutDown(App& a) {
    a.net.shutdown();
    a.audio.shutdown();
    if (a.session) xrDestroySession(a.session);
    if (a.instance) xrDestroyInstance(a.instance);
}

#ifdef __ANDROID__
void android_main(android_app* app) {
    App a;
    a.android = app;
    if (app->activity->externalDataPath) a.dataPath = app->activity->externalDataPath;
    platformSetAssets(app->activity->assetManager, nullptr);
    app->userData = &a;
    app->onAppCmd = onAppCmd;
    JNIEnv* env = nullptr;
    app->activity->vm->AttachCurrentThread(&env, nullptr);
    g_vm = app->activity->vm;
    g_activity = app->activity->clazz;

    if (!initGraphics(a) || !initInstance(a) || !initActions(a) || !initSession(a) || !initContent(a)) {
        LOGE("initialisation failed");
        ANativeActivity_finish(app->activity);
    } else {
        startUp(a);
    }
    while (!app->destroyRequested) {
        int events;
        android_poll_source* src;
        const int timeout = (!a.resumed && !a.running && !a.quit) ? -1 : 0;
        while (ALooper_pollOnce(timeout, nullptr, &events, (void**)&src) >= 0) {
            if (src) src->process(app, src);
            if (app->destroyRequested) break;
            if (a.resumed || a.running) break;
        }
        if (a.quit) { ANativeActivity_finish(app->activity); a.quit = false; }
        if (!a.instance) continue;
        pollXrEvents(a);
        if (!a.running) { usleep(10000); continue; }
        if (!renderFrame(a)) LOGE("frame failed");
    }
    shutDown(a);
    app->activity->vm->DetachCurrentThread();
}
#else
// --selftest out.ppm: no headset needed. Loads everything, draws the court from player 1's spot into a 1280x720
// image and exits (0 = every shader, model and texture loaded). Checks the PC renderer on any machine.
static Pose eventCamera(App& a, const Pose& base, double t, float dt, bool freeCam);   // (the desktop spectator, below)
static Pose desktopPreset(const App& a, int v);
static void desktopHud(App& a, const Pose& eye, float ty, float tx);
static void desktopRender(App& a, const Pose& eye, int W, int H, float dt);
static bool dcWinSplit(const App& a);

struct ShotView {
    int w = 1280, h = 720; bool custom = false; Vec3 p; float yaw = 0, pitch = 0, fovY = 64;
    std::string title; float titleH = 0.12f, titleY = 0;   // title height (fraction of the image) and offset (-1..1)
    std::string menu;   // --menu title|xp: draw that menu page in front of the camera (layout checks)
    std::string scene;  // --scene action: two players mid-rally (store art)
    bool lobby = false; // --lobby: the lobby balcony and posters (other pictures show the plain court)
    int tunePart = 0;   // --scene tuning --tune N: the developer tuning tool on that part
    int tuneRow = 1;    // --tunerow R: its value picked
    int suit = -1;      // --suit N: the players' outfit in the pictures (avatar checks)
    int hair = -1;      // --hair N: their hair
    int face = -1;      // --face B F: blue's body type (0 / 1) and face (0..3)
    std::string look;   // --look "visor=1,mask=3,headgear=2": avatar.cfg keys for blue (accessory checks)
    std::string dcshot;       // --dcshot meet|bump|win|play: the desktop spectator's broadcast moment (its camera and overlay)
    int layer = 0, keyCol = 3;  // --layer 0-3: its VIEW LAYERS; --keycol 0-2 (pictures only): an old colour key
    std::string nations[2];     // --nations USA FRA: the two players' countries (NATIONS; "-" = none)
    int crowdFans = -1, crowdHype[2] = {0, 0}, crowdCheer = -1;   // --crowd FANS HYPE0 HYPE1 [END]: The Crowd
    std::string recordDir;   // --record DIR SECONDS: a bot-vs-bot match recorded frame by frame (a trailer; DIR/f%05d.ppm
    float recordSeconds = 0; // and DIR/events.csv for the sound), with scripted cameras; --size sets the frame size
    std::string opts;   // --opts hotdog,free,adv,round2,time5,kg0,kg1: MATCH OPTIONS in the pictures
    float mockDark = -1.f;   // --mockdark K: pictures only, a darker arena (GitHub #9): floor, dais and structure x K
    int winStyle = 0;           // --winscreen split|winner|camera (or 0-2): its WIN SCREEN
    bool swingTest = false;   // --swingtest: the head snaps round just before the picture (the hair mid-swing)
    float armsUp = -1;        // --armsup DEG: the tuning scene's hands raised this far (0 forward, 90 overhead)
    float swingDeg = 60.f, swingHold = 0.1f;   // --swingdeg D, --swinghold S: how far it turned, how long ago (action scene)
    bool passthrough = false;   // --passthrough MODE (1 ARENA, 2 WINDOW, 3 OUTLINE): drawn over a stand-in room (a checkerboard)
    int ptMode = 1;
    float ptRoom = 1.f;         // --ptroom: ROOM BRIGHTNESS in the picture
};

static Pose lookFrom(const Vec3& pos, const Vec3& target);
static void stepFrame(App& a, PlayerInput in[2], int L, double t, float dt);

// --record: the trailer's cameras by time (s): a wide establishing move, over blue's shoulder, low at the orange end,
// alongside the ball, and a slow orbit high over the court for the finish
static Pose trailerCamera(const App& a, float tt) {
    // (inside the hall: the side stands start 8.4 m out and the end walls are 13.6 m out, so the camera stays within
    // |z| < 7 and |x| < 12.5)
    const Game& g = a.game;
    const Vec3 h0 = a.viewIn[0].head.p, h1 = a.viewIn[1].head.p;
    if (tt < 5.f) {
        const float s = tt / 5.f, e = s * s * (3 - 2 * s);
        return lookFrom(Vec3{-11.5f, 3.8f, 6.6f} * (1 - e) + Vec3{-3.5f, 2.6f, 6.4f} * e, {0.5f, 1.1f, 0});
    }
    if (tt < 10.f) return lookFrom(h0 + Vec3{-1.3f, 0.30f, 0.50f}, h1 + Vec3{0, -0.2f, 0});
    if (tt < 14.5f) return lookFrom(h1 + Vec3{1.4f, 0.25f, -0.55f}, h0 + Vec3{0, -0.2f, 0});
    if (tt < 19.5f) {
        const Vec3 b = g.pl[0].ball.state == BallState::Flying ? g.pl[0].ball.pos : g.pl[1].ball.pos;
        const float x = clampf(b.x * 0.55f, -5.f, 5.f);
        return lookFrom({x, 2.2f, 6.2f}, {b.x * 0.8f, 1.2f, 0});
    }
    if (tt < 21.9f) {   // up at the stands: flags and the cheer bursts, the court below
        const float s = (tt - 19.5f) / 2.4f;
        return lookFrom({-3.f + 4.f * s, 1.7f, -6.6f}, {-1.f + 4.f * s, 4.6f, 9.f});
    }
    // the winner up close, the confetti over them (orange is player 1, standing at +X facing -X)
    const int w = g.matchWinner >= 0 ? g.matchWinner : 1;
    const Vec3 hw = a.viewIn[w].head.p;
    const float side = w == 0 ? 1.f : -1.f, s = std::min(1.f, (tt - 21.9f) / 6.f);
    return lookFrom(hw + Vec3{side * (2.6f - 0.6f * s), 0.45f - 0.2f * s, 0.9f - 1.4f * s}, hw + Vec3{0, 0.1f, 0});
}

static int selfTest(App& a, const char* out, const ShotView& v) {
    if (!initGraphics(a) || !initContent(a)) return 2;
    a.noOnline = true;
    startUp(a);
    a.lobby = v.lobby;
    const int W = v.w, H = v.h;
    GLuint fbo = 0, col = 0, dep = 0;
    glGenTextures(1, &col);
    glBindTexture(GL_TEXTURE_2D, col);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, W, H);
    glGenRenderbuffers(1, &dep);
    glBindRenderbuffer(GL_RENDERBUFFER, dep);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, W, H);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, col, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, dep);
    glViewport(0, 0, W, H);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glClearColor(0.01f, 0.012f, 0.02f, 1);
    if (v.passthrough) { a.mrView = true; a.ptMode = std::max(1, std::min(PT_COUNT - 1, v.ptMode)); a.ptFloor = kPtModeFloor[a.ptMode]; glClearColor(0, 0, 0, 0); }
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    Pose eye{Quat::axisAngle({0, 1, 0}, -1.5707963f), a.game.tun.start[0] + Vec3{-0.6f, 1.7f, 0}};   // facing the opponent (+X)
    if (v.custom) eye = {Quat::axisAngle({0, 1, 0}, deg2rad(v.yaw)) * Quat::axisAngle({1, 0, 0}, deg2rad(v.pitch)), v.p};
    const float ty = std::tan(deg2rad(v.fovY) * 0.5f), tx = ty * W / H;
    a.gfx.beginEye(Mat4::viewFromPose(eye), Mat4::projection(-std::atan(tx), std::atan(tx), std::atan(ty), -std::atan(ty), 0.05f, 2000.f), eye.p);
    if (v.scene.rfind("stance", 0) == 0 && v.scene.size() == 7) {   // stance0..5: the practice bot standing in that stance
        Game& g = a.game;
        g.reset(true);
        g.match = MatchState::Countdown;
        g.stateTimer = 100.f;
        g.pl[1].botStance = v.scene[6] - '0';
        a.viewing = true;
        PlayerInput in[2];
        in[0].headTracked = true;
        in[0].head = {Quat::axisAngle({0, 1, 0}, -kPi * 0.5f), g.tun.start[0] + Vec3{0, 1.65f, 0}};
        for (int i = 0; i < 72 * 3; ++i) { PlayerInput tmp[2] = {in[0], in[1]}; g.step(1.f / 72, tmp); }
        for (int p = 0; p < 2; ++p) { a.viewIn[p] = g.pl[p].in; a.viewInValid[p] = true; }
    }
    if (v.scene == "meet" || v.scene == "meetoffer") {   // the fist bump start: both players face to face in the centre, right fists out
        Game& g = a.game;
        g.match = MatchState::Meet;
        g.stateTimer = 7.f;
        a.viewing = true;
        AvatarDesc looks[2] = {avatar::preset(10), avatar::preset(12)};
        for (int p = 0; p < 2; ++p)   // --nations: each in their country's kit (the presets' own suits otherwise)
            for (int n = 1; n <= avatar::nationCount(); ++n)
                if (v.nations[p] == avatar::nation(n)->code) avatar::setNation(looks[p], n);
        for (int p = 0; p < 2; ++p) {
            const float side = p ? 1.f : -1.f;
            PlayerInput in;
            in.headTracked = true;
            const Quat q = Quat::axisAngle({0, 1, 0}, p ? kPi * 0.5f : -kPi * 0.5f);
            in.head = {q, Vec3{g.tun.meetSpot * side, 1.64f, 0.f}};
            in.avatar = looks[p];
            for (int h = 0; h < 2; ++h) in.hand[h].tracked = true;
            in.hand[1].grip = {q, in.head.p + q.rotate({0.10f, -0.42f, -0.36f})};
            // the other arm hangs by the side, the hand pointing down (at the hip, pointing forward, it folded into the body)
            in.hand[0].grip = {q * Quat::axisAngle({1, 0, 0}, deg2rad(-88.f)), in.head.p + q.rotate({-0.20f, -0.85f, -0.02f})};
            a.viewIn[p] = in;
            a.viewInValid[p] = true;
            g.pl[p].in = in;
            g.pl[p].isBot = false;
        }
        if (v.scene == "meetoffer") {   // orange offers a fist in its ring, blue's closed fist on its way to it
            for (int p = 0; p < 2; ++p) {
                HandInput& hi = g.pl[p].in.hand[1];
                const Vec3 front = g.meetRing(p) + Vec3{p ? -0.03f : -0.10f, 0.f, 0.f};
                hi.grip.p = front - hi.grip.q.rotate(g.tun.fistFront);
                hi.trigger = 1;
                a.viewIn[p] = g.pl[p].in;
            }
        }
        // every kind of event through the real handlers (logs, sounds, haptics): a missing name once crashed both headsets
        for (int t = 0; t <= (int)EventType::FistBump; ++t) {
            GameEvent e{};
            e.type = (EventType)t; e.player = 0; e.hand = -1;
            g.events.push_back(e);
        }
        handleEvents(a);
        g.events.clear();
        a.bumpFlash = a.foulFlash = a.hitFlash[0] = a.hitFlash[1] = 0;   // (the picture shows the meeting, not those events' flashes)
        LOGI("selftest: all %d event kinds handled", (int)EventType::FistBump + 1);
    }
    if (v.scene == "tuning") {   // the developer tuning tool: you in front of its mirror, hands up, the panel beside
        a.devMode = true;
        PlayerInput in;
        in.headTracked = true;
        in.head = {Quat::axisAngle({0, 1, 0}, -1.5707963f), a.game.tun.start[0] + Vec3{-0.6f, 1.7f, 0}};   // (--view moves only the camera)
        in.avatar = avatar::preset(10);
        if (v.suit >= 0) { in.avatar.suit = (uint8_t)v.suit; in.avatar.pattern = 0; }
        if (v.hair >= 0) { in.avatar.hair = (uint8_t)v.hair; in.avatar.headgear = 0; }
        a.myAvatar = in.avatar;
        const Quat q = Quat::axisAngle({0, 1, 0}, -1.5707963f);
        for (int h = 0; h < 2; ++h) {
            const float s = h ? 1.f : -1.f;
            in.hand[h].tracked = true;
            in.hand[h].grip = {q * Quat::axisAngle({1, 0, 0}, -0.35f) * Quat::axisAngle({0, 0, 1}, s * 0.30f), in.head.p + q.rotate({s * 0.20f, -0.38f, -0.40f})};
            if (v.armsUp >= 0) {   // straight arms swung up from forward (0) to overhead (90), controllers along them
                const float a = deg2rad(v.armsUp);
                const Vec3 sh{s * 0.18f, -0.29f, 0.04f};
                in.hand[h].grip = {q * Quat::axisAngle({1, 0, 0}, a), in.head.p + q.rotate(sh + Vec3{s * 0.04f, std::sin(a) * 0.62f, -std::cos(a) * 0.62f})};
            }
        }
        a.localIn = in;
        a.viewIn[0] = in;
        a.viewInValid[0] = true;
        a.game.pl[0].in = in;
        a.viewHead = in.head;
        if (v.swingTest) preSwing(a, a.net.localIndex(), in, 50.f, 0.25f, 0.12f);   // the head turned 50 degrees in 0.25 s, 0.12 s ago
        openTuning(a, in);
        a.tunePart = v.tunePart % kTunePartCount;
        a.tuneRow = v.tuneRow;
        a.tuneHover = 2 * 1 + 1;
    }
    if (v.scene == "action" || v.scene == "foul" || v.scene == "exp" || v.scene == "expcorners" || v.scene == "throw" ||
        v.scene == "block" || v.scene == "strike") {   // store art: blue throws, orange raises a shield, the ball mid-court
        Game& g = a.game;
        g.match = MatchState::InProgress;
        g.pl[0].score = 3;
        g.pl[1].score = 2;
        g.roundClock = 84.f;
        a.viewing = true;
        AvatarDesc looks[2] = {avatar::preset(10), avatar::preset(12)};   // CIRCUIT, TANK AND SHORTS
        if (v.suit >= 0) {   // (a light main colour and a red trim, so the details read in the picture)
            looks[0].suit = looks[1].suit = (uint8_t)v.suit; looks[0].pattern = looks[1].pattern = 0;
            looks[0].primary = 1; looks[0].secondary = 10;
        }
        if (v.hair >= 0) { looks[0].hair = looks[1].hair = (uint8_t)v.hair; looks[0].headgear = looks[1].headgear = 0; }
        if (v.face >= 0) { looks[0].body = (uint8_t)v.face; looks[0].headgear = 0; }
        if (!v.look.empty()) { std::string t = v.look; for (char& c : t) if (c == ',') c = '\n'; avatar::fromText(t, looks[0]); }
        for (int p = 0; p < 2; ++p) {
            const float side = p ? -1.f : 1.f;   // +X for blue, -X for orange
            PlayerInput in;
            in.headTracked = true;
            const Quat q = Quat::axisAngle({0, 1, 0}, p ? kPi * 0.5f : -kPi * 0.5f) * Quat::axisAngle({1, 0, 0}, -0.08f);
            in.head = {q, Vec3{-6.85f * side, 1.64f, p ? -0.15f : 0.12f}};
            in.avatar = looks[p];
            for (int h = 0; h < 2; ++h) in.hand[h].tracked = true;
            if (p == 0) {   // follow-through of a throw: right arm reaching down the court, left arm back
                in.hand[1].grip = {q * Quat::axisAngle({1, 0, 0}, 0.25f), in.head.p + q.rotate({0.24f, -0.18f, -0.55f})};
                // (the left arm swung back and clear of the body: at the hip, the hand sank into the shorts)
                in.hand[0].grip = {q * Quat::axisAngle({1, 0, 0}, -1.3f), in.head.p + q.rotate({-0.34f, -0.66f, 0.20f})};
            } else {        // shield up on the right arm, left fist guarding: hands tipped up and outside the chest, so the
                            // elbows stay down by the ribs (across the chest the upper arm sank into the body)
                in.hand[1].grip = {q * Quat::axisAngle({1, 0, 0}, 0.60f), in.head.p + q.rotate({0.24f, -0.30f, -0.38f})};
                in.hand[0].grip = {q * Quat::axisAngle({1, 0, 0}, 0.60f), in.head.p + q.rotate({-0.22f, -0.36f, -0.30f})};
            }
            a.viewIn[p] = in;
            a.viewInValid[p] = true;
            g.pl[p].in = in;
            g.pl[p].isBot = false;
        }
        if (v.swingTest) preSwing(a, 0, g.pl[0].in, v.swingDeg, 0.2f, v.swingHold);   // blue's head turned in 0.2 s, a moment ago
        Ball& b0 = g.pl[0].ball;
        b0.state = BallState::Flying;
        b0.pos = {-3.6f, 1.50f, 0.20f};
        b0.vel = {13.f, 0.2f, -0.4f};
        b0.charged = true;
        b0.scale = 2.2f;   // grown by strikes
        Ball& b1 = g.pl[1].ball;
        b1.state = BallState::Held;
        b1.heldHand = 1;
        b1.charged = true;
        b1.scale = 1.f;
        b1.pos = g.launcherPoint(1, 1);
        g.pl[1].hands[1].shieldUp = true;
        if (v.scene == "exp") { g.tun.experimental = true; g.tun.expRoom = 0; g.useArena(ARENA_DIAMOND, a.classicCol); }   // the EXPERIMENTAL rooms
        if (v.scene == "expcorners") { g.tun.experimental = true; g.tun.expRoom = 1; g.useArena(ARENA_CORNERS, a.classicCol); }
        if (v.scene == "foul") {   // blue's own throw landed in blue's own foul zone: FOUL there, the ball faded on its way home
            b0.state = BallState::Returning; b0.scored = true; b0.homing = true; b0.scale = 1.f;
            b0.pos = {-5.0f, 0.45f, -1.15f}; b0.vel = {-6.f, 2.f, 1.f};
            a.foulFlash = 0.9f; a.foulAt = {-5.0f, 0.17f, -1.35f};
        }
        // (manual and box pictures) the ball just leaving blue's hand; meeting orange's shield; through orange's strike ring
        if (v.scene == "throw") { b0.scale = 1.f; b0.pos = g.pl[0].in.hand[1].grip.p + Vec3{0.45f, 0.02f, -0.02f}; }
        if (v.scene == "block") { b0.scale = 1.3f; b0.pos = g.shieldPose(1, 1).p + Vec3{-0.30f, 0.f, 0.f}; }
        if (v.scene == "strike") { b0.scale = 1.f; b0.pos = g.strikeZoneCenter(1) + Vec3{-0.05f, 0.10f, 0.42f}; }   // (beside orange, in the ring)
    }
    if (!v.recordDir.empty() && v.recordSeconds > 0) {   // --record: a bot-vs-bot match, frame by frame
        Game& g = a.game;
        g.tun.advanced = false;
        g.tun.basicMatchTime = std::max(5.f, v.recordSeconds - 9.f);   // (the match ends a few seconds before the clip)
        g.tun.meetTime = 0;
        g.reset(true);
        g.pl[0].isBot = true;
        g.pl[0].botLevel = g.pl[1].botLevel = 1;   // AMATEUR bots for a trailer: fewer blocks, more points in a short clip
        AvatarDesc looks[2];
        for (int p = 0; p < 2; ++p) {
            AvatarDesc d = avatar::preset(p ? 12 : 10);
            for (int n = 1; n <= avatar::nationCount(); ++n)
                if (v.nations[p] == avatar::nation(n)->code) avatar::setNation(d, n);
            looks[p] = d;
            g.pl[p].in.avatar = d;
            g.pl[p].in.headTracked = true;
        }
        a.botAvatar = looks[1];   // (each frame the practice bot is given the bot's look: here, the chosen one)
        a.viewing = true;
        a.menuOpen = false;
        a.testCrowdOn = true;
        a.testCrowd = CrowdInfo{};
        a.testCrowd.fans = 48;
        a.testCrowd.waves[0] = 14; a.testCrowd.waves[1] = 11;
        FILE* ev = fopen((v.recordDir + "/events.csv").c_str(), "w");
        if (ev) fprintf(ev, "t,event,player,value\n");
        const float dt = 1.f / 30.f;
        double t = 1000.0;
        a.startTime = t;
        g_clockOverride = t;
        a.lastTime = 0;
        const int frames = (int)std::lround(v.recordSeconds * 30.f);
        std::vector<uint8_t> px((size_t)W * H * 4), row((size_t)W * 3);
        uint32_t rs = 4242u;
        auto rnd = [&rs]() { rs = rs * 1664525u + 1013904223u; return (rs >> 8) / 16777216.f; };
        float excite[2] = {0.3f, 0.3f};   // the fans' mood: up after a point or a block, settling back
        for (int f = 0; f < frames; ++f) {
            const float tt = f * dt;
            g_clockOverride = t;
            if (f % 15 == 0) {   // the phones' cheers, twice a second
                CrowdInfo& c = a.testCrowd;
                c.at = t;
                for (int p = 0; p < 2; ++p) {
                    c.cheers[p] = (int)(excite[p] * 30.f * (0.6f + 0.8f * rnd()));
                    c.hype[p] = std::min(100, (int)(excite[p] * 100.f));
                    excite[p] = std::max(0.25f, excite[p] * 0.82f);
                }
            }
            PlayerInput in[2] = {g.pl[0].in, g.pl[1].in};
            stepFrame(a, in, 0, t, dt);
            for (int p = 0; p < 2; ++p) { g.pl[p].in.avatar = looks[p]; a.viewIn[p].avatar = looks[p]; }
            for (const GameEvent& e : g.events) {
                if (ev) fprintf(ev, "%.4f,%s,%d,%.3f\n", tt, eventName(e.type), e.player, e.value);
                if (e.type == EventType::HitPlayer && e.value >= 0 && e.value < 2) excite[(int)e.value] = 1.f;
                if (e.type == EventType::ShieldBlock && e.player >= 0 && e.player < 2) excite[e.player] = std::max(excite[e.player], 0.7f);
            }
            const Pose eye = trailerCamera(a, tt);
            a.viewHead = eye;
            a.localIn.head = eye;
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glViewport(0, 0, W, H);
            glEnable(GL_FRAMEBUFFER_SRGB);
            glClearColor(0.01f, 0.012f, 0.02f, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            const float fy = std::tan(deg2rad(v.fovY) * 0.5f), fx = fy * W / H;
            a.gfx.beginEye(Mat4::viewFromPose(eye), Mat4::projection(-std::atan(fx), std::atan(fx), std::atan(fy), -std::atan(fy), 0.05f, 2000.f), eye.p);
            drawWorld(a, 0);
            drawWorld(a, 1);
            glFinish();
            glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            char fn[64];
            std::snprintf(fn, sizeof fn, "/f%05d.ppm", f);
            if (FILE* fo = fopen((v.recordDir + fn).c_str(), "wb")) {
                fprintf(fo, "P6\n%d %d\n255\n", W, H);
                for (int y = H - 1; y >= 0; --y) {
                    for (int x = 0; x < W; ++x) std::memcpy(&row[(size_t)x * 3], &px[((size_t)y * W + x) * 4], 3);
                    fwrite(row.data(), 1, row.size(), fo);
                }
                fclose(fo);
            }
            t += dt;
        }
        if (ev) fclose(ev);
        g_clockOverride = -1.0;
        LOGI("record: %d frames (%dx%d) -> %s, final score %d-%d", frames, W, H, v.recordDir.c_str(), g.pl[0].score, g.pl[1].score);
        return 0;
    }
    for (int p = 0; p < 2; ++p) {   // --nations: the players' countries
        for (int n = 1; n <= avatar::nationCount(); ++n)
            if (v.nations[p] == avatar::nation(n)->code) {
                avatar::setNation(a.viewIn[p].avatar, n);
                avatar::setNation(a.game.pl[p].in.avatar, n);
            }
    }
    if (v.mockDark >= 0.f) {   // --mockdark K (a mock-up for the owner; not a game setting): the grey parts darker.
        // Material order as tools/build_open_assets.py writes them (sorted names): environment 1 dais, 14 structure;
        // floor 2 runway, 3 seam, 4 tile; environment 4 screen, 13 stand edges
        auto dim = [&](Model& m, std::initializer_list<int> ids) {   // the colour, and the reflections (metal, gloss)
            for (int i : ids) if (i < (int)m.mats.size()) {
                for (int k = 0; k < 3; ++k) { m.mats[i].tint[k] *= v.mockDark; m.mats[i].emissive[k] *= v.mockDark; }
                m.mats[i].emissive[3] *= v.mockDark;
            }
        };
        dim(a.environment, {1, 4, 13, 14});   // dais, screen block, stand edges, structure
        dim(a.floor, {2, 3, 4});              // runway, seams, tiles
    }
    if (!v.opts.empty()) {   // --opts: MATCH OPTIONS
        Tunables& t = a.game.tun;
        auto has = [&](const char* k) { return v.opts.find(k) != std::string::npos; };
        if (has("adv")) t.advanced = true;
        if (has("basic")) t.advanced = false;
        if (has("hotdog")) t.shields = false;
        if (has("free")) t.basicMatchTime = 0;
        if (has("time5")) t.basicMatchTime = 300;
        if (has("round2")) { t.advancedRoundTime = 120; a.game.roundClock = 83.f; }
        if (has("kg0")) t.knuckleMode = 0;
        if (has("kg1")) t.knuckleMode = 1;
        if (has("sudden")) a.game.overtime = true;
    }
    if (v.crowdFans >= 0) {   // --crowd: fans on phones (the jumbotron's count and hype; END's lights flaring)
        a.testCrowdOn = true;
        a.testCrowd.fans = v.crowdFans;
        a.testCrowd.hype[0] = v.crowdHype[0];
        a.testCrowd.hype[1] = v.crowdHype[1];
        if (v.crowdCheer == 0 || v.crowdCheer == 1) {
            a.crowdFlare[v.crowdCheer] = 1.f;
            a.testCrowd.waves[v.crowdCheer] = v.crowdFans;
            burstCrowdFx(a, v.crowdCheer, 60);   // and a cheer burst caught mid-air
            stepCrowdFx(a, 0.8f);
            for (int i = 0; i < 6; ++i) stepCrowdFx(a, 0.05f);
        }
    }
    if (v.scene == "builder") {   // the avatar builder and its mirror in front of you (--nations: your country)
        PlayerInput in;
        in.headTracked = true;
        in.head = eye;
        for (int n = 1; n <= avatar::nationCount(); ++n)
            if (v.nations[0] == avatar::nation(n)->code) avatar::setNation(a.myAvatar, n);
        in.avatar = a.myAvatar;
        a.localIn = in;
        a.viewHead = eye;
        openBuilder(a, in);
    }
    if (v.menu == "careers") {   // the jumbotron's career lines (made-up, wide numbers); no menu
        a.careersOk = true;
        for (int p = 0; p < 2; ++p) {
            Net::CareerStats& c = a.careers[p];
            c.known = true; c.matches = 1240; c.wins = 815; c.throws = 9410; c.onTarget = 6262; c.hits = 2120;
            c.returnHits = 1314; c.blocks = 1880; c.faced = 3160; c.catches = 1031; c.topSpeed = 27.8f;
        }
    }
    if (!v.dcshot.empty()) {   // the desktop spectator watching this match: its side camera eased into the moment
        SpecMatch fake;
        fake.ip = 1;
        std::snprintf(fake.name, sizeof fake.name, "TEST MATCH");
        a.net.matches().push_back(fake);
        a.watchIp = 1;
        a.dcView = 1;
        Game& g = a.game;
        if (v.dcshot == "win") {
            g.match = MatchState::MatchSummary;
            g.matchWinner = 0;
            g.pl[0].score = 7; g.pl[1].score = 4;
            a.winAt = nowSeconds() - 1.2;
        }
        if (v.dcshot == "bump") a.bumpFlash = 0.9f;
        if (v.dcshot == "vs") {   // the VS screen, with made-up careers
            a.dcMatchupForce = true;
            a.careersOk = true;
            a.watchIp = a.statsHost = 1;
            const char* nm[2] = {"MOJO", "FRIEND"};
            const uint32_t m[2][8] = {{24, 15, 410, 262, 120, 88, 160, 31}, {9, 3, 150, 81, 40, 30, 72, 9}};
            for (int p = 0; p < 2; ++p) {
                Net::CareerStats& c = a.careers[p];
                c.known = true;
                std::snprintf(c.name, sizeof c.name, "%s", nm[p]);
                c.matches = m[p][0]; c.wins = m[p][1]; c.throws = m[p][2]; c.onTarget = m[p][3];
                c.hits = m[p][4]; c.blocks = m[p][5]; c.faced = m[p][6]; c.catches = m[p][7];
                c.topSpeed = p ? 21.4f : 24.8f; c.returnHits = p ? 5 : 14;
            }
        }
        a.viewLayer = v.layer & 3;
        a.dcKeyCol = v.keyCol % 4;
        a.dcWinStyle = v.winStyle % 3;
        if (v.custom && v.dcshot == "play") a.dcView = 0;   // (the given camera, no broadcast moment)
        else for (int f = 0; f <= 90; ++f) eye = eventCamera(a, desktopPreset(a, a.dcView), 1000.0 + f / 60.0, 1.f / 60.f, false);
        a.viewHead = eye;
        a.dcFov = v.fovY;
        desktopRender(a, eye, W, H, 1.f / 60.f);
        if (dcWinSplit(a)) desktopRender(a, eye, W, H, 1.f / 60.f);   // (a second frame: the close-ups have settled)
    } else {
        a.viewHead = eye;   // (what turns to the viewer, FOUL and the sprites, turns to the picture's camera)
        drawWorld(a, 0);
        drawWorld(a, 1);
    }
    if (!v.menu.empty() && v.menu != "careers") {
        a.menuOpen = true;
        a.titleOpen = v.menu == "title";
        a.xpOpen = v.menu == "xp";
        a.settingsOpen = v.menu == "settings" || v.menu == "voice" || v.menu == "graphics";
        a.voicePage = v.menu == "voice";
        a.gfxPage = v.menu == "graphics";
        if (a.gfxPage) a.roomFeel = 2;
        if (v.menu == "passthrough") { a.settingsOpen = true; a.ptPage = true; a.hasPassthrough = true; a.ptMode = PT_WINDOW; a.ptFloor = 1.f; }
        a.helpOpen = v.menu == "help";
        if (v.menu == "name") { a.nameEdit = true; a.editTarget = 0; a.nameBuf = "MOJO"; }
        if (v.menu == "titlename") { a.titleOpen = true; a.nameEdit = true; a.editTarget = 0; a.nameBuf = "MOJO"; }   // (CHANGE NAME pressed on the title page)
        if (v.menu == "spectate") a.net.spectate();
        if (v.menu == "statsnotice") a.statsNoticeOpen = true;
        if (v.menu == "stats") {   // MY STATS with a made-up career
            a.statsOpen = true; a.myCareerOk = true;
            Net::CareerStats& c = a.myCareer;
            c.known = true; c.matches = 24; c.wins = 15; c.throws = 410; c.onTarget = 262; c.hits = 120; c.blocks = 88;
            c.faced = 160; c.catches = 31; c.returnHits = 14; c.topSpeed = 24.8f;
        }
        if (v.menu == "bug") { a.bugOpen = true; a.bugCat = 1; a.bugNote = "BALL WENT THROUGH THE WALL"; }
        if (v.menu == "rules") { a.rulesOpen = true; a.rulesForHost = true; a.game.tun.advanced = true; }
        if (v.menu == "rulesprac") { a.rulesOpen = true; a.rulesForHost = false; a.game.tun.advanced = false; a.game.tun.experimental = true; }
        if (v.menu == "optionsexp") {   // MATCH OPTIONS for practice, EXPERIMENTAL: the room choice
            a.rulesOpen = true; a.rulesForHost = false; a.rulesStep = 1; optionsFrom(a, a.game.tun);
            a.optAdvanced = false; a.optExperimental = true; a.optExpRoom = 0;
        }
        if (v.menu == "options" || v.menu == "options2") {   // MATCH OPTIONS: Basic, hotdog, free play / Advanced, 2 min rounds
            a.rulesOpen = true; a.rulesForHost = true; a.rulesStep = 1; optionsFrom(a, a.game.tun);
            if (v.menu == "options") { a.optAdvanced = false; a.optShields = false; a.optMatchTime = 0; }
            else { a.optAdvanced = true; a.optRoundTime = 120; a.optKnuckle = 1; }
        }
        if (v.menu == "practice" || v.menu == "practice2") {   // MATCH OPTIONS for practice: UNBEATABLE, HOTDOG / MATCH MY LEVEL
            a.rulesOpen = true; a.rulesForHost = false; a.rulesStep = 1; optionsFrom(a, a.game.tun);
            a.optShields = v.menu == "practice2"; a.optBotLevel = kBotLevels - 1;
            if (v.menu == "practice2") { a.optBotAdapt = true; a.game.tun.botMatch = 3.4f; }
        }
        if (v.menu == "report") {   // a room of example people (pictures of the page only)
            const char* nm[5] = {"MOJO", "SPARKY", "QUEST7", "ZAP_ZAP", "WATCHER"};
            const uint8_t ro[5] = {Net::VOICE_PLAYER, Net::VOICE_WAITING, Net::VOICE_WAITING, Net::VOICE_WAITING, Net::VOICE_SPECTATOR};
            for (int i = 0; i < 5; ++i) { Net::Person p; p.id = 0x1000u + i; p.role = ro[i]; std::snprintf(p.name, sizeof p.name, "%s", nm[i]); a.reportFake.push_back(p); }
            a.reportOpen = true; a.reportWho = 0x1001u; std::snprintf(a.reportName, sizeof a.reportName, "SPARKY"); a.reportWhy = 0;
            a.reportNote = "SHOUTING INSULTS IN THE WAITING AREA"; a.mutedIds.push_back(0x1001u);
        }
        a.menuPose = {eye.q, eye.transform({0, 0.09f, -0.95f})};
        if (v.lobby) { PlayerInput pi; pi.head = eye; openMenu(a, pi); a.menuOpen = true; }
        a.viewHead = eye;
        a.localIn.head = eye;
        drawTitleShowcase(a, 0);
        drawTitleShowcase(a, 1);
        glClear(GL_DEPTH_BUFFER_BIT);
        drawMenu(a);
    }
    if (!v.title.empty()) {   // store art: the title in the game's own font, a glow behind white letters
        const float d = 2.f, halfH = d * ty;
        glDisable(GL_DEPTH_TEST);
        const float glow[4] = {0.15f, 0.75f, 1.f, 0.35f}, white[4] = {1, 1, 1, 1};
        for (int k = v.titleH < 0 ? 1 : 0; k < 2; ++k) {   // a negative height: the white letters only (the glow is added later)
            const float h = std::fabs(v.titleH) * 2 * halfH * (k ? 1.f : 1.06f);
            Pose tp{eye.q, eye.transform({0, v.titleY * halfH, -d + (k ? 0.f : 0.01f)})};
            a.gfx.drawText(v.title, tp, h, k ? white : glow);
        }
        glEnable(GL_DEPTH_TEST);
    }
    // the TRANSPARENT key: the window's final pass into a plain RGBA8 target, as OBS would capture it; the picture is that
    // over a checkerboard, and OUT.alpha.pgm is its alpha channel (white = solid)
    const bool alphaOut = !v.dcshot.empty() && a.viewLayer >= 3 && a.dcKeyCol == 3;
    GLuint aFbo = 0, aCol = 0;
    if (alphaOut) {
        glGenTextures(1, &aCol);
        glBindTexture(GL_TEXTURE_2D, aCol);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
        glGenFramebuffers(1, &aFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, aFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, aCol, 0);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glViewport(0, 0, W, H);
        a.gfx.transparentOut(col);
        glEnable(GL_FRAMEBUFFER_SRGB);
    }
    glFinish();
    std::vector<uint8_t> px((size_t)W * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    const GLenum err = glGetError();
    FILE* f = fopen(out, "wb");
    if (!f) return 3;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    FILE* fa = nullptr;
    if (alphaOut) {
        fa = fopen((std::string(out) + ".alpha.pgm").c_str(), "wb");
        if (fa) fprintf(fa, "P5\n%d %d\n255\n", W, H);
    }
    size_t solid = 0, clear = 0;
    for (int y = H - 1; y >= 0; --y)
        for (int x = 0; x < W; ++x) {
            uint8_t* c = &px[((size_t)y * W + x) * 4];
            if (alphaOut) {   // straight alpha over a grey checkerboard
                if (fa) fputc(c[3], fa);
                solid += c[3] >= 250;
                clear += c[3] == 0;
                const int bg = (((x / 32) + (y / 32)) & 1) ? 160 : 96;
                for (int k = 0; k < 3; ++k) c[k] = (uint8_t)((c[k] * c[3] + bg * (255 - c[3])) / 255);
            }
            if (v.passthrough) {   // premultiplied over the stand-in room, as the compositor does
                const int room = (((x / 48) + (y / 48)) & 1) ? 150 : 105;
                const int warm[3] = {int((room + 12) * v.ptRoom), int(room * v.ptRoom), int((room - 14) * v.ptRoom)};
                for (int k = 0; k < 3; ++k) c[k] = (uint8_t)std::min(255, c[k] + (255 - c[3]) * warm[k] / 255);
            }
            fwrite(c, 1, 3, f);
        }
    fclose(f);
    if (fa) fclose(fa);
    if (alphaOut) LOGI("selftest: alpha %.1f%% solid, %.1f%% clear", 100.0 * solid / ((double)W * H), 100.0 * clear / ((double)W * H));
    LOGI("selftest: wrote %s (GL error 0x%x)", out, err);
    shutDown(a);
    return err == GL_NO_ERROR ? 0 : 4;
}

// PC: ProjectArena.exe, the "assets" folder beside it; player files in %APPDATA%\ProjectArena
// ------------------------------------------------------------------ desktop spectator
static Pose lookFrom(const Vec3& pos, const Vec3& target) {
    Vec3 f = target - pos;
    f = length(f) > 1e-5f ? normalize(f) : Vec3{0, 0, -1};
    const float yaw = std::atan2(-f.x, -f.z), pitch = std::asin(clampf(f.y, -1.f, 1.f));
    return {Quat::axisAngle({0, 1, 0}, yaw) * Quat::axisAngle({1, 0, 0}, pitch), pos};
}

static const char* kDcViewName[6] = {"FREE CAMERA", "SIDE", "HIGH SIDE", "BLUE END", "ORANGE END", "TOP"};
static const char* kDcLayerName[4] = {"FULL", "NO WALLS", "MINIMAL", "KEY"};
// TRANSPARENT: no colour at all behind the players (OBS Game Capture with Allow Transparency; no colour can clash)
static const char* kDcKeyName[4] = {"GREEN", "MAGENTA", "BLUE", "TRANSPARENT"};
static const float kDcKeyRgb[4][3] = {{0, 1, 0}, {1, 0, 1}, {0, 0, 1}, {0, 0, 0}};
// the VS screen (MATCHUP): at the fist bump and the countdown (or any time with M), two portraits facing each other,
// VS between them, each player's name, flag and career stats under them
static bool dcVsScreen(const App& a) {
    const Game& g = a.game;
    if (!a.watchIp) return false;
    if (a.dcMatchupForce) return true;
    return a.dcMatchup && (g.match == MatchState::Meet || g.match == MatchState::Countdown);
}

// WIN SCREEN, per window (a stream with several spectator windows can show each): SPLIT the winner on the left and the
// loser on the right, each alone in a close-up; WINNER the camera flies to the winner, their name over it; YOUR CAMERA
// the camera stays where it is, the winner's name over it
static const char* kDcWinName[3] = {"SPLIT", "WINNER", "YOUR CAMERA"};
static bool dcWinSplit(const App& a) {
    return a.watchIp && a.dcWinStyle == 0 && a.game.match == MatchState::MatchSummary && a.game.matchWinner >= 0 && a.game.matchWinner < 2;
}

// Broadcast moments: at the fist bump (the preset cameras, never the free camera) a close side view of the two
// players' fists, pushing in; when the match is won with WIN SCREEN > WINNER (any camera) the winner up close, the
// camera drifting round them. It eases in and back to the chosen camera over half a second.
static Pose eventCamera(App& a, const Pose& base, double t, float dt, bool freeCam) {
    const Game& g = a.game;
    int kind = 0;
    if (a.watchIp && g.match == MatchState::Meet && !freeCam) kind = 1;
    else if (a.watchIp && a.dcWinStyle == 1 && g.match == MatchState::MatchSummary && g.matchWinner >= 0 && g.matchWinner < 2) kind = 2;
    if (kind != a.dcEventSkip) a.dcEventSkip = 0;   // (that moment is over)
    if (kind && kind == a.dcEventSkip) kind = 0;    // a camera picked by hand during it
    if (kind && (kind != a.dcEventKind || a.dcEventMix <= 0)) { a.dcEventKind = kind; a.dcEventT0 = t; }
    a.dcEventMix = clampf(a.dcEventMix + (kind ? dt : -dt) * 2.f, 0.f, 1.f);
    if (a.dcEventMix <= 0) { a.dcEventKind = 0; return base; }
    const float age = float(t - a.dcEventT0);
    Pose ev;
    if (a.dcEventKind == 1) {   // the fists meet between the two heads, a little lower
        Vec3 T = (a.viewIn[0].head.p + a.viewIn[1].head.p) * 0.5f;
        T.y -= 0.30f;
        const float push = 1.75f - 0.40f * std::min(1.f, age / 4.f);
        ev = lookFrom(T + Vec3{0.20f, 0.10f, push}, T);
    } else {
        const int w = std::max(0, std::min(1, g.matchWinner));
        const Vec3 H = a.viewIn[w].head.p;
        a.dcWinT = age < 0.05f ? H : a.dcWinT + (H - a.dcWinT) * std::min(1.f, dt * 3.f);
        const float side = w == 0 ? 1.f : -1.f;   // blue stands at -X and faces +X: the camera in front of them
        const float ang = 0.5f * std::sin(age * 0.35f);
        ev = lookFrom(a.dcWinT + Vec3{side * 1.9f * std::cos(ang), 0.05f, 1.9f * std::sin(ang)}, a.dcWinT - Vec3{0, 0.25f, 0});
    }
    const float k = a.dcEventMix * a.dcEventMix * (3 - 2 * a.dcEventMix);
    return blendPose(base, ev, k);
}

static Pose desktopPreset(const App& a, int v) {
    if (a.watchIp == 0) return lookFrom({-1.3f, 1.75f, 0.f}, {1.4f, 0.85f, 0.f});   // the hub of live miniatures
    switch (v) {
    case 2: return lookFrom({0, 5.2f, 6.6f}, {0, 0.6f, 0});   // above the near glass (further back is in the stands)
    case 3: return lookFrom({-12.0f, 2.8f, 0}, {0, 1.3f, 0});
    case 4: return lookFrom({12.0f, 2.8f, 0}, {0, 1.3f, 0});
    case 5: return lookFrom({0, 11.f, 5.5f}, {0, 0, 0});   // steep, off-centre (the scoreboard hangs over the middle)
    default: return lookFrom({0, 2.7f, 7.3f}, {0, 1.0f, 0});   // (the hall wall is just behind)
    }
}

static bool dcPressed(App& a, int vk) {   // key went down this frame (window focused)
    const bool down = GetForegroundWindow() == a.hwnd && (GetAsyncKeyState(vk) & 0x8000);
    const bool edge = down && !a.dcKey[vk & 255];
    a.dcKey[vk & 255] = down;
    return edge;
}
static bool dcDown(const App& a, int vk) { return GetForegroundWindow() == a.hwnd && (GetAsyncKeyState(vk) & 0x8000); }

// A PlayStation 4 controller (DualShock 4) read directly as a HID device, over USB or Bluetooth (Windows does not
// show it as an XInput controller unless Steam Input translates it). A background thread finds it (once a second
// while none is there) and keeps the latest report, turned into an XInput-shaped state.
static const uint16_t kDs4Pids[] = {0x05C4, 0x09CC, 0x0BA0};   // DualShock 4 v1, v2, the Sony wireless adapter
static std::mutex g_ds4Mu;
static XINPUT_GAMEPAD g_ds4{};
static std::atomic<bool> g_ds4Ok{false};

// one input report (USB and Bluetooth's basic report: id 0x01; Bluetooth's full report: id 0x11, 2 bytes later)
static bool ds4Parse(const uint8_t* r, DWORD n, XINPUT_GAMEPAD& g) {
    int o;
    if (n >= 10 && r[0] == 0x01) o = 1;
    else if (n >= 12 && r[0] == 0x11) o = 3;
    else return false;
    auto stick = [](uint8_t v, bool flip) {   // 0..255, 128 = centre; XInput's y is up
        const int c = (int)v - 128;
        return (SHORT)std::max(-32767, std::min(32767, (flip ? -c : c) * 258));
    };
    g = {};
    g.sThumbLX = stick(r[o], false);
    g.sThumbLY = stick(r[o + 1], true);
    g.sThumbRX = stick(r[o + 2], false);
    g.sThumbRY = stick(r[o + 3], true);
    const uint8_t b0 = r[o + 4], b1 = r[o + 5];
    static const WORD hat[8] = {XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT,
                                XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN,
                                XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT,
                                XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_UP};
    WORD w = (b0 & 0x0F) < 8 ? hat[b0 & 0x0F] : 0;
    if (b0 & 0x10) w |= XINPUT_GAMEPAD_X;   // square
    if (b0 & 0x20) w |= XINPUT_GAMEPAD_A;   // cross
    if (b0 & 0x40) w |= XINPUT_GAMEPAD_B;   // circle
    if (b0 & 0x80) w |= XINPUT_GAMEPAD_Y;   // triangle
    if (b1 & 0x01) w |= XINPUT_GAMEPAD_LEFT_SHOULDER;    // L1
    if (b1 & 0x02) w |= XINPUT_GAMEPAD_RIGHT_SHOULDER;   // R1
    if (b1 & 0x10) w |= XINPUT_GAMEPAD_BACK;             // SHARE
    if (b1 & 0x20) w |= XINPUT_GAMEPAD_START;            // OPTIONS
    if (b1 & 0x40) w |= XINPUT_GAMEPAD_LEFT_THUMB;       // L3
    if (b1 & 0x80) w |= XINPUT_GAMEPAD_RIGHT_THUMB;      // R3
    g.wButtons = w;
    g.bLeftTrigger = r[o + 7];    // L2
    g.bRightTrigger = r[o + 8];   // R2
    return true;
}

static HANDLE ds4Open() {
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    HANDLE found = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA di{sizeof di};
    for (DWORD i = 0; found == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &di); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &di, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<uint8_t> buf(need);
        auto* det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf.data();
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &di, det, need, nullptr, nullptr)) continue;
        HANDLE h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES at{sizeof at};
        bool ds4 = false;
        if (HidD_GetAttributes(h, &at) && at.VendorID == 0x054C)
            for (uint16_t pid : kDs4Pids) ds4 |= at.ProductID == pid;
        if (ds4) found = h;
        else CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

static void ds4Thread() {
    std::vector<uint8_t> rep(1024);
    for (;;) {
        HANDLE h = ds4Open();
        if (h == INVALID_HANDLE_VALUE) { Sleep(1000); continue; }
        LOGI("desktop: PlayStation 4 controller connected");
        DWORD n = 0;
        while (ReadFile(h, rep.data(), (DWORD)rep.size(), &n, nullptr)) {   // ~250 reports a second over USB
            XINPUT_GAMEPAD g;
            static int seen = 0;
            if (seen < 1 && n) { ++seen; LOGI("desktop: PS4 controller report 0x%02X, %u bytes", rep[0], (unsigned)n); }
            if (!ds4Parse(rep.data(), n, g)) continue;
            { std::lock_guard<std::mutex> lk(g_ds4Mu); g_ds4 = g; }
            g_ds4Ok = true;
        }
        g_ds4Ok = false;
        CloseHandle(h);
        LOGI("desktop: PlayStation 4 controller disconnected");
        Sleep(500);
    }
}

// A game controller: an Xbox controller (XInput; also a PlayStation controller that Steam Input translates) or a
// PlayStation 4 controller (above). The XInput DLL is loaded at run time, so the game starts on any PC; a missing
// controller is looked for once a second (polling an empty slot is slow). ok = false when none is connected.
struct DcPad { bool ok = false; WORD buttons = 0, pressed = 0; float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0; };
static DcPad dcPad(App& a) {
    static bool ds4Started = false;
    if (!ds4Started) { ds4Started = true; std::thread(ds4Thread).detach(); }
    typedef DWORD(WINAPI * GetStateFn)(DWORD, XINPUT_STATE*);
    static GetStateFn getState = nullptr;
    static bool tried = false;
    static int user = -1;
    static double nextScan = 0;
    if (!tried) {
        tried = true;
        for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"})
            if (HMODULE m = LoadLibraryW(dll))
                if ((getState = (GetStateFn)GetProcAddress(m, "XInputGetState")) != nullptr) break;
        if (!getState) LOGW("desktop: no XInput on this PC (game controllers off)");
    }
    DcPad p;
    XINPUT_STATE s{};
    if (!getState && !g_ds4Ok) { a.dcPadOk = false; return p; }
    if (getState && user >= 0 && getState((DWORD)user, &s) != ERROR_SUCCESS) {
        LOGI("desktop: game controller %d disconnected", user);
        user = -1;
        a.dcPadPrev = 0;
    }
    if (getState && user < 0 && nowSeconds() >= nextScan) {
        nextScan = nowSeconds() + 1.0;
        for (int u = 0; u < 4 && user < 0; ++u)
            if (getState((DWORD)u, &s) == ERROR_SUCCESS) {
                user = u;
                LOGI("desktop: game controller %d connected", u);
                a.dcPadPrev = s.Gamepad.wButtons;   // (a button held while plugging in is not a press)
            }
    }
    XINPUT_GAMEPAD g;
    if (user >= 0) g = s.Gamepad;
    else if (g_ds4Ok) { std::lock_guard<std::mutex> lk(g_ds4Mu); g = g_ds4; }
    else { a.dcPadOk = false; return p; }
    auto axis = [](SHORT v, int dz) {
        const float f = std::max(-1.f, v / 32767.f), d = dz / 32767.f;
        return std::fabs(f) < d ? 0.f : (f - std::copysign(d, f)) / (1.f - d);
    };
    p.ok = a.dcPadOk = true;
    p.buttons = g.wButtons;
    p.pressed = (WORD)(g.wButtons & ~a.dcPadPrev);
    a.dcPadPrev = g.wButtons;
    p.lx = axis(g.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
    p.ly = axis(g.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
    p.rx = axis(g.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
    p.ry = axis(g.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
    p.lt = g.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? g.bLeftTrigger / 255.f : 0.f;
    p.rt = g.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? g.bRightTrigger / 255.f : 0.f;
    return p;
}

static void desktopFullscreen(App& a, bool on) {
    if (on == a.dcFull) return;
    a.dcFull = on;
    if (on) {
        GetWindowRect(a.hwnd, &a.dcWinRect);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(a.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(a.hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(a.hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(a.hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        const RECT r = a.dcWinRect;
        SetWindowPos(a.hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_NOZORDER);
    }
}

// keyboard / mouse: which match, which camera; returns the eye pose
enum { DC_VIEW = 100, DC_AUTO = 110, DC_FREE, DC_NEXT, DC_HUB, DC_FULL, DC_HELP, DC_PANEL, DC_LAYER, DC_LAYERBACK, DC_KEYCOL, DC_WINSTYLE, DC_MATCHUP };

static LRESULT CALLBACK dcPanelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    App* a = (App*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_COMMAND && a && HIWORD(w) == BN_CLICKED) { a->dcCmd = LOWORD(w); SetForegroundWindow(a->hwnd); return 0; }
    if (m == WM_CLOSE) { ShowWindow(h, SW_HIDE); return 0; }   // F1 brings it back
    return DefWindowProcW(h, m, w, l);
}

// a small window beside the spectator with a button for everything: never part of the captured picture
static void desktopPanel(App& a) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = dcPanelProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));   // (app/src/pc/ProjectArena.rc)
    wc.lpszClassName = L"ProjectArenaSpectatorControls";
    RegisterClassW(&wc);
    RECT r;
    GetWindowRect(a.hwnd, &r);
    a.dcPanel = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Spectator Controls", WS_CAPTION | WS_SYSMENU,
                                r.right + 8, r.top, 300, 726, a.hwnd, nullptr, wc.hInstance, nullptr);
    if (!a.dcPanel) return;
    SetWindowLongPtrW(a.dcPanel, GWLP_USERDATA, (LONG_PTR)&a);
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    auto add = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
        HWND c = CreateWindowW(cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, a.dcPanel, (HMENU)(INT_PTR)id, wc.hInstance, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        return c;
    };
    int y = 10;
    add(L"STATIC", L"Cameras", SS_LEFT, 12, y, 260, 16, 0); y += 20;
    const wchar_t* views[5] = {L"1  Side", L"2  High side", L"3  Blue end", L"4  Orange end", L"5  Top"};
    for (int i = 0; i < 5; ++i) add(L"BUTTON", views[i], BS_PUSHBUTTON, 12 + (i % 2) * 134, y + (i / 2) * 30, 128, 26, DC_VIEW + 1 + i);
    add(L"BUTTON", L"A  Auto camera", BS_PUSHBUTTON, 146, y + 60, 128, 26, DC_AUTO);
    y += 96;
    add(L"BUTTON", L"F  Free camera", BS_PUSHBUTTON, 12, y, 262, 26, DC_FREE); y += 30;
    add(L"STATIC", L"Free camera: hold the right mouse button to look,\nW A S D move, Q / E down / up, Shift faster.\nMouse wheel zooms in any camera.",
        SS_LEFT, 12, y, 270, 48, 0); y += 56;
    add(L"STATIC", L"Matches", SS_LEFT, 12, y, 260, 16, 0); y += 20;
    add(L"BUTTON", L"Tab  Next live match", BS_PUSHBUTTON, 12, y, 128, 26, DC_NEXT);
    add(L"BUTTON", L"H  Spectator hub", BS_PUSHBUTTON, 146, y, 128, 26, DC_HUB); y += 36;
    add(L"STATIC", L"Window", SS_LEFT, 12, y, 260, 16, 0); y += 20;
    add(L"BUTTON", L"F11  Full screen", BS_PUSHBUTTON, 12, y, 128, 26, DC_FULL);
    add(L"BUTTON", L"F2  Help in picture", BS_PUSHBUTTON, 146, y, 128, 26, DC_HELP); y += 36;
    add(L"STATIC", L"View layers (streaming)", SS_LEFT, 12, y, 260, 16, 0); y += 20;
    add(L"BUTTON", L"V  Next layer", BS_PUSHBUTTON, 12, y, 128, 26, DC_LAYER); y += 30;
    add(L"STATIC", L"Full, no walls (the outline stays), minimal, or key: the\nplayers and balls alone, transparent behind them (OBS\nGame Capture with Allow Transparency).", SS_LEFT, 12, y, 276, 46, 0); y += 50;
    add(L"BUTTON", L"R  Win screen", BS_PUSHBUTTON, 12, y, 128, 26, DC_WINSTYLE);
    add(L"BUTTON", L"M  VS screen", BS_PUSHBUTTON, 146, y + 30, 128, 26, DC_MATCHUP);
    add(L"STATIC", L"split, the winner up\nclose, or your camera", SS_LEFT, 146, y - 2, 140, 30, 0); y += 30;
    add(L"STATIC", L"VS: portraits + stats", SS_LEFT, 12, y + 4, 130, 20, 0); y += 34;
    add(L"STATIC", L"Game controller (Xbox or PlayStation 4)", SS_LEFT, 12, y, 270, 16, 0); y += 18;
    add(L"STATIC", L"A / cross next match, B / circle hub, X / square auto,\nY / triangle free camera, LB / RB or D-pad left / right\ncameras, D-pad up / down view layers, triggers zoom,\nView help, Menu full screen, right stick click: win screen.\nFree camera: sticks, LB / RB down / up.",
        SS_LEFT, 12, y, 276, 76, 0); y += 82;
    a.dcStatus = add(L"STATIC", L"", SS_LEFT, 12, y, 270, 48, 0); y += 54;
    add(L"STATIC", L"F1 shows / hides this window. Streaming: capture only\nthe \"Project Arena - Spectator\" window; this one\nstays off the stream.",
        SS_LEFT, 12, y, 270, 48, 0);
    ShowWindow(a.dcPanel, SW_SHOWNOACTIVATE);
}

static Pose desktopControls(App& a, double t, float dt) {
    std::vector<SpecMatch>& ms = a.net.matches();
    // keys and the Spectator Controls window's buttons give the same commands
    int cmd = a.dcCmd;
    a.dcCmd = 0;
    if (dcPressed(a, VK_TAB)) cmd = DC_NEXT;
    if (dcPressed(a, 'H')) cmd = DC_HUB;
    for (int v = 1; v <= 5; ++v) if (dcPressed(a, '0' + v)) cmd = DC_VIEW + v;
    const bool freeKey = dcPressed(a, 'F'), zeroKey = dcPressed(a, '0');
    if (freeKey || zeroKey) cmd = DC_FREE;
    if (a.dcView != 0 && dcPressed(a, 'A')) cmd = DC_AUTO;   // (A moves left in the free camera)
    if (dcPressed(a, VK_F1)) cmd = DC_PANEL;
    if (dcPressed(a, VK_F2)) cmd = DC_HELP;
    if (dcPressed(a, VK_F11)) cmd = DC_FULL;
    if (dcPressed(a, VK_ESCAPE) && a.dcFull) cmd = DC_FULL;
    if (dcPressed(a, 'V')) cmd = DC_LAYER;
    if (dcPressed(a, 'R')) cmd = DC_WINSTYLE;
    if (dcPressed(a, 'M')) cmd = DC_MATCHUP;
    // a game controller: A next match, B hub, X auto camera, Y free camera, d-pad left / right the cameras,
    // triggers zoom, View the help bar, Menu full screen (the free camera: below)
    const DcPad pad = dcPad(a);
    if (pad.ok) {
        const WORD b = pad.pressed;
        if (b) LOGI("desktop: controller buttons 0x%04X", b);
        if (b & XINPUT_GAMEPAD_A) cmd = DC_NEXT;
        if (b & XINPUT_GAMEPAD_B) cmd = DC_HUB;
        if (b & XINPUT_GAMEPAD_X) cmd = DC_AUTO;
        if (b & XINPUT_GAMEPAD_Y) cmd = DC_FREE;
        // the cameras: D-pad left / right, or LB / RB (in the free camera LB / RB move it down / up instead)
        const WORD next = XINPUT_GAMEPAD_DPAD_RIGHT | (a.dcView != 0 ? XINPUT_GAMEPAD_RIGHT_SHOULDER : 0);
        const WORD prev = XINPUT_GAMEPAD_DPAD_LEFT | (a.dcView != 0 ? XINPUT_GAMEPAD_LEFT_SHOULDER : 0);
        if (b & (next | prev)) {
            const int v = a.dcView == 0 ? 1 : a.dcView;
            cmd = DC_VIEW + (v - 1 + ((b & next) ? 1 : 4)) % 5 + 1;
        }
        if (b & XINPUT_GAMEPAD_BACK) cmd = DC_HELP;
        if (b & XINPUT_GAMEPAD_DPAD_UP) cmd = DC_LAYER;
        if (b & XINPUT_GAMEPAD_DPAD_DOWN) cmd = DC_LAYERBACK;
        if (b & XINPUT_GAMEPAD_START) cmd = DC_FULL;
        if (b & XINPUT_GAMEPAD_RIGHT_THUMB) cmd = DC_WINSTYLE;
        a.dcFov -= (pad.rt - pad.lt) * 45.f * dt;
    }
    if (cmd == DC_NEXT && !ms.empty()) {   // next live match
        size_t k = 0;
        for (size_t i = 0; i < ms.size(); ++i) if (ms[i].ip == a.watchIp) k = i + 1;
        a.watchIp = ms[k % ms.size()].ip;
        a.dcHubPinned = false;
        LOGI("desktop: watching %s", ms[k % ms.size()].name);
    }
    if (cmd == DC_HUB) { a.watchIp = 0; a.dcHubPinned = true; LOGI("desktop: hub"); }
    if (cmd > DC_VIEW && cmd <= DC_VIEW + 5) {
        a.dcView = cmd - DC_VIEW; a.dcAuto = false;
        if (a.dcEventKind) a.dcEventSkip = a.dcEventKind;   // picked during the fist bump / winner close-up: show it now
        LOGI("desktop: camera %s", kDcViewName[a.dcView]);
    }
    if (cmd == DC_FREE) {   // free camera from where the camera is now
        const Pose cur = desktopPreset(a, a.dcView);
        if (a.dcView != 0) {
            const Vec3 f = cur.q.rotate({0, 0, -1});
            a.dcPos = cur.p; a.dcYaw = std::atan2(-f.x, -f.z); a.dcPitch = std::asin(clampf(f.y, -1.f, 1.f));
        }
        a.dcView = 0; a.dcAuto = false;
    }
    if (cmd == DC_AUTO) { a.dcAuto = !a.dcAuto; a.dcAutoAt = t + 8.0; if (a.dcAuto && a.dcView == 0) a.dcView = 1; }
    if (cmd == DC_HELP) a.dcHelp = !a.dcHelp;
    if (cmd == DC_LAYER || cmd == DC_LAYERBACK) {
        a.viewLayer = (a.viewLayer + (cmd == DC_LAYER ? 1 : 3)) % 4;
        LOGI("desktop: view layers %s", kDcLayerName[a.viewLayer]);
    }
    if (cmd == DC_MATCHUP) {   // the VS screen: shown now (and at every fist bump), or off
        a.dcMatchupForce = !a.dcMatchupForce;
        a.dcMatchup = a.dcMatchupForce ? 1 : 0;
        LOGI("desktop: VS screen %s", a.dcMatchupForce ? "on" : "off");
    }
    if (cmd == DC_WINSTYLE) {
        a.dcWinStyle = (a.dcWinStyle + 1) % 3;
        LOGI("desktop: win screen %s", kDcWinName[a.dcWinStyle]);
    }
    if (cmd == DC_FULL) desktopFullscreen(a, !a.dcFull);
    if (cmd == DC_PANEL && a.dcPanel) ShowWindow(a.dcPanel, IsWindowVisible(a.dcPanel) ? SW_HIDE : SW_SHOWNOACTIVATE);
    // nobody chosen: the first live match after a moment (a stream should show play, not the hub)
    if (a.watchIp == 0 && !a.dcHubPinned && !ms.empty()) {
        if (a.dcWatchAt == 0) a.dcWatchAt = t + 2.0;
        if (t > a.dcWatchAt) { a.watchIp = ms[0].ip; a.dcWatchAt = 0; LOGI("desktop: watching %s", ms[0].name); }
    }
    if (a.dcAuto && a.watchIp && t > a.dcAutoAt) {   // auto director: side, blue end, high, orange end
        static const int order[4] = {1, 3, 2, 4};
        int k = 0;
        for (int i = 0; i < 4; ++i) if (order[i] == a.dcView) k = (i + 1) % 4;
        a.dcView = order[k];
        a.dcAutoAt = t + 9.0;
    }
    a.dcFov = clampf(a.dcFov - a.dcWheel * 4.f, 25.f, 100.f);
    a.dcWheel = 0;
    POINT m;
    GetCursorPos(&m);
    if (a.dcView == 0) {   // free camera: right mouse button looks, WASD / Q E move, Shift faster
        if (dcDown(a, VK_RBUTTON)) {
            a.dcYaw -= (m.x - a.dcMouse.x) * 0.004f;
            a.dcPitch = clampf(a.dcPitch - (m.y - a.dcMouse.y) * 0.004f, -1.5f, 1.5f);
        }
        const Quat q = Quat::axisAngle({0, 1, 0}, a.dcYaw) * Quat::axisAngle({1, 0, 0}, a.dcPitch);
        const Vec3 f = q.rotate({0, 0, -1}), r = q.rotate({1, 0, 0});
        const float sp = (dcDown(a, VK_SHIFT) || (pad.buttons & XINPUT_GAMEPAD_LEFT_THUMB) ? 9.f : 3.f) * dt;
        if (pad.ok) {   // left stick moves, right stick looks, LB / RB down / up, click the left stick: faster
            a.dcYaw -= pad.rx * 2.2f * dt;
            a.dcPitch = clampf(a.dcPitch + pad.ry * 1.6f * dt, -1.5f, 1.5f);
            const Quat qp = Quat::axisAngle({0, 1, 0}, a.dcYaw) * Quat::axisAngle({1, 0, 0}, a.dcPitch);
            a.dcPos += qp.rotate({0, 0, -1}) * (pad.ly * sp) + qp.rotate({1, 0, 0}) * (pad.lx * sp);
            if (pad.buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) a.dcPos.y += sp;
            if (pad.buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) a.dcPos.y -= sp;
        }
        if (dcDown(a, 'W')) a.dcPos += f * sp;
        if (dcDown(a, 'S')) a.dcPos -= f * sp;
        if (dcDown(a, 'D')) a.dcPos += r * sp;
        if (dcDown(a, 'A')) a.dcPos -= r * sp;
        if (dcDown(a, 'E')) a.dcPos.y += sp;
        if (dcDown(a, 'Q')) a.dcPos.y -= sp;
        a.dcMouse = m;
        return eventCamera(a, {q, a.dcPos}, t, dt, true);
    }
    a.dcMouse = m;
    return eventCamera(a, desktopPreset(a, a.dcView), t, dt, false);
}

static void desktopHud(App& a, const Pose& eye, float ty, float tx) {
    const float white[4] = {1, 1, 1, 1}, grey[4] = {0.65f, 0.7f, 0.78f, 1}, cyan[4] = {0.3f, 0.85f, 1.0f, 1};
    float blue[4] = {0.35f, 0.75f, 1.0f, 1}, orange[4] = {1.0f, 0.6f, 0.25f, 1};
    const float bar[4] = {0.0f, 0.01f, 0.03f, 0.72f};
    if (a.nationsOn) for (int k = 0; k < 3; ++k) { blue[k] = 0.65f * a.sideRGB[0][k] + 0.35f; orange[k] = 0.65f * a.sideRGB[1][k] + 0.35f; }
    auto txt = [&](const std::string& s, float x, float y, float h, const float* c) {   // x, y in -1..1 of the view
        a.gfx.drawText(s, {eye.q, eye.transform({x * tx, y * ty, -1.f})}, h * ty, c);
    };
    auto flag = [&](int p, float x, float y, float h) {   // a side's flag, h tall (in -1..1 of the view)
        drawFlag(a, a.sideNation[p], {eye.q, eye.transform({x * tx, y * ty, -1.001f})}, h * ty * 1.5f);
    };
    auto panel = [&](float x, float y, float w, float h) {
        a.gfx.drawPanel(Mat4::fromPose({eye.q, eye.transform({x * tx, y * ty, -1.002f})}, {w * tx, h * ty, 1}), bar);
    };
    glDisable(GL_DEPTH_TEST);
    char buf[128];
    std::vector<SpecMatch>& ms = a.net.matches();
    const SpecMatch* w = nullptr;
    for (const SpecMatch& m : ms) if (m.ip == a.watchIp) w = &m;
    if (w) {
        const Game& g = a.game;
        auto who = [&](int p) -> std::string {
            const uint32_t id = a.watchRoom.slot[p];
            if (p == 1 && a.watchRoom.bot) return "BOT";
            if (const char* n = a.watchRoom.nameOf(id)) return n;
            return sideName(a, p);
        };
        panel(0, 0.88f, a.nationsOn ? 1.50f : 1.15f, 0.2f);
        flag(0, -0.66f, 0.88f, 0.10f);   // NATIONS: the flags at the banner's ends
        flag(1, 0.66f, 0.88f, 0.10f);
        txt(who(0), -0.30f, 0.90f, 0.06f, blue);
        txt(who(1), 0.30f, 0.90f, 0.06f, orange);
        snprintf(buf, sizeof buf, "%d  -  %d", g.pl[0].score, g.pl[1].score);
        txt(buf, 0, 0.90f, 0.08f, white);
        const std::string clk = clockLine(g), rl = rulesLine(g.tun);
        if (g.tun.advanced) snprintf(buf, sizeof buf, "%s  -  ROUND %d  (%d - %d)%s%s", rl.c_str(), std::max(1, g.round), g.pl[0].roundWins,
                                     g.pl[1].roundWins, clk.empty() ? "" : "  -  ", clk.c_str());
        else snprintf(buf, sizeof buf, "%s  -  %s", rl.c_str(), clk.c_str());
        txt(buf, 0, 0.815f, 0.032f, grey);
        snprintf(buf, sizeof buf, "%s%s", w->name[0] ? w->name : "LIVE MATCH", ms.size() > 1 ? "   (TAB: NEXT MATCH)" : "");
        txt(buf, -0.62f, -0.93f, 0.028f, grey);
        snprintf(buf, sizeof buf, "%s%s%s%s", kDcViewName[a.dcView % 6], a.dcAuto ? "  (AUTO)" : "", a.viewLayer ? "   " : "",
                 a.viewLayer ? kDcLayerName[a.viewLayer & 3] : "");
        if (!(a.viewLayer >= 3 && !a.dcHelp)) txt(buf, 0.70f, -0.93f, 0.028f, grey);   // (KEY: a clean picture unless F2)
        const float gold[4] = {1.0f, 0.85f, 0.3f, 1};
        if (dcVsScreen(a)) {
            // (the VS screen: no FIST BUMP text over the portraits, owner 2026-10-20)
        } else if (a.bumpFlash > 0) {   // the fist bump: FIST BUMP! pops in
            const float s = 1.f + 0.5f * std::max(0.f, (a.bumpFlash - 0.8f) / 0.2f);
            const float gc[4] = {gold[0], gold[1], gold[2], std::min(1.f, a.bumpFlash * 2.f)};
            txt("FIST BUMP!", 0, 0.66f, 0.09f * s, gc);
        } else if (g.match == MatchState::Meet) {
            txt("FIST BUMP TO START", 0, 0.66f, 0.05f, white);
        }
        if (a.nationsOn && !dcVsScreen(a) && (g.match == MatchState::Meet || g.match == MatchState::WaitingForPlayers || a.bumpFlash > 0)) {
            // the matchup card: UNITED STATES  VS  FRANCE, flag over flag
            panel(0, 0.42f, 1.30f, 0.34f);
            const Vec3 r = eye.q.rotate({1, 0, 0});
            const bool swap = dot(a.viewIn[0].head.p - a.viewIn[1].head.p, r) > 0;   // blue stands on the right here
            for (int p = 0; p < 2; ++p) {
                const float x = (p != swap) ? 0.36f : -0.36f;
                flag(p, x, 0.47f, 0.17f);
                txt(sideName(a, p), x, 0.33f, 0.035f, p ? orange : blue);
            }
            txt("VS", 0, 0.42f, 0.07f, gold);
        }
        if (g.match == MatchState::MatchSummary && g.matchWinner >= 0 && g.matchWinner < 2 && !dcWinSplit(a)) {   // the win splash
            const int wn = g.matchWinner;
            const float age = a.winAt > 0 ? float(nowSeconds() - a.winAt) : 0.f;
            const float pop = 1.f + 0.6f * std::pow(std::max(0.f, 1.f - age / 0.4f), 2.f);
            const float pulse = 0.85f + 0.15f * std::sin(age * 4.f);
            const float* tc = wn == 0 ? blue : orange;
            const float glow[4] = {tc[0], tc[1], tc[2], 0.35f * pulse};
            if (a.sideNation[wn]) {   // NATIONS: the winner's flag over the lower third, and the country under the score
                flag(wn, 0, -0.24f, 0.17f * std::min(1.f, age / 0.25f + 0.3f));
                txt(sideName(a, wn), 0, -0.785f, 0.035f, tc);
            }
            panel(0, -0.55f, 1.40f * std::min(1.f, age / 0.25f + 0.2f), 0.40f);   // the lower third (the winner stays in view)
            panel(0, -0.55f, 1.46f * std::min(1.f, age / 0.25f + 0.2f), 0.44f);
            std::string nm = who(wn);
            for (char& c : nm) c = (char)std::toupper((unsigned char)c);
            txt(nm, 0.004f, -0.454f, 0.15f * pop, glow);   // a soft glow behind the name
            txt(nm, 0, -0.45f, 0.14f * pop, tc);
            txt("WINS!", 0, -0.61f, 0.09f * pop * pulse, gold);
            if (g.tun.advanced) snprintf(buf, sizeof buf, "ROUNDS  %d  -  %d", g.pl[0].roundWins, g.pl[1].roundWins);
            else snprintf(buf, sizeof buf, "FINAL  %d  -  %d", g.pl[0].score, g.pl[1].score);
            txt(buf, 0, -0.71f, 0.045f, white);
        }
    } else {
        txt("PROJECT ARENA  -  SPECTATOR", 0, 0.88f, 0.07f, cyan);
        if (const char* v = updateAvailable(a)) {
            snprintf(buf, sizeof buf, "UPDATE AVAILABLE: %s  -  ON STEAM OR GITHUB", v);
            txt(buf, 0, 0.72f, 0.03f, orange);
        }
        if (!a.net.online()) txt("ON YOUR HOME NETWORK", 0, 0.79f, 0.035f, grey);
        else if (!a.net.onlineReady()) txt("CONNECTING TO THE COMMUNITY SERVER...", 0, 0.79f, 0.035f, grey);
        else if (ms.empty()) txt("NO LIVE MATCHES YET  -  THEY APPEAR HERE WHEN SOMEONE PLAYS", 0, 0.79f, 0.035f, grey);
        else {
            snprintf(buf, sizeof buf, "%d LIVE MATCH%s  -  TAB TO WATCH", (int)ms.size(), ms.size() == 1 ? "" : "ES");
            txt(buf, 0, 0.79f, 0.035f, white);
        }
    }
    if (!a.dcHelp && nowSeconds() < a.dcHintUntil) txt("F1: CONTROLS", 0.82f, 0.93f, 0.028f, grey);
    if (a.dcHelp) {
        panel(0, -0.80f, 1.96f, 0.13f);
        txt("1-5 CAMERAS   A AUTO   F FREE (RIGHT MOUSE + WASD / Q E)   WHEEL ZOOM   V LAYERS   TAB NEXT MATCH   H HUB   F11 FULL   F2 HIDE",
            0, a.dcPadOk ? -0.775f : -0.80f, 0.026f, white);
        if (a.dcPadOk)
            txt("CONTROLLER:  A NEXT MATCH   B HUB   X AUTO   Y FREE (STICKS, LB / RB)   D-PAD CAMERAS   TRIGGERS ZOOM   VIEW HIDE",
                0, -0.825f, 0.026f, white);
    }
    if (const char* note = a.net.noticeText()) txt(note, 0, 0.66f, 0.04f, orange);
    glEnable(GL_DEPTH_TEST);
}

// the picture: the world from `eye` (VIEW LAYERS > KEY: on the key colour, alpha 0 behind for OBS transparency), or the
// win screen: the winner and the loser side by side, each alone in a close-up on a dark backdrop in their colour
static void desktopRender(App& a, const Pose& eye, int W, int H, float dt) {
    const float* key = kDcKeyRgb[a.dcKeyCol % 4];
    glViewport(0, 0, W, H);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    if (a.viewLayer >= 3) glClearColor(key[0], key[1], key[2], 0);
    else glClearColor(0.01f, 0.012f, 0.02f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    const float ty = std::tan(deg2rad(a.dcFov) * 0.5f), tx = ty * W / H;
    if (dcVsScreen(a) && !dcWinSplit(a)) {
        const double t = nowSeconds();
        if (a.dcVsT == 0) a.dcVsT = t;
        const float age = float(t - a.dcVsT);
        const int pH = H * 58 / 100;   // the portraits: the top 58 %, the stats panels below
        for (int half = 0; half < 2; ++half) {
            const int p = half;   // blue on the left, facing right (the camera on their left), orange mirrored
            const int x0 = half * (W / 2), hw = half ? W - W / 2 : W / 2;
            glViewport(x0, H - pH, hw, pH);
            glEnable(GL_SCISSOR_TEST);
            glScissor(x0, H - pH, hw, pH);
            if (a.viewLayer >= 3) glClearColor(key[0], key[1], key[2], 0);
            else glClearColor(0.07f * a.sideRGB[p][0] + 0.005f, 0.07f * a.sideRGB[p][1] + 0.005f, 0.07f * a.sideRGB[p][2] + 0.005f, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
            const Pose hd = a.viewIn[p].head;
            Vec3 fwd = hd.q.rotate({0, 0, -1});
            fwd.y = 0;
            fwd = length(fwd) > 1e-3f ? normalize(fwd) : Vec3{p ? -1.f : 1.f, 0, 0};
            const Vec3 right = cross(fwd, Vec3{0, 1, 0});
            const float s = half ? 1.f : -1.f;   // (the camera to the player's left on the left: they look across)
            a.dcClose[p] = age < 0.05f ? hd.p : a.dcClose[p] + (hd.p - a.dcClose[p]) * std::min(1.f, dt * 4.f);
            const Vec3 aim = a.dcClose[p] - Vec3{0, 0.14f, 0};
            const float push = 0.95f - 0.12f * std::min(1.f, age / 3.f);
            const Pose e = lookFrom(aim + fwd * push + right * (s * 0.34f) + Vec3{0, 0.04f, 0}, aim);
            const float hty = std::tan(deg2rad(34.f) * 0.5f), htx = hty * hw / pH;
            a.gfx.beginEye(Mat4::viewFromPose(e), Mat4::projection(-std::atan(htx), std::atan(htx), std::atan(hty), -std::atan(hty), 0.05f, 200.f), e.p);
            a.viewHead = e;
            a.onlyPlayer = p;
            drawWorld(a, 0);
            drawWorld(a, 1);
            a.onlyPlayer = -1;
        }
        glViewport(0, 0, W, H);
        a.gfx.beginEye(Mat4::viewFromPose(eye), Mat4::projection(-std::atan(tx), std::atan(tx), std::atan(ty), -std::atan(ty), 0.05f, 2000.f), eye.p);
        a.viewHead = eye;
        glDisable(GL_DEPTH_TEST);
        auto txt = [&](const std::string& s, float x, float y, float h, const float* c) {
            a.gfx.drawText(s, {eye.q, eye.transform({x * tx, y * ty, -1.f})}, h * ty, c);
        };
        const float bar[4] = {0.0f, 0.01f, 0.03f, 0.80f}, gold[4] = {1.0f, 0.85f, 0.3f, 1}, goldG[4] = {1.0f, 0.55f, 0.1f, 0.45f};
        const float white[4] = {1, 1, 1, 1}, grey[4] = {0.70f, 0.74f, 0.80f, 1};
        const float line[4] = {0.9f, 0.9f, 1.0f, 0.5f};
        a.gfx.drawPanel(Mat4::fromPose({eye.q, eye.transform({0, 0.16f * ty, -1.001f})}, {0.004f * tx, 1.68f * ty, 1}), line);
        char lines[2][5][48];
        for (int p = 0; p < 2; ++p) {
            const float cx = p ? 0.5f : -0.5f;
            const float col[4] = {0.65f * a.sideRGB[p][0] + 0.35f, 0.65f * a.sideRGB[p][1] + 0.35f, 0.65f * a.sideRGB[p][2] + 0.35f, 1};
            a.gfx.drawPanel(Mat4::fromPose({eye.q, eye.transform({cx * tx, -0.58f * ty, -1.002f})}, {0.94f * tx, 0.80f * ty, 1}), bar);
            std::string nm = sideName(a, p);
            if (const char* n = a.watchRoom.nameOf(a.watchRoom.slot[p])) nm = n;
            else if (p == 1 && a.watchRoom.bot) nm = "BOT";
            for (char& ch : nm) ch = (char)std::toupper((unsigned char)ch);
            const float fit = std::min(1.f, 9.f / float(std::max<size_t>(1, nm.size())));
            txt(nm, cx, -0.27f, 0.10f * fit, col);
            if (a.sideNation[p]) {
                drawFlag(a, a.sideNation[p], {eye.q, eye.transform({(cx - 0.36f) * tx, -0.27f * ty, -1.001f})}, 0.09f * ty * 1.5f);
                txt(avatar::nation(a.sideNation[p])->name, cx, -0.355f, 0.032f, grey);
            }
            const int n = careerLines(a, p, lines[p]);
            for (int k = 0; k < n; ++k) txt(lines[p][k], cx, -0.46f - 0.085f * k, 0.042f, k ? grey : white);
        }
        const float pop = 1.f + 0.8f * std::pow(std::max(0.f, 1.f - age / 0.35f), 2.f);
        txt("VS", 0.006f, 0.16f, 0.22f * pop, goldG);
        txt("VS", 0, 0.165f, 0.20f * pop, gold);
        glEnable(GL_DEPTH_TEST);
        desktopHud(a, eye, ty, tx);
        return;
    }
    a.dcVsT = 0;
    if (!dcWinSplit(a)) {
        a.dcCloseT = 0;
        a.gfx.beginEye(Mat4::viewFromPose(eye), Mat4::projection(-std::atan(tx), std::atan(tx), std::atan(ty), -std::atan(ty), 0.05f, 2000.f), eye.p);
        drawWorld(a, 0);
        drawWorld(a, 1);
        desktopHud(a, eye, ty, tx);
        return;
    }
    const Game& g = a.game;
    const int wn = g.matchWinner;
    const double t = nowSeconds();
    const bool first = a.dcCloseT == 0;
    if (first) a.dcCloseT = t;
    const float age = float(t - a.dcCloseT);
    for (int half = 0; half < 2; ++half) {
        const int p = half == 0 ? wn : 1 - wn;
        const int x0 = half * (W / 2), hw = half ? W - W / 2 : W / 2;
        glViewport(x0, 0, hw, H);
        glEnable(GL_SCISSOR_TEST);
        glScissor(x0, 0, hw, H);
        float bg[2][3] = {{0.010f, 0.030f, 0.070f}, {0.070f, 0.028f, 0.010f}};   // a dark blue / orange backdrop
        if (a.nationsOn) for (int k = 0; k < 3; ++k) bg[p][k] = 0.07f * a.sideRGB[p][k] + 0.005f;   // (or the country's)
        if (a.viewLayer >= 3) glClearColor(key[0], key[1], key[2], 0);
        else glClearColor(bg[p][0], bg[p][1], bg[p][2], 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        // the close-up: in front of the player (blue stands at -X and faces +X), the head and shoulders, drifting
        const Vec3 head = a.viewIn[p].head.p;
        a.dcClose[p] = first ? head : a.dcClose[p] + (head - a.dcClose[p]) * std::min(1.f, dt * 3.f);
        const float side = p == 0 ? 1.f : -1.f, ang = 0.35f * std::sin(age * 0.4f + p * 2.f);
        const Vec3 at = a.dcClose[p] - Vec3{0, 0.32f, 0};
        const Pose e = lookFrom(at + Vec3{side * 1.45f * std::cos(ang), 0.18f, 1.45f * std::sin(ang)}, at);
        const float hty = std::tan(deg2rad(40.f) * 0.5f), htx = hty * hw / H;
        a.gfx.beginEye(Mat4::viewFromPose(e), Mat4::projection(-std::atan(htx), std::atan(htx), std::atan(hty), -std::atan(hty), 0.05f, 200.f), e.p);
        a.viewHead = e;
        a.onlyPlayer = p;
        drawWorld(a, 0);
        drawWorld(a, 1);
        a.onlyPlayer = -1;
        // the label: the winner's name and WINS!, the other's name and the score
        glDisable(GL_DEPTH_TEST);
        auto txt = [&](const std::string& s, float x, float y, float h, const float* c) {
            a.gfx.drawText(s, {e.q, e.transform({x * htx, y * hty, -1.f})}, h * hty, c);
        };
        float blue[4] = {0.35f, 0.75f, 1.0f, 1}, orange[4] = {1.0f, 0.6f, 0.25f, 1};
        const float gold[4] = {1.0f, 0.85f, 0.3f, 1};
        if (a.nationsOn) for (int k = 0; k < 3; ++k) { blue[k] = 0.65f * a.sideRGB[0][k] + 0.35f; orange[k] = 0.65f * a.sideRGB[1][k] + 0.35f; }
        const float white[4] = {1, 1, 1, 1}, grey[4] = {0.70f, 0.74f, 0.80f, 1}, bar[4] = {0.0f, 0.01f, 0.03f, 0.72f};
        a.gfx.drawPanel(Mat4::fromPose({e.q, e.transform({0, -0.72f * hty, -1.002f})}, {1.8f * htx, 0.42f * hty, 1}), bar);
        std::string nm = sideName(a, p);
        if (const char* n = a.watchRoom.nameOf(a.watchRoom.slot[p])) nm = n;
        else if (p == 1 && a.watchRoom.bot) nm = "BOT";
        drawFlag(a, a.sideNation[p], {e.q, e.transform({-0.70f * htx, -0.72f * hty, -1.001f})}, 0.26f * hty * 1.5f);   // NATIONS
        for (char& c : nm) c = (char)std::toupper((unsigned char)c);
        const float pop = p == wn ? 1.f + 0.6f * std::pow(std::max(0.f, 1.f - age / 0.4f), 2.f) : 1.f;
        const float fit = std::min(1.f, 7.f / float(std::max<size_t>(1, nm.size())));   // (a long name: smaller)
        txt(nm, a.sideNation[p] ? 0.12f : 0.f, -0.60f, (p == wn ? 0.15f : 0.13f) * pop * fit, p == 0 ? blue : orange);
        if (p == wn) txt("WINS!", 0, -0.84f, 0.10f * pop * (0.85f + 0.15f * std::sin(age * 4.f)), gold);
        else {
            char sc[48];
            if (g.tun.advanced) std::snprintf(sc, sizeof sc, "ROUNDS  %d - %d", g.pl[wn].roundWins, g.pl[p].roundWins);
            else std::snprintf(sc, sizeof sc, "FINAL  %d - %d", g.pl[wn].score, g.pl[p].score);
            txt(sc, 0, -0.82f, 0.075f, white);
        }
        (void)grey;
        glEnable(GL_DEPTH_TEST);
    }
    // the dividing line and the shared top bar (the scoreboard; desktopHud leaves its own splash out here)
    glViewport(0, 0, W, H);
    a.gfx.beginEye(Mat4::viewFromPose(eye), Mat4::projection(-std::atan(tx), std::atan(tx), std::atan(ty), -std::atan(ty), 0.05f, 2000.f), eye.p);
    a.viewHead = eye;
    glDisable(GL_DEPTH_TEST);
    const float line[4] = {0.9f, 0.9f, 1.0f, 0.5f};
    a.gfx.drawPanel(Mat4::fromPose({eye.q, eye.transform({0, 0, -1.001f})}, {0.004f * tx, ty, 1}), line);
    glEnable(GL_DEPTH_TEST);
    desktopHud(a, eye, ty, tx);
}

static void desktopFrame(App& a) {
    const double t = nowSeconds();
    static double statusAt = 0;
    if (a.dcStatus && t > statusAt) {   // what is on screen, in the controls window
        statusAt = t + 0.5;
        const SpecMatch* w = nullptr;
        for (const SpecMatch& m : a.net.matches()) if (m.ip == a.watchIp) w = &m;
        char b[200];
        snprintf(b, sizeof b, "Now: %s\nCamera: %s%s, layers: %s%s\nWin screen: %s", w ? (w->name[0] ? w->name : "live match") : "the spectator hub",
                 w ? kDcViewName[a.dcView % 6] : "the hub's", a.dcAuto ? " (auto)" : "", kDcLayerName[a.viewLayer & 3],
                 a.viewLayer == 3 ? " (transparent)" : "", kDcWinName[a.dcWinStyle % 3]);
        SetWindowTextA(a.dcStatus, b);
    }
    const float dt = a.lastTime > 0 ? std::min(0.1f, float(t - a.lastTime)) : 0.f;
    a.lastTime = t;
    if (a.net.mode() != Net::Mode::Spectate) { a.net.spectate(); a.watchIp = 0; }   // always watching
    const Pose eye = desktopControls(a, t, dt);
    const int L = a.net.localIndex();
    PlayerInput in[2];
    in[L].headTracked = true;
    in[L].head = eye;   // no hands: nothing to grab, no menus; the camera is the spectator
    a.menuOpen = false;
    stepFrame(a, in, L, t, dt);
    a.viewHead = eye;
    const int W = std::max(64, a.winW), H = std::max(64, a.winH);
    if (W != a.dW || H != a.dH) {   // the frame in an sRGB texture, blitted to the window like the headset mirror
        if (a.dFbo) { glDeleteFramebuffers(1, &a.dFbo); glDeleteTextures(1, &a.dCol); glDeleteRenderbuffers(1, &a.dDep); }
        glGenTextures(1, &a.dCol);
        glBindTexture(GL_TEXTURE_2D, a.dCol);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, W, H);
        glGenRenderbuffers(1, &a.dDep);
        glBindRenderbuffer(GL_RENDERBUFFER, a.dDep);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, W, H);
        glGenFramebuffers(1, &a.dFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, a.dFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, a.dCol, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, a.dDep);
        a.dW = W; a.dH = H;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, a.dFbo);
    desktopRender(a, eye, W, H, dt);
    glDisable(GL_FRAMEBUFFER_SRGB);
    if (a.viewLayer >= 3 && a.dcKeyCol == 3) {   // TRANSPARENT: straight alpha into the window (it has an alpha channel)
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, a.winW, a.winH);
        a.gfx.transparentOut(a.dCol);
    } else {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, a.dFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glViewport(0, 0, a.winW, a.winH);
        glBlitFramebuffer(0, 0, W, H, 0, 0, a.winW, a.winH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glEnable(GL_FRAMEBUFFER_SRGB);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static int desktopMain(App& a, bool asked) {
    a.desktop = true;
    LOGI("desktop spectator (%s)", asked ? "--spectate" : "no headset found");
    startUp(a);
    a.startTitle = false; a.firstHelp = false; a.menuOpen = false; a.titleOpen = false;
    std::wstring title = L"Project Arena - Spectator";
    if (!a.dcTag.empty()) title += L" - " + std::wstring(a.dcTag.begin(), a.dcTag.end());   // (ASCII, from --tag)
    SetWindowTextW(a.hwnd, title.c_str());
    typedef BOOL(WINAPI * SwapInterval)(int);
    if (auto si = (SwapInterval)wglGetProcAddress("wglSwapIntervalEXT")) si(1);   // the monitor paces frames here
    GetCursorPos(&a.dcMouse);
    desktopPanel(a);
    a.dcHintUntil = nowSeconds() + 8.0;
    while (!a.quit) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        if (a.quit) break;
        desktopFrame(a);
        SwapBuffers(a.hdc);
    }
    shutDown(a);
    return 0;
}

int main(int argc, char** argv) {
    App a;
    const char* selftestOut = nullptr;
    const char* probeAddr = nullptr;
    const char* bugServer = nullptr;
    const char* bugNoteArg = nullptr;
    bool playerReport = false;   // --player-report host:port NOTE: a REPORT PLAYER report about an example person
    const char* probeName = nullptr;
    double probeSeconds = 60;
    bool noDialog = false;
    bool spectateArg = false;   // --spectate: the desktop spectator window even with a headset connected
    const char* serverArg = nullptr;
    ShotView view;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--selftest") && i + 1 < argc) selftestOut = argv[++i];
        else if (!strcmp(argv[i], "--view") && i + 8 < argc) {   // --view W H x y z yawDeg pitchDeg fovYDeg (store art)
            view.custom = true;
            view.w = atoi(argv[i + 1]); view.h = atoi(argv[i + 2]);
            view.p = {(float)atof(argv[i + 3]), (float)atof(argv[i + 4]), (float)atof(argv[i + 5])};
            view.yaw = (float)atof(argv[i + 6]); view.pitch = (float)atof(argv[i + 7]); view.fovY = (float)atof(argv[i + 8]);
            i += 8;
        } else if (!strcmp(argv[i], "--title") && i + 3 < argc) {   // --title TEXT heightFraction yOffset
            view.title = argv[i + 1]; view.titleH = (float)atof(argv[i + 2]); view.titleY = (float)atof(argv[i + 3]);
            i += 3;
        }
        else if (!strcmp(argv[i], "--menu") && i + 1 < argc) view.menu = argv[++i];
        else if (!strcmp(argv[i], "--scene") && i + 1 < argc) view.scene = argv[++i];
        else if (!strcmp(argv[i], "--lobby")) view.lobby = true;
        else if (!strcmp(argv[i], "--tune") && i + 1 < argc) view.tunePart = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tunerow") && i + 1 < argc) view.tuneRow = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--suit") && i + 1 < argc) view.suit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hair") && i + 1 < argc) view.hair = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--swingtest")) view.swingTest = true;
        else if (!strcmp(argv[i], "--armsup") && i + 1 < argc) view.armsUp = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--dcshot") && i + 1 < argc) view.dcshot = argv[++i];
        else if (!strcmp(argv[i], "--layer") && i + 1 < argc) view.layer = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--keycol") && i + 1 < argc) view.keyCol = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--nations") && i + 2 < argc) { view.nations[0] = argv[++i]; view.nations[1] = argv[++i]; }
        else if (!strcmp(argv[i], "--opts") && i + 1 < argc) view.opts = argv[++i];
        else if (!strcmp(argv[i], "--record") && i + 2 < argc) { view.recordDir = argv[++i]; view.recordSeconds = (float)atof(argv[++i]); }
        else if (!strcmp(argv[i], "--size") && i + 2 < argc) { view.w = atoi(argv[++i]); view.h = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--mockdark") && i + 1 < argc) view.mockDark = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--crowd") && i + 3 < argc) {
            view.crowdFans = atoi(argv[++i]); view.crowdHype[0] = atoi(argv[++i]); view.crowdHype[1] = atoi(argv[++i]);
            if (i + 1 < argc && argv[i + 1][0] != '-') view.crowdCheer = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--winscreen") && i + 1 < argc) {
            const char* w = argv[++i];
            view.winStyle = !strcmp(w, "winner") ? 1 : !strcmp(w, "camera") ? 2 : !strcmp(w, "split") ? 0 : std::max(0, atoi(w)) % 3;
        }
        else if (!strcmp(argv[i], "--swingdeg") && i + 1 < argc) view.swingDeg = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--swinghold") && i + 1 < argc) view.swingHold = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--look") && i + 1 < argc) view.look = argv[++i];
        else if (!strcmp(argv[i], "--face") && i + 2 < argc) { view.face = (atoi(argv[i + 1]) & 1) | (atoi(argv[i + 2]) << 1); i += 2; }
        else if (!strcmp(argv[i], "--passthrough") && i + 1 < argc) { view.passthrough = true; view.ptMode = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--ptroom") && i + 1 < argc) view.ptRoom = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--bug-report") && i + 2 < argc) { bugServer = argv[i + 1]; bugNoteArg = argv[i + 2]; i += 2; }
        else if (!strcmp(argv[i], "--player-report") && i + 2 < argc) { bugServer = argv[i + 1]; bugNoteArg = argv[i + 2]; i += 2; playerReport = true; }
        else if (!strcmp(argv[i], "--xc-probe") && i + 2 < argc) { probeAddr = argv[i + 1]; probeName = argv[i + 2]; i += 2; }
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) probeSeconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--no-dialog")) noDialog = true;
        else if (!strcmp(argv[i], "--spectate")) spectateArg = true;
        else if (!strcmp(argv[i], "--tag") && i + 1 < argc) {   // a name for this spectator window's title
            for (const char* c = argv[++i]; *c && a.dcTag.size() < 24; ++c)
                if (*c >= 32 && *c < 127) a.dcTag += (char)std::toupper((unsigned char)*c);
        }
        else if (!strcmp(argv[i], "--server") && i + 1 < argc) serverArg = argv[++i];   // this session's community server
    }
    a.resumed = true;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    char assets[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, (dir + L"\\assets").c_str(), -1, assets, sizeof assets, nullptr, nullptr);
    platformSetAssets(nullptr, assets);
    char appdata[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
        a.dataPath = std::string(appdata) + "\\ProjectArena";
        CreateDirectoryA(a.dataPath.c_str(), nullptr);
    }
    logOpenFile(dataDir(a));
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (selftestOut) return selfTest(a, selftestOut, view);
    if (bugServer) {   // test: a report as REPORT A BUG would send it
        a.net.init();
        loadPlayerName(a);
        loadAvatar(a);
        a.serverAddr = bugServer;
        if (playerReport) {
            Net::Person p; p.id = 0x1234ABCDu; p.role = Net::VOICE_WAITING; std::snprintf(p.name, sizeof p.name, "EXAMPLE");
            a.reportFake.push_back(p);
            a.reportWho = p.id; std::memcpy(a.reportName, p.name, sizeof a.reportName); a.reportWhy = 3; a.reportNote = bugNoteArg;
            playerReportSubmit(a);
        } else {
            a.bugCat = 5;
            a.bugNote = bugNoteArg;
            bugSubmit(a);
        }
        for (int k = 0; k < 200 && a.bugDone == 0; ++k) Sleep(50);
        std::printf("%s\n", a.bugStatus.c_str());
        return a.bugDone == 1 ? 0 : 5;
    }
    if (probeAddr) {   // EXPERIMENTAL: --xc-probe host:port NAME [--seconds N]: the cross-play connection test, no VR
        xc::socketsInit();
        a.net.setName(probeName);
        const std::string name = sparcName(a);
        const double t0 = nowSeconds();
        a.sparc.connect(probeAddr, name, sparcId(a));
        xc::SparcLink::State last = xc::SparcLink::State::Off;
        double report = 0;
        while (true) {
            const double now = nowSeconds();
            a.sparc.update(now);
            const xc::SparcLink::State st = a.sparc.state();
            if (st != last || now - report > 5) {
                std::printf("%7.2f  %-10s  map %s  objects %d  players %d  packets in %llu out %llu  errors %u  %s\n", now - t0,
                            a.sparc.stateText(), a.sparc.map().c_str(), a.sparc.channelCount(), a.sparc.countOf("PlaygroundPlayerState"),
                            (unsigned long long)a.sparc.packetsIn(), (unsigned long long)a.sparc.packetsOut(), a.sparc.decodeErrors(),
                            a.sparc.error().c_str());
                std::fflush(stdout);
                last = st;
                report = now;
            }
            if (st == xc::SparcLink::State::Failed) return 2;
            if (now - t0 > probeSeconds) break;
            Sleep(5);
        }
        for (auto& kv : a.sparc.actors()) std::printf("  ch %4u  %s\n", kv.first, kv.second.path.c_str());
        for (const std::string& n : a.sparc.playerNames()) std::printf("  player: %s\n", n.c_str());
        const bool joined = a.sparc.state() == xc::SparcLink::State::Joined;
        a.sparc.disconnect("PROBE DONE");
        return joined ? 0 : 3;
    }
    // no headset (or --spectate): the desktop spectator window instead of an error
    if (!initGraphics(a)) {
        if (!noDialog) MessageBoxA(a.hwnd, "Project Arena needs an OpenGL 4.3 graphics driver.\n\nDetails: %APPDATA%\\ProjectArena\\projectarena_log.txt",
                                   "Project Arena", MB_ICONERROR);
        return 1;
    }
    const bool vr = !spectateArg && initInstance(a) && initActions(a) && initSession(a);
    if (!initContent(a)) { LOGE("initialisation failed"); return 1; }
    a.serverOverride = serverArg ? serverArg : "";
    if (!vr) {
        a.viewLayer = view.layer & 3;   // --layer 0-3 / --keycol 0-3: start in a view layer (an OBS key / transparency setup)
        a.dcKeyCol = 3;   // KEY is transparent (--keycol is still accepted from old shortcuts, and ignored)
        a.dcWinStyle = view.winStyle;   // --winscreen split|winner|camera
        return desktopMain(a, spectateArg);
    }
    startUp(a);
    bool exitRequested = false;
    while (!(a.quit && !a.running)) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        if (a.quit && a.running && !exitRequested) { xrRequestExitSession(a.session); exitRequested = true; }
        pollXrEvents(a);
        if (a.quit && !a.running) break;
        if (!a.running) { Sleep(10); continue; }
        if (!renderFrame(a)) LOGE("frame failed");
        SwapBuffers(a.hdc);
    }
    shutDown(a);
    return 0;
}
#endif
