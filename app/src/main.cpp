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
#include <map>
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
    bool onlinePref = false;
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
    // open art set only (absent in the original set): glossy floor + its mirrored light sources, additive glow layers
    Model floor, floorReflect, courtFx, envFx, courtPulse;
    double startTime = 0;
    struct Trail {
        Vec3 core[48], helixA[48], helixB[48];
        int n = 0;
        float spinAngle = 0;
        float strikeFlash = 0;
        Vec3 burstPos; float burst = 0;   // dissolve burst at the back wall
    } trail[2];
    XrAction thumbstick = XR_NULL_HANDLE, thumbstickL = XR_NULL_HANDLE;
    bool stickLatched = false, stickLatchedY = false, stickLatchedL = false;
    // live gauntlet tuning (applied to the tool frame): pitch about grip X, forward / up offsets
    float tunePitchDeg = -25.f, tuneFwd = 0, tuneUp = 0;   // gauntlet pitch: -25 = fist along the forearm (owner)
    Game game;
    Audio audio;
    Net net;
    double lastTime = 0;
    float hitFlash[2] = {0, 0};
    float bumpFlash = 0;            // FIST BUMP! (1 -> 0 over a second)
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
    GLuint patternTex[8] = {};
    AvatarDesc myAvatar, botAvatar;
    PlayerInput localIn;           // the latest local input (the builder's mirror runs while the game is frozen)
    // avatar builder: a mirror 0.85 m ahead (the reflection copies you) and a pointer-driven panel beside it
    bool builderOpen = false, prevBuilderTrig = true;
    Pose builderPose;
    Vec3 mirrorPoint, mirrorNormal;
    int builderHover = -1, presetIdx = 0;
    Vec3 builderHit;
    bool builderHitValid = false;
    PlayerInput viewIn[2];         // what is drawn for each player (remote ones smoothed)
    bool viewInValid[2] = {false, false};
    bool titleOpen = false;        // title screen (game start): the menu shows PLAY / MULTIPLAYER / ...
    bool settingsOpen = false;
    int pointerHand = 1;           // hand that points at menus (settings: right / left)
    int fxLevel = 0;               // hit effects: 0 full, 1 soft, 2 low (photosensitivity)
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
    std::string serverAddr;        // community relay "host[:port]" (settings.cfg server=...)         // the menu shows a keyboard for the lobby name
    std::string nameBuf;
    bool devMode = false;
    // voice chat: push-to-talk (left grip) by default, open mic, or off; mutes by who is talking
    int voiceMode = 0;             // 0 push-to-talk, 1 open mic, 2 off
    bool muteSpectators = false, mutePlayers = false, muteAll = false;
    bool voicePage = false;        // settings sub-page
    bool ptt = false, talking = false, micAsked = false, micDenied = false;
    float micGain = 1.0f;          // automatic microphone gain
    int voiceSent = 0, voiceRecv = 0;
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

// RECORD_AUDIO is a runtime permission: ask once, the headset shows its own prompt
static bool micPermission(bool request) {
    if (!g_vm || !g_activity) return false;
    JNIEnv* env = nullptr;
    g_vm->AttachCurrentThread(&env, nullptr);
    jclass cls = env->GetObjectClass(g_activity);
    jstring perm = env->NewStringUTF("android.permission.RECORD_AUDIO");
    jmethodID check = env->GetMethodID(cls, "checkSelfPermission", "(Ljava/lang/String;)I");
    jint r = check ? env->CallIntMethod(g_activity, check, perm) : -1;
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (r == 0) return true;
    if (request) {
        jclass strCls = env->FindClass("java/lang/String");
        jobjectArray arr = env->NewObjectArray(1, strCls, perm);
        jmethodID req = env->GetMethodID(cls, "requestPermissions", "([Ljava/lang/String;I)V");
        if (req) env->CallVoidMethod(g_activity, req, arr, 1);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    return false;
}
#else
bool netAcquireMulticastLock() { return false; }   // Windows delivers LAN broadcasts without one
static bool micPermission(bool) { return true; }   // Windows asks for microphone access itself
#endif

static double nowSeconds() {
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
    pfd.iLayerType = PFD_MAIN_PLANE;
    int pf = ChoosePixelFormat(a.hdc, &pfd);
    if (!pf || !SetPixelFormat(a.hdc, pf, &pfd)) { LOGE("SetPixelFormat failed"); return false; }
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
        {a.menu, P(L, "b/click")}, {a.buttonY, P(L, "trackpad/force")},
        {a.thumbClick, P(L, "thumbstick/click")}, {a.thumbClick, P(R, "thumbstick/click")},
        {a.thumbstick, P(R, "thumbstick")}, {a.thumbstickL, P(L, "thumbstick")}}));
    suggest("/interaction_profiles/htc/vive_controller", with({
        {a.trigger, P(L, "trigger/value")}, {a.trigger, P(R, "trigger/value")}, {a.squeeze, P(L, "squeeze/click")}, {a.squeeze, P(R, "squeeze/click")},
        {a.buttonA, P(R, "trackpad/click")}, {a.buttonX, P(L, "trackpad/click")}, {a.buttonA, P(L, "trackpad/click")},
        {a.buttonB, P(R, "menu/click")}, {a.menu, P(L, "menu/click")},
        {a.thumbstick, P(R, "trackpad")}, {a.thumbstickL, P(L, "trackpad")}}));
    suggest("/interaction_profiles/microsoft/motion_controller", with({
        {a.trigger, P(L, "trigger/value")}, {a.trigger, P(R, "trigger/value")}, {a.squeeze, P(L, "squeeze/click")}, {a.squeeze, P(R, "squeeze/click")},
        {a.buttonA, P(R, "trackpad/click")}, {a.buttonX, P(L, "trackpad/click")}, {a.buttonA, P(L, "trackpad/click")},
        {a.buttonB, P(R, "menu/click")}, {a.menu, P(L, "menu/click")},
        {a.thumbClick, P(L, "thumbstick/click")}, {a.thumbClick, P(R, "thumbstick/click")},
        {a.thumbstick, P(R, "thumbstick")}, {a.thumbstickL, P(L, "thumbstick")}}));
    suggest("/interaction_profiles/khr/simple_controller", with({
        {a.trigger, P(L, "select/click")}, {a.trigger, P(R, "select/click")}, {a.menu, P(L, "menu/click")}, {a.buttonB, P(R, "menu/click")}}));
    return true;
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
    const uint8_t* d = file.data();
    uint32_t n; std::memcpy(&n, d + 4, 4);
    const uint8_t* p = d + 8;
    for (uint32_t i = 0; i < n; ++i, p += 40) {
        float v[9]; uint32_t tags;
        std::memcpy(v, p, 36); std::memcpy(&tags, p + 36, 4);
        CollisionTri t{{v[0], v[1], v[2]}, {v[3], v[4], v[5]}, {v[6], v[7], v[8]}, {}, tags};
        t.n = normalize(cross(t.b - t.a, t.c - t.a));
        out.push_back(t);
    }
    LOGI("collision: %u tris", n);
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

static bool initContent(App& a) {
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
    a.gfx.loadModel("head", a.head);
    a.gfx.loadModel("torso", a.torso);
    if (assetExists("floor.mdl")) {
        a.gfx.loadModel("floor", a.floor);
        a.gfx.loadModel("floor_reflect", a.floorReflect);
        a.gfx.loadModel("court_fx", a.courtFx);
        a.gfx.loadModel("env_fx", a.envFx);
        a.gfx.loadModel("court_pulse", a.courtPulse);
    }
    a.startTime = nowSeconds();
    a.gfx.loadModel("strikezone", a.strikezone);
    if (assetExists("av_bhead_m.mdl")) {
        char n[32];
        auto part = [&](const char* name) { Model m; if (a.gfx.loadModel(name, m)) a.avModels[name] = m; };
        for (const char bk : {'m', 'f'}) {   // the skinned MPFB bodies (tools/build_body_parts.py)
            snprintf(n, sizeof n, "av_bhead_%c", bk); part(n);
            snprintf(n, sizeof n, "av_wfade_%c", bk); part(n);
            for (int k = 0; k < 10; ++k) { snprintf(n, sizeof n, "av_top_%d_%c", k, bk); part(n); }
            for (int k = 1; k <= 3; ++k) { snprintf(n, sizeof n, "av_bottom_%d_%c", k, bk); part(n); }
            // head parts fitted to this body's head (tools/blender/make_avatar.py --head)
            for (int k = 1; k <= 8; ++k) { snprintf(n, sizeof n, "av_hair_%d_%c", k, bk); part(n); }
            for (int k = 1; k <= 7; ++k) { snprintf(n, sizeof n, "av_visor_%d_%c", k, bk); part(n); }
            for (int k = 1; k <= 6; ++k) { snprintf(n, sizeof n, "av_gear_%d_%c", k, bk); part(n); }
            for (int k = 1; k <= 5; ++k) { snprintf(n, sizeof n, "av_mask_%d_%c", k, bk); part(n); }
        }
        for (int k = 1; k < 8; ++k) if (const char* t = avatar::patternTexture(k)) a.patternTex[k] = a.gfx.texture(t, false);
        LOGI("avatars: %d part models", (int)a.avModels.size());
    }
    // VisualTransform (1,-0.5,0) pitch 5; ShieldTransform (20,0,10) roll 180 scale 10;
    // KnuckleGuardTransform (10.5,0,0)
    g_lancerLocal = ueLocalToGrip({1, -0.5f, 0}, {5, 0, 0}, {1, 1, 1});
    g_shieldLocal = ueLocalToGrip({20, 0, 10}, {0, 0, 180}, {10, 10, 10});
    g_knuckleLocal = ueLocalToGrip({1.5f, 0, 0}, {0, 0, 0}, {1, 1, 1});   // KnuckleGuardTransform (10.5) + TempMesh (-9)
    g_plateLocal = ueLocalToGrip({0, 0, 0}, {0, 0, 180}, {10, 10, 10});   // Shield_01 plate centred, normal = grip Y
    a.game.setArena(loadCollision());
    a.game.reset(true);
    // team 0 = (0.1,0.7,1.0), team 1 = (1.0,0.4,0.1)
    float c0[4] = {0.1f, 0.7f, 1.0f, 1}, c1[4] = {1.0f, 0.4f, 0.1f, 1};
    a.gfx.setTeamColors(c0, c1, 1.0f, 1.0f);
    a.audio.init();
    return ok;
}

