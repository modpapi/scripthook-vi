// GENERIC. SHA-256 (FIPS 180-4), self-contained: no OS crypto API, so the
// result cannot depend on the machine. Used by shvi_nativedb to name inputs
// and outputs in its manifest.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace shvi::sha256 {

namespace detail {
inline constexpr std::uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline std::uint32_t Rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

inline void Block(std::uint32_t h[8], const std::uint8_t* p) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) |
               (std::uint32_t(p[4 * i + 2]) << 8) | std::uint32_t(p[4 * i + 3]);
    for (int i = 16; i < 64; ++i) {
        std::uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        std::uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        std::uint32_t S1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
        std::uint32_t ch = (e & f) ^ (~e & g);
        std::uint32_t t1 = hh + S1 + ch + kK[i] + w[i];
        std::uint32_t S0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
        std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        std::uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
}  // namespace detail

inline std::array<std::uint8_t, 32> Digest(const void* data, std::size_t n) {
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::size_t full = n / 64;
    for (std::size_t i = 0; i < full; ++i) detail::Block(h, p + 64 * i);
    std::uint8_t tail[128] = {};
    std::size_t rest = n - 64 * full;
    if (rest) std::memcpy(tail, p + 64 * full, rest);
    tail[rest] = 0x80;
    std::size_t tailLen = rest + 1 + 8 <= 64 ? 64 : 128;
    std::uint64_t bits = static_cast<std::uint64_t>(n) * 8;
    for (int i = 0; i < 8; ++i) tail[tailLen - 1 - i] = static_cast<std::uint8_t>(bits >> (8 * i));
    for (std::size_t off = 0; off < tailLen; off += 64) detail::Block(h, tail + off);
    std::array<std::uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j) out[4 * i + j] = static_cast<std::uint8_t>(h[i] >> (24 - 8 * j));
    return out;
}

// 64 lowercase hex digits.
inline std::string Hex(const void* data, std::size_t n) {
    static const char* kDigits = "0123456789abcdef";
    auto d = Digest(data, n);
    std::string s(64, '0');
    for (int i = 0; i < 32; ++i) {
        s[2 * i] = kDigits[d[i] >> 4];
        s[2 * i + 1] = kDigits[d[i] & 0xF];
    }
    return s;
}

}  // namespace shvi::sha256
