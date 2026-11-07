#include "sofia_hash.h"

#include <cmath>

namespace xm {

std::array<uint8_t, 16> md5(const std::string& msg) {
    static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                              5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    static const std::array<uint32_t, 64> K = [] {
        std::array<uint32_t, 64> k{};
        for (int i = 0; i < 64; i++) k[i] = (uint32_t)std::floor(4294967296.0 * std::fabs(std::sin(i + 1.0)));
        return k;
    }();
    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    std::string m = msg;
    m.push_back((char)0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    uint64_t bits = (uint64_t)msg.size() * 8;
    for (int i = 0; i < 8; i++) m.push_back((char)((bits >> (8 * i)) & 0xff));
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t M[16];
        for (int i = 0; i < 16; i++) {
            const unsigned char* p = (const unsigned char*)m.data() + off + 4 * i;
            M[i] = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
        }
        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; i++) {
            uint32_t F;
            int g;
            if (i < 16) { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) % 16; }
            else { F = C ^ (B | ~D); g = (7 * i) % 16; }
            F = F + A + K[i] + M[g];
            A = D; D = C; C = B;
            B = B + ((F << S[i]) | (F >> (32 - S[i])));
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }
    std::array<uint8_t, 16> out{};
    uint32_t v[4] = {a0, b0, c0, d0};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) out[i * 4 + j] = (v[i] >> (8 * j)) & 0xff;
    return out;
}

std::string md5_hex(const std::string& msg) {
    static const char* h = "0123456789abcdef";
    std::string o;
    for (uint8_t b : md5(msg)) { o.push_back(h[b >> 4]); o.push_back(h[b & 15]); }
    return o;
}

std::string sofia_hash(const std::string& password) {
    static const std::string chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    auto d = md5(password);
    std::string out;
    for (int i = 0; i < 16; i += 2) out.push_back(chars[(d[i] + d[i + 1]) % 62]);
    return out;
}

}  // namespace xm
