// xmdemux.h - parser for the frame envelope used in XM/iCSee recording downloads (OPPlayBack).
//
// Layout (each rule below was checked by arithmetic against a real 3 MB download from an
// XM530V200_X6C-WEQ camera, firmware V5.04.R02.000807DD; the first 6 frames of the dump chain
// together exactly: I-frame -> audio -> audio -> P-frame -> audio):
//
//   I-frame  00 00 01 FC | 4 bytes (unknown meaning) | u32le time | u32le length | payload
//   P-frame  00 00 01 FD | u32le length | payload
//   audio    00 00 01 FA | 1 byte codec? | 1 byte ? | u16le length | payload
//   info     00 00 01 F9 | 12 bytes (seen once: 0e 00 08 00 00 ...)     <- length only seen ONCE
//
// Video payloads begin with an Annex-B start code (00 00 00 01) followed by HEVC NAL units
// (first NAL byte 0x40 = VPS) in the sample. Time is bit-packed:
//   year-2000:6 | month:4 | day:5 | hour:5 | minute:6 | second:6  (verified on one I-frame)
//
// Window downloads (StartTime/EndTime narrower than the file) were seen to arrive as whole 32768-byte
// chunks (2195456 = 67 x 32768), so they start and end in the middle of a frame; Stats::leading_bytes
// and Frame::after_i exist for that.
//
// NOT verified: meaning of the 4 unknown I-frame bytes and of the audio codec byte, the F9 length
// for any frame other than the first, and whether H.264 recordings use the same layout.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace xm {

struct Time { int year, month, day, hour, minute, second; };

inline Time decode_time(uint32_t t) {
    return {int(t >> 26) + 2000, int((t >> 22) & 0xF), int((t >> 17) & 0x1F),
            int((t >> 12) & 0x1F), int((t >> 6) & 0x3F), int(t & 0x3F)};
}

enum class Kind { Info, IFrame, PFrame, Audio };

struct Frame {
    Kind kind;
    size_t offset;          // position of the 00 00 01 Fx marker
    size_t total;           // bytes occupied, header included
    const uint8_t* payload; // frame data after the header (not valid for Info)
    size_t payload_len;
    bool after_i = false;   // an I-frame has already been delivered (video before it cannot be decoded)
    bool has_time = false;
    uint32_t time_raw = 0;
    uint8_t unknown4[4] = {0, 0, 0, 0};  // I-frame: the 4 bytes before the time field
    uint8_t audio_hdr[2] = {0, 0};       // audio: the 2 bytes before the length field
};

struct Stats {
    size_t resyncs = 0;        // times the parser had to search for the next marker
    size_t skipped_bytes = 0;  // bytes passed over while searching, after the first marker was found
    size_t leading_bytes = 0;  // bytes before the first marker: a window download starts in the middle of a frame
    bool truncated_tail = false;
    size_t tail_bytes = 0;     // bytes of an incomplete last frame (not delivered)
    size_t info_frames = 0, i_frames = 0, p_frames = 0, audio_frames = 0;
};

inline bool is_marker(const uint8_t* p) {
    return p[0] == 0 && p[1] == 0 && p[2] == 1 && (p[3] == 0xF9 || p[3] == 0xFA || p[3] == 0xFC || p[3] == 0xFD);
}

inline uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
inline uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// Walks the buffer and calls `cb` for every complete frame. Never guesses silently: bytes that do
// not fit the layout are counted in Stats (resyncs / skipped_bytes).
inline Stats parse(const uint8_t* d, size_t n, const std::function<void(const Frame&)>& cb) {
    const size_t kMaxFrame = 16u << 20;
    Stats st;
    size_t pos = 0;
    bool seen_marker = false, seen_i = false;
    auto find_marker = [&](size_t from) -> size_t {
        for (size_t q = from; q + 4 <= n; q++) if (is_marker(d + q)) return q;
        return n;
    };
    while (pos + 4 <= n) {
        if (!is_marker(d + pos)) {
            size_t q = find_marker(pos + 1);
            if (seen_marker) { st.resyncs++; st.skipped_bytes += q - pos; }
            else st.leading_bytes += q - pos;  // expected for a window download: it starts mid-frame
            pos = q;
            continue;
        }
        seen_marker = true;
        Frame f{};
        f.offset = pos;
        size_t hdr = 0, len = 0;
        switch (d[pos + 3]) {
            case 0xFC:
                if (pos + 16 > n) { st.truncated_tail = true; st.tail_bytes = n - pos; return st; }
                f.kind = Kind::IFrame; hdr = 16;
                for (int i = 0; i < 4; i++) f.unknown4[i] = d[pos + 4 + i];
                f.time_raw = le32(d + pos + 8); f.has_time = true;
                len = le32(d + pos + 12);
                break;
            case 0xFD:
                if (pos + 8 > n) { st.truncated_tail = true; st.tail_bytes = n - pos; return st; }
                f.kind = Kind::PFrame; hdr = 8; len = le32(d + pos + 4);
                break;
            case 0xFA:
                if (pos + 8 > n) { st.truncated_tail = true; st.tail_bytes = n - pos; return st; }
                f.kind = Kind::Audio; hdr = 8;
                f.audio_hdr[0] = d[pos + 4]; f.audio_hdr[1] = d[pos + 5];
                len = le16(d + pos + 6);
                break;
            default: {  // 0xF9: length known only for the first frame (16 bytes); otherwise up to the next marker
                f.kind = Kind::Info;
                size_t q = (pos + 20 <= n && is_marker(d + pos + 16)) ? pos + 16 : find_marker(pos + 4);
                if (q >= n && pos + 16 > n) { st.truncated_tail = true; st.tail_bytes = n - pos; return st; }
                hdr = q - pos; len = 0;
                break;
            }
        }
        if (len > kMaxFrame) {  // implausible length: treat this marker as noise
            st.resyncs++;
            size_t q = find_marker(pos + 4);
            st.skipped_bytes += q - pos;
            pos = q;
            continue;
        }
        f.total = hdr + len;
        if (pos + f.total > n) { st.truncated_tail = true; st.tail_bytes = n - pos; return st; }
        f.payload = d + pos + hdr;
        f.payload_len = len;
        switch (f.kind) {
            case Kind::Info: st.info_frames++; break;
            case Kind::IFrame: st.i_frames++; break;
            case Kind::PFrame: st.p_frames++; break;
            case Kind::Audio: st.audio_frames++; break;
        }
        if (f.kind == Kind::IFrame) seen_i = true;
        f.after_i = seen_i;
        cb(f);
        pos += f.total;
    }
    if (pos < n) { st.truncated_tail = true; st.tail_bytes = n - pos; }
    return st;
}

