// timeline_geometry.h - GTK-free logic of the 24-hour timeline (unit-tested).
#pragma once
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "archive.h"

namespace icmon {

constexpr int kDaySeconds = 86400;

struct Segment {
    int from = 0, to = 0;  // seconds since the start of the day, from < to
};

// Recordings of the day as drawable segments: clipped to the day, contiguous or overlapping neighbours
// (gap <= max_gap seconds) merged, so a day of 5-minute files draws as a few long bars.
inline std::vector<Segment> day_segments(const std::vector<xm::RecordingFile>& files, xm::CamTime day_start, int max_gap = 2) {
    std::vector<Segment> segs;
    for (const auto& f : files) {
        long long a = std::max<long long>(f.begin - day_start, 0), b = std::min<long long>(f.end - day_start, kDaySeconds);
        if (b <= a) continue;
        segs.push_back({(int)a, (int)b});
    }
    std::sort(segs.begin(), segs.end(), [](const Segment& x, const Segment& y) { return x.from < y.from; });
    std::vector<Segment> out;
    for (const auto& s : segs) {
        if (!out.empty() && s.from <= out.back().to + max_gap) out.back().to = std::max(out.back().to, s.to);
        else out.push_back(s);
    }
    return out;
}

// x in [left, left+width] -> second of the day, clamped to [0, 86400].
inline int second_from_x(double x, double left, double width) {
    if (width <= 0) return 0;
    double t = (x - left) / width * kDaySeconds;
    return (int)std::clamp(t, 0.0, (double)kDaySeconds);
}
inline double x_from_second(int sec, double left, double width) { return left + (double)sec / kDaySeconds * width; }

// The recording that contains `sec` of the day, or nullptr.
inline const xm::RecordingFile* file_at(const std::vector<xm::RecordingFile>& files, xm::CamTime day_start, int sec) {
    return xm::find_file(files, day_start + sec);
}

inline std::string clock_text(int sec) {
    sec = std::clamp(sec, 0, kDaySeconds);
    char b[16];
    snprintf(b, sizeof b, "%02d:%02d:%02d", sec / 3600 % 24, sec / 60 % 60, sec % 60);
    return b;
}

}  // namespace icmon
