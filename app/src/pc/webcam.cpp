// The selfie overlay's webcam (see webcam.h): Media Foundation's source reader, asked for RGB32 (it converts what the
// camera gives), read in a loop on its own thread.
#include "webcam.h"
#include "../log.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

namespace webcam {
namespace {
std::mutex mu;
std::vector<uint8_t> frame;
int fw = 0, fh = 0;
uint32_t frameNo = 0;
std::string why = "OFF";
std::atomic<bool> running{false}, round_{true};
std::atomic<int> count{0};
std::thread worker;
// (review 2026-11-02: a stalled camera must not freeze the window: a stop waits a moment for the thread, then lets it
// finish on its own; each start is a new generation, and a thread from an older one changes nothing)
std::atomic<int> gen{0};
std::shared_ptr<std::atomic<bool>> workerDone;

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

thread_local int tGen = -1;   // (the generation this thread belongs to: an older one's troubles aren't shown)
void fail(const char* w) { std::lock_guard<std::mutex> lk(mu); if (tGen == gen) why = w; LOGW("webcam: %s", w); }

void loop(int device, int myGen, std::shared_ptr<std::atomic<bool>> done) {
    tGen = myGen;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    IMFAttributes* attr = nullptr;
    IMFActivate** devs = nullptr;
    UINT32 n = 0;
    IMFMediaSource* src = nullptr;
    IMFSourceReader* reader = nullptr;
    do {
        if (FAILED(MFCreateAttributes(&attr, 1)) ||
            FAILED(attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)) ||
            FAILED(MFEnumDeviceSources(attr, &devs, &n))) { fail("NO CAMERA ACCESS"); break; }
        count = (int)n;
        if (n == 0) { fail("NO CAMERA FOUND"); break; }
        const UINT32 pick = (UINT32)((device % (int)n + (int)n) % (int)n);
        if (FAILED(devs[pick]->ActivateObject(IID_PPV_ARGS(&src)))) { fail("CAMERA IN USE OR BLOCKED"); break; }
        IMFAttributes* ra = nullptr;
        MFCreateAttributes(&ra, 1);
        ra->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);   // (any camera format turned into RGB32)
        const HRESULT hr = MFCreateSourceReaderFromMediaSource(src, ra, &reader);
        release(ra);
        if (FAILED(hr)) { fail("CAMERA IN USE OR BLOCKED"); break; }
        IMFMediaType* mt = nullptr;
        MFCreateMediaType(&mt);
        mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        const HRESULT st = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mt);
        release(mt);
        if (FAILED(st)) { fail("CAMERA FORMAT NOT SUPPORTED"); break; }
        IMFMediaType* cur = nullptr;
        UINT32 w = 0, h = 0;
        INT32 stride = 0;
        if (SUCCEEDED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur))) {
            MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h);
            UINT32 s = 0;
            stride = SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &s)) ? (INT32)s : (INT32)(w * 4);
            release(cur);
        }
        if (!w || !h) { fail("CAMERA FORMAT NOT SUPPORTED"); break; }
        { std::lock_guard<std::mutex> lk(mu); if (tGen == gen) why.clear(); }
        LOGI("webcam: camera %u of %u, %ux%u", pick + 1, n, w, h);
        const int step = (int)((w + 639) / 640);   // (at most 640 wide: a corner of the picture needs no more)
        const int ow = (int)w / step, oh = (int)h / step;
        std::vector<uint8_t> out((size_t)ow * oh * 4);
        while (running && gen == myGen) {
            DWORD idx = 0, flags = 0;
            LONGLONG ts = 0;
            IMFSample* sample = nullptr;
            if (FAILED(reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &idx, &flags, &ts, &sample))) { fail("CAMERA STOPPED"); break; }
            if (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)) { release(sample); fail("CAMERA STOPPED"); break; }
            if (!sample) continue;
            IMFMediaBuffer* buf = nullptr;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf))) {
                BYTE* data = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(buf->Lock(&data, nullptr, &len)) && len >= (DWORD)(std::abs(stride) * (int)h)) {
                    const bool rnd = round_;
                    const float cx = 0.5f * ow, cy = 0.5f * oh, rr = 0.5f * (float)std::min(ow, oh);
                    for (int y = 0; y < oh; ++y) {
                        // (a negative stride: the rows bottom-up)
                        const int sy = y * step;
                        const BYTE* row = stride >= 0 ? data + (size_t)sy * stride : data + (size_t)(h - 1 - sy) * (size_t)(-stride);
                        uint8_t* o = &out[(size_t)y * ow * 4];
                        for (int x = 0; x < ow; ++x) {
                            const BYTE* px = row + (size_t)((ow - 1 - x) * step) * 4;   // (mirrored, as a selfie)
                            o[x * 4 + 0] = px[2]; o[x * 4 + 1] = px[1]; o[x * 4 + 2] = px[0];
                            float a = 255.f;
                            if (rnd) {   // a soft edge, a pixel and a half
                                const float d = std::sqrt((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
                                a = 255.f * std::max(0.f, std::min(1.f, (rr - d) / 1.5f));
                            }
                            o[x * 4 + 3] = (uint8_t)a;
                        }
                    }
                    buf->Unlock();
                    std::lock_guard<std::mutex> lk(mu);
                    if (gen == myGen) { frame = out; fw = ow; fh = oh; ++frameNo; }
                }
                release(buf);
            }
            release(sample);
        }
    } while (false);
    release(reader);
    if (src) { src->Shutdown(); release(src); }
    if (devs) { for (UINT32 i = 0; i < n; ++i) release(devs[i]); CoTaskMemFree(devs); }
    release(attr);
    MFShutdown();
    CoUninitialize();
    *done = true;
}
}   // namespace

void start(int device) {
    stop();
    { std::lock_guard<std::mutex> lk(mu); why = "STARTING"; frame.clear(); fw = fh = 0; }
    running = true;
    workerDone = std::make_shared<std::atomic<bool>>(false);
    worker = std::thread(loop, device, gen.load(), workerDone);
}
void stop() {
    running = false;
    ++gen;
    if (worker.joinable()) {
        for (int i = 0; i < 150 && workerDone && !*workerDone; ++i) Sleep(10);   // (up to 1.5 s for the camera's next frame)
        if (workerDone && *workerDone) worker.join(); else worker.detach();
    }
    std::lock_guard<std::mutex> lk(mu);
    if (why.empty() || why == "STARTING") why = "OFF";
}
int devices() { return count; }
bool latest(uint32_t& seen, std::vector<uint8_t>& rgba, int& w, int& h) {
    std::lock_guard<std::mutex> lk(mu);
    if (frameNo == seen || frame.empty()) return false;
    seen = frameNo; rgba = frame; w = fw; h = fh;
    return true;
}
void setRound(bool round) { round_ = round; }
std::string status() { std::lock_guard<std::mutex> lk(mu); return why; }
}   // namespace webcam
