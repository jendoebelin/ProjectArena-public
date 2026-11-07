// The Meta Horizon Store's entitlement check and in-app update: see entitlement.h. Meta's loader is opened at run time
// (dlopen), so the game links nothing of Meta's and builds without the SDK; only a store build ships the library.
// The calls (Meta's native Platform SDK, https://developers.meta.com/horizon/documentation/native/ps-entitlement-check/):
// ovr_PlatformInitializeAndroidAsynchronous(appId, activity, env), ovr_Entitlement_GetIsViewerEntitled(), then
// ovr_PopMessage() each frame until the entitlement answer arrives: not an error = the player owns the game.
// In-app update (https://developers.meta.com/horizon/documentation/unity/ps-in-app-update-apis/): ovr_Application_
// GetVersion (installed and latest), StartAppDownload (answers when the download is done), CheckAppDownloadProgress (on
// a timer meanwhile), InstallAppUpdateAndRelaunch (the headset closes the game, installs and opens it again).
#include "entitlement.h"

#include <string>

#if defined(__ANDROID__) && defined(PA_STORE)
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "log.h"

#ifndef PA_META_APP_ID
#error "a store build needs the app's ID: ./build.sh reads it from meta.local.cfg (app_id=...)"
#endif
#define PA_STR2(x) #x
#define PA_STR(x) PA_STR2(x)

