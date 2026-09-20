#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

// Self-contained, dependency-free SHA-256 (FIPS 180-4). Nothing in this
// project links a crypto library (see Makefile LIBS) — this exists purely
// so the raw APK's hash can be logged for compat-report integrity checks,
// cross-checked against the hash the website computes over the submitted
// APK. Not hardware-accelerated; only used once per install (see
// sha256File's caller in loader.cpp), not a per-frame hot path.
struct Sha256Ctx {
    uint32_t state[8];
    uint64_t bitlen   = 0;
    uint8_t  buffer[64];
    uint32_t bufferLen = 0;
};

void sha256Init(Sha256Ctx& ctx);
void sha256Update(Sha256Ctx& ctx, const uint8_t* data, size_t len);
// Finalizes ctx (which must not be reused afterward) and returns the
// 64-character lowercase hex digest.
std::string sha256FinalHex(Sha256Ctx& ctx);

// Hashes a whole file on disk, streaming in fixed-size chunks — safe for
// large APKs, never loads the whole file into memory at once. Returns ""
// if the file can't be opened.
std::string sha256File(const std::string& path);
