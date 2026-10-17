// Small mixer playing 16-bit PCM WAV clips from the game assets (audio/*.wav), plus voice chat.
// Output: AAudio on Quest, WASAPI (shared mode) on Windows; both run the same 48 kHz stereo float mixer.
#pragma once
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "game.h"

#ifdef __ANDROID__
#include <aaudio/AAudio.h>
#endif

class Audio {
public:
    bool init();
    void shutdown();
    void pause();
    void resume();
    void play(const std::string& clip, float gain = 1.f, float pan = 0.f, bool loop = false);
    // local: the listener's player index (HitPlayer names the victim, RoundEnd / MatchEnd the winner)
    void onEvent(const GameEvent& e, const Vec3& listener, int local = 0);
    void setVolume(float v) { volume_ = v < 0 ? 0 : v > 1 ? 1 : v; }   // master 0..1
    // voice chat (16 kHz mono): capture through a second, input stream (Quest: needs RECORD_AUDIO); each speaker's
    // frames go through a small jitter buffer and are mixed into the output
    bool startCapture();
    void stopCapture();
    bool capturing() const { return capOn_; }
    int readCapture(int16_t* out, int max);   // 16 kHz samples captured since the last call
    void pushVoice(uint32_t speaker, const int16_t* pcm, int n, float gain, float pan);

private:
    struct Clip { std::vector<float> samples; int channels = 1; };   // 48 kHz
    struct Voice { const Clip* clip; size_t pos; float gainL, gainR; bool loop; };
    const Clip* get(const std::string& name);
    void mix(float* out, int frames);                 // 48 kHz stereo float, interleaved
    void captured(const int16_t* in, int frames);     // 48 kHz mono from the microphone
    std::map<std::string, Clip> clips_;
    std::vector<Voice> voices_;
    std::mutex mu_;
    std::map<std::string, std::string> eventMap_;
    std::atomic<float> volume_{0.8f};
    std::atomic<bool> outOn_{false}, capOn_{false}, paused_{false};
    std::mutex capMu_;
    std::vector<int16_t> cap_;               // 16 kHz captured samples
    int capPhase_ = 0, capAcc_ = 0;
    struct VoiceQ { std::deque<int16_t> q; float frac = 0; bool playing = false; float gainL = 1, gainR = 1; int idle = 0; };
    std::map<uint32_t, VoiceQ> vq_;
#ifdef __ANDROID__
    AAudioStream* stream_ = nullptr;
    AAudioStream* in_ = nullptr;
    static aaudio_data_callback_result_t cb(AAudioStream*, void* user, void* data, int32_t frames);
    static aaudio_data_callback_result_t inCb(AAudioStream*, void* user, void* data, int32_t frames);
#else
    std::thread outThread_, capThread_;
    std::atomic<bool> stopOut_{false}, stopCap_{false};
    void outLoop();
    void capLoop();
#endif
};
