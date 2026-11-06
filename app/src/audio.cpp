#include "audio.h"
#include "log.h"
#include "platform.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// a PCM WAV: 16-bit (the game's own sounds), and (0.3.16, SOUND STUDIO's own sounds) 8, 24 and 32-bit and 32-bit float;
// more than two channels are mixed down to one
static bool parseWav(const uint8_t* d, size_t n, std::vector<float>& out, int& channels, int& rate) {
    if (n < 44 || std::memcmp(d, "RIFF", 4) || std::memcmp(d + 8, "WAVE", 4)) return false;
    size_t p = 12;
    int bits = 16, tag = 1;
    while (p + 8 <= n) {
        uint32_t sz; std::memcpy(&sz, d + p + 4, 4);
        if (!std::memcmp(d + p, "fmt ", 4) && p + 8 + 16 <= n) {
            uint16_t fmt, ch; uint32_t sr; uint16_t bps;
            std::memcpy(&fmt, d + p + 8, 2); std::memcpy(&ch, d + p + 10, 2); std::memcpy(&sr, d + p + 12, 4); std::memcpy(&bps, d + p + 22, 2);
            if (fmt == 0xFFFE && sz >= 26 && p + 8 + 26 <= n) std::memcpy(&fmt, d + p + 8 + 24, 2);   // (WAVE_FORMAT_EXTENSIBLE)
            channels = ch; rate = (int)sr; bits = bps; tag = fmt;
        } else if (!std::memcmp(d + p, "data", 4)) {
            if (channels < 1 || channels > 8 || rate < 8000 || rate > 192000) return false;
            const size_t bytes = std::min<size_t>(sz, n - p - 8), w = (size_t)bits / 8;
            if (!(tag == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) && !(tag == 3 && bits == 32)) return false;
            const uint8_t* s = d + p + 8;
            const size_t cnt = bytes / w;
            out.resize(cnt);
            for (size_t i = 0; i < cnt; ++i) {
                const uint8_t* q = s + i * w;
                if (tag == 3) { float f; std::memcpy(&f, q, 4); out[i] = std::isfinite(f) ? std::max(-1.f, std::min(1.f, f)) : 0.f; }
                else if (bits == 8) out[i] = (q[0] - 128) / 128.f;
                else if (bits == 16) { int16_t v; std::memcpy(&v, q, 2); out[i] = v / 32768.f; }
                else if (bits == 24) out[i] = (float)((int32_t)((uint32_t)q[0] << 8 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 24) >> 8) / 8388608.f;
                else { int32_t v; std::memcpy(&v, q, 4); out[i] = (float)(v / 2147483648.0); }
            }
            if (channels > 2) {   // (mixed down)
                const size_t frames = cnt / channels;
                for (size_t f = 0; f < frames; ++f) {
                    float sum = 0;
                    for (int c = 0; c < channels; ++c) sum += out[f * channels + c];
                    out[f] = sum / channels;
                }
                out.resize(frames);
                channels = 1;
            }
            return true;
        }
        p += 8 + sz + (sz & 1);
    }
    return false;
}

static void resampleTo48k(std::vector<float>& s, int channels, int rate) {   // linear, to the mixer's rate
    if (rate == 48000 || s.empty()) return;
    const size_t frames = s.size() / channels, outFrames = size_t(double(frames) * 48000 / rate);
    std::vector<float> o(outFrames * channels);
    for (size_t i = 0; i < outFrames; ++i) {
        const double src = double(i) * rate / 48000;
        const size_t i0 = std::min(frames - 1, size_t(src)), i1 = std::min(frames - 1, i0 + 1);
        const float t = float(src - i0);
        for (int ch = 0; ch < channels; ++ch) o[i * channels + ch] = s[i0 * channels + ch] * (1 - t) + s[i1 * channels + ch] * t;
    }
    s.swap(o);
}

