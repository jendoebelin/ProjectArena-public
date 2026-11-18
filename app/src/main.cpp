// Project Arena -- native OpenXR entry point for Meta Quest (Android, GLES 3) and PC VR (Windows, OpenGL).
// Responsibilities: OpenXR instance/session/swapchains, controller input + haptics, the platform lifecycle,
// mapping tracking space onto the court layout, rendering, and driving Game (game.cpp).
#include <atomic>
#include <chrono>
#include <ctime>
#include <memory>
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
#include <commctrl.h>   // (the Spectator Controls window's tabs)
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

#include "auto_rotation.h"
#include "avatar.h"
#include "game.h"
#include "controls.h"   // BUTTONS: what each controller button does
#include "reach_log.h"
#include "log.h"
#include "render.h"
#include "audio.h"
#include "net.h"
#include "record.h"
#include "training.h"
#include "sound_studio.h"
#include "entitlement.h"   // the Meta store's ownership check (store builds only)
#ifdef _WIN32
#include "pc/discord_rpc.h"   // Discord status (PC only)
#include "pc/webcam.h"        // the selfie overlay's webcam (PC only)
#include "pc/updater.h"       // UPDATE NOW (PC only)
#endif
#if __has_include("servers_default.h")
#include "servers_default.h"   // generated at build time (tools/write_servers_header.py)
#endif
#ifndef PA_DEFAULT_SERVER
#define PA_DEFAULT_SERVER ""
#endif
#include "voice_codec.h"

#define XR_CHECK(x) do { XrResult r_ = (x); if (XR_FAILED(r_)) { LOGE("%s failed: %d (main.cpp:%d)", #x, (int)r_, __LINE__); return false; } } while (0)

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

static constexpr int kCtlStep = 5, kCtlMax = 30;   // CONTROLLER ANGLE: degrees a step, and how far each way

struct App {
    // ARENA GLASS (owner, 2026-11-14: players hard to see through the walls from the stream's cameras): the court's glass,
    // its glowing lines and the strike-zone rings this strong in the PC spectator's picture (1 NORMAL); nothing else changes
    // (the headsets draw the arena as always; nothing is sent). shotGlass: a --glass picture uses it too
    float dcGlass = 1.f;
    bool shotGlass = false;
    bool shotGloves = false;   // (pictures, --handsout: blue drawn as in the headset, the gauntlets only)
    int shotQueue = -1;   // --scene queue: the waiting spot whose kiosk and queue screen a picture shows (-1: none)
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
    int dcView = 1;                 // 1 side, 2 high, 3 blue end, 4 orange end, 5 top, 6 / 7 blue's / orange's eyes, 0 free, 8 MY CAMERA
    Pose dcEye;                     // the eyes camera, smoothed (a headset's every twitch is not for an audience)
    bool dcEyeSet = false;
    // MY CAMERAS (owner, 2026-11-02): the views you save (the headset's director list, warps.cfg), which one is showing,
    // AUTO's turn through them, a move between two of them (a fade glides, 0.8 s), a follow camera's place along the court
    int dcMy = -1;
    Pose dcLast, dcFrom;
    bool dcLastOk = false;
    float dcGlide = 0, dcFollowX = 0;
    uint8_t dcNewHold = 1, dcNewTrans = 1;   // what a newly saved camera gets (the panel's buttons change it)
    bool dcAuto = true, dcHelp = false, dcHubPinned = false, dcFull = false;   // dcHelp: the help bar in the picture
    int dcCmd = 0;                  // a button pressed in the Spectator Controls window (handled next frame)
    double dcHintUntil = 0;         // "F1: CONTROLS" in the picture for a few seconds after start
    double dcAutoAt = 0, dcWatchAt = 0;
    // AUTO ROTATION (owner, 2026-11-12): the presets and my cameras AUTO plays, ticked, in order (app/src/auto_rotation.h;
    // autocams.cfg once the streamer has changed it), the row it played last, its list in the controls window
    std::vector<autorot::Item> dcAutoList;
    bool dcAutoCustom = false, dcAutoLoaded = false;
    int dcAutoRow = -1, dcAutoTick = -1, dcAutoTickOn = 0;
    HWND dcAutoView = nullptr;
    std::wstring dcAutoShown;
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
    // the win screen's players as last seen in the room (a player who leaves while it shows stays, still, with their name;
    // the host puts a bot's input in an empty place, so their own last pose, not the slot's new one)
    PlayerInput dcHeldIn[2];
    std::string dcHeldName[2];
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
    // you hold PEEK's button (Y as standard: BUTTONS) during play
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
    // KEEPING THE CENTRE (0.3.19, issue #16; players 2026-11-11: "the view doesn't keep its center"): the headset says
    // its origin will move (a system recenter, the headset back on, tracking settling) before it does; each change waits
    // here until the frame it takes effect (its changeTime), then is applied (applySpaceChanges). Applied at once, the
    // court was re-anchored against the old origin and left off by the change (often ~20 degrees) until B
    struct PendingSpaceChange { XrTime changeTime; bool poseValid; XrPosef delta; };
    std::deque<PendingSpaceChange> spaceChanges;
    bool prevRecenter = false;
    // PLAYERS' NAMES (0.3.19, owner 2026-11-11: a quick way to see who's who, nothing over heads all the time): the left
    // stick's click (as standard: BUTTONS) shows everyone's name over their head until it's clicked again
    bool showNames = false, prevNamesClick = false;

    Renderer gfx;
    Model arena, environment, projectile, shell, spawner, shield, lancer, knuckle, head, torso, strikezone;
    Model gauntlets[3];   // GAUNTLET styles 0..2 (STRIKER, TITAN, PHANTOM); 3 = CONTROLLER (the lancer)
    Model gauntletHand[3], gauntletCuff[3];   // the same, split at the wrist (the cuff can be moved: TUNING)
    // HANDS APART: those fists and cuffs as shapes ([k][0] the fist, [k][1] the cuff; [3][0] the classic lancer), so a
    // player's two hands are drawn apart where their gauntlets would cross (avatar.h): built on a thread at start-up,
    // used once handShapesReady; noApart: pictures, --noapart
    avatar::HandShape handShape[4][2];
    std::atomic<bool> handShapesReady{false};
    bool noApart = false;
    bool noTwist = false;   // pictures, --notwist: FOREARM TWIST off
    bool shotHitboxes = false;   // pictures, --hitboxes: the knuckle guards' and shields' hit boxes drawn over them
    float shotArmScale = -1.f;   // (pictures, --armscale: every avatar's arms sized this; THE REACH)
    // the last few hands moved apart (the same hands are drawn for each eye and pass of a frame: worked out once)
    struct ApartMemo { Pose grip[2]; int style = -1; Vec3 shift{0, 0, 0}; };
    mutable ApartMemo apartMemo[8];
    mutable int apartNext = 0;
    // open art set only (absent in the original set): glossy floor + its mirrored light sources, additive glow layers
    Model floor, floorReflect, courtFx, envFx, courtPulse;
    Model stadiumFx, floorReflectEnv;   // the stadium's light beams / halos and its reflection in the floor (not in PASSTHROUGH)
    Model courtOutline;                 // PASSTHROUGH > OUTLINE: the neon edges and the floor lines
    double startTime = 0;
    struct Trail {
        Vec3 core[48], helixA[48], helixB[48];
        int n = 0;
        int stale = 0;      // frames the ball hasn't moved (a picture from the host only every few frames)
        float spinAngle = 0;
        float strikeFlash = 0;
        Vec3 burstPos; float burst = 0;   // dissolve burst at the back wall
    } trail[kMaxPlayers];
    // FIREBALL BLITZ: where each fireball was drawn lately (its echoes: smaller copies following it along its path)
    struct FireEcho {
        static constexpr int kN = 32;
        Vec3 p[kN]; float t[kN] = {};
        int n = 0, head = 0;
    } fireEcho[kMaxPlayers][Game::kMaxFire];
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
    // CONTROLLER ANGLE (0.3.20; a player through the owner, 2026-11-15: going from one headset's controllers to
    // another's, throws that felt natural had to be learned again; "options to set it to exactly what feels most
    // comfortable"): your own TILT, TWIST and TURN in degrees on top of the standard angle above, saved on this headset.
    // Your hands as the game sees them follow it (the gauntlet, the throw, the shield, the knuckle guard), and a guest
    // sends its hands turned already, so the host judges them the same; the laser keeps the controller's own pointing
    int ctlTilt = 0, ctlTwist = 0, ctlTurn = 0;
    bool ctlPage = false;          // settings: CONTROLLERS (POINTER and CONTROLLER ANGLE)
    // BALL FEEL (two players' report, 2026-11-17: "it doesn't feel like a ball ... more like old school pong"; "the
    // proportion/ratio of the plasma halo vs the core of the ball or maybe the heaviness"): four switches tried on this
    // headset (BALL FEEL (TEST)), then a room option, the host's (the owner, that evening: "make this a room option ...
    // heavier and glowing core is awesome and should definitely be default"): Tunables::ballFeel, BALL_* bits
    Quat ballQ[kMaxPlayers];       // (each ball's drawn turn: SPIN VISIBLE)
    Vec3 heldVis[kMaxPlayers];     // (a held ball where it is drawn: WEIGHT HEAVY)
    bool heldVisOn[kMaxPlayers] = {};
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
    float hitFlash[kMaxPlayers] = {0, 0, 0, 0};
    bool reachCue[2] = {false, false};   // THE REACH ROUND THE FIST: a hand of yours could take your ball now (it brightens)
    float bumpFlash = 0;            // FIST BUMP! (1 -> 0 over a second)
    float leadFlash = 0;            // FOUR CORNERS: a new leader (2.5 -> 0: the room flashes their colour, "ORANGE LEADS")
    // TRAINING (0.3.12): the lessons (training.h), your bests (training.cfg), its menu pages and what pops up on court
    training::Trainer trainer;
    // SOUND STUDIO (0.3.16, sound_studio.h): on (a TRAINING lesson runs endlessly as its moment), the section open, the
    // library synced from the server, each event's choice (a library file; none: the game's own), the clips loaded
    bool studioOn = false;
    int studioSection = 0;
    studio::Library studioLib;
    std::vector<studio::Sound> studioSounds;
    std::map<std::string, std::string> studioPick;
    std::map<std::string, bool> studioLoaded;
    std::string studioDelArmed;     // one of your own files, DELETE pressed once (pressed again soon: deleted)
    double studioDelUntil = 0;
    training::Bests trainBests;
    bool trainOpen = false;         // the lessons
    int trainBoard = -2;            // a leaderboard page: the lesson's index (-1: overall; -2: none)
    int trainBoardKind = 0;         // 0 FASTEST, 1 MOST ACCURATE
    double trainBoardAsked = 0;
    bool trainNewBest = false;      // the run just finished beat a best
    bool trainVideo = false;        // the tutorial video beside the card (SHOW ME HOW; off at first)
    // TRAINING's tutorial videos: a real run of the lesson (assets tutorials/lesson_<id>.par, a recording) replayed in
    // the game, filmed from over the player's shoulder onto the screen beside the card
    struct TrainFilm {
        int lesson = -1;                    // the lesson id it is for (-1: none asked for yet)
        bool have = false;                  // there is one
        rec::Replay rp;
        float t = 0, endWait = 0;           // its time (game seconds from its start); the pause at its end
        size_t frame = 0;
        std::vector<training::Target> targets;
        training::Flight flight;
        int bounces = 0;
        std::string caption;
        float captionT = -9.f;
        double renderedAt = 0;
    } film;
    double trainMenuAt = 0;         // the card opens then (after the move from the lobby onto the court)
    bool prevTrainA = true;         // A: START on the card, NEXT LESSON / TRY AGAIN on the result
    double trainLogAt = 0;
    struct TrainPop { std::string text; Vec3 pos; float t; float rgb[3]; float size; };
    std::vector<TrainPop> trainPops;   // TOO SLOW, STRIKE, BLOCKED ... where it happened (2 s)
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
    // DEFAULTS (0.3.19, owner 2026-11-11): each settings page's own button puts that page back as a fresh install has
    // it; pressed twice within 4 s (the first press arms it: RESET? PRESS AGAIN). Which page is armed: DEF_*
    double defaultsArmedUntil = 0;
    int defaultsArmed = -1;
    int dcMatchup = 1;
    bool dcMatchupForce = false;
    double dcVsT = 0;
    int sideNation[2] = {0, 0};
    float sideRGB[2][3] = {{0.1f, 0.7f, 1.0f}, {1.0f, 0.4f, 0.1f}};
    bool nationsOn = false;
    // The Crowd (fans on phones): each end's light flare from cheers (1 -> 0), the crowd sound's level, the last
    // update seen, whether an end's cheer has played (until its hype settles); testCrowd: --crowd in the pictures
    float crowdFlare[2] = {0, 0};
    double crowdBlipAt = 0, crowdBlipFor = -1;   // THE CROWD: the last crowd blip, and the update it answered
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
    // THE PUNCH LANDS (two players' report, 2026-11-17: a punch "kind of just felt like the ball hit the slider in pong"):
    // a knuckle hit's flash, and the ball squashed against the fist a moment
    struct PunchFx { Vec3 pos; float t, power; int team; };
    std::vector<PunchFx> punches;
    struct Squash { float t = 9.f, power = 0.f; Vec3 axis{0, 1, 0}; };
    Squash squash[kMaxPlayers];
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
    std::map<std::string, SwishShape> swishShapes;   // TENNIS SKIRT / KILT: each skirt's sway shape (<model>.swish)
    float shotSwish[3] = {0, 0, 0};   // (pictures, --swish X Z TWIST: every skirt swung this much)
    bool shotSwishOn = false;
    std::map<int64_t, SwingState> swings;   // drawn avatar * 1024 + rig id
    // THE REACH: each drawn avatar's rig across frames (the torso's lean, the shoulders, ARM SIZING), by the same key as
    // its hair (drawAvatarBody's swingKey, else its team): stepped by its first draw of a frame, read by the rest
    std::map<int, avatar::RigState> rigStates;
    // hair under headgear (av_gear_* / av_visor_*.tuck): per body the maps, and a texture per body, gear and face shield
    struct Tuck { bool ok = false; int w = 0, h = 0; float c[3] = {0, 0, 0}; std::vector<uint8_t> v; };
    Tuck tuckGear[2][9], tuckVisor[2][8];
    std::map<int, GLuint> tuckTex;
    GLuint patternTex[2][8] = {};   // per body: each body's own UV layout (tex/open_pat_<name>_<m|f>)
    AvatarDesc myAvatar, botAvatar;
    PlayerInput localIn;           // the latest local input (the builder's mirror runs while the game is frozen)
    // avatar builder: a mirror 0.85 m ahead (the reflection copies you) and a pointer-driven panel beside it
    bool builderOpen = false, prevBuilderTrig = true;
    bool replaceBuilder = false;   // (a recenter while the builder is open: its mirror and panel placed again)
    float mirrorArmsLogged = 0;    // (the mirror's ARM SIZING as last written to the log)
    AvatarDesc builderStart;   // the look when the builder opened (CANCEL puts it back)
    Pose builderPose;
    Vec3 mirrorPoint, mirrorNormal;
    int builderHover = -1, presetIdx = 0;
    int lastNation = 1;             // PRESET > COUNTRY FLAG: the country it picks first (the last one chosen)
    std::vector<uint32_t> kickVoted;   // REPORT PLAYER > VOTE TO REMOVE: who we voted on (this session)
    bool statsNoticeOpen = false, statsNoticeSeen = false;   // the one-time notice about statistics (settings.cfg)
    bool ownFail = false, ownQuit = false;   // a store build: Meta couldn't confirm the purchase (the page, then the game closes)
    double ownFailAt = 0;
    bool storeAsked = false;   // (the store asked for its latest version this run)
    Vec3 builderHit;
    bool builderHitValid = false;
    PlayerInput viewIn[kMaxPlayers];   // what is drawn for each player (remote ones smoothed)
    bool viewInValid[kMaxPlayers] = {false, false, false, false};
    bool titleOpen = false;        // title screen (game start): the menu shows PLAY / MULTIPLAYER / ...
    bool settingsOpen = false;
    int pointerHand = 1;           // hand that points at menus (settings: right / left)
    int fxLevel = 0;               // hit effects: 0 full, 1 soft, 2 low (photosensitivity)
    // BALL MARKS (0.3.17, a community idea): where a ball bounces off the floor, a wall or the glass, a mark that
    // fades. Style 0: a glowing ring in the thrower's colour; 1: a scorch (a hot glow that cools, a burnt patch)
    struct Mark { Vec3 p, n; float age = 0, life = 1.5f, size = 0.3f, rgb[3] = {1, 1, 1}, roll = 0; uint32_t seed = 0; };
    std::vector<Mark> marks;
    int markStyle = 1;
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
    bool creditsOpen = false;      // HOW TO PLAY > CREDITS (shown while helpOpen)
    bool nameEdit = false;
    int editTarget = 0;            // keyboard edits: 0 the lobby name, 1 the community server address, 2 team,
                                   // 4 bug note, 5 player report note
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
    bool movePlayersOpen = false;  // MOVE PLAYERS (hosting four players): swap two corners, make one THE ONE
    bool moveHost = false;         // ... its HAND OFF HOST form instead (any room): who hosts it from now on
    double watchGone = 0;
    int watchPeople = 0;
    double voiceZoneAt = 0;        // when we came where voice works (the push-to-talk reminder shows for a while)           // ... and how many people its room had (one alone: no one to hand it to)          // spectating: when the watched match went quiet (it may come back under a new host)
    int moveSel = -1;              // ... the corner picked first (-1: none)
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
    int optExpRoom = 0;            // EXPERIMENTAL's room: 0 DIAMOND, 1 FOUR CORNERS (four players)
    int optTeams = TEAMS_FFA;      // FOUR CORNERS: FREE-FOR-ALL, 2 V 2, 3 V 1
    bool optOneTurns = false;      // 3 V 1 hosted: THE ONE takes turns (Tunables::oneTurns), or you are THE ONE
    // DISCORD STATUS (PC, 0.3.13; kept on Quest too: the same settings file): "Playing Project Arena" in your Discord profile; what it last said and since when
    bool discordOn = true;
    std::string discordKey;
    int64_t discordSince = 0;
    // SELFIE (0.3.13, streamers): the PC spectator's webcam as a picture-in-picture over the broadcast; its corner
    // (0 top left, 1 top right, 2 bottom left, 3 bottom right), size (small, medium, large), round or square, which
    // camera; its texture and the picture last put in it (selfieTest: a test card instead, for pictures)
    bool updateOpen = false;   // UPDATE NOW's page (0.3.13)
    int dcEyesOf = -1;   // the PC spectator's eyes camera: whose eyes (their own body not drawn; shared: drawWorld asks)
    // SELFIE in the headset (0.3.13, spectators who stream): a camera in front of your avatar, its picture in the lower
    // right of your view (and so in a cast or recording); its render target, and when it last drew
    bool headSelfie = false;
    // SELFIE CAPTURE (promo, sideloaded builds only; adb: touch <externalDataPath>/selfie_capture_on or _off): while
    // on, the selfie camera's view is also drawn at 2560x1440 twice a second and saved as
    // <externalDataPath>/selfiecap_<launch time>_NNNNN.ppm (the time keeps a later session from writing over an
    // earlier one's frames); the spectator's own view doesn't change
    bool hsCap = false;
    double hsCapAt = 0;
    int hsCapN = 0;
    long hsCapStamp = 0;
    unsigned hsCapFbo = 0, hsCapTex = 0, hsCapDepth = 0;
    unsigned hsFbo = 0, hsTex = 0, hsDepth = 0;
    double hsAt = 0;
    bool selfieOn = false, selfieRound = true, selfieTest = false;
    int selfieCorner = 3, selfieSize = 1, selfieCam = 0, selfieW = 0, selfieH = 0;
    unsigned selfieTex = 0;
    uint32_t selfieSeen = 0;
    int optHealth = 5, optBalls = 1, optOctPlayers = 4;   // FIREBALL BLITZ: HEALTH, BALLS (4 = RAPID FIRE), PLAYERS (practice)
    int optStrikeSpeed = 0;        // STRIKE SPEED (0.3.19): 0 REGULATION, 1 FAST, 2 FASTER
    int optFoulShift = 0;          // FOUL LINE (0.3.20): -2 .. +2 steps of 0.5 m, + LONGER (toward the centre)
    // ROOMS AND THEIR OPTIONS (0.3.21): SHIELDS ALWAYS ON (MAIDEN), AIM ASSIST (-1 the rules'), STRIKE ZONE, BALL GROWTH
    bool optMaiden = false;
    int optAim = -1, optZone = 1, optGrowth = 3;
    int optRounds = 2;             // ROUNDS: rounds to win, 1 to 5 (BEST OF 1 to 9)
    int optBotPlay = 0;            // BOT STYLE: 0 BRAWLER, 1 DUELLIST (with the bot's level)
    bool roundsPad = false;        // ... BEST OF typed on a number pad (digits only)
    int optShieldSize = 0;         // SHIELD SIZE: 0 REGULATION .. 4 DOUBLE
    int optBallFeel = BALL_FEEL_DEFAULT;   // BALL FEEL: BALL_* bits
    bool roomBallPage = false;     // ... its four switches, a page of the room's own
    std::string roundsBuf;
    bool fairTold = false;         // FAIR MATCHES: this match's countdown has told the host what plays at regulation
    // FIREBALL BLITZ on screen: the last knockout or "THE ROOM CLOSES IN" (and how long it shows), the fireballs' puffs as they
    // fizzle out, and the ring as last seen (it closing in: a blink to your new place)
    std::string octMsg;
    float octMsgT = 0;
    int octMsgP = -1;
    struct Puff { Vec3 pos; float t; int p; };
    std::vector<Puff> puffs;
    uint16_t fireDanger = 0;   // the fireballs (bit owner * 3 + k) that would hit you where you stand
    bool wasOut = false;       // knocked out (in the dugout) as last seen: a change blinks you there or back
    uint8_t ringSeen = 0;
    Vec3 ringStartSeen[kMaxPlayers];
    float octWallR = 7.5f;   // its walls as drawn (sliding to where the game has them)
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
    int cfgVersion = 0;             // settings.cfg cfgver= (2: older files may hold a test server address; 3: gauntlet pitch default;
                                    // 9: BALL LOOK-AHEAD on by default)
    uint32_t reportWho = 0;
    char reportName[17] = {};
    int reportWhy = -1;
    std::string reportNote;
    std::vector<uint32_t> mutedIds;
    std::vector<Net::Person> reportFake;   // self-test pictures only (--menu report)
    std::string serverOverride, serverSaved;   // PC --server host:port for this session; the saved choice stays
    std::string serverAddr;        // community relay "host[:port]" (settings.cfg server=...)         // the menu shows a keyboard for the lobby name
    std::string nameBuf;
    bool devMode = false;
    bool noOnline = false;   // --selftest: never connect to the community server
    int trailMax = 0;        // --record: the newest points of a ball's trail drawn (0 all; -1 none, --notrail)
    bool mockContact = false;   // --contact (pictures only, a mock-up for the owner, 2026-11-11): contact shadows
    // voice chat: push-to-talk (left grip) by default, open mic, or off; mutes by who is talking
    int voiceMode = 0;             // 0 push-to-talk, 1 open mic, 2 off
    // BUTTONS (controls.h; a player's report, 2026-11-17: let us change the push-to-talk button, and the controls; the
    // owner: a page to remap them, with a LEFT-HANDED preset): what each button does, on this headset
    controls::Map btnMap = controls::standard();
    int ctlKind = 0;               // controls::K_*: the controllers in your hands (the interaction profile; 0 until known)
    bool btnDown[controls::S_COUNT] = {};   // each button this frame (readInput)
    float gripVal[2] = {};                  // the grips as push to talk reads them (see pushToTalk)
    bool btnPage = false;                   // SETTINGS > CONTROLLERS > BUTTONS (or VOICE CHAT's TALK BUTTON)
    std::string btnNote; double btnNoteUntil = 0;   // the page's word on a change it can't make
    bool muteSpectators = false, mutePlayers = false, muteAll = false;
    bool voicePage = false;        // settings sub-page
    bool gfxPage = false;          // settings: GRAPHICS (the arena shows around the buttons)
    bool ptt = false, talking = false, micAsked = false, micDenied = false;   // micDenied: asked, not granted (yet)
    bool micFailed = false;        // allowed, but the microphone wouldn't open (tried again every 10 s)
    bool micGranted = false;       // (as last checked, once a second in a voice zone)
    double micCheckAt = 0;
    double micAskAt = 0;           // the headset's microphone prompt comes then (a heads-up shows till then; 0: none due)
    float micGain = 1.0f;          // automatic microphone gain
    int voiceSent = 0, voiceRecv = 0;
    float pttGripMax = 0; int pttHeldFrames = 0;   // push-to-talk diagnostics: the talk button's peak, frames held (per log line)
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
        float pitch = 0, fov = 0;  // the PC spectator's cameras (0.3.13): looking up or down, its zoom (0: as the view is)
    };
    std::vector<WarpCam> warps;
    int warpIdx = -1;
    bool autoCam = false, prevA2 = true, prevX = true, prevY2 = true, turnLatch = false;
    double autoNext = 0, lastFly = 0;
    bool fadeCapture = false, fadePending = false;      // capture this frame, then move and fade it out
    Pose fadeTarget;
    float fadeT = 0;               // > 0 while the captured view fades out over the new one
    GLuint capTex[2] = {}, capFbo[2] = {};
    bool fadeDark = false;         // the move pending is a blink through dark (startBlink), not a cross-fade
    float darkOut = 0, darkIn = 0; // the blink: darkening before the move, lightening after it
    GLuint blackTex = 0;
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
// UPDATE NOW on Quest (0.3.13): who installed the game (the Meta store, or someone with adb / SideQuest: no installer),
// and a link opened in the headset (the store page, or the GitHub page)
static std::string androidInstaller() {
    if (!g_vm || !g_activity) return "?";
    JNIEnv* env = nullptr;
    g_vm->AttachCurrentThread(&env, nullptr);
    if (env->PushLocalFrame(16) != 0) { env->ExceptionClear(); return "?"; }
    std::string out = "?";
    jclass act = env->GetObjectClass(g_activity);
    jobject pm = env->CallObjectMethod(g_activity, env->GetMethodID(act, "getPackageManager", "()Landroid/content/pm/PackageManager;"));
    jobject pkg = env->CallObjectMethod(g_activity, env->GetMethodID(act, "getPackageName", "()Ljava/lang/String;"));
    if (!env->ExceptionCheck() && pm && pkg) {
        jmethodID get = env->GetMethodID(env->GetObjectClass(pm), "getInstallerPackageName", "(Ljava/lang/String;)Ljava/lang/String;");
        jstring inst = get ? (jstring)env->CallObjectMethod(pm, get, pkg) : nullptr;
        if (env->ExceptionCheck()) env->ExceptionClear();
        else if (!inst) out = "";
        else { const char* c = env->GetStringUTFChars(inst, nullptr); out = c ? c : "?"; env->ReleaseStringUTFChars(inst, c); }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->PopLocalFrame(nullptr);
    return out;
}
static bool androidOpenUrl(const char* url) {
    if (!g_vm || !g_activity) return false;
    JNIEnv* env = nullptr;
    g_vm->AttachCurrentThread(&env, nullptr);
    if (env->PushLocalFrame(16) != 0) { env->ExceptionClear(); return false; }
    jclass uriCls = env->FindClass("android/net/Uri");
    jobject uri = env->CallStaticObjectMethod(uriCls, env->GetStaticMethodID(uriCls, "parse", "(Ljava/lang/String;)Landroid/net/Uri;"), env->NewStringUTF(url));
    jclass intentCls = env->FindClass("android/content/Intent");
    jobject intent = env->NewObject(intentCls, env->GetMethodID(intentCls, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V"),
                                    env->NewStringUTF("android.intent.action.VIEW"), uri);
    env->CallObjectMethod(intent, env->GetMethodID(intentCls, "addFlags", "(I)Landroid/content/Intent;"), 0x10000000);   // NEW_TASK
    env->CallVoidMethod(g_activity, env->GetMethodID(env->GetObjectClass(g_activity), "startActivity", "(Landroid/content/Intent;)V"), intent);
    const bool ok = !env->ExceptionCheck();
    if (!ok) env->ExceptionClear();
    env->PopLocalFrame(nullptr);
    LOGI("update: opened %s (%s)", url, ok ? "ok" : "failed");
    return ok;
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
    if (!mk(a.buttonA, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_a", true)) return false;   // (per hand: right A, left X)
    if (!mk(a.buttonB, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_b", false)) return false;
    if (!mk(a.buttonY, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_y", false)) return false;
    if (!mk(a.buttonX, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_x", false)) return false;
    if (!mk(a.thumbClick, XR_ACTION_TYPE_BOOLEAN_INPUT, "thumb_click", true)) return false;
    if (!mk(a.menu, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", false)) return false;
    if (!mk(a.haptic, XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", true)) return false;
    if (!mk(a.thumbstick, XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick", false)) return false;
    if (!mk(a.thumbstickL, XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick_left", false)) return false;
    if (!mk(a.pushToTalk, XR_ACTION_TYPE_FLOAT_INPUT, "push_to_talk", true)) return false;   // (per hand: PUSH TO TALK BUTTON)

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
        {a.pushToTalk, path(a, "/user/hand/left/input/squeeze/value")}, {a.pushToTalk, path(a, "/user/hand/right/input/squeeze/value")},
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
        {a.menu, P(L, "b/click")}, {a.buttonY, P(L, "trackpad/force")}, {a.pushToTalk, P(L, "squeeze/force")}, {a.pushToTalk, P(R, "squeeze/force")},
        {a.thumbClick, P(L, "thumbstick/click")}, {a.thumbClick, P(R, "thumbstick/click")},
        {a.thumbstick, P(R, "thumbstick")}, {a.thumbstickL, P(L, "thumbstick")}}));
    suggest("/interaction_profiles/htc/vive_controller", with({
        {a.trigger, P(L, "trigger/value")}, {a.trigger, P(R, "trigger/value")}, {a.squeeze, P(L, "squeeze/click")}, {a.squeeze, P(R, "squeeze/click")},
        {a.buttonA, P(R, "trackpad/click")}, {a.buttonX, P(L, "trackpad/click")}, {a.buttonA, P(L, "trackpad/click")},
        {a.buttonB, P(R, "menu/click")}, {a.menu, P(L, "menu/click")}, {a.pushToTalk, P(L, "squeeze/click")}, {a.pushToTalk, P(R, "squeeze/click")},
        {a.thumbstick, P(R, "trackpad")}, {a.thumbstickL, P(L, "trackpad")}}));
    suggest("/interaction_profiles/microsoft/motion_controller", with({
        {a.trigger, P(L, "trigger/value")}, {a.trigger, P(R, "trigger/value")}, {a.squeeze, P(L, "squeeze/click")}, {a.squeeze, P(R, "squeeze/click")},
        {a.buttonA, P(R, "trackpad/click")}, {a.buttonX, P(L, "trackpad/click")}, {a.buttonA, P(L, "trackpad/click")},
        {a.buttonB, P(R, "menu/click")}, {a.menu, P(L, "menu/click")}, {a.pushToTalk, P(L, "squeeze/click")}, {a.pushToTalk, P(R, "squeeze/click")},
        {a.thumbClick, P(L, "thumbstick/click")}, {a.thumbClick, P(R, "thumbstick/click")},
        {a.thumbstick, P(R, "thumbstick")}, {a.thumbstickL, P(L, "thumbstick")}}));
    suggest("/interaction_profiles/khr/simple_controller", with({
        {a.trigger, P(L, "select/click")}, {a.trigger, P(R, "select/click")}, {a.menu, P(L, "menu/click")}, {a.buttonB, P(R, "menu/click")}}));
    return true;
}

// BUTTONS (controls.h): the map as it works on the controllers in your hands, and a job's buttons by name
static controls::Map btnMap(const App& a) { return controls::effective(a.btnMap, a.ctlKind); }
static int talkSlot(const App& a) { return controls::firstSlot(btnMap(a), controls::J_TALK, a.ctlKind); }
static std::string jobButtons(const App& a, int job, bool clicks) {   // "B / RIGHT STICK CLICK" (clicks: a stick's CLICK)
    const controls::Map m = btnMap(a);
    std::string s;
    for (int k = 0; k < controls::S_COUNT; ++k) {
        if (m.job[k] != job || !controls::onController(a.ctlKind, k)) continue;
        if (!s.empty()) s += " / ";
        s += controls::slotName(a.ctlKind, k);
        if (clicks && (k == controls::S_LSTICK || k == controls::S_RSTICK)) s += " CLICK";
    }
    return s;
}
static std::string readyName(const App& a) {   // PRESS A: READY's button (A as standard)
    const int s = controls::firstSlot(btnMap(a), controls::J_READY, a.ctlKind);
    return s < 0 ? std::string("A") : std::string(controls::slotName(a.ctlKind, s));
}
static std::string talkHint(const App& a) {   // HOLD LEFT GRIP TO TALK, HOLD THE LEFT STICK IN TO TALK
    const int s = talkSlot(a);
    if (s < 0) return "PUSH TO TALK: NO BUTTON";
    const bool stick = s == controls::S_LSTICK || s == controls::S_RSTICK;
    return std::string(stick ? "HOLD THE " : "HOLD ") + controls::slotName(a.ctlKind, s) + (stick ? " IN TO TALK" : " TO TALK");
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
static Mat4 ueLocalToGrip(Vec3 locCm, Vec3 pyrDeg, Vec3 scale) { return avatar::ueLocalToGrip(locCm, pyrDeg, scale); }

static Mat4 g_lancerLocal, g_shieldLocal, g_knuckleLocal, g_plateLocal;
// SHIELD SIZE: the plate grown in its own width and height (shield space: x and z), as it plays
static Mat4 shieldSizeM(const Game& g) { const float s = g.tun.shieldScale(); return Mat4::fromPose({Quat{}, {0, 0, 0}}, {s, 1.f, s}); }
constexpr float kStrikeRingModel = 0.75f;   // strikezone.mdl: a disc of this radius (m), drawn scaled to the strike's own

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

// <model>.swish (TENNIS SKIRT / KILT, tools/import_pr25.py): 'SWSH', u32 kind, f32 waist y, hem y, centre x, z, the most a
// vertex moves, the first row's y, the row step, u32 rows, cols, rows x cols f32 body radii
static void loadSwish(App& a, const char* model) {
    std::vector<uint8_t> d;
    if (!assetRead(std::string(model) + ".swish", d) || d.size() < 44 || std::memcmp(d.data(), "SWSH", 4) != 0) return;
    uint32_t kind, rows, cols;
    float f[7];
    std::memcpy(&kind, d.data() + 4, 4);
    std::memcpy(f, d.data() + 8, sizeof f);
    std::memcpy(&rows, d.data() + 36, 4);
    std::memcpy(&cols, d.data() + 40, 4);
    if (rows < 2 || cols < 8 || rows > 512 || cols > 512 || d.size() < 44 + (size_t)rows * cols * 4 || f[6] <= 0) return;
    std::vector<float> r((size_t)rows * cols);
    std::memcpy(r.data(), d.data() + 44, r.size() * 4);
    SwishShape s;
    s.kind = (int)kind;
    s.waistY = f[0]; s.hemY = f[1];
    s.c[0] = f[2]; s.c[1] = f[3]; s.c[2] = f[4];
    s.p[0] = f[5]; s.p[1] = 1.f / f[6]; s.p[2] = (float)rows; s.p[3] = (float)cols;
    s.tex = a.gfx.swishTexture((int)cols, (int)rows, r.data());
    a.swishShapes[model] = s;
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
            for (int k = 0; k < 16; ++k) { snprintf(n, sizeof n, "av_top_%d_%c", k, bk); part(n); }
            for (int s = 14; s < 16; ++s)   // VECTOR and NIGHTFORGE: their own LOWER BODY 1-4
                for (int k = 1; k <= 4; ++k) { snprintf(n, sizeof n, "av_sbottom_%d_%d_%c", s, k, bk); part(n); }
            for (int k = 10; k < 14; ++k) { snprintf(n, sizeof n, "av_tpulse_%d_%c", k, bk); part(n); }   // the moving pulses
            for (int k = 1; k <= 6; ++k) { snprintf(n, sizeof n, "av_bottom_%d_%c", k, bk); part(n); if (k >= 5) loadSwish(a, n); }
            // head parts fitted to this body's head (tools/blender/make_avatar.py --head)
            const int bi = bk == 'f' ? 1 : 0;
            for (int k = 1; k <= 8; ++k) { snprintf(n, sizeof n, "av_hair_%d_%c", k, bk); part(n); loadSwing(a, n); }
            for (int k : {3, 7}) { snprintf(n, sizeof n, "av_hair_%dt_%c", k, bk); part(n); loadSwing(a, n); }   // tail, loose ends
            for (int k = 1; k <= 7; ++k) { snprintf(n, sizeof n, "av_visor_%d_%c", k, bk); part(n); loadTuck(n, a.tuckVisor[bi][k]); }
            for (int k = 1; k <= 8; ++k) { snprintf(n, sizeof n, "av_gear_%d_%c", k, bk); part(n); loadTuck(n, a.tuckGear[bi][k]); }
            for (int k = 1; k <= 5; ++k) { snprintf(n, sizeof n, "av_mask_%d_%c", k, bk); part(n); }
            for (int f = 1; f < avatar::kFaces; ++f) {   // FACE 2..4: their heads, and face shields / masks fitted to them
                snprintf(n, sizeof n, "av_bhead_%c%d.mdl", bk, f);
                if (!assetExists(n)) continue;   // (the PR #25 bodies have one face each: FACE 2..4 draw the base head's)
                snprintf(n, sizeof n, "av_bhead_%c%d", bk, f); part(n);
                for (int k = 1; k <= 7; ++k) { snprintf(n, sizeof n, "av_visor_%d_%c%d", k, bk, f); part(n); }
                for (int k = 1; k <= 5; ++k) { snprintf(n, sizeof n, "av_mask_%d_%c%d", k, bk, f); part(n); }
            }
        }
        for (int k = 1; k < 8; ++k)
            if (const char* t = avatar::patternTexture(k))
                for (int bi = 0; bi < 2; ++bi) {   // (each body's own, else the one both bodies shared)
                    const std::string own = std::string(t) + (bi ? "_f" : "_m");
                    a.patternTex[bi][k] = a.gfx.texture(assetExists("tex/" + own + ".tex") ? own.c_str() : t, false);
                }
        LOGI("avatars: %d part models, %d hair swing rigs", (int)a.avModels.size(), (int)a.swingRigs.size());
    }
    // VisualTransform (1,-0.5,0) pitch 5; ShieldTransform (20,0,10) roll 180 scale 10;
    // KnuckleGuardTransform (10.5,0,0)
    g_lancerLocal = ueLocalToGrip({1, -0.5f, 0}, {5, 0, 0}, {1, 1, 1});
    g_shieldLocal = ueLocalToGrip({20, 0, 10}, {0, 0, 180}, {10, 10, 10});
    g_knuckleLocal = ueLocalToGrip({1.5f, 0, 0}, {0, 0, 0}, {1, 1, 1});   // KnuckleGuardTransform (10.5) + TempMesh (-9)
    g_plateLocal = ueLocalToGrip({0, 0, 0}, {0, 0, 180}, {10, 10, 10});   // Shield_01 plate centred, normal = grip Y
    static std::atomic<bool> shapesStarted{false};   // (once: the frames read them once they're ready)
    if (!shapesStarted.exchange(true)) {   // HANDS APART: the gauntlets' shapes, from their models read again (the
        // renderer keeps no copy), built on a thread (about a quarter of the loading on a PC); the hands are drawn as
        // tracked until they're ready
        struct Job { std::vector<uint8_t> data; int k, part, sides; };
        auto jobs = std::make_shared<std::vector<Job>>();
        auto add = [&](const std::string& name, int k, int part, int sides) {
            std::vector<uint8_t> d;
            if (assetRead(name + ".mdl", d)) jobs->push_back({std::move(d), k, part, sides});
        };
        for (int k = 0; k < 3; ++k) {
            add("gauntlet_" + std::to_string(k) + "_hand", k, 0, 5);
            add("gauntlet_" + std::to_string(k) + "_cuff", k, 1, 4);   // (open at both ends)
        }
        add("lancer", 3, 0, 5);
        const Mat4 local = g_lancerLocal;
        std::thread([&a, jobs, local]() {
            const double t0 = nowSeconds();
            for (Job& j : *jobs) avatar::handShapeFromModel(a.handShape[j.k][j.part], j.data.data(), j.data.size(), local, j.sides);
            a.handShapesReady.store(true, std::memory_order_release);
            LOGI("hands apart: the gauntlets' shapes in %.0f ms (%d, %d points)", (nowSeconds() - t0) * 1000.0,
                 (int)a.handShape[0][0].pts.size(), (int)a.handShape[0][1].pts.size());
        }).detach();
    }
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
static void placeBuilder(App& a, const Pose& head);
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
static const char* kAppVersion = "0.3.21";   // (0.3.20: the Meta competition build)
// A scrolling texture's offset, kept in [0, 1) (the textures repeat, so it looks the same). The headset's fragment
// shader works at medium precision (16-bit floats): an offset that grows with the time played lost its fractions after
// some minutes, and the ball's and fireballs' plasma broke into big square blocks (a tester's report, 2026-11-02).
static float wrapUV(double v) { return float(v - std::floor(v)); }

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
    // a store copy (0.3.18): the store says (asked once the ownership check passes); its timing is the store's own
    if (entitlement::updateState() != entitlement::Update::Unavailable) {
        const entitlement::Update us = entitlement::updateState();
        if (us == entitlement::Update::Available || us == entitlement::Update::Downloading ||
            us == entitlement::Update::Downloaded || us == entitlement::Update::Installing)
            return entitlement::updateLatest()[0] ? entitlement::updateLatest() : "THE NEWEST VERSION";
        return nullptr;
    }
    const char* v = a.net.latestVersion();
    if (!v[0] || !versionNewer(v, kAppVersion)) return nullptr;
    static std::string shown;
    if (shown != v) { shown = v; LOGI("update: %s is out (this is %s)", v, kAppVersion); }
    return v;
}
#ifdef __ANDROID__
static bool isDesktop(const App&) { return false; }
#else
static bool isDesktop(const App& a) { return a.desktop; }   // (the PC spectator window, no headset)
#endif
static const char* kStorePage = "https://www.meta.com/experiences/1342896058915258/";   // (the store page since the launch, 2026-11-10; was the alpha invite)
static const char* kReleasePage = "https://github.com/jendoebelin/ProjectArena-public/releases/latest";
// how this copy updates. Quest: 0 the Meta store, 1 sideloaded (adb, SideQuest), 2 can't tell (both offered). PC: 0 here
// (download and restart), 1 Steam, 2 the download page only. `why`: what to say about it
static int updateHow(std::string& why) {
#ifdef __ANDROID__
    static std::string inst = "?";
    if (inst == "?") { inst = androidInstaller(); LOGI("update: installed by '%s'", inst.c_str()); }
    if (inst.find("oculus") != std::string::npos || inst.find("meta") != std::string::npos) { why = "THE STORE INSTALLS IT"; return 0; }
    if (inst.empty() || inst == "com.android.shell" || inst.find("packageinstaller") != std::string::npos || inst.find("sidequest") != std::string::npos) {
        why = "DOWNLOAD THE NEW APK AND INSTALL IT AS YOU DID BEFORE (SIDEQUEST)";
        return 1;
    }
    why = "FROM THE STORE, OR THE GITHUB PAGE IF YOU INSTALLED IT YOURSELF";
    return 2;
#else
    return updater::how(why);
#endif
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
    {   // (a copy Steam installed says so: its updates come from Steam, its reports are told apart)
        std::string why;
        snprintf(b, sizeof b, "build: %s PC%s\n", kAppVersion, updater::how(why) == 1 ? " (Steam)" : "");
    }
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
// TRAINING's own pages beside the court (a lesson's card and its result): no side menu, the court and the coach in view
static float trainColX(int track) { return -0.325f + 0.325f * track; }   // the lessons page: a column a track
static float trainRowY(int place) { return 0.165f - 0.0705f * place; }
static float studioRowY(int k) { return 0.125f - 0.052f * k; }   // SOUND STUDIO's rows, an event each
static int studioChosen(const App& a, const std::string& ev) {   // the library sound chosen for an event (-1: the game's own)
    auto it = a.studioPick.find(ev);
    if (it == a.studioPick.end() || it->second.empty()) return -1;
    for (int i = 0; i < (int)a.studioSounds.size(); ++i)
        if (a.studioSounds[i].ready && a.studioSounds[i].file == it->second) return i;
    return -1;
}
static bool studioVoteLit(const App& a, uint32_t ip) {   // UP (ip bit 8) or DOWN of row ip & 255: your vote for its sound
    const studio::Section& sec = studio::sections()[a.studioSection];
    const uint32_t k = ip & 0xFFu;
    if (k >= sec.events.size()) return false;
    const int s = studioChosen(a, sec.events[k]);
    return s >= 0 && a.studioSounds[s].vote == ((ip & 0x100u) ? 1 : -1);
}
static bool otherPage(const App& a) {   // a page other than TRAINING's is open
    return a.titleOpen || a.settingsOpen || a.statsOpen || a.helpOpen || a.bugOpen || a.reportOpen || a.rulesOpen || a.xpOpen ||
           a.statsNoticeOpen || a.movePlayersOpen || a.updateOpen || a.ownFail;
}
static bool trainSide(const App& a) {
    using training::Phase;
    return a.trainer.active() && !a.trainOpen && a.trainBoard == -2 && !otherPage(a) &&
           (a.trainer.phase == Phase::Card || a.trainer.phase == Phase::Done);
}

// holding area outside the tank (waiting players): four spots along the -Z side, facing the glass
static Vec3 holdingSpot(int spot) { return {-4.5f + 3.0f * (spot & 3), 0, -4.2f}; }
static Quat yawToward(const Vec3& f) { return Quat::axisAngle({0, 1, 0}, std::atan2(-f.x, -f.z)); }   // -Z along f
static bool cornersRoom(const App& a) { return a.game.arenaKind == ARENA_FOURCORNERS || a.game.arenaKind == ARENA_CORNERS; }
// FIREBALL BLITZ: its waiting spots in a row outside the glass behind the wall between places 0 and 2 (its floor is level
// with the stadium's walkway); the gallery seat outside the wall opposite
static Pose octWaiting(int spot) {
    const Vec3 out = normalize(Vec3{-1, 0, -1}), along = normalize(Vec3{1, 0, -1});
    const float sd = (spot & 4) ? -1.f : 1.f;   // (4-7: the deck on the other side)
    return {yawToward(-out * sd), out * (9.2f * sd) + along * (sd * 1.5f * ((spot & 3) - 1.5f))};
}
// a waiting spot (its floor point) and the way it faces: FOUR CORNERS' room fills the classic holding area, so its four
// spots stand outside the room's glass, along the wall between corners 0 and 2, facing in
static Pose holdingPose(const App& a, int spot) {
    if (a.game.octagon()) return octWaiting(spot);
    if (cornersRoom(a)) {
        const Vec3 c0{-exparena::kCorner, 0, 0}, c1{0, 0, -exparena::kCorner}, in = normalize(Vec3{1, 0, 1});
        const float sd = (spot & 4) ? -1.f : 1.f;   // (4-7: the deck on the other side)
        return {yawToward(in * sd), (lerp(c0, c1, 0.2f + 0.2f * (spot & 3)) - in * 0.9f) * sd};
    }
    return {Quat{}, holdingSpot(spot)};   // (facing +Z: -Z forward turned round below)
}
static Vec3 holdingFwd(const App& a, int spot) { return cornersRoom(a) || a.game.octagon() ? holdingPose(a, spot).q.rotate({0, 0, -1}) : Vec3{0, 0, 1}; }
// the spectator's floating gallery seat at the arena it watches (FOUR CORNERS: outside the room, by a side wall)
static Pose spectatorSeat(const App& a) {
    if (a.game.octagon()) { const Vec3 in = normalize(Vec3{-1, 0, 1}); return {yawToward(in), Vec3{6.9f, 1.0f, -6.9f}}; }
    if (cornersRoom(a)) { const Vec3 in = normalize(Vec3{1, 0, -1}); return {yawToward(in), Vec3{-5.6f, 1.0f, 5.6f}}; }
    return {Quat{}, {0, 1.0f, 5.4f}};
}
// each player's colour: two players their end's (a country's with NATIONS); FOUR CORNERS free-for-all four colours,
// teams their team's
static const float kFourRGB[4][3] = {{0.1f, 0.7f, 1.0f}, {1.0f, 0.4f, 0.1f}, {0.25f, 0.95f, 0.35f}, {0.75f, 0.4f, 1.0f}};
static const float* playerRGB(const App& a, int p) {
    const Game& g = a.game;
    if (g.np == 2) return a.sideRGB[p & 1];
    return (g.tun.teamMode == TEAMS_FFA || g.octagon()) ? kFourRGB[p & 3] : a.sideRGB[g.team(p) & 1];
}

// BALL MARKS: the surface the ball bounced off (asked of the arena's own collision, the same on every machine: the event
// carries only where the ball was), and a mark on it, sized by the ball's speed
static void addMark(App& a, const Vec3& at, int player, float speed) {
    // (HIT EFFECTS: LOW: every other bounce. It used to skip whenever the count of marks showing was odd, so once one
    // showed every bounce was refused until it faded: about one at a time, not half; found porting to Unity, 2026-11-09)
    static uint32_t bounces = 0;
    if (a.fxLevel >= 2 && (bounces++ & 1)) return;
    static const Vec3 dirs[] = {{0, -1, 0}, {0, 1, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1},
                                {0.7071f, 0, 0.7071f}, {-0.7071f, 0, 0.7071f}, {0.7071f, 0, -0.7071f}, {-0.7071f, 0, -0.7071f}};
    // (sweepArena tests a sphere at the end point against surfaces it is moving into: a sphere round the bounce point
    // nudged toward each direction finds the surfaces on that side; the nearest wins)
    const float R = 0.22f;
    float best = 1e9f;
    Vec3 bp, bn;
    for (const Vec3& d : dirs) {
        Vec3 hp, hn;
        uint32_t tags = 0;
        if (!a.game.sweepArena(at - d * 0.01f, at, R, hp, hn, tags)) continue;
        const Vec3 surf = hp - hn * R;
        if (length(surf - at) < best) { best = length(surf - at); bp = surf; bn = hn; }
    }
    if (best > 1e8f || length(bn) < 0.5f) return;
    if (dot(bn, at - bp) < 0) bn = -bn;   // (facing the ball's side)
    App::Mark m;
    m.n = normalize(bn);
    m.p = bp + m.n * 0.006f;
    m.size = clampf(0.20f + 0.016f * speed, 0.20f, 0.55f) * (a.fxLevel >= 1 ? 0.85f : 1.f);
    m.life = 4.0f;
    const float* c = player >= 0 ? playerRGB(a, player) : nullptr;
    for (int k = 0; k < 3; ++k) m.rgb[k] = c ? c[k] : 1.f;
    static uint32_t h = 4242u;
    h = h * 1664525u + 1013904223u;
    m.seed = h;
    m.roll = (h >> 8) / 16777216.f * 6.2832f;
    if (a.marks.size() >= 48) a.marks.erase(a.marks.begin());
    a.marks.push_back(m);
}

static void drawMarks(App& a) {
    // a ripple in the surface (owner's reference, 2026-11-07: a shimmer in the glass, not a bright spot): as the ball
    // leaves, a thin faint ring in the thrower's colour spreads out, with hue-shifted rings just inside and outside it
    // (an iridescent edge); a faint haze stays where it hit, fading over 4 s, so that angle can be aimed for again
    const float soft = a.fxLevel >= 1 ? 0.7f : 1.f;
    for (const App::Mark& m : a.marks) {
        const float t = m.age / m.life;
        const Vec3 at = m.p + m.n * 0.002f;
        const float* c = m.rgb;
        const float in[3] = {0.5f * c[0] + 0.5f * c[2], 0.5f * c[1] + 0.5f * c[0], 0.5f * c[2] + 0.5f * c[1]};   // (the hue a little
        const float out[3] = {0.5f * c[0] + 0.5f * c[1], 0.5f * c[1] + 0.5f * c[2], 0.5f * c[2] + 0.5f * c[0]};  // either way round)
        const float haze = 0.10f * std::pow(1.f - t, 1.5f) * soft * std::min(1.f, m.age / 0.05f);
        const float hz[4] = {(0.5f + 0.5f * c[0]) * haze, (0.5f + 0.5f * c[1]) * haze, (0.5f + 0.5f * c[2]) * haze, 1};
        a.gfx.drawSpriteFlat(at, m.size * 0.55f, hz, m.n, 0);
        const float rt = m.age / 0.9f;   // the ripple: out to the mark's size in 0.9 s, fading as it goes
        if (rt < 1.f) {
            const float e = 1.f - rt * (2.f - rt);   // (eases out)
            const float k = (1.f - rt) * (1.f - rt) * 0.55f * soft;
            const float R = m.size * (0.25f + 0.85f * (1.f - e));
            const float main[4] = {c[0] * k, c[1] * k, c[2] * k, 1};
            const float ki = k * 0.45f;
            const float ci[4] = {in[0] * ki, in[1] * ki, in[2] * ki, 1}, co[4] = {out[0] * ki, out[1] * ki, out[2] * ki, 1};
            a.gfx.drawSpriteFlat(at, R, main, m.n, 1);
            a.gfx.drawSpriteFlat(at, R * 0.93f, ci, m.n, 1);
            a.gfx.drawSpriteFlat(at, R * 1.07f, co, m.n, 1);
        }
    }
}
static const float* teamRGB(const App& a, int t) {   // (a team's colour: the same as its players')
    return a.game.np == 4 && (a.game.tun.teamMode == TEAMS_FFA || a.game.octagon()) ? kFourRGB[t & 3] : a.sideRGB[t & 1];
}
// the arena the rules in play ask for (practice, our room, the room we joined, the match we watch); the lobby is classic
static int arenaFor(const App& a) {
    const Tunables& t = a.game.tun;
    if (!t.experimental || inLobby(a) || (a.spectating && !a.watchIp)) return ARENA_CLASSIC;
    if (t.expRoom == ROOM_BLITZ) return ARENA_OCTAGON;
    if (t.expRoom == ROOM_CORNERS) return t.players == 4 ? ARENA_FOURCORNERS : ARENA_CORNERS;
    return ARENA_DIAMOND;
}

// the fist bump start, face to face in the centre: in a room, and in practice against the bot (FIST BUMP FIRST)
static bool meetHere(const App& a) {
    return a.game.match == MatchState::Meet && (a.net.inRoom() || (a.net.mode() == Net::Mode::Offline && !inLobby(a)));
}

static void viewTarget(const App& a, int local, Vec3& pos, Vec3& fwd) {
    if (inLobby(a)) {
        pos = kLobbySpot;
        fwd = {0, 0, -1};
    } else if (a.net.localWaiting()) {
        pos = holdingPose(a, a.net.localSpot()).p;
        fwd = holdingFwd(a, a.net.localSpot());
    } else if (a.spectating) {
        if (a.watchIp) { const Pose st = spectatorSeat(a); pos = st.p; fwd = st.q.rotate({0, 0, -1}); }
        else { pos = {0, 0, 0}; fwd = {1, 0, 0}; }
    } else if (meetHere(a)) {   // face to face in the centre (fist bump start)
        pos = {local == 0 ? -a.game.tun.meetSpot : a.game.tun.meetSpot, 0, 0};
        fwd = local == 0 ? Vec3{1, 0, 0} : Vec3{-1, 0, 0};
    } else if (a.game.octagon() && !a.game.alive(local) && ((a.game.startMask >> local) & 1) &&
               a.game.match != MatchState::WaitingForPlayers) {   // FIREBALL BLITZ, knocked out: the dugout (owner, 2026-11-02)
        const Pose d = holdingPose(a, 4 + (local & 3));
        pos = d.p;
        fwd = d.q.rotate({0, 0, -1});
    } else {
        pos = a.game.tun.start[local];
        fwd = local < 2 && !a.game.octagon() ? (local == 0 ? Vec3{1, 0, 0} : Vec3{-1, 0, 0}) : a.game.facing(local);   // (each faces the centre)
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
// A player's move through dark (FIREBALL BLITZ: to the dugout, back, and the room closing in): the view darkens, you are
// moved, it lightens. Not the cross-fade above: its old picture is held on the screen, so for half a second the room
// turned with your head (a player in the dugout, 2026-11-14: "the room moved with his head")
static const float kBlinkOut = 0.12f, kBlinkIn = 0.30f;
static void startBlink(App& a, const Pose& target) {
    a.fadeTarget = target;
    a.fadeDark = true;
    a.fadeCapture = false;
    a.fadePending = false;
    a.fadeT = 0;
    a.darkOut = kBlinkOut;
    a.darkIn = 0;
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
    for (int p = 0; p < a.game.np; ++p) {
        const Ball& b = a.game.pl[p].ball;
        if (b.state == BallState::Flying || b.state == BallState::Returning) { out = b.pos; return true; }
    }
    for (int p = 0; p < a.game.np; ++p) if (a.game.pl[p].ball.state == BallState::Held) { out = a.game.pl[p].ball.pos; return true; }
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
    if (a.btnMap.leftSticks) std::swap(l, r);   // (BUTTONS: the left stick turns, the right moves)
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
    const controls::Map bm = btnMap(a);   // (BUTTONS: a button that talks doesn't work the camera; AUTO is on the board too)
    const bool X = getBool(a, a.buttonX) && bm.job[controls::S_X] != controls::J_TALK;
    const bool A = getBool(a, a.buttonA, a.handPath[1]) && !X && bm.job[controls::S_A] != controls::J_TALK;
    const bool Y = getBool(a, a.buttonY) && bm.job[controls::S_Y] != controls::J_TALK;
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
static void openMenu(App& a, const PlayerInput& in);
static const double kOwnFailSeconds = 30.0;   // the purchase not confirmed: the page, then the game closes
// a store build whose purchase Meta couldn't confirm (entitlement.h): out of any room, every page shut, a page that says
// why with QUIT GAME, and the game closes by itself after kOwnFailSeconds (Meta: "show an error message and quit")
static void ownershipFailed(App& a, const PlayerInput& in) {
    LOGE("store: the purchase couldn't be confirmed (%s)", entitlement::why());
    if (a.net.inRoom()) a.net.leave();
    if (a.builderOpen) closeBuilder(a);
    if (a.tuneOpen) closeTuning(a);
    a.titleOpen = a.settingsOpen = a.voicePage = a.ptPage = a.gfxPage = a.ctlPage = a.roomBallPage = a.btnPage = a.xpOpen = a.bugOpen = a.reportOpen = a.rulesOpen =
        a.helpOpen = a.movePlayersOpen = a.updateOpen = a.statsOpen = a.statsNoticeOpen = a.nameEdit = a.startTitle = false;
    a.ownFail = true;
    a.ownFailAt = nowSeconds();
    a.quitArmed = true;   // (QUIT GAME: one press)
    openMenu(a, in);
}

// KEEPING THE CENTRE: the origin changes whose time has come (their changeTime at or before this frame's)
constexpr float kSettleMove = 0.05f, kSettleTurn = 3.f;   // a change smaller than both is tracking settling (m, degrees)
static void applySpaceChanges(App& a, XrTime t) {
    while (!a.spaceChanges.empty() && a.spaceChanges.front().changeTime <= t) {
        const App::PendingSpaceChange c = a.spaceChanges.front();
        a.spaceChanges.pop_front();
        const XrPosef& d = c.delta;
        const float moved = std::sqrt(d.position.x * d.position.x + d.position.z * d.position.z);
        const float turned = 2.f * std::acos(std::min(1.f, std::fabs(d.orientation.w))) * 57.2958f;
        // (small: tracking settling, at most 0.9 cm across in every log the owner's headset has written. A Meta button
        // recenter made where you already nearly faced the headset's own forward moved it 20 cm and turned it under 10
        // degrees: under 25 cm and 10 degrees until 2026-11-17, it was taken for settling and the court stayed where it
        // was, off to the side, the owner facing the lobby's right-hand posters)
        if (c.poseValid && a.recentered && moved < kSettleMove && turned < kSettleTurn) {
            // small: tracking settling. Keep the court fixed across the room, but follow a new floor height
            // (compensating the height kept the court at the old, wrong floor)
            XrPosef dxz = d;
            dxz.position.y = 0;
            a.trackToWorld = a.trackToWorld * fromXr(dxz);
            LOGI("reference space shifted (%.3f, %.3f, %.3f, turned %.1f deg): compensated across, floor %+.3f m followed",
                 d.position.x, d.position.y, d.position.z, turned, d.position.y);
        } else if (c.poseValid && a.recentered) {
            // large: a system recenter (the Meta button, the headset back on): put them back on their spot, facing in
            LOGI("system recenter (moved %.2f m, turned %.0f deg): re-placing the player", moved, turned);
            a.recentered = false;
        } else {
            a.recentered = false;   // no valid delta: re-anchor on the next tracked frame
        }
    }
}

static PlayerInput readInput(App& a, XrTime t, int local) {
    applySpaceChanges(a, t);   // (KEEPING THE CENTRE: before the head is read against the origin)
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
    {   // BUTTONS (controls.h): each button this frame; each job below reads its own buttons
        using namespace controls;
        a.gripVal[0] = getFloat(a, a.pushToTalk, a.handPath[0]);   // (the grips as push to talk reads them: see pushToTalk)
        a.gripVal[1] = getFloat(a, a.pushToTalk, a.handPath[1]);
        bool* d = a.btnDown;
        d[S_LGRIP] = a.gripVal[0] > 0.6f;
        d[S_RGRIP] = a.gripVal[1] > 0.6f;
        d[S_A] = getBool(a, a.buttonA, a.handPath[1]);
        d[S_X] = getBool(a, a.buttonA, a.handPath[0]);
        d[S_B] = getBool(a, a.buttonB);
        d[S_Y] = getBool(a, a.buttonY);
        d[S_LSTICK] = getBool(a, a.thumbClick, a.handPath[0]);
        d[S_RSTICK] = getBool(a, a.thumbClick, a.handPath[1]);
    }
    const controls::Map bm = btnMap(a);
    bool rc = controls::held(bm, controls::J_RECENTER, a.ctlKind, a.btnDown);   // (B and the right stick's click as standard)
    {   // PLAYERS' NAMES on / off (the left stick's click as standard)
        const bool nc = controls::held(bm, controls::J_NAMES, a.ctlKind, a.btnDown);
        if (nc && !a.prevNamesClick) {
            a.showNames = !a.showNames;
            if (a.net.localWaiting())   // (waiting in a room: the queue screen comes and goes with them)
                a.net.noticeLocal(a.showNames ? "NAMES AND QUEUE SCREEN ON" : "NAMES AND QUEUE SCREEN OFF");
            else a.net.noticeLocal(a.showNames ? "PLAYERS' NAMES ON" : "PLAYERS' NAMES OFF");
        }
        a.prevNamesClick = nc;
    }
    if (local != g_lastLocal) { a.recentered = false; g_lastLocal = local; }
    {   // moving between playing / the hub / watching a match re-places the viewer
        uint32_t key = a.spectating ? (a.watchIp ? a.watchIp : 1u) : a.net.localWaiting() ? 10u + (uint32_t)a.net.localSpot()
                     : meetHere(a) ? 2u : inLobby(a) ? 3u : 0u;   // the centre meeting, then your end
        // (and when the room's shape changes: a player who joins waits by their own rules' room until the host's first
        // picture says FOUR CORNERS, whose waiting spots are elsewhere; testers 2026-10-30 saw them inside the room)
        key = key * 8u + (uint32_t)(a.game.arenaKind + 1);
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
        if (a.fadeDark) { a.fadeDark = false; a.darkIn = kBlinkIn; }
        else a.fadeT = 0.5f;
    }
    if (headOk && (!a.recentered || (rc && !a.prevRecenter))) {
        LOGI("recenter cause: %s", !a.recentered ? "initial/reference-space change" : "button (B / thumbstick click)");
        recenter(a, headTrack, local);
        a.replaceBuilder = true;   // (the builder, if open, placed again in front of you below)
    }
    const bool y = controls::held(bm, controls::J_PEEK, a.ctlKind, a.btnDown);   // (Y as standard)
    a.prevY = y;
    a.peekHeld = y && !a.spectating;   // PASSTHROUGH > PEEK: the room while PEEK's button is held
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
#ifndef PA_STORE
            // promo: SELFIE CAPTURE. touch selfie_capture_on or selfie_capture_off: the file's presence sets the state,
            // so a touch the game missed (closed, asleep) still does what it says at the next launch
            for (int on = 0; on < 2; ++on) {
                std::string cf = std::string(ext) + (on ? "/selfie_capture_on" : "/selfie_capture_off");
                if (!fileExists(cf)) continue;
                fileRemove(cf);
                a.hsCap = on != 0;
                LOGI("selfie capture %s%s", a.hsCap ? "on" : "off",
                     a.hsCap && !(a.headSelfie && a.spectating) ? " (nothing is saved until you spectate with SELFIE on)" : "");
            }
#endif
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
    if (a.replaceBuilder && headOk) {   // (recentered: the builder's mirror and panel in front of you again)
        a.replaceBuilder = false;
        if (a.builderOpen) { placeBuilder(a, in.head); LOGI("avatar builder: placed again (recentered)"); }
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
            // (CONTROLLER ANGLE: your own tilt, turn and twist on top of the standard angle; mirrored on the left hand)
            Quat tq = in.hand[h].grip.q * gripEuler({a.tunePitchDeg + a.ctlTilt, a.tuneYawDeg + a.ctlTurn, a.tuneRollDeg + a.ctlTwist}, h == 0);
            in.hand[h].grip = {tq, in.hand[h].grip.p + tq.rotate({(h == 0 ? -1.f : 1.f) * a.tuneX, a.tuneUp, -a.tuneFwd})};
        }
        if (vel.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT)
            in.hand[h].angVel = a.trackToWorld.q.rotate({vel.angularVelocity.x, vel.angularVelocity.y, vel.angularVelocity.z});
        in.hand[h].trigger = getFloat(a, a.trigger, a.handPath[h]);
        in.hand[h].squeeze = getFloat(a, a.squeeze, a.handPath[h]);
    }
    in.buttonA = controls::held(btnMap(a), controls::J_READY, a.ctlKind, a.btnDown);   // READY (A and X as standard)
    in.avatar = avatar::uniform(a.myAvatar);   // (with a country: the team kit, which older builds can show too)
    float pttGrip = 0;   // TALK's buttons (the left grip as standard: BUTTONS; the grips as suit each controller)
    {
        const controls::Map m = btnMap(a);
        for (int s = 0; s < controls::S_COUNT; ++s)
            if (m.job[s] == controls::J_TALK && controls::onController(a.ctlKind, s))
                pttGrip = std::max(pttGrip, s == controls::S_LGRIP ? a.gripVal[0] : s == controls::S_RGRIP ? a.gripVal[1]
                                            : a.btnDown[s] ? 1.f : 0.f);
    }
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
#ifdef __ANDROID__
    if (!a.ownFail && entitlement::poll() == entitlement::State::Failed) ownershipFailed(a, in);
#endif
    if (a.ownFail) {   // the page stays up; the game closes by itself
        a.menuOpen = true;
        if (!a.ownQuit && nowSeconds() - a.ownFailAt > kOwnFailSeconds) {
            a.ownQuit = true;
            LOGI("store: closing (the purchase wasn't confirmed)");
            saveSettings(a);
            if (a.session != XR_NULL_HANDLE) xrRequestExitSession(a.session); else a.quit = true;
        }
    }
    bool menu = getBool(a, a.menu);
    if (menu && !a.prevMenu && !a.ownFail) {
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
                     ReportSend, ReportBack, Forfeit, Experimental, XpBack, XpServer, XpConnect, Host, Leave, Rules, Close, Join, Spectate, Watch, Hub, Avatar, Name, Key, KeyDel, KeyDone, Help, HelpDone, Play, Multi, Settings, Back, VolDown, VolUp, Hand, Fx, Online, Server, ServerDefault, DeleteStats, ClearWarps, Queue, HideOutside, VoicePage, VoiceMode, MuteSpec, MutePlayers, MuteAll, Director, JoinRoom, BotFill, Tuning, Passthrough, PtMode, PtAdjust, PtMono, PtEdges, PtPreset, PtReveal, PtPeek, Crowd, Turn, OptShields, OptKnuckle, OptTime, OptGo, OptBack, OptStrike, OptFoul, ReportHostKick, OptBot, Record, OptMeet, GfxPage, FovDown, FovUp, OptRoom, OptTeams,
                     Train, TrainPick, TrainStart, TrainRetry, TrainNext, TrainList, TrainLeave, TrainBoard, TrainKind, TrainBoardBack, TrainShow, TrainVideo,
                     MovePlayers, MoveCorner, MoveOne, MoveBack, OptHealth, OptBalls, OptPlayers, Discord,
                     UpdateOpen, UpdateGo, UpdateRestart, UpdatePage, UpdateStore, UpdateBack, HeadSelfie, PrivateTalk, HandOff, HandOffTo,
                     LookAhead, Studio, StudioSection, StudioPrev, StudioNext, StudioPlay, StudioVote, StudioMoment, StudioLeave, StudioDelete,
                     Credits, CreditsBack, UpdateStoreGet, UpdateStoreInstall, Defaults, CtlPage, CtlAdj,
                     BallPage, BallCore, BallTrue, BallSpin, BallWeight, BallBack, BtnPage, BtnSlot, BtnSticks, BtnPreset,
                     RoomPick, OptAim, OptZone, OptGrowth, OptRounds, RoundsPad, PadKey, PadDel, PadOk, PadCancel, OptBotPlay, OptShieldSize };
// HOW TO PLAY > CREDITS: one page, film-credit style (small grey headings, the names under them)
enum { CR_HEAD, CR_NAME, CR_SMALL };
struct CreditRow { int kind; const char* text; };
static const CreditRow kCredits[] = {
    {CR_HEAD, "CREATED AND DIRECTED BY"},
    {CR_NAME, "JENNIFER DOEBELIN"},
    {CR_HEAD, "STUDIO"},
    {CR_NAME, "CROSSGENGAMING"},
    {CR_HEAD, "MADE WITH"},
    {CR_NAME, "CLAUDE"},
    {CR_HEAD, "COMMUNITY"},
    {CR_NAME, "ASTROBLEEM    R0GUEICE    THEINSANIAC"},
    {CR_NAME, "SPEKKIOCT    SKANDI"},
    {CR_HEAD, "THANK YOU"},
    {CR_NAME, "EVERY ALPHA TESTER"},
#ifdef PA_STORE
    {CR_SMALL, "OPENXR LOADER (KHRONOS GROUP)  -  ANDROID NATIVE APP GLUE  -  META PLATFORM SDK"},
#else
    {CR_SMALL, "OPENXR LOADER (KHRONOS GROUP)  -  ANDROID NATIVE APP GLUE"},
#endif
    {CR_SMALL, "LICENCES: NOTICE.TXT AT GITHUB.COM/JENDOEBELIN/PROJECTARENA-PUBLIC"},
};

struct MenuItem {
    std::string label;
    float x, y, w, h;
    MenuAct act;
    uint32_t ip;
    bool enabled;
};

static void haptic(App& a, int hand, float amp, float seconds);

// ---- MATCH OPTIONS: the page's choices <-> the rules, their labels, and whether a match is on
// DEFAULTS (0.3.19): each settings page back as a fresh install has it, that page only. Never: a person muted by
// name, the name, the avatar, the stats, the server (its own USE DEFAULT); MATCH OPTIONS keeps the bot's level, MATCH MY
// LEVEL and FIST BUMP FIRST (yours, not the rules') and which rules and room you picked
enum { DEF_SETTINGS, DEF_GRAPHICS, DEF_VOICE, DEF_PASSTHROUGH, DEF_MATCH, DEF_CONTROLLERS, DEF_BALL, DEF_BUTTONS };
static const char* defaultsLabel(const App& a, int page) {
    const bool armed = a.defaultsArmed == page && nowSeconds() < a.defaultsArmedUntil;
    return armed ? (page == DEF_MATCH ? "PRESS AGAIN" : "RESET? PRESS AGAIN") : "DEFAULTS";
}
static void applyDefaults(App& a, int page) {
    // (the values are App's own initial ones: a fresh install's)
    switch (page) {
    case DEF_SETTINGS:
        a.volume = 8; a.audio.setVolume(a.volume / 10.f);
        a.hideOutside = false;
        a.crowdMode = 0;
        a.turnMode = 0;
        if (!a.net.inherited()) { a.net.setBotFill(true); a.botFill = true; }   // (a room taken over keeps its own)
        break;
    case DEF_CONTROLLERS:   // (POINTER, and the standard angle)
        a.pointerHand = 1;
        a.ctlTilt = a.ctlTwist = a.ctlTurn = 0;
        break;
    case DEF_BUTTONS:   // (BUTTONS: STANDARD, the pointer in the right hand)
        a.btnMap = controls::standard();
        a.pointerHand = 1;
        break;
    case DEF_BALL:   // (the room's BALL FEEL: the standard ball, the owner's pick; nothing changes until START / APPLY)
        a.optBallFeel = BALL_FEEL_DEFAULT;
        break;
    case DEF_GRAPHICS:
        a.roomFeel = 0;
        a.fxLevel = 0;
        a.net.setLookAhead(true);   // (BALL LOOK-AHEAD: on, from 0.3.21)
        a.discordOn = true;
        break;
    case DEF_VOICE:   // (anyone muted by name stays muted)
        a.voiceMode = 0; a.micDenied = false;
        a.muteSpectators = a.mutePlayers = a.muteAll = false;
        a.net.setPrivateTalk(false);
        break;
    case DEF_PASSTHROUGH:
        a.ptMode = 0; a.ptFloor = 0.8f; a.ptGlass = 1.f; a.ptRoom = 1.f; a.ptMono = false; a.ptEdges = false;
        a.ptEdgeReveal = true; a.ptPeekOn = true;
        a.ptStyleDirty = true;
        break;
    case DEF_MATCH: {   // the rules' own: as a fresh Tunables has them (Basic 3 min, Advanced untimed rounds, shields on,
                        // knuckle guards as the rules say; FIREBALL BLITZ: its own, no shields, knuckle guards on)
        const Tunables t;
        a.optShields = t.shields; a.optKnuckle = t.knuckleMode;
        a.optMatchTime = t.basicMatchTime; a.optRoundTime = t.advancedRoundTime;
        a.optHealth = t.health; a.optBalls = t.fireBalls; a.optOctPlayers = t.octPlayers;
        a.optTeams = t.teamMode; a.optOneTurns = t.oneTurns;
        a.optStrikeSpeed = t.strikeSpeed;
        a.optFoulShift = t.foulShift;
        a.optMaiden = t.maiden; a.optAim = t.aimAssist; a.optZone = t.strikeZone; a.optGrowth = t.ballGrowth;   // (the rules')
        a.optRounds = t.advancedRoundsToWin;
        a.optShieldSize = t.shieldSize;
        a.optBallFeel = t.ballFeel;   // (BALL FEEL: the standard ball)
        if (a.optExperimental && a.optExpRoom == ROOM_BLITZ) { a.optShields = false; a.optKnuckle = 1; }
        break;
    }
    }
}

static const char* strikeLabel(const Tunables& t) {
    static const char* k[3] = {"STRIKES: REGULATION", "STRIKES: FAST", "STRIKES: FASTER"};
    return k[std::max(0, std::min(2, t.strikeSpeed))];
}

// FOUL LINE: REGULATION, LONGER 1 / 2 (toward the centre: a longer way to throw), SHORTER 1 / 2
static std::string foulLabel(const Tunables& t) {
    const int s = std::max(-2, std::min(2, t.foulShift));
    return s == 0 ? std::string("FOUL LINE: REGULATION") : std::string("FOUL LINE: ") + (s > 0 ? "LONGER " : "SHORTER ") + std::to_string(s > 0 ? s : -s);
}

static void optionsFrom(App& a, const Tunables& t) {
    a.optAdvanced = t.advanced; a.optShields = t.shields; a.optKnuckle = t.knuckleMode; a.optExperimental = t.experimental; a.optExpRoom = t.expRoom; a.optTeams = t.teamMode; a.optOneTurns = t.oneTurns;
    a.optHealth = t.health; a.optBalls = t.fireBalls; a.optOctPlayers = t.octPlayers;
    a.optMatchTime = t.basicMatchTime; a.optRoundTime = t.advancedRoundTime;
    a.optStrikeSpeed = t.strikeSpeed;
    a.optFoulShift = t.foulShift;
    a.optMaiden = t.maiden; a.optAim = t.aimAssist; a.optZone = t.strikeZone; a.optGrowth = t.ballGrowth;
    a.optRounds = t.advancedRoundsToWin;
    a.optBotPlay = t.botPlay;
    a.optShieldSize = t.shieldSize;
    a.optBallFeel = t.ballFeel & BALL_FEEL_ALL;
    a.optBotLevel = t.botLevel; a.optBotAdapt = t.botAdapt; a.optBotMeet = t.botMeet;
}
// ROOMS AND THEIR OPTIONS (the owner's design, 2026-11-17): a room is chosen by its shape, then set up with its own
// options. 0 the SQUARE (BASIC and ADVANCED), 1 DIAMOND, 2 FOUR CORNERS, 3 FIREBALL BLITZ (EXPERIMENTAL's rooms)
static int roomOf(const Tunables& t) { return t.experimental ? 1 + std::max(0, std::min(kRooms - 1, t.expRoom)) : 0; }
static const char* kRoomTitle[4] = {"SQUARE", "DIAMOND", "FOUR CORNERS", "FIREBALL BLITZ"};
// the room's page: its options' rows (two to a row), then the bot's level just under them
static int optionRows(const Tunables& t) { const int r = roomOf(t); return r <= 1 ? 6 : 3; }   // (with BALL FEEL)
static constexpr int kMaxBestOf = 9;   // ROUNDS: at most BEST OF 9 (the owner: a most we allow; Tunables: 5 rounds won)
// the number typed on BEST OF's pad, when it can be one: odd (no ties), 1 to kMaxBestOf; otherwise 0
static int padBestOf(const App& a) {
    if (a.roundsBuf.empty() || a.roundsBuf.size() > 2) return 0;
    const int n = std::atoi(a.roundsBuf.c_str());
    return n >= 1 && n <= kMaxBestOf && (n & 1) ? n : 0;
}
static float botRowY(const Tunables& t) { return 0.165f - 0.06f * (float)optionRows(t) - 0.005f; }
static const char* kAimName[4] = {"OFF", "LOW", "MEDIUM", "HIGH"};
// the room and its match as the boards and lists name them (the owner, 2026-11-17: no BASIC and ADVANCED on screen;
// the SQUARE's two ways to play are its match: a TIMED MATCH, or BEST OF 3 rounds, first to 4)
static std::string roomName(const Tunables& t) {
    const int r = roomOf(t);
    if (r > 1) return kRoomTitle[r];
    return std::string(kRoomTitle[r]) + (t.advanced ? " - BEST OF " + std::to_string(2 * t.roundsToWinNow() - 1) : std::string(" - TIMED MATCH"));
}
static const char* kZoneName[3] = {"SMALL", "REGULATION", "LARGE"};
static const char* kGrowthName[4] = {"NONE", "QUARTER", "HALF", "REGULATION"};
static const char* kShieldSizeName[5] = {"REGULATION", "+25%", "+50%", "+75%", "DOUBLE"};
// EXPERIMENTAL's rooms (0.3.12): DIAMOND (two players) and FOUR CORNERS (four: the CORNERS room as the owner asked,
// 2026-10-29; the two-player CORNERS is no longer offered); 0.3.13: FIREBALL BLITZ (up to four, fireballs, last one standing)
static const char* kBallsName[5] = {"", "ONE", "TWO", "THREE", "RAPID FIRE"};

static void optionsInto(const App& a, Tunables& t) {
    t.advanced = a.optAdvanced && (!a.optExperimental || a.optExpRoom == ROOM_DIAMOND); t.shields = a.optShields; t.knuckleMode = a.optKnuckle; t.experimental = a.optExperimental; t.expRoom = std::max(0, std::min(kRooms - 1, a.optExpRoom));
    t.players = t.experimental && fourPlayerRoom(t.expRoom) ? 4 : 2;
    t.health = a.optHealth; t.fireBalls = a.optBalls;
    if (!a.rulesForHost) t.octPlayers = a.optOctPlayers;   // (practice only: a hosted room plays every corner, Game::octCap)
    t.teamMode = std::max(0, std::min(2, a.optTeams));
    t.oneTurns = a.optOneTurns && a.rulesForHost;
    if (!a.rulesForHost && !a.net.inRoom()) t.theOne = 0;   // (practice 3 v 1: you are THE ONE; hosting, the room decides)
    t.basicMatchTime = a.optMatchTime; t.advancedRoundTime = a.optRoundTime;
    t.strikeSpeed = std::max(0, std::min(2, a.optStrikeSpeed));
    t.foulShift = std::max(-2, std::min(2, a.optFoulShift));
    t.maiden = a.optMaiden && a.optShields;
    t.aimAssist = std::max(-1, std::min(3, a.optAim));
    t.strikeZone = std::max(0, std::min(2, a.optZone));
    t.ballGrowth = std::max(0, std::min(3, a.optGrowth));
    t.advancedRoundsToWin = std::max(1, std::min(5, a.optRounds));
    t.botPlay = a.optBotPlay & 1;
    t.shieldSize = std::max(0, std::min(4, a.optShieldSize));
    t.ballFeel = a.optBallFeel & BALL_FEEL_ALL;
    t.botLevel = a.optBotLevel; t.botAdapt = a.optBotAdapt; t.botMeet = a.optBotMeet;
}
static std::string timeLabel(float secs, bool rounds) {
    if (secs <= 0.f) return rounds ? "OFF (NO LIMIT)" : "FREE PLAY (NO LIMIT)";
    char b[16];
    std::snprintf(b, sizeof b, "%d:%02d", (int)secs / 60, (int)secs % 60);
    return b;
}
// "SQUARE - BEST OF 3", "SQUARE - TIMED MATCH - HOTDOG - 5 MIN", "SQUARE - BEST OF 3 - 2 MIN ROUNDS - KNUCKLE GUARDS": the
// room, its match and what differs
static std::string rulesLine(const Tunables& t) {
    static const char* kTeams[3] = {"FREE-FOR-ALL", "2 V 2", "3 V 1"};
    std::string s = !t.experimental ? roomName(t) : (t.expRoom == ROOM_BLITZ ? std::string("FIREBALL BLITZ - HEALTH ") + std::to_string(t.health) +
                                                                                                   (t.fireBalls >= 4 ? " - RAPID FIRE" : t.fireBalls > 1 ? std::string(" - ") + kBallsName[t.fireBalls] + " BALLS" : "")
                                                                : t.expRoom == ROOM_CORNERS ? std::string("FOUR CORNERS - ") + kTeams[std::max(0, std::min(2, t.teamMode))]
                                                                : roomName(t));
    if (!t.shields) s += " - HOTDOG";
    if (t.advanced && t.advancedRoundTime > 0) s += " - " + std::to_string((int)t.advancedRoundTime / 60) + " MIN ROUNDS";
    if (!t.advanced && t.basicMatchTime <= 0) s += " - FREE PLAY";
    else if (!t.advanced && t.basicMatchTime != 180.f) s += " - " + std::to_string((int)t.basicMatchTime / 60) + " MIN";
    if (t.knuckleMode >= 0 && (t.knuckleMode != 0) != !t.advanced) s += t.knuckleMode ? " - KNUCKLE GUARDS" : " - NO KNUCKLE GUARDS";
    if (t.perStrike() != t.speedIncreasePerStrike) s += t.strikeSpeed >= 2 ? " - FASTER STRIKES" : " - FAST STRIKES";
    if (t.foulMove() != 0.f) s += " - " + foulLabel(t).substr(0, 9) + foulLabel(t).substr(10);   // (" - FOUL LINE LONGER 2")
    if (t.maidenNow()) s += " - MAIDEN";   // (ROOMS AND THEIR OPTIONS)
    if (t.aimAssistNow() != (t.advanced ? 1 : 2)) s += std::string(" - AIM ASSIST ") + kAimName[t.aimAssistNow()];
    if (t.strikeZoneNow() != 1) s += std::string(" - ") + kZoneName[t.strikeZoneNow()] + " STRIKE ZONE";
    if (t.ballGrowthNow() != 3) s += std::string(" - BALL GROWTH ") + kGrowthName[t.ballGrowthNow()];
    if (t.shieldSizeNow() != 0) s += std::string(" - SHIELDS ") + kShieldSizeName[t.shieldSizeNow()];
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
    if (a.guestRules && !a.net.inherited()) optionsInto(a, a.ownRules);   // (a guest choosing practice rules: ours, kept for after the room)
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
    if (a.titleOpen || a.xpOpen || a.trainOpen || a.trainBoard >= -1 || (a.trainer.active() && !otherPage(a))) return NAV_HOME;
    if (inLobby(a) && !a.menuOpen) return NAV_LOBBY;
    if (a.rulesOpen) return a.rulesForHost ? NAV_MULTI : NAV_PRACTICE;
    if (a.movePlayersOpen) return NAV_MULTI;
    if (a.updateOpen) return NAV_HOME;
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
    if (!a.nameEdit && !trainSide(a) && !a.ownFail) {   // the side menu: every area one click away
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
    if (a.ownFail) {   // a store build, the purchase not confirmed: only QUIT GAME
        v.push_back({"QUIT GAME", 0, -0.33f, 0.34f, 0.07f, MenuAct::Quit, 0, true});
        return v;
    }
    if (a.statsNoticeOpen && !a.nameEdit) {   // the one-time notice before the first online match
        v.push_back({"OK", 0, -0.33f, 0.30f, 0.07f, MenuAct::StatsNoticeOk, 0, true});
        return v;
    }
    if (a.trainBoard >= -1 && !a.nameEdit) {   // a TRAINING leaderboard
        const bool timed = a.trainBoard >= 0 && training::lessons()[a.trainBoard].timed();
        if (timed) {
            v.push_back({"FASTEST", -0.19f, -0.40f, 0.34f, 0.06f, MenuAct::TrainKind, 0, a.trainBoardKind != 0});
            v.push_back({"MOST ACCURATE", 0.19f, -0.40f, 0.34f, 0.06f, MenuAct::TrainKind, 1, a.trainBoardKind != 1});
        }
        v.push_back({"BACK", 0, -0.48f, 0.30f, 0.06f, MenuAct::TrainBoardBack, 0, true});
        return v;
    }
    if (a.trainOpen && !a.nameEdit) {   // the lessons, in two columns; each open once the one before is passed
        const auto& Ls = training::lessons();
        for (int i = 0; i < (int)Ls.size(); ++i) {   // (a column a track)
            char lb[48];
            std::snprintf(lb, sizeof lb, "%d  %s", training::placeInTrack(i), Ls[i].name);
            v.push_back({lb, trainColX(Ls[i].track), trainRowY(training::placeInTrack(i) - 1), 0.30f, 0.05f, MenuAct::TrainPick, (uint32_t)i,
                         a.trainBests.unlocked(i) && a.net.mode() == Net::Mode::Offline});
        }
        v.push_back({"LEADERBOARD: OVERALL", -0.20f, -0.46f, 0.38f, 0.055f, MenuAct::TrainBoard, 0xFFFFFFFFu, true});
        v.push_back({"MAIN MENU", 0.20f, -0.46f, 0.38f, 0.055f, MenuAct::TrainLeave, 0, true});
        return v;
    }
    if (a.studioOn && !a.nameEdit && !otherPage(a)) {   // SOUND STUDIO: the sections, each event's choice, PLAY and the votes
        const auto& S = studio::sections();
        for (int i = 0; i < (int)S.size(); ++i)
            v.push_back({S[i].name, -0.3125f + 0.125f * i, 0.205f, 0.12f, 0.05f, MenuAct::StudioSection, (uint32_t)i, true});   // (the open one lit)
        const studio::Section& sec = S[a.studioSection];
        for (int k = 0; k < (int)sec.events.size(); ++k) {
            const float y = studioRowY(k);
            v.push_back({"<", -0.13f, y, 0.045f, 0.045f, MenuAct::StudioPrev, (uint32_t)k, true});
            v.push_back({">", 0.15f, y, 0.045f, 0.045f, MenuAct::StudioNext, (uint32_t)k, true});
            v.push_back({"PLAY", 0.22f, y, 0.075f, 0.045f, MenuAct::StudioPlay, (uint32_t)k, true});
            const int s = studioChosen(a, sec.events[k]);
            if (s >= 0 && !a.studioSounds[s].official && !a.studioSounds[s].mine) {   // a library sound: your vote, lit
                v.push_back({"UP", 0.29f, y, 0.055f, 0.045f, MenuAct::StudioVote, (uint32_t)k | 0x100u, true});
                v.push_back({"DOWN", 0.35f, y, 0.06f, 0.045f, MenuAct::StudioVote, (uint32_t)k, true});
            } else if (s >= 0 && a.studioSounds[s].mine) {   // your own file: DELETE (press twice), to free the headset's space
                const bool armed = a.studioDelArmed == a.studioSounds[s].file && nowSeconds() < a.studioDelUntil;
                v.push_back({armed ? "SURE?" : "DELETE", 0.32f, y, 0.115f, 0.045f, MenuAct::StudioDelete, (uint32_t)k, true});
            }
        }
        v.push_back({"HIDE THIS PAGE", -0.19f, -0.455f, 0.36f, 0.055f, MenuAct::StudioMoment, 0, true});
        v.push_back({"MAIN MENU", 0.19f, -0.455f, 0.36f, 0.055f, MenuAct::StudioLeave, 0, true});
        return v;
    }
    if (a.trainer.active() && !a.nameEdit && !otherPage(a)) {   // in a lesson: its card, its result, or (the menu button) a pause
        using training::Phase;
        const int i = a.trainer.lesson;
        const bool last = training::nextInTrack(i) < 0;
        if (a.trainer.phase == Phase::Card) {
            v.push_back({"START", 0.19f, -0.25f, 0.36f, 0.08f, MenuAct::TrainStart, 0, true});
            v.push_back({!a.film.have ? "VIDEO COMING SOON" : a.trainVideo ? "HIDE THE VIDEO" : "SHOW ME HOW", -0.19f, -0.25f, 0.36f, 0.08f,
                         MenuAct::TrainVideo, 0, a.film.have});
            v.push_back({"TRAINING MENU", -0.19f, -0.355f, 0.36f, 0.06f, MenuAct::TrainList, 0, true});
            v.push_back({"LEADERBOARD", 0.19f, -0.355f, 0.36f, 0.06f, MenuAct::TrainBoard, (uint32_t)i, true});
            v.push_back({"MAIN MENU", 0, -0.445f, 0.36f, 0.06f, MenuAct::TrainLeave, 0, true});
        } else if (a.trainer.phase == Phase::Done) {
            const bool next = a.trainer.result.passed && !last;
            v.push_back({next ? "NEXT LESSON" : "TRAINING MENU", 0.19f, -0.29f, 0.36f, 0.08f, next ? MenuAct::TrainNext : MenuAct::TrainList, 0, true});
            v.push_back({"TRY AGAIN", -0.19f, -0.29f, 0.36f, 0.08f, MenuAct::TrainRetry, 0, true});
            v.push_back({"LEADERBOARD", -0.19f, -0.39f, 0.36f, 0.06f, MenuAct::TrainBoard, (uint32_t)i, true});
            v.push_back({"TRAINING MENU", 0.19f, -0.39f, 0.36f, 0.06f, MenuAct::TrainList, 0, next});
            v.push_back({"MAIN MENU", 0, -0.47f, 0.36f, 0.06f, MenuAct::TrainLeave, 0, true});
        } else {
            v.push_back({"RESUME", 0, 0.10f, 0.56f, 0.08f, MenuAct::Close, 0, true});
            v.push_back({a.film.have ? "SHOW ME HOW" : "VIDEO COMING SOON", 0, 0.00f, 0.56f, 0.08f, MenuAct::TrainShow, 0, a.film.have});
            v.push_back({"RESTART THE LESSON", 0, -0.10f, 0.56f, 0.08f, MenuAct::TrainRetry, 0, true});
            v.push_back({"TRAINING MENU", 0, -0.20f, 0.56f, 0.08f, MenuAct::TrainList, 0, true});
            v.push_back({"MAIN MENU", 0, -0.30f, 0.56f, 0.08f, MenuAct::TrainLeave, 0, true});
        }
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
    if (a.updateOpen && !a.nameEdit) {   // UPDATE NOW: what this copy can do
        std::string why;
        const int how = updateHow(why);
#ifdef __ANDROID__
        using U = entitlement::Update;
        const U us = entitlement::updateState();
        if (how == 0 && us != U::Unavailable) {   // a store copy (0.3.18): the update downloads and installs right here
            char lb[48];
            if (us == U::Available) {
                if (entitlement::updateSizeMB() > 0.5) std::snprintf(lb, sizeof lb, "DOWNLOAD UPDATE (%.0f MB)", entitlement::updateSizeMB());
                else std::snprintf(lb, sizeof lb, "DOWNLOAD UPDATE");
                v.push_back({lb, 0, 0.04f, 0.56f, 0.08f, MenuAct::UpdateStoreGet, 0, true});
            } else if (us == U::Downloading) v.push_back({"DOWNLOADING...", 0, 0.04f, 0.56f, 0.08f, MenuAct::None, 0, false});
            else if (us == U::Downloaded) v.push_back({"INSTALL AND RESTART", 0, 0.04f, 0.56f, 0.08f, MenuAct::UpdateStoreInstall, 0, true});
            else if (us == U::Installing) v.push_back({"INSTALLING...", 0, 0.04f, 0.56f, 0.08f, MenuAct::None, 0, false});
            if (us == U::UpToDate || us == U::Failed)
                v.push_back({"OPEN THE STORE PAGE", 0, -0.06f, 0.56f, 0.06f, MenuAct::UpdateStore, 0, true});
        } else {   // the store page for everyone (a sideloaded copy can switch to the store's), GitHub's for sideloaded ones
            v.push_back({"OPEN THE STORE PAGE", 0, how == 0 ? 0.0f : 0.06f, 0.56f, 0.08f, MenuAct::UpdateStore, 0, true});
            if (how != 0) v.push_back({"OPEN THE GITHUB PAGE", 0, -0.04f, 0.56f, 0.08f, MenuAct::UpdatePage, 0, true});
        }
#else
        const updater::State st = updater::state();
        if (how == 0 && st == updater::READY) v.push_back({"RESTART NOW", 0, 0.04f, 0.56f, 0.08f, MenuAct::UpdateRestart, 0, true});
        else if (how == 0) v.push_back({"DOWNLOAD AND RESTART", 0, 0.04f, 0.56f, 0.08f, MenuAct::UpdateGo, 0,
                                        st == updater::IDLE || st == updater::FAILED});
        if (how != 1) v.push_back({"OPEN THE DOWNLOAD PAGE", 0, -0.06f, 0.56f, 0.06f, MenuAct::UpdatePage, 0, true});
#endif
        v.push_back({"BACK", 0, -0.30f, 0.36f, 0.07f, MenuAct::UpdateBack, 0, true});
        return v;
    }
    if (a.movePlayersOpen && a.moveHost && !a.nameEdit) {   // HAND OFF HOST: everyone in the room, a button each
        int i = 0;
        for (const Net::Person& p : a.net.peopleHere()) {
            if (p.role == Net::VOICE_SPECTATOR || i >= 8) continue;
            v.push_back({std::string("MAKE ") + p.name + " HOST", (i % 2) ? 0.21f : -0.21f, 0.15f - 0.085f * (float)(i / 2), 0.40f, 0.07f,
                         MenuAct::HandOffTo, p.id, a.net.mode() == Net::Mode::Host});
            ++i;
        }
        v.push_back({"BACK", 0, -0.30f, 0.36f, 0.08f, MenuAct::MoveBack, 0, true});
        return v;
    }
    if (a.movePlayersOpen && !a.nameEdit) {   // MOVE PLAYERS: a row a corner (pick two to swap them), and THE ONE
        const Game& g = a.game;
        const RoomInfo& r = a.net.room();
        const bool one = g.tun.teamMode == TEAMS_3V1, host = a.net.mode() == Net::Mode::Host && g.np == 4;
        static const char* kColour[4] = {"BLUE", "ORANGE", "GREEN", "VIOLET"};
        for (int c = 0; c < 4; ++c) {
            const char* who = r.slot[c] ? r.nameOf(r.slot[c]) : nullptr;
            std::string label = std::to_string(c + 1) + "  " + (r.slot[c] ? (who && who[0] ? who : "A PLAYER") : r.isBot(c) || g.pl[c].isBot ? "BOT" : "EMPTY");
            if (g.tun.teamMode == TEAMS_2V2) label += c < 2 ? "  - BLUE TEAM" : "  - ORANGE TEAM";
            else if (one) label += c == g.tun.theOne ? "  - THE ONE" : "  - THE THREE";
            else label += std::string("  - ") + kColour[c];
            v.push_back({label, one ? -0.10f : 0, 0.15f - 0.085f * c, one ? 0.54f : 0.74f, 0.07f, MenuAct::MoveCorner, (uint32_t)c, host});
            if (one) v.push_back({"MAKE THE ONE", 0.28f, 0.15f - 0.085f * c, 0.18f, 0.07f, MenuAct::MoveOne, (uint32_t)c, host && c != g.tun.theOne});
        }
        v.push_back({"BACK", 0, -0.30f, 0.36f, 0.08f, MenuAct::MoveBack, 0, true});
        return v;
    }
    if (a.rulesOpen && a.rulesStep == 1 && a.roomBallPage && !a.nameEdit) {   // the room's BALL FEEL: its four switches
        const int f = a.optBallFeel;
        v.push_back({f & BALL_CORE ? "CORE: SOLID" : "CORE: GLOW", -0.19f, 0.10f, 0.36f, 0.075f, MenuAct::BallCore, 0, true});
        v.push_back({f & BALL_TRUE ? "GLOW: TRUE SIZE" : "GLOW: WIDE", 0.19f, 0.10f, 0.36f, 0.075f, MenuAct::BallTrue, 0, true});
        v.push_back({f & BALL_SPIN ? "SPIN: VISIBLE" : "SPIN: SUBTLE", -0.19f, 0.00f, 0.36f, 0.075f, MenuAct::BallSpin, 0, true});
        v.push_back({f & BALL_WEIGHT ? "WEIGHT: HEAVY" : "WEIGHT: LIGHT", 0.19f, 0.00f, 0.36f, 0.075f, MenuAct::BallWeight, 0, true});
        v.push_back({"BACK", -0.19f, -0.335f, 0.36f, 0.065f, MenuAct::BallBack, 0, true});
        v.push_back({f == BALL_FEEL_DEFAULT ? std::string("STANDARD BALL") : std::string("BACK TO STANDARD"), 0.19f, -0.335f, 0.36f, 0.065f,
                     MenuAct::Defaults, DEF_BALL, f != BALL_FEEL_DEFAULT});
        return v;
    }
    if (a.rulesOpen && a.rulesStep == 1 && a.roundsPad && !a.nameEdit) {   // BEST OF n typed: digits only, odd, 1 to kMaxBestOf
        static const char kKeys[] = "123456789";
        for (int k = 0; k < 9; ++k)
            v.push_back({std::string(1, kKeys[k]), -0.12f + 0.12f * (float)(k % 3), 0.085f - 0.08f * (float)(k / 3), 0.10f, 0.068f,
                         MenuAct::PadKey, (uint32_t)kKeys[k], true});
        v.push_back({"DEL", -0.12f, -0.155f, 0.10f, 0.068f, MenuAct::PadDel, 0, !a.roundsBuf.empty()});
        v.push_back({"0", 0.f, -0.155f, 0.10f, 0.068f, MenuAct::PadKey, (uint32_t)'0', !a.roundsBuf.empty()});
        const int n = padBestOf(a);
        v.push_back({"BACK", -0.19f, -0.335f, 0.36f, 0.065f, MenuAct::PadCancel, 0, true});
        v.push_back({n > 0 ? "SET BEST OF " + std::to_string(n) : std::string("SET"), 0.19f, -0.335f, 0.36f, 0.065f, MenuAct::PadOk, 0, n > 0});
        return v;
    }
    if (a.rulesOpen && a.rulesStep == 1 && !a.nameEdit) {   // THE ROOM'S OPTIONS (ROOMS AND THEIR OPTIONS)
        Tunables t;
        optionsInto(a, t);
        // practice, and a room where the bot fills in (owner, 2026-10-29: FOUR CORNERS' bots): the bot's level too
        const bool bot = !a.rulesForHost || a.net.botFill();   // (the room's: a room taken over keeps its own)
        const int room = roomOf(t);
        const bool two = room <= 1;   // (the SQUARE and DIAMOND: every option; the four-player rooms theirs for now)
        // the options two to a row, top down (the SQUARE: BASIC and ADVANCED first)
        int n = 0;
        auto put = [&](const std::string& label, MenuAct act, uint32_t ip = 0) {
            v.push_back({label, n % 2 ? 0.19f : -0.19f, 0.165f - 0.06f * (n / 2), 0.36f, 0.052f, act, ip, true});
            ++n;
        };
        auto putTime = [&]() {   // the clock in half a row: - TIME +
            const float x = n % 2 ? 0.19f : -0.19f, y = 0.165f - 0.06f * (n / 2);
            const bool rounds = t.advanced && room <= 1;
            const float secs = rounds ? t.advancedRoundTime : t.basicMatchTime;
            char b[16];
            std::snprintf(b, sizeof b, "%d:%02d", (int)secs / 60, (int)secs % 60);
            const std::string l = std::string(rounds ? "ROUND: " : "TIME: ") + (secs > 0.f ? b : rounds ? "NO LIMIT" : "FREE PLAY");
            v.push_back({"-", x - 0.155f, y, 0.05f, 0.052f, MenuAct::OptTime, 0, true});
            v.push_back({l, x, y, 0.24f, 0.052f, MenuAct::None, 0, true});
            v.push_back({"+", x + 0.155f, y, 0.05f, 0.052f, MenuAct::OptTime, 1, true});
            ++n;
        };
        const std::string knuckles = t.knuckleGuards() ? "KNUCKLES: ON" : "KNUCKLES: OFF";
        const std::string shields = !t.shields ? "SHIELDS: OFF (HOTDOG)" : t.maiden && two ? "SHIELDS: ALWAYS (MAIDEN)" : "SHIELDS: ON";
        if (room <= 1) {   // the SQUARE and DIAMOND: the match, a TIMED MATCH or ROUNDS (BEST OF n: - / +, or typed), then all
            const int bo = 2 * t.roundsToWinNow() - 1;
            v.push_back({"TIMED MATCH", -0.28f, 0.165f, 0.18f, 0.052f, MenuAct::RulesPick, 0, true});
            v.push_back({"ROUNDS", -0.09f, 0.165f, 0.18f, 0.052f, MenuAct::RulesPick, 1, true});
            v.push_back({"-", 0.04f, 0.165f, 0.05f, 0.052f, MenuAct::OptRounds, 0, t.advanced && bo > 1});
            v.push_back({"BEST OF " + std::to_string(bo), 0.19f, 0.165f, 0.24f, 0.052f, MenuAct::RoundsPad, 0, t.advanced});
            v.push_back({"+", 0.34f, 0.165f, 0.05f, 0.052f, MenuAct::OptRounds, 1, t.advanced && bo < kMaxBestOf});
            n = 2;
        }
        if (room == 3) {   // FIREBALL BLITZ: KNUCKLES, HEALTH, BALLS, the clock (no shields, no strikes)
            put(knuckles, MenuAct::OptKnuckle);
            put(std::string("HEALTH: ") + std::to_string(t.health), MenuAct::OptHealth);
            put(std::string("BALLS: ") + kBallsName[std::max(1, std::min(4, t.fireBalls))], MenuAct::OptBalls);
            putTime();
        } else if (room == 2) {   // FOUR CORNERS: the teams (3 v 1: you are THE ONE; hosting, it can take turns instead)
            static const char* kTeams[3] = {"TEAMS: FREE-FOR-ALL", "TEAMS: 2 V 2", "3 V 1: YOU ARE THE ONE"};
            put(shields, MenuAct::OptShields);
            put(knuckles, MenuAct::OptKnuckle);
            put(t.teamMode == TEAMS_3V1 && t.oneTurns ? "3 V 1: ONE TAKES TURNS" : kTeams[std::max(0, std::min(2, t.teamMode))], MenuAct::OptTeams);
            putTime();
        } else {   // the SQUARE and DIAMOND
            put(shields, MenuAct::OptShields);
            put(std::string("SHIELD SIZE: ") + kShieldSizeName[t.shieldSizeNow()], MenuAct::OptShieldSize);
            put(knuckles, MenuAct::OptKnuckle);
            put(std::string("AIM ASSIST: ") + kAimName[t.aimAssistNow()], MenuAct::OptAim);
            put(std::string("STRIKE ZONE: ") + kZoneName[t.strikeZoneNow()], MenuAct::OptZone);
            put(strikeLabel(t), MenuAct::OptStrike);
            put(std::string("BALL GROWTH: ") + kGrowthName[t.ballGrowthNow()], MenuAct::OptGrowth);
            if (room == 0) put(foulLabel(t), MenuAct::OptFoul);   // (DIAMOND's foul zone is its platform)
            putTime();
        }
        // BALL FEEL, every room's (the owner, 2026-11-17): how the ball looks and feels for everyone at the arena
        put((t.ballFeel & BALL_FEEL_ALL) == BALL_FEEL_DEFAULT ? "BALL FEEL: STANDARD" : "BALL FEEL: CUSTOM", MenuAct::BallPage);
        if (bot) {   // - / +, the slider (one step a level, lit up to the one chosen), and MATCH MY LEVEL
            const std::string bl = t.botAdapt ? std::string("BOT: MATCH MY LEVEL (NOW ") + botSkillAt(a.game.tun.botMatch).name + ")"
                                              : std::string("BOT: ") + botSkill(t.botLevel).name;
            const float yb = botRowY(t);
            if (room <= 1) {   // (the SQUARE, DIAMOND: its style beside its level, any match: the owner, 11-17)
                v.push_back({"-", -0.345f, yb, 0.05f, 0.058f, MenuAct::OptBot, 0, t.botAdapt || t.botLevel > 0});
                v.push_back({bl, -0.155f, yb, 0.32f, 0.058f, MenuAct::None, 0, true});
                v.push_back({"+", 0.035f, yb, 0.05f, 0.058f, MenuAct::OptBot, 1, t.botAdapt || t.botLevel < kBotLevels - 1});
                v.push_back({t.botPlay ? "STYLE: DUELLIST" : "STYLE: BRAWLER", 0.235f, yb, 0.27f, 0.058f, MenuAct::OptBotPlay, 0, true});
            } else {
                v.push_back({"-", -0.335f, yb, 0.07f, 0.058f, MenuAct::OptBot, 0, t.botAdapt || t.botLevel > 0});
                v.push_back({bl, 0, yb, 0.56f, 0.058f, MenuAct::None, 0, true});
                v.push_back({"+", 0.335f, yb, 0.07f, 0.058f, MenuAct::OptBot, 1, t.botAdapt || t.botLevel < kBotLevels - 1});
            }
            for (int l = 0; l < kBotLevels; ++l)
                v.push_back({"", -0.33f + 0.07f * l, yb - 0.05f, 0.062f, 0.03f, MenuAct::OptBot, (uint32_t)(10 + l), true});
            v.push_back({"MATCH MY LEVEL", 0.225f, yb - 0.05f, 0.29f, 0.03f, MenuAct::OptBot, 2, true});
            if (!a.rulesForHost && room == 3)   // (FIREBALL BLITZ practice: how many play, you and bots)
                v.push_back({std::string("PLAYERS: ") + std::to_string(std::max(2, std::min(4, a.optOctPlayers))), 0, -0.415f, 0.46f, 0.038f, MenuAct::OptPlayers, 0, true});
            else if (!a.rulesForHost) v.push_back({t.botMeet ? "FIST BUMP FIRST: ON" : "FIST BUMP FIRST: OFF", 0, -0.415f, 0.46f, 0.038f, MenuAct::OptMeet, 0, true});
        }
        if (a.rulesEdit && a.rulesForHost && a.net.mode() == Net::Mode::Host && a.game.np == 4)   // (who plays in which corner)
            v.push_back({"MOVE PLAYERS", 0, -0.415f, 0.46f, 0.038f, MenuAct::MovePlayers, 0, true});
        const char* go = a.rulesEdit ? (rulesLive(a) ? "APPLY FROM THE NEXT MATCH" : "APPLY") : a.rulesForHost ? "OPEN MY ARENA" : "START PRACTICE";
        v.push_back({go, 0.19f, -0.36f, 0.36f, 0.06f, MenuAct::OptGo, 0, true});
        v.push_back({"BACK", -0.19f, -0.36f, 0.36f, 0.06f, MenuAct::OptBack, 0, true});
        // (beside the title: the rules' own settings; the bot's level stays as you chose it)
        v.push_back({defaultsLabel(a, DEF_MATCH), 0.355f, 0.29f, 0.15f, 0.05f, MenuAct::Defaults, DEF_MATCH, true});
        return v;
    }
    if (a.rulesOpen && !a.nameEdit) {   // CHOOSE A ROOM: each room's picture (drawMenu draws them on these)
        for (int k = 0; k < 4; ++k)
            v.push_back({"", k % 2 ? 0.205f : -0.205f, k < 2 ? 0.07f : -0.195f, 0.39f, 0.25f, MenuAct::RoomPick, (uint32_t)k, true});
        v.push_back({"BACK", 0, -0.375f, 0.36f, 0.055f, MenuAct::RulesBack, 0, true});
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
    if (a.helpOpen && a.creditsOpen) {   // HOW TO PLAY > CREDITS
        v.push_back({"BACK", 0, -0.33f, 0.30f, 0.07f, MenuAct::CreditsBack, 0, true});
        return v;
    }
    if (a.helpOpen) {
        v.push_back({"GOT IT", -0.20f, -0.33f, 0.30f, 0.07f, MenuAct::HelpDone, 0, true});
        v.push_back({"CREDITS", 0.20f, -0.33f, 0.30f, 0.07f, MenuAct::Credits, 0, true});
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
        const int ps = controls::firstSlot(btnMap(a), controls::J_PEEK, a.ctlKind);   // (PEEK's button: BUTTONS)
        if (ps < 0)
            v.push_back({"PEEK: NO BUTTON", 0.19f, -0.29f, 0.36f, 0.06f, MenuAct::None, 0, false});
        else
            v.push_back({std::string("PEEK (HOLD ") + controls::slotName(a.ctlKind, ps) + (a.ptPeekOn ? "): ON" : "): OFF"),
                         0.19f, -0.29f, 0.36f, 0.06f, MenuAct::PtPeek, 0, true});
        v.push_back({"BACK", -0.19f, -0.375f, 0.36f, 0.06f, MenuAct::Back, 0, true});
        v.push_back({defaultsLabel(a, DEF_PASSTHROUGH), 0.19f, -0.375f, 0.36f, 0.06f, MenuAct::Defaults, DEF_PASSTHROUGH, true});
        return v;
    }
    if (a.settingsOpen && a.btnPage) {   // BUTTONS (controls.h): each button's job, the left hand's on the left
        using namespace controls;
        const Map m = btnMap(a);
        static const int col[2][4] = {{S_LGRIP, S_X, S_Y, S_LSTICK}, {S_RGRIP, S_A, S_B, S_RSTICK}};
        for (int c = 0; c < 2; ++c)
            for (int r = 0; r < 4; ++r) {
                const int s = col[c][r];
                if (!onController(a.ctlKind, s)) continue;   // (one these controllers haven't: not shown)
                v.push_back({std::string(slotName(a.ctlKind, s)) + ": " + jobName(m.job[s]), c ? 0.19f : -0.19f, 0.15f - 0.08f * r,
                             0.36f, 0.065f, MenuAct::BtnSlot, (uint32_t)s, true});
            }
        v.push_back({m.leftSticks ? "SPECTATING: THE LEFT STICK TURNS" : "SPECTATING: THE RIGHT STICK TURNS", 0, -0.175f, 0.74f, 0.06f,
                     MenuAct::BtnSticks, 0, true});
        v.push_back({"STANDARD", -0.19f, -0.255f, 0.36f, 0.065f, MenuAct::BtnPreset, 0, true});
        v.push_back({"LEFT-HANDED", 0.19f, -0.255f, 0.36f, 0.065f, MenuAct::BtnPreset, 1, true});
        v.push_back({"BACK", -0.19f, -0.385f, 0.36f, 0.055f, MenuAct::Back, 0, true});
        v.push_back({defaultsLabel(a, DEF_BUTTONS), 0.19f, -0.385f, 0.36f, 0.055f, MenuAct::Defaults, DEF_BUTTONS, true});
        return v;
    }
    if (a.settingsOpen && a.gfxPage) {   // GRAPHICS: field of view (room size feel) and hit effects; the arena around it
        char b[48];
        snprintf(b, sizeof b, a.roomFeel == 0 ? "NORMAL" : "%+d%%", a.roomFeel * 10);   // (10% a step, +-50%: owner 2026-10-30)
        v.push_back({b, 0, -0.04f, 0.20f, 0.075f, MenuAct::None, 0, true});
        v.push_back({"-", -0.17f, -0.04f, 0.09f, 0.075f, MenuAct::FovDown, 0, a.roomFeel > -5});
        v.push_back({"+", 0.17f, -0.04f, 0.09f, 0.075f, MenuAct::FovUp, 0, a.roomFeel < 5});
        static const char* fx[3] = {"HIT EFFECTS: FULL", "HIT EFFECTS: SOFT", "HIT EFFECTS: LOW"};
#ifdef _WIN32
        v.push_back({fx[a.fxLevel % 3], -0.19f, -0.22f, 0.36f, 0.07f, MenuAct::Fx, 0, true});
        v.push_back({a.discordOn ? "DISCORD STATUS: ON" : "DISCORD STATUS: OFF", 0.19f, -0.22f, 0.36f, 0.07f, MenuAct::Discord, 0, true});
#else
        v.push_back({fx[a.fxLevel % 3], 0, -0.22f, 0.40f, 0.07f, MenuAct::Fx, 0, true});
#endif
        // BALL LOOK-AHEAD (0.3.15): playing in someone else's room, the balls where the host will have them
        // (two-player rooms only: in four-player rooms and FIREBALL BLITZ it stands down, Game::lookAhead)
        v.push_back({a.net.lookAhead() ? "BALL LOOK-AHEAD (1-V-1 ONLINE): ON" : "BALL LOOK-AHEAD (1-V-1 ONLINE): OFF", 0, -0.30f, 0.62f, 0.06f,
                     MenuAct::LookAhead, 0, true});
        v.push_back({"BACK", -0.19f, -0.39f, 0.36f, 0.055f, MenuAct::Back, 0, true});
        v.push_back({defaultsLabel(a, DEF_GRAPHICS), 0.19f, -0.39f, 0.36f, 0.055f, MenuAct::Defaults, DEF_GRAPHICS, true});
        return v;
    }
    if (a.settingsOpen && a.ctlPage) {   // CONTROLLERS: POINTER, and CONTROLLER ANGLE (your hands follow it as you set it)
        v.push_back({a.pointerHand == 1 ? "POINTER: RIGHT" : "POINTER: LEFT", -0.19f, 0.15f, 0.36f, 0.07f, MenuAct::Hand, 0, true});
        v.push_back({"BUTTONS", 0.19f, 0.15f, 0.36f, 0.07f, MenuAct::BtnPage, 0, true});   // (what each button does)
        const char* name[3] = {"TILT", "TWIST", "TURN"};
        const int val[3] = {a.ctlTilt, a.ctlTwist, a.ctlTurn};
        for (int k = 0; k < 3; ++k) {
            char b[32];
            if (val[k] == 0) snprintf(b, sizeof b, "%s: STANDARD", name[k]);
            else snprintf(b, sizeof b, "%s: %+d", name[k], val[k]);
            const float y = -0.005f - 0.085f * k;
            v.push_back({b, 0, y, 0.40f, 0.07f, MenuAct::None, 0, true});
            v.push_back({"-", -0.27f, y, 0.09f, 0.07f, MenuAct::CtlAdj, (uint32_t)(k * 2), val[k] > -kCtlMax});
            v.push_back({"+", 0.27f, y, 0.09f, 0.07f, MenuAct::CtlAdj, (uint32_t)(k * 2 + 1), val[k] < kCtlMax});
        }
        v.push_back({"BACK", -0.19f, -0.385f, 0.36f, 0.055f, MenuAct::Back, 0, true});
        v.push_back({defaultsLabel(a, DEF_CONTROLLERS), 0.19f, -0.385f, 0.36f, 0.055f, MenuAct::Defaults, DEF_CONTROLLERS, true});
        return v;
    }
    if (a.settingsOpen && a.voicePage) {
        static const char* modes[3] = {"VOICE: PUSH TO TALK", "VOICE: OPEN MIC", "VOICE: OFF"};
        v.push_back({modes[a.voiceMode % 3], -0.205f, 0.12f, 0.39f, 0.08f, MenuAct::VoiceMode, 0, true});
        {   // TALK BUTTON: TALK's button (+: more than one); it opens BUTTONS, where every button's job is chosen
            const controls::Map m = btnMap(a);
            const int ts = controls::firstSlot(m, controls::J_TALK, a.ctlKind);
            std::string b = "TALK BUTTON: ";
            b += ts < 0 ? "NONE" : controls::slotName(a.ctlKind, ts);
            if (controls::count(m, controls::J_TALK, a.ctlKind) > 1) b += " +";
            v.push_back({b, 0.205f, 0.12f, 0.39f, 0.08f, MenuAct::BtnPage, 0, true});
        }
        v.push_back({a.muteSpectators ? "SPECTATORS: MUTED" : "SPECTATORS: ON", 0, 0.02f, 0.80f, 0.08f, MenuAct::MuteSpec, 0, true});
        v.push_back({a.mutePlayers ? "OTHER PLAYERS: MUTED" : "OTHER PLAYERS: ON", 0, -0.08f, 0.80f, 0.08f, MenuAct::MutePlayers, 0, true});
        v.push_back({a.muteAll ? "MUTE ALL: ON" : "MUTE ALL: OFF", 0, -0.18f, 0.80f, 0.08f, MenuAct::MuteAll, 0, true});
        // (hosting: whether spectators hear the room; owner, 2026-11-02: practise your strategy where rivals can watch)
        v.push_back({a.net.privateTalk() ? "ROOM TALK: PRIVATE (SPECTATORS DON'T HEAR IT)" : "ROOM TALK: OPEN (SPECTATORS HEAR IT)",
                     0, -0.28f, 0.80f, 0.07f, MenuAct::PrivateTalk, 0, true});
        v.push_back({"BACK", -0.19f, -0.385f, 0.36f, 0.055f, MenuAct::Back, 0, true});
        v.push_back({defaultsLabel(a, DEF_VOICE), 0.19f, -0.385f, 0.36f, 0.055f, MenuAct::Defaults, DEF_VOICE, true});
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
        v.push_back({"CONTROLLERS", -0.19f, 0.04f, 0.36f, 0.075f, MenuAct::CtlPage, 0, true});   // (POINTER and the angle)
        v.push_back({a.net.botFill() ? "BOT FILLS IN: ON" : "BOT FILLS IN: OFF", 0.19f, 0.04f, 0.36f, 0.075f, MenuAct::BotFill, 0, true});
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
        v.push_back({"BACK", -0.19f, -0.385f, 0.36f, 0.055f, MenuAct::Back, 0, true});
        v.push_back({defaultsLabel(a, DEF_SETTINGS), 0.19f, -0.385f, 0.36f, 0.055f, MenuAct::Defaults, DEF_SETTINGS, true});
        if (a.devMode) v.push_back({"TUNING (DEVELOPER)", 0, -0.455f, 0.36f, 0.05f, MenuAct::Tuning, 0, true});
        return v;
    }
    if (a.titleOpen) {
        v.push_back({"PLAY PRACTICE", -0.145f, 0.08f, 0.27f, 0.085f, MenuAct::Play, 0, true});
        v.push_back({"TRAINING", 0.145f, 0.08f, 0.27f, 0.085f, MenuAct::Train, 0, a.net.mode() == Net::Mode::Offline});
        v.push_back({"MULTIPLAYER", 0, -0.02f, 0.56f, 0.085f, MenuAct::Multi, 0, true});
        v.push_back({"CUSTOMIZE AVATAR", -0.145f, -0.12f, 0.27f, 0.085f, MenuAct::Avatar, 0, !a.avModels.empty()});
        v.push_back({"SOUND STUDIO", 0.145f, -0.12f, 0.27f, 0.085f, MenuAct::Studio, 0, a.net.mode() == Net::Mode::Offline});
        v.push_back({"SETTINGS", -0.145f, -0.22f, 0.27f, 0.07f, MenuAct::Settings, 0, true});
        v.push_back({"HOW TO PLAY", 0.145f, -0.22f, 0.27f, 0.07f, MenuAct::Help, 0, true});
        v.push_back({"CHANGE NAME", 0, -0.31f, 0.56f, 0.07f, MenuAct::Name, 0, true});
        v.push_back({"REPORT A BUG", 0, -0.385f, 0.56f, 0.045f, MenuAct::Bug, 0, true});
        if (updateAvailable(a)) v.push_back({"UPDATE NOW", 0.215f, 0.145f, 0.16f, 0.04f, MenuAct::UpdateOpen, 0, true});
        return v;
    }
    const Net::Mode m = a.net.mode();
    const bool inGame = (m == Net::Mode::Host || m == Net::Mode::Client);
    const float lx = -0.25f, rx = 0.25f, bw = 0.42f;
    const bool spec = m == Net::Mode::Spectate;
    if (spec) {
        // spectators: their own column (no hosting, avatar or spectate entries)
        v.push_back({"STOP SPECTATING", lx, 0.12f, bw, 0.08f, MenuAct::Leave, 0, true});
        v.push_back({a.headSelfie ? "SELFIE: ON" : "SELFIE: OFF", rx, 0.175f, bw, 0.06f, MenuAct::HeadSelfie, 0, true});
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
        if (m == Net::Mode::Host) {   // HOST HANDOFF (0.3.13): someone else hosts the room from now on. In CUSTOMIZE AVATAR's place
            // (never open while hosting): over the games list it covered GAMES ONLINE (owner's report, 2026-11-02)
            bool anyone = false;
            for (const Net::Person& p : a.net.peopleHere()) anyone |= p.role != Net::VOICE_SPECTATOR;
            v.push_back({"HAND OFF HOST", lx, -0.26f, bw, 0.08f, MenuAct::HandOff, 0, anyone});
        } else {
            v.push_back({"CUSTOMIZE AVATAR", lx, -0.26f, bw, 0.08f, MenuAct::Avatar, 0,
                         !a.avModels.empty() && (m == Net::Mode::Offline || m == Net::Mode::Searching)});
        }
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
            snprintf(buf, sizeof buf, "%s  %s%s  %d-%d%s", lg.name, lg.advanced ? "ROUNDS" : "TIMED", lg.shields ? "" : " HOTDOG",
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
    const Vec3 at = f;   // (in front of you, SOUND STUDIO too: owner, 2026-11-12: off to the left was off-putting)
    float yaw = std::atan2(-at.x, -at.z);   // panel +Z faces the player
    a.menuPose = {Quat::axisAngle({0, 1, 0}, yaw), in.head.p + at * 0.9f + Vec3{0, -0.15f, 0}};
    if (inLobby(a))   // the lobby: always in the middle, between the posters, facing the balcony (owner)
        a.menuPose = {Quat{}, {kLobbySpot.x, in.head.p.y - 0.22f, kLobbySpot.z - 0.95f}};
    a.menuOpen = true;
    a.prevMenuTrig = true;   // ignore a trigger that is already held
    LOGI("menu: open");
}

static void openMenu(App& a, const PlayerInput& in);
static void saveTraining(App& a);
static void filmLoad(App& a, int lessonIndex);
static void filmReset(App& a);
static void stopTraining(App& a) {   // out of TRAINING: the game as it was (practice against the bot)
    if (!a.trainer.active()) return;
    a.trainer.stop(a.game);
    a.game.reset(true);
    a.trainPops.clear();
    LOGI("training: left");
}
static void beginLesson(App& a, int i, bool go) {   // its card (the coach shows it), or straight to 3-2-1
    if (a.net.mode() != Net::Mode::Offline) return;   // (training is played offline)
    i = std::max(0, std::min((int)training::lessons().size() - 1, i));
    a.lobby = false;
    a.titleOpen = a.trainOpen = false;
    a.trainBoard = -2;
    a.trainNewBest = false;
    a.trainPops.clear();
    a.trainer.endless = false;   // (SOUND STUDIO sets it after)
    a.trainer.begin(a.game, i, a.classicCol);
    filmLoad(a, i);
    LOGI("training: lesson %d %s", training::lessons()[i].id, training::lessons()[i].name);
    a.trainVideo = false;
    if (go) { a.trainer.start(a.game); a.menuOpen = false; }
    else { a.menuOpen = false; a.trainMenuAt = nowSeconds() + 0.35; }   // (the card opens in front of you on the court)
}
static void askBoard(App& a) {
    if (a.trainBoard < -1) return;
    const int id = a.trainBoard < 0 ? 0 : training::lessons()[a.trainBoard].id;
    a.net.requestBoard(id, a.trainBoard < 0 ? 1 : a.trainBoardKind);
    a.trainBoardAsked = nowSeconds();
}

// ---- SOUND STUDIO (0.3.16)
static void studioApply(App& a, const std::string& ev) {   // the event's choice into the mixer (studio only)
    const int s = studioChosen(a, ev);
    if (s < 0) { a.audio.setOverride(ev, ""); return; }
    if (a.studioSounds[s].mine) {   // your own .wav (my_sounds), loaded once
        const std::string clip = "mine/" + a.studioSounds[s].file;
        auto it = a.studioLoaded.find(clip);
        if (it == a.studioLoaded.end()) it = a.studioLoaded.emplace(clip, a.audio.loadFile(clip, a.studioSounds[s].path)).first;
        if (!it->second) a.net.noticeLocal("THAT FILE ISN'T A WAV THE GAME CAN PLAY - EXPORT IT AS WAV");
        a.audio.setOverride(ev, it->second ? clip : "");
        return;
    }
    if (a.studioSounds[s].official) {   // the game's own sound of another slot: its clip as it is
        a.audio.setOverride(ev, a.audio.officialClip(a.studioSounds[s].event));
        return;
    }
    const std::string& f = a.studioSounds[s].file;
    const std::string clip = "studio/" + f;
    auto it = a.studioLoaded.find(clip);
    if (it == a.studioLoaded.end()) it = a.studioLoaded.emplace(clip, a.audio.loadFile(clip, a.studioLib.path(f))).first;
    a.audio.setOverride(ev, it->second ? clip : "");
}
static void studioAddLocal(App& a) {   // your own .wav files (my_sounds) and the game's own sounds, after the library's
    if (const char* d = dataDir(a))
        for (const studio::Sound& m : studio::mySounds(std::string(d) + "/my_sounds")) a.studioSounds.push_back(m);
    for (const studio::Sound& o : studio::officialSounds()) a.studioSounds.push_back(o);
}
static void studioOpenSection(App& a, int i) {   // a section, and its moment: its TRAINING lesson, endless
    const auto& S = studio::sections();
    a.studioSection = std::max(0, std::min((int)S.size() - 1, i));
    const int id = S[a.studioSection].lesson ? S[a.studioSection].lesson : (a.trainer.active() ? 0 : 21);
    const int li = id ? training::lessonById(id) : -1;
    if (li >= 0 && (!a.trainer.active() || a.trainer.lesson != li)) {
        beginLesson(a, li, true);
        a.trainer.endless = true;
        // the page opens once you stand on the court (owner, 2026-11-12: opened in the lobby, it stayed behind there
        // and the studio began with no menu), as a lesson's card does
        a.menuOpen = false;
        a.trainMenuAt = nowSeconds() + 0.35;
        return;
    }
    openMenu(a, a.localIn);
}
static void studioOpen(App& a) {
    if (a.net.mode() != Net::Mode::Offline) return;
    a.titleOpen = false;
    a.studioOn = true;
    a.studioSounds.clear();   // (yours and the game's own at once; the library's join them when synced)
    for (auto it = a.studioLoaded.begin(); it != a.studioLoaded.end();)   // (your files read afresh: you may have changed them)
        it = it->first.compare(0, 5, "mine/") == 0 ? a.studioLoaded.erase(it) : std::next(it);
    studioAddLocal(a);
    a.studioPick.clear();
    if (const char* d = dataDir(a)) a.studioLib.sync(a.serverAddr, std::string(d) + "/sound_library", a.net.statsKey());
    LOGI("sound studio: open");
    studioOpenSection(a, 0);
}
static void studioLeave(App& a) {   // MAIN MENU: the game's own sounds again, out of the lesson to the lobby
    a.studioOn = false;
    a.audio.clearOverrides();
    a.studioPick.clear();
    if (a.trainer.active()) { stopTraining(a); a.lobby = true; }
    LOGI("sound studio: closed");
    openMenu(a, a.localIn);
    a.titleOpen = true;
}
static void studioStep(App& a, int k, int d) {   // the event's next or previous choice, played at once
    const studio::Section& sec = studio::sections()[a.studioSection];
    if (k < 0 || k >= (int)sec.events.size()) return;
    const std::string ev = sec.events[k];
    const std::vector<int> ch = studio::choicesFor(ev, a.studioSounds);
    const int cur = studioChosen(a, ev);
    int at = 0;
    for (int i = 0; i < (int)ch.size(); ++i) if (ch[i] == cur) at = i;
    at = (at + d + (int)ch.size()) % (int)ch.size();
    a.studioPick[ev] = ch[at] < 0 ? "" : a.studioSounds[ch[at]].file;
    studioApply(a, ev);
    a.audio.cue(ev);
}
static void studioVote(App& a, int k, int dir) {   // UP / DOWN; your vote pressed again takes it back
    const studio::Section& sec = studio::sections()[a.studioSection];
    if (k < 0 || k >= (int)sec.events.size()) return;
    const int s = studioChosen(a, sec.events[k]);
    if (s < 0 || a.studioSounds[s].official || a.studioSounds[s].mine) return;   // (votes are for the library's new sounds)
    studio::Sound& snd = a.studioSounds[s];
    const int v = snd.vote == dir ? 0 : dir;
    if (a.net.sendSoundVote(snd.file, v)) {   // (kept until the community server takes it: at the latest when you leave)
        snd.vote = v;
        LOGI("sound studio: vote %+d for %s", v, snd.file.c_str());
        a.net.noticeLocal(v > 0 ? "YOU VOTED UP - IT GOES TO THE COMMUNITY'S COUNT" : v < 0 ? "YOU VOTED DOWN - IT GOES TO THE COMMUNITY'S COUNT"
                                                     : "YOUR VOTE IS TAKEN BACK");
    }
}
static void studioDelete(App& a, int k) {   // one of your own files off the headset (DELETE, then SURE? within 4 s)
    const studio::Section& sec = studio::sections()[a.studioSection];
    if (k < 0 || k >= (int)sec.events.size()) return;
    const std::string ev = sec.events[k];
    const int s = studioChosen(a, ev);
    if (s < 0 || !a.studioSounds[s].mine) return;
    const studio::Sound snd = a.studioSounds[s];
    if (a.studioDelArmed != snd.file || nowSeconds() > a.studioDelUntil) {
        a.studioDelArmed = snd.file;
        a.studioDelUntil = nowSeconds() + 4.0;
        return;
    }
    a.studioDelArmed.clear();
    const bool gone = std::remove(snd.path.c_str()) == 0;
    LOGI("sound studio: %s %s", snd.path.c_str(), gone ? "deleted" : "could not be deleted");
    if (!gone) { a.net.noticeLocal("COULDN'T DELETE THAT FILE"); return; }
    a.studioSounds.erase(a.studioSounds.begin() + s);
    for (auto& kv : a.studioPick) if (kv.second == snd.file) kv.second.clear();
    for (const studio::Section& sc : studio::sections())
        for (const char* e : sc.events) studioApply(a, e);
    a.net.noticeLocal(("DELETED " + snd.name).c_str());
}
static void studioUpdate(App& a) {   // every frame: the synced library in; out of the studio if its lesson stopped
    if (!a.studioOn) return;
    if (!a.trainer.active() || a.net.mode() != Net::Mode::Offline) {
        a.studioOn = false;
        a.audio.clearOverrides();
        a.studioPick.clear();
        return;
    }
    std::vector<studio::Sound> got;
    if (a.studioLib.take(got)) {
        for (studio::Sound& s : got) a.net.pendingSoundVote(s.file, s.vote);   // (a vote not yet sent beats the server's)
        a.studioSounds = got;
        studioAddLocal(a);
        for (const studio::Section& sec : studio::sections())
            for (const char* ev : sec.events) studioApply(a, ev);
    }
}

static void menuActivate(App& a, const MenuItem& it) {
    LOGI("menu: %s", it.label.c_str());
    if (it.act != MenuAct::Quit) a.quitArmed = false;
    switch (it.act) {
    case MenuAct::Train: a.titleOpen = false; a.trainOpen = true; break;
    case MenuAct::Studio: studioOpen(a); break;
    case MenuAct::StudioSection: studioOpenSection(a, (int)it.ip); break;
    case MenuAct::StudioPrev: studioStep(a, (int)it.ip, -1); break;
    case MenuAct::StudioNext: studioStep(a, (int)it.ip, 1); break;
    case MenuAct::StudioPlay:
        if (it.ip < studio::sections()[a.studioSection].events.size()) a.audio.cue(studio::sections()[a.studioSection].events[it.ip]);
        break;
    case MenuAct::StudioVote: studioVote(a, (int)(it.ip & 0xFFu), (it.ip & 0x100u) ? 1 : -1); break;
    case MenuAct::StudioMoment: a.menuOpen = false; break;
    case MenuAct::StudioLeave: studioLeave(a); break;
    case MenuAct::StudioDelete: studioDelete(a, (int)it.ip); break;
    case MenuAct::TrainPick: beginLesson(a, (int)it.ip, false); break;
    case MenuAct::TrainStart: a.trainer.start(a.game); a.menuOpen = false; break;
    case MenuAct::TrainRetry: beginLesson(a, a.trainer.lesson, true); break;
    case MenuAct::TrainNext: if (training::nextInTrack(a.trainer.lesson) >= 0) beginLesson(a, training::nextInTrack(a.trainer.lesson), false); break;
    case MenuAct::TrainList: a.trainOpen = true; a.trainBoard = -2; break;
    case MenuAct::TrainLeave:   // MAIN MENU: out of training (to the lobby), the main menu open
        a.trainOpen = false;
        a.trainBoard = -2;
        a.trainMenuAt = 0;
        if (a.trainer.active()) {
            stopTraining(a);
            a.lobby = true;
            openMenu(a, a.localIn);
        }
        a.titleOpen = true;
        break;
    case MenuAct::TrainBoard:
        a.trainBoard = it.ip == 0xFFFFFFFFu ? -1 : (int)it.ip;
        a.trainBoardKind = a.trainBoard >= 0 && training::lessons()[a.trainBoard].timed() ? 0 : 1;
        askBoard(a);
        break;
    case MenuAct::TrainKind: a.trainBoardKind = (int)it.ip; askBoard(a); break;
    case MenuAct::TrainBoardBack: a.trainBoard = -2; break;
    case MenuAct::TrainShow: beginLesson(a, a.trainer.lesson, false); a.trainVideo = true; break;   // (its card again, the coach's video on)
    case MenuAct::TrainVideo: a.trainVideo = !a.trainVideo; if (a.trainVideo) filmReset(a); break;
    case MenuAct::Quit:
        if (!a.quitArmed) { a.quitArmed = true; break; }   // the second press quits
        LOGI("menu: quit game");
        saveSettings(a);
        if (a.net.inRoom()) a.net.leave();
        if (a.session != XR_NULL_HANDLE) xrRequestExitSession(a.session); else a.quit = true;
        break;
    case MenuAct::Nav: {
        a.titleOpen = a.settingsOpen = a.voicePage = a.ptPage = a.gfxPage = a.ctlPage = a.roomBallPage = a.btnPage = a.xpOpen = a.bugOpen = a.reportOpen = a.rulesOpen = a.helpOpen = a.movePlayersOpen = a.updateOpen = false;
        a.statsOpen = false;
        a.trainOpen = false;
        a.trainBoard = -2;
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
        case NAV_HELP: a.helpOpen = true; a.creditsOpen = false; break;
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
    case MenuAct::Defaults:   // two presses within 4 s, on the same page
        if (a.defaultsArmed == (int)it.ip && nowSeconds() < a.defaultsArmedUntil) {
            applyDefaults(a, (int)it.ip);
            a.defaultsArmed = -1;
            a.defaultsArmedUntil = 0;
            if (it.ip != DEF_MATCH) saveSettings(a);   // (MATCH OPTIONS: nothing changes until START / APPLY)
            a.net.noticeLocal(it.ip == DEF_MATCH ? "THE RULES' OWN SETTINGS - PRESS APPLY OR START" : "BACK TO THE DEFAULTS");
            LOGI("menu: defaults on page %u", it.ip);
        } else {
            a.defaultsArmed = (int)it.ip;
            a.defaultsArmedUntil = nowSeconds() + 4.0;
        }
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
        a.settingsOpen = false; a.voicePage = false; a.ptPage = false; a.gfxPage = false; a.ctlPage = false; a.roomBallPage = false; a.btnPage = false; a.reportOpen = true;
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
    case MenuAct::Host: a.rulesOpen = true; a.rulesForHost = true; a.rulesEdit = false; a.rulesStep = 0; optionsFrom(a, a.game.tun); break;   // CHOOSE THE RULES, then the room opens
    case MenuAct::RoomPick: {   // CHOOSE A ROOM: then its options (nothing changes until START / APPLY)
        const int was = a.optExperimental ? 1 + a.optExpRoom : 0, k = (int)std::min<uint32_t>(3, it.ip);
        a.optExperimental = k > 0;
        if (k > 0) a.optExpRoom = k - 1;
        if (k >= 2) a.optAdvanced = false;   // (the four-player rooms: a timed match; the SQUARE and DIAMOND: either)
        a.roundsPad = false; a.roomBallPage = false;
        if (k == 3 && was != 3) { a.optShields = false; a.optKnuckle = 1; }   // FIREBALL BLITZ's own: no shields, knuckles on
        if (k != 3 && was == 3) { a.optShields = true; a.optKnuckle = -1; }   // (and back from it: the rules')
        a.rulesStep = 1;
        break;
    }
    case MenuAct::RulesPick: a.optAdvanced = it.ip == 1; break;   // the SQUARE, DIAMOND: a TIMED MATCH or ROUNDS
    case MenuAct::OptShields: {   // ON, OFF (HOTDOG), and in the SQUARE and DIAMOND ALWAYS (MAIDEN)
        const bool two = !a.optExperimental || a.optExpRoom == ROOM_DIAMOND;
        if (a.optShields && !a.optMaiden) a.optShields = false;
        else if (!a.optShields && two) { a.optShields = true; a.optMaiden = true; }
        else { a.optShields = true; a.optMaiden = false; }
        break;
    }
    case MenuAct::OptAim: {   // OFF, LOW, MEDIUM, HIGH (kept as "the rules'" when it is theirs)
        const int rules = a.optAdvanced ? 1 : 2, cur = a.optAim >= 0 ? a.optAim : rules, next = (cur + 1) % 4;
        a.optAim = next == rules ? -1 : next;
        break;
    }
    case MenuAct::OptZone: a.optZone = (a.optZone + 1) % 3; break;       // SMALL, REGULATION, LARGE
    case MenuAct::OptGrowth: a.optGrowth = (a.optGrowth + 1) % 4; break;   // NONE, QUARTER, HALF, REGULATION
    case MenuAct::OptRounds: a.optRounds = std::max(1, std::min((kMaxBestOf + 1) / 2, a.optRounds + (it.ip ? 1 : -1))); break;
    case MenuAct::RoundsPad: a.roundsPad = true; a.roundsBuf.clear(); break;   // BEST OF n typed
    case MenuAct::PadKey:   // (digits only: the pad has no other keys; two at most, no leading zero)
        if (a.roundsBuf.size() < 2 && !(a.roundsBuf.empty() && it.ip == '0')) a.roundsBuf += (char)it.ip;
        break;
    case MenuAct::PadDel: if (!a.roundsBuf.empty()) a.roundsBuf.pop_back(); break;
    case MenuAct::PadOk:
        if (padBestOf(a) > 0) a.optRounds = (padBestOf(a) + 1) / 2;
        a.roundsPad = false; a.roundsBuf.clear();
        break;
    case MenuAct::PadCancel: a.roundsPad = false; a.roundsBuf.clear(); break;
    case MenuAct::OptBotPlay: a.optBotPlay = 1 - (a.optBotPlay & 1); break;   // BOT STYLE: BRAWLER, DUELLIST
    case MenuAct::OptShieldSize: a.optShieldSize = (a.optShieldSize + 1) % 5; break;   // REGULATION .. DOUBLE
    case MenuAct::OptStrike: a.optStrikeSpeed = (a.optStrikeSpeed + 1) % 3; break;   // REGULATION, FAST, FASTER
    case MenuAct::OptFoul: a.optFoulShift = a.optFoulShift >= 2 ? -2 : a.optFoulShift + 1; break;   // 0, LONGER 1, 2, SHORTER 2, 1
    case MenuAct::OptRoom:   // DIAMOND, FOUR CORNERS, FIREBALL BLITZ (its own defaults: knuckle guards on, no shields)
        a.optExpRoom = (a.optExpRoom + 1) % kRooms;
        if (a.optExpRoom == ROOM_BLITZ) { a.optShields = false; a.optKnuckle = 1; }
        break;
    case MenuAct::OptHealth: { static const int kHealth[4] = {1, 3, 5, 10}; int i = 0; while (i < 4 && kHealth[i] <= a.optHealth) ++i; a.optHealth = kHealth[i % 4]; break; }
    case MenuAct::Discord: a.discordOn = !a.discordOn; saveSettings(a); break;
    case MenuAct::UpdateOpen: a.titleOpen = false; a.updateOpen = true; entitlement::updateCheck(); break;   // (store copies: ask the store)
    case MenuAct::UpdateStoreGet: entitlement::updateDownload(); break;
    case MenuAct::UpdateStoreInstall: saveSettings(a); if (a.net.inRoom()) a.net.leave(); entitlement::updateInstall(); break;
    case MenuAct::HeadSelfie: a.headSelfie = !a.headSelfie; saveSettings(a); break;
    case MenuAct::PrivateTalk: a.net.setPrivateTalk(!a.net.privateTalk()); saveSettings(a); break;
    case MenuAct::LookAhead:
        a.net.setLookAhead(!a.net.lookAhead());
        saveSettings(a);
        LOGI("menu: ball look-ahead %s", a.net.lookAhead() ? "on" : "off");
        break;
    case MenuAct::UpdateBack: a.updateOpen = false; a.titleOpen = true; break;
#ifdef __ANDROID__
    case MenuAct::UpdateStore: androidOpenUrl(kStorePage); break;
    case MenuAct::UpdatePage: androidOpenUrl(kReleasePage); break;
#else
    case MenuAct::UpdateGo: if (const char* v = updateAvailable(a)) updater::start(v); break;
    case MenuAct::UpdateRestart: if (updater::restart()) { saveSettings(a); a.quit = true; } break;
    case MenuAct::UpdatePage: updater::openPage(); break;
#endif
    case MenuAct::OptBalls: a.optBalls = a.optBalls % 4 + 1; break;
    case MenuAct::OptPlayers: a.optOctPlayers = a.optOctPlayers >= 4 ? 2 : a.optOctPlayers + 1; break;
    case MenuAct::MovePlayers: a.rulesOpen = false; a.movePlayersOpen = true; a.moveHost = false; a.moveSel = -1; break;
    case MenuAct::HandOff: a.movePlayersOpen = true; a.moveHost = true; break;
    case MenuAct::HandOffTo:   // (the match pauses while the room changes hands, then goes on)
        if (a.net.hostHandOff(it.ip, a.game)) { a.movePlayersOpen = false; a.moveHost = false; a.menuOpen = false; }
        break;
    case MenuAct::MoveCorner:   // the first corner picked, then the one it swaps with (the same again: unpicked)
        if (a.moveSel < 0) a.moveSel = (int)it.ip;
        else if (a.moveSel == (int)it.ip) a.moveSel = -1;
        else { a.net.hostSwap(a.moveSel, (int)it.ip, a.game); a.moveSel = -1; }
        break;
    case MenuAct::MoveOne: a.net.hostMakeOne((int)it.ip, a.game); a.moveSel = -1; break;
    case MenuAct::MoveBack: a.movePlayersOpen = false; a.moveSel = -1; a.rulesOpen = !a.moveHost; a.moveHost = false; break;
    case MenuAct::OptTeams:   // FREE-FOR-ALL, 2 V 2, 3 V 1 with you THE ONE, (hosting) 3 V 1 taking turns
        if (a.optTeams == TEAMS_3V1 && !a.optOneTurns && a.rulesForHost) a.optOneTurns = true;
        else { a.optTeams = (a.optTeams + 1) % 3; a.optOneTurns = false; }
        break;
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
    case MenuAct::OptBack: a.rulesStep = 0; a.roundsPad = false; a.roomBallPage = false; break;
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
    case MenuAct::BotFill:   // (a room taken over keeps its own setting: ours stays as we saved it)
        a.net.setBotFill(!a.net.botFill());
        if (!a.net.inherited()) { a.botFill = a.net.botFill(); saveSettings(a); }
        break;
    case MenuAct::Rules:   // the rules and MATCH OPTIONS of this arena (or of practice)
        a.settingsOpen = false;
        a.rulesOpen = true; a.rulesStep = 0; a.rulesEdit = true;
        a.rulesForHost = a.net.mode() == Net::Mode::Host;
        optionsFrom(a, a.game.tun);
        break;
    case MenuAct::Close: a.menuOpen = false; a.nameEdit = false; a.helpOpen = false; a.settingsOpen = false; a.titleOpen = false; a.xpOpen = false; a.rulesOpen = false; a.movePlayersOpen = false; a.updateOpen = false; break;
    case MenuAct::Spectate: a.net.spectate(); a.watchIp = 0; a.menuOpen = false; break;
    case MenuAct::Watch: a.watchIp = it.ip; a.menuOpen = false; break;
    case MenuAct::Hub: a.watchIp = 0; a.autoCam = false; a.dirOpen = false; break;
    case MenuAct::Queue: a.net.queue(a.net.localQueuePos() < 0); break;
    case MenuAct::HideOutside: a.hideOutside = !a.hideOutside; saveSettings(a); break;
    case MenuAct::ClearWarps: a.warps.clear(); a.warpIdx = -1; a.autoCam = false; saveWarps(a); break;
    case MenuAct::Director: openDirector(a, a.localIn); break;
    case MenuAct::Avatar: a.titleOpen = false; openBuilder(a, a.localIn); break;
    case MenuAct::Play:   // CHOOSE THE RULES first (then MATCH OPTIONS)
        stopTraining(a);
        a.titleOpen = false; a.rulesOpen = true; a.rulesForHost = false; a.rulesEdit = false; a.rulesStep = 0;
        optionsFrom(a, a.game.tun);
        break;
    case MenuAct::Multi: a.titleOpen = false; break;
    case MenuAct::Settings: a.settingsOpen = true; break;
    case MenuAct::Back: if (a.btnPage) a.btnPage = false; else if (a.ctlPage) a.ctlPage = false; else if (a.voicePage) a.voicePage = false; else if (a.ptPage) a.ptPage = false; else if (a.gfxPage) a.gfxPage = false; else a.settingsOpen = false; saveSettings(a); break;
    case MenuAct::GfxPage: a.gfxPage = true; break;
    case MenuAct::CtlPage: a.ctlPage = true; break;
    case MenuAct::BallPage: a.roomBallPage = true; a.roundsPad = false; break;   // (the room's BALL FEEL)
    case MenuAct::BallBack: a.roomBallPage = false; break;
    case MenuAct::BallCore: a.optBallFeel ^= BALL_CORE; break;   // (nothing changes until START / APPLY)
    case MenuAct::BallTrue: a.optBallFeel ^= BALL_TRUE; break;
    case MenuAct::BallSpin: a.optBallFeel ^= BALL_SPIN; break;
    case MenuAct::BallWeight: a.optBallFeel ^= BALL_WEIGHT; break;
    case MenuAct::CtlAdj: {   // (k * 2 + up): a step of kCtlStep degrees, live on your hands
        int* val[3] = {&a.ctlTilt, &a.ctlTwist, &a.ctlTurn};
        int& x = *val[std::min(2u, it.ip / 2)];
        x = std::max(-kCtlMax, std::min(kCtlMax, x + (it.ip & 1 ? kCtlStep : -kCtlStep)));
        saveSettings(a);
        break;
    }
    case MenuAct::FovDown: a.roomFeel = std::max(-5, a.roomFeel - 1); saveSettings(a); break;
    case MenuAct::FovUp: a.roomFeel = std::min(5, a.roomFeel + 1); saveSettings(a); break;
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
    case MenuAct::BtnPage: a.btnPage = true; break;
    case MenuAct::BtnSlot: {   // BUTTONS: that button's next job (a job that must keep a button keeps its last: say so)
        controls::Map m = btnMap(a);
        const int s = (int)(it.ip % controls::S_COUNT);
        if (!controls::canLeave(m, s, a.ctlKind)) {
            a.btnNote = std::string(controls::jobName(m.job[s])) + " NEEDS A BUTTON: GIVE IT ANOTHER ONE FIRST";
            a.btnNoteUntil = nowSeconds() + 4.0;
            break;
        }
        m.job[s] = (uint8_t)controls::nextJob(m, s, a.ctlKind);
        a.btnMap = m;
        a.btnNoteUntil = 0;
        saveSettings(a);
        break;
    }
    case MenuAct::BtnSticks: a.btnMap = btnMap(a); a.btnMap.leftSticks = !a.btnMap.leftSticks; saveSettings(a); break;
    case MenuAct::BtnPreset:   // STANDARD (the pointer in the right hand) or LEFT-HANDED (all mirrored, the pointer left)
        a.btnMap = it.ip ? controls::leftHanded() : controls::standard();
        a.pointerHand = it.ip ? 0 : 1;
        a.btnNoteUntil = 0;
        saveSettings(a);
        break;
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
    case MenuAct::Help: a.helpOpen = true; a.creditsOpen = false; break;
    case MenuAct::Credits: a.creditsOpen = true; break;
    case MenuAct::CreditsBack: a.creditsOpen = false; break;
    case MenuAct::HelpDone:
        a.helpOpen = false;
        a.creditsOpen = false;
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
                if (a.trainer.active() && nowSeconds() > a.trainLogAt) {   // (TRAINING's pages: what the pointer does, now and then)
                    a.trainLogAt = nowSeconds() + 2.0;
                    LOGI("training menu: pointer at (%.2f %.2f) on the page, over %d, trigger %.2f", lp.x, lp.y, a.menuHover,
                         in.hand[a.pointerHand].trigger);
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
    const float ua = (a.ptRunning || a.ownFail) ? 0.998f : 0.f;   // over your room the menus are solid (never see-through); the store page too
    const float bg[4] = {0.004f, 0.007f, 0.016f, std::max(ua, 0.90f)}, edge[4] = {0.05f, 0.35f, 0.60f, std::max(ua, 0.9f)};
    const float btn[4] = {0.03f, 0.05f, 0.08f, std::max(ua, 0.95f)}, hov[4] = {0.05f, 0.40f, 0.75f, std::max(ua, 0.98f)},
                dis[4] = {0.015f, 0.015f, 0.02f, std::max(ua, 0.9f)};
    const float white[4] = {1, 1, 1, 1}, grey[4] = {0.35f, 0.38f, 0.42f, 1}, cyan[4] = {0.3f, 0.85f, 1.0f, 1};
    const bool airy = a.settingsOpen && a.gfxPage && !a.nameEdit;   // GRAPHICS: the arena shows around its buttons
    if (!airy) {
        quad(0, -0.09f, 1.04f, 0.98f, -0.002f, edge);
        quad(0, -0.09f, 1.0f, 0.94f, 0, bg);
    }
    if (!a.nameEdit && !airy && !trainSide(a) && !a.ownFail) {   // side menu
        quad(kNavX, -0.09f, 0.30f, 0.98f, -0.002f, edge);
        quad(kNavX, -0.09f, 0.28f, 0.94f, 0, bg);
    }
    char buf[96];
    if (a.ownFail) {   // a store build: Meta couldn't confirm the purchase
        text("PROJECT ARENA COULDN'T CONFIRM YOUR PURCHASE", 0, 0.25f, 0.034f, cyan);
        text("META COULDN'T CONFIRM THAT THIS ACCOUNT OWNS THE GAME", 0, 0.15f, 0.021f, white);
        text("CHECK THAT THE HEADSET IS SIGNED IN TO THE ACCOUNT THAT GOT IT", 0, 0.08f, 0.021f, white);
        text("AND IS CONNECTED TO THE INTERNET, THEN START THE GAME AGAIN", 0, 0.035f, 0.021f, white);
        const int left = std::max(0, (int)std::ceil(kOwnFailSeconds - (nowSeconds() - a.ownFailAt)));
        std::snprintf(buf, sizeof buf, "THE GAME CLOSES IN %d S", left);
        text(buf, 0, -0.06f, 0.024f, grey);
        text("HELP: THE PROJECT ARENA DISCORD, OR GITHUB.COM/JENDOEBELIN/PROJECTARENA-PUBLIC", 0, -0.20f, 0.016f, grey);
    } else if (a.trainBoard >= -1 && !a.nameEdit) {   // a TRAINING leaderboard
        const bool overall = a.trainBoard < 0;
        const training::Lesson* L = overall ? nullptr : &training::lessons()[a.trainBoard];
        const int kind = overall ? 1 : a.trainBoardKind;
        text("LEADERBOARD", 0, 0.29f, 0.050f, cyan);
        text(overall ? std::string("OVERALL - EVERY LESSON'S BEST ACCURACY ADDED UP") : std::string(L->name) + (kind ? " - MOST ACCURATE" : " - FASTEST"),
             0, 0.225f, 0.024f, white);
        const Net::Board* bd = a.net.board(overall ? 0 : L->id, kind);
        auto value = [&](uint32_t v) {
            char s[24];
            if (overall) std::snprintf(s, sizeof s, "%u.%u", v / 10, v % 10);
            else if (kind == 0) std::snprintf(s, sizeof s, "%u.%u S", v / 1000, (v / 100) % 10);
            else std::snprintf(s, sizeof s, "%u.%u%%", v / 10, v % 10);
            return std::string(s);
        };
        if (bd) {
            for (size_t i = 0; i < bd->rows.size() && i < 10; ++i) {
                const float y = 0.16f - 0.045f * i;
                std::snprintf(buf, sizeof buf, "%d", (int)i + 1);
                text(buf, -0.36f, y, 0.026f, i == 0 ? cyan : white);
                text(bd->rows[i].name, -0.08f, y, 0.026f, white);
                text(value(bd->rows[i].value), 0.28f, y, 0.026f, white);
            }
            if (bd->rows.empty()) text("NOBODY YET - BE THE FIRST", 0, 0.10f, 0.026f, grey);
            if (bd->myPlace > 0) std::snprintf(buf, sizeof buf, "YOU: NUMBER %d OF %d  -  %s", bd->myPlace, bd->players, value(bd->myValue).c_str());
            else std::snprintf(buf, sizeof buf, "YOU: NOT ON THIS ONE YET (PASS THE LESSON)");
            text(buf, 0, -0.32f, 0.022f, cyan);
        } else if (!a.net.onlineReady()) {
            text("GO ONLINE TO SEE THE LEADERBOARDS", 0, 0.08f, 0.028f, white);
            text("MULTIPLAYER - ONLINE: ON. YOUR BESTS ARE KEPT AND SENT WHEN YOU ARE", 0, 0.03f, 0.018f, grey);
        } else {
            text(nowSeconds() - a.trainBoardAsked < 4.0 ? "ASKING THE SERVER..." : "NO ANSWER: THE SERVER MAY NOT HAVE LEADERBOARDS YET",
                 0, 0.08f, 0.024f, grey);
        }
    } else if (a.trainOpen && !a.nameEdit) {   // the lessons
        text("TRAINING", 0, 0.31f, 0.05f, cyan);
        text("THREE TRACKS - PASS A LESSON TO OPEN THE NEXT IN ITS TRACK", 0, 0.262f, 0.019f, white);
        for (int t = 0; t < training::kTracks; ++t) text(training::trackName(t), trainColX(t), 0.215f, 0.026f, cyan);
        const auto& Ls = training::lessons();
        for (int i = 0; i < (int)Ls.size(); ++i) {
            const training::Best& b = a.trainBests.b[Ls[i].id];
            const float x = trainColX(Ls[i].track), y = trainRowY(training::placeInTrack(i) - 1) - 0.034f;
            if (!a.trainBests.unlocked(i)) std::snprintf(buf, sizeof buf, "LOCKED");
            else if (!b.passed) std::snprintf(buf, sizeof buf, "%s", Ls[i].teach);
            else if (Ls[i].timed()) std::snprintf(buf, sizeof buf, "BEST %.1f S - %d%%", b.time, b.permille() / 10);
            else std::snprintf(buf, sizeof buf, "BEST %d / %d", b.successes, b.attempts);
            text(buf, x, y, 0.0125f, b.passed ? cyan : grey);
        }
        std::snprintf(buf, sizeof buf, "TRAINING SCORE %d.%d  (EVERY LESSON'S BEST ACCURACY ADDED UP)", a.trainBests.total() / 10, a.trainBests.total() % 10);
        text(buf, 0, -0.405f, 0.017f, grey);
        if (a.net.mode() != Net::Mode::Offline) text("LEAVE THE ROOM TO TRAIN", 0, -0.51f, 0.02f, white);
    } else if (a.studioOn && !a.nameEdit && !otherPage(a)) {   // SOUND STUDIO
        const studio::Section& sec = studio::sections()[a.studioSection];
        const float gold[4] = {1.0f, 0.85f, 0.35f, 1};
        text("SOUND STUDIO", 0, 0.31f, 0.05f, cyan);
        const studio::Library::State st = a.studioLib.state();
        if (st == studio::Library::LOADING)
            std::snprintf(buf, sizeof buf, "GETTING THE LIBRARY FROM THE SERVER...  %d / %d", a.studioLib.progress(), a.studioLib.total());
        else if (st == studio::Library::FAILED)
            std::snprintf(buf, sizeof buf, "NO NEW SOUNDS: %s (ONLINE: ON)  -  THE GAME'S OWN CAN BE TRIED IN ANY SLOT", a.studioLib.error().c_str());
        else {
            int lib = 0;
            for (const studio::Sound& s : a.studioSounds) lib += s.official || s.mine ? 0 : 1;
            if (lib) std::snprintf(buf, sizeof buf, "%d NEW SOUNDS TO TRY, ONLY IN HERE  -  VOTE UP OR DOWN (AGAIN: TAKEN BACK)", lib);
            else std::snprintf(buf, sizeof buf, "NO NEW SOUNDS YET: TRY THE GAME'S OWN IN ANY SLOT, OR PROPOSE ONE (DOCS/SOUND_STUDIO.MD)");
        }
        {   // your own: how many, how much room they take, where they go
            int mine = 0;
            double mb = 0;
            for (const studio::Sound& s : a.studioSounds)
                if (s.mine) {
                    ++mine;
                    if (FILE* f = std::fopen(s.path.c_str(), "rb")) { std::fseek(f, 0, SEEK_END); mb += std::ftell(f) / 1048576.0; std::fclose(f); }
                }
            char line[160];
            if (mine) std::snprintf(line, sizeof line, "MINE: %d .WAV FILE%s, %.1f MB, IN THE GAME'S MY_SOUNDS FOLDER  -  DELETE ONE WITH ITS DELETE BUTTON",
                                    mine, mine == 1 ? "" : "S", mb);
            else std::snprintf(line, sizeof line, "TRY YOUR OWN: PUT .WAV FILES (WAV ONLY) IN THE GAME'S MY_SOUNDS FOLDER, THEN OPEN THE STUDIO AGAIN");
            text(line, 0, -0.404f, 0.0105f, grey);
        }
        text(buf, 0, 0.27f, 0.0135f, st == studio::Library::FAILED ? gold : white);
        text(sec.how, 0, 0.245f, 0.0125f, grey);
        text("YOUR VOTE", 0.32f, 0.158f, 0.0125f, cyan);   // (over UP / DOWN: owner 2026-11-06, the votes weren't obvious)
        text("SOUND", 0.01f, 0.158f, 0.0125f, cyan);
        text("THE MOMENT PLAYS ON WHILE THIS PAGE IS OPEN: CHANGE A SOUND AND HEAR IT. MENU BRINGS THE PAGE BACK", 0, -0.383f, 0.0115f, grey);
        for (int k = 0; k < (int)sec.events.size(); ++k) {
            const float y = studioRowY(k);
            const std::string ev = sec.events[k];
            {   // its slot: the asset catalog's code and name ("S12 SHIELD BREAK"), smaller when long
                const std::string sl = studio::slotName(ev);
                text(sl, -0.265f, y - 0.006f, std::min(0.0165f, 0.215f / (0.75f * (float)sl.size())), white);
            }
            const int s = studioChosen(a, ev);
            text(s < 0 ? std::string("OFFICIAL") : a.studioSounds[s].name, 0.01f, y - 0.002f, 0.0155f, s < 0 ? grey : cyan);
            if (s >= 0 && a.studioSounds[s].mine)   // (your own .wav: only here)
                text("MINE - ONLY ON THIS HEADSET", 0.01f, y - 0.019f, 0.0095f, grey);
            else if (s >= 0 && a.studioSounds[s].official)   // (the game's own sound of another slot, tried here)
                text("THE GAME'S OWN, TRIED HERE", 0.01f, y - 0.019f, 0.0095f, grey);
            else if (s >= 0 && a.studioSounds[s].event != ev)   // (made for another event: tried here)
                text("MADE FOR " + studio::slotName(a.studioSounds[s].event), 0.01f, y - 0.019f, 0.0095f, grey);
        }
    } else if (a.trainer.active() && !a.nameEdit && !otherPage(a)) {   // a lesson: its card, its result, or paused
        using namespace training;
        const Lesson& L = a.trainer.L();
        const float gold[4] = {1.0f, 0.85f, 0.35f, 1};
        const int i = a.trainer.lesson;
        {
            int n = 0;
            const int place = placeInTrack(i, &n);
            std::snprintf(buf, sizeof buf, "%s - LESSON %d OF %d", trackName(L.track), place, n);
        }
        text(buf, 0, 0.30f, 0.022f, cyan);
        text(L.name, 0, 0.245f, 0.05f, gold);
        auto goal = [&]() {
            char s[96];
            if (L.kind == THROW) std::snprintf(s, sizeof s, "TAKE ALL %d TARGETS IN %d THROWS", L.goal(), L.budget);
            else if (L.kind == STRIKES) std::snprintf(s, sizeof s, "%d STRIKES IN %d THROWS", L.pass, L.budget);
            else if (L.kind == CATCH) std::snprintf(s, sizeof s, "CATCH %d OF %d", L.pass, L.budget);
            else if (L.guard == G_SENDBACK) std::snprintf(s, sizeof s, "SEND THROWS INTO ALL %d TARGETS - %d THROWS COME", L.goal(), L.budget);
            else std::snprintf(s, sizeof s, "%s %d OF %d THROWS", L.guard == G_SHIELD ? "BLOCK" : L.guard == G_KNUCKLE ? "PUNCH AWAY"
                                                                  : L.guard == G_EITHER ? "BLOCK OR PUNCH" : "DODGE", L.pass, L.budget);
            return std::string(s);
        };
        const Best& b = a.trainBests.b[L.id];
        std::string best = "YOUR BEST: NOT PASSED YET";
        if (b.passed) {
            if (L.timed()) std::snprintf(buf, sizeof buf, "YOUR BEST: %.1f S - %d / %d (%d%%)", b.time, b.successes, b.attempts, b.permille() / 10);
            else std::snprintf(buf, sizeof buf, "YOUR BEST: %d / %d (%d%%)", b.successes, b.attempts, b.permille() / 10);
            best = buf;
        }
        if (a.trainer.phase == Phase::Card) {
            text(L.teach, 0, 0.18f, 0.028f, white);
            text(L.tip1, 0, 0.11f, 0.021f, white);
            text(L.tip2, 0, 0.065f, 0.021f, white);
            text(goal(), 0, -0.01f, 0.024f, gold);
            text(best, 0, -0.07f, 0.019f, grey);
            text(!a.film.have ? "A VIDEO OF THIS LESSON IS COMING" : a.trainVideo ? "THE VIDEO ON THE RIGHT: A REAL RUN OF THIS LESSON"
                                : "SHOW ME HOW: A VIDEO OF A REAL RUN, IN SLOW MOTION", 0, -0.13f, 0.019f, cyan);
            text("START (OR PRESS " + readyName(a) + (L.timed() ? "): 3-2-1, THEN THE CLOCK RUNS" : ") - ONLY ACCURACY COUNTS HERE"), 0, -0.17f, 0.019f, grey);
        } else if (a.trainer.phase == Phase::Done) {
            const Result& r = a.trainer.result;
            text(r.passed ? "PASSED!" : "NOT THIS TIME", 0, 0.16f, 0.06f, r.passed ? gold : white);
            if (L.timed() && r.passed) { std::snprintf(buf, sizeof buf, "TIME %.1f S", r.time); text(buf, 0, 0.08f, 0.032f, white); }
            std::snprintf(buf, sizeof buf, "ACCURACY %d / %d (%d%%)", r.successes, r.attempts, r.permille() / 10);
            text(buf, 0, 0.03f, 0.028f, white);
            if (a.trainNewBest) text("A NEW BEST!", 0, -0.03f, 0.03f, gold);
            if (!r.passed) text(goal(), 0, -0.03f, 0.022f, grey);
            text(best, 0, -0.09f, 0.019f, grey);
            if (r.passed && nextInTrack(i) >= 0) {
                std::snprintf(buf, sizeof buf, "NEXT: %s - %s", lessons()[nextInTrack(i)].name, lessons()[nextInTrack(i)].teach);
                text(buf, 0, -0.15f, 0.018f, cyan);
            }
            if (r.passed) text(a.net.onlineReady() ? "SENT TO THE LEADERBOARDS" : "KEPT ON THIS HEADSET - SENT WHEN YOU GO ONLINE", 0, -0.20f, 0.016f, grey);
            text("PRESS " + readyName(a) + (r.passed && nextInTrack(i) >= 0 ? " FOR THE NEXT LESSON" : " TO TRY AGAIN"), 0, -0.235f, 0.018f, cyan);
        } else {
            text("PAUSED", 0, 0.178f, 0.028f, white);
        }
    } else if (a.statsNoticeOpen && !a.nameEdit) {
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
    } else if (a.rulesOpen && a.rulesStep == 1 && a.roomBallPage && !a.nameEdit) {   // the room's BALL FEEL
        Tunables t;
        optionsInto(a, t);
        text(kRoomTitle[roomOf(t)], 0, 0.29f, 0.055f, cyan);
        text("BALL FEEL: HOW THE BALL LOOKS AND FEELS FOR EVERYONE AT YOUR ARENA", 0, 0.205f, 0.019f, white);
        text("CORE: A SOLID CENTRE IN THE GLOW - GLOW: TRUE SIZE SHOWS EXACTLY WHAT HITS", 0, -0.10f, 0.017f, grey);
        text("SPIN: IT TURNS AS IT FLIES - WEIGHT: IT TRAILS YOUR HAND, THE CONTROLLER HUMS", 0, -0.14f, 0.017f, grey);
        text("ONLY HOW IT LOOKS AND FEELS: NOTHING CHANGES IN PLAY", 0, -0.20f, 0.017f, grey);
    } else if (a.rulesOpen && a.rulesStep == 1 && a.roundsPad && !a.nameEdit) {   // BEST OF n typed
        Tunables t;
        optionsInto(a, t);
        text(kRoomTitle[roomOf(t)], 0, 0.29f, 0.055f, cyan);
        text("HOW MANY ROUNDS?", 0, 0.225f, 0.022f, white);
        text("BEST OF " + a.roundsBuf + "_", 0, 0.165f, 0.045f, white);
        const int n = padBestOf(a), typed = std::atoi(a.roundsBuf.c_str());
        const float amber[4] = {1.0f, 0.75f, 0.2f, 1};
        if (a.roundsBuf.empty()) text("AN ODD NUMBER, 1 TO 9", 0, -0.24f, 0.02f, grey);
        else if (n > 0) text(n == 1 ? "ONE ROUND DECIDES THE MATCH" : "THE FIRST TO WIN " + std::to_string((n + 1) / 2) + " ROUNDS WINS THE MATCH", 0, -0.24f, 0.02f, grey);
        else if (typed > kMaxBestOf) text("AT MOST BEST OF 9", 0, -0.24f, 0.02f, amber);
        else text("AN ODD NUMBER, SO A MATCH CAN'T END TIED: 1, 3, 5, 7 OR 9", 0, -0.24f, 0.02f, amber);
    } else if (a.rulesOpen && a.rulesStep == 1 && !a.nameEdit) {   // THE ROOM'S OPTIONS
        Tunables t;
        optionsInto(a, t);
        const int room = roomOf(t);
        text(kRoomTitle[room], -0.02f, 0.29f, std::min(0.055f, 0.50f / (0.75f * (float)std::strlen(kRoomTitle[room]))), cyan);   // (clear of DEFAULTS)
        const bool withBot = !a.rulesForHost || a.net.botFill();
        const float yb = botRowY(t);
        text(a.rulesForHost || a.net.mode() == Net::Mode::Host ? "ONLY THE HOST CAN CHANGE THESE" : "FOR PRACTICE AGAINST THE BOT",
             0, 0.225f, 0.02f, white);
        const bool blitz = room == 3;
        const char* hint = rulesLive(a) && a.rulesEdit ? "A MATCH IS ON: THE NEW RULES START WITH THE NEXT MATCH"
                         : blitz ? (t.timed() ? "LAST ONE STANDING WINS - AT TIME THE MOST HEALTH, A TIE: SUDDEN DEATH" : "NO CLOCK: THE LAST ONE STANDING WINS")
                         : !t.shields ? "HOTDOG MODE: NO SHIELDS - DODGE, CATCH AND PUNCH"
                         : t.maidenNow() && room <= 1 ? "MAIDEN: A SHIELD WHILE YOU HOLD YOUR BALL, AND IT NEVER BREAKS"
                         : t.freePlay() ? "FREE PLAY: NO CLOCK AND NO WINNER - PRACTICE OR PLAY ON"
                         : t.advanced && t.advancedRoundTime > 0 ? "AT TIME, MORE POINTS WINS THE ROUND - A TIE GOES TO SUDDEN DEATH"
                         : t.advanced ? (t.roundsToWinNow() == 1 ? "NO TIME LIMIT: FIRST TO 4 POINTS WINS THE MATCH" : "NO TIME LIMIT: FIRST TO 4 POINTS WINS A ROUND")
                         : "AT TIME, MORE POINTS WINS - A TIE GOES TO SUDDEN DEATH";
        text(hint, 0, withBot ? yb - 0.106f : yb + 0.02f, 0.017f, grey);
        if (withBot) {   // what the level means (every level moves as a person can: reach, speed, reaction)
            static const char* kBotLine[kBotLevels] = {
                "SLOW TO REACT, OFTEN MISREADS THE BALL - A GENTLE START",
                "BLOCKS SOME, MISSES CURVES AND BANK SHOTS",
                "A SOLID PLAYER - READS CURVES AND BANKS",
                "QUICK HANDS, ANGLES ITS BLOCKS, PICKS ITS SHOTS",
                "RARELY MISSES - GUARDS ITS STRIKE ZONE",
                "READS EVERY BALL - CAN YOU SCORE ONE?"};
            text(t.botAdapt ? "IT GETS BETTER WHEN YOU SCORE, EASIER WHEN IT DOES - CLOSE MATCHES"
                            : kBotLine[std::max(0, std::min(kBotLevels - 1, t.botLevel))], 0, yb - 0.08f, 0.017f, white);
        }
    } else if (a.rulesOpen && !a.nameEdit) {   // CHOOSE A ROOM (the pictures: after the buttons, below)
        text("CHOOSE A ROOM", 0, 0.29f, 0.055f, cyan);
        text(a.rulesForHost ? "FOR YOUR ROOM - EVERYONE WHO JOINS PLAYS ITS RULES" : "FOR PRACTICE AGAINST THE BOT", 0, 0.222f, 0.02f, white);
    } else if (a.updateOpen && !a.nameEdit) {
        const char* v = entitlement::updateLatest()[0] ? entitlement::updateLatest() : a.net.latestVersion();
        snprintf(buf, sizeof buf, "UPDATE TO %s", v[0] ? v : "THE NEWEST VERSION");
        text(buf, 0, 0.29f, 0.055f, cyan);
        snprintf(buf, sizeof buf, "THIS IS %s", kAppVersion);
        text(buf, 0, 0.215f, 0.022f, white);
        std::string why;
        const int how = updateHow(why);
        if (!why.empty()) text(why, 0, 0.16f, 0.020f, grey);
#ifndef __ANDROID__
        if (how == 0) {
            const updater::State st = updater::state();
            const float amber[4] = {1.0f, 0.75f, 0.2f, 1};
            if (st == updater::IDLE) text("IT DOWNLOADS, CHECKS AND UNPACKS IT; THEN THE GAME CLOSES, UPDATES AND STARTS AGAIN", 0, 0.12f, 0.018f, grey);
            else if (st == updater::DOWNLOADING) { snprintf(buf, sizeof buf, "DOWNLOADING   %d%%", (int)(updater::progress() * 100)); text(buf, 0, 0.12f, 0.026f, white); }
            else if (st == updater::CHECKING) text("CHECKING THE DOWNLOAD", 0, 0.12f, 0.026f, white);
            else if (st == updater::UNPACKING) text("UNPACKING", 0, 0.12f, 0.026f, white);
            else if (st == updater::READY) text("READY: YOUR SETTINGS STAY. RESTART NOW TO FINISH", 0, 0.12f, 0.024f, amber);
            else text("COULD NOT UPDATE: " + updater::error(), 0, 0.12f, 0.022f, amber);
        }
#else
        using U = entitlement::Update;
        const U us = entitlement::updateState();
        const float amber[4] = {1.0f, 0.75f, 0.2f, 1};
        if (how == 0 && us != U::Unavailable) {
            if (us == U::Checking) text("ASKING THE STORE FOR THE NEWEST VERSION", 0, 0.12f, 0.020f, grey);
            else if (us == U::Available) {
                std::string nm = entitlement::updateLatest();
                for (char& c : nm) c = (char)std::toupper((unsigned char)c);
                text("THE STORE HAS " + (nm.empty() ? std::string("IT") : nm) + " READY: IT DOWNLOADS HERE, THEN THE GAME RESTARTS ON IT", 0, 0.12f, 0.018f, grey);
            } else if (us == U::Downloading) {
                snprintf(buf, sizeof buf, "DOWNLOADING   %d%%", (int)(entitlement::updateProgress() * 100.f));
                text(buf, 0, 0.12f, 0.026f, white);
            } else if (us == U::Downloaded) text("DOWNLOADED: INSTALL AND RESTART (YOUR SETTINGS STAY)", 0, 0.12f, 0.022f, amber);
            else if (us == U::Installing) text("INSTALLING: THE GAME CLOSES AND OPENS AGAIN", 0, 0.12f, 0.022f, amber);
            else if (us == U::UpToDate) text("THE STORE HASN'T GOT IT YET: A NEW VERSION CAN TAKE A WHILE TO ARRIVE", 0, 0.12f, 0.018f, grey);
            else if (us == U::Failed) {
                std::string e = entitlement::updateError();
                for (char& c : e) c = (char)std::toupper((unsigned char)c);
                text("COULD NOT UPDATE: " + e, 0, 0.12f, 0.018f, amber);
            }
        } else if (how != 0) {
            text("TO SWITCH TO THE STORE VERSION, UNINSTALL THIS COPY FIRST (A STORE COPY STARTS NEW CAREER STATS)", 0, -0.13f, 0.016f, grey);
        }
#endif
    } else if (a.movePlayersOpen && a.moveHost && !a.nameEdit) {
        text("HAND OFF HOST", 0, 0.29f, 0.055f, cyan);
        text("WHO HOSTS THIS ROOM FROM NOW ON? YOU STAY IN IT", 0, 0.215f, 0.022f, white);
        text("A MATCH PAUSES WHILE THE ROOM CHANGES HANDS, THEN GOES ON", 0, -0.22f, 0.019f, grey);
    } else if (a.movePlayersOpen && !a.nameEdit) {
        text("MOVE PLAYERS", 0, 0.29f, 0.055f, cyan);
        text("POINT AT ONE CORNER, THEN ANOTHER: THEY SWAP", 0, 0.215f, 0.022f, white);
        text(a.game.np == 4 && a.net.mode() == Net::Mode::Host ? "TEAMS GO WITH THE CORNER - THE MATCH STARTS AGAIN"
                                                                : "ONLY WHILE YOU HOST A ROOM FOR FOUR", 0, -0.22f, 0.019f, grey);
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
            snprintf(buf, sizeof buf, "UPDATE AVAILABLE: %s", v);
            text(buf, -0.10f, 0.145f, 0.022f, amber);
        }
    } else if (a.settingsOpen && !a.nameEdit) {
        text(a.btnPage ? "BUTTONS" : a.ctlPage ? "CONTROLLERS" : a.voicePage ? "VOICE CHAT" : a.ptPage ? "PASSTHROUGH" : a.gfxPage ? "GRAPHICS AND VIEW" : "SETTINGS", 0, 0.29f, 0.055f, cyan);
        if (a.btnPage) {   // BUTTONS
            const float amber[4] = {1.0f, 0.75f, 0.2f, 1};
            text("POINT AT A BUTTON TO CHANGE ITS JOB - THE TRIGGER AND MENU STAY AS THEY ARE", 0, 0.205f, 0.018f, white);
            if (nowSeconds() < a.btnNoteUntil) text(a.btnNote, 0, -0.32f, 0.018f, amber);
            else text("A GRIP: TALK, PEEK, NAMES OR NOTHING - TALK, READY AND RECENTER KEEP A BUTTON", 0, -0.32f, 0.016f, grey);
        }
        if (a.ctlPage && !a.btnPage) {
            text("CONTROLLER ANGLE", 0, 0.065f, 0.032f, white);
            text("HOW YOUR HANDS SIT ON THE CONTROLLERS: WATCH THEM AS YOU SET IT", 0, -0.27f, 0.018f, grey);
            text("TILT: UP / DOWN - TWIST: ROLL - TURN: LEFT / RIGHT - THIS HEADSET ONLY", 0, -0.305f, 0.016f, grey);
        }
        if (a.gfxPage) {
            const float strip[4] = {0.004f, 0.007f, 0.016f, 0.985f};
            quad(0, -0.01f, 0.88f, 0.25f, -0.001f, strip);   // a strip behind the slider only: the arena shows around it
            text("FIELD OF VIEW", 0, 0.08f, 0.04f, white);
            text("HOW BIG THE ARENA FEELS: + WIDER, - CLOSER. LOOK AROUND WHILE YOU SET IT", 0, 0.03f, 0.019f, grey);
            for (int k = -5; k <= 5; ++k) {   // the slider: a notch for each step, the chosen one lit
                const float on[4] = {0.3f, 0.85f, 1.0f, 1}, off[4] = {0.10f, 0.16f, 0.22f, 1};
                quad(0.032f * k, -0.10f, k == a.roomFeel ? 0.022f : 0.012f, k == a.roomFeel ? 0.04f : 0.022f, 0.004f, k == a.roomFeel ? on : off);
            }
        }
        if (a.voicePage && !a.btnPage) text("ONLY PEOPLE AT YOUR ARENA CAN HEAR YOU", 0, 0.205f, 0.024f, grey);
        if (a.ptPage) {
            std::string h = kPtModeHelp[a.ptMode % PT_COUNT];
            if (a.ptMode == PT_WINDOW)   // what hides the real things inside the court (environment depth)
                h = !a.hasDepth ? "WINDOW: YOUR ROOM THROUGH THE GLASS (NO DEPTH ON THIS HEADSET)"
                    : a.depthRunning ? "WINDOW: THINGS INSIDE THE COURT HIDDEN (ROOM DEPTH ON)"
                    : !a.scenePerm ? "WINDOW: ALLOW SPATIAL DATA TO HIDE THINGS INSIDE THE COURT"
                    : a.depthFailed ? "WINDOW: ROOM DEPTH COULD NOT START" : "WINDOW: STARTING ROOM DEPTH";
            text(h, 0, 0.205f, 0.022f, grey);
        }
    } else if (a.helpOpen && a.creditsOpen && !a.nameEdit) {   // HOW TO PLAY > CREDITS
        text("CREDITS", 0, 0.29f, 0.055f, cyan);
        float y = 0.205f;
        for (const CreditRow& r : kCredits) {
            if (r.kind == CR_SMALL && y > -0.20f) y = -0.20f;   // (the components and licences: at the foot)
            text(r.text, 0, y, r.kind == CR_HEAD ? 0.016f : r.kind == CR_NAME ? 0.025f : 0.014f, r.kind == CR_NAME ? white : grey);
            y -= r.kind == CR_HEAD ? 0.028f : r.kind == CR_NAME ? 0.045f : 0.028f;
        }
    } else if (a.helpOpen && !a.nameEdit) {
        text("HOW TO PLAY", 0, 0.29f, 0.055f, cyan);
        // (BUTTONS: the names' and recenter's buttons as they are; the standard: LEFT STICK CLICK, B / RIGHT STICK CLICK)
        const std::string names = jobButtons(a, controls::J_NAMES, true), rec = jobButtons(a, controls::J_RECENTER, true);
        const std::string l7 = names.empty() ? std::string("PLAYERS' NAMES: NO BUTTON (SETTINGS > CONTROLLERS)") : names + ": PLAYERS' NAMES";
        const std::string l8 = rec + ": RECENTER   MENU: GAMES";
        const char* lines[] = {
            "GRAB: REACH FOR YOUR BALL, HOLD THE TRIGGER",
            "THROW: SWING AND RELEASE - FLICK TO CURVE IT",
            "HIT YOUR OPPONENT TO SCORE",
            "HOLDING A CHARGED BALL RAISES A SHIELD",
            "PUNCH BALLS AWAY WITH YOUR KNUCKLE GUARD",
            "BALL THROUGH THEIR RING = STRIKE: BIGGER, FASTER",
            "THROW PAST YOUR FOUL LINE TO RECHARGE",
            l7.c_str(),
            l8.c_str()};
        for (int i = 0; i < 9; ++i) text(lines[i], 0, 0.20f - 0.055f * i, 0.024f, i >= 7 ? grey : white);
    } else if (a.nameEdit) {
        text(a.editTarget == 5 ? "REPORT NOTE" : a.editTarget == 4 ? "BUG NOTE" : a.editTarget == 2 ? "TEAM NAME" : a.editTarget ? "COMMUNITY SERVER" : "YOUR NAME", 0, 0.29f, 0.055f, cyan);
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
            snprintf(buf, sizeof buf, "YOU: %s   -   %s   -   %s", a.net.name(), a.net.status(), roomName(a.game.tun).c_str());
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
                            (it.act == MenuAct::RulesPick && (int)it.ip == (a.optAdvanced ? 1 : 0)) ||
                            (it.act == MenuAct::RoomPick && (int)it.ip == roomOf(a.game.tun)) || (it.act == MenuAct::ReportWho && it.ip == a.reportWho) ||
                            (it.act == MenuAct::ReportWhy && (int)it.ip == a.reportWhy) || (it.act == MenuAct::MoveCorner && (int)it.ip == a.moveSel) || (it.act == MenuAct::PtMode && (int)it.ip == a.ptMode) ||
                            (it.act == MenuAct::BtnPreset && controls::sameMap(a.btnMap, it.ip ? controls::leftHanded() : controls::standard())) ||
                            (it.act == MenuAct::OptBot && it.ip >= 10 &&
                             (int)it.ip - 10 <= (a.optBotAdapt ? (int)std::lround(a.game.tun.botMatch) : a.optBotLevel)) ||
                            (it.act == MenuAct::OptBot && it.ip == 2 && a.optBotAdapt) ||
                            (it.act == MenuAct::StudioSection && (int)it.ip == a.studioSection) || (it.act == MenuAct::StudioVote && studioVoteLit(a, it.ip));
        quad(it.x, it.y, it.w, it.h, 0.003f, !it.enabled && !row ? dis : ((int)i == a.menuHover || picked) ? hov : btn);
        float th = row ? 0.026f : 0.034f;
        const float tw = th * 0.75f * (float)it.label.size();          // the font's glyphs are 0.75 x their height
        if (tw > it.w * 0.90f) th *= it.w * 0.90f / tw;                 // long labels shrink instead of spilling over
        text(it.label, it.x, it.y, th, it.enabled || row ? white : grey);
    }
    if (a.rulesOpen && a.rulesStep == 0 && !a.nameEdit) {   // CHOOSE A ROOM: each room from above, its name, who it's for
        // (who each is for; how it plays is the room's options, the owner: the match's format is one of them)
        static const char* kLine[4] = {"TWO PLAYERS", "TWO PLAYERS", "FOUR PLAYERS", "UP TO FOUR PLAYERS"};
        const GLuint pics = a.gfx.texture("room_pictures", true);   // (tools/make_room_pictures.py: a quarter each)
        const float img[4] = {1, 1, 1, 1};
        // (raised buttons, the owner 2026-11-17: each stands out from the panel, its face 1.1 cm proud (1.6 lit: pointed at, or
        // the room now), a shadow on the panel below and to the right, its top and left edges lit, the others shaded)
        const float shadow[4] = {0.f, 0.f, 0.f, 0.6f}, edgeHi[4] = {0.35f, 0.65f, 0.95f, 1.f}, edgeLo[4] = {0.008f, 0.015f, 0.03f, 1.f};
        for (int k = 0; k < 4; ++k) {
            const float x = k % 2 ? 0.205f : -0.205f, y = k < 2 ? 0.07f : -0.195f, w = 0.39f, h = 0.25f;
            const bool lit = a.menuHover == k || roomOf(a.game.tun) == k;   // (the first four items: these buttons)
            const float z = lit ? 0.016f : 0.011f;
            quad(x + 0.010f, y - 0.012f, w, h, 0.0015f, shadow);
            quad(x, y, w, h, z - 0.002f, lit ? hov : btn);
            quad(x, y + h * 0.5f - 0.004f, w, 0.008f, z - 0.001f, edgeHi);
            quad(x - w * 0.5f + 0.004f, y, 0.008f, h, z - 0.001f, edgeHi);
            quad(x, y - h * 0.5f + 0.004f, w, 0.008f, z - 0.001f, edgeLo);
            quad(x + w * 0.5f - 0.004f, y, 0.008f, h, z - 0.001f, edgeLo);
            const float uv[4] = {0.5f, 0.5f, 0.5f * (float)(k % 2), 0.5f * (float)(k / 2)};
            // (the words higher, clear of the button's bottom edge: the owner, 11-17; the picture a little smaller and higher)
            if (pics) a.gfx.drawTexturedPanel(Mat4::fromPose({a.menuPose.q, at(x, y + 0.032f, z)}, {0.29f, 0.18125f, 1}), pics, img, false, uv);
            a.gfx.drawText(kRoomTitle[k], {a.menuPose.q, at(x, y - 0.078f, z + 0.001f)}, 0.025f, white);
            a.gfx.drawText(kLine[k], {a.menuPose.q, at(x, y - 0.100f, z + 0.001f)}, 0.014f, white);
        }
    }
    if (games == 0 && !a.ownFail && !a.nameEdit && !a.helpOpen && !a.titleOpen && !a.settingsOpen && !a.xpOpen && !a.bugOpen && !a.reportOpen && !a.rulesOpen && !a.movePlayersOpen && !a.updateOpen &&
        !a.statsOpen && !a.statsNoticeOpen && !a.trainOpen && a.trainBoard < -1 && !(a.trainer.active() && !otherPage(a))) {
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
    if (!a.statsOpen && !a.trainOpen && a.trainBoard < -1 && !(a.trainer.active() && !otherPage(a))) text("POINT AND PULL THE TRIGGER TO SELECT", 0, -0.455f, 0.022f, grey);
    if ((a.titleOpen || a.settingsOpen || a.updateOpen) && !a.nameEdit) {   // the version, bottom right (owner, 2026-10-30)
#ifdef __ANDROID__
        snprintf(buf, sizeof buf, "VERSION %s", kAppVersion);
#else
        std::string why;
        snprintf(buf, sizeof buf, "VERSION %s%s", kAppVersion, updater::how(why) == 1 ? " STEAM" : "");   // (a Steam copy)
#endif
        text(buf, 0.37f, -0.487f, 0.016f, grey);
    }
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

// THE PUNCH LANDS: the flash where the ball met the knuckles, and that ball squashed along its new way (power 0.2 .. 1,
// from how fast the punch sent it: a still fist's 7 m/s to 16)
static void addPunchFx(App& a, const GameEvent& e, float power) {
    App::PunchFx f;
    f.pos = e.pos; f.t = 0; f.power = power; f.team = e.player & 1;
    if (a.punches.size() < 6) a.punches.push_back(f);
    int b = e.ball;   // (a guest isn't sent which ball: the nearest)
    if (b < 0 || b >= a.game.np) {
        float best = 1e9f;
        for (int q = 0; q < a.game.np; ++q) {
            const float d = length(a.game.pl[q].ball.pos - e.pos);
            if (d < best) { best = d; b = q; }
        }
    }
    if (b >= 0 && b < kMaxPlayers) {
        const Vec3 v = a.game.pl[b].ball.vel;
        a.squash[b].t = 0; a.squash[b].power = power; a.squash[b].axis = length(v) > 1e-3f ? normalize(v) : Vec3{0, 1, 0};
    }
}

static void updateFx(App& a, float dt) {
    for (size_t i = 0; i < a.punches.size();) {
        a.punches[i].t += dt;
        if (a.punches[i].t > 0.2f) { a.punches[i] = a.punches.back(); a.punches.pop_back(); } else ++i;
    }
    for (App::Squash& s : a.squash) s.t += dt;
    // BALL FEEL (TEST): each ball's drawn turn (SPIN VISIBLE: its spin, and flying a tumble at a quarter of a rolling
    // ball's rate) and where a held one is drawn (WEIGHT HEAVY: following the hand 12 ms behind, at most 3 cm)
    for (int p = 0; p < a.game.np && p < kMaxPlayers; ++p) {
        const Ball& b = a.game.pl[p].ball;
        Vec3 w = b.spin;
        const float sp = length(b.vel);
        if (b.state == BallState::Flying && sp > 0.5f) {
            const Vec3 roll = cross(Vec3{0, 1, 0}, b.vel / sp);
            if (length(roll) > 1e-3f) w += normalize(roll) * (sp / std::max(0.05f, a.game.ballRadius(p)) * 0.25f);
        }
        const float wl = length(w);
        if (wl > 1e-3f && b.state != BallState::Held) a.ballQ[p] = nlerp(Quat{}, Quat::axisAngle(w / wl, wl * dt) * a.ballQ[p], 1.f);   // (renormalized)
        if (b.state == BallState::Held) {
            if (!a.heldVisOn[p]) a.heldVis[p] = b.pos;
            a.heldVis[p] += (b.pos - a.heldVis[p]) * (1.f - std::exp(-dt / 0.012f));
            const Vec3 off = a.heldVis[p] - b.pos;
            if (length(off) > 0.03f) a.heldVis[p] = b.pos + normalize(off) * 0.03f;
            a.heldVisOn[p] = true;
        } else {
            a.heldVisOn[p] = false;
        }
    }
    for (App::Mark& m : a.marks) m.age += dt;   // BALL MARKS fade out
    a.marks.erase(std::remove_if(a.marks.begin(), a.marks.end(), [](const App::Mark& m) { return m.age > m.life; }), a.marks.end());
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

// THE PUNCH LANDS: a white-hot flash, quick to cool to the puncher's colour, and a wider glow rolling out (0.18 s)
static void drawPunchFx(App& a, const App::PunchFx& f) {
    const float team[2][3] = {{0.1f, 0.7f, 1.0f}, {1.0f, 0.4f, 0.1f}};
    const float* tc = team[f.team];
    const float u = f.t / 0.18f;
    if (u >= 1.f) return;
    const float lvl = a.fxLevel == 0 ? 1.f : a.fxLevel == 1 ? 0.55f : 0.25f;
    const float k = (1.f - u) * (1.f - u) * lvl * (0.6f + 0.6f * f.power);
    float core[4], glow[4];
    for (int i = 0; i < 3; ++i) {
        core[i] = (tc[i] * u + (1.f - u)) * 1.4f * k;
        glow[i] = tc[i] * 0.8f * k;
    }
    core[3] = glow[3] = 1.f;
    a.gfx.drawSprite(f.pos, (0.10f + 0.14f * f.power) * (0.5f + 0.8f * easeOut(u * 1.6f)), core, a.viewHead.p, 0);
    a.gfx.drawSprite(f.pos, (0.22f + 0.30f * f.power) * easeOut(u), glow, a.viewHead.p, 0);
}

static void drawFx(App& a) {
    const int L = a.net.localIndex();
    for (const App::HitFx& h : a.hits) drawHitFx(a, h, L);
    for (const App::PunchFx& f : a.punches) drawPunchFx(a, f);
    for (int p = 0; p < a.game.np; ++p) drawSpawnFx(a, p);
}

// the host counts each match for the community server's statistics (StatCounter, game.h: throws, throws on target,
// hits and the points from returns, blocks, on-target balls faced, catches, the top throw speed); sent when a match
// between two people ends
static std::array<uint8_t, 4> finalScore(const Game& g) {   // what both players' games agree on (CONFIRM)
    auto c = [](int v) { return (uint8_t)std::max(0, std::min(255, v)); };
    return {c(g.pl[0].score), c(g.pl[1].score), c(g.pl[0].roundWins), c(g.pl[1].roundWins)};
}

static void countStat(App& a, const GameEvent& e) {
    if (a.game.np != 2) return;   // (FOUR CORNERS: careers compare one-on-one matches)
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

static void addMark(App& a, const Vec3& at, int player, float speed);
// your ball in a hand's reach (game.h ballInReach): it brightens, and that hand gets a light tick as it comes in reach
// (two players' report, 2026-11-17: "hard to tell where the hitbox is for picking up balls")
static void reachCue(App& a) {
    const int L = a.net.localIndex();
    for (int h = 0; h < 2; ++h) {
        const bool in = L >= 0 && L < a.game.np && !a.viewing && !a.spectating && a.game.pl[L].in.hand[h].trigger < 0.5f &&
                        a.game.ballInReach(L, h);
        if (in && !a.reachCue[h]) haptic(a, h, 0.25f, 0.03f);
        a.reachCue[h] = in;
    }
    // BALL FEEL WEIGHT HEAVY: holding your ball, the controller hums as you swing it (from 1 m/s, stronger faster)
    if ((a.game.tun.ballFeel & BALL_WEIGHT) && L >= 0 && L < a.game.np && !a.viewing && !a.spectating) {
        const Ball& b = a.game.pl[L].ball;
        if (b.state == BallState::Held && b.heldHand >= 0 && b.heldHand < 2) {
            const float sp = length(a.game.recentHandVel(L, b.heldHand, a.game.tun.holdOffset, a.game.now));
            const float amp = clampf((sp - 1.f) / 6.f, 0.f, 0.6f);
            if (amp > 0.04f) haptic(a, b.heldHand, amp, 0.025f);
        }
    }
}

// a hit, and how the ball got there: each ball's bounces since it was last thrown or touched (the bounces themselves
// are too many for the log), with the bot's read of it when a bot is hit (the owner's practice, 2026-11-17: UNBEATABLE
// hit by its own ball, punched back past it, 2-3 s later from behind; not found in tests)
static void logHitTrail(App& a, const GameEvent& e) {
    static Vec3 trail[kMaxPlayers][8];
    static int count[kMaxPlayers] = {};
    const int own = e.type == EventType::Bounce || e.type == EventType::Throw || e.type == EventType::Spawn ? e.player : e.ball;
    if (own < 0 || own >= kMaxPlayers) return;
    if (e.type == EventType::Bounce) {
        if (count[own] < 8) trail[own][count[own]] = e.pos;
        ++count[own];
        return;
    }
    if (e.type == EventType::Throw || e.type == EventType::Spawn || e.type == EventType::KnuckleBlock ||
        e.type == EventType::ShieldBlock || e.type == EventType::Caught) {   // (thrown or touched: its trail starts again)
        count[own] = 0;
        return;
    }
    if (e.type != EventType::HitPlayer) return;
    char b[400];
    int n = std::snprintf(b, sizeof b, "hit trail: ball %d into player %d after %d bounces:", own, e.player, count[own]);
    for (int i = 0; i < std::min(8, count[own]) && n < (int)sizeof b - 32; ++i)
        n += std::snprintf(b + n, sizeof b - n, " (%.2f %.2f %.2f)", trail[own][i].x, trail[own][i].y, trail[own][i].z);
    if (e.player >= 0 && e.player < a.game.np && a.game.pl[e.player].isBot) {
        const PlayerState& s = a.game.pl[e.player];
        n += std::snprintf(b + n, sizeof b - n, "; its read of its own ball: mode %d plan %d catch %d hitT %+.2f, of the other: mode %d plan %d",
                           s.botRead[1].mode, s.botRead[1].plan, s.botRead[1].catchIt ? 1 : 0,
                           s.botRead[1].hitT >= 0 ? s.botRead[1].hitT - a.game.now : -1.f, s.botRead[0].mode, s.botRead[0].plan);
    }
    LOGI("%s", b);
    count[own] = 0;
}

static void handleEvents(App& a) {
    const int L = a.net.localIndex();
    for (GameEvent e : a.game.events) {
        if (e.type == EventType::Bounce) addMark(a, e.pos, e.player, e.value);
        bool mine = !a.viewing && e.player == L;
        if (a.net.mode() == Net::Mode::Host && a.net.online()) countStat(a, e);
        if (e.type == EventType::MatchEnd && a.net.mode() == Net::Mode::Client && a.net.online() && a.net.localSlot() >= 0 &&
            a.game.np == 2 && e.player >= 0 && e.player < 2)
            a.net.confirmMatch(e.player, finalScore(a.game).data());   // (the host's result counts once we agree)
        if (e.type != EventType::Bounce && e.type != EventType::Countdown)
            LOGI("event %s player %d hand %d value %.2f pos (%.2f %.2f %.2f) spin %.1f", eventName(e.type), e.player, e.hand, e.value,
                 e.pos.x, e.pos.y, e.pos.z, e.player >= 0 ? length(a.game.pl[e.player].ball.spin) : 0.f);
        logHitTrail(a, e);
        switch (e.type) {
        // haptics: strong and long enough to feel through the Touch controllers' motors
        case EventType::Grab: if (mine && e.hand >= 0) haptic(a, e.hand, 0.85f, 0.12f); break;
        case EventType::Caught: if (mine && e.hand >= 0) haptic(a, e.hand, 1.0f, 0.18f); break;
        case EventType::ShieldBlock: if (mine && e.hand >= 0) haptic(a, e.hand, 1.0f, 0.28f); break;
        case EventType::ShieldBreak: if (mine && e.hand >= 0) haptic(a, e.hand, 1.0f, 0.35f); break;
        case EventType::KnuckleBlock: {   // THE PUNCH LANDS: a short hard knock in that hand (was 0.2 s), a flash, a squash
            const float power = clampf((e.value - 6.f) / 10.f, 0.2f, 1.f);
            if (mine && e.hand >= 0) haptic(a, e.hand, 1.0f, 0.05f + 0.05f * power);
            addPunchFx(a, e, power);
            break;
        }
        case EventType::Throw: if (mine && e.hand >= 0) haptic(a, e.hand, 0.6f, 0.08f); break;
        case EventType::ThrowFail: if (mine && e.hand >= 0) haptic(a, e.hand, 0.4f, 0.06f); break;
        case EventType::Foul: a.foulFlash = 1.2f; a.foulAt = e.pos; break;
        case EventType::LeadChange: a.leadFlash = 2.5f; break;
        case EventType::Knockout: {   // FIREBALL BLITZ: who is out (you: in big letters, a long buzz)
            static const char* kOut[4] = {"BLUE IS OUT", "ORANGE IS OUT", "GREEN IS OUT", "VIOLET IS OUT"};
            const bool me = e.player == L && !a.viewing && !a.spectating;
            a.octMsg = me ? "YOU'RE OUT" : kOut[e.player & 3];
            a.octMsgP = e.player; a.octMsgT = 2.0f;
            if (me) { haptic(a, 0, 1, 0.6f); haptic(a, 1, 1, 0.6f); }
            break;
        }
        case EventType::RingClose: a.octMsg = "THE ROOM CLOSES IN"; a.octMsgP = -1; a.octMsgT = std::max(1.f, e.value); break;
        case EventType::Fizzle: if (a.puffs.size() < 32) a.puffs.push_back({e.pos, 0.f, e.player}); break;
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
        // the listener's side and right hand (it was always player 0's: the orange player heard their hits as the
        // other player's, and their left and right swapped)
        const Game& g = a.game;
        const bool ours = e.player >= 0 && (e.type == EventType::HitPlayer  ? e.player == L
                                          : e.type == EventType::MatchEnd   ? e.player == g.team(L)   // (the winning team)
                                          : e.type == EventType::RoundEnd   ? g.team(e.player) == g.team(L) : false);
        Vec3 right;
        if (a.viewing) { const Vec3 r = a.viewHead.q.rotate({1, 0, 0}); right = normalize(Vec3{r.x, 0, r.z}); }
        else { const Vec3 f = g.facing(L); right = {-f.z, 0, f.x}; }
        a.audio.onEvent(e, a.viewing ? a.viewHead.p : g.pl[L].in.head.p, ours, right);
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
    L.pattern = d.pattern < 8 ? a.patternTex[avatar::bodyType(d)][d.pattern] : 0;
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
        a.titleOpen = a.settingsOpen = a.statsOpen = a.reportOpen = a.bugOpen = a.helpOpen = a.rulesOpen = a.movePlayersOpen = false;
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
            if (c->hype[p] >= 60 && !a.crowdCheered[p]) { a.crowdCheered[p] = true; if (sound) { const std::string clip = a.audio.officialClip("Cheer"); a.audio.play(clip.empty() ? "oa_Cheer" : clip, 0.8f, 0.f, false, true); } }
            if (c->hype[p] < 35) a.crowdCheered[p] = false;
        }
    }
    // the fans' cheering: a short, quiet crowd blip when they cheer, at most one every 1.5 s, a little louder with their
    // hype (owner, 2026-11-12: the crowd loop that ran under the whole match, louder with the hype, was annoying; S29
    // is a blip now). Under voices: Audio's CROWD UNDER VOICES
    if (c && sound && c->fans > 0 && (c->cheers[0] > 0 || c->cheers[1] > 0) && c->at != a.crowdBlipFor) {
        a.crowdBlipFor = c->at;
        const double now = nowSeconds();
        if (now - a.crowdBlipAt >= 1.5) {
            a.crowdBlipAt = now;
            const float hype = std::max(c->hype[0], c->hype[1]) / 100.f;
            const std::string clip = a.audio.officialClip("Crowd");
            a.audio.play(clip.empty() ? "oa_Crowd" : clip, 0.35f + 0.4f * hype, 0.f, false, true);
        }
    }
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
    const int b = avatar::bodyType(d), g = d.headgear < 9 ? d.headgear : 0, v = d.visor < 8 ? d.visor : 0;
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
    const int b = avatar::bodyType(d), g = d.headgear < 9 ? d.headgear : 0;
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

// the builder's (and the tuning tool's) mirror's own rig state and hair swing: past every other draw's key (the room's
// members are 1000 + up to 0xFFFFF). Keyed as your court place it was the court player's too: in the lobby the bot
// playing in that place below the balcony stepped it first each frame, and the reflection turned, swayed (TENNIS SKIRT,
// KILT) and sized its arms from the bot's moves, not yours (the owner, 2026-11-17: on the balcony the mirror breaks,
// the skirt doesn't swish)
constexpr int kMirrorRig = 0x200000;

// Body (torso, neck, head and its parts, arms) for one player; `pre` places it (miniatures, the builder mirror),
// preMirror when `pre` has a negative determinant. Hands are drawn by the caller.
static void drawAvatarBody(App& a, const PlayerInput& in, int team, int pass, const Mat4& pre, bool preMirror, bool lod,
                           float flash, int swingKey = -1) {
    avatar::RigState& rs = a.rigStates[swingKey >= 0 ? swingKey : team];   // (THE REACH: its own, once a frame)
    if (a.shotArmScale > 0) rs.armScale = a.shotArmScale;   // (pictures: sized before the posture is worked out)
    avatar::stepRig(rs, in, in.avatar, nowSeconds());
    if (a.shotArmScale > 0) rs.armScale = a.shotArmScale;
    avatar::Rig rig = avatar::solveRig(in, in.avatar, &rs);
    // NATIONS: the country's flag printed high on the jersey's back, just under the collar (owner, 2026-10-29: on the
    // number panel it sat at the waist), and small on the left chest. Printed on the cloth itself (bind pose boxes),
    // so it bends and turns with it (GitHub-free report, 2026-11-07: as a flat card it stuck out like paper)
    int nationDecals = 0;
    Mat4 nationBox[2];
    GLuint nationTex = 0;
    if (!lod && avatar::nationOf(in.avatar)) {
        const bool fem = avatar::bodyType(in.avatar) == 1;
        auto box = [](const Pose& at, float w, float h, float depth) {   // bind space -> the unit box at `at`
            const Pose inv{at.q.conj(), at.q.conj().rotate(-at.p)};
            return Mat4::fromPose({Quat{}, {0, 0, 0}}, {1.f / w, 1.f / h, 1.f / depth}) * Mat4::fromPose(inv);
        };
        const Pose back{Quat::axisAngle({1, 0, 0}, deg2rad(15.f)), fem ? Vec3{0, -0.11f, 0.090f} : Vec3{0, -0.12f, 0.125f}};
        const Pose crest{Quat::axisAngle({0, 1, 0}, kPi) * Quat::axisAngle({1, 0, 0}, -deg2rad(fem ? 45.f : 18.f)),
                         fem ? Vec3{-0.08f, -0.118f, -0.147f} : Vec3{-0.08f, -0.150f, -0.131f}};
        nationBox[0] = box(back, 0.13f, 0.13f / 1.5f, 0.16f);   // (deep enough for every build's back, short of the chest)
        nationBox[1] = box(crest, 0.055f, 0.055f / 1.5f, 0.10f);
        nationTex = a.gfx.texture(avatar::nation(avatar::nationOf(in.avatar))->flag, true);
        nationDecals = 2;
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
        float swing4[4] = {0, 0, 0, 0};
        applyLook(dp, L);
        dp.mirrored = p.mirrored != preMirror;
        if (flash > 0) { dp.emissiveAdd[0] = flash; dp.emissiveAdd[1] = flash * 0.3f; }
        if (p.flags & avatar::PART_NEON)
            for (int k = 0; k < 3; ++k) dp.emissiveAdd[k] += L.ch[avatar::CH_GLOW][k] * 0.35f;
        if (p.flags & avatar::PART_SKINNED) {
            dp.bones = rig.skin;
            if (rig.bent) dp.pelvis = &rig.pelvis;   // (THE REACH: the waist)
            if (rig.twisted && !a.noTwist) dp.twist = rig.twist;   // (FOREARM TWIST)
            auto sw = a.swishShapes.find(it->first);
            if (sw != a.swishShapes.end()) {   // (TENNIS SKIRT / KILT: its swing, and the hem lifted as it swings out)
                const avatar::Swish& s = rs.swish;
                const float x = a.shotSwishOn ? a.shotSwish[0] : s.x, z = a.shotSwishOn ? a.shotSwish[1] : s.z;
                const float len = std::max(0.1f, (sw->second.waistY - sw->second.hemY) * 0.6f);
                swing4[0] = x; swing4[1] = z; swing4[2] = a.shotSwishOn ? a.shotSwish[2] : s.twist;
                swing4[3] = std::min(0.018f, (x * x + z * z) / (2.f * len));
                dp.swish = swing4;
                dp.swishShape = &sw->second;
            }
        }
        if (nationDecals && (p.flags & avatar::PART_SKINNED) && !std::strncmp(p.model, "av_top_", 7)) {   // (the jersey)
            dp.decals = nationDecals;
            dp.decal[0] = nationBox[0];
            dp.decal[1] = nationBox[1];
            dp.decalTex = nationTex;
        }
        if (p.flags & avatar::PART_PULSE) dp.uvOff[1] = wrapUV(-(nowSeconds() - a.startTime) * 1.6);   // ~40 cm/s
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
                dp.pelvis = nullptr;   // (a hair rig's bone 0 is the head: no waist)
                dp.twist = nullptr;    // (nor forearms)
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

// the builder's mirror (0.85 m ahead, on the floor you stand on) and its panel, in front of the head; again when the
// view recenters while it's open (the owner, 2026-11-17: opened at load-in, before the first recenter, the mirror faced
// another way and stayed so)
static void placeBuilder(App& a, const Pose& head) {
    Vec3 f = head.q.rotate({0, 0, -1});
    f.y = 0;
    f = length(f) > 1e-3f ? normalize(f) : Vec3{1, 0, 0};
    const Vec3 right = cross(f, Vec3{0, 1, 0});
    a.mirrorNormal = -f;
    // (on the floor you stand on: on the balcony its glass and ring were drawn on the court, 2.5 m below; owner, 2026-11-14)
    a.mirrorPoint = Vec3{head.p.x, floorHere(a), head.p.z} + f * 0.85f;
    Vec3 pp = head.p + f * 0.95f + right * 0.66f + Vec3{0, -0.30f, 0};
    Vec3 to = head.p - pp;
    to.y = 0;
    to = normalize(to);
    a.builderPose = {Quat::axisAngle({0, 1, 0}, std::atan2(to.x, to.z)), pp};
}

static void openBuilder(App& a, const PlayerInput& in) {
    placeBuilder(a, in.head);
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
        // the fist turned 30 degrees about the knuckles, in line with the cuff and the forearm like the hands (owner,
        // 2026-11-16: "make the fist line up like the hands"; the controller's tilt had turned it 30 degrees outward
        // from the cuff, seen from above with the palm down), about the knuckle bumper (Tunables::knuckleOffset); then
        // 2 cm toward the thumb and 2.5 cm back into the cuff's opening (her marks: no gap between fist and cuff), the
        // cuff's tuning moving the rest of the way (avatar.h). Drawn only: the bumper stays where it was
        a.gfx.draw(a.gauntletHand[k], gm * Mat4::fromPose(avatar::fistPose()) * g_lancerLocal, dp, pass);
        a.gfx.draw(a.gauntletCuff[k], gm * Mat4::fromPose(avatar::cuffPose(false)) * g_lancerLocal, dp, pass);
        return;
    }
    a.gfx.draw(gauntletOf(a, d), gm * g_lancerLocal, dp, pass);
}

// HANDS APART (owner, 2026-11-16, her video: with the controllers held together, the gauntlets crossed into each
// other): one player's input with the two drawn hands moved apart, half each, until their gauntlets (`look`'s, as
// drawGauntlet draws them) only touch; at most 3 cm each. Drawing only (the body's arms follow): the game, the ball
// and what is sent keep the tracked hands, so every version is unaffected
static PlayerInput handsApart(const App& a, const PlayerInput& in, const AvatarDesc& look) {
    if (a.noApart || !a.handShapesReady.load(std::memory_order_acquire) || !in.hand[0].tracked || !in.hand[1].tracked ||
        length(in.hand[0].grip.p - in.hand[1].grip.p) > 0.40f)
        return in;
    const int k = avatar::gauntletOf(look);
    const bool plate = k < 3 && a.gauntletHand[k].valid() && a.gauntletCuff[k].valid();
    if (!plate && k < 3 && a.gauntlets[k].valid()) return in;   // (a whole plate gauntlet: no shape for it)
    const avatar::HandShape* fist = plate ? &a.handShape[k][0] : &a.handShape[3][0];
    const avatar::HandShape* cuff = plate ? &a.handShape[k][1] : nullptr;
    if (!fist->valid()) return in;
    const int style = plate ? k : 3;
    auto same = [](const Pose& x, const Pose& y) {
        return x.p.x == y.p.x && x.p.y == y.p.y && x.p.z == y.p.z && x.q.x == y.q.x && x.q.y == y.q.y && x.q.z == y.q.z && x.q.w == y.q.w;
    };
    const App::ApartMemo* memo = nullptr;
    for (const App::ApartMemo& m : a.apartMemo)
        if (m.style == style && same(m.grip[0], in.hand[0].grip) && same(m.grip[1], in.hand[1].grip)) { memo = &m; break; }
    Vec3 S{0, 0, 0};
    if (memo) {
        S = memo->shift;
    } else {
        const int parts = cuff && cuff->valid() ? 2 : 1;
        avatar::HandPart P[2][2];
        for (int h = 0; h < 2; ++h) {
            Mat4 mirror = Mat4::identity();
            if (h == 0) mirror.m[0] = -1;
            const Mat4 gm = Mat4::fromPose(in.hand[h].grip) * mirror;
            P[h][0].shape = fist;
            P[h][0].place = plate ? gm * Mat4::fromPose(avatar::fistPose()) : gm;
            P[h][1].shape = cuff;
            P[h][1].place = gm * Mat4::fromPose(avatar::cuffPose(false));
            for (avatar::HandPart& p : P[h]) p.inv = avatar::affineInverse(p.place);
        }
        S = avatar::handsApartShift(P[0], parts, P[1], parts, 0.06f);
        App::ApartMemo& m = a.apartMemo[a.apartNext];
        a.apartNext = (a.apartNext + 1) % 8;
        m.grip[0] = in.hand[0].grip; m.grip[1] = in.hand[1].grip; m.style = style; m.shift = S;
    }
    if (length(S) < 1e-5f) return in;
    PlayerInput out = in;
    out.hand[0].grip.p = out.hand[0].grip.p - S * 0.5f;
    out.hand[1].grip.p = out.hand[1].grip.p + S * 0.5f;
    return out;
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
    const PlayerInput in = handsApart(a, a.localIn, a.localIn.avatar);
    const Mat4 R = reflectMat(a.mirrorPoint, a.mirrorNormal);
    drawAvatarBody(a, in, L, pass, R, true, false, 0, kMirrorRig);   // (its own rig, not your court place's)
    {   // (the arms' sizing in the log as it changes: what a headset test did, not a guess from its video)
        const float s = a.rigStates[kMirrorRig].armScale;
        if (std::fabs(s - a.mirrorArmsLogged) >= 0.04f) { LOGI("avatar builder: arms x%.2f", s); a.mirrorArmsLogged = s; }
    }
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
    Vec3 foot{in.head.p.x, a.mirrorPoint.y + 0.004f, in.head.p.z};
    Vec3 rf = foot - n * (2.f * dot(foot - a.mirrorPoint, n));
    const float ring[4] = {look.rim[0] * 0.5f, look.rim[1] * 0.5f, look.rim[2] * 0.5f, 1};
    a.gfx.drawSpriteFlat({rf.x, a.mirrorPoint.y + 0.006f, rf.z}, 1.1f, ring, {0, 1, 0}, 1);
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

// the knuckle glass follows the bumper: the knuckleguard model is authored round the original bumper centre
// (make_props.py knuckleguard), the bumper is wherever Tunables::knuckleOffset puts it
static Mat4 knuckleShift(const Game& g) {
    static const Vec3 kGlassCentre{-0.022f, 0.010f, -0.137f};
    return Mat4::fromPose({Quat{}, g.tun.knuckleOffsetNow() - kGlassCentre});
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
            a.gfx.draw(a.shield, R * grip * Mat4::fromPose(g.shieldLocal(h)) * shieldSizeM(g) * g_plateLocal, sp, pass);
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
    // the skeleton, in the mirror (you have no body of your own): neck, shoulders, elbows, wrists (the mirror's rig)
    const auto rsIt = a.rigStates.find(kMirrorRig);
    const avatar::Rig rig = avatar::solveRig(in, in.avatar, rsIt != a.rigStates.end() ? &rsIt->second : nullptr);
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

// ------------------------------------------------------------------ TRAINING's tutorial videos
static void filmReset(App& a) {
    App::TrainFilm& f = a.film;
    f.t = 0; f.endWait = 0; f.frame = 0; f.flight = training::Flight{}; f.bounces = 0; f.caption.clear(); f.captionT = -9.f;
    f.targets.clear();
    if (f.lesson < 0) return;
    const training::Lesson& L = training::lessons()[training::lessonById(f.lesson)];
    for (const training::TargetDef& d : L.targets) f.targets.push_back({d});
    training::refreshLive(L, f.targets);
}
static void filmLoad(App& a, int lessonIndex) {   // the lesson's video (once each time its card opens)
    App::TrainFilm& f = a.film;
    const int id = training::lessons()[lessonIndex].id;
    if (f.lesson == id) { filmReset(a); return; }
    f.lesson = id;
    std::vector<uint8_t> bytes;
    char path[48];
    std::snprintf(path, sizeof path, "tutorials/lesson_%d.par", id);
    f.have = assetRead(path, bytes) && f.rp.load(bytes.data(), bytes.size());
    if (!f.have) f.rp = rec::Replay{};
    LOGI("training: video %s %s", path, f.have ? "loaded" : "(none)");
    filmReset(a);
}
// the video on, played at 60% speed: its frames as they come, the targets they take, what to say about them
static void filmStep(App& a, float dt) {
    using namespace training;
    App::TrainFilm& f = a.film;
    if (!a.trainVideo || !f.have || !a.menuOpen || a.trainer.phase != Phase::Card) return;
    const auto& fr = f.rp.frames;
    const int me = f.rp.local;
    const Lesson& L = lessons()[lessonById(f.lesson)];
    if (f.frame + 1 >= fr.size()) {   // at its end: a moment, then again
        if ((f.endWait += dt) > 2.0f) filmReset(a);
        return;
    }
    f.t += 0.6f * dt;
    auto say = [&](const char* t) { f.caption = t; f.captionT = f.t; };
    while (f.frame + 1 < fr.size() && fr[f.frame + 1].t - fr[0].t <= f.t) {
        const rec::Replay::Frame& x = fr[++f.frame];
        const float clock = x.t - fr[0].t;
        const rec::Replay::BallAt& rb = x.ball[me];
        Ball b;
        b.state = rb.state; b.pos = rb.pos; b.vel = rb.vel; b.spin = rb.spin; b.scale = rb.scale; b.charged = rb.charged;
        b.instigator = rb.instigator; b.speedLimit = length(rb.vel);
        Vec3 end = rb.pos;
        bool endSet = rb.state == BallState::Flying;
        for (const GameEvent& e : x.events) {
            if (e.type == EventType::Bounce && e.player == me) ++f.bounces;
            if (e.type == EventType::Throw && e.player == me) { b.bounces = f.bounces = 0; f.flight.start(b); f.flight.prev = e.pos; }
            if (e.type == EventType::Returned && e.player == me) { end = e.pos; endSet = true; }
            if (e.player == me) {
                if (e.type == EventType::Grab) say("TAKE YOUR BALL: PULL THE TRIGGER AT IT");
                if (e.type == EventType::Throw) say("SWING THROUGH AND LET GO OF THE TRIGGER");
                if (e.type == EventType::Caught) say("CAUGHT: THE TRIGGER HELD AS IT CAME BACK");
                if (e.type == EventType::ShieldBlock) say("BLOCKED: THE SHIELD ON THE HAND HOLDING THE BALL");
                if (e.type == EventType::KnuckleBlock) say("PUNCHED AWAY WITH THE KNUCKLE GUARD");
                if (e.type == EventType::HitPlayer) say("HIT: MOVE OR BLOCK IN TIME");
            }
            if (e.type == EventType::Strike && e.player != me && e.ball == me) say("STRIKE!");
        }
        b.bounces = f.bounces;
        if (L.kind == THROW && f.flight.on && endSet) {
            const int i = judge(L, f.targets, f.flight, b, 0.1f * rb.scale, end, clock, nullptr);
            if (i >= 0) {
                f.targets[i].down = true; f.targets[i].downT = clock; f.flight.on = false;
                refreshLive(L, f.targets);
                say("TARGET!");
            }
        }
        if (rb.state != BallState::Flying) f.flight.on = false;
    }
}
// called before the eye views each frame: the video's picture (about 30 a second) onto the screen's texture
static void filmRender(App& a) {
    using namespace training;
    App::TrainFilm& f = a.film;
    if (!a.trainVideo || !f.have || !a.menuOpen || !trainSide(a) || a.trainer.phase != Phase::Card || f.rp.frames.empty()) return;
    if (nowSeconds() - f.renderedAt < 1.0 / 30) return;
    f.renderedAt = nowSeconds();
    GLint keepFbo = 0, keepVp[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &keepFbo);
    glGetIntegerv(GL_VIEWPORT, keepVp);
    ensureRtt(a);
    const rec::Replay::Frame& x = f.rp.frames[std::min(f.frame, f.rp.frames.size() - 1)];
    const int me = f.rp.local;
    Game g = a.game;   // the lesson's own game, as the recording had it at this moment
    for (int p = 0; p < 2; ++p) {
        if (x.kept[p]) g.pl[p].in = x.in[p];
        g.pl[p].in.avatar = p == me ? a.myAvatar : a.botAvatar;
        const rec::Replay::BallAt& rb = x.ball[p];
        Ball& b = g.pl[p].ball;
        b.state = rb.state; b.pos = rb.pos; b.vel = rb.vel; b.spin = rb.spin; b.scale = rb.scale; b.charged = rb.charged;
        b.instigator = rb.instigator; b.heldHand = rb.heldHand;
        for (int h = 0; h < 2; ++h) {
            g.pl[p].hands[h].knuckleUp = g.tun.knuckleGuards() && g.pl[p].in.hand[h].tracked;
            g.pl[p].hands[h].shieldUp = g.tun.shields && rb.state == BallState::Held && rb.heldHand == h && (rb.charged || g.tun.maidenNow());
        }
    }
    // the camera: behind the player and to their right, looking down the court past them
    const Vec3 head = x.in[me].head.p;
    const Vec3 cam = head + Vec3{-1.55f, 0.45f, 1.05f}, look{head.x + 6.f, 1.25f, head.z * 0.5f};
    const Vec3 d = normalize(look - cam);
    const Pose eye{Quat::axisAngle({0, 1, 0}, std::atan2(-d.x, -d.z)) * Quat::axisAngle({1, 0, 0}, std::asin(clampf(d.y, -1.f, 1.f))), cam};
    std::swap(a.game, g);
    PlayerInput keepIn[kMaxPlayers];
    bool keepValid[kMaxPlayers];
    for (int p = 0; p < kMaxPlayers; ++p) { keepIn[p] = a.viewIn[p]; keepValid[p] = a.viewInValid[p]; }
    for (int p = 0; p < 2; ++p) { a.viewIn[p] = a.game.pl[p].in; a.viewInValid[p] = true; }
    const Pose keepHead = a.viewHead;
    const bool keepViewing = a.viewing;
    const int keepTrail = a.trailMax;
    std::swap(a.trainer.targets, f.targets);
    const float keepClock = a.trainer.clock;
    a.trainer.clock = x.t - f.rp.frames[0].t;
    a.viewing = true;      // (the player's own body drawn too)
    a.trailMax = -1;       // (the live game's trails aren't this one's)
    a.viewHead = eye;
    glBindFramebuffer(GL_FRAMEBUFFER, a.rttFbo[0]);
    glViewport(0, 0, 640, 360);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glClearColor(0.01f, 0.012f, 0.02f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    const float tv = std::tan(deg2rad(62.f) * 0.5f), th = tv * 16.f / 9.f;
    a.gfx.beginEye(Mat4::viewFromPose(eye), Mat4::projection(-std::atan(th), std::atan(th), std::atan(tv), -std::atan(tv), 0.05f, 2000.f), eye.p);
    a.rttPass = true;
    drawWorld(a, 0);
    drawWorld(a, 1);
    a.rttPass = false;
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)keepFbo);
    glViewport(keepVp[0], keepVp[1], keepVp[2], keepVp[3]);
    a.trainer.clock = keepClock;
    std::swap(a.trainer.targets, f.targets);
    a.trailMax = keepTrail;
    a.viewing = keepViewing;
    a.viewHead = keepHead;
    for (int p = 0; p < kMaxPlayers; ++p) { a.viewIn[p] = keepIn[p]; a.viewInValid[p] = keepValid[p]; }
    std::swap(a.game, g);
}

// SELFIE in the headset: 30 times a second, a camera 0.85 m in front of your face looking back at you draws the arena
// behind you and your own avatar (no one else sees it: it's for your cast) into its own picture
static const int kHsSize = 384;
static void drawOwnAvatar(App& a, int pass) {   // the spectator's avatar as the selfie sees it (the game draws none)
    PlayerInput in = a.localIn;
    in.avatar = a.myAvatar;
    in = handsApart(a, in, in.avatar);
    if (!a.avModels.empty()) drawAvatarBody(a, in, 2, pass, Mat4::identity(), false, false, 0, 2000);   // (its own)
    const AvatarLook lk = lookOf(a, in.avatar, 2);
    for (int h = 0; h < 2; ++h) {
        if (!in.hand[h].tracked) continue;
        Mat4 mirror = Mat4::identity();
        if (h == 0) mirror.m[0] = -1;
        DrawParams hp;
        hp.mirrored = (h == 0);
        if (!a.avModels.empty()) applyLook(hp, lk);
        drawGauntlet(a, in.avatar, Mat4::fromPose(in.hand[h].grip) * mirror, hp, pass);
    }
}
static void headSelfieRender(App& a) {
    if (!a.headSelfie || !a.spectating || isDesktop(a) || !a.localIn.headTracked) return;
    if (nowSeconds() - a.hsAt < 1.0 / 30) return;
    a.hsAt = nowSeconds();
    if (!a.hsFbo) {
        glGenRenderbuffers(1, &a.hsDepth);
        glBindRenderbuffer(GL_RENDERBUFFER, a.hsDepth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kHsSize, kHsSize);
        glGenTextures(1, &a.hsTex);
        glBindTexture(GL_TEXTURE_2D, a.hsTex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, kHsSize, kHsSize);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glGenFramebuffers(1, &a.hsFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, a.hsFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, a.hsTex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, a.hsDepth);
    }
    GLint keepFbo = 0, keepVp[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &keepFbo);
    glGetIntegerv(GL_VIEWPORT, keepVp);
    // the camera: in front of the face (level, the way you face), a little above, looking back at your head
    const Pose head = a.localIn.head;
    Vec3 f = head.q.rotate({0, 0, -1});
    f.y = 0;
    f = length(f) > 1e-3f ? normalize(f) : Vec3{0, 0, -1};
    const Vec3 cam = head.p + f * 0.85f + Vec3{0, 0.08f, 0}, look = head.p - Vec3{0, 0.12f, 0};
    const Vec3 d = normalize(look - cam);
    const Pose eye{Quat::axisAngle({0, 1, 0}, std::atan2(-d.x, -d.z)) * Quat::axisAngle({1, 0, 0}, std::asin(clampf(d.y, -1.f, 1.f))), cam};
    const Pose keepHead = a.viewHead;
    a.viewHead = eye;
    glBindFramebuffer(GL_FRAMEBUFFER, a.hsFbo);
    glViewport(0, 0, kHsSize, kHsSize);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glClearColor(0.01f, 0.012f, 0.02f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    const float t = std::tan(deg2rad(56.f) * 0.5f);
    a.gfx.beginEye(Mat4::viewFromPose(eye), Mat4::projection(-std::atan(t), std::atan(t), std::atan(t), -std::atan(t), 0.05f, 2000.f), eye.p);
    a.rttPass = true;
    drawWorld(a, 0);
    drawOwnAvatar(a, 0);
    drawWorld(a, 1);
    drawOwnAvatar(a, 1);
#ifndef PA_STORE
    if (a.hsCap && nowSeconds() - a.hsCapAt >= 0.5 && dataDir(a)) {   // SELFIE CAPTURE: the same camera, full size, saved
        a.hsCapAt = nowSeconds();
        const int CW = 2560, CH = 1440;
        if (!a.hsCapFbo) {
            glGenRenderbuffers(1, &a.hsCapDepth);
            glBindRenderbuffer(GL_RENDERBUFFER, a.hsCapDepth);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, CW, CH);
            glGenTextures(1, &a.hsCapTex);
            glBindTexture(GL_TEXTURE_2D, a.hsCapTex);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, CW, CH);
            glGenFramebuffers(1, &a.hsCapFbo);
            glBindFramebuffer(GL_FRAMEBUFFER, a.hsCapFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, a.hsCapTex, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, a.hsCapDepth);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, a.hsCapFbo);
        glViewport(0, 0, CW, CH);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        const float tx = t * CW / CH;
        a.gfx.beginEye(Mat4::viewFromPose(eye), Mat4::projection(-std::atan(tx), std::atan(tx), std::atan(t), -std::atan(t), 0.05f, 2000.f), eye.p);
        drawWorld(a, 0);
        drawOwnAvatar(a, 0);
        drawWorld(a, 1);
        drawOwnAvatar(a, 1);
        std::vector<uint8_t> px((size_t)CW * CH * 4);   // (RGBA rows are 4-byte aligned: the default pack alignment)
        glReadPixels(0, 0, CW, CH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        if (!a.hsCapStamp) a.hsCapStamp = (long)std::time(nullptr);
        char name[64];
        std::snprintf(name, sizeof name, "/selfiecap_%ld_%05d.ppm", a.hsCapStamp, ++a.hsCapN);
        if (std::FILE* fp = std::fopen((std::string(dataDir(a)) + name).c_str(), "wb")) {
            std::fprintf(fp, "P6\n%d %d\n255\n", CW, CH);
            std::vector<uint8_t> row((size_t)CW * 3);
            for (int y = CH - 1; y >= 0; --y) {   // (GL's rows run bottom up)
                const uint8_t* s = &px[(size_t)y * CW * 4];
                for (int x = 0; x < CW; ++x) { row[x * 3] = s[x * 4]; row[x * 3 + 1] = s[x * 4 + 1]; row[x * 3 + 2] = s[x * 4 + 2]; }
                std::fwrite(row.data(), 1, row.size(), fp);
            }
            std::fclose(fp);
        } else {
            LOGI("selfie capture: can't write %s%s (storage full?); capture off", dataDir(a), name);
            a.hsCap = false;
        }
    }
#endif
    a.rttPass = false;
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)keepFbo);
    glViewport(keepVp[0], keepVp[1], keepVp[2], keepVp[3]);
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
    const int sp = a.shotQueue >= 0 ? a.shotQueue : a.net.localSpot();
    const Vec3 s = holdingPose(a, sp).p, f = holdingFwd(a, sp);
    return {yawToward(f), Vec3{s.x, 0.95f, s.z} + f * 0.55f};   // (-Z along f: the quad's +Z faces the player)
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
    a.game.tun.players = 2;   // (the lobby's match is always two bots on the classic court)
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
        a.titleOpen = a.settingsOpen = a.voicePage = a.ptPage = a.gfxPage = a.ctlPage = a.roomBallPage = a.btnPage = a.helpOpen = a.xpOpen = a.rulesOpen = a.bugOpen = a.reportOpen = a.movePlayersOpen = false;
    };
    switch (which) {
    case 0: case kPosterCount: menuPage(); a.rulesOpen = true; a.rulesForHost = false; break;   // PLAY THE BOT / the court
    case 1: menuPage(); break;                                                       // MULTIPLAYER page
    case 2: menuPage(); a.helpOpen = true; a.creditsOpen = false; break;
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
    const PlayerInput mine = handsApart(a, a.localIn, a.myAvatar);
    for (int h = 0; h < 2; ++h)
        if (mine.hand[h].tracked)
            drawLancerAt(a, Mat4::fromPose(mine.hand[h].grip), h, look, pass, Mat4::identity(), false, a.myAvatar);
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
    // posters on stands (none while the builder is open: from CUSTOMIZE your reflection stood behind its poster; owner,
    // 2026-11-14)
    for (int i = 0; i < kPosterCount && !a.builderOpen; ++i) {
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

// PLAYERS' NAMES: a name over a head, turned to face you, on a dark plate so it reads against the arena
static void drawNameTag(App& a, const std::string& name, const Vec3& at) {
    const Vec3 f = normalize(Vec3{a.viewHead.p.x - at.x, 0, a.viewHead.p.z - at.z});
    const Pose p{Quat::axisAngle({0, 1, 0}, std::atan2(f.x, f.z)), at};
    const float h = 0.075f, w = std::max(0.2f, h * 0.75f * (float)name.size() + 0.06f);
    const float plate[4] = {0.01f, 0.02f, 0.04f, 0.78f}, white[4] = {1, 1, 1, 1};
    a.gfx.drawPanel(Mat4::fromPose(p * Pose{Quat{}, {0, 0, -0.003f}}, {w, h * 1.5f, 1}), plate);
    a.gfx.drawText(name, p, h, white);
}

// The JOIN QUEUE kiosk in front of a waiting player, and the QUEUE SCREEN on it (a player's report, 2026-11-11: "in
// the queue, the score and the players are hard to see over the arena"; the owner's pick, 2026-11-14: on the kiosk):
// the match's players in their colours, the score and strikes (FOUR CORNERS: the corners or teams; FIREBALL BLITZ:
// health and how many are still in), the clock or the round, the queue's next two, and the round's end. Tilted up
// toward the eyes, below the view of the court. myPos: our place in the queue (-1 none); next: the first two's names.
// The owner, 2026-11-15: "have it appear to your right offcenter so it doesn't block play of the arena and maybe only
// when you press in the left thumbstick like when you display the player name": the screen stands to the right of the
// spot, turned toward the player, and shows with PLAYERS' NAMES (the left stick's click); the JOIN QUEUE button is
// always there
static void drawQueueStation(App& a, int myPos, int queued, const std::string next[2]) {
    const Game& g = a.game;
    const Pose k = queueKioskPose(a);
    const float bg[4] = {0.02f, 0.02f, 0.03f, 0.9f}, btn[4] = {0.10f, 0.03f, 0.03f, 0.95f}, hov[4] = {0.75f, 0.15f, 0.10f, 0.98f};
    const float white[4] = {1, 1, 1, 1}, grey[4] = {0.55f, 0.6f, 0.68f, 1}, gold[4] = {1.0f, 0.85f, 0.3f, 1};
    a.gfx.drawPanel(Mat4::fromPose(k * Pose{Quat{}, {0, 0.03f, -0.002f}}, {0.46f, 0.20f, 1}), bg);
    a.gfx.drawPanel(Mat4::fromPose(k, {0.40f, 0.09f, 1}), a.queueHover == 0 ? hov : btn);
    a.gfx.drawText(myPos < 0 ? "JOIN QUEUE" : "LEAVE QUEUE", k * Pose{Quat{}, {0, 0, 0.004f}}, 0.04f, white);
    char buf[64];
    if (myPos >= 0) snprintf(buf, sizeof buf, "YOU ARE %s IN LINE", myPos == 0 ? "NEXT" : "SECOND");
    else snprintf(buf, sizeof buf, "%d OF 2 IN THE QUEUE", queued);
    a.gfx.drawText(buf, k * Pose{Quat{}, {0, 0.085f, 0.004f}}, 0.022f, grey);
    a.gfx.drawText(roomName(g.tun), k * Pose{Quat{}, {0, -0.055f, 0.004f}}, 0.020f, grey);
    if (!a.showNames && a.shotQueue < 0) return;   // (the screen: only with the left stick's click)
    // the screen: to the player's right (the kiosk's +X) and a little nearer, at about eye height, turned toward
    // them (68 degrees) with its top tilted back, clear of the court ahead
    const float W = 0.62f, H = 0.27f;
    const Pose sc = k * Pose{Quat::axisAngle({0, 1, 0}, deg2rad(-68.f)) * Quat::axisAngle({1, 0, 0}, deg2rad(-15.f)),
                             {0.68f, 0.36f, 0.34f}};
    auto at = [&](float u, float v) { return sc * Pose{Quat{}, {u, v, 0.004f}}; };
    const float frame[4] = {0.25f, 0.30f, 0.38f, 0.9f};
    a.gfx.drawPanel(Mat4::fromPose(sc * Pose{Quat{}, {0, 0, -0.003f}}, {W + 0.015f, H + 0.015f, 1}), frame);
    a.gfx.drawPanel(Mat4::fromPose(sc, {W, H, 1}), bg);
    auto col = [&](int p, float c[4]) { const float* t = g.np == 4 ? teamRGB(a, p) : a.sideRGB[p & 1]; c[0] = t[0] * 0.6f + 0.4f; c[1] = t[1] * 0.6f + 0.4f; c[2] = t[2] * 0.6f + 0.4f; c[3] = 1; };
    float c0[4], c1[4];
    if (g.np == 2) {
        col(0, c0); col(1, c1);
        std::string n0 = playerLabel(a, 0), n1 = playerLabel(a, 1);
        if (n0.size() > 12) n0.resize(12);
        if (n1.size() > 12) n1.resize(12);
        a.gfx.drawText(n0, at(-0.20f, 0.085f), 0.030f, c0);
        a.gfx.drawText(n1, at(0.20f, 0.085f), 0.030f, c1);
        snprintf(buf, sizeof buf, "%d", g.pl[0].score); a.gfx.drawText(buf, at(-0.075f, 0.03f), 0.075f, c0);
        snprintf(buf, sizeof buf, "%d", g.pl[1].score); a.gfx.drawText(buf, at(0.075f, 0.03f), 0.075f, c1);
        a.gfx.drawText("-", at(0, 0.03f), 0.05f, grey);
        snprintf(buf, sizeof buf, "STRIKES %d", g.pl[0].strikesAgainst); a.gfx.drawText(buf, at(-0.20f, 0.03f), 0.020f, grey);
        snprintf(buf, sizeof buf, "STRIKES %d", g.pl[1].strikesAgainst); a.gfx.drawText(buf, at(0.20f, 0.03f), 0.020f, grey);
        if (g.tun.advanced) snprintf(buf, sizeof buf, "ROUND %d  -  ROUNDS %d - %d  %s", std::max(1, g.round), g.pl[0].roundWins, g.pl[1].roundWins, clockLine(g).c_str());
        else snprintf(buf, sizeof buf, "%s%s", clockLine(g).c_str(), g.tun.shields ? "" : "  -  HOTDOG");
        a.gfx.drawText(buf, at(0, -0.035f), 0.024f, g.overtime ? gold : white);
    } else {   // four: each corner or team, its score (FIREBALL BLITZ: its health); how many are still in
        const bool teams = !g.octagon() && g.tun.teamMode != TEAMS_FFA;
        const int n = teams ? 2 : 4;
        for (int t = 0; t < n; ++t) {
            float c[4]; col(t, c);
            const float u = (t - 0.5f * (n - 1)) * (W / n);
            if (g.octagon()) {   // its health: a square for each point left (as over their heads), OUT when gone
                if (!g.alive(t)) a.gfx.drawText("OUT", at(u, 0.04f), 0.04f, grey);
                else {
                    const int hp = std::max(1, g.tun.health);
                    const float pip = std::min(0.024f, (W / n - 0.02f) / (hp * 1.35f)), gap = pip * 1.35f;
                    for (int i = 0; i < hp; ++i) {
                        const bool full = i < g.pl[t].health;
                        const float pc[4] = {full ? c[0] : 0.14f, full ? c[1] : 0.14f, full ? c[2] : 0.16f, full ? 1.f : 0.7f};
                        a.gfx.drawPanel(Mat4::fromPose(at((u - 0.5f * gap * (hp - 1)) + i * gap, 0.04f), {pip, pip, 1}), pc);
                    }
                }
            } else {
                snprintf(buf, sizeof buf, "%d", g.teamScore(t));
                a.gfx.drawText(buf, at(u, 0.04f), 0.07f, c);
            }
            std::string nm = teams ? (t == 0 ? "BLUE TEAM" : "ORANGE TEAM") : playerLabel(a, t);
            if (nm.size() > (n == 2 ? 14u : 9u)) nm.resize(n == 2 ? 14 : 9);
            a.gfx.drawText(nm, at(u, 0.095f), 0.022f, c);
        }
        if (g.octagon()) snprintf(buf, sizeof buf, "%d STILL IN", g.aliveCount());
        else snprintf(buf, sizeof buf, "%s", clockLine(g).c_str());
        a.gfx.drawText(buf, at(0, -0.035f), 0.024f, white);
    }
    // the queue's next two (YOU when it is us), or the round's end
    const char* end = g.match == MatchState::RoundSummary ? "ROUND OVER" : g.match == MatchState::MatchSummary ? "MATCH OVER" : nullptr;
    if (end) a.gfx.drawText(end, at(0, -0.09f), 0.028f, gold);
    else if (!next[0].empty()) {
        std::string t = "NEXT: " + next[0] + (next[1].empty() ? "" : "   THEN: " + next[1]);
        a.gfx.drawText(t, at(0, -0.09f), t.size() > 34 ? 0.018f : 0.022f, next[0] == "YOU" ? gold : grey);
    } else a.gfx.drawText("NOBODY IN THE QUEUE", at(0, -0.09f), 0.020f, grey);
}

// FOUL LINE moved (0.3.20): the court drawn with its line where the line is now. The court is built as a middle (out
// to the line, 4.75 m) and two ends in their players' colours (the line to the back wall, 9.5 m): glass, frames,
// hexagons, floor strips and floor. So the line itself moves, each end stretches or shrinks to start at the new line
// (its back wall where it is) and the middle with them; the rest (the centre line and emblem, the back walls) as it is.
// The pieces are told apart by their bounds, so any asset set's court follows. Only the drawing: the walls are where
// they were. (Collision: Game's foul zone follows the line on its own)
static void drawCourtModel(App& a, const Model& m, const Mat4& M, DrawParams dp, int pass) {
    const float mv = a.game.arenaKind <= ARENA_CLASSIC && a.game.np == 2 ? a.game.tun.foulMove() : 0.f;
    if (mv == 0.f || m.secMin.empty()) { a.gfx.draw(m, M, dp, pass); return; }
    const float L0 = 4.75f, L = L0 - mv, back = 9.5f;
    const float kEnd = (back - L) / (back - L0), kMid = L / L0;
    Mat4 each[64];
    uint64_t moved = 0;
    for (size_t s = 0; s < m.secMin.size() && s < 64; ++s) {
        const float lo = m.secMin[s].x, hi = m.secMax[s].x, c = 0.5f * (lo + hi), sg = c < 0 ? -1.f : 1.f;
        const float in = std::min(std::fabs(lo), std::fabs(hi)), out = std::max(std::fabs(lo), std::fabs(hi));
        if (hi - lo < 0.4f && std::fabs(std::fabs(c) - L0) < 0.15f)   // the line: moved
            each[s] = Mat4::fromPose({Quat{}, {sg * mv * -1.f, 0, 0}});
        else if (lo * hi > 0 && std::fabs(in - L0) < 0.35f && std::fabs(out - back) < 0.25f)   // an end: x' = k x + c (1 - k)
            each[s] = Mat4::fromPose({Quat{}, {sg * back * (1.f - kEnd), 0, 0}}, {kEnd, 1, 1});
        else if (lo < 0 && hi > 0 && std::fabs(-lo - L0) < 0.15f && std::fabs(hi - L0) < 0.15f)   // the middle
            each[s] = Mat4::fromPose({Quat{}, {0, 0, 0}}, {kMid, 1, 1});
        else continue;
        moved |= 1ull << s;
    }
    if (!moved || m.sections.size() > 64) { a.gfx.draw(m, M, dp, pass); return; }
    // each piece in the model's own order (the floor is see-through: its dark underlay drawn after the ends' floor is
    // what makes them dark; another order changes the colours)
    for (int s = 0; s < (int)m.sections.size(); ++s) {
        dp.only = s;
        a.gfx.draw(m, (moved >> s) & 1 ? M * each[s] : M, dp, pass);
    }
}

// FAIR MATCHES: at a match's countdown, the host is told when an option it chose plays at regulation because a
// player's game doesn't have it (their own rules stay chosen for the next match)
static void fairMatchNotice(App& a) {
    const Tunables& t = a.game.tun;
    if (a.net.mode() != Net::Mode::Host || a.game.match != MatchState::Countdown) { a.fairTold = false; return; }
    if (a.fairTold) return;
    a.fairTold = true;
    const char* what = nullptr;
    if (a.game.np == 2 && !t.experimental && t.foulShift != 0 && t.foulShiftNow() == 0) what = "FOUL LINE: REGULATION (A PLAYER NEEDS 0.3.20)";
    else if (a.game.np == 2 && t.strikeSpeed != 0 && t.strikeSpeedNow() == 0) what = "STRIKES: REGULATION (A PLAYER NEEDS 0.3.19)";
    else if (a.game.np == 2 && !t.roomOptionsNow() && (!t.experimental || t.expRoom == ROOM_DIAMOND) &&
             ((t.maiden && t.shields) || t.aimAssist >= 0 || t.strikeZone != 1 || t.ballGrowth != 3 ||
              (t.advanced && t.advancedRoundsToWin != 2) || (t.shields && t.shieldSize != 0)))
        what = "ROOM OPTIONS: AS THE RULES SAY (A PLAYER NEEDS 0.3.21)";
    if (what) {
        a.net.noticeLocal(what);
        LOGI("fair match: %s (the oldest player's game: %d)", what, t.matchVersion);
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
            const PlayerInput shown = handsApart(a, m.in, m.in.avatar);
            if (!a.avModels.empty()) drawAvatarBody(a, shown, 2, pass, Mat4::identity(), false, false, 0, 1000 + int(m.id & 0xFFFFF));
            const AvatarLook look = lookOf(a, m.in.avatar, 2);
            for (int h = 0; h < 2; ++h)
                if (shown.hand[h].tracked && !insideTank(m.in.hand[h].grip.p))
                    drawLancerAt(a, Mat4::fromPose(shown.hand[h].grip), h, look, pass, Mat4::identity(), false, m.in.avatar);
            if (pass == 1 && m.queuePos >= 0) {
                Vec3 f = normalize(Vec3{a.viewHead.p.x - m.in.head.p.x, 0, a.viewHead.p.z - m.in.head.p.z});
                static const char* kPlace[4] = {"NEXT", "2ND", "3RD", "4TH"};
                a.gfx.drawText(kPlace[std::max(0, std::min(3, (int)m.queuePos))], {Quat::axisAngle({0, 1, 0}, std::atan2(f.x, f.z)), m.in.head.p + Vec3{0, 0.32f, 0}},
                               0.07f, kRed);
            }
            if (pass == 1 && a.showNames)
                if (const char* n = rm->nameOf(m.id)) drawNameTag(a, n, m.in.head.p + Vec3{0, m.queuePos >= 0 ? 0.44f : 0.32f, 0});
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
        const PlayerInput mine = handsApart(a, a.localIn, a.myAvatar);
        for (int h = 0; h < 2; ++h)
            if (mine.hand[h].tracked)
                drawLancerAt(a, Mat4::fromPose(mine.hand[h].grip), h, look, pass, Mat4::identity(), false, a.myAvatar);
    }
    if (pass == 1 && a.showNames) {   // PLAYERS' NAMES: the players in the match, not yourself
        const int L = a.watchIp != 0 ? -1 : a.net.localSlot();
        for (int p = 0; p < a.game.np; ++p)
            if (p != L && a.game.pl[p].in.headTracked) drawNameTag(a, playerLabel(a, p), a.game.pl[p].in.head.p + Vec3{0, 0.36f, 0});
    }
    if (pass != 1) return;
    // the holding area: a glowing strip with four spots (everyone in the room sees it, players included)
    for (int s = 0; s < 4; ++s) {
        const float ring[4] = {0.35f, 0.08f, 0.06f, 1};
        Vec3 c = holdingPose(a, s).p;
        a.gfx.drawSpriteFlat({c.x, 0.006f, c.z}, 1.1f, ring, {0, 1, 0}, 1);
    }
    // spectators near this arena: small blue lights, only for people outside the tank (never for the players)
    if (!inTank)
        for (const RoomInfo::Watcher& w : rm->watchers)
            if (w.where == 1 && w.id != me) a.gfx.drawSprite(w.pos, 0.09f, kBlue, a.viewHead.p, 0);
    // our own JOIN QUEUE kiosk, and the queue screen on it
    if (a.net.localWaiting()) {
        int queued = 0;
        std::string line[2];   // who is next and after them (the queue's first two)
        for (const RoomInfo::Member& m : rm->members) {
            queued += m.queuePos >= 0;
            if (m.queuePos == 0 || m.queuePos == 1)
                if (const char* n = rm->nameOf(m.id)) line[m.queuePos] = m.id == a.net.session() ? "YOU" : n;
        }
        drawQueueStation(a, a.net.localQueuePos(), queued, line);
    }
}

// ------------------------------------------------------------------ spectator hub: live miniature arenas
static void drawMiniMatch(App& a, const Game& g, const Vec3& at0, const Quat& q, float s, int pass, bool hover, const RoomInfo* rmini = nullptr) {
    // (the EXPERIMENTAL rooms as their own shapes, 0.3.12: FOUR CORNERS is four times as wide, so smaller)
    const int room = !g.tun.experimental ? -1 : std::max(0, std::min(kRooms - 1, g.tun.expRoom));
    if (room >= 1) s *= 0.55f;
    // (and lifted so the room's lowest point sits on the table: FOUR CORNERS' floor and DIAMOND's V reach below the
    // platforms, and the table's pillar went up through the middle of the play, testers 2026-10-30)
    Vec3 at = at0;
    if (room >= 0) at.y += s * (room == 2 ? 0.f : room == 1 ? -exparena::kFloor : exparena::kRollReach - exparena::kRollCY);
    auto M = [&](const Mat4& local) { return Mat4::fromPose({q, at}, {s, s, s}) * local; };
    auto W = [&](const Vec3& p) { return at + q.rotate(p * s); };
    const float teamCol[2][4] = {{0.1f, 0.7f, 1.0f, 1}, {1.0f, 0.4f, 0.1f, 1}};
    DrawParams dp;
    if (room < 0) {
        drawCourtModel(a, a.floor.valid() ? a.floor : a.arena, M(Mat4::identity()), dp, pass);
        drawCourtModel(a, a.arena, M(Mat4::identity()), dp, pass);
    } else if (pass == 1) {   // its edges in light, and each player's platform
        const float edge[4] = {hover ? 0.45f : 0.25f, hover ? 1.0f : 0.8f, 1.0f, 1}, plat[4] = {0.10f, 0.35f, 0.55f, 1};
        auto line = [&](const Vec3& p0, const Vec3& p1) { const Vec3 l[2] = {W(p0), W(p1)}; a.gfx.drawRibbon(l, 2, 0.006f, edge, false); };
        if (room == 2) {   // FIREBALL BLITZ: the octagon as it is now (it closes in)
            const float R = g.ringApothem / std::cos(kPi / 8.f);
            for (int i = 0; i < 8; ++i) {
                const float a0 = kPi / 8.f + i * kPi / 4.f, a1 = a0 + kPi / 4.f;
                const Vec3 v0{R * std::cos(a0), 0, R * std::sin(a0)}, v1{R * std::cos(a1), 0, R * std::sin(a1)};
                line(v0, v1);
                line(v0 + Vec3{0, exparena::kOctH, 0}, v1 + Vec3{0, exparena::kOctH, 0});
                line(v0, v0 + Vec3{0, exparena::kOctH, 0});
            }
        } else if (room == 1) {
            using namespace exparena;
            const Vec3 c[4] = {{-kCorner, 0, 0}, {0, 0, kCorner}, {kCorner, 0, 0}, {0, 0, -kCorner}};
            for (int i = 0; i < 4; ++i) {
                const Vec3 n = c[(i + 1) % 4];
                line(c[i] + Vec3{0, kFloor, 0}, n + Vec3{0, kFloor, 0});
                line(c[i] + Vec3{0, kCeiling, 0}, n + Vec3{0, kCeiling, 0});
                line(c[i] + Vec3{0, kFloor, 0}, c[i] + Vec3{0, kCeiling, 0});
            }
        } else {
            Vec3 e[4];
            exparena::diamondEnd(e);
            const float X = exparena::kRollLen;
            for (int i = 0; i < 4; ++i) {
                line(e[i] + Vec3{-X, 0, 0}, e[(i + 1) % 4] + Vec3{-X, 0, 0});
                line(e[i] + Vec3{X, 0, 0}, e[(i + 1) % 4] + Vec3{X, 0, 0});
                line(e[i] + Vec3{-X, 0, 0}, e[i] + Vec3{X, 0, 0});
            }
        }
        for (int p = 0; p < g.np; ++p) if (room != 2 || g.alive(p)) a.gfx.drawSpriteFlat(W(g.tun.start[p]), 2.4f * s, plat, {0, 1, 0}, 1);
        if (room == 2)   // (its fireballs)
            for (int p = 0; p < g.np; ++p)
                for (const Ball& f : g.fire[p])
                    if (f.state == BallState::Flying) {
                        const float fc[4] = {1.0f, 0.55f, 0.15f, 1};
                        a.gfx.drawSprite(W(f.pos), 0.5f * s, fc, a.viewHead.p, 0);
                    }
    }
    if (pass == 1 && room < 0) {
        DrawParams cp;
        if (hover) for (int k = 0; k < 3; ++k) cp.tintMul[k] = 1.6f;
        drawCourtModel(a, a.courtFx, M(Mat4::identity()), cp, 1);
        DrawParams pp;
        pp.uvOff[1] = wrapUV(-(nowSeconds() - a.startTime) * 0.9);
        a.gfx.draw(a.courtPulse, M(Mat4::identity()), pp, 1);
    }
    if (pass == 1 && rmini) {   // the people in that arena's room: red = waiting / queued, blue = spectators there
        const uint32_t me = a.net.session();
        for (const RoomInfo::Member& m : rmini->members)
            if (m.id != me && m.in.headTracked) a.gfx.drawSprite(W(m.in.head.p), 0.035f, kRed, a.viewHead.p, 0);
        for (const RoomInfo::Watcher& w : rmini->watchers)
            if (w.where == 1 && w.id != me) a.gfx.drawSprite(W(w.pos), 0.03f, kBlue, a.viewHead.p, 0);
    }
    for (int p = 0; p < g.np; ++p) {   // (FOUR CORNERS: all four, in their colours)
        if (rmini && !rmini->slot[p] && !rmini->isBot(p)) continue;   // empty tank slot
        const PlayerState& ps = g.pl[p];
        DrawParams bp;
        const float* pc = g.np == 2 ? teamCol[p] : (g.tun.teamMode == TEAMS_FFA || g.octagon()) ? kFourRGB[p & 3] : teamCol[g.team(p) & 1];
        for (int k = 0; k < 3; ++k) bp.tintMul[k] = 0.7f + 0.3f * pc[k];
        Pose hp = ps.in.head;
        const PlayerInput pin = handsApart(a, ps.in, ps.in.avatar);
        if (hp.p.y > 0.3f && !a.avModels.empty()) {
            drawAvatarBody(a, pin, p, pass, M(Mat4::identity()), false, true, 0, 3000 + p);   // (its own, not the court's)
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
            drawGauntlet(a, ps.in.avatar, M(Mat4::fromPose(pin.hand[h].grip) * mirror), hp2, pass);
        }
        const Ball& b = ps.ball;
        if (pass == 1 && b.state != BallState::Dead && b.pos.y > 0.f && (rmini == nullptr || rmini->slot[p] || rmini->isBot(p))) {
            float c[4] = {pc[0] * 1.4f, pc[1] * 1.4f, pc[2] * 1.4f, 1};
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
            // (a box's size is half its extent: the pillar once reached up through the table and the play, testers
            // 2026-10-30; the table's top is the miniature's floor)
            const float top = 0.04f, leg = 0.5f * (at.y - top);
            a.gfx.drawBox(Mat4::fromPose({q, at - Vec3{0, 0.5f * top, 0}}, {0.625f, 0.5f * top, 0.19f}), ped, false);
            a.gfx.drawBox(Mat4::fromPose({q, Vec3{at.x, leg, at.z}}, {0.07f, leg, 0.07f}), ped, false);
        }
        drawMiniMatch(a, ms[i].game, at, q, s, pass, hover, &ms[i].room);
        if (pass == 1) {
            // its glow on the floor round the pedestal (0.3.16: drawn at the table top, the miniature's own floor, it
            // showed through the arena's glass, testers 2026-11-06)
            const float ring[4] = {hover ? 0.4f : 0.08f, hover ? 0.9f : 0.3f, hover ? 1.0f : 0.45f, 1};
            a.gfx.drawSpriteFlat(Vec3{at.x, 0.008f, at.z}, 1.5f, ring, {0, 1, 0}, 1);
            Vec3 lf = normalize(Vec3{eye.x - at.x, 0, eye.z - at.z});
            Quat lq = Quat::axisAngle({0, 1, 0}, std::atan2(lf.x, lf.z));
            char buf[64];
            const Game& g = ms[i].game;
            a.gfx.drawText(ms[i].name[0] ? ms[i].name : "LIVE GAME", {lq, at + Vec3{0, 0.46f, 0}}, 0.05f, hover ? cyan : white);
            if (g.tun.advanced) snprintf(buf, sizeof buf, "%s BEST OF %d%s   %d  -  %d   ROUND %d", kRoomTitle[roomOf(g.tun)], 2 * g.tun.roundsToWinNow() - 1,
                                         g.tun.shields ? "" : " HOTDOG", g.pl[0].score, g.pl[1].score, std::max(1, g.round));
            else if (g.octagon())   // (FIREBALL BLITZ: everyone's health)
                snprintf(buf, sizeof buf, "FIREBALL BLITZ   %d - %d - %d - %d   %s", g.pl[0].health, g.pl[1].health, g.pl[2].health, g.pl[3].health,
                         clockLine(g).c_str());
            else if (g.np == 4 && (g.tun.teamMode == TEAMS_FFA || g.octagon()))   // (FOUR CORNERS: the four scores, or the teams')
                snprintf(buf, sizeof buf, "FOUR CORNERS   %d - %d - %d - %d   %s", g.pl[0].score, g.pl[1].score, g.pl[2].score, g.pl[3].score,
                         clockLine(g).c_str());
            else if (g.np == 4)
                snprintf(buf, sizeof buf, "FOUR CORNERS %s   %d  -  %d   %s", g.tun.teamMode == TEAMS_3V1 ? "3 V 1" : "2 V 2", g.teamScore(0),
                         g.teamScore(1), clockLine(g).c_str());
            else snprintf(buf, sizeof buf, "%s%s   %d  -  %d   %s", g.tun.experimental ? "DIAMOND" : "SQUARE", g.tun.shields ? "" : " HOTDOG",
                          g.pl[0].score, g.pl[1].score, clockLine(g).c_str());
            if (!a.net.live(ms[i])) snprintf(buf, sizeof buf, "WAITING FOR PLAYERS");   // (0.3.16: no game going there)
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
    // the hub's sign, for a headset (the PC window has its own title and TAB TO WATCH: the sign, up in the arena,
    // ran into that title whenever the camera centred on a match, testers 2026-11-06; and pointing is a headset's)
    if (pass == 1) {
        Quat fq = Quat::axisAngle({0, 1, 0}, -kPi * 0.5f);   // facing a viewer looking +X
        if (!isDesktop(a)) {
            a.gfx.drawText("SPECTATOR HUB", {fq, {2.4f, 2.1f, 0}}, 0.14f, cyan);
            if (n == 0) {
                a.gfx.drawText("NO LIVE GAMES ON YOUR NETWORK YET", {fq, {2.4f, 1.6f, 0}}, 0.07f, white);
                a.gfx.drawText("WHEN A PLAYER HOSTS A GAME IT APPEARS HERE", {fq, {2.4f, 1.45f, 0}}, 0.05f, grey);
            } else {
                a.gfx.drawText("POINT AT AN ARENA AND PULL THE TRIGGER TO WATCH IT", {fq, {2.4f, 1.85f, 0}}, 0.045f, grey);
            }
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
    for (int p = 0; p < kMaxPlayers; ++p) if (rm->slot[p] == id) { out = a.viewIn[p].head.p; return true; }
    for (const RoomInfo::Member& m : rm->members) if (m.id == id) { out = m.in.head.p; return true; }
    for (const RoomInfo::Watcher& w : rm->watchers) if (w.id == id) { out = w.pos; return true; }
    return false;
}

// Voice: the microphone runs only inside an arena room (or at an arena as a spectator) with voice on and the
// permission given; 40 ms frames are sent while push-to-talk is held (or always with open mic, above a noise gate).
static void updateVoice(App& a) {
    const bool zone = a.net.voiceZone() && a.voiceMode != 2;
    // the permission: checked once a second, so a prompt answered later (or a grant from the headset's settings) starts
    // the microphone without changing VOICE (GitHub #15, astrobleem: a pending prompt once read as no). Asked only when
    // you first go to talk in a voice zone (TALK held, or OPEN MIC), once a session, after a heads-up by your hand and
    // in the banner: the headset's prompt puts the game in the background, a grey space with the prompt coming later
    // (the owner, 2026-11-17: asked on entering a room, it looked like a crash)
    const double now = nowSeconds();
    if (zone && !a.audio.capturing() && now >= a.micCheckAt) {
        a.micCheckAt = now + 1.0;
        a.micGranted = micPermission(false);
        a.micDenied = !a.micGranted && a.micAsked;
        a.micFailed = a.micGranted && !a.audio.startCapture();
        if (a.micFailed) a.micCheckAt = now + 10.0;   // (busy or missing: not every second, nor a log line each time)
    }
    if (zone && !a.micGranted && !a.micAsked && a.micAskAt == 0 && a.micCheckAt > 0 && (a.ptt || a.voiceMode == 1)) {
        a.micAskAt = now + 3.0;
        a.net.noticeLocal("VOICE NEEDS THE MIC: THE HEADSET WILL ASK");
        LOGI("voice: the microphone asked for in 3 s (the heads-up shown)");
    }
    if (a.micAskAt > 0 && now >= a.micAskAt) {
        a.micAskAt = 0;
        if (zone) {   // (left the room meanwhile: asked the next time you go to talk)
            a.micAsked = true;
            a.micGranted = micPermission(true);   // (the headset's prompt, unless already given)
            a.micCheckAt = now + 1.0;
        }
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
            LOGI("voice: sent %d frames, received %d (mic gain %.1fx, %s; mic %s, %s peak %.2f, held %d frames)", a.voiceSent,
                 a.voiceRecv, a.micGain, a.voiceMode == 0 ? "push to talk" : a.voiceMode == 1 ? "open mic" : "off",
                 a.audio.capturing() ? "on" : "off", jobButtons(a, controls::J_TALK, false).c_str(), a.pttGripMax, a.pttHeldFrames);
        a.voiceSent = a.voiceRecv = 0;
        a.pttGripMax = 0;
        a.pttHeldFrames = 0;
        a.voiceLogAt = t;
    }
    a.net.voiceInbox().clear();
}

// READY UP (rooms for four, 0.3.14): before a match, PRESS A WHEN READY in front of you until you have, then who the
// match waits for; READY over the others who are
static void drawReadyUp(App& a) {
    const Game& g = a.game;
    const int L = a.net.localSlot();
    if (!a.net.inRoom() || g.np != 4 || g.match != MatchState::WaitingForPlayers || L < 0 || a.menuOpen) return;
    const float cyan[4] = {0.3f, 0.85f, 1.0f, 1}, green[4] = {0.3f, 1.0f, 0.45f, 1}, white[4] = {1, 1, 1, 1};
    const Pose h = a.viewHead;
    Vec3 f = h.q.rotate({0, 0, -1}); f.y = 0;
    if (length(f) < 1e-3f) return;
    f = normalize(f);
    const Pose card{Quat::axisAngle({0, 1, 0}, std::atan2(-f.x, -f.z)), h.p + f * 1.3f + Vec3{0, -0.15f, 0}};
    int waiting = 0;
    for (int p = 0; p < g.np; ++p) waiting += !g.pl[p].isBot && g.pl[p].in.headTracked && !g.pl[p].ready;
    if (!g.pl[L].ready) {
        a.gfx.drawText("PRESS " + readyName(a) + " WHEN READY", card, 0.07f, cyan);   // (READY's button: A as standard)
        a.gfx.drawText("THE MATCH STARTS WHEN EVERYONE IS", card * Pose{Quat{}, {0, -0.10f, 0}}, 0.03f, white);
    } else {
        char t[48];
        std::snprintf(t, sizeof t, waiting ? "READY - WAITING FOR %d MORE" : "READY", waiting);
        a.gfx.drawText(t, card, 0.05f, green);
    }
    for (int p = 0; p < g.np; ++p) {   // the others who are ready (people only: a bot always is)
        if (p == L || g.pl[p].isBot || !g.pl[p].ready) continue;
        const Vec3 at = a.viewIn[p].head.p + Vec3{0, 0.35f, 0};
        Vec3 to = h.p - at; to.y = 0;
        if (length(to) < 1e-3f) continue;
        a.gfx.drawText("READY", {Quat::axisAngle({0, 1, 0}, std::atan2(to.x, to.z)), at}, 0.08f, green);
    }
}

// HUD near the left hand: the push-to-talk hint, TALKING, or mic state; small white lights over people talking
static void drawVoiceHud(App& a) {
    const double now = nowSeconds();
    if (!a.net.voiceZone()) { a.voiceZoneAt = 0; return; }
    if (a.voiceZoneAt == 0) a.voiceZoneAt = now;
    const float grey[4] = {0.6f, 0.65f, 0.72f, 1}, red[4] = {1.0f, 0.3f, 0.25f, 1}, white[4] = {1, 1, 1, 1};
    const PlayerInput& in = a.localIn;
    // (owner, 2026-11-02: the hint stayed by your hand all the time, spectating too, and got in the way. TALKING and a
    // microphone problem always show; the reminder and the mic's steady state only for the first 10 s in a room, and
    // spectators only see TALKING)
    const bool spectator = a.net.mode() == Net::Mode::Spectate;
    const bool intro = !spectator && now - a.voiceZoneAt < 10.0;
    const std::string hint = talkHint(a);   // (HOLD LEFT GRIP TO TALK as standard: TALK's button, BUTTONS)
    const char* t = a.talking ? (a.voiceMode == 1 ? "OPEN MIC - TALKING" : "TALKING")
                  : spectator ? nullptr
                  : a.micAskAt > 0 ? "THE HEADSET WILL ASK FOR THE MIC"
                  : a.micDenied && a.voiceMode != 2 ? "MIC NEEDS PERMISSION" : a.micFailed && a.voiceMode != 2 ? "MIC NOT AVAILABLE"
                  : !intro ? nullptr
                  : a.voiceMode == 2 ? "VOICE OFF" : a.muteAll ? "ALL MUTED" : a.voiceMode == 1 ? "OPEN MIC" : hint.c_str();
    const int ts = talkSlot(a);
    const int hh = a.voiceMode == 0 && ts >= 0 ? controls::slotHand(ts) : 0;   // (by the hand that talks)
    if (t && in.hand[hh].tracked && !a.menuOpen) {
        Vec3 at = in.hand[hh].grip.p + Vec3{0, 0.12f, 0};
        Vec3 to = a.viewHead.p - at;
        to.y = 0;
        to = normalize(to);
        a.gfx.drawText(t, {Quat::axisAngle({0, 1, 0}, std::atan2(to.x, to.z)), at}, 0.022f, a.talking ? red : grey);
    }
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
    for (int p = 0; p < a.game.np; ++p) {
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
// (wide, dropY: the stadium as drawn: the EXPERIMENTAL arenas widen it and lower it; 1, 0 for the classic court)
static void drawFanFlags(App& a, float now, float wide, float dropY) {
    auto in = [&](const Vec3& v) { return Vec3{v.x * wide, v.y + dropY, v.z * wide}; };   // a classic stand spot, there
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
                const Vec3 wrist = in(s.p) + Vec3{0, 1.15f, 0};
                const float col[4] = {a.sideRGB[p][0], a.sideRGB[p][1], a.sideRGB[p][2], 1};
                a.gfx.drawTexturedPanel(Mat4::fromPose({qf, wrist + qf.rotate({0, 0.32f, 0})}, {0.64f, 0.64f, 1}),
                                        a.gfx.texture("fx_foam", true), col, false);
                continue;
            }
            const Vec3 top = in(s.p) + Vec3{0, poleH, 0};
            const float pole[4] = {0.55f, 0.56f, 0.6f, 1};
            a.gfx.drawBox(Mat4::fromPose({q, in(s.p) + Vec3{0, poleH * 0.5f, 0}}, {0.015f, poleH * 0.5f, 0.015f}), pole, false);   // (half sizes)
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
        const Vec3 fp = in(f.p);
        Vec3 d = eye - fp;
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
            a.gfx.drawTexturedPanel(Mat4::fromPose({q, fp}, {sz, sz / 1.5f, 1}), a.gfx.texture(N->flag, true), white, false);
        } else if (f.kind == 2) {   // no country: a foam finger in the end's colour
            const float col[4] = {sc[0], sc[1], sc[2], alpha};
            a.gfx.drawTexturedPanel(Mat4::fromPose({q, fp}, {sz * 1.2f, sz * 1.2f, 1}), a.gfx.texture("fx_foam", true), col, false);
        } else if (!a.game.tun.shields) {   // HOTDOG MODE: hot dogs and smiley faces (the community's nod)
            a.gfx.drawTexturedPanel(Mat4::fromPose({q, fp}, {sz * 1.1f, sz * 1.1f, 1}),
                                    a.gfx.texture(f.kind == 0 ? "fx_hotdog" : "fx_smile", true), white, false);
        } else {
            const bool star = f.kind == 1;
            const float col[4] = {star ? 1.f : 0.55f + 0.45f * sc[0], star ? 0.85f : 0.25f + 0.45f * sc[1],
                                  star ? 0.3f : 0.35f + 0.45f * sc[2], alpha};
            a.gfx.drawTexturedPanel(Mat4::fromPose({q, fp}, {sz, sz, 1}), a.gfx.texture(star ? "fx_star" : "fx_heart", true), col, false);
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
        for (int p = 0; p < a.game.np; ++p) {   // a solid slab you stand on (your ball's spawner on it), steel with your colour's glow
            DrawParams pp;
            const float* pc = playerRGB(a, p);
            for (int k = 0; k < 3; ++k) { pp.tintMul[k] = 0.30f + 0.12f * pc[k]; pp.emissiveAdd[k] = 0.05f * pc[k]; }
            a.gfx.draw(a.gfx.cylinder, Mat4::fromPose({Quat{}, a.game.tun.start[p]}, {kPlatformR, kPlatformThick, kPlatformR}), pp, 0);
        }
        return;
    }
    for (int p = 0; p < a.game.np; ++p) {   // its top: rings and lines of light, so it reads as a floor to stand on
        const float px = a.game.tun.start[p].x, pz = a.game.tun.start[p].z;
        const float* pc = playerRGB(a, p);
        const float lineC[4] = {pc[0] * 0.45f, pc[1] * 0.45f, pc[2] * 0.45f, 1};
        for (int k = -2; k <= 2; ++k) {
            const float off = 0.4f * k, half = std::sqrt(std::max(0.f, kPlatformR * kPlatformR - off * off)) - 0.05f;
            const Vec3 a0[2] = {{px + off, 0.006f, pz - half}, {px + off, 0.006f, pz + half}}, a1[2] = {{px - half, 0.006f, pz + off}, {px + half, 0.006f, pz + off}};
            a.gfx.drawRibbon(a0, 2, 0.006f, lineC, false);
            a.gfx.drawRibbon(a1, 2, 0.006f, lineC, false);
        }
    }
    if (meeting)
        for (int p = 0; p < 2; ++p) {
            const float r[4] = {a.sideRGB[p][0] * 0.8f, a.sideRGB[p][1] * 0.8f, a.sideRGB[p][2] * 0.8f, 1};
            a.gfx.drawSpriteFlat({p ? 0.02f : -0.02f, 0.008f, 0}, 2.9f, r, {0, 1, 0}, 1);
        }
    for (int p = 0; p < a.game.np; ++p) {   // a glowing rim in the player's colour, a soft glow underneath (it floats)
        const Vec3 c = a.game.tun.start[p];
        const float* pc = playerRGB(a, p);
        const float pulse = 0.85f + 0.15f * std::sin(now * 2.f + p * 3.f);
        const float rim[4] = {pc[0] * pulse, pc[1] * pulse, pc[2] * pulse, 1};
        const float under[4] = {pc[0] * 0.25f, pc[1] * 0.25f, pc[2] * 0.25f, 1};
        const float rimSoft[4] = {rim[0] * 0.6f, rim[1] * 0.6f, rim[2] * 0.6f, 1};
        a.gfx.drawSpriteFlat(c + Vec3{0, 0.008f, 0}, 2.f * kPlatformR + 0.12f, rimSoft, {0, 1, 0}, 1);
        a.gfx.drawSpriteFlat(c + Vec3{0, -kPlatformThick - 0.02f, 0}, 3.4f, under, {0, -1, 0}, 0);
        a.gfx.drawSpriteFlat({c.x, belowY + 0.02f, c.z}, 2.6f, under, {0, 1, 0}, 0);   // its light on what is below
    }
}

// A centre-court announcement (ROUND WON, YOU WIN, who leads, who is out; a player's report, 2026-11-11: "the
// between-rounds and match-end text gets lost against the see-through arena"): the text on a dark plate with a thin
// edge in its colour, the plate faded with it. (drawText: each character 0.75 of the height wide)
static void drawAnnouncement(App& a, const std::string& t, const Pose& at, float h, const float col[4]) {
    const float w = 0.75f * h * (float)t.size() + h * 0.9f, ph = h * 1.75f;
    const float plate[4] = {0.01f, 0.02f, 0.045f, 0.82f * col[3]};
    const float edge[4] = {col[0] * 0.8f, col[1] * 0.8f, col[2] * 0.8f, 0.9f * col[3]};
    const Vec3 back = at.q.rotate({0, 0, -0.012f});
    a.gfx.drawPanel(Mat4::fromPose({at.q, at.p + back}, {w, ph, 1}), plate);
    for (int s = -1; s <= 1; s += 2)
        a.gfx.drawPanel(Mat4::fromPose({at.q, at.p + back * 0.5f + at.q.rotate({0, s * ph * 0.5f, 0})}, {w, h * 0.06f, 1}), edge);
    a.gfx.drawText(t, at, h, col);
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
    const float glass[4] = {0.02f, 0.06f, 0.11f, 0.22f * (a.mrView ? a.ptGlass : 1.f)};   // (PASSTHROUGH: GLASS WALLS)
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
// FOUR CORNERS (0.3.12): the room leans toward whoever leads: their corners pulse brighter, the grid takes on their
// colour, and a new leader washes the whole room in it for a moment.
static void drawExpArena(App& a, int pass, float now) {
    using namespace exparena;
    const Game& g = a.game;
    const int lead = g.np == 4 && g.match != MatchState::WaitingForPlayers ? g.leader() : -1;
    const float* lc = lead >= 0 ? teamRGB(a, lead) : nullptr;
    const float flash = lead >= 0 ? std::min(1.f, a.leadFlash / 1.5f) : 0.f;   // (held, then fading over the last 1.5 s)
    const float pulse = 0.5f + 0.5f * std::sin(now * 3.0f);
    const Vec3 c[4] = {{-kCorner, 0, 0}, {0, 0, kCorner}, {kCorner, 0, 0}, {0, 0, -kCorner}};
    const float side = length(c[1] - c[0]), midY = 0.5f * (kFloor + kCeiling), tall = kCeiling - kFloor;
    if (pass == 0) { drawPlatforms(a, 0, now, kFloor); return; }
    // the glass: four walls, the floor below (nearly solid, so it reads as a floor) and the ceiling
    // (PASSTHROUGH: GLASS WALLS and COURT FLOOR set the glass and the floor below, as on the classic court)
    const float gm = a.mrView ? a.ptGlass : 1.f, fm = a.mrView ? std::min(1.f, a.ptFloor) : 1.f;
    const float glass[4] = {0.02f, 0.06f, 0.11f, 0.20f * gm}, floorC[4] = {0.006f, 0.014f, 0.03f, 0.92f * fm},
                ceil[4] = {0.012f, 0.03f, 0.06f, 0.35f * gm};
    for (int w = 0; w < 4; ++w) {
        const Vec3 d = normalize(c[(w + 1) % 4] - c[w]), m = (c[w] + c[(w + 1) % 4]) * 0.5f;
        a.gfx.drawPanel(Mat4::fromPose({Quat::axisAngle({0, 1, 0}, std::atan2(-d.z, d.x)), m + Vec3{0, midY, 0}}, {side, tall, 1}), glass);
    }
    const Quat flat = Quat::axisAngle({0, 1, 0}, kPi * 0.25f) * Quat::axisAngle({1, 0, 0}, -kPi * 0.5f);
    a.gfx.drawPanel(Mat4::fromPose({flat, {0, kFloor, 0}}, {side, side, 1}), floorC);
    a.gfx.drawPanel(Mat4::fromPose({flat, {0, kCeiling, 0}}, {side, side, 1}), ceil);
    // the back walls: the corner behind each player, in its colour
    for (int p = 0; p < g.np; ++p) {
        static const int kCornerOf[4] = {0, 2, 3, 1};   // (players 2 and 3: the corners at z = -9.5 and +9.5)
        const int ci = kCornerOf[p];
        const float* pc = playerRGB(a, p);
        const bool leads = lead >= 0 && g.team(p) == lead;
        const float tint[4] = {pc[0] * 0.35f, pc[1] * 0.35f, pc[2] * 0.35f, leads ? 0.26f + 0.12f * pulse + 0.15f * flash : 0.22f};
        for (int nb = -1; nb <= 1; nb += 2) {   // the two walls meeting in that corner, as far as the back wall goes
            const Vec3 o = c[(ci + nb + 4) % 4], d = normalize(o - c[ci]);
            const float len = (kCorner - kBackX) / std::max(std::fabs(d.x), std::fabs(d.z));
            const Vec3 m = c[ci] + d * (0.5f * len);
            a.gfx.drawPanel(Mat4::fromPose({Quat::axisAngle({0, 1, 0}, std::atan2(-d.z, d.x)), m + Vec3{0, midY, 0} - normalize(Vec3{m.x, 0, m.z}) * 0.01f}, {len, tall, 1}), tint);
            const float edge[4] = {pc[0], pc[1], pc[2], 1};
            const Vec3 e = c[ci] + d * len;
            const Vec3 ln[2] = {e + Vec3{0, kFloor, 0}, e + Vec3{0, kCeiling, 0}};
            a.gfx.drawRibbon(ln, 2, leads ? 0.035f + 0.015f * pulse : 0.025f, edge, false);
        }
    }
    // the neon grid: lines up the walls at every strip, rings round the room every metre, the corners brighter
    // (FOUR CORNERS: a quarter of the way to the leader's colour, all the way for a moment when the lead changes)
    float grid[4] = {0.06f, 0.32f, 0.55f, 1}, bright[4] = {0.25f, 0.8f, 1.0f, 1};
    if (lc) {
        const float k = 0.25f + 0.75f * flash;
        for (int i = 0; i < 3; ++i) {
            grid[i] += (lc[i] * 0.55f - grid[i]) * k;
            bright[i] += (lc[i] - bright[i]) * k;
        }
    }
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
    if (g.np == 4) {   // the waiting deck outside the glass (between corners 0 and 2): a floor for the waiting spots,
        // which are at the platforms' height while the stadium round the room is lowered (testers 2026-10-30: you
        // waited in mid-air, beside the stands, nothing under you)
        // (and its twin across the room, for spots 4-7: a room for four holds four playing and four waiting, or eight
        // waiting with nobody playing; during a match, FIREBALL BLITZ's dugout)
        const float deck[4] = {0.02f, 0.035f, 0.06f, 0.96f}, rim[4] = {0.15f, 0.6f, 0.9f, 1}, spot[4] = {0.12f, 0.45f, 0.65f, 1};
        for (float sd : {1.f, -1.f}) {
            const Vec3 d = normalize(c[3] - c[0]) * sd, out = normalize(Vec3{-1, 0, -1}) * sd, mid = ((c[0] + c[3]) * 0.5f) * sd + out * 1.0f;
            const float len = 0.8f * length(c[3] - c[0]), wide = 1.8f;
            const Quat dq = Quat::axisAngle({0, 1, 0}, kPi * 0.25f) * Quat::axisAngle({1, 0, 0}, -kPi * 0.5f);
            a.gfx.drawPanel(Mat4::fromPose({dq, mid + Vec3{0, -0.01f, 0}}, {len, wide, 1}), deck);
            for (float o : {0.1f, 1.9f}) {
                const Vec3 e0 = mid - d * (0.5f * len) + out * (o - 1.0f), e1 = mid + d * (0.5f * len) + out * (o - 1.0f);
                const Vec3 ln[2] = {e0 + Vec3{0, 0.005f, 0}, e1 + Vec3{0, 0.005f, 0}};
                a.gfx.drawRibbon(ln, 2, 0.02f, rim, false);
                if (o > 1.f) { const Vec3 rail[2] = {e0 + Vec3{0, 1.0f, 0}, e1 + Vec3{0, 1.0f, 0}}; a.gfx.drawRibbon(rail, 2, 0.015f, rim, false); }
            }
        }
        for (int k = 0; k < 8; ++k) a.gfx.drawSpriteFlat(holdingPose(a, k).p + Vec3{0, 0.008f, 0}, 0.9f, spot, {0, 1, 0}, 1);
    }
    drawPlatforms(a, 1, now, kFloor);
}

// ------------------------------------------------------------------ TRAINING on court
// The targets and rings (the coach's own while it shows the lesson), the coach beside you (its own game: it really
// throws and blocks), at the far end the coach throwing at you, the board over centre court, and the words that pop up
// where something happened.
static void drawTrainBall(App& a, const Ball& b, float r, const float* rgb, int pass) {
    if (b.state == BallState::Dead || (b.state == BallState::Spawning && b.pos.y < 0.f)) return;
    DrawParams bp;
    const float glow = b.charged ? 2.0f : 0.6f;
    for (int k = 0; k < 3; ++k) bp.tintMul[k] = rgb[k] * glow;
    const float s = r / 0.08f;   // (the sphere is 8 cm)
    a.gfx.draw(a.projectile, Mat4::fromPose({Quat{}, b.pos}, {s, s, s}), bp, pass);
    if (pass == 1) {
        const float core[4] = {rgb[0], rgb[1], rgb[2], 0.9f};
        a.gfx.drawSphere(b.pos, r * 0.55f, core, true);
    }
}
static void drawTraining(App& a, int pass) {
    using namespace training;
    const Trainer& T = a.trainer;
    if (!T.active()) return;
    const Lesson& L = T.L();
    const Game& g = a.game;
    if (pass != 1) return;   // (the coach shows the lesson in its video beside the card: drawTrainVideo)
    auto faceMe = [&](const Vec3& p) { Vec3 d = p - a.viewHead.p; d.y = 0; return yawToward(length(d) > 1e-3f ? normalize(d) : Vec3{1, 0, 0}); };
    auto circle = [&](const Vec3& c, float r, const Vec3& u, const Vec3& v, float w, const float* col) {
        Vec3 pts[33];
        for (int i = 0; i <= 32; ++i) {
            const float t = i * (6.2831853f / 32);
            pts[i] = c + u * (r * std::cos(t)) + v * (r * std::sin(t));
        }
        a.gfx.drawRibbon(pts, 33, w, col, false);
    };
    const float now = float(nowSeconds() - a.startTime);
    if (T.coachThere()) {
        const Vec3 over = g.pl[1].in.head.p + Vec3{0, 0.45f, 0};
        const float gold[4] = {1.0f, 0.85f, 0.35f, 1};
        a.gfx.drawText("COACH", {faceMe(over), over}, 0.16f, gold);
    }
    // the targets: a glowing disc with rings on the far wall (a ring in the air for CURVE)
    const std::vector<Target>& tg = T.targets;
    const float t = T.clock;
    float base[3] = {1.0f, 0.78f, 0.25f};
    if (L.minSpeed > 0) { base[0] = 1.0f; base[1] = 0.35f; base[2] = 0.2f; }
    else if (L.bank) { base[0] = 0.35f; base[1] = 1.0f; base[2] = 0.5f; }
    else if (!L.targets.empty() && L.targets[0].hoopR > 0) { base[0] = 0.3f; base[1] = 0.85f; base[2] = 1.0f; }
    const Vec3 up{0, 1, 0}, side{0, 0, 1};
    for (size_t i = 0; i < tg.size(); ++i) {
        const Target& x = tg[i];
        if (!x.live) continue;
        const Vec3 c = x.at(t);
        if (x.down) {   // taken: a burst of rings, fading
            const float k = clampf((t - x.downT) / 0.6f, 0.f, 1.f);
            if (k >= 1.f) continue;
            const float col[4] = {1.f, 0.95f, 0.7f, 1.f - k};
            circle(c, x.d.r * (1.f + 2.5f * k), up, side, 0.05f * (1.f - k) + 0.01f, col);
            circle(c, x.d.r * (0.5f + 1.5f * k), up, side, 0.03f * (1.f - k) + 0.01f, col);
            continue;
        }
        const float pulse = 0.75f + 0.25f * std::sin(now * 4.f + (float)i);
        const float disc[4] = {base[0] * 0.35f * pulse, base[1] * 0.35f * pulse, base[2] * 0.35f * pulse, 1};
        a.gfx.drawSpriteFlat(c + Vec3{-0.02f, 0, 0}, 2.2f * x.d.r, disc, {-1, 0, 0}, 1);
        const float rim[4] = {base[0], base[1], base[2], 1};
        circle(c + Vec3{-0.03f, 0, 0}, x.d.r, up, side, 0.035f, rim);
        circle(c + Vec3{-0.03f, 0, 0}, x.d.r * 0.55f, up, side, 0.025f, rim);
        const float eye[4] = {1, 1, 1, 0.9f};
        a.gfx.drawSphere(c + Vec3{-0.05f, 0, 0}, 0.06f, eye, true);
        if (c.x < kWallX - 0.1f) {   // (in the air: a thin beam down to its spot on the floor, so you can judge how far)
            const float beam[4] = {base[0] * 0.5f, base[1] * 0.5f, base[2] * 0.5f, 1};
            const Vec3 ln[2] = {c - Vec3{0, x.d.r, 0}, Vec3{c.x, 0.01f, c.z}};
            a.gfx.drawRibbon(ln, 2, 0.012f, beam, false);
            const float spot[4] = {base[0] * 0.3f, base[1] * 0.3f, base[2] * 0.3f, 1};
            a.gfx.drawSpriteFlat({c.x, 0.008f, c.z}, 0.6f, spot, {0, 1, 0}, 1);
        }
        if (x.d.hoopR > 0) {   // its ring in the air, and a faint line from it to the target
            const float hc[4] = {base[0], base[1], base[2], 1};
            circle(x.d.hoop, x.d.hoopR, up, side, 0.04f * pulse, hc);
            const float faint[4] = {base[0] * 0.3f, base[1] * 0.3f, base[2] * 0.3f, 1};
            circle(x.d.hoop, x.d.hoopR * 0.15f, up, side, 0.02f, faint);
        }
    }
    // the board over centre court, facing you (not in the tutorial video's picture)
    if (!a.rttPass) {
        const Quat q = Quat::axisAngle({0, 1, 0}, deg2rad(-90.f));
        const Vec3 c0{-0.5f, 2.95f, 0};
        auto at = [&](float u, float v) { return c0 + q.rotate({u, v, 0.004f}); };
        auto txt = [&](const std::string& s, float u, float v, float h, const float* col) { a.gfx.drawText(s, {q, at(u, v)}, h, col); };
        const float board[4] = {0.004f, 0.008f, 0.018f, 0.72f}, edge[4] = {base[0], base[1], base[2], 0.95f};
        const float white[4] = {1, 1, 1, 1}, grey[4] = {0.62f, 0.68f, 0.76f, 1}, gold[4] = {1.0f, 0.85f, 0.35f, 1};
        a.gfx.drawPanel(Mat4::fromPose({q, c0 + q.rotate({0, 0, -0.01f})}, {2.4f, 0.66f, 1}), board);
        a.gfx.drawPanel(Mat4::fromPose({q, c0 + q.rotate({0, 0.33f, -0.005f})}, {2.4f, 0.02f, 1}), edge);
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s %d - %s", trackName(L.track), placeInTrack(T.lesson), L.name);
        txt(buf, 0, 0.24f, 0.06f, gold);
        const Phase ph = T.phase;
        if (ph == Phase::Card) txt("PRESS " + readyName(a) + " TO START", 0, 0.05f, 0.11f, white);
        else if (ph == Phase::Countdown) { std::snprintf(buf, sizeof buf, "%d", std::max(1, (int)std::ceil(g.stateTimer))); txt(buf, 0, 0.05f, 0.24f, white); }
        else if (ph == Phase::Done) txt(T.result.passed ? "PASSED!" : "NOT THIS TIME", 0, 0.05f, 0.16f, T.result.passed ? gold : white);
        else {
            const int cs = (int)(T.clock * 10.f);
            if (L.timed()) std::snprintf(buf, sizeof buf, "%d:%02d.%d", cs / 600, (cs / 10) % 60, cs % 10);
            else std::snprintf(buf, sizeof buf, "%d / %d", T.attempts, L.budget);
            txt(buf, 0, 0.05f, 0.2f, white);
        }
        const char* what = L.kind == THROW || L.guard == G_SENDBACK ? "TARGETS" : L.kind == STRIKES ? "STRIKES" : L.kind == CATCH ? "CAUGHT"
                         : L.guard == G_SHIELD || L.guard == G_EITHER ? "BLOCKED" : L.guard == G_KNUCKLE ? "PUNCHED" : "DODGED";
        std::snprintf(buf, sizeof buf, "%s %d / %d", what, T.successes, L.goal());
        txt(buf, -0.8f, 0.05f, 0.07f, edge);
        const int left = L.kind == THROW || L.kind == STRIKES ? L.budget - T.throws : L.budget - T.attempts;
        std::snprintf(buf, sizeof buf, "%s LEFT %d", L.kind == THROW || L.kind == STRIKES ? "THROWS" : "TRIES", std::max(0, left));
        txt(buf, 0.8f, 0.10f, 0.05f, grey);
        if (T.attempts > 0) { std::snprintf(buf, sizeof buf, "ACCURACY %d%%", T.successes * 100 / T.attempts); txt(buf, 0.8f, 0.0f, 0.05f, grey); }
        if (ph == Phase::Done) txt(readyName(a) + (T.result.passed && nextInTrack(T.lesson) >= 0 ? ": NEXT LESSON  -  MENU BUTTON: MORE" : ": TRY AGAIN  -  MENU BUTTON: MORE"),
                                   0, -0.2f, 0.045f, gold);
        else txt(!T.line.empty() ? T.line : std::string(L.tip1), 0, -0.2f, 0.04f, T.line.empty() ? grey : gold);
    }
    for (const App::TrainPop& p : a.trainPops) {   // what just happened, where it happened
        if (a.rttPass) break;
        const float fade = clampf((2.f - p.t) / 0.6f, 0.f, 1.f);
        const Vec3 at = p.pos + Vec3{0, 0.25f * p.t, 0};
        const float col[4] = {p.rgb[0], p.rgb[1], p.rgb[2], fade};
        a.gfx.drawText(p.text, {faceMe(at), at}, p.size, col);
    }
}

// TRAINING's tutorial video (SHOW ME HOW): beside a lesson's card, a screen with a real run of the lesson (filmRender),
// slow motion, and what is happening in it
static void drawTrainVideo(App& a, int pass) {
    using namespace training;
    if (pass != 1 || !a.menuOpen || !a.trainVideo || !trainSide(a) || a.trainer.phase != Phase::Card) return;
    const Lesson& L = a.trainer.L();
    const Vec3 local{1.02f, -0.04f, 0.22f};
    const Vec3 eyeLocal = a.menuPose.q.conj().rotate(a.viewHead.p - a.menuPose.p);
    const Quat q = a.menuPose.q * Quat::axisAngle({0, 1, 0}, std::atan2(eyeLocal.x - local.x, eyeLocal.z - local.z));
    const Vec3 center = a.menuPose.transform(local);
    const float frame[4] = {0.55f, 0.42f, 0.12f, 1}, deck[4] = {0.006f, 0.01f, 0.022f, 0.95f}, img[4] = {1, 1, 1, 0.999f};
    a.gfx.drawPanel(Mat4::fromPose({q, center + q.rotate({0, 0, -0.012f})}, {0.98f, 0.70f, 1}), frame);
    a.gfx.drawPanel(Mat4::fromPose({q, center + q.rotate({0, 0, -0.01f})}, {0.96f, 0.68f, 1}), deck);
    const float white[4] = {1, 1, 1, 1}, gold[4] = {1.0f, 0.85f, 0.35f, 1}, grey[4] = {0.62f, 0.68f, 0.76f, 1};
    auto text = [&](const std::string& t, float x, float y, float h, const float* c) {
        const float w = 0.75f * h * (float)t.size();
        if (w > 0.9f) h *= 0.9f / w;
        a.gfx.drawText(t, {q, center + q.rotate({x, y, 0.005f})}, h, c);
    };
    text(std::string("SHOW ME HOW - ") + L.name + " - SLOW MOTION", 0, 0.30f, 0.026f, gold);
    if (!a.film.have || !a.rttTex[0]) {
        text("NO VIDEO FOR THIS LESSON YET", 0, 0.02f, 0.03f, white);
        text(L.tip1, 0, -0.05f, 0.02f, grey);
        text(L.tip2, 0, -0.09f, 0.02f, grey);
        return;
    }
    a.gfx.drawTexturedPanel(Mat4::fromPose({q, center + q.rotate({0, 0.02f, 0.002f})}, {0.88f, 0.495f, 1}), a.rttTex[0], img);
    const bool fresh = a.film.t - a.film.captionT < 1.8f;
    text(fresh ? a.film.caption : std::string(L.tip1), 0, -0.29f, 0.024f, fresh ? white : grey);
}

// ------------------------------------------------------------------ FIREBALL BLITZ
// The octagon (it closes in as players are knocked out: its walls follow the game's), its floor level with yours, the
// glass behind each player in their colour, a neon grid; the fireballs with their flames and the puffs as they fizzle;
// everyone's health over their head (yours in front of you, at your waist)
static void drawOctArena(App& a, int pass, float now) {
    using namespace exparena;
    const Game& g = a.game;
    if (pass == 0) return;
    a.octWallR += (g.ringApothem - a.octWallR) * 0.08f;   // (the walls slide in, as everyone blinks to their new place)
    const float A = a.octWallR, R = A / std::cos(kPi / 8.f), side = 2.f * A * std::tan(kPi / 8.f);
    const float gm = a.mrView ? a.ptGlass : 1.f, fm = a.mrView ? std::min(1.f, a.ptFloor) : 1.f;
    const float glass[4] = {0.02f, 0.06f, 0.11f, 0.18f * gm}, floorC[4] = {0.006f, 0.014f, 0.03f, 0.94f * fm},
                ceil[4] = {0.012f, 0.03f, 0.06f, 0.30f * gm};
    const float pulse = 0.5f + 0.5f * std::sin(now * 3.0f);
    auto vert = [&](int i) { const float t = kPi / 8.f + i * kPi / 4.f; return Vec3{R * std::cos(t), 0, R * std::sin(t)}; };
    // the floor and the ceiling (a square as wide as the room: its corners under the stands, outside the glass)
    const Quat flat = Quat::axisAngle({1, 0, 0}, -kPi * 0.5f);
    a.gfx.drawPanel(Mat4::fromPose({flat, {0, 0.002f, 0}}, {2.f * A, 2.f * A, 1}), floorC);
    a.gfx.drawPanel(Mat4::fromPose({flat, {0, kOctH, 0}}, {2.f * A, 2.f * A, 1}), ceil);
    // the eight glass walls; the one behind a player still in, in their colour (pulsing as the room is about to close)
    const bool closing = g.ringCloseAt >= 0;
    for (int i = 0; i < 8; ++i) {
        const float t = i * kPi / 4.f;
        const Vec3 n{std::cos(t), 0, std::sin(t)};
        const Quat q = Quat::axisAngle({0, 1, 0}, std::atan2(n.x, n.z));
        int owner = -1;
        for (int p = 0; p < g.np; ++p)
            if (g.alive(p) && dot(normalize(Vec3{g.tun.start[p].x, 0, g.tun.start[p].z}), n) > 0.97f) owner = p;
        float c[4] = {glass[0], glass[1], glass[2], glass[3]};
        if (owner >= 0) {
            const float* pc = playerRGB(a, owner);
            for (int k = 0; k < 3; ++k) c[k] = pc[k] * 0.35f;
            c[3] = 0.24f + (closing ? 0.12f * pulse : 0.f);
        }
        a.gfx.drawPanel(Mat4::fromPose({q, n * (A - 0.01f) + Vec3{0, 0.5f * kOctH, 0}}, {side, kOctH, 1}), c);
    }
    // the neon: the corners up the walls, rings round the room every metre, lines across the floor
    float grid[4] = {0.06f, 0.32f, 0.55f, 1}, bright[4] = {0.25f, 0.8f, 1.0f, 1};
    if (closing) for (int k = 0; k < 3; ++k) { const float warn[3] = {1.0f, 0.45f, 0.1f}; bright[k] += (warn[k] - bright[k]) * pulse; }
    for (int i = 0; i < 8; ++i) {
        const Vec3 v = vert(i);
        const Vec3 up[2] = {v, v + Vec3{0, kOctH, 0}};
        a.gfx.drawRibbon(up, 2, 0.025f, bright, false);
    }
    for (float y = 0.01f; y <= kOctH + 0.01f; y += 1.f) {
        const bool rim = y < 0.02f;
        Vec3 ring[9];
        for (int i = 0; i <= 8; ++i) ring[i] = vert(i % 8) + Vec3{0, std::min(y, kOctH), 0};
        a.gfx.drawRibbon(ring, 9, rim ? 0.03f : 0.010f, rim ? bright : grid, false);
    }
    for (int i = -6; i <= 6; ++i) {   // the floor grid, a line a metre, inside the walls
        for (int k = 0; k < 2; ++k) {
            const float u = (float)i;
            // (as far as the octagon reaches along this line)
            float reach = A;
            for (int w = 0; w < 8; ++w) {
                const float t = w * kPi / 4.f;
                const Vec3 n{std::cos(t), 0, std::sin(t)};
                const float across = k ? n.x : n.z, along = k ? n.z : n.x;
                if (std::fabs(along) > 1e-3f) reach = std::min(reach, (A - u * across) / std::fabs(along));
            }
            if (reach <= 0.1f) continue;
            const Vec3 ln[2] = {k ? Vec3{u, 0.006f, -reach} : Vec3{-reach, 0.006f, u}, k ? Vec3{u, 0.006f, reach} : Vec3{reach, 0.006f, u}};
            a.gfx.drawRibbon(ln, 2, 0.008f, grid, false);
        }
    }
    {   // the waiting spots outside its glass, four each side (the one across is the dugout during a match)
        const float sp[4] = {0.10f, 0.35f, 0.5f, 1};
        for (int k = 0; k < 8; ++k) a.gfx.drawSpriteFlat(holdingPose(a, k).p + Vec3{0, 0.008f, 0}, 0.9f, sp, {0, 1, 0}, 1);
    }
    // each player's spot on the floor, in their colour
    for (int p = 0; p < g.np; ++p) {
        if (!g.alive(p)) continue;
        const float* pc = playerRGB(a, p);
        const float c[4] = {pc[0] * 0.5f, pc[1] * 0.5f, pc[2] * 0.5f, 1};
        a.gfx.drawSpriteFlat(g.tun.start[p] + Vec3{0, 0.008f, 0}, 1.6f, c, {0, 1, 0}, 1);
    }
}

// FIREBALL BLITZ's card, in front of you while you wait and count down (owner, 2026-11-02: "where on the body can you
// get hit, and how to avoid it?"): a glowing figure of where a fireball hits, and the ways out
static void drawBlitzHelp(App& a, int pass) {
    const Game& g = a.game;
    const int L = a.net.localIndex();
    // (owner, 2026-11-02: only where you'd want it: in the room's waiting area, in front of your spot)
    if (pass != 1 || a.spectating || a.viewing || !a.net.localWaiting()) return;
    const Pose spotPose = holdingPose(a, a.net.localSpot());
    const Vec3 f = spotPose.q.rotate({0, 0, -1}), right{-f.z, 0, f.x};
    const Quat q = yawToward(f);   // (facing you)
    const Vec3 c = spotPose.p + f * 1.3f + Vec3{0, 1.25f, 0};
    const float bg[4] = {0.004f, 0.008f, 0.018f, 0.85f}, edge[4] = {0.3f, 0.85f, 1.0f, 0.9f};
    a.gfx.drawPanel(Mat4::fromPose({q, c + q.rotate({0, 0, -0.01f})}, {1.30f, 0.80f, 1}), bg);
    a.gfx.drawPanel(Mat4::fromPose({q, c + q.rotate({0, 0.40f, -0.005f})}, {1.30f, 0.008f, 1}), edge);
    auto txt = [&](const char* t, float x, float y, float h, const float* col) { a.gfx.drawText(t, {q, c + q.rotate({x, y, 0.004f})}, h, col); };
    const float cyan[4] = {0.3f, 0.85f, 1.0f, 1}, white[4] = {1, 1, 1, 1}, grey[4] = {0.65f, 0.7f, 0.78f, 1}, red[4] = {1.0f, 0.35f, 0.25f, 1};
    txt("HOW NOT TO GET HIT", 0, 0.31f, 0.06f, cyan);
    // the figure: head, body and legs, in red (a hit anywhere counts), and a fireball hopping toward it
    const Vec3 fig = c + q.rotate({-0.42f, 0.02f, 0.02f});
    const float zone[4] = {0.45f, 0.06f, 0.04f, 1};
    a.gfx.drawSphere(fig + Vec3{0, 0.20f, 0}, 0.038f, zone, true);   // (head, body, legs: thin, so it reads as a figure)
    for (int i = 0; i <= 6; ++i) a.gfx.drawSphere(fig + Vec3{0, 0.13f - 0.036f * i, 0}, i == 6 ? 0.04f : 0.034f, zone, true);   // (to the hips)
    const float legs[4] = {0.10f, 0.12f, 0.16f, 1};   // (the legs, dim: a hit there doesn't count)
    for (int i = 0; i <= 7; ++i)
        for (int sd = -1; sd <= 1; sd += 2) a.gfx.drawSphere(fig + Vec3{0, -0.08f - 0.026f * i, 0} + right * (0.022f * sd), 0.014f, legs, true);
    const float t = std::fmod(float(nowSeconds()), 1.6f) / 1.6f;   // (a fireball hops in from the side, up to the waist)
    const Vec3 ball = fig + right * (-0.22f + 0.17f * t) + Vec3{0, -0.27f + 0.20f * std::fabs(std::sin(t * 6.28f)), 0};
    const float fb[4] = {1.0f, 0.5f, 0.1f, 1};
    a.gfx.drawSphere(ball, 0.016f, fb, true);
    txt("HIPS AND UP", -0.42f, -0.33f, 0.026f, red);
    // the ways out
    const float x0 = 0.17f;
    txt("HITS COUNT FROM YOUR HIPS UP", x0, 0.19f, 0.032f, white);
    txt("STEP ASIDE, OR DUCK AS IT RISES", x0, 0.09f, 0.032f, white);
    txt("PUNCH IT AWAY: YOUR KNUCKLE GUARDS", x0, 0.0f, 0.032f, white);
    if (g.tun.shields) txt("OR BLOCK IT WITH YOUR SHIELD", x0, -0.08f, 0.032f, white);
    txt("A FIREBALL TURNS RED, AND A RED RING", x0, g.tun.shields ? -0.18f : -0.12f, 0.026f, red);
    txt("SHOWS AT YOUR FEET, WHEN IT WILL HIT YOU", x0, g.tun.shields ? -0.23f : -0.17f, 0.026f, red);
    txt("IT FIZZLES OUT AT ITS 4TH BOUNCE", x0, -0.31f, 0.024f, grey);
}

static const Vec3 kUpV{0, 1, 0};
static void drawFireballs(App& a, int pass, const float (*teamCol)[4]) {
    const Game& g = a.game;
    const int L = a.net.localIndex();
    const float now = float(nowSeconds() - a.startTime);
    for (int p = 0; p < g.np; ++p)
        for (int k = 0; k < Game::kMaxFire; ++k) {
            const Ball& f = g.fire[p][k];
            App::FireEcho& fe = a.fireEcho[p][k];
            if (f.state != BallState::Flying) { fe.n = 0; continue; }
            const float* tc = teamCol[p & 3];
            const float r = g.tun.projectileRadius * f.scale;
            // its echoes (a player's idea, 2026-11-10: smaller copies trailing the one you threw, along the hops it just
            // made). Drawn here only, from where this screen showed it: no rule or network change. Smaller and fainter
            // with age, without the core, the floor light or the warning, so the real one always stands out
            {
                const int N = App::FireEcho::kN;
                const int last = (fe.head + N - 1) % N;
                if (fe.n && length(f.pos - fe.p[last]) > 1.5f) fe.n = 0;   // (a new fireball in this place)
                if (!fe.n || now - fe.t[last] >= 1.f / 120.f) {           // (once a frame, however often it's drawn)
                    fe.p[fe.head] = f.pos; fe.t[fe.head] = now;
                    fe.head = (fe.head + 1) % N; fe.n = std::min(N, fe.n + 1);
                }
                const float fireC[3] = {1.0f, 0.42f, 0.08f};
                for (int e = 1; e <= 4; ++e) {
                    const float want = now - 0.055f * e;   // (where it was this long ago, between two of the points)
                    Vec3 at{};
                    bool found = false;
                    for (int i = 0; i + 1 < fe.n && !found; ++i) {
                        const int a1 = (fe.head + N - 1 - i) % N, a0 = (fe.head + N - 2 - i) % N;
                        if (fe.t[a0] <= want && want <= fe.t[a1]) {
                            const float u = (want - fe.t[a0]) / std::max(1e-4f, fe.t[a1] - fe.t[a0]);
                            at = lerp(fe.p[a0], fe.p[a1], u);
                            found = true;
                        }
                    }
                    if (!found || length(at - f.pos) < r * 1.6f) continue;   // (too young, or bunched up behind a slow one)
                    const float fade = 0.75f - 0.15f * e;                     // 0.6, 0.45, 0.3, 0.15
                    const float sc = f.scale * (g.tun.projectileRadius / 0.08f) * (0.72f - 0.12f * e);   // 60 .. 24 %
                    DrawParams ep;
                    for (int c = 0; c < 3; ++c) ep.tintMul[c] = 2.0f * fade * (0.65f * fireC[c] + 0.35f * tc[c]);
                    ep.alphaMul = fade;
                    ep.uvOff[0] = wrapUV(now * 0.6 + e * 0.13); ep.uvOff[1] = wrapUV(now * 0.4);
                    a.gfx.draw(a.projectile, Mat4::fromPose({Quat{}, at}, {sc, sc, sc}), ep, pass);
                    a.gfx.draw(a.shell, Mat4::fromPose({Quat::axisAngle(kUpV, now * 9.f + e), at}, {sc, sc, sc}), ep, pass);
                    if (pass == 1) {
                        const float eg[4] = {(0.5f + 0.5f * tc[0]) * fade, (0.25f + 0.25f * tc[1]) * fade, (0.05f + 0.2f * tc[2]) * fade, 1};
                        a.gfx.drawSprite(at, r * 6.f * (0.72f - 0.12f * e), eg, a.viewHead.p, 0);
                    }
                }
            }
            {   // the ball you threw, as it was on the tee, on fire: its plasma sphere and shell in fire colours, both passes
                DrawParams bp;
                const float fire[3] = {1.0f, 0.42f, 0.08f};
                for (int c = 0; c < 3; ++c) bp.tintMul[c] = 2.0f * (0.65f * fire[c] + 0.35f * tc[c]);
                const float s = f.scale * (g.tun.projectileRadius / 0.08f);
                DrawParams pp = bp;
                pp.uvOff[0] = wrapUV(now * 0.6); pp.uvOff[1] = wrapUV(now * 0.4);   // (its veins racing: flames)
                a.gfx.draw(a.projectile, Mat4::fromPose({Quat{}, f.pos}, {s, s, s}), pp, pass);
                a.gfx.draw(a.shell, Mat4::fromPose({Quat::axisAngle(kUpV, now * 9.f), f.pos}, {s, s, s}), bp, pass);
                if (pass == 0) continue;
            }
            // its flames: a white-hot core, an orange glow in its colour, a flickering tail behind it (coming at you: red)
            const bool danger = (a.fireDanger >> (p * Game::kMaxFire + k)) & 1;
            const float core[4] = {1.0f, danger ? 0.5f : 0.9f, danger ? 0.4f : 0.6f, 1};
            a.gfx.drawSphere(f.pos, r * 0.6f, core, true);
            const float pulse = 0.6f + 0.4f * std::sin(now * 18.f);
            const float glow[4] = {danger ? 1.0f * pulse : 0.5f + 0.5f * tc[0], danger ? 0.08f : 0.25f + 0.25f * tc[1], danger ? 0.05f : 0.05f + 0.2f * tc[2], 1};
            a.gfx.drawSprite(f.pos, r * 6.f, glow, a.viewHead.p, 0);
            const float sp = length(f.vel);
            const Vec3 back = sp > 1e-3f ? -f.vel / sp : Vec3{0, 0, 0};
            for (int i = 1; i <= 6; ++i) {
                const float flick = 0.8f + 0.2f * std::sin(now * 31.f + i * 1.7f + p * 3.1f + k);
                const float fade = (1.f - i / 7.f) * flick;
                const float fc[4] = {1.0f * fade, (0.45f + 0.2f * tc[1]) * fade, 0.08f * fade, 1};
                a.gfx.drawSprite(f.pos + back * (0.07f * i), r * (4.5f - 0.5f * i), fc, a.viewHead.p, 0);
            }
            const float pool[4] = {0.35f, 0.14f, 0.03f, 1};   // (its light on the floor below)
            a.gfx.drawSpriteFlat({f.pos.x, 0.005f, f.pos.z}, 0.6f + 0.4f * f.pos.y, pool, {0, 1, 0}, 0);
        }
    if (pass != 1) return;
    if (a.fireDanger) {   // one is coming at you: a red ring round your feet
        const Vec3 hd = g.pl[L].in.head.p;
        const float pulse = 0.6f + 0.4f * std::sin(now * 18.f);
        const float ring[4] = {0.9f * pulse, 0.08f, 0.05f, 1};
        a.gfx.drawSpriteFlat({hd.x, 0.012f, hd.z}, 1.1f, ring, {0, 1, 0}, 1);
    }
    for (const App::Puff& u : a.puffs) {   // a fireball fizzling out: a puff of sparks and smoke, rising
        const float k = 1.f - u.t / 0.6f;
        if (k <= 0) continue;
        const float c[4] = {1.0f * k, 0.5f * k, 0.12f * k, 1};
        for (int i = 0; i < 6; ++i) {
            const float t = i * 1.047f;
            const Vec3 d{std::cos(t) * 0.25f * u.t / 0.6f, 0.3f * u.t, std::sin(t) * 0.25f * u.t / 0.6f};
            a.gfx.drawSprite(u.pos + d, 0.12f * k + 0.05f, c, a.viewHead.p, 0);
        }
    }
    // health: a pip a hit, over everyone's head (turned to you); yours in front of you at your waist
    const int maxH = std::max(1, g.tun.health);
    for (int p = 0; p < g.np; ++p) {
        if (!g.alive(p) || (g.match == MatchState::WaitingForPlayers && !g.pl[p].in.headTracked)) continue;
        const bool mine = p == L && !a.viewing && !a.spectating;
        const Vec3 head = mine ? g.pl[p].in.head.p : a.viewIn[p].head.p;
        Vec3 at = head + Vec3{0, 0.40f, 0};
        Vec3 to = a.viewHead.p - at;
        if (mine) {
            const Vec3 f = g.facing(p);
            at = Vec3{head.x, std::max(0.6f, head.y - 0.75f), head.z} + f * 0.55f;
            to = head - at;
        }
        const Quat q = Quat::axisAngle({0, 1, 0}, std::atan2(to.x, to.z)) * Quat::axisAngle({1, 0, 0}, mine ? -0.6f : 0.f);
        const float pip = maxH > 5 ? 0.045f : 0.07f, gap = pip * 1.35f;
        const float* tc = teamCol[p & 3];
        const float back[4] = {0.01f, 0.02f, 0.04f, 0.7f};
        a.gfx.drawPanel(Mat4::fromPose({q, at - q.rotate({0, 0, 0.004f})}, {gap * maxH + 0.04f, pip + 0.04f, 1}), back);
        for (int i = 0; i < maxH; ++i) {
            const bool full = i < g.pl[p].health;
            const float c[4] = {full ? tc[0] * 0.7f + 0.3f : 0.12f, full ? tc[1] * 0.7f + 0.3f : 0.12f, full ? tc[2] * 0.7f + 0.3f : 0.14f, full ? 0.95f : 0.6f};
            a.gfx.drawPanel(Mat4::fromPose({q, at + q.rotate({(i - 0.5f * (maxH - 1)) * gap, 0, 0})}, {pip, pip, 1}), c);
        }
    }
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
    const bool corners = g.arenaKind == ARENA_CORNERS || g.arenaKind == ARENA_FOURCORNERS, oct = g.octagon();
    const bool exp = corners || oct || g.arenaKind == ARENA_DIAMOND;   // an EXPERIMENTAL room
    // (CORNERS reaches 9.5 m to the sides: the stadium widened to stand round it)
    // (and lowered below the room's floor: the stadium's own floor round the classic court ran through the room as a ledge)
    const Mat4 outside = corners ? Mat4::fromPose({Quat{}, {0, exparena::kFloor - 0.4f, 0}}, {1.35f, 1.f, 1.35f})
                       : oct ? Mat4::fromPose({Quat{}, {0, -0.4f, 0}}, {1.35f, 1.f, 1.35f}) : Mat4::identity();   // (FIREBALL BLITZ: as wide)
    updateSides(a);
    updateStats(a);
    float pcol[kMaxPlayers][3];   // each player's colour (playerRGB)
    for (int p = 0; p < kMaxPlayers; ++p) for (int k = 0; k < 3; ++k) pcol[p][k] = playerRGB(a, p < g.np ? p : p & 1)[k];
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
        if (!minimal) drawCourtModel(a, a.floorReflect, Mat4::identity(), rp, 1);   // (MINIMAL: no stadium in the floor either)
        if (!a.mrView && !minimal && a.floorReflectEnv.valid()) a.gfx.draw(a.floorReflectEnv, Mat4::identity(), rp, 1);
        const Vec3 eye = a.viewHead.p;
        const float (*tcol)[3] = pcol;
        for (int p = 0; p < g.np; ++p) {
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
        drawCourtModel(a, a.floor, Mat4::identity(), fp, 1);
    }
    if (!a.mrView && !minimal) a.gfx.draw(a.environment, outside, dp, pass);   // PASSTHROUGH: your room instead
    if (keyBg) {
        // (nothing of the court)
    } else if (exp) {
        if (g.arenaKind == ARENA_DIAMOND) drawDiamondArena(a, pass, now); else if (oct) drawOctArena(a, pass, now); else drawExpArena(a, pass, now);
    } else if (outline) {
        drawCourtModel(a, a.courtOutline, Mat4::identity(), dp, pass);
    } else {
        DrawParams ap;
        ap.glassMul = a.mrView ? a.ptGlass : 1.f;   // PASSTHROUGH: GLASS WALLS
        if (isDesktop(a) || a.shotGlass) ap.glowMul = a.dcGlass;   // (the PC spectator's ARENA GLASS)
        drawCourtModel(a, a.arena, Mat4::identity(), ap, pass);
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
        const Vec3 st = spectatorSeat(a).p;
        a.gfx.drawSpriteFlat(st + Vec3{0, 0.004f, 0}, 1.8f, pc, {0, 1, 0}, 0);
        a.gfx.drawSpriteFlat(st + Vec3{0, 0.006f, 0}, 2.0f, rc, {0, 1, 0}, 1);
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
        xp.uvOff[0] = wrapUV(now * 0.035);   // crowd lights twinkle (env_fx = crowd lights only; they sample a scrolling twinkle strip)
        if (!a.mrView && !minimal) a.gfx.draw(a.envFx, outside, xp, 1);
        DrawParams cp;   // static glow layers: light halos, holographic rings, end-zone energy, light beams
        DrawParams cg = cp;   // (the court's: the PC spectator's ARENA GLASS fades its hexagons and end-zone rings)
        if (isDesktop(a) || a.shotGlass) cg.glowMul = a.dcGlass;
        if (!outline && !minimal && !exp) drawCourtModel(a, a.courtFx, Mat4::identity(), cg, 1);
        if (!a.mrView && !minimal && a.stadiumFx.valid()) a.gfx.draw(a.stadiumFx, outside, cp, 1);
        DrawParams pp;   // energy pulses racing along the neon outline
        pp.uvOff[1] = wrapUV(-now * 0.9);
        if (!keyBg && !exp) a.gfx.draw(a.courtPulse, Mat4::identity(), pp, 1);
    }
    float teamCol[kMaxPlayers][4];   // (each player's colour, for its ball, spawner, ring and trail)
    for (int p = 0; p < kMaxPlayers; ++p) { for (int k = 0; k < 3; ++k) teamCol[p][k] = pcol[p][k]; teamCol[p][3] = 1; }
    if (pass == 1 && !a.marks.empty()) drawMarks(a);   // BALL MARKS where the balls bounced
    if (pass == 1 && a.mockContact) {   // (--contact, pictures only: a soft shadow on the floor under each player and ball)
        const Game& cg = a.game;
        auto floorBelow = [&](const Vec3& at, float& y) {   // the arena's own floor under it (as BALL MARKS finds surfaces)
            Vec3 hp, hn; uint32_t tags = 0;
            if (!cg.sweepArena(at, {at.x, -2.f, at.z}, 0.02f, hp, hn, tags) || hn.y < 0.6f) return false;
            y = hp.y - 0.02f + 0.006f;
            return true;
        };
        for (int p = 0; p < cg.np; ++p) {
            const Vec3 h = cg.pl[p].in.head.p;
            float y;
            if (!floorBelow(h, y)) y = 0.03f;   // (where the players stand, beyond the balls' floor: the drawn floor)
            a.gfx.drawShadowFlat({h.x, y, h.z}, 0.95f, 0.6f, {0, 1, 0});
            auto ballShadow = [&](const Ball& b) {
                float fy;
                if (b.state == BallState::Dead || !floorBelow(b.pos, fy)) return;
                const float ht = b.pos.y - fy;
                a.gfx.drawShadowFlat({b.pos.x, fy, b.pos.z}, 0.22f + 0.14f * ht, 0.6f * clampf(1.f - ht / 3.f, 0.15f, 1.f), {0, 1, 0});
            };
            ballShadow(cg.pl[p].ball);
            for (int k = 0; k < Game::kMaxFire; ++k) if (cg.fire[p][k].state == BallState::Flying) ballShadow(cg.fire[p][k]);
        }
    }
    if (pass == 1 && !minimal && !a.mrView)   // The Crowd: flags waving in the stands, the cheers pouring out of them (the
        drawFanFlags(a, now, corners || oct ? 1.35f : 1.f,   // EXPERIMENTAL arenas: in their widened, lowered stadium)
                     corners ? exparena::kFloor - 0.4f : oct ? -0.4f : 0.f);
    if (pass == 1 && !minimal && !a.mrView && !corners && !oct) drawJumbotron(a);   // the screen over centre court: names, score, stats
    for (int p = 0; p < g.np && !minimal; ++p) {
        if (oct && !g.alive(p)) continue;   // (FIREBALL BLITZ: out, no tee)
        const BallState st = g.pl[p].ball.state;
        float glow = (st == BallState::Spawning || st == BallState::Dead) ? 0.55f : 0.16f;
        DrawParams sp;
        sp.emissiveAdd[0] = teamCol[p][0] * glow; sp.emissiveAdd[1] = teamCol[p][1] * glow; sp.emissiveAdd[2] = teamCol[p][2] * glow;
        a.gfx.draw(a.spawner, Mat4::fromPose({Quat{}, g.tun.spawner[p]}), sp, pass);
        // the SHOT CLOCK (owner, 2026-11-03: as the original showed it, but simple): a ring round the launchpad that
        // builds while your ball sits in your end, full and red once it has run out (each throw of the other player is
        // then their strike); nothing while the clock is full
        const float clock = g.pl[p].shotClock, full = std::max(0.1f, g.tun.shotClockSeconds);
        if (pass == 1 && g.np == 2 && g.tun.useShotClock && g.match == MatchState::InProgress && clock < full - 0.05f) {
            const int kSeg = 48, lit = clock <= 0.f ? kSeg : (int)std::ceil(kSeg * (1.f - clock / full));
            const float R = 0.42f, pulse = clock <= 0.f ? 0.75f + 0.25f * std::sin((float)now * 8.f) : 1.f;
            const float red[4] = {1.0f * pulse, 0.18f * pulse, 0.12f * pulse, 1};
            const float col[4] = {teamCol[p][0] * 0.9f, teamCol[p][1] * 0.9f, teamCol[p][2] * 0.9f, 1};
            const Vec3 c = g.tun.spawner[p] + Vec3{0, 0.03f, 0};
            const float start = std::atan2(g.facing(p).z, g.facing(p).x);   // (it starts toward the court and goes round)
            for (int k = 0; k < lit; ++k) {
                const float ang = start + 2.f * kPi * (k + 0.5f) / kSeg;
                const Vec3 at = c + Vec3{R * std::cos(ang), 0, R * std::sin(ang)};
                const Quat q = Quat::axisAngle({0, 1, 0}, -ang);   // (each piece along the circle)
                a.gfx.drawBox(Mat4::fromPose({q, at}, {0.011f, 0.003f, kPi * R / kSeg * 0.92f}), clock <= 0.f ? red : col, true);
            }
        }
    }
    bool anyoneWatched = a.watchRoom.botMask != 0 || !a.watchRoom.members.empty();
    for (int p = 0; p < kMaxPlayers; ++p) anyoneWatched |= a.watchRoom.slot[p] != 0;
    for (int p = 0; p < g.np; ++p) {
        if (!a.spectating && !a.net.slotFilled(p)) continue;
        if (only >= 0 && p != only) continue;
        // (TRAINING: nobody at the far end, or the coach's own game shows it there)
        if (p == 1 && a.trainer.active() && !a.trainer.coachThere()) continue;
        // (an empty place left out; the close-ups (VS, the win screen) picked their player themselves: always drawn)
        if (only < 0 && a.spectating && a.watchIp && !a.watchRoom.slot[p] && !a.watchRoom.isBot(p) && anyoneWatched) continue;
        if (oct && g.match != MatchState::WaitingForPlayers && !((g.startMask >> p) & 1)) continue;   // (FIREBALL BLITZ: not in this match)
        if (oct && !g.alive(p) && g.match != MatchState::WaitingForPlayers) {   // (knocked out: a faint ghost where they watch from)
            const PlayerState& ps0 = g.pl[p];
            if (pass == 1 && (p != L || a.viewing) && !ps0.isBot) {
                const float gc[4] = {teamCol[p][0] * 0.25f, teamCol[p][1] * 0.25f, teamCol[p][2] * 0.25f, 1};
                const Vec3 hd = a.viewIn[p].head.p;
                for (int k = 0; k < 4; ++k) a.gfx.drawSprite(hd - Vec3{0, 0.4f * k, 0}, 0.45f - 0.05f * k, gc, a.viewHead.p, 0);
                const float oc[4] = {teamCol[p][0] * 0.5f + 0.3f, teamCol[p][1] * 0.5f + 0.3f, teamCol[p][2] * 0.5f + 0.3f, 0.8f};
                const Vec3 to = a.viewHead.p - hd;
                a.gfx.drawText("OUT", {Quat::axisAngle({0, 1, 0}, std::atan2(-to.x, -to.z) + kPi), hd + Vec3{0, 0.35f, 0}}, 0.10f, oc);
            }
            continue;
        }
        PlayerState& ps = g.pl[p];
        // hands: lancer tool, knuckle guard, shield (gauntlets wear the player's avatar colours); remote players smoothed
        // between network updates, the two drawn hands kept apart (HANDS APART)
        const AvatarDesc& look = (p == L && !a.viewing) ? a.myAvatar : a.viewIn[p].avatar;
        const PlayerInput vin = handsApart(a, a.viewIn[p], look);
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
                a.gfx.draw(a.shield, grip * Mat4::fromPose(g.shieldLocal(h)) * shieldSizeM(g) * g_plateLocal, sp, pass);
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
            if (a.shotHitboxes && pass == 1) {   // (pictures: what hits, over what is drawn)
                static const float green[4] = {0.0f, 0.5f, 0.1f, 1.f}, red[4] = {0.6f, 0.05f, 0.0f, 1.f};
                a.gfx.drawBox(Mat4::fromPose(g.knucklePose(p, h), g.tun.knuckleHalf), green, true);
                if (ps.hands[h].shieldUp || p == 0) a.gfx.drawBox(Mat4::fromPose(g.shieldPose(p, h), g.tun.shieldHalfNow()), red, true);
                if (!ps.hands[h].shieldUp && p == 0) {   // (and the shield as drawn when up)
                    DrawParams sp;
                    sp.alphaMul = 0.7f;
                    a.gfx.draw(a.shield, grip * Mat4::fromPose(g.shieldLocal(h)) * shieldSizeM(g) * g_plateLocal, sp, pass);
                }
            }
        }
        // opponent body (local player has no body, like the original first-person view); spectators see both
        // (the PC spectator's eyes camera: that player's own body left out, as in their headset)
        if (p == a.dcEyesOf && !a.rttPass) {
        } else if ((p != L || (a.viewing && !a.shotGloves)) && !a.avModels.empty() && vin.head.p.y > 0.3f) {
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
            if (p == a.net.localIndex() && (a.reachCue[0] || a.reachCue[1])) glow *= 1.6f;   // (in reach: brighter)
            bp.tintMul[0] = teamCol[p][0] * glow; bp.tintMul[1] = teamCol[p][1] * glow; bp.tintMul[2] = teamCol[p][2] * glow;
            if (oct) {   // (FIREBALL BLITZ: on fire on the tee as in flight)
                const float fire[3] = {1.0f, 0.42f, 0.08f};
                for (int c = 0; c < 3; ++c) bp.tintMul[c] = 2.0f * (0.65f * fire[c] + 0.35f * teamCol[p][c]);
            }
            float s = b.scale * (g.tun.projectileRadius / 0.08f);   // visual sphere is 8 cm; collision radius 10 cm
            if (b.state == BallState::Spawning) {
                float r = clampf((b.pos.y - g.tun.spawnLauncherHeight) / std::max(0.05f, g.idlePoint(p).y - g.tun.spawnLauncherHeight), 0.f, 1.f);
                s *= 0.35f + 0.65f * std::min(1.f, r * 2.5f);
            }
            // THE PUNCH LANDS: just punched, squashed along its new way and bulging round it, round again in 0.14 s
            Mat4 squash = Mat4::identity();
            if (a.squash[p].t < 0.14f && b.state == BallState::Flying) {
                const float amt = a.squash[p].power * 0.32f * (1.f - a.squash[p].t / 0.14f) * (1.f - a.squash[p].t / 0.14f);
                const float along = 1.f - amt, round = 1.f + 0.5f * amt;
                const Vec3 ax = a.squash[p].axis;
                Vec3 col[3];
                for (int i = 0; i < 3; ++i) {
                    Vec3 e{0, 0, 0};
                    (&e.x)[i] = 1.f;
                    col[i] = e * round + ax * ((along - round) * (&ax.x)[i]);
                }
                squash = Mat4::fromBasis(col[0], col[1], col[2], {0, 0, 0});
            }
            // BALL FEEL (the room's): GLOW TRUE SIZE draws its outside (the shell, 12.5 cm) at the 10 cm it hits; WEIGHT
            // HEAVY a held ball a little behind the hand; SPIN VISIBLE the whole ball turned as it tumbles and spins
            const int feel = a.game.tun.ballFeel;
            const float vis = (feel & BALL_TRUE) ? 0.8f : 1.f;
            s *= vis;
            const Vec3 bpos = (feel & BALL_WEIGHT) && b.state == BallState::Held && a.heldVisOn[p] ? a.heldVis[p] : b.pos;
            const Quat turn = (feel & BALL_SPIN) ? a.ballQ[p] : Quat{};
            Mat4 m = Mat4::fromPose({Quat{}, bpos}) * squash * Mat4::fromPose({turn, {0, 0, 0}}, {s, s, s});
            {
                DrawParams pp = bp;   // energy veins drift across the plasma sphere
                float tt = float(nowSeconds() - a.startTime);
                pp.uvOff[0] = wrapUV(tt * 0.21 + a.trail[p].spinAngle * 0.05);
                pp.uvOff[1] = wrapUV(tt * 0.13);
                a.gfx.draw(a.projectile, m, pp, pass);
            }
            {   // the shell fragments spin with the ball (VFX_Projectile_Shell)
                float sl = length(b.spin);
                Quat sq = (feel & BALL_SPIN) ? a.ballQ[p] : sl > 1e-3f ? Quat::axisAngle(b.spin / sl, a.trail[p].spinAngle) : Quat{};
                a.gfx.draw(a.shell, Mat4::fromPose({Quat{}, bpos}) * squash * Mat4::fromPose({sq, {0, 0, 0}}, {s, s, s}), bp, pass);
            }
            if ((feel & BALL_CORE) && pass == 0) {   // (BALL FEEL CORE SOLID: a lit core inside the plasma, 62% of what hits)
                DrawParams cp;
                cp.forceShader = SH_LIT;
                for (int k = 0; k < 3; ++k) { cp.tintMul[k] = teamCol[p][k] * (b.charged ? 0.8f : 0.35f); cp.emissiveAdd[k] = teamCol[p][k] * (b.charged ? 0.38f : 0.12f); }
                const float cs = g.ballRadius(p) * 0.62f / 0.08f;
                a.gfx.draw(a.projectile, Mat4::fromPose({Quat{}, bpos}) * squash * Mat4::fromPose({turn, {0, 0, 0}}, {cs, cs, cs}), cp, pass);
            }
            const int t0 = a.trailMax > 0 ? std::max(0, a.trail[p].n - a.trailMax) : 0;   // (--record: the oldest left out)
            if (pass == 1 && a.trailMax >= 0 && a.trail[p].n - t0 > 1) {
                const App::Trail& tr = a.trail[p];
                const int tn = tr.n - t0;
                float tc[4] = {teamCol[p][0] * 1.2f, teamCol[p][1] * 1.2f, teamCol[p][2] * 1.2f, 1};
                a.gfx.drawRibbon(tr.core + t0, tn, g.ballRadius(p) * 0.7f, tc);
                if (length(b.spin) > 3.0f || b.state == BallState::Spawning) {   // twisting tracers: spin, or the spawn rise
                    float hc[4] = {0.9f, 0.9f, 0.9f, 1};
                    a.gfx.drawRibbon(tr.helixA + t0, tn, 0.012f, hc);
                    a.gfx.drawRibbon(tr.helixB + t0, tn, 0.012f, hc);
                }
            }
            if (pass == 1 && b.state != BallState::Spawning) {
                // electric arcs crackling over the plasma sphere: jagged great-circle segments, re-rolled ~18x/s
                const float now2 = float(nowSeconds() - a.startTime);
                const uint32_t tick = (uint32_t)(now2 * 18.f);
                const float R0 = g.ballRadius(p) * 1.08f * vis;
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
                        pts[i] = bpos + (d * (1.f + 0.12f * jit) + side * jit * 0.45f) * R0;
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
                a.gfx.drawSphere(bpos, g.ballRadius(p) * 0.55f * vis, core, true);
            }
        }
    }
    if (oct && only < 0) drawFireballs(a, pass, teamCol);
    if (oct && only < 0 && !a.rttPass) drawBlitzHelp(a, pass);
    if (pass == 1 && only < 0)
        for (int p = 0; p < g.np; ++p)
            if (a.trail[p].burst > 0) {   // ball dissolving at the back wall
                float k = a.trail[p].burst / 0.45f;
                float c[4] = {teamCol[p][0] * k * 2, teamCol[p][1] * k * 2, teamCol[p][2] * k * 2, 1};
                a.gfx.drawSphere(a.trail[p].burstPos, 0.1f + (1 - k) * 0.6f, c, true);
            }
    for (int p = 0; p < g.np && !minimal && !oct; ++p) {   // strike-zone rings (FIREBALL BLITZ: no strikes)
        DrawParams zp;
        float f = 0.35f + 1.5f * a.trail[p].strikeFlash;
        zp.forceShader = SH_ADDITIVE;
        if (isDesktop(a) || a.shotGlass) f *= a.dcGlass;   // (the PC spectator's ARENA GLASS)
        zp.tintMul[0] = teamCol[p][0] * f; zp.tintMul[1] = teamCol[p][1] * f; zp.tintMul[2] = teamCol[p][2] * f;
        // (the ring faces along its player's facing: corners 2 and 3 a quarter turn round)
        const Quat rq = p < 2 ? Quat{} : Quat::axisAngle({0, 1, 0}, kPi * 0.5f);
        // (STRIKE ZONE: the ring as big as it plays. Its model is a 75 cm disc, and it was drawn at that for a 70 cm
        // strike: a ball could cross inside the ring and score no strike; Grant's pull request #26, 2026-11-18)
        const float zs = g.tun.strikeRadius() / kStrikeRingModel;
        a.gfx.draw(a.strikezone, Mat4::fromPose({rq, g.strikeZoneCenter(p)}, {zs, zs, zs}), zp, pass);
    }
    if (!minimal) drawRoomPeople(a, pass);
    if (pass == 1 && a.shotQueue >= 0) {   // (--scene queue: a waiting spot's kiosk and queue screen, sample names)
        const std::string next[2] = {"YOU", "SPARKY"};
        drawQueueStation(a, 0, 2, next);
    }
    if (!minimal) drawLobby(a, pass);
    if (!minimal) drawTraining(a, pass);
    if (pass == 1 && !a.rttPass) drawVoiceHud(a);
    if (pass == 1 && !a.rttPass) drawReadyUp(a);
    if (pass == 1 && !a.rttPass && a.headSelfie && a.spectating && !isDesktop(a) && a.hsTex) {   // SELFIE: in the lower right
        const Pose& h = a.viewHead;
        const Vec3 c = h.transform({0.19f, -0.15f, -0.55f});
        const float sz = 0.15f, white[4] = {1, 1, 1, 1}, edge[4] = {0.3f, 0.85f, 1.0f, 0.9f}, t = 0.004f;
        a.gfx.drawTexturedPanel(Mat4::fromPose({h.q, c}, {sz, sz, 1}), a.hsTex, white, true);
        for (int k = 0; k < 4; ++k) {
            const Vec3 o = k < 2 ? Vec3{0, (k ? 0.5f : -0.5f) * sz, -0.0005f} : Vec3{(k == 3 ? 0.5f : -0.5f) * sz, 0, -0.0005f};
            a.gfx.drawPanel(Mat4::fromPose({h.q, c + h.q.rotate(o)}, k < 2 ? Vec3{sz + t, t, 1} : Vec3{t, sz + t, 1}), edge);
        }
        const float red[4] = {1.0f, 0.25f, 0.2f, 1};
        a.gfx.drawSprite(c + h.q.rotate({-0.5f * sz + 0.012f, 0.5f * sz - 0.012f, 0.001f}), 0.012f, red, h.p, 0);   // (a "live" light)
    }
    if (pass == 1 && only < 0) drawFx(a);
    if (pass == 1 && !minimal && !a.trainer.active()) {
        // Holographic scoreboard over centre court, facing the local player (mirrored for the far-end player)
        char buf[96];
        const float white[4] = {1, 1, 1, 1}, grey[4] = {0.55f, 0.6f, 0.68f, 1};
        int rival = g.np == 2 ? 1 - L : -1;   // (four players: a rival's colour for the right-hand edge)
        for (int q = 0; q < g.np && rival < 0; ++q) if (g.rivals(L, q)) rival = q;
        const float* myCol = teamCol[L];
        const float* opCol = teamCol[rival < 0 ? 0 : rival];
        int secs = (int)std::ceil(std::max(0.f, g.roundClock));
        const float sx = (L == 0) ? 1.f : -1.f;
        Quat faceP0 = Quat::axisAngle({0, 1, 0}, deg2rad(L == 0 ? -90.f : 90.f));   // quad +Z -> toward local player
        Vec3 c0{-0.5f * sx, 2.95f, 0};
        if (g.np == 4) { faceP0 = yawToward(g.facing(L)); c0 = Vec3{0, 2.95f, 0} - g.facing(L) * 0.5f; }   // (FOUR CORNERS: toward your corner)
        if (a.spectating || inLobby(a)) {   // above the roof, facing the gallery seat / lobby
            faceP0 = Quat{}; c0 = {0, 4.75f, 0.2f};
            if (cornersRoom(a)) { faceP0 = yawToward(normalize(Vec3{1, 0, -1})); c0 = {0, 4.75f, 0}; }
        } else if (a.net.localWaiting()) {   // facing the holding area
            faceP0 = Quat::axisAngle({0, 1, 0}, kPi); c0 = {0, 4.75f, -0.2f};
            if (cornersRoom(a)) { faceP0 = yawToward(normalize(Vec3{1, 0, 1})); c0 = {0, 4.75f, 0}; }
        }
        auto at = [&](float u, float v, float w = 0.f) { return c0 + faceP0.rotate({u, v, w}); };
        auto txt = [&](const char* t, float u, float v, float h, const float* c) { a.gfx.drawText(t, {faceP0, at(u, v, 0.004f)}, h, c); };
        const float board[4] = {0.004f, 0.008f, 0.018f, 0.72f};
        a.gfx.drawPanel(Mat4::fromPose({faceP0, at(0, 0, -0.01f)}, {2.4f, 0.66f, 1}), board);
        const float edgeL[4] = {myCol[0], myCol[1], myCol[2], 0.95f}, edgeR[4] = {opCol[0], opCol[1], opCol[2], 0.95f};
        a.gfx.drawPanel(Mat4::fromPose({faceP0, at(-1.19f, 0, -0.005f)}, {0.025f, 0.66f, 1}), edgeL);
        a.gfx.drawPanel(Mat4::fromPose({faceP0, at(1.19f, 0, -0.005f)}, {0.025f, 0.66f, 1}), edgeR);
        const float myC[4] = {myCol[0] * 0.6f + 0.4f, myCol[1] * 0.6f + 0.4f, myCol[2] * 0.6f + 0.4f, 1};
        const float opC[4] = {opCol[0] * 0.6f + 0.4f, opCol[1] * 0.6f + 0.4f, opCol[2] * 0.6f + 0.4f, 1};
        if (g.np == 2) {
            snprintf(buf, sizeof buf, "%d", g.pl[L].score);
            txt(buf, -0.80f, 0.04f, 0.36f, myC);
            snprintf(buf, sizeof buf, "%d", g.pl[1 - L].score);
            txt(buf, 0.80f, 0.04f, 0.36f, opC);
            drawFlag(a, a.sideNation[L], {faceP0, at(-0.50f, 0.04f, 0.003f)}, 0.21f);   // NATIONS: each side's flag
            drawFlag(a, a.sideNation[1 - L], {faceP0, at(0.50f, 0.04f, 0.003f)}, 0.21f);
        } else if ((g.tun.teamMode == TEAMS_FFA || g.octagon())) {   // FOUR CORNERS, everyone for themselves: four scores, yours marked
            for (int p = 0; p < 4; ++p) {
                const float x = (p < 2 ? -1.f : 1.f) * (p % 2 ? 0.62f : 0.95f);
                const float c[4] = {teamCol[p][0] * 0.6f + 0.4f, teamCol[p][1] * 0.6f + 0.4f, teamCol[p][2] * 0.6f + 0.4f, 1};
                if (oct && !((g.startMask >> p) & 1) && g.match != MatchState::WaitingForPlayers) continue;
                if (oct && !g.alive(p)) snprintf(buf, sizeof buf, "OUT");
                else snprintf(buf, sizeof buf, "%d", oct ? g.pl[p].health : g.pl[p].score);   // (FIREBALL BLITZ: health left)
                txt(buf, x, 0.08f, oct && !g.alive(p) ? 0.10f : 0.20f, c);
                if (p == L && !a.viewing) txt("YOU", x, -0.08f, 0.045f, c);
            }
        } else {   // teams: yours and theirs (3 v 1: THE ONE named)
            const int mt = g.team(L), ot = 1 - mt;
            snprintf(buf, sizeof buf, "%d", g.teamScore(mt));
            txt(buf, -0.80f, 0.04f, 0.36f, myC);
            snprintf(buf, sizeof buf, "%d", g.teamScore(ot));
            txt(buf, 0.80f, 0.04f, 0.36f, opC);
            if (g.tun.teamMode == TEAMS_3V1) {
                const bool one = g.tun.theOne == L;
                txt(one ? "YOU: THE ONE" : "YOUR THREE", -0.80f, -0.13f, 0.04f, myC);
                txt(one ? "THE THREE" : "THE ONE", 0.80f, -0.13f, 0.04f, opC);
            } else {
                txt("YOUR TEAM", -0.80f, -0.13f, 0.04f, myC);
                txt("THEIRS", 0.80f, -0.13f, 0.04f, opC);
            }
        }
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
                txt((g.tun.shields ? roomName(g.tun) : roomName(g.tun) + " - HOTDOG").c_str(), 0, -0.11f, 0.055f, grey);
        }
        if (g.np == 2) {
            snprintf(buf, sizeof buf, "STRIKES %d", g.pl[1 - L].strikesAgainst);
            txt(buf, -0.80f, -0.22f, 0.05f, grey);
            snprintf(buf, sizeof buf, "STRIKES %d", g.pl[L].strikesAgainst);
            txt(buf, 0.80f, -0.22f, 0.05f, grey);
        } else if (oct) {   // (FIREBALL BLITZ: how many are still in)
            snprintf(buf, sizeof buf, "HEALTH  -  %d STILL IN", g.aliveCount());
            txt(buf, -0.80f, -0.24f, 0.045f, grey);
        } else {   // (four players: the strikes you hold make your ball bigger)
            snprintf(buf, sizeof buf, "YOUR STRIKES %d", g.pl[L].strikesFor);
            txt(buf, -0.80f, -0.24f, 0.045f, grey);
        }
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
            if (g.np == 4 && (inLobby(a) || a.spectating || a.net.localWaiting())) {   // FOUR CORNERS: the colour, or the team
                static const char* kFour[4] = {"BLUE WINS", "ORANGE WINS", "GREEN WINS", "VIOLET WINS"};
                banner = g.matchWinner < 0 ? "A DRAW"
                       : (g.tun.teamMode == TEAMS_FFA || g.octagon()) ? kFour[g.matchWinner & 3]
                       : g.tun.teamMode == TEAMS_3V1 ? (g.matchWinner == 1 ? "THE ONE WINS" : "THE THREE WIN")
                       : (g.matchWinner == 0 ? "BLUE TEAM WINS" : "ORANGE TEAM WINS");
            } else if (inLobby(a) || a.spectating || a.net.localWaiting()) {   // BLUE WINS, or the country: UNITED STATES WINS
                snprintf(buf, sizeof buf, "%s WINS", sideName(a, g.matchWinner == 1 ? 1 : 0));
                banner = buf;
            } else {   // (by team: two players, the same; PRESS: READY's button, A as standard)
                snprintf(buf, sizeof buf, "%s  -  PRESS %s", g.matchWinner == g.team(L) ? "YOU WIN" : "YOU LOSE", readyName(a).c_str());
                banner = buf;
            }
            break;
        default: break;
        }
        if (banner) drawAnnouncement(a, banner, {faceP0, g.np == 4 ? Vec3{0, 2.2f, 0} - g.facing(L) * 1.5f : Vec3{-1.5f * sx, 2.2f, 0}}, 0.22f, white);
        if (!banner && g.np == 4 && a.leadFlash > 0 && g.leader() >= 0) {   // FOUR CORNERS: who leads now, in their colour
            static const char* kLeads[4] = {"BLUE LEADS", "ORANGE LEADS", "GREEN LEADS", "VIOLET LEADS"};
            const int lt = g.leader();
            const bool ours = !a.viewing && !a.spectating && !a.net.localWaiting() && lt == g.team(L);
            const char* t = ours ? ((g.tun.teamMode == TEAMS_FFA || g.octagon()) ? "YOU LEAD" : "YOUR TEAM LEADS")
                          : (g.tun.teamMode == TEAMS_FFA || g.octagon()) ? kLeads[lt & 3]
                          : g.tun.teamMode == TEAMS_3V1 ? (lt == 1 ? "THE ONE LEADS" : "THE THREE LEAD")
                          : (lt == 0 ? "BLUE TEAM LEADS" : "ORANGE TEAM LEADS");
            const float* lc = teamRGB(a, lt);
            const float col[4] = {lc[0] * 0.6f + 0.4f, lc[1] * 0.6f + 0.4f, lc[2] * 0.6f + 0.4f, std::min(1.f, a.leadFlash)};
            drawAnnouncement(a, t, {faceP0, Vec3{0, 2.2f, 0} - g.facing(L) * 1.5f}, 0.18f, col);
        }
        if (!banner && oct && a.octMsgT > 0) {   // FIREBALL BLITZ: who is out, or the room closing in (with its countdown)
            const float* mc = a.octMsgP >= 0 ? teamCol[a.octMsgP & 3] : nullptr;
            const float col[4] = {mc ? mc[0] * 0.6f + 0.4f : 1.f, mc ? mc[1] * 0.6f + 0.4f : 0.62f, mc ? mc[2] * 0.6f + 0.4f : 0.2f, std::min(1.f, a.octMsgT)};
            std::string m = a.octMsg;
            if (a.octMsgP < 0 && g.ringCloseAt >= 0) m += "   " + std::to_string((int)std::ceil(std::max(0.f, g.ringCloseAt - g.now)));
            drawAnnouncement(a, m, {faceP0, Vec3{0, 2.2f, 0} - g.facing(L) * 1.5f}, 0.19f, col);
        }
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
    // (TRAINING's board replaces the scoreboard above, whose block also draws the menu: the menu, then, from here;
    // it was never drawn during a lesson, its card, pause and result pages all invisible in the headset)
    if (pass == 1 && !minimal && a.trainer.active() && !a.rttPass) {
        if (a.ptFadeNow > 0.f) a.gfx.fadeToRoom(a.ptFadeNow);
        drawMenu(a);
    }
    if (!a.rttPass) drawBuilder(a, pass);
    if (!a.rttPass) drawTuning(a, pass);
    if (!a.rttPass) drawTitleShowcase(a, pass);
    if (!a.rttPass) drawTrainVideo(a, pass);
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
    // (TRAINING: a recording a lesson, from its GO to its end)
    if (a.trainer.active() && a.trainer.phase != training::Phase::Play && a.trainer.phase != training::Phase::Countdown) over = true;
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
            m.lesson = a.trainer.active() ? a.trainer.L().id : 0;
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

// TRAINING: what each moment of a lesson says and sounds like; a finished lesson's result kept and sent
static void trainPop(App& a, const char* text, const Vec3& at, float r, float g, float b, float size) {
    if (a.trainPops.size() >= 8) a.trainPops.erase(a.trainPops.begin());
    a.trainPops.push_back({text, at, 0.f, {r, g, b}, size});
}
static void trainingFeedback(App& a, float dt, training::Phase before) {
    using namespace training;
    for (size_t i = 0; i < a.trainPops.size();) {
        a.trainPops[i].t += dt;
        if (a.trainPops[i].t > 2.f) a.trainPops.erase(a.trainPops.begin() + i); else ++i;
    }
    const Vec3 ear = a.game.pl[0].in.head.p;
    auto pan = [&](const Vec3& p) { return clampf((p.z - ear.z) * 0.4f, -0.8f, 0.8f); };
    auto sound = [&](const char* key, const char* fallback, const Vec3& p, float gain) {
        if (!a.audio.cue(key, gain, pan(p)) && fallback) a.audio.cue(fallback, gain, pan(p));
    };
    for (const CueEvent& c : a.trainer.cues) {
        const bool onWall = c.pos.x > 3.f;   // (on the far wall: big letters)
        const float big = onWall ? 0.32f : 0.12f;
        switch (c.cue) {
        case Cue::TargetDown:
            sound("TargetHit", "Strike", c.pos, 1.f);
            trainPop(a, "HIT!", c.pos + Vec3{-0.3f, 0.6f, 0}, 1.f, 0.85f, 0.3f, big);
            haptic(a, a.pointerHand, 0.6f, 0.08f);
            break;
        case Cue::Ring: sound("Recharge", nullptr, c.pos, 0.7f); trainPop(a, "THROUGH!", c.pos + Vec3{0, 0.55f, 0}, 0.4f, 1.f, 0.6f, 0.2f); break;
        case Cue::TooSlow: sound("ThrowFail", nullptr, c.pos, 0.8f); trainPop(a, "TOO SLOW", c.pos + Vec3{-0.3f, 0.6f, 0}, 1.f, 0.45f, 0.3f, big); break;
        case Cue::NotBanked: sound("ThrowFail", nullptr, c.pos, 0.8f); trainPop(a, "OFF A WALL FIRST", c.pos + Vec3{-0.3f, 0.6f, 0}, 1.f, 0.45f, 0.3f, big); break;
        case Cue::NoRing: sound("ThrowFail", nullptr, c.pos, 0.8f); trainPop(a, "THROUGH THE RING FIRST", c.pos + Vec3{-0.3f, 0.6f, 0}, 1.f, 0.45f, 0.3f, big); break;
        case Cue::Strike: trainPop(a, "STRIKE!", c.pos + Vec3{0, 0.9f, 0}, 1.f, 0.85f, 0.3f, 0.3f); break;
        case Cue::BodyHit: trainPop(a, "PAST THE BODY, INTO THE RING", c.pos + Vec3{0, 0.9f, 0}, 1.f, 0.45f, 0.3f, 0.2f); break;
        case Cue::Blocked: trainPop(a, "BLOCKED!", c.pos + Vec3{0.6f, 0.35f, 0}, 0.4f, 0.9f, 1.f, 0.07f); break;
        case Cue::Punched: trainPop(a, "PUNCHED!", c.pos + Vec3{0.6f, 0.35f, 0}, 0.4f, 0.9f, 1.f, 0.07f); break;
        case Cue::Dodged: sound("Recharge", nullptr, c.pos, 0.6f); trainPop(a, "DODGED!", ear + Vec3{1.2f, 0.25f, 0}, 0.4f, 1.f, 0.6f, 0.08f); break;
        case Cue::Hit: trainPop(a, "HIT - TRY AGAIN", ear + Vec3{1.2f, 0.25f, 0}, 1.f, 0.45f, 0.3f, 0.07f); break;
        case Cue::Caught: trainPop(a, "CAUGHT!", c.pos + Vec3{0.6f, 0.3f, 0}, 0.4f, 1.f, 0.6f, 0.07f); break;
        case Cue::Dropped: sound("ThrowFail", nullptr, c.pos, 0.6f); trainPop(a, "MISSED", ear + Vec3{1.2f, 0.25f, 0}, 1.f, 0.45f, 0.3f, 0.07f); break;
        case Cue::Recharged: break;
        case Cue::Go: sound("RoundStart", nullptr, ear, 1.f); break;
        case Cue::Passed: sound("MatchEndWin", nullptr, ear, 1.f); break;
        case Cue::Failed: sound("MatchEndLose", nullptr, ear, 1.f); break;
        }
    }
    if (before != Phase::Done && a.trainer.phase == Phase::Done) {   // the lesson over: the result kept, sent, shown
        const Result& r = a.trainer.result;
        const Lesson& L = lessons()[r.lesson];
        a.trainNewBest = a.trainBests.add(r);
        LOGI("training: %s %s, %d / %d, %.1f s%s", L.name, r.passed ? "passed" : "not passed", r.successes, r.attempts, r.time,
             a.trainNewBest ? " (a best)" : "");
        if (a.trainNewBest) {
            saveTraining(a);
            const Best& b = a.trainBests.b[L.id];
            a.net.submitTraining(L.id, L.timed() ? b.time : 0.f, b.successes, b.attempts);
        }
        openMenu(a, a.localIn);
    }
}

#ifdef _WIN32
// DISCORD STATUS: "Project Arena" (playing) or "Project Arena - Spectator" (watching), a line saying what, a second
// with the rules, how long, and the room's size
static void discordUpdate(App& a) {
    if (!a.discordOn) { discord::clear(); return; }
    discord::Activity d;
    const Game& g = a.game;
    const Net::Mode m = a.net.mode();
    d.spectator = a.desktop || m == Net::Mode::Spectate || a.spectating;
    if (d.spectator) {
        const SpecMatch* w = nullptr;
        for (const SpecMatch& sm : a.net.matches()) if (sm.ip == a.watchIp) w = &sm;
        d.details = w ? std::string("Watching ") + (w->name[0] ? w->name : "a live match") : "In the spectator hub";
        d.state = w ? rulesLine(w->game.tun) : "Choosing a match";
    } else if (a.trainer.active()) {
        d.details = "Training";
        d.state = "Lessons and leaderboards";
    } else if (inLobby(a)) {
        d.details = "In the lobby";
    } else if (m == Net::Mode::Host || m == Net::Mode::Client) {
        d.details = m == Net::Mode::Host ? "Hosting an arena" : "In an online arena";
        d.state = rulesLine(g.tun);
        const RoomInfo& r = a.net.room();
        int n = (int)r.members.size();
        for (int p = 0; p < kMaxPlayers; ++p) n += r.slot[p] != 0;
        d.partySize = std::max(1, n);
        d.partyMax = (g.np == 4 ? 4 : 2) + Net::kMaxWaiting;
    } else {
        d.details = std::string("Practice vs ") + (g.tun.botAdapt ? "MATCH MY LEVEL" : botSkill(g.tun.botLevel).name);
        d.state = rulesLine(g.tun);
    }
    const std::string key = std::string(d.spectator ? "s" : "p") + d.details;
    if (key != a.discordKey) { a.discordKey = key; a.discordSince = (int64_t)time(nullptr); }   // (the time: since this began)
    d.start = a.discordSince;
    discord::set(d);
}
#endif

// ---- REACH LOG (developer mode only): reach_log.h; the lines go to <files>/reach_log.csv
static void reachLog(App& a, float dt) {
    static ReachLog R;
    static FILE* f = nullptr;
    static bool tried = false;
    if (!a.devMode || a.spectating) { R.have = false; return; }
    if (!tried) {
        tried = true;
        if (const char* dir = dataDir(a)) {
            const std::string fn = std::string(dir) + "/reach_log.csv";
            bool fresh = true;
            if (FILE* e = std::fopen(fn.c_str(), "rb")) { std::fseek(e, 0, SEEK_END); fresh = std::ftell(e) == 0; std::fclose(e); }
            f = std::fopen(fn.c_str(), "a");
            if (f && fresh) std::fprintf(f, "%s\n", ReachLog::kHeader);
            if (f) { std::fprintf(f, "# session, %s\n", kAppVersion); std::fflush(f); }
            LOGI("reach log %s: %s", f ? "on" : "could not open", fn.c_str());
        }
    }
    if (!f) return;
    std::vector<std::string> lines;
    const Net::Mode m = a.net.mode();
    R.step(a.game, a.net.localIndex(), dt, m == Net::Mode::Client ? "guest" : m == Net::Mode::Host ? "host" : "offline", lines);
    for (const std::string& s : lines) std::fprintf(f, "%s\n", s.c_str());
    if (!lines.empty()) std::fflush(f);
}

static void stepFrame(App& a, PlayerInput* in, int L, double t, float dt) {
    // the EXPERIMENTAL rooms: by the rules in play (practice, rooms, spectating; 0.3.12); the lobby stays classic, two
    // bots. Our own rules (practice, hosting) take their players from the room chosen (FOUR CORNERS: four); a guest's
    // and a spectator's come with the host's snapshots
    {
        const Net::Mode nm = a.net.mode();
        if (inLobby(a)) a.game.tun.players = 2;
        else if (nm == Net::Mode::Offline || nm == Net::Mode::Searching || nm == Net::Mode::Host)
            a.game.tun.players = a.game.tun.experimental && fourPlayerRoom(a.game.tun.expRoom) ? 4 : 2;
    }
    if (a.trainer.active() && (a.net.mode() != Net::Mode::Offline || inLobby(a) || a.spectating)) stopTraining(a);
    if (a.menuOpen && a.trainBoard >= -1 && nowSeconds() - a.trainBoardAsked > 5.0) askBoard(a);   // (a board open: kept fresh)
    a.game.useArena(arenaFor(a), a.classicCol);
    if (int done = a.bugDone.exchange(0)) {
        std::lock_guard<std::mutex> lk(a.bugMx);
        if (!a.bugStatus.empty()) a.net.noticeLocal(a.bugStatus.c_str());
        (void)done;
    }

    a.localIn = in[L];
    a.viewHead = in[L].head;
    a.bumpFlash = std::max(0.f, a.bumpFlash - dt);
    a.leadFlash = std::max(0.f, a.leadFlash - dt);
    a.foulFlash = std::max(0.f, a.foulFlash - dt);
    a.octMsgT = std::max(0.f, a.octMsgT - dt);
    a.fireDanger = 0;
    {   // FIREBALL BLITZ: knocked out, a blink to the dugout; the next match, a blink back to your place
        const Game& g = a.game;
        const bool out = g.octagon() && !a.spectating && !a.viewing && !a.net.localWaiting() && !g.alive(L) &&
                         ((g.startMask >> L) & 1) && g.match != MatchState::WaitingForPlayers;
        if (out != a.wasOut) {
            a.wasOut = out;
            Vec3 pos, fwd;
            viewTarget(a, L, pos, fwd);
            startBlink(a, {yawToward(fwd), pos + Vec3{0, a.viewHead.p.y, 0}});
        }
    }
    if (a.game.octagon() && !a.spectating && !a.viewing && a.game.alive(L) && a.game.match == MatchState::InProgress)
        for (int o = 0; o < a.game.np; ++o)
            for (int k = 0; k < Game::kMaxFire; ++k) {
                const Ball& f = a.game.fire[o][k];
                if (f.state == BallState::Flying && f.instigator != L && a.game.fireHitTime(f, a.game.pl[L].in.head.p, 1.2f) >= 0)
                    a.fireDanger |= (uint16_t)(1u << (o * Game::kMaxFire + k));
            }
#ifdef _WIN32
    discordUpdate(a);
#endif
    for (App::Puff& u : a.puffs) u.t += dt;
    a.puffs.erase(std::remove_if(a.puffs.begin(), a.puffs.end(), [](const App::Puff& u) { return u.t > 0.6f; }), a.puffs.end());
    if (a.game.octagon()) {   // FIREBALL BLITZ: the room closed in: a blink to your new place, where you stood in yours
        const Game& g = a.game;
        if (g.ringMask != a.ringSeen) {
            const Vec3 o = a.ringStartSeen[L], n = g.tun.start[L];
            if (a.ringSeen && !a.spectating && !a.viewing && !a.net.localWaiting() && ((g.ringMask >> L) & 1) && length(n - o) > 0.05f) {
                auto yaw = [](const Vec3& v) { return std::atan2(v.x, v.z); };
                const Quat turn = Quat::axisAngle({0, 1, 0}, yaw(Vec3{-n.x, 0, -n.z}) - yaw(Vec3{-o.x, 0, -o.z}));
                const Pose h = a.viewHead;
                startBlink(a, {turn * h.q, n + turn.rotate(h.p - o)});
            }
            a.ringSeen = g.ringMask;
        }
        for (int p = 0; p < kMaxPlayers; ++p) a.ringStartSeen[p] = g.tun.start[p];
    } else {
        a.ringSeen = 0;
    }
    {   // BLOCKS ONLINE in the session log: each late block counted, each wait that ended in a hit (the host's game)
        static int seenBlocks = 0, seenHits = 0;
        const Game& g = a.game;
        if (g.lateBlocks > seenBlocks || g.lateWaitHits > seenHits) {
            static const char* kKind[3] = {"a late shield block counted", "a late knuckle-guard punch counted", "no block came: the hit"};
            const int k = g.lateLastKind >= 0 && g.lateLastKind <= 2 ? g.lateLastKind : 2;
            LOGI("blocks online: %s (player %d, round trip %.0f ms; %d late blocks, %d waits ended in a hit so far)", kKind[k],
                 g.lateLastPlayer, g.lateLastRtt * 1000.f, g.lateBlocks, g.lateWaitHits);
        }
        seenBlocks = g.lateBlocks;
        seenHits = g.lateWaitHits;
    }
    updateCrowd(a, dt);
    {   // a guest plays by the host's rules; ours come back when we leave (and are never saved over). Hosting a room
        // taken over from its host (HOST HANDOFF) is still the room's rules, not ours
        const bool guest = a.net.mode() == Net::Mode::Client || a.net.mode() == Net::Mode::Spectate || a.net.inherited();
        if (!guest && a.guestRules) {
            Tunables& t = a.game.tun;
            t.advanced = a.ownRules.advanced; t.shields = a.ownRules.shields; t.knuckleMode = a.ownRules.knuckleMode;
            t.basicMatchTime = a.ownRules.basicMatchTime; t.advancedRoundTime = a.ownRules.advancedRoundTime;
            // (and the rest a host's snapshots set: its EXPERIMENTAL room, FOUR CORNERS' teams; they stayed until 0.3.12)
            t.experimental = a.ownRules.experimental; t.expRoom = a.ownRules.expRoom; t.players = a.ownRules.players;
            t.teamMode = a.ownRules.teamMode; t.theOne = a.ownRules.theOne; t.oneTurns = a.ownRules.oneTurns;
            t.health = a.ownRules.health; t.fireBalls = a.ownRules.fireBalls; t.octPlayers = a.ownRules.octPlayers;
            a.guestRules = false;
            if (a.game.match == MatchState::WaitingForPlayers || a.net.mode() == Net::Mode::Offline) a.game.reset(a.game.pl[1].isBot);
            LOGI("rules -> %s (our own again)", rulesLine(a.game.tun).c_str());
        } else if (!guest) {
            // (practice 3 v 1: you are THE ONE; a value left from hosting, where it took turns, once stayed)
            if (a.net.mode() == Net::Mode::Offline && !a.trainer.active()) a.game.tun.theOne = 0;
            a.ownRules = a.trainer.active() ? a.trainer.savedRules() : a.game.tun;   // (TRAINING's own rules are never ours)
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
                a.menuOpen = false; a.titleOpen = a.settingsOpen = a.voicePage = a.ptPage = a.gfxPage = a.ctlPage = a.roomBallPage = a.btnPage = a.helpOpen = a.xpOpen = a.rulesOpen = a.movePlayersOpen = false;
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
    for (int p = 2; p < a.game.np; ++p)   // (FOUR CORNERS' other bots: looks of their own)
        if (a.game.pl[p].isBot) a.game.pl[p].in.avatar = avatar::preset((3 * p + 1) % avatar::presetCount());
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
        if (a.watchIp && !w) {   // the match is gone: handed over to another host (follow it), or ended
            uint32_t next = 0;
            bool listed = false;
            for (const LanGame& lg : a.net.games()) { if (lg.continues == a.watchIp) next = lg.ip; listed |= lg.ip == a.watchIp; }
            // (a host gone quiet with others in its room: it may turn up under the next host within ~10 s; wait that long
            // for it. Nobody else there: nobody to take it over)
            if (!listed && a.watchGone == 0) a.watchGone = nowSeconds();
            const double wait = a.watchPeople >= 2 ? 12.0 : 0.0;
            if (next) { a.watchIp = next; a.watchGone = 0; LOGI("spectate: the room has a new host, still watching it"); }
            else if (!listed && nowSeconds() - a.watchGone >= wait) { a.watchIp = 0; a.watchGone = 0; LOGI("spectate: match ended, back to the hub"); }
        } else {
            a.watchGone = 0;
            if (w) {   // (how many people its room has: a host and at least one more can hand it over)
                a.watchPeople = (int)w->room.members.size();
                for (int p = 0; p < kMaxPlayers; ++p) a.watchPeople += w->room.slot[p] != 0;
            }
        }
        if (w) {   // watched match: its state and events drive the normal renderer, audio and effects
            const std::shared_ptr<const ArenaData> keep = a.game.arena;   // (our arena, not the copy's)
            const int keepKind = a.game.arenaKind;
            a.game = w->game;
            a.game.arena = keep;
            a.game.arenaKind = keepKind;
            a.game.useArena(arenaFor(a), a.classicCol);   // (the watched match's room)
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
        // FAIR MATCHES: hosting, an option an older player's game lacks plays at regulation this match (offline and
        // as a guest: everything; a guest gets the host's rules as they play)
        a.game.tun.matchVersion = a.net.matchVersion();
        fairMatchNotice(a);
        if (a.trainer.active()) {   // TRAINING: the lesson steps the game (and the coach's), paused while its menu is up mid-lesson
            using training::Phase;
            in[0] = mine;
            const bool mid = a.trainer.phase == Phase::Play || a.trainer.phase == Phase::Countdown;
            const Phase before = a.trainer.phase;
            if (a.trainMenuAt > 0 && nowSeconds() > a.trainMenuAt && a.recentered) {   // the card, now you are on the court
                a.trainMenuAt = 0;
                openMenu(a, a.localIn);
            }
            filmStep(a, dt);
            const bool A = mine.buttonA && !a.prevTrainA;
            a.prevTrainA = mine.buttonA;
            if (A && a.trainer.phase == Phase::Card) { a.trainer.start(a.game); a.menuOpen = false; a.trainMenuAt = 0; LOGI("training: started (A)"); }
            else if (A && a.trainer.phase == Phase::Done) {
                const int nx = training::nextInTrack(a.trainer.lesson);
                const bool next = a.trainer.result.passed && nx >= 0;
                LOGI("training: A -> %s", next ? "next lesson" : "try again");
                beginLesson(a, next ? nx : a.trainer.lesson, !next);
            } else if (a.trainer.phase == Phase::Card && !a.menuOpen && a.trainMenuAt == 0) {
                a.trainer.start(a.game); LOGI("training: started (card closed)");
            }
            // (SOUND STUDIO: its moment plays on with its page open beside you, to change a sound and hear it at once;
            // owner's test 2026-11-06: having to close the menu to play and open it to choose wasn't something to guess)
            if (!(a.builderOpen || a.tuneOpen || (a.menuOpen && mid && !a.studioOn))) a.trainer.step(a.game, dt, in);
            trainingFeedback(a, dt, before);
        } else if (a.net.exchange(mine, in, a.game, dt)) {
            // practice is paused while you customize your avatar (the builder only opens offline)
            const bool paused = (a.builderOpen || a.tuneOpen || (a.menuOpen && a.titleOpen)) && a.net.mode() != Net::Mode::Host && !inLobby(a);
            a.game.octCap = a.net.mode() == Net::Mode::Offline ? a.game.tun.octPlayers : 4;   // (FIREBALL BLITZ: practice's PLAYERS)
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
    studioUpdate(a);
    recordStep(a, dt);
    handleEvents(a);
    reachCue(a);   // (THE REACH ROUND THE FIST: your ball brightens in reach)
    {   // the last 10 seconds of a timed match or round: a very soft tick each second (owner, 2026-10-27)
        const Game& g = a.game;
        const int left = g.match == MatchState::InProgress && g.tun.timed() && !g.overtime && g.roundClock > 0.f && !inLobby(a)
                             ? (int)std::ceil(g.roundClock) : 0;
        if (left >= 1 && left <= 10 && left != a.clockTick) a.audio.play("oa_ClockTick", 0.6f);
        a.clockTick = left;
    }
    smoothPlayers(a, dt);
    a.fadeT = std::max(0.f, a.fadeT - dt);
    if (a.darkOut > 0 && (a.darkOut -= dt) <= 0) { a.darkOut = 0; a.fadePending = true; }   // dark: move now
    a.darkIn = std::max(0.f, a.darkIn - dt);
    updateFx(a, dt);
    for (float& f : a.hitFlash) f = std::max(0.f, f - dt);
    for (int p = 0; p < a.game.np; ++p) {
        App::Trail& tr = a.trail[p];
        const Ball& b = a.game.pl[p].ball;
        tr.strikeFlash = std::max(0.f, tr.strikeFlash - dt);
        tr.burst = std::max(0.f, tr.burst - dt);
        const bool rising = b.state == BallState::Spawning && b.pos.y > 0.f;
        if (b.state != BallState::Flying && b.state != BallState::Returning && !rising) { tr.n = 0; tr.stale = 0; continue; }
        float sl = rising ? 10.f : length(b.spin);
        tr.spinAngle += sl * dt;
        Vec3 axis = rising ? Vec3{0, 1, 0} : sl > 1e-3f ? b.spin / sl : Vec3{0, 1, 0};
        Vec3 perp = normalize(cross(axis, std::fabs(axis.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}));
        float hr = a.game.ballRadius(p) * 1.3f;
        // (someone waiting or watching gets the host's picture 30 times a second, the headset draws 72-90: the ball
        // stood still for frames while its spiral turned on the spot, then jumped, drawn as arrows (owner, 2026-11-12,
        // in the queue). A point only when it has moved, the frames between filled along the way, as smooth and as
        // long as a player's)
        if (tr.n > 0 && length(b.pos - tr.core[tr.n - 1]) < 1e-5f) { ++tr.stale; continue; }
        const int k = tr.n > 0 ? std::min(tr.stale + 1, 8) : 1;
        const Vec3 from = tr.n > 0 ? tr.core[tr.n - 1] : b.pos;
        tr.stale = 0;
        for (int j = 1; j <= k; ++j) {
            const Vec3 at = from + (b.pos - from) * ((float)j / (float)k);
            const Vec3 offA = Quat::axisAngle(axis, tr.spinAngle - (float)(k - j) * sl * dt).rotate(perp) * hr;
            if (tr.n == 48) {
                for (int i = 1; i < 48; ++i) { tr.core[i - 1] = tr.core[i]; tr.helixA[i - 1] = tr.helixA[i]; tr.helixB[i - 1] = tr.helixB[i]; }
                tr.n = 47;
            }
            tr.core[tr.n] = at;
            tr.helixA[tr.n] = at + offA;
            tr.helixB[tr.n] = at - offA;
            tr.n++;
        }
    }
    for (const GameEvent& e : a.game.events)
        if (e.type == EventType::Strike && e.player >= 0) a.trail[e.player].strikeFlash = 1.0f;
    reachLog(a, dt);   // (developer mode: the REACH LOG, reach_log.csv)
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
    PlayerInput in[kMaxPlayers];
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
    if (fs.shouldRender) filmRender(a);
    if (fs.shouldRender) headSelfieRender(a);
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
            if (a.roomFeel != 0 && nv >= 2) {   // FIELD OF VIEW: eye separation x 1/(1 + 10% a step); the headset gets the real poses
                const Vec3 mid = (a.trackToWorld * fromXr(a.views[0].pose)).p * 0.5f + (a.trackToWorld * fromXr(a.views[1].pose)).p * 0.5f;
                eye.p = mid + (eye.p - mid) * (1.f / (1.f + 0.10f * a.roomFeel));
            }
            a.ptFadeNow = ptOn ? a.ptReveal : 0.f;   // ROOM AT EDGES / PEEK (drawWorld fades the world under the menus)
            a.curEye = (int)i;
            const XrFovf& f = a.views[i].fov;
            Mat4 proj = Mat4::projection(f.angleLeft, f.angleRight, f.angleUp, f.angleDown, 0.05f, 2000.f);
            a.gfx.beginEye(Mat4::viewFromPose(eye), proj, eye.p);
            drawWorld(a, 0);
            drawWorld(a, 1);
            if (a.fadeT > 0 && a.capTex[i]) a.gfx.drawScreenTexture(a.capTex[i], std::min(1.f, a.fadeT / 0.5f));
            {   // a blink (startBlink): plain dark over the whole view, nothing in it to follow the head
                const float dark = a.darkOut > 0 ? 1.f - a.darkOut / kBlinkOut : (a.fadeDark && a.fadePending) ? 1.f : a.darkIn / kBlinkIn;
                if (dark > 0) {
                    if (!a.blackTex) {
                        const uint8_t px[4] = {0, 0, 0, 255};
                        glGenTextures(1, &a.blackTex);
                        glBindTexture(GL_TEXTURE_2D, a.blackTex);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    }
                    a.gfx.drawScreenTexture(a.blackTex, std::min(1.f, dark));
                }
            }
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
                    if (a.xrRequestDisplayRefreshRateFB) {
                        // 90 Hz; Quest 2 72 Hz: its GPU is about half a Quest 3's, and FIREBALL BLITZ keeps a Quest 3's
                        // ~65-80% busy at 90 (measured 2026-11-07), too close at 90 on a Quest 2 (72: a quarter more time a frame)
                        float hz = 90.0f;
#ifdef __ANDROID__
                        char model[92] = "";
                        __system_property_get("ro.product.model", model);
                        if (std::strstr(model, "Quest 2")) hz = 72.0f;
#endif
                        LOGI("display: %.0f Hz asked", hz);
                        a.xrRequestDisplayRefreshRateFB(a.session, hz);
                    }
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
            // (applied when it takes effect: applySpaceChanges, from readInput with the frame's display time)
            App::PendingSpaceChange c{rc->changeTime, rc->poseValid != XR_FALSE, rc->poseInPreviousSpace};
            auto at = a.spaceChanges.begin();
            while (at != a.spaceChanges.end() && at->changeTime <= c.changeTime) ++at;
            a.spaceChanges.insert(at, c);
            break;
        }
        case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED: {   // the controllers in your hands (BUTTONS: which they have)
            XrInteractionProfileState st{XR_TYPE_INTERACTION_PROFILE_STATE};
            char name[XR_MAX_PATH_LENGTH] = "";
            uint32_t n = 0;
            if (a.session != XR_NULL_HANDLE && XR_SUCCEEDED(xrGetCurrentInteractionProfile(a.session, a.handPath[0], &st)) &&
                st.interactionProfile != XR_NULL_PATH &&
                XR_SUCCEEDED(xrPathToString(a.instance, st.interactionProfile, sizeof name, &n, name))) {
                const std::string p = name;
                using namespace controls;
                a.ctlKind = p.find("htc/vive") != std::string::npos ? K_VIVE : p.find("microsoft/motion") != std::string::npos ? K_WMR
                          : p.find("valve/index") != std::string::npos ? K_INDEX : p.find("oculus/touch") != std::string::npos ? K_TOUCH
                          : K_UNKNOWN;
                LOGI("controllers: %s (push to talk: %s)", name, jobButtons(a, J_TALK, false).c_str());
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

static void saveTraining(App& a) {   // TRAINING: your bests (training.cfg)
    const char* ext = dataDir(a);
    if (!ext) return;
    const std::string s = a.trainBests.save();
    if (FILE* fp = fopen((std::string(ext) + "/training.cfg").c_str(), "w")) { fwrite(s.data(), 1, s.size(), fp); fclose(fp); }
}
static void loadTraining(App& a) {   // ... and each one sent again (the server keeps the better; queued until online)
    const char* ext = dataDir(a);
    if (!ext) return;
    std::string s;
    if (FILE* fp = fopen((std::string(ext) + "/training.cfg").c_str(), "r")) {
        char buf[512];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, fp)) > 0) s.append(buf, n);
        fclose(fp);
    }
    a.trainBests.load(s);
    for (const training::Lesson& L : training::lessons()) {
        const training::Best& b = a.trainBests.b[L.id];
        if (b.passed && b.attempts > 0) a.net.submitTraining(L.id, L.timed() ? b.time : 0.f, b.successes, b.attempts);
    }
}

static void saveWarps(App& a) {
    const char* ext = dataDir(a);
    if (!ext) return;
    if (FILE* fp = fopen((std::string(ext) + "/warps.cfg").c_str(), "w")) {
        for (const App::WarpCam& w : a.warps) {
            Vec3 f = w.pose.q.rotate({0, 0, -1});
            fprintf(fp, "%.3f %.3f %.3f %.4f %d %d %d %.4f %.1f\n", w.pose.p.x, w.pose.p.y, w.pose.p.z, std::atan2(-f.x, -f.z), w.hold, w.trans,
                    w.trig, w.pitch, w.fov);
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
        float x, y, z, yaw, pitch = 0, fov = 0;
        int hold = 1, trans = 1, trig = 0;
        if (sscanf(line, "%f %f %f %f %d %d %d %f %f", &x, &y, &z, &yaw, &hold, &trans, &trig, &pitch, &fov) < 4) continue;
        App::WarpCam w;
        w.pose = {Quat::axisAngle({0, 1, 0}, yaw), {x, y, z}};   // (level: the headset's head; the PC adds the pitch)
        w.pitch = std::isfinite(pitch) ? clampf(pitch, -1.5f, 1.5f) : 0.f;
        w.fov = std::isfinite(fov) && fov >= 25.f && fov <= 100.f ? fov : 0.f;
        w.hold = (uint8_t)(hold & 3); w.trans = (uint8_t)(trans & 1); w.trig = (uint8_t)std::min(2, std::max(0, trig));
        a.warps.push_back(w);
    }
    fclose(fp);
}

static void saveSettings(App& a) {
    const char* ext = dataDir(a);
    if (!ext) return;
    std::string f = std::string(ext) + "/settings.cfg";
    // the player's own rules: a host's while we are its guest, or TRAINING's (and SOUND STUDIO's) lesson rules while one
    // runs, were saved as theirs (owner, 2026-11-12: FIST BUMP FIRST went off after the studio; the lesson turns it off)
    const Tunables& R = a.guestRules ? a.ownRules : a.trainer.active() ? a.trainer.savedRules() : a.game.tun;
    if (FILE* fp = fopen(f.c_str(), "w")) {
        fprintf(fp, "cfgver=9\n");
        fprintf(fp, "advanced=%d\nshield=%d\npitch=%.1f\nfwd=%.3f\nup=%.3f\nvolume=%d\nhand=%d\nfx=%d\nhideoutside=%d\nvoice=%d\nmutespec=%d\nmuteplayers=%d\nmuteall=%d\nonline=%d\nbotfill=%d\npassthrough=%d\nptfloor=%.2f\nptglass=%.2f\nptroom=%.2f\nptmono=%d\nptedges=%d\nptreveal=%d\nptpeek=%d\nteam=%s\nserver=%s\nstatsnotice=%d\ncrowd=%d\nshields=%d\nknuckles=%d\nmatchtime=%.0f\nroundtime=%.0f\nturn=%d\nbotlevel=%d\nbotadapt=%d\nbotmatch=%.2f\nrecord=%d\nbotmeet=%d\nroomfeel=%d\nexperimental=%d\nexproom=%d\nteams=%d\noneturns=%d\nhealth=%d\nfireballs=%d\noctplayers=%d\ndiscord=%d\nprivatetalk=%d\nheadselfie=%d\nselfie=%d\nselfiecorner=%d\nselfiesize=%d\nselfieround=%d\nselfiecam=%d\n",
                R.advanced ? 1 : 0, a.game.tun.shieldPlacement, a.tunePitchDeg, a.tuneFwd, a.tuneUp, a.volume,
                a.pointerHand, a.fxLevel, a.hideOutside ? 1 : 0, a.voiceMode, a.muteSpectators ? 1 : 0, a.mutePlayers ? 1 : 0,
                a.muteAll ? 1 : 0, a.onlinePref ? 1 : 0, a.botFill ? 1 : 0, a.ptMode, a.ptFloor, a.ptGlass, a.ptRoom, a.ptMono ? 1 : 0, a.ptEdges ? 1 : 0, a.ptEdgeReveal ? 1 : 0, a.ptPeekOn ? 1 : 0, a.team.c_str(),
                (a.serverOverride.empty() ? a.serverAddr : a.serverSaved).c_str(),
                a.statsNoticeSeen ? 1 : 0, a.crowdMode, R.shields ? 1 : 0,
                R.knuckleMode, R.basicMatchTime,
                R.advancedRoundTime, a.turnMode, R.botLevel,
                R.botAdapt ? 1 : 0, R.botMatch, a.recordOn ? 1 : 0,
                R.botMeet ? 1 : 0, a.roomFeel, R.experimental ? 1 : 0, R.expRoom, R.teamMode,
                R.oneTurns ? 1 : 0, R.health,
                R.fireBalls, R.octPlayers, a.discordOn ? 1 : 0,
                a.net.privateTalk() ? 1 : 0, a.headSelfie ? 1 : 0, a.selfieOn ? 1 : 0, a.selfieCorner, a.selfieSize, a.selfieRound ? 1 : 0, a.selfieCam);
        fprintf(fp, "lookahead=%d\n", a.net.lookAhead() ? 1 : 0);
        fprintf(fp, "strikespeed=%d\n", R.strikeSpeed);
        fprintf(fp, "foulline=%d\n", R.foulShift);
        fprintf(fp, "maiden=%d\naimassist=%d\nstrikezone=%d\nballgrowth=%d\nroundswin=%d\nbotplay=%d\n", R.maiden ? 1 : 0, R.aimAssist,
                R.strikeZone, R.ballGrowth, R.advancedRoundsToWin, R.botPlay);   // (ROOMS AND THEIR OPTIONS, BOT STYLE)
        fprintf(fp, "shieldsize=%d\n", R.shieldSize);   // (SHIELD SIZE)
        fprintf(fp, "ballfeel=%d\n", R.ballFeel & BALL_FEEL_ALL);   // (BALL FEEL, the room's)
        fprintf(fp, "ctltilt=%d\nctltwist=%d\nctlturn=%d\n", a.ctlTilt, a.ctlTwist, a.ctlTurn);   // (CONTROLLER ANGLE)
        fprintf(fp, "buttons=%s\n", controls::save(a.btnMap).c_str());   // (BUTTONS)
        fprintf(fp, "dcglass=%.2f\n", a.dcGlass);   // (the PC spectator's ARENA GLASS)
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
        if (std::strncmp(line, "buttons=", 8) == 0) {   // BUTTONS (controls.h; one this game can't read: STANDARD)
            std::string v = line + 8;
            while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
            if (!controls::load(a.btnMap, v)) LOGW("settings: buttons=%s not read (STANDARD kept)", v.c_str());
            continue;
        }
        if (std::strncmp(line, "team=", 5) == 0) {
            a.team = line + 5;
            while (!a.team.empty() && (a.team.back() == '\n' || a.team.back() == '\r' || a.team.back() == ' ')) a.team.pop_back();
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
        else if (k == "roomfeel") a.roomFeel = std::max(-5, std::min(5, (int)v));
        else if (k == "experimental") a.game.tun.experimental = v != 0;
        else if (k == "exproom") { a.game.tun.expRoom = std::max(0, std::min(kRooms - 1, (int)v)); a.game.tun.players = fourPlayerRoom(a.game.tun.expRoom) && a.game.tun.experimental ? 4 : 2; }
        else if (k == "health") a.game.tun.health = std::max(1, std::min(10, (int)v));
        else if (k == "fireballs") a.game.tun.fireBalls = std::max(1, std::min(4, (int)v));
        else if (k == "octplayers") a.game.tun.octPlayers = std::max(2, std::min(4, (int)v));
        else if (k == "discord") a.discordOn = v != 0;
        else if (k == "dcglass") a.dcGlass = clampf(v, 0.1f, 1.f);
        else if (k == "selfie") a.selfieOn = v != 0;
        else if (k == "headselfie") a.headSelfie = v != 0;
        else if (k == "privatetalk") a.net.setPrivateTalk(v != 0);
        else if (k == "lookahead") a.net.setLookAhead(v != 0);
        else if (k == "selfiecorner") a.selfieCorner = std::max(0, std::min(3, (int)v));
        else if (k == "selfiesize") a.selfieSize = std::max(0, std::min(2, (int)v));
        else if (k == "selfieround") a.selfieRound = v != 0;
        else if (k == "selfiecam") a.selfieCam = std::max(0, std::min(15, (int)v));
        else if (k == "teams") a.game.tun.teamMode = std::max(0, std::min(2, (int)v));
        else if (k == "oneturns") a.game.tun.oneTurns = v != 0;
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
        else if (k == "strikespeed") a.game.tun.strikeSpeed = std::max(0, std::min(2, (int)v));
        else if (k == "foulline") a.game.tun.foulShift = std::max(-2, std::min(2, (int)v));
        else if (k == "maiden") a.game.tun.maiden = v != 0;
        else if (k == "aimassist") a.game.tun.aimAssist = std::max(-1, std::min(3, (int)v));
        else if (k == "strikezone") a.game.tun.strikeZone = std::max(0, std::min(2, (int)v));
        else if (k == "ballgrowth") a.game.tun.ballGrowth = std::max(0, std::min(3, (int)v));
        else if (k == "roundswin") a.game.tun.advancedRoundsToWin = std::max(1, std::min(5, (int)v));
        else if (k == "botplay") a.game.tun.botPlay = ((int)v) & 1;
        else if (k == "shieldsize") a.game.tun.shieldSize = std::max(0, std::min(4, (int)v));
        else if (k == "ctltilt") a.ctlTilt = std::max(-kCtlMax, std::min(kCtlMax, (int)v));
        else if (k == "ballfeel") a.game.tun.ballFeel = (int)v & BALL_FEEL_ALL;   // (BALL FEEL, the room's; the test's
                                                                              // own keys left behind: the owner's pick for all)
        else if (k == "ctltwist") a.ctlTwist = std::max(-kCtlMax, std::min(kCtlMax, (int)v));
        else if (k == "ctlturn") a.ctlTurn = std::max(-kCtlMax, std::min(kCtlMax, (int)v));
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
    a.net.setBotFill(a.botFill);   // (a saved BOT FILLS IN: OFF never reached the room before 0.3.13)
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
    if (a.cfgVersion < 9 && !a.net.lookAhead()) {   // BALL LOOK-AHEAD on by default (owner, 2026-11-17): once, for everyone
        a.net.setLookAhead(true);                    // (every older file says lookahead=0 unless it was turned on)
        LOGI("settings: ball look-ahead on (new default)");
        saveSettings(a);
    }
    if (a.cfgVersion < 5 && !a.onlinePref) {   // older builds started on home Wi-Fi: online is the default now, once
        a.onlinePref = true;
        LOGI("settings: online on (new default)");
        saveSettings(a);
    }
    a.net.setBotFill(a.botFill);
    if (!a.serverOverride.empty()) { a.serverSaved = a.serverAddr; a.serverAddr = a.serverOverride; a.onlinePref = true; }
    // (pictures and probes stay off the community server: --selftest renders once showed up there as players)
    if (a.onlinePref && !a.serverAddr.empty() && !a.noOnline) a.net.goOnline(a.serverAddr.c_str());
    if (!a.serverAddr.empty()) { std::vector<std::string> ob = bugOutbox(a); if (!ob.empty()) bugSendAll(a, ob); }   // reports saved earlier
    loadWarps(a);
    loadTraining(a);
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
    entitlement::start(env, app->activity->clazz);   // (store builds: Meta asks for it within 10 s of launch)

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
        if (entitlement::poll() == entitlement::State::Owned) {   // (Meta's answer read even with the headset off; readInput shows the page)
            if (!a.storeAsked) { a.storeAsked = true; entitlement::updateCheck(); }   // a store copy: a newer version in the store? (UPDATE NOW)
        }
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
    std::string buttons;   // --buttons DIGITS: BUTTONS as settings.cfg has them (controls::save)
    int ctlKind = 0;       // --ctlkind K: the controllers in the hands (controls::K_*: 2 Index, 3 Vive, 4 WMR)
    float filmSeconds = 8.f;   // --film S: the tutorial video that far in (--menu traincard)
    std::string scene;  // --scene action: two players mid-rally (store art)
    bool lobby = false; // --lobby: the lobby balcony and posters (other pictures show the plain court)
    int tunePart = 0;   // --scene tuning --tune N: the developer tuning tool on that part
    int tuneRow = 1;    // --tunerow R: its value picked
    int suit = -1;      // --suit N: the players' outfit in the pictures (avatar checks)
    int hair = -1;      // --hair N: their hair
    int face = -1;      // --face B F: blue's body type (0 / 1) and face (0..3)
    std::string look;   // --look "visor=1,mask=3,headgear=2": avatar.cfg keys for blue (accessory checks)
    std::string dcshot;       // --dcshot meet|bump|win|play: the desktop spectator's broadcast moment (its camera and overlay)
    float glass = -1;         // --glass 0.35: the PC spectator's ARENA GLASS in the picture
    int queue = -1;           // --queue SPOT: that waiting spot's kiosk and queue screen (sample queue)
    int foul = 0;             // --foul N: the FOUL LINE option (-2 .. 2) in the picture
    int selfie = -1;          // --selfie N: the SELFIE overlay with a test card (N: corner 0-3, + 4 square, + 8 small, + 16 large)
    bool headSelfie = false;  // --headselfie: a spectator in the headset with SELFIE on, seen through their eyes
    int layer = 0, keyCol = 3;  // --layer 0-3: its VIEW LAYERS; --keycol 0-2 (pictures only): an old colour key
    std::string nations[2];     // --nations USA FRA: the two players' countries (NATIONS; "-" = none)
    int crowdFans = -1, crowdHype[2] = {0, 0}, crowdCheer = -1;   // --crowd FANS HYPE0 HYPE1 [END]: The Crowd
    std::string recordDir;   // --record DIR SECONDS: a bot-vs-bot match recorded frame by frame (a trailer; DIR/f%05d.ppm
    float recordSeconds = 0; // and DIR/events.csv for the sound), with scripted cameras; --size sets the frame size
    int recordStep = 1;      // --recordstep N: only every Nth frame written (scouting a match for stills)
    std::vector<int> recordFrames;   // --recordframes 120,431: only these frames written (stills; --view fixes the camera)
    int recordBot = 1;       // --recordbot L: both bots' level (AMATEUR by default)
    bool recordMeet = false; // --recordmeet: the match starts with the two bots meeting for the fist bump
    bool noTrail = false;    // --notrail: stills without the balls' trails
    int aimPlayer = -1; Vec3 aimFrom, aimAt;   // --recordaim P dx dy dz ax ay az: a camera that follows player P's head
    std::string opts;   // --opts hotdog,free,adv,round2,time5,kg0,kg1: MATCH OPTIONS in the pictures
    float mockDark = -1.f;   // --mockdark K: pictures only, a darker arena (GitHub #9): floor, dais and structure x K
    bool contact = false;    // --contact: pictures only, contact shadows under the players and balls (a mock-up)
    bool depthOut = false;   // --depth: also OUT.depth (float32 depth, W x H rows bottom-up) for the AO mock-up
    int winStyle = 0;           // --winscreen split|winner|camera (or 0-2): its WIN SCREEN
    bool handsOut = false;   // --handsout: blue's hands held straight out in front, palms down (the gauntlets' wrists)
    // --handstouch N: blue's two hands held together (HANDS APART): 0 the tops (palms down and in), 1 the palms, 2 the
    // knuckles, 3 the wrists crossed; 4 at the chest, 5 at the belly, 6 the arms crossed (ELBOWS OUT OF THE BODY); 7
    // issue #24's far reach (THE REACH); 8 a long reach; 9 the arms out, palms turned up, 10 turned down (FOREARM TWIST);
    // --touchgap M: the hands this far from the middle instead; --noapart: drawn as tracked; --noelbow: the elbows
    // placed as before ELBOWS OUT OF THE BODY
    int handsTouch = -1;
    float touchGap = -1.f;
    bool noApart = false, noElbow = false, noTwist = false;   // --notwist: FOREARM TWIST off
    bool hitboxes = false;   // --hitboxes: the knuckle guards' and shields' hit boxes over them (green, red)
    float punchT = -1.f;     // --punch T: blue's ball as punched T s ago (THE PUNCH LANDS: its flash and squash)
    std::string ballFeel;    // --ballfeel ctsw: BALL FEEL (TEST): c CORE SOLID, t GLOW TRUE SIZE, s SPIN VISIBLE, w WEIGHT HEAVY
    float swish[3] = {0, 0, 0};   // --swish X Z TWIST: every skirt swung this much (m, m, rad; a picture has no motion)
    bool swishOn = false;
    float armScale = -1.f;   // --armscale X: the avatars' arms sized X (ARM SIZING; a picture has no time to measure them)
    bool swingTest = false;   // --swingtest: the head snaps round just before the picture (the hair mid-swing)
    float armsUp = -1;        // --armsup DEG: the tuning scene's hands raised this far (0 forward, 90 overhead)
    float swingDeg = 60.f, swingHold = 0.1f;   // --swingdeg D, --swinghold S: how far it turned, how long ago (action scene)
    bool passthrough = false;   // --passthrough MODE (1 ARENA, 2 WINDOW, 3 OUTLINE): drawn over a stand-in room (a checkerboard)
    int ptMode = 1;
    float ptRoom = 1.f;         // --ptroom: ROOM BRIGHTNESS in the picture
    int marks = -1;             // --marks STYLE (0 ring, 1 scorch): BALL MARKS on the floor and walls, at several ages
};

static Pose lookFrom(const Vec3& pos, const Vec3& target);
static void stepFrame(App& a, PlayerInput* in, int L, double t, float dt);

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
    for (int i = 0; i < 500 && !a.handShapesReady.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));   // (HANDS APART)
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
    if (v.glass >= 0) { a.dcGlass = v.glass; a.shotGlass = true; }   // (--glass: ARENA GLASS)
    a.noApart = v.noApart;   // (--noapart: the hands drawn as tracked, HANDS APART off)
    a.noTwist = v.noTwist;   // (--notwist: FOREARM TWIST off)
    a.shotHitboxes = v.hitboxes;   // (--hitboxes)
    if (!v.ballFeel.empty()) {   // (--ballfeel)
        const std::string& f = v.ballFeel;   // (letters c t s w; "-": none)
        a.game.tun.ballFeel = (f.find('c') != std::string::npos ? BALL_CORE : 0) | (f.find('t') != std::string::npos ? BALL_TRUE : 0) |
                              (f.find('s') != std::string::npos ? BALL_SPIN : 0) | (f.find('w') != std::string::npos ? BALL_WEIGHT : 0);
        a.optBallFeel = a.game.tun.ballFeel;
        if (a.game.tun.ballFeel & BALL_SPIN) for (int p = 0; p < kMaxPlayers; ++p) a.ballQ[p] = Quat::axisAngle(normalize(Vec3{0.3f, 0.2f, 1.f}), 0.9f);
    }
    a.shotSwishOn = v.swishOn;   // (--swish: TENNIS SKIRT / KILT swung)
    for (int k = 0; k < 3; ++k) a.shotSwish[k] = v.swish[k];
    if (v.noElbow) avatar::rigTuning().elbowAllow = 1e9f;   // (--noelbow: ELBOWS OUT OF THE BODY off)
    a.shotArmScale = v.armScale;   // (--armscale: ARM SIZING as given)
    a.game.tun.foulShift = std::max(-2, std::min(2, v.foul));         // (--foul: the FOUL LINE moved)
    if (v.queue >= 0) a.shotQueue = v.queue & 7;   // (--queue: the queue screen)
    const float ty = std::tan(deg2rad(v.fovY) * 0.5f), tx = ty * W / H;
    a.gfx.beginEye(Mat4::viewFromPose(eye), Mat4::projection(-std::atan(tx), std::atan(tx), std::atan(ty), -std::atan(ty), 0.05f, 2000.f), eye.p);
    if (v.scene == "four" || v.scene == "four2v2" || v.scene == "four3v1" || v.scene == "fourlead") {   // FOUR CORNERS: four bots a few seconds in
        Game& g = a.game;
        g.tun.experimental = true; g.tun.expRoom = 1; g.tun.players = 4; g.tun.meetTime = 0; g.tun.botLevel = 2;
        g.tun.teamMode = v.scene == "four2v2" ? TEAMS_2V2 : v.scene == "four3v1" ? TEAMS_3V1 : TEAMS_FFA;
        g.tun.theOne = 0;
        g.useArena(ARENA_FOURCORNERS, a.classicCol);
        g.rngSeed(7);
        g.resetWithBots(0xF);
        a.viewing = true;
        PlayerInput in[kMaxPlayers];
        for (int i = 0; i < 72 * 9; ++i) { PlayerInput tmp[kMaxPlayers]; g.step(1.f / 72, tmp); }
        g.pl[0].score = 3; g.pl[1].score = 1; g.pl[2].score = 4; g.pl[3].score = 2;
        if (v.scene == "fourlead") a.leadFlash = 2.0f;   // (green has just taken the lead)
        for (int p = 0; p < 4; ++p) {
            g.pl[p].in.avatar = avatar::preset((3 * p + 1) % avatar::presetCount());
            a.viewIn[p] = g.pl[p].in; a.viewInValid[p] = true;
        }
        (void)in;
    }
    if (v.scene == "blitz" || v.scene == "blitz3" || v.scene == "blitzrapid" || v.scene == "blitzwait") {   // FIREBALL BLITZ: four bots mid-match (blitz3: one
        Game& g = a.game;                                                          // out, the room closed in; rapid: RAPID FIRE)
        g.tun.experimental = true; g.tun.expRoom = ROOM_BLITZ; g.tun.players = 4; g.tun.meetTime = 0; g.tun.botLevel = 3;
        g.tun.health = 5; g.tun.fireBalls = v.scene == "blitzrapid" ? 4 : 1; g.tun.basicMatchTime = 0;
        g.useArena(ARENA_OCTAGON, a.classicCol);
        g.rngSeed(11);
        g.resetWithBots(0xF);
        a.viewing = true;
        const float secs = v.scene == "blitz3" ? 60.f : v.scene == "blitzwait" ? 0.f : 7.5f;   // (blitzwait: before it starts)
        for (int i = 0; i < 72 * secs; ++i) {
            PlayerInput tmp[kMaxPlayers];
            g.step(1.f / 72, tmp);
            if (v.scene == "blitz3" && g.aliveCount() == 3 && g.ringCloseAt < 0 && g.match == MatchState::InProgress) break;
            if (v.scene == "blitz3" && i == 72 * 5) g.pl[2].health = 1;   // (so someone goes out soon)
        }
        a.octWallR = g.ringApothem;
        if (v.scene == "blitzwait") { a.viewing = false; g.pl[0].isBot = false; }   // (you, at your place: the card in front of you)
        for (int p = 0; p < 4; ++p) {
            g.pl[p].in.avatar = avatar::preset((3 * p + 1) % avatar::presetCount());
            a.viewIn[p] = g.pl[p].in; a.viewInValid[p] = true;
        }
    }
    if (v.scene.rfind("train", 0) == 0) {   // TRAINING: trainN, lesson N's card a few seconds in (the coach showing it); trainplayN: playing
        const bool play = v.scene.rfind("trainplay", 0) == 0;
        const int n = std::atoi(v.scene.c_str() + (play ? 9 : 5));
        a.lobby = false;
        a.trainer.begin(a.game, n - 1, a.classicCol);
        if (play) a.trainer.start(a.game);
        PlayerInput in[kMaxPlayers];
        const Quat face = Quat::axisAngle({0, 1, 0}, deg2rad(-90.f));
        const size_t atSec = v.scene.find('@');   // (trainN@SECONDS: that far into it)
        const float secs = atSec != std::string::npos ? (float)std::atof(v.scene.c_str() + atSec + 1) : play ? 7.f : 4.f;
        for (int i = 0; i < (int)(72 * secs); ++i) {
            in[0] = PlayerInput{};
            in[0].headTracked = true;
            in[0].head = {face, a.game.tun.start[0] + Vec3{0, 1.65f, 0}};
            for (int h = 0; h < 2; ++h) {
                in[0].hand[h].tracked = true;
                in[0].hand[h].grip = {face, a.game.tun.start[0] + Vec3{0.25f, 1.05f, h ? 0.28f : -0.28f}};
            }
            in[0].avatar = a.myAvatar;
            a.trainer.step(a.game, 1.f / 72, in);
            trainingFeedback(a, 1.f / 72, a.trainer.phase);
        }
        for (int p = 0; p < 2; ++p) { a.viewIn[p] = a.game.pl[p].in; a.viewInValid[p] = true; }
        a.viewIn[0] = in[0];
    }
    if (v.scene.rfind("stance", 0) == 0 && v.scene.size() == 7) {   // stance0..5: the practice bot standing in that stance
        Game& g = a.game;
        g.reset(true);
        g.match = MatchState::Countdown;
        g.stateTimer = 100.f;
        g.pl[1].botStance = v.scene[6] - '0';
        a.viewing = true;
        PlayerInput in[kMaxPlayers];
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
        for (int t = 0; t <= (int)EventType::RingClose; ++t) {
            GameEvent e{};
            e.type = (EventType)t; e.player = 0; e.hand = -1;
            g.events.push_back(e);
        }
        handleEvents(a);
        g.events.clear();
        a.bumpFlash = a.foulFlash = a.hitFlash[0] = a.hitFlash[1] = 0;   // (the picture shows the meeting, not those events' flashes)
        LOGI("selftest: all %d event kinds handled", (int)EventType::RingClose + 1);
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
        v.scene == "block" || v.scene == "strike" || v.scene == "release" || v.scene == "swing") {   // store art: blue throws, orange raises a shield, the ball mid-court
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
            if (p == 0 && v.handsTouch >= 0) {   // (the controllers held together, as in the owner's video, 2026-11-16)
                a.shotGloves = true;
                const Quat f = Quat::axisAngle({0, 1, 0}, -kPi * 0.5f);   // (facing down the court, as the head)
                const Vec3 c = in.head.p + f.rotate({0, -0.30f, -0.40f});
                Quat qr, ql;
                Vec3 dr, dl;
                switch (v.handsTouch) {
                case 0:   // the tops together, palms down and in (her video's start)
                    qr = Quat::axisAngle({0, 0, 1}, kPi * 0.25f); ql = Quat::axisAngle({0, 0, 1}, -kPi * 0.25f);
                    dr = {0.055f, 0, 0}; dl = {-0.055f, 0, 0};
                    break;
                case 1:   // the palms together
                    dr = {0.035f, 0, 0}; dl = {-0.035f, 0, 0};
                    break;
                case 2:   // the knuckles together
                    qr = Quat::axisAngle({0, 1, 0}, kPi * 0.5f); ql = Quat::axisAngle({0, 1, 0}, -kPi * 0.5f);
                    dr = {0.06f, 0, 0}; dl = {-0.06f, 0, 0};
                    break;
                case 3:   // the wrists crossed, right over left
                    qr = Quat::axisAngle({0, 1, 0}, kPi * 0.20f); ql = Quat::axisAngle({0, 1, 0}, -kPi * 0.20f);
                    dr = {-0.045f, 0.035f, 0}; dl = {0.045f, -0.005f, 0};
                    break;
                case 4:   // the fists together at the chest (her video's end, in the builder's mirror)
                    qr = Quat::axisAngle({0, 1, 0}, deg2rad(-30.f)) * Quat::axisAngle({1, 0, 0}, deg2rad(40.f));
                    ql = Quat::axisAngle({0, 1, 0}, deg2rad(30.f)) * Quat::axisAngle({1, 0, 0}, deg2rad(40.f));
                    dr = {0.05f, -0.05f, 0.16f}; dl = {-0.05f, -0.05f, 0.16f};
                    break;
                case 5:   // at the belly
                    qr = Quat::axisAngle({0, 1, 0}, deg2rad(-30.f)); ql = Quat::axisAngle({0, 1, 0}, deg2rad(30.f));
                    dr = {0.06f, -0.15f, 0.20f}; dl = {-0.06f, -0.15f, 0.20f};
                    break;
                case 6:   // the arms crossed
                    qr = Quat::axisAngle({0, 1, 0}, deg2rad(70.f)); ql = Quat::axisAngle({0, 1, 0}, deg2rad(-70.f));
                    dr = {-0.12f, -0.05f, 0.16f}; dl = {0.12f, -0.08f, 0.20f};
                    break;
                case 9:   // FOREARM TWIST: the arms out in front, the palms turned up (the forearms twisted the most)
                case 10:  // ... the palms turned down
                    qr = Quat::axisAngle({0, 0, 1}, deg2rad(v.handsTouch == 9 ? -110.f : 110.f));
                    ql = Quat::axisAngle({0, 0, 1}, deg2rad(v.handsTouch == 9 ? 110.f : -110.f));
                    dr = {0.22f, 0.02f, -0.12f}; dl = {-0.22f, 0.02f, -0.12f};
                    break;
                case 8:   // THE REACH: a long-armed player's long reach to the front and side (the tests' ARM SIZING reach)
                    qr = Quat::axisAngle({0, 1, 0}, deg2rad(-40.f));
                    ql = Quat::axisAngle({0, 1, 0}, deg2rad(8.f)) * Quat::axisAngle({1, 0, 0}, deg2rad(-22.f));
                    dr = {0.50f, -0.02f, -0.20f}; dl = {-0.21f, -0.24f, 0.10f};
                    break;
                default:  // THE REACH: issue #24's far reach (tools/avatar_pose.cpp reach: the right hand far out to the side)
                    qr = Quat::axisAngle({0, 1, 0}, deg2rad(-60.f));
                    ql = Quat::axisAngle({0, 1, 0}, deg2rad(8.f)) * Quat::axisAngle({1, 0, 0}, deg2rad(-22.f));
                    dr = {0.80f, -0.22f, 0.05f}; dl = {-0.21f, -0.24f, 0.10f};
                    break;
                }
                if (v.touchGap >= 0) { dr.x = dr.x < 0 ? -v.touchGap : v.touchGap; dl.x = dl.x < 0 ? -v.touchGap : v.touchGap; }
                in.hand[1].grip = {f * qr, c + f.rotate(dr)};
                in.hand[0].grip = {f * ql, c + f.rotate(dl)};
                if (v.handsTouch >= 4) a.shotGloves = false;   // (the body drawn: these are about the arms)
            }
            if (p == 0 && v.handsOut) {   // (both controllers held straight out at chest height, palms down; blue drawn
                a.shotGloves = true;      // as in the headset, the gauntlets only: the gauntlets' wrists, plan item 17)
                const Quat flat = Quat::axisAngle({0, 1, 0}, -kPi * 0.5f);
                for (int h = 0; h < 2; ++h)
                    in.hand[h].grip = {flat * Quat::axisAngle({0, 0, 1}, h == 1 ? kPi * 0.5f : -kPi * 0.5f),
                                       in.head.p + flat.rotate({h == 1 ? 0.20f : -0.20f, -0.30f, -0.45f})};
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
        if (v.scene == "swing") {   // (thumbnails: mid-swing, the throwing arm reaching through the release, the guard
            // hand back by the chest, the ball a moment out of the hand and curving away)
            PlayerInput& in = g.pl[0].in;
            const Quat q = in.head.q;
            in.hand[1].grip = {q * Quat::axisAngle({0, 1, 0}, -0.35f) * Quat::axisAngle({1, 0, 0}, 0.20f), in.head.p + q.rotate({0.34f, -0.24f, -0.50f})};
            in.hand[0].grip = {q * Quat::axisAngle({1, 0, 0}, 0.60f), in.head.p + q.rotate({-0.24f, -0.38f, -0.22f})};
            a.viewIn[0] = in;
            b0.scale = 1.f;
            b0.pos = in.hand[1].grip.p + Vec3{0.55f, 0.12f, 0.22f};
            b0.spin = {0, 14.f, 0};   // (a curve: the twisting tracers)
            // the trail the game draws behind a flying ball, along the curve it took from the hand (oldest first)
            App::Trail& tr = a.trail[0];
            const Vec3 from = in.hand[1].grip.p, to = b0.pos, bend{0.f, -0.05f, -0.06f};
            tr.n = 16;
            for (int i = 0; i < tr.n; ++i) {
                const float t = (float)i / (tr.n - 1);
                const Vec3 c = from + (to - from) * t + bend * std::sin(kPi * t);
                const float ang = t * 6.f;
                const Vec3 w = Vec3{0, std::cos(ang), std::sin(ang)} * 0.03f;
                tr.core[i] = c; tr.helixA[i] = c + w; tr.helixB[i] = c - w;
            }
        }
        if (v.scene == "release") {   // (thumbnails, seen from the front: blue in the guard stance, elbows down by the ribs,
            // the ball just off the right hand; the follow-through above reads as broken arms from in front, owner 2026-11-02)
            PlayerInput& in = g.pl[0].in;
            const Quat q = in.head.q;
            in.hand[1].grip = {q * Quat::axisAngle({1, 0, 0}, 0.45f), in.head.p + q.rotate({0.22f, -0.32f, -0.42f})};
            in.hand[0].grip = {q * Quat::axisAngle({1, 0, 0}, 0.60f), in.head.p + q.rotate({-0.22f, -0.36f, -0.30f})};
            a.viewIn[0] = in;
            b0.scale = 1.f;
            b0.pos = in.hand[1].grip.p + Vec3{0.38f, 0.04f, 0.f};
        }
        if (v.scene == "block") { b0.scale = 1.3f; b0.pos = g.shieldPose(1, 1).p + Vec3{-0.30f, 0.f, 0.f}; }
        if (v.scene == "strike") { b0.scale = 1.f; b0.pos = g.strikeZoneCenter(1) + Vec3{-0.05f, 0.10f, 0.42f}; }   // (beside orange, in the ring)
        if (v.punchT >= 0) {   // (--punch T: blue's ball punched T s ago, as hard as a punch can, THE PUNCH LANDS)
            a.punches.push_back({b0.pos, v.punchT, 1.f, 0});
            a.squash[0].t = v.punchT; a.squash[0].power = 1.f;
            a.squash[0].axis = length(b0.vel) > 1e-3f ? normalize(b0.vel) : Vec3{1, 0, 0};
        }
    }
    if (!v.recordDir.empty() && v.recordSeconds > 0) {   // --record: a bot-vs-bot match, frame by frame
        Game& g = a.game;
        g.tun.advanced = false;
        g.tun.basicMatchTime = std::max(5.f, v.recordSeconds - 9.f);   // (the match ends a few seconds before the clip)
        const float meetTime = g.tun.meetTime;
        g.tun.meetTime = 0;
        g.reset(true);
        // a trail is its last 48 points, one a frame: as long as on a 72 Hz headset, not 30 frames' worth (2.4x longer)
        a.trailMax = v.noTrail ? -1 : (int)std::lround(48 * 30.f / 72.f);
        if (v.recordMeet) { g.tun.meetTime = meetTime; g.match = MatchState::Meet; g.stateTimer = meetTime; }   // (two bots never meet on their own)
        g.pl[0].isBot = true;
        g.pl[0].botLevel = g.pl[1].botLevel = v.recordBot;   // AMATEUR for a trailer (fewer blocks, more points); --recordbot for stills
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
            a.viewing = true;   // (stepFrame makes blue the local player again: drawn as gloves only, no body)
            for (const GameEvent& e : g.events) {
                if (ev) fprintf(ev, "%.4f,%s,%d,%.3f\n", tt, eventName(e.type), e.player, e.value);
                if (e.type == EventType::Throw && e.player >= 0 && e.player < 2)   // (stills: the curved throws, by frame)
                    LOGI("record: frame %d throw by %d, spin %.1f", f, e.player, length(g.pl[e.player].ball.spin));
                if (e.type == EventType::HitPlayer && e.value >= 0 && e.value < 2) excite[(int)e.value] = 1.f;
                if (e.type == EventType::ShieldBlock && e.player >= 0 && e.player < 2) excite[e.player] = std::max(excite[e.player], 0.7f);
            }
            Pose cam = v.custom ? eye : trailerCamera(a, tt);   // (--view: one fixed camera, for stills)
            if (v.aimPlayer == 0 || v.aimPlayer == 1) {
                const Vec3 h = a.viewIn[v.aimPlayer].head.p;
                cam = lookFrom(h + v.aimFrom, h + v.aimAt);
            }
            a.viewHead = cam;
            a.localIn.head = cam;
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glViewport(0, 0, W, H);
            glEnable(GL_FRAMEBUFFER_SRGB);
            glClearColor(0.01f, 0.012f, 0.02f, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            const float fy = std::tan(deg2rad(v.fovY) * 0.5f), fx = fy * W / H;
            a.gfx.beginEye(Mat4::viewFromPose(cam), Mat4::projection(-std::atan(fx), std::atan(fx), std::atan(fy), -std::atan(fy), 0.05f, 2000.f), cam.p);
            drawWorld(a, 0);
            drawWorld(a, 1);
            const bool keep = v.recordFrames.empty() ? f % std::max(1, v.recordStep) == 0
                                                     : std::find(v.recordFrames.begin(), v.recordFrames.end(), f) != v.recordFrames.end();
            if (!keep) { t += dt; continue; }
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
    a.mockContact = v.contact;
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
        if (has("clock")) { a.game.pl[0].shotClock = 4.f; a.game.pl[1].shotClock = 0.f; }   // the SHOT CLOCK rings: building, run out
    }
    if (v.marks >= 0) {   // --marks: bounces on the floor, both side walls and the far end, from just now to fading out
        a.markStyle = v.marks;
        const struct { Vec3 p; int who; float speed, age; } k[] = {
            {{-2.0f, 0.14f, 0.6f}, 0, 14.f, 0.04f}, {{-3.6f, 0.14f, -0.8f}, 1, 9.f, 0.35f}, {{-5.4f, 0.14f, 0.3f}, 0, 6.f, 1.1f},
            {{-2.6f, 1.3f, 2.55f}, 0, 16.f, 0.12f}, {{-4.4f, 0.9f, 2.55f}, 1, 8.f, 0.9f}, {{-3.2f, 1.6f, -2.55f}, 1, 12.f, 0.5f},
            {{-6.0f, 1.1f, -2.55f}, 0, 7.f, 2.0f}, {{-9.45f, 1.5f, 0.6f}, 1, 15.f, 0.2f}};
        for (const auto& m : k) {
            addMark(a, m.p, m.who, m.speed);
            if (!a.marks.empty()) a.marks.back().age = std::min(m.age, a.marks.back().life * 0.9f);
        }
        LOGI("picture: %zu marks", a.marks.size());
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
        if (v.dcshot == "round") { g.match = MatchState::RoundSummary; g.lastRoundWinner = 0; g.stateTimer = 3.f; }   // (ROUND WON)
#ifdef _WIN32
        if (v.selfie >= 0) {   // SELFIE with its test card
            a.selfieOn = a.selfieTest = true; a.selfieCorner = v.selfie & 3; a.selfieRound = !(v.selfie & 4);
            a.selfieSize = (v.selfie & 8) ? 0 : (v.selfie & 16) ? 2 : 1;
        }
#endif
        if (v.dcshot == "hub") {   // the hub of live miniatures, this match among them
            a.desktop = true;      // (as the PC window draws it: no headset's sign in the arena)
            a.net.matches().back().game = g;
            a.net.matches().back().room.botMask = 0xF;   // (its players shown: bots, here)
            a.watchIp = 0;
            a.spectating = true;
        }
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
        if (v.custom && (v.dcshot == "play" || v.dcshot == "round")) a.dcView = 0;   // (the given camera, no broadcast moment)
        else for (int f = 0; f <= 90; ++f) eye = eventCamera(a, desktopPreset(a, a.dcView), 1000.0 + f / 60.0, 1.f / 60.f, false);
        a.viewHead = eye;
        a.dcFov = v.fovY;
        desktopRender(a, eye, W, H, 1.f / 60.f);
        if (dcWinSplit(a)) desktopRender(a, eye, W, H, 1.f / 60.f);   // (a second frame: the close-ups have settled)
    } else {
        a.viewHead = eye;   // (what turns to the viewer, FOUL and the sprites, turns to the picture's camera)
        if (v.headSelfie) {   // SELFIE in the headset: the spectator at the picture's camera, hands up, waving
            a.spectating = true; a.watchIp = 1; a.headSelfie = true;
            a.localIn = PlayerInput{};
            a.localIn.headTracked = true; a.localIn.head = eye;
            for (int h = 0; h < 2; ++h) {
                a.localIn.hand[h].tracked = true;
                a.localIn.hand[h].grip = {eye.q * Quat::axisAngle({1, 0, 0}, 0.4f), eye.transform({h ? 0.22f : -0.22f, -0.12f, -0.30f})};
            }
            headSelfieRender(a);
            a.viewHead = eye;
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);   // (the picture's own target again)
            glViewport(0, 0, W, H);
            a.gfx.beginEye(Mat4::viewFromPose(eye), Mat4::projection(-std::atan(tx), std::atan(tx), std::atan(ty), -std::atan(ty), 0.05f, 2000.f), eye.p);
        }
        drawWorld(a, 0);
        drawWorld(a, 1);
    }
    if (!v.menu.empty() && v.menu != "careers") {
        a.menuOpen = true;
        a.titleOpen = v.menu == "title";
        a.xpOpen = v.menu == "xp";
        a.settingsOpen = v.menu == "settings" || v.menu == "voice" || v.menu == "graphics" || v.menu == "controllers";
        if (v.menu == "controllers") { a.ctlPage = true; a.ctlTilt = 10; a.ctlTwist = -5; }
        a.voicePage = v.menu == "voice";
        if (!v.buttons.empty() && !controls::load(a.btnMap, v.buttons)) LOGW("--buttons %s not read", v.buttons.c_str());
        a.ctlKind = v.ctlKind;
        if (v.menu == "buttons") a.settingsOpen = a.ctlPage = a.btnPage = true;
        a.gfxPage = v.menu == "graphics";
        if (a.gfxPage) a.roomFeel = 2;
        if (v.menu == "passthrough") { a.settingsOpen = true; a.ptPage = true; a.hasPassthrough = true; a.ptMode = PT_WINDOW; a.ptFloor = 1.f; }
        a.helpOpen = v.menu == "help" || v.menu.rfind("credits", 0) == 0;
        a.creditsOpen = v.menu.rfind("credits", 0) == 0;   // (HOW TO PLAY > CREDITS)
        if (v.menu == "name") { a.nameEdit = true; a.editTarget = 0; a.nameBuf = "MOJO"; }
        if (v.menu == "titlename") { a.titleOpen = true; a.nameEdit = true; a.editTarget = 0; a.nameBuf = "MOJO"; }   // (CHANGE NAME pressed on the title page)
        if (v.menu == "spectate") a.net.spectate();
        if (v.menu == "statsnotice") a.statsNoticeOpen = true;
        if (v.menu == "ownfail") { a.ownFail = true; a.ownFailAt = nowSeconds() - 4.0; }   // (a store build, the purchase not confirmed)
        if (v.menu == "trainlist" || v.menu == "trainboard") {   // TRAINING: the lessons (some passed), a leaderboard
            a.trainBests.load("1 1 14.2 5 5\n2 1 25.3 8 11\n3 1 16.0 5 7\n4 1 21.7 4 9\n");
            a.trainOpen = v.menu == "trainlist";
            if (v.menu == "trainboard") {
                a.trainBoard = 2; a.trainBoardKind = 0;
                Net::Board b;
                b.lesson = training::lessons()[2].id; b.kind = 0; b.myPlace = 4; b.players = 23; b.myValue = 16000;
                static const char* nm[6] = {"ASTROBLEEM", "MOJO", "SPARKY", "JEN", "QUICKHANDS", "LEFTY"};
                const uint32_t ms[6] = {12100, 13400, 15250, 16000, 17800, 19950};
                for (int i = 0; i < 6; ++i) { Net::BoardRow r{}; std::snprintf(r.name, sizeof r.name, "%s", nm[i]); r.value = ms[i]; b.rows.push_back(r); }
                a.net.boardForPictures(b);
            }
        }
        if (v.menu == "studio" || v.menu == "studiomatch") {   // SOUND STUDIO with a made-up library: SHIELD (or MATCH)
            beginLesson(a, training::lessonById(21), true);
            a.trainer.endless = true;
            a.studioOn = true;
            a.studioSection = v.menu == "studio" ? 0 : 4;
            const char* files[4] = {"ShieldBreak__glass_crack.wav", "ShieldBreak__deep_thud.wav", "ShieldBlock__soft_tap.wav", "Countdown__beep_beep.wav"};
            for (int i = 0; i < 4; ++i) {
                studio::Sound s;
                s.file = files[i]; s.event = studio::eventOf(files[i]); s.name = studio::prettyName(files[i]); s.ready = true; s.vote = i == 0 ? 1 : 0;
                a.studioSounds.push_back(s);
            }
            for (const studio::Sound& o : studio::officialSounds()) a.studioSounds.push_back(o);
            a.studioPick["Recharge"] = "official:Strike";   // (the game's own sound of another slot)
            a.studioPick["ShieldBreak"] = files[0];
            a.studioPick["ShieldBlock"] = files[1];   // (another event's sound, tried here)
            a.studioPick["Countdown"] = files[3];
            a.menuOpen = true;   // (starting the lesson closed it)
        }
        if (v.menu == "trainclick" && a.trainer.active()) {   // (a check: the result page's buttons answer the pointer)
            a.trainer.phase = training::Phase::Done;
            a.trainer.result = training::Result{a.trainer.lesson, true, 5, 6, 15.2f};
            PlayerInput pi;
            pi.headTracked = true;
            pi.head = {Quat::axisAngle({0, 1, 0}, deg2rad(-90.f)), a.game.tun.start[0] + Vec3{0, 1.7f, 0}};
            openMenu(a, pi);
            const std::vector<MenuItem> items = menuItems(a);
            for (const MenuItem& it : items) {
                const Vec3 target = a.menuPose.transform({it.x, it.y, 0});
                const Vec3 o = pi.head.p + Vec3{0.2f, -0.4f, 0.2f};
                a.aim[a.pointerHand] = {yawToward(normalize(Vec3{target.x - o.x, 0, target.z - o.z})) *
                                            Quat::axisAngle({1, 0, 0}, std::asin(clampf(normalize(target - o).y, -1.f, 1.f))), o};
                a.aimValid[a.pointerHand] = true;
                pi.hand[a.pointerHand].trigger = 0; updateMenu(a, pi);
                LOGI("trainclick: %s -> hover %d (%s)", it.label.c_str(), a.menuHover, a.menuHover >= 0 ? menuItems(a)[a.menuHover].label.c_str() : "-");
            }
        }
        if (v.menu == "traincard" && a.trainer.active()) {   // (its tutorial video, some seconds in)
            a.trainVideo = true;
            filmLoad(a, a.trainer.lesson);
            for (int k = 0; k < (int)(72 * v.filmSeconds); ++k) filmStep(a, 1.f / 72);
            a.film.renderedAt = 0;
            filmRender(a);
            if (a.rttFbo[0]) {   // (the video's own picture as well: OUT.film.ppm)
                GLint keep = 0;
                glGetIntegerv(GL_FRAMEBUFFER_BINDING, &keep);
                glBindFramebuffer(GL_FRAMEBUFFER, a.rttFbo[0]);
                std::vector<uint8_t> px(640 * 360 * 4);
                glReadPixels(0, 0, 640, 360, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)keep);
                if (FILE* ff = fopen((std::string(out) + ".film.ppm").c_str(), "wb")) {
                    fprintf(ff, "P6\n640 360\n255\n");
                    for (int yy = 359; yy >= 0; --yy) for (int xx = 0; xx < 640; ++xx) fwrite(&px[(yy * 640 + xx) * 4], 1, 3, ff);
                    fclose(ff);
                }
            }
        }
        if (v.menu == "traindone" && a.trainer.active()) {   // a lesson just passed, a new best
            a.trainer.phase = training::Phase::Done;
            a.trainer.result = training::Result{a.trainer.lesson, true, 5, 6, 15.2f};
            a.trainNewBest = true;
        }
        if (v.menu == "stats") {   // MY STATS with a made-up career
            a.statsOpen = true; a.myCareerOk = true;
            Net::CareerStats& c = a.myCareer;
            c.known = true; c.matches = 24; c.wins = 15; c.throws = 410; c.onTarget = 262; c.hits = 120; c.blocks = 88;
            c.faced = 160; c.catches = 31; c.returnHits = 14; c.topSpeed = 24.8f;
        }
        if (v.menu == "bug") { a.bugOpen = true; a.bugCat = 1; a.bugNote = "BALL WENT THROUGH THE WALL"; }
        if (v.menu == "update") a.updateOpen = true;   // UPDATE NOW's page (as this copy would update)
        if (v.menu == "rules") { a.rulesOpen = true; a.rulesForHost = true; a.game.tun.advanced = true; }
        if (v.menu == "rulesprac") { a.rulesOpen = true; a.rulesForHost = false; a.game.tun.advanced = false; a.game.tun.experimental = true; }
        if (v.menu == "optionsexp") {   // MATCH OPTIONS for practice, EXPERIMENTAL: the room choice
            a.rulesOpen = true; a.rulesForHost = false; a.rulesStep = 1; optionsFrom(a, a.game.tun);
            a.optAdvanced = false; a.optExperimental = true; a.optExpRoom = 0;
        }
        if (v.menu == "moveplayers") {   // MOVE PLAYERS, FOUR CORNERS 3 v 1 (not hosting here: the rows show, greyed)
            a.game.tun.experimental = true; a.game.tun.expRoom = 1; a.game.tun.players = 4; a.game.tun.teamMode = TEAMS_3V1;
            a.game.tun.theOne = 2;
            a.game.resetWithBots(0xE);
            a.movePlayersOpen = true; a.moveSel = 1;
        }
        if (v.menu == "optionsblitz") {   // MATCH OPTIONS for practice, FIREBALL BLITZ: health, balls, how many play
            a.rulesOpen = true; a.rulesForHost = false; a.rulesStep = 1; optionsFrom(a, a.game.tun);
            a.optAdvanced = false; a.optExperimental = true; a.optExpRoom = ROOM_BLITZ; a.optShields = false; a.optKnuckle = 1;
            a.optHealth = 5; a.optBalls = 4; a.optOctPlayers = 3;
        }
        if (v.menu == "options3v1") {   // MATCH OPTIONS hosting FOUR CORNERS, 3 V 1 with THE ONE taking turns
            a.rulesOpen = true; a.rulesForHost = true; a.rulesStep = 1; optionsFrom(a, a.game.tun);
            a.optAdvanced = false; a.optExperimental = true; a.optExpRoom = 1; a.optTeams = TEAMS_3V1; a.optOneTurns = true;
        }
        if (v.menu == "roomsquare" || v.menu == "roomdiamond" || v.menu.rfind("roundspad", 0) == 0 || v.menu == "roomball") {   // ROOMS AND
            a.rulesOpen = true; a.rulesForHost = false; a.rulesStep = 1; optionsFrom(a, a.game.tun);   // (THEIR OPTIONS: ROUNDS, MAIDEN, ...)
            a.optExperimental = v.menu == "roomdiamond"; a.optExpRoom = ROOM_DIAMOND; a.optAdvanced = true; a.optRounds = 3;
            a.optMaiden = true; a.optAim = 3; a.optZone = 2; a.optGrowth = 1; a.optShieldSize = 2;
            if (v.menu.rfind("roundspad", 0) == 0) { a.roundsPad = true; a.roundsBuf = v.menu.substr(9); }   // (--menu roundspad4: "4" typed)
            if (v.menu == "roomball") a.roomBallPage = true;   // (the room's BALL FEEL)
        }
        if (v.menu == "roomcorners" || v.menu == "roomblitz") {   // (the four-player rooms' pages, with BALL FEEL)
            a.rulesOpen = true; a.rulesForHost = true; a.rulesStep = 1; optionsFrom(a, a.game.tun);
            a.optAdvanced = false; a.optExperimental = true; a.optExpRoom = v.menu == "roomblitz" ? ROOM_BLITZ : ROOM_CORNERS;
            if (v.menu == "roomblitz") { a.optShields = false; a.optKnuckle = 1; }
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
        if (trainSide(a)) {   // (TRAINING's card where the headset puts it: for a player at the start, facing the court)
            PlayerInput pi;
            pi.head = {Quat::axisAngle({0, 1, 0}, deg2rad(-90.f)), a.game.tun.start[0] + Vec3{0, 1.7f, 0}};
            openMenu(a, pi);
        }
        a.viewHead = eye;
        a.localIn.head = eye;
        drawTitleShowcase(a, 0);
        drawTitleShowcase(a, 1);
        drawTrainVideo(a, 0);
        drawTrainVideo(a, 1);
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
    if (v.depthOut) {   // (--depth: the AO mock-up's input; near 0.05, far 2000, OpenGL's -1..1 depth range)
        std::vector<float> dz((size_t)W * H);
        glReadPixels(0, 0, W, H, GL_DEPTH_COMPONENT, GL_FLOAT, dz.data());
        if (FILE* fd = fopen((std::string(out) + ".depth").c_str(), "wb")) { fwrite(dz.data(), sizeof(float), dz.size(), fd); fclose(fd); }
    }
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

// 1-5 the views; 6, 7 blue's and orange's own eyes (owner, 2026-11-02: first person, from their headset, smoothed);
// 8 MY CAMERA (dcMy); 0 the free camera
static const char* kDcViewName[8] = {"FREE CAMERA", "SIDE", "HIGH SIDE", "BLUE END", "ORANGE END", "TOP", "BLUE'S EYES", "ORANGE'S EYES"};
static const int kDcMine = 8;
static const char* kDcLayerName[4] = {"FULL", "NO WALLS", "MINIMAL", "KEY"};
// TRANSPARENT: no colour at all behind the players (OBS Game Capture with Allow Transparency; no colour can clash)
static const char* kDcKeyName[4] = {"GREEN", "MAGENTA", "BLUE", "TRANSPARENT"};
static const float kDcKeyRgb[4][3] = {{0, 1, 0}, {1, 0, 1}, {0, 0, 1}, {0, 0, 0}};
// the VS screen (MATCHUP): at the fist bump and the countdown (or any time with M), two portraits facing each other,
// VS between them, each player's name, flag and career stats under them
static bool dcVsScreen(const App& a) {
    const Game& g = a.game;
    if (!a.watchIp) return false;
    if (g.np != 2) return false;   // (FOUR CORNERS: no one-on-one matchup card)
    if (a.dcMatchupForce) return true;
    return a.dcMatchup && (g.match == MatchState::Meet || g.match == MatchState::Countdown);
}

// WIN SCREEN, per window (a stream with several spectator windows can show each): SPLIT the winner on the left and the
// loser on the right, each alone in a close-up; WINNER the camera flies to the winner, their name over it; YOUR CAMERA
// the camera stays where it is, the winner's name over it
static const char* kDcWinName[3] = {"SPLIT", "WINNER", "YOUR CAMERA"};
static bool dcWinSplit(const App& a) {
    return a.watchIp && a.dcWinStyle == 0 && a.game.np == 2 && a.game.match == MatchState::MatchSummary && a.game.matchWinner >= 0 &&
           a.game.matchWinner < 2;
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

static std::string dcViewName(const App& a) {
    if (a.dcView == kDcMine && a.dcMy >= 0 && a.dcMy < (int)a.warps.size()) {
        static const char* kTrig[3] = {"", "  ACTION", "  FOLLOW"};
        const App::WarpCam& w = a.warps[a.dcMy];
        return "MY CAMERA " + std::to_string(a.dcMy + 1) + kTrig[std::min(2, (int)w.trig)];
    }
    return kDcViewName[a.dcView % 8];
}
static Pose desktopPreset(const App& a, int v) {
    if (a.watchIp == 0) return lookFrom({-1.3f, 1.75f, 0.f}, {1.4f, 0.85f, 0.f});   // the hub of live miniatures
    if (cornersRoom(a)) {   // FOUR CORNERS (0.3.12): outside its glass, the whole room in view
        if (v == 2) return lookFrom({0, 12.5f, 3.f}, {0, -0.5f, 0});
        if (v != 3 && v != 4 && v != 5) return lookFrom({-8.2f, 5.2f, 8.2f}, {0, 0.2f, 0});
    }
    switch (v) {
    case 2: return lookFrom({0, 5.2f, 6.6f}, {0, 0.6f, 0});   // above the near glass (further back is in the stands)
    // (the ends from above their glass, looking down the court: at head height behind it the near player stood inside
    // the end's rings and the far one was small through two walls; owner, 2026-11-14)
    case 3: return lookFrom({-12.5f, 7.0f, 0}, {0, 0.6f, 0});
    case 4: return lookFrom({12.5f, 7.0f, 0}, {0, 0.6f, 0});
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
enum { DC_VIEW = 100, DC_AUTO = 110, DC_FREE, DC_NEXT, DC_HUB, DC_FULL, DC_HELP, DC_PANEL, DC_LAYER, DC_LAYERBACK, DC_KEYCOL, DC_WINSTYLE, DC_MATCHUP,
       DC_MYSAVE, DC_MYHOLD, DC_MYTRANS, DC_MYTRIG, DC_MYCLEAR, DC_DISCORD, DC_SELFIE, DC_SELFIECORNER, DC_SELFIESIZE,
       DC_SELFIESHAPE, DC_SELFIECAM, DC_UPDATE, DC_GLASS, DC_AUTOLIST, DC_AUTOHOLD, DC_AUTOUP, DC_AUTODOWN, DC_MYGO = 140, DC_MYSET = 150 };   // (DC_MYGO / DC_MYSET + n: camera n)

// the Spectator Controls window's tabbed pages (SETUP, AUTO, HELP), each its controls, shown one at a time
static std::vector<HWND> g_dcPage[3];
static HWND g_dcTabs = nullptr;
static bool g_dcAutoFilling = false;   // (the AUTO ROTATION list being filled: its ticks changing are not the streamer's)
static void dcShowPage(int k) {
    for (int i = 0; i < 3; ++i)
        for (HWND c : g_dcPage[i]) ShowWindow(c, i == k ? SW_SHOW : SW_HIDE);
}

static LRESULT CALLBACK dcPanelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    App* a = (App*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_COMMAND && a && HIWORD(w) == BN_CLICKED) { a->dcCmd = LOWORD(w); SetForegroundWindow(a->hwnd); return 0; }
    if (m == WM_NOTIFY && ((NMHDR*)l)->hwndFrom == g_dcTabs && ((NMHDR*)l)->code == TCN_SELCHANGE) {
        dcShowPage((int)SendMessageW(g_dcTabs, TCM_GETCURSEL, 0, 0));
        return 0;
    }
    if (m == WM_NOTIFY && a && ((NMHDR*)l)->hwndFrom == a->dcAutoView && ((NMHDR*)l)->code == LVN_ITEMCHANGED && !g_dcAutoFilling) {
        const NMLISTVIEW* nm = (const NMLISTVIEW*)l;   // an IN AUTO tick clicked (handled next frame)
        if ((nm->uChanged & LVIF_STATE) && ((nm->uNewState ^ nm->uOldState) & LVIS_STATEIMAGEMASK) && (nm->uOldState & LVIS_STATEIMAGEMASK)) {
            a->dcAutoTick = nm->iItem;
            a->dcAutoTickOn = ((nm->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
        }
        return 0;
    }
    if (m == WM_CTLCOLORSTATIC) return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);   // (labels on the window's own colour)
    if (m == WM_CLOSE) { ShowWindow(h, SW_HIDE); return 0; }   // F1 brings it back
    return DefWindowProcW(h, m, w, l);
}

// a small window beside the spectator, never part of the captured picture (owner, 2026-11-02: one long list was too busy;
// what a streamer needs mid-stream always in view): LIVE at the top, then the tabs (SETUP, AUTO, HELP), then what is on screen
static void desktopPanel(App& a) {
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_TAB_CLASSES};
    InitCommonControlsEx(&icc);
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
    const int W = 340;   // (client width)
    RECT want{0, 0, W, 760};
    AdjustWindowRectEx(&want, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOOLWINDOW);
    a.dcPanel = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Spectator Controls", WS_CAPTION | WS_SYSMENU,
                                r.right + 8, r.top, want.right - want.left, want.bottom - want.top, a.hwnd, nullptr, wc.hInstance, nullptr);
    if (!a.dcPanel) return;
    SetWindowLongPtrW(a.dcPanel, GWLP_USERDATA, (LONG_PTR)&a);
    static HFONT font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    static HFONT bold = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    int page = -1;   // (the page the next controls belong to; -1: every page)
    auto add = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT f = nullptr) {
        HWND c = CreateWindowW(cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, a.dcPanel, (HMENU)(INT_PTR)id, wc.hInstance, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : font), TRUE);
        if (page >= 0) g_dcPage[page].push_back(c);
        return c;
    };
    const int L = 12, full = W - 2 * L, half = (full - 8) / 2, third = (full - 16) / 3, B = 28;   // (button height)
    auto button = [&](const wchar_t* t, int col, int cols, int y, int id) {   // one of `cols` in a row
        const int w = cols == 1 ? full : cols == 2 ? half : third;
        add(L"BUTTON", t, BS_PUSHBUTTON, L + col * (w + 8), y, w, B, id);
    };
    auto heading = [&](const wchar_t* t, int y) { add(L"STATIC", t, SS_LEFT, L, y, full, 18, 0, bold); };
    auto note = [&](const wchar_t* t, int y, int lines) { add(L"STATIC", t, SS_LEFT, L, y, full, 17 * lines, 0); };
    // ---- LIVE: everything used while streaming, always in view (owner, 2026-11-02: no fumbling mid-stream)
    page = -1;
    int y = 10;
    auto cell = [&](const wchar_t* t, int col, int cols, int y, int id) {   // a row of `cols` equal buttons
        const int w = (full - 4 * (cols - 1)) / cols;
        add(L"BUTTON", t, BS_PUSHBUTTON, L + col * (w + 4), y, w, B, id);
    };
    heading(L"Cameras", y); y += 22;
    cell(L"1 Side", 0, 5, y, DC_VIEW + 1); cell(L"2 High", 1, 5, y, DC_VIEW + 2); cell(L"3 Blue", 2, 5, y, DC_VIEW + 3);
    cell(L"4 Orange", 3, 5, y, DC_VIEW + 4); cell(L"5 Top", 4, 5, y, DC_VIEW + 5); y += B + 6;
    button(L"6  Blue's eyes", 0, 2, y, DC_VIEW + 6); button(L"7  Orange's eyes", 1, 2, y, DC_VIEW + 7); y += B + 6;
    button(L"F  Free camera", 0, 2, y, DC_FREE); button(L"A  Auto on / off", 1, 2, y, DC_AUTO); y += B + 10;
    heading(L"My cameras (Ctrl + number saves)", y); y += 22;
    for (int i = 0; i < 8; ++i) { wchar_t t[4] = {wchar_t(L'1' + i), 0}; cell(t, i, 8, y, DC_MYGO + i); }
    y += B + 6;
    button(L"Save this view", 0, 2, y, DC_MYSAVE); button(L"V  Next view layer", 1, 2, y, DC_LAYER); y += B + 10;
    heading(L"Show", y); y += 22;
    button(L"Tab  Next live match", 0, 2, y, DC_NEXT); button(L"H  Spectator hub", 1, 2, y, DC_HUB); y += B + 6;
    button(L"C  Selfie on / off", 0, 2, y, DC_SELFIE); button(L"F11  Full screen", 1, 2, y, DC_FULL); y += B + 12;
    // ---- the tabs: set up before going live, and the keys
    g_dcTabs = CreateWindowW(WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 6, y, W - 12, 26, a.dcPanel, nullptr, wc.hInstance, nullptr);
    SendMessageW(g_dcTabs, WM_SETFONT, (WPARAM)bold, TRUE);
    const wchar_t* tabs[3] = {L"Setup", L"Auto", L"Help"};
    for (int i = 0; i < 3; ++i) {
        TCITEMW it{};
        it.mask = TCIF_TEXT;
        it.pszText = (LPWSTR)tabs[i];
        SendMessageW(g_dcTabs, TCM_INSERTITEMW, i, (LPARAM)&it);
    }
    const int top = y + 36;
    // ---- SETUP
    page = 0;
    y = top;
    heading(L"My camera showing", y); y += 22;
    button(L"Hold time", 0, 3, y, DC_MYHOLD); button(L"Fade or cut", 1, 3, y, DC_MYTRANS); button(L"Camera type", 2, 3, y, DC_MYTRIG); y += B + 6;
    button(L"Clear all my cameras", 0, 1, y, DC_MYCLEAR); y += B + 10;
    heading(L"Selfie (your webcam; it stays on this PC)", y); y += 22;
    button(L"Corner", 0, 3, y, DC_SELFIECORNER); button(L"Size", 1, 3, y, DC_SELFIESIZE); button(L"Round / square", 2, 3, y, DC_SELFIESHAPE); y += B + 6;
    button(L"Next webcam", 0, 2, y, DC_SELFIECAM); y += B + 10;
    heading(L"Screens", y); y += 22;
    button(L"R  Win screen", 0, 2, y, DC_WINSTYLE); button(L"M  VS screen", 1, 2, y, DC_MATCHUP); y += B + 6;
    button(L"F2  Help in picture", 0, 2, y, DC_HELP); button(L"Discord status on / off", 1, 2, y, DC_DISCORD); y += B + 6;
    button(L"Arena glass: normal / faint / fainter", 0, 1, y, DC_GLASS); y += B + 10;
    heading(L"Updates", y); y += 22;
    button(L"Update now / restart to update", 0, 1, y, DC_UPDATE); y += B + 6;
    int pageEnd = y;
    // ---- AUTO: AUTO ROTATION (owner, 2026-11-12; its own page: on SETUP the window grew taller than a 1080 screen)
    page = 1;
    y = top;
    heading(L"Auto rotation", y); y += 22;
    note(L"AUTO plays the ticked cameras in this order, each for\nits hold time. Action cameras aren't listed: they still\ncut in on hits near them.", y, 3); y += 56;
    a.dcAutoView = add(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | WS_BORDER, L, y, full, 222, DC_AUTOLIST);
    SendMessageW(a.dcAutoView, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
    {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.cx = full - 92; c.pszText = (LPWSTR)L"In auto  Camera";
        SendMessageW(a.dcAutoView, LVM_INSERTCOLUMNW, 0, (LPARAM)&c);
        c.cx = 68; c.pszText = (LPWSTR)L"Hold";
        SendMessageW(a.dcAutoView, LVM_INSERTCOLUMNW, 1, (LPARAM)&c);
    }
    y += 226;
    button(L"Hold time", 0, 3, y, DC_AUTOHOLD); button(L"Move up", 1, 3, y, DC_AUTOUP); button(L"Move down", 2, 3, y, DC_AUTODOWN); y += B + 6;
    pageEnd = std::max(pageEnd, y);
    // ---- HELP
    page = 2;
    y = top;
    heading(L"Keyboard", y); y += 22;
    note(L"1-7 views (6, 7 the players' eyes), F free (right mouse looks, W A S D\nmoves, Q / E down and up, Shift faster), A auto, Tab\nnext match, H hub, V view layer, R win screen, M VS,\nC selfie, F11 full screen, F2 help. Ctrl + 1-8 saves\nmy camera, Shift + 1-8 shows it. Wheel zooms.", y, 5); y += 92;
    heading(L"Controller (Xbox or PlayStation 4)", y); y += 22;
    note(L"A next match, B hub, X auto, Y free camera, D-pad\nleft / right cameras, up / down layers, triggers zoom.", y, 2); y += 40;
    note(L"Stream only the \"Project Arena - Spectator\" window;\nthis one stays off it. F1 shows or hides it.", y, 2);
    // ---- every page: what is on screen now
    page = -1;
    y = pageEnd + 4;
    heading(L"Now", y); y += 20;
    a.dcStatus = add(L"STATIC", L"", SS_LEFT, L, y, full, 17 * 10, 0);
    y += 17 * 10 + 8;
    RECT fit{0, 0, W, y};   // (the window as tall as what is in it)
    AdjustWindowRectEx(&fit, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOOLWINDOW);
    SetWindowPos(a.dcPanel, nullptr, 0, 0, fit.right - fit.left, fit.bottom - fit.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    dcShowPage(0);
    ShowWindow(a.dcPanel, SW_SHOWNOACTIVATE);
}

// AUTO ROTATION's list on this PC (autocams.cfg; only once the streamer has changed it: until then it follows the cameras)
static void saveAutoList(App& a) {
    const char* ext = dataDir(a);
    if (!ext || !a.dcAutoCustom) return;
    if (FILE* fp = fopen((std::string(ext) + "/autocams.cfg").c_str(), "w")) {
        fputs(autorot::save(a.dcAutoList).c_str(), fp);
        fclose(fp);
    }
}

static void loadAutoList(App& a) {
    a.dcAutoLoaded = true;
    const char* ext = dataDir(a);
    if (!ext) return;
    FILE* fp = fopen((std::string(ext) + "/autocams.cfg").c_str(), "r");
    if (!fp) return;
    std::string s;
    char b[256];
    size_t n;
    while ((n = fread(b, 1, sizeof b, fp)) > 0 && s.size() < 4096) s.append(b, n);
    fclose(fp);
    a.dcAutoList = autorot::load(s);
    a.dcAutoCustom = !a.dcAutoList.empty();
    LOGI("desktop: auto rotation, the streamer's list (%d rows)", (int)a.dcAutoList.size());
}

// the list in the controls window, filled again only when what it shows has changed (the row picked stays picked)
static void dcAutoRefresh(App& a, int select = -1) {
    if (!a.dcAutoView) return;
    static const wchar_t* kName[8] = {L"", L"Side", L"High side", L"Blue end", L"Orange end", L"Top", L"Blue's eyes", L"Orange's eyes"};
    std::vector<std::wstring> name, hold;
    std::wstring sig;
    for (const autorot::Item& it : a.dcAutoList) {
        wchar_t n[48], h[24];
        if (it.mine) {
            const App::WarpCam& w = a.warps[it.id];
            swprintf(n, 48, L"My camera %d%s", it.id + 1, w.trig == 2 ? L" (follow)" : L"");
            swprintf(h, 24, L"%.0f s", kHoldSecs[w.hold & 3]);
        } else {
            swprintf(n, 48, L"%s", kName[it.id & 7]);
            swprintf(h, 24, L"%.0f s", it.hold);
        }
        name.push_back(n); hold.push_back(h);
        sig += n; sig += L'|'; sig += h; sig += it.on ? L"+" : L"-";
    }
    if (sig == a.dcAutoShown && select < 0) return;
    a.dcAutoShown = sig;
    const int sel = select >= 0 ? select : (int)SendMessageW(a.dcAutoView, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    g_dcAutoFilling = true;
    SendMessageW(a.dcAutoView, LVM_DELETEALLITEMS, 0, 0);
    for (int i = 0; i < (int)name.size(); ++i) {
        LVITEMW it{};
        it.mask = LVIF_TEXT; it.iItem = i; it.pszText = (LPWSTR)name[i].c_str();
        SendMessageW(a.dcAutoView, LVM_INSERTITEMW, 0, (LPARAM)&it);
        it.iSubItem = 1; it.pszText = (LPWSTR)hold[i].c_str();
        SendMessageW(a.dcAutoView, LVM_SETITEMTEXTW, i, (LPARAM)&it);
        LVITEMW st{};
        st.stateMask = LVIS_STATEIMAGEMASK; st.state = INDEXTOSTATEIMAGEMASK(a.dcAutoList[i].on ? 2 : 1);
        SendMessageW(a.dcAutoView, LVM_SETITEMSTATE, i, (LPARAM)&st);
    }
    if (sel >= 0 && sel < (int)name.size()) {
        LVITEMW st{};
        st.stateMask = LVIS_SELECTED | LVIS_FOCUSED; st.state = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(a.dcAutoView, LVM_SETITEMSTATE, sel, (LPARAM)&st);
        SendMessageW(a.dcAutoView, LVM_ENSUREVISIBLE, sel, FALSE);
    }
    g_dcAutoFilling = false;
}

static Pose desktopControls(App& a, double t, float dt) {
    std::vector<SpecMatch>& ms = a.net.matches();
    // keys and the Spectator Controls window's buttons give the same commands
    int cmd = a.dcCmd;
    a.dcCmd = 0;
    if (dcPressed(a, VK_TAB)) cmd = DC_NEXT;
    if (dcPressed(a, 'H')) cmd = DC_HUB;
    const bool ctrl = dcDown(a, VK_CONTROL), shift = dcDown(a, VK_SHIFT);
    for (int v = 1; v <= 8; ++v)   // 1 to 5 the views; Ctrl + 1 to 8 saves the view as MY CAMERA n, Shift + 1 to 8 shows it
        if (dcPressed(a, '0' + v)) cmd = ctrl ? DC_MYSET + v - 1 : shift ? DC_MYGO + v - 1 : v <= 7 ? DC_VIEW + v : 0;
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
    if (dcPressed(a, 'C')) cmd = DC_SELFIE;
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
        if (b & (next | prev)) {   // round the five views, then your cameras
            const int n = 7 + (int)a.warps.size();
            const int at = a.dcView == kDcMine && a.dcMy >= 0 ? 7 + a.dcMy : (a.dcView == 0 ? 1 : a.dcView) - 1;
            const int k = (at + ((b & next) ? 1 : n - 1)) % n;
            cmd = k < 7 ? DC_VIEW + k + 1 : DC_MYGO + k - 7;
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
    if (cmd > DC_VIEW && cmd <= DC_VIEW + 7) {
        a.dcView = cmd - DC_VIEW; a.dcAuto = false;
        if (a.dcEventKind) a.dcEventSkip = a.dcEventKind;   // picked during the fist bump / winner close-up: show it now
        LOGI("desktop: camera %s", kDcViewName[a.dcView % 8]);
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
    // MY CAMERAS: save the view (free, a preset, or one of yours) as camera n, show camera n, change the one showing
    auto showMine = [&](int i, bool byHand) {
        if (i < 0 || i >= (int)a.warps.size()) return;
        const App::WarpCam& w = a.warps[i];
        if (w.trans && a.dcLastOk) { a.dcFrom = a.dcLast; a.dcGlide = 1.f; }
        else a.dcGlide = 0.f;
        a.dcView = kDcMine; a.dcMy = i;
        a.dcFollowX = w.pose.p.x;
        if (w.fov > 0) a.dcFov = w.fov;
        if (byHand) { a.dcAuto = false; if (a.dcEventKind) a.dcEventSkip = a.dcEventKind; }
        a.dcAutoAt = t + kHoldSecs[w.hold & 3];
    };
    if (cmd == DC_MYSAVE) cmd = DC_MYSET + std::min(7, (int)a.warps.size());   // (the button: the next free one)
    if (cmd >= DC_MYSET && cmd < DC_MYSET + 8) {
        const int i = cmd - DC_MYSET;
        Pose cur = a.dcView == 0 ? Pose{Quat::axisAngle({0, 1, 0}, a.dcYaw) * Quat::axisAngle({1, 0, 0}, a.dcPitch), a.dcPos}
                 : a.dcView == kDcMine && a.dcMy >= 0 && a.dcMy < (int)a.warps.size() ? a.dcLast : desktopPreset(a, a.dcView);
        const Vec3 f = cur.q.rotate({0, 0, -1});
        App::WarpCam w;
        w.pose = {Quat::axisAngle({0, 1, 0}, std::atan2(-f.x, -f.z)), cur.p};
        w.pitch = std::asin(clampf(f.y, -1.f, 1.f));
        w.fov = a.dcFov;
        w.hold = a.dcNewHold; w.trans = a.dcNewTrans;
        if (i < (int)a.warps.size()) { w.trig = a.warps[i].trig; w.hold = a.warps[i].hold; w.trans = a.warps[i].trans; a.warps[i] = w; }
        else a.warps.push_back(w);
        const int at = std::min(i, (int)a.warps.size() - 1);
        saveWarps(a);
        LOGI("desktop: my camera %d saved (%.1f %.1f %.1f)", at + 1, w.pose.p.x, w.pose.p.y, w.pose.p.z);
        a.dcView = kDcMine; a.dcMy = at; a.dcGlide = 0; a.dcFollowX = w.pose.p.x;
        a.dcHintUntil = 0;
    }
    if (cmd >= DC_MYGO && cmd < DC_MYGO + 8) showMine(cmd - DC_MYGO, true);
    {
        App::WarpCam* cw = a.dcView == kDcMine && a.dcMy >= 0 && a.dcMy < (int)a.warps.size() ? &a.warps[a.dcMy] : nullptr;
        if (cmd == DC_MYHOLD) { if (cw) cw->hold = (uint8_t)((cw->hold + 1) & 3); else a.dcNewHold = (uint8_t)((a.dcNewHold + 1) & 3); saveWarps(a); }
        if (cmd == DC_MYTRANS) { if (cw) cw->trans ^= 1; else a.dcNewTrans ^= 1; saveWarps(a); }
        if (cmd == DC_MYTRIG && cw) { cw->trig = (uint8_t)((cw->trig + 1) % 3); saveWarps(a); }
        if (cmd == DC_MYCLEAR) { a.warps.clear(); a.dcMy = -1; if (a.dcView == kDcMine) a.dcView = 1; saveWarps(a); LOGI("desktop: my cameras cleared"); }
    }
    if (a.dcView == kDcMine && (a.dcMy < 0 || a.dcMy >= (int)a.warps.size())) a.dcView = 1;   // (cleared, or cleared in the headset)
    if (cmd == DC_HELP) a.dcHelp = !a.dcHelp;
    if (cmd == DC_SELFIE) {   // SELFIE: your webcam over the broadcast (read only on this PC)
        a.selfieOn = !a.selfieOn;
        if (a.selfieOn) { webcam::setRound(a.selfieRound); webcam::start(a.selfieCam); } else webcam::stop();
        saveSettings(a);
        LOGI("desktop: selfie %s", a.selfieOn ? "on" : "off");
    }
    if (cmd == DC_SELFIECORNER) { a.selfieCorner = (a.selfieCorner + 1) % 4; saveSettings(a); }
    if (cmd == DC_SELFIESIZE) { a.selfieSize = (a.selfieSize + 1) % 3; saveSettings(a); }
    if (cmd == DC_SELFIESHAPE) { a.selfieRound = !a.selfieRound; webcam::setRound(a.selfieRound); saveSettings(a); }
    if (cmd == DC_SELFIECAM) {   // the next camera (if there is more than one)
        a.selfieCam = webcam::devices() > 1 ? (a.selfieCam + 1) % webcam::devices() : 0;
        if (a.selfieOn) webcam::start(a.selfieCam);
        saveSettings(a);
    }
    if (cmd == DC_UPDATE) {   // UPDATE NOW: download and check it; once it is ready, the same button restarts into it
        std::string why;
        const int how = updater::how(why);
        const char* v = updateAvailable(a);
        if (!v) MessageBoxA(a.dcPanel, (std::string("This is the newest version (") + kAppVersion + ").").c_str(), "Project Arena", MB_OK | MB_ICONINFORMATION);
        else if (how == 1) MessageBoxA(a.dcPanel, "Steam updates this copy: restart Steam if it has not.", "Project Arena", MB_OK | MB_ICONINFORMATION);
        else if (how == 2) updater::openPage();
        else if (updater::state() == updater::READY) {
            if (MessageBoxA(a.dcPanel, "The update is ready. Close the game, put the new version in place and start it again?\n(Your settings and cameras stay.)",
                            "Project Arena", MB_YESNO | MB_ICONQUESTION) == IDYES && updater::restart()) { saveSettings(a); a.quit = true; }
        } else if (updater::state() == updater::IDLE || updater::state() == updater::FAILED) {
            const std::string q = std::string("Download version ") + v + " now? It is checked and unpacked while you watch; you choose when to restart.";
            if (MessageBoxA(a.dcPanel, q.c_str(), "Project Arena", MB_YESNO | MB_ICONQUESTION) == IDYES) updater::start(v);
        }
    }
    if (cmd == DC_DISCORD) { a.discordOn = !a.discordOn; saveSettings(a); LOGI("desktop: Discord status %s", a.discordOn ? "on" : "off"); }
    if (cmd == DC_GLASS) {   // ARENA GLASS: NORMAL, FAINT (35%), FAINTER (20%), round again; saved on this PC
        a.dcGlass = a.dcGlass > 0.9f ? 0.35f : a.dcGlass > 0.3f ? 0.2f : 1.f;
        saveSettings(a);
        LOGI("desktop: arena glass %.0f%%", a.dcGlass * 100.f);
    }
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
    // AUTO ROTATION: the list made whole for the cameras there are; the streamer's ticks, hold times and order
    if (!a.dcAutoLoaded) loadAutoList(a);
    {
        std::vector<uint8_t> trig;
        for (const App::WarpCam& w : a.warps) trig.push_back(w.trig);
        if (autorot::sync(a.dcAutoList, a.dcAutoCustom, trig)) saveAutoList(a);
        const int sel = a.dcAutoView ? (int)SendMessageW(a.dcAutoView, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED) : -1;
        const int n = (int)a.dcAutoList.size();
        int moved = -1;
        if (a.dcAutoTick >= 0 && a.dcAutoTick < n) {   // (the first change: the list is the streamer's from now on)
            a.dcAutoCustom = true;
            a.dcAutoList[a.dcAutoTick].on = a.dcAutoTickOn != 0;
            saveAutoList(a);
            LOGI("desktop: auto rotation, row %d %s", a.dcAutoTick + 1, a.dcAutoTickOn ? "ticked" : "unticked");
        }
        a.dcAutoTick = -1;
        if (cmd == DC_AUTOHOLD && sel >= 0 && sel < n) {   // yours: its own hold (the headset's too); a preset's: 2, 4, 9, 15 s
            autorot::Item& it = a.dcAutoList[sel];
            if (it.mine) { a.warps[it.id].hold = (uint8_t)((a.warps[it.id].hold + 1) & 3); saveWarps(a); }
            else { it.hold = autorot::nextHold(it.hold); a.dcAutoCustom = true; saveAutoList(a); }
        }
        if ((cmd == DC_AUTOUP || cmd == DC_AUTODOWN) && sel >= 0 && sel < n) {
            const int to = sel + (cmd == DC_AUTOUP ? -1 : 1);
            if (to >= 0 && to < n) {
                std::swap(a.dcAutoList[sel], a.dcAutoList[to]);
                a.dcAutoCustom = true;
                saveAutoList(a);
                moved = to;
            }
        }
        dcAutoRefresh(a, moved);
    }
    if (a.dcAuto && a.watchIp && !a.warps.empty()) {   // AUTO with your cameras: an action camera near a hit takes over
        for (const GameEvent& e : a.game.events) {
            if (e.type != EventType::HitPlayer && e.type != EventType::Strike && e.type != EventType::MatchEnd) continue;
            int best = -1;
            float bd = 1e9f;
            for (int i = 0; i < (int)a.warps.size(); ++i)
                if (a.warps[i].trig == 1 && !(a.dcView == kDcMine && i == a.dcMy) && length(a.warps[i].pose.p - e.pos) < bd) { bd = length(a.warps[i].pose.p - e.pos); best = i; }
            if (best >= 0) showMine(best, false);
        }
    }
    if (a.dcAuto && a.watchIp && t > a.dcAutoAt) {   // ... and goes round the AUTO ROTATION list's ticked ones, each its hold
        const std::vector<autorot::Item>& L = a.dcAutoList;
        int cur = a.dcView == kDcMine ? autorot::rowOf(L, true, a.dcMy) : autorot::rowOf(L, false, a.dcView);
        if (cur < 0) cur = a.dcAutoRow;   // (an action camera, or the free camera: on from the row played last)
        const int r = autorot::next(L, cur);
        if (r < 0) a.dcAutoAt = t + 1.0;   // (nothing ticked: the camera stays)
        else {
            a.dcAutoRow = r;
            if (L[r].mine) showMine(L[r].id, false);
            else { a.dcView = L[r].id; a.dcAutoAt = t + L[r].hold; }
        }
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
    Pose base = desktopPreset(a, a.dcView);
    a.dcEyesOf = -1;
    if ((a.dcView == 6 || a.dcView == 7) && a.watchIp) {   // a player's own eyes (their body hidden; their hands show)
        const int p = a.dcView - 6;
        const Pose h = a.viewIn[p].head;
        if (a.viewInValid[p] && h.p.y > 0.3f) {
            const Pose want{h.q, h.p + h.q.rotate({0, 0, -0.06f})};
            if (!a.dcEyeSet || length(a.dcEye.p - want.p) > 2.f) { a.dcEye = want; a.dcEyeSet = true; }
            a.dcEye.p = lerp(a.dcEye.p, want.p, std::min(1.f, dt * 10.f));
            a.dcEye.q = nlerp(a.dcEye.q, want.q, std::min(1.f, dt * 6.f));
            base = a.dcEye;
            a.dcEyesOf = p;
        }
    } else {
        a.dcEyeSet = false;
    }
    if (a.dcView == kDcMine && a.dcMy >= 0 && a.dcMy < (int)a.warps.size()) {   // one of yours (a follow camera glides along the
        const App::WarpCam& w = a.warps[a.dcMy];                           //  court with the ball, as in the headset)
        base = {w.pose.q * Quat::axisAngle({1, 0, 0}, w.pitch), w.pose.p};
        if (w.trig == 2) {
            Vec3 bp;
            if (liveBall(a, bp)) a.dcFollowX += (clampf(bp.x, -8.f, 8.f) - a.dcFollowX) * (1.f - std::exp(-dt * 1.8f));
            base.p.x = a.dcFollowX;
        }
    }
    if (a.dcGlide > 0) {   // a fade: from where the camera was, eased (0.8 s)
        a.dcGlide = std::max(0.f, a.dcGlide - dt / 0.8f);
        const float k = 1.f - a.dcGlide, e = k * k * (3 - 2 * k);
        base = blendPose(a.dcFrom, base, e);
    }
    const Pose out = eventCamera(a, base, t, dt, false);
    a.dcLast = out;
    a.dcLastOk = true;
    return out;
}

#ifdef _WIN32
// SELFIE: the webcam's latest picture into its texture (a test card for pictures), drawn in its corner of the view
static void drawSelfie(App& a, const Pose& eye, float ty, float tx) {
    if (!a.selfieOn) return;
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    bool fresh = webcam::latest(a.selfieSeen, px, w, h);
    if (a.selfieTest && !a.selfieTex) {   // (a test card: colour bars and a face-sized circle, round-masked like a camera's)
        w = 640; h = 480; px.assign((size_t)w * h * 4, 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t* o = &px[((size_t)y * w + x) * 4];
                const int bar = x * 6 / w;
                static const uint8_t kBars[6][3] = {{230, 230, 230}, {230, 220, 40}, {40, 220, 230}, {40, 200, 60}, {220, 50, 200}, {40, 60, 230}};
                const float d = std::sqrt(float((x - 320) * (x - 320) + (y - 230) * (y - 230)));
                const bool face = d < 110;
                o[0] = face ? 240 : kBars[bar][0]; o[1] = face ? 190 : kBars[bar][1]; o[2] = face ? 150 : kBars[bar][2];
                const float r2 = std::sqrt(float((x - 320) * (x - 320) + (y - 240) * (y - 240)));
                o[3] = !a.selfieRound ? 255 : (uint8_t)(255.f * clampf((240.f - r2) / 1.5f, 0.f, 1.f));
            }
        fresh = true;
    }
    if (fresh && w > 0 && h > 0) {
        if (!a.selfieTex) glGenTextures(1, &a.selfieTex);
        glBindTexture(GL_TEXTURE_2D, a.selfieTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        a.selfieW = w; a.selfieH = h;
    }
    static const float kHeight[3] = {0.34f, 0.48f, 0.64f};   // (of the view's height, -1..1 units)
    const float hh = kHeight[a.selfieSize % 3];
    const float aspect = a.selfieH > 0 ? (float)a.selfieW / a.selfieH : 4.f / 3.f;
    // what shows: round, a circle as tall as the picture; square, the whole picture
    const float visW = (a.selfieRound ? hh : hh * aspect) * ty / tx;    // (in -1..1 units across)
    const bool right = a.selfieCorner & 1, top = a.selfieCorner < 2;
    const float cx = (right ? 1.f : -1.f) * (0.98f - 0.5f * visW);
    const float cy = top ? 0.68f - 0.5f * hh : -0.84f + 0.5f * hh;   // (under the score; above the status line)
    const Vec3 c = eye.transform({cx * tx, cy * ty, -0.999f});
    const std::string ws = webcam::status();
    const float cyan[4] = {0.3f, 0.85f, 1.0f, 1}, grey[4] = {0.65f, 0.7f, 0.78f, 1};
    if (a.selfieTex && (ws.empty() || a.selfieTest)) {
        const float white[4] = {1, 1, 1, 1};
        a.gfx.drawTexturedPanel(Mat4::fromPose({eye.q, c}, {hh * ty * aspect, hh * ty, 1}), a.selfieTex, white, false);
        const float ring[4] = {0.12f, 0.36f, 0.48f, 1};
        if (a.selfieRound) a.gfx.drawSprite(c, hh * ty * 1.08f, ring, eye.p, 1);   // (a soft ring round it)
        else {
            const float ew = hh * ty * aspect, eh = hh * ty, t = 0.006f * ty;
            const float edge[4] = {cyan[0], cyan[1], cyan[2], 0.9f};
            a.gfx.drawPanel(Mat4::fromPose({eye.q, c + eye.q.rotate({0, 0.5f * eh, 0.0005f})}, {ew + t, t, 1}), edge);
            a.gfx.drawPanel(Mat4::fromPose({eye.q, c + eye.q.rotate({0, -0.5f * eh, 0.0005f})}, {ew + t, t, 1}), edge);
            a.gfx.drawPanel(Mat4::fromPose({eye.q, c + eye.q.rotate({0.5f * ew, 0, 0.0005f})}, {t, eh + t, 1}), edge);
            a.gfx.drawPanel(Mat4::fromPose({eye.q, c + eye.q.rotate({-0.5f * ew, 0, 0.0005f})}, {t, eh + t, 1}), edge);
        }
    } else {   // no picture yet: why (STARTING, NO CAMERA FOUND, CAMERA IN USE OR BLOCKED ...)
        a.gfx.drawText("SELFIE: " + (ws.empty() ? std::string("STARTING") : ws), {eye.q, c}, 0.03f * ty, grey);
    }
}
#endif

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
    const float gold4[4] = {1.0f, 0.85f, 0.3f, 1};
    std::vector<SpecMatch>& ms = a.net.matches();
    const SpecMatch* w = nullptr;
    for (const SpecMatch& m : ms) if (m.ip == a.watchIp) w = &m;
    if (w) {
        const Game& g = a.game;
        auto who = [&](int p) -> std::string {
            const uint32_t id = a.watchRoom.slot[p];
            if (a.watchRoom.isBot(p) || (p == 1 && a.watchRoom.bot)) return "BOT";
            if (const char* n = a.watchRoom.nameOf(id)) return n;
            return g.np == 4 ? std::string("PLAYER ") + char('1' + p) : std::string(sideName(a, p));
        };
        if (g.np == 4) {   // FOUR CORNERS: four names and scores in their colours, or the two teams
            static const char* kTeamName[2][2] = {{"BLUE TEAM", "ORANGE TEAM"}, {"THE THREE", "THE ONE"}};
            panel(0, 0.86f, 1.70f, 0.26f);
            if ((g.tun.teamMode == TEAMS_FFA || g.octagon())) {
                for (int p = 0; p < 4; ++p) {
                    const float* pc = playerRGB(a, p);
                    const float c[4] = {0.6f * pc[0] + 0.4f, 0.6f * pc[1] + 0.4f, 0.6f * pc[2] + 0.4f, 1};
                    const float x = -0.60f + 0.40f * p;
                    txt(who(p), x, 0.92f, 0.045f, c);
                    snprintf(buf, sizeof buf, "%d", g.pl[p].score);
                    txt(buf, x, 0.84f, 0.065f, white);
                }
            } else {
                const bool one = g.tun.teamMode == TEAMS_3V1;
                for (int t = 0; t < 2; ++t) {
                    const float* tc = teamRGB(a, t);
                    const float c[4] = {0.6f * tc[0] + 0.4f, 0.6f * tc[1] + 0.4f, 0.6f * tc[2] + 0.4f, 1};
                    const float x = t ? 0.42f : -0.42f;
                    std::string names;
                    for (int p = 0; p < 4; ++p) if (g.team(p) == t) names += (names.empty() ? "" : "  ") + who(p);
                    txt(kTeamName[one][t], x, 0.93f, 0.04f, c);
                    txt(names, x, 0.875f, 0.026f, c);
                }
                snprintf(buf, sizeof buf, "%d  -  %d", g.teamScore(0), g.teamScore(1));
                txt(buf, 0, 0.90f, 0.08f, white);
            }
            txt(rulesLine(g.tun) + "  -  " + clockLine(g), 0, 0.765f, 0.03f, grey);
            if (g.match == MatchState::MatchSummary && g.matchWinner >= 0) {   // the winner (FREE-FOR-ALL) or the winning team
                const int wt = g.matchWinner;
                const float* tc = teamRGB(a, wt);
                const float c[4] = {0.6f * tc[0] + 0.4f, 0.6f * tc[1] + 0.4f, 0.6f * tc[2] + 0.4f, 1};
                const std::string nm = (g.tun.teamMode == TEAMS_FFA || g.octagon()) ? who(wt) : kTeamName[g.tun.teamMode == TEAMS_3V1][wt & 1];
                panel(0, -0.55f, 1.40f, 0.40f);
                txt(nm, 0, -0.47f, 0.12f, c);
                txt((g.tun.teamMode == TEAMS_FFA || g.octagon()) ? "WINS!" : "WIN!", 0, -0.62f, 0.08f, gold4);
            }
        } else {
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
        }
        snprintf(buf, sizeof buf, "%s%s", w->name[0] ? w->name : "LIVE MATCH", ms.size() > 1 ? "   (TAB: NEXT MATCH)" : "");
        txt(buf, -0.62f, -0.93f, 0.028f, grey);
        snprintf(buf, sizeof buf, "%s%s%s%s", dcViewName(a).c_str(), a.dcAuto ? "  (AUTO)" : "", a.viewLayer ? "   " : "",
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
        if (a.nationsOn && g.np == 2 && !dcVsScreen(a) && (g.match == MatchState::Meet || g.match == MatchState::WaitingForPlayers || a.bumpFlash > 0)) {
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
        if (g.np == 2 && g.match == MatchState::MatchSummary && g.matchWinner >= 0 && g.matchWinner < 2 && !dcWinSplit(a)) {   // the win splash
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
            const updater::State us = updater::state();
            if (us == updater::DOWNLOADING) snprintf(buf, sizeof buf, "UPDATE %s: DOWNLOADING %d%%", v, (int)(updater::progress() * 100));
            else if (us == updater::READY) snprintf(buf, sizeof buf, "UPDATE %s READY: SETUP > RESTART TO UPDATE", v);
            else if (us == updater::FAILED) snprintf(buf, sizeof buf, "UPDATE %s: %s", v, updater::error().c_str());
            else snprintf(buf, sizeof buf, "UPDATE AVAILABLE: %s  -  SETUP > UPDATE NOW", v);
            txt(buf, 0, 0.72f, 0.03f, orange);
        }
        {
            snprintf(buf, sizeof buf, "VERSION %s", kAppVersion);
            txt(buf, 0.85f, -0.85f, 0.024f, grey);
        }
        if (!a.net.online()) txt("ON YOUR HOME NETWORK", 0, 0.79f, 0.035f, grey);
        else if (!a.net.onlineReady()) txt("CONNECTING TO THE COMMUNITY SERVER...", 0, 0.79f, 0.035f, grey);
        else if (ms.empty()) txt("NO LIVE MATCHES YET  -  THEY APPEAR HERE WHEN SOMEONE PLAYS", 0, 0.79f, 0.035f, grey);
        else {   // (0.3.16: a room nobody plays in is WAITING, not a live match)
            int live = 0;
            for (const SpecMatch& m : ms) live += a.net.live(m) ? 1 : 0;
            const int waiting = (int)ms.size() - live;
            if (live && waiting)
                snprintf(buf, sizeof buf, "%d LIVE MATCH%s, %d ROOM%s WAITING  -  TAB TO WATCH", live, live == 1 ? "" : "ES", waiting, waiting == 1 ? "" : "S");
            else if (live) snprintf(buf, sizeof buf, "%d LIVE MATCH%s  -  TAB TO WATCH", live, live == 1 ? "" : "ES");
            else snprintf(buf, sizeof buf, "%d ROOM%s WAITING FOR PLAYERS  -  TAB TO WATCH", waiting, waiting == 1 ? "" : "S");
            txt(buf, 0, 0.79f, 0.035f, white);
        }
    }
#ifdef _WIN32
    drawSelfie(a, eye, ty, tx);
#endif
    if (!a.dcHelp && nowSeconds() < a.dcHintUntil) txt("F1: CONTROLS", 0.82f, 0.93f, 0.028f, grey);
    if (a.dcHelp) {
        panel(0, -0.80f, 1.96f, 0.13f);
        txt("1-7 CAMERAS   CTRL+1-8 SAVE   SHIFT+1-8 MINE   A AUTO   F FREE (RMB + WASD / Q E)   WHEEL ZOOM   V LAYERS   TAB NEXT   H HUB   F11 FULL   F2 HIDE",
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
        // the player as the room last listed them: one who left while this shows stays, still, with their name
        // (no room list yet: everyone is here)
        bool listed = a.watchRoom.botMask != 0 || !a.watchRoom.members.empty();
        for (int k = 0; k < kMaxPlayers; ++k) listed |= a.watchRoom.slot[k] != 0;
        const bool here = !listed || a.watchRoom.slot[p] != 0 || a.watchRoom.isBot(p);
        std::string nm = sideName(a, p);
        if (const char* n = a.watchRoom.nameOf(a.watchRoom.slot[p])) nm = n;
        else if (p == 1 && a.watchRoom.bot) nm = "BOT";
        if (first || here) { a.dcHeldIn[p] = a.viewIn[p]; a.dcHeldName[p] = nm; }
        const PlayerInput liveIn = a.viewIn[p];
        if (!here) { a.viewIn[p] = a.dcHeldIn[p]; nm = a.dcHeldName[p]; }
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
        a.viewIn[p] = liveIn;
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
        char b[512];
        static const char* kHoldName[4] = {"2 s", "4 s", "8 s", "15 s"};
        static const char* kTrigName[3] = {"in turn", "action", "follow"};
        char mine[96] = "none saved yet";
        if (!a.warps.empty()) {
            const App::WarpCam* cw = a.dcView == kDcMine && a.dcMy >= 0 && a.dcMy < (int)a.warps.size() ? &a.warps[a.dcMy] : nullptr;
            if (cw) snprintf(mine, sizeof mine, "%d saved; this one %s, %s, %s", (int)a.warps.size(), kHoldName[cw->hold & 3],
                             cw->trans ? "fade" : "cut", kTrigName[std::min(2, (int)cw->trig)]);
            else snprintf(mine, sizeof mine, "%d saved", (int)a.warps.size());
        }
        static const char* kCorner[4] = {"top left", "top right", "bottom left", "bottom right"};
        static const char* kSize[3] = {"small", "medium", "large"};
        char selfie[96];
        const std::string ws = webcam::status();
        if (!a.selfieOn) snprintf(selfie, sizeof selfie, "off (C)");
        else snprintf(selfie, sizeof selfie, "%s, %s, %s%s%s", kCorner[a.selfieCorner & 3], kSize[a.selfieSize % 3], a.selfieRound ? "round" : "square",
                      ws.empty() ? "" : " - ", ws.c_str());
        const char* glass = a.dcGlass > 0.9f ? "normal" : a.dcGlass > 0.3f ? "faint (35%)" : "fainter (20%)";
        snprintf(b, sizeof b, "Now: %s\nCamera: %s%s, layers: %s%s\nMy cameras: %s\nWin screen: %s\nArena glass: %s\nDiscord status: %s\nSelfie: %s\nVersion: %s", w ? (w->name[0] ? w->name : "live match") : "the spectator hub",
                 w ? dcViewName(a).c_str() : "the hub's", a.dcAuto ? " (auto)" : "", kDcLayerName[a.viewLayer & 3],
                 a.viewLayer == 3 ? " (transparent)" : "", mine, kDcWinName[a.dcWinStyle % 3], glass, a.discordOn ? "on" : "off", selfie, kAppVersion);
        SetWindowTextA(a.dcStatus, b);
        for (int i = 0; i < 8; ++i)   // (my cameras' buttons: only the saved ones can be clicked)
            if (HWND c = GetDlgItem(a.dcPanel, DC_MYGO + i)) EnableWindow(c, i < (int)a.warps.size());
    }
    const float dt = a.lastTime > 0 ? std::min(0.1f, float(t - a.lastTime)) : 0.f;
    a.lastTime = t;
    if (a.net.mode() != Net::Mode::Spectate) { a.net.spectate(); a.watchIp = 0; }   // always watching
    const Pose eye = desktopControls(a, t, dt);
    const int L = a.net.localIndex();
    PlayerInput in[kMaxPlayers];
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
    if (a.selfieOn) { webcam::setRound(a.selfieRound); webcam::start(a.selfieCam); }   // (left on last time)
    while (!a.quit) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        if (a.quit) break;
        desktopFrame(a);
        SwapBuffers(a.hdc);
    }
    webcam::stop();
    discord::stop();   // (review 2026-11-02: left running, its thread aborted the game as it closed)
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
        else if (!strcmp(argv[i], "--buttons") && i + 1 < argc) view.buttons = argv[++i];
        else if (!strcmp(argv[i], "--ctlkind") && i + 1 < argc) view.ctlKind = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scene") && i + 1 < argc) view.scene = argv[++i];
        else if (!strcmp(argv[i], "--film") && i + 1 < argc) view.filmSeconds = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--lobby")) view.lobby = true;
        else if (!strcmp(argv[i], "--tune") && i + 1 < argc) view.tunePart = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tunerow") && i + 1 < argc) view.tuneRow = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--suit") && i + 1 < argc) view.suit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hair") && i + 1 < argc) view.hair = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--swingtest")) view.swingTest = true;
        else if (!strcmp(argv[i], "--handsout")) view.handsOut = true;   // (blue's hands straight out, palms down)
        else if (!strcmp(argv[i], "--handstouch") && i + 1 < argc) view.handsTouch = atoi(argv[++i]);   // (HANDS APART)
        else if (!strcmp(argv[i], "--touchgap") && i + 1 < argc) view.touchGap = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--noapart")) view.noApart = true;
        else if (!strcmp(argv[i], "--noelbow")) view.noElbow = true;
        else if (!strcmp(argv[i], "--notwist")) view.noTwist = true;
        else if (!strcmp(argv[i], "--hitboxes")) view.hitboxes = true;
        else if (!strcmp(argv[i], "--ballfeel") && i + 1 < argc) view.ballFeel = argv[++i];
        else if (!strcmp(argv[i], "--punch") && i + 1 < argc) view.punchT = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--swish") && i + 3 < argc) {
            view.swishOn = true;
            for (int k = 0; k < 3; ++k) view.swish[k] = (float)atof(argv[++i]);
        }
        else if (!strcmp(argv[i], "--armscale") && i + 1 < argc) view.armScale = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--armsup") && i + 1 < argc) view.armsUp = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--dcshot") && i + 1 < argc) view.dcshot = argv[++i];
        else if (!strcmp(argv[i], "--glass") && i + 1 < argc) view.glass = (float)atof(argv[++i]);   // ARENA GLASS in the picture
        else if (!strcmp(argv[i], "--queue") && i + 1 < argc) view.queue = atoi(argv[++i]);   // a waiting spot's queue screen
        else if (!strcmp(argv[i], "--foul") && i + 1 < argc) view.foul = atoi(argv[++i]);     // the FOUL LINE option
        else if (!strcmp(argv[i], "--selfie") && i + 1 < argc) view.selfie = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--headselfie")) view.headSelfie = true;
        else if (!strcmp(argv[i], "--layer") && i + 1 < argc) view.layer = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--keycol") && i + 1 < argc) view.keyCol = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--nations") && i + 2 < argc) { view.nations[0] = argv[++i]; view.nations[1] = argv[++i]; }
        else if (!strcmp(argv[i], "--opts") && i + 1 < argc) view.opts = argv[++i];
        else if (!strcmp(argv[i], "--record") && i + 2 < argc) { view.recordDir = argv[++i]; view.recordSeconds = (float)atof(argv[++i]); }
        else if (!strcmp(argv[i], "--size") && i + 2 < argc) { view.w = atoi(argv[++i]); view.h = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--recordstep") && i + 1 < argc) view.recordStep = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--recordbot") && i + 1 < argc) view.recordBot = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--recordmeet")) view.recordMeet = true;
        else if (!strcmp(argv[i], "--notrail")) view.noTrail = true;
        else if (!strcmp(argv[i], "--recordaim") && i + 7 < argc) {
            view.aimPlayer = atoi(argv[++i]);
            view.aimFrom = {(float)atof(argv[i + 1]), (float)atof(argv[i + 2]), (float)atof(argv[i + 3])};
            view.aimAt = {(float)atof(argv[i + 4]), (float)atof(argv[i + 5]), (float)atof(argv[i + 6])};
            i += 6;
        }
        else if (!strcmp(argv[i], "--recordframes") && i + 1 < argc)
            for (const char* c = argv[++i]; *c; ) { view.recordFrames.push_back(atoi(c)); while (*c && *c != ',') ++c; if (*c) ++c; }
        else if (!strcmp(argv[i], "--mockdark") && i + 1 < argc) view.mockDark = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--contact")) view.contact = true;
        else if (!strcmp(argv[i], "--depth")) view.depthOut = true;
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
        else if (!strcmp(argv[i], "--marks") && i + 1 < argc) view.marks = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--face") && i + 2 < argc) { view.face = (atoi(argv[i + 1]) & 1) | (atoi(argv[i + 2]) << 1); i += 2; }
        else if (!strcmp(argv[i], "--passthrough") && i + 1 < argc) { view.passthrough = true; view.ptMode = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--ptroom") && i + 1 < argc) view.ptRoom = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--bug-report") && i + 2 < argc) { bugServer = argv[i + 1]; bugNoteArg = argv[i + 2]; i += 2; }
        else if (!strcmp(argv[i], "--player-report") && i + 2 < argc) { bugServer = argv[i + 1]; bugNoteArg = argv[i + 2]; i += 2; playerReport = true; }
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
    // no headset (or --spectate): the desktop spectator window instead of an error
    if (!initGraphics(a)) {
        if (!noDialog) MessageBoxA(a.hwnd, "Project Arena needs an OpenGL 4.3 graphics driver.\n\nDetails: %APPDATA%\\ProjectArena\\projectarena_log.txt",
                                   "Project Arena", MB_ICONERROR);
        return 1;
    }
    const bool vr = !spectateArg && initInstance(a) && initActions(a) && initSession(a);
    if (!initContent(a)) { LOGE("initialisation failed"); return 1; }
    a.serverOverride = serverArg ? serverArg : "";
    discord::start();   // (only here: never for pictures, probes or reports)
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
    discord::stop();
    return 0;
}
#endif
