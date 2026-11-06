#include "sound_studio.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include "log.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <direct.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace studio {

const std::vector<Section>& sections() {
    static const std::vector<Section> s = {
        {"SHIELD", {"ShieldBlock", "ShieldBreak", "Recharge"}, 21,
         "TAKE YOUR BALL: YOUR SHIELD COMES UP. THE COACH THROWS AT IT: THE BLOCK, THE BREAK, THEN THE RECHARGE"},
        {"THROWS", {"ThrowSlow", "ThrowMed", "ThrowFast", "ThrowFull", "ThrowFail", "TargetHit"}, 1,
         "THROW AT THE TARGETS, SLOW TO FULL SPEED; A THROW TOO WEAK TO FLY IS THE FAIL"},
        {"BALL", {"Spawn", "SpawnRise", "Grab", "Caught", "Bounce", "Returned"}, 8,
         "THROW AT THE FAR WALL AND CATCH YOUR BALL AS IT COMES BACK"},
        {"HITS", {"KnuckleBlock", "HitPlayerSelf", "HitPlayerOpponent", "Strike", "Foul"}, 23,
         "THE COACH THROWS AT YOU: PUNCH IT AWAY, OR LET ONE HIT YOU"},
        {"MATCH", {"Countdown", "RoundStart", "RoundEndWin", "RoundEndLose", "MatchEndWin", "MatchEndLose", "Overtime", "FistBump",
                   "LeadChange", "ClockTick"}, 0, "PRESS PLAY TO HEAR EACH ONE"},
        {"CROWD", {"Ambience", "Crowd", "Cheer"}, 0, "PRESS PLAY TO HEAR EACH ONE"},
    };
    return s;
}

std::string prettyEvent(const std::string& e) {
    std::string out;
    for (size_t i = 0; i < e.size(); ++i) {
        const char c = e[i];
        if (i && c >= 'A' && c <= 'Z' && !(e[i - 1] >= 'A' && e[i - 1] <= 'Z')) out += ' ';
        out += (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
    }
    return out;
}

namespace {
struct Slot { const char* event; const char* code; const char* name; };
// (the same list as tools/gen_catalog.py's SOUNDS; tools/run_studio_test.py checks they agree)
const Slot kSlots[] = {
        {"Spawn", "S01", "BALL APPEARS"},
        {"SpawnRise", "S02", "LAUNCHER RISES"},
        {"Grab", "S03", "GRAB"},
        {"Caught", "S04", "CATCH"},
        {"ThrowSlow", "S05", "THROW (SLOW)"},
        {"ThrowMed", "S06", "THROW (MEDIUM)"},
        {"ThrowFast", "S07", "THROW (FAST)"},
        {"ThrowFull", "S08", "THROW (FULL POWER)"},
        {"ThrowFail", "S09", "DROPPED THROW"},
        {"Bounce", "S10", "WALL BOUNCE"},
        {"ShieldBlock", "S11", "SHIELD BLOCK"},
        {"ShieldBreak", "S12", "SHIELD BREAK"},
        {"KnuckleBlock", "S13", "KNUCKLE PUNCH"},
        {"HitPlayerOpponent", "S14", "YOU SCORED"},
        {"HitPlayerSelf", "S15", "YOU WERE HIT"},
        {"Returned", "S16", "BALL RETURNS"},
        {"Foul", "S17", "FOUL"},
        {"Countdown", "S18", "COUNTDOWN BEEP"},
        {"RoundStart", "S19", "ROUND START"},
        {"RoundEndWin", "S20", "ROUND WON"},
        {"RoundEndLose", "S21", "ROUND LOST"},
        {"MatchEndWin", "S22", "MATCH WON"},
        {"MatchEndLose", "S23", "MATCH LOST"},
        {"Recharge", "S24", "RECHARGE"},
        {"Strike", "S25", "STRIKE"},
        {"Overtime", "S26", "SUDDEN DEATH"},
        {"Ambience", "S27", "ARENA AMBIENCE"},
        {"FistBump", "S28", "FIST BUMP"},
        {"Crowd", "S29", "CROWD"},
        {"Cheer", "S30", "CROWD CHEER"},
        {"ClockTick", "S31", "CLOCK TICK"},
        {"LeadChange", "S32", "LEAD CHANGE"},
        {"TargetHit", "S33", "TARGET HIT"},
};
const Slot* slotOf(const std::string& e) {
    for (const Slot& s : kSlots) if (e == s.event) return &s;
    return nullptr;
}
}  // namespace

std::string codeOf(const std::string& e) { const Slot* s = slotOf(e); return s ? s->code : ""; }
std::string slotName(const std::string& e) {
    const Slot* s = slotOf(e);
    return s ? std::string(s->code) + " " + s->name : prettyEvent(e);
}

std::string eventOf(const std::string& file) {
    const size_t k = file.find("__");
    return k == std::string::npos ? std::string() : file.substr(0, k);
}

std::string prettyName(const std::string& file) {
    const size_t k = file.find("__");
    std::string n = k == std::string::npos ? file : file.substr(k + 2);
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".wav") == 0) n.resize(n.size() - 4);
    for (char& c : n) c = c == '_' ? ' ' : (c >= 'a' && c <= 'z' ? (char)(c - 32) : c);
    return n;
}

