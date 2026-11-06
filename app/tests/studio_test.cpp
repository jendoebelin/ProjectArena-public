// SOUND STUDIO's library on a headset (app/src/sound_studio.cpp), against a private relay serving a sound library:
// SHA-256, the catalog and downloads, checked files kept, a sound removed on the server removed here, the choices'
// order. Build / run: tools/run_studio_test.py
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include "../src/sound_studio.h"
#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#endif

bool netAcquireMulticastLock() { return false; }

static int g_fail = 0, g_pass = 0;
#define CHECK(c, msg) do { if (c) { ++g_pass; std::printf("  ok   %s\n", msg); } else { ++g_fail; std::printf("  FAIL %s  (line %d)\n", msg, __LINE__); } } while (0)

static bool exists(const std::string& p) { FILE* f = std::fopen(p.c_str(), "rb"); if (f) std::fclose(f); return f != nullptr; }

static bool syncNow(studio::Library& lib, const std::string& server, const std::string& folder, std::vector<studio::Sound>& out) {
    const uint8_t key[16] = {7, 7, 7};
    lib.sync(server, folder, key);
    for (int i = 0; i < 200 && lib.state() == studio::Library::LOADING; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return lib.take(out);
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 4) { std::printf("usage: studio_test SERVER FOLDER SERVER_LIBRARY\n"); return 2; }
    const std::string server = argv[1], folder = argv[2], serverLib = argv[3];
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    std::printf("[SHA-256]\n");
    const char* abc = "abc";
    CHECK(studio::sha256Hex((const uint8_t*)abc, 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "the standard test: abc");
    std::string a55(55, 'a'), a56(56, 'a'), a200(200, 'a');
    CHECK(studio::sha256Hex((const uint8_t*)a55.data(), 55) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318" &&
          studio::sha256Hex((const uint8_t*)a56.data(), 56) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a" &&
          studio::sha256Hex((const uint8_t*)a200.data(), 200) == "c2a908d98f5df987ade41b5fce213067efbcc21ef2240212a41e54b5e7c28ae5",
          "the padding's edges (55, 56 bytes) and several blocks");
    std::printf("[names]\n");
    CHECK(studio::prettyEvent("ShieldBreak") == "SHIELD BREAK" && studio::prettyEvent("HitPlayerSelf") == "HIT PLAYER SELF", "an event's name, spaced");
    CHECK(studio::prettyName("ShieldBreak__glass_crack.wav") == "GLASS CRACK" && studio::eventOf("ShieldBreak__glass_crack.wav") == "ShieldBreak",
          "a sound's name and its event, from its file name");
    CHECK(studio::slotName("ShieldBreak") == "S12 SHIELD BREAK" && studio::codeOf("TargetHit") == "S33" &&
          studio::slotName("HitPlayerOpponent") == "S14 YOU SCORED", "each event's slot: the asset catalog's code and name");
    std::printf("[MINE: your own .wav files]\n");
    {
        const std::string mine = folder + "_mine";
        std::vector<studio::Sound> none = studio::mySounds(mine);   // (made, with its README, the first time)
        CHECK(none.empty() && exists(mine + "/README.txt"), "the folder is made the first time, with a README saying what goes in it");
        const unsigned char wav[46] = {'R','I','F','F',38,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,1,0,0x44,0xAC,0,0,0x88,0x58,1,0,2,0,16,0,
                                       'd','a','t','a',2,0,0,0,0,0};
        for (const char* n : {"S12_glass.wav", "ShieldBreak__deep.wav", "random noise.wav", "S2_rise.WAV", "notes.txt", "song.mp3"}) {
            FILE* f = std::fopen((mine + "/" + n).c_str(), "wb");
            if (f) { std::fwrite(wav, 1, sizeof wav, f); std::fclose(f); }
        }
        std::vector<studio::Sound> m = studio::mySounds(mine);
        auto find = [&](const char* n) { for (const studio::Sound& s : m) if (s.file == std::string("mine:") + n) return &s; return (const studio::Sound*)nullptr; };
        CHECK(m.size() == 4 && !find("notes.txt") && !find("song.mp3") && !find("README.txt"), "only .wav files count (any case), not MP3s or notes");
        CHECK(find("S12_glass.wav") && find("S12_glass.wav")->event == "ShieldBreak" && find("ShieldBreak__deep.wav")->event == "ShieldBreak" &&
              find("S2_rise.WAV")->event == "SpawnRise" && find("random noise.wav")->event.empty() && find("random noise.wav")->name == "RANDOM NOISE",
              "a name starting with a slot's code or event puts it in that slot (S2 is S02); any other name has none");
        std::vector<studio::Sound> all = m;
        for (const studio::Sound& o : studio::officialSounds()) all.push_back(o);
        const auto c = studio::choicesFor("ShieldBreak", all);
        bool ord = c.size() == 4 + 1 + 32 && c[0] == -1 && all[c[1]].mine && all[c[1]].event == "ShieldBreak" && all[c[2]].mine &&
                   all[c[2]].event == "ShieldBreak" && all[c[3]].mine && all[c[3]].event.empty() && all[c[4]].mine && all[c[4]].event == "SpawnRise" &&
                   all[c[5]].official;
        CHECK(ord, "a slot's choices: OFFICIAL, yours for it, yours without a slot, yours for other slots, then the game's own");
    }
    std::printf("[the library]\n");
    studio::Library lib;
    std::vector<studio::Sound> got;
    const bool ok = syncNow(lib, server, folder, got);
    CHECK(ok && lib.state() == studio::Library::READY && got.size() == 4, "the catalog: the four sounds on the server");
    bool all = true;
    for (const studio::Sound& s : got) all = all && s.ready && exists(folder + "/" + s.file);
    CHECK(all, "each downloaded and checked against its SHA-256");
    const auto ch = studio::choicesFor("ShieldBreak", got);
    bool order = ch.size() == 5 && ch[0] == -1;
    for (size_t i = 1; i < ch.size(); ++i) order = order && (i <= 2) == (got[ch[i]].event == "ShieldBreak");
    CHECK(order, "an event's choices: OFFICIAL, then the sounds made for it, then the others");
    {   // with the game's own sounds added: they come after every library sound, and its own slot's isn't listed again
        std::vector<studio::Sound> all = got;
        for (const studio::Sound& o : studio::officialSounds()) all.push_back(o);
        const auto c2 = studio::choicesFor("ShieldBreak", all);
        bool ok2 = c2.size() == 5 + 32 && c2[0] == -1;
        for (size_t i = 1; i < c2.size(); ++i) ok2 = ok2 && all[c2[i]].official == (i >= 5) && all[c2[i]].event != (all[c2[i]].official ? "ShieldBreak" : "");
        CHECK(ok2 && studio::officialSounds().size() == 33 && all[c2[5]].name == "S01 BALL APPEARS",
              "the game's own sounds of the other 32 slots come last, in the catalog's order");
        const auto c3 = studio::choicesFor("Overtime", studio::officialSounds());
        CHECK(c3.size() == 33 && c3[0] == -1, "with an empty library, every slot can still try the game's 32 other sounds");
    }
    // a sound removed on the server goes from the headset
    std::string gone = serverLib + "/ShieldBlock__thud_test.wav";
    std::rename(gone.c_str(), (serverLib + "/removed/ShieldBlock__thud_test.wav").c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    std::vector<studio::Sound> got2;
    syncNow(lib, server, folder, got2);
    CHECK(got2.size() == 3 && !exists(folder + "/ShieldBlock__thud_test.wav"), "a sound taken off the server is deleted here too");
    // a damaged copy here is fetched again
    {
        FILE* f = std::fopen((folder + "/Recharge__chime_test.wav").c_str(), "r+b");
        if (f) { std::fseek(f, 100, SEEK_SET); std::fputc(0x55, f); std::fclose(f); }
    }
    std::vector<studio::Sound> got3;
    syncNow(lib, server, folder, got3);
    bool fine = got3.size() == 3;
    for (const studio::Sound& s : got3) fine = fine && s.ready;
    CHECK(fine, "a damaged copy fails its checksum and is downloaded again");
    std::rename((serverLib + "/removed/ShieldBlock__thud_test.wav").c_str(), gone.c_str());
    studio::Library none;
    std::vector<studio::Sound> got4;
    syncNow(none, "127.0.0.1:9", folder + "_x", got4);
    CHECK(none.state() == studio::Library::FAILED && !none.error().empty(), "no server: the studio says why");
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
