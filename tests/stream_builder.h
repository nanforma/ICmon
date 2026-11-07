// TEST-ONLY: builds synthetic streams in the frame layout documented in src/xm/xmdemux.h.
// It proves the code follows the layout; it says nothing about what a real camera sends.
#pragma once
#include <cstdint>
#include <string>

#include "camtime.h"

namespace xmtest {

inline void le32w(std::string& s, uint32_t v) { for (int i = 0; i < 4; i++) s.push_back((char)((v >> (8 * i)) & 0xff)); }
inline std::string F9() { return std::string("\x00\x00\x01\xF9\x0e\x00\x08\x00", 8) + std::string(8, '\0'); }
inline std::string I(uint32_t packed_time, size_t n, char fill = 'x') {
    std::string s("\x00\x00\x01\xFC\x53\x0c\x20\x44", 8);
    le32w(s, packed_time); le32w(s, (uint32_t)n);
    s += std::string("\x00\x00\x00\x01\x40\x01", 6);
    return s + std::string(n - 6, fill);
}
inline std::string P(size_t n) {
    std::string s("\x00\x00\x01\xFD", 4);
    le32w(s, (uint32_t)n);
    s += std::string("\x00\x00\x00\x01\x02\x01", 6);
    return s + std::string(n - 6, 'y');
}
inline std::string A(size_t n) {
    std::string s("\x00\x00\x01\xFA\x0e\x02", 6);
    s.push_back((char)(n & 0xff)); s.push_back((char)(n >> 8));
    return s + std::string(n, 'a');
}

struct GopCounts { size_t i = 0, p = 0, a = 0; };

// One GOP: info, I-frame stamped `t`, then 35 P-frames with audio between (mirrors the observed GOP of 36).
inline std::string gop(xm::CamTime t, GopCounts* c = nullptr, size_t i_bytes = 40000, char fill = 'x') {
    std::string s = F9() + I(xm::encode_time(xm::from_cam_time(t)), i_bytes, fill) + A(320) + A(320);
    for (int k = 0; k < 35; k++) s += P(400) + A(320);
    if (c) { c->i += 1; c->p += 35; c->a += 2 + 35; }
    return s;
}

}  // namespace xmtest
