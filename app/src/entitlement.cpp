// The Meta Horizon Store's entitlement check: see entitlement.h. Meta's loader is opened at run time (dlopen), so the
// game links nothing of Meta's and builds without the SDK; only a store build ships the library and runs the check.
// The calls (Meta's native Platform SDK, https://developers.meta.com/horizon/documentation/native/ps-entitlement-check/):
// ovr_PlatformInitializeAndroidAsynchronous(appId, activity, env), ovr_Entitlement_GetIsViewerEntitled(), then
// ovr_PopMessage() each frame until the entitlement answer arrives: not an error = the player owns the game.
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
typedef uint64_t (*ReqFn)();                             // ovr_Entitlement_GetIsViewerEntitled
typedef Msg (*PopFn)();
typedef uint32_t (*TypeFn)(Msg);                         // ovr_Message_GetType -> ovrMessageType
typedef const char* (*TypeNameFn)(uint32_t);             // ovrMessageType_ToString
typedef bool (*IsErrFn)(Msg);
typedef void* (*GetErrFn)(Msg);                          // ovr_Message_GetError -> ovrErrorHandle
typedef const char* (*ErrTextFn)(void*);                 // ovr_Error_GetMessage
typedef void* (*GetInitFn)(Msg);                         // ovr_Message_GetPlatformInitialize
typedef int (*InitResFn)(void*);                         // ovr_PlatformInitialize_GetResult (0 = success)
typedef void (*FreeFn)(Msg);

// the two message types (OVR_MessageType.h); their names are asked of Meta's runtime first, these are the fallback
const uint32_t kMsgInitAsync = 0x1AD307B4u, kMsgEntitled = 0x186B58B1u;
const double kTimeout = 30.0;   // no answer by then: failed (a missing or broken Platform runtime must not mean "owned")

State g_state = State::Off;
std::string g_why;
double g_started = 0;
PopFn pop; TypeFn type; TypeNameFn typeName; IsErrFn isErr; GetErrFn getErr; ErrTextFn errText; GetInitFn getInit;
InitResFn initRes; FreeFn freeMsg;

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
    return t == fallback;
}

}  // namespace

void start(void* jniEnv, void* activity) {
    if (g_state != State::Off) return;
    g_state = State::Checking;
    g_started = now();
    void* lib = dlopen("libovrplatformloader.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fail(std::string("Meta's library not found: ") + (dlerror() ? dlerror() : "?")); return; }
    InitFn init = (InitFn)dlsym(lib, "ovr_PlatformInitializeAndroidAsynchronous");
    ReqFn entitled = (ReqFn)dlsym(lib, "ovr_Entitlement_GetIsViewerEntitled");
    pop = (PopFn)dlsym(lib, "ovr_PopMessage");
    type = (TypeFn)dlsym(lib, "ovr_Message_GetType");
    typeName = (TypeNameFn)dlsym(lib, "ovrMessageType_ToString");
    isErr = (IsErrFn)dlsym(lib, "ovr_Message_IsError");
    getErr = (GetErrFn)dlsym(lib, "ovr_Message_GetError");
    errText = (ErrTextFn)dlsym(lib, "ovr_Error_GetMessage");
    getInit = (GetInitFn)dlsym(lib, "ovr_Message_GetPlatformInitialize");
    initRes = (InitResFn)dlsym(lib, "ovr_PlatformInitialize_GetResult");
    freeMsg = (FreeFn)dlsym(lib, "ovr_FreeMessage");
    if (!init || !entitled || !pop || !type || !isErr || !freeMsg) { fail("Meta's library is missing a function"); return; }
    const uint64_t r = init(PA_STR(PA_META_APP_ID), activity, jniEnv);
    LOGI("entitlement: Platform SDK starting (request %llu)", (unsigned long long)r);
    const uint64_t q = entitled();   // (queued until the start-up finishes)
    LOGI("entitlement: asked (request %llu)", (unsigned long long)q);
}

State poll() {
    if (g_state != State::Checking) return g_state;
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
            else { g_state = State::Owned; LOGI("entitlement: owned (%.1f s)", now() - g_started); }
        } else {
            LOGI("entitlement: message 0x%08x (%s)", t, n ? n : "?");
        }
        freeMsg(m);
        if (g_state != State::Checking) return g_state;
    }
    if (now() - g_started > kTimeout) fail("no answer from Meta in 30 s");
    return g_state;
}

const char* why() { return g_why.c_str(); }

}  // namespace entitlement

#else   // every other build: no check

namespace entitlement {
void start(void*, void*) {}
State poll() { return State::Off; }
const char* why() { return ""; }
}  // namespace entitlement

#endif
