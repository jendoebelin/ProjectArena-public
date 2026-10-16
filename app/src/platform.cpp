#include "platform.h"
#include <cstdio>

#ifdef __ANDROID__
#include <android/asset_manager.h>
#include <unistd.h>

static AAssetManager* g_am = nullptr;

void platformSetAssets(void* am, const char*) { g_am = (AAssetManager*)am; }

bool assetRead(const std::string& path, std::vector<uint8_t>& out) {
    AAsset* a = g_am ? AAssetManager_open(g_am, path.c_str(), AASSET_MODE_BUFFER) : nullptr;
    if (!a) return false;
    size_t n = (size_t)AAsset_getLength(a);
    out.resize(n);
    AAsset_read(a, out.data(), n);
    AAsset_close(a);
    return true;
}

bool assetExists(const std::string& path) {
    AAsset* a = g_am ? AAssetManager_open(g_am, path.c_str(), AASSET_MODE_UNKNOWN) : nullptr;
    if (a) AAsset_close(a);
    return a != nullptr;
}

bool fileExists(const std::string& path) { return access(path.c_str(), F_OK) == 0; }
void fileRemove(const std::string& path) { unlink(path.c_str()); }

#else
#include <io.h>
#include <stdio.h>

static std::string g_dir = "assets";

void platformSetAssets(void*, const char* folder) { if (folder) g_dir = folder; }

bool assetRead(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen((g_dir + "/" + path).c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    size_t got = n > 0 ? fread(out.data(), 1, (size_t)n, f) : 0;
    fclose(f);
    out.resize(got);
    return true;
}

bool assetExists(const std::string& path) { return _access((g_dir + "/" + path).c_str(), 0) == 0; }
bool fileExists(const std::string& path) { return _access(path.c_str(), 0) == 0; }
void fileRemove(const std::string& path) { _unlink(path.c_str()); }
#endif