// ---- SHA-256 (FIPS 180-4), for checking each download against the catalog
namespace {
const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
void block(uint32_t h[8], const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
}  // namespace

std::string sha256Hex(const uint8_t* data, size_t n) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t i = 0;
    for (; i + 64 <= n; i += 64) block(h, data + i);
    uint8_t tail[128] = {};
    const size_t r = n - i;
    std::memcpy(tail, data + i, r);
    tail[r] = 0x80;
    const size_t len = r + 1 + 8 <= 64 ? 64 : 128;
    const uint64_t bits = (uint64_t)n * 8;
    for (int k = 0; k < 8; ++k) tail[len - 1 - k] = (uint8_t)(bits >> (8 * k));
    block(h, tail);
    if (len == 128) block(h, tail + 64);
    char out[65];
    for (int k = 0; k < 8; ++k) std::snprintf(out + 8 * k, 9, "%08x", h[k]);
    return std::string(out, 64);
}

// ---- the library on the headset: the folder, and a list of what is in it (sound_library/library.txt)
namespace {
void makeDir(const std::string& p) {
#ifdef _WIN32
    _mkdir(p.c_str());
#else
    mkdir(p.c_str(), 0770);
#endif
}
bool readFile(const std::string& p, std::vector<uint8_t>& out) {
    out.clear();
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    uint8_t buf[16384];
    for (size_t k; (k = std::fread(buf, 1, sizeof buf, f)) > 0;) out.insert(out.end(), buf, buf + k);
    std::fclose(f);
    return true;
}
bool writeFile(const std::string& p, const std::vector<uint8_t>& d) {
    const std::string tmp = p + ".part";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(d.data(), 1, d.size(), f) == d.size();
    std::fclose(f);
    if (!ok) { std::remove(tmp.c_str()); return false; }
    std::remove(p.c_str());
    return std::rename(tmp.c_str(), p.c_str()) == 0;
}
std::vector<std::string> readList(const std::string& p) {
    std::vector<std::string> v;
    std::vector<uint8_t> d;
    if (!readFile(p, d)) return v;
    std::string s(d.begin(), d.end()), line;
    for (char c : s) {
        if (c == '\n') { if (!line.empty()) v.push_back(line); line.clear(); }
        else if (c != '\r') line += c;
    }
    if (!line.empty()) v.push_back(line);
    return v;
}
}  // namespace

void Library::sync(const std::string& server, const std::string& folder, const uint8_t key[16]) {
    if (state_ == LOADING) return;
    if (th_.joinable()) th_.join();
    state_ = LOADING;
    done_ = total_ = 0;
    folder_ = folder;
    th_ = std::thread(&Library::run, this, server, folder, std::vector<uint8_t>(key, key + 16));
}

bool Library::take(std::vector<Sound>& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!fresh_) return false;
    fresh_ = false;
    out = result_;
    return true;
}

void Library::run(std::string server, std::string folder, std::vector<uint8_t> key) {
    std::vector<LibSound> cat;
    std::string err;
    if (server.empty() || !fetchSoundCatalog(server.c_str(), key.data(), cat, err)) {
        std::lock_guard<std::mutex> lk(mu_);
        err_ = server.empty() ? "NO SERVER SET" : err;
        LOGW("sound studio: no library (%s)", err_.c_str());
        state_ = FAILED;
        return;
    }
    makeDir(folder);
    const std::string listPath = folder + "/library.txt";
    std::vector<std::string> had = readList(listPath), kept;
    for (const std::string& f : had)   // gone from the server: gone from here
        if (std::none_of(cat.begin(), cat.end(), [&](const LibSound& c) { return c.file == f; })) {
            std::remove((folder + "/" + f).c_str());
            LOGI("sound studio: %s left the library", f.c_str());
        }
    total_ = (int)cat.size();
    std::vector<Sound> out;
    int fetched = 0;
    for (const LibSound& c : cat) {
        Sound s;
        s.file = c.file; s.event = eventOf(c.file); s.name = prettyName(c.file); s.vote = c.vote;
        const std::string p = folder + "/" + c.file;
        std::vector<uint8_t> d;
        if (readFile(p, d) && d.size() == c.size && sha256Hex(d.data(), d.size()) == c.sha) s.ready = true;
        else {
            std::string e;
            if (fetchSound(server.c_str(), c.file, d, e) && d.size() == c.size && sha256Hex(d.data(), d.size()) == c.sha && writeFile(p, d)) {
                s.ready = true;
                ++fetched;
            } else {
                LOGW("sound studio: %s not downloaded (%s)", c.file.c_str(), e.empty() ? "its checksum doesn't match" : e.c_str());
            }
        }
        if (s.ready) kept.push_back(c.file);
        out.push_back(s);
        ++done_;
    }
    {
        std::string list;
        for (const std::string& f : kept) list += f + "\n";
        writeFile(listPath, std::vector<uint8_t>(list.begin(), list.end()));
    }
    LOGI("sound studio: %d sounds in the library (%d downloaded now)", (int)out.size(), fetched);
    std::lock_guard<std::mutex> lk(mu_);
    result_ = out;
    fresh_ = true;
    err_.clear();
    state_ = READY;
}