const Audio::Clip* Audio::get(const std::string& name) {
    auto it = clips_.find(name);
    if (it != clips_.end()) return it->second.samples.empty() ? nullptr : &it->second;
    Clip& c = clips_[name];
    std::vector<uint8_t> file;
    if (!assetRead("audio/" + name + ".wav", file)) { LOGW("audio clip missing: %s", name.c_str()); return nullptr; }
    int rate = 48000;
    std::vector<float> s;
    if (parseWav(file.data(), file.size(), s, c.channels, rate)) {
        if (rate != 48000) {   // linear resample to the mixer rate
            size_t frames = s.size() / c.channels, outFrames = size_t(double(frames) * 48000 / rate);
            c.samples.resize(outFrames * c.channels);
            for (size_t i = 0; i < outFrames; ++i) {
                double src = double(i) * rate / 48000;
                size_t i0 = std::min(frames - 1, size_t(src)), i1 = std::min(frames - 1, i0 + 1);
                float t = float(src - i0);
                for (int ch = 0; ch < c.channels; ++ch) c.samples[i * c.channels + ch] = s[i0 * c.channels + ch] * (1 - t) + s[i1 * c.channels + ch] * t;
            }
        } else c.samples = std::move(s);
    }
    return c.samples.empty() ? nullptr : &c;
}

void Audio::mix(float* out, int frames) {
    Audio* self = this;
    std::memset(out, 0, frames * 2 * sizeof(float));
    std::lock_guard<std::mutex> lk(self->mu_);
    for (auto& v : self->voices_) {
        for (int32_t f = 0; f < frames; ++f) {
            size_t frameCount = v.clip->samples.size() / v.clip->channels;
            if (v.pos >= frameCount) { if (v.loop) v.pos = 0; else break; }
            float l, r;
            if (v.clip->channels == 2) { l = v.clip->samples[v.pos * 2]; r = v.clip->samples[v.pos * 2 + 1]; }
            else l = r = v.clip->samples[v.pos];
            out[f * 2] += l * v.gainL;
            out[f * 2 + 1] += r * v.gainR;
            ++v.pos;
        }
    }
    self->voices_.erase(std::remove_if(self->voices_.begin(), self->voices_.end(), [](const Voice& v) {
        return !v.loop && v.pos >= v.clip->samples.size() / v.clip->channels; }), self->voices_.end());
    // voice: each speaker resampled 16 -> 48 kHz (linear), after ~80 ms of buffering; re-buffers on underrun
    for (auto it = self->vq_.begin(); it != self->vq_.end();) {
        Audio::VoiceQ& v = it->second;
        if (!v.playing && v.q.size() >= 1280) v.playing = true;
        if (v.playing) {
            for (int32_t f = 0; f < frames; ++f) {
                if (v.q.size() < 2) { v.playing = false; break; }
                float smp = (v.q[0] * (1.f - v.frac) + v.q[1] * v.frac) * (1.f / 32768.f);
                out[f * 2] += smp * v.gainL;
                out[f * 2 + 1] += smp * v.gainR;
                v.frac += 1.f / 3.f;
                while (v.frac >= 1.f) { v.q.pop_front(); v.frac -= 1.f; }
            }
            v.idle = 0;
        } else if (++v.idle > 2000) {   // ~20 s of silence: forget the speaker
            it = self->vq_.erase(it);
            continue;
        }
        ++it;
    }
    const float vol = self->volume_.load();
    for (int32_t i = 0; i < frames * 2; ++i) out[i] = std::max(-1.f, std::min(1.f, out[i] * vol));
}

bool Audio::init() {
    // event -> clip mapping (audio/events.txt: "<Event> <clip>")
    std::vector<uint8_t> file;
    if (assetRead("audio/events.txt", file)) {
        std::string txt(file.begin(), file.end());
        size_t p = 0;
        while (p < txt.size()) {
            size_t e = txt.find('\n', p); if (e == std::string::npos) e = txt.size();
            std::string line = txt.substr(p, e - p);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t sp = line.find(' ');
            if (sp != std::string::npos) eventMap_[line.substr(0, sp)] = line.substr(sp + 1);
            p = e + 1;
        }
    }
#ifdef __ANDROID__
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, 48000);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, &Audio::cb, this);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &stream_);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) { LOGE("AAudio open failed: %d", r); stream_ = nullptr; return false; }
    AAudioStream_requestStart(stream_);
#else
    stopOut_ = false;
    outThread_ = std::thread(&Audio::outLoop, this);
#endif
    outOn_ = true;
    LOGI("audio started (%zu event mappings)", eventMap_.size());
    if (eventMap_.count("Ambience")) play(eventMap_["Ambience"], 0.35f, 0, true);
    return true;
}