// ------------------------------------------------------------------ frame
static void openMenu(App& a, const PlayerInput& in);
static void updateMenu(App& a, PlayerInput& in);
static void openBuilder(App& a, const PlayerInput& in);
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
static const char* kAppVersion = "0.3.1";
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
// holding area outside the tank (waiting players): four spots along the -Z side, facing the glass
static Vec3 holdingSpot(int spot) { return {-4.5f + 3.0f * (spot & 3), 0, -4.2f}; }

static void viewTarget(const App& a, int local, Vec3& pos, Vec3& fwd) {
    if (a.net.localWaiting()) {
        pos = holdingSpot(a.net.localSpot());
        fwd = {0, 0, 1};
    } else if (a.spectating) {
        if (a.watchIp) { pos = {0, 1.0f, 5.4f}; fwd = {0, 0, -1}; }
        else { pos = {0, 0, 0}; fwd = {1, 0, 0}; }
    } else if (a.game.match == MatchState::Meet && a.net.inRoom()) {   // face to face in the centre (fist bump start)
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

// Watching a match: left stick flies (head-relative), right stick turns (snap 30 deg) and rises / sinks;
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
    if (!a.turnLatch && std::fabs(r.x) > 0.75f) {
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
                     : (a.game.match == MatchState::Meet && a.net.inRoom()) ? 2u : 0u;   // the centre meeting, then your end
        if (key != a.viewKey) { a.viewKey = key; a.recentered = false; }
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
            Quat tq = in.hand[h].grip.q * Quat::axisAngle({1, 0, 0}, deg2rad(a.tunePitchDeg));
            in.hand[h].grip = {tq, in.hand[h].grip.p + tq.rotate({0, a.tuneUp, -a.tuneFwd})};
        }
        if (vel.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT)
            in.hand[h].angVel = a.trackToWorld.q.rotate({vel.angularVelocity.x, vel.angularVelocity.y, vel.angularVelocity.z});
        in.hand[h].trigger = getFloat(a, a.trigger, a.handPath[h]);
        in.hand[h].squeeze = getFloat(a, a.squeeze, a.handPath[h]);
    }
    in.buttonA = getBool(a, a.buttonA);
    in.avatar = a.myAvatar;
    a.ptt = in.hand[0].squeeze > 0.6f;
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
        else if (a.menuOpen) { a.menuOpen = false; a.nameEdit = false; a.helpOpen = false; a.settingsOpen = false; a.titleOpen = false; LOGI("menu: closed"); }
        else openMenu(a, in);
    }
    a.prevMenu = menu;
    if (a.menuOpen && a.devMode) {   // gauntlet / shield tuning with the thumbsticks, only while the menu is open
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a.thumbstick;
        XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
        xrGetActionStateVector2f(a.session, &gi, &st);
        float x = st.isActive ? st.currentState.x : 0.f;
        float y = st.isActive ? st.currentState.y : 0.f;
        if (!a.stickLatchedY && std::fabs(y) > 0.8f) {
            a.stickLatchedY = true;
            a.tunePitchDeg += y > 0 ? 5.f : -5.f;
            LOGI("tune: pitch %.0f deg fwd %.2f up %.2f", a.tunePitchDeg, a.tuneFwd, a.tuneUp);
            saveSettings(a);
        } else if (std::fabs(y) < 0.3f) {
            a.stickLatchedY = false;
        }
        if (!a.stickLatched && std::fabs(x) > 0.8f) {
            a.stickLatched = true;
            a.game.tun.shieldPlacement = (a.game.tun.shieldPlacement + (x > 0 ? 1 : 3)) & 3;
            LOGI("shield placement -> %d", a.game.tun.shieldPlacement);
            saveSettings(a);
        } else if (std::fabs(x) < 0.3f) {
            a.stickLatched = false;
        }
        XrActionStateGetInfo gl{XR_TYPE_ACTION_STATE_GET_INFO};
        gl.action = a.thumbstickL;
        XrActionStateVector2f sl{XR_TYPE_ACTION_STATE_VECTOR2F};
        xrGetActionStateVector2f(a.session, &gl, &sl);
        float lx = sl.isActive ? sl.currentState.x : 0.f, ly = sl.isActive ? sl.currentState.y : 0.f;
        if (!a.stickLatchedL && (std::fabs(lx) > 0.8f || std::fabs(ly) > 0.8f)) {
            a.stickLatchedL = true;
            if (std::fabs(ly) > std::fabs(lx)) a.tuneFwd += ly > 0 ? 0.01f : -0.01f;
            else a.tuneUp += lx > 0 ? 0.01f : -0.01f;
            LOGI("tune: pitch %.0f deg fwd %.2f up %.2f", a.tunePitchDeg, a.tuneFwd, a.tuneUp);
            saveSettings(a);
        } else if (std::fabs(lx) < 0.3f && std::fabs(ly) < 0.3f) {
            a.stickLatchedL = false;
        }
    }
    if (a.lanTriggerPoll == 0 && dataDir(a)) {   // adb test hook, like lan_toggle
        std::string f = std::string(dataDir(a)) + "/menu_toggle";
        if (fileExists(f)) {
            fileRemove(f);
            if (a.menuOpen) a.menuOpen = false; else openMenu(a, in);
        }
    }
    updateMenu(a, in);
    updateBuilder(a, in);
    updateDirector(a, in);
    return in;
}

// ------------------------------------------------------------------ lobby menu
enum class MenuAct { None, MainMenu, Nav, Bug, BugCat, BugNote, BugSend, BugCancel, RulesPick, RulesBack, Report, ReportWho, ReportWhy, ReportNote, ReportMute,
                     ReportSend, ReportBack, Forfeit, Experimental, XpBack, XpServer, XpConnect, Host, Leave, Rules, Close, Join, Spectate, Watch, Hub, Avatar, Name, Key, KeyDel, KeyDone, Help, HelpDone, Play, Multi, Settings, Back, VolDown, VolUp, Hand, Fx, Online, Server, ServerDefault, ClearWarps, Queue, HideOutside, VoicePage, VoiceMode, MuteSpec, MutePlayers, MuteAll, Director, JoinRoom, BotFill };
struct MenuItem {
    std::string label;
    float x, y, w, h;
    MenuAct act;
    uint32_t ip;
    bool enabled;
};

static void haptic(App& a, int hand, float amp, float seconds);

// side menu (left of the panel): the areas of the game, the current one lit
enum { NAV_HOME, NAV_PRACTICE, NAV_MULTI, NAV_SPECTATE, NAV_AVATAR, NAV_SETTINGS, NAV_REPORT, NAV_BUG, NAV_HELP, kNavCount };
static const char* kNavLabels[kNavCount] = {"MAIN MENU", "PRACTICE", "MULTIPLAYER", "SPECTATE", "AVATAR", "SETTINGS",
                                            "REPORT PLAYER", "REPORT BUG", "HOW TO PLAY"};
static const float kNavX = -0.67f, kNavTop = 0.29f, kNavStep = 0.088f;

static int navCurrent(const App& a) {
    if (a.titleOpen || a.xpOpen) return NAV_HOME;
    if (a.rulesOpen) return a.rulesForHost ? NAV_MULTI : NAV_PRACTICE;
    if (a.settingsOpen) return NAV_SETTINGS;
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
            v.push_back({kNavLabels[i], kNavX, kNavTop - kNavStep * i, 0.24f, 0.068f, MenuAct::Nav, (uint32_t)i, on});
        }
    }
    return v;
}

