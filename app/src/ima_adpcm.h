// IMA ADPCM, the Microsoft WAV kind (format 0x11): 4 bits a sample, a quarter of 16-bit PCM's size. The game's music
// (MENU MUSIC and GAME MUSIC, 2026-11-21: CrossGenGaming's own background track, six minutes at 24 kHz stereo, 8.7 MB
// instead of 35) is kept this way and decoded a block at a time as it plays (audio.cpp).
// A block: for each channel a 4-byte header (its first sample as int16, the step index, a spare byte), then groups of
// 4 bytes a channel in turn, 8 samples each (each byte's low 4 bits first). tools: ffmpeg -c:a adpcm_ima_wav.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ima {

static const int16_t kStep[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};
static const int8_t kIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

// one block (n bytes, `ch` channels, at most `spb` samples a channel) into out (interleaved, room for spb * ch):
// returns the samples a channel it held (a file's last block can be short; 0: not a block)
inline int decodeBlock(const uint8_t* blk, size_t n, int ch, int spb, int16_t* out) {
    if (ch < 1 || ch > 2 || spb < 1 || n < (size_t)(4 * ch)) return 0;
    int pred[2] = {0, 0}, idx[2] = {0, 0};
    for (int c = 0; c < ch; ++c) {
        int16_t p;
        std::memcpy(&p, blk + 4 * c, 2);
        pred[c] = p;
        idx[c] = blk[4 * c + 2] > 88 ? 88 : blk[4 * c + 2];
        out[c] = p;
    }
    const uint8_t* d = blk + 4 * ch;
    const size_t avail = n - 4 * ch;
    int s = 1;   // (the next sample a channel: the header's is the first)
    for (size_t off = 0; s < spb && off + 4 * (size_t)ch <= avail; off += 4 * (size_t)ch, s += 8)
        for (int c = 0; c < ch; ++c)
            for (int b = 0; b < 4; ++b) {
                const uint8_t byte = d[off + 4 * c + b];
                for (int half = 0; half < 2; ++half) {
                    const int code = half ? byte >> 4 : byte & 15, step = kStep[idx[c]];
                    // (the step's share as a multiply, as ffmpeg's encoder works it out; the reference's sum of shifts
                    // rounds a little differently, and the decoded track then drifts from what was encoded)
                    const int diff = ((2 * (code & 7) + 1) * step) >> 3;
                    pred[c] += (code & 8) ? -diff : diff;
                    pred[c] = pred[c] < -32768 ? -32768 : pred[c] > 32767 ? 32767 : pred[c];
                    idx[c] += kIndex[code];
                    idx[c] = idx[c] < 0 ? 0 : idx[c] > 88 ? 88 : idx[c];
                    const int si = s + b * 2 + half;
                    if (si < spb) out[si * ch + c] = (int16_t)pred[c];
                }
            }
    return s < spb ? s : spb;
}

// a WAV file of IMA ADPCM: where its blocks are and their shape (false: not one)
struct Wav {
    const uint8_t* data = nullptr;   // the first block
    size_t bytes = 0;                // all the blocks
    int ch = 0, rate = 0, blockAlign = 0, spb = 0;
};
inline bool parseWav(const uint8_t* f, size_t n, Wav& w) {
    if (n < 12 || std::memcmp(f, "RIFF", 4) != 0 || std::memcmp(f + 8, "WAVE", 4) != 0) return false;
    bool fmtOk = false;
    for (size_t p = 12; p + 8 <= n;) {
        uint32_t sz;
        std::memcpy(&sz, f + p + 4, 4);
        const uint8_t* c = f + p + 8;
        if (std::memcmp(f + p, "fmt ", 4) == 0 && sz >= 20 && p + 8 + 20 <= n) {
            uint16_t fmt, chs, ba, spb;
            uint32_t sr;
            std::memcpy(&fmt, c, 2); std::memcpy(&chs, c + 2, 2); std::memcpy(&sr, c + 4, 4); std::memcpy(&ba, c + 12, 2);
            std::memcpy(&spb, c + 18, 2);
            if (fmt == 0x11 && chs >= 1 && chs <= 2 && ba > 4 * chs && spb > 1) {
                w.ch = chs; w.rate = (int)sr; w.blockAlign = ba; w.spb = spb;
                fmtOk = true;
            }
        } else if (std::memcmp(f + p, "data", 4) == 0) {
            w.data = c;
            w.bytes = sz < n - (p + 8) ? sz : n - (p + 8);
        }
        p += 8 + (size_t)sz + (sz & 1);
    }
    return fmtOk && w.data && w.bytes >= (size_t)w.blockAlign;
}

// plays a Wav at 48 kHz stereo, a frame at a time, looped: decoded a block at a time; 24 kHz doubled (each sample
// twice, the first halfway from the one before)
struct Player {
    const Wav* w = nullptr;
    std::vector<int16_t> blk;
    int n = 0, at = 0, step = 1, phase = 0;   // the block's samples a channel, the next; 48 kHz / its rate; where in a pair
    size_t next = 0;                          // the next block's place
    float prev[2] = {0, 0}, cur[2] = {0, 0};
    bool start(const Wav& wav) {
        if (wav.rate != 24000 && wav.rate != 48000) return false;
        w = &wav;
        blk.assign((size_t)wav.spb * wav.ch, 0);
        step = 48000 / wav.rate;
        n = at = phase = 0;
        next = 0;
        prev[0] = prev[1] = cur[0] = cur[1] = 0;
        return true;
    }
    bool frame(float& l, float& r) {   // false: a broken file (nothing more to play)
        if (!w) return false;
        if (phase == 0) {
            if (at >= n) {   // the next block (after the last: the first again)
                if (next >= w->bytes) next = 0;
                const size_t len = w->bytes - next < (size_t)w->blockAlign ? w->bytes - next : (size_t)w->blockAlign;
                n = decodeBlock(w->data + next, len, w->ch, w->spb, blk.data());
                next += (size_t)w->blockAlign;
                at = 0;
                if (n <= 0) { w = nullptr; return false; }
            }
            for (int c = 0; c < 2; ++c) {
                prev[c] = cur[c];
                cur[c] = blk[(size_t)at * w->ch + (w->ch == 2 ? c : 0)] * (1.f / 32768.f);
            }
            ++at;
        }
        const bool between = step == 2 && phase == 0;
        l = between ? 0.5f * (prev[0] + cur[0]) : cur[0];
        r = between ? 0.5f * (prev[1] + cur[1]) : cur[1];
        phase = (phase + 1) % step;
        return true;
    }
};

}   // namespace ima
