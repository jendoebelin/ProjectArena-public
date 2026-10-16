// What differs between the Quest (Android) and PC (Windows) builds outside the OpenXR / GL setup in main.cpp:
// where the game assets come from, and small file helpers for the per-player data folder.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Quest: the APK's AAssetManager. PC: the "assets" folder next to ProjectArena.exe.
void platformSetAssets(void* androidAssetManager, const char* folder);
bool assetRead(const std::string& path, std::vector<uint8_t>& out);
bool assetExists(const std::string& path);

bool fileExists(const std::string& path);
void fileRemove(const std::string& path);
