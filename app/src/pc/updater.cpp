// UPDATE NOW on PC (see updater.h). WinHTTP for the downloads (HTTPS; GitHub's redirect to its file host is followed),
// BCrypt for the SHA-256, tar.exe (in Windows since 2018) for the zip. Paths as Windows has them (wide): a user name or
// folder with accents or symbols must still work (review, 2026-11-02).
#include "updater.h"
#include "../log.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwctype>
#include <mutex>
#include <thread>
#include <vector>

namespace updater {
namespace {
const wchar_t* kHost = L"github.com";
const char* kRepoPath = "/jendoebelin/ProjectArena-public/releases";
std::atomic<int> st{IDLE};
std::atomic<float> prog{0};
std::mutex mu;
std::string err;                         // FAILED: why
std::wstring unpacked, tmpDir, tmpZip;   // READY: the new game's folder; what to clear away after (the download, its copy)

std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }   // (ASCII: URLs, versions)
std::string utf8(const std::wstring& w) {
    if (w.empty()) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring exeDir() {
    std::wstring p(32768, 0);
    p.resize(GetModuleFileNameW(nullptr, &p[0], (DWORD)p.size()));
    return p.substr(0, p.find_last_of(L"\\/"));
}
std::wstring tempDir() {
    wchar_t t[MAX_PATH + 1];
    const DWORD n = GetTempPathW(MAX_PATH + 1, t);
    return std::wstring(t, n);
}
std::string forCmd(const std::wstring& path) {   // a path for the script: UTF-8 (the script switches cmd to it), % doubled
    std::string o;
    for (char c : utf8(path)) { o += c; if (c == '%') o += '%'; }
    return o;
}
void fail(const std::string& why) { std::lock_guard<std::mutex> lk(mu); err = why; st = FAILED; LOGW("update: %s", why.c_str()); }

// GET https://github.com<path> (and wherever it redirects) into `out`; progress while it comes
bool get(const std::string& path, std::vector<char>& out, bool showProgress) {
    out.clear();
    HINTERNET s = WinHttpOpen(L"ProjectArena-Updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    bool ok = false;
    HINTERNET c = WinHttpConnect(s, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", widen(path).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
        DWORD code = 0, len = sizeof code;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
        DWORD total = 0;
        len = sizeof total;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &total, &len, WINHTTP_NO_HEADER_INDEX);
        if (code == 200) {
            ok = true;
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(r, &avail)) { ok = false; break; }
                if (!avail) break;
                const size_t at = out.size();
                out.resize(at + avail);
                DWORD got = 0;
                if (!WinHttpReadData(r, out.data() + at, avail, &got)) { ok = false; break; }
                out.resize(at + got);
                if (showProgress && total) prog = std::min(1.f, (float)out.size() / (float)total);
                if (out.size() > (400u << 20)) { ok = false; break; }   // (no release is near this)
            }
        } else {
            LOGW("update: %s answered %lu", path.c_str(), code);
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

std::string sha256(const std::vector<char>& d) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    unsigned char h[32] = {};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return "";
    BCryptHash(alg, nullptr, 0, (PUCHAR)d.data(), (ULONG)d.size(), h, 32);
    BCryptCloseAlgorithmProvider(alg, 0);
    char hex[65];
    for (int i = 0; i < 32; ++i) std::snprintf(hex + 2 * i, 3, "%02x", h[i]);
    return hex;
}

bool run(const std::wstring& cmd, DWORD& code) {   // a console tool, hidden, waited for
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring c = cmd;
    if (!CreateProcessW(nullptr, &c[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 120000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return true;
}

void work(std::string ver) {
    st = DOWNLOADING; prog = 0;
    const std::string zipName = "ProjectArena-" + ver + "-PC.zip";
    const std::string base = std::string(kRepoPath) + "/download/v" + ver + "/";
    std::vector<char> sums, zip;
    if (!get(base + "SHA256SUMS.txt", sums, false)) { fail("COULD NOT REACH GITHUB"); return; }
    if (!get(base + zipName, zip, true)) { fail("THE DOWNLOAD FAILED"); return; }
    st = CHECKING;
    const std::string list(sums.begin(), sums.end()), have = sha256(zip);
    // (sha256sum writes "hash  name", or "hash *name" in binary mode: the releases' list is the second)
    if (have.empty() || (list.find(have + "  " + zipName) == std::string::npos && list.find(have + " *" + zipName) == std::string::npos)) {
        fail("THE DOWNLOAD DID NOT MATCH ITS CHECKSUM");
        return;
    }
    st = UNPACKING;
    const std::wstring dir = tempDir() + L"ProjectArenaUpdate-" + widen(ver), zipPath = dir + L".zip";
    if (FILE* f = _wfopen(zipPath.c_str(), L"wb")) { std::fwrite(zip.data(), 1, zip.size(), f); std::fclose(f); }
    else { fail("COULD NOT SAVE THE DOWNLOAD"); return; }
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    DWORD code = 1;
    if (!run(L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + zipPath + L"\" -C \"" + dir + L"\"", code) || code != 0) { fail("COULD NOT UNPACK THE DOWNLOAD"); return; }
    if (GetFileAttributesW((dir + L"\\ProjectArena\\ProjectArena.exe").c_str()) == INVALID_FILE_ATTRIBUTES) { fail("THE DOWNLOAD HAD NO GAME IN IT"); return; }
    { std::lock_guard<std::mutex> lk(mu); unpacked = dir + L"\\ProjectArena"; tmpDir = dir; tmpZip = zipPath; }
    LOGI("update: %s downloaded, checked and unpacked (%zu bytes)", ver.c_str(), zip.size());
    st = READY;
}
}   // namespace

int how(std::string& why) {
    static int known = -1;   // (worked out once: the menus ask every frame, and the answer doesn't change)
    static std::string knownWhy;
    if (known < 0) {
        const std::wstring d = exeDir();
        std::wstring low = d;
        std::transform(low.begin(), low.end(), low.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
        HANDLE h = INVALID_HANDLE_VALUE;
        if (low.find(L"steamapps") != std::wstring::npos || GetEnvironmentVariableW(L"SteamAppId", nullptr, 0) > 0) {
            known = 1;
            knownWhy = "STEAM UPDATES THIS COPY: RESTART STEAM IF IT HAS NOT";
        } else if ((h = CreateFileW((d + L"\\update_probe.tmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_FLAG_DELETE_ON_CLOSE, nullptr)) == INVALID_HANDLE_VALUE) {   // (can the game write over itself?)
            known = 2;
            knownWhy = "THIS FOLDER IS PROTECTED: DOWNLOAD THE NEW ZIP FROM THE PAGE";
        } else {
            CloseHandle(h);
            known = 0;
        }
    }
    why = knownWhy;
    return known;
}
void start(const std::string& version) {
    if (st == DOWNLOADING || st == CHECKING || st == UNPACKING) return;
    { std::lock_guard<std::mutex> lk(mu); err.clear(); unpacked.clear(); }
    st = DOWNLOADING;
    std::thread(work, version).detach();   // (on its own: quitting mid-download must not wait for it, or abort)
}
State state() { return (State)st.load(); }
float progress() { return prog; }
std::string error() { std::lock_guard<std::mutex> lk(mu); return err; }
bool restart() {
    std::wstring from, dir, zip;
    { std::lock_guard<std::mutex> lk(mu); from = unpacked; dir = tmpDir; zip = tmpZip; }
    if (st != READY || from.empty()) return false;
    const std::wstring to = exeDir(), script = tempDir() + L"ProjectArenaUpdate.cmd";
    FILE* f = _wfopen(script.c_str(), L"wb");
    if (!f) return false;
    // waits for this game to close, copies the new files over the old (your settings and saves stay), starts it again,
    // then clears away the download, its unpacked copy, and itself. UTF-8 (chcp 65001) so any path works; % doubled.
    // (ping waits the second: timeout needs a console, and this runs without one)
    const unsigned long pid = GetCurrentProcessId();
    std::fprintf(f, "@echo off\r\nchcp 65001 >nul\r\n:wait\r\ntasklist /FI \"PID eq %lu\" | find \"%lu\" >nul && (ping -n 2 127.0.0.1 >nul & goto wait)\r\n",
                 pid, pid);
    std::fprintf(f, "robocopy \"%s\" \"%s\" /E /R:5 /W:1 /NFL /NDL /NJH /NJS /NP >nul\r\n", forCmd(from).c_str(), forCmd(to).c_str());
    std::fprintf(f, "start \"\" \"%s\\ProjectArena.exe\"\r\n", forCmd(to).c_str());
    std::fprintf(f, "rmdir /s /q \"%s\" >nul 2>&1\r\ndel /q \"%s\" >nul 2>&1\r\n", forCmd(dir).c_str(), forCmd(zip).c_str());
    std::fprintf(f, "(goto) 2>nul & del \"%%~f0\"\r\n");
    std::fclose(f);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"cmd.exe /c \"" + script + L"\"";
    // (out of any job the game runs in, where Windows allows it: closing the game must not close the script with it)
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &si, &pi) &&
        !CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    LOGI("update: closing to put the new version in place");
    return true;
}
void openPage() {
    ShellExecuteA(nullptr, "open", "https://github.com/jendoebelin/ProjectArena-public/releases/latest", nullptr, nullptr, SW_SHOWNORMAL);
}
}   // namespace updater
