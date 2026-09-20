#include "compat/sha256.h"
#include <cstdio>
#include <cstring>

namespace {

constexpr uint32_t kK[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

constexpr uint32_t kIV[8] = {
    0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
    0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19,
};

inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

void transform(uint32_t state[8], const uint8_t chunk[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t(chunk[i * 4]) << 24) | (uint32_t(chunk[i * 4 + 1]) << 16) |
               (uint32_t(chunk[i * 4 + 2]) << 8) | uint32_t(chunk[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1    = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch    = (e & f) ^ (~e & g);
        uint32_t temp1 = h + S1 + ch + kK[i] + w[i];
        uint32_t S0    = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj   = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

} // namespace

void sha256Init(Sha256Ctx& ctx) {
    memcpy(ctx.state, kIV, sizeof(kIV));
    ctx.bitlen    = 0;
    ctx.bufferLen = 0;
}

void sha256Update(Sha256Ctx& ctx, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        ctx.buffer[ctx.bufferLen++] = data[i];
        if (ctx.bufferLen == 64) {
            transform(ctx.state, ctx.buffer);
            ctx.bitlen += 512;
            ctx.bufferLen = 0;
        }
    }
}

std::string sha256FinalHex(Sha256Ctx& ctx) {
    uint32_t i = ctx.bufferLen;

    // Pad: 0x80 then zeros, leaving 8 bytes at the end for the bit length.
    if (ctx.bufferLen < 56) {
        ctx.buffer[i++] = 0x80;
        while (i < 56) ctx.buffer[i++] = 0x00;
    } else {
        ctx.buffer[i++] = 0x80;
        while (i < 64) ctx.buffer[i++] = 0x00;
        transform(ctx.state, ctx.buffer);
        memset(ctx.buffer, 0, 56);
    }

    ctx.bitlen += (uint64_t)ctx.bufferLen * 8;
    for (int j = 0; j < 8; j++)
        ctx.buffer[63 - j] = (uint8_t)(ctx.bitlen >> (j * 8));
    transform(ctx.state, ctx.buffer);

    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int w = 0; w < 8; w++) {
        for (int j = 3; j >= 0; j--) {
            uint8_t byte = (uint8_t)(ctx.state[w] >> (j * 8));
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 0xF]);
        }
    }
    return out;
}

std::string sha256File(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "";
    Sha256Ctx ctx;
    sha256Init(ctx);
    uint8_t buf[64 * 1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        sha256Update(ctx, buf, n);
    fclose(f);
    return sha256FinalHex(ctx);
}