std::vector<int> choicesFor(const std::string& event, const std::vector<Sound>& lib) {
    std::vector<int> v{-1};
    for (int i = 0; i < (int)lib.size(); ++i) if (lib[i].ready && !lib[i].official && !lib[i].mine && lib[i].event == event) v.push_back(i);
    for (int i = 0; i < (int)lib.size(); ++i) if (lib[i].ready && lib[i].mine && lib[i].event == event) v.push_back(i);
    for (int i = 0; i < (int)lib.size(); ++i) if (lib[i].ready && lib[i].mine && lib[i].event.empty()) v.push_back(i);
    std::vector<int> rest, games;   // (the others, then the game's own; each in the catalog's order: S01 first)
    for (int i = 0; i < (int)lib.size(); ++i)
        if (lib[i].ready && lib[i].event != event && !(lib[i].mine && lib[i].event.empty())) (lib[i].official ? games : rest).push_back(i);
    auto byCode = [&](int a, int b) { return codeOf(lib[a].event) < codeOf(lib[b].event); };
    std::stable_sort(rest.begin(), rest.end(), byCode);
    std::stable_sort(games.begin(), games.end(), byCode);
    v.insert(v.end(), rest.begin(), rest.end());
    v.insert(v.end(), games.begin(), games.end());
    return v;
}

std::string slotFromName(const std::string& file) {
    // a code at the start (S12, s12, S2 for S02), then anything not a digit
    if (file.size() >= 2 && (file[0] == 'S' || file[0] == 's') && file[1] >= '0' && file[1] <= '9') {
        size_t k = 1;
        int num = 0;
        while (k < file.size() && k < 3 && file[k] >= '0' && file[k] <= '9') num = num * 10 + (file[k++] - '0');
        if (k == file.size() || !(file[k] >= '0' && file[k] <= '9')) {
            char code[8];
            std::snprintf(code, sizeof code, "S%02d", num);
            for (const Slot& s : kSlots) if (code == std::string(s.code)) return s.event;
        }
    }
    const size_t u = file.find("__");   // an event's name before "__"
    if (u != std::string::npos) {
        const std::string e = file.substr(0, u);
        for (const Slot& s : kSlots) if (e == s.event) return s.event;
    }
    return "";
}

std::vector<Sound> mySounds(const std::string& folder) {
    std::vector<std::string> names;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((folder + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(fd.cFileName); } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    if (DIR* d = opendir(folder.c_str())) {
        while (dirent* e = readdir(d)) if (e->d_name[0] != '.') names.push_back(e->d_name);
        closedir(d);
    }
#endif
    if (names.empty()) {   // (first time: the folder, and a note saying what goes in it)
        makeDir(folder);
        const std::string note =
            "SOUND STUDIO: your own sounds, to try on this headset (or PC) only.\n"
            "Put .wav files in this folder (WAV format only: export as WAV from Audacity or any sound editor).\n"
            "Name a file after its sound slot to put it first in that slot: S12_glass.wav or ShieldBreak__glass.wav\n"
            "(the slots S01-S33 are in docs/SOUND_STUDIO.md); any other name can be tried in every slot.\n"
            "Open the SOUND STUDIO again to see new files. To share a sound, propose it on GitHub or Discord.\n";
        writeFile(folder + "/README.txt", std::vector<uint8_t>(note.begin(), note.end()));
    }
    std::sort(names.begin(), names.end());
    std::vector<Sound> v;
    for (const std::string& n : names) {
        if (n.size() < 5) continue;
        std::string ext = n.substr(n.size() - 4);
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext != ".wav" || v.size() >= 60) continue;   // (WAV only)
        Sound s;
        s.file = "mine:" + n;
        s.path = folder + "/" + n;
        s.event = slotFromName(n);
        std::string label = n.substr(0, n.size() - 4);
        for (char& c : label) c = c == '_' ? ' ' : (char)std::toupper((unsigned char)c);
        if (label.size() > 22) label = label.substr(0, 21) + ".";
        s.name = label;
        s.ready = s.mine = true;
        v.push_back(s);
    }
    return v;
}

std::vector<Sound> officialSounds() {
    std::vector<Sound> v;
    for (const Slot& s : kSlots) {
        Sound o;
        o.file = std::string("official:") + s.event;
        o.event = s.event;
        o.name = std::string(s.code) + " " + s.name;
        o.ready = o.official = true;
        v.push_back(o);
    }
    return v;
}

}  // namespace studio