#ifdef __ANDROID__
aaudio_data_callback_result_t Audio::cb(AAudioStream*, void* user, void* data, int32_t frames) {
    ((Audio*)user)->mix((float*)data, frames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
void Audio::shutdown() { stopCapture(); if (stream_) { AAudioStream_requestStop(stream_); AAudioStream_close(stream_); stream_ = nullptr; } outOn_ = false; }
void Audio::pause() { if (stream_) AAudioStream_requestPause(stream_); }
void Audio::resume() { if (stream_) AAudioStream_requestStart(stream_); }
#else
void Audio::shutdown() {
    stopCapture();
    stopOut_ = true;
    if (outThread_.joinable()) outThread_.join();
    outOn_ = false;
}
void Audio::pause() { paused_ = true; }
void Audio::resume() { paused_ = false; }
#endif

void Audio::play(const std::string& name, float gain, float pan, bool loop) {
    const Clip* c = get(name);
    if (!c || !outOn_) return;
    float l = gain * std::sqrt(0.5f * (1 - pan)), r = gain * std::sqrt(0.5f * (1 + pan));
    std::lock_guard<std::mutex> lk(mu_);
    if (voices_.size() > 24) {   // the oldest one-shot makes room (a loop keeps playing)
        auto it = std::find_if(voices_.begin(), voices_.end(), [](const Voice& v) { return !v.loop; });
        voices_.erase(it != voices_.end() ? it : voices_.begin());
    }
    voices_.push_back({c, 0, l, r, loop});
}

void Audio::setLoop(const std::string& name, float gain) {
    const Clip* c = get(name);
    if (!c || !outOn_) return;
    const float g = std::max(0.f, gain) * 0.7071f;
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = voices_.begin(); it != voices_.end(); ++it)
        if (it->clip == c && it->loop) {
            if (g <= 0.0005f) voices_.erase(it);
            else it->gainL = it->gainR = g;
            return;
        }
    if (g > 0.0005f) voices_.push_back({c, 0, g, g, true});
}

const std::string* Audio::clipFor(const std::string& key) const {
    auto o = override_.find(key);
    if (o != override_.end()) return &o->second;
    auto it = eventMap_.find(key);
    return it == eventMap_.end() ? nullptr : &it->second;
}

bool Audio::loadFile(const std::string& clip, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> file;
    uint8_t buf[16384];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) file.insert(file.end(), buf, buf + n);
    std::fclose(f);
    Clip c;
    int rate = 48000;
    if (file.size() > 24u * 1024 * 1024 || !parseWav(file.data(), file.size(), c.samples, c.channels, rate) || c.samples.empty()) return false;
    resampleTo48k(c.samples, c.channels, rate);   // (your own sounds may be at any rate; the library's are 48 kHz)
    if (c.samples.size() / c.channels > 48000u * 30) c.samples.resize(48000u * 30 * c.channels);   // (30 s at most)
    clips_[clip] = std::move(c);
    return true;
}

void Audio::setOverride(const std::string& key, const std::string& clip) {
    if (clip.empty()) override_.erase(key);
    else override_[key] = clip;
}

bool Audio::cue(const std::string& key, float gain, float pan) {
    const std::string* c = clipFor(key);
    if (!c) return false;
    play(*c, gain, pan);
    return true;
}

void Audio::onEvent(const GameEvent& e, const Vec3& listener, bool ours, const Vec3& right) {
    std::string key = eventName(e.type);
    // from the listener's side (it used to be player 0's: the orange player heard "you were hit" when they scored)
    if (e.type == EventType::HitPlayer) key = ours ? "HitPlayerSelf" : "HitPlayerOpponent";
    if (e.type == EventType::RoundEnd || e.type == EventType::MatchEnd) key += ours ? "Win" : "Lose";
    if (e.type == EventType::Throw) key = e.value < 6 ? "ThrowSlow" : e.value < 9 ? "ThrowMed" : e.value < 12 ? "ThrowFast" : "ThrowFull";
    {   // (SOUND STUDIO's choice for it)
        auto o = override_.find(key);
        if (o != override_.end()) {
            const float d = length(e.pos - listener);
            const float gain = (e.pos.x == 0 && e.pos.y == 0 && e.pos.z == 0) ? 1.f : clampf(2.0f / (1.0f + d), 0.15f, 1.f);
            play(o->second, gain, clampf(dot(e.pos - listener, right) * 0.4f, -0.8f, 0.8f));
            return;
        }
    }
    auto it = eventMap_.find(key);
    if (it == eventMap_.end()) it = eventMap_.find(eventName(e.type));
    if (it == eventMap_.end() && e.type == EventType::FistBump) it = eventMap_.find("KnuckleBlock");   // a solid knock
    if (it == eventMap_.end() && e.type == EventType::LeadChange) it = eventMap_.find("Recharge");    // (sets without the chime)
    // FIREBALL BLITZ (0.3.13, sounds of their own to come): a knockout the lead's chime, a fizzle a failed throw's puff,
    // the room closing in the countdown's beep (not the overtime call: since 0.3.13 a voice saying "sudden death")
    if (it == eventMap_.end() && e.type == EventType::Knockout) it = eventMap_.find("LeadChange");
    if (it == eventMap_.end() && e.type == EventType::Knockout) it = eventMap_.find("ShieldBreak");
    if (it == eventMap_.end() && e.type == EventType::Fizzle) it = eventMap_.find("ThrowFail");
    if (it == eventMap_.end() && e.type == EventType::RingClose) it = eventMap_.find("Countdown");
    if (it == eventMap_.end()) return;
    float d = length(e.pos - listener);
    float gain = (e.pos.x == 0 && e.pos.y == 0 && e.pos.z == 0) ? 1.f : clampf(2.0f / (1.0f + d), 0.15f, 1.f);
    float pan = clampf(dot(e.pos - listener, right) * 0.4f, -0.8f, 0.8f);   // court-relative pan, toward the listener's right
    play(it->second, gain, pan);
}

