// Windows: a minimal OpenXR loader. It finds the active runtime the way the Khronos loader does (the
// XR_RUNTIME_JSON environment variable, else HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime), loads the runtime DLL
// named by that manifest's "library_path", negotiates through xrNegotiateLoaderRuntimeInterface and forwards the
// OpenXR calls the game makes (xr_funcs.inc). API layers are not supported: the game needs none.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include "../log.h"

static PFN_xrGetInstanceProcAddr g_gipa = nullptr;
static XrInstance g_instance = XR_NULL_HANDLE;
static char g_runtimeName[512] = "";

static std::string activeRuntimeManifest() {
    char env[1024];
    DWORD n = GetEnvironmentVariableA("XR_RUNTIME_JSON", env, sizeof env);
    if (n > 0 && n < sizeof env) return env;
    char buf[1024];
    DWORD sz = sizeof buf;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1", "ActiveRuntime", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
                     nullptr, buf, &sz) == ERROR_SUCCESS)
        return buf;
    return "";
}

// "library_path": "..." from the runtime manifest (JSON string escapes: \\ and \/ are enough here)
static std::string libraryPath(const std::string& manifest) {
    FILE* f = fopen(manifest.c_str(), "rb");
    if (!f) return "";
    std::string txt;
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) txt.append(b, n);
    fclose(f);
    size_t k = txt.find("\"library_path\"");
    if (k == std::string::npos) return "";
    size_t q = txt.find('"', txt.find(':', k) + 1);
    if (q == std::string::npos) return "";
    std::string out;
    for (size_t i = q + 1; i < txt.size() && txt[i] != '"'; ++i) {
        if (txt[i] == '\\' && i + 1 < txt.size()) ++i;
        out += txt[i];
    }
    const bool absolute = out.size() > 1 && (out[1] == ':' || (out[0] == '\\' && out[1] == '\\'));
    if (!absolute) {
        size_t slash = manifest.find_last_of("\\/");
        out = (slash == std::string::npos ? std::string() : manifest.substr(0, slash + 1)) + out;
    }
    return out;
}

bool xrLoaderInit() {
    if (g_gipa) return true;
    const std::string manifest = activeRuntimeManifest();
    if (manifest.empty()) { LOGE("OpenXR: no active runtime (install SteamVR, Meta Quest Link or another OpenXR runtime)"); return false; }
    const std::string lib = libraryPath(manifest);
    HMODULE dll = lib.empty() ? nullptr : LoadLibraryA(lib.c_str());
    if (!dll) { LOGE("OpenXR: cannot load runtime %s (manifest %s)", lib.c_str(), manifest.c_str()); return false; }
    auto negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(dll, "xrNegotiateLoaderRuntimeInterface");
    if (!negotiate) { LOGE("OpenXR: %s has no xrNegotiateLoaderRuntimeInterface", lib.c_str()); return false; }
    XrNegotiateLoaderInfo li{};
    li.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    li.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
    li.structSize = sizeof li;
    li.minInterfaceVersion = 1;
    li.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    li.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    li.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
    XrNegotiateRuntimeRequest rr{};
    rr.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
    rr.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
    rr.structSize = sizeof rr;
    if (XR_FAILED(negotiate(&li, &rr)) || !rr.getInstanceProcAddr) { LOGE("OpenXR: runtime negotiation failed (%s)", lib.c_str()); return false; }
    g_gipa = rr.getInstanceProcAddr;
    snprintf(g_runtimeName, sizeof g_runtimeName, "%s", manifest.c_str());
    LOGI("OpenXR: runtime %s", manifest.c_str());
    return true;
}

static void xrLoaderGet(const char* name, PFN_xrVoidFunction* f) {
    *f = nullptr;
    if (g_gipa) g_gipa(g_instance, name, f);
}

static void xrLoaderSetInstance(XrInstance i) { g_instance = i; }

XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    if (!g_gipa) { *function = nullptr; return XR_ERROR_RUNTIME_UNAVAILABLE; }
    return g_gipa(instance, name, function);
}

#include "xr_funcs.inc"
