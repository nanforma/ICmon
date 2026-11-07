#include "rtsp_url.h"

#include <cstdio>

namespace icmon {

// The form used by the user's own working script (mpv): credentials in the address, path "channel=1".
// The stream number is not part of it: with this address {stream} has no effect.
const char* const kDefaultRtspTemplate = "rtsp://{user}:{password}@{host}:{port}/channel={channel}";

std::string percent_encode(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                     c == '_' || c == '~';
        if (plain) {
            o.push_back((char)c);
        } else {
            char b[4];
            snprintf(b, sizeof b, "%%%02X", c);
            o += b;
        }
    }
    return o;
}

UrlResult build_rtsp_url(const std::string& tmpl, const CameraConfig& cam, const std::string& password, int stream) {
    UrlResult r;
    if (tmpl.compare(0, 7, "rtsp://") != 0) {
        r.error = "The RTSP address must start with rtsp://";
        return r;
    }
    std::string host = cam.host;
    if (host.find(':') != std::string::npos && host.front() != '[') host = "[" + host + "]";  // literal IPv6
    std::string out;
    for (size_t i = 0; i < tmpl.size();) {
        if (tmpl[i] != '{') { out.push_back(tmpl[i++]); continue; }
        size_t e = tmpl.find('}', i);
        if (e == std::string::npos) { r.error = "Unclosed { in the RTSP address"; return r; }
        std::string k = tmpl.substr(i + 1, e - i - 1);
        if (k == "host") out += host;
        else if (k == "port") out += std::to_string(cam.rtsp_port);
        else if (k == "user") out += percent_encode(cam.user);
        else if (k == "password") out += percent_encode(password);
        else if (k == "channel") out += std::to_string(cam.channel);
        else if (k == "stream") out += std::to_string(stream);
        else { r.error = "Unknown placeholder {" + k + "} in the RTSP address"; return r; }
        i = e + 1;
    }
    r.url = std::move(out);
    return r;
}

}  // namespace icmon
