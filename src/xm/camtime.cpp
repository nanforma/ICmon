#include "camtime.h"

#include <cstdio>
#include <ctime>

#include "error.h"

namespace xm {

CamTime to_cam_time(const Time& t) {
    tm m{};
    m.tm_year = t.year - 1900; m.tm_mon = t.month - 1; m.tm_mday = t.day;
    m.tm_hour = t.hour; m.tm_min = t.minute; m.tm_sec = t.second;
    return (CamTime)timegm(&m);
}

Time from_cam_time(CamTime v) {
    time_t tt = (time_t)v;
    tm m{};
    gmtime_r(&tt, &m);
    return {m.tm_year + 1900, m.tm_mon + 1, m.tm_mday, m.tm_hour, m.tm_min, m.tm_sec};
}

uint32_t encode_time(const Time& t) {
    return ((uint32_t)(t.year - 2000) << 26) | ((uint32_t)t.month << 22) | ((uint32_t)t.day << 17) |
           ((uint32_t)t.hour << 12) | ((uint32_t)t.minute << 6) | (uint32_t)t.second;
}

CamTime packed_to_cam_time(uint32_t packed) { return to_cam_time(decode_time(packed)); }

CamTime parse_cam_time(const std::string& s) {
    int y, mo, d, h, mi, se;
    char tail;
    if (sscanf(s.c_str(), "%d-%d-%d %d:%d:%d%c", &y, &mo, &d, &h, &mi, &se, &tail) != 6 || mo < 1 || mo > 12 || d < 1 ||
        d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 60)
        throw Error(Errc::Protocol, "bad time \"" + s + "\" (expected YYYY-MM-DD HH:MM:SS)");
    return to_cam_time({y, mo, d, h, mi, se});
}

std::string format_cam_time(CamTime v) {
    Time t = from_cam_time(v);
    char b[32];
    snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d", t.year, t.month, t.day, t.hour, t.minute, t.second);
    return b;
}

std::string format_cam_clock(CamTime v) {
    Time t = from_cam_time(v);
    char b[16];
    snprintf(b, sizeof b, "%02d:%02d:%02d", t.hour, t.minute, t.second);
    return b;
}

}  // namespace xm