namespace entitlement {
namespace {

typedef void* Msg;   // ovrMessageHandle
typedef uint64_t (*InitFn)(const char*, void*, void*);   // ovr_PlatformInitializeAndroidAsynchronous -> ovrRequest
typedef uint64_t (*ReqFn)();                             // ovr_Entitlement_GetIsViewerEntitled, ovr_Application_GetVersion, ...
typedef uint64_t (*ReqOptFn)(void*);                     // ovr_Application_InstallAppUpdateAndRelaunch(options)
typedef Msg (*PopFn)();
typedef uint32_t (*TypeFn)(Msg);                         // ovr_Message_GetType -> ovrMessageType
typedef const char* (*TypeNameFn)(uint32_t);             // ovrMessageType_ToString, ovrAppStatus_ToString, ...
typedef bool (*IsErrFn)(Msg);
typedef void* (*GetHFn)(Msg);                            // ovr_Message_GetError / GetPlatformInitialize / GetApplicationVersion ...
typedef const char* (*TextFn)(void*);                    // ovr_Error_GetMessage, ovr_ApplicationVersion_GetLatestName
typedef int (*IntFn)(void*);                             // ovr_PlatformInitialize_GetResult (0 = success), ..._GetCurrentCode
typedef long long (*I64Fn)(void*);                       // ovr_ApplicationVersion_GetSize, ovr_AppDownloadProgressResult_Get...Bytes
typedef void* (*CreateFn)();                             // ovr_ApplicationOptions_Create
typedef void (*FreeFn)(void*);                           // ovr_FreeMessage, ovr_ApplicationOptions_Destroy

// the two message types (OVR_MessageType.h); their names are asked of Meta's runtime first, these are the fallback
const uint32_t kMsgInitAsync = 0x1AD307B4u, kMsgEntitled = 0x186B58B1u;
const double kTimeout = 30.0;   // no answer by then: failed (a missing or broken Platform runtime must not mean "owned")

State g_state = State::Off;
std::string g_why;
double g_started = 0;
bool g_sdk = false;   // the library is open (its functions below are set)
PopFn pop; TypeFn type; TypeNameFn typeName; IsErrFn isErr; GetHFn getErr; TextFn errText; GetHFn getInit;
IntFn initRes; FreeFn freeMsg;
// in-app update
ReqFn appGetVersion, appStartDownload, appCheckProgress; ReqOptFn appInstall; CreateFn optCreate; FreeFn optDestroy;
GetHFn msgVersion, msgProgress; IntFn verCurrent, verLatest, progStatus; TextFn verLatestName; I64Fn verSize, progTotal, progDone;
TypeNameFn statusName;
Update g_up = Update::Unavailable;
std::string g_upName, g_upError;
long long g_upSize = 0, g_upDone = 0, g_upTotal = 0;
double g_upAsked = -1e9, g_progAsked = -1e9;
bool g_progOut = false;

double now() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

void fail(const std::string& why) {
    g_state = State::Failed;
    g_why = why;
    LOGE("entitlement: FAILED (%s)", why.c_str());
}

std::string errorOf(Msg m) {
    void* e = getErr ? getErr(m) : nullptr;
    const char* s = e && errText ? errText(e) : nullptr;
    return s && *s ? s : "no reason given";
}

bool isType(uint32_t t, const char* name, uint32_t fallback) {
    const char* n = typeName ? typeName(t) : nullptr;
    if (n && *n && strstr(n, name)) return true;
    return fallback && t == fallback;
}

void upFail(const std::string& why) {
    g_up = Update::Failed;
    g_upError = why;
    LOGE("store update: %s", why.c_str());
}

}  // namespace

void start(void* jniEnv, void* activity) {
    if (g_state != State::Off) return;
    g_state = State::Checking;
    g_started = now();
    void* lib = dlopen("libovrplatformloader.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fail(std::string("Meta's library not found: ") + (dlerror() ? dlerror() : "?")); return; }
    auto sym = [&](const char* n) { return dlsym(lib, n); };
    InitFn init = (InitFn)sym("ovr_PlatformInitializeAndroidAsynchronous");
    ReqFn entitled = (ReqFn)sym("ovr_Entitlement_GetIsViewerEntitled");
    pop = (PopFn)sym("ovr_PopMessage");
    type = (TypeFn)sym("ovr_Message_GetType");
    typeName = (TypeNameFn)sym("ovrMessageType_ToString");
    isErr = (IsErrFn)sym("ovr_Message_IsError");
    getErr = (GetHFn)sym("ovr_Message_GetError");
    errText = (TextFn)sym("ovr_Error_GetMessage");
    getInit = (GetHFn)sym("ovr_Message_GetPlatformInitialize");
    initRes = (IntFn)sym("ovr_PlatformInitialize_GetResult");
    freeMsg = (FreeFn)sym("ovr_FreeMessage");
    appGetVersion = (ReqFn)sym("ovr_Application_GetVersion");
    appStartDownload = (ReqFn)sym("ovr_Application_StartAppDownload");
    appCheckProgress = (ReqFn)sym("ovr_Application_CheckAppDownloadProgress");
    appInstall = (ReqOptFn)sym("ovr_Application_InstallAppUpdateAndRelaunch");
    optCreate = (CreateFn)sym("ovr_ApplicationOptions_Create");
    optDestroy = (FreeFn)sym("ovr_ApplicationOptions_Destroy");
    msgVersion = (GetHFn)sym("ovr_Message_GetApplicationVersion");
    msgProgress = (GetHFn)sym("ovr_Message_GetAppDownloadProgressResult");
    verCurrent = (IntFn)sym("ovr_ApplicationVersion_GetCurrentCode");
    verLatest = (IntFn)sym("ovr_ApplicationVersion_GetLatestCode");
    verLatestName = (TextFn)sym("ovr_ApplicationVersion_GetLatestName");
    verSize = (I64Fn)sym("ovr_ApplicationVersion_GetSize");
    progTotal = (I64Fn)sym("ovr_AppDownloadProgressResult_GetDownloadBytes");
    progDone = (I64Fn)sym("ovr_AppDownloadProgressResult_GetDownloadedBytes");
    progStatus = (IntFn)sym("ovr_AppDownloadProgressResult_GetStatusCode");
    statusName = (TypeNameFn)sym("ovrAppStatus_ToString");
    if (!init || !entitled || !pop || !type || !isErr || !freeMsg) { fail("Meta's library is missing a function"); return; }
    g_sdk = true;
    const uint64_t r = init(PA_STR(PA_META_APP_ID), activity, jniEnv);
    LOGI("entitlement: Platform SDK starting (request %llu)", (unsigned long long)r);
    const uint64_t q = entitled();   // (queued until the start-up finishes)
    LOGI("entitlement: asked (request %llu)", (unsigned long long)q);
}

State poll() {
    if (!g_sdk) return g_state;
    while (Msg m = pop()) {
        const uint32_t t = type(m);
        const char* n = typeName ? typeName(t) : nullptr;
        if (isType(t, "PlatformInitializeAndroidAsynchronous", kMsgInitAsync)) {
            const int res = (getInit && initRes) ? initRes(getInit(m)) : 0;
            if (isErr(m) || res != 0) {
                char b[160];
                snprintf(b, sizeof b, "the Platform SDK didn't start (result %d: %s)", res, isErr(m) ? errorOf(m).c_str() : "");
                fail(b);
            } else {
                LOGI("entitlement: Platform SDK started");
            }
        } else if (isType(t, "Entitlement_GetIsViewerEntitled", kMsgEntitled)) {
            if (isErr(m)) fail("not entitled: " + errorOf(m));
            else if (g_state == State::Checking) { g_state = State::Owned; LOGI("entitlement: owned (%.1f s)", now() - g_started); }
        } else if (isType(t, "Application_GetVersion", 0)) {
            if (isErr(m)) upFail("the store didn't say the latest version: " + errorOf(m));
            else if (void* v = msgVersion ? msgVersion(m) : nullptr) {
                const int cur = verCurrent ? verCurrent(v) : 0, latest = verLatest ? verLatest(v) : 0;
                const char* name = verLatestName ? verLatestName(v) : nullptr;
                g_upName = name ? name : "";
                g_upSize = verSize ? verSize(v) : 0;
                g_up = latest > cur ? Update::Available : Update::UpToDate;
                LOGI("store update: installed %d, latest %d (%s, %lld bytes)", cur, latest, g_upName.c_str(), g_upSize);
            }
        } else if (isType(t, "Application_StartAppDownload", 0)) {   // (answers when the download is done)
            if (isErr(m)) upFail("the download didn't finish: " + errorOf(m));
            else { g_up = Update::Downloaded; LOGI("store update: downloaded"); }
        } else if (isType(t, "Application_CheckAppDownloadProgress", 0)) {
            g_progOut = false;
            if (!isErr(m) && g_up == Update::Downloading)
                if (void* p = msgProgress ? msgProgress(m) : nullptr) {
                    g_upTotal = progTotal ? progTotal(p) : 0;
                    g_upDone = progDone ? progDone(p) : 0;
                    const char* st = statusName && progStatus ? statusName(progStatus(p)) : nullptr;
                    LOGI("store update: %lld / %lld bytes (%s)", g_upDone, g_upTotal, st ? st : "?");
                }
        } else if (isType(t, "Application_InstallAppUpdateAndRelaunch", 0)) {
            if (isErr(m)) upFail("the install didn't start: " + errorOf(m));   // (on success the headset closes the game)
        } else {
            LOGI("entitlement: message 0x%08x (%s)", t, n ? n : "?");
        }
        freeMsg(m);
    }
    if (g_up == Update::Downloading && !g_progOut && appCheckProgress && now() - g_progAsked > 0.5) {
        g_progAsked = now();
        g_progOut = true;
        appCheckProgress();
    }
    if (g_state == State::Checking && now() - g_started > kTimeout) fail("no answer from Meta in 30 s");
    return g_state;
}

const char* why() { return g_why.c_str(); }

void updateCheck() {
    if (g_state != State::Owned || !appGetVersion) return;
    if (g_up == Update::Downloading || g_up == Update::Downloaded || g_up == Update::Installing) return;
    if (g_up != Update::Failed && now() - g_upAsked < 30.0) return;   // (asked lately: the answer stands)
    g_upAsked = now();
    g_up = Update::Checking;
    appGetVersion();
}

void updateDownload() {
    if (g_up != Update::Available || !appStartDownload) return;
    g_up = Update::Downloading;
    g_upDone = 0;
    g_upTotal = g_upSize;
    LOGI("store update: downloading %s", g_upName.c_str());
    appStartDownload();
}

void updateInstall() {
    if (g_up != Update::Downloaded || !appInstall) return;
    g_up = Update::Installing;
    LOGI("store update: installing and restarting");
    void* opt = optCreate ? optCreate() : nullptr;
    appInstall(opt);
    if (opt && optDestroy) optDestroy(opt);
}

Update updateState() { return g_state == State::Owned && appGetVersion ? g_up : Update::Unavailable; }
const char* updateLatest() { return g_upName.c_str(); }
double updateSizeMB() { return (double)(g_upTotal > 0 ? g_upTotal : g_upSize) / 1e6; }
float updateProgress() { return g_upTotal > 0 ? (float)((double)g_upDone / (double)g_upTotal) : 0.f; }
const char* updateError() { return g_upError.c_str(); }

}  // namespace entitlement

#else   // every other build: no check, no in-app update

namespace entitlement {
void start(void*, void*) {}
State poll() { return State::Off; }
const char* why() { return ""; }
void updateCheck() {}
void updateDownload() {}
void updateInstall() {}
Update updateState() { return Update::Unavailable; }
const char* updateLatest() { return ""; }
double updateSizeMB() { return 0; }
float updateProgress() { return 0; }
const char* updateError() { return ""; }
}  // namespace entitlement

#endif