// ------------------------------------------------------------------ voice chat
void Audio::captured(const int16_t* in, int frames) {   // 48 -> 16 kHz: average three samples
    std::lock_guard<std::mutex> lk(capMu_);
    for (int f = 0; f < frames; ++f) {
        capAcc_ += in[f];
        if (++capPhase_ == 3) { cap_.push_back((int16_t)(capAcc_ / 3)); capAcc_ = 0; capPhase_ = 0; }
    }
    if (cap_.size() > 16000) cap_.erase(cap_.begin(), cap_.end() - 16000);
}

#ifdef __ANDROID__
aaudio_data_callback_result_t Audio::inCb(AAudioStream*, void* user, void* data, int32_t frames) {
    ((Audio*)user)->captured((const int16_t*)data, frames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

bool Audio::startCapture() {
    if (in_) return true;
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 1);
    AAudioStreamBuilder_setSampleRate(b, 48000);
    AAudioStreamBuilder_setInputPreset(b, AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION);   // echo cancellation
    AAudioStreamBuilder_setDataCallback(b, &Audio::inCb, this);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &in_);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) { LOGW("voice: microphone open failed: %d", r); in_ = nullptr; return false; }
    AAudioStream_requestStart(in_);
    capOn_ = true;
    LOGI("voice: microphone on (%d Hz)", AAudioStream_getSampleRate(in_));
    return true;
}

void Audio::stopCapture() {
    if (!in_) return;
    AAudioStream_requestStop(in_);
    AAudioStream_close(in_);
    in_ = nullptr;
    capOn_ = false;
    std::lock_guard<std::mutex> lk(capMu_);
    cap_.clear();
    LOGI("voice: microphone off");
}
#else
bool Audio::startCapture() {
    if (capOn_) return true;
    stopCap_ = false;
    capOn_ = true;
    capThread_ = std::thread(&Audio::capLoop, this);
    return true;
}

void Audio::stopCapture() {
    if (!capOn_ && !capThread_.joinable()) return;
    stopCap_ = true;
    if (capThread_.joinable()) capThread_.join();
    capOn_ = false;
    std::lock_guard<std::mutex> lk(capMu_);
    cap_.clear();
    LOGI("voice: microphone off");
}
#endif

int Audio::readCapture(int16_t* out, int max) {
    std::lock_guard<std::mutex> lk(capMu_);
    int n = std::min<int>(max, (int)cap_.size());
    std::copy(cap_.begin(), cap_.begin() + n, out);
    cap_.erase(cap_.begin(), cap_.begin() + n);
    return n;
}

void Audio::pushVoice(uint32_t speaker, const int16_t* pcm, int n, float gain, float pan) {
    std::lock_guard<std::mutex> lk(mu_);
    VoiceQ& v = vq_[speaker];
    if (v.q.size() > 16000 / 2) v.q.clear();   // more than 0.5 s behind: drop the backlog
    v.q.insert(v.q.end(), pcm, pcm + n);
    v.gainL = gain * std::sqrt(0.5f * (1 - pan));
    v.gainR = gain * std::sqrt(0.5f * (1 + pan));
}