// Incremental version of parse() for a download that arrives in packets. Feed bytes as they come;
// the callback gets every complete frame once. Frame::payload is valid only during the callback.
// Frame::offset counts from the first byte ever fed. Call finish() at the end of the data: whatever
// is left over is reported in Stats (truncated_tail / tail_bytes), never delivered.
// Feeding byte by byte gives the same frames as one parse() over the whole buffer (tested).
class StreamParser {
public:
    using Callback = std::function<void(const Frame&)>;
    explicit StreamParser(Callback cb) : cb_(std::move(cb)) {}

    void feed(const uint8_t* d, size_t n) {
        buf_.insert(buf_.end(), d, d + n);
        run(false);
    }
    const Stats& finish() {
        run(true);
        return st_;
    }
    const Stats& stats() const { return st_; }

private:
    static constexpr size_t kMaxFrame = 16u << 20;
    Callback cb_;
    std::vector<uint8_t> buf_;
    size_t base_ = 0;  // absolute offset of buf_[0]
    Stats st_;
    bool seen_marker_ = false, seen_i_ = false, in_junk_ = false;

    void account(size_t k) {
        if (!seen_marker_) st_.leading_bytes += k;
        else {
            if (!in_junk_) { st_.resyncs++; in_junk_ = true; }
            st_.skipped_bytes += k;
        }
    }

    void run(bool final_) {
        const size_t n = buf_.size();
        const uint8_t* d = buf_.data();
        size_t pos = 0;
        auto find_marker = [&](size_t from) -> size_t {
            for (size_t q = from; q + 4 <= n; q++) if (is_marker(d + q)) return q;
            return n;
        };
        while (pos + 4 <= n) {
            if (!is_marker(d + pos)) {
                size_t q = find_marker(pos + 1);
                size_t stop = (q == n) ? n - 3 : q;  // keep 3 bytes: a marker may be split across feeds
                account(stop - pos);
                pos = stop;
                continue;
            }
            seen_marker_ = true;
            Frame f{};
            f.offset = base_ + pos;
            size_t hdr = 0, len = 0;
            bool need_more = false;
            switch (d[pos + 3]) {
                case 0xFC:
                    if (pos + 16 > n) { need_more = true; break; }
                    f.kind = Kind::IFrame; hdr = 16;
                    for (int i = 0; i < 4; i++) f.unknown4[i] = d[pos + 4 + i];
                    f.time_raw = le32(d + pos + 8); f.has_time = true;
                    len = le32(d + pos + 12);
                    break;
                case 0xFD:
                    if (pos + 8 > n) { need_more = true; break; }
                    f.kind = Kind::PFrame; hdr = 8; len = le32(d + pos + 4);
                    break;
                case 0xFA:
                    if (pos + 8 > n) { need_more = true; break; }
                    f.kind = Kind::Audio; hdr = 8;
                    f.audio_hdr[0] = d[pos + 4]; f.audio_hdr[1] = d[pos + 5];
                    len = le16(d + pos + 6);
                    break;
                default:  // 0xF9 info: 16 bytes when the next marker follows there, else up to the next marker
                    f.kind = Kind::Info;
                    if (pos + 20 <= n && is_marker(d + pos + 16)) hdr = 16;
                    else {
                        size_t q = (pos + 20 <= n) ? find_marker(pos + 4) : n;
                        if (q < n) hdr = q - pos;
                        else if (final_) hdr = n - pos;
                        else need_more = true;
                    }
                    break;
            }
            if (need_more) break;
            if (len > kMaxFrame) {  // implausible length: this marker is noise
                size_t q = find_marker(pos + 4);
                size_t stop = (q == n) ? n - 3 : q;
                account(stop - pos);
                pos = stop;
                continue;
            }
            f.total = hdr + len;
            if (pos + f.total > n) break;  // wait for the rest of the frame
            f.payload = d + pos + hdr;
            f.payload_len = len;
            switch (f.kind) {
                case Kind::Info: st_.info_frames++; break;
                case Kind::IFrame: st_.i_frames++; break;
                case Kind::PFrame: st_.p_frames++; break;
                case Kind::Audio: st_.audio_frames++; break;
            }
            if (f.kind == Kind::IFrame) seen_i_ = true;
            f.after_i = seen_i_;
            in_junk_ = false;
            cb_(f);
            pos += f.total;
        }
        if (final_ && pos < n) {
            if (!seen_marker_) st_.leading_bytes += n - pos;
            else { st_.truncated_tail = true; st_.tail_bytes = n - pos; }
            pos = n;
        }
        buf_.erase(buf_.begin(), buf_.begin() + pos);
        base_ += pos;
    }
};

}  // namespace xm