static std::vector<MenuItem> menuItemsBase(App& a) {
    std::vector<MenuItem> v;
    if (a.bugOpen && !a.nameEdit) {
        for (int i = 0; i < 6; ++i)
            v.push_back({kBugCats[i], (i % 2) ? 0.21f : -0.21f, 0.13f - 0.085f * (i / 2), 0.40f, 0.07f, MenuAct::BugCat, (uint32_t)i, true});
        std::string note = a.bugNote.empty() ? std::string("ADD A NOTE (OPTIONAL)") : "NOTE: " + a.bugNote;
        if (note.size() > 40) note = note.substr(0, 39) + ".";
        v.push_back({note, 0, -0.145f, 0.82f, 0.07f, MenuAct::BugNote, 0, true});
        v.push_back({a.bugSending ? "SENDING..." : "SEND REPORT", 0.19f, -0.25f, 0.36f, 0.08f, MenuAct::BugSend, 0, a.bugCat >= 0 && !a.bugSending});
        v.push_back({"CANCEL", -0.19f, -0.25f, 0.36f, 0.08f, MenuAct::BugCancel, 0, true});
        return v;
    }
    if (a.rulesOpen && !a.nameEdit) {
        v.push_back({"ADVANCED", 0, 0.12f, 0.70f, 0.10f, MenuAct::RulesPick, 1, true});
        v.push_back({"BASIC", 0, -0.04f, 0.70f, 0.10f, MenuAct::RulesPick, 0, true});
        v.push_back({"EXPERIMENTAL - COMING LATER", 0, -0.20f, 0.70f, 0.08f, MenuAct::RulesPick, 2, false});
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
        v.push_back({here && isMuted(a, a.reportWho) ? "UNMUTE" : "MUTE", -0.28f, -0.39f, 0.25f, 0.07f, MenuAct::ReportMute, 0, here});
        v.push_back({a.bugSending ? "SENDING..." : "SEND REPORT", 0, -0.39f, 0.25f, 0.07f, MenuAct::ReportSend, 0,
                     here && a.reportWhy >= 0 && !a.bugSending});
        v.push_back({"BACK", 0.28f, -0.39f, 0.25f, 0.07f, MenuAct::ReportBack, 0, true});
        return v;
    }
    if (a.helpOpen) {
        v.push_back({"GOT IT", 0, -0.33f, 0.36f, 0.07f, MenuAct::HelpDone, 0, true});
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
        snprintf(b, sizeof b, "VOLUME  %d%%", a.volume * 10);
        v.push_back({b, 0, 0.12f, 0.46f, 0.08f, MenuAct::None, 0, true});
        v.push_back({"-", -0.32f, 0.12f, 0.10f, 0.08f, MenuAct::VolDown, 0, a.volume > 0});
        v.push_back({"+", 0.32f, 0.12f, 0.10f, 0.08f, MenuAct::VolUp, 0, a.volume < 10});
        v.push_back({a.pointerHand == 1 ? "POINTER: RIGHT" : "POINTER: LEFT", -0.19f, 0.02f, 0.36f, 0.08f, MenuAct::Hand, 0, true});
        v.push_back({a.botFill ? "BOT FILLS IN: ON" : "BOT FILLS IN: OFF", 0.19f, 0.02f, 0.36f, 0.08f, MenuAct::BotFill, 0, true});
        static const char* fx[3] = {"HIT EFFECTS: FULL", "HIT EFFECTS: SOFT", "HIT EFFECTS: LOW"};
        v.push_back({fx[a.fxLevel % 3], -0.19f, -0.08f, 0.36f, 0.08f, MenuAct::Fx, 0, true});
        v.push_back({a.hideOutside ? "OUTSIDE: HIDDEN" : "OUTSIDE: SHOWN", 0.19f, -0.08f, 0.36f, 0.08f, MenuAct::HideOutside, 0, true});
        v.push_back({a.game.tun.advanced ? "RULES: ADVANCED" : "RULES: BASIC", -0.19f, -0.18f, 0.36f, 0.08f, MenuAct::Rules, 0,
                     a.net.mode() != Net::Mode::Client && a.net.mode() != Net::Mode::Spectate});
        v.push_back({"VOICE AND MUTES", 0.19f, -0.18f, 0.36f, 0.08f, MenuAct::VoicePage, 0, true});
        std::string sv = "SERVER: " + (a.serverAddr.empty() ? std::string("NOT SET") : a.serverAddr);
        if (sv.size() > 34) sv = sv.substr(0, 33) + ".";
        const bool custom = PA_DEFAULT_SERVER[0] && a.serverAddr != PA_DEFAULT_SERVER;
        v.push_back({sv, custom ? -0.12f : 0.f, -0.28f, custom ? 0.50f : 0.74f, 0.07f, MenuAct::Server, 0, true});
        if (custom) v.push_back({"USE DEFAULT", 0.26f, -0.28f, 0.22f, 0.07f, MenuAct::ServerDefault, 0, true});
        v.push_back({"BACK", 0, -0.37f, 0.36f, 0.06f, MenuAct::Back, 0, true});
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
            snprintf(buf, sizeof buf, "%s  %s  %d-%d%s", lg.name, lg.advanced ? "ADV" : "BASIC", lg.score0, lg.score1,
                     lg.ip == a.watchIp ? "  (WATCHING)" : "");
            v.push_back({buf, rx, 0.08f - 0.09f * i, bw, 0.07f, MenuAct::Watch, lg.ip, lg.ip != a.watchIp});
        } else {
            snprintf(buf, sizeof buf, "JOIN  %s  %s%s", lg.name, lg.advanced ? "ADVANCED" : "BASIC", mine ? "  (IN)" : lg.full ? "  FULL" : "");
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
    a.menuOpen = true;
    a.prevMenuTrig = true;   // ignore a trigger that is already held
    LOGI("menu: open");
}

static void menuActivate(App& a, const MenuItem& it) {
    LOGI("menu: %s", it.label.c_str());
    switch (it.act) {
    case MenuAct::Experimental: a.xpOpen = true; break;
    case MenuAct::Nav: {
        a.titleOpen = a.settingsOpen = a.voicePage = a.xpOpen = a.bugOpen = a.reportOpen = a.rulesOpen = a.helpOpen = false;
        a.nameEdit = false;
        auto go = [&](MenuAct act) { MenuItem x{"", 0, 0, 0, 0, act, 0, true}; menuActivate(a, x); };
        switch ((int)it.ip) {
        case NAV_HOME: a.titleOpen = true; break;
        case NAV_PRACTICE: go(MenuAct::Play); break;
        case NAV_MULTI: break;   // the multiplayer page (rooms; the spectator's page while spectating)
        case NAV_SPECTATE: if (a.net.mode() != Net::Mode::Spectate) go(MenuAct::Spectate); break;
        case NAV_AVATAR: go(MenuAct::Avatar); break;
        case NAV_SETTINGS: a.settingsOpen = true; break;
        case NAV_REPORT: go(MenuAct::Report); break;
        case NAV_BUG: go(MenuAct::Bug); break;
        case NAV_HELP: a.helpOpen = true; break;
        default: break;
        }
        break;
    }
    case MenuAct::MainMenu: a.titleOpen = true; a.settingsOpen = false; break;
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
        a.settingsOpen = false; a.voicePage = false; a.reportOpen = true;
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
    case MenuAct::ReportSend: playerReportSubmit(a); a.reportOpen = false; break;
    case MenuAct::ReportBack: a.reportOpen = false; break;
    case MenuAct::Forfeit: a.net.forfeit(); a.menuOpen = false; break;
    case MenuAct::XpBack: a.xpOpen = false; break;
    case MenuAct::XpServer: a.nameEdit = true; a.editTarget = 3; a.nameBuf = a.sparcAddr; break;
    case MenuAct::XpConnect:
        if (a.sparc.state() != xc::SparcLink::State::Off && a.sparc.state() != xc::SparcLink::State::Failed) a.sparc.disconnect("LEFT");
        else a.sparc.connect(a.sparcAddr.c_str(), sparcName(a), sparcId(a));
        break;
    case MenuAct::Host: a.rulesOpen = true; a.rulesForHost = true; break;   // CHOOSE THE RULES, then the room opens
    case MenuAct::RulesPick: {
        if (it.ip > 1) break;   // EXPERIMENTAL: not defined yet
        const bool adv = it.ip == 1;
        if (a.game.tun.advanced != adv) {
            a.game.tun.advanced = adv;
            a.game.reset(a.game.pl[1].isBot);
            saveSettings(a);
        }
        LOGI("rules -> %s (%s)", adv ? "advanced" : "basic", a.rulesForHost ? "host a room" : "practice");
        a.rulesOpen = false;
        if (a.rulesForHost) {
            a.net.host();
        } else {
            a.titleOpen = false; a.menuOpen = false;
            if (a.net.mode() == Net::Mode::Offline) a.game.reset(true);
        }
        break;
    }
    case MenuAct::RulesBack: a.rulesOpen = false; if (!a.rulesForHost) a.titleOpen = true; break;
    case MenuAct::Leave: a.net.leave(); break;
    case MenuAct::Join: a.net.join(it.ip, false); a.menuOpen = false; break;         // the room's waiting area, then JOIN QUEUE
    case MenuAct::JoinRoom: a.net.join(it.ip, false); a.menuOpen = false; break;
    case MenuAct::BotFill: a.botFill = !a.botFill; a.net.setBotFill(a.botFill); saveSettings(a); break;
    case MenuAct::Rules: {
        bool bot = a.game.pl[1].isBot;
        a.game.tun.advanced = !a.game.tun.advanced;
        a.game.reset(bot);
        LOGI("rules -> %s", a.game.tun.advanced ? "Brawl Advanced" : "Brawl Basic");
        saveSettings(a);
        break;
    }
    case MenuAct::Close: a.menuOpen = false; a.nameEdit = false; a.helpOpen = false; a.settingsOpen = false; a.titleOpen = false; a.xpOpen = false; a.rulesOpen = false; break;
    case MenuAct::Spectate: a.net.spectate(); a.watchIp = 0; a.menuOpen = false; break;
    case MenuAct::Watch: a.watchIp = it.ip; a.menuOpen = false; break;
    case MenuAct::Hub: a.watchIp = 0; a.autoCam = false; a.dirOpen = false; break;
    case MenuAct::Queue: a.net.queue(a.net.localQueuePos() < 0); break;
    case MenuAct::HideOutside: a.hideOutside = !a.hideOutside; saveSettings(a); break;
    case MenuAct::ClearWarps: a.warps.clear(); a.warpIdx = -1; a.autoCam = false; saveWarps(a); break;
    case MenuAct::Director: openDirector(a, a.localIn); break;
    case MenuAct::Avatar: a.titleOpen = false; openBuilder(a, a.localIn); break;
    case MenuAct::Play: a.titleOpen = false; a.rulesOpen = true; a.rulesForHost = false; break;   // CHOOSE THE RULES first
    case MenuAct::Multi: a.titleOpen = false; break;
    case MenuAct::Settings: a.settingsOpen = true; break;
    case MenuAct::Back: if (a.voicePage) a.voicePage = false; else a.settingsOpen = false; saveSettings(a); break;
    case MenuAct::VoicePage: a.voicePage = true; break;
    case MenuAct::VoiceMode: a.voiceMode = (a.voiceMode + 1) % 3; a.micDenied = false; saveSettings(a); break;
    case MenuAct::MuteSpec: a.muteSpectators = !a.muteSpectators; saveSettings(a); break;
    case MenuAct::MutePlayers: a.mutePlayers = !a.mutePlayers; saveSettings(a); break;
    case MenuAct::MuteAll: a.muteAll = !a.muteAll; saveSettings(a); break;
    case MenuAct::VolDown: a.volume = std::max(0, a.volume - 1); a.audio.setVolume(a.volume / 10.f); saveSettings(a); break;
    case MenuAct::VolUp: a.volume = std::min(10, a.volume + 1); a.audio.setVolume(a.volume / 10.f); saveSettings(a); break;
    case MenuAct::Hand: a.pointerHand = 1 - a.pointerHand; saveSettings(a); break;
    case MenuAct::Fx: a.fxLevel = (a.fxLevel + 1) % 3; saveSettings(a); break;
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
    case MenuAct::Key: if (a.nameBuf.size() < (a.editTarget >= 4 ? 60u : a.editTarget == 1 || a.editTarget == 3 ? 48u : 16u) && !(it.ip == ' ' && (a.nameBuf.empty() || a.nameBuf.back() == ' '))) a.nameBuf += (char)it.ip; break;
    case MenuAct::KeyDel: if (!a.nameBuf.empty()) a.nameBuf.pop_back(); break;
    case MenuAct::KeyDone:
        while (!a.nameBuf.empty() && a.nameBuf.back() == ' ') a.nameBuf.pop_back();
        if (a.editTarget == 5) { a.reportNote = a.nameBuf; }
        else if (a.editTarget == 4) { a.bugNote = a.nameBuf; }
        else if (a.editTarget == 3) { if (!a.nameBuf.empty()) a.sparcAddr = a.nameBuf; saveSettings(a); }
        else if (a.editTarget == 2) { a.team = a.nameBuf.substr(0, 16); saveSettings(a); a.menuOpen = false; }
        else if (a.editTarget == 1) { a.serverAddr = a.nameBuf; saveSettings(a); if (!a.serverAddr.empty()) a.net.goOnline(a.serverAddr.c_str()); }
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
    const float bg[4] = {0.004f, 0.007f, 0.016f, 0.90f}, edge[4] = {0.05f, 0.35f, 0.60f, 0.9f};
    const float btn[4] = {0.03f, 0.05f, 0.08f, 0.95f}, hov[4] = {0.05f, 0.40f, 0.75f, 0.98f}, dis[4] = {0.015f, 0.015f, 0.02f, 0.9f};
    const float white[4] = {1, 1, 1, 1}, grey[4] = {0.35f, 0.38f, 0.42f, 1}, cyan[4] = {0.3f, 0.85f, 1.0f, 1};
    quad(0, -0.09f, 1.04f, 0.98f, -0.002f, edge);
    quad(0, -0.09f, 1.0f, 0.94f, 0, bg);
    if (!a.nameEdit) {   // side menu
        quad(kNavX, -0.09f, 0.30f, 0.98f, -0.002f, edge);
        quad(kNavX, -0.09f, 0.28f, 0.94f, 0, bg);
    }
    char buf[96];
    if (a.bugOpen && !a.nameEdit) {
        text("REPORT A BUG", 0, 0.29f, 0.055f, cyan);
        text("WHAT KIND OF PROBLEM? PICK ONE, ADD A NOTE IF YOU LIKE", 0, 0.215f, 0.022f, white);
        text("SENDS THE NOTE, YOUR SETTINGS AND THE GAME'S RECENT LOG - NOTHING ELSE", 0, -0.33f, 0.018f, grey);
        text("TIP: REPORT RIGHT AFTER IT HAPPENS", 0, -0.37f, 0.018f, grey);
    } else if (a.rulesOpen && !a.nameEdit) {
        text("CHOOSE THE RULES", 0, 0.29f, 0.055f, cyan);
        text(a.rulesForHost ? "FOR YOUR ROOM - EVERYONE WHO JOINS PLAYS THESE RULES" : "FOR PRACTICE AGAINST THE BOT", 0, 0.215f, 0.022f, white);
        text("BEST OF 3 ROUNDS, FIRST TO 4 POINTS - LOW AIM ASSIST", 0, 0.045f, 0.019f, grey);
        text("3 MINUTE MATCH - KNUCKLE GUARDS - MORE AIM ASSIST", 0, -0.115f, 0.019f, grey);
        text("NEW RULES TO TRY OUT - NOT READY YET", 0, -0.265f, 0.019f, grey);
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
    } else if (a.settingsOpen) {
        text(a.voicePage ? "VOICE CHAT" : "SETTINGS", 0, 0.29f, 0.055f, cyan);
        if (a.voicePage) text("ONLY PEOPLE AT YOUR ARENA CAN HEAR YOU", 0, 0.205f, 0.024f, grey);
    } else if (a.helpOpen) {
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
        snprintf(buf, sizeof buf, "%s_", a.nameBuf.c_str());
        text(buf, 0, 0.205f, 0.045f, white);
    } else if (!a.titleOpen && !a.settingsOpen) {
        text("MULTIPLAYER", 0, 0.29f, 0.055f, cyan);
        if (a.net.inRoom())
            snprintf(buf, sizeof buf, "YOU: %s   -   %s   -   %s RULES", a.net.name(), a.net.status(), a.game.tun.advanced ? "ADVANCED" : "BASIC");
        else
            snprintf(buf, sizeof buf, "YOU: %s   -   %s", a.net.name(), a.net.status());
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
                            (it.act == MenuAct::RulesPick && (int)it.ip == (a.game.tun.advanced ? 1 : 0)) || (it.act == MenuAct::ReportWho && it.ip == a.reportWho) ||
                            (it.act == MenuAct::ReportWhy && (int)it.ip == a.reportWhy);
        quad(it.x, it.y, it.w, it.h, 0.003f, !it.enabled && !row ? dis : ((int)i == a.menuHover || picked) ? hov : btn);
        float th = row ? 0.026f : 0.034f;
        const float tw = th * 0.75f * (float)it.label.size();          // the font's glyphs are 0.75 x their height
        if (tw > it.w * 0.90f) th *= it.w * 0.90f / tw;                 // long labels shrink instead of spilling over
        text(it.label, it.x, it.y, th, it.enabled || row ? white : grey);
    }
    if (games == 0 && !a.nameEdit && !a.helpOpen && !a.titleOpen && !a.settingsOpen && !a.xpOpen && !a.bugOpen && !a.reportOpen && !a.rulesOpen) {
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
    text("POINT AND PULL THE TRIGGER TO SELECT", 0, -0.455f, 0.022f, grey);
    if (a.spectating && a.watchIp && !a.titleOpen && !a.settingsOpen && !a.nameEdit && !a.helpOpen) {
        snprintf(buf, sizeof buf, "STICKS FLY - A SAVE CAMERA - X NEXT - Y AUTO CAMERA   (%d CAMERAS)", (int)a.warps.size());
        text(buf, 0, -0.495f, 0.02f, grey);
    }
    if (a.devMode) {   // gauntlet / shield tuning (thumbsticks only act while this menu is open)
        static const char* kPlace[4] = {"ABOVE", "BACK OF HAND", "BACK OF HAND (OLD)", "PALM SIDE"};
        snprintf(buf, sizeof buf, "SHIELD: %s (R-STICK L/R)   GAUNTLET PITCH %+.0f (R-STICK U/D)", kPlace[a.game.tun.shieldPlacement & 3],
                 a.tunePitchDeg);
        text(buf, 0, -0.505f, 0.02f, grey);
        snprintf(buf, sizeof buf, "GAUNTLET FWD %+.2f  UP %+.2f (L-STICK)", a.tuneFwd, a.tuneUp);
        text(buf, 0, -0.535f, 0.02f, grey);
    }
    // laser + cursor
    if (a.aimValid[a.pointerHand]) {
        Vec3 o = a.aim[a.pointerHand].p, d = a.aim[a.pointerHand].q.rotate({0, 0, -1});
        float len = a.menuHitValid ? length(a.menuHit - o) : 1.5f;
        const float laser[4] = {0.15f, 0.6f, 1.0f, 1};
        a.gfx.drawBox(Mat4::fromPose({a.aim[a.pointerHand].q, o + d * (len * 0.5f)}, {0.003f, 0.003f, len}), laser, true);
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

static void handleEvents(App& a) {
    const int L = a.net.localIndex();
    for (GameEvent e : a.game.events) {
        bool mine = !a.viewing && e.player == L;
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
        a.audio.onEvent(e, a.game.pl[L].in.head.p, L);
    }
}


// ------------------------------------------------------------------ avatars
static const float kTeamRGB[3][3] = {{0.1f, 0.7f, 1.0f}, {1.0f, 0.4f, 0.1f}, {0.85f, 0.85f, 0.9f}};   // 2 = outside the tank

struct AvatarLook { float ch[avatar::CH_COUNT][4]; float rim[4]; GLuint pattern; };

static AvatarLook lookOf(const App& a, const AvatarDesc& d, int team) {
    AvatarLook L;
    const int t = team < 0 || team > 2 ? 2 : team;
    avatar::channelColors(d, kTeamRGB[t], L.ch);
    for (int k = 0; k < 3; ++k) L.rim[k] = kTeamRGB[t][k];
    L.rim[3] = 0.5f;   // the team-colour rim light that marks every player (like the original's)
    L.pattern = d.pattern < 8 ? a.patternTex[d.pattern] : 0;
    return L;
}

static void applyLook(DrawParams& dp, const AvatarLook& L) {
    dp.channels = L.ch;
    std::memcpy(dp.rim, L.rim, sizeof dp.rim);
    dp.pattern = L.pattern;
}

// Body (torso, neck, head and its parts, arms) for one player; `pre` places it (miniatures, the builder mirror),
// preMirror when `pre` has a negative determinant. Hands are drawn by the caller.
static void drawAvatarBody(App& a, const PlayerInput& in, int team, int pass, const Mat4& pre, bool preMirror, bool lod,
                           float flash) {
    avatar::Rig rig = avatar::solveRig(in, in.avatar);
    static std::vector<avatar::PartDraw> list;
    list.clear();
    avatar::drawList(rig, in, in.avatar, list, lod);
    const AvatarLook L = lookOf(a, in.avatar, team);
    for (const avatar::PartDraw& p : list) {
        auto it = a.avModels.find(p.model);
        if (it == a.avModels.end()) continue;
        DrawParams dp;
        applyLook(dp, L);
        dp.mirrored = p.mirrored != preMirror;
        if (flash > 0) { dp.emissiveAdd[0] = flash; dp.emissiveAdd[1] = flash * 0.3f; }
        if (p.flags & avatar::PART_NEON)
            for (int k = 0; k < 3; ++k) dp.emissiveAdd[k] += L.ch[avatar::CH_GLOW][k] * 0.35f;
        if (p.flags & avatar::PART_SKINNED) dp.bones = rig.skin;
        a.gfx.draw(it->second, pre * p.m, dp, pass);
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
static float builderPresetY() { return kBRow0 - avatar::F_COUNT * kBRowH - 0.014f; }
static float builderButtonsY() { return builderPresetY() - 0.08f; }

// hit codes: field * 2 (+1 = next), 100 / 101 preset prev / next, 102 random, 103 done
static int builderHitTest(float lx, float ly) {
    for (int f = 0; f < avatar::F_COUNT; ++f) {
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
    float yb = builderButtonsY();
    if (std::fabs(ly - yb) < 0.032f) {
        if (std::fabs(lx + 0.17f) < 0.15f) return 102;
        if (std::fabs(lx - 0.17f) < 0.15f) return 103;
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
    a.menuOpen = false;
    a.prevBuilderTrig = true;
    a.presetIdx = 0;
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
        a.presetIdx += hit == 101 ? 1 : -1;
        d = avatar::preset(a.presetIdx);
        LOGI("avatar: preset %s", avatar::presetName(a.presetIdx));
    } else if (hit == 102) {
        d = avatar::randomLook((uint32_t)(nowSeconds() * 1000.0));
    } else if (hit == 103) {
        closeBuilder(a);
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
                if (std::fabs(lp.x) < 0.44f && lp.y < 0.52f && lp.y > builderButtonsY() - 0.06f) { a.builderHit = p; a.builderHitValid = true; }
                a.builderHover = builderHitTest(lp.x, lp.y);
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
    return d.gauntlet < 3 && a.gauntlets[d.gauntlet].valid() ? a.gauntlets[d.gauntlet] : a.lancer;
}

static void drawLancerAt(App& a, const Mat4& grip, int h, const AvatarLook& L, int pass, const Mat4& pre, bool preMirror,
                         const AvatarDesc& d) {
    Mat4 mirror = Mat4::identity();
    if (h == 0) mirror.m[0] = -1;
    DrawParams dp;
    applyLook(dp, L);
    dp.mirrored = (h == 0) != preMirror;
    a.gfx.draw(gauntletOf(a, d), pre * grip * mirror * g_lancerLocal, dp, pass);
}

// main menu: your avatar on a slowly turning pedestal to the right of the menu, your name above it
static void drawTitleShowcase(App& a, int pass) {
    if (!a.menuOpen || !a.titleOpen || a.builderOpen || a.helpOpen || a.avModels.empty()) return;
    const Vec3 right = a.menuPose.q.rotate({1, 0, 0}), away = a.menuPose.q.rotate({0, 0, -1});
    Vec3 base = a.menuPose.p + right * 1.9f + away * 1.4f;   // clear of the panel (about 40 degrees to the right)
    base.y = 0;
    Vec3 toUser = a.viewHead.p - base;
    toUser.y = 0;
    toUser = length(toUser) > 1e-3f ? normalize(toUser) : Vec3{0, 0, 1};
    const float face = std::atan2(-toUser.x, -toUser.z) + kPi;   // looking at the player
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
    drawAvatarBody(a, in, 0, pass, Mat4::identity(), false, false, 0);
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
    const Quat lq = Quat::axisAngle({0, 1, 0}, face);
    a.gfx.drawText(a.net.name(), {lq, base + Vec3{0, 1.90f, 0}}, 0.08f, white);
}

static void drawBuilder(App& a, int pass) {
    if (!a.builderOpen) return;
    const int L = a.net.localIndex();
    const PlayerInput& in = a.localIn;
    const Mat4 R = reflectMat(a.mirrorPoint, a.mirrorNormal);
    // the reflection: your avatar, moving with you
    drawAvatarBody(a, in, L, pass, R, true, false, 0);
    const AvatarLook look = lookOf(a, in.avatar, L);
    for (int h = 0; h < 2; ++h)
        if (in.hand[h].tracked) drawLancerAt(a, Mat4::fromPose(in.hand[h].grip), h, look, pass, R, true, in.avatar);
    if (pass != 1) return;
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
    // the panel
    auto at = [&](float x, float y, float z) { return a.builderPose.transform({x, y, z}); };
    auto quad = [&](float x, float y, float w, float h, float z, const float* c) {
        a.gfx.drawPanel(Mat4::fromPose({a.builderPose.q, at(x, y, z)}, {w, h, 1}), c);
    };
    auto text = [&](const std::string& t, float x, float y, float h, const float* c) {
        a.gfx.drawText(t, {a.builderPose.q, at(x, y, 0.006f)}, h, c);
    };
    const float bg[4] = {0.004f, 0.007f, 0.016f, 0.90f}, frame[4] = {edge[0] * 0.6f, edge[1] * 0.6f, edge[2] * 0.6f, 0.9f};
    const float btn[4] = {0.03f, 0.05f, 0.08f, 0.95f}, hov[4] = {0.05f, 0.40f, 0.75f, 0.98f};
    const float white[4] = {1, 1, 1, 1}, grey[4] = {0.45f, 0.5f, 0.56f, 1}, cyan[4] = {0.3f, 0.85f, 1.0f, 1};
    const float top = 0.52f, bottom = builderButtonsY() - 0.06f;
    quad(0, (top + bottom) * 0.5f, 0.90f, top - bottom + 0.03f, -0.002f, frame);
    quad(0, (top + bottom) * 0.5f, 0.87f, top - bottom, 0, bg);
    text("CUSTOMIZE AVATAR", 0, 0.455f, 0.042f, cyan);
    const AvatarDesc& d = a.myAvatar;
    for (int f = 0; f < avatar::F_COUNT + 1; ++f) {
        const bool presetRow = f == avatar::F_COUNT;
        const float y = presetRow ? builderPresetY() : kBRow0 - f * kBRowH;
        const int hp = presetRow ? 100 : f * 2;
        text(presetRow ? "PRESET" : avatar::fieldName(avatar::rowField(f)), kBLabel, y, 0.024f, grey);
        quad(kBPrev, y, 0.05f, 0.044f, 0.003f, a.builderHover == hp ? hov : btn);
        quad(kBNext, y, 0.05f, 0.044f, 0.003f, a.builderHover == hp + 1 ? hov : btn);
        text("<", kBPrev, y, 0.028f, white);
        text(">", kBNext, y, 0.028f, white);
        if (presetRow) {
            text(avatar::presetName(a.presetIdx), kBValue, y, 0.024f, white);
            continue;
        }
        const avatar::Field fld = avatar::rowField(f);
        text(avatar::optionName(fld, avatar::get(d, fld)), kBValue, y, 0.024f, white);
        if (avatar::isColor(fld)) {
            float c3[3];
            avatar::optionColor(fld, avatar::get(d, fld), kTeamRGB[L & 1], c3);
            const float sw[4] = {c3[0], c3[1], c3[2], 1};
            quad(0.395f, y, 0.032f, 0.032f, 0.003f, sw);
        }
    }
    const float yb = builderButtonsY();
    quad(-0.17f, yb, 0.30f, 0.06f, 0.003f, a.builderHover == 102 ? hov : btn);
    quad(0.17f, yb, 0.30f, 0.06f, 0.003f, a.builderHover == 103 ? hov : btn);
    text("RANDOM", -0.17f, yb, 0.03f, white);
    text("DONE", 0.17f, yb, 0.03f, white);
    // laser + cursor
    if (a.aimValid[a.pointerHand]) {
        Vec3 o = a.aim[a.pointerHand].p, dd = a.aim[a.pointerHand].q.rotate({0, 0, -1});
        float len = a.builderHitValid ? length(a.builderHit - o) : 1.5f;
        const float laser[4] = {0.15f, 0.6f, 1.0f, 1};
        a.gfx.drawBox(Mat4::fromPose({a.aim[a.pointerHand].q, o + dd * (len * 0.5f)}, {0.003f, 0.003f, len}), laser, true);
        if (a.builderHitValid) a.gfx.drawSphere(a.builderHit, 0.008f, cyan, true);
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
        a.gfx.drawBox(Mat4::fromPose({a.aim[a.pointerHand].q, o + d * (len * 0.5f)}, {0.003f, 0.003f, len}), laser, true);
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
    const bool trig = in.hand[a.pointerHand].trigger > 0.7f;
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

static void drawRoomPeople(App& a, int pass) {
    const RoomInfo* rm = currentRoom(a);
    if (!rm) return;
    const bool inTank = a.net.inRoom() && !a.net.localWaiting();
    const uint32_t me = a.net.session();
    // waiting players stand in the holding area: full avatars, a neutral rim light; the players may hide them
    if (!(inTank && a.hideOutside)) {
        for (const RoomInfo::Member& m : rm->members) {
            if (m.id == me || !m.in.headTracked || insideTank(m.in.head.p)) continue;
            if (!a.avModels.empty()) drawAvatarBody(a, m.in, 2, pass, Mat4::identity(), false, false, 0);
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
        a.gfx.drawBox(Mat4::fromPose({a.aim[a.pointerHand].q, o + d * (len * 0.5f)}, {0.003f, 0.003f, len}), laser, true);
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
            a.gfx.draw(gauntletOf(a, ps.in.avatar), M(Mat4::fromPose(ps.in.hand[h].grip) * mirror * g_lancerLocal), hp2, pass);
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
            int secs = (int)std::ceil(std::max(0.f, g.roundClock));
            if (g.tun.advanced) snprintf(buf, sizeof buf, "ADVANCED   %d  -  %d   ROUND %d", g.pl[0].score, g.pl[1].score, std::max(1, g.round));
            else snprintf(buf, sizeof buf, "BASIC   %d  -  %d   %d:%02d", g.pl[0].score, g.pl[1].score, secs / 60, secs % 60);
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
    if (zone && !a.micDenied && !a.audio.capturing()) {
        if (micPermission(!a.micAsked)) a.audio.startCapture();
        else if (a.micAsked && !micPermission(false)) a.micDenied = true;
        a.micAsked = true;
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
        if (a.voiceSent || a.voiceRecv)
            LOGI("voice: sent %d frames, received %d (mic gain %.1fx, %s)", a.voiceSent, a.voiceRecv, a.micGain,
                 a.voiceMode == 0 ? "push to talk" : a.voiceMode == 1 ? "open mic" : "off");
        a.voiceSent = a.voiceRecv = 0;
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
        const char* t = a.voiceMode == 2 ? "VOICE OFF" : a.micDenied ? "MIC NOT ALLOWED" : a.muteAll ? "ALL MUTED"
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

static void drawWorld(App& a, int pass) {
    if (a.spectating && !a.watchIp) { drawHub(a, pass); return; }
    Game& g = a.game;
    const int L = a.net.localIndex();
    DrawParams dp;
    const float now = float(nowSeconds() - a.startTime);
    if (pass == 1 && a.floor.valid()) {
        // glossy floor: mirrored light sources (and the balls) seen through the semi-transparent floor
        DrawParams rp;
        a.gfx.draw(a.floorReflect, Mat4::identity(), rp, 1);
        const Vec3 eye = a.viewHead.p;
        const float tcol[2][3] = {{0.1f, 0.7f, 1.0f}, {1.0f, 0.4f, 0.1f}};
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
        DrawParams fp;
        fp.alphaMul = 0.80f;
        a.gfx.draw(a.floor, Mat4::identity(), fp, 1);
    }
    a.gfx.draw(a.environment, Mat4::identity(), dp, pass);
    a.gfx.draw(a.arena, Mat4::identity(), dp, pass);
    if (pass == 1 && a.spectating) {   // the floating gallery seat the spectator stands on
        const float pc[4] = {0.03f, 0.08f, 0.12f, 1}, rc[4] = {0.15f, 0.5f, 0.75f, 1};
        a.gfx.drawSpriteFlat({0, 1.004f, 5.4f}, 1.8f, pc, {0, 1, 0}, 0);
        a.gfx.drawSpriteFlat({0, 1.006f, 5.4f}, 2.0f, rc, {0, 1, 0}, 1);
    }
    if (pass == 1) {
        DrawParams xp;
        xp.uvOff[0] = now * 0.035f;   // crowd lights twinkle (env_fx = crowd lights only; they sample a scrolling twinkle strip)
        a.gfx.draw(a.envFx, Mat4::identity(), xp, 1);
        DrawParams cp;   // static glow layers: light halos, holographic rings, end-zone energy, light beams
        a.gfx.draw(a.courtFx, Mat4::identity(), cp, 1);
        DrawParams pp;   // energy pulses racing along the neon outline
        pp.uvOff[1] = -now * 0.9f;
        a.gfx.draw(a.courtPulse, Mat4::identity(), pp, 1);
    }
    const float teamCol[2][4] = {{0.1f, 0.7f, 1.0f, 1}, {1.0f, 0.4f, 0.1f, 1}};
    for (int p = 0; p < 2; ++p) {
        const BallState st = g.pl[p].ball.state;
        float glow = (st == BallState::Spawning || st == BallState::Dead) ? 0.55f : 0.16f;
        DrawParams sp;
        sp.emissiveAdd[0] = teamCol[p][0] * glow; sp.emissiveAdd[1] = teamCol[p][1] * glow; sp.emissiveAdd[2] = teamCol[p][2] * glow;
        a.gfx.draw(a.spawner, Mat4::fromPose({Quat{}, g.tun.spawner[p]}), sp, pass);
    }
    for (int p = 0; p < 2; ++p) {
        if (!a.spectating && !a.net.slotFilled(p)) continue;
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
            a.gfx.draw(gauntletOf(a, look), grip * mirror * g_lancerLocal, hp, pass);
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
                a.gfx.draw(a.knuckle, grip * mirror * g_knuckleLocal, kp, pass);
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
        if (b.state != BallState::Dead && (b.state != BallState::Spawning || b.pos.y > 0.f)) {
            DrawParams bp;
            float glow = b.charged ? 2.0f : 0.6f;   // uncharged ball: "your Ball has lost its charge"
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
                const int nArcs = b.charged ? 3 : 1;
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
    if (pass == 1)
        for (int p = 0; p < 2; ++p)
            if (a.trail[p].burst > 0) {   // ball dissolving at the back wall
                float k = a.trail[p].burst / 0.45f;
                float c[4] = {teamCol[p][0] * k * 2, teamCol[p][1] * k * 2, teamCol[p][2] * k * 2, 1};
                a.gfx.drawSphere(a.trail[p].burstPos, 0.1f + (1 - k) * 0.6f, c, true);
            }
    for (int p = 0; p < 2; ++p) {   // strike-zone rings
        DrawParams zp;
        float f = 0.35f + 1.5f * a.trail[p].strikeFlash;
        zp.forceShader = SH_ADDITIVE;
        zp.tintMul[0] = teamCol[p][0] * f; zp.tintMul[1] = teamCol[p][1] * f; zp.tintMul[2] = teamCol[p][2] * f;
        a.gfx.draw(a.strikezone, Mat4::fromPose({Quat{}, g.strikeZoneCenter(p)}), zp, pass);
    }
    drawRoomPeople(a, pass);
    if (pass == 1 && !a.rttPass) drawVoiceHud(a);
    if (pass == 1) drawFx(a);
    if (pass == 1) {
        // Holographic scoreboard over centre court, facing the local player (mirrored for the far-end player)
        char buf[96];
        const float white[4] = {1, 1, 1, 1}, grey[4] = {0.55f, 0.6f, 0.68f, 1};
        const float* myCol = teamCol[L];
        const float* opCol = teamCol[1 - L];
        int secs = (int)std::ceil(std::max(0.f, g.roundClock));
        const float sx = (L == 0) ? 1.f : -1.f;
        Quat faceP0 = Quat::axisAngle({0, 1, 0}, deg2rad(L == 0 ? -90.f : 90.f));   // quad +Z -> toward local player
        Vec3 c0{-0.5f * sx, 2.95f, 0};
        if (a.spectating) { faceP0 = Quat{}; c0 = {0, 4.75f, 0.2f}; }   // above the roof, facing the gallery seat
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
        if (g.tun.advanced) {
            snprintf(buf, sizeof buf, "ROUND %d", std::max(1, g.round));
            txt(buf, 0, 0.12f, 0.12f, white);
            snprintf(buf, sizeof buf, "%d - %d", g.pl[L].roundWins, g.pl[1 - L].roundWins);
            txt(buf, 0, -0.06f, 0.10f, grey);
        } else {
            snprintf(buf, sizeof buf, "%d:%02d", secs / 60, secs % 60);
            txt(buf, 0, 0.06f, 0.20f, g.overtime ? edgeR : white);
            txt(g.overtime ? "SUDDEN DEATH" : "BASIC", 0, -0.11f, 0.055f, grey);
        }
        snprintf(buf, sizeof buf, "STRIKES %d", g.pl[1 - L].strikesAgainst);
        txt(buf, -0.80f, -0.22f, 0.05f, grey);
        snprintf(buf, sizeof buf, "STRIKES %d", g.pl[L].strikesAgainst);
        txt(buf, 0.80f, -0.22f, 0.05f, grey);
        txt(g.pl[L].ball.charged ? "BALL CHARGED" : "NO CHARGE", 0, -0.24f, 0.045f, g.pl[L].ball.charged ? myC : grey);
        snprintf(buf, sizeof buf, "%s  -  %s RULES", a.net.status(), g.tun.advanced ? "ADVANCED" : "BASIC");
        txt(buf, 0, -0.42f, 0.045f, grey);
        const char* banner = nullptr;
        switch (g.match) {
        case MatchState::WaitingForPlayers: banner = "WAITING FOR PLAYERS"; break;
        case MatchState::Meet: snprintf(buf, sizeof buf, "FIST BUMP TO START   %d", (int)std::ceil(g.stateTimer)); banner = buf; break;
        case MatchState::Countdown: snprintf(buf, sizeof buf, "ROUND %d   %d", g.round, (int)std::ceil(g.stateTimer)); banner = buf; break;
        case MatchState::RoundSummary: banner = g.lastRoundWinner == L ? "ROUND WON" : "ROUND LOST"; break;
        case MatchState::MatchSummary: banner = g.matchWinner == L ? "YOU WIN  -  PRESS A" : "YOU LOSE  -  PRESS A"; break;
        default: break;
        }
        if (banner) a.gfx.drawText(banner, {faceP0, {-1.5f * sx, 2.2f, 0}}, 0.15f, white);
        if (a.bumpFlash > 0 && !a.spectating && !a.net.localWaiting()) {
            const float gold[4] = {1.0f, 0.85f, 0.3f, std::min(1.f, a.bumpFlash * 2.f)};
            const Quat face = Quat::axisAngle({0, 1, 0}, deg2rad(L == 0 ? -90.f : 90.f));
            a.gfx.drawText("FIST BUMP!", {face, {L == 0 ? 1.6f : -1.6f, 2.30f, 0}}, 0.12f, gold);
        }
        if (g.match == MatchState::Meet && a.bumpFlash <= 0 && !a.spectating && !a.net.localWaiting()) {   // between the two players
            const float cyanT[4] = {0.3f, 0.85f, 1.0f, 1};
            const Quat face = Quat::axisAngle({0, 1, 0}, deg2rad(L == 0 ? -90.f : 90.f));
            const float pulse = 0.5f + 0.5f * std::sin((float)nowSeconds() * 5.f);
            const float glowT[4] = {0.3f, 0.85f, 1.0f, 0.55f + 0.35f * pulse};
            a.gfx.drawSprite({0, 1.2f, 0}, 0.10f + 0.04f * pulse, glowT, a.viewHead.p, 0);   // meet here
            a.gfx.drawText("BUMP HERE", {face, {L == 0 ? 0.08f : -0.08f, 1.36f, 0}}, 0.035f, cyanT);
            a.gfx.drawText("BUMP FISTS - HOLD BOTH TRIGGERS", {face, {L == 0 ? 1.6f : -1.6f, 2.15f, 0}}, 0.08f, cyanT);   // above the other player
            snprintf(buf, sizeof buf, "OR THE MATCH STARTS IN %d", (int)std::ceil(g.stateTimer));
            a.gfx.drawText(buf, {face, {L == 0 ? 1.6f : -1.6f, 2.03f, 0}}, 0.05f, white);
        }
        if (const char* note = a.net.noticeText()) {
            const float amber[4] = {1.0f, 0.75f, 0.2f, 1};
            a.gfx.drawText(note, {faceP0, {-1.5f * sx, 1.9f, 0}}, 0.12f, amber);
        }
        if (!a.rttPass) drawMenu(a);
    }
    if (!a.rttPass) drawBuilder(a, pass);
    if (!a.rttPass) drawTitleShowcase(a, pass);
    if (pass == 1) drawDirector(a);
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
    a.sparc.update(t);   // EXPERIMENTAL cross-play link (does nothing unless connected)
    if (int done = a.bugDone.exchange(0)) {
        std::lock_guard<std::mutex> lk(a.bugMx);
        if (!a.bugStatus.empty()) a.net.noticeLocal(a.bugStatus.c_str());
        (void)done;
    }

    const int L = a.net.localIndex();
    PlayerInput in[2];
    in[L] = readInput(a, predicted, L);
    a.localIn = in[L];
    a.viewHead = in[L].head;
    a.bumpFlash = std::max(0.f, a.bumpFlash - dt);
    if (a.game.match == MatchState::Meet && a.net.inRoom() && !a.net.localWaiting() && !a.spectating) {
        // closer fists buzz harder (you can feel the other hand coming); otherwise a light reminder every 2 s
        const PlayerState& o = a.game.pl[1 - L];
        float best = 9.f;
        int bh = 1;
        for (int h = 0; h < 2; ++h)
            for (int oh = 0; oh < 2; ++oh)
                if (in[L].hand[h].tracked && o.in.hand[oh].tracked) {
                    const float d = length(in[L].hand[h].grip.p - o.in.hand[oh].grip.p);
                    if (d < best) { best = d; bh = h; }
                }
        if (best < 0.45f && t - a.meetTick > 0.12) { a.meetTick = t; haptic(a, bh, 0.15f + 0.6f * (1.f - best / 0.45f), 0.05f); }
        else if (t - a.meetTick > 2.0) { a.meetTick = t; haptic(a, 0, 0.25f, 0.06f); haptic(a, 1, 0.25f, 0.06f); }
    }
    if (a.game.pl[1].isBot) a.game.pl[1].in.avatar = a.botAvatar;
    updateHub(a, in[L]);
    updateQueueKiosk(a, in[L]);
    if (a.net.mode() == Net::Mode::Spectate) {
        if (!a.spectating) { a.savedGame = a.game; a.spectating = true; }
        const PlayerInput mine = in[L];
        a.net.setPresence(a.watchIp ? 1 : 0, mine.head.p, a.watchIp);
        a.net.exchange(mine, in, a.game, dt);
        SpecMatch* w = nullptr;
        for (SpecMatch& m : a.net.matches()) if (m.ip == a.watchIp) w = &m;
        if (a.watchIp && !w) { a.watchIp = 0; LOGI("spectate: match ended, back to the hub"); }
        if (w) {   // watched match: its state and events drive the normal renderer, audio and effects
            std::vector<CollisionTri> keep;
            keep.swap(a.game.arena);
            a.game = w->game;
            a.game.arena.swap(keep);
            a.game.events.swap(w->events);
            w->events.clear();
            a.watchRoom = w->room;
            directorEvents(a);
        } else {
            a.game.events.clear();
        }
    } else {
        if (a.spectating) { a.game = a.savedGame; a.spectating = false; a.watchIp = 0; }
        const PlayerInput mine = in[L];
        if (a.net.exchange(mine, in, a.game, dt)) {
            // practice is paused while you customize your avatar (the builder only opens offline)
            const bool paused = (a.builderOpen || (a.menuOpen && a.titleOpen)) && a.net.mode() != Net::Mode::Host;
            if (!paused) a.game.step(dt, in);
            a.net.sendState(a.game);
        }
    }
    a.viewing = a.spectating || a.net.localWaiting();
    updateVoice(a);
    {   // team cameras shared by a teammate
        char team[17];
        std::vector<uint8_t> blob;
        if (a.net.takeCameras(team, blob) && !a.team.empty() && a.team == team && unpackWarps(a, blob)) {
            saveWarps(a);
            a.net.noticeLocal("TEAM CAMERAS RECEIVED");
        }
    }
    handleEvents(a);
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

    std::vector<XrCompositionLayerProjectionView> projViews(a.views.size(), {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::vector<XrCompositionLayerBaseHeader*> layers;
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
            glClearColor(0.01f, 0.012f, 0.02f, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            Pose eye = a.trackToWorld * fromXr(a.views[i].pose);
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
                // small: tracking settling. Keep the court fixed in the room.
                a.trackToWorld = a.trackToWorld * fromXr(rc->poseInPreviousSpace);
                LOGI("reference space shifted (%.3f, %.3f, %.3f): compensated", d.position.x, d.position.y, d.position.z);
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
        fprintf(fp, "cfgver=3\n");
        fprintf(fp, "advanced=%d\nshield=%d\npitch=%.1f\nfwd=%.3f\nup=%.3f\nvolume=%d\nhand=%d\nfx=%d\nhideoutside=%d\nvoice=%d\nmutespec=%d\nmuteplayers=%d\nmuteall=%d\nonline=%d\nbotfill=%d\nteam=%s\nserver=%s\nsparcserver=%s\n",
                a.game.tun.advanced ? 1 : 0, a.game.tun.shieldPlacement, a.tunePitchDeg, a.tuneFwd, a.tuneUp, a.volume,
                a.pointerHand, a.fxLevel, a.hideOutside ? 1 : 0, a.voiceMode, a.muteSpectators ? 1 : 0, a.mutePlayers ? 1 : 0,
                a.muteAll ? 1 : 0, a.net.online() ? 1 : 0, a.botFill ? 1 : 0, a.team.c_str(), a.serverAddr.c_str(),
                a.sparcAddr.c_str());
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
        else if (k == "pitch") a.tunePitchDeg = clampf(v, -60, 60);
        else if (k == "fwd") a.tuneFwd = clampf(v, -0.2f, 0.2f);
        else if (k == "up") a.tuneUp = clampf(v, -0.2f, 0.2f);
        else if (k == "volume") a.volume = std::max(0, std::min(10, (int)v));
        else if (k == "hand") a.pointerHand = ((int)v) ? 1 : 0;
        else if (k == "fx") a.fxLevel = std::max(0, std::min(2, (int)v));
        else if (k == "hideoutside") a.hideOutside = v != 0;
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
    const char* ext = dataDir(a);
    if (!ext) return;
    std::string f = std::string(ext) + "/player_name.txt";
    char name[32] = {};
    if (FILE* fp = fopen(f.c_str(), "r")) {
        if (fgets(name, sizeof name, fp)) name[strcspn(name, "\r\n")] = 0;
        fclose(fp);
    }
    if (!name[0]) {
        snprintf(name, sizeof name, "PLAYER %03d", (int)(nowSeconds() * 1000.0) % 1000);
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

static void startUp(App& a) {
    logOpenFile(dataDir(a));
    a.net.init();
    loadPlayerName(a);
    loadAvatar(a);
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
    if (a.cfgVersion < 3 && a.tunePitchDeg == 0.f) {   // 0.3.1: the gauntlet's new default angle (saved 0 = never tuned)
        a.tunePitchDeg = -25.f;
        LOGI("settings: gauntlet pitch 0 -> -25 (new default)");
        saveSettings(a);
    }
    if (a.cfgVersion < 2 && a.sparcAddr != PA_SPARC_SERVER) {   // EXPERIMENTAL cross-play: test addresses from older builds
        LOGI("settings: cross-play server %s -> %s", a.sparcAddr.c_str(), PA_SPARC_SERVER[0] ? PA_SPARC_SERVER : "(none)");
        a.sparcAddr = PA_SPARC_SERVER;
        saveSettings(a);
    }
    a.net.setBotFill(a.botFill);
    if (a.onlinePref && !a.serverAddr.empty()) a.net.goOnline(a.serverAddr.c_str());
    if (!a.serverAddr.empty()) { std::vector<std::string> ob = bugOutbox(a); if (!ob.empty()) bugSendAll(a, ob); }   // reports saved earlier
    loadWarps(a);
    if (const char* ext = dataDir(a)) a.firstHelp = !fileExists(std::string(ext) + "/help_seen");
    if (const char* ext = dataDir(a)) a.devMode = fileExists(std::string(ext) + "/dev_mode");
    LOGI("developer mode %s", a.devMode ? "on" : "off");
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
struct ShotView {
    int w = 1280, h = 720; bool custom = false; Vec3 p; float yaw = 0, pitch = 0, fovY = 64;
    std::string title; float titleH = 0.12f, titleY = 0;   // title height (fraction of the image) and offset (-1..1)
    std::string menu;   // --menu title|xp: draw that menu page in front of the camera (layout checks)
    std::string scene;  // --scene action: two players mid-rally (store art)
};

static int selfTest(App& a, const char* out, const ShotView& v) {
    if (!initGraphics(a) || !initContent(a)) return 2;
    startUp(a);
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
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    Pose eye{Quat::axisAngle({0, 1, 0}, -1.5707963f), a.game.tun.start[0] + Vec3{-0.6f, 1.7f, 0}};   // facing the opponent (+X)
    if (v.custom) eye = {Quat::axisAngle({0, 1, 0}, deg2rad(v.yaw)) * Quat::axisAngle({1, 0, 0}, deg2rad(v.pitch)), v.p};
    const float ty = std::tan(deg2rad(v.fovY) * 0.5f), tx = ty * W / H;
    a.gfx.beginEye(Mat4::viewFromPose(eye), Mat4::projection(-std::atan(tx), std::atan(tx), std::atan(ty), -std::atan(ty), 0.05f, 2000.f), eye.p);
    if (v.scene == "meet") {   // the fist bump start: both players face to face in the centre, right fists out
        Game& g = a.game;
        g.match = MatchState::Meet;
        g.stateTimer = 7.f;
        a.viewing = true;
        const AvatarDesc looks[2] = {avatar::preset(10), avatar::preset(12)};
        for (int p = 0; p < 2; ++p) {
            const float side = p ? 1.f : -1.f;
            PlayerInput in;
            in.headTracked = true;
            const Quat q = Quat::axisAngle({0, 1, 0}, p ? kPi * 0.5f : -kPi * 0.5f);
            in.head = {q, Vec3{g.tun.meetSpot * side, 1.64f, 0.f}};
            in.avatar = looks[p];
            for (int h = 0; h < 2; ++h) in.hand[h].tracked = true;
            in.hand[1].grip = {q, in.head.p + q.rotate({0.10f, -0.42f, -0.36f})};
            in.hand[0].grip = {q, in.head.p + q.rotate({-0.22f, -0.62f, -0.08f})};
            a.viewIn[p] = in;
            a.viewInValid[p] = true;
            g.pl[p].in = in;
            g.pl[p].isBot = false;
        }
        // every kind of event through the real handlers (logs, sounds, haptics): a missing name once crashed both headsets
        for (int t = 0; t <= (int)EventType::FistBump; ++t) {
            GameEvent e{};
            e.type = (EventType)t; e.player = 0; e.hand = -1;
            g.events.push_back(e);
        }
        handleEvents(a);
        g.events.clear();
        LOGI("selftest: all %d event kinds handled", (int)EventType::FistBump + 1);
    }
    if (v.scene == "action") {   // store art: blue throws, orange raises a shield, the ball mid-court
        Game& g = a.game;
        g.match = MatchState::InProgress;
        g.pl[0].score = 3;
        g.pl[1].score = 2;
        g.roundClock = 84.f;
        a.viewing = true;
        const AvatarDesc looks[2] = {avatar::preset(10), avatar::preset(12)};   // CIRCUIT, TANK AND SHORTS
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
                in.hand[0].grip = {q * Quat::axisAngle({1, 0, 0}, -0.9f), in.head.p + q.rotate({-0.30f, -0.55f, -0.05f})};
            } else {        // shield up on the right arm, left fist guarding
                in.hand[1].grip = {q * Quat::axisAngle({1, 0, 0}, -0.15f), in.head.p + q.rotate({0.14f, -0.32f, -0.42f})};
                in.hand[0].grip = {q * Quat::axisAngle({1, 0, 0}, -0.45f), in.head.p + q.rotate({-0.22f, -0.42f, -0.32f})};
            }
            a.viewIn[p] = in;
            a.viewInValid[p] = true;
            g.pl[p].in = in;
            g.pl[p].isBot = false;
        }
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
    }
    drawWorld(a, 0);
    drawWorld(a, 1);
    if (!v.menu.empty()) {
        a.menuOpen = true;
        a.titleOpen = v.menu == "title";
        a.xpOpen = v.menu == "xp";
        a.settingsOpen = v.menu == "settings" || v.menu == "voice";
        a.voicePage = v.menu == "voice";
        a.helpOpen = v.menu == "help";
        if (v.menu == "name") { a.nameEdit = true; a.editTarget = 0; a.nameBuf = "MOJO"; }
        if (v.menu == "spectate") a.net.spectate();
        if (v.menu == "bug") { a.bugOpen = true; a.bugCat = 1; a.bugNote = "BALL WENT THROUGH THE WALL"; }
        if (v.menu == "rules") { a.rulesOpen = true; a.rulesForHost = true; a.game.tun.advanced = true; }
        if (v.menu == "report") {   // a room of example people (pictures of the page only)
            const char* nm[5] = {"MOJO", "SPARKY", "QUEST7", "ZAP_ZAP", "WATCHER"};
            const uint8_t ro[5] = {Net::VOICE_PLAYER, Net::VOICE_WAITING, Net::VOICE_WAITING, Net::VOICE_WAITING, Net::VOICE_SPECTATOR};
            for (int i = 0; i < 5; ++i) { Net::Person p; p.id = 0x1000u + i; p.role = ro[i]; std::snprintf(p.name, sizeof p.name, "%s", nm[i]); a.reportFake.push_back(p); }
            a.reportOpen = true; a.reportWho = 0x1001u; std::snprintf(a.reportName, sizeof a.reportName, "SPARKY"); a.reportWhy = 0;
            a.reportNote = "SHOUTING INSULTS IN THE WAITING AREA"; a.mutedIds.push_back(0x1001u);
        }
        a.menuPose = {eye.q, eye.transform({0, 0.09f, -0.95f})};
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
    glFinish();
    std::vector<uint8_t> px((size_t)W * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    const GLenum err = glGetError();
    FILE* f = fopen(out, "wb");
    if (!f) return 3;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = H - 1; y >= 0; --y)
        for (int x = 0; x < W; ++x) fwrite(&px[((size_t)y * W + x) * 4], 1, 3, f);
    fclose(f);
    LOGI("selftest: wrote %s (GL error 0x%x)", out, err);
    shutDown(a);
    return err == GL_NO_ERROR ? 0 : 4;
}

// PC: ProjectArena.exe, the "assets" folder beside it; player files in %APPDATA%\ProjectArena
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
        else if (!strcmp(argv[i], "--bug-report") && i + 2 < argc) { bugServer = argv[i + 1]; bugNoteArg = argv[i + 2]; i += 2; }
        else if (!strcmp(argv[i], "--player-report") && i + 2 < argc) { bugServer = argv[i + 1]; bugNoteArg = argv[i + 2]; i += 2; playerReport = true; }
        else if (!strcmp(argv[i], "--xc-probe") && i + 2 < argc) { probeAddr = argv[i + 1]; probeName = argv[i + 2]; i += 2; }
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) probeSeconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--no-dialog")) noDialog = true;
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
    if (!initGraphics(a) || !initInstance(a) || !initActions(a) || !initSession(a) || !initContent(a)) {
        LOGE("initialisation failed");
        if (!noDialog) MessageBoxA(a.hwnd, "Project Arena could not start VR.\n\nConnect your headset, start SteamVR (or Meta Quest Link), "
                            "then try again.\n\nDetails: %APPDATA%\\ProjectArena\\projectarena_log.txt", "Project Arena", MB_ICONERROR);
        return 1;
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
