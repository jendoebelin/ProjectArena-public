// G.711 mu-law: 16-bit PCM <-> 8-bit, for voice chat (no codec library needed; 16 kHz mono = 16 KB/s per talker).
#pragma once
#include <cstdint>

inline uint8_t ulawEncode(int16_t pcm) {
    const int BIAS = 0x84, CLIP = 32635;
    int s = pcm, sign = (s >> 8) & 0x80;
    if (sign) s = -s;
    if (s > CLIP) s = CLIP;
    s += BIAS;
    int exp = 7;
    for (int mask = 0x4000; (s & mask) == 0 && exp > 0; mask >>= 1) --exp;
    int mant = (s >> (exp + 3)) & 0x0F;
    return (uint8_t)~(sign | (exp << 4) | mant);
}

inline int16_t ulawDecode(uint8_t u) {
    u = (uint8_t)~u;
    int sign = u & 0x80, exp = (u >> 4) & 7, mant = u & 0x0F;
    int s = ((mant << 3) + 0x84) << exp;
    s -= 0x84;
    return (int16_t)(sign ? -s : s);
}
