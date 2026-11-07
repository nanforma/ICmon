// av_timeline.h - assigns presentation timestamps to the frames of an archive download.
//
// The camera's time stamps have 1 s resolution (I-frames only), too coarse for playback. Audio is
// continuous G.711 (8000 bytes = 1 s), so the audio byte count is used as the clock, and every GOP of
// video is re-anchored to it at its I-frame:   pts(video) = max(audio_seconds_so_far, end of previous video frame) + j / fps.
// This prevents drift between sound and picture without assuming an exact frame rate.
// The frame rate (about 12.48 fps, measured once on one camera) only sets the spacing inside a GOP.
//
// Frames before the first I-frame of a download (after_i == false) are rejected: they cannot be decoded.
#pragma once
#include <cstdint>

#include "xmdemux.h"

namespace xm {

struct Stamped {
    bool video = false;
    uint64_t pts_ns = 0, dur_ns = 0;
};

class AvTimeline {
public:
    explicit AvTimeline(double fps = 12.48, double audio_bytes_per_sec = 8000.0) : fps_(fps), abps_(audio_bytes_per_sec) {}

    // Returns true when `f` should be played; `out` then holds its timestamps.
    bool stamp(const Frame& f, Stamped& out) {
        if (f.kind == Kind::Info || !f.after_i) return false;
        if (f.kind == Kind::Audio) {
            out.video = false;
            out.pts_ns = ns(audio_bytes_ / abps_);
            out.dur_ns = ns(f.payload_len / abps_);
            audio_bytes_ += f.payload_len;
            return true;
        }
        if (f.kind == Kind::IFrame) {
            // Re-anchor to the audio clock, but never earlier than the end of the previous video frame:
            // GStreamer does not like time stamps that go backwards.
            uint64_t a = ns(audio_bytes_ / abps_);
            anchor_ns_ = a > next_min_ns_ ? a : next_min_ns_;
            j_ = 0;
            have_anchor_ = true;
        } else if (!have_anchor_) return false;
        out.video = true;
        out.pts_ns = anchor_ns_ + ns(j_ / fps_);
        out.dur_ns = ns(1.0 / fps_);
        next_min_ns_ = out.pts_ns + out.dur_ns;
        j_++;
        return true;
    }

private:
    static uint64_t ns(double s) { return s <= 0 ? 0 : (uint64_t)(s * 1e9 + 0.5); }
    double fps_, abps_;
    double audio_bytes_ = 0;
    uint64_t anchor_ns_ = 0, next_min_ns_ = 0, j_ = 0;
    bool have_anchor_ = false;
};

}  // namespace xm