#ifndef __ANDROID__
// ------------------------------------------------------------------ Windows: WASAPI shared mode, event driven.
// We ask for 48 kHz float stereo (output) / 48 kHz 16-bit mono (microphone) and let Windows convert.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

static const CLSID kMMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
static const IID kIMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
static const IID kIAudioClient = __uuidof(IAudioClient);
static const IID kIAudioRenderClient = __uuidof(IAudioRenderClient);
static const IID kIAudioCaptureClient = __uuidof(IAudioCaptureClient);

static IAudioClient* openClient(EDataFlow flow, ERole role, bool floatStereo, HANDLE ev) {
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* ac = nullptr;
    if (FAILED(CoCreateInstance(kMMDeviceEnumerator, nullptr, CLSCTX_ALL, kIMMDeviceEnumerator, (void**)&en))) return nullptr;
    HRESULT hr = en->GetDefaultAudioEndpoint(flow, role, &dev);
    en->Release();
    if (FAILED(hr)) return nullptr;
    hr = dev->Activate(kIAudioClient, CLSCTX_ALL, nullptr, (void**)&ac);
    dev->Release();
    if (FAILED(hr)) return nullptr;
    WAVEFORMATEX wf{};
    wf.wFormatTag = floatStereo ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    wf.nChannels = floatStereo ? 2 : 1;
    wf.nSamplesPerSec = 48000;
    wf.wBitsPerSample = floatStereo ? 32 : 16;
    wf.nBlockAlign = wf.nChannels * wf.wBitsPerSample / 8;
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000 /* 20 ms */, 0, &wf, nullptr);
    if (FAILED(hr) || FAILED(ac->SetEventHandle(ev))) { LOGW("audio: WASAPI initialise failed 0x%08lx", (unsigned long)hr); ac->Release(); return nullptr; }
    return ac;
}

void Audio::outLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    IAudioClient* ac = openClient(eRender, eConsole, true, ev);
    IAudioRenderClient* rc = nullptr;
    UINT32 bufFrames = 0;
    if (ac && SUCCEEDED(ac->GetBufferSize(&bufFrames)) && SUCCEEDED(ac->GetService(kIAudioRenderClient, (void**)&rc))) {
        ac->Start();
        LOGI("audio: WASAPI output %u frames", bufFrames);
        while (!stopOut_) {
            if (WaitForSingleObject(ev, 200) != WAIT_OBJECT_0) continue;
            UINT32 pad = 0;
            if (FAILED(ac->GetCurrentPadding(&pad))) break;   // device lost (unplugged): give up quietly
            UINT32 n = bufFrames - pad;
            if (!n) continue;
            BYTE* p = nullptr;
            if (FAILED(rc->GetBuffer(n, &p))) break;
            if (paused_) std::memset(p, 0, n * 2 * sizeof(float)); else mix((float*)p, (int)n);
            rc->ReleaseBuffer(n, 0);
        }
        ac->Stop();
    } else {
        LOGW("audio: no output device");
    }
    if (rc) rc->Release();
    if (ac) ac->Release();
    CloseHandle(ev);
    CoUninitialize();
}

void Audio::capLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    IAudioClient* ac = openClient(eCapture, eCommunications, false, ev);
    IAudioCaptureClient* cc = nullptr;
    if (ac && SUCCEEDED(ac->GetService(kIAudioCaptureClient, (void**)&cc))) {
        ac->Start();
        LOGI("voice: microphone on (WASAPI)");
        while (!stopCap_) {
            if (WaitForSingleObject(ev, 200) != WAIT_OBJECT_0) continue;
            UINT32 n = 0;
            while (SUCCEEDED(cc->GetNextPacketSize(&n)) && n) {
                BYTE* p = nullptr;
                DWORD fl = 0;
                if (FAILED(cc->GetBuffer(&p, &n, &fl, nullptr, nullptr))) break;
                if (fl & AUDCLNT_BUFFERFLAGS_SILENT) { std::vector<int16_t> z(n, 0); captured(z.data(), (int)n); }
                else captured((const int16_t*)p, (int)n);
                cc->ReleaseBuffer(n);
            }
        }
        ac->Stop();
    } else {
        LOGW("voice: no microphone");
    }
    if (cc) cc->Release();
    if (ac) ac->Release();
    CloseHandle(ev);
    CoUninitialize();
}
#endif
